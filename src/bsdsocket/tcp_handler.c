/*
 * TCP: -- an AmigaDOS handler that makes a socket a file handle.
 *
 * AmigaOS has no socket-as-file-handle. A descriptor belongs to a SocketBase,
 * a SocketBase belongs to one task, and there is no way to hand either to
 * Open(), Read(), Write() or SystemTagList(). That is the wall every "attach a
 * shell to a connection" idea hits, and a DOS handler is the way through it:
 * DOS routes Open("TCP:...") here, this file makes the connection, and the
 * caller gets an ordinary BPTR.
 *
 * What it buys, in order of how surprising it is:
 *
 *   Type TCP:host/daytime          every AmigaDOS command that reads a file
 *   Copy TCP:host/1234 TO RAM:x    can now read a socket, unmodified
 *   Echo >TCP:host/1234 "hi"       shell redirection into a connection
 *   Open("TCP:OBTAIN=<id>")        the other half of handoff.c: a program
 *                                  accepts a connection, releases a copy of
 *                                  the socket, and opens it as a file handle
 *                                  that SystemTagList() will take as
 *                                  SYS_Input/SYS_Output.
 *
 * NAME SYNTAX, which is Roadshow's (tcp-handler.doc) and not invented here:
 *
 *   TCP:<host>/<service>           connect to <service> on <host>
 *   TCP:<service>                  listen on <service>, accept one connection
 *   TCP:HOST=<h>/PORT=<p>          the same, spelled out; H=/P=/S=/SERVICE=
 *   TCP:OBTAIN=<id>                ObtainSocket() a socket someone parked
 *
 * Components are separated by '/'. A component with an '=' is a keyword; a
 * bare component fills SERVICE first and HOST second, which is what makes
 * "TCP:<service>" a listener and "TCP:<host>/<service>" a connection, exactly
 * as the Roadshow document's two examples require.
 *
 * A handler answers a DOSPACKET when it can and not before, and the packet's
 * sender is asleep in the meantime. A single-process handler therefore has to
 * hold every unanswerable packet in a queue and drive them from one
 * WaitSelect() -- which works, but only for the packets. It does nothing for
 * the two calls that genuinely block and are not sockets: gethostbyname() and
 * connect(). One name lookup would stall every other file handle the handler
 * owns.
 *
 * So this is shaped like console.handler instead: a control process owns the
 * device node and answers ACTION_FINDINPUT/FINDOUTPUT/FINDUPDATE by starting a
 * *session* process, handing it the packet, and forgetting about it. The
 * session opens its own bsdsocket.library -- which is the only correct way to
 * get a descriptor table, since every opener gets its own child base
 * (library.c) -- connects, points the FileHandle's fh_Type at its own port,
 * and replies. Every later packet for that handle goes straight to the
 * session, which may block in recv() for as long as it likes because nobody
 * else is behind it.
 *
 * The FileHandle's fh_Type is ours to change: dos.library sets it to the
 * device's port before sending the packet and re-initialises it on every
 * retry "in case handler played with it" (v40 dos, bcplio.c findstream). That
 * is also what makes ACTION_WAIT_CHAR usable at all -- WaitForChar() sends
 * only a timeout and NOT the file handle, so a handler with one port for many
 * files cannot tell which file is being asked about. One port per file makes
 * the question unambiguous.
 *
 * Neither process uses its pr_MsgPort for DOS packets. Both do their own DOS
 * and library I/O (CreateNewProc, the resolver reading DEVS:Internet), and
 * dos.library's DoPkt replies arrive on pr_MsgPort; sharing the two would mean
 * a reply and an incoming packet in the same queue and a pr_PktWait hook to
 * tell them apart. A second MsgPort costs nothing and removes the problem.
 *
 *   peer closed, all data delivered   Read() returns 0. Ordinary EOF.
 *   connection reset                  Read() returns -1, IoErr() set. Not 0:
 *                                     a reset means the transfer was cut
 *                                     short, and reporting that as EOF turns
 *                                     a truncated file into a successful copy.
 *   nothing to read yet               Read() blocks. There is no idle timeout;
 *                                     this is a connection, not a file.
 *   name lookup or connect failed     Open() fails, so Read() never happens.
 *
 * AmigaDOS has no error code for "connection reset by peer", so the mapping
 * table below picks the nearest thing DOS can print and the real errno goes to
 * the serial log.
 *
 * SPDX-License-Identifier: MIT
 */

#include "bsdsocket_vectors.h"
#include "tcp_handler.h"

#include <proto/dos.h>
#include <proto/exec.h>

#include <dos/dosextens.h>
#include <dos/dostags.h>
#include <dos/filehandler.h>

/* ------------------------------------------------------------- constants -- */

#define TCP_DEVICE_NAME     "TCP"

/*
 * Session stacks. A session runs the resolver and, if it is the first opener
 * on the machine, netstack_startup() -- both of which run NetX Duo code on the
 * calling task's stack through the bsd_nx_enter() bracket.
 */
#define TCP_CTRL_STACK      8192UL
#define TCP_SESSION_STACK   16384UL

#define TCP_CTRL_PRI        5
#define TCP_SESSION_PRI     5

#define TCP_NAME_MAX        256
#define TCP_HOST_MAX        128
#define TCP_SERVICE_MAX      64

/* fd_set words, matching select.c's layout: bit (fd%32) of word (fd/32). */
#define TCP_FD_WORDS        ((BSD_MAX_DTABLESIZE + 31) / 32)

/* ----------------------------------------------------------------- state -- */

typedef struct TcpBoot
{
    struct Task    *tb_Parent;
    BOOL            tb_Ok;
} TcpBoot;

/*
 * Library globals. There is exactly one handler per loaded library, created
 * once under the master semaphore, so these need no further locking; the
 * session count is touched from several processes and is therefore only ever
 * changed inside Forbid().
 */
static struct MsgPort    *tcp_ctrl_port;
static struct DeviceNode *tcp_node;
static TcpBoot           *tcp_boot;
static LONG               tcp_sessions;
static BOOL               tcp_started;

/* Per open file handle. Lives on the session process's stack. */
typedef struct TcpSession
{
    struct MsgPort       *ts_Port;      /* fh_Type: this handle's packets   */
    struct AmiSocketBase *ts_Base;      /* this session's own child base    */
    LONG                  ts_Fd;
} TcpSession;

typedef struct TcpName
{
    char    tn_Host[TCP_HOST_MAX];
    char    tn_Service[TCP_SERVICE_MAX];
    LONG    tn_Obtain;
    BOOL    tn_HasObtain;
} TcpName;

static VOID tcp_ctrl_main(VOID);
static VOID tcp_session_main(VOID);

/* ------------------------------------------------------------- utilities -- */

static BOOL tcp_ci_equal(const char *a, const char *b)
{
    while (*a != '\0' && *b != '\0')
    {
        char ca = *a++;
        char cb = *b++;

        if (ca >= 'a' && ca <= 'z')
            ca = (char)(ca - 'a' + 'A');
        if (cb >= 'a' && cb <= 'z')
            cb = (char)(cb - 'a' + 'A');

        if (ca != cb)
            return FALSE;
    }

    return (*a == '\0' && *b == '\0');
}

/* Decimal only, and only when the WHOLE string is one. -1 means "it is not". */
static LONG tcp_decimal(const char *s)
{
    LONG value = 0;

    if (s == NULL || *s == '\0')
        return -1;

    while (*s != '\0')
    {
        if (*s < '0' || *s > '9')
            return -1;

        value = value * 10 + (*s - '0');
        if (value > 0x7FFFFF)               /* nothing here needs more */
            return -1;

        s++;
    }

    return value;
}

static VOID tcp_bstr_to_c(BSTR bs, char *out, ULONG size)
{
    const UBYTE *b = (const UBYTE *)BADDR(bs);
    ULONG        len, i;

    if (size == 0)
        return;

    out[0] = '\0';
    if (b == NULL)
        return;

    len = b[0];
    if (len > size - 1)
        len = size - 1;

    for (i = 0; i < len; i++)
        out[i] = (char)b[1 + i];

    out[len] = '\0';
}

/*
 * Reply a packet.
 *
 * Not ReplyPkt(): that stamps dp_Port with the CURRENT process's pr_MsgPort,
 * and neither of our processes takes DOS packets there -- see the header
 * comment. The convention dp_Port carries is "the port to send this packet
 * back to", so it has to be the port this handle actually listens on.
 */
static VOID tcp_reply(struct DosPacket *pkt, LONG res1, LONG res2,
                      struct MsgPort *from)
{
    struct MsgPort *reply = pkt->dp_Port;
    struct Message *msg   = pkt->dp_Link;

    pkt->dp_Res1 = res1;
    pkt->dp_Res2 = res2;
    pkt->dp_Port = from;

    msg->mn_Node.ln_Name = (char *)pkt;

    PutMsg(reply, msg);
}

/* --------------------------------------------------------- errno -> DOS -- */

/*
 * There is no DOS error for most of these, so the table says what each one is
 * being reported AS rather than pretending to be a translation. The socket
 * errno is logged next to it at every site that uses this.
 */
typedef struct TcpErrorMap
{
    LONG    tem_Errno;
    LONG    tem_DosError;
} TcpErrorMap;

static const TcpErrorMap tcp_error_map[] =
{
    { AMI_ENOMEM,        ERROR_NO_FREE_STORE      },
    { AMI_ENOBUFS,       ERROR_NO_FREE_STORE      },
    { AMI_EMFILE,        ERROR_NO_FREE_STORE      },
    { AMI_ENFILE,        ERROR_NO_FREE_STORE      },
    { AMI_EADDRINUSE,    ERROR_OBJECT_IN_USE      },
    { AMI_EADDRNOTAVAIL, ERROR_OBJECT_NOT_FOUND   },
    { AMI_ENETDOWN,      ERROR_DEVICE_NOT_MOUNTED },
    { AMI_ENETUNREACH,   ERROR_DEVICE_NOT_MOUNTED },
    { AMI_EHOSTDOWN,     ERROR_DEVICE_NOT_MOUNTED },
    { AMI_EHOSTUNREACH,  ERROR_DEVICE_NOT_MOUNTED },
    { AMI_EACCES,        ERROR_READ_PROTECTED     },
    { AMI_EPERM,         ERROR_READ_PROTECTED     },
    { AMI_EINVAL,        ERROR_OBJECT_NOT_FOUND   }
};

static LONG tcp_dos_error(LONG err)
{
    ULONG i;

    for (i = 0; i < sizeof(tcp_error_map) / sizeof(tcp_error_map[0]); i++)
    {
        if (tcp_error_map[i].tem_Errno == err)
            return tcp_error_map[i].tem_DosError;
    }

    /*
     * ECONNREFUSED, ECONNRESET, ETIMEDOUT, EPIPE, ENOTCONN and everything
     * else all mean the same thing to a program holding a file handle: the
     * thing at the other end is not there. "Object not found" is the closest
     * AmigaDOS gets, and Fault() has a sentence for it.
     */
    return ERROR_OBJECT_NOT_FOUND;
}

/* ---------------------------------------------------------- name parsing -- */

/*
 * "TCP:host/service" -> TcpName. Returns 0, or a DOS error code.
 *
 * dp_Arg3 carries the whole path including the device name, because the lock
 * in dp_Arg2 is NULL (v40 dos, bcplio.c findstream: the BSTR it sends is the
 * caller's string verbatim). Skipping to past the first ':' is what every
 * handler does with it.
 */
static LONG tcp_parse(const char *path, TcpName *out)
{
    const char *p = path;
    char        bare[2][TCP_HOST_MAX];
    ULONG       bares = 0;
    ULONG       i;

    bsd_bzero(out, sizeof(*out));

    for (i = 0; path[i] != '\0'; i++)
    {
        if (path[i] == ':')
        {
            p = &path[i + 1];
            break;
        }
    }

    while (*p != '\0')
    {
        char  comp[TCP_HOST_MAX];
        LONG  eq  = -1;
        ULONG len = 0;

        while (p[len] != '\0' && p[len] != '/')
            len++;

        if (len >= sizeof(comp))
            return ERROR_OBJECT_NOT_FOUND;

        for (i = 0; i < len; i++)
        {
            comp[i] = p[i];
            if (comp[i] == '=' && eq < 0)
                eq = (LONG)i;
        }
        comp[len] = '\0';

        p += len;
        if (*p == '/')
            p++;

        if (len == 0)
            continue;                   /* "TCP:host//service", be tolerant */

        if (eq < 0)
        {
            if (bares >= 2)
                return ERROR_OBJECT_NOT_FOUND;

            bsd_strncpy(bare[bares], comp, sizeof(bare[0]));
            bares++;
            continue;
        }

        {
            const char *value = &comp[eq + 1];

            comp[eq] = '\0';

            if (tcp_ci_equal(comp, "H") || tcp_ci_equal(comp, "HOST"))
            {
                bsd_strncpy(out->tn_Host, value, sizeof(out->tn_Host));
            }
            else if (tcp_ci_equal(comp, "P") || tcp_ci_equal(comp, "PORT") ||
                     tcp_ci_equal(comp, "S") || tcp_ci_equal(comp, "SERVICE"))
            {
                bsd_strncpy(out->tn_Service, value, sizeof(out->tn_Service));
            }
            else if (tcp_ci_equal(comp, "O") || tcp_ci_equal(comp, "OBTAIN"))
            {
                LONG id = tcp_decimal(value);

                if (id < 0)
                    return ERROR_BAD_NUMBER;

                out->tn_Obtain    = id;
                out->tn_HasObtain = TRUE;
            }
            else
            {
                return ERROR_OBJECT_NOT_FOUND;
            }
        }
    }

    /*
     * Where the bare components go, and why SERVICE comes first.
     *
     * The Roadshow document has to be satisfied in both directions at once:
     * "TCP:localhost/daytime" is a connection, and "TCP:<service name>" alone
     * is the same thing as "TCP:service=<service name>" -- a listener. So two
     * bare components are host then service, and a single one is a service.
     */
    if (bares == 2)
    {
        if (out->tn_Host[0] != '\0' || out->tn_Service[0] != '\0')
            return ERROR_OBJECT_NOT_FOUND;

        bsd_strncpy(out->tn_Host, bare[0], sizeof(out->tn_Host));
        bsd_strncpy(out->tn_Service, bare[1], sizeof(out->tn_Service));
    }
    else if (bares == 1)
    {
        if (out->tn_Service[0] == '\0')
            bsd_strncpy(out->tn_Service, bare[0], sizeof(out->tn_Service));
        else if (out->tn_Host[0] == '\0')
            bsd_strncpy(out->tn_Host, bare[0], sizeof(out->tn_Host));
        else
            return ERROR_OBJECT_NOT_FOUND;
    }

    if (out->tn_HasObtain)
        return 0;                       /* OBTAIN ignores everything else */

    if (out->tn_Service[0] == '\0')
        return ERROR_OBJECT_NOT_FOUND;

    return 0;
}

/* --------------------------------------------------------- socket set-up -- */

/* A service name or a port number -> a port. 0 means "no such service". */
static UWORD tcp_service_port(struct AmiSocketBase *base, const char *service)
{
    struct servent *se;
    LONG            number = tcp_decimal(service);

    if (number > 0 && number <= 65535)
        return (UWORD)number;

    se = bsd_getservbyname((STRPTR)service, (STRPTR)"tcp", base);
    if (se == NULL)
        return 0;

    return BSD_NTOHS((UWORD)se->s_port);
}

/*
 * Wait until `fd` has something to say. `timeout` is NULL for "as long as it
 * takes". Returns >0 ready, 0 timed out, -1 failed.
 *
 * Every wait in this file goes through WaitSelect() rather than through a
 * blocking socket call, and that is deliberate: a blocking accept() was
 * observed once, under this emulator, not to return after a connection had
 * plainly been established (the peer's Open() had already succeeded), while
 * the same connection made the descriptor readable to WaitSelect() straight
 * away. Waiting on readiness and then taking the socket in a call that cannot
 * block is the shape that has never stalled.
 */
static LONG tcp_wait_ready(struct AmiSocketBase *base, LONG fd,
                           struct timeval *timeout)
{
    ULONG fds[TCP_FD_WORDS];
    ULONG i;

    for (i = 0; i < TCP_FD_WORDS; i++)
        fds[i] = 0;

    fds[(ULONG)fd / 32] = 1UL << ((ULONG)fd % 32);

    return bsd_WaitSelect(fd + 1, fds, NULL, NULL, timeout, NULL, base);
}

static VOID tcp_sockaddr(struct sockaddr_in *sin, ULONG addr, UWORD port)
{
    bsd_bzero(sin, sizeof(*sin));

    /*
     * sin_len is at offset 0 in this NDK's 4.4BSD sockaddr_in and the library
     * decides the family from the bytes rather than from a struct member (see
     * bsd_sa_family()), so both fields are set.
     */
    sin->sin_len         = sizeof(*sin);
    sin->sin_family      = AF_INET;
    sin->sin_port        = BSD_HTONS(port);
    sin->sin_addr.s_addr = addr;
}

/* A host name or dotted quad -> an address. Returns 0, or a DOS error. */
static LONG tcp_resolve(struct AmiSocketBase *base, const char *host,
                        ULONG *addr_out)
{
    struct hostent *he;
    in_addr_t       literal = bsd_inet_addr((STRPTR)host, base);

    if (literal != (in_addr_t)-1)
    {
        *addr_out = (ULONG)literal;
        return 0;
    }

    he = bsd_gethostbyname((STRPTR)host, base);
    if (he == NULL || he->h_addr_list == NULL ||
        he->h_addr_list[0] == NULL || he->h_length != 4)
    {
        AMI_WARN("TCP: cannot resolve '%s'", host);
        return ERROR_OBJECT_NOT_FOUND;
    }

    bsd_bcopy((APTR)he->h_addr_list[0], addr_out, 4);

    return 0;
}

/*
 * Everything between "the name parsed" and "there is a connected socket".
 * Returns 0 with *fd_out set, or a DOS error code.
 */
static LONG tcp_open_socket(struct AmiSocketBase *base, const TcpName *name,
                            LONG *fd_out)
{
    struct sockaddr_in sin;
    LONG               fd;
    LONG               err;

    if (name->tn_HasObtain)
    {
        /*
         * The other half of handoff.c. The socket was parked by
         * ReleaseSocket()/ReleaseCopyOfSocket() in some other task's base;
         * ObtainSocket() moves it into ours and this process becomes the one
         * NetX Duo's callbacks signal.
         */
        fd = bsd_ObtainSocket(name->tn_Obtain, AF_INET, SOCK_STREAM, 0, base);
        if (fd < 0)
        {
            err = bsd_Errno(base);
            AMI_WARN("TCP: OBTAIN=%ld failed, errno %ld",
                     (long)name->tn_Obtain, (long)err);
            return (err == AMI_ENOENT) ? ERROR_OBJECT_NOT_FOUND
                                       : tcp_dos_error(err);
        }

        *fd_out = fd;
        return 0;
    }

    {
        UWORD port = tcp_service_port(base, name->tn_Service);
        ULONG addr = 0;

        if (port == 0)
        {
            AMI_WARN("TCP: no service '%s'", name->tn_Service);
            return ERROR_OBJECT_NOT_FOUND;
        }

        if (name->tn_Host[0] != '\0')
        {
            err = tcp_resolve(base, name->tn_Host, &addr);
            if (err != 0)
                return err;
        }

        fd = bsd_socket(AF_INET, SOCK_STREAM, 0, base);
        if (fd < 0)
            return tcp_dos_error(bsd_Errno(base));

        if (name->tn_Host[0] != '\0')
        {
            tcp_sockaddr(&sin, addr, port);

            if (bsd_connect(fd, (struct sockaddr *)&sin, sizeof(sin), base) < 0)
            {
                err = bsd_Errno(base);
                AMI_WARN("TCP: connect to %s/%s failed, errno %ld",
                         name->tn_Host, name->tn_Service, (long)err);
                (VOID)bsd_CloseSocket(fd, base);
                return tcp_dos_error(err);
            }

            *fd_out = fd;
            return 0;
        }

        /*
         * No host: Roadshow's "allow other networked hosts to open a
         * connection to the local machine on the port specified here". One
         * connection, then the listener is done -- a DOS file handle is one
         * stream and there is nowhere to put a second.
         */
        {
            LONG on = 1;
            LONG client;

            (VOID)bsd_setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on,
                                 sizeof(on), base);

            tcp_sockaddr(&sin, INADDR_ANY, port);

            if (bsd_bind(fd, (struct sockaddr *)&sin, sizeof(sin), base) < 0 ||
                bsd_listen(fd, 1, base) < 0)
            {
                err = bsd_Errno(base);
                AMI_WARN("TCP: listen on %s failed, errno %ld",
                         name->tn_Service, (long)err);
                (VOID)bsd_CloseSocket(fd, base);
                return tcp_dos_error(err);
            }

            if (tcp_wait_ready(base, fd, NULL) < 0)
            {
                err = bsd_Errno(base);
                (VOID)bsd_CloseSocket(fd, base);
                return tcp_dos_error(err);
            }

            client = bsd_accept(fd, NULL, NULL, base);
            if (client < 0)
            {
                err = bsd_Errno(base);
                (VOID)bsd_CloseSocket(fd, base);
                return tcp_dos_error(err);
            }

            (VOID)bsd_CloseSocket(fd, base);

            *fd_out = client;
            return 0;
        }
    }
}

/* ---------------------------------------------------------- the session -- */

static VOID tcp_session_read(TcpSession *s, struct DosPacket *pkt)
{
    APTR buffer = (APTR)pkt->dp_Arg2;
    LONG length = pkt->dp_Arg3;
    LONG got;

    if (length <= 0)
    {
        tcp_reply(pkt, 0, 0, s->ts_Port);
        return;
    }

    got = bsd_recv(s->ts_Fd, buffer, length, 0, s->ts_Base);

    if (got >= 0)
    {
        /* 0 is EOF to DOS, which is exactly what an orderly close means. */
        tcp_reply(pkt, got, 0, s->ts_Port);
        return;
    }

    {
        LONG err = bsd_Errno(s->ts_Base);

        AMI_WARN("TCP: read failed, errno %ld", (long)err);
        tcp_reply(pkt, -1, tcp_dos_error(err), s->ts_Port);
    }
}

static VOID tcp_session_write(TcpSession *s, struct DosPacket *pkt)
{
    UBYTE *buffer = (UBYTE *)pkt->dp_Arg2;
    LONG   length = pkt->dp_Arg3;
    LONG   done   = 0;

    if (length <= 0)
    {
        tcp_reply(pkt, 0, 0, s->ts_Port);
        return;
    }

    /*
     * DOS wants "all of it, or an error". send() on a blocking socket can
     * still come back short when the window closes mid-transfer, so this
     * loops rather than trusting one call.
     */
    while (done < length)
    {
        LONG sent = bsd_send(s->ts_Fd, buffer + done, length - done, 0,
                             s->ts_Base);

        if (sent > 0)
        {
            done += sent;
            continue;
        }

        {
            LONG err = bsd_Errno(s->ts_Base);

            AMI_WARN("TCP: write failed after %ld of %ld bytes, errno %ld",
                     (long)done, (long)length, (long)err);
            tcp_reply(pkt, -1, tcp_dos_error(err), s->ts_Port);
            return;
        }
    }

    tcp_reply(pkt, done, 0, s->ts_Port);
}

/*
 * ACTION_WAIT_CHAR. dp_Arg1 is a timeout in microseconds and there is no file
 * handle in the packet at all -- see the header comment on why each handle has
 * its own port. This is the one place the handler uses WaitSelect(), and it is
 * the natural use for it: one descriptor, one timeout.
 */
static VOID tcp_session_wait_char(TcpSession *s, struct DosPacket *pkt)
{
    struct timeval tv;
    ULONG          micros = (ULONG)pkt->dp_Arg1;
    LONG           ready;

    tv.tv_secs  = (LONG)(micros / 1000000UL);
    tv.tv_micro = (LONG)(micros % 1000000UL);

    ready = tcp_wait_ready(s->ts_Base, s->ts_Fd, &tv);

    /*
     * A closed connection counts as ready: the next Read() will return
     * immediately, which is precisely what WaitForChar() is asked to predict.
     */
    tcp_reply(pkt, (ready > 0) ? DOSTRUE : DOSFALSE, 0, s->ts_Port);
}

/*
 * The FIND packet, from the point of view of the process that will own the
 * handle. Returns TRUE if the handle is live and the session should carry on.
 */
static BOOL tcp_session_open(TcpSession *s, struct DosPacket *pkt)
{
    struct FileHandle *fh = (struct FileHandle *)BADDR(pkt->dp_Arg1);
    char               path[TCP_NAME_MAX];
    TcpName            name;
    LONG               err;

    tcp_bstr_to_c((BSTR)pkt->dp_Arg3, path, sizeof(path));

    AMI_INFO("TCP: open '%s'", path);

    err = tcp_parse(path, &name);
    if (err != 0)
    {
        AMI_WARN("TCP: cannot parse '%s'", path);
        tcp_reply(pkt, DOSFALSE, err, tcp_ctrl_port);
        return FALSE;
    }

    s->ts_Port = CreateMsgPort();
    if (s->ts_Port == NULL)
    {
        tcp_reply(pkt, DOSFALSE, ERROR_NO_FREE_STORE, tcp_ctrl_port);
        return FALSE;
    }

    /*
     * Our own opener base. Not the caller's and not the master's: a descriptor
     * table belongs to one task and this is the task that will use it.
     */
    s->ts_Base = (struct AmiSocketBase *)
        OpenLibrary((STRPTR)BSD_LIB_NAME, BSD_LIB_VERSION);
    if (s->ts_Base == NULL)
    {
        DeleteMsgPort(s->ts_Port);
        s->ts_Port = NULL;
        tcp_reply(pkt, DOSFALSE, ERROR_DEVICE_NOT_MOUNTED, tcp_ctrl_port);
        return FALSE;
    }

    err = tcp_open_socket(s->ts_Base, &name, &s->ts_Fd);
    if (err != 0)
    {
        CloseLibrary((struct Library *)s->ts_Base);
        s->ts_Base = NULL;
        DeleteMsgPort(s->ts_Port);
        s->ts_Port = NULL;
        tcp_reply(pkt, DOSFALSE, err, tcp_ctrl_port);
        return FALSE;
    }

    /*
     * fh_Type moves the handle off the device's port and onto ours. fh_Port
     * is dos.library's "interactive" flag: a connection is a stream with no
     * length and no seek, which is what interactive means to DOS, and it is
     * what allows WaitForChar() at all.
     */
    fh->fh_Arg1 = (LONG)s;
    fh->fh_Type = s->ts_Port;
    fh->fh_Port = s->ts_Port;

    AMI_INFO("TCP: '%s' is socket %ld", path, (long)s->ts_Fd);

    tcp_reply(pkt, DOSTRUE, 0, s->ts_Port);

    return TRUE;
}

static VOID tcp_session_close(TcpSession *s)
{
    AmiSocket *sock;
    BOOL       shared;

    if (s->ts_Base == NULL)
        return;

    /*
     * A socket taken through TCP:OBTAIN= can outlive this session: the program
     * that released a COPY of it still holds the original descriptor. Its
     * as_Owner points at this base, though, and this base is about to be freed
     * -- so the next receive or disconnect callback would Signal() a task read
     * out of freed memory. Handing it back to nobody is exactly what
     * handoff.c does with a parked socket, and bsd_event_post() already treats
     * a NULL owner as "there is no task to wake".
     *
     * The refcount is read BEFORE the close because after it the block may be
     * gone. This is the local half of a general hazard -- any base that closes
     * while a socket it owns is still referenced elsewhere has it -- and the
     * general fix belongs in bsd_socket_release().
     */
    sock   = bsd_lookup(s->ts_Base, s->ts_Fd);
    shared = (sock != NULL && sock->as_RefCount > 1);

    (VOID)bsd_CloseSocket(s->ts_Fd, s->ts_Base);

    if (shared && sock->as_Owner == s->ts_Base)
        sock->as_Owner = NULL;

    CloseLibrary((struct Library *)s->ts_Base);
    s->ts_Base = NULL;
}

static VOID tcp_session_main(VOID)
{
    struct Process   *me = (struct Process *)FindTask(NULL);
    struct Message   *msg;
    struct DosPacket *pkt;
    TcpSession        s;
    BOOL              running;

    s.ts_Port = NULL;
    s.ts_Base = NULL;
    s.ts_Fd   = -1;

    /*
     * The control process forwarded the FIND packet to our pr_MsgPort. It is
     * the only message that ever arrives there; from here on pr_MsgPort is
     * dos.library's, for the DOS calls the resolver makes.
     */
    WaitPort(&me->pr_MsgPort);
    msg = GetMsg(&me->pr_MsgPort);
    pkt = (struct DosPacket *)msg->mn_Node.ln_Name;

    running = tcp_session_open(&s, pkt);

    while (running)
    {
        WaitPort(s.ts_Port);

        while ((msg = GetMsg(s.ts_Port)) != NULL)
        {
            pkt = (struct DosPacket *)msg->mn_Node.ln_Name;

            switch (pkt->dp_Type)
            {
                case ACTION_READ:
                    tcp_session_read(&s, pkt);
                    break;

                case ACTION_WRITE:
                    tcp_session_write(&s, pkt);
                    break;

                case ACTION_WAIT_CHAR:
                    tcp_session_wait_char(&s, pkt);
                    break;

                case ACTION_END:
                    tcp_session_close(&s);
                    tcp_reply(pkt, DOSTRUE, 0, s.ts_Port);
                    running = FALSE;
                    break;

                case ACTION_IS_FILESYSTEM:
                    tcp_reply(pkt, DOSFALSE, 0, s.ts_Port);
                    break;

                case ACTION_FLUSH:
                case ACTION_CHANGE_SIGNAL:
                    tcp_reply(pkt, DOSTRUE, 0, s.ts_Port);
                    break;

                case ACTION_SEEK:
                    /* A connection has no position and nothing to go back to. */
                    tcp_reply(pkt, -1, ERROR_SEEK_ERROR, s.ts_Port);
                    break;

                default:
                    AMI_INFO("TCP: unhandled packet %ld on a file handle",
                             (long)pkt->dp_Type);
                    tcp_reply(pkt, DOSFALSE, ERROR_ACTION_NOT_KNOWN, s.ts_Port);
                    break;
            }

            if (!running)
                break;
        }
    }

    tcp_session_close(&s);

    if (s.ts_Port != NULL)
        DeleteMsgPort(s.ts_Port);

    /*
     * Inside Forbid() so that the control process cannot see the count reach
     * zero, answer ACTION_DIE and take the library's segment away while these
     * last instructions are still executing out of it.
     */
    Forbid();
    tcp_sessions--;
}

/* ---------------------------------------------------------- the control -- */

static VOID tcp_ctrl_find(struct DosPacket *pkt)
{
    struct Process *session;
    struct TagItem  tags[6];

    tags[0].ti_Tag  = NP_Entry;
    tags[0].ti_Data = (ULONG)tcp_session_main;
    tags[1].ti_Tag  = NP_Name;
    tags[1].ti_Data = (ULONG)"TCP: session";
    tags[2].ti_Tag  = NP_StackSize;
    tags[2].ti_Data = TCP_SESSION_STACK;
    tags[3].ti_Tag  = NP_Priority;
    tags[3].ti_Data = (ULONG)TCP_SESSION_PRI;
    tags[4].ti_Tag  = NP_Cli;
    tags[4].ti_Data = FALSE;
    tags[5].ti_Tag  = TAG_DONE;
    tags[5].ti_Data = 0;

    Forbid();
    tcp_sessions++;
    Permit();

    session = CreateNewProc(tags);
    if (session == NULL)
    {
        Forbid();
        tcp_sessions--;
        Permit();

        AMI_ERROR("TCP: cannot start a session process");
        tcp_reply(pkt, DOSFALSE, ERROR_NO_FREE_STORE, tcp_ctrl_port);
        return;
    }

    /* Hand the packet over. The session replies it, not us. */
    PutMsg(&session->pr_MsgPort, pkt->dp_Link);
}

/*
 * Info() on the device. Answered rather than refused because commands ask it
 * about a copy destination before they write, and "0 blocks free" reads as a
 * full disk. There is no disk, so the numbers say "plenty, and valid".
 */
static VOID tcp_ctrl_disk_info(struct DosPacket *pkt, struct InfoData *info)
{
    if (info == NULL)
    {
        tcp_reply(pkt, DOSFALSE, ERROR_INVALID_LOCK, tcp_ctrl_port);
        return;
    }

    bsd_bzero(info, sizeof(*info));

    info->id_DiskState     = ID_VALIDATED;
    info->id_NumBlocks     = 0x100000;
    info->id_NumBlocksUsed = 0;
    info->id_BytesPerBlock = 512;
    info->id_DiskType      = ID_DOS_DISK;

    tcp_reply(pkt, DOSTRUE, 0, tcp_ctrl_port);
}

static BOOL tcp_ctrl_publish(VOID)
{
    struct DosList *list;
    BOOL            taken;

    tcp_ctrl_port = CreateMsgPort();
    if (tcp_ctrl_port == NULL)
        return FALSE;

    /*
     * Roadshow adds TCP: "unless there already is an assignment, a volume or a
     * file system device of that name". AddDosEntry refuses a duplicate device
     * or volume by itself; an ASSIGN of the same name it would happily shadow,
     * so that one is checked here.
     */
    list = LockDosList(LDF_ASSIGNS | LDF_READ);
    taken = (FindDosEntry(list, (STRPTR)TCP_DEVICE_NAME, LDF_ASSIGNS) != NULL);
    UnLockDosList(LDF_ASSIGNS | LDF_READ);

    if (taken)
    {
        AMI_WARN("TCP: not published -- something is already assigned to it");
        DeleteMsgPort(tcp_ctrl_port);
        tcp_ctrl_port = NULL;
        return FALSE;
    }

    tcp_node = (struct DeviceNode *)MakeDosEntry((STRPTR)TCP_DEVICE_NAME,
                                                 DLT_DEVICE);
    if (tcp_node == NULL)
    {
        DeleteMsgPort(tcp_ctrl_port);
        tcp_ctrl_port = NULL;
        return FALSE;
    }

    tcp_node->dn_Task      = tcp_ctrl_port;
    tcp_node->dn_StackSize = TCP_SESSION_STACK;
    tcp_node->dn_Priority  = TCP_SESSION_PRI;
    tcp_node->dn_GlobalVec = (BPTR)-1;      /* not a BCPL program */

    if (!AddDosEntry((struct DosList *)tcp_node))
    {
        AMI_WARN("TCP: not published -- the name is already in the DOS list");
        FreeDosEntry((struct DosList *)tcp_node);
        tcp_node = NULL;
        DeleteMsgPort(tcp_ctrl_port);
        tcp_ctrl_port = NULL;
        return FALSE;
    }

    return TRUE;
}

static VOID tcp_ctrl_main(VOID)
{
    BOOL running = TRUE;
    BOOL ok      = tcp_ctrl_publish();

    tcp_boot->tb_Ok = ok;
    Signal(tcp_boot->tb_Parent, SIGF_SINGLE);

    if (!ok)
        return;

    AMI_INFO("TCP: handler ready");

    while (running)
    {
        struct Message *msg;

        WaitPort(tcp_ctrl_port);

        while ((msg = GetMsg(tcp_ctrl_port)) != NULL)
        {
            struct DosPacket *pkt = (struct DosPacket *)msg->mn_Node.ln_Name;

            switch (pkt->dp_Type)
            {
                case ACTION_FINDINPUT:
                case ACTION_FINDOUTPUT:
                case ACTION_FINDUPDATE:
                    tcp_ctrl_find(pkt);
                    break;

                /*
                 * DOSFALSE with dp_Res2 == 0 is load-bearing and is not a
                 * failure. dos.library's MatchFirst() (v40, patternhack.c)
                 * asks this about any path containing a ':' and, on a NO with
                 * no error, returns the path verbatim instead of trying to
                 * Lock() it -- which is the entire reason `Type TCP:...`
                 * works without this handler implementing locks.
                 */
                case ACTION_IS_FILESYSTEM:
                    tcp_reply(pkt, DOSFALSE, 0, tcp_ctrl_port);
                    break;

                case ACTION_DISK_INFO:
                    tcp_ctrl_disk_info(pkt, (struct InfoData *)
                                            BADDR(pkt->dp_Arg1));
                    break;

                case ACTION_FLUSH:
                    tcp_reply(pkt, DOSTRUE, 0, tcp_ctrl_port);
                    break;

                /*
                 * Lock("TCP:...") -- and something really does send it: `Copy`
                 * locks its source to compare it against C: before it decides
                 * what kind of copy this is (v40 copy.c). Refusing is the
                 * whole answer; Copy carries on and opens the stream instead.
                 * It is here rather than in the default arm so that the log
                 * only ever names packets nobody has thought about.
                 */
                case ACTION_LOCATE_OBJECT:
                    tcp_reply(pkt, DOSFALSE, ERROR_ACTION_NOT_KNOWN,
                              tcp_ctrl_port);
                    break;

                case ACTION_DIE:
                    if (tcp_sessions > 0)
                    {
                        tcp_reply(pkt, DOSFALSE, ERROR_OBJECT_IN_USE,
                                  tcp_ctrl_port);
                        break;
                    }

                    /*
                     * Off the DOS list first, so nothing can arrive after the
                     * reply; then Forbid() so that whoever asked cannot free
                     * the library segment these instructions live in before
                     * this process has left it.
                     */
                    LockDosList(LDF_DEVICES | LDF_WRITE);
                    RemDosEntry((struct DosList *)tcp_node);
                    UnLockDosList(LDF_DEVICES | LDF_WRITE);

                    FreeDosEntry((struct DosList *)tcp_node);
                    tcp_node = NULL;

                    AMI_INFO("TCP: handler stopped");

                    {
                        struct MsgPort *port = tcp_ctrl_port;

                        Forbid();
                        tcp_reply(pkt, DOSTRUE, 0, port);
                        tcp_ctrl_port = NULL;
                        DeleteMsgPort(port);
                    }

                    running = FALSE;
                    break;

                default:
                    AMI_INFO("TCP: unhandled packet %ld on the device",
                             (long)pkt->dp_Type);
                    tcp_reply(pkt, DOSFALSE, ERROR_ACTION_NOT_KNOWN,
                              tcp_ctrl_port);
                    break;
            }

            if (!running)
                break;
        }
    }

    /* Still inside the Forbid() taken above; the port dies with us. */
}

/* ----------------------------------------------------------------- entry -- */

VOID bsd_tcp_handler_start(struct AmiSocketBase *master)
{
    TcpBoot         boot;
    struct Process *proc;
    struct TagItem  tags[6];
    struct Task    *me = FindTask(NULL);

    /*
     * CreateNewProc() wants a Process to inherit from. Every real opener is
     * one; a bare Task that opens the library simply does not get a TCP:
     * device rather than taking the machine down.
     */
    if (me == NULL || me->tc_Node.ln_Type != NT_PROCESS)
        return;

    ObtainSemaphore(&master->sb_Lock);
    if (tcp_started)
    {
        ReleaseSemaphore(&master->sb_Lock);
        return;
    }
    tcp_started = TRUE;
    ReleaseSemaphore(&master->sb_Lock);

    boot.tb_Parent = me;
    boot.tb_Ok     = FALSE;
    tcp_boot       = &boot;

    tags[0].ti_Tag  = NP_Entry;
    tags[0].ti_Data = (ULONG)tcp_ctrl_main;
    tags[1].ti_Tag  = NP_Name;
    tags[1].ti_Data = (ULONG)"TCP: handler";
    tags[2].ti_Tag  = NP_StackSize;
    tags[2].ti_Data = TCP_CTRL_STACK;
    tags[3].ti_Tag  = NP_Priority;
    tags[3].ti_Data = (ULONG)TCP_CTRL_PRI;
    tags[4].ti_Tag  = NP_Cli;
    tags[4].ti_Data = FALSE;
    tags[5].ti_Tag  = TAG_DONE;
    tags[5].ti_Data = 0;

    SetSignal(0, SIGF_SINGLE);

    proc = CreateNewProc(tags);
    if (proc == NULL)
    {
        AMI_ERROR("TCP: cannot start the handler process");
        tcp_boot = NULL;
        return;
    }

    /*
     * Bounded: the handler signals before it does anything that can block, and
     * `boot` is on this stack so it has to be gone before we return.
     */
    Wait(SIGF_SINGLE);
    tcp_boot = NULL;

    if (!boot.tb_Ok)
        AMI_WARN("TCP: device not available");
}

BOOL bsd_tcp_handler_alive(VOID)
{
    return (tcp_ctrl_port != NULL) ? TRUE : FALSE;
}

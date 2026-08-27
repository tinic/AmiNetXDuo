/*
 * src/bsdsocket/tcp_handler.c on the host: the TCP: AmigaDOS handler.
 *
 * WHY THIS ONE WAS OUTSIDE THE TIER, AND WHY IT IS IN NOW.  tcp_handler.c is a
 * DOS handler as well as a socket, so it needs the whole ACTION_ and ERROR_
 * vocabulary and struct FileHandle, which are now in host/shim.  What actually
 * kept it out is narrower and is the reason this target is 32-bit only: the
 * file reaches a struct FileHandle through BADDR(pkt->dp_Arg1) and hands its
 * session pointer back through fh_Arg1, and dp_Arg1 and fh_Arg1 are LONG.  A
 * pointer only survives that round trip where a pointer is four bytes, so the
 * file is compiled here at the target's width rather than with the casts
 * silenced.  tests/bsdsocket/CMakeLists.txt says the same thing next to the
 * target.
 *
 * The translation unit is #included rather than linked, because most of the
 * file is static: the packet vocabulary lives in two switch statements inside
 * tcp_ctrl_main() and tcp_session_main(), and the TCP: path grammar is
 * tcp_parse().  What runs is still the shipping code, unmodified.
 *
 * The two loops are driven for real.  WaitPort()/GetMsg()/PutMsg() below are a
 * per-port queue, so a test queues DOS packets, calls the loop, and reads the
 * dp_Res1/dp_Res2 the handler replied with.  A loop that asks for a packet
 * nobody queued aborts rather than spinning.
 *
 * SPDX-License-Identifier: MIT
 */

#include "bsdsocket_vectors.h"

#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/dostags.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned long h_checks;
static unsigned long h_failures;

#define CHECK(cond, what)                                                     \
    do {                                                                      \
        h_checks++;                                                           \
        if (!(cond)) {                                                        \
            h_failures++;                                                     \
            printf("  FAIL %s\n", (what));                                    \
        }                                                                     \
    } while (0)

#define H_PORTS    16
#define H_QUEUE    64
#define H_REPLIES  64
#define H_PACKETS  32

static struct MsgPort  h_ports[H_PORTS];
static unsigned        h_ports_used;

static struct Process  h_proc;
static struct AmiSocketBase h_base;
static AmiConfig       h_cfg;
static struct DosList  h_dos_head;
static struct DeviceNode h_made_node;

typedef struct HQueued
{
    struct MsgPort *port;
    struct Message *msg;
} HQueued;

static struct
{
    HQueued         queue[H_QUEUE];
    unsigned        queued;

    struct DosPacket *reply[H_REPLIES];
    struct MsgPort   *reply_to[H_REPLIES];
    unsigned          replies;

    /* Called when a loop asks a port for a packet and there is none.  A test
       uses it to answer the port the handler created for itself. */
    void  (*stall)(struct MsgPort *port);
    unsigned stalls;

    /* DOS list */
    BOOL            name_taken;         /* FindDosEntry() answers something  */
    BOOL            make_fails;
    BOOL            add_fails;
    unsigned        makes, frees, adds, rems;
    unsigned        locks, unlocks;
    ULONG           last_lock_flags;

    /* Processes */
    BOOL            createproc_fails;
    unsigned        procs;
    APTR            last_entry;
    const char     *last_proc_name;
    ULONG           last_stack;

    /* Ports */
    unsigned        ports_made, ports_deleted;

    /* Exec */
    unsigned        signals;
    struct Task    *last_signalled;
    unsigned        opens, closes;
    BOOL            open_fails;

    /* The socket layer */
    LONG            errno_value;
    LONG            socket_fd;          /* what bsd_socket() answers         */
    LONG            accept_fd;
    LONG            obtain_fd;
    LONG            connect_result;
    LONG            bind_result;
    LONG            listen_result;
    LONG            select_result;
    unsigned        selects;
    struct ami_timeval last_timeout;
    BOOL            had_timeout;

    LONG            recv_plan[4];
    unsigned        recv_planned, recv_calls;
    LONG            send_plan[4];
    unsigned        send_planned, send_calls;
    ULONG           sent_total;

    unsigned        closesockets;
    LONG            last_closed_fd;
    unsigned        lookups;            /* MUST stay 0: use after free       */
    unsigned        connects, binds, listens, accepts, setsockopts;
    ULONG           last_connect_addr;
    UWORD           last_connect_port;
    UWORD           last_bind_port;

    BOOL            service_known;
    UWORD           service_port;
    BOOL            host_known;
    ULONG           host_addr;
    in_addr_t       inet_addr_result;
} h;

static void h_abort(const char *what)
{
    printf("  FAIL unreachable: %s\n", what);
    h_failures++;
    exit(2);
}

/* --------------------------------------------------------------- shim ---- */

struct MsgPort *CreateMsgPort(VOID)
{
    struct MsgPort *p;

    if (h_ports_used >= H_PORTS)
        h_abort("out of fake message ports");

    p = &h_ports[h_ports_used++];
    memset(p, 0, sizeof(*p));
    h.ports_made++;

    return p;
}

VOID DeleteMsgPort(struct MsgPort *port)
{
    (VOID)port;
    h.ports_deleted++;
}

VOID PutMsg(struct MsgPort *port, struct Message *message)
{
    if (h.queued >= H_QUEUE)
        h_abort("fake message queue full");

    h.queue[h.queued].port = port;
    h.queue[h.queued].msg  = message;
    h.queued++;

    if (h.replies < H_REPLIES)
    {
        h.reply[h.replies]    = (struct DosPacket *)message->mn_Node.ln_Name;
        h.reply_to[h.replies] = port;
        h.replies++;
    }
}

struct Message *GetMsg(struct MsgPort *port)
{
    unsigned i;

    for (i = 0; i < h.queued; i++)
    {
        if (h.queue[i].port == port)
        {
            struct Message *m = h.queue[i].msg;

            memmove(&h.queue[i], &h.queue[i + 1],
                    (h.queued - i - 1) * sizeof(h.queue[0]));
            h.queued--;

            return m;
        }
    }

    return NULL;
}

static int h_pending(struct MsgPort *port)
{
    unsigned i;

    for (i = 0; i < h.queued; i++)
    {
        if (h.queue[i].port == port)
            return 1;
    }

    return 0;
}

struct MsgPort *WaitPort(struct MsgPort *port)
{
    if (!h_pending(port))
    {
        h.stalls++;
        if (h.stall != NULL)
            h.stall(port);
    }

    if (!h_pending(port))
        h_abort("a loop waited on a port with nothing queued for it");

    return port;
}

VOID Forbid(VOID) { }
VOID Permit(VOID) { }

VOID ObtainSemaphore(struct SignalSemaphore *sigSem) { (VOID)sigSem; }
VOID ReleaseSemaphore(struct SignalSemaphore *sigSem) { (VOID)sigSem; }

struct Task *FindTask(const char *name)
{
    (VOID)name;

    return &h_proc.pr_Task;
}

VOID Signal(struct Task *task, ULONG signalSet)
{
    (VOID)signalSet;
    h.signals++;
    h.last_signalled = task;
}

ULONG Wait(ULONG signalSet)
{
    return signalSet;
}

ULONG SetSignal(ULONG newSignals, ULONG signalSet)
{
    (VOID)newSignals;
    (VOID)signalSet;

    return 0;
}

struct Library *OpenLibrary(const UBYTE *libName, ULONG version)
{
    (VOID)libName;
    (VOID)version;

    h.opens++;

    return h.open_fails ? NULL : (struct Library *)&h_base;
}

VOID CloseLibrary(struct Library *library)
{
    (VOID)library;
    h.closes++;
}

struct Process *CreateNewProc(const struct TagItem *tags)
{
    static struct Process session;
    const struct TagItem *t;

    for (t = tags; t->ti_Tag != TAG_DONE; t++)
    {
        if (t->ti_Tag == NP_Entry)
            h.last_entry = (APTR)t->ti_Data;
        else if (t->ti_Tag == NP_Name)
            h.last_proc_name = (const char *)t->ti_Data;
        else if (t->ti_Tag == NP_StackSize)
            h.last_stack = t->ti_Data;
    }

    if (h.createproc_fails)
        return NULL;

    h.procs++;
    memset(&session, 0, sizeof(session));

    return &session;
}

struct DosList *LockDosList(ULONG flags)
{
    h.locks++;
    h.last_lock_flags = flags;

    return &h_dos_head;
}

VOID UnLockDosList(ULONG flags)
{
    (VOID)flags;
    h.unlocks++;
}

struct DosList *FindDosEntry(const struct DosList *dlist, const UBYTE *name,
                             ULONG flags)
{
    (VOID)dlist;
    (VOID)name;
    (VOID)flags;

    return h.name_taken ? &h_dos_head : NULL;
}

struct DosList *MakeDosEntry(const UBYTE *name, LONG type)
{
    (VOID)name;
    (VOID)type;

    h.makes++;

    if (h.make_fails)
        return NULL;

    memset(&h_made_node, 0, sizeof(h_made_node));

    return (struct DosList *)&h_made_node;
}

VOID FreeDosEntry(struct DosList *dlist)
{
    (VOID)dlist;
    h.frees++;
}

LONG AddDosEntry(struct DosList *dlist)
{
    (VOID)dlist;
    h.adds++;

    return h.add_fails ? DOSFALSE : DOSTRUE;
}

LONG RemDosEntry(struct DosList *dlist)
{
    (VOID)dlist;
    h.rems++;

    return DOSTRUE;
}

const AmiConfig *netstack_config(VOID)
{
    return &h_cfg;
}

/* AMI_ERROR/WARN/INFO are compiled into every build now rather than out of the
   default one, and tcp_handler.c reaches them on paths this test drives. */
VOID ami_log(int level, const char *fmt, ...)
{
    (VOID)level;
    (VOID)fmt;
}

/* ------------------------------------------------------ socket vectors ---- */

VOID bsd_bzero(APTR p, ULONG size)
{
    memset(p, 0, (size_t)size);
}

VOID bsd_bcopy(const APTR src, APTR dst, ULONG size)
{
    memmove(dst, src, (size_t)size);
}

VOID bsd_strncpy(char *dst, const char *src, ULONG size)
{
    ULONG i;

    if (size == 0)
        return;

    for (i = 0; i + 1 < size && src[i] != '\0'; i++)
        dst[i] = src[i];

    dst[i] = '\0';
}

LONG bsd_Errno(struct AmiSocketBase *base)
{
    (VOID)base;

    return h.errno_value;
}

LONG bsd_socket(LONG domain, LONG type, LONG protocol,
                struct AmiSocketBase *base)
{
    (VOID)domain;
    (VOID)type;
    (VOID)protocol;
    (VOID)base;

    return h.socket_fd;
}

LONG bsd_CloseSocket(LONG fd, struct AmiSocketBase *base)
{
    (VOID)base;

    h.closesockets++;
    h.last_closed_fd = fd;

    return 0;
}

/*
 * MUST NOT BE CALLED from tcp_session_close().  ebe8bab9 removed a
 * bsd_lookup() there whose result was read after bsd_CloseSocket() had
 * potentially freed the AmiSocket; the counter is how that stays removed.
 */
AmiSocket *bsd_lookup(struct AmiSocketBase *base, LONG fd)
{
    (VOID)base;
    (VOID)fd;

    h.lookups++;

    return NULL;
}

LONG bsd_connect(LONG fd, struct sockaddr *name, socklen_t namelen,
                 struct AmiSocketBase *base)
{
    const struct sockaddr_in *sin = (const struct sockaddr_in *)name;

    (VOID)fd;
    (VOID)namelen;
    (VOID)base;

    h.connects++;
    h.last_connect_addr = sin->sin_addr.s_addr;
    h.last_connect_port = (UWORD)BSD_NTOHS(sin->sin_port);

    return h.connect_result;
}

LONG bsd_bind(LONG fd, struct sockaddr *name, socklen_t namelen,
              struct AmiSocketBase *base)
{
    const struct sockaddr_in *sin = (const struct sockaddr_in *)name;

    (VOID)fd;
    (VOID)namelen;
    (VOID)base;

    h.binds++;
    h.last_bind_port = (UWORD)BSD_NTOHS(sin->sin_port);

    return h.bind_result;
}

LONG bsd_listen(LONG fd, LONG backlog, struct AmiSocketBase *base)
{
    (VOID)fd;
    (VOID)backlog;
    (VOID)base;

    h.listens++;

    return h.listen_result;
}

LONG bsd_accept(LONG fd, struct sockaddr *addr, socklen_t *addrlen,
                struct AmiSocketBase *base)
{
    (VOID)fd;
    (VOID)addr;
    (VOID)addrlen;
    (VOID)base;

    h.accepts++;

    return h.accept_fd;
}

LONG bsd_setsockopt(LONG fd, LONG level, LONG optname, APTR optval,
                    socklen_t optlen, struct AmiSocketBase *base)
{
    (VOID)fd;
    (VOID)level;
    (VOID)optname;
    (VOID)optval;
    (VOID)optlen;
    (VOID)base;

    h.setsockopts++;

    return 0;
}

LONG bsd_ObtainSocket(LONG id, LONG domain, LONG type, LONG protocol,
                      struct AmiSocketBase *base)
{
    (VOID)id;
    (VOID)domain;
    (VOID)type;
    (VOID)protocol;
    (VOID)base;

    return h.obtain_fd;
}

LONG bsd_WaitSelect(LONG nfds, APTR rd, APTR wr, APTR ex,
                    struct timeval *timeout, ULONG *sigmask,
                    struct AmiSocketBase *base)
{
    (VOID)nfds;
    (VOID)rd;
    (VOID)wr;
    (VOID)ex;
    (VOID)sigmask;
    (VOID)base;

    h.selects++;
    h.had_timeout = (timeout != NULL) ? TRUE : FALSE;
    if (timeout != NULL)
        h.last_timeout = *timeout;

    return h.select_result;
}

LONG bsd_recv(LONG fd, APTR buf, LONG len, LONG flags,
              struct AmiSocketBase *base)
{
    LONG n;

    (VOID)fd;
    (VOID)flags;
    (VOID)base;

    if (h.recv_calls >= h.recv_planned)
        h_abort("bsd_recv() called more often than the test planned");

    n = h.recv_plan[h.recv_calls++];
    if (n > 0)
        memset(buf, 'x', (size_t)((n < len) ? n : len));

    return n;
}

LONG bsd_send(LONG fd, APTR buf, LONG len, LONG flags,
              struct AmiSocketBase *base)
{
    LONG n;

    (VOID)fd;
    (VOID)buf;
    (VOID)len;
    (VOID)flags;
    (VOID)base;

    if (h.send_calls >= h.send_planned)
        h_abort("bsd_send() called more often than the test planned");

    n = h.send_plan[h.send_calls++];
    if (n > 0)
        h.sent_total += (ULONG)n;

    return n;
}

struct servent *bsd_getservbyname(STRPTR name, STRPTR proto,
                                  struct AmiSocketBase *base)
{
    static struct servent se;
    static char           sname[16];

    (VOID)proto;
    (VOID)base;

    if (!h.service_known)
        return NULL;

    bsd_strncpy(sname, (const char *)name, sizeof(sname));
    memset(&se, 0, sizeof(se));
    se.s_name = sname;
    se.s_port = (short)BSD_HTONS(h.service_port);

    return &se;
}

struct hostent *bsd_gethostbyname(STRPTR name, struct AmiSocketBase *base)
{
    static struct hostent he;
    static ULONG          addr;
    static char          *list[2];

    (VOID)name;
    (VOID)base;

    if (!h.host_known)
        return NULL;

    addr    = h.host_addr;
    list[0] = (char *)&addr;
    list[1] = NULL;

    memset(&he, 0, sizeof(he));
    he.h_length    = 4;
    he.h_addr_list = list;

    return &he;
}

in_addr_t bsd_inet_addr(STRPTR cp, struct AmiSocketBase *base)
{
    (VOID)cp;
    (VOID)base;

    return h.inet_addr_result;
}

/* The shipping translation unit, whole. */
#include "tcp_handler.c"

/* --------------------------------------------------------------- help ---- */

static void h_reset(void)
{
    memset(&h, 0, sizeof(h));
    memset(&h_cfg, 0, sizeof(h_cfg));
    memset(&h_base, 0, sizeof(h_base));
    memset(&h_dos_head, 0, sizeof(h_dos_head));
    memset(&h_proc, 0, sizeof(h_proc));

    h_ports_used = 0;
    h_proc.pr_Task.tc_Node.ln_Type = NT_PROCESS;

    h_cfg.tcp_handler = TRUE;

    h.socket_fd        = 3;
    h.accept_fd        = 4;
    h.obtain_fd        = 5;
    h.select_result    = 1;
    h.inet_addr_result = (in_addr_t)-1;   /* not a dotted quad by default */

    /* The file-scope state tcp_handler.c keeps between calls. */
    tcp_ctrl_port = NULL;
    tcp_node      = NULL;
    tcp_boot      = NULL;
    tcp_sessions  = 0;
    tcp_started   = FALSE;
}

/* A DOS packet and the Message that carries it, in one object. */
typedef struct HPacket
{
    struct Message   msg;
    struct DosPacket pkt;
} HPacket;

static HPacket h_pool[H_PACKETS];
static unsigned h_pool_used;

static struct MsgPort h_caller_port;

static HPacket *h_packet(LONG type, LONG a1, LONG a2, LONG a3)
{
    HPacket *p;

    if (h_pool_used >= H_PACKETS)
        h_abort("out of fake DOS packets");

    p = &h_pool[h_pool_used++];
    memset(p, 0, sizeof(*p));

    p->msg.mn_Node.ln_Name = (char *)&p->pkt;
    p->pkt.dp_Link         = &p->msg;
    p->pkt.dp_Port         = &h_caller_port;
    p->pkt.dp_Type         = type;
    p->pkt.dp_Arg1         = a1;
    p->pkt.dp_Arg2         = a2;
    p->pkt.dp_Arg3         = a3;

    return p;
}

static void h_send(struct MsgPort *port, HPacket *p)
{
    if (h.queued >= H_QUEUE)
        h_abort("fake message queue full");

    h.queue[h.queued].port = port;
    h.queue[h.queued].msg  = &p->msg;
    h.queued++;
}

/* A BSTR the handler can BADDR().  Four-aligned, which BADDR needs. */
static BSTR h_bstr(const char *s)
{
    static UBYTE buf[4][260] __attribute__((aligned(4)));
    static unsigned n;
    UBYTE *b = buf[n++ & 3];
    size_t len = strlen(s);

    if (len > 255)
        len = 255;

    b[0] = (UBYTE)len;
    memcpy(&b[1], s, len);

    return (BSTR)MKBADDR(b);
}

static HPacket *h_after[16];
static unsigned h_after_n;
static unsigned h_stall_wave;
static HPacket *h_auto_die;
static LONG     h_sessions_at_stall;

/* Forget any script and any wave the last test left behind. */
static void h_script_reset(void)
{
    h_after_n           = 0;
    h_stall_wave        = 0;
    h_auto_die          = NULL;
    h_sessions_at_stall = -1;
}

/*
 * Answer the port a loop made for itself: the script cannot be queued before
 * the loop runs, because tcp_ctrl_publish() and tcp_session_open() are what
 * create the port.
 *
 * A loop that asks a second time is one whose ACTION_DIE was refused because a
 * session was still open.  That is a state several tests want to reach, so the
 * second wave drops the count and sends another DIE rather than leaving the
 * loop with nothing; h_auto_die is that packet, and its reply is an assertion
 * in its own right.
 */
static void h_deliver_after(struct MsgPort *port)
{
    unsigned i;

    if (h_stall_wave++ == 0 && h_after_n > 0)
    {
        for (i = 0; i < h_after_n; i++)
            h_send(port, h_after[i]);

        h_after_n = 0;
        return;
    }

    if (port != tcp_ctrl_port)
        h_abort("a session loop ran out of script without an ACTION_END");

    h_sessions_at_stall = tcp_sessions;
    tcp_sessions        = 0;
    h_auto_die          = h_packet(ACTION_DIE, 0, 0, 0);
    h_send(port, h_auto_die);
}

/* -------------------------------------------------------- the grammar ---- */

static void t_parse(void)
{
    TcpName n;

    printf("TCP: the path grammar\n");

    h_reset();

    CHECK(tcp_parse("TCP:example.com/telnet", &n) == 0 &&
          strcmp(n.tn_Host, "example.com") == 0 &&
          strcmp(n.tn_Service, "telnet") == 0,
          "TCP:host/service");

    CHECK(tcp_parse("TCP:smtp", &n) == 0 &&
          n.tn_Host[0] == '\0' && strcmp(n.tn_Service, "smtp") == 0,
          "one bare word is the service, which is how a listener is opened");

    CHECK(tcp_parse("TCP:HOST=a.b/PORT=25", &n) == 0 &&
          strcmp(n.tn_Host, "a.b") == 0 && strcmp(n.tn_Service, "25") == 0,
          "HOST= and PORT= keywords");

    CHECK(tcp_parse("TCP:h=a.b/s=25", &n) == 0 &&
          strcmp(n.tn_Host, "a.b") == 0 && strcmp(n.tn_Service, "25") == 0,
          "their one-letter forms, and lower case");

    CHECK(tcp_parse("TCP:SERVICE=25/H=a.b", &n) == 0 &&
          strcmp(n.tn_Host, "a.b") == 0 && strcmp(n.tn_Service, "25") == 0,
          "SERVICE= as well, in either order");

    CHECK(tcp_parse("TCP:host//service", &n) == 0 &&
          strcmp(n.tn_Host, "host") == 0 &&
          strcmp(n.tn_Service, "service") == 0,
          "an empty component is skipped");

    CHECK(tcp_parse("example.com/telnet", &n) == 0 &&
          strcmp(n.tn_Host, "example.com") == 0,
          "the device name is optional, so a relative path parses");

    CHECK(tcp_parse("TCP:OBTAIN=7", &n) == 0 &&
          n.tn_HasObtain == TRUE && n.tn_Obtain == 7,
          "OBTAIN= takes a released descriptor");

    CHECK(tcp_parse("TCP:O=7", &n) == 0 && n.tn_Obtain == 7,
          "and its one-letter form");

    CHECK(tcp_parse("TCP:OBTAIN=7/HOST=a.b", &n) == 0 &&
          n.tn_HasObtain == TRUE,
          "OBTAIN= takes precedence over a host that is also named");

    CHECK(tcp_parse("TCP:OBTAIN=7/a/b/c", &n) == ERROR_OBJECT_NOT_FOUND,
          "but the rest of the path is still parsed, so nonsense is refused");

    CHECK(tcp_parse("TCP:OBTAIN=x", &n) == ERROR_BAD_NUMBER,
          "a non-numeric OBTAIN= is a bad number, not a missing object");

    CHECK(tcp_parse("TCP:OBTAIN=-1", &n) == ERROR_BAD_NUMBER,
          "and so is a negative one");

    CHECK(tcp_parse("TCP:OBTAIN=99999999", &n) == ERROR_BAD_NUMBER,
          "and one too large to be a descriptor");

    CHECK(tcp_parse("TCP:", &n) == ERROR_OBJECT_NOT_FOUND,
          "no service is not a thing that can be opened");

    CHECK(tcp_parse("TCP:HOST=a.b", &n) == ERROR_OBJECT_NOT_FOUND,
          "a host with no service either");

    CHECK(tcp_parse("TCP:a/b/c", &n) == ERROR_OBJECT_NOT_FOUND,
          "three bare words are refused");

    CHECK(tcp_parse("TCP:HOST=a/b/c", &n) == ERROR_OBJECT_NOT_FOUND,
          "and so is a bare pair on top of a keyword");

    CHECK(tcp_parse("TCP:WHAT=1/25", &n) == ERROR_OBJECT_NOT_FOUND,
          "an unknown keyword is refused rather than ignored");

    {
        char big[TCP_HOST_MAX + 8];

        memset(big, 'a', sizeof(big) - 1);
        big[sizeof(big) - 1] = '\0';

        CHECK(tcp_parse(big, &n) == ERROR_OBJECT_NOT_FOUND,
              "a component longer than the buffer is refused, not truncated");
    }
}

static void t_error_map(void)
{
    printf("TCP: errno to DOS error\n");

    h_reset();

    CHECK(tcp_dos_error(AMI_ENOMEM)        == ERROR_NO_FREE_STORE,   "ENOMEM");
    CHECK(tcp_dos_error(AMI_ENOBUFS)       == ERROR_NO_FREE_STORE,   "ENOBUFS");
    CHECK(tcp_dos_error(AMI_EMFILE)        == ERROR_NO_FREE_STORE,   "EMFILE");
    CHECK(tcp_dos_error(AMI_EADDRINUSE)    == ERROR_OBJECT_IN_USE,   "EADDRINUSE");
    CHECK(tcp_dos_error(AMI_ENETDOWN)      == ERROR_DEVICE_NOT_MOUNTED, "ENETDOWN");
    CHECK(tcp_dos_error(AMI_EHOSTUNREACH)  == ERROR_DEVICE_NOT_MOUNTED, "EHOSTUNREACH");
    CHECK(tcp_dos_error(AMI_EACCES)        == ERROR_READ_PROTECTED,  "EACCES");
    CHECK(tcp_dos_error(AMI_ECONNREFUSED)  == ERROR_OBJECT_NOT_FOUND,
          "an errno with no row falls back to object not found");
}

static void t_bstr(void)
{
    char out[8];

    printf("TCP: the BSTR conversion\n");

    h_reset();

    tcp_bstr_to_c(h_bstr("TCP:x"), out, sizeof(out));
    CHECK(strcmp(out, "TCP:x") == 0, "a short name comes across whole");

    tcp_bstr_to_c(h_bstr("0123456789"), out, sizeof(out));
    CHECK(strcmp(out, "0123456") == 0 && out[7] == '\0',
          "a long one is truncated and still terminated");

    out[0] = 'z';
    tcp_bstr_to_c((BSTR)0, out, sizeof(out));
    CHECK(out[0] == '\0', "a null BSTR gives an empty string, not low memory");
}

/* ------------------------------------------------- the device vocabulary -- */

static struct DosPacket *h_reply_of(HPacket *p)
{
    unsigned i;

    for (i = 0; i < h.replies; i++)
    {
        if (h.reply[i] == &p->pkt)
            return &p->pkt;
    }

    return NULL;
}

/* Whether the packet went back to the port its caller named, which is what
   a reply is; a FIND that is handed to a session process goes elsewhere. */
static int h_answered(HPacket *p)
{
    unsigned i;

    for (i = 0; i < h.replies; i++)
    {
        if (h.reply[i] == &p->pkt && h.reply_to[i] == &h_caller_port)
            return 1;
    }

    return 0;
}

/*
 * Run the control loop.  tcp_ctrl_main() publishes TCP: itself and only then
 * has a port, so the script is delivered from the stall hook the first time
 * the loop asks that port for a packet.  Every script has to end in
 * ACTION_DIE or the loop never returns.
 */
static TcpBoot h_boot;

static void h_run_ctrl_with(void (*stall)(struct MsgPort *port))
{
    h_boot.tb_Parent = &h_proc.pr_Task;
    h_boot.tb_Ok     = FALSE;
    tcp_boot         = &h_boot;

    h.stall = stall;

    tcp_ctrl_main();

    tcp_boot = NULL;
}

static void h_run_ctrl(void)
{
    h_run_ctrl_with(h_deliver_after);
}

static void t_device_packets(void)
{
    HPacket *fs, *disk, *info, *locate, *examine, *next, *parent, *flush;
    HPacket *unknown, *die;

    printf("TCP: what the device answers\n");

    h_reset();
    h_pool_used = 0;
    h_script_reset();

    fs      = h_packet(ACTION_IS_FILESYSTEM, 0, 0, 0);
    disk    = h_packet(ACTION_DISK_INFO, 0, 0, 0);
    info    = h_packet(ACTION_INFO, 0, 0, 0);
    locate  = h_packet(ACTION_LOCATE_OBJECT, 0, 0, 0);
    examine = h_packet(ACTION_EXAMINE_OBJECT, 0, 0, 0);
    next    = h_packet(ACTION_EXAMINE_NEXT, 0, 0, 0);
    parent  = h_packet(ACTION_PARENT, 0, 0, 0);
    flush   = h_packet(ACTION_FLUSH, 0, 0, 0);
    unknown = h_packet(ACTION_SEEK, 0, 0, 0);
    die     = h_packet(ACTION_DIE, 0, 0, 0);

    h_after[h_after_n++] = fs;
    h_after[h_after_n++] = disk;
    h_after[h_after_n++] = info;
    h_after[h_after_n++] = locate;
    h_after[h_after_n++] = examine;
    h_after[h_after_n++] = next;
    h_after[h_after_n++] = parent;
    h_after[h_after_n++] = flush;
    h_after[h_after_n++] = unknown;
    h_after[h_after_n++] = die;

    h_run_ctrl();

    CHECK(h_boot.tb_Ok == TRUE, "the parent was told the handler is up");
    CHECK(h.signals == 1 && h.last_signalled == &h_proc.pr_Task,
          "and told exactly once, on its own task");
    CHECK(h.adds == 1, "the device node went on the DOS list");

    CHECK(h_reply_of(fs) != NULL && fs->pkt.dp_Res1 == DOSFALSE &&
          fs->pkt.dp_Res2 == 0,
          "IS_FILESYSTEM is DOSFALSE with dp_Res2 zero: a stream, not a disk");

    CHECK(disk->pkt.dp_Res1 == DOSFALSE &&
          disk->pkt.dp_Res2 == ERROR_ACTION_NOT_KNOWN,
          "DISK_INFO is refused, which is what keeps TCP: off the Workbench "
          "and out of Info");

    CHECK(info->pkt.dp_Res1 == DOSFALSE &&
          info->pkt.dp_Res2 == ERROR_ACTION_NOT_KNOWN,
          "and so is INFO");

    CHECK(locate->pkt.dp_Res1 == DOSFALSE &&
          locate->pkt.dp_Res2 == ERROR_ACTION_NOT_KNOWN,
          "LOCATE_OBJECT is action not known, so Copy carries on and opens "
          "the stream");

    CHECK(examine->pkt.dp_Res1 == DOSFALSE &&
          examine->pkt.dp_Res2 == ERROR_OBJECT_WRONG_TYPE &&
          next->pkt.dp_Res2    == ERROR_OBJECT_WRONG_TYPE &&
          parent->pkt.dp_Res2  == ERROR_OBJECT_WRONG_TYPE,
          "the directory packets get a reason of their own");

    CHECK(flush->pkt.dp_Res1 == DOSTRUE, "FLUSH succeeds");

    CHECK(unknown->pkt.dp_Res1 == DOSFALSE &&
          unknown->pkt.dp_Res2 == ERROR_ACTION_NOT_KNOWN,
          "anything else is action not known");

    CHECK(die->pkt.dp_Res1 == DOSTRUE, "DIE succeeds when nothing is open");
    CHECK(h.rems == 1 && h.frees == 1,
          "and takes the device node off the list and frees it");
    CHECK(tcp_node == NULL && tcp_ctrl_port == NULL, "and forgets both");
    CHECK(tcp_started == FALSE, "and clears the started flag");
    CHECK(bsd_tcp_handler_alive() == FALSE, "so the handler is not alive");
    CHECK(h.ports_deleted == 1, "the control port is deleted exactly once");

    /* Every reply carries the port the handler chose, not the port of
       whichever process happened to answer. */
    CHECK(fs->pkt.dp_Port != NULL && fs->pkt.dp_Port != &h_caller_port,
          "a reply carries the handler's port back, not the caller's");
}

static void t_die_refused_while_open(void)
{
    HPacket *die, *flush;

    printf("TCP: DIE while a session is open\n");

    h_reset();
    h_pool_used = 0;
    h_script_reset();

    tcp_sessions = 1;

    die   = h_packet(ACTION_DIE, 0, 0, 0);
    flush = h_packet(ACTION_FLUSH, 0, 0, 0);

    h_after[h_after_n++] = die;
    h_after[h_after_n++] = flush;

    h_run_ctrl();

    CHECK(die->pkt.dp_Res1 == DOSFALSE &&
          die->pkt.dp_Res2 == ERROR_OBJECT_IN_USE,
          "DIE is refused while a session is open");
    CHECK(flush->pkt.dp_Res1 == DOSTRUE,
          "and the handler carries on answering after refusing");
    CHECK(h_auto_die != NULL && h_auto_die->pkt.dp_Res1 == DOSTRUE,
          "the next DIE, with the count back at zero, is taken");
    CHECK(h.rems == 1, "and only then does the device node leave the DOS list");
    CHECK(bsd_tcp_handler_alive() == FALSE, "and the handler is gone");
}

static void t_publish_refusals(void)
{
    printf("TCP: publishing refusals\n");

    /* Something is already assigned to TCP:. */
    h_reset();
    h.name_taken = TRUE;

    CHECK(tcp_ctrl_publish() == FALSE, "an assigned TCP: is not overwritten");
    CHECK(h.makes == 0, "no device node is made");
    CHECK(h.ports_made == 1 && h.ports_deleted == 1,
          "and the port it opened is deleted again");
    CHECK(tcp_ctrl_port == NULL, "leaving nothing behind");

    /* MakeDosEntry() fails. */
    h_reset();
    h.make_fails = TRUE;

    CHECK(tcp_ctrl_publish() == FALSE, "no memory for a device node");
    CHECK(h.ports_deleted == 1, "the port is deleted");
    CHECK(tcp_ctrl_port == NULL, "and forgotten");

    /* AddDosEntry() loses the race. */
    h_reset();
    h.add_fails = TRUE;

    CHECK(tcp_ctrl_publish() == FALSE, "the name was taken between the two");
    CHECK(h.frees == 1, "the device node is freed");
    CHECK(h.ports_deleted == 1, "the port is deleted");
    CHECK(tcp_node == NULL && tcp_ctrl_port == NULL, "and both forgotten");

    /* A publish that fails must leave tcp_started clear, or the handler can
       never be started again. */
    h_reset();
    h_pool_used = 0;
    h_script_reset();
    h.name_taken = TRUE;
    tcp_started  = TRUE;

    h_run_ctrl();

    CHECK(h_boot.tb_Ok == FALSE, "the parent is told the publish failed");
    CHECK(h.signals == 1, "and told, rather than left waiting");
    CHECK(tcp_started == FALSE,
          "a handler that never published does not stay marked started");
}

static void t_ctrl_find(void)
{
    HPacket *find, *die;

    printf("TCP: the FIND packet on the device\n");

    h_reset();
    h_pool_used = 0;
    h_script_reset();

    find = h_packet(ACTION_FINDINPUT, 0, 0, (LONG)h_bstr("TCP:host/telnet"));
    die  = h_packet(ACTION_DIE, 0, 0, 0);

    h_after[h_after_n++] = find;
    h_after[h_after_n++] = die;

    h_run_ctrl();

    CHECK(h.procs == 1, "a session process is started");
    CHECK(h.last_entry == (APTR)tcp_session_main,
          "running tcp_session_main()");
    CHECK(h.last_stack == TCP_SESSION_STACK, "on the session stack size");
    CHECK(h_answered(find) == 0,
          "the FIND packet is handed to the session, not answered by the "
          "device");
    CHECK(h_sessions_at_stall == 1,
          "the session count goes up before the process exists, so a DIE "
          "cannot take the handler down under it");
    CHECK(die->pkt.dp_Res1 == DOSFALSE &&
          die->pkt.dp_Res2 == ERROR_OBJECT_IN_USE,
          "and the DIE behind it is refused");

    /* And the count goes back down when the process cannot be made. */
    h_reset();
    h_pool_used = 0;
    h_script_reset();
    h.createproc_fails = TRUE;

    find = h_packet(ACTION_FINDOUTPUT, 0, 0, (LONG)h_bstr("TCP:host/telnet"));
    die  = h_packet(ACTION_DIE, 0, 0, 0);
    h_after[h_after_n++] = find;
    h_after[h_after_n++] = die;

    h_run_ctrl();

    CHECK(tcp_sessions == 0,
          "a process that could not start gives its slot back");
    CHECK(find->pkt.dp_Res1 == DOSFALSE &&
          find->pkt.dp_Res2 == ERROR_NO_FREE_STORE,
          "and the open is refused");
    CHECK(die->pkt.dp_Res1 == DOSTRUE, "so the DIE behind it is taken");
}

/* ------------------------------------------------- the handle vocabulary -- */

static struct FileHandle h_fh __attribute__((aligned(4)));

/* Queue a FIND on the session process's port, then whatever follows on the
   port the session makes for itself. */
static HPacket *h_open_session(const char *path)
{
    HPacket *find = h_packet(ACTION_FINDINPUT, (LONG)MKBADDR(&h_fh), 0,
                             (LONG)h_bstr(path));

    memset(&h_fh, 0, sizeof(h_fh));
    h_send(&h_proc.pr_MsgPort, find);

    /* tcp_ctrl_find() raises the count before the process exists; this drives
       the session process on its own, so it raises it here instead. */
    tcp_sessions = 1;

    return find;
}

static void t_session_open(void)
{
    HPacket *find, *end;

    printf("TCP: opening a file handle\n");

    h_reset();
    h_pool_used = 0;
    h_script_reset();
    h.stall     = h_deliver_after;
    h.service_known = TRUE;
    h.service_port  = 23;
    h.host_known    = TRUE;
    h.host_addr     = 0x0A000001UL;

    find = h_open_session("TCP:host/telnet");
    end  = h_packet(ACTION_END, 0, 0, 0);
    h_after[h_after_n++] = end;

    tcp_session_main();

    CHECK(find->pkt.dp_Res1 == DOSTRUE, "the open succeeds");
    CHECK(h_fh.fh_Arg1 != 0, "the handle carries the session");
    CHECK(h_fh.fh_Type != NULL && h_fh.fh_Type == h_fh.fh_Port,
          "and points at the port the session listens on");
    CHECK(h.connects == 1 && h.last_connect_port == 23,
          "connected to the resolved service port");
    CHECK(h.last_connect_addr == 0x0A000001UL, "and the resolved address");
    CHECK(h.opens == 1 && h.closes == 1,
          "the session took a base of its own and gave it back");
    CHECK(h.closesockets == 1, "and closed its socket");
    CHECK(h.lookups == 0,
          "and did not look the socket up after closing it");
    CHECK(end->pkt.dp_Res1 == DOSTRUE, "END succeeds");
    CHECK(tcp_sessions == 0,
          "the session count is dropped on the way out");
}

static void t_session_open_refusals(void)
{
    HPacket *find;

    printf("TCP: opens that are refused\n");

    /* A name that does not parse. */
    h_reset();
    h_pool_used = 0;
    h.stall = NULL;

    find = h_open_session("TCP:");
    tcp_session_main();

    CHECK(find->pkt.dp_Res1 == DOSFALSE &&
          find->pkt.dp_Res2 == ERROR_OBJECT_NOT_FOUND,
          "an unparseable name is refused");
    CHECK(h.opens == 0, "without opening a base");
    CHECK(h.ports_made == 0, "or a port");

    /* No such service. */
    h_reset();
    h_pool_used = 0;
    h.stall = NULL;
    h.service_known = FALSE;

    find = h_open_session("TCP:host/nosuch");
    tcp_session_main();

    CHECK(find->pkt.dp_Res1 == DOSFALSE &&
          find->pkt.dp_Res2 == ERROR_OBJECT_NOT_FOUND,
          "an unknown service is refused");
    CHECK(h.opens == 1 && h.closes == 1, "and the base is given back");
    CHECK(h.ports_deleted == 1, "and the port deleted");

    /* A numeric service needs no database. */
    h_reset();
    h_pool_used = 0;
    h_script_reset();
    h.stall = h_deliver_after;
    h.service_known = FALSE;
    h.inet_addr_result = (in_addr_t)0x0A000002UL;

    find = h_open_session("TCP:10.0.0.2/8080");
    h_after[h_after_n++] = h_packet(ACTION_END, 0, 0, 0);
    tcp_session_main();

    CHECK(find->pkt.dp_Res1 == DOSTRUE, "a numeric port needs no service");
    CHECK(h.last_connect_port == 8080, "and is used as given");
    CHECK(h.last_connect_addr == 0x0A000002UL,
          "a dotted quad is not sent to the resolver");

    /* connect() fails. */
    h_reset();
    h_pool_used = 0;
    h.stall = NULL;
    h.service_known = TRUE;
    h.service_port  = 23;
    h.host_known    = TRUE;
    h.connect_result = -1;
    h.errno_value    = AMI_ECONNREFUSED;

    find = h_open_session("TCP:host/telnet");
    tcp_session_main();

    CHECK(find->pkt.dp_Res1 == DOSFALSE &&
          find->pkt.dp_Res2 == ERROR_OBJECT_NOT_FOUND,
          "a refused connection is reported");
    CHECK(h.closesockets == 1, "and the socket is closed");

    /* OBTAIN= for a descriptor nobody released. */
    h_reset();
    h_pool_used = 0;
    h.stall = NULL;
    h.obtain_fd   = -1;
    h.errno_value = AMI_ENOENT;

    find = h_open_session("TCP:OBTAIN=7");
    tcp_session_main();

    CHECK(find->pkt.dp_Res1 == DOSFALSE &&
          find->pkt.dp_Res2 == ERROR_OBJECT_NOT_FOUND,
          "OBTAIN= for a descriptor nobody released is not found");
}

static void t_session_listen(void)
{
    HPacket *find;

    printf("TCP: a handle with no host listens\n");

    h_reset();
    h_pool_used = 0;
    h_script_reset();
    h.stall = h_deliver_after;
    h.service_known = TRUE;
    h.service_port  = 7070;

    find = h_open_session("TCP:7070");
    h_after[h_after_n++] = h_packet(ACTION_END, 0, 0, 0);

    tcp_session_main();

    CHECK(find->pkt.dp_Res1 == DOSTRUE, "the listener opens");
    CHECK(h.setsockopts == 1, "SO_REUSEADDR is set before the bind");
    CHECK(h.binds == 1 && h.last_bind_port == 7070, "bound to the service");
    CHECK(h.listens == 1, "and listening");
    CHECK(h.selects == 1 && h.had_timeout == FALSE,
          "the wait for a caller has no timeout");
    CHECK(h.accepts == 1, "one connection is accepted");
    CHECK(h.closesockets == 2,
          "the listening socket is closed as well as the accepted one");
    CHECK(h.connects == 0, "and nothing was connected");
}

static void t_session_io(void)
{
    HPacket *find, *rd, *rd0, *rderr, *wr, *wrshort, *wrerr, *seek, *fh;
    HPacket *sig, *end;
    static char buf[64] __attribute__((aligned(4)));

    printf("TCP: reading and writing a handle\n");

    h_reset();
    h_pool_used = 0;
    h_script_reset();
    h.stall     = h_deliver_after;
    h.service_known = TRUE;
    h.service_port  = 23;
    h.host_known    = TRUE;

    h.recv_plan[0] = 12;    h.recv_plan[1] = 0;   h.recv_plan[2] = -1;
    h.recv_planned = 3;
    /* One short write that has to be resumed, then a failure part way in. */
    h.send_plan[0] = 4;     h.send_plan[1] = 6;   h.send_plan[2] = -1;
    h.send_planned = 3;

    find    = h_open_session("TCP:host/telnet");

    rd      = h_packet(ACTION_READ,  0, (LONG)buf, 64);
    rd0     = h_packet(ACTION_READ,  0, (LONG)buf, 64);
    rderr   = h_packet(ACTION_READ,  0, (LONG)buf, 64);
    wr      = h_packet(ACTION_WRITE, 0, (LONG)buf, 10);
    wrerr   = h_packet(ACTION_WRITE, 0, (LONG)buf, 10);
    wrshort = h_packet(ACTION_READ,  0, (LONG)buf, 0);
    seek    = h_packet(ACTION_SEEK,  0, 0, 0);
    fh      = h_packet(ACTION_EXAMINE_FH, 0, 0, 0);
    sig     = h_packet(ACTION_CHANGE_SIGNAL, 0, 0, 0);
    end     = h_packet(ACTION_END, 0, 0, 0);

    h.errno_value = AMI_ENETDOWN;

    h_after[h_after_n++] = rd;
    h_after[h_after_n++] = rd0;
    h_after[h_after_n++] = rderr;
    h_after[h_after_n++] = wr;
    h_after[h_after_n++] = wrerr;
    h_after[h_after_n++] = wrshort;
    h_after[h_after_n++] = seek;
    h_after[h_after_n++] = end;

    tcp_session_main();

    CHECK(find->pkt.dp_Res1 == DOSTRUE, "the handle opened");

    CHECK(rd->pkt.dp_Res1 == 12 && rd->pkt.dp_Res2 == 0,
          "a read reports the bytes it got");
    CHECK(rd0->pkt.dp_Res1 == 0 && rd0->pkt.dp_Res2 == 0,
          "an orderly close is end of file, not an error");
    CHECK(rderr->pkt.dp_Res1 == -1 &&
          rderr->pkt.dp_Res2 == ERROR_DEVICE_NOT_MOUNTED,
          "a failed read maps its errno");

    CHECK(wr->pkt.dp_Res1 == 10 && h.sent_total == 10,
          "a short write is resumed until the whole buffer is gone");
    CHECK(h.send_calls == 3,
          "two calls for the ten bytes and one for the write that failed");
    CHECK(wrerr->pkt.dp_Res1 == -1 &&
          wrerr->pkt.dp_Res2 == ERROR_DEVICE_NOT_MOUNTED,
          "a failed write maps its errno");

    CHECK(wrshort->pkt.dp_Res1 == 0,
          "a zero length read is zero bytes and no error");

    CHECK(seek->pkt.dp_Res1 == -1 && seek->pkt.dp_Res2 == ERROR_SEEK_ERROR,
          "a stream cannot seek");

    (VOID)fh;
    (VOID)sig;
}

/*
 * WaitForChar() hands the handler a signed microsecond count.  A nonpositive
 * one is a poll; casting it to ULONG first made -1 into a 4294 second wait and
 * an invalid timeout looked like a hung TCP: file (c88bdbb8).
 */
static void t_wait_char(void)
{
    HPacket *find, *w1, *w2, *w3, *end;

    printf("TCP: WAIT_CHAR timeouts\n");

    h_reset();
    h_pool_used = 0;
    h_script_reset();
    h.stall     = h_deliver_after;
    h.service_known = TRUE;
    h.service_port  = 23;
    h.host_known    = TRUE;
    h.select_result = 0;

    find = h_open_session("TCP:host/telnet");

    w1  = h_packet(ACTION_WAIT_CHAR, 2500000L, 0, 0);
    w2  = h_packet(ACTION_WAIT_CHAR, -1L, 0, 0);
    w3  = h_packet(ACTION_WAIT_CHAR, 0L, 0, 0);
    end = h_packet(ACTION_END, 0, 0, 0);

    h_after[h_after_n++] = w1;
    h_after[h_after_n++] = w2;
    h_after[h_after_n++] = w3;
    h_after[h_after_n++] = end;

    /* The open takes one select of its own only when it listens; this one
       connects, so every select below is a WAIT_CHAR. */
    tcp_session_main();

    CHECK(find->pkt.dp_Res1 == DOSTRUE, "the handle opened");
    CHECK(h.selects == 3, "each WAIT_CHAR asked once");
    CHECK(w1->pkt.dp_Res1 == DOSFALSE,
          "a timeout with nothing readable is DOSFALSE");
    CHECK(w3->pkt.dp_Res1 == DOSFALSE, "and so is a poll");
    CHECK(h.had_timeout == TRUE, "every WAIT_CHAR carries a timeout");
    CHECK(h.last_timeout.tv_secs == 0 && h.last_timeout.tv_micro == 0,
          "and a zero one is a poll");
    (VOID)w2;
}

/* The -1 case on its own, because the last timeout is what the harness keeps. */
static void t_wait_char_negative(void)
{
    HPacket *find, *w, *end;

    printf("TCP: WAIT_CHAR with a negative timeout\n");

    h_reset();
    h_pool_used = 0;
    h_script_reset();
    h.stall     = h_deliver_after;
    h.service_known = TRUE;
    h.service_port  = 23;
    h.host_known    = TRUE;
    h.select_result = 0;

    find = h_open_session("TCP:host/telnet");
    w    = h_packet(ACTION_WAIT_CHAR, -1L, 0, 0);
    end  = h_packet(ACTION_END, 0, 0, 0);

    h_after[h_after_n++] = w;
    h_after[h_after_n++] = end;

    tcp_session_main();

    CHECK(find->pkt.dp_Res1 == DOSTRUE, "the handle opened");
    CHECK(h.last_timeout.tv_secs == 0 && h.last_timeout.tv_micro == 0,
          "a negative timeout polls rather than waiting 4294 seconds");
    CHECK(w->pkt.dp_Res1 == DOSFALSE, "and answers DOSFALSE");
}

static void t_handle_misc_packets(void)
{
    HPacket *find, *isfs, *flush, *sig, *examine, *unknown, *end;

    printf("TCP: the rest of the handle vocabulary\n");

    h_reset();
    h_pool_used = 0;
    h_script_reset();
    h.stall     = h_deliver_after;
    h.service_known = TRUE;
    h.service_port  = 23;
    h.host_known    = TRUE;

    find    = h_open_session("TCP:host/telnet");
    isfs    = h_packet(ACTION_IS_FILESYSTEM, 0, 0, 0);
    flush   = h_packet(ACTION_FLUSH, 0, 0, 0);
    sig     = h_packet(ACTION_CHANGE_SIGNAL, 0, 0, 0);
    examine = h_packet(ACTION_EXAMINE_FH, 0, 0, 0);
    unknown = h_packet(ACTION_LOCATE_OBJECT, 0, 0, 0);
    end     = h_packet(ACTION_END, 0, 0, 0);

    h_after[h_after_n++] = isfs;
    h_after[h_after_n++] = flush;
    h_after[h_after_n++] = sig;
    h_after[h_after_n++] = examine;
    h_after[h_after_n++] = unknown;
    h_after[h_after_n++] = end;

    tcp_session_main();

    CHECK(find->pkt.dp_Res1 == DOSTRUE, "the handle opened");
    CHECK(isfs->pkt.dp_Res1 == DOSFALSE && isfs->pkt.dp_Res2 == 0,
          "IS_FILESYSTEM on a handle is DOSFALSE with no error");
    CHECK(flush->pkt.dp_Res1 == DOSTRUE, "FLUSH succeeds");
    CHECK(sig->pkt.dp_Res1 == DOSTRUE, "CHANGE_SIGNAL succeeds");
    CHECK(examine->pkt.dp_Res1 == DOSFALSE &&
          examine->pkt.dp_Res2 == ERROR_OBJECT_WRONG_TYPE,
          "EXAMINE_FH says a stream is the wrong type");
    CHECK(unknown->pkt.dp_Res1 == DOSFALSE &&
          unknown->pkt.dp_Res2 == ERROR_ACTION_NOT_KNOWN,
          "anything else on a handle is action not known");
}

/* ------------------------------------------------------------- start ----- */

static void t_start(void)
{
    printf("TCP: bsd_tcp_handler_start()\n");

    /* Switched off in the configuration. */
    h_reset();
    h_cfg.tcp_handler = FALSE;

    bsd_tcp_handler_start(&h_base);

    CHECK(h.procs == 0, "TCP=OFF starts no process");
    CHECK(tcp_started == FALSE, "and claims nothing");

    /* Switched on. */
    h_reset();
    bsd_tcp_handler_start(&h_base);

    CHECK(h.procs == 1, "TCP=ON starts the control process");
    CHECK(h.last_entry == (APTR)tcp_ctrl_main, "running tcp_ctrl_main()");
    CHECK(h.last_stack == TCP_CTRL_STACK, "on the control stack size");
    CHECK(tcp_started == TRUE, "and records that it did");

    /* A second open must not start a second handler. */
    bsd_tcp_handler_start(&h_base);
    CHECK(h.procs == 1, "a second library open starts no second handler");

    /* The process cannot be made. */
    h_reset();
    h.createproc_fails = TRUE;

    bsd_tcp_handler_start(&h_base);

    CHECK(tcp_started == FALSE,
          "a handler that could not start does not stay marked started");
    CHECK(tcp_boot == NULL, "and the boot record is not left dangling");

    /* Called from a plain Task rather than a Process. */
    h_reset();
    h_proc.pr_Task.tc_Node.ln_Type = NT_TASK;

    bsd_tcp_handler_start(&h_base);

    CHECK(h.procs == 0, "nothing is started from a Task");
    CHECK(tcp_started == FALSE, "and nothing is claimed");
}

int main(void)
{
    printf("tcp_handler.c host checks\n\n");

    t_parse();
    t_error_map();
    t_bstr();
    t_device_packets();
    t_die_refused_while_open();
    t_publish_refusals();
    t_ctrl_find();
    t_session_open();
    t_session_open_refusals();
    t_session_listen();
    t_session_io();
    t_wait_char();
    t_wait_char_negative();
    t_handle_misc_packets();
    t_start();

    printf("\n%lu checks, %lu failures\n", h_checks, h_failures);

    return (h_failures == 0) ? 0 : 1;
}

/*
 * clients/dropbear -- the AmigaOS half of the Dropbear port.
 *
 * Everything `dbclient` calls that this toolchain does not have, in one
 * translation unit.  curl did not need this file: its own lib/curl_setup.h
 * already knows that on classic AmigaOS a socket is not a file descriptor,
 * that close() is CloseSocket() and that select() is WaitSelect().  Dropbear
 * knows none of that -- `grep -ril amiga src/` finds nothing -- which is the
 * situation docs/RESEARCH.md §11.7 predicted for wget.
 *
 * Nothing in third_party/dropbear is patched.  The whole port is this file,
 * two shim headers, a localoptions.h and the flags in build.sh.
 *
 * Two descriptor spaces, made into one.  bsdsocket.library hands out socket
 * descriptors from its own table starting at 0 (src/bsdsocket/socket.c,
 * bsd_fd_alloc()).  newlib hands out file descriptors starting at 0 as well,
 * and 0/1/2 are the Shell's stdin/stdout/stderr.  So `socket()` can and does
 * return 0, and a program holding both -- which every SSH client is, since it
 * copies between a socket and a terminal -- cannot tell them apart.
 *
 * Rather than teach Dropbear the difference, which is a patch to every call
 * site rebased forever, the two spaces are merged here by offset:
 *
 *     fd  0 ..  63     newlib: stdin, stdout, stderr, and files
 *     fd 64 .. 191     bsdsocket, biased by DB_SOCK_BASE
 *     fd 192, 193      the wakeup "pipe" -- see pipe() below
 *
 * Every function that takes a descriptor dispatches on that range, so
 * Dropbear's read()/write()/close()/select()/fcntl() do the right thing for
 * whatever it hands them, and its `int` is an `int` everywhere.
 *
 * read/write/close exist in newlib and cannot simply be redefined, so they are
 * taken with `-Wl,--wrap`; the linker sends Dropbear's references to
 * __wrap_read and leaves __real_read for us.  That also survives
 * `atomicio(read, ...)`, which passes read as a function pointer, where a
 * macro would not have.
 *
 * FD_SETSIZE is 256 (build.sh), because 192 has to fit in an fd_set and
 * newlib's default is 64.  dbutil.c's dropbear_fd_set() checks the bound
 * itself, so an overflow is a legible error and not a smashed stack.
 *
 * The socket calls are the Roadshow NDK inlines, the same 121 vectors `fetch`
 * and curl use.
 *
 * fork(), vfork(), execv(), setsid(), dup(), dup2() and kill() fail with
 * ENOSYS.  AmigaOS has no fork and pretending otherwise would produce a client
 * that looks like it worked.  dbclient reaches them only through -J proxycmd
 * and the SSH_ASKPASS helper, both compiled out in localoptions.h, so a linked
 * dbclient contains the stubs and calls none.
 *
 * tcgetattr()/tcsetattr() fabricate a termios and drive raw mode with
 * SetMode().  AmigaOS has no termios, but a console has the one capability an
 * interactive session needs -- SetMode(handle, 1) is raw -- so tcgetattr()
 * reports a plausible cooked terminal and tcsetattr() maps Dropbear's ICANON
 * switch onto SetMode().  A non-console fd still returns ENOTTY, so
 * `dbclient -T user@host command` still asks the server for no pty.
 *
 * pipe() is a ring buffer between two of this program's own descriptors, and it
 * has a second form: either end can instead be a struct FileHandle whose
 * fh_Type is a MsgPort of ours, so a spawned command's Read() and Write() are
 * DOS packets this process answers.  That is what carries a conversation --
 * scp's protocol is one -- between an SSH channel and an AmigaDOS command with
 * no PIPE: handler on the volume.  See db_pipe_fh() and the runner below.
 *
 * dbutil.c's spawn_command() is three pipe()s and a fork(), and the child
 * branch redirects the pipes onto 0/1/2 and calls execv().  There is no fork()
 * here, so the whole function is replaced with __wrap_spawn_command() below,
 * the same -Wl,--wrap mechanism the file already uses for read/write/close,
 * and for the same reason: it leaves third_party/dropbear unpatched.
 *
 * The command line is not guessed and not parsed out of a log.  The wrapper
 * calls Dropbear's own exec_fn (execchild), which does the real work -- forced
 * commands, the environment, the chdir to the home directory -- and ends in
 * execv(shell, {"sh", "-c", cmd}).  execv() then longjmp()s back into the
 * wrapper with that command in hand; the wrapper's frame is live, because
 * exec_fn was called from it.
 *
 * SPDX-License-Identifier: MIT
 */

#include <exec/types.h>
#include <exec/io.h>            /* struct IOStdReq -- the console ConUnit */
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/dostags.h>        /* SystemTagList() tags */
#include <devices/conunit.h>    /* struct ConUnit, cu_XMax/cu_YMax */
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/bsdsocket.h>

#include <sys/types.h>
#include <sys/time.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>              /* snprintf() */
#include <stdlib.h>
#include <stdarg.h>
#include <setjmp.h>             /* the execv() -> spawn_command() jump */
#include <signal.h>
#include <sys/stat.h>           /* chmod() */
#include <sys/wait.h>           /* waitpid() */

#include <grp.h>                /* getgrnam() */

#include "aminetxduo/random.h"

/* Must match DROPBEAR_URANDOM_DEV in clients/dropbear/localoptions.h. */
#define AMIGA_URANDOM_DEV  "RANDOM:"

/* ------------------------------------------------------------------------ */
/* The NDK inlines are macros with the plain BSD names, so a function called
   socket() cannot be written while they are visible.  Each one is captured in
   a static wrapper here, the macros are then #undef'd, and the public
   functions at the bottom of the file call the wrappers.  */

static LONG nx_socket(LONG d, LONG t, LONG p)          { return socket(d, t, p); }
static LONG nx_bind(LONG s, APTR n, LONG l)            { return bind(s, n, l); }
static LONG nx_listen(LONG s, LONG b)                  { return listen(s, b); }
static LONG nx_accept(LONG s, APTR n, APTR l)          { return accept(s, n, l); }
static LONG nx_connect(LONG s, APTR n, LONG l)         { return connect(s, n, l); }
static LONG nx_send(LONG s, APTR b, LONG l, LONG f)    { return send(s, b, l, f); }
static LONG nx_recv(LONG s, APTR b, LONG l, LONG f)    { return recv(s, b, l, f); }
static LONG nx_shutdown(LONG s, LONG h)                { return shutdown(s, h); }
static LONG nx_setsockopt(LONG s, LONG lv, LONG o, APTR v, LONG l)
                                                       { return setsockopt(s, lv, o, v, l); }
static LONG nx_getsockopt(LONG s, LONG lv, LONG o, APTR v, APTR l)
                                                       { return getsockopt(s, lv, o, v, l); }
static LONG nx_getsockname(LONG s, APTR n, APTR l)     { return getsockname(s, n, l); }
static LONG nx_getpeername(LONG s, APTR n, APTR l)     { return getpeername(s, n, l); }
static LONG nx_ioctlsocket(LONG s, ULONG r, APTR a)    { return IoctlSocket(s, r, a); }
static LONG nx_closesocket(LONG s)                     { return CloseSocket(s); }
static LONG nx_waitselect(LONG n, APTR r, APTR w, APTR e, APTR t, ULONG *sigs)
                                                       { return WaitSelect(n, r, w, e,
                                                                (struct __timeval *)t, sigs); }
static struct hostent *nx_gethostbyname(APTR n)        { return gethostbyname(n); }
static struct hostent *nx_gethostbyaddr(APTR a, LONG l, LONG t)
                                                       { return gethostbyaddr(a, l, t); }
static struct servent *nx_getservbyname(APTR n, APTR p) { return getservbyname(n, p); }
static char *nx_inet_ntoa(ULONG ip)                    { return (char *)Inet_NtoA(ip); }
static ULONG nx_inet_addr(APTR cp)                     { return inet_addr(cp); }
static LONG nx_socketbasetaglist(APTR tags)            { return SocketBaseTagList(tags); }

#undef socket
#undef bind
#undef listen
#undef accept
#undef connect
#undef send
#undef recv
#undef sendto
#undef recvfrom
#undef shutdown
#undef setsockopt
#undef getsockopt
#undef getsockname
#undef getpeername
#undef gethostbyname
#undef gethostbyaddr
#undef getservbyname
#undef inet_addr
#undef inet_aton
#undef inet_ntoa
#undef select

#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <pwd.h>
#include <sys/resource.h>
#include <sys/select.h>
#include <fcntl.h>
#include <termios.h>
#include <sys/filio.h>
#include <sys/ioctl.h>          /* TIOCGWINSZ, struct winsize */
#include <dos/dostags.h>


/* ------------------------------------------------------ the descriptor map */

#define DB_SOCK_BASE    64
#define DB_SOCK_LIMIT   192
#define DB_PIPE_BASE    192
#define DB_PIPE_PAIRS   8
#define DB_PIPE_LIMIT   (DB_PIPE_BASE + 2 * DB_PIPE_PAIRS)
#define DB_RAND_FD      DB_PIPE_LIMIT

#define IS_SOCK(fd)     ((fd) >= DB_SOCK_BASE && (fd) < DB_SOCK_LIMIT)
#define IS_PIPE(fd)     ((fd) >= DB_PIPE_BASE && (fd) < DB_PIPE_LIMIT)
#define IS_RAND(fd)     ((fd) == DB_RAND_FD)

#define SOCKOF(fd)      ((LONG)((fd) - DB_SOCK_BASE))

/* Set while __wrap_spawn_command() is running Dropbear's own child path in
   this process -- see the spawn_command() note at the top of the file, and
   the two places below that have to know: close() and execv(). */
static int exec_capturing;

/*
 * The pipe table.  Declared here rather than beside pipe(), because read(),
 * write(), close() and select() all come first in the file and all four have
 * to see it.
 *
 * Two kinds of pipe are needed; they are the same object with different fields
 * set:
 *
 *   a wakeup pipe   ses.signal_pipe: one byte in, one byte out.  Lossy, because
 *                   a second byte says nothing the first did not and refusing
 *                   one would stall the writer instead.
 *   a DOS pipe      the far end is a struct FileHandle held by a spawned
 *                   command, and this process answers its ACTION_READ and
 *                   ACTION_WRITE packets out of `buf`.  Flow-controlled, since a
 *                   command's bytes are not droppable.  This is the one that
 *                   makes scp work; see the block comment above db_pipe_fh().
 *
 * There used to be a third, a file source: the command's output was collected in
 * a T: file and the read end drained it after the command had exited.  That is
 * what a synchronous SystemTagList() forces and it cannot carry a conversation,
 * so it is gone along with the sink that was the command's stdin.
 *
 * `buf` is a ring, not the compacting queue it was: fine for a wakeup pipe
 * carrying one byte, quadratic for a DOS pipe carrying a file, because every
 * Read() memmove()d the remainder down.
 *
 * 4 KB because scp's protocol writes in blocks and a ring smaller than a block
 * turns one Write() into several packet round trips.  Eight pairs of it is 32 KB
 * of BSS.
 */
#define DB_PIPE_BUF     4096

struct db_pipe
{
    unsigned      taken   : 1;
    unsigned      rclosed : 1;      /* the read end has been close()d */
    unsigned      wclosed : 1;      /* the write end has been close()d */
    unsigned      lossy   : 1;      /* overflow drops rather than refuses */
    unsigned      dosread : 1;      /* the DOS end reads (command's stdin) */
    unsigned      doswrite: 1;      /* the DOS end writes (command's stdout) */
    unsigned      dosend  : 1;      /* the DOS end has sent ACTION_END */
    struct DosPacket *held;         /* a packet this pipe cannot answer yet */
    int           count;            /* bytes in the ring */
    int           rd;               /* ring read cursor  */
    int           wr;               /* ring write cursor */
    unsigned char buf[DB_PIPE_BUF];
};

static struct db_pipe db_pipes[DB_PIPE_PAIRS];

#define PIPE_PAIR(fd)       (&db_pipes[((fd) - DB_PIPE_BASE) / 2])
#define PIPE_INDEX(fd)      (((fd) - DB_PIPE_BASE) / 2)
#define PIPE_IS_READ(fd)    ((((fd) - DB_PIPE_BASE) & 1) == 0)

/* ------------------------------------------------------------- the ring --- */

static int ring_used(const struct db_pipe *p)  { return p->count; }
static int ring_free(const struct db_pipe *p)  { return DB_PIPE_BUF - p->count; }

static int ring_put(struct db_pipe *p, const unsigned char *src, int n)
{
    int done = 0;

    if (n > ring_free(p))
        n = ring_free(p);

    while (done < n)
    {
        int run = DB_PIPE_BUF - p->wr;

        if (run > n - done)
            run = n - done;
        memcpy(p->buf + p->wr, src + done, (size_t)run);
        p->wr = (p->wr + run) % DB_PIPE_BUF;
        done += run;
    }
    p->count += done;

    return done;
}

static int ring_get(struct db_pipe *p, unsigned char *dst, int n)
{
    int done = 0;

    if (n > ring_used(p))
        n = ring_used(p);

    while (done < n)
    {
        int run = DB_PIPE_BUF - p->rd;

        if (run > n - done)
            run = n - done;
        memcpy(dst + done, p->buf + p->rd, (size_t)run);
        p->rd = (p->rd + run) % DB_PIPE_BUF;
        done += run;
    }
    p->count -= done;

    return done;
}

/* ---------------------------------------------------------- the DOS pipe --- */

/*
 * A pipe a spawned command can use as its stdin or stdout, with no PIPE:
 * handler on the boot volume and no second process to run it.
 *
 * AmigaDOS I/O is packets.  Read() on a file handle sends ACTION_READ to the
 * MsgPort in fh_Type and sleeps until somebody replies; Write() sends
 * ACTION_WRITE.  Nothing says that port has to belong to a filesystem, so this
 * process answers them itself: AllocDosObject(DOS_FILEHANDLE) gives a handle,
 * fh_Type points at one port of ours and fh_Arg1 carries the db_pipe index, and
 * a command handed that handle by SystemTagList() is talking to the ring above.
 * src/bsdsocket/tcp_handler.c does the same for TCP:; the difference is that
 * this one needs no session process, because a ring buffer never blocks the way
 * a name lookup does.
 *
 * A packet is answered when it can be and not before -- that is what makes it a
 * pipe rather than a file.  An ACTION_READ on an empty ring, or an ACTION_WRITE
 * on a full one, is parked in p->held and retried whenever the other side moves
 * bytes (db_pipe_retry) or closes.  One held packet per pipe is enough: the
 * command is one process and it is inside one Read() at a time.
 *
 * Servicing happens from __wrap_select(), which is where Dropbear waits anyway,
 * and the port's signal goes into the same WaitSelect() mask as the socket. So a
 * command's Read() wakes the session loop exactly as channel data does.
 */
static struct MsgPort *db_pipe_port;

static int db_pipe_dos_open(const struct db_pipe *p)
{
    return (p->dosread || p->doswrite) && !p->dosend;
}

static struct MsgPort *db_pipe_port_get(VOID)
{
    if (db_pipe_port == NULL)
    {
        db_pipe_port = CreateMsgPort();
        if (db_pipe_port == NULL)
            return NULL;
    }
    return db_pipe_port;
}

/* The signal WaitSelect() has to include so an arriving packet wakes us. */
static ULONG db_pipe_sigmask(VOID)
{
    if (db_pipe_port == NULL)
        return 0;
    return 1UL << (ULONG)db_pipe_port->mp_SigBit;
}

/*
 * Reply a packet.  Not ReplyPkt(): dp_Port has to name the port the packet
 * comes back to us on, which is ours, and ReplyPkt() stamps it with the
 * current process's pr_MsgPort.  The same reason tcp_handler.c has its own.
 */
static VOID db_pipe_reply(struct DosPacket *pkt, LONG res1, LONG res2)
{
    struct MsgPort *reply = pkt->dp_Port;

    pkt->dp_Res1 = res1;
    pkt->dp_Res2 = res2;
    pkt->dp_Port = db_pipe_port;
    PutMsg(reply, pkt->dp_Link);
}

/* Try to answer whatever this pipe has parked.  Called after either side moves
   bytes or closes, and from the service loop. */
static VOID db_pipe_retry(struct db_pipe *p)
{
    struct DosPacket *pkt = p->held;
    LONG              n;

    if (pkt == NULL)
        return;

    /*
     * A pipe with a DOS end has only that end on the far side: the local fd
     * facing the command is left open and unused, so exactly one of wclosed and
     * rclosed can ever be set and each means what it says.  A command reading
     * (its stdin) ends at wclosed, Dropbear having stopped writing; a command
     * writing (its stdout) fails at rclosed, Dropbear having stopped reading.
     */
    if (pkt->dp_Type == ACTION_READ)
    {
        if (ring_used(p) > 0)
        {
            n = (LONG)ring_get(p, (unsigned char *)pkt->dp_Arg2,
                               (int)pkt->dp_Arg3);
        }
        else if (p->wclosed)
        {
            n = 0;                      /* end of file */
        }
        else
        {
            return;                     /* still nothing; keep waiting */
        }
    }
    else                                /* ACTION_WRITE */
    {
        if (ring_free(p) > 0)
        {
            n = (LONG)ring_put(p, (const unsigned char *)pkt->dp_Arg2,
                               (int)pkt->dp_Arg3);
        }
        else if (p->rclosed)
        {
            /* Nobody will ever read it.  -1 with a real error, so a command
               writing into a closed channel fails rather than looping. */
            p->held = NULL;
            db_pipe_reply(pkt, -1, ERROR_INVALID_LOCK);
            return;
        }
        else
        {
            return;
        }
    }

    p->held = NULL;
    db_pipe_reply(pkt, n, 0);
}

/*
 * Drain the port.  Every packet either gets answered now or is parked; a second
 * packet arriving for a pipe that already has one parked is a protocol error on
 * the command's side (one process, one Read() at a time) and is refused rather
 * than overwriting the first.
 */
static VOID db_pipe_service(VOID)
{
    struct Message *msg;

    if (db_pipe_port == NULL)
        return;

    while ((msg = GetMsg(db_pipe_port)) != NULL)
    {
        struct DosPacket *pkt = (struct DosPacket *)msg->mn_Node.ln_Name;
        struct db_pipe   *p;
        LONG              idx;

        /*
         * Only READ, WRITE and END carry fh_Arg1 in dp_Arg1.  The rest carry
         * something else entirely -- ACTION_WAIT_CHAR's dp_Arg1 is a timeout in
         * microseconds -- so they are answered without looking at a pipe at all.
         * That is the limitation one port for many handles brings and the reason
         * src/bsdsocket/tcp_handler.c gives a port to each of its sessions; here
         * nothing needs a per-file answer to any of them.
         */
        switch (pkt->dp_Type)
        {
            case ACTION_READ:
            case ACTION_WRITE:
            case ACTION_END:
                break;

            /* A pipe is not a filesystem and has no position. */
            case ACTION_IS_FILESYSTEM:
                db_pipe_reply(pkt, DOSFALSE, 0);
                continue;

            case ACTION_SEEK:
                db_pipe_reply(pkt, -1, ERROR_SEEK_ERROR);
                continue;

            /* Cannot say which handle is being asked about, so the answer is
               "no character waiting" and the caller comes back with a Read().
               Nothing a spawned command does depends on it. */
            case ACTION_WAIT_CHAR:
                db_pipe_reply(pkt, DOSFALSE, 0);
                continue;

            case ACTION_FLUSH:
            case ACTION_SET_FILE_SIZE:
                db_pipe_reply(pkt, DOSTRUE, 0);
                continue;

            default:
                db_pipe_reply(pkt, DOSFALSE, ERROR_ACTION_NOT_KNOWN);
                continue;
        }

        idx = pkt->dp_Arg1;
        if (idx < 0 || idx >= DB_PIPE_PAIRS)
        {
            db_pipe_reply(pkt, DOSFALSE, ERROR_INVALID_LOCK);
            continue;
        }
        p = &db_pipes[idx];

        switch (pkt->dp_Type)
        {
            case ACTION_READ:
            case ACTION_WRITE:
                if (p->held != NULL)
                {
                    db_pipe_reply(pkt, -1, ERROR_OBJECT_IN_USE);
                    break;
                }
                p->held = pkt;
                db_pipe_retry(p);
                break;

            case ACTION_END:
                p->dosend = 1;
                /* Whatever this end had parked can never be answered now. */
                if (p->held != NULL)
                {
                    struct DosPacket *held = p->held;

                    p->held = NULL;
                    db_pipe_reply(held, -1, ERROR_INVALID_LOCK);
                }
                db_pipe_reply(pkt, DOSTRUE, 0);
                if (p->rclosed && p->wclosed)
                    memset(p, 0, sizeof(*p));
                break;

            default:                    /* unreachable: filtered above */
                db_pipe_reply(pkt, DOSFALSE, ERROR_ACTION_NOT_KNOWN);
                break;
        }
    }
}

/*
 * A DOS file handle on one end of pipe `idx`.  `for_read` means the COMMAND
 * reads it, so it is the command's stdin and Dropbear holds the write end.
 *
 * fh_Arg1 is the pipe index and fh_Type this process's port, which is all
 * db_pipe_service() needs to route a packet.  DOS frees the handle when it is
 * closed, so this must not be freed here.
 */
static BPTR db_pipe_fh(int idx, int for_read)
{
    struct FileHandle *fh;

    if (db_pipe_port_get() == NULL)
        return (BPTR)0;

    fh = (struct FileHandle *)AllocDosObject(DOS_FILEHANDLE, NULL);
    if (fh == NULL)
        return (BPTR)0;

    fh->fh_Type = db_pipe_port;
    fh->fh_Arg1 = (LONG)idx;

    if (for_read)
        db_pipes[idx].dosread = 1;
    else
        db_pipes[idx].doswrite = 1;

    db_pipes[idx].lossy = 0;        /* a command's data is not droppable */

    return MKBADDR(fh);
}

extern void dropbear_log(int priority, const char *format, ...);

/* The few things the runner and the pipe service need before they are
   defined; the rest of each subject stays with its own section. */
#define DB_CMD_MAX      512
#define DB_CMD_STACK    (256UL * 1024UL)
static pid_t child_pid;                 /* 0 when there is nothing to report */
static int   child_status;
static void (*sigchld_handler)(int);


#define DB_LOG_WARNING  4
#define DB_LOG_INFO     6

#define DB_SUCCESS      0
#define DB_FAILURE      (-1)

/* --------------------------------------------------------- the runner --- */

/*
 * SystemTagList() does not return until the command has finished, and the
 * command cannot finish until somebody answers its Read() -- which is this
 * process.  Calling it inline was therefore fine for a command that only ever
 * wrote (its output went to a file and was read back afterwards) and is a
 * deadlock for anything conversational, scp being the whole point.
 *
 * So the synchronous call moves to a Process of its own whose only job is to
 * hold it.  Dropbear returns to its session loop, answers the command's packets
 * from __wrap_select() as they arrive, and learns the return code when the
 * runner signals.  Both file handles are closed by the runner rather than here:
 * closing them is what sends ACTION_END, which is how this side learns the
 * command has stopped reading and stopped writing.
 *
 * The record is handed over through the child's tc_UserData, with SIGF_SINGLE
 * as the handshake, because the child starts running the moment CreateNewProc()
 * returns and a plain global would race the next spawn.  The child waits before
 * it looks.
 */
#define DB_RUNNER_STACK   (16UL * 1024UL)

typedef struct
{
    struct Task *rn_Parent;
    BPTR         rn_In;                 /* the command's stdin  */
    BPTR         rn_Out;                /* the command's stdout */
    LONG         rn_Rc;
    volatile LONG rn_Done;
    char         rn_Cmd[DB_CMD_MAX];
} DbRunner;

static DbRunner *db_runner;             /* the one in flight, or NULL */
static int       child_ready;           /* child_status is a real exit code */

static VOID db_runner_main(VOID)
{
    struct Process *me = (struct Process *)FindTask(NULL);
    DbRunner       *r;
    struct Task    *parent;
    struct TagItem  tags[4];
    int             nt = 0;

    /* The parent is filling tc_UserData; it signals when it is done. */
    (VOID)Wait(SIGF_SINGLE);

    r = (DbRunner *)me->pr_Task.tc_UserData;
    if (r == NULL)
        return;

    tags[nt].ti_Tag = SYS_Input;    tags[nt++].ti_Data = (ULONG)r->rn_In;
    tags[nt].ti_Tag = SYS_Output;   tags[nt++].ti_Data = (ULONG)r->rn_Out;
    tags[nt].ti_Tag = NP_StackSize; tags[nt++].ti_Data = DB_CMD_STACK;
    tags[nt].ti_Tag = TAG_END;      tags[nt].ti_Data   = 0;

    r->rn_Rc = SystemTagList((CONST_STRPTR)r->rn_Cmd, tags);

    /* System() does not close handles it was given, and here that is the point:
       these two closes are the command's end of file in both directions. */
    Close(r->rn_In);
    Close(r->rn_Out);

    /* rn_Done is published last, and nothing in the record is read after it:
       the parent frees the record the moment it sees the flag. */
    parent = r->rn_Parent;
    r->rn_Done = 1;
    Signal(parent, SIGBREAKF_CTRL_E);
}

/*
 * Has the command finished?  Called from __wrap_select() once a pass.  Raising
 * SIGCHLD's handler here is the same sequence a real signal would produce: it
 * writes to ses.signal_pipe, the session loop calls svr_chansess_checksignal(),
 * and waitpid() hands over the status.
 */
static VOID db_runner_poll(VOID)
{
    DbRunner *r = db_runner;

    if (r == NULL || r->rn_Done == 0)
        return;

    if (r->rn_Rc == -1)
    {
        /* System() could not start a Shell at all -- not the command failing,
           which comes back as that command's rc.  127 is what a Unix shell
           reports for the same thing and the only number a client can act on. */
        dropbear_log(DB_LOG_WARNING, "amiga: could not run a shell (IoErr %ld)",
                     (long)IoErr());
        r->rn_Rc = 127;
    }

    child_status = (int)((r->rn_Rc & 0xFF) << 8);
    child_ready  = 1;

    /* The runner has published rn_Done and reads nothing further, so the record
       is ours to give back.  Missing this cost 576 bytes a command. */
    db_runner = NULL;
    FreeVec(r);

    if (sigchld_handler != NULL)
        sigchld_handler(SIGCHLD);
}



static int pipe_readable(int fd)
{
    const struct db_pipe *p = PIPE_PAIR(fd);

    if (!PIPE_IS_READ(fd))
        return 0;

    /* A pipe whose writer has closed is readable: Read() returns 0, and 0 is
       end of file, which is progress. */
    return ring_used(p) > 0 || p->wclosed || (p->doswrite && p->dosend);
}

/* Writable when there is room, or when nothing is ever going to read it. */
static int pipe_writable(int fd)
{
    const struct db_pipe *p = PIPE_PAIR(fd);

    if (PIPE_IS_READ(fd))
        return 0;

    if (p->lossy)
        return 1;

    return ring_free(p) > 0 || p->rclosed || (p->dosread && p->dosend);
}

static int pipe_read(int fd, void *buf, size_t len)
{
    struct db_pipe *p = PIPE_PAIR(fd);

    if (!PIPE_IS_READ(fd)) { errno = EBADF; return -1; }

    if (ring_used(p) > 0)
    {
        int n = ring_get(p, (unsigned char *)buf, (int)len);

        /* Room appeared, so a command blocked in Write() can be answered. */
        db_pipe_retry(p);
        return n;
    }

    if (p->wclosed || (p->doswrite && p->dosend))
        return 0;

    errno = EAGAIN;
    return -1;
}

static int pipe_write(int fd, const void *buf, size_t len)
{
    struct db_pipe *p = PIPE_PAIR(fd);
    int n;

    if (PIPE_IS_READ(fd)) { errno = EBADF; return -1; }

    /* The command that was going to read this has closed its handle.  Report
       the bytes written rather than stall: nothing is ever going to take them,
       and a channel with data pending forever never closes. */
    if (p->dosread && p->dosend)
        return (int)len;

    n = ring_put(p, (const unsigned char *)buf, (int)len);

    if (n > 0)
        db_pipe_retry(p);           /* a command blocked in Read() can go */

    /* A lossy pipe drops the overflow rather than refusing it.  That is the
       wakeup pipe, where a second byte says nothing the first did not and
       refusing would stall the writer instead. */
    if (p->lossy)
        return (int)len;

    if (n == 0)
    {
        errno = EAGAIN;
        return -1;
    }

    return n;                       /* a short write is reported short */
}

static int pipe_close(int fd)
{
    struct db_pipe *p = PIPE_PAIR(fd);

    if (PIPE_IS_READ(fd))
    {
        p->rclosed = 1;
    }
    else
    {
        p->wclosed = 1;
    }

    /* A command asleep on this pipe has to be woken, or it waits for a reply
       that is never coming and the runner never returns.  db_pipe_retry() sees
       the closed flag and answers with end of file. */
    db_pipe_retry(p);

    /* Released only once both ends are closed.  Dropbear closes one end and
       keeps the other, so freeing on the first close would hand the live end
       to the next caller.  A pipe with a DOS end open is not free either: its
       FileHandle still names this slot. */
    if (p->rclosed && p->wclosed && !db_pipe_dos_open(p))
        memset(p, 0, sizeof(*p));

    return 0;
}

extern int __real_read(int fd, void *buf, size_t len);
extern int __real_write(int fd, const void *buf, size_t len);
extern int __real_close(int fd);
extern int __real_open(const char *path, int flags, ...);


/* ----------------------------------------------------- no requesters ---- */

/*
 * dbrandom.c's write_urandom() feeds the pool back to the random device with
 * fopen(DROPBEAR_URANDOM_DEV, "w") and treats the result as opportunistic.
 * On AmigaOS "RANDOM:" looks like a device name, and dos.library's answer to
 * an unmounted device is not an error code but a system requester reading
 *
 *     Please insert volume RANDOM: in any drive
 *
 * with Retry and Cancel buttons.  On a headless emulator run there is nobody
 * to click Cancel, so the process waits forever and the whole run times out
 * with `dbclient -V` as the last thing it printed.  A user running dbclient
 * from a Shell on a real Amiga gets the requester on their screen.
 *
 * pr_WindowPtr = -1 is dos.library's documented "never put up a requester,
 * fail the call instead", and is the right setting for any ported client;
 * every one of them assumes a failed open() returns.  It is set once, in a
 * constructor, so it is in place before main() and therefore before Dropbear's
 * first file access.
 */
__attribute__((constructor)) static void amiga_no_requesters(void)
{
    struct Process *proc = (struct Process *)FindTask(NULL);

    if (proc != NULL && proc->pr_Task.tc_Node.ln_Type == NT_PROCESS)
        proc->pr_WindowPtr = (APTR)-1;
}


/* ------------------------------------------------------------ SocketBase  */

/*
 * Declared by the NDK inlines and dereferenced by every one of them.  It is
 * opened lazily rather than in a constructor for the reason curl's
 * lib/amigaos.c gives: a program that never reaches the network should not
 * fail to start because a library is missing.  SBTC_ERRNOPTR is what makes
 * `errno` after a failed connect() mean anything -- without it every socket
 * error reads as whatever the last stdio call left behind.
 */
struct Library *SocketBase = NULL;

/*
 * A destructor and not atexit().
 *
 * This toolchain's exit() is the crt0's, not newlib's -- crt0.o defines _exit,
 * __exit and exit as one symbol, so libc's exit.o is never even pulled in -- and
 * all it runs is the __EXIT_LIST__.  Nothing in libc.a puts __call_exitprocs()
 * on that list (the only entries are __exitcommandline, __exit_fh and malloc's
 * __free_all), so an atexit() handler in a linked client never runs at all.
 * The DTOR list does: it is what makes dbprofile.c's report appear.
 *
 * Missing this cost 2,776 bytes per process and rather more than that in
 * consequences: with no CloseLibrary() the library never runs
 * bsd_child_destroy(), so the base, its descriptor table, every socket the
 * process opened and its cached ThreadX registration all outlive it -- the last
 * of those pointing at a dead task's stack.  docs/RESEARCH.md 78.2.
 */
__attribute__((destructor)) static void amiga_sock_cleanup(void)
{
    if (SocketBase != NULL)
    {
        CloseLibrary(SocketBase);
        SocketBase = NULL;
    }
}

static int amiga_sock_init(void)
{
    struct TagItem tags[2];

    if (SocketBase != NULL)
        return 0;

    SocketBase = OpenLibrary((CONST_STRPTR)"bsdsocket.library", 4);
    if (SocketBase == NULL)
    {
        errno = ENOSYS;
        return -1;
    }

    tags[0].ti_Tag  = SBTM_SETVAL(SBTC_ERRNOPTR(sizeof(errno)));
    tags[0].ti_Data = (ULONG)&errno;
    tags[1].ti_Tag  = TAG_END;
    tags[1].ti_Data = 0;
    (void)nx_socketbasetaglist(tags);

    return 0;
}

#define NEED_SOCKETS()  do { if (amiga_sock_init() != 0) return -1; } while (0)
#define NEED_SOCKETS_P() do { if (amiga_sock_init() != 0) return NULL; } while (0)


/* --------------------------------------------------------- socket calls -- */

int socket(int domain, int type, int protocol)
{
    LONG s;

    NEED_SOCKETS();

    s = nx_socket(domain, type, protocol);
    if (s < 0)
        return -1;

    /* A socket that lands outside the window would alias a file descriptor
       further up, which is the one failure this whole scheme exists to
       prevent.  Refusing is the only safe answer. */
    if (s >= (LONG)(DB_SOCK_LIMIT - DB_SOCK_BASE))
    {
        (void)nx_closesocket(s);
        errno = EMFILE;
        return -1;
    }

    return (int)s + DB_SOCK_BASE;
}

int bind(int fd, const struct sockaddr *addr, socklen_t len)
{
    if (!IS_SOCK(fd)) { errno = EBADF; return -1; }
    return (int)nx_bind(SOCKOF(fd), (APTR)addr, (LONG)len);
}

int listen(int fd, int backlog)
{
    if (!IS_SOCK(fd)) { errno = EBADF; return -1; }
    return (int)nx_listen(SOCKOF(fd), backlog);
}

/*
 * The one call in here no client needed: dbclient only ever connects.  It is
 * written the same way as socket(), including the out-of-window refusal: an
 * accepted socket comes from the same bsdsocket descriptor space as one from
 * socket(), so it can land outside the window just as easily, and on a busy
 * listener it is more likely to.  Refusing by closing the accepted socket
 * drops that one connection rather than aliasing a file descriptor.
 */
int accept(int fd, struct sockaddr *addr, socklen_t *len)
{
    LONG s;

    if (!IS_SOCK(fd)) { errno = EBADF; return -1; }

    s = nx_accept(SOCKOF(fd), (APTR)addr, (APTR)len);
    if (s < 0)
        return -1;

    if (s >= (LONG)(DB_SOCK_LIMIT - DB_SOCK_BASE))
    {
        (void)nx_closesocket(s);
        errno = EMFILE;
        return -1;
    }

    return (int)s + DB_SOCK_BASE;
}

int connect(int fd, const struct sockaddr *addr, socklen_t len)
{
    if (!IS_SOCK(fd)) { errno = EBADF; return -1; }
    return (int)nx_connect(SOCKOF(fd), (APTR)addr, (LONG)len);
}

int shutdown(int fd, int how)
{
    if (!IS_SOCK(fd)) { errno = ENOTSOCK; return -1; }
    return (int)nx_shutdown(SOCKOF(fd), how);
}

int setsockopt(int fd, int level, int name, const void *val, socklen_t len)
{
    if (!IS_SOCK(fd)) { errno = ENOTSOCK; return -1; }
    return (int)nx_setsockopt(SOCKOF(fd), level, name, (APTR)val, (LONG)len);
}

int getsockopt(int fd, int level, int name, void *val, socklen_t *len)
{
    if (!IS_SOCK(fd)) { errno = ENOTSOCK; return -1; }
    return (int)nx_getsockopt(SOCKOF(fd), level, name, (APTR)val, (APTR)len);
}

int getsockname(int fd, struct sockaddr *addr, socklen_t *len)
{
    if (!IS_SOCK(fd)) { errno = ENOTSOCK; return -1; }
    return (int)nx_getsockname(SOCKOF(fd), (APTR)addr, (APTR)len);
}

int getpeername(int fd, struct sockaddr *addr, socklen_t *len)
{
    if (!IS_SOCK(fd)) { errno = ENOTSOCK; return -1; }
    return (int)nx_getpeername(SOCKOF(fd), (APTR)addr, (APTR)len);
}


/* ------------------------------------------------------------ resolver --- */

struct hostent *gethostbyname(const char *name)
{
    NEED_SOCKETS_P();
    return nx_gethostbyname((APTR)name);
}

struct hostent *gethostbyaddr(const void *addr, socklen_t len, int type)
{
    NEED_SOCKETS_P();
    return nx_gethostbyaddr((APTR)addr, (LONG)len, type);
}

struct servent *getservbyname(const char *name, const char *proto)
{
    NEED_SOCKETS_P();
    return nx_getservbyname((APTR)name, (APTR)proto);
}

char *inet_ntoa(struct in_addr in)
{
    if (amiga_sock_init() != 0)
        return (char *)"0.0.0.0";
    return nx_inet_ntoa((ULONG)in.s_addr);
}

/*
 * inet_addr() cannot distinguish 255.255.255.255 from failure, which is why
 * inet_aton() exists.  bsdsocket.library publishes inet_aton on vector 98, but
 * a Roadshow older than that vector would jump off the end of its own LVO
 * table, so this goes through inet_addr and spells out the one ambiguous
 * address.
 */
int inet_aton(const char *cp, struct in_addr *addr)
{
    ULONG v;

    if (cp == NULL || addr == NULL)
        return 0;

    if (amiga_sock_init() != 0)
        return 0;

    v = nx_inet_addr((APTR)cp);
    if (v == 0xFFFFFFFFUL && strcmp(cp, "255.255.255.255") != 0)
        return 0;

    addr->s_addr = v;
    return 1;
}


/* -------------------------------------------------------- read/write/close */

/*
 * dbrandom.c's seedrandom() opens DROPBEAR_URANDOM_DEV, reads 32 bytes, and
 * dropbear_exit()s if it cannot -- there is no fallback and no way to build
 * without one.  This machine has no /dev/urandom, so localoptions.h renames
 * the device to "RANDOM:" (a name shaped like an AmigaOS device, so nothing
 * pretends to be Unix) and open() answers it here.
 *
 * The bytes come from src/common/ami_random.c, the same generator
 * bsdsocket.library uses for TCP initial sequence numbers and nx_secure uses
 * for TLS key agreement: one entropy story per machine, and one place to fix
 * it.
 *
 * The conditioning is textbook -- SHA-256 mixing, counter-mode expansion,
 * forward ratchet.  The collection is guesswork: it credits itself about 21
 * bits from E-Clock jitter, the clock and the task list against the 64 it sets
 * as its own bar, so ami_random_is_seeded() returns FALSE by construction and
 * include/aminetxduo/random.h says so in its first paragraph.  Several of its
 * sources measured identical across three cold boots under FS-UAE and are
 * credited nothing.
 *
 * For an SSH client that is survivable.  What the client generates from this
 * pool is its ephemeral curve25519 private key and its session cookie, both
 * per-connection, both forward-secret, and neither written down; an attacker
 * who can predict them can read that session.
 *
 * For an SSH server it is not survivable: a host key generated from a 21-bit
 * pool can be enumerated, it persists on disk, and clients pin it, so it
 * defeats the protocol rather than weakening it.  A server on this machine
 * needs its host key generated somewhere else, or a real seed fed in through
 * ami_random_add_entropy() before dropbearkey runs.
 */
static int rand_fd_open = 0;

static int rand_fill(void *buf, size_t len)
{
    static int inited = 0;

    if (!inited)
    {
        ami_random_init();
        inited = 1;
    }

    ami_random_bytes((APTR)buf, (ULONG)len);
    return (int)len;
}

int __wrap_open(const char *path, int flags, ...)
{
    va_list ap;
    int mode = 0;

    path = amiga_fix_path(path);

    if (path != NULL && strcmp(path, AMIGA_URANDOM_DEV) == 0)
    {
        /* Read-only, and only one at a time; nothing here opens two. */
        if ((flags & 3) != 0 || rand_fd_open)
        {
            errno = EACCES;
            return -1;
        }
        rand_fd_open = 1;
        return DB_RAND_FD;
    }

    va_start(ap, flags);
    mode = va_arg(ap, int);
    va_end(ap);

    return __real_open(path, flags, mode);
}

/* The interactive console reader lives further down (it needs select's fd
   helpers); read() and write() here are its callers before then. */
static int con_active(void);
static int con_read(void *buf, size_t len);
static int con_write(int fd, const void *buf, size_t len);

int __wrap_read(int fd, void *buf, size_t len)
{
    if (IS_SOCK(fd))
        return (int)nx_recv(SOCKOF(fd), (APTR)buf, (LONG)len, 0);

    /* stdin, while an interactive session's console reader child is running --
       hand back what it has buffered rather than read the console twice. */
    if (fd == 0 && con_active())
        return con_read(buf, len);

    if (IS_RAND(fd))
        return rand_fill(buf, len);

    if (IS_PIPE(fd))
        return pipe_read(fd, buf, len);

    return __real_read(fd, buf, len);
}

int __wrap_write(int fd, const void *buf, size_t len)
{
    if (IS_SOCK(fd))
        return (int)nx_send(SOCKOF(fd), (APTR)buf, (LONG)len, 0);

    if (IS_PIPE(fd))
        return pipe_write(fd, buf, len);

    if (IS_RAND(fd))
        return (int)len;                /* swallowed: see rand_fill() */

    /* The client's own console output (prompts, messages) before an interactive
       session is up: give it the CR the Amiga console needs.  Once the session
       runs, the server's byte stream (already CRLF) passes straight through. */
    if ((fd == 1 || fd == 2) && !con_active())
        return con_write(fd, buf, len);

    return __real_write(fd, buf, len);
}

/*
 * The real size of the Amiga console.
 *
 * Dropbear tells the server the terminal geometry (put_winsize()), and without
 * an answer here it ships a fixed 80x25, leaving vim, nano, less and every
 * other full-screen program drawing to the wrong size.  The Amiga console will
 * report it: a Window Status Request written to it -- CSI '0' SP 'q' -- comes
 * back through the input stream as a Window Bounds Report,
 * CSI 1;1;<rows>;<cols> SP 'r'.  Raw mode is required to read that reply
 * without waiting for a newline.
 *
 * This runs while the pty request is built, before tcsetattr() starts the
 * interactive reader child, so nothing else is reading the console and the
 * reply is ours.  The con_active() guard refuses to fight the reader should
 * that ever change; the wait is bounded so a console that never answers cannot
 * stall the connection.
 */
static struct ConUnit *con_unit;    /* the console's ConUnit -- stable once found */

/* Dynamic resize.  The reader child polls the ConUnit's live size -- its poll
   already runs every CON_POLL_US for the quit check -- and on a change wakes the
   parent, which hands it to the SIGWINCH handler Dropbear gave us (the only door
   to cli_ses.winchange, which the port cannot reach directly). */
static int          con_last_x, con_last_y;    /* last-seen console size        */
static volatile int con_resized;               /* reader saw the window change  */
static void       (*con_winch)(int);           /* Dropbear's SIGWINCH handler   */

/*
 * Find the console's ConUnit.
 *
 * The console handler answers an ACTION_DISK_INFO packet by putting its
 * console.device IORequest in InfoData.id_InUse; that request's io_Unit is the
 * ConUnit.  The pointer does not move for the life of the window, so it is
 * cached: the first call sends the packet, later calls just return it.
 */
static struct ConUnit *con_get_unit(void)
{
    BPTR               fh;
    struct FileHandle *fhp;
    struct MsgPort    *port;
    struct InfoData   *id;

    if (con_unit != NULL)
        return con_unit;

    fh = Output();
    if (fh == (BPTR)0 || !IsInteractive(fh))
        return NULL;
    fhp  = (struct FileHandle *)BADDR(fh);
    port = fhp->fh_Type;
    if (port == NULL)
        return NULL;

    /* InfoData must be longword aligned for MKBADDR; AllocMem returns it so. */
    id = (struct InfoData *)AllocMem(sizeof(struct InfoData), MEMF_PUBLIC | MEMF_CLEAR);
    if (id == NULL)
        return NULL;

    if (DoPkt(port, ACTION_DISK_INFO, (LONG)MKBADDR(id), 0, 0, 0, 0))
    {
        struct IOStdReq *ios = (struct IOStdReq *)id->id_InUse;
        if (ios != NULL)
            con_unit = (struct ConUnit *)ios->io_Unit;
    }
    FreeMem(id, sizeof(struct InfoData));
    return con_unit;
}

/*
 * The real size of the Amiga console, for TIOCGWINSZ.
 *
 * cu_XMax/cu_YMax are the last column and row (so the count is one more).  This
 * is a plain memory read of a live structure -- no console I/O, so nothing to
 * echo and no bell.  The documented Window Status Request (CSI 0 SP q) would be
 * the portable route, but Kickstart 3.x's CON: handler does not answer it.
 */
static int con_query_size(int *rows, int *cols)
{
    struct ConUnit *cu = con_get_unit();

    if (cu != NULL && cu->cu_XMax > 0 && cu->cu_YMax > 0)
    {
        *cols = cu->cu_XMax + 1;
        *rows = cu->cu_YMax + 1;
        return 0;
    }
    return -1;
}

/*
 * Capture Dropbear's SIGWINCH handler.
 *
 * AmigaOS delivers no Unix signals, so signal() has nothing to install -- but
 * Dropbear's SIGWINCH handler is the only door to cli_ses.winchange, which this
 * file cannot reach directly.  Remember it; the resize path calls it by hand.
 * Return SIG_DFL rather than SIG_ERR so Dropbear does not think it failed.
 */
typedef void (*con_sigfn)(int);

con_sigfn __wrap_signal(int sig, con_sigfn handler)
{
    if (sig == SIGWINCH)
        con_winch = handler;
    return (con_sigfn)0;
}

extern int __real_ioctl(int fd, unsigned long request, ...);

/*
 * Dropbear reaches TIOCGWINSZ through ioctl(); newlib has no answer, so it fell
 * back to 80x25 every time.  Answer it from the console, pass socket ioctls to
 * the library, and leave everything else to newlib.
 */
int __wrap_ioctl(int fd, unsigned long request, ...)
{
    va_list ap;
    void   *arg;

    va_start(ap, request);
    arg = va_arg(ap, void *);
    va_end(ap);

    if (request == TIOCGWINSZ)
    {
        struct winsize *ws = (struct winsize *)arg;
        int rows = 0, cols = 0;

        if (ws == NULL)
        {
            errno = EFAULT;
            return -1;
        }
        if ((fd == 0 || fd == 1 || fd == 2) && con_query_size(&rows, &cols) == 0)
        {
            ws->ws_row = (unsigned short)rows;
            ws->ws_col = (unsigned short)cols;
        }
        else
        {
            ws->ws_row = 25;
            ws->ws_col = 80;
        }
        ws->ws_xpixel = 0;
        ws->ws_ypixel = 0;
        return 0;
    }

    if (IS_SOCK(fd))
        return (int)nx_ioctlsocket(SOCKOF(fd), request, arg);

    return __real_ioctl(fd, request, arg);
}

int __wrap_close(int fd)
{
    /*
     * 0, 1 and 2 are not ours to close, and closing them reboots the machine.
     *
     * On Unix a process owns its own descriptor table and close(1) at exit is
     * free.  On AmigaOS a Shell command does not own Input() and Output():
     * they are the parent's DOS FileHandles, lent for the duration.  newlib's
     * close() Close()s the BPTR, and then the Shell -- or, here,
     * SystemTagList() in ClientRun -- Close()s the same freed FileHandle
     * again.  A double Close() on a machine with no memory protection is not
     * an error return.
     *
     * Dropbear does this: common-channel.c's close_chan_fd() calls m_close()
     * on the session channel's readfd/writefd/errfd, and for
     * `dbclient -T host command` those three are stdin, stdout and stderr.
     *
     * Observed: every pass ran `dbclient ... "echo AMIGA-SSH-OK; uname -a;
     * date"`, wrote the server's real answer into the report, and then the
     * emulated A1200 rebooted before ClientRun could write the return code, so
     * the command list started over from the top forever and the run looked
     * like a hang with a correct transcript inside it.
     *
     * Nothing in a client needs to close the terminal it was given, and writes
     * to a "closed" stdout still have to work because Dropbear may report an
     * error after closing the channel.
     */
    if (fd == 0 || fd == 1 || fd == 2)
        return 0;

    /*
     * Neither is anything else, while a command is being captured.
     *
     * dbutil.c's run_command() closes every descriptor from 3 to ses.maxfd
     * before it execs, so that a child cannot inherit a file the server opened
     * as root.  In a forked child that is right.  Here it runs in the server's
     * own process, and that range is the session socket, the listener's pipes
     * and the pipes the command's output is about to arrive on -- closing them
     * ends the session in the middle of setting it up.
     */
    if (exec_capturing)
        return 0;

    if (IS_SOCK(fd))
        return (int)nx_closesocket(SOCKOF(fd));

    if (IS_RAND(fd))
    {
        rand_fd_open = 0;
        return 0;
    }

    if (IS_PIPE(fd))
        return pipe_close(fd);

    return __real_close(fd);
}


/* ------------------------------------------------------------- fcntl ----- */

/*
 * The only fcntl() a client makes is setnonblocking()'s
 * F_GETFL / F_SETFL|O_NONBLOCK pair.  For a socket that is
 * IoctlSocket(FIONBIO), which is a different call and not a flag word, so
 * F_GETFL answers 0 and F_SETFL does the work.  For a DOS handle there is no
 * non-blocking mode at all, and saying so would make Dropbear exit -- so it
 * succeeds, and select() below is what keeps the promise instead, by never
 * reporting a handle readable unless a read will return.
 */
int fcntl(int fd, int cmd, ...)
{
    if (cmd == F_GETFL)
        return 0;

    if (cmd == F_SETFL)
    {
        if (IS_SOCK(fd))
        {
            LONG on = 1;
            return (int)nx_ioctlsocket(SOCKOF(fd), FIONBIO, &on);
        }
        return 0;
    }

    errno = EINVAL;
    return -1;
}


/* ---------------------------------------------- interactive console reader */

/*
 * WaitSelect() waits on sockets and Exec signals, never on a DOS console, so an
 * interactive session cannot wait on the keyboard and the socket at once
 * through it directly.  A small child process bridges the gap: it blocks on
 * WaitForChar() -- which wakes the instant a key is pressed, so it is not a
 * spin -- reads each byte into a ring the parent drains, and Signal()s the
 * parent.  select() (below) folds that signal into WaitSelect()'s mask, so
 * Dropbear's loop waits on the socket and the keyboard with no polling.
 *
 * Started and stopped by tcsetattr()'s raw/cooked switch, which is Dropbear's
 * cli_tty_setup()/cli_tty_cleanup() pairing: the child lives exactly as long as
 * the raw-mode session, and con_reader_stop() waits for it (cr_DoneSig) before
 * freeing anything it might still Signal, so a stray signal to a dead task
 * cannot happen.
 */
#define CON_RING_SIZE   256U                  /* power of two -- see the mask */
#define CON_POLL_US     100000UL              /* WaitForChar() quit-check bound */

typedef struct
{
    BPTR           cr_Handle;                 /* the console to read (Input()) */
    struct Task   *cr_Parent;
    ULONG          cr_DataSig;                /* parent: a byte is waiting     */
    ULONG          cr_DoneSig;                /* child -> parent: I have stopped */
    volatile ULONG cr_Head;                   /* child writes                  */
    volatile ULONG cr_Tail;                   /* parent reads                  */
    volatile UBYTE cr_Quit;
    volatile UBYTE cr_Eof;
    UBYTE          cr_Buf[CON_RING_SIZE];
} ConReader;

static ConReader   *con_reader;
static BYTE         con_data_bit = -1;
static BYTE         con_done_bit = -1;
static volatile int con_intr;             /* a local Ctrl-C to feed the remote */

static void con_ring_put(ConReader *cr, UBYTE b)
{
    ULONG head = cr->cr_Head;
    ULONG next = (head + 1U) & (CON_RING_SIZE - 1U);

    if (next != cr->cr_Tail)                   /* drop on overflow: typing is slow */
    {
        cr->cr_Buf[head] = b;
        cr->cr_Head = next;
    }
}

/* The child.  Picks cr up from the global, which is set before it is spawned. */
static void con_child(void)
{
    ConReader *cr = con_reader;
    char       c;

    while (!cr->cr_Quit)
    {
        /* A window resize shows up in the ConUnit's live size.  Piggyback on
           this poll (it already runs every CON_POLL_US for the quit check): on
           a change, wake the parent, which calls Dropbear's SIGWINCH handler. */
        if (con_unit != NULL)
        {
            int x = con_unit->cu_XMax, y = con_unit->cu_YMax;

            if (x > 0 && y > 0 && (x != con_last_x || y != con_last_y))
            {
                con_last_x  = x;
                con_last_y  = y;
                con_resized = 1;
                Signal(cr->cr_Parent, cr->cr_DataSig);
            }
        }

        if (!WaitForChar(cr->cr_Handle, CON_POLL_US))
            continue;                          /* nothing yet; re-check cr_Quit */

        if (Read(cr->cr_Handle, &c, 1) != 1)
        {
            cr->cr_Eof = 1;
            Signal(cr->cr_Parent, cr->cr_DataSig);
            break;
        }

        if ((UBYTE)c == 0x9B)
        {
            /* The Amiga console emits an 8-bit CSI (0x9B) to introduce a control
               sequence -- arrow keys are 0x9B 'A'..'D'.  The server, and the
               ncurses "amiga" terminfo, speak the 7-bit form ESC '[', and a lone
               0x9B is not valid UTF-8 either, so rewrite it. */
            con_ring_put(cr, 0x1B);
            con_ring_put(cr, (UBYTE)'[');
        }
        else
        {
            con_ring_put(cr, (UBYTE)c);
        }
        Signal(cr->cr_Parent, cr->cr_DataSig);
    }

    Signal(cr->cr_Parent, cr->cr_DoneSig);     /* last act; then touch nothing */
}

static void con_reader_start(BPTR handle)
{
    ConReader     *cr;
    struct TagItem tags[5];

    if (con_reader != NULL)
        return;

    con_data_bit = AllocSignal(-1);
    con_done_bit = AllocSignal(-1);
    if (con_data_bit < 0 || con_done_bit < 0)
        goto fail;

    cr = (ConReader *)AllocMem(sizeof(*cr), MEMF_PUBLIC | MEMF_CLEAR);
    if (cr == NULL)
        goto fail;

    cr->cr_Handle  = handle;
    cr->cr_Parent  = FindTask(NULL);
    cr->cr_DataSig = 1UL << con_data_bit;
    cr->cr_DoneSig = 1UL << con_done_bit;

    con_reader = cr;

    /* Prime the resize detector (and cache the ConUnit) so the child's first
       poll compares against the real starting size, not zero. */
    {
        struct ConUnit *cu = con_get_unit();
        if (cu != NULL) { con_last_x = cu->cu_XMax; con_last_y = cu->cu_YMax; }
    }

    /* Same priority as us, not higher.  A higher-priority reader that keeps
       finding input -- fast typing -- never blocks, so the parent (this task)
       is never scheduled to drain the ring or service the socket, and the
       session hangs until the typing stops.  Equal priority time-slices the
       two, so the parent always gets the CPU back. */
    tags[0].ti_Tag = NP_Entry;     tags[0].ti_Data = (ULONG)con_child;
    tags[1].ti_Tag = NP_Name;      tags[1].ti_Data = (ULONG)"AmiNetXDuo ssh console";
    tags[2].ti_Tag = NP_StackSize; tags[2].ti_Data = 8192UL;
    tags[3].ti_Tag = NP_Priority;  tags[3].ti_Data = (ULONG)0;
    tags[4].ti_Tag = TAG_END;      tags[4].ti_Data = 0;

    if (CreateNewProc(tags) == NULL)
    {
        con_reader = NULL;
        FreeMem(cr, sizeof(*cr));
        goto fail;
    }
    return;

fail:
    if (con_data_bit >= 0) { FreeSignal(con_data_bit); con_data_bit = -1; }
    if (con_done_bit >= 0) { FreeSignal(con_done_bit); con_done_bit = -1; }
}

static void con_reader_stop(void)
{
    if (con_reader == NULL)
        return;

    con_reader->cr_Quit = 1;
    Wait(con_reader->cr_DoneSig);              /* the child's final signal      */

    FreeMem(con_reader, sizeof(*con_reader));
    con_reader = NULL;
    FreeSignal(con_data_bit); con_data_bit = -1;
    FreeSignal(con_done_bit); con_done_bit = -1;
}

static int con_active(void)
{
    return con_reader != NULL;
}

static int con_readable(void)
{
    ConReader *cr = con_reader;
    return cr != NULL && (con_intr || cr->cr_Tail != cr->cr_Head || cr->cr_Eof);
}

/* The parent's read() of the console: drain the ring the child fills. */
static int con_read(void *buf, size_t len)
{
    ConReader *cr = con_reader;
    UBYTE     *out = (UBYTE *)buf;
    int        n = 0;

    /* A Ctrl-C the local Shell raised goes to the remote as a literal ^C rather
       than aborting the client -- select() sets con_intr; see there. */
    if (con_intr && (size_t)n < len)
    {
        con_intr = 0;
        out[n++] = 0x03;
    }

    while ((size_t)n < len && cr->cr_Tail != cr->cr_Head)
    {
        out[n++] = cr->cr_Buf[cr->cr_Tail];
        cr->cr_Tail = (cr->cr_Tail + 1U) & (CON_RING_SIZE - 1U);
    }

    if (n == 0)
    {
        if (cr->cr_Eof)
            return 0;
        errno = EWOULDBLOCK;
        return -1;
    }
    return n;
}

/*
 * Console output with the newline the Amiga console wants.
 *
 * Dropbear is Unix code and ends its own lines -- the password prompt, "Host
 * key...", "Permission denied" -- with a bare '\n', but the Amiga console only
 * moves to the start of the next line on CR+LF, so those prompts pile up on one
 * line.  Insert a CR before any LF that does not already have one (so the
 * server's own CRLF, once the session is up, is never doubled).  Only the
 * client's own output -- fd 1/2 while no interactive session is running -- goes
 * through here; the session's byte stream is passed straight through untouched.
 */
static char con_wr_prev[3];               /* last byte written per fd, for CRLF */

static int con_write(int fd, const void *buf, size_t len)
{
    const char *in   = (const char *)buf;
    char        out[256];
    ULONG       o    = 0;
    size_t      i;
    char        prev = (fd >= 0 && fd < 3) ? con_wr_prev[fd] : 0;

    for (i = 0; i < len; i++)
    {
        char c = in[i];

        if (o >= sizeof(out) - 2)       /* room for a CR + the byte */
        {
            __real_write(fd, out, o);
            o = 0;
        }

        if (c == '\n' && prev != '\r')
            out[o++] = '\r';
        out[o++] = c;
        prev = c;
    }

    if (o > 0)
        __real_write(fd, out, o);
    if (fd >= 0 && fd < 3)
        con_wr_prev[fd] = prev;

    return (int)len;
}


/* -------------------------------------------------------------- select --- */

/*
 * WaitSelect() understands sockets and nothing else, so the mixed set has to
 * be taken apart.  A DOS handle's readiness is answered without blocking:
 *
 *   not interactive (a file, or NIL:)  -- always readable.  read() returns
 *                                         data or 0 for end of file, and
 *                                         either is progress.
 *   interactive (a console)            -- WaitForChar(handle, 0).
 *   any handle, for writing            -- always writable.  A DOS Write() to
 *                                         a console or a file completes.
 *
 * If anything off-socket is already ready, WaitSelect() is called with a zero
 * timeout so the answer is not delayed behind the socket.
 *
 * An interactive stdin is only noticed when WaitSelect() next returns, so a
 * console keystroke can wait as long as select_timeout().  That is why
 * `dbclient -T ... command` is the supported shape and an interactive session
 * is not.  Doing better needs the same mechanism a telnet server needs: a DOS
 * handler that makes a Shell look like a socket, which is scoped separately.
 */
static BPTR dos_handle_for(int fd)
{
    switch (fd)
    {
        case 0:  return Input();
        case 1:  return Output();
        case 2:  return Output();
        default: return (BPTR)0;
    }
}

static int dos_readable(int fd)
{
    BPTR h = dos_handle_for(fd);

    if (h == (BPTR)0)
        return 1;                       /* a plain file: a read will return */

    if (!IsInteractive(h))
        return 1;

    return WaitForChar(h, 0) ? 1 : 0;
}

int select(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds,
           struct timeval *timeout)
{
    fd_set sock_r, sock_w;
    fd_set out_r, out_w;
    struct timeval tv_zero;
    struct timeval *tv;
    LONG  sock_n = 0;
    int   ready = 0;
    int   other_ready = 0;
    int   have_sockets = 0;
    int   con_watch = 0;              /* the interactive console is in readfds */
    int   con_fd = -1;
    int   pipe_watch = 0;             /* a spawned command is on a pipe */
    int   fd;
    LONG  rc;

    /* Answer whatever a spawned command has asked for, so the readiness this
       pass reports is current rather than one pass stale, and notice a command
       that has finished. */
    db_pipe_service();
    db_runner_poll();

    FD_ZERO(&sock_r);
    FD_ZERO(&sock_w);
    FD_ZERO(&out_r);
    FD_ZERO(&out_w);

    if (nfds > DB_PIPE_LIMIT)
        nfds = DB_PIPE_LIMIT;

    for (fd = 0; fd < nfds; fd++)
    {
        int want_r = (readfds  != NULL) && FD_ISSET(fd, readfds);
        int want_w = (writefds != NULL) && FD_ISSET(fd, writefds);

        if (!want_r && !want_w)
            continue;

        if (IS_SOCK(fd))
        {
            LONG s = SOCKOF(fd);

            if (want_r) FD_SET((int)s, &sock_r);
            if (want_w) FD_SET((int)s, &sock_w);
            if (s + 1 > sock_n)
                sock_n = s + 1;
            have_sockets = 1;
            continue;
        }

        if (IS_PIPE(fd))
        {
            if (want_r && pipe_readable(fd)) { FD_SET(fd, &out_r); other_ready++; }
            if (want_w && pipe_writable(fd)) { FD_SET(fd, &out_w); other_ready++; }

            /* A pipe with a command on the far end can become ready without
               anything on this side happening, so its port has to be in the
               wait mask below.  One with no DOS end never does. */
            if (db_pipe_dos_open(PIPE_PAIR(fd)))
                pipe_watch = 1;
            continue;
        }

        /* The interactive console is read by the child reader, not here: its
           readiness is the ring it fills, and its wakeup is an Exec signal that
           goes into WaitSelect()'s mask below -- never WaitForChar() from this
           process, which would fight the child for the same handle. */
        if (want_r && fd == 0 && con_active())
        {
            con_watch = 1;
            con_fd    = fd;
            if (con_readable()) { FD_SET(fd, &out_r); other_ready++; }
        }
        else if (want_r && dos_readable(fd)) { FD_SET(fd, &out_r); other_ready++; }

        if (want_w) { FD_SET(fd, &out_w); other_ready++; }
    }

    if (have_sockets)
    {
        ULONG sigs;

        if (amiga_sock_init() != 0)
            return -1;

        if (other_ready > 0 || con_readable())
        {
            tv_zero.tv_sec  = 0;
            tv_zero.tv_usec = 0;
            tv = &tv_zero;
        }
        else
        {
            tv = timeout;
        }

        /* Wait on the console keystroke signal alongside the sockets, so a key
           wakes the same WaitSelect() the socket does.
           Always include it while watching the console, never conditionally on
           the ring being empty: a byte can land between the readiness check
           above and here, and excluding the signal then would block with the
           byte already queued and its wake-up not in the mask -- a lost wakeup
           that hangs until some other signal (Ctrl-C) happens to arrive.  A
           stale signal only costs one harmless spurious wakeup.
           Ctrl-C rides the same USER mask: kept out of the break mask, it is
           consumed here rather than returning EINTR and being reposted (the
           spin that locked up), and fed to the remote as ^C. */
        sigs = 0;
        if (con_watch)    sigs |= con_reader->cr_DataSig;
        if (con_active()) sigs |= SIGBREAKF_CTRL_C;
        if (pipe_watch)   sigs |= db_pipe_sigmask();

        rc = nx_waitselect(sock_n, &sock_r, &sock_w, NULL, (APTR)tv, &sigs);
        if (rc < 0)
            return -1;

        if ((sigs & SIGBREAKF_CTRL_C) != 0)
            con_intr = 1;

        /* The reader woke us for a window resize: hand it to Dropbear's SIGWINCH
           handler, which sets cli_ses.winchange, and the session loop sends the
           new size (read live from the ConUnit) on this same pass. */
        if (con_resized)
        {
            con_resized = 0;
            if (con_winch != NULL)
                con_winch(SIGWINCH);
        }

        for (fd = DB_SOCK_BASE; fd < nfds && fd < DB_SOCK_LIMIT; fd++)
        {
            LONG s = SOCKOF(fd);

            if (FD_ISSET((int)s, &sock_r)) { FD_SET(fd, &out_r); ready++; }
            if (FD_ISSET((int)s, &sock_w)) { FD_SET(fd, &out_w); ready++; }
        }

        if (con_fd >= 0 && con_readable() && !FD_ISSET(con_fd, &out_r))
        {
            FD_SET(con_fd, &out_r);
            ready++;
        }

        /* A command's packet may have arrived while we were in there.  Service
           it and re-read the pipes, or the readiness reported is the one from
           before the wait and Dropbear spins until the next timeout. */
        if (pipe_watch)
        {
            db_pipe_service();

            for (fd = DB_PIPE_BASE; fd < nfds && fd < DB_PIPE_LIMIT; fd++)
            {
                int want_r = (readfds  != NULL) && FD_ISSET(fd, readfds);
                int want_w = (writefds != NULL) && FD_ISSET(fd, writefds);

                if (want_r && pipe_readable(fd) && !FD_ISSET(fd, &out_r))
                {
                    FD_SET(fd, &out_r);
                    ready++;
                }
                if (want_w && pipe_writable(fd) && !FD_ISSET(fd, &out_w))
                {
                    FD_SET(fd, &out_w);
                    ready++;
                }
            }
        }
    }
    else if (con_watch && !con_readable())
    {
        /* No socket in the set -- only the console.  Wait on its signal or a
           Ctrl-C (a key always ends it; an interactive loop always has the
           socket too, so this is the rare case). */
        ULONG got = Wait(con_reader->cr_DataSig | SIGBREAKF_CTRL_C);

        if ((got & SIGBREAKF_CTRL_C) != 0)
            con_intr = 1;
        if (con_readable()) { FD_SET(con_fd, &out_r); ready++; }
    }
    else if (other_ready == 0)
    {
        /* Nothing to wait on but the clock.  Delay() takes ticks of 1/50 s. */
        ULONG ticks = 0;

        if (timeout != NULL)
            ticks = (ULONG)timeout->tv_sec * TICKS_PER_SECOND
                  + (ULONG)(timeout->tv_usec / (1000000L / TICKS_PER_SECOND));

        /* A command on a pipe and no socket in the set: wait on its packets
           rather than the clock, since the packet is the only thing that can
           make progress.  With a timeout, the Delay() is still bounded and the
           next pass services whatever arrived -- latency, not a stall. */
        if (pipe_watch && timeout == NULL)
        {
            (VOID)Wait(db_pipe_sigmask());
            db_pipe_service();
        }
        else if (ticks > 0)
        {
            Delay(ticks);
        }

        if (pipe_watch)
        {
            db_pipe_service();

            for (fd = DB_PIPE_BASE; fd < nfds && fd < DB_PIPE_LIMIT; fd++)
            {
                int want_r = (readfds  != NULL) && FD_ISSET(fd, readfds);
                int want_w = (writefds != NULL) && FD_ISSET(fd, writefds);

                if (want_r && pipe_readable(fd) && !FD_ISSET(fd, &out_r))
                {
                    FD_SET(fd, &out_r);
                    other_ready++;
                }
                if (want_w && pipe_writable(fd) && !FD_ISSET(fd, &out_w))
                {
                    FD_SET(fd, &out_w);
                    other_ready++;
                }
            }
        }
    }

    ready += other_ready;

    if (readfds  != NULL) *readfds  = out_r;
    if (writefds != NULL) *writefds = out_w;
    if (exceptfds != NULL) FD_ZERO(exceptfds);

    return ready;
}


/* ---------------------------------------------------------------- pipe --- */

/*
 * Eight pairs.  There was one, which was enough for dbclient and is not enough
 * for the server: svr-main.c makes a childpipe per connection and
 * common-session.c then makes ses.signal_pipe, so the second call failed and
 * the session died with "Signal pipe failed" after a successful accept.  A
 * command adds three more.
 *
 * Freed on close, so a server that handled connections in a loop would not run
 * out.  This one cannot -- DEBUG_NOFORK means it handles one and exits -- but a
 * pipe table that leaks only because nothing loops is a trap for whatever
 * changes that.
 *
 * fds[0] is the read end and fds[1] the write end, and PIPE_IS_READ() depends
 * on that being the even one.
 */
int pipe(int fds[2])
{
    int i;

    for (i = 0; i < DB_PIPE_PAIRS; i++)
    {
        if (!db_pipes[i].taken)
        {
            memset(&db_pipes[i], 0, sizeof(db_pipes[i]));
            db_pipes[i].taken = 1;
            /* Lossy until something makes it a DOS pipe.  That is the wakeup
               pipe's behaviour and it was every pipe's before there were DOS
               ones: a full ring drops rather than refusing, because refusing
               would stall a writer that only ever says "wake up". */
            db_pipes[i].lossy = 1;
            fds[0] = DB_PIPE_BASE + 2 * i;
            fds[1] = DB_PIPE_BASE + 2 * i + 1;
            return 0;
        }
    }

    errno = EMFILE;
    return -1;
}


/* -------------------------------------------------------------- signals -- */

/*
 * AmigaOS has Exec signals, which are bits a task Wait()s on, and not POSIX
 * signals, which interrupt a running program.  There is nothing to install.
 * Returning SIG_DFL rather than SIG_ERR matters: dbutil.c treats SIG_ERR as
 * fatal, and a client that cannot install a handler it will never receive is
 * not in trouble.
 *
 * Ctrl-C therefore does not interrupt dbclient: the Shell's break signal
 * (SIGBREAKF_CTRL_C) is never polled, because the place to poll it is inside
 * WaitSelect() and that is the library's loop.
 */
void (*signal(int sig, void (*handler)(int)))(int)
{
    (void)sig;
    (void)handler;
    return SIG_DFL;
}

int kill(pid_t pid, int sig)
{
    (void)pid;
    (void)sig;
    errno = ESRCH;
    return -1;
}


/* ------------------------------------------------------- process stubs --- */

pid_t fork(void)   { errno = ENOSYS; return -1; }
pid_t vfork(void)  { errno = ENOSYS; return -1; }
pid_t setsid(void) { errno = ENOSYS; return -1; }

/* ----------------------------------------------- running a command ------- */

/*
 * dbutil.c's spawn_command() is the only place Dropbear starts a program: three
 * pipe()s, a fork(), and a child branch that dup2()s the pipes onto 0/1/2 and
 * calls execv().  None of those exist on this machine, and the failure is not
 * graceful: fork() returns -1, spawn_command() reports DROPBEAR_FAILURE, and
 * the client gets an authenticated session that runs nothing and exits 0 with
 * empty output.
 *
 * __wrap_spawn_command() below replaces the whole function.  On AmigaOS the
 * equivalent of "run this and give me its output" is SystemTagList(), which
 * starts a Shell:
 *
 *   the command line   comes from Dropbear, via execv() -- see exec_capturing
 *   stdin              NIL:.  A synchronous Shell cannot be fed by a channel
 *                      that is only read while the session loop runs.
 *   stdout and stderr  one file, because AmigaOS 3.x has one: a process's
 *                      pr_CES is NULL by default and errors go to its output.
 *                      The Shell's own messages land there too, which is why
 *                      SYS_Output is used rather than a ">" appended to the
 *                      command line.
 *   the exit status    System()'s return code, which is the command's rc
 *
 * Nothing crosses a task boundary.  The command runs in a Process of its own
 * and never touches the server's SocketBase -- src/bsdsocket/tcp_handler.c
 * states the rule and the ThreadX bracket enforces it, so a spawned process
 * using its parent's base gets ENETDOWN.  The socket stays in the session's
 * task and only a DOS file passes between them; a network command run over SSH
 * opens its own bsdsocket.library, as any AmigaOS program does.
 *
 * It runs synchronously, which is a limitation: the session loop is stopped for
 * as long as the command takes, so nothing is echoed back while it runs and a
 * command that never exits never returns.  The alternative is SYS_Asynch plus
 * a wakeup through WaitSelect()'s signal mask, which is a larger design.  This
 * is the piece that makes `ssh amiga "command"` return its output, and stops
 * there.
 */


/* 256 KB.  A Shell gives a command 4,096 bytes and every ported program on this
   machine needs far more than that (clientrun.c allocates 512 KB for the same
   reason).  A command arriving over SSH is exactly as likely to be
   a ported program as one typed at a Shell prompt. */

static jmp_buf exec_jump;
static int     exec_have_cmd;
static char    exec_cmd[DB_CMD_MAX];

/*
 * ENOSYS is still the answer for a caller that means "replace this process",
 * and there is no such caller: the one execv() in a linked server is
 * run_command()'s, at the end of the child path __wrap_spawn_command() calls.
 * While that is running, execv() is a hand-back rather than a call -- it takes
 * the argv apart and jumps to the wrapper, whose frame is live because it is
 * the frame that called into here.
 *
 * run_shell_command() builds argv as {shell, "-c", command} for a command and
 * {"-shell"} for a login shell.  The second has no answer here: an interactive
 * AmigaOS Shell needs a console handle and there is no pty to give it one.
 */
int execv(const char *path, char *const argv[])
{
    (void)path;

    if (exec_capturing)
    {
        exec_have_cmd = 0;
        exec_cmd[0] = '\0';

        if (argv != NULL && argv[1] != NULL && strcmp(argv[1], "-c") == 0
            && argv[2] != NULL && strlen(argv[2]) < sizeof(exec_cmd))
        {
            strcpy(exec_cmd, argv[2]);
            exec_have_cmd = 1;
        }

        longjmp(exec_jump, 1);
    }

    errno = ENOSYS;
    return -1;
}

int dup(int fd)
{
    (void)fd;
    errno = ENOSYS;
    return -1;
}

int dup2(int from, int to)
{
    (void)from;
    (void)to;
    errno = ENOSYS;
    return -1;
}

int chdir(const char *path)
{
    BPTR lock;
    BPTR old;

    if (path == NULL) { errno = EFAULT; return -1; }

    lock = Lock((CONST_STRPTR)path, SHARED_LOCK);
    if (lock == (BPTR)0) { errno = ENOENT; return -1; }

    old = CurrentDir(lock);
    if (old != (BPTR)0)
        UnLock(old);
    return 0;
}

int fsync(int fd)
{
    (void)fd;
    return 0;                           /* DOS Close() already flushed it */
}

/*
 * SetProtection().  The RWED bits are active low -- a set bit forbids the
 * operation -- the opposite of every POSIX mode, so the conversion is
 * inverted.  The other AmigaOS bits (archive, script, pure) are cleared: a
 * chmod states the whole mode rather than adjusting part of it.
 *
 * The three POSIX classes collapse into one: AmigaOS has no owner, group or
 * other, so the mode is read as the owner bits and the rest is dropped.
 */
int chmod(const char *path, mode_t mode)
{
    ULONG prot = 0;

    if (path == NULL) { errno = EFAULT; return -1; }

    if ((mode & S_IRUSR) == 0) prot |= FIBF_READ;
    if ((mode & S_IWUSR) == 0) prot |= FIBF_WRITE | FIBF_DELETE;
    if ((mode & S_IXUSR) == 0) prot |= FIBF_EXECUTE;

    if (!SetProtection((CONST_STRPTR)amiga_fix_path(path), prot))
    {
        errno = (IoErr() == ERROR_OBJECT_NOT_FOUND) ? ENOENT : EACCES;
        return -1;
    }
    return 0;
}

/*
 * There is no ownership to change.  ENOSYS rather than a silent 0, so a caller
 * is not told a change it asked for happened.
 */
int chown(const char *path, uid_t uid, gid_t gid)
{
    (void)path; (void)uid; (void)gid;
    errno = ENOSYS;
    return -1;
}



/*
 * There is a child now -- SystemTagList()'s, already finished by the time
 * anybody asks -- so this reports it once and ECHILD after that.
 *
 * The status word is newlib's: WIFEXITED() reads the low byte and
 * WEXITSTATUS() the next one up, so an AmigaDOS return code (0, 5, 10, 20)
 * shifted left by eight is a normal exit with that code.  No AmigaOS command
 * is ever killed by a signal, so WIFSIGNALED() is never true and the client
 * always sees an exit status rather than an exit signal.
 */

pid_t waitpid(pid_t pid, int *status, int options)
{
    (void)pid; (void)options;

    /* Not until the runner says so.  It used to be unconditional because the
       command had already finished by the time spawn_command() returned; now it
       runs alongside this process and reporting early would send the client an
       exit status before the command had produced its output. */
    if (child_pid != 0 && child_ready)
    {
        pid_t done = child_pid;

        if (status != NULL)
            *status = child_status;
        child_pid = 0;
        return done;
    }

    errno = ECHILD;
    return -1;
}

/*
 * Installs nothing and remembers one thing: SIGCHLD's handler.
 *
 * AmigaOS delivers no asynchronous signals to a C handler, so there is no
 * mechanism to attach one to.  Reporting failure would be worse than reporting
 * success: commonsetup() treats an error as fatal, so a -1 would refuse to
 * start the server over handlers that cannot fire.
 *
 * sesssigchild_handler() is also reachable synchronously.  Its job is to write
 * a byte to ses.signal_pipe so the session loop wakes and calls
 * svr_chansess_checksignal(), which reaps with waitpid() and hands the exit
 * status to the channel.  __wrap_spawn_command() knows when the child exited,
 * so it calls the handler itself at that moment: the same sequence a real
 * SIGCHLD would produce, and the only route by which a client learns a
 * command's return code.
 *
 * oldact is zeroed rather than left alone, so a caller that saves and later
 * restores gets a defined value instead of stack contents.
 */

int sigaction(int sig, const struct sigaction *act, struct sigaction *oldact)
{
    if (oldact != NULL)
        memset(oldact, 0, sizeof(*oldact));

    if (sig == SIGCHLD && act != NULL)
        sigchld_handler = act->sa_handler;

    return 0;
}

/* ------------------------------------------------- __wrap_spawn_command --- */

/*
 * Dropbear's, declared rather than included: this file includes none of
 * Dropbear's headers, and a prototype plus two return codes is the whole ABI
 * it needs.  <syslog.h> is not included either -- proto/bsdsocket.h makes
 * syslog() a macro and the NDK header then does not compile -- so the two
 * priorities are spelled out.  This build has no syslog anyway
 * (--disable-syslog), so svr_dropbear_log() ignores the value and writes to
 * stderr; the numbers are the syslog ones so that a build which did have one
 * would file them correctly.
 */

int __wrap_spawn_command(void (*exec_fn)(const void *user_data),
                         const void *exec_data,
                         int *ret_writefd, int *ret_readfd, int *ret_errfd,
                         pid_t *ret_pid)
{
    static pid_t next_pid = 1000;       /* not 0: chansess->pid == 0 means
                                           "no command has run on this
                                           channel" */

    struct TagItem   run_tags[4];
    struct Process  *proc;
    DbRunner        *runner;
    int              in[2], out[2], err[2];
    BPTR             cmd_in  = (BPTR)0;
    BPTR             cmd_out = (BPTR)0;

    /*
     * Dropbear's own child path, run in this process to the point where it
     * would have called execv().  It applies the forced command, sets the
     * environment and chdir()s to the home directory; doing any of that here
     * instead would be a second implementation of it.
     */
    exec_have_cmd  = 0;
    exec_capturing = 1;
    if (setjmp(exec_jump) == 0)
    {
        exec_fn(exec_data);

        /* Only reachable if execv() was never called, which means Dropbear
           gave up before it got there. */
        exec_capturing = 0;
        dropbear_log(DB_LOG_WARNING, "amiga: no command to run");
        return DB_FAILURE;
    }
    exec_capturing = 0;

    if (!exec_have_cmd)
    {
        dropbear_log(DB_LOG_WARNING,
                     "amiga: no interactive shell on this machine -- "
                     "give ssh a command to run");
        return DB_FAILURE;
    }

    if (pipe(in) != 0)
        return DB_FAILURE;
    if (pipe(out) != 0)
    {
        (void)__wrap_close(in[0]); (void)__wrap_close(in[1]);
        return DB_FAILURE;
    }
    if (pipe(err) != 0)
    {
        (void)__wrap_close(in[0]);  (void)__wrap_close(in[1]);
        (void)__wrap_close(out[0]); (void)__wrap_close(out[1]);
        return DB_FAILURE;
    }

    /*
     * stderr keeps the old shape: closing the write end makes it report end of
     * file at once.  The Shell's own errors go to its Output(), which is the
     * stdout pipe, so nothing is lost -- see the SYS_Output note below.
     */
    (void)__wrap_close(err[1]);

    /*
     * in and out do NOT get their far local end closed.  Each has a DOS end
     * instead -- the FileHandle the command is given -- and that end is the
     * authority on whether anybody is still reading or writing.  Closing the
     * unused local fd as well would set rclosed/wclosed and make the pipe report
     * end of file to the command before it had read a byte.
     */
    cmd_in  = db_pipe_fh(PIPE_INDEX(in[0]),  1);    /* command reads this  */
    cmd_out = db_pipe_fh(PIPE_INDEX(out[1]), 0);    /* command writes this */

    if (cmd_in == (BPTR)0 || cmd_out == (BPTR)0)
    {
        dropbear_log(DB_LOG_WARNING, "amiga: cannot make a pipe for the command");
        goto fail;
    }

    /*
     * SYS_Input and SYS_Output rather than a redirection appended to the command
     * line, for two reasons that both still hold now the targets are pipes.
     *
     * The Shell's own messages go to its Output(), and a redirection in the
     * command line applies to the command, not to them.  So `NoSuchCommand`
     * printed "Unknown command" onto the server's stderr and the client got an
     * empty transfer with a return code of 10 and no explanation.  With the
     * handle passed in, the Shell's Output() is the channel and everything
     * lands there.
     *
     * The command line also stays the user's: a command containing its own
     * redirection is not fighting one appended to it.
     *
     * Handing over a handle is safe because dos.library says so, in System()'s
     * own autodoc: "The input and output filehandles will not be closed by
     * System, you must close them (if needed) after System returns".  The runner
     * closes both, and those two closes are what tell this side the command has
     * finished reading and finished writing.
     */
    runner = (DbRunner *)AllocVec(sizeof(DbRunner), MEMF_PUBLIC | MEMF_CLEAR);
    if (runner == NULL)
    {
        dropbear_log(DB_LOG_WARNING, "amiga: out of memory for the command");
        goto fail;
    }

    runner->rn_Parent = FindTask(NULL);
    runner->rn_In     = cmd_in;
    runner->rn_Out    = cmd_out;
    snprintf(runner->rn_Cmd, sizeof(runner->rn_Cmd), "%s", exec_cmd);

    dropbear_log(DB_LOG_INFO, "amiga: running '%s'", exec_cmd);

    run_tags[0].ti_Tag = NP_Entry;      run_tags[0].ti_Data = (ULONG)db_runner_main;
    run_tags[1].ti_Tag = NP_Name;       run_tags[1].ti_Data = (ULONG)"dbssh command";
    run_tags[2].ti_Tag = NP_StackSize;  run_tags[2].ti_Data = DB_RUNNER_STACK;
    run_tags[3].ti_Tag = TAG_END;       run_tags[3].ti_Data = 0;

    proc = CreateNewProc(run_tags);
    if (proc == NULL)
    {
        dropbear_log(DB_LOG_WARNING, "amiga: cannot start a process for '%s'",
                     exec_cmd);
        FreeVec(runner);
        goto fail;
    }

    /* The runner is parked in Wait(SIGF_SINGLE) until this pair happens. */
    proc->pr_Task.tc_UserData = (APTR)runner;
    Signal((struct Task *)proc, SIGF_SINGLE);

    db_runner    = runner;
    child_ready  = 0;
    child_pid    = next_pid++;
    child_status = 0;

    *ret_writefd = in[1];
    *ret_readfd  = out[0];
    if (ret_errfd != NULL)
        *ret_errfd = err[0];
    if (ret_pid != NULL)
        *ret_pid = child_pid;

    /*
     * SIGCHLD is not raised here any more: the command is still running.
     * db_runner_poll(), called once per pass of __wrap_select(), raises it when
     * the runner reports the exit code -- the same sequence, at the point a real
     * signal would have arrived rather than before the command started.
     */
    return DB_SUCCESS;

fail:
    if (cmd_in != (BPTR)0)
        Close(cmd_in);
    if (cmd_out != (BPTR)0)
        Close(cmd_out);
    (void)__wrap_close(in[0]);  (void)__wrap_close(in[1]);
    (void)__wrap_close(out[0]); (void)__wrap_close(out[1]);
    (void)__wrap_close(err[0]);
    return DB_FAILURE;
}

/*
 * <unistd.h> defines environ as (*environ_ptr), and environ_ptr is declared
 * by that header but defined nowhere in the toolchain -- so any client that
 * touches environ fails to link rather than failing at runtime.
 *
 * An empty, correctly terminated environment is the accurate answer: AmigaOS
 * keeps its environment in ENV:, a directory rather than a char ** the process
 * owns, and the code that wants a named variable goes through GetVar() (see
 * amiga_pw_fill above).  The one caller of environ here is execchild(), which
 * is unreachable because execv() fails.
 */
static char  *amiga_environ_empty[1] = { NULL };
static char **amiga_environ          = amiga_environ_empty;
char        ***environ_ptr           = &amiga_environ;

/*
 * getenv() over ENV:, and the terminal type in particular.
 *
 * environ is empty (above), so newlib's getenv() answers NULL for everything,
 * and send_chansess_pty_req() then tells the server TERM=vt100, whose terminfo
 * drives the VT100 line-drawing charset the Amiga console does not have, so vim
 * paints its frames as garbage.  getenv() is routed through GetVar() instead,
 * where the rest of this file reads named variables, so a `setenv TERM ...` is
 * honoured, and TERM defaults to "amiga", the ncurses entry written for this
 * console (it knows there is no line-drawing).  -Wl,--wrap=getenv.
 */
char *__wrap_getenv(const char *name)
{
    static char value[256];
    LONG        len;

    if (name == NULL)
        return NULL;

    len = GetVar((STRPTR)name, (STRPTR)value, (LONG)sizeof(value) - 1, 0);
    if (len >= 0)
    {
        value[len] = '\0';
        while (len > 0 && (value[len - 1] == '\n' || value[len - 1] == '\r'))
            value[--len] = '\0';
        return value;
    }

    if (strcmp(name, "TERM") == 0)
        return (char *)"amiga";

    return NULL;
}

/*
 * A Process address is unique for as long as the process exists, which is
 * what dbrandom.c and gensignkey.c want it for -- a per-run distinguisher
 * hashed into the pool, not an identity.
 */
pid_t getpid(void)
{
    return (pid_t)(ULONG)FindTask(NULL);
}

uid_t getuid(void)  { return 0; }
uid_t geteuid(void) { return 0; }
gid_t getgid(void)  { return 0; }
gid_t getegid(void) { return 0; }

/*
 * The server calls svr_switch_user() after authentication, and reaches these
 * because getuid() above says 0 and getpwnam() fills pw_uid/pw_gid with 0 too.
 * Switching from 0 to 0 is a no-op, and POSIX setuid(0) as root succeeds, so
 * success is the correct answer.
 *
 * Any other id fails with EPERM, which is also correct: there is nothing on
 * this machine to switch to.  Spelled as a comparison rather than an
 * unconditional
 * `return 0` so that a future getpwnam() handing out a nonzero uid gets a
 * refusal instead of silently appearing to work.
 */
int setuid(uid_t uid)
{
    if (uid == 0) return 0;
    errno = EPERM;
    return -1;
}

int setgid(gid_t gid)
{
    if (gid == 0) return 0;
    errno = EPERM;
    return -1;
}

int initgroups(const char *user, gid_t group)
{
    (void)user;
    if (group == 0) return 0;
    errno = EPERM;
    return -1;
}

/*
 * NULL, always.  There is no group database on AmigaOS 3.x, and unlike
 * getpwnam() -- which can answer with the user running the program -- there is
 * no group that could be meant.  sshpty.c uses it to find "tty" so it can
 * chown a pty node, which this machine does not have.
 */
struct group *getgrnam(const char *name)
{
    (void)name;
    errno = ENOENT;
    return NULL;
}

/*
 * ENOSYS, and it has to be exactly that.  common-session.c:71 checks
 * `getgroups(0, NULL) == -1 && errno == ENOSYS` when DROPBEAR_SVR_MULTIUSER is
 * 0, as a guard against that option being set by accident on a machine that
 * does have users -- and dropbear_exit()s if the call succeeds.  AmigaOS 3.x
 * has no groups and no users, so ENOSYS is both what the guard wants and what
 * is true.
 */
int getgroups(int size, gid_t *list)
{
    (void)size;
    (void)list;
    errno = ENOSYS;
    return -1;
}

/*
 * disallow_core() reads the limit, zeroes it and writes it back.  Returning
 * a zeroed struct rather than failing keeps that from operating on an
 * uninitialised one; AmigaOS writes no core files, so the intent is already
 * satisfied.
 */
int getrlimit(int resource, struct rlimit *lim)
{
    (void)resource;
    if (lim == NULL) { errno = EFAULT; return -1; }
    lim->rlim_cur = 0;
    lim->rlim_max = 0;
    return 0;
}

int setrlimit(int resource, struct rlimit *lim)
{
    (void)resource;
    (void)lim;
    return 0;
}


/* ------------------------------------------------------------- users ----- */

/*
 * AmigaOS 3.x is a single-user machine with no account database, so there is
 * nothing to look up and this answers with the one user there is.  The name
 * matters: it is what dbclient sends as the SSH username when the command line
 * does not say `user@host`, so ENV:USER is read first and is the documented
 * way to set it.  HOME likewise feeds the `~/.ssh/id_dropbear` default.
 */
static struct passwd amiga_pw;
static char amiga_pw_name[64];
static char amiga_pw_dir[256];

static void amiga_pw_fill(const char *name)
{
    if (name != NULL)
    {
        strncpy(amiga_pw_name, name, sizeof(amiga_pw_name) - 1);
        amiga_pw_name[sizeof(amiga_pw_name) - 1] = '\0';
    }
    else
    {
        LONG len = GetVar((CONST_STRPTR)"USER", (STRPTR)amiga_pw_name,
                          sizeof(amiga_pw_name), 0);
        if (len <= 0)
            strcpy(amiga_pw_name, "amiga");
    }

    if (GetVar((CONST_STRPTR)"HOME", (STRPTR)amiga_pw_dir,
               sizeof(amiga_pw_dir), 0) <= 0)
        strcpy(amiga_pw_dir, "SYS:");

    memset(&amiga_pw, 0, sizeof(amiga_pw));
    amiga_pw.pw_name   = amiga_pw_name;
    amiga_pw.pw_passwd = (char *)"";
    amiga_pw.pw_dir    = amiga_pw_dir;
    amiga_pw.pw_shell  = (char *)"";
    amiga_pw.pw_gecos  = amiga_pw_name;
}

struct passwd *getpwuid(uid_t uid)
{
    (void)uid;
    amiga_pw_fill(NULL);
    return &amiga_pw;
}

struct passwd *getpwnam(const char *name)
{
    amiga_pw_fill(name);
    return &amiga_pw;
}


/* ------------------------------------------------------------ terminal --- */

/*
 * AmigaOS has no termios, but an interactive console has the one thing an
 * interactive ssh session needs: raw mode, via SetMode().  These fabricate a
 * plausible cooked terminal for tcgetattr() and map Dropbear's raw/cooked
 * switch onto SetMode() in tcsetattr().  A file or NIL: still returns ENOTTY,
 * which is how `dbclient -T host command` detects "no terminal" and asks the
 * server for no pty.
 */
int tcgetattr(int fd, struct termios *t)
{
    BPTR h = dos_handle_for(fd);
    int  i;

    if (h == (BPTR)0 || !IsInteractive(h))
    {
        errno = ENOTTY;
        return -1;
    }

    if (t == NULL)
        return 0;

    /* Dropbear saves this, ships it to the server's pty so the remote shell
       echoes, and derives a raw copy for the local side.  The only field
       tcsetattr() reads back is c_lflag's ICANON. */
    t->c_iflag = 0;
    t->c_oflag = 0;
    t->c_cflag = CREAD | CS8;
    t->c_lflag = ISIG | ICANON | ECHO;

    for (i = 0; i < NCCS; i++)
        t->c_cc[i] = 0;
    t->c_cc[VINTR]  = 0x03;      /* ^C  */
    t->c_cc[VERASE] = 0x7F;      /* DEL */
    t->c_cc[VMIN]   = 1;
    t->c_cc[VTIME]  = 0;

    return 0;
}

int tcsetattr(int fd, int actions, const struct termios *t)
{
    BPTR h = dos_handle_for(fd);

    (void)actions;

    if (h == (BPTR)0 || !IsInteractive(h))
    {
        errno = ENOTTY;
        return -1;
    }

    /* Dropbear clears ICANON for its raw local mode.  SetMode(h, 1) is the
       console's raw mode -- no echo, no line editing, a keystroke at a time --
       and SetMode(h, 0) restores cooked mode on cleanup.  The raw/cooked switch
       is also where the console reader child lives and dies (see it above): raw
       means an interactive session is starting, cooked means it is ending. */
    if (t != NULL && (t->c_lflag & ICANON) == 0)
    {
        SetMode(h, 1);
        con_reader_start(h);
    }
    else
    {
        con_reader_stop();
        SetMode(h, 0);
    }
    return 0;
}

/*
 * getpass() over dos.library.  SetMode(handle, 1) puts a console into raw
 * mode, in which it stops echoing and Read() returns each keystroke instead of
 * waiting for a line, which is what a password prompt wants.  A
 * non-interactive stdin (a file, a NIL:) skips the mode change and reads a
 * line, so a scripted run still works.
 */
char *getpass(const char *prompt)
{
    static char buf[128];
    BPTR  in  = Input();
    int   raw = 0;
    ULONG n   = 0;
    char  c;

    /* Output through __wrap_write(), not DOS Write(): the CR-for-LF fixup that
       makes the other prompts break lines lives there, and Read() consuming the
       Return in raw mode means the newline has to be emitted by us. */
    if (prompt != NULL)
        __wrap_write(1, prompt, strlen(prompt));

    if (IsInteractive(in))
        raw = SetMode(in, 1) ? 1 : 0;

    while (n + 1 < sizeof(buf))
    {
        if (Read(in, &c, 1) != 1)
            break;
        if (c == '\n' || c == '\r')
            break;
        if (c == '\b' || c == 0x7F)
        {
            if (n > 0)
                n--;
            continue;
        }
        buf[n++] = c;
    }

    buf[n] = '\0';

    if (raw)
        SetMode(in, 0);

    __wrap_write(1, "\n", 1);      /* con_write() gives it the CR the console needs */

    return buf;
}

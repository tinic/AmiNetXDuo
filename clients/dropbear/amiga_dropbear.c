/* clients/dropbear, the AmigaOS half of the Dropbear port.
 * build.sh must set FD_SETSIZE >= DB_PIPE_LIMIT (newlib's default is 64).
 * SPDX-License-Identifier: MIT */

#include <exec/types.h>
#include <exec/io.h>            /* struct IOStdReq, the console ConUnit */
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
#include <limits.h>
#include <reent.h>              /* struct _reent, _ssize_t */
#include <sys/stat.h>           /* chmod() */
#include <sys/wait.h>           /* waitpid() */
#include <grp.h>                /* getgrnam() */

#include "aminetxduo/random.h"

/* Must match DROPBEAR_URANDOM_DEV in clients/dropbear/localoptions.h. */
#define AMIGA_URANDOM_DEV  "RANDOM:"

/* ------------------------------------------------------------------------ */
/* The NDK inlines are macros with the plain BSD names, so these static wrappers
   must be captured before the #undef block below; the public functions at the
   bottom of the file then call them.  */

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

#include "amiga_stdio.h"


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
   this process.  The two places below that have to know are close(), which
   must refuse to close anything, and execv(), which hands the command back. */
static int exec_capturing;

/* Three shapes of one object: a wakeup pipe (buffered), a file source (`src`
   drained, then deleted when the read end closes), and a sink (writes discarded
   and always succeeding: a full pipe leaves a channel unable to close). */
#define DB_PIPE_BUF     128

struct db_pipe
{
    unsigned      taken   : 1;
    unsigned      rclosed : 1;      /* the read end has been close()d */
    unsigned      wclosed : 1;      /* the write end has been close()d */
    unsigned      sink    : 1;      /* writes are discarded, never buffered */
    BPTR          src;              /* the read end drains this file, or 0 */
    char          srcname[64];      /* deleted with the read end, if set */
    int           len;
    int           pos;
    unsigned char buf[DB_PIPE_BUF];
};

static struct db_pipe db_pipes[DB_PIPE_PAIRS];

#define PIPE_PAIR(fd)       (&db_pipes[((fd) - DB_PIPE_BASE) / 2])
#define PIPE_IS_READ(fd)    ((((fd) - DB_PIPE_BASE) & 1) == 0)

static int pipe_readable(int fd)
{
    const struct db_pipe *p = PIPE_PAIR(fd);

    if (!PIPE_IS_READ(fd))
        return 0;

    /* A file is always readable: Read() returns data or zero, and zero is end
       of file, which is progress.  A pipe whose write end is closed is
       readable for the same reason. */
    return p->pos < p->len || p->src != (BPTR)0 || p->wclosed;
}

static int pipe_read(int fd, void *buf, size_t len)
{
    struct db_pipe *p = PIPE_PAIR(fd);

    if (!PIPE_IS_READ(fd)) { errno = EBADF; return -1; }

    if (p->pos < p->len)
    {
        int n = p->len - p->pos;

        if ((size_t)n > len)
            n = (int)len;
        memcpy(buf, p->buf + p->pos, (size_t)n);
        p->pos += n;
        if (p->pos == p->len)
            p->pos = p->len = 0;
        return n;
    }

    if (p->src != (BPTR)0)
    {
        LONG n = Read(p->src, (APTR)buf, (LONG)len);

        if (n < 0) { errno = EIO; return -1; }
        return (int)n;                  /* 0 is end of file */
    }

    if (p->wclosed)
        return 0;

    errno = EAGAIN;
    return -1;
}

static int pipe_write(int fd, const void *buf, size_t len)
{
    struct db_pipe *p = PIPE_PAIR(fd);
    int space;

    if (PIPE_IS_READ(fd)) { errno = EBADF; return -1; }

    if (p->sink)
        return (int)len;

    if (p->pos > 0)
    {
        memmove(p->buf, p->buf + p->pos, (size_t)(p->len - p->pos));
        p->len -= p->pos;
        p->pos = 0;
    }

    /* What does not fit is dropped rather than refused.  The only buffered
       pipe here is the wakeup one, where a second byte says nothing the first
       did not, and refusing would stall the writer instead. */
    space = DB_PIPE_BUF - p->len;
    if (space > 0)
    {
        int n = ((size_t)space < len) ? space : (int)len;

        memcpy(p->buf + p->len, buf, (size_t)n);
        p->len += n;
    }

    return (int)len;
}

static int pipe_close(int fd)
{
    struct db_pipe *p = PIPE_PAIR(fd);

    if (PIPE_IS_READ(fd))
    {
        p->rclosed = 1;
        if (p->src != (BPTR)0)
        {
            Close(p->src);
            p->src = (BPTR)0;
            if (p->srcname[0] != '\0')
                DeleteFile((CONST_STRPTR)p->srcname);
        }
    }
    else
    {
        p->wclosed = 1;
    }

    /* Released only once both ends are closed.  Dropbear closes one end and
       keeps the other, so freeing on the first close would hand the live end
       to the next caller. */
    if (p->rclosed && p->wclosed)
        memset(p, 0, sizeof(*p));

    return 0;
}

extern int __real_read(int fd, void *buf, size_t len);
extern int __real_write(int fd, const void *buf, size_t len);
extern int __real_close(int fd);
extern int __real_open(const char *path, int flags, ...);


/* ----------------------------------------------------- no requesters ---- */

/* pr_WindowPtr = -1 is dos.library's "fail the call, never put up a requester".
   It must be in place before Dropbear's first file access, or a headless run
   wedges forever on "Please insert volume RANDOM:". */
__attribute__((constructor)) static void amiga_no_requesters(void)
{
    struct Process *proc = (struct Process *)FindTask(NULL);

    if (proc != NULL && proc->pr_Task.tc_Node.ln_Type == NT_PROCESS)
        proc->pr_WindowPtr = (APTR)-1;
}


/* ------------------------------------------------------------ SocketBase  */

/* Declared by the NDK inlines and dereferenced by every one of them.  Opened
   lazily, so a program that never reaches the network still starts;
   SBTC_ERRNOPTR is what makes `errno` after a failed socket call mean anything. */
struct Library *SocketBase = NULL;

static void amiga_sock_cleanup(void)
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

    atexit(amiga_sock_cleanup);
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

/* Not reached by dbclient.  An accepted socket comes from the same bsdsocket
   descriptor space as socket()'s, so it can land outside the window just as
   easily and must be refused rather than alias a file descriptor. */
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

/* Not bsdsocket's own inet_aton (vector 98): a Roadshow older than that vector
   jumps off the end of its LVO table.  Hence inet_addr, plus the one address it
   cannot tell apart from failure. */
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

/* dbrandom.c's seedrandom() exits if this device cannot be read; there is no
   build without one.  The pool credits itself ~21 bits and reports itself
   unseeded: survivable for per-connection keys, NOT for generating a host key. */
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

static struct amiga_stdio_override *amiga_stdio_descriptor(void)
{
    struct Task *task = FindTask(NULL);
    struct amiga_stdio_override *stdio;
    ULONG tagged;

    if (task == NULL)
        return NULL;
    tagged = (ULONG)task->tc_UserData;
    if ((tagged & 3UL) != AMIGA_STDIO_USERDATA_TAG)
        return NULL;
    stdio = (struct amiga_stdio_override *)(tagged & ~3UL);
    if (stdio->magic != AMIGA_STDIO_MAGIC)
        return NULL;
    return stdio;
}

static struct amiga_mempipe *amiga_stdio_pipe(int fd)
{
    struct amiga_stdio_override *stdio = amiga_stdio_descriptor();

    if (stdio == NULL)
        return NULL;
    return fd == 0 ? stdio->input : fd == 1 ? stdio->output : NULL;
}

/* RunCommand() is hosting this invocation when scp installed a stdio
   descriptor.  Its small runner must regain control after Dropbear calls
   exit(), so the common argv shim returns the status instead of terminating
   the hosting Process. */
int amiga_client_exit_returns(void)
{
    return amiga_stdio_descriptor() != NULL;
}

unsigned long amiga_client_stack_size(void)
{
    return amiga_stdio_descriptor() != NULL ? 128UL * 1024UL : 32UL * 1024UL;
}

/* newlib's fd 2 is wired to Output() on this target.  AmigaDOS keeps the
   separately redirected error stream in pr_CES, so using newlib's write(2)
   would put diagnostics into stdout.  That is merely untidy for a terminal,
   but corrupts byte protocols such as scp when stdout is a pipe. */
static BPTR amiga_error_handle(void)
{
    struct Process *proc = (struct Process *)FindTask(NULL);
    struct amiga_stdio_override *stdio = amiga_stdio_descriptor();

    if (stdio != NULL && stdio->error != (BPTR)0)
        return stdio->error;
    if (proc != NULL && proc->pr_Task.tc_Node.ln_Type == NT_PROCESS
        && proc->pr_CES != (BPTR)0)
        return proc->pr_CES;
    return Output();
}

static int amiga_raw_write(int fd, const void *buf, size_t len)
{
    struct amiga_mempipe *pipe = amiga_stdio_pipe(fd);
    BPTR handle = (BPTR)0;
    LONG n;

    if (pipe != NULL)
    {
        if (len > (size_t)LONG_MAX)
        {
            errno = EINVAL;
            return -1;
        }
        n = amiga_mempipe_write(pipe, buf, (ULONG)len);
        if (n < 0)
            errno = EPIPE;
        return (int)n;
    }
    if (fd != 2)
        return __real_write(fd, buf, len);
    if (len > (size_t)LONG_MAX)
    {
        errno = EINVAL;
        return -1;
    }

    handle = amiga_error_handle();
    n = Write(handle, (APTR)buf, (LONG)len);
    if (n < 0)
    {
        errno = EIO;
        return -1;
    }
    return (int)n;
}

static int amiga_raw_read(int fd, void *buf, size_t len)
{
    struct amiga_mempipe *pipe = amiga_stdio_pipe(fd);
    LONG n;

    if (pipe == NULL)
        return __real_read(fd, buf, len);
    if (len > (size_t)LONG_MAX)
    {
        errno = EINVAL;
        return -1;
    }

    n = amiga_mempipe_read(pipe, buf, (ULONG)len);
    if (n < 0)
    {
        errno = EIO;
        return -1;
    }
    return (int)n;
}

extern _ssize_t __real__read_r(struct _reent *, int, void *, size_t);

_ssize_t __wrap__read_r(struct _reent *reent, int fd, void *buf, size_t len)
{
    int n;

    if (fd != 0)
        return __real__read_r(reent, fd, buf, len);

    n = amiga_raw_read(fd, buf, len);
    if (n < 0 && reent != NULL)
        reent->_errno = errno;
    return (_ssize_t)n;
}

/* newlib stdio reaches _write_r() from inside libc.a, so --wrap=write cannot
   see it.  In particular, Dropbear's fprintf(stderr, ...) takes this path. */
extern _ssize_t __real__write_r(struct _reent *, int, const void *, size_t);

_ssize_t __wrap__write_r(struct _reent *reent, int fd,
                         const void *buf, size_t len)
{
    int n;

    if (fd != 1 && fd != 2)
        return __real__write_r(reent, fd, buf, len);

    n = amiga_raw_write(fd, buf, len);
    if (n < 0 && reent != NULL)
        reent->_errno = errno;
    return (_ssize_t)n;
}

int __wrap_read(int fd, void *buf, size_t len)
{
    if (IS_SOCK(fd))
        return (int)nx_recv(SOCKOF(fd), (APTR)buf, (LONG)len, 0);

    /* stdin, while an interactive session's console reader child is running,
       hand back what it has buffered rather than read the console twice. */
    if (fd == 0 && con_active())
        return con_read(buf, len);

    if (IS_RAND(fd))
        return rand_fill(buf, len);

    if (IS_PIPE(fd))
        return pipe_read(fd, buf, len);

    return amiga_raw_read(fd, buf, len);
}

int __wrap_write(int fd, const void *buf, size_t len)
{
    /* A zero-length write must not reach a DOS handler: writechannel_fallback()
       issues write(fd, p, 0) on every pass where the channel buffer is empty,
       and a handler that waits for room on one waits for ever. */
    if (len == 0)
        return 0;

    if (IS_SOCK(fd))
        return (int)nx_send(SOCKOF(fd), (APTR)buf, (LONG)len, 0);

    if (IS_PIPE(fd))
        return pipe_write(fd, buf, len);

    if (IS_RAND(fd))
        return (int)len;                /* swallowed: see rand_fill() */

    if (fd == 1 && amiga_stdio_pipe(fd) != NULL)
        return amiga_raw_write(fd, buf, len);

    /* The client's own console output (prompts, messages) before an interactive
       session is up: give it the CR the Amiga console needs.  Once the session
       runs, the server's byte stream (already CRLF) passes straight through. */
    if ((fd == 1 || fd == 2) && !con_active())
    {
        BPTR handle = (fd == 2) ? amiga_error_handle() : Output();

        if (handle != (BPTR)0 && IsInteractive(handle))
            return con_write(fd, buf, len);
    }

    if (fd == 2)
        return amiga_raw_write(fd, buf, len);

    return __real_write(fd, buf, len);
}

/* The console's real geometry, for put_winsize(); without it Dropbear ships a
   fixed 80x25.  Read before tcsetattr() starts the reader child, so nothing else
   holds the console, and the wait is bounded. */
static struct ConUnit *con_unit;    /* the console's ConUnit, stable once found */

/* Dynamic resize: the reader child polls the ConUnit's live size and wakes the
   parent, which calls Dropbear's SIGWINCH handler -- the only door to
   cli_ses.winchange, which this file cannot reach directly. */
static int          con_last_x, con_last_y;    /* last-seen console size        */
static volatile int con_resized;               /* reader saw the window change  */
static void       (*con_winch)(int);           /* Dropbear's SIGWINCH handler   */

/* ACTION_DISK_INFO makes the console handler put its console.device IORequest in
   id_InUse; that request's io_Unit is the ConUnit.  It does not move for the
   life of the window, so it is cached. */
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

/* cu_XMax/cu_YMax are the LAST column and row, so the count is one more.  A
   plain memory read, no console I/O; the documented Window Status Request would
   be portable but Kickstart 3.x's CON: handler does not answer it. */
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

/* AmigaOS delivers no Unix signals, so nothing is installed.  Dropbear's
   SIGWINCH handler is remembered because it is the only door to
   cli_ses.winchange.  Return SIG_DFL, not the SIG_ERR Dropbear reads as failure. */
typedef void (*con_sigfn)(int);

con_sigfn __wrap_signal(int sig, con_sigfn handler)
{
    if (sig == SIGWINCH)
        con_winch = handler;
    return (con_sigfn)0;
}

extern int __real_ioctl(int fd, unsigned long request, ...);

/* TIOCGWINSZ answered from the console (newlib has no answer and fell back to
   80x25), socket ioctls to the library, everything else to newlib. */
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
     * 0, 1 and 2 are the parent Shell's DOS FileHandles, only lent to us:
     * newlib's close() Close()s the BPTR and the Shell then Close()s it again.
     * A double Close() on a machine with no memory protection reboots it.
     */
    if (fd == 0 || fd == 1 || fd == 2)
        return 0;

    /*
     * Nor anything else while a command is captured: run_command() closes 3 ..
     * ses.maxfd before exec'ing, and here that range is the live session socket
     * and the pipes the command's output is about to arrive on.
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

/* setnonblocking()'s F_GETFL / F_SETFL pair, the only fcntl a client makes.  A
   socket needs IoctlSocket(FIONBIO), not a flag word; a DOS handle has no
   non-blocking mode and must still succeed -- select() keeps the promise. */
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

/* WaitSelect() waits on sockets and Exec signals, never on a DOS console, so a
   child process blocks on WaitForChar() and Signal()s the parent.  Started and
   stopped by tcsetattr()'s raw/cooked switch; the stop waits for cr_DoneSig. */
#define CON_RING_SIZE   256U                  /* power of two, see the mask */
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
            /* The Amiga console introduces control sequences with an 8-bit CSI
               (0x9B).  The server and the ncurses "amiga" terminfo speak the
               7-bit ESC '[', and a lone 0x9B is not valid UTF-8 either. */
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

static void con_reader_stop(void);

static void con_reader_start(BPTR handle)
{
    ConReader     *cr;
    struct TagItem tags[5];
    static int     con_registered;

    if (con_reader != NULL)
        return;

    /*
     * Dropbear leaves through exit(), and a session torn down by an error never
     * reaches tcsetattr()'s cooked branch.  Without this the child Process
     * outlives the ConReader it signals and the signal bits are never freed.
     */
    if (!con_registered)
        con_registered = (atexit(con_reader_stop) == 0);

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

    /* Same priority as us, never higher: a higher-priority reader that keeps
       finding input never blocks, so the parent is never scheduled to drain the
       ring or service the socket and the session hangs until the typing stops. */
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
       than aborting the client, select() sets con_intr; see there. */
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

/* The Amiga console only starts a new line on CR+LF and Dropbear ends its own
   lines with a bare LF, so a CR is inserted before any LF that lacks one.  Only
   the client's own fd 1/2 output comes here; a session's stream is already CRLF. */
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
            amiga_raw_write(fd, out, o);
            o = 0;
        }

        if (c == '\n' && prev != '\r')
            out[o++] = '\r';
        out[o++] = c;
        prev = c;
    }

    if (o > 0)
        amiga_raw_write(fd, out, o);
    if (fd >= 0 && fd < 3)
        con_wr_prev[fd] = prev;

    return (int)len;
}


/* -------------------------------------------------------------- select --- */

/* WaitSelect() understands sockets and nothing else, so the mixed set is taken
   apart here.  A non-interactive DOS handle is always readable (a read returns
   data or end of file) and any handle is writable; a console asks WaitForChar(). */
static BPTR dos_handle_for(int fd)
{
    switch (fd)
    {
        case 0:  return amiga_stdio_pipe(0) != NULL ? (BPTR)0 : Input();
        case 1:  return amiga_stdio_pipe(1) != NULL ? (BPTR)0 : Output();
        case 2:  return amiga_error_handle();
        default: return (BPTR)0;
    }
}

static int dos_readable(int fd)
{
    struct amiga_mempipe *pipe = amiga_stdio_pipe(fd);
    BPTR h = dos_handle_for(fd);

    if (pipe != NULL)
        return amiga_mempipe_read_ready(pipe);
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
    int   mem_watch = 0;              /* scp's shared-memory stdin */
    int   fd;
    LONG  rc;

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
            if (want_w)                      { FD_SET(fd, &out_w); other_ready++; }
            continue;
        }

        /* The reader child owns the console handle: readiness is the ring it
           fills and its wakeup is the Exec signal folded into WaitSelect() below.
           Never WaitForChar() here, that would fight the child for the handle. */
        if (want_r && fd == 0 && con_active())
        {
            con_watch = 1;
            con_fd    = fd;
            if (con_readable()) { FD_SET(fd, &out_r); other_ready++; }
        }
        else if (want_r && fd == 0 && amiga_stdio_pipe(0) != NULL)
        {
            mem_watch = 1;
            if (dos_readable(fd)) { FD_SET(fd, &out_r); other_ready++; }
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

        /* Always include the reader's signal while watching the console, never
           conditionally on the ring being empty: a byte landing in between is a
           lost wakeup.  Ctrl-C rides the same mask and is fed on as a ^C. */
        sigs = 0;
        if (con_watch)    sigs |= con_reader->cr_DataSig;
        if (con_active()) sigs |= SIGBREAKF_CTRL_C;
        if (mem_watch)    sigs |= SIGBREAKF_CTRL_F;

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
        if (mem_watch && dos_readable(0) && !FD_ISSET(0, &out_r))
        {
            FD_SET(0, &out_r);
            ready++;
        }
    }
    else if (con_watch && !con_readable())
    {
        /* No socket in the set, only the console.  Wait on its signal or a
           Ctrl-C (a key always ends it; an interactive loop always has the
           socket too, so this is the rare case). */
        ULONG got = Wait(con_reader->cr_DataSig | SIGBREAKF_CTRL_C);

        if ((got & SIGBREAKF_CTRL_C) != 0)
            con_intr = 1;
        if (con_readable()) { FD_SET(con_fd, &out_r); ready++; }
    }
    else if (mem_watch && !dos_readable(0))
    {
        (VOID)Wait(SIGBREAKF_CTRL_F);
        if (dos_readable(0)) { FD_SET(0, &out_r); ready++; }
    }
    else if (other_ready == 0)
    {
        /* Nothing to wait on but the clock.  Delay() takes ticks of 1/50 s. */
        ULONG ticks = 0;

        if (timeout != NULL)
            ticks = (ULONG)timeout->tv_sec * TICKS_PER_SECOND
                  + (ULONG)(timeout->tv_usec / (1000000L / TICKS_PER_SECOND));

        if (ticks > 0)
            Delay(ticks);
    }

    ready += other_ready;

    if (readfds  != NULL) *readfds  = out_r;
    if (writefds != NULL) *writefds = out_w;
    if (exceptfds != NULL) FD_ZERO(exceptfds);

    return ready;
}


/* ---------------------------------------------------------------- pipe --- */

/* Eight pairs: a server makes a childpipe per connection plus ses.signal_pipe,
   and a command adds three more.  fds[0] is the read end and fds[1] the write
   end, and PIPE_IS_READ() depends on the read end being the even one. */
int pipe(int fds[2])
{
    int i;

    for (i = 0; i < DB_PIPE_PAIRS; i++)
    {
        if (!db_pipes[i].taken)
        {
            memset(&db_pipes[i], 0, sizeof(db_pipes[i]));
            db_pipes[i].taken = 1;
            fds[0] = DB_PIPE_BASE + 2 * i;
            fds[1] = DB_PIPE_BASE + 2 * i + 1;
            return 0;
        }
    }

    errno = EMFILE;
    return -1;
}


/* -------------------------------------------------------------- signals -- */

/* AmigaOS has Exec signals, which a task Wait()s on, and not POSIX signals;
   there is nothing to install.  Return SIG_DFL rather than SIG_ERR, which
   dbutil.c treats as fatal. */
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

/* spawn_command() replaced whole: upstream is three pipes and a fork().  A
   SystemTagList() Shell runs the command synchronously instead, stdin NIL: and
   stdout+stderr one file; no socket or SocketBase may cross that task boundary. */

#define DB_CMD_MAX      512

/* 256 KB.  A Shell gives a command 4,096 bytes and every ported program on this
   machine needs far more; a command arriving over SSH is as likely to be one as
   a command typed at a Shell prompt. */
#define DB_CMD_STACK    (256UL * 1024UL)

static jmp_buf exec_jump;
static int     exec_have_cmd;
static char    exec_cmd[DB_CMD_MAX];

/* ENOSYS, unless __wrap_spawn_command() is capturing: then this is a hand-back,
   taking {shell,"-c",cmd} apart and longjmp()ing to the wrapper, whose frame is
   live.  A login shell ({"-shell"}) has no answer here and never gets one. */
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

/* The AmigaOS RWED protection bits are ACTIVE LOW -- a set bit forbids -- so the
   conversion is inverted, and the other bits are cleared because a chmod states
   the whole mode.  The three POSIX classes collapse into the owner bits. */
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

/* There is no ownership to change.  ENOSYS rather than a silent 0, so a caller
   is not told a change it asked for happened. */
int chown(const char *path, uid_t uid, gid_t gid)
{
    (void)path; (void)uid; (void)gid;
    errno = ENOSYS;
    return -1;
}

/* One child, SystemTagList()'s, already finished by the time anybody asks:
   reported once, then ECHILD.  The status word is newlib's, so an AmigaDOS
   return code shifted left by eight reads as a normal exit with that code. */
static pid_t child_pid;                 /* 0 when there is nothing to report */
static int   child_status;

pid_t waitpid(pid_t pid, int *status, int options)
{
    (void)pid; (void)options;

    if (child_pid != 0)
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

/* Installs nothing and remembers one thing, SIGCHLD's handler:
   __wrap_spawn_command() calls it by hand, which is the only route by which a
   client learns a command's return code.  Reporting failure would be fatal. */
static void (*sigchld_handler)(int);

int sigaction(int sig, const struct sigaction *act, struct sigaction *oldact)
{
    if (oldact != NULL)
        memset(oldact, 0, sizeof(*oldact));

    if (sig == SIGCHLD && act != NULL)
        sigchld_handler = act->sa_handler;

    return 0;
}

/* ------------------------------------------------- __wrap_spawn_command --- */

/* Dropbear's, declared rather than included.  <syslog.h> cannot be included
   either -- proto/bsdsocket.h makes syslog() a macro -- so the two priorities
   are spelled out, with the syslog numbers. */
extern void dropbear_log(int priority, const char *format, ...);

#define DB_LOG_WARNING  4
#define DB_LOG_INFO     6

#define DB_SUCCESS      0
#define DB_FAILURE      (-1)

/* Where a command's output is parked between the Shell writing it and the
   channel reading it.  T: is the AmigaOS scratch assign; a machine without one
   gets the current directory, which execchild() has made the user's home. */
static BPTR spawn_outfile(char *name, size_t namelen)
{
    static ULONG serial;
    static const char *const dirs[] = { "T:", "" };
    unsigned i;

    for (i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++)
    {
        BPTR fh;

        snprintf(name, namelen, "%sdbssh-%lx-%lu.out", dirs[i],
                 (unsigned long)(ULONG)FindTask(NULL), (unsigned long)serial);

        fh = Open((CONST_STRPTR)name, MODE_NEWFILE);
        if (fh != (BPTR)0)
        {
            serial++;
            return fh;
        }
    }

    return (BPTR)0;
}

int __wrap_spawn_command(void (*exec_fn)(const void *user_data),
                         const void *exec_data,
                         int *ret_writefd, int *ret_readfd, int *ret_errfd,
                         pid_t *ret_pid)
{
    static pid_t next_pid = 1000;       /* not 0: chansess->pid == 0 means
                                           "no command has run on this
                                           channel" */

    struct TagItem cmd_tags[4];
    char  outname[64];
    int   in[2], out[2], err[2];
    int   nt;
    LONG  rc;
    BPTR  fh, cmd_in, cmd_out;

    /*
     * Dropbear's own child path, run in this process up to the execv(): it
     * applies the forced command, sets the environment and chdir()s home.
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
                     "amiga: no interactive shell on this machine, "
                     "give ssh a command to run");
        return DB_FAILURE;
    }

    cmd_out = spawn_outfile(outname, sizeof(outname));
    if (cmd_out == (BPTR)0)
    {
        dropbear_log(DB_LOG_WARNING, "amiga: cannot create an output file");
        return DB_FAILURE;
    }

    if (pipe(in) != 0)
    {
        Close(cmd_out);
        return DB_FAILURE;
    }
    if (pipe(out) != 0)
    {
        (void)__wrap_close(in[0]); (void)__wrap_close(in[1]);
        Close(cmd_out);
        return DB_FAILURE;
    }
    if (pipe(err) != 0)
    {
        (void)__wrap_close(in[0]);  (void)__wrap_close(in[1]);
        (void)__wrap_close(out[0]); (void)__wrap_close(out[1]);
        Close(cmd_out);
        return DB_FAILURE;
    }

    /* Only one end of each pair is ever handed out; closing the other is what
       upstream's parent branch does too, and here it is also what makes the
       output and stderr pipes report end of file. */
    (void)__wrap_close(in[0]);
    (void)__wrap_close(out[1]);
    (void)__wrap_close(err[1]);

    PIPE_PAIR(in[1])->sink = 1;         /* the command's stdin is NIL: */

    /*
     * System() does not close the handles passed in these tags -- only
     * SYS_Asynch takes ownership -- so cmd_out must be Closed below, which is
     * also what flushes it.  Omit SYS_Input rather than pass 0; dos reads the tag.
     */
    cmd_in = Open((CONST_STRPTR)"NIL:", MODE_OLDFILE);

    nt = 0;
    cmd_tags[nt].ti_Tag = SYS_Output;   cmd_tags[nt++].ti_Data = (ULONG)cmd_out;
    if (cmd_in != (BPTR)0)
    {
        cmd_tags[nt].ti_Tag = SYS_Input; cmd_tags[nt++].ti_Data = (ULONG)cmd_in;
    }
    cmd_tags[nt].ti_Tag = NP_StackSize; cmd_tags[nt++].ti_Data = DB_CMD_STACK;
    cmd_tags[nt].ti_Tag = TAG_END;      cmd_tags[nt].ti_Data   = 0;

    dropbear_log(DB_LOG_INFO, "amiga: running '%s'", exec_cmd);
    rc = SystemTagList((CONST_STRPTR)exec_cmd, cmd_tags);

    Close(cmd_out);
    if (cmd_in != (BPTR)0)
        Close(cmd_in);

    if (rc == -1)
    {
        /* System() could not start a Shell at all, which is not the command
           failing.  127 is what a Unix shell reports for the same thing and the
           only number a client can act on. */
        dropbear_log(DB_LOG_WARNING, "amiga: could not run a shell (IoErr %ld)",
                     (long)IoErr());
        rc = 127;
    }

    fh = Open((CONST_STRPTR)outname, MODE_OLDFILE);
    if (fh != (BPTR)0)
    {
        struct db_pipe *p = PIPE_PAIR(out[0]);

        p->src = fh;
        snprintf(p->srcname, sizeof(p->srcname), "%s", outname);
    }
    else
    {
        /* wclosed above already makes this end of file, so the channel closes
           cleanly with no output rather than hanging. */
        dropbear_log(DB_LOG_WARNING, "amiga: cannot read back %s", outname);
    }

    child_pid    = next_pid++;
    child_status = (int)((rc & 0xFF) << 8);

    *ret_writefd = in[1];
    *ret_readfd  = out[0];
    if (ret_errfd != NULL)
        *ret_errfd = err[0];
    if (ret_pid != NULL)
        *ret_pid = child_pid;

    /*
     * The child has already exited, so raise SIGCHLD's handler now: it writes
     * to ses.signal_pipe, the loop notices and waitpid() hands the status on.
     */
    if (sigchld_handler != NULL)
        sigchld_handler(SIGCHLD);

    return DB_SUCCESS;
}

/* <unistd.h> defines environ as (*environ_ptr), and nothing in this toolchain
   defines environ_ptr, so a client touching environ fails to link.  AmigaOS keeps
   its environment in ENV:, so an empty one is the accurate answer. */
static char  *amiga_environ_empty[1] = { NULL };
static char **amiga_environ          = amiga_environ_empty;
char        ***environ_ptr           = &amiga_environ;

/* getenv() over ENV: (-Wl,--wrap=getenv): environ is empty above, so newlib's
   getenv() answers NULL and pty-req would claim TERM=vt100, whose line-drawing
   charset this console has not got.  TERM defaults to the "amiga" terminfo. */
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

/* A Process address is unique for as long as the process exists, which is what
   dbrandom.c and gensignkey.c want: a per-run distinguisher, not an identity. */
pid_t getpid(void)
{
    return (pid_t)(ULONG)FindTask(NULL);
}

uid_t getuid(void)  { return 0; }
uid_t geteuid(void) { return 0; }
gid_t getgid(void)  { return 0; }
gid_t getegid(void) { return 0; }

/* svr_switch_user() reaches these with 0, and POSIX setuid(0) as root succeeds,
   so success is correct.  Spelled as a comparison so a future getpwnam() handing
   out a nonzero uid gets a refusal instead of appearing to work. */
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

/* NULL, always: no group database on AmigaOS 3.x, and unlike getpwnam() no group
   that could be meant.  sshpty.c wants "tty" to chown a pty node this machine
   does not have. */
struct group *getgrnam(const char *name)
{
    (void)name;
    errno = ENOENT;
    return NULL;
}

/* ENOSYS, and it has to be exactly that: with DROPBEAR_SVR_MULTIUSER 0,
   common-session.c:71 checks `getgroups(0, NULL) == -1 && errno == ENOSYS` and
   dropbear_exit()s if the call succeeds. */
int getgroups(int size, gid_t *list)
{
    (void)size;
    (void)list;
    errno = ENOSYS;
    return -1;
}

/* disallow_core() reads the limit, zeroes it and writes it back; a zeroed struct
   keeps that from operating on an uninitialised one. */
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

/* One user, and the name matters: it is what dbclient sends as the SSH username
   when the command line has no `user@host`, so ENV:USER is read first.  HOME
   feeds the ~/.ssh/id_dropbear default. */
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

/* AmigaOS has no termios; SetMode() is the raw mode an interactive session needs.
   A file or NIL: must still return ENOTTY, which is how `dbclient -T host command`
   detects "no terminal" and asks the server for no pty. */
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

    /* SetMode(h, 1) is the console's raw mode and SetMode(h, 0) cooked.  The
       switch is also where the console reader child lives and dies: raw means an
       interactive session is starting, cooked means it is ending. */
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

/* getpass() over dos.library: SetMode(handle, 1) stops the echo and makes Read()
   return each keystroke instead of waiting for a line.  A non-interactive stdin
   skips the mode change and reads a line, so a scripted run still works. */
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

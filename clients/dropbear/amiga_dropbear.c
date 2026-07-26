/*
 * clients/dropbear -- the AmigaOS half of the Dropbear port.
 *
 * WHAT THIS FILE IS
 *
 *   Everything `dbclient` calls that this toolchain does not have, in one
 *   translation unit.  It is the file curl did not need: curl's own
 *   lib/curl_setup.h already knows that on classic AmigaOS a socket is not a
 *   file descriptor, that close() is CloseSocket() and that select() is
 *   WaitSelect().  Dropbear knows none of that -- `grep -ril amiga src/`
 *   finds nothing -- so it is exactly the situation docs/RESEARCH.md §11.7
 *   predicted for wget, and this is the answer to it.
 *
 *   Nothing in third_party/dropbear is patched.  The whole port is this file,
 *   two shim headers, a localoptions.h and the flags in build.sh.
 *
 * THE ONE IDEA: TWO DESCRIPTOR SPACES, MADE INTO ONE
 *
 *   bsdsocket.library hands out socket descriptors from its own table
 *   starting at 0 (src/bsdsocket/socket.c, bsd_fd_alloc()).  newlib hands out
 *   file descriptors starting at 0 as well, and 0/1/2 are the Shell's
 *   stdin/stdout/stderr.  So `socket()` can and does return 0, and a program
 *   holding both -- which every SSH client is, since it copies between a
 *   socket and a terminal -- cannot tell them apart.
 *
 *   Rather than teach Dropbear the difference (which is a patch to every call
 *   site, rebased forever), the two spaces are merged here by OFFSET:
 *
 *       fd  0 ..  63     newlib: stdin, stdout, stderr, and files
 *       fd 64 .. 191     bsdsocket, biased by DB_SOCK_BASE
 *       fd 192, 193      the wakeup "pipe" -- see pipe() below
 *
 *   Every function that takes a descriptor dispatches on that range, so
 *   Dropbear's read()/write()/close()/select()/fcntl() do the right thing for
 *   whatever it hands them, and its `int` is an `int` everywhere.
 *
 *   read/write/close exist in newlib and cannot simply be redefined, so they
 *   are taken with `-Wl,--wrap`; the linker sends Dropbear's references to
 *   __wrap_read and leaves __real_read for us.  That also survives
 *   `atomicio(read, ...)`, which passes read as a FUNCTION POINTER -- a macro
 *   would not have.
 *
 *   FD_SETSIZE is 256 (build.sh), because 192 has to fit in an fd_set and
 *   newlib's default is 64.  dbutil.c's dropbear_fd_set() checks the bound
 *   itself, so an overflow is a legible error and not a smashed stack.
 *
 * WHAT IS HONEST AND WHAT IS A STUB
 *
 *   The socket calls are real: they are the Roadshow NDK inlines, the same
 *   121 vectors `fetch` and curl use.
 *
 *   fork(), vfork(), execv(), setsid(), dup(), dup2() and kill() FAIL with
 *   ENOSYS.  AmigaOS has no fork and pretending otherwise would produce a
 *   client that looks like it worked.  dbclient reaches them only through
 *   -J proxycmd and the SSH_ASKPASS helper, and both are compiled out in
 *   localoptions.h, so a linked dbclient contains the stubs and calls none.
 *
 *   tcgetattr()/tcsetattr() FAIL with ENOTTY.  AmigaOS has no termios; a
 *   console is a DOS handle and raw mode is SetMode().  Dropbear calls them
 *   only on the pty path, so `dbclient -T user@host command` never does and
 *   an interactive attempt fails with Dropbear's own message rather than
 *   silently sending a terminal mode nobody set.
 *
 *   pipe() returns two descriptors that are never readable.  Its one use in
 *   the client is ses.signal_pipe, whose entire job is to wake select() from
 *   a signal handler -- and signal() here installs nothing, because AmigaOS
 *   delivers no asynchronous signals to a C handler.  A pipe that never
 *   fires is therefore not a stub, it is the correct implementation of a
 *   wakeup that can never be needed.
 *
 * SPDX-License-Identifier: MIT
 */

#include <exec/types.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/bsdsocket.h>

#include <sys/types.h>
#include <sys/time.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
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
static LONG nx_waitselect(LONG n, APTR r, APTR w, APTR e, APTR t)
                                                       { return WaitSelect(n, r, w, e,
                                                                (struct __timeval *)t, 0); }
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


/* ------------------------------------------------------ the descriptor map */

#define DB_SOCK_BASE    64
#define DB_SOCK_LIMIT   192
#define DB_PIPE_BASE    192
#define DB_PIPE_LIMIT   194
#define DB_RAND_FD      194

#define IS_SOCK(fd)     ((fd) >= DB_SOCK_BASE && (fd) < DB_SOCK_LIMIT)
#define IS_PIPE(fd)     ((fd) >= DB_PIPE_BASE && (fd) < DB_PIPE_LIMIT)
#define IS_RAND(fd)     ((fd) == DB_RAND_FD)
#define SOCKOF(fd)      ((LONG)((fd) - DB_SOCK_BASE))

extern int __real_read(int fd, void *buf, size_t len);
extern int __real_write(int fd, const void *buf, size_t len);
extern int __real_close(int fd);
extern int __real_open(const char *path, int flags, ...);


/* ----------------------------------------------------- no requesters ---- */

/*
 * THE BUG THIS FIXES, BECAUSE IT COST A RUN AND WILL COST THE NEXT ONE TOO.
 *
 *   dbrandom.c's write_urandom() feeds the pool back to the random device
 *   with fopen(DROPBEAR_URANDOM_DEV, "w"), and calls the result opportunistic
 *   -- "don't worry about failure".  On Unix it is.  On AmigaOS, "RANDOM:"
 *   looks like a DEVICE NAME, and dos.library's answer to an unmounted device
 *   is not an error code: it is a system requester reading
 *
 *       Please insert volume RANDOM: in any drive
 *
 *   with Retry and Cancel buttons.  On a headless emulator run there is
 *   nobody to click Cancel, so the process waits forever and the whole run
 *   times out with `dbclient -V` as the last thing it printed.
 *
 *   pr_WindowPtr = -1 is dos.library's documented "never put up a requester,
 *   fail the call instead", and it is the right setting for ANY ported client
 *   -- every one of them assumes a failed open() returns.  It is set once, in
 *   a constructor, so it is in place before main() and therefore before
 *   Dropbear's first file access.
 *
 *   This applies to a real Amiga too, not only to the emulator: a user who
 *   ran dbclient from a Shell would get the requester on their screen.
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

/*
 * The one call in here no client needed: dbclient only ever connects.  It is
 * written the same way as socket(), including the out-of-window refusal --
 * an accepted socket comes from the same bsdsocket descriptor space as one
 * from socket(), so it can land outside the window just as easily, and on a
 * busy listener it is MORE likely to.
 *
 * Refusing by closing the accepted socket drops that one connection rather
 * than aliasing a file descriptor, which is the trade this window scheme
 * exists to make.
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
 * inet_addr() cannot distinguish 255.255.255.255 from failure, which is the
 * whole reason inet_aton() exists.  bsdsocket.library publishes inet_aton on
 * vector 98 -- but a Roadshow older than that vector would jump off the end
 * of its own LVO table, so this is done over inet_addr and the one ambiguous
 * address is spelled out.
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
 * THE ENTROPY DEVICE, AND WHAT IT ACTUALLY IS
 *
 * dbrandom.c's seedrandom() opens DROPBEAR_URANDOM_DEV, reads 32 bytes, and
 * dropbear_exit()s if it cannot -- there is no fallback and no way to build
 * without one.  This machine has no /dev/urandom, so localoptions.h renames
 * the device to "RANDOM:" (a name shaped like an AmigaOS device, so nothing
 * pretends to be Unix) and open() answers it here.
 *
 * The bytes come from src/common/ami_random.c, which is the SAME generator
 * bsdsocket.library uses for TCP initial sequence numbers and nx_secure uses
 * for TLS key agreement.  That is deliberate: one entropy story per machine,
 * and one place to fix it.
 *
 * WHAT THAT GENERATOR IS WORTH, SAID PLAINLY
 *
 *   The conditioning is textbook -- SHA-256 mixing, counter-mode expansion,
 *   forward ratchet.  The COLLECTION is guesswork.  It credits itself about
 *   21 bits from E-Clock jitter, the clock and the task list, against the 64
 *   it sets as its own bar, so ami_random_is_seeded() returns FALSE by
 *   construction and include/aminetxduo/random.h says so in its first
 *   paragraph.  Several of its sources were MEASURED identical across three
 *   cold boots under FS-UAE and are credited nothing.
 *
 *   For an SSH CLIENT that is survivable but not comfortable.  What the
 *   client generates from this pool is its ephemeral curve25519 private key
 *   and its session cookie -- both per-connection, both forward-secret, and
 *   neither of them written down.  An attacker who can predict them can read
 *   THAT session.
 *
 *   For an SSH SERVER it is not survivable, and that is the finding to carry
 *   into any server work: a host key generated from a 21-bit pool is a key an
 *   attacker can enumerate, it persists on disk, and clients pin it -- so it
 *   defeats the protocol rather than weakening it.  A server on this machine
 *   needs its host key generated somewhere else, or needs a real seed fed in
 *   through ami_random_add_entropy() before dropbearkey runs.
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

int __wrap_read(int fd, void *buf, size_t len)
{
    if (IS_SOCK(fd))
        return (int)nx_recv(SOCKOF(fd), (APTR)buf, (LONG)len, 0);

    if (IS_RAND(fd))
        return rand_fill(buf, len);

    if (IS_PIPE(fd))
    {
        /* Never has anything in it; see the header comment. */
        errno = EAGAIN;
        return -1;
    }

    return __real_read(fd, buf, len);
}

int __wrap_write(int fd, const void *buf, size_t len)
{
    if (IS_SOCK(fd))
        return (int)nx_send(SOCKOF(fd), (APTR)buf, (LONG)len, 0);

    if (IS_RAND(fd) || IS_PIPE(fd))
        return (int)len;                /* swallowed, and nothing is waiting */

    return __real_write(fd, buf, len);
}

int __wrap_close(int fd)
{
    /*
     * 0, 1 AND 2 ARE NOT OURS TO CLOSE, AND CLOSING THEM REBOOTS THE MACHINE.
     *
     * On Unix a process owns its own descriptor table and close(1) at exit is
     * free.  On AmigaOS a Shell command does NOT own Input() and Output():
     * they are the parent's DOS FileHandles, lent for the duration.  newlib's
     * close() Close()s the BPTR, and then the Shell -- or, here,
     * SystemTagList() in ClientRun -- Close()s the same freed FileHandle
     * again.  A double Close() on a machine with no memory protection is not
     * an error return.
     *
     * Dropbear does exactly this: common-channel.c's close_chan_fd() calls
     * m_close() on the session channel's readfd/writefd/errfd, and for
     * `dbclient -T host command` those three are STDIN, STDOUT and STDERR.
     *
     * MEASURED, because it looked like a success at first.  Every pass ran
     * `dbclient ... "echo AMIGA-SSH-OK; uname -a; date"`, wrote the server's
     * real answer into the report, and then the emulated A1200 REBOOTED
     * before ClientRun could write the return code -- so the command list
     * started over from the top, forever, and the run looked like a hang with
     * a correct transcript inside it.
     *
     * Refusing is right rather than merely safe: nothing in a client needs to
     * close the terminal it was given, and writes to a "closed" stdout still
     * have to work because Dropbear may report an error after closing the
     * channel.
     */
    if (fd == 0 || fd == 1 || fd == 2)
        return 0;

    if (IS_SOCK(fd))
        return (int)nx_closesocket(SOCKOF(fd));

    if (IS_RAND(fd))
    {
        rand_fd_open = 0;
        return 0;
    }

    if (IS_PIPE(fd))
        return 0;

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
 * THE LIMITATION, STATED: an interactive stdin is only noticed when
 * WaitSelect() next returns, so a console keystroke can wait as long as
 * select_timeout() -- which is the reason `dbclient -T ... command` is the
 * supported shape and an interactive session is not.  Doing better needs the
 * same mechanism a telnet server needs: a DOS handler that makes a Shell
 * look like a socket.  That is being scoped separately; it is not this file.
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
            /* readable: never.  writable: always, and nobody asks. */
            if (want_w) { FD_SET(fd, &out_w); other_ready++; }
            continue;
        }

        if (want_r && dos_readable(fd)) { FD_SET(fd, &out_r); other_ready++; }
        if (want_w)                     { FD_SET(fd, &out_w); other_ready++; }
    }

    if (have_sockets)
    {
        if (amiga_sock_init() != 0)
            return -1;

        if (other_ready > 0)
        {
            tv_zero.tv_sec  = 0;
            tv_zero.tv_usec = 0;
            tv = &tv_zero;
        }
        else
        {
            tv = timeout;
        }

        rc = nx_waitselect(sock_n, &sock_r, &sock_w, NULL, (APTR)tv);
        if (rc < 0)
            return -1;

        for (fd = DB_SOCK_BASE; fd < nfds && fd < DB_SOCK_LIMIT; fd++)
        {
            LONG s = SOCKOF(fd);

            if (FD_ISSET((int)s, &sock_r)) { FD_SET(fd, &out_r); ready++; }
            if (FD_ISSET((int)s, &sock_w)) { FD_SET(fd, &out_w); ready++; }
        }
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

static int pipe_taken = 0;

int pipe(int fds[2])
{
    if (pipe_taken)
    {
        errno = EMFILE;
        return -1;
    }

    pipe_taken = 1;
    fds[0] = DB_PIPE_BASE;
    fds[1] = DB_PIPE_BASE + 1;
    return 0;
}


/* -------------------------------------------------------------- signals -- */

/*
 * AmigaOS has Exec signals, which are bits a task Wait()s on, and not POSIX
 * signals, which interrupt a running program.  There is nothing to install.
 * Returning SIG_DFL rather than SIG_ERR matters: dbutil.c treats SIG_ERR as
 * fatal, and a client that cannot install a handler it will never receive is
 * not in trouble.
 *
 * The consequence to know about: Ctrl-C does not interrupt dbclient.  The
 * Shell's break signal (SIGBREAKF_CTRL_C) is never polled, because the place
 * to poll it is inside WaitSelect() and that is the library's loop.
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

int execv(const char *path, char *const argv[])
{
    (void)path;
    (void)argv;
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
 * SetProtection().  The RWED bits are ACTIVE LOW -- a SET bit forbids the
 * operation -- which is the opposite of every POSIX mode, so the conversion
 * is inverted deliberately and not by accident.  The other AmigaOS bits
 * (archive, script, pure) are cleared, which is what a chmod means: the
 * caller is stating the whole mode, not adjusting part of it.
 *
 * The three POSIX classes collapse into one: AmigaOS has no owner, group or
 * other, so the mode is read as the OWNER bits and the rest is dropped.
 */
int chmod(const char *path, mode_t mode)
{
    ULONG prot = 0;

    if (path == NULL) { errno = EFAULT; return -1; }

    if ((mode & S_IRUSR) == 0) prot |= FIBF_READ;
    if ((mode & S_IWUSR) == 0) prot |= FIBF_WRITE | FIBF_DELETE;
    if ((mode & S_IXUSR) == 0) prot |= FIBF_EXECUTE;

    if (!SetProtection((CONST_STRPTR)path, prot))
    {
        errno = (IoErr() == ERROR_OBJECT_NOT_FOUND) ? ENOENT : EACCES;
        return -1;
    }
    return 0;
}

/*
 * There is no ownership to change.  ENOSYS rather than a silent 0: a caller
 * that gave a file away and was told it worked would be worse off than one
 * told the platform cannot.
 */
int chown(const char *path, uid_t uid, gid_t gid)
{
    (void)path; (void)uid; (void)gid;
    errno = ENOSYS;
    return -1;
}

/*
 * ECHILD is not a stub, it is the answer.  fork() and vfork() above always
 * fail, so this process has never had a child and never can -- "no children
 * to wait for" is exactly what happened.
 */
pid_t waitpid(pid_t pid, int *status, int options)
{
    (void)pid; (void)status; (void)options;
    errno = ECHILD;
    return -1;
}

/*
 * Succeeds and installs nothing, for the same reason signal() above does:
 * AmigaOS delivers no asynchronous signals to a C handler, so there is no
 * mechanism a handler could be attached to.
 *
 * Reporting failure would be worse than reporting success.  The server calls
 * this from commonsetup() for SIGCHLD, SIGINT and SIGPIPE and treats an error
 * as fatal, so a -1 would refuse to start over handlers that could not fire
 * anyway: SIGCHLD needs a child, which fork() never produces, and bsdsocket
 * raises no SIGPIPE.
 *
 * oldact is zeroed rather than left alone, so a caller that saves and later
 * restores gets a defined value instead of stack contents.
 */
int sigaction(int sig, const struct sigaction *act, struct sigaction *oldact)
{
    (void)sig;
    (void)act;

    if (oldact != NULL)
        memset(oldact, 0, sizeof(*oldact));

    return 0;
}

/*
 * <unistd.h> defines environ as (*environ_ptr), and environ_ptr is declared
 * by that header but defined nowhere in the toolchain -- so any client that
 * touches environ fails to link rather than failing at runtime.
 *
 * An empty, correctly terminated environment is the accurate answer.  AmigaOS
 * keeps its environment in ENV:, which is a directory and not a char ** the
 * process owns; the code that wants a named variable already goes through
 * GetVar() (see amiga_pw_fill above).  The one caller of environ here is
 * execchild(), which is unreachable because execv() fails.
 */
static char  *amiga_environ_empty[1] = { NULL };
static char **amiga_environ          = amiga_environ_empty;
char        ***environ_ptr           = &amiga_environ;

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
 * Switching from 0 to 0 is not a privilege change, it is a no-op, and POSIX
 * setuid(0) as root succeeds -- so returning success is the true answer and
 * not a convenient one.
 *
 * Any OTHER id fails with EPERM, which is also true: there is nothing on this
 * machine to switch to.  Spelled as a comparison rather than an unconditional
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
 * getpwnam() -- which has one honest answer, the user running the program --
 * there is no group that could be meant.  sshpty.c uses it to find "tty" so
 * it can chown a pty node, which is a thing this machine does not have.
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
 * AmigaOS 3.x is a single-user machine with no account database at all, so
 * there is nothing to look up and the honest thing is to answer with the one
 * user there is.  The name matters: it is what dbclient sends as the SSH
 * username when the command line does not say `user@host`, so ENV:USER is
 * read first and that is the documented way to set it.  HOME likewise feeds
 * the `~/.ssh/id_dropbear` default.
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

int tcgetattr(int fd, struct termios *t)
{
    (void)fd;
    (void)t;
    errno = ENOTTY;
    return -1;
}

int tcsetattr(int fd, int actions, const struct termios *t)
{
    (void)fd;
    (void)actions;
    (void)t;
    errno = ENOTTY;
    return -1;
}

/*
 * getpass() over dos.library.  SetMode(handle, 1) puts a console into RAW
 * mode, in which it stops echoing and Read() returns each keystroke instead
 * of waiting for a line -- which is exactly what a password prompt wants.
 * A non-interactive stdin (a file, a NIL:) skips the mode change and reads a
 * line, so a scripted run still works.
 */
char *getpass(const char *prompt)
{
    static char buf[128];
    BPTR  in  = Input();
    BPTR  out = Output();
    int   raw = 0;
    ULONG n   = 0;
    char  c;

    if (prompt != NULL)
    {
        Write(out, (APTR)prompt, (LONG)strlen(prompt));
        Flush(out);
    }

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

    Write(out, (APTR)"\n", 1);
    Flush(out);

    return buf;
}

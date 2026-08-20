/*
 * AmiNetXDuo, the access patterns real network clients use.
 *
 * bsdsocktest checks that each vector behaves; this checks that the sequences
 * a ported client actually issues behave.  A stack can pass "connect() to
 * loopback listener" and still hang a client that does non-blocking connect +
 * select-for-writable + getsockopt(SO_ERROR), because nothing in the
 * per-vector suite ever puts those three together.
 *
 * Every group below is copied from a real program, named in its comment:
 *
 *   A  curl   lib/amigaos.c            SocketBaseTags() init, errno mirroring
 *   B  curl   lib/cf-socket.c          non-blocking connect -> writable -> SO_ERROR
 *   C  curl   lib/cf-socket.c          the same, to a closed port
 *   D  curl   lib/socketpair.c         wakeup_inet(): its own loopback socketpair
 *   E  control + active-mode data connection (the shape ftp used)
 *   F  curl   lib/select.c             select() over a wide, sparse fd set
 *   G  curl                            descriptor churn across many transfers
 *   H  curl   lib/cf-socket.c          send() after the peer has gone
 *   I  wget   src/connect.c            blocking connect (wget has no async path)
 *   J  ssh/nc                          a raised descriptor table
 *   K  nc -N                           half-close in both directions
 *   L  nc, telnet                      FIONREAD before reading
 *   M  nc -l                           re-listen on the same port after close
 *   N  http server                     write a whole response, then close
 *   O  nc, ssh -L                    simultaneous listening sockets
 *   P  wget, nc, ssh                 getaddrinfo / getnameinfo
 *   Q  UDP clients                    source-address value-result arguments
 *
 * Style follows tests/conformance/conf_probe.c: an ordinary AmigaOS program
 * that opens bsdsocket.library and calls vectors, linked against none of our
 * code.  Unlike the probe this one asserts, it exits RETURN_FAIL if any
 * check fails, so it can be a gate.
 *
 * SPDX-License-Identifier: MIT
 */

#include <exec/types.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include <sys/socket.h>
#include <sys/filio.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <libraries/bsdsocket.h>
#include <proto/bsdsocket.h>

#include <string.h>

struct Library *SocketBase;

/*
 * The errno curl installs with SBTC_ERRNOPTR(sizeof(errno)).  A plain int,
 * because that is what curl passes: group A checks that the library writes
 * through this pointer at the width the caller asked for.
 */
static int  c_errno;
static LONG c_h_errno;

static LONG t_fdcb_busy_fd = -1;
static LONG t_fdcb_reject_alloc;
static LONG t_fdcb_reject_free_fd = -1;
static LONG t_fdcb_checks;
static LONG t_fdcb_allocs;
static LONG t_fdcb_frees;

static LONG t_fd_callback(register LONG fd     __asm("d0"),
                          register LONG action __asm("d1"))
{
    switch (action)
    {
        case FDCB_CHECK:
            t_fdcb_checks++;
            return (fd == t_fdcb_busy_fd) ? EBUSY : 0;

        case FDCB_ALLOC:
            t_fdcb_allocs++;
            if (t_fdcb_reject_alloc)
            {
                t_fdcb_reject_alloc = 0;
                return EACCES;
            }
            return 0;

        case FDCB_FREE:
            t_fdcb_frees++;
            return (fd == t_fdcb_reject_free_fd) ? ENOTSOCK : 0;
    }

    return EINVAL;
}

#ifndef INADDR_LOOPBACK
#define INADDR_LOOPBACK 0x7f000001UL
#endif

#define BASE_PORT   7900

static ULONG t_checks;
static ULONG t_failures;

static VOID t_ok(BOOL ok, const char *what, LONG detail)
{
    t_checks++;
    if (ok)
    {
        Printf((STRPTR)"  ok   %s\n", (LONG)what);
    }
    else
    {
        t_failures++;
        Printf((STRPTR)"  FAIL %s (detail=%ld errno=%ld)\n",
               (LONG)what, detail, (LONG)c_errno);
    }
}

static VOID t_group(const char *name)
{
    Printf((STRPTR)"\n=== %s ===\n", (LONG)name);
}

static VOID addr_in(struct sockaddr_in *sa, ULONG host, UWORD port)
{
    memset(sa, 0, sizeof(*sa));
    sa->sin_family      = AF_INET;
    sa->sin_port        = htons(port);
    sa->sin_addr.s_addr = htonl(host);
}

static VOID set_nonblock(LONG fd, LONG on)
{
    LONG flag = on;

    (VOID)IoctlSocket(fd, FIONBIO, (char *)&flag);
}

/* WaitSelect() with a millisecond timeout, the way curl's our_select() does. */
static LONG wait_ms(LONG nfds, fd_set *r, fd_set *w, fd_set *e, LONG ms)
{
    struct timeval tv;

    tv.tv_secs  = ms / 1000;
    tv.tv_micro = (ms % 1000) * 1000;

    return WaitSelect(nfds, r, w, e, &tv, NULL);
}

/*
 * A listener on 127.0.0.1:port with SO_REUSEADDR, bound and listening.
 * port 0 means "pick one"; the chosen port comes back through *chosen.
 */
static LONG make_listener(UWORD port, UWORD *chosen)
{
    struct sockaddr_in sa;
    socklen_t          sl = sizeof(sa);
    LONG               fd, one = 1;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;

    (VOID)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    addr_in(&sa, INADDR_LOOPBACK, port);
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0)
    {
        CloseSocket(fd);
        return -1;
    }

    if (listen(fd, 5) < 0)
    {
        CloseSocket(fd);
        return -1;
    }

    if (chosen != NULL)
    {
        memset(&sa, 0, sizeof(sa));
        if (getsockname(fd, (struct sockaddr *)&sa, &sl) < 0)
        {
            CloseSocket(fd);
            return -1;
        }
        *chosen = ntohs(sa.sin_port);
    }

    return fd;
}

/* Blocking connect to 127.0.0.1:port. */
static LONG make_client(UWORD port)
{
    struct sockaddr_in sa;
    LONG               fd;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;

    addr_in(&sa, INADDR_LOOPBACK, port);
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0)
    {
        CloseSocket(fd);
        return -1;
    }

    return fd;
}

/* Send then receive one short pattern, both ways.  Returns TRUE if intact. */
static BOOL echo_check(LONG a, LONG b)
{
    static const char msg[] = "AmiNetXDuo client pattern";
    char              buf[64];
    LONG              rc;

    memset(buf, 0, sizeof(buf));

    rc = send(a, (UBYTE *)msg, sizeof(msg), 0);
    if (rc != (LONG)sizeof(msg))
        return FALSE;

    rc = recv(b, (UBYTE *)buf, sizeof(buf), 0);
    if (rc != (LONG)sizeof(msg))
        return FALSE;

    return (memcmp(buf, msg, sizeof(msg)) == 0);
}


/* ---- A. curl's library initialisation (lib/amigaos.c) ------------------ */

static VOID group_a(VOID)
{
    LONG rc;
    LONG fd;

    t_group("A  curl init: SocketBaseTags + errno mirroring");

    /*
     * Verbatim from Curl_amiga_init().  curl treats any nonzero return as
     * fatal and refuses to run, so an unrecognised tag here means curl never
     * issues a request.
     */
    rc = SocketBaseTags(SBTM_SETVAL(SBTC_ERRNOPTR(sizeof(c_errno))),
                        (ULONG)&c_errno,
                        SBTM_SETVAL(SBTC_LOGTAGPTR), (ULONG)"curl",
                        TAG_DONE);
    t_ok(rc == 0, "SocketBaseTags(ERRNOPTR, LOGTAGPTR) accepted", rc);

    /* And the h_errno pointer, which the resolver path needs. */
    rc = SocketBaseTags(SBTM_SETVAL(SBTC_HERRNOLONGPTR), (ULONG)&c_h_errno,
                        TAG_DONE);
    t_ok(rc == 0, "SocketBaseTags(HERRNOLONGPTR) accepted", rc);

    /* SBTF_REF says the tag data is a pointer to the value.  Null must name
       the first tag as invalid, rather than silently reading/writing zero. */
    rc = SocketBaseTags(SBTM_GETREF(SBTC_BREAKMASK), 0UL, TAG_DONE);
    t_ok(rc == 1, "SocketBaseTags rejects a null GETREF pointer", rc);

    rc = SocketBaseTags(SBTM_SETREF(SBTC_BREAKMASK), 0UL, TAG_DONE);
    t_ok(rc == 1, "SocketBaseTags rejects a null SETREF pointer", rc);

    /* A failing call must land in the caller's own int, not just internally. */
    c_errno = 0;
    rc = CloseSocket(4242);
    t_ok(rc < 0 && c_errno == EBADF,
         "errno mirrored into the caller's int at its own width", (LONG)c_errno);

    /* curl ignores getdtablesize(); ssh and nc size their fd sets with it. */
    rc = getdtablesize();
    t_ok(rc >= 64, "getdtablesize() >= 64", rc);

    /* SBTC_RELEASESTRPTR: what a client prints in its version banner. */
    {
        ULONG strptr = 0;

        rc = SocketBaseTags(SBTM_GETREF(SBTC_RELEASESTRPTR), (ULONG)&strptr,
                            TAG_DONE);
        t_ok(rc == 0 && strptr != 0, "SBTC_RELEASESTRPTR readable", rc);
    }

    /* A socket must still be creatable after all that. */
    fd = socket(AF_INET, SOCK_STREAM, 0);
    t_ok(fd >= 0, "socket() after SocketBaseTags", fd);
    if (fd >= 0)
        CloseSocket(fd);

    /* AmiTCP's legacy link-library coordination returns positive errno
       values. CHECK skips an externally occupied descriptor, while ALLOC and
       FREE may refuse an operation and their exact error must reach errno. */
    rc = SocketBaseTags(SBTM_SETVAL(SBTC_FDCALLBACK),
                        (ULONG)t_fd_callback, TAG_DONE);
    t_ok(rc == 0, "install SBTC_FDCALLBACK", rc);

    t_fdcb_busy_fd = 0;
    fd = socket(AF_INET, SOCK_STREAM, 0);
    t_ok(fd == 1 && t_fdcb_checks >= 2 && t_fdcb_allocs == 1,
         "FDCB_CHECK skips a link-library descriptor before FDCB_ALLOC", fd);

    rc = Dup2Socket(fd, 5);
    t_ok(rc == 5 && t_fdcb_allocs == 2,
         "Dup2Socket explicit target performs CHECK and ALLOC", rc);
    if (rc >= 0)
        CloseSocket(rc);

    /* A descriptor going away is not refusable: acting on an FDCB_FREE errno
       made CloseSocket() answer -1 with the socket still allocated and the
       number still taken, and an application told its close failed does not
       call it again.  The callback is told, and the close proceeds. */
    t_fdcb_reject_free_fd = fd;
    rc = CloseSocket(fd);
    t_ok(rc == 0 && t_fdcb_frees >= 1,
         "a refused FDCB_FREE does not veto CloseSocket", rc);

    t_fdcb_reject_free_fd = -1;

    t_fdcb_reject_alloc = 1;
    fd = socket(AF_INET, SOCK_STREAM, 0);
    t_ok(fd < 0 && c_errno == EACCES,
         "positive FDCB_ALLOC errno is preserved", fd);

    /* CHECK is the link library's only way to say whether a number is still
       occupied.  Once its simulated file is gone, the next allocation must
       ask again and reclaim fd 0; retaining the first refusal would leak one
       table entry per transient collision until socket() reported EMFILE. */
    t_fdcb_busy_fd = -1;
    fd = socket(AF_INET, SOCK_STREAM, 0);
    t_ok(fd == 0, "FDCB_CHECK refusal is not retained as a reservation", fd);
    if (fd >= 0)
        CloseSocket(fd);

    rc = SocketBaseTags(SBTM_SETVAL(SBTC_FDCALLBACK), 0UL, TAG_DONE);
    t_ok(rc == 0, "remove SBTC_FDCALLBACK", rc);
}


/* ---- B. non-blocking connect that succeeds (curl lib/cf-socket.c) ------ */

static VOID group_b(VOID)
{
    struct sockaddr_in sa;
    socklen_t          sl;
    LONG               lst, cli, srv, rc;
    LONG               soerr = -1;
    fd_set             wfds, efds;
    UWORD              port = 0;

    t_group("B  curl: non-blocking connect -> writable -> SO_ERROR == 0");

    lst = make_listener(0, &port);
    if (lst < 0)
    {
        t_ok(FALSE, "listener for the non-blocking connect", lst);
        return;
    }
    t_ok(port != 0, "bind(port 0) + getsockname() gave a port", (LONG)port);

    cli = socket(AF_INET, SOCK_STREAM, 0);
    set_nonblock(cli, 1);

    addr_in(&sa, INADDR_LOOPBACK, port);
    rc = connect(cli, (struct sockaddr *)&sa, sizeof(sa));

    /*
     * curl accepts either answer here: 0 means the connect completed inside
     * the call (normal on loopback), EINPROGRESS means come back on writable.
     * Anything else is an immediate connect failure and curl gives up on the
     * address.
     */
    t_ok(rc == 0 || (rc < 0 && c_errno == EINPROGRESS),
         "non-blocking connect() returned 0 or EINPROGRESS", rc);

    /* cf_socket_connect(): wait for writable, then verifyconnect(). */
    FD_ZERO(&wfds);
    FD_ZERO(&efds);
    FD_SET(cli, &wfds);
    FD_SET(cli, &efds);
    rc = wait_ms(cli + 1, NULL, &wfds, &efds, 5000);
    t_ok(rc >= 1 && FD_ISSET(cli, &wfds),
         "WaitSelect() reports the connecting socket writable", rc);

    sl = sizeof(soerr);
    rc = getsockopt(cli, SOL_SOCKET, SO_ERROR, &soerr, &sl);
    t_ok(rc == 0 && soerr == 0,
         "getsockopt(SO_ERROR) == 0 on a completed connect", soerr);

    /* verifyconnect() passing is what makes curl call getpeername() next. */
    memset(&sa, 0, sizeof(sa));
    sl = sizeof(sa);
    rc = getpeername(cli, (struct sockaddr *)&sa, &sl);
    t_ok(rc == 0 && ntohs(sa.sin_port) == port,
         "getpeername() on the completed non-blocking connect", rc);

    srv = accept(lst, NULL, NULL);
    t_ok(srv >= 0, "accept(listener, NULL, NULL)", srv);

    if (srv >= 0)
    {
        set_nonblock(cli, 0);
        t_ok(echo_check(cli, srv), "data flows on the accepted pair", 0);
        CloseSocket(srv);
    }

    CloseSocket(cli);
    CloseSocket(lst);
}


/* ---- C. non-blocking connect that is refused --------------------------- */

static VOID group_c(VOID)
{
    struct sockaddr_in sa;
    socklen_t          sl;
    LONG               cli, rc;
    LONG               soerr = -1;
    fd_set             wfds, efds;

    t_group("C  curl: non-blocking connect to a closed port -> SO_ERROR");

    cli = socket(AF_INET, SOCK_STREAM, 0);
    set_nonblock(cli, 1);

    /* Nothing is listening on this one. */
    addr_in(&sa, INADDR_LOOPBACK, BASE_PORT + 91);
    rc = connect(cli, (struct sockaddr *)&sa, sizeof(sa));
    t_ok(rc < 0, "connect() to a closed port did not report success", rc);

    if (rc < 0 && c_errno == EINPROGRESS)
    {
        FD_ZERO(&wfds);
        FD_ZERO(&efds);
        FD_SET(cli, &wfds);
        FD_SET(cli, &efds);
        rc = wait_ms(cli + 1, NULL, &wfds, &efds, 5000);
        t_ok(rc >= 1, "WaitSelect() wakes on the refused connect", rc);
    }
    else
    {
        t_ok(c_errno == ECONNREFUSED,
             "connect() failed immediately with ECONNREFUSED", (LONG)c_errno);
    }

    /*
     * curl learns why the connect failed from SO_ERROR, not from errno, and
     * reads it exactly once: BSD clears the pending error on read, and curl
     * relies on that when it retries the next address.
     */
    sl = sizeof(soerr);
    rc = getsockopt(cli, SOL_SOCKET, SO_ERROR, &soerr, &sl);
    t_ok(rc == 0 && soerr == ECONNREFUSED,
         "getsockopt(SO_ERROR) == ECONNREFUSED", soerr);

    soerr = -1;
    sl = sizeof(soerr);
    rc = getsockopt(cli, SOL_SOCKET, SO_ERROR, &soerr, &sl);
    t_ok(rc == 0 && soerr == 0, "SO_ERROR cleared on read", soerr);

    CloseSocket(cli);
}


/* ---- D. curl's own socketpair (lib/socketpair.c wakeup_inet) ----------- */

static VOID group_d(VOID)
{
    struct sockaddr_in sa;
    socklen_t          sl;
    LONG               listener, s0, s1, rc, one = 1;
    fd_set             rfds;
    char               rnd[9]  = "curlwake";
    char               check[9];

    t_group("D  curl: wakeup_inet(), its own loopback socketpair");

    /*
     * wakeup_inet() step for step.  curl builds a socketpair this way on every
     * platform without AF_UNIX, which includes AmigaOS, and does it for every
     * multi handle, so a stack that cannot do this cannot run curl at all,
     * whatever it can do for plain client sockets.
     */
    listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    t_ok(listener >= 0, "socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)", listener);
    if (listener < 0)
        return;

    rc = setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    t_ok(rc == 0, "setsockopt(SO_REUSEADDR) on the listener", rc);

    addr_in(&sa, INADDR_LOOPBACK, 0);
    rc = bind(listener, (struct sockaddr *)&sa, sizeof(sa));
    t_ok(rc == 0, "bind(127.0.0.1, port 0)", rc);

    memset(&sa, 0, sizeof(sa));
    sl = sizeof(sa);
    rc = getsockname(listener, (struct sockaddr *)&sa, &sl);
    t_ok(rc == 0 && sl >= (socklen_t)sizeof(sa) && sa.sin_port != 0,
         "getsockname() returned the ephemeral port and a full addrlen", rc);

    rc = listen(listener, 1);
    t_ok(rc == 0, "listen(listener, 1)", rc);

    s0 = socket(AF_INET, SOCK_STREAM, 0);
    rc = connect(s0, (struct sockaddr *)&sa, sizeof(sa));
    t_ok(rc == 0, "connect() to our own listener", rc);

    /* curl makes the listener non-blocking and polls before accepting. */
    set_nonblock(listener, 1);
    FD_ZERO(&rfds);
    FD_SET(listener, &rfds);
    rc = wait_ms(listener + 1, &rfds, NULL, NULL, 1000);
    t_ok(rc >= 1 && FD_ISSET(listener, &rfds),
         "WaitSelect() reports the listener readable", rc);

    s1 = accept(listener, NULL, NULL);
    t_ok(s1 >= 0, "non-blocking accept() after the poll said ready", s1);

    if (s1 >= 0)
    {
        memset(check, 0, sizeof(check));
        rc = send(s0, (UBYTE *)rnd, sizeof(rnd), 0);
        t_ok(rc == (LONG)sizeof(rnd), "write the nonce down the pair", rc);

        FD_ZERO(&rfds);
        FD_SET(s1, &rfds);
        (VOID)wait_ms(s1 + 1, &rfds, NULL, NULL, 1000);

        rc = recv(s1, (UBYTE *)check, sizeof(check), 0);
        t_ok(rc == (LONG)sizeof(rnd) && memcmp(rnd, check, sizeof(rnd)) == 0,
             "the nonce came back intact, curl accepts the pair", rc);

        /* Both ends non-blocking, as curl leaves them. */
        set_nonblock(s0, 1);
        set_nonblock(s1, 1);
        rc = recv(s1, (UBYTE *)check, sizeof(check), 0);
        t_ok(rc < 0 && c_errno == EWOULDBLOCK,
             "drained non-blocking end returns EWOULDBLOCK", rc);

        CloseSocket(s1);
    }

    CloseSocket(s0);
    CloseSocket(listener);
}


/* ---- E. a control connection and a data connection, together ---------- */

static VOID group_e(VOID)
{
    LONG  ctl_c = -1, ctl_s = -1;
    LONG  lst = -1, data_c = -1, data_s = -1;
    LONG  ctl_lst;
    UWORD ctl_port = 0, data_port = 0;
    int   round;

    t_group("E  ftp: control connection live while data connections come and go");

    /* The control connection, set up once and kept for the whole session. */
    ctl_lst = make_listener(0, &ctl_port);
    if (ctl_lst < 0)
    {
        t_ok(FALSE, "control listener", ctl_lst);
        return;
    }

    ctl_c = make_client(ctl_port);
    ctl_s = accept(ctl_lst, NULL, NULL);
    CloseSocket(ctl_lst);

    t_ok(ctl_c >= 0 && ctl_s >= 0, "control connection established", ctl_c);
    if (ctl_c < 0 || ctl_s < 0)
        return;

    t_ok(echo_check(ctl_c, ctl_s), "control connection carries data", 0);

    /*
     * Active mode: for each transfer the client opens a fresh listener, tells
     * the server the port (PORT command), the server connects back, the data
     * moves, the data connection closes, and the control connection has to
     * still be there afterwards.  Three rounds, because one working round says
     * nothing about the next.
     */
    for (round = 0; round < 3; round++)
    {
        lst = make_listener(0, &data_port);
        if (lst < 0)
        {
            t_ok(FALSE, "active-mode data listener", lst);
            break;
        }

        /* The "server" connects back to the port the PORT command named. */
        data_c = make_client(data_port);
        data_s = accept(lst, NULL, NULL);

        t_ok(data_c >= 0 && data_s >= 0,
             "data connection accepted alongside the live control connection",
             (LONG)round);

        if (data_c >= 0 && data_s >= 0)
        {
            t_ok(echo_check(data_c, data_s), "data connection carries data",
                 (LONG)round);

            /* And the control connection is untouched by any of it. */
            t_ok(echo_check(ctl_s, ctl_c),
                 "control connection still works during the transfer",
                 (LONG)round);
        }

        if (data_s >= 0)
            CloseSocket(data_s);
        if (data_c >= 0)
            CloseSocket(data_c);
        CloseSocket(lst);

        data_c = data_s = lst = -1;
    }

    /* Passive mode's shape: two outbound connections open at once. */
    {
        LONG  p_lst, p_c1, p_c2, p_s1, p_s2;
        UWORD p_port = 0;

        p_lst = make_listener(0, &p_port);
        p_c1  = (p_lst >= 0) ? make_client(p_port) : -1;
        p_s1  = (p_c1  >= 0) ? accept(p_lst, NULL, NULL) : -1;
        p_c2  = (p_s1  >= 0) ? make_client(p_port) : -1;
        p_s2  = (p_c2  >= 0) ? accept(p_lst, NULL, NULL) : -1;

        t_ok(p_s1 >= 0 && p_s2 >= 0,
             "two connections accepted on one listener, both still open", p_s2);

        if (p_s1 >= 0 && p_s2 >= 0)
        {
            t_ok(echo_check(p_c1, p_s1) && echo_check(p_c2, p_s2),
                 "both concurrent connections carry their own data", 0);
        }

        if (p_s2 >= 0) CloseSocket(p_s2);
        if (p_c2 >= 0) CloseSocket(p_c2);
        if (p_s1 >= 0) CloseSocket(p_s1);
        if (p_c1 >= 0) CloseSocket(p_c1);
        if (p_lst >= 0) CloseSocket(p_lst);
    }

    t_ok(echo_check(ctl_c, ctl_s),
         "control connection survives the whole session", 0);

    CloseSocket(ctl_s);
    CloseSocket(ctl_c);
}


/* ---- F. select() over a wide, sparse descriptor set (curl) ------------- */

#define F_PAIRS     12

static VOID group_f(VOID)
{
    LONG  lst;
    LONG  cli[F_PAIRS];
    LONG  srv[F_PAIRS];
    UWORD port = 0;
    fd_set rfds;
    LONG   maxfd = 0;
    LONG   rc, i, ready;

    t_group("F  curl: WaitSelect() over a wide, sparse descriptor set");

    for (i = 0; i < F_PAIRS; i++)
    {
        cli[i] = srv[i] = -1;
    }

    /*
     * One listener, many accepted connections, the shape of a curl multi
     * handle with parallel transfers, or an ssh session with several channels.
     */
    lst = make_listener(0, &port);
    if (lst < 0)
    {
        t_ok(FALSE, "listener for the wide set", lst);
        return;
    }
    maxfd = lst;

    for (i = 0; i < F_PAIRS; i++)
    {
        cli[i] = make_client(port);
        if (cli[i] < 0)
            break;
        srv[i] = accept(lst, NULL, NULL);
        if (srv[i] < 0)
            break;

        if (srv[i] > maxfd) maxfd = srv[i];
        if (cli[i] > maxfd) maxfd = cli[i];
    }

    t_ok(i == F_PAIRS,
         "12 connections accepted on one listener, all 25 fds open", i);

    if (i == F_PAIRS)
    {
        /* Nothing has been sent, so nothing must read as ready. */
        FD_ZERO(&rfds);
        for (i = 0; i < F_PAIRS; i++)
            FD_SET(srv[i], &rfds);

        rc = wait_ms(maxfd + 1, &rfds, NULL, NULL, 200);
        t_ok(rc == 0, "idle wide set times out with nothing set", rc);

        /* Now make exactly three of them readable, spread across the set. */
        (VOID)send(cli[0],  (UBYTE *)"a", 1, 0);
        (VOID)send(cli[5],  (UBYTE *)"b", 1, 0);
        (VOID)send(cli[11], (UBYTE *)"c", 1, 0);

        FD_ZERO(&rfds);
        for (i = 0; i < F_PAIRS; i++)
            FD_SET(srv[i], &rfds);

        rc = wait_ms(maxfd + 1, &rfds, NULL, NULL, 3000);
        t_ok(rc == 3, "WaitSelect() counted exactly the three ready sockets", rc);

        ready = 0;
        for (i = 0; i < F_PAIRS; i++)
        {
            if (FD_ISSET(srv[i], &rfds))
                ready++;
        }
        t_ok(ready == 3 && FD_ISSET(srv[0], &rfds) &&
             FD_ISSET(srv[5], &rfds) && FD_ISSET(srv[11], &rfds),
             "and set exactly those three bits, no others", ready);

        /* Everything writable at once, curl's send path asks this. */
        {
            fd_set wfds;

            FD_ZERO(&wfds);
            for (i = 0; i < F_PAIRS; i++)
                FD_SET(cli[i], &wfds);

            rc = wait_ms(maxfd + 1, NULL, &wfds, NULL, 2000);
            t_ok(rc == F_PAIRS, "all 12 client sockets report writable", rc);
        }
    }

    for (i = 0; i < F_PAIRS; i++)
    {
        if (srv[i] >= 0) CloseSocket(srv[i]);
        if (cli[i] >= 0) CloseSocket(cli[i]);
    }
    CloseSocket(lst);
}


/* ---- N. write a whole response, then close (http server, ftp data) ----- */

/*
 * Under the 8 KB receive window: the reader below does not read until the
 * writer has closed, so a payload larger than the window would deadlock a
 * blocking send(), correct BSD behaviour, and not what this group is about.
 */
#define N_BYTES     6144

static VOID group_n(VOID)
{
    static UBYTE payload[1024];

    LONG  lst, cli, srv, rc;
    UWORD port = 0;
    LONG  sent = 0, got = 0, i;
    UBYTE buf[1024];
    BOOL  intact = TRUE;

    t_group("N  server writes a response and closes, does it arrive whole?");

    for (i = 0; i < (LONG)sizeof(payload); i++)
        payload[i] = (UBYTE)(i * 7 + 3);

    lst = make_listener(0, &port);
    cli = (lst >= 0) ? make_client(port) : -1;
    srv = (cli >= 0) ? accept(lst, NULL, NULL) : -1;

    if (srv < 0)
    {
        t_ok(FALSE, "pair for the write-then-close test", srv);
        if (cli >= 0) CloseSocket(cli);
        if (lst >= 0) CloseSocket(lst);
        return;
    }

    /*
     * A non-blocking reader would interleave; this one does not read until the
     * writer has closed, so every byte has to survive in the stack's buffers
     * across the close.  A close that resets instead of finishing the
     * connection loses whatever has not been acknowledged yet, and the caller
     * sees a truncated response with no error anywhere.
     */
    while (sent < N_BYTES)
    {
        rc = send(srv, payload, sizeof(payload), 0);
        if (rc <= 0)
            break;
        sent += rc;
    }
    t_ok(sent == N_BYTES, "server wrote the whole response", sent);

    CloseSocket(srv);

    for (;;)
    {
        rc = recv(cli, buf, sizeof(buf), 0);
        if (rc <= 0)
            break;

        for (i = 0; i < rc; i++)
        {
            if (buf[i] != payload[(got + i) % sizeof(payload)])
            {
                intact = FALSE;
                break;
            }
        }
        got += rc;
    }

    t_ok(got == sent, "client read every byte the server sent before closing",
         got);
    t_ok(intact, "and the bytes were the ones that were sent", got);
    t_ok(rc == 0, "the close read as clean EOF, not as an error", rc);

    CloseSocket(cli);
    CloseSocket(lst);
}


/* ---- O. how many listeners can exist at once (nc, ftp, ssh -L) --------- */

#define O_WANT      24

static VOID group_o(VOID)
{
    LONG  lst[O_WANT];
    UWORD port;
    LONG  i, opened = 0;

    t_group("O  nc / ssh -L: simultaneous listening sockets");

    for (i = 0; i < O_WANT; i++)
        lst[i] = -1;

    for (i = 0; i < O_WANT; i++)
    {
        port   = 0;
        lst[i] = make_listener(0, &port);
        if (lst[i] < 0)
            break;
        opened++;
    }

    Printf((STRPTR)"    %ld simultaneous listeners before errno %ld\n",
           opened, (LONG)c_errno);

    /*
     * ssh with several -L forwards, or an ftp client during a multi-file
     * transfer, wants more than a handful.  16 is the floor this asserts.
     */
    t_ok(opened >= 16, "at least 16 simultaneous listeners", opened);

    for (i = 0; i < opened; i++)
        CloseSocket(lst[i]);
}


/* ---- G. descriptor churn (curl across many transfers) ------------------ */

#define G_ROUNDS    64

static VOID group_g(VOID)
{
    LONG  lst, cli, srv;
    UWORD port = 0;
    LONG  i, failures = 0, maxfd = 0;

    t_group("G  curl: descriptor churn across many transfers");

    lst = make_listener(0, &port);
    if (lst < 0)
    {
        t_ok(FALSE, "churn listener", lst);
        return;
    }

    for (i = 0; i < G_ROUNDS; i++)
    {
        cli = make_client(port);
        srv = (cli >= 0) ? accept(lst, NULL, NULL) : -1;

        if (cli < 0 || srv < 0 || !echo_check(cli, srv))
            failures++;

        if (cli > maxfd) maxfd = cli;
        if (srv > maxfd) maxfd = srv;

        if (srv >= 0) CloseSocket(srv);
        if (cli >= 0) CloseSocket(cli);
    }

    t_ok(failures == 0, "64 connect/accept/transfer/close rounds", failures);

    /*
     * Descriptors must be recycled, not consumed.  64 rounds through a
     * 64-entry table would run out on the first pass if close() leaked one.
     */
    t_ok(maxfd < 16, "descriptor numbers stayed low, fds are recycled", maxfd);

    CloseSocket(lst);
}


/* ---- H. send() after the peer has gone --------------------------------- */

static VOID group_h(VOID)
{
    LONG  lst, cli, srv, rc, i;
    UWORD port = 0;
    char  buf[512];

    t_group("H  curl: send() after the peer has gone");

    lst = make_listener(0, &port);
    cli = (lst >= 0) ? make_client(port) : -1;
    srv = (cli >= 0) ? accept(lst, NULL, NULL) : -1;

    if (srv < 0)
    {
        t_ok(FALSE, "pair for the peer-gone test", srv);
        if (cli >= 0) CloseSocket(cli);
        if (lst >= 0) CloseSocket(lst);
        return;
    }

    CloseSocket(srv);

    memset(buf, 'x', sizeof(buf));

    /*
     * The first send() after the FIN/RST may still be accepted; what matters
     * is that it fails within a bounded number of attempts rather than
     * hanging, and that the errno is one curl understands as "connection is
     * over" (EPIPE or ECONNRESET).  There is no SIGPIPE on AmigaOS, so this
     * is the only signal curl gets.
     */
    rc = 0;
    for (i = 0; i < 16; i++)
    {
        c_errno = 0;
        rc = send(cli, (UBYTE *)buf, sizeof(buf), 0);
        if (rc < 0)
            break;
    }

    t_ok(rc < 0, "send() to a closed peer eventually fails", i);
    t_ok(c_errno == EPIPE || c_errno == ECONNRESET || c_errno == ENOTCONN,
         "and fails with EPIPE / ECONNRESET / ENOTCONN", (LONG)c_errno);

    /* recv() must report end-of-stream, not block. */
    rc = recv(cli, (UBYTE *)buf, sizeof(buf), 0);
    t_ok(rc <= 0, "recv() on the dead connection returns EOF or error", rc);

    CloseSocket(cli);
    CloseSocket(lst);
}


/* ---- I. wget's blocking connect (src/connect.c) ------------------------ */

static VOID group_i(VOID)
{
    struct sockaddr_in sa;
    LONG               fd, rc;

    t_group("I  wget: BLOCKING connect (wget has no non-blocking path)");

    /*
     * wget calls connect() on a blocking socket and relies on the OS to bound
     * it; its run_with_timeout() only works where sigsetjmp+alarm exist, which
     * is not AmigaOS.  So a blocking connect to a closed port must come back
     * promptly with ECONNREFUSED; if it hangs, wget hangs, with no timeout of
     * its own.
     */
    fd = socket(AF_INET, SOCK_STREAM, 0);
    addr_in(&sa, INADDR_LOOPBACK, BASE_PORT + 92);

    c_errno = 0;
    rc = connect(fd, (struct sockaddr *)&sa, sizeof(sa));
    t_ok(rc < 0, "blocking connect() to a closed port failed", rc);
    t_ok(c_errno == ECONNREFUSED,
         "and failed with ECONNREFUSED, not a timeout", (LONG)c_errno);

    CloseSocket(fd);

    /* wget's bind_local(): the FTP active-mode listener, its exact sequence. */
    {
        struct sockaddr_in bsa;
        socklen_t          bsl;
        LONG               one = 1;
        LONG               lfd;
        int                port = 0;

        lfd = socket(AF_INET, SOCK_STREAM, 0);
        rc  = setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        t_ok(rc == 0, "bind_local: setsockopt(SO_REUSEADDR)", rc);

        addr_in(&bsa, INADDR_LOOPBACK, 0);
        rc = bind(lfd, (struct sockaddr *)&bsa, sizeof(bsa));
        t_ok(rc == 0, "bind_local: bind()", rc);

        bsl = sizeof(bsa);
        rc  = getsockname(lfd, (struct sockaddr *)&bsa, &bsl);
        port = ntohs(bsa.sin_port);
        t_ok(rc == 0 && port != 0, "bind_local: getsockname() gave the port", rc);

        rc = listen(lfd, 1);
        t_ok(rc == 0, "bind_local: listen()", rc);

        /* accept_connection(): select for read, then accept with a sockaddr. */
        {
            LONG               peer;
            fd_set             rfds;
            struct sockaddr_in psa;
            socklen_t          psl = sizeof(psa);

            peer = make_client((UWORD)port);

            FD_ZERO(&rfds);
            FD_SET(lfd, &rfds);
            rc = wait_ms(lfd + 1, &rfds, NULL, NULL, 3000);
            t_ok(rc >= 1, "accept_connection: select said readable", rc);

            memset(&psa, 0, sizeof(psa));
            rc = accept(lfd, (struct sockaddr *)&psa, &psl);
            t_ok(rc >= 0 && psa.sin_family == AF_INET && psa.sin_addr.s_addr != 0,
                 "accept() filled the peer sockaddr", rc);

            if (rc >= 0) CloseSocket(rc);
            if (peer >= 0) CloseSocket(peer);
        }

        CloseSocket(lfd);
    }
}


/* ---- J. a raised descriptor table (ssh, nc with many channels) --------- */

#define J_WANT      100

static VOID group_j(VOID)
{
    LONG fds[J_WANT];
    LONG rc, i, opened = 0, highest = -1;

    t_group("J  ssh/nc: a raised descriptor table");

    rc = SocketBaseTags(SBTM_SETVAL(SBTC_DTABLESIZE), 128, TAG_DONE);
    t_ok(rc == 0, "SocketBaseTags(SBTC_DTABLESIZE, 128) accepted", rc);

    rc = getdtablesize();
    t_ok(rc == 128, "getdtablesize() now reports 128", rc);

    for (i = 0; i < J_WANT; i++)
    {
        fds[i] = socket(AF_INET, SOCK_STREAM, 0);
        if (fds[i] < 0)
            break;
        opened++;
        if (fds[i] > highest)
            highest = fds[i];
    }

    t_ok(opened == J_WANT, "100 sockets open simultaneously", opened);
    t_ok(highest >= 64, "descriptors were handed out above 64", highest);

    /*
     * WaitSelect() over a range wider than the old table.  The caller's fd_set
     * has to be big enough, FD_SETSIZE is 64 in this toolchain, so this
     * uses a hand-built 128-bit map, which is what a client that raised the
     * table would have to do too.
     */
    if (highest >= 64)
    {
        ULONG          bitmap[4];       /* 128 bits */
        struct timeval tv;

        memset(bitmap, 0, sizeof(bitmap));
        bitmap[highest / 32] |= (1UL << (highest % 32));

        tv.tv_secs  = 0;
        tv.tv_micro = 200000;

        rc = WaitSelect(highest + 1, NULL, (fd_set *)bitmap, NULL, &tv, NULL);
        t_ok(rc >= 0, "WaitSelect() accepts nfds past the old 64-fd table", rc);
    }

    for (i = 0; i < opened; i++)
        CloseSocket(fds[i]);

    /* Put it back, so later groups run on the shipped default. */
    (VOID)SocketBaseTags(SBTM_SETVAL(SBTC_DTABLESIZE), 64, TAG_DONE);
}


/* ---- K. half-close in both directions (nc -N, ftp) --------------------- */

static VOID group_k(VOID)
{
    LONG  lst, cli, srv, rc;
    UWORD port = 0;
    char  buf[64];

    t_group("K  nc -N / ftp: half-close in both directions");

    lst = make_listener(0, &port);
    cli = (lst >= 0) ? make_client(port) : -1;
    srv = (cli >= 0) ? accept(lst, NULL, NULL) : -1;

    if (srv < 0)
    {
        t_ok(FALSE, "pair for the half-close test", srv);
        if (cli >= 0) CloseSocket(cli);
        if (lst >= 0) CloseSocket(lst);
        return;
    }

    /* Client sends its request, then says "that is all I will send". */
    rc = send(cli, (UBYTE *)"REQUEST", 7, 0);
    t_ok(rc == 7, "client sent its request", rc);

    rc = shutdown(cli, 1 /* SHUT_WR */);
    t_ok(rc == 0, "shutdown(SHUT_WR) on the client", rc);

    /* The server must still see the request, then EOF. */
    memset(buf, 0, sizeof(buf));
    rc = recv(srv, (UBYTE *)buf, sizeof(buf), 0);
    t_ok(rc == 7 && memcmp(buf, "REQUEST", 7) == 0,
         "server read the request sent before the half-close", rc);

    rc = recv(srv, (UBYTE *)buf, sizeof(buf), 0);
    t_ok(rc == 0, "server then sees clean EOF", rc);

    /*
     * The half-closed client now selects for read with nothing on the wire.
     * `nc -N` sits in exactly this loop waiting for the rest of the answer, so
     * it must sleep: a socket that has sent its own FIN and heard nothing back
     * is not readable, however close FIN_WAIT_1 looks to a state comparison.
     */
    {
        fd_set rfds;

        FD_ZERO(&rfds);
        FD_SET(cli, &rfds);
        rc = wait_ms(cli + 1, &rfds, NULL, NULL, 400);

        t_ok(rc == 0,
             "half-closed socket with nothing pending is NOT readable", rc);
    }

    /* And the server can still reply down the still-open direction. */
    rc = send(srv, (UBYTE *)"REPLY", 5, 0);
    t_ok(rc == 5, "server replies after the client half-closed", rc);

    memset(buf, 0, sizeof(buf));
    rc = recv(cli, (UBYTE *)buf, sizeof(buf), 0);
    t_ok(rc == 5 && memcmp(buf, "REPLY", 5) == 0,
         "client read the reply on its still-open receive side", rc);

    CloseSocket(srv);
    CloseSocket(cli);
    CloseSocket(lst);
}


/* ---- L. FIONREAD before reading (nc, telnet) --------------------------- */

static VOID group_l(VOID)
{
    LONG  lst, cli, srv, rc;
    UWORD port = 0;
    LONG  avail = -1;
    char  buf[64];
    fd_set rfds;

    t_group("L  nc / telnet: FIONREAD before reading");

    lst = make_listener(0, &port);
    cli = (lst >= 0) ? make_client(port) : -1;
    srv = (cli >= 0) ? accept(lst, NULL, NULL) : -1;

    if (srv < 0)
    {
        t_ok(FALSE, "pair for the FIONREAD test", srv);
        if (cli >= 0) CloseSocket(cli);
        if (lst >= 0) CloseSocket(lst);
        return;
    }

    avail = -1;
    rc = IoctlSocket(srv, FIONREAD, (char *)&avail);
    t_ok(rc == 0 && avail == 0, "FIONREAD on an idle socket reports 0", avail);

    (VOID)send(cli, (UBYTE *)"0123456789", 10, 0);

    FD_ZERO(&rfds);
    FD_SET(srv, &rfds);
    (VOID)wait_ms(srv + 1, &rfds, NULL, NULL, 3000);

    avail = -1;
    rc = IoctlSocket(srv, FIONREAD, (char *)&avail);
    t_ok(rc == 0 && avail == 10, "FIONREAD reports the 10 queued bytes", avail);

    rc = recv(srv, (UBYTE *)buf, 4, 0);
    t_ok(rc == 4, "partial read of 4 bytes", rc);

    avail = -1;
    rc = IoctlSocket(srv, FIONREAD, (char *)&avail);
    t_ok(rc == 0 && avail == 6, "FIONREAD reports the 6 bytes still buffered",
         avail);

    (VOID)recv(srv, (UBYTE *)buf, sizeof(buf), 0);

    CloseSocket(srv);
    CloseSocket(cli);
    CloseSocket(lst);
}


/* ---- M. re-listen on the same port after close (nc -l, ftp) ------------ */

static VOID group_m(VOID)
{
    LONG  lst, cli, srv;
    UWORD port = 0;
    LONG  i, failures = 0;

    t_group("M  nc -l / ftp: re-listen on the SAME port after close");

    /* Round 0 picks the port; rounds 1..3 must get it back. */
    lst = make_listener(0, &port);
    if (lst < 0)
    {
        t_ok(FALSE, "first listener", lst);
        return;
    }
    CloseSocket(lst);

    for (i = 0; i < 4; i++)
    {
        lst = make_listener(port, NULL);
        if (lst < 0)
        {
            failures++;
            Printf((STRPTR)"    round %ld: re-bind of port %ld failed, errno %ld\n",
                   i, (LONG)port, (LONG)c_errno);
            break;
        }

        cli = make_client(port);
        srv = (cli >= 0) ? accept(lst, NULL, NULL) : -1;

        if (srv < 0 || !echo_check(cli, srv))
            failures++;

        if (srv >= 0) CloseSocket(srv);
        if (cli >= 0) CloseSocket(cli);
        CloseSocket(lst);
    }

    t_ok(failures == 0,
         "4 listen/accept/close cycles on one port with SO_REUSEADDR",
         failures);
}


/* ---- P. getaddrinfo, both directions (wget, nc, ssh) ------------------- */

static VOID group_p(VOID)
{
    struct addrinfo  hints;
    struct addrinfo *res = NULL;
    LONG             rc;
    char             hbuf[64];
    char             sbuf[32];

    t_group("P  wget / nc / ssh: getaddrinfo and getnameinfo");

    /*
     * These four vectors are in the Roadshow tail of the LVO table, past where
     * bsdsocktest's own inline header stops, so nothing in the conformance
     * suite calls them and this is their only coverage in a non-IPv6 build.
     */

    /* 1. The numeric client lookup: what nc and ssh do for a dotted quad. */
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags    = AI_NUMERICHOST;

    rc = getaddrinfo((STRPTR)"127.0.0.1", (STRPTR)"80", &hints, &res);
    t_ok(rc == 0 && res != NULL, "getaddrinfo(127.0.0.1, 80, AI_NUMERICHOST)", rc);

    if (rc == 0 && res != NULL)
    {
        struct sockaddr_in *sa = (struct sockaddr_in *)res->ai_addr;

        t_ok(res->ai_family == AF_INET &&
             res->ai_addrlen >= (socklen_t)sizeof(struct sockaddr_in) &&
             sa != NULL && sa->sin_family == AF_INET &&
             ntohl(sa->sin_addr.s_addr) == INADDR_LOOPBACK &&
             ntohs(sa->sin_port) == 80,
             "and the result is 127.0.0.1 port 80 in a sockaddr_in",
             (LONG)(sa ? ntohs(sa->sin_port) : -1));

        freeaddrinfo(res);
        res = NULL;
    }

    /* An overlong decimal service must not wrap modulo ULONG and select an
       unrelated valid port (4294967376 is 80 modulo 2^32). */
    hints.ai_flags |= AI_NUMERICSERV;
    rc = getaddrinfo((STRPTR)"127.0.0.1", (STRPTR)"4294967376",
                     &hints, &res);
    t_ok(rc == EAI_NONAME && res == NULL,
         "getaddrinfo rejects an overflowing numeric service", rc);

    /* A protocol-only hint has to synthesize the matching socket type. The
       old STREAM/UDP result was internally inconsistent and socket() rejected
       the supposedly usable addrinfo node. */
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_protocol = IPPROTO_UDP;
    hints.ai_flags    = AI_NUMERICHOST;

    rc = getaddrinfo((STRPTR)"127.0.0.1", (STRPTR)"53", &hints, &res);
    t_ok(rc == 0 && res != NULL && res->ai_socktype == SOCK_DGRAM &&
         res->ai_protocol == IPPROTO_UDP,
         "getaddrinfo(IPPROTO_UDP) synthesizes SOCK_DGRAM", rc);
    if (rc == 0 && res != NULL)
    {
        LONG fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);

        t_ok(fd >= 0, "socket() accepts the protocol-only result", fd);
        if (fd >= 0) CloseSocket(fd);
        freeaddrinfo(res);
        res = NULL;
    }

    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_UDP;
    hints.ai_flags    = AI_NUMERICHOST;

    rc = getaddrinfo((STRPTR)"127.0.0.1", NULL, &hints, &res);
    t_ok(rc != 0, "getaddrinfo rejects SOCK_STREAM/IPPROTO_UDP", rc);

    /* Only ai_flags, ai_family, ai_socktype, and ai_protocol are inputs.
       Roadshow exposes EAI_BADHINTS specifically for dirty output fields. */
    memset(&hints, 0, sizeof(hints));
    hints.ai_addrlen = 1;
    rc = getaddrinfo((STRPTR)"127.0.0.1", NULL, &hints, &res);
    t_ok(rc == EAI_BADHINTS, "getaddrinfo rejects hints.ai_addrlen", rc);

    memset(&hints, 0, sizeof(hints));
    hints.ai_addr = (struct sockaddr *)&hints;
    rc = getaddrinfo((STRPTR)"127.0.0.1", NULL, &hints, &res);
    t_ok(rc == EAI_BADHINTS, "getaddrinfo rejects hints.ai_addr", rc);

    memset(&hints, 0, sizeof(hints));
    hints.ai_canonname = (char *)&hints;
    rc = getaddrinfo((STRPTR)"127.0.0.1", NULL, &hints, &res);
    t_ok(rc == EAI_BADHINTS, "getaddrinfo rejects hints.ai_canonname", rc);

    memset(&hints, 0, sizeof(hints));
    hints.ai_next = &hints;
    rc = getaddrinfo((STRPTR)"127.0.0.1", NULL, &hints, &res);
    t_ok(rc == EAI_BADHINTS, "getaddrinfo rejects hints.ai_next", rc);

    /*
     * A service name with NO hints at all. Naming neither a socket type nor a
     * protocol means both are acceptable, so DEVS:Internet/services has to be
     * searched for udp as well as tcp, and the result must describe the
     * protocol the service actually exists for. Defaulting socktype before
     * the lookup instead of after it made every udp-only service EAI_SERVICE.
     */
    rc = getaddrinfo((STRPTR)"127.0.0.1", (STRPTR)"tftp", NULL, &res);
    t_ok(rc == 0 && res != NULL && res->ai_socktype == SOCK_DGRAM &&
         res->ai_protocol == IPPROTO_UDP,
         "getaddrinfo(no hints) resolves a udp-only service", rc);
    if (rc == 0 && res != NULL)
    {
        struct sockaddr_in *sin = (struct sockaddr_in *)res->ai_addr;

        t_ok(sin != NULL && ntohs(sin->sin_port) == 69,
             "the udp-only service carries its port", rc);
        freeaddrinfo(res);
        res = NULL;
    }

    /* The tcp half of the same rule: still tried first, still SOCK_STREAM. */
    rc = getaddrinfo((STRPTR)"127.0.0.1", (STRPTR)"ftp", NULL, &res);
    t_ok(rc == 0 && res != NULL && res->ai_socktype == SOCK_STREAM &&
         res->ai_protocol == IPPROTO_TCP,
         "getaddrinfo(no hints) resolves a tcp-only service", rc);
    if (rc == 0 && res != NULL)
    {
        freeaddrinfo(res);
        res = NULL;
    }

    /* 2. The server lookup: AI_PASSIVE, then bind and listen on the result. */
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags    = AI_PASSIVE;

    rc = getaddrinfo(NULL, (STRPTR)"0", &hints, &res);
    t_ok(rc == 0 && res != NULL, "getaddrinfo(NULL, AI_PASSIVE)", rc);

    if (rc == 0 && res != NULL)
    {
        LONG fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);

        t_ok(fd >= 0, "socket() from the getaddrinfo result", fd);

        if (fd >= 0)
        {
            rc = bind(fd, res->ai_addr, res->ai_addrlen);
            t_ok(rc == 0, "bind() to the AI_PASSIVE address", rc);

            rc = listen(fd, 1);
            t_ok(rc == 0, "listen() on it, the nc -l path", rc);

            CloseSocket(fd);
        }

        freeaddrinfo(res);
        res = NULL;
    }

    /* 3. A name, resolved and then actually connected to. */
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    rc = getaddrinfo((STRPTR)"localhost", NULL, &hints, &res);
    t_ok(rc == 0 && res != NULL, "getaddrinfo(\"localhost\")", rc);

    if (rc == 0 && res != NULL)
    {
        struct sockaddr_in *sa = (struct sockaddr_in *)res->ai_addr;
        UWORD               port = 0;
        LONG                lst, srv;

        lst = make_listener(0, &port);
        if (lst >= 0 && sa != NULL)
        {
            LONG fd = socket(res->ai_family, res->ai_socktype,
                             res->ai_protocol);

            sa->sin_port = htons(port);
            rc = connect(fd, res->ai_addr, res->ai_addrlen);
            srv = (rc == 0) ? accept(lst, NULL, NULL) : -1;

            t_ok(rc == 0 && srv >= 0,
                 "connect() straight to the getaddrinfo result", rc);
            if (srv >= 0)
            {
                t_ok(echo_check(fd, srv), "and data flows over it", 0);
                CloseSocket(srv);
            }

            if (fd >= 0) CloseSocket(fd);
        }
        if (lst >= 0) CloseSocket(lst);

        freeaddrinfo(res);
        res = NULL;
    }

    /* 4. getnameinfo, the reverse direction, ftp's PORT, nc -v, ssh logs. */
    {
        struct sockaddr_in sa;

        addr_in(&sa, INADDR_LOOPBACK, 80);
        memset(hbuf, 0, sizeof(hbuf));
        memset(sbuf, 0, sizeof(sbuf));

        rc = getnameinfo((struct sockaddr *)&sa, sizeof(sa),
                         (STRPTR)hbuf, sizeof(hbuf),
                         (STRPTR)sbuf, sizeof(sbuf),
                         NI_NUMERICHOST | NI_NUMERICSERV);
        t_ok(rc == 0 && strcmp(hbuf, "127.0.0.1") == 0 &&
             strcmp(sbuf, "80") == 0,
             "getnameinfo(NI_NUMERICHOST|NI_NUMERICSERV)", rc);
        if (rc != 0 || strcmp(hbuf, "127.0.0.1") != 0)
            Printf((STRPTR)"    host=\"%s\" serv=\"%s\"\n",
                   (LONG)hbuf, (LONG)sbuf);

        /* NI_NAMEREQD asks about the host, not the service. Port 65000 is
           absent from the staged services file, and the decimal fallback is
           the correct answer: failing here would break the ordinary
           resolve-the-peer call, whose port is always ephemeral. */
        addr_in(&sa, INADDR_LOOPBACK, 65000);
        memset(sbuf, 0, sizeof(sbuf));
        rc = getnameinfo((struct sockaddr *)&sa, sizeof(sa),
                         NULL, 0, (STRPTR)sbuf, sizeof(sbuf), NI_NAMEREQD);
        t_ok(rc == 0 && strcmp(sbuf, "65000") == 0,
             "NI_NAMEREQD does not require a service name", rc);

        rc = getnameinfo((struct sockaddr *)&sa, sizeof(sa),
                         (STRPTR)hbuf, sizeof(hbuf), NULL, 0, 0x80000000UL);
        t_ok(rc == EAI_BADFLAGS,
             "getnameinfo() rejects undefined flag bits", rc);
    }

    /* 5. A name that cannot resolve must fail, not hang, and gai_strerror
       must have something to say about why. */
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags    = AI_NUMERICHOST;     /* so this cannot go to DNS */

    rc = getaddrinfo((STRPTR)"not-a-host.invalid", NULL, &hints, &res);
    t_ok(rc != 0, "getaddrinfo() of an unresolvable name fails", rc);
    if (rc == 0 && res != NULL)
        freeaddrinfo(res);

    {
        STRPTR msg = gai_strerror(EAI_NONAME);

        t_ok(msg != NULL && msg[0] != '\0', "gai_strerror(EAI_NONAME)", 0);
    }
}


/* ---- Q. datagram source-address arguments ----------------------------- */

static VOID group_q(VOID)
{
    struct sockaddr_in local;
    struct sockaddr_in from;
    struct msghdr      msg;
    struct iovec       iov;
    socklen_t          local_len;
    LONG               fd;
    LONG               rc;
    char               sent = 'q';
    char               received = 0;

    t_group("Q  UDP clients: source-address value-result arguments");

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
    {
        t_ok(FALSE, "create UDP socket for address argument checks", fd);
        return;
    }

    addr_in(&local, INADDR_LOOPBACK, 0);
    rc = bind(fd, (struct sockaddr *)&local, sizeof(local));
    local_len = sizeof(local);
    if (rc == 0)
        rc = getsockname(fd, (struct sockaddr *)&local, &local_len);
    t_ok(rc == 0, "bind UDP socket for address argument checks", rc);

    if (rc == 0)
        rc = sendto(fd, &sent, 1, 0, (struct sockaddr *)&local,
                    sizeof(local));
    t_ok(rc == 1, "queue a loopback datagram", rc);

    memset(&from, 0, sizeof(from));
    rc = recvfrom(fd, &received, 1, 0, (struct sockaddr *)&from, NULL);
    t_ok(rc < 0 && c_errno == EFAULT,
         "recvfrom(address, NULL length) fails without consuming data", rc);

    memset(&msg, 0, sizeof(msg));
    memset(&from, 0, sizeof(from));
    iov.iov_base       = &received;
    iov.iov_len        = 1;
    msg.msg_name       = &from;
    msg.msg_namelen    = 0;
    msg.msg_iov        = &iov;
    msg.msg_iovlen     = 1;

    rc = recvmsg(fd, &msg, 0);
    t_ok(rc == 1 && received == sent &&
         msg.msg_namelen == (socklen_t)sizeof(struct sockaddr_in),
         "recvmsg reports required source-address length at capacity zero",
         rc);

    CloseSocket(fd);
}


/* ------------------------------------------------------------------ main -- */

int main(void)
{
    SocketBase = OpenLibrary((STRPTR)"bsdsocket.library", 4);
    if (SocketBase == NULL)
    {
        Printf((STRPTR)"bsdsocket.library not available\n");
        return RETURN_FAIL;
    }

    Printf((STRPTR)"AmiNetXDuo, client access patterns\n");

    group_a();
    group_b();
    group_c();
    group_d();
    group_e();
    group_f();
    group_g();
    group_h();
    group_i();
    group_j();
    group_k();
    group_l();
    group_m();
    group_n();
    group_o();
    group_p();
    group_q();

    Printf((STRPTR)"\n%ld checks, %ld failures\n",
           (LONG)t_checks, (LONG)t_failures);

    CloseLibrary(SocketBase);

    return (t_failures == 0) ? RETURN_OK : RETURN_FAIL;
}

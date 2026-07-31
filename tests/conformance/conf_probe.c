/*
 * AmiNetXDuo -- conformance triage probe.
 *
 * bsdsocktest reports pass/fail; when a whole category collapses it does not
 * say which call broke first.  This walks the same sequence by hand and
 * prints the return value and errno of every step, so a wall of "not ok" can
 * be reduced to one root cause.
 *
 * Order matters: everything that needs a pristine library runs before
 * anything that churns descriptors, because a descriptor-table or NetX Duo
 * bookkeeping bug poisons every later call and hides what was actually
 * broken.
 *
 * Not a test: it asserts nothing and always exits 0.  Read the output.
 *
 * SPDX-License-Identifier: MIT
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include <sys/socket.h>
#include <sys/filio.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <libraries/bsdsocket.h>
#include <proto/bsdsocket.h>

#include <string.h>

struct Library *SocketBase;

static LONG bsd_errno;
static LONG bsd_h_errno;

/* Not in the Amiga netinet/in.h, same as bsdsocktest's own testutil.h. */
#ifndef INADDR_LOOPBACK
#define INADDR_LOOPBACK 0x7f000001UL
#endif

#define PORT 7700

static VOID p(const char *what, LONG rc)
{
    Printf((STRPTR)"%-44s rc=%-6ld errno=%ld\n", (LONG)what, rc, bsd_errno);
}

static VOID addr_in(struct sockaddr_in *sa, ULONG host, UWORD port)
{
    memset(sa, 0, sizeof(*sa));
    sa->sin_family = AF_INET;
    sa->sin_port   = htons(port);
    sa->sin_addr.s_addr = htonl(host);
}

/* ---- 1. a full loopback conversation on a pristine library ------------- */

static VOID probe_loopback(VOID)
{
    struct sockaddr_in sa;
    socklen_t          sl;
    LONG               lst, cli, srv, rc;
    LONG               one = 1;
    LONG               val = 0;
    char               buf[64];

    Printf((STRPTR)"=== 1. loopback, pristine library ===\n");

    lst = socket(AF_INET, SOCK_STREAM, 0);      p("socket() listener", lst);
    rc = setsockopt(lst, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    p("setsockopt(SO_REUSEADDR)", rc);

    addr_in(&sa, INADDR_LOOPBACK, PORT);
    rc = bind(lst, (struct sockaddr *)&sa, sizeof(sa));
    p("bind(127.0.0.1:7700)", rc);

    rc = listen(lst, 5);                        p("listen(5)", rc);

    sl = sizeof(sa);
    rc = getsockname(lst, (struct sockaddr *)&sa, &sl);
    p("getsockname(listener)", rc);
    Printf((STRPTR)"    -> port=%ld addr=%08lx len=%ld\n",
           (LONG)ntohs(sa.sin_port), (LONG)ntohl(sa.sin_addr.s_addr), (LONG)sl);

    cli = socket(AF_INET, SOCK_STREAM, 0);      p("socket() client", cli);
    addr_in(&sa, INADDR_LOOPBACK, PORT);
    rc = connect(cli, (struct sockaddr *)&sa, sizeof(sa));
    p("connect(127.0.0.1:7700)", rc);

    sl = sizeof(sa);
    srv = accept(lst, (struct sockaddr *)&sa, &sl);
    p("accept()", srv);

    rc = send(cli, (APTR)"hello", 5, 0);        p("send(5)", rc);
    memset(buf, 0, sizeof(buf));
    rc = recv(srv, (APTR)buf, sizeof(buf), 0);  p("recv()", rc);
    Printf((STRPTR)"    -> \"%s\"\n", (LONG)buf);

    val = 0; sl = sizeof(val);
    rc = getsockopt(cli, SOL_SOCKET, SO_TYPE, &val, &sl);
    p("getsockopt(SO_TYPE)", rc);
    Printf((STRPTR)"    -> val=%ld len=%ld (SOCK_STREAM=%ld)\n",
           val, (LONG)sl, (LONG)SOCK_STREAM);

    one = 1;
    rc = IoctlSocket(cli, FIONBIO, (APTR)&one); p("IoctlSocket(FIONBIO,1)", rc);
    val = 0;
    rc = IoctlSocket(srv, FIONREAD, (APTR)&val);p("IoctlSocket(FIONREAD)", rc);
    Printf((STRPTR)"    -> pending=%ld\n", val);

    CloseSocket(cli);
    CloseSocket(srv);
    CloseSocket(lst);
}

/* ---- 2. UDP on loopback ------------------------------------------------ */

static VOID probe_udp(VOID)
{
    struct sockaddr_in sa;
    socklen_t          sl;
    LONG               rx, tx, rc;
    char               buf[64];

    Printf((STRPTR)"=== 2. UDP loopback ===\n");

    rx = socket(AF_INET, SOCK_DGRAM, 0);        p("socket() udp rx", rx);
    addr_in(&sa, INADDR_LOOPBACK, PORT + 1);
    rc = bind(rx, (struct sockaddr *)&sa, sizeof(sa));
    p("bind(udp 127.0.0.1:7701)", rc);

    if (rc < 0)
    {
        addr_in(&sa, INADDR_ANY, PORT + 1);
        rc = bind(rx, (struct sockaddr *)&sa, sizeof(sa));
        p("bind(udp 0.0.0.0:7701)", rc);
    }

    tx = socket(AF_INET, SOCK_DGRAM, 0);        p("socket() udp tx", tx);
    addr_in(&sa, INADDR_LOOPBACK, PORT + 1);
    rc = sendto(tx, (APTR)"udp", 3, 0, (struct sockaddr *)&sa, sizeof(sa));
    p("sendto(127.0.0.1:7701)", rc);

    memset(buf, 0, sizeof(buf));
    sl = sizeof(sa);
    rc = recvfrom(rx, (APTR)buf, sizeof(buf), 0, (struct sockaddr *)&sa, &sl);
    p("recvfrom()", rc);
    Printf((STRPTR)"    -> \"%s\" from %08lx:%ld\n", (LONG)buf,
           (LONG)ntohl(sa.sin_addr.s_addr), (LONG)ntohs(sa.sin_port));

    CloseSocket(rx);
    CloseSocket(tx);
}

/* ---- 3. descriptor churn ---------------------------------------------- */

static VOID probe_churn(VOID)
{
    LONG fds[8];
    LONG i, rc;

    Printf((STRPTR)"=== 3. create 8 / close 8 / create 8 ===\n");

    for (i = 0; i < 8; i++)
    {
        fds[i] = socket(AF_INET, SOCK_STREAM, 0);
        Printf((STRPTR)"  create[%ld] rc=%-4ld errno=%ld\n",
               i, fds[i], bsd_errno);
    }
    for (i = 0; i < 8; i++)
    {
        rc = CloseSocket(fds[i]);
        Printf((STRPTR)"  close [%ld] rc=%-4ld errno=%ld\n", i, rc, bsd_errno);
    }
    for (i = 0; i < 8; i++)
    {
        fds[i] = socket(AF_INET, SOCK_STREAM, 0);
        Printf((STRPTR)"  again [%ld] rc=%-4ld errno=%ld\n",
               i, fds[i], bsd_errno);
    }
    for (i = 0; i < 8; i++)
    {
        if (fds[i] >= 0)
            CloseSocket(fds[i]);
    }
}

/* ---- 4. argument validation ------------------------------------------- */

static VOID probe_args(VOID)
{
    LONG rc;

    Printf((STRPTR)"=== 4. argument validation ===\n");

    rc = socket(AF_INET, SOCK_RAW, 1);          p("socket(SOCK_RAW,ICMP)", rc);
    if (rc >= 0)
        CloseSocket(rc);
    rc = socket(-1, SOCK_STREAM, 0);            p("socket(-1,SOCK_STREAM)", rc);
    rc = socket(AF_INET, 999, 0);               p("socket(AF_INET,999)", rc);
    rc = CloseSocket(99);                       p("CloseSocket(99)", rc);
    rc = getdtablesize();                       p("getdtablesize()", rc);
}

/* ---- 5. edges the bsdsocket.doc names outright ------------------------- */

static VOID probe_doc_edges(VOID)
{
    struct sockaddr_in sa;
    struct timeval     tv;
    socklen_t          sl;
    LONG               fd, rc, val;

    Printf((STRPTR)"=== 5. documented edge cases ===\n");

    /* "the number of microseconds must be smaller than 1000000 and the number
       of seconds must not be larger than 100000000" -> EINVAL (22). */
    tv.tv_secs = 0; tv.tv_micro = 1000000;
    rc = WaitSelect(0, NULL, NULL, NULL, &tv, NULL);
    p("WaitSelect(tv_micro=1000000) [EINVAL 22]", rc);

    tv.tv_secs = 100000001UL; tv.tv_micro = 0;
    rc = WaitSelect(0, NULL, NULL, NULL, &tv, NULL);
    p("WaitSelect(tv_secs>1e8)      [EINVAL 22]", rc);

    fd = socket(AF_INET, SOCK_DGRAM, 0);        p("socket() udp", fd);

    /* "[ENOTTY] The specified request does not apply to the kind of object
       that the descriptor s references." */
    val = 0;
    rc = IoctlSocket(fd, 0x7fffffffUL, (APTR)&val);
    p("IoctlSocket(bogus request)   [ENOTTY 25]", rc);

    /* "Datagram sockets may dissolve the association by connecting to an
       invalid address, such as a null address." */
    addr_in(&sa, INADDR_LOOPBACK, PORT + 2);
    rc = connect(fd, (struct sockaddr *)&sa, sizeof(sa));
    p("connect(udp peer)", rc);
    sl = sizeof(sa);
    rc = getpeername(fd, (struct sockaddr *)&sa, &sl);
    p("getpeername(connected udp)   [0]", rc);

    memset(&sa, 0, sizeof(sa));
    rc = connect(fd, (struct sockaddr *)&sa, sizeof(sa));
    p("connect(udp null address)    [0]", rc);
    sl = sizeof(sa);
    rc = getpeername(fd, (struct sockaddr *)&sa, &sl);
    p("getpeername(dissolved)       [ENOTCONN 57]", rc);

    CloseSocket(fd);

    /* "[EBADF] No socket with the given Id could be found." */
    rc = ObtainSocket(0x4d4f4e45L, AF_INET, SOCK_STREAM, 0);
    p("ObtainSocket(unknown id)     [EBADF 9]", rc);

    /* "[EBADF] The socket descriptor number is not valid." */
    rc = ReleaseSocket(99, UNIQUE_ID);
    p("ReleaseSocket(99)            [EBADF 9]", rc);

    /* The doc says shutdown() on an unconnected socket is [ENOTCONN]; this
       library returns 0. Recorded, not asserted -- see the socket audit. */
    fd = socket(AF_INET, SOCK_STREAM, 0);
    rc = shutdown(fd, 2);
    p("shutdown(unconnected)        [doc: ENOTCONN 57]", rc);
    CloseSocket(fd);
}

/* ---- 6. the socket-layer audit fixes ----------------------------------- */

static VOID probe_audit_fixes(VOID)
{
    struct sockaddr_in sa;
    struct timeval     tv;
    socklen_t          sl;
    LONG               fd, lst, rc, mask;
    UBYTE             *big;

    Printf((STRPTR)"=== 6. socket-layer audit fixes ===\n");

    /* socket()'s ERRORS list has no ESOCKTNOSUPPORT; the only "not supported"
       code in it is "[EPROTONOSUPPORT] The protocol type or the specified
       protocol is not supported within this domain." */
    rc = socket(AF_INET, SOCK_SEQPACKET, 0);
    p("socket(SOCK_SEQPACKET)       [EPROTONOSUPPORT 43]", rc);
    if (rc >= 0)
        CloseSocket(rc);

    rc = socket(AF_INET, SOCK_RDM, 0);
    p("socket(SOCK_RDM)             [EPROTONOSUPPORT 43]", rc);
    if (rc >= 0)
        CloseSocket(rc);

    /* "[EOPNOTSUPP] The referenced socket is not of type SOCK_STREAM." */
    fd = socket(AF_INET, SOCK_DGRAM, 0);
    rc = accept(fd, NULL, NULL);
    p("accept(udp socket)           [EOPNOTSUPP 45]", rc);
    CloseSocket(fd);

    /* A stream socket that never listened is the other failure, and BSD keeps
       it separate: EINVAL, not EOPNOTSUPP. */
    fd = socket(AF_INET, SOCK_STREAM, 0);
    rc = accept(fd, NULL, NULL);
    p("accept(tcp, never listened)  [EINVAL 22]", rc);
    CloseSocket(fd);

    /* "[EFAULT] The addr parameter is not in a writable part of the user
       address space." addrlen says how much room addr has. */
    lst = socket(AF_INET, SOCK_STREAM, 0);
    addr_in(&sa, INADDR_LOOPBACK, PORT + 4);
    rc = bind(lst, (struct sockaddr *)&sa, sizeof(sa));
    p("bind(127.0.0.1:7704)", rc);
    rc = listen(lst, 1);
    p("listen(1)", rc);
    rc = accept(lst, (struct sockaddr *)&sa, NULL);
    p("accept(addr, addrlen=NULL)   [EFAULT 14]", rc);
    CloseSocket(lst);

    /* "The 'nfds' parameter may be truncated if it covers more sockets than
       are currently in use." -- clamped, not EINVAL. */
    tv.tv_secs = 0; tv.tv_micro = 0;
    rc = WaitSelect(getdtablesize() + 64, NULL, NULL, NULL, &tv, NULL);
    p("WaitSelect(nfds > table)     [0, truncated]", rc);

    /*
     * "If the message is too long to pass atomically through the underlying
     * protocol, the error EMSGSIZE is returned, and the message is not
     * transmitted."  The ceiling is per interface: NetX Duo's loopback carries
     * a 65535-byte MTU, so 65507 is the largest datagram it takes and 65508 is
     * the first refused.  An Ethernet interface at MTU 1500 refuses from 1473.
     */
    big = (UBYTE *)AllocMem(65536UL + 16UL, MEMF_ANY | MEMF_CLEAR);
    if (big != NULL)
    {
        fd = socket(AF_INET, SOCK_DGRAM, 0);
        addr_in(&sa, INADDR_LOOPBACK, PORT + 5);

        rc = sendto(fd, big, 65507, 0, (struct sockaddr *)&sa, sizeof(sa));
        p("sendto(65507 to loopback)    [not EMSGSIZE]", rc);

        rc = sendto(fd, big, 65508, 0, (struct sockaddr *)&sa, sizeof(sa));
        p("sendto(65508 to loopback)    [EMSGSIZE 40]", rc);

        /* The same boundary off-box, where the MTU is the Ethernet 1500 and
           the ceiling is therefore 1472. The destination only has to route:
           nothing has to answer. */
        {
            ULONG gateway = inet_addr((STRPTR)"10.0.2.2");

            if (gateway != INADDR_NONE)
            {
                memset(&sa, 0, sizeof(sa));
                sa.sin_family = AF_INET;
                sa.sin_port   = htons(PORT + 5);
                sa.sin_addr.s_addr = gateway;

                rc = sendto(fd, big, 1472, 0, (struct sockaddr *)&sa,
                            sizeof(sa));
                p("sendto(1472 off-box)         [not EMSGSIZE]", rc);

                rc = sendto(fd, big, 1473, 0, (struct sockaddr *)&sa,
                            sizeof(sa));
                p("sendto(1473 off-box)         [EMSGSIZE 40]", rc);
            }
        }

        CloseSocket(fd);
        FreeMem(big, 65536UL + 16UL);
    }

    /*
     * "When this event is found, the 'errno' variable will be set to the error
     * code associated with the socket which triggered the error event. The
     * error code associated with the socket is not cleared."
     *
     * A non-blocking connect to a port nothing listens on is refused on
     * loopback almost at once; the FD_ERROR event follows.
     */
    fd = socket(AF_INET, SOCK_STREAM, 0);
    mask = FD_CONNECT | FD_ERROR;
    rc = setsockopt(fd, SOL_SOCKET, SO_EVENTMASK, &mask, sizeof(mask));
    p("setsockopt(SO_EVENTMASK)", rc);

    rc = IoctlSocket(fd, FIONBIO, (APTR)&mask);   /* mask != 0: non-blocking */
    p("IoctlSocket(FIONBIO)", rc);

    addr_in(&sa, INADDR_LOOPBACK, PORT + 6);
    rc = connect(fd, (struct sockaddr *)&sa, sizeof(sa));
    p("connect(dead port, non-block)[refused on loopback]", rc);

    {
        LONG  tries;
        ULONG events = 0;

        for (tries = 0; tries < 50; tries++)
        {
            tv.tv_secs = 0; tv.tv_micro = 100000;
            (VOID)WaitSelect(0, NULL, NULL, NULL, &tv, NULL);

            bsd_errno = 0;
            rc = GetSocketEvents(&events);
            if (rc >= 0)
                break;
        }

        Printf((STRPTR)"%-44s rc=%-6ld errno=%ld events=0x%lx\n",
               (LONG)"GetSocketEvents(FD_ERROR)    [errno ECONNREFUSED 61]",
               rc, bsd_errno, events);

        /* The second half of the contract: this read did not clear it. */
        sl = sizeof(rc);
        rc = 0;
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &rc, &sl) == 0)
            Printf((STRPTR)"%-44s SO_ERROR=%ld\n",
                   (LONG)"getsockopt(SO_ERROR) after it", rc);
    }

    CloseSocket(fd);
}

int main(VOID)
{
    SocketBase = OpenLibrary((STRPTR)"bsdsocket.library", 4);
    if (SocketBase == NULL)
    {
        Printf((STRPTR)"probe: no bsdsocket.library v4\n");
        return RETURN_FAIL;
    }

    SocketBaseTags(SBTM_SETVAL(SBTC_ERRNOLONGPTR),  (ULONG)&bsd_errno,
                   SBTM_SETVAL(SBTC_HERRNOLONGPTR), (ULONG)&bsd_h_errno,
                   TAG_DONE);

    probe_loopback();
    probe_udp();
    probe_churn();
    probe_args();
    probe_doc_edges();
    probe_audit_fixes();

    CloseLibrary(SocketBase);
    return RETURN_OK;
}

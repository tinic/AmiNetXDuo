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
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include <sys/socket.h>
#include <sys/filio.h>
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

    CloseLibrary(SocketBase);
    return RETURN_OK;
}

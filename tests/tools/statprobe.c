/*
 * StatProbe -- does GetNetworkStatistics() report the stack that is running?
 *
 * The call hands back 4.4BSD's own kernel statistics structures by value, so
 * there are three separate things a build cannot check and this can:
 *
 *   1. The return value is A BYTE COUNT. Not zero-on-success and not an entry
 *      count -- the two other readings the prototype allows. A NULL
 *      destination asks how much would be needed and copies nothing.
 *
 *   2. THE NUMBERS ARE THE RUNNING STACK'S. This machine got its address by
 *      DHCP over UDP, so udps_ipackets and ips_total cannot be zero. A stub
 *      returning a zeroed struct of the right size passes every structural
 *      check and fails these.
 *
 *   3. pcd_tcp_state IS 4.4BSD's ENUMERATION, NOT NetX Duo's, and A BSD
 *      listen() Is one entry. Both halves are asserted by the same
 *      experiment. The enumerations agree up to CLOSE_WAIT and diverge after
 *      it. And this stack implements one listening descriptor as TWO
 *      NX_TCP_SOCKETs -- the descriptor's own, left in CLOSED, and a spare
 *      parked on the port in SYN_RECEIVED (src/bsdsocket/socket.c) -- so a
 *      literal report would show one listen() as two sockets, on one port,
 *      neither of them listening and one of them apparently mid-handshake
 *      with nobody. The probe listens on a port and asserts that exactly one
 *      entry appears and that its state is TCPS_LISTEN.
 *
 * Vectors are called by hand at their LVOs, the same way routeprobe.c and
 * ifprobe.c do: the NDK inlines assume a global SocketBase.
 *
 * SPDX-License-Identifier: MIT
 */

#include <exec/types.h>
#include <dos/dos.h>

/* <libraries/bsdsocket.h> pulls in <sys/socket.h>, which uses size_t and
   ssize_t without declaring them. Same ordering note as ifprobe.c. */
#include <stddef.h>
#include <sys/types.h>
#include <libraries/bsdsocket.h>
#include <netinet/ip_var.h>
#include <netinet/tcp_var.h>
#include <netinet/tcp_fsm.h>
#include <netinet/udp_var.h>

#include <proto/exec.h>
#include <proto/dos.h>

/* ------------------------------------------------------------- vectors ---- */

static LONG p_stats(struct Library *base, LONG type, LONG version,
                    APTR destination, LONG size)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = type;
    register LONG            d1  __asm("d1") = version;
    register APTR            a0  __asm("a0") = destination;
    register LONG            d2  __asm("d2") = size;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");

    __asm __volatile ("jsr a6@(-510:W)"     /* GetNetworkStatistics -0x1fe */
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0)
                      : "r" (a6), "r" (d0), "r" (d1), "r" (a0), "r" (d2)
                      : "a1", "cc", "memory");
    return res;
}

static LONG p_socket(struct Library *base, LONG domain, LONG type, LONG proto)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = domain;
    register LONG            d1  __asm("d1") = type;
    register LONG            d2  __asm("d2") = proto;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");

    __asm __volatile ("jsr a6@(-30:W)"      /* socket */
                      : "=r" (res), "=r" (_clob_d1)
                      : "r" (a6), "r" (d0), "r" (d1), "r" (d2)
                      : "a0", "a1", "cc", "memory");
    return res;
}

typedef struct ProbeAddr
{
    UBYTE   sin_len;
    UBYTE   sin_family;
    UWORD   sin_port;
    ULONG   sin_addr;
    UBYTE   sin_zero[8];
} ProbeAddr;

static LONG p_bind(struct Library *base, LONG s, const ProbeAddr *sa)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = s;
    register CONST_APTR      a0  __asm("a0") = (CONST_APTR)sa;
    register LONG            d1  __asm("d1") = (LONG)sizeof(*sa);
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");

    __asm __volatile ("jsr a6@(-36:W)"      /* bind */
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0)
                      : "r" (a6), "r" (d0), "r" (a0), "r" (d1)
                      : "a1", "cc", "memory");
    return res;
}

static LONG p_listen(struct Library *base, LONG s, LONG backlog)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = s;
    register LONG            d1  __asm("d1") = backlog;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");

    __asm __volatile ("jsr a6@(-42:W)"      /* listen */
                      : "=r" (res), "=r" (_clob_d1)
                      : "r" (a6), "r" (d0), "r" (d1)
                      : "a0", "a1", "cc", "memory");
    return res;
}

static LONG p_close(struct Library *base, LONG s)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = s;
    register LONG            res __asm("d0");

    __asm __volatile ("jsr a6@(-120:W)"     /* CloseSocket */
                      : "=r" (res)
                      : "r" (a6), "r" (d0)
                      : "d1", "a0", "a1", "cc", "memory");
    return res;
}

static LONG p_errno(struct Library *base)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            res __asm("d0");

    __asm __volatile ("jsr a6@(-162:W)"     /* Errno */
                      : "=r" (res)
                      : "r" (a6)
                      : "d1", "a0", "a1", "cc", "memory");
    return res;
}

#define P_AF_INET       2
#define P_SOCK_STREAM   1
#define PROBE_PORT      7777

/* ------------------------------------------------------------------ main -- */

/*
 * The whole of a struct plus a little, so a short read cannot be mistaken for
 * a full one and an overrun would be visible as a changed guard byte.
 */
#define GUARD_BYTE  0x5A

static UBYTE buffer[512];

static VOID p_fill_guard(VOID)
{
    ULONG i;

    for (i = 0; i < sizeof(buffer); i++)
        buffer[i] = GUARD_BYTE;
}

static BOOL p_guard_intact(ULONG from)
{
    ULONG i;

    for (i = from; i < sizeof(buffer); i++)
    {
        if (buffer[i] != GUARD_BYTE)
            return FALSE;
    }

    return TRUE;
}

int main(void)
{
    struct Library *base;
    ProbeAddr       sa;
    LONG            rc;
    LONG            listener;
    LONG            entry;
    LONG            udp_before;
    LONG            tcp_before;
    LONG            tcp_after;
    ULONG           i;
    ULONG           n;

    base = OpenLibrary((CONST_STRPTR)"bsdsocket.library", 4);
    if (base == NULL)
    {
        Printf((CONST_STRPTR)"StatProbe: no bsdsocket.library\n");
        return RETURN_FAIL;
    }

    entry = (LONG)sizeof(struct protocol_connection_data);
    Printf((CONST_STRPTR)"sizes: ipstat %ld tcpstat %ld udpstat %ld pcd %ld\n",
           (LONG)sizeof(struct ipstat), (LONG)sizeof(struct tcpstat),
           (LONG)sizeof(struct udpstat), entry);

    /* ---- the version is mandatory ---------------------------------------- */
    rc = p_stats(base, NETSTATUS_ip, 0, buffer, (LONG)sizeof(buffer));
    Printf((CONST_STRPTR)"version 0: rc %ld (errno %ld)%s\n",
           rc, p_errno(base),
           (LONG)((rc < 0) ? " -- refused, correctly" : " -- ACCEPTED, WRONG"));

    /* ---- a type the API never defined ------------------------------------ */
    rc = p_stats(base, 99, NETWORKSTATUS_VERSION, buffer, (LONG)sizeof(buffer));
    Printf((CONST_STRPTR)"type 99: rc %ld (errno %ld)%s\n",
           rc, p_errno(base),
           (LONG)((rc < 0) ? " -- refused, correctly" : " -- ACCEPTED, WRONG"));

    /* ---- a documented type this stack keeps nothing for ------------------- */
    rc = p_stats(base, NETSTATUS_mb, NETWORKSTATUS_VERSION, buffer,
                 (LONG)sizeof(buffer));
    Printf((CONST_STRPTR)"NETSTATUS_mb: rc %ld (errno %ld)%s\n",
           rc, p_errno(base),
           (LONG)((rc < 0) ? " -- refused, correctly" : " -- ACCEPTED, WRONG"));

    /* ---- NULL asks the size ---------------------------------------------- */
    rc = p_stats(base, NETSTATUS_ip, NETWORKSTATUS_VERSION, NULL, 0);
    Printf((CONST_STRPTR)"NETSTATUS_ip size: %ld%s\n", rc,
           (LONG)((rc == (LONG)sizeof(struct ipstat))
                      ? " -- sizeof(struct ipstat), correctly" : " -- WRONG"));

    /* ---- and the numbers themselves --------------------------------------
     *
     * This machine leased its address by DHCP, which is UDP over IP, so
     * neither of these can be zero on a stack that is really counting.
     */
    p_fill_guard();
    rc = p_stats(base, NETSTATUS_ip, NETWORKSTATUS_VERSION, buffer,
                 (LONG)sizeof(buffer));
    {
        const struct ipstat *s = (const struct ipstat *)buffer;

        Printf((CONST_STRPTR)"NETSTATUS_ip: rc %ld total %ld localout %ld "
                             "badsum %ld guard %s\n",
               rc, (LONG)s->ips_total, (LONG)s->ips_localout,
               (LONG)s->ips_badsum,
               (LONG)(p_guard_intact(sizeof(struct ipstat)) ? "intact"
                                                            : "OVERRUN"));
    }

    p_fill_guard();
    rc = p_stats(base, NETSTATUS_udp, NETWORKSTATUS_VERSION, buffer,
                 (LONG)sizeof(buffer));
    {
        const struct udpstat *s = (const struct udpstat *)buffer;

        Printf((CONST_STRPTR)"NETSTATUS_udp: rc %ld ipackets %ld opackets %ld "
                             "guard %s\n",
               rc, (LONG)s->udps_ipackets, (LONG)s->udps_opackets,
               (LONG)(p_guard_intact(sizeof(struct udpstat)) ? "intact"
                                                             : "OVERRUN"));
    }

    p_fill_guard();
    rc = p_stats(base, NETSTATUS_tcp, NETWORKSTATUS_VERSION, buffer,
                 (LONG)sizeof(buffer));
    Printf((CONST_STRPTR)"NETSTATUS_tcp: rc %ld guard %s\n", rc,
           (LONG)(p_guard_intact(sizeof(struct tcpstat)) ? "intact"
                                                         : "OVERRUN"));

    p_fill_guard();
    rc = p_stats(base, NETSTATUS_icmp, NETWORKSTATUS_VERSION, buffer,
                 (LONG)sizeof(buffer));
    Printf((CONST_STRPTR)"NETSTATUS_icmp: rc %ld guard %s\n", rc,
           (LONG)(p_guard_intact((ULONG)rc) ? "intact" : "OVERRUN"));

    /* ---- a caller that asks for less than the whole struct ----------------
     *
     * "size -- Number of bytes to copy" is the caller's limit. Copying the
     * whole struct regardless would overrun a buffer sized against an older
     * layout, which is the one way this call can corrupt an application.
     */
    p_fill_guard();
    rc = p_stats(base, NETSTATUS_ip, NETWORKSTATUS_VERSION, buffer, 8);
    Printf((CONST_STRPTR)"NETSTATUS_ip into 8 bytes: rc %ld guard %s\n", rc,
           (LONG)(p_guard_intact(8) ? "intact" : "OVERRUN"));

    /* ---- the per-connection tables ---------------------------------------- */
    udp_before = p_stats(base, NETSTATUS_udp_sockets, NETWORKSTATUS_VERSION,
                         NULL, 0);
    tcp_before = p_stats(base, NETSTATUS_tcp_sockets, NETWORKSTATUS_VERSION,
                         NULL, 0);
    Printf((CONST_STRPTR)"sockets before: udp %ld bytes, tcp %ld bytes\n",
           udp_before, tcp_before);

    listener = p_socket(base, P_AF_INET, P_SOCK_STREAM, 0);
    if (listener < 0)
    {
        Printf((CONST_STRPTR)"no socket (errno %ld)\n", p_errno(base));
        CloseLibrary(base);
        return RETURN_FAIL;
    }

    for (i = 0; i < sizeof(sa); i++)
        ((UBYTE *)&sa)[i] = 0;

    sa.sin_len    = (UBYTE)sizeof(sa);
    sa.sin_family = P_AF_INET;
    sa.sin_port   = PROBE_PORT;

    rc = p_bind(base, listener, &sa);
    Printf((CONST_STRPTR)"bind to port %ld: rc %ld (errno %ld)\n",
           (LONG)PROBE_PORT, rc, p_errno(base));

    rc = p_listen(base, listener, 1);
    Printf((CONST_STRPTR)"listen: rc %ld (errno %ld)\n", rc, p_errno(base));

    tcp_after = p_stats(base, NETSTATUS_tcp_sockets, NETWORKSTATUS_VERSION,
                        NULL, 0);
    Printf((CONST_STRPTR)"tcp sockets after listen: %ld bytes (%ld more)%s\n",
           tcp_after, tcp_after - tcp_before,
           (LONG)((tcp_after == tcp_before + entry)
                      ? " -- one more connection, correctly" : " -- WRONG"));

    p_fill_guard();
    rc = p_stats(base, NETSTATUS_tcp_sockets, NETWORKSTATUS_VERSION, buffer,
                 (LONG)sizeof(buffer));

    n = (rc > 0) ? ((ULONG)rc / (ULONG)entry) : 0;
    Printf((CONST_STRPTR)"tcp table: rc %ld, %ld entries, guard %s\n",
           rc, (LONG)n, (LONG)(p_guard_intact((ULONG)rc) ? "intact"
                                                         : "OVERRUN"));

    for (i = 0; i < n; i++)
    {
        const struct protocol_connection_data *pcd =
            &((const struct protocol_connection_data *)buffer)[i];

        Printf((CONST_STRPTR)"  tcp local %ld foreign %ld state %ld "
                             "rq %ld sq %ld\n",
               (LONG)pcd->pcd_local_port, (LONG)pcd->pcd_foreign_port,
               pcd->pcd_tcp_state, (LONG)pcd->pcd_receive_queue_size,
               (LONG)pcd->pcd_send_queue_size);

        if (pcd->pcd_local_port == PROBE_PORT)
            Printf((CONST_STRPTR)"listener state: %ld%s\n", pcd->pcd_tcp_state,
                   (LONG)((pcd->pcd_tcp_state == TCPS_LISTEN)
                              ? " -- TCPS_LISTEN, correctly"
                              : " -- NOT THE BSD NUMBER, WRONG"));
    }

    /*
     * A buffer that holds exactly one entry, while there is at least one:
     * "size" is a limit and the call must fill what fits and report only
     * that, rather than writing the whole table.
     */
    if (n > 0)
    {
        p_fill_guard();
        rc = p_stats(base, NETSTATUS_tcp_sockets, NETWORKSTATUS_VERSION,
                     buffer, entry);
        Printf((CONST_STRPTR)"tcp table into one entry: rc %ld guard %s%s\n",
               rc, (LONG)(p_guard_intact((ULONG)entry) ? "intact" : "OVERRUN"),
               (LONG)((rc == entry) ? " -- one entry, correctly" : " -- WRONG"));
    }

    (VOID)p_close(base, listener);

    CloseLibrary(base);

    return RETURN_OK;
}

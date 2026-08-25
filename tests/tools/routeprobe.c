/*
 * RouteProbe, checks that NX_ENABLE_IP_STATIC_ROUTING reaches the wire.
 *
 * SPDX-License-Identifier: MIT
 */

#include <exec/types.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include "aminetxduo/netstatus.h"

/* The experiment, in one place so the shell script can quote it. */
#define PROBE_DEST      0xC0A84D05UL    /* 192.168.77.5 , nothing routes here */
#define PROBE_NETWORK   0xC0A84D00UL    /* 192.168.77.0                         */
#define PROBE_MASK      0xFFFFFF00UL    /* /24                                  */
#define PROBE_NEXTHOP_HOST      249
#define PROBE_ABSENT    0xC0A85800UL    /* 192.168.88.0, never added          */
#define PROBE_PORT      9999

typedef struct ProbeAddr
{
    UBYTE   sin_len;
    UBYTE   sin_family;
    UWORD   sin_port;
    ULONG   sin_addr;
    UBYTE   sin_zero[8];
} ProbeAddr;

#define P_AF_INET       2
#define P_SOCK_DGRAM    2

static LONG p_socket(struct Library *base, LONG domain, LONG type, LONG proto)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = domain;
    register LONG            d1  __asm("d1") = type;
    register LONG            d2  __asm("d2") = proto;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");

    __asm __volatile ("jsr a6@(-30:W)"
                      : "=r" (res), "=r" (_clob_d1)
                      : "r" (a6), "r" (d0), "r" (d1), "r" (d2)
                      : "a0", "a1", "cc", "memory");
    return res;
}

static LONG p_sendto(struct Library *base, LONG s, const void *buf, LONG len,
                     const ProbeAddr *to)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = s;
    register CONST_APTR      a0  __asm("a0") = (CONST_APTR)buf;
    register LONG            d1  __asm("d1") = len;
    register LONG            d2  __asm("d2") = 0;
    register CONST_APTR      a1  __asm("a1") = (CONST_APTR)to;
    register LONG            d3  __asm("d3") = (LONG)sizeof(*to);
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");
    register LONG _clob_a1 __asm("a1");

    __asm __volatile ("jsr a6@(-60:W)"
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0), "=r" (_clob_a1)
                      : "r" (a6), "r" (d0), "r" (a0), "r" (d1), "r" (d2),
                        "r" (a1), "r" (d3)
                      : "cc", "memory");
    return res;
}

static LONG p_close(struct Library *base, LONG s)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = s;
    register LONG            res __asm("d0");

    __asm __volatile ("jsr a6@(-120:W)"
                      : "=r" (res)
                      : "r" (a6), "r" (d0)
                      : "d1", "a0", "a1", "cc", "memory");
    return res;
}

static LONG p_errno(struct Library *base)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            res __asm("d0");

    __asm __volatile ("jsr a6@(-162:W)"
                      : "=r" (res)
                      : "r" (a6)
                      : "d1", "a0", "a1", "cc", "memory");
    return res;
}

static LONG p_query(struct Library *base, ULONG what, APTR buffer, ULONG size)
{
    register struct Library *a6  __asm("a6") = base;
    register ULONG           d0  __asm("d0") = AMI_NETSTATUS_MAGIC;
    register ULONG           d1  __asm("d1") = what;
    register APTR            a0  __asm("a0") = buffer;
    register ULONG           d2  __asm("d2") = size;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");

    __asm __volatile ("jsr a6@(-870:W)"     /* AMI_NETSTATUS_QUERY_LVO   */
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0)
                      : "r" (a6), "r" (d0), "r" (d1), "r" (a0), "r" (d2)
                      : "a1", "cc", "memory");
    return res;
}

static LONG p_control(struct Library *base, ULONG op, NetStatusControl *ctl)
{
    register struct Library *a6  __asm("a6") = base;
    register ULONG           d0  __asm("d0") = AMI_NETSTATUS_MAGIC;
    register ULONG           d1  __asm("d1") = op;
    register APTR            a0  __asm("a0") = (APTR)ctl;
    register ULONG           d2  __asm("d2") = (ULONG)sizeof(*ctl);
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");

    __asm __volatile ("jsr a6@(-876:W)"     /* AMI_NETSTATUS_CONTROL_LVO */
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0)
                      : "r" (a6), "r" (d0), "r" (d1), "r" (a0), "r" (d2)
                      : "a1", "cc", "memory");
    return res;
}

_Static_assert(AMI_NETSTATUS_QUERY_LVO   == -870, "NetStackQuery LVO moved");
_Static_assert(AMI_NETSTATUS_CONTROL_LVO == -876, "NetStackControl LVO moved");

static struct
{
    NetStatusHeader hdr;
    NetStatusRoute  e[8];
} probe_routes;

static VOID probe_ctl_init(NetStatusControl *ctl);

static VOID probe_zero(APTR p, ULONG n)
{
    UBYTE *b = (UBYTE *)p;

    while (n-- != 0)
        *b++ = 0;
}

static VOID probe_ctl_init(NetStatusControl *ctl)
{
    probe_zero(ctl, sizeof(*ctl));
    ctl->nsc_Magic   = AMI_NETSTATUS_MAGIC;
    ctl->nsc_Version = (UWORD)AMI_NETSTATUS_VERSION;
}

static VOID probe_dotted(ULONG addr, char *out)
{
    static const char digits[] = "0123456789";
    LONG  shift;
    ULONG pos = 0;

    for (shift = 24; shift >= 0; shift -= 8)
    {
        ULONG v = (addr >> shift) & 0xFFUL;

        if (v >= 100)
            out[pos++] = digits[v / 100];
        if (v >= 10)
            out[pos++] = digits[(v / 10) % 10];
        out[pos++] = digits[v % 10];

        if (shift != 0)
            out[pos++] = '.';
    }
    out[pos] = '\0';
}

/* Every route the stack has, with its flags. */
static VOID show_routes(struct Library *base, const char *when)
{
    LONG n;
    LONG i;

    probe_zero(&probe_routes, sizeof(probe_routes));
    probe_routes.hdr.nsh_Magic   = AMI_NETSTATUS_MAGIC;
    probe_routes.hdr.nsh_Version = AMI_NETSTATUS_VERSION;

    n = p_query(base, NETSTATUS_ROUTES, &probe_routes, sizeof(probe_routes));
    if (n < 0)
    {
        Printf((CONST_STRPTR)"routes %s: the query failed\n", (LONG)when);
        return;
    }

    Printf((CONST_STRPTR)"routes %s: %ld\n", (LONG)when, n);

    for (i = 0; i < n; i++)
    {
        const NetStatusRoute *r = &probe_routes.e[i];
        char                  dest[16];
        char                  gw[16];
        char                  mask[16];
        char                  flags[6];
        UWORD                 f = 0;

        probe_dotted(r->nsr_Destination, dest);
        probe_dotted(r->nsr_Gateway, gw);
        probe_dotted(r->nsr_NetMask, mask);

        if (r->nsr_Flags & NETSTATUS_RT_UP)      flags[f++] = 'U';
        if (r->nsr_Flags & NETSTATUS_RT_GATEWAY) flags[f++] = 'G';
        if (r->nsr_Flags & NETSTATUS_RT_HOST)    flags[f++] = 'H';
        if (r->nsr_Flags & NETSTATUS_RT_STATIC)  flags[f++] = 'S';
        flags[f] = '\0';

        Printf((CONST_STRPTR)"  %-16s %-16s %-16s %-5s if %ld\n",
               (LONG)dest, (LONG)gw, (LONG)mask, (LONG)flags,
               (LONG)r->nsr_Interface);
    }
}

static ULONG local_network(struct Library *base)
{
    LONG n;
    LONG i;

    probe_zero(&probe_routes, sizeof(probe_routes));
    probe_routes.hdr.nsh_Magic   = AMI_NETSTATUS_MAGIC;
    probe_routes.hdr.nsh_Version = AMI_NETSTATUS_VERSION;

    n = p_query(base, NETSTATUS_ROUTES, &probe_routes, sizeof(probe_routes));

    for (i = 0; i < n; i++)
    {
        const NetStatusRoute *r = &probe_routes.e[i];

        if (r->nsr_Gateway != 0)
            continue;
        if (r->nsr_NetMask == 0 || r->nsr_NetMask == 0xFFFFFFFFUL)
            continue;
        if (r->nsr_Destination == 0)
            continue;
        if ((r->nsr_Destination & 0xFF000000UL) == 0x7F000000UL)
            continue;

        return r->nsr_Destination & r->nsr_NetMask;
    }

    return 0;
}

static VOID send_one(struct Library *base)
{
    static const UBYTE payload[8] = { 'r', 'o', 'u', 't', 'e', 'd', '!', '\n' };
    ProbeAddr sa;
    LONG      s;
    LONG      sent;

    probe_zero(&sa, sizeof(sa));
    sa.sin_len    = (UBYTE)sizeof(sa);
    sa.sin_family = P_AF_INET;
    sa.sin_port   = PROBE_PORT;
    sa.sin_addr   = PROBE_DEST;

    s = p_socket(base, P_AF_INET, P_SOCK_DGRAM, 0);
    if (s < 0)
    {
        Printf((CONST_STRPTR)"send: no socket (errno %ld)\n", p_errno(base));
        return;
    }

    sent = p_sendto(base, s, payload, (LONG)sizeof(payload), &sa);
    Printf((CONST_STRPTR)"send to 192.168.77.5: %ld (errno %ld)\n", sent, p_errno(base));

    Delay(150);

    (VOID)p_close(base, s);
}

int main(void)
{
    struct Library  *base;
    NetStatusControl ctl;
    struct
    {
        NetStatusHeader hdr;
        NetStatusSystem e;
    } sys;
    LONG  rc;
    ULONG next_hop;
    char  hop_text[16];

    base = OpenLibrary((CONST_STRPTR)"bsdsocket.library", 4);
    if (base == NULL)
    {
        Printf((CONST_STRPTR)"RouteProbe: no bsdsocket.library\n");
        return RETURN_FAIL;
    }

    probe_zero(&sys, sizeof(sys));
    sys.hdr.nsh_Magic   = AMI_NETSTATUS_MAGIC;
    sys.hdr.nsh_Version = AMI_NETSTATUS_VERSION;

    if (p_query(base, NETSTATUS_SYSTEM, &sys, sizeof(sys)) > 0)
    {
        Printf((CONST_STRPTR)"static routing: %s\n",
               (LONG)((sys.e.nss_Flags & NETSTATUS_SYS_ROUTING)
                          ? "compiled in" : "NOT COMPILED IN"));
    }
    else
    {
        Printf((CONST_STRPTR)"static routing: the system query failed\n");
    }

    show_routes(base, "before");

    next_hop = local_network(base);
    if (next_hop == 0)
    {
        Printf((CONST_STRPTR)"RouteProbe: no interface route, is the interface up?\n");
        CloseLibrary(base);
        return RETURN_FAIL;
    }
    next_hop |= PROBE_NEXTHOP_HOST;
    probe_dotted(next_hop, hop_text);
    Printf((CONST_STRPTR)"route next hop: %s\n", (LONG)hop_text);

    probe_ctl_init(&ctl);
    ctl.nsc_Destination = PROBE_NETWORK;
    ctl.nsc_NetMask     = PROBE_MASK;
    ctl.nsc_Gateway     = next_hop;

    rc = p_control(base, NETCTRL_ROUTE_ADD, &ctl);
    Printf((CONST_STRPTR)"add 192.168.77.0/24 via %s: %ld (errno %ld)\n",
           (LONG)hop_text, rc, p_errno(base));

    show_routes(base, "with");

    probe_ctl_init(&ctl);
    ctl.nsc_Destination = 0xAC100000UL;         /* 172.16.0.0   */
    ctl.nsc_NetMask     = 0xFFFF0000UL;         /* /16          */
    ctl.nsc_Gateway     = 0x08080808UL;         /* 8.8.8.8      */

    rc = p_control(base, NETCTRL_ROUTE_ADD, &ctl);
    Printf((CONST_STRPTR)"add 172.16.0.0/16 via 8.8.8.8: %ld (errno %ld)%s\n",
           rc, p_errno(base),
           (LONG)((rc != 0) ? ", refused, correctly" : ", ACCEPTED, WRONG"));

    probe_ctl_init(&ctl);
    ctl.nsc_Destination = PROBE_ABSENT;
    ctl.nsc_NetMask     = PROBE_MASK;

    rc = p_control(base, NETCTRL_ROUTE_DELETE, &ctl);
    Printf((CONST_STRPTR)"delete 192.168.88.0/24 (never added): %ld (errno %ld)%s\n",
           rc, p_errno(base),
           (LONG)((rc != 0) ? ", refused, correctly" : ", ACCEPTED, WRONG"));

    send_one(base);

    probe_ctl_init(&ctl);
    ctl.nsc_Destination = PROBE_NETWORK;
    ctl.nsc_NetMask     = PROBE_MASK;

    rc = p_control(base, NETCTRL_ROUTE_DELETE, &ctl);
    Printf((CONST_STRPTR)"delete 192.168.77.0/24: %ld (errno %ld)\n",
           rc, p_errno(base));

    show_routes(base, "after");

    CloseLibrary(base);

    return RETURN_OK;
}

/*
 * mcastprobe, exercises group membership the way an SSDP client does.
 *
 * Both families, one pass each.  The order matters and is the point: an SSDP
 * receiver binds the group, joins it, and only then reads.  Each step is
 * checked and reported, so a failure says which one broke rather than
 * "nothing arrived".
 *
 *   1. multicast TTL / hops, LOOP and IF set and read back
 *   2. bind to the group, refused by every build before 0.15.2
 *   3. join, then again (must be EADDRINUSE)
 *   4. one M-SEARCH to the group
 *   5. read for a few seconds, printing whatever answers
 *   6. leave, then again (must be EADDRNOTAVAIL)
 *
 * IPv4 uses 239.255.255.250:1900, IPv6 the link-local ff02::c:1900.  The v6
 * pass is skipped, not failed, on a build without IPv6: socket(AF_INET6)
 * fails and there is nothing to test.
 *
 * IPV6_MULTICAST_LOOP reads back 0 whatever is set, on purpose, this stack
 * has no IPv6 multicast loopback, and the option says so rather than storing
 * a value it will not honour.
 *
 * Step 5 needs something on the LAN that answers SSDP, a router, a printer,
 * a TV.  Steps 1-4 and 6 do not, and are the ones that test this stack.
 *
 * The wire side used to be read out of FS-UAE's A2065 frame dump through
 * tests/trace/a2065pcap.py, and there is no such dump under Amiberry.  What is
 * left is a bridged run watched from another machine, or tools/winuae-run.sh
 * with -a2065log2 and a2065pcap.py --winuae.  What to look for either way is
 * an IGMP v2 report for 239.255.255.250.
 *
 * On the v6 side the thing to look for is a Multicast Listener Report for
 * ff02::c, and a Done or a CHANGE_TO_INCLUDE_MODE record when the leave
 * happens.  tests/ipv6/run-mld.sh drives this program for exactly that and
 * reads the answer out of a capture on the segment.
 *
 * A one-off probe rather than a command or a test, so it has no CMake entry.
 * Compile it by hand and stage it like any other executable:
 *
 *   . tools/amiga-toolchain.sh
 *   "$AMIGA_GCC" -O2 -m68020 -I"$AMIGA_NDK" -o McastProbe \
 *       tests/tools/mcastprobe.c
 *
 * The vectors are called by hand at the LVOs docs/RESEARCH.md 3.2 lists, for
 * the same reason src/tools/toolsock.c does it: the NDK inlines assume a
 * global SocketBase.
 *
 * SPDX-License-Identifier: MIT
 */

#include <exec/types.h>
#include <devices/timer.h>      /* struct timeval, for WaitSelect */
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>

typedef struct ProbeAddr
{
    UBYTE   sin_len;
    UBYTE   sin_family;
    UWORD   sin_port;
    ULONG   sin_addr;
    UBYTE   sin_zero[8];
} ProbeAddr;

/*
 * struct sockaddr_in6 on this NDK: 28 bytes, family at offset 0 and NO length
 * byte.  See aminetxduo/in6.h, it is not a sockaddr_in with a wider address.
 */
typedef struct ProbeAddr6
{
    UBYTE   sin6_family;
    UBYTE   sin6_pad;
    UWORD   sin6_port;
    ULONG   sin6_flowinfo;
    UBYTE   sin6_addr[16];
    ULONG   sin6_scope_id;
} ProbeAddr6;

/* struct ip_mreq: two in_addr, group first. */
typedef struct ProbeMreq
{
    ULONG   imr_multiaddr;
    ULONG   imr_interface;
} ProbeMreq;

/* struct ipv6_mreq: the group, then an interface INDEX rather than address. */
typedef struct ProbeMreq6
{
    UBYTE   ipv6mr_multiaddr[16];
    ULONG   ipv6mr_interface;
} ProbeMreq6;

#define P_AF_INET           2
#define P_SOCK_DGRAM        2
#define P_IPPROTO_IP        0
#define P_IP_MULTICAST_IF   9
#define P_IP_MULTICAST_TTL  10
#define P_IP_MULTICAST_LOOP 11
#define P_IP_ADD_MEMBERSHIP 12
#define P_IP_DROP_MEMBERSHIP 13

#define P_AF_INET6          23
#define P_IPPROTO_IPV6      41
#define P_IPV6_MULTICAST_IF   9
#define P_IPV6_MULTICAST_HOPS 10
#define P_IPV6_MULTICAST_LOOP 11
#define P_IPV6_JOIN_GROUP     12
#define P_IPV6_LEAVE_GROUP    13

#define P_EADDRINUSE        48
#define P_EADDRNOTAVAIL     49

/* 239.255.255.250:1900, SSDP (RFC-less, but universally implemented). */
#define P_SSDP_GROUP    ((239UL << 24) | (255UL << 16) | (255UL << 8) | 250UL)
#define P_SSDP_PORT     1900

/* ff02::c, the link-local SSDP group.  Link-local scope is the one that
   works without MLD, which is why the probe uses it and not ff05::c. */
static const UBYTE p_ssdp6_group[16] =
{
    0xff, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x0c
};

/* ------------------------------------------------------------- vectors ---- */

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

static LONG p_bind(struct Library *base, LONG s, const void *name, LONG namelen)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = s;
    register CONST_APTR      a0  __asm("a0") = (CONST_APTR)name;
    register LONG            d1  __asm("d1") = namelen;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");

    __asm __volatile ("jsr a6@(-36:W)"
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0)
                      : "r" (a6), "r" (d0), "r" (a0), "r" (d1)
                      : "a1", "cc", "memory");
    return res;
}

static LONG p_sendto(struct Library *base, LONG s, const void *buf, LONG len,
                     const void *to, LONG tolen)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = s;
    register CONST_APTR      a0  __asm("a0") = (CONST_APTR)buf;
    register LONG            d1  __asm("d1") = len;
    register LONG            d2  __asm("d2") = 0;
    register CONST_APTR      a1  __asm("a1") = (CONST_APTR)to;
    register LONG            d3  __asm("d3") = tolen;
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

static LONG p_recvfrom(struct Library *base, LONG s, void *buf, LONG len,
                       void *from, LONG *fromlen)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = s;
    register APTR            a0  __asm("a0") = (APTR)buf;
    register LONG            d1  __asm("d1") = len;
    register LONG            d2  __asm("d2") = 0;
    register APTR            a1  __asm("a1") = (APTR)from;
    register APTR            a2  __asm("a2") = (APTR)fromlen;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");
    register LONG _clob_a1 __asm("a1");
    register LONG _clob_a2 __asm("a2");

    __asm __volatile ("jsr a6@(-72:W)"
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0),
                        "=r" (_clob_a1), "=r" (_clob_a2)
                      : "r" (a6), "r" (d0), "r" (a0), "r" (d1), "r" (d2),
                        "r" (a1), "r" (a2)
                      : "cc", "memory");
    return res;
}

static LONG p_setsockopt(struct Library *base, LONG s, LONG level, LONG name,
                         const void *val, LONG len)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = s;
    register LONG            d1  __asm("d1") = level;
    register LONG            d2  __asm("d2") = name;
    register CONST_APTR      a0  __asm("a0") = (CONST_APTR)val;
    register LONG            d3  __asm("d3") = len;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");

    __asm __volatile ("jsr a6@(-90:W)"
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0)
                      : "r" (a6), "r" (d0), "r" (d1), "r" (d2), "r" (a0),
                        "r" (d3)
                      : "a1", "cc", "memory");
    return res;
}

static LONG p_getsockopt(struct Library *base, LONG s, LONG level, LONG name,
                         void *val, LONG *len)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = s;
    register LONG            d1  __asm("d1") = level;
    register LONG            d2  __asm("d2") = name;
    register APTR            a0  __asm("a0") = (APTR)val;
    register APTR            a1  __asm("a1") = (APTR)len;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");
    register LONG _clob_a1 __asm("a1");

    __asm __volatile ("jsr a6@(-96:W)"
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0), "=r" (_clob_a1)
                      : "r" (a6), "r" (d0), "r" (d1), "r" (d2), "r" (a0),
                        "r" (a1)
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

/*
 * WaitSelect(nfds, read, write, except, timeout, signals), so the read in
 * step 5 gives up instead of blocking for ever on a LAN with no UPnP on it.
 */
static LONG p_waitselect(struct Library *base, LONG nfds, ULONG *readfds,
                         struct timeval *tv)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = nfds;
    register APTR            a0  __asm("a0") = (APTR)readfds;
    register APTR            a1  __asm("a1") = NULL;
    register APTR            a2  __asm("a2") = NULL;
    register APTR            a3  __asm("a3") = (APTR)tv;
    register APTR            d1  __asm("d1") = NULL;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");
    register LONG _clob_a1 __asm("a1");
    register LONG _clob_a2 __asm("a2");
    register LONG _clob_a3 __asm("a3");

    __asm __volatile ("jsr a6@(-126:W)"
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0),
                        "=r" (_clob_a1), "=r" (_clob_a2), "=r" (_clob_a3)
                      : "r" (a6), "r" (d0), "r" (a0), "r" (a1), "r" (a2),
                        "r" (a3), "r" (d1)
                      : "cc", "memory");
    return res;
}

/* --------------------------------------------------------------- probes --- */

static const char p_msearch[] =
    "M-SEARCH * HTTP/1.1\r\n"
    "HOST: 239.255.255.250:1900\r\n"
    "MAN: \"ssdp:discover\"\r\n"
    "MX: 2\r\n"
    "ST: ssdp:all\r\n"
    "\r\n";

static const char p_msearch6[] =
    "M-SEARCH * HTTP/1.1\r\n"
    "HOST: [ff02::c]:1900\r\n"
    "MAN: \"ssdp:discover\"\r\n"
    "MX: 2\r\n"
    "ST: ssdp:all\r\n"
    "\r\n";

static UBYTE p_rxbuf[1500];

static VOID p_step(const char *what, LONG rc, struct Library *sb, LONG want)
{
    LONG err = (rc < 0) ? p_errno(sb) : 0;

    if (want == 0)
    {
        Printf((CONST_STRPTR)"  %-34s %s (rc %ld, errno %ld)\n",
               (LONG)what, (LONG)((rc == 0) ? "ok" : "FAILED"), rc, err);
        return;
    }

    /* A step whose success IS an error code. */
    Printf((CONST_STRPTR)"  %-34s %s (rc %ld, errno %ld, wanted %ld)\n",
           (LONG)what, (LONG)((rc < 0 && err == want) ? "ok" : "FAILED"),
           rc, err, want);
}

static VOID p_probe_v4(struct Library *sb)
{
    ProbeAddr       group;
    ProbeMreq       mreq;
    LONG            s;
    LONG            value;
    LONG            back;
    LONG            backlen;
    ULONG           i;
    LONG            rounds;

    Printf((CONST_STRPTR)"IPv4, 239.255.255.250:1900\n");

    s = p_socket(sb, P_AF_INET, P_SOCK_DGRAM, 0);
    if (s < 0)
    {
        Printf((CONST_STRPTR)"  socket failed, errno %ld\n", p_errno(sb));
        return;
    }

    Printf((CONST_STRPTR)"options:\n");

    value = 2;
    p_step("setsockopt(IP_MULTICAST_TTL, 2)",
           p_setsockopt(sb, s, P_IPPROTO_IP, P_IP_MULTICAST_TTL, &value,
                        (LONG)sizeof(value)), sb, 0);

    back    = -1;
    backlen = (LONG)sizeof(back);
    p_step("getsockopt(IP_MULTICAST_TTL)",
           p_getsockopt(sb, s, P_IPPROTO_IP, P_IP_MULTICAST_TTL, &back,
                        &backlen), sb, 0);
    Printf((CONST_STRPTR)"      reads back %ld, wanted 2\n", back);

    value = 0;
    p_step("setsockopt(IP_MULTICAST_LOOP, 0)",
           p_setsockopt(sb, s, P_IPPROTO_IP, P_IP_MULTICAST_LOOP, &value,
                        (LONG)sizeof(value)), sb, 0);

    /* INADDR_ANY: the route chooses, which is what a portable client sends. */
    value = 0;
    p_step("setsockopt(IP_MULTICAST_IF, ANY)",
           p_setsockopt(sb, s, P_IPPROTO_IP, P_IP_MULTICAST_IF, &value,
                        (LONG)sizeof(value)), sb, 0);

    Printf((CONST_STRPTR)"membership:\n");

    for (i = 0; i < (ULONG)sizeof(group.sin_zero); i++)
        group.sin_zero[i] = 0;

    group.sin_len    = (UBYTE)sizeof(group);
    group.sin_family = P_AF_INET;
    group.sin_port   = P_SSDP_PORT;
    group.sin_addr   = P_SSDP_GROUP;

    p_step("bind(239.255.255.250:1900)",
           p_bind(sb, s, &group, (LONG)sizeof(group)), sb, 0);

    mreq.imr_multiaddr = P_SSDP_GROUP;
    mreq.imr_interface = 0UL;

    p_step("IP_ADD_MEMBERSHIP",
           p_setsockopt(sb, s, P_IPPROTO_IP, P_IP_ADD_MEMBERSHIP, &mreq,
                        (LONG)sizeof(mreq)), sb, 0);

    p_step("IP_ADD_MEMBERSHIP again",
           p_setsockopt(sb, s, P_IPPROTO_IP, P_IP_ADD_MEMBERSHIP, &mreq,
                        (LONG)sizeof(mreq)), sb, P_EADDRINUSE);

    Printf((CONST_STRPTR)"traffic:\n");

    p_step("sendto(M-SEARCH)",
           (p_sendto(sb, s, p_msearch, (LONG)(sizeof(p_msearch) - 1), &group,
                     (LONG)sizeof(group)) == (LONG)(sizeof(p_msearch) - 1))
               ? 0 : -1, sb, 0);

    /* Four one-second passes, so a slow responder still lands inside. */
    for (rounds = 0; rounds < 4; rounds++)
    {
        struct timeval tv;
        /* One word is an fd_set for anything below 32, which a probe that
           opens one socket always is. */
        ULONG          readfds = (1UL << s);
        LONG           ready;

        tv.tv_secs  = 1;
        tv.tv_micro = 0;

        ready = p_waitselect(sb, s + 1, &readfds, &tv);
        if (ready <= 0)
            continue;

        {
            ProbeAddr from;
            LONG      fromlen = (LONG)sizeof(from);
            LONG      got;

            from.sin_addr = 0;
            got = p_recvfrom(sb, s, p_rxbuf, (LONG)sizeof(p_rxbuf) - 1,
                             &from, &fromlen);
            if (got < 0)
            {
                Printf((CONST_STRPTR)"  recvfrom FAILED, errno %ld\n",
                       p_errno(sb));
                break;
            }

            Printf((CONST_STRPTR)"  %ld bytes from %ld.%ld.%ld.%ld:%ld\n",
                   got,
                   (LONG)((from.sin_addr >> 24) & 0xffUL),
                   (LONG)((from.sin_addr >> 16) & 0xffUL),
                   (LONG)((from.sin_addr >> 8) & 0xffUL),
                   (LONG)(from.sin_addr & 0xffUL),
                   (LONG)from.sin_port);
        }
    }

    Printf((CONST_STRPTR)"leaving:\n");

    p_step("IP_DROP_MEMBERSHIP",
           p_setsockopt(sb, s, P_IPPROTO_IP, P_IP_DROP_MEMBERSHIP, &mreq,
                        (LONG)sizeof(mreq)), sb, 0);

    p_step("IP_DROP_MEMBERSHIP again",
           p_setsockopt(sb, s, P_IPPROTO_IP, P_IP_DROP_MEMBERSHIP, &mreq,
                        (LONG)sizeof(mreq)), sb, P_EADDRNOTAVAIL);

    (VOID)p_close(sb, s);
}

/* --------------------------------------------------------------- IPv6 ----- */

static VOID p_probe_v6(struct Library *sb)
{
    ProbeAddr6      group;
    ProbeMreq6      mreq;
    LONG            s;
    LONG            value;
    LONG            back;
    LONG            backlen;
    ULONG           i;
    LONG            rounds;

    Printf((CONST_STRPTR)"\nIPv6, ff02::c:1900\n");

    s = p_socket(sb, P_AF_INET6, P_SOCK_DGRAM, 0);
    if (s < 0)
    {
        Printf((CONST_STRPTR)"  no AF_INET6 in this build (errno %ld), "
                             "skipped\n", p_errno(sb));
        return;
    }

    Printf((CONST_STRPTR)"options:\n");

    value = 2;
    p_step("setsockopt(IPV6_MULTICAST_HOPS, 2)",
           p_setsockopt(sb, s, P_IPPROTO_IPV6, P_IPV6_MULTICAST_HOPS, &value,
                        (LONG)sizeof(value)), sb, 0);

    back    = -1;
    backlen = (LONG)sizeof(back);
    p_step("getsockopt(IPV6_MULTICAST_HOPS)",
           p_getsockopt(sb, s, P_IPPROTO_IPV6, P_IPV6_MULTICAST_HOPS, &back,
                        &backlen), sb, 0);
    Printf((CONST_STRPTR)"      reads back %ld, wanted 2\n", back);

    value = 1;
    p_step("setsockopt(IPV6_MULTICAST_LOOP, 1)",
           p_setsockopt(sb, s, P_IPPROTO_IPV6, P_IPV6_MULTICAST_LOOP, &value,
                        (LONG)sizeof(value)), sb, 0);

    back    = -1;
    backlen = (LONG)sizeof(back);
    p_step("getsockopt(IPV6_MULTICAST_LOOP)",
           p_getsockopt(sb, s, P_IPPROTO_IPV6, P_IPV6_MULTICAST_LOOP, &back,
                        &backlen), sb, 0);
    Printf((CONST_STRPTR)"      reads back %ld, wanted 0, no IPv6 "
                         "multicast loopback in this stack\n", back);

    /* 0: the route chooses, which is what a portable client sends. */
    value = 0;
    p_step("setsockopt(IPV6_MULTICAST_IF, 0)",
           p_setsockopt(sb, s, P_IPPROTO_IPV6, P_IPV6_MULTICAST_IF, &value,
                        (LONG)sizeof(value)), sb, 0);

    Printf((CONST_STRPTR)"membership:\n");

    group.sin6_family   = P_AF_INET6;
    group.sin6_pad      = 0;
    group.sin6_port     = P_SSDP_PORT;
    group.sin6_flowinfo = 0UL;
    group.sin6_scope_id = 0UL;
    for (i = 0; i < 16UL; i++)
        group.sin6_addr[i] = p_ssdp6_group[i];

    p_step("bind(ff02::c:1900)",
           p_bind(sb, s, &group, (LONG)sizeof(group)), sb, 0);

    for (i = 0; i < 16UL; i++)
        mreq.ipv6mr_multiaddr[i] = p_ssdp6_group[i];
    mreq.ipv6mr_interface = 0UL;

    p_step("IPV6_JOIN_GROUP",
           p_setsockopt(sb, s, P_IPPROTO_IPV6, P_IPV6_JOIN_GROUP, &mreq,
                        (LONG)sizeof(mreq)), sb, 0);

    p_step("IPV6_JOIN_GROUP again",
           p_setsockopt(sb, s, P_IPPROTO_IPV6, P_IPV6_JOIN_GROUP, &mreq,
                        (LONG)sizeof(mreq)), sb, P_EADDRINUSE);

    Printf((CONST_STRPTR)"traffic:\n");

    p_step("sendto(M-SEARCH)",
           (p_sendto(sb, s, p_msearch6, (LONG)(sizeof(p_msearch6) - 1), &group,
                     (LONG)sizeof(group)) == (LONG)(sizeof(p_msearch6) - 1))
               ? 0 : -1, sb, 0);

    for (rounds = 0; rounds < 4; rounds++)
    {
        struct timeval tv;
        ULONG          readfds = (1UL << s);
        LONG           ready;

        tv.tv_secs  = 1;
        tv.tv_micro = 0;

        ready = p_waitselect(sb, s + 1, &readfds, &tv);
        if (ready <= 0)
            continue;

        {
            ProbeAddr6 from;
            LONG       fromlen = (LONG)sizeof(from);
            LONG       got;

            got = p_recvfrom(sb, s, p_rxbuf, (LONG)sizeof(p_rxbuf) - 1,
                             &from, &fromlen);
            if (got < 0)
            {
                Printf((CONST_STRPTR)"  recvfrom FAILED, errno %ld\n",
                       p_errno(sb));
                break;
            }

            Printf((CONST_STRPTR)"  %ld bytes from %lx:%lx:...:%lx port %ld\n",
                   got,
                   (LONG)(((ULONG)from.sin6_addr[0] << 8) | from.sin6_addr[1]),
                   (LONG)(((ULONG)from.sin6_addr[2] << 8) | from.sin6_addr[3]),
                   (LONG)(((ULONG)from.sin6_addr[14] << 8) | from.sin6_addr[15]),
                   (LONG)from.sin6_port);
        }
    }

    Printf((CONST_STRPTR)"leaving:\n");

    p_step("IPV6_LEAVE_GROUP",
           p_setsockopt(sb, s, P_IPPROTO_IPV6, P_IPV6_LEAVE_GROUP, &mreq,
                        (LONG)sizeof(mreq)), sb, 0);

    p_step("IPV6_LEAVE_GROUP again",
           p_setsockopt(sb, s, P_IPPROTO_IPV6, P_IPV6_LEAVE_GROUP, &mreq,
                        (LONG)sizeof(mreq)), sb, P_EADDRNOTAVAIL);

    (VOID)p_close(sb, s);
}

int main(void)
{
    struct Library *sb;

    sb = OpenLibrary((CONST_STRPTR)"bsdsocket.library", 4UL);
    if (sb == NULL)
    {
        Printf((CONST_STRPTR)"no bsdsocket.library\n");
        return RETURN_FAIL;
    }

    p_probe_v4(sb);
    p_probe_v6(sb);

    CloseLibrary(sb);

    return RETURN_OK;
}

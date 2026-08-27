/*
 * SrcProbe, does a socket send from the address it was bound to?
 *
 * argv: 1 = the guest's own address, 2 = a destination on the same link, and
 * optionally 3 = the guest's address on a SECOND interface and 4 = a host on
 * that subnet with a listener on port 7805.
 *
 * SPDX-License-Identifier: MIT
 */

#include <exec/types.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>

/* struct sockaddr_in, open-coded, four fields and a pad, unchanged since
   4.2BSD. */
typedef struct ProbeAddr
{
    UBYTE   sin_len;
    UBYTE   sin_family;
    UWORD   sin_port;
    ULONG   sin_addr;
    UBYTE   sin_zero[8];
} ProbeAddr;

/* struct sockaddr_in6, open-coded the same way.  The NDK's is the Linux one:
   no sin6_len, the family at offset 0 and a byte of padding after it.  28
   bytes. */
typedef struct ProbeAddr6
{
    UBYTE   sin6_family;
    UBYTE   sin6_pad;
    UWORD   sin6_port;
    ULONG   sin6_flowinfo;
    UBYTE   sin6_addr[16];
    ULONG   sin6_scope_id;
} ProbeAddr6;

#define P_AF_INET           2
#define P_AF_INET6          23
#define P_SOCK_STREAM       1
#define P_SOCK_DGRAM        2

#define P_EADDRNOTAVAIL     49
#define P_ENETUNREACH       51

#define P_LOOPBACK          0x7F000001UL

/* Ports of our own, above anything the stack or SLIRP uses. */
#define PORT_UDP_LAN        7801
#define PORT_UDP_LOOP       7802
#define PORT_TCP_LOOP       7803
#define PORT_TCP_LAN        7804
#define PORT_TCP_ALT        7805

/* ------------------------------------------------------------- vectors ---- */

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

static LONG p_bind_raw(struct Library *base, LONG s, CONST_APTR sa, LONG salen)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = s;
    register CONST_APTR      a0  __asm("a0") = sa;
    register LONG            d1  __asm("d1") = salen;
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

static LONG p_connect_raw(struct Library *base, LONG s, CONST_APTR sa, LONG salen)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = s;
    register CONST_APTR      a0  __asm("a0") = sa;
    register LONG            d1  __asm("d1") = salen;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");

    __asm __volatile ("jsr a6@(-54:W)"      /* connect */
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0)
                      : "r" (a6), "r" (d0), "r" (a0), "r" (d1)
                      : "a1", "cc", "memory");
    return res;
}

static LONG p_sendto_raw(struct Library *base, LONG s, const void *buf, LONG len,
                         CONST_APTR to, LONG tolen)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = s;
    register CONST_APTR      a0  __asm("a0") = (CONST_APTR)buf;
    register LONG            d1  __asm("d1") = len;
    register LONG            d2  __asm("d2") = 0;
    register CONST_APTR      a1  __asm("a1") = to;
    register LONG            d3  __asm("d3") = tolen;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");
    register LONG _clob_a1 __asm("a1");

    __asm __volatile ("jsr a6@(-60:W)"      /* sendto */
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0),
                        "=r" (_clob_a1)
                      : "r" (a6), "r" (d0), "r" (a0), "r" (d1), "r" (d2),
                        "r" (a1), "r" (d3)
                      : "cc", "memory");
    return res;
}

static LONG p_recvfrom(struct Library *base, LONG s, void *buf, LONG len,
                       ProbeAddr *from)
{
    LONG namelen = (LONG)sizeof(*from);

    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = s;
    register APTR            a0  __asm("a0") = (APTR)buf;
    register LONG            d1  __asm("d1") = len;
    register LONG            d2  __asm("d2") = 0;
    register APTR            a1  __asm("a1") = (APTR)from;
    register APTR            a2  __asm("a2") = (APTR)&namelen;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");
    register LONG _clob_a1 __asm("a1");

    __asm __volatile ("jsr a6@(-72:W)"      /* recvfrom */
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0),
                        "=r" (_clob_a1)
                      : "r" (a6), "r" (d0), "r" (a0), "r" (d1), "r" (d2),
                        "r" (a1), "r" (a2)
                      : "cc", "memory");
    return res;
}

static LONG p_getsockname_raw(struct Library *base, LONG s, APTR sa, LONG salen)
{
    LONG namelen = salen;

    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = s;
    register APTR            a0  __asm("a0") = sa;
    register APTR            a1  __asm("a1") = (APTR)&namelen;
    register LONG            res __asm("d0");
    register LONG _clob_a0 __asm("a0");
    register LONG _clob_a1 __asm("a1");

    __asm __volatile ("jsr a6@(-102:W)"     /* getsockname */
                      : "=r" (res), "=r" (_clob_a0), "=r" (_clob_a1)
                      : "r" (a6), "r" (d0), "r" (a0), "r" (a1)
                      : "d1", "cc", "memory");
    return res;
}

static LONG p_accept(struct Library *base, LONG s, ProbeAddr *from)
{
    LONG namelen = (LONG)sizeof(*from);

    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = s;
    register APTR            a0  __asm("a0") = (APTR)from;
    register APTR            a1  __asm("a1") = (APTR)&namelen;
    register LONG            res __asm("d0");
    register LONG _clob_a0 __asm("a0");
    register LONG _clob_a1 __asm("a1");

    __asm __volatile ("jsr a6@(-48:W)"      /* accept */
                      : "=r" (res), "=r" (_clob_a0), "=r" (_clob_a1)
                      : "r" (a6), "r" (d0), "r" (a0), "r" (a1)
                      : "d1", "cc", "memory");
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

static LONG p_bind(struct Library *base, LONG s, const ProbeAddr *sa)
{
    return p_bind_raw(base, s, (CONST_APTR)sa, (LONG)sizeof(*sa));
}

static LONG p_connect(struct Library *base, LONG s, const ProbeAddr *sa)
{
    return p_connect_raw(base, s, (CONST_APTR)sa, (LONG)sizeof(*sa));
}

static LONG p_sendto(struct Library *base, LONG s, const void *buf, LONG len,
                     const ProbeAddr *to)
{
    return p_sendto_raw(base, s, buf, len, (CONST_APTR)to, (LONG)sizeof(*to));
}

static LONG p_getsockname(struct Library *base, LONG s, ProbeAddr *sa)
{
    return p_getsockname_raw(base, s, (APTR)sa, (LONG)sizeof(*sa));
}

static LONG p_connect6(struct Library *base, LONG s, const ProbeAddr6 *sa)
{
    return p_connect_raw(base, s, (CONST_APTR)sa, (LONG)sizeof(*sa));
}

static LONG p_sendto6(struct Library *base, LONG s, const void *buf, LONG len,
                      const ProbeAddr6 *to)
{
    return p_sendto_raw(base, s, buf, len, (CONST_APTR)to, (LONG)sizeof(*to));
}

static LONG p_getsockname6(struct Library *base, LONG s, ProbeAddr6 *sa)
{
    return p_getsockname_raw(base, s, (APTR)sa, (LONG)sizeof(*sa));
}

/* --------------------------------------------------------------- checks --- */

static LONG p_checks;
static LONG p_failures;

/*
 * ONE LINE PER CHECK, key=value, at column zero.  run-srcsel.sh matches
 * `<key>=ok` exactly, so a renamed key makes the harness say which key went
 * missing instead of quietly passing on an arm that no longer runs.
 */
static BOOL p_check(BOOL ok, const char *what, LONG detail)
{
    p_checks++;

    if (!ok)
    {
        p_failures++;
        Printf((CONST_STRPTR)"%s=fail:%ld\n", (ULONG)what, (ULONG)detail);
        return FALSE;
    }

    Printf((CONST_STRPTR)"%s=ok\n", (ULONG)what);
    return TRUE;
}

static VOID p_addr(ProbeAddr *sa, ULONG host, UWORD port)
{
    ULONG i;

    sa->sin_len    = (UBYTE)sizeof(*sa);
    sa->sin_family = P_AF_INET;
    sa->sin_port   = port;
    sa->sin_addr   = host;

    for (i = 0; i < sizeof(sa->sin_zero); i++)
        sa->sin_zero[i] = 0;
}

/* An IPv6 address from four longwords, most significant first, written into the
   16 network-order bytes of a sockaddr_in6. */
static VOID p_addr6(ProbeAddr6 *sa, ULONG w0, ULONG w1, ULONG w2, ULONG w3,
                    UWORD port)
{
    ULONG word[4];
    ULONG i;

    word[0] = w0;
    word[1] = w1;
    word[2] = w2;
    word[3] = w3;

    sa->sin6_family   = P_AF_INET6;
    sa->sin6_pad      = 0;
    sa->sin6_port     = port;
    sa->sin6_flowinfo = 0;
    sa->sin6_scope_id = 0;

    for (i = 0; i < 16; i++)
        sa->sin6_addr[i] = (UBYTE)((word[i / 4] >> (24 - (8 * (i % 4)))) & 0xFF);
}

/* The four longwords back out of one, most significant first. */
static ULONG p_word6(const ProbeAddr6 *sa, ULONG index)
{
    ULONG base = index * 4;

    return ((ULONG)sa->sin6_addr[base]     << 24) |
           ((ULONG)sa->sin6_addr[base + 1] << 16) |
           ((ULONG)sa->sin6_addr[base + 2] <<  8) |
            (ULONG)sa->sin6_addr[base + 3];
}

static BOOL p_same6(const ProbeAddr6 *sa, ULONG w0, ULONG w1, ULONG w2, ULONG w3)
{
    return (BOOL)(p_word6(sa, 0) == w0 && p_word6(sa, 1) == w1 &&
                  p_word6(sa, 2) == w2 && p_word6(sa, 3) == w3);
}

/* Dotted decimal only: the addresses come from the harness, not from a user. */
static ULONG p_parse(const char *s, ULONG fallback)
{
    ULONG octet[4];
    ULONG n = 0;
    ULONG v = 0;
    BOOL  digit = FALSE;

    if (s == NULL)
        return fallback;

    for (;; s++)
    {
        if (*s >= '0' && *s <= '9')
        {
            v = (v * 10UL) + (ULONG)(*s - '0');
            if (v > 255UL)
                return fallback;
            digit = TRUE;
            continue;
        }

        if (!digit || n >= 4)
            return fallback;

        octet[n++] = v;
        v = 0;
        digit = FALSE;

        if (*s != '.')
            break;
    }

    if (n != 4)
        return fallback;

    return (octet[0] << 24) | (octet[1] << 16) | (octet[2] << 8) | octet[3];
}

/* -------------------------------------------------------- the datagrams --- */

/* Send one datagram to a receiver in this process and report the source address
   it arrived with: a send that returns len proves only that something left. */
static VOID p_udp_source(struct Library *base, ULONG bind_to, ULONG dest,
                         UWORD port, const char *what)
{
    static const UBYTE payload[] = "srcprobe";
    LONG      server = -1;
    LONG      client = -1;
    ProbeAddr sa;
    ProbeAddr from;
    UBYTE     buffer[32];
    LONG      rc;

    server = p_socket(base, P_AF_INET, P_SOCK_DGRAM, 0);
    client = p_socket(base, P_AF_INET, P_SOCK_DGRAM, 0);
    if (server < 0 || client < 0)
    {
        (VOID)p_check(FALSE, what, p_errno(base));
        goto done;
    }

    /* The receiver takes the wildcard, so it is not itself under test. */
    p_addr(&sa, 0UL, port);
    if (p_bind(base, server, &sa) != 0)
    {
        (VOID)p_check(FALSE, what, p_errno(base));
        goto done;
    }

    p_addr(&sa, bind_to, 0);
    if (p_bind(base, client, &sa) != 0)
    {
        (VOID)p_check(FALSE, what, p_errno(base));
        goto done;
    }

    p_addr(&sa, dest, port);
    rc = p_sendto(base, client, payload, (LONG)sizeof(payload), &sa);
    if (rc != (LONG)sizeof(payload))
    {
        (VOID)p_check(FALSE, what, p_errno(base));
        goto done;
    }

    from.sin_addr = 0;
    rc = p_recvfrom(base, server, buffer, (LONG)sizeof(buffer), &from);
    if (rc != (LONG)sizeof(payload))
    {
        (VOID)p_check(FALSE, what, rc);
        goto done;
    }

    (VOID)p_check((BOOL)(from.sin_addr == bind_to), what,
                  (LONG)from.sin_addr);

done:
    if (client >= 0)
        (VOID)p_close(base, client);
    if (server >= 0)
        (VOID)p_close(base, server);
}

/* -------------------------------------------------- RFC 6724, on target --- */

/*
 * getsockname() on a connected socket bound to in6addr_any reports the RFC 6724
 * selection, so no peer has to answer for any of these.  The addresses below
 * are RFC 3849 constants and must agree with run-srcsel.sh's ADDRESS6.
 */

/* 2001:db8:6724:1::10/64, the guest's own, from ADDRESS6. */
#define SELF6_0     0x20010DB8UL
#define SELF6_1     0x67240001UL
#define SELF6_2     0UL
#define SELF6_3     0x10UL

/* 2001:db8:6724:1::1, the same /64: on-link, no router needed. */
#define ONLINK6_3   0x1UL

/* 2001:db8:6724:9::1, a different /64 with no route to it at all. */
#define OFF6_1      0x67240009UL

/*
 * THE SECOND STATIC ADDRESS, and the whole of what separates RFC 6724 rule 6
 * from rule 8 on a real machine.
 *
 *   SELF6    2001:db8:6724:1::10/64   policy label 1  (::/0)
 *   SECOND6  2001:0:6724:1::10/31     policy label 5  (2001::/32, Teredo)
 *
 * The destination RULE6_ below is 2001:1:6724:1::99.  It is on-link through
 * SECOND6's /31 and through nothing else, so there is a route to it; its label
 * is 1, because 2001:0001:: is not inside 2001:0000::/32.
 *
 *   CommonPrefixLen(SECOND6, dest) = 31      rule 8 says SECOND6
 *   CommonPrefixLen(SELF6,   dest) = 20
 *   label(dest) == label(SELF6)              rule 6 says SELF6
 *
 * Rule 6 runs first, so the answer is SELF6, and the answer is different from
 * what every rule below it would give.  Rules 1, 2, 3 and 5 cannot decide it:
 * neither address IS the destination, both are global scope, both are VALID
 * and both are on eth0.
 *
 * THE /31 IS DELIBERATE AND NOT A TYPO.  The policy table's boundary is at
 * /32, so an on-link prefix that holds addresses with two different labels has
 * to be shorter than that; /31 is the longest that does, and it is short
 * enough to leave OFF6_ above off-link, which is what keeps
 * v6_offlink_no_route_refused meaning what it meant.
 */
#define SECOND6_0   0x20010000UL
#define SECOND6_1   0x67240001UL
#define SECOND6_2   0UL
#define SECOND6_3   0x10UL

/* 2001:1:6724:1::99: label 1 like SELF6, on-link through SECOND6's /31. */
#define RULE6_0     0x20010001UL
#define RULE6_1     0x67240001UL
#define RULE6_3     0x99UL

/* 2001:0:6724:2::99: label 5 like SECOND6, same /31.  The control -- rule 6
   and rule 8 agree here, and the answer has to be the other address, or the
   arm above is only reporting that SELF6 is always chosen. */
#define CTRL6_0     0x20010000UL
#define CTRL6_1     0x67240002UL
#define CTRL6_3     0x99UL

#define LL6_0       0xFE800000UL        /* fe80::1  */

#define PORT_UDP_V6 7806

/*
 * Duplicate address detection has to finish before either address is this
 * node's, and nothing waits for it: a TENTATIVE address is correctly not a
 * candidate, so running the arm too early reads as no route.  Up to 15 s.
 */
static BOOL p_wait6(struct Library *base)
{
    ProbeAddr6 sa;
    LONG       tries;
    LONG       s;

    for (tries = 0; tries < 30; tries++)
    {
        s = p_socket(base, P_AF_INET6, P_SOCK_DGRAM, 0);
        if (s >= 0)
        {
            p_addr6(&sa, SELF6_0, SELF6_1, SELF6_2, ONLINK6_3, PORT_UDP_V6);

            if (p_connect6(base, s, &sa) == 0)
            {
                p_addr6(&sa, 0, 0, 0, 0, 0);
                if (p_getsockname6(base, s, &sa) == 0 &&
                    p_same6(&sa, SELF6_0, SELF6_1, SELF6_2, SELF6_3))
                {
                    (VOID)p_close(base, s);
                    return p_check(TRUE, "v6_address_ready", tries);
                }
            }

            (VOID)p_close(base, s);
        }

        Delay(25);
    }

    return p_check(FALSE, "v6_address_ready", p_errno(base));
}

static VOID p_ipv6_source(struct Library *base)
{
    ProbeAddr6 sa;
    ProbeAddr6 name;
    LONG       s;
    LONG       rc;

    s = p_socket(base, P_AF_INET6, P_SOCK_DGRAM, 0);
    if (!p_check((BOOL)(s >= 0), "v6_socket", p_errno(base)))
    {
        return;
    }
    (VOID)p_close(base, s);

    if (!p_wait6(base))
        return;

    /* A destination on this node's own /64: Rule 2 takes the global, since the
       link-local has scope 2 and the destination scope 14. */
    s = p_socket(base, P_AF_INET6, P_SOCK_DGRAM, 0);
    if (s >= 0)
    {
        p_addr6(&sa, SELF6_0, SELF6_1, SELF6_2, ONLINK6_3, PORT_UDP_V6);
        rc = p_connect6(base, s, &sa);
        if (p_check((BOOL)(rc == 0), "v6_connect_onlink", p_errno(base)))
        {
            p_addr6(&name, 0, 0, 0, 0, 0);
            (VOID)p_getsockname6(base, s, &name);
            (VOID)p_check(p_same6(&name, SELF6_0, SELF6_1, SELF6_2, SELF6_3),
                          "v6_src_onlink_is_global", (LONG)p_word6(&name, 0));
        }
        (VOID)p_close(base, s);
    }

    /* A link-local destination: Rule 2 now takes the link-local, because the
       global has more scope than the destination.  Which link-local this node
       holds is derived from the MAC, so fe80::/10 is what is asserted. */
    s = p_socket(base, P_AF_INET6, P_SOCK_DGRAM, 0);
    if (s >= 0)
    {
        p_addr6(&sa, LL6_0, 0, 0, 1, PORT_UDP_V6);
        rc = p_connect6(base, s, &sa);
        if (p_check((BOOL)(rc == 0), "v6_connect_linklocal", p_errno(base)))
        {
            p_addr6(&name, 0, 0, 0, 0, 0);
            (VOID)p_getsockname6(base, s, &name);

            (VOID)p_check((BOOL)((p_word6(&name, 0) & 0xFFC00000UL) == LL6_0),
                          "v6_src_linklocal_is_linklocal",
                          (LONG)p_word6(&name, 0));
            (VOID)p_check((BOOL)(!p_same6(&name, SELF6_0, SELF6_1, SELF6_2,
                                          SELF6_3)),
                          "v6_src_linklocal_is_not_global",
                          (LONG)p_word6(&name, 3));
        }
        (VOID)p_close(base, s);
    }

    /* ::1.  RFC 6724 4 puts the loopback interface on a link of its own, so
       the candidate set for a loopback destination holds ::1 and nothing
       else. */
    s = p_socket(base, P_AF_INET6, P_SOCK_DGRAM, 0);
    if (s >= 0)
    {
        p_addr6(&sa, 0, 0, 0, 1, PORT_UDP_V6);
        rc = p_connect6(base, s, &sa);
        if (p_check((BOOL)(rc == 0), "v6_connect_loopback", p_errno(base)))
        {
            p_addr6(&name, 0, 0, 0, 0, 0);
            (VOID)p_getsockname6(base, s, &name);
            (VOID)p_check(p_same6(&name, 0, 0, 0, 1),
                          "v6_src_loopback_is_loopback",
                          (LONG)p_word6(&name, 0));
        }
        (VOID)p_close(base, s);
    }

    /*
     * RULE 6, THE ONE THIS HARNESS COULD NOT REACH.  See SECOND6_ above for
     * the arithmetic; the short version is that rule 8 would answer SECOND6
     * and rule 6 answers SELF6.
     *
     * The control below runs FIRST, because it is also the wait: the second
     * address goes through duplicate address detection like the first and is
     * not a candidate until it is VALID, and running the rule 6 arm before
     * that would read as rule 6 working when there was only ever one address
     * to choose from.
     */
    {
        LONG tries;

        for (tries = 0; tries < 30; tries++)
        {
            s = p_socket(base, P_AF_INET6, P_SOCK_DGRAM, 0);
            if (s < 0)
                break;

            p_addr6(&sa, CTRL6_0, CTRL6_1, 0, CTRL6_3, PORT_UDP_V6);
            if (p_connect6(base, s, &sa) == 0)
            {
                p_addr6(&name, 0, 0, 0, 0, 0);
                if (p_getsockname6(base, s, &name) == 0 &&
                    p_same6(&name, SECOND6_0, SECOND6_1, SECOND6_2, SECOND6_3))
                {
                    (VOID)p_close(base, s);
                    break;
                }
            }

            (VOID)p_close(base, s);
            s = -1;
            Delay(25);
        }

        (VOID)p_check((BOOL)(tries < 30), "v6_second_address_ready",
                      (LONG)tries);
    }

    /* The control, asserted rather than only waited on. */
    s = p_socket(base, P_AF_INET6, P_SOCK_DGRAM, 0);
    if (s >= 0)
    {
        p_addr6(&sa, CTRL6_0, CTRL6_1, 0, CTRL6_3, PORT_UDP_V6);
        rc = p_connect6(base, s, &sa);
        if (p_check((BOOL)(rc == 0), "v6_connect_second_prefix",
                    p_errno(base)))
        {
            p_addr6(&name, 0, 0, 0, 0, 0);
            (VOID)p_getsockname6(base, s, &name);
            (VOID)p_check(p_same6(&name, SECOND6_0, SECOND6_1, SECOND6_2,
                                  SECOND6_3),
                          "v6_src_matching_label_is_second",
                          (LONG)p_word6(&name, 0));
        }
        (VOID)p_close(base, s);
    }

    /* And rule 6 itself. */
    s = p_socket(base, P_AF_INET6, P_SOCK_DGRAM, 0);
    if (s >= 0)
    {
        p_addr6(&sa, RULE6_0, RULE6_1, 0, RULE6_3, PORT_UDP_V6);
        rc = p_connect6(base, s, &sa);
        if (p_check((BOOL)(rc == 0), "v6_connect_rule6", p_errno(base)))
        {
            p_addr6(&name, 0, 0, 0, 0, 0);
            (VOID)p_getsockname6(base, s, &name);

            (VOID)p_check(p_same6(&name, SELF6_0, SELF6_1, SELF6_2, SELF6_3),
                          "v6_rule6_label_beats_longest_prefix",
                          (LONG)p_word6(&name, 0));
            (VOID)p_check((BOOL)(!p_same6(&name, SECOND6_0, SECOND6_1,
                                          SECOND6_2, SECOND6_3)),
                          "v6_rule6_not_the_longer_prefix",
                          (LONG)p_word6(&name, 1));
        }
        (VOID)p_close(base, s);
    }

    /* A global destination that is not on-link, with no default router: there
       is no outgoing interface for it, so there is no source either and the
       send is refused. */
    s = p_socket(base, P_AF_INET6, P_SOCK_DGRAM, 0);
    if (s >= 0)
    {
        static const UBYTE payload[] = "srcprobe6";

        p_addr6(&sa, SELF6_0, OFF6_1, 0, 1, PORT_UDP_V6);
        rc = p_sendto6(base, s, payload, (LONG)sizeof(payload), &sa);
        (VOID)p_check((BOOL)(rc == -1), "v6_offlink_no_route_refused",
                      p_errno(base));
        (VOID)p_close(base, s);
    }
}

/* ------------------------------------------------------------------ main -- */

int main(int argc, char **argv)
{
    static const UBYTE payload[] = "srcprobe";
    struct Library *base;
    ProbeAddr       sa;
    ProbeAddr       name;
    ULONG           self;
    ULONG           peer;
    ULONG           alt;
    ULONG           dest;
    LONG            s;
    LONG            listener;
    LONG            rc;

    self = p_parse((argc > 1) ? argv[1] : NULL, 0x0A00020FUL);  /* 10.0.2.15 */
    peer = p_parse((argc > 2) ? argv[2] : NULL, 0x0A000202UL);  /* 10.0.2.2  */
    alt  = (argc > 3) ? p_parse(argv[3], 0) : 0;
    dest = (argc > 4) ? p_parse(argv[4], 0) : 0;

    base = OpenLibrary((CONST_STRPTR)"bsdsocket.library", 4);
    if (base == NULL)
    {
        Printf((CONST_STRPTR)"srcprobe_library=fail\n");
        return RETURN_FAIL;
    }

    Printf((CONST_STRPTR)"srcprobe_self=0x%08lx\nsrcprobe_peer=0x%08lx\n",
           self, peer);

    /* ---- the bound address is the source ------------------------------- */

    p_udp_source(base, self, self, PORT_UDP_LAN,
                 "v4_udp_bound_iface_src");
    p_udp_source(base, P_LOOPBACK, P_LOOPBACK, PORT_UDP_LOOP,
                 "v4_udp_bound_loopback_src");

    /* ---- an unbound socket still routes -------------------------------- */

    s = p_socket(base, P_AF_INET, P_SOCK_DGRAM, 0);
    if (s >= 0)
    {
        p_addr(&sa, peer, 9);
        rc = p_sendto(base, s, payload, (LONG)sizeof(payload), &sa);
        (VOID)p_check((BOOL)(rc == (LONG)sizeof(payload)),
                      "v4_udp_unbound_sends",
                      p_errno(base));
        (VOID)p_close(base, s);
    }

    /* ---- a destination the bound address cannot reach ------------------ */

    s = p_socket(base, P_AF_INET, P_SOCK_DGRAM, 0);
    if (s >= 0)
    {
        p_addr(&sa, self, 0);
        rc = p_bind(base, s, &sa);
        if (p_check((BOOL)(rc == 0), "v4_bind_iface",
                    p_errno(base)))
        {
            p_addr(&sa, P_LOOPBACK, PORT_UDP_LOOP);
            rc = p_sendto(base, s, payload, (LONG)sizeof(payload), &sa);
            (VOID)p_check((BOOL)(rc == -1 &&
                                 p_errno(base) == P_ENETUNREACH),
                          "v4_udp_iface_to_loopback_unreach",
                          p_errno(base));
        }
        (VOID)p_close(base, s);
    }

    /* ---- TCP: connect() refuses what it cannot honour ------------------ */

    listener = p_socket(base, P_AF_INET, P_SOCK_STREAM, 0);
    if (listener >= 0)
    {
        p_addr(&sa, P_LOOPBACK, PORT_TCP_LOOP);
        rc = p_bind(base, listener, &sa);
        if (p_check((BOOL)(rc == 0), "v4_listener_bind_loopback",
                    p_errno(base)))
        {
            /*
             * Backlog of three, not one: two arms below connect here and
             * neither accepts, so with a backlog of one the second connect
             * burns the whole TCP connect timeout and reads like the defect.
             */
            rc = p_listen(base, listener, 3);
            (VOID)p_check((BOOL)(rc == 0), "v4_listen_loopback", p_errno(base));

            /* The bind and the route agree, so the connect goes ahead. */
            s = p_socket(base, P_AF_INET, P_SOCK_STREAM, 0);
            if (s >= 0)
            {
                p_addr(&sa, P_LOOPBACK, 0);
                rc = p_bind(base, s, &sa);
                (VOID)p_check((BOOL)(rc == 0), "v4_client_bind_loopback",
                              p_errno(base));

                p_addr(&sa, P_LOOPBACK, PORT_TCP_LOOP);
                rc = p_connect(base, s, &sa);
                if (p_check((BOOL)(rc == 0),
                            "v4_connect_loopback_from_loopback",
                            p_errno(base)))
                {
                    name.sin_addr = 0;
                    (VOID)p_getsockname(base, s, &name);
                    (VOID)p_check((BOOL)(name.sin_addr == P_LOOPBACK),
                                  "v4_getsockname_bound_loopback",
                                  (LONG)name.sin_addr);
                }
                (VOID)p_close(base, s);
            }

            /*
             * INADDR_ANY, which the arm above cannot reach because it binds.
             * A socket bound to nothing has no local address to report, so
             * getsockname() has to name the interface the packets leave by.
             */
            s = p_socket(base, P_AF_INET, P_SOCK_STREAM, 0);
            if (s >= 0)
            {
                p_addr(&sa, P_LOOPBACK, PORT_TCP_LOOP);
                rc = p_connect(base, s, &sa);
                if (p_check((BOOL)(rc == 0),
                            "v4_unbound_connect_loopback", p_errno(base)))
                {
                    name.sin_addr = 0;
                    (VOID)p_getsockname(base, s, &name);
                    (VOID)p_check((BOOL)(name.sin_addr == P_LOOPBACK),
                                  "v4_getsockname_unbound_route",
                                  (LONG)name.sin_addr);
                }
                (VOID)p_close(base, s);
            }

            /* The same for a datagram, which has no connect interface
               recorded on it and reaches the route lookup instead. */
            s = p_socket(base, P_AF_INET, P_SOCK_DGRAM, 0);
            if (s >= 0)
            {
                p_addr(&sa, P_LOOPBACK, PORT_UDP_LOOP);
                rc = p_connect(base, s, &sa);
                if (p_check((BOOL)(rc == 0),
                            "v4_unbound_dgram_connect_loopback",
                            p_errno(base)))
                {
                    name.sin_addr = 0;
                    (VOID)p_getsockname(base, s, &name);
                    (VOID)p_check((BOOL)(name.sin_addr == P_LOOPBACK),
                                  "v4_getsockname_unbound_dgram",
                                  (LONG)name.sin_addr);
                }
                (VOID)p_close(base, s);
            }

            /*
             * ENETUNREACH and not EADDRNOTAVAIL: the bound address exists, it
             * just has no route to the destination.  EADDRNOTAVAIL is now only
             * a bound address the machine does not have.
             */
            s = p_socket(base, P_AF_INET, P_SOCK_STREAM, 0);
            if (s >= 0)
            {
                p_addr(&sa, self, 0);
                rc = p_bind(base, s, &sa);
                (VOID)p_check((BOOL)(rc == 0),
                              "v4_client_bind_iface_for_loopback",
                              p_errno(base));

                p_addr(&sa, P_LOOPBACK, PORT_TCP_LOOP);
                rc = p_connect(base, s, &sa);
                (VOID)p_check((BOOL)(rc == -1 &&
                                     p_errno(base) == P_ENETUNREACH),
                              "v4_connect_loopback_from_iface_unreach",
                              p_errno(base));
                (VOID)p_close(base, s);
            }
        }
        (VOID)p_close(base, listener);
    }

    /* ---- TCP: the interface address is the source it connects from ----- */

    /* The pinned path on a physical interface rather than loopback: the bind
       and the route agree, so the connect goes through
       nxd_tcp_client_socket_source_connect(). */
    listener = p_socket(base, P_AF_INET, P_SOCK_STREAM, 0);
    if (listener >= 0)
    {
        p_addr(&sa, self, PORT_TCP_LAN);
        rc = p_bind(base, listener, &sa);
        if (p_check((BOOL)(rc == 0), "v4_listener_bind_iface",
                    p_errno(base)))
        {
            rc = p_listen(base, listener, 1);
            (VOID)p_check((BOOL)(rc == 0), "v4_listen_iface",
                          p_errno(base));

            s = p_socket(base, P_AF_INET, P_SOCK_STREAM, 0);
            if (s >= 0)
            {
                p_addr(&sa, self, 0);
                rc = p_bind(base, s, &sa);
                (VOID)p_check((BOOL)(rc == 0),
                              "v4_client_bind_iface",
                              p_errno(base));

                p_addr(&sa, self, PORT_TCP_LAN);
                rc = p_connect(base, s, &sa);
                if (p_check((BOOL)(rc == 0),
                            "v4_connect_iface_from_iface",
                            p_errno(base)))
                {
                    LONG conn;

                    name.sin_addr = 0;
                    conn = p_accept(base, listener, &name);
                    if (p_check((BOOL)(conn >= 0), "v4_listener_accepted",
                                p_errno(base)))
                    {
                        (VOID)p_check((BOOL)(name.sin_addr == self),
                                      "v4_accepted_peer_is_bound_src",
                                      (LONG)name.sin_addr);
                        (VOID)p_close(base, conn);
                    }
                }
                (VOID)p_close(base, s);
            }

            /* Nothing bound: the route chooses, as it always did. */
            s = p_socket(base, P_AF_INET, P_SOCK_STREAM, 0);
            if (s >= 0)
            {
                p_addr(&sa, self, PORT_TCP_LAN);
                rc = p_connect(base, s, &sa);
                if (p_check((BOOL)(rc == 0),
                            "v4_unbound_connect_route",
                            p_errno(base)))
                {
                    name.sin_addr = 0;
                    (VOID)p_getsockname(base, s, &name);
                    (VOID)p_check((BOOL)(name.sin_addr == self),
                                  "v4_getsockname_unbound_iface",
                                  (LONG)name.sin_addr);
                }
                (VOID)p_close(base, s);
            }
        }
        (VOID)p_close(base, listener);
    }

    /* ---- TCP: source on one interface, route out of the other ---------- */

    /* The case the source connect exists for, and the only one that needs a
       second interface.  What source the SYN carried is for the receiver to
       say. */
    if (alt != 0 && dest != 0)
    {
        s = p_socket(base, P_AF_INET, P_SOCK_STREAM, 0);
        if (s >= 0)
        {
            p_addr(&sa, alt, 0);
            rc = p_bind(base, s, &sa);
            if (p_check((BOOL)(rc == 0), "v4_bind_second_iface", p_errno(base)))
            {
                p_addr(&sa, dest, PORT_TCP_ALT);
                rc = p_connect(base, s, &sa);
                if (p_check((BOOL)(rc == 0),
                            "v4_connect_from_second_iface",
                            p_errno(base)))
                {
                    name.sin_addr = 0;
                    (VOID)p_getsockname(base, s, &name);
                    (VOID)p_check((BOOL)(name.sin_addr == alt),
                                  "v4_getsockname_second_iface",
                                  (LONG)name.sin_addr);
                }
            }
            (VOID)p_close(base, s);
        }
    }

    /* ---- an address the machine does not have -------------------------- */

    s = p_socket(base, P_AF_INET, P_SOCK_DGRAM, 0);
    if (s >= 0)
    {
        p_addr(&sa, 0xC0000201UL, 0);           /* 192.0.2.1, RFC 5737 */
        rc = p_bind(base, s, &sa);
        (VOID)p_check((BOOL)(rc == -1 && p_errno(base) == P_EADDRNOTAVAIL),
                      "v4_bind_foreign_eaddrnotavail",
                      p_errno(base));
        (VOID)p_close(base, s);
    }

    /* ---- RFC 6724, the source nothing named ---------------------------- */

    p_ipv6_source(base);

    Printf((CONST_STRPTR)"srcprobe_checks=%ld\nsrcprobe_failures=%ld\n",
           p_checks, p_failures);

    CloseLibrary(base);

    return (p_failures == 0) ? RETURN_OK : RETURN_FAIL;
}

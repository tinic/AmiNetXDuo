/*
 * SrcProbe, does a socket send from the address it was bound to?
 *
 * bind() names a local address; the send direction is meant to honour it.
 * NetX Duo has no bind-to-address, so the library maps the bound address to
 * the index nxd_udp_socket_source_send() and, for TCP,
 * nxd_tcp_client_socket_source_connect() take.  None of that is reachable
 * from a host build: it reads NX_IP's live interface table.
 *
 * TWO INTERFACES ARE WHAT THIS WANTS AND THE LAB GUEST HAS ONE.  So what is
 * measured here is the single-interface half:
 *
 *   * a datagram bound to the interface address arrives with that address as
 *     its source, not merely "the send returned len", which was already true
 *     when the source was whatever NetX picked;
 *   * the same on loopback, bound to 127.0.0.1;
 *   * a destination the bound address cannot reach is refused, rather than
 *     sent from another address or dropped inside the stack with the send
 *     reported successful;
 *   * a TCP connect() leaves from the bound address, on the interface and on
 *     loopback, and one whose bound address cannot reach the destination is
 *     refused before the SYN;
 *   * an unbound socket still routes as it always did.
 *
 * AND THE OTHER HALF, WHICH IS RFC 6724.  Everything above is about a source
 * the caller named.  p_ipv6_source() at the end is about the source nothing
 * named: what src/ipv6/ipv6_srcsel.c picks for an unbound AF_INET6 socket, on
 * a real machine, through the real library.  Its four addresses are constants
 * shared with run-srcsel.sh; see the block comment there.
 *
 * EVERY CHECK IS ONE key=value LINE at column zero, and run-srcsel.sh matches
 * the keys exactly.  Nothing in this program's output is meant to be read as
 * prose by anything but a person.
 *
 * argv[1] is the guest's own address (SLIRP's first lease, 10.0.2.15, by
 * default) and argv[2] a destination on the same link (SLIRP's gateway).
 *
 * argv[3] and argv[4] are the two-interface arm and are optional: the guest's
 * address on its SECOND interface, and a host on the same subnet with a
 * listener on port 7805.  Given both, the probe connects from the second
 * address to that host, the case a one-interface guest cannot reach, where
 * the bound address is on one interface and the unconstrained route leaves by
 * the other.  It is the RECEIVER that has to be asked what source it saw; all
 * the guest can say is that the connect was made.
 *
 * The vectors are called by hand at the LVOs docs/RESEARCH.md 3.2 lists, for
 * the reason src/tools/toolsock.c gives: the NDK inlines assume a global
 * SocketBase.
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

/*
 * struct sockaddr_in6, open-coded the same way.  The NDK's is the Linux one:
 * no sin6_len, the family at offset 0 and a byte of padding after it, which is
 * what tests/ipv6/ipv6_socket_test.c already assumes and what
 * src/bsdsocket/addr.c writes.  28 bytes.
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

/*
 * The four vectors above take an address of any family, so each family gets a
 * typed wrapper rather than a cast at every call site.  There is no p_bind6():
 * RFC 6724 selection is what an UNBOUND socket gets, so the IPv6 arm never
 * binds and a wrapper for it would be a function nothing calls.  Nothing else changes:
 * these are the same LVOs with the same registers, and the length is the only
 * thing sockaddr_in and sockaddr_in6 disagree about.
 */

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
 * ONE LINE PER CHECK, key=value, at column zero.
 *
 * `what` is the key and nothing else -- what the check means is in the comment
 * above the call.  run-srcsel.sh matches `<key>=ok` exactly, so a check that
 * is renamed, reworded or removed makes the harness say which key went
 * missing instead of quietly passing on an arm that no longer runs.  It used
 * to match sentences, which meant a sentence could not be improved without
 * breaking the harness and that a near-miss match read as a pass.
 *
 * The failure form carries the detail, usually an errno, in the value.
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

/*
 * An IPv6 address from four longwords, most significant first, written into
 * the 16 network-order bytes of a sockaddr_in6.  The probe's addresses are
 * constants (below), so there is no text parser here and nothing to get wrong
 * between the harness and the guest.
 */
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

/*
 * Send one datagram to a receiver in this process and report the source
 * address it arrived with.  That address is the measurement: a send that
 * returns len proves only that something left.
 */
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
 * THE SOURCE THE STACK PICKS WHEN NOTHING NAMED ONE.
 *
 * Everything above is bind(): the caller named the source and the send has to
 * honour it.  This is the other half, RFC 6724 source address selection, which
 * decides what an UNBOUND socket sends from.  src/ipv6/ipv6_srcsel.c supplies
 * it for the whole stack, and tests/ipv6/host drives its rules one at a time
 * over a hand-built NX_IP; what this arm adds is that the answer on a real
 * machine, through the real library, with a real interface, is the same.
 *
 * getsockname() on a connected socket bound to in6addr_any reports that
 * selection (src/bsdsocket/options.c), so no peer has to answer for any of
 * these: the guest can say the whole thing itself.
 *
 * The addresses are the RFC 3849 documentation prefix and are constants on
 * both sides -- run-srcsel.sh writes SELF6 into the interface file's ADDRESS6
 * and the four here have to agree with it.  Nothing on a LAN answers for
 * 2001:db8::/32, so the arm is inert on the wire.
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

#define LL6_0       0xFE800000UL        /* fe80::1  */

#define PORT_UDP_V6 7806

/*
 * Duplicate address detection has to finish before either address is this
 * node's, and nothing waits for it: AddNetInterface returns as soon as the
 * interface is configured and the addresses are TENTATIVE for a second or so
 * after that.  A TENTATIVE address is correctly not a candidate, so running
 * the arm too early reads as "no route" -- which is exactly the failure the
 * arm is looking for, and would be indistinguishable from it.
 *
 * So the readiness is its own check with its own key, and the arms below run
 * only after it.  Up to fifteen seconds, in half-second steps.
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

    /*
     * A destination on this node's own /64.  Both of the interface's addresses
     * are candidates and Rule 2 takes the global: the link-local has scope 2
     * and the destination has scope 14.
     */
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

    /*
     * A link-local destination.  The same two candidates, and Rule 2 now takes
     * the link-local, because the global has more scope than the destination
     * and the link-local has exactly enough.  Which link-local this node holds
     * is derived from the MAC, so what is asserted is fe80::/10 and not the
     * global -- picking the global here is the defect, and it is what a
     * first-match walk over the address list does when the global happens to
     * be first.
     */
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

    /*
     * ::1.  RFC 6724 4 puts the loopback interface on a link of its own, so
     * the candidate set for a loopback destination holds ::1 and nothing else
     * -- and the interface's addresses are not candidates for it however near
     * they are numerically.
     */
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
     * A global destination on a prefix that is not on-link, with no default
     * router configured.  There is no outgoing interface for it, so there is
     * no source either and the send is refused rather than leaving with an
     * address that cannot carry it back.  This is the refusal the vendored
     * routine made by never considering an address of too small a scope, and
     * it is kept.
     */
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

    /*
     * The refusal that used to be a silent drop: NetX takes the bound
     * interface as a constraint on the route, finds none, and discards the
     * datagram inside _nx_ip_packet_send() with NX_SUCCESS already returned.
     */
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
             * Three, not one.  Two arms below connect to this listener and
             * neither accepts, so each keeps the slot it landed on until the
             * listener is closed.  With a backlog of one the second connect
             * has nowhere to land and spends the whole TCP connect timeout
             * getting there, 191 s of a 300 s run, and reports ETIMEDOUT --
             * which reads exactly like the defect it is testing for.
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
             *
             * A socket bound to nothing has no local address to report, so
             * getsockname() has to name the interface the packets leave by.
             * This one leaves by loopback, and it is the case a
             * single-interface guest can settle: the answer used to be
             * nx_ip_interface[0], the Ethernet address, whatever the peer was.
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
             * No route from the bound address: 127.0.0.1 leaves by loopback
             * and the bind names the interface address, so the source cannot
             * be honoured and the connect is refused before the SYN.
             *
             * ENETUNREACH and not EADDRNOTAVAIL: the bound address exists, it
             * just has no route to the destination.  EADDRNOTAVAIL is now
             * only a bound address the machine does not have, the case that
             * used to be refused, source on one interface and route out of
             * the other, is what source_connect() connects.
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

    /*
     * The pinned path on a physical interface rather than on loopback: the
     * destination is the guest's own interface address, which _nx_ip_route_find()
     * answers with that same interface, so the bind and the route agree and
     * the connect goes through nxd_tcp_client_socket_source_connect().  What
     * the peer end sees is asserted, not just that connect() returned 0.
     */
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

    /*
     * The case the source connect exists for, and the only one that needs a
     * second interface.  alt is an address on it and dest is reachable from
     * both, so the unconstrained route leaves by the first interface while
     * the bind names the second: before nxd_tcp_client_socket_source_connect()
     * this was EADDRNOTAVAIL.  What source the SYN carried is for the
     * receiver to say.
     */
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

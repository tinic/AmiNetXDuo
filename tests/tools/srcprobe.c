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

#define P_AF_INET           2
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

static LONG p_connect(struct Library *base, LONG s, const ProbeAddr *sa)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = s;
    register CONST_APTR      a0  __asm("a0") = (CONST_APTR)sa;
    register LONG            d1  __asm("d1") = (LONG)sizeof(*sa);
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");

    __asm __volatile ("jsr a6@(-54:W)"      /* connect */
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0)
                      : "r" (a6), "r" (d0), "r" (a0), "r" (d1)
                      : "a1", "cc", "memory");
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

static LONG p_getsockname(struct Library *base, LONG s, ProbeAddr *sa)
{
    LONG namelen = (LONG)sizeof(*sa);

    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = s;
    register APTR            a0  __asm("a0") = (APTR)sa;
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

/* --------------------------------------------------------------- checks --- */

static LONG p_checks;
static LONG p_failures;

static BOOL p_check(BOOL ok, const char *what, LONG detail)
{
    p_checks++;

    if (!ok)
    {
        p_failures++;
        Printf((CONST_STRPTR)"  FAIL %s (%ld)\n", (ULONG)what, (ULONG)detail);
        return FALSE;
    }

    Printf((CONST_STRPTR)"  ok   %s\n", (ULONG)what);
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
        Printf((CONST_STRPTR)"SrcProbe: no bsdsocket.library\n");
        return RETURN_FAIL;
    }

    Printf((CONST_STRPTR)"SrcProbe: self 0x%08lx peer 0x%08lx\n", self, peer);

    /* ---- the bound address is the source ------------------------------- */

    p_udp_source(base, self, self, PORT_UDP_LAN,
                 "a datagram bound to the interface address has it as source");
    p_udp_source(base, P_LOOPBACK, P_LOOPBACK, PORT_UDP_LOOP,
                 "a datagram bound to 127.0.0.1 has it as source");

    /* ---- an unbound socket still routes -------------------------------- */

    s = p_socket(base, P_AF_INET, P_SOCK_DGRAM, 0);
    if (s >= 0)
    {
        p_addr(&sa, peer, 9);
        rc = p_sendto(base, s, payload, (LONG)sizeof(payload), &sa);
        (VOID)p_check((BOOL)(rc == (LONG)sizeof(payload)),
                      "an unbound datagram still leaves by the route",
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
        if (p_check((BOOL)(rc == 0), "bind to the interface address",
                    p_errno(base)))
        {
            p_addr(&sa, P_LOOPBACK, PORT_UDP_LOOP);
            rc = p_sendto(base, s, payload, (LONG)sizeof(payload), &sa);
            (VOID)p_check((BOOL)(rc == -1 &&
                                 p_errno(base) == P_ENETUNREACH),
                          "sending to loopback from the interface address is "
                          "ENETUNREACH",
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
        if (p_check((BOOL)(rc == 0), "listener bind to 127.0.0.1",
                    p_errno(base)))
        {
            rc = p_listen(base, listener, 1);
            (VOID)p_check((BOOL)(rc == 0), "listen", p_errno(base));

            /* The bind and the route agree, so the connect goes ahead. */
            s = p_socket(base, P_AF_INET, P_SOCK_STREAM, 0);
            if (s >= 0)
            {
                p_addr(&sa, P_LOOPBACK, 0);
                rc = p_bind(base, s, &sa);
                (VOID)p_check((BOOL)(rc == 0), "client bind to 127.0.0.1",
                              p_errno(base));

                p_addr(&sa, P_LOOPBACK, PORT_TCP_LOOP);
                rc = p_connect(base, s, &sa);
                if (p_check((BOOL)(rc == 0),
                            "connect to 127.0.0.1 from 127.0.0.1",
                            p_errno(base)))
                {
                    name.sin_addr = 0;
                    (VOID)p_getsockname(base, s, &name);
                    (VOID)p_check((BOOL)(name.sin_addr == P_LOOPBACK),
                                  "getsockname reports the bound 127.0.0.1",
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
                              "client bind to the interface address",
                              p_errno(base));

                p_addr(&sa, P_LOOPBACK, PORT_TCP_LOOP);
                rc = p_connect(base, s, &sa);
                (VOID)p_check((BOOL)(rc == -1 &&
                                     p_errno(base) == P_ENETUNREACH),
                              "connect to 127.0.0.1 from the interface "
                              "address is ENETUNREACH",
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
        if (p_check((BOOL)(rc == 0), "listener bind to the interface address",
                    p_errno(base)))
        {
            rc = p_listen(base, listener, 1);
            (VOID)p_check((BOOL)(rc == 0), "listen on the interface address",
                          p_errno(base));

            s = p_socket(base, P_AF_INET, P_SOCK_STREAM, 0);
            if (s >= 0)
            {
                p_addr(&sa, self, 0);
                rc = p_bind(base, s, &sa);
                (VOID)p_check((BOOL)(rc == 0),
                              "client bind to the interface address",
                              p_errno(base));

                p_addr(&sa, self, PORT_TCP_LAN);
                rc = p_connect(base, s, &sa);
                if (p_check((BOOL)(rc == 0),
                            "connect to the interface address from the "
                            "interface address",
                            p_errno(base)))
                {
                    LONG conn;

                    name.sin_addr = 0;
                    conn = p_accept(base, listener, &name);
                    if (p_check((BOOL)(conn >= 0), "the listener accepted it",
                                p_errno(base)))
                    {
                        (VOID)p_check((BOOL)(name.sin_addr == self),
                                      "the accepted peer address is the bound "
                                      "source",
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
                (VOID)p_check((BOOL)(rc == 0),
                              "an unbound connect still leaves by the route",
                              p_errno(base));
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
            if (p_check((BOOL)(rc == 0), "bind to the second interface's "
                        "address", p_errno(base)))
            {
                p_addr(&sa, dest, PORT_TCP_ALT);
                rc = p_connect(base, s, &sa);
                if (p_check((BOOL)(rc == 0),
                            "connect from the second interface's address",
                            p_errno(base)))
                {
                    name.sin_addr = 0;
                    (VOID)p_getsockname(base, s, &name);
                    (VOID)p_check((BOOL)(name.sin_addr == alt),
                                  "getsockname reports the second interface's "
                                  "address",
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
                      "bind to a foreign address is EADDRNOTAVAIL",
                      p_errno(base));
        (VOID)p_close(base, s);
    }

    Printf((CONST_STRPTR)"SrcProbe: %ld checks, %ld failures\n",
           p_checks, p_failures);

    CloseLibrary(base);

    return (p_failures == 0) ? RETURN_OK : RETURN_FAIL;
}

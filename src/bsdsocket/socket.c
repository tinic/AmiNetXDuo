/*
 * bsdsocket.library -- socket lifecycle and the per-opener descriptor table.
 *
 * socket/bind/listen/accept/connect/shutdown/CloseSocket, built on
 * nx_tcp_socket_* and nx_udp_socket_*.
 *
 * The one genuinely awkward mapping is listen/accept. NetX Duo hands an
 * incoming connection to a *named socket* that was parked on the port, and a
 * server keeps listening by handing it a fresh socket afterwards
 * (nx_tcp_server_socket_relisten). BSD instead spawns a new descriptor per
 * connection. So a listening descriptor here keeps a spare "incoming" socket
 * parked on the port; accept() promotes that socket to a descriptor of its
 * own and parks another one.
 *
 * SPDX-License-Identifier: MIT
 */

#include "bsdsocket_vectors.h"
#include "netmonitor.h"

/* For _nx_tcp_packet_send_fin() -- see bsd_tcp_send_fin() below. */
#include "nx_tcp.h"

#include "aminetxduo/random.h"

#include <proto/exec.h>

/*
 * How many datagrams a UDP socket may hold un-read.
 *
 * This is a pool-budget decision, not a tuning knob. A queued datagram is a
 * whole NX_PACKET out of the shared pool for as long as the application does
 * not read it, and the pool is the stack's only buffer: 16..256 packets of
 * 1568 bytes, sized from AvailMem() at startup (netstack.h). Everything else
 * that can pin packets is already budgeted the same way -- the SANA-II
 * readers keep up to 8 outstanding CMD_READs, and NX_TCP_MAXIMUM_TX_QUEUE
 * caps a TCP socket at 8 in flight.
 *
 * A flat 8 was the old value, and it is far too tight on anything but the
 * floor: a sender that bursts 200 datagrams into loopback before the receiver
 * runs loses 192 of them. A flat large value is worse -- one socket nobody is
 * reading would drain the pool and the IP thread could not allocate to
 * receive or transmit anything at all, which is a machine-wide failure caused
 * by one careless application.
 *
 * So: a quarter of the pool, never fewer than the 8 we had, never more than
 * 64. On the 4 MB floor (16 packets) that is 8, exactly as before; on a
 * full 256-packet pool it is 64. Three sockets asleep on full queues still
 * leave a quarter of the pool for the stack.
 */
#define BSD_UDP_QUEUE_MIN       8
#define BSD_UDP_QUEUE_CEILING   64
#define BSD_UDP_POOL_SHARE      4       /* 1/N of the pool per socket       */

static char bsd_tcp_name[] = "AmiNetXDuo TCP";
static char bsd_udp_name[] = "AmiNetXDuo UDP";

/*
 * shutdown(SHUT_WR): send a FIN and keep receiving.
 *
 * nx_tcp_socket_disconnect() cannot express this, in either of its two modes.
 * With NX_NO_WAIT it sends a RESET (nx_tcp_socket_disconnect.c, the
 * !NX_DISABLE_RESET_DISCONNECT branch) -- so the peer's queued data is thrown
 * away and its next send() fails; with a wait option it sends a FIN and then,
 * when the wait expires without the peer also closing, calls
 * _nx_tcp_socket_block_cleanup() and tears the connection down anyway.  Either
 * way the half of the connection the application asked to KEEP is destroyed.
 *
 * The TCP state machine itself is perfectly capable of it:
 * nx_tcp_socket_packet_process.c calls _nx_tcp_socket_state_data_check() in
 * both FIN_WAIT_1 and FIN_WAIT_2, so a socket that has sent its FIN goes on
 * queuing received data normally.  Only the disconnect *API* is unable to
 * stop there.  So this open-codes the graceful branch of
 * _nx_tcp_socket_disconnect() -- state change, FIN timeout, sequence bump,
 * FIN -- and stops before the suspension and the cleanup that follow it.
 *
 * This matters for nc -N, for telnet, and for every ftp data connection: the
 * whole point of half-close is "I have finished sending, now tell me the
 * rest", and a RESET answers that with silence.
 *
 * The caller holds the ThreadX baton (bsd_nx_enter), so no other ThreadX
 * thread -- the IP thread included -- can be inside the socket while this
 * runs; the IP protection mutex is taken as well because that is what
 * _nx_tcp_packet_send_fin() is called under everywhere else.
 */
static VOID bsd_tcp_send_fin(AmiSocket *sock)
{
    NX_TCP_SOCKET *tcp = &sock->as_Nx.tcp;
    NX_IP         *ip  = tcp->nx_tcp_socket_ip_ptr;

    if (ip == NX_NULL)
        return;

    tx_mutex_get(&ip->nx_ip_protection, TX_WAIT_FOREVER);

    if (tcp->nx_tcp_socket_state == NX_TCP_ESTABLISHED)
    {
        tcp->nx_tcp_socket_state = NX_TCP_FIN_WAIT_1;
    }
    else if (tcp->nx_tcp_socket_state == NX_TCP_CLOSE_WAIT)
    {
        /* The peer closed first, so our FIN is the last one: LAST_ACK. */
        tcp->nx_tcp_socket_state = NX_TCP_LAST_ACK;
    }
    else
    {
        /* Already closing, or never established. Nothing to send. */
        tx_mutex_put(&ip->nx_ip_protection);
        return;
    }

    /* The FIN timeout is armed here only if nothing is still unacknowledged;
       otherwise the transmit path arms it when the queue drains. */
    if (tcp->nx_tcp_socket_transmit_sent_head == NX_NULL)
    {
        tcp->nx_tcp_socket_timeout         = tcp->nx_tcp_socket_timeout_rate;
        tcp->nx_tcp_socket_timeout_retries = 0;
    }

    tcp->nx_tcp_socket_tx_sequence++;
    _nx_tcp_packet_send_fin(tcp, tcp->nx_tcp_socket_tx_sequence - 1);

    tx_mutex_put(&ip->nx_ip_protection);
}

static ULONG bsd_udp_queue_max(VOID)
{
    NX_PACKET_POOL *pool = netstack_pool();
    ULONG           queue;

    if (pool == NULL)
        return BSD_UDP_QUEUE_MIN;

    queue = pool->nx_packet_pool_total / BSD_UDP_POOL_SHARE;

    if (queue < BSD_UDP_QUEUE_MIN)
        queue = BSD_UDP_QUEUE_MIN;
    if (queue > BSD_UDP_QUEUE_CEILING)
        queue = BSD_UDP_QUEUE_CEILING;

    return queue;
}

/*
 * How large a receive window THIS machine can afford to advertise right now.
 *
 * WHY IT CANNOT BE A CONSTANT.  The window is what a bulk transfer is limited
 * by: on loopback the sender held exactly one segment in flight against the
 * whole advertised window, advertised a zero window 64 times in a 128-segment
 * transfer, and waited 22 ms (median) between segments with nothing it was
 * allowed to do.  Sizing the window here instead moves that path 230 -> 283
 * KB/s with nothing else changed.  It also cannot simply BE the ceiling,
 * because a TCP socket's receive queue is packets off the same pool
 * the SANA-II readers pin an eighth of, and forty concurrent sockets each
 * promising 32 KB promise several times the whole pool.  Exhausting it drops
 * frames stack-wide, which is a functional failure and not a slowdown.
 *
 * WHY THE SOCKET COUNT AND NOT JUST THE POOL.  AMI_SANA2_RX_DEPTH_IPV4 could
 * be settled from the pool alone because the number of things drawing on it
 * -- two or three reader threads -- is fixed and known when the stack starts.
 * Here it is not: the consumer count is one socket for a bulk transfer and
 * forty for `tests/curl` d03_parallel_40, and a number chosen at stack start
 * would have to assume the worst.  Assuming forty gives every socket the
 * floor and throws the whole gain away.  So the pool sets a BUDGET and the
 * live socket count divides it.
 *
 * THE FLOOR IS THE STATUS QUO, deliberately.  8192 is what every socket got
 * before this function existed, and forty concurrent transfers were measured
 * passing on it.  So a socket can never come out of here with less than a
 * configuration that is known to work, and everything above the floor is an
 * over-commitment that the budget bounds: granting budget/n to the n'th
 * socket, clamped, the excess over the floor summed across every socket comes
 * to about 1.1x the budget -- 56,370 bytes, 36 of 256 packets, on the 8 MB
 * profile.  That is the same order as the eighth the SANA-II readers take,
 * and it is why the share here is also an eighth.
 *
 * WHY AT CREATE TIME AND NOT CONTINUOUSLY.  A window that tracked the socket
 * count would have to shrink an established socket's advertised window, and
 * NetX Duo has no supported way to do that: nx_tcp_socket_rx_window_current
 * is unsigned and derived from the default by subtraction, so lowering the
 * default under a socket with data queued underflows it, and retracting the
 * right edge of a window the peer is already filling is the one thing RFC 793
 * says not to do.  Create time is the only safe point, and it is enough,
 * because the two workloads that matter are distinguishable there: a bulk
 * transfer opens one or two sockets and a concurrent client opens forty.
 *
 * The pool's FREE count was considered instead of the socket count and is
 * worse: curl's multi interface opens all forty sockets before any of them
 * carries data, so the pool is still full at the moment every one of them
 * would be sized.
 *
 * The counter is NetX Duo's own, maintained by nx_tcp_socket_create/delete,
 * so there is none here to leak -- and a leaked one would pin every future
 * socket at the floor while looking like nothing at all.  It counts listeners
 * and parked spares, which never carry a connection: that over-counts
 * consumers, which is the direction to be wrong in.  The socket being created
 * is not on the list yet, hence the +1.
 */
ULONG ami_bsd_tcp_window(VOID)
{
    static ULONG    last_budget = 0;
    NX_PACKET_POOL *pool = netstack_pool();
    NX_IP          *ip   = netstack_ip();
    ULONG           budget;
    ULONG           window;

    if (pool == NULL || ip == NULL)
        return (ULONG)BSD_TCP_WINDOW;

    budget = (pool->nx_packet_pool_total / (ULONG)BSD_TCP_WINDOW_POOL_SHARE) *
             pool->nx_packet_pool_payload_size;

    if (budget != last_budget)
    {
        last_budget = budget;
        AMI_INFO("bsdsocket: TCP window budget %ld bytes (pool %ld packets), "
                 "%ld..%ld per socket",
                 (long)budget, (long)pool->nx_packet_pool_total,
                 (long)BSD_TCP_WINDOW, (long)BSD_TCP_WINDOW_CEILING);
    }

    window = budget / (ip->nx_ip_tcp_created_sockets_count + 1UL);

    /* Floor last, so AMINETXDUO_TCP_WINDOW -- which sets both -- pins every
       socket at it whichever side of the built-in pair it falls. */
    if (window > (ULONG)BSD_TCP_WINDOW_CEILING)
        window = (ULONG)BSD_TCP_WINDOW_CEILING;
    if (window < (ULONG)BSD_TCP_WINDOW)
        window = (ULONG)BSD_TCP_WINDOW;

    return window;
}

/* ------------------------------------------------- initial sequence number */

/*
 * NetX Duo'S ISN IS BIASED, NOT ABSENT -- AND THE BIAS IS WORTH 9 BITS.
 *
 * It does randomise, and the claim that it does not is wrong.  The code is
 * nxd_tcp_client_socket_connect.c:411 and nx_tcp_server_socket_accept.c:106,
 * and both read:
 *
 *     if (socket -> nx_tcp_socket_tx_sequence == 0)
 *     {
 *         socket -> nx_tcp_socket_tx_sequence  = ((ULONG)NX_RAND()) << 16;
 *         socket -> nx_tcp_socket_tx_sequence |=  (ULONG)NX_RAND();
 *     }
 *     else
 *         socket -> nx_tcp_socket_tx_sequence += 0x10000 + (ULONG)NX_RAND();
 *
 * NX_RAND() is ami_random_rand() here -- a SHA-256 hash DRBG over an entropy
 * pool (src/common/ami_random.c) -- so the generator is not the problem.  The
 * COMBINING STEP is.  `|` is a bitwise OR of two independent draws, and
 * rand() is specified to return 0..0x7FFFFFFF, so:
 *
 *   bits  0..15   come from the second draw alone           -- uniform
 *   bits 16..30   are (first draw) OR (second draw)         -- 1 with
 *                                                              probability 3/4
 *   bit     31    is the first draw's bit 15 alone (the second draw's bit 31
 *                 is always zero)                           -- uniform
 *
 * Fifteen of the thirty-two bits are therefore three-quarters ones.  That is
 * 29.2 bits of Shannon entropy, and -- the number that matters for guessing --
 * a MIN-ENTROPY OF 23.2 BITS: the single likeliest ISN comes up 438 times more
 * often than it would under a uniform distribution, and an attacker who tries
 * the dense-upper-half values first faces 2^23 rather than 2^32.
 *
 * It is visible in a packet trace, which is how it was confirmed rather than
 * argued: across the SYNs captured in docs/RESEARCH.md 28.4, bits 16..30 were
 * set in 35 of 45 places before this function existed (0.78, against the 0.75
 * the expression predicts) and 69 of 135 after it (0.51, against 0.50).
 *
 * WHY IT MATTERS.  Blind injection into an established connection needs a
 * sequence number inside the receive window; off-path, it also needs the
 * ephemeral port.  Nine bits off the sequence space is not by itself a break,
 * but it is nine bits an attacker gets for free, and predictable sequence
 * numbers are the ingredient every off-path TCP attack since Morris has
 * needed.
 *
 * THE FIX WITHOUT PATCHING third_party/.  The `else` branch above is a
 * supported path rather than a fallback: it is what every REUSED socket takes.
 * Seeding tx_sequence at create time makes a fresh socket take the branch a
 * reused one takes, and that branch ADDS rather than ORs, so with a full
 * 32-bit seed the result is uniform over the whole space.  No vendored file
 * changes, no symbol override, no linker wrapper.
 *
 * WHAT THIS IS NOT.  It is not RFC 6528.  6528 computes
 * M + F(local addr, local port, remote addr, remote port, secret); the
 * four-tuple hash is there so a new connection on a RECENTLY USED four-tuple
 * gets an ISN above the old one, which is what makes TIME-WAIT recycling safe.
 * What this produces is a purely random ISN -- RFC 793 / RFC 1948 against
 * prediction, and silent about recycling, which the 2 MSL timer already
 * answers.  6528 proper is not reachable from here in any case: NetX Duo picks
 * tx_sequence inside connect(), and at create time there is no peer to hash.
 *
 * Cost: one DRBG draw per TCP socket created, at create time, off every hot
 * path.
 */
/*
 * SO_KEEPALIVE IS OFF UNTIL THE APPLICATION ASKS, AND NetX Duo DOES NOT AGREE.
 *
 * nx_tcp_socket_create.c:166 sets nx_tcp_socket_keepalive_enabled = NX_TRUE
 * unconditionally when NX_ENABLE_TCP_KEEPALIVE is defined, so defining it alone
 * would put every socket in the machine on a two-hour keepalive timer whether
 * anything asked or not.  That is not what SO_KEEPALIVE means: 4.4BSD, POSIX
 * and every stack since have it default OFF, and a program that never sets it
 * has a right to expect an idle connection to stay quiet.
 *
 * So the create path clears it and setsockopt(SO_KEEPALIVE) is the only thing
 * that sets it (src/bsdsocket/options.c).  Called on the three sockets this
 * library creates, next to the ISN seed, because both are "make the socket the
 * shape the BSD API promises before anyone can use it".
 */
static VOID bsd_tcp_keepalive_default(NX_TCP_SOCKET *tcp)
{
#ifdef NX_ENABLE_TCP_KEEPALIVE
    tcp->nx_tcp_socket_keepalive_enabled = NX_FALSE;
#else
    (VOID)tcp;
#endif
}

static VOID bsd_tcp_seed_isn(NX_TCP_SOCKET *tcp)
{
    ULONG seed = ami_random_ulong();

    /*
     * Zero is the one value that means "not seeded" to the code above, so it
     * must not be handed back: a zero draw would silently restore the biased
     * branch.  One draw in 2^32, and it costs a comparison.
     */
    if (seed == 0)
        seed = 1;

    tcp->nx_tcp_socket_tx_sequence = seed;
}

/* --------------------------------------------------------- descriptor table */

static LONG bsd_table_ensure(struct AmiSocketBase *base)
{
    if (base->sb_Table != NULL)
        return 0;

    base->sb_Table = (AmiSocket **)ami_alloc(
        (ULONG)BSD_DEFAULT_DTABLESIZE * sizeof(AmiSocket *));
    if (base->sb_Table == NULL)
        return -1;

    base->sb_TableSize = BSD_DEFAULT_DTABLESIZE;

    return 0;
}

LONG bsd_table_resize(struct AmiSocketBase *base, LONG size)
{
    AmiSocket **table;
    LONG        i, copy;

    if (size < 1 || size > BSD_MAX_DTABLESIZE)
        return -1;

    if (bsd_table_ensure(base) != 0)
        return -1;

    if (size == base->sb_TableSize)
        return 0;

    /* Never shrink below the highest descriptor in use. */
    for (i = base->sb_TableSize - 1; i >= size; i--)
    {
        if (base->sb_Table[i] != NULL)
            return -1;
    }

    table = (AmiSocket **)ami_alloc((ULONG)size * sizeof(AmiSocket *));
    if (table == NULL)
        return -1;

    copy = (size < base->sb_TableSize) ? size : base->sb_TableSize;
    for (i = 0; i < copy; i++)
        table[i] = base->sb_Table[i];

    ami_free(base->sb_Table);
    base->sb_Table     = table;
    base->sb_TableSize = size;

    return 0;
}

AmiSocket *bsd_lookup(struct AmiSocketBase *base, LONG fd)
{
    if (base->sb_Table == NULL || fd < 0 || fd >= base->sb_TableSize)
        return NULL;

    return base->sb_Table[fd];
}

LONG bsd_fd_alloc(struct AmiSocketBase *base, AmiSocket *sock)
{
    LONG fd;

    if (bsd_table_ensure(base) != 0)
        return -1;

    for (fd = 0; fd < base->sb_TableSize; fd++)
    {
        if (base->sb_Table[fd] == NULL)
        {
            base->sb_Table[fd] = sock;
            return fd;
        }
    }

    return -1;
}

VOID bsd_fd_free(struct AmiSocketBase *base, LONG fd)
{
    if (base->sb_Table != NULL && fd >= 0 && fd < base->sb_TableSize)
        base->sb_Table[fd] = NULL;
}

/* ----------------------------------------------------------- socket objects */

static AmiSocket *bsd_socket_alloc(struct AmiSocketBase *base,
                                   UWORD domain, UWORD type, LONG protocol)
{
    AmiSocket *sock = (AmiSocket *)ami_alloc(sizeof(AmiSocket));

    if (sock == NULL)
        return NULL;

    sock->as_Owner    = base;
    sock->as_RefCount = 1;
    sock->as_Domain   = domain;
    sock->as_Type     = type;
    sock->as_Protocol = protocol;
    sock->as_Ttl      = (LONG)NX_IP_TIME_TO_LIVE;

    switch (type)
    {
        case SOCK_STREAM: sock->as_Flags = ASF_TCP; break;
        case SOCK_RAW:    sock->as_Flags = ASF_RAW; break;
        default:          sock->as_Flags = ASF_UDP; break;
    }

    /*
     * The version tag has to be set even though ami_alloc() cleared the block:
     * a cleared NXD_ADDRESS reads as version 0, not NX_IP_VERSION_V4 (4), and
     * every "is this address unset" test in options.c and transfer.c asks the
     * version first. A zero there is not "IPv4 0.0.0.0", it is "no family",
     * and getsockname() on a connected socket bound to INADDR_ANY would stop
     * reporting the interface address.
     */
    bsd_addr_from_v4(&sock->as_LocalAddr, 0UL);
    bsd_addr_from_v4(&sock->as_PeerAddr, 0UL);

#ifdef AMINETXDUO_IPV6
    if (domain == AF_INET6)
    {
        sock->as_Flags |= ASF_INET6;

        sock->as_LocalAddr.nxd_ip_version = NX_IP_VERSION_V6;
        sock->as_PeerAddr.nxd_ip_version  = NX_IP_VERSION_V6;

        /*
         * IPV6_V6ONLY DEFAULTS TO OFF, i.e. an AF_INET6 socket is dual-stack.
         *
         * This is not a copy of anyone's default -- Linux says off, modern
         * BSD says on -- it follows from what NetX Duo is. Its port tables are
         * family-agnostic: nx_tcp_server_socket_listen() registers a listen on
         * a PORT, and the SYN that arrives for it may be v4 or v6. There is no
         * arrangement under which an AF_INET6 socket and an AF_INET socket can
         * both hold port 80 here, so the "V6ONLY on lets you run separate v4
         * and v6 servers" argument for defaulting it on does not apply, while
         * the "one socket serves both" behaviour it would block is the only
         * one available.
         *
         * Setting it to 1 is still honoured -- see options.c -- and then a v4
         * peer is refused rather than reported as ::ffff:a.b.c.d.
         */
    }
#endif

    return sock;
}

/* --------------------------------------------------------- orderly close --
 *
 * RFC 793 3.5: CLOSE IS A FIN.
 *
 * It was a RESET here, on every connection, in every capture this project has
 * ever taken -- docs/RESEARCH.md 12.3 called it a risk, 16.9 an observation
 * and 27.6 an assertion with the packet next to it. A RESET tells the peer to
 * throw away everything it has not yet handed to its application, so a server
 * that writes a reply and closes could destroy the reply; and it denied every
 * peer the orderly teardown the protocol promises.
 *
 * The reason it was a RESET is that nx_tcp_socket_disconnect() offers two
 * behaviours and neither is close(). NX_NO_WAIT sends a RESET and returns;
 * anything else sends a FIN and then SUSPENDS THE CALLER until the peer
 * answers or the wait expires. A blocking CloseSocket() is not acceptable --
 * the descriptor is gone the instant the call is made and an application that
 * closes and exits must not wait on a peer that may never answer -- so the
 * RESET was what was left.
 *
 * WHAT THIS DOES INSTEAD
 *
 * The FIN goes out through the same open-coded path shutdown(SHUT_WR) uses
 * (bsd_tcp_send_fin, above), which is already proven -- tcpdrill c04 asserts
 * the FIN|ACK and the ACK that comes back. CloseSocket() then returns, and the
 * AmiSocket is parked on a list until TCP has finished with the connection:
 * NetX Duo's fast periodic drives FIN_WAIT_1 -> FIN_WAIT_2 -> TIMED_WAIT and
 * LAST_ACK -> CLOSED by itself, retransmits the FIN, and gives up with a reset
 * after NX_TCP_MAXIMUM_RETRIES. So the state machine needs nothing from us
 * except that the control block stay alive, which is exactly what the list is
 * for -- nx_tcp_socket_delete() refuses anything that is not CLOSED, and
 * deleting nothing is the AmiSocket-per-connection leak docs/RESEARCH.md 12.5
 * predicted.
 *
 * WHEN IT IS STILL A RESET, AND BOTH ARE THE RULE RATHER THAN AN ESCAPE
 *
 *   - Data arrived that the application never read. RFC 1122 4.2.2.13 is
 *     explicit: the peer must not be told its data was delivered when it is
 *     about to be discarded, so this close is an abort. Every BSD does this.
 *   - SO_LINGER on with a zero timeout, which is the documented way to ask
 *     for an abortive close and is what the option is mostly used for.
 *
 * AND WHEN IT BLOCKS: SO_LINGER on with a nonzero timeout, which is the
 * documented way to ask for that. nx_tcp_socket_disconnect() with a wait is
 * precisely those semantics, including the tear-down when the wait expires,
 * so the option finally does what its name says instead of selecting between
 * two flavours of reset.
 *
 * TIME_WAIT, HONESTLY
 *
 * A socket that closes first reaches TIMED_WAIT and NetX Duo holds it there
 * for 2MSL, which its own defaults make 240 seconds. This does not wait that
 * long: the sweep below reclaims a socket as soon as it reaches TIMED_WAIT,
 * and nx_tcp_client_socket_unbind() collapses the state on the way out --
 * which is what NetX Duo itself does whenever an application unbinds, and
 * what this library has always done. Four minutes of an AmiSocket and an
 * ephemeral port per closed connection is not affordable on the 4 MB floor,
 * and the exposure it buys -- an old duplicate landing on a reused port -- is
 * bounded by NetX Duo allocating ephemeral ports in ascending order.
 */

#define BSD_CLOSING_DEADLINE    (60UL * NX_IP_PERIODIC_RATE)

/*
 * Global, not per base, and that is the point: the closing connection has to
 * outlive the base, because CloseLibrary() is very often the next thing that
 * happens. Every access is inside a bsd_nx_enter() bracket, which is the
 * ThreadX baton -- one holder at a time across every base -- so the list needs
 * no lock of its own.
 */
static AmiSocket *bsd_closing_head;

static BOOL bsd_socket_destroy(AmiSocket *sock);

/*
 * Data arriving on a connection the application has closed.
 *
 * There is nobody left to read it, so acknowledging it would be a lie: the
 * peer would be told its data was delivered and then never hear about it
 * again. 4.4BSD's tcp_input answers this with a reset --
 *
 *     if (so->so_state & SS_NOFDREF && tp->t_state > TCPS_CLOSE_WAIT && tlen)
 *             tp = tcp_drop(tp, ECONNRESET);
 *
 * -- and it is the same rule as RFC 1122 4.2.2.13's "close with unread data",
 * one segment later. Without it a closed socket in FIN_WAIT_2 goes on
 * acknowledging for as long as the peer keeps writing, which is what made
 * tests/clients' "send() to a closed peer eventually fails" stop failing.
 *
 * THE TEARDOWN IS NOT DONE HERE. This runs from inside
 * _nx_tcp_socket_state_data_check(), which has more to do with both the socket
 * and the packet after it returns; tearing the control block down underneath it
 * is not something to do for a corner case. Sending the RST is safe -- it only
 * builds and transmits a packet -- and the socket is then given an expired
 * timeout, so NetX Duo's own fast periodic reaches _nx_tcp_socket_connection_
 * reset() on the next tick, from the top of the IP thread and with nothing in
 * flight. bsd_closing_sweep() collects it after that.
 */
static VOID bsd_closing_data_notify(NX_TCP_SOCKET *tcp)
{
    NX_TCP_HEADER header;

    /* A bare FIN queues nothing and must not be answered this way; only real
       data that nobody can read is a reset. */
    if (tcp->nx_tcp_socket_receive_queue_count == 0)
        return;

    /* The fake-header idiom nx_tcp_socket_disconnect.c uses for the same
       call: word_3 carries only the flag that says this is not a real
       received header. */
    header.nx_tcp_header_word_3         = NX_TCP_ACK_BIT;
    header.nx_tcp_acknowledgment_number = tcp->nx_tcp_socket_tx_sequence;
    header.nx_tcp_sequence_number       = tcp->nx_tcp_socket_rx_sequence;
    _nx_tcp_packet_send_rst(tcp, &header);

    tcp->nx_tcp_socket_timeout_retries = tcp->nx_tcp_socket_timeout_max_retries;
    tcp->nx_tcp_socket_timeout         = 1;
}

static VOID bsd_closing_park(AmiSocket *sock)
{
    sock->as_Flags |= ASF_CLOSING;

    /* See above: from here on, data is a reset rather than an acknowledgement. */
    nx_tcp_socket_receive_notify(&sock->as_Nx.tcp, bsd_closing_data_notify);

    /*
     * Nothing may point at the caller any more. The base is about to be freed
     * in the common case, and NetX Duo's receive/disconnect callbacks find
     * their AmiSocket through this pointer -- select.c returns immediately on
     * a NULL one, which is the disarm.
     */
    sock->as_Nx.tcp.nx_tcp_socket_reserved_ptr = NX_NULL;
    sock->as_Owner = NULL;

    sock->as_ClosingAt   = tx_time_get();
    sock->as_ClosingNext = bsd_closing_head;
    bsd_closing_head     = sock;
}

/*
 * Force a TCP socket into a state nx_tcp_socket_delete() will accept.
 *
 * THIS IS NOT A SECOND WAY TO CLOSE A SOCKET. It is what is left when the
 * ordinary one has already been tried and NetX Duo still holds the block.
 *
 * nx_tcp_socket_disconnect() does NOTHING AT ALL for a socket that is not in
 * ESTABLISHED, SYN_SENT, SYN_RECEIVED or CLOSE_WAIT: it returns
 * NX_NOT_CONNECTED at nx_tcp_socket_disconnect.c:106 before touching a field.
 * Those four exclude every state a half-closed connection actually sits in --
 * FIN_WAIT_1, FIN_WAIT_2, CLOSING, TIMED_WAIT, LAST_ACK -- so the sweep's
 * "resetting" below was a no-op for precisely the case it was written for,
 * and the socket then failed nx_tcp_socket_delete()'s "state must be CLOSED"
 * test and was leaked. docs/RESEARCH.md 40 measures 33 of them in 66 seconds.
 *
 * Presenting the socket as ESTABLISHED is what makes NetX Duo run its own
 * abort on it: one RST built from the socket's real sequence numbers, then
 * _nx_tcp_socket_block_cleanup() to release the transmit queue and the
 * timers, leaving CLOSED (a client socket) or LISTEN (a server one). Writing
 * the field is safe here because bsd_nx_enter() holds the ThreadX baton for
 * the whole of the caller, so the IP thread is not running.
 */
static VOID bsd_tcp_abort(NX_TCP_SOCKET *tcp)
{
    UINT state = tcp->nx_tcp_socket_state;

    if (state == NX_TCP_CLOSED || state == NX_TCP_LISTEN_STATE)
        return;

    if (state != NX_TCP_ESTABLISHED && state != NX_TCP_SYN_SENT &&
        state != NX_TCP_SYN_RECEIVED && state != NX_TCP_CLOSE_WAIT)
        tcp->nx_tcp_socket_state = NX_TCP_ESTABLISHED;

    nx_tcp_socket_disconnect(tcp, NX_NO_WAIT);
}

VOID bsd_closing_sweep(VOID)
{
    AmiSocket  *sock;
    AmiSocket **link = &bsd_closing_head;
    ULONG       now  = tx_time_get();

    while ((sock = *link) != NULL)
    {
        UINT state = sock->as_Nx.tcp.nx_tcp_socket_state;
        /*
         * LISTEN is a finished close, not an unfinished one: a SERVER socket
         * that completes its shutdown lands there rather than in CLOSED --
         * _nx_tcp_socket_block_cleanup() branches on
         * nx_tcp_socket_client_type. Without it every accepted socket waited
         * out the full deadline before anyone looked at it again.
         */
        BOOL done  = (state == NX_TCP_CLOSED) || (state == NX_TCP_TIMED_WAIT) ||
                     (state == NX_TCP_LISTEN_STATE);
        /* Unreadable data is a reset (see bsd_closing_data_notify); this is
           the same rule for anything that got queued before the notify was
           installed, or while the socket was between states. */
        BOOL late  = ((ULONG)(now - sock->as_ClosingAt) >= BSD_CLOSING_DEADLINE) ||
                     (sock->as_Nx.tcp.nx_tcp_socket_receive_queue_count != 0);

        if (!done && !late)
        {
            link = &sock->as_ClosingNext;
            continue;
        }

        *link = sock->as_ClosingNext;
        sock->as_ClosingNext = NULL;

        /*
         * Past the deadline and still not finished means the peer has stopped
         * answering and NetX Duo's own retry limit has not fired either --
         * a half-open connection. Reset it rather than hold the block for
         * ever; this is the only path here that emits a RESET after the FIN.
         */
        if (!done)
        {
            /* Not a warning when it is the data rule: that is a peer still
               writing to a socket nobody owns, which is ordinary. */
            if (sock->as_Nx.tcp.nx_tcp_socket_receive_queue_count == 0)
                AMI_WARN("bsdsocket: close did not complete in %ld s "
                         "(state %ld); resetting",
                         (long)(BSD_CLOSING_DEADLINE / NX_IP_PERIODIC_RATE),
                         (long)state);

            bsd_tcp_abort(&sock->as_Nx.tcp);
        }

        if (bsd_socket_destroy(sock))
            ami_free(sock);
    }
}

/*
 * Start the close. TRUE means the socket is finished with and the caller may
 * delete it now; FALSE means the FIN is in flight and the block has been
 * parked, so it is no longer the caller's to free.
 */
static BOOL bsd_tcp_close_start(AmiSocket *sock)
{
    NX_TCP_SOCKET *tcp   = &sock->as_Nx.tcp;
    UINT           state = tcp->nx_tcp_socket_state;

    /* A handshake that never finished has nothing to close down gracefully;
       disconnect() winds SYN_SENT/SYN_RECEIVED back without sending a RESET
       (nx_tcp_socket_disconnect.c takes that branch before the NX_NO_WAIT
       one). */
    if (state == NX_TCP_SYN_SENT || state == NX_TCP_SYN_RECEIVED)
    {
        nx_tcp_socket_disconnect(tcp, NX_NO_WAIT);
        return TRUE;
    }

    /* RFC 1122 4.2.2.13: unread data turns a close into an abort. */
    if (sock->as_RxPending != NULL || tcp->nx_tcp_socket_receive_queue_count != 0)
    {
        nx_tcp_socket_disconnect(tcp, NX_NO_WAIT);
        return TRUE;
    }

    /* SO_LINGER, l_linger == 0: the documented abortive close. */
    if (sock->as_LingerOn != 0 && sock->as_LingerTime == 0)
    {
        nx_tcp_socket_disconnect(tcp, NX_NO_WAIT);
        return TRUE;
    }

    /* SO_LINGER, l_linger > 0: block until the peer answers or the time is
       up, which is what NetX Duo's waiting disconnect already is. */
    if (sock->as_LingerOn != 0)
    {
        nx_tcp_socket_disconnect(tcp, (ULONG)sock->as_LingerTime *
                                          NX_IP_PERIODIC_RATE);
        return TRUE;
    }

    /* The default. bsd_tcp_send_fin() does nothing if the FIN has already
       gone -- a close after shutdown(SHUT_WR) -- which is correct, and the
       state test below then parks the socket exactly as it would have. */
    bsd_tcp_send_fin(sock);

    state = tcp->nx_tcp_socket_state;

    if (state == NX_TCP_CLOSED || state == NX_TCP_LISTEN_STATE)
        return TRUE;

    bsd_closing_park(sock);

    return FALSE;
}

/*
 * Tear the NetX Duo socket down; safe to call more than once.
 *
 * Returns TRUE only when NetX Duo has actually let go of the control block --
 * i.e. when the caller may hand the memory back to the allocator. This is not
 * bookkeeping pedantry: nx_tcp_socket_delete() links every live socket into
 * ip->nx_ip_tcp_created_sockets_ptr, and freeing a socket that is still on
 * that list means the next nx_tcp_socket_create() walks a freed-and-reused
 * address. It reports NX_PTR_ERROR (EFAULT) and TCP socket() is dead for the
 * lifetime of the process.
 */
static BOOL bsd_socket_destroy(AmiSocket *sock)
{
    UINT status;

    if ((sock->as_Flags & ASF_ORPHANED) != 0)
        return FALSE;                   /* already known to be un-deletable */

    if ((sock->as_Flags & ASF_DELETED) != 0)
        return TRUE;

    /*
     * The FIN goes out before anything is discarded, because whether there is
     * anything to discard is part of the decision -- see bsd_tcp_close_start().
     * ASF_CLOSING means this is the sweep coming back for a socket it already
     * closed, so the close is not started twice.
     */
    if ((sock->as_Flags & (ASF_TCP | ASF_RAW | ASF_CLOSING)) == ASF_TCP &&
        (sock->as_Flags & (ASF_CONNECTED | ASF_CONNECTING)) != 0)
    {
        if (!bsd_tcp_close_start(sock))
            return FALSE;
    }

    if (sock->as_RxPending != NULL)
    {
        nx_packet_release(sock->as_RxPending);
        sock->as_RxPending = NULL;
    }

    if ((sock->as_Flags & ASF_RAW) != 0)
    {
        /* No NetX Duo object to delete: raw.c owns the queue and the filter
           registration, and both go here. */
        bsd_raw_close(sock);
        sock->as_Flags |= ASF_DELETED;

        return TRUE;
    }

    if ((sock->as_Flags & ASF_TCP) != 0)
    {
        /*
         * Give the port back. A socket that came off a listen port is
         * released with unaccept() whether or not it was ever accepted --
         * nx_tcp_client_socket_unbind() does not know about listen state, and
         * leaving it bound makes the delete below return NX_STILL_BOUND.
         */
        if ((sock->as_Flags & ASF_SERVER) != 0)
            nx_tcp_server_socket_unaccept(&sock->as_Nx.tcp);
        else if ((sock->as_Flags & ASF_NXBOUND) != 0)
            nx_tcp_client_socket_unbind(&sock->as_Nx.tcp);

        status = nx_tcp_socket_delete(&sock->as_Nx.tcp);

        /*
         * NX_STILL_BOUND means the flags said this socket was finished with
         * and NetX Duo disagrees -- it is on a port list, or its state is not
         * CLOSED (nx_tcp_socket_delete.c:94). Leaking the block is the safe
         * answer but it is a permanent one, so abort the connection outright
         * and ask once more. The unbind is unconditional this time because
         * ASF_NXBOUND is the library's belief and the port list is the fact.
         */
        if (status == NX_STILL_BOUND)
        {
            bsd_tcp_abort(&sock->as_Nx.tcp);

            if ((sock->as_Flags & ASF_SERVER) != 0)
                nx_tcp_server_socket_unaccept(&sock->as_Nx.tcp);
            else
                nx_tcp_client_socket_unbind(&sock->as_Nx.tcp);

            status = nx_tcp_socket_delete(&sock->as_Nx.tcp);
        }
    }
    else
    {
        if ((sock->as_Flags & ASF_NXBOUND) != 0)
            nx_udp_socket_unbind(&sock->as_Nx.udp);

        status = nx_udp_socket_delete(&sock->as_Nx.udp);
    }

    /* NX_NOT_CREATED means it was never on the created list, so the memory is
       ours either way. Anything else means NetX Duo still points at it. */
    if (status != NX_SUCCESS && status != NX_NOT_CREATED)
    {
        /*
         * The TCP state and the socket flags are in the message because
         * without them this warning says only "it leaked": NX_STILL_BOUND has
         * three separate causes in nx_tcp_socket_delete.c (still on the port
         * list, a bind in progress, or a state that is not CLOSED) and the
         * repair for each is different. docs/RESEARCH.md 37.5 had 830 of
         * these and could not say which.
         */
        AMI_WARN("bsdsocket: %s_socket_delete refused (%ld) state %ld "
                 "flags 0x%lx port %ld; leaking %ld bytes rather than "
                 "corrupting the created list",
                 ((sock->as_Flags & ASF_TCP) != 0) ? "nx_tcp" : "nx_udp",
                 (long)status,
                 ((sock->as_Flags & ASF_TCP) != 0)
                     ? (long)sock->as_Nx.tcp.nx_tcp_socket_state : 0L,
                 (long)sock->as_Flags, (long)sock->as_LocalPort,
                 (long)sizeof(AmiSocket));

        sock->as_Flags |= ASF_ORPHANED;

        /*
         * The block stays alive for NetX Duo's benefit, but it must stop
         * pointing at ours: the owning base is about to be freed, and a late
         * receive or disconnect callback would signal a dead task.
         */
        if ((sock->as_Flags & ASF_TCP) != 0)
            sock->as_Nx.tcp.nx_tcp_socket_reserved_ptr = NX_NULL;
        else
            sock->as_Nx.udp.nx_udp_socket_reserved_ptr = NX_NULL;

        sock->as_Owner = NULL;

        return FALSE;
    }

    sock->as_Flags |= ASF_DELETED;

    return TRUE;
}

VOID bsd_socket_release(struct AmiSocketBase *base, AmiSocket *sock)
{
    if (sock == NULL)
        return;

    if (sock->as_RefCount > 0)
        sock->as_RefCount--;

    if (sock->as_RefCount > 0)
    {
        /*
         * THE SOCKET OUTLIVES THIS BASE, AND as_Owner MUST NOT.
         *
         * as_Owner is the base a NetX Duo receive/disconnect callback signals
         * (select.c, bsd_event_post), and ObtainSocket() sets it to the base
         * that took the socket. When that base closes while another reference
         * is still held -- ReleaseCopyOfSocket() plus ObtainSocket(), or a
         * Dup2Socket() across bases, which is every inetd-style handoff --
         * the reference count keeps the AmiSocket alive and the base is freed
         * underneath the pointer. The next callback then Signal()s a
         * struct Task read out of freed memory, which on a machine with no
         * memory protection is a write into whatever now occupies it.
         *
         * Found by the TCP: handler work (docs/RESEARCH.md 34), where it hung
         * the first socket-handoff run.
         *
         * NULL is the right answer rather than "the other holder", because
         * there is no way to know which holder that is: one NX socket has one
         * owner and handoff.c says so. Events are recorded in as_Events
         * either way, so a poll still sees them; only the asynchronous wakeup
         * is lost, and it is lost to the base that no longer exists.
         */
        if (sock->as_Owner == base)
            sock->as_Owner = NULL;

        return;
    }

    /*
     * Drop the listen request BEFORE tearing down the socket parked on it,
     * or NetX Duo is left holding a pointer to freed memory.
     *
     * The parked socket sits in SYN_RECEIVED, not LISTEN -- bsd_listen() posts
     * the accept up front so NetX Duo answers a SYN without the application
     * having called accept(). nx_tcp_server_socket_unlisten() refuses a socket
     * that is not in LISTEN state, and disconnect() is what winds a *server*
     * socket back there (nx_tcp_socket_disconnect.c: "Server socket, return to
     * LISTEN state").
     */
    if ((sock->as_Flags & ASF_LISTENING) != 0)
    {
        NX_IP *ip = netstack_ip();

        if (sock->as_Incoming != NULL)
            nx_tcp_socket_disconnect(&sock->as_Incoming->as_Nx.tcp, NX_NO_WAIT);

        if (ip != NULL)
            nx_tcp_server_socket_unlisten(ip, sock->as_ListenPort);

        sock->as_Flags &= ~ASF_LISTENING;
    }

    /* A listening socket owns the spare parked on the port. */
    if (sock->as_Incoming != NULL)
    {
        if (bsd_socket_destroy(sock->as_Incoming))
            ami_free(sock->as_Incoming);
        sock->as_Incoming = NULL;
    }

    if (bsd_socket_destroy(sock))
        ami_free(sock);
}

VOID bsd_close_all(struct AmiSocketBase *base)
{
    LONG fd;

    if (base->sb_Table == NULL)
        return;

    /* One bracket for the lot: this runs from CloseLibrary(), where the whole
       table goes at once and adopting per socket would be pure overhead. */
    if (bsd_nx_enter(base) != 0)
    {
        AMI_WARN("bsdsocket: CloseLibrary with the kernel down; "
                 "sockets left to the stack teardown");
        return;
    }

    for (fd = 0; fd < base->sb_TableSize; fd++)
    {
        AmiSocket *sock = base->sb_Table[fd];

        if (sock == NULL)
            continue;

        base->sb_Table[fd] = NULL;
        bsd_socket_release(base, sock);
    }

    /*
     * Take back whatever a PREVIOUS program left closing. Not the ones this
     * loop has just parked -- their FINs went out microseconds ago and none of
     * them can be finished yet -- which is the whole reason the list is global
     * and outlives the base.
     */
    bsd_closing_sweep();

    bsd_nx_leave(base);
}

/* -------------------------------------------------------- sockaddr helpers */

VOID bsd_addr_from_v4(NXD_ADDRESS *addr, ULONG v4)
{
    addr->nxd_ip_version       = NX_IP_VERSION_V4;
    addr->nxd_ip_address.v4    = v4;
}

/*
 * Which family is this sockaddr?
 *
 * It cannot be answered by reading a struct member, because the two structs
 * this NDK offers do not agree on where the family lives:
 *
 *   sockaddr_in    [0] = sin_len (16, or 0 from a memset), [1] = sin_family
 *   sockaddr_in6   [0] = sin6_family, [1] = padding
 *
 * So the answer comes from the bytes plus the length the caller declared,
 * longest and most specific first. The AF_INET6 test requires BOTH a 23 in
 * byte 0 and a length that could hold a sockaddr_in6, which no sockaddr_in
 * can satisfy (a sin_len of 23 with a namelen of 28 is not something any
 * caller produces).
 */
LONG bsd_sa_family(const struct sockaddr *sa, socklen_t len)
{
    const UBYTE *b = (const UBYTE *)sa;

    if (sa == NULL)
        return -1;

#ifdef AMINETXDUO_IPV6
    if (len >= (socklen_t)sizeof(struct sockaddr_in6) && b[0] == AF_INET6)
        return AF_INET6;
#endif

    if (len < (socklen_t)sizeof(struct sockaddr_in))
        return -1;

    if (b[1] == AF_INET)
        return AF_INET;

    /*
     * AF_UNSPEC is legal on connect() (BSD's "dissolve the association") and
     * on the destination of a sendto() to a connected socket. A zeroed
     * sockaddr reaches here.
     */
    if (b[1] == AF_UNSPEC && b[0] <= (UBYTE)sizeof(struct sockaddr_in))
        return AF_UNSPEC;

    return -1;
}

LONG bsd_sockaddr_get(struct AmiSocketBase *base, const struct sockaddr *sa,
                      socklen_t len, NXD_ADDRESS *addr, UINT *port,
                      ULONG *scope_id)
{
    LONG family;

    if (scope_id != NULL)
        *scope_id = 0;

    if (sa == NULL)
        return bsd_fail(base, AMI_EFAULT);

    family = bsd_sa_family(sa, len);

    if (family == -1)
    {
        /* Too short to be anything is EINVAL; the right size but the wrong
           family is EAFNOSUPPORT, which is what BSD reports and what the
           conformance suite checks. */
        return bsd_fail(base,
                        (len < (socklen_t)sizeof(struct sockaddr_in))
                            ? AMI_EINVAL : AMI_EAFNOSUPPORT);
    }

#ifdef AMINETXDUO_IPV6
    if (family == AF_INET6)
    {
        const struct sockaddr_in6 *sin6 = (const struct sockaddr_in6 *)sa;

        addr->nxd_ip_version = NX_IP_VERSION_V6;
        bsd_in6_to_words(sin6->sin6_addr.s6_addr, addr->nxd_ip_address.v6);

        *port = (UINT)BSD_NTOHS(sin6->sin6_port);

        if (scope_id != NULL)
            *scope_id = (ULONG)sin6->sin6_scope_id;

        return 0;
    }
#endif

    {
        const struct sockaddr_in *sin = (const struct sockaddr_in *)sa;

        /* m68k is the wire order, so the ntoh* below are documentation. */
        bsd_addr_from_v4(addr, BSD_NTOHL(sin->sin_addr.s_addr));
        *port = (UINT)BSD_NTOHS(sin->sin_port);
    }

    return 0;
}

VOID bsd_sockaddr_put(const AmiSocket *sock, struct sockaddr *sa,
                      socklen_t *len, const NXD_ADDRESS *addr, UINT port)
{
    socklen_t copy;

    if (sa == NULL || len == NULL)
        return;

#ifdef AMINETXDUO_IPV6
    /*
     * The SHAPE follows the socket, not the address. An AF_INET6 socket that
     * accepted an IPv4 connection must report the peer as a sockaddr_in6
     * holding ::ffff:a.b.c.d -- an application that called accept() on an
     * AF_INET6 socket has a sockaddr_in6 on its stack and nothing else.
     */
    if (sock != NULL && (sock->as_Flags & ASF_INET6) != 0)
    {
        struct sockaddr_in6 sin6;
        NXD_ADDRESS         mapped;
        const NXD_ADDRESS  *use = addr;

        if (addr->nxd_ip_version == NX_IP_VERSION_V4)
        {
            bsd_addr_to_v4mapped(&mapped, addr->nxd_ip_address.v4);
            use = &mapped;
        }

        bsd_bzero(&sin6, sizeof(sin6));
        /* No sin6_len field in this NDK's sockaddr_in6 -- see the note in
           bsdsocket_internal.h. Setting one would corrupt sin6_family. */
        sin6.sin6_family = AF_INET6;
        sin6.sin6_port   = (in_port_t)BSD_HTONS((UWORD)port);
        bsd_words_to_in6(use->nxd_ip_address.v6, sin6.sin6_addr.s6_addr);
        sin6.sin6_scope_id = sock->as_ScopeId;

        copy = *len;
        if (copy > (socklen_t)sizeof(sin6))
            copy = (socklen_t)sizeof(sin6);

        bsd_bcopy(&sin6, sa, (ULONG)copy);
        *len = (socklen_t)sizeof(struct sockaddr_in6);

        return;
    }
#else
    (VOID)sock;
#endif

    {
        struct sockaddr_in sin;

        bsd_bzero(&sin, sizeof(sin));
        sin.sin_len    = (UBYTE)sizeof(struct sockaddr_in);
        sin.sin_family = AF_INET;
        sin.sin_port   = (in_port_t)BSD_HTONS((UWORD)port);
        sin.sin_addr.s_addr =
            BSD_HTONL((addr->nxd_ip_version == NX_IP_VERSION_V4)
                          ? addr->nxd_ip_address.v4 : 0UL);

        copy = *len;
        if (copy > (socklen_t)sizeof(sin))
            copy = (socklen_t)sizeof(sin);

        bsd_bcopy(&sin, sa, (ULONG)copy);
        *len = (socklen_t)sizeof(struct sockaddr_in);
    }
}

/* ---------------------------------------------------------------- vectors */

LONG bsd_socket(register LONG domain   __asm("d0"),
                register LONG type     __asm("d1"),
                register LONG protocol __asm("d2"),
                register struct AmiSocketBase *SocketBase __asm("a6"))
{
    AmiSocket *sock;
    NX_IP     *ip = netstack_ip();
    UINT       status;
    LONG       fd;

    if (ip == NULL)
        return bsd_fail(SocketBase, AMI_ENETDOWN);

#ifdef AMINETXDUO_IPV6
    if (domain != AF_INET && domain != AF_INET6)
        return bsd_fail(SocketBase, AMI_EAFNOSUPPORT);

    /*
     * An AF_INET6 socket on a stack whose IPv6 half failed to come up would
     * be a socket that can never send anything; EAFNOSUPPORT now is better
     * than ENETUNREACH on every later call.
     */
    if (domain == AF_INET6 && !netstack_ipv6_enabled())
        return bsd_fail(SocketBase, AMI_EAFNOSUPPORT);
#else
    if (domain != AF_INET)
        return bsd_fail(SocketBase, AMI_EAFNOSUPPORT);
#endif

    if (type != SOCK_STREAM && type != SOCK_DGRAM && type != SOCK_RAW)
        return bsd_fail(SocketBase, AMI_ESOCKTNOSUPPORT);

    if (type == SOCK_RAW)
    {
        /*
         * IPv4 only, and the protocol number is mandatory.
         *
         * Mandatory because it is the demultiplex key: raw.c matches an
         * inbound datagram's IP protocol byte against it, and it is also what
         * goes into the header of everything the socket sends. BSD reports
         * EPROTONOSUPPORT for socket(AF_INET, SOCK_RAW, 0) for the same
         * reason.
         *
         * NOT EACCES. bsdsocktest skips this test when it sees EACCES, which
         * on an OS with no privilege model would be a lie -- and there is
         * nothing to skip now that it works.
         */
        if (domain != AF_INET)
            return bsd_fail(SocketBase, AMI_EAFNOSUPPORT);

        if (protocol <= 0 || protocol > 255)
            return bsd_fail(SocketBase, AMI_EPROTONOSUPPORT);
    }
    else if (protocol != 0 &&
             protocol != ((type == SOCK_STREAM) ? IPPROTO_TCP : IPPROTO_UDP))
    {
        return bsd_fail(SocketBase, AMI_EPROTONOSUPPORT);
    }

    sock = bsd_socket_alloc(SocketBase, (UWORD)domain, (UWORD)type, protocol);
    if (sock == NULL)
        return bsd_fail(SocketBase, AMI_ENOBUFS);

    if (bsd_nx_enter(SocketBase) != 0)
    {
        ami_free(sock);
        return bsd_fail(SocketBase, AMI_ENETDOWN);
    }

    /* A program that opens sockets is the one that can afford to reclaim the
       ones an earlier program left closing, and it wants their ports back. */
    bsd_closing_sweep();

    if (type == SOCK_RAW)
    {
        if (bsd_raw_open(SocketBase, sock) != 0)
        {
            bsd_nx_leave(SocketBase);
            ami_free(sock);
            return -1;                  /* bsd_raw_open set errno */
        }

        status = NX_SUCCESS;
    }
    else if (type == SOCK_STREAM)
    {
        status = nx_tcp_socket_create(ip, &sock->as_Nx.tcp, bsd_tcp_name,
                                      NX_IP_NORMAL, NX_FRAGMENT_OKAY,
                                      NX_IP_TIME_TO_LIVE, ami_bsd_tcp_window(),
                                      bsd_tcp_urgent_notify,
                                      bsd_tcp_disconnect_callback);
        if (status == NX_SUCCESS)
        {
            bsd_tcp_seed_isn(&sock->as_Nx.tcp);
            bsd_tcp_keepalive_default(&sock->as_Nx.tcp);
        }
    }
    else
    {
        status = nx_udp_socket_create(ip, &sock->as_Nx.udp, bsd_udp_name,
                                      NX_IP_NORMAL, NX_FRAGMENT_OKAY,
                                      NX_IP_TIME_TO_LIVE,
                                      bsd_udp_queue_max());
    }

    if (status != NX_SUCCESS)
    {
        bsd_nx_leave(SocketBase);
        ami_free(sock);
        return bsd_fail(SocketBase, bsd_errno_from_nx(status));
    }

    bsd_events_attach(sock);

    fd = bsd_fd_alloc(SocketBase, sock);
    if (fd < 0)
    {
        if (bsd_socket_destroy(sock))
            ami_free(sock);
        bsd_nx_leave(SocketBase);
        return bsd_fail(SocketBase, AMI_EMFILE);
    }

    bsd_nx_leave(SocketBase);

    return fd;
}

LONG bsd_bind(register LONG sock_fd            __asm("d0"),
              register struct sockaddr *name   __asm("a0"),
              register socklen_t namelen       __asm("d1"),
              register struct AmiSocketBase *SocketBase __asm("a6"))
{
    AmiSocket  *sock = bsd_lookup(SocketBase, sock_fd);
    NXD_ADDRESS addr;
    ULONG       scope = 0;
    UINT        port = 0;
    UINT        status;

    if (sock == NULL)
        return bsd_fail(SocketBase, AMI_EBADF);

    /*
     * "The hook function will be invoked before dropping into the kernel
     * 'bind()' call" -- so here, with a socket already known to exist and
     * before anything about it has changed. A hook that denies the call
     * returns the errno to fail it with, and the call must look to the
     * application exactly as if the stack had refused it.
     *
     * After the descriptor lookup rather than before it, so that a monitor
     * cannot be used to probe which descriptors exist.
     */
    if (bsd_netmon_have(MHT_Bind))
    {
        struct BindMonitorMsg bmm;
        LONG                  denied;

        bsd_bzero(&bmm, sizeof(bmm));
        bmm.bmm_Size    = (LONG)sizeof(bmm);
        bmm.bmm_Caller  = bsd_netmon_caller(SocketBase);
        bmm.bmm_Socket  = sock_fd;
        bmm.bmm_Name    = name;
        bmm.bmm_NameLen = (LONG)namelen;

        denied = bsd_netmon_dispatch(MHT_Bind, &bmm);
        if (denied > 0)
            return bsd_fail(SocketBase, denied);
    }

    if ((sock->as_Flags & ASF_BOUND) != 0)
        return bsd_fail(SocketBase, AMI_EINVAL);

    if (bsd_sockaddr_get(SocketBase, name, namelen, &addr, &port, &scope) != 0)
        return -1;

#ifdef AMINETXDUO_IPV6
    /*
     * The families have to agree. An AF_INET socket handed a sockaddr_in6
     * (or the reverse) is a programming error, not something to interpret --
     * except for the v4-mapped case on a dual-stack socket, which is a
     * legitimate way of saying "bind the v4 side of this port".
     */
    if ((sock->as_Flags & ASF_INET6) != 0)
    {
        if (addr.nxd_ip_version == NX_IP_VERSION_V4)
            return bsd_fail(SocketBase, AMI_EAFNOSUPPORT);
        if (!bsd_addr_normalise(sock, &addr))
            return bsd_fail(SocketBase, AMI_EINVAL);
        sock->as_ScopeId = scope;
    }
    else if (addr.nxd_ip_version == NX_IP_VERSION_V6)
    {
        return bsd_fail(SocketBase, AMI_EAFNOSUPPORT);
    }
#endif

    /*
     * NetX Duo binds a socket to a PORT, not to an address: there is no
     * nx_*_socket_bind that takes one. A bind to a specific local address is
     * therefore recorded (so getsockname reports it) but does not restrict
     * what the socket receives. That is a real gap, and it is the same one
     * the IPv4 path has always had.
     */
    sock->as_LocalAddr = addr;
    sock->as_LocalPort = port;

    if ((sock->as_Flags & ASF_RAW) != 0)
    {
        /* A raw socket has no port and NetX Duo has nothing to bind it to.
           The address is recorded so getsockname() answers, exactly as the
           IPv4 note above describes for the transport sockets. */
        sock->as_Flags |= ASF_BOUND;

        return 0;
    }

    if ((sock->as_Flags & ASF_UDP) != 0)
    {
        if (bsd_nx_enter(SocketBase) != 0)
            return bsd_fail(SocketBase, AMI_ENETDOWN);

        status = nx_udp_socket_bind(&sock->as_Nx.udp,
                                    (port != 0) ? port : NX_ANY_PORT,
                                    NX_NO_WAIT);
        if (status != NX_SUCCESS)
        {
            bsd_nx_leave(SocketBase);
            return bsd_fail(SocketBase, bsd_errno_from_nx(status));
        }

        nx_udp_socket_port_get(&sock->as_Nx.udp, &port);
        bsd_nx_leave(SocketBase);

        sock->as_LocalPort = port;
        sock->as_Flags |= ASF_NXBOUND;
    }
    else
    {
        /*
         * TCP takes the port here too, rather than deferring to connect() or
         * listen(). BSD binds at bind() time and two things depend on it:
         * getsockname() after bind(port 0) must report the ephemeral port the
         * stack picked, and a second bind() to a port already in use must fail
         * with EADDRINUSE. Neither is observable if the port is only claimed
         * later.
         *
         * The listening descriptor's own NX socket is never used for a
         * connection -- listen() parks a separate socket on the port -- so
         * holding the port here does not collide with
         * nx_tcp_server_socket_listen(), which registers a listen request and
         * does not touch the bound-port table.
         */
        if (bsd_nx_enter(SocketBase) != 0)
            return bsd_fail(SocketBase, AMI_ENETDOWN);

        status = nx_tcp_client_socket_bind(&sock->as_Nx.tcp,
                                           (port != 0) ? port : NX_ANY_PORT,
                                           NX_NO_WAIT);
        if (status != NX_SUCCESS)
        {
            bsd_nx_leave(SocketBase);
            return bsd_fail(SocketBase,
                            (status == NX_PORT_UNAVAILABLE ||
                             status == NX_ALREADY_BOUND)
                                ? AMI_EADDRINUSE
                                : bsd_errno_from_nx(status));
        }

        nx_tcp_client_socket_port_get(&sock->as_Nx.tcp, &port);
        bsd_nx_leave(SocketBase);

        sock->as_LocalPort = port;
        sock->as_Flags |= ASF_NXBOUND;
    }

    sock->as_Flags |= ASF_BOUND;

    return 0;
}

/*
 * Park a fresh socket on a listening descriptor's port, so the port keeps
 * answering. Call inside a ThreadX bracket; TRUE means the listener has a
 * spare again.
 *
 * THE LISTENER MUST SURVIVE THIS FAILING. The version this replaces cleared
 * as_Incoming, tried once, and on failure left the listener with nothing --
 * and as_Incoming is what bsd_accept() checks first, so every later accept()
 * on that descriptor returned EINVAL for the life of the socket.
 * docs/RESEARCH.md 37.4 measured 1,951 consecutive EINVALs behind exactly one
 * `relisten failed` line. So there are three attempts here rather than one,
 * and bsd_accept() calls this again on a listener that has no spare, which is
 * the part that matters: whatever ended the last attempt, the next accept()
 * gets a clean try rather than a permanent refusal.
 *
 * WHY THE TRIGGER IS NOT ASSUMED. 37.4 published a root cause for that
 * NX_INVALID_RELISTEN and then retracted it: nx_tcp_packet_process.c:650
 * clears the listen request's socket slot itself when the SYN arrives, so the
 * obvious "the slot is still occupied" explanation predicts a failure that is
 * structural, and the measured one is intermittent. Nothing here depends on
 * knowing which it was.
 */
static BOOL bsd_listen_rearm(struct AmiSocketBase *base, AmiSocket *sock)
{
    NX_IP     *ip = netstack_ip();
    AmiSocket *spare;
    UINT       status;

    if (ip == NULL || (sock->as_Flags & ASF_LISTENING) == 0)
        return FALSE;

    if (sock->as_Incoming != NULL)
        return TRUE;

    spare = bsd_socket_alloc(base, sock->as_Domain, sock->as_Type,
                             sock->as_Protocol);
    if (spare == NULL)
        return FALSE;

    status = nx_tcp_socket_create(ip, &spare->as_Nx.tcp, bsd_tcp_name,
                                  NX_IP_NORMAL, NX_FRAGMENT_OKAY,
                                  NX_IP_TIME_TO_LIVE, ami_bsd_tcp_window(),
                                  bsd_tcp_urgent_notify,
                                  bsd_tcp_disconnect_callback);
    if (status != NX_SUCCESS)
    {
        ami_free(spare);
        return FALSE;
    }

    bsd_tcp_seed_isn(&spare->as_Nx.tcp);
    bsd_tcp_keepalive_default(&spare->as_Nx.tcp);

    spare->as_Flags    |= ASF_INCOMING | ASF_SERVER;
    spare->as_Parent    = sock;
    spare->as_LocalPort = sock->as_ListenPort;
    bsd_events_attach(spare);

    /*
     * NX_CONNECTION_PENDING means a queued connection was handed straight to
     * the spare -- still a success, and the next accept() returns at once.
     */
    status = nx_tcp_server_socket_relisten(ip, sock->as_ListenPort,
                                           &spare->as_Nx.tcp);

    if (status != NX_SUCCESS && status != NX_CONNECTION_PENDING)
    {
        /*
         * Rebuild the listen request from nothing. unlisten() drops whatever
         * state it had got into, including any queued connection requests --
         * that is the cost, and it is cheap next to a listener that never
         * accepts again -- and listen() then puts the spare on the port
         * exactly as bsd_listen() did in the first place.
         */
        AMI_WARN("bsdsocket: relisten on port %ld failed (%ld); rebuilding "
                 "the listen request",
                 (long)sock->as_ListenPort, (long)status);

        nx_tcp_server_socket_unlisten(ip, sock->as_ListenPort);

        status = nx_tcp_server_socket_listen(ip, sock->as_ListenPort,
                                             &spare->as_Nx.tcp,
                                             sock->as_Backlog,
                                             bsd_listen_callback);
    }

    if (status != NX_SUCCESS && status != NX_CONNECTION_PENDING)
    {
        AMI_WARN("bsdsocket: port %ld has no listen request left (%ld); "
                 "the next accept() will try again",
                 (long)sock->as_ListenPort, (long)status);

        if (bsd_socket_destroy(spare))
            ami_free(spare);

        return FALSE;
    }

    /* Arm it, exactly as bsd_listen() does -- see the comment there. Without
       this the next client's SYN goes unanswered. */
    (VOID)nx_tcp_server_socket_accept(&spare->as_Nx.tcp, NX_NO_WAIT);

    sock->as_Incoming = spare;
    if (status == NX_CONNECTION_PENDING)
        sock->as_Flags |= ASF_ACCEPTPEND;

    return TRUE;
}

LONG bsd_listen(register LONG sock_fd __asm("d0"),
                register LONG backlog __asm("d1"),
                register struct AmiSocketBase *SocketBase __asm("a6"))
{
    AmiSocket *sock = bsd_lookup(SocketBase, sock_fd);
    AmiSocket *incoming;
    NX_IP     *ip = netstack_ip();
    UINT       status;

    if (sock == NULL)
        return bsd_fail(SocketBase, AMI_EBADF);

    if (ip == NULL)
        return bsd_fail(SocketBase, AMI_ENETDOWN);

    if ((sock->as_Flags & ASF_TCP) == 0)
        return bsd_fail(SocketBase, AMI_EOPNOTSUPP);

    if ((sock->as_Flags & ASF_LISTENING) != 0)
        return 0;                               /* idempotent, as in BSD */

    if ((sock->as_Flags & ASF_CONNECTED) != 0)
        return bsd_fail(SocketBase, AMI_EISCONN);

    if (sock->as_LocalPort == 0)
        return bsd_fail(SocketBase, AMI_EDESTADDRREQ);

    if (backlog < 1)
        backlog = 1;
    if (backlog > BSD_MAX_BACKLOG)
        backlog = BSD_MAX_BACKLOG;

    /*
     * The listening descriptor itself never carries a connection: it parks a
     * spare socket on the port for NetX Duo to hand the next connection to.
     */
    incoming = bsd_socket_alloc(SocketBase, sock->as_Domain, sock->as_Type,
                                sock->as_Protocol);
    if (incoming == NULL)
        return bsd_fail(SocketBase, AMI_ENOBUFS);

    if (bsd_nx_enter(SocketBase) != 0)
    {
        ami_free(incoming);
        return bsd_fail(SocketBase, AMI_ENETDOWN);
    }

    status = nx_tcp_socket_create(ip, &incoming->as_Nx.tcp, bsd_tcp_name,
                                  NX_IP_NORMAL, NX_FRAGMENT_OKAY,
                                  NX_IP_TIME_TO_LIVE, ami_bsd_tcp_window(),
                                  bsd_tcp_urgent_notify,
                                  bsd_tcp_disconnect_callback);
    if (status == NX_SUCCESS)
    {
        bsd_tcp_seed_isn(&incoming->as_Nx.tcp);
        bsd_tcp_keepalive_default(&incoming->as_Nx.tcp);
    }
    if (status != NX_SUCCESS)
    {
        bsd_nx_leave(SocketBase);
        ami_free(incoming);
        return bsd_fail(SocketBase, bsd_errno_from_nx(status));
    }

    incoming->as_Flags     |= ASF_INCOMING | ASF_SERVER;
    incoming->as_Parent     = sock;
    incoming->as_LocalPort  = sock->as_LocalPort;
    bsd_events_attach(incoming);

    status = nx_tcp_server_socket_listen(ip, sock->as_LocalPort,
                                         &incoming->as_Nx.tcp,
                                         (UINT)backlog, bsd_listen_callback);
    if (status != NX_SUCCESS)
    {
        if (bsd_socket_destroy(incoming))
            ami_free(incoming);
        bsd_nx_leave(SocketBase);
        return bsd_fail(SocketBase, bsd_errno_from_nx(status));
    }

    /*
     * Arm the parked socket NOW, before anyone calls accept().
     *
     * BSD completes the three-way handshake in the stack and queues the
     * finished connection for accept(); NetX Duo does not. Its SYN handler
     * only sends the SYN+ACK "if an accept call with suspension has already
     * been made for this socket" -- i.e. if the socket is already in
     * SYN_RECEIVED (nx_tcp_packet_process.c). A socket left in LISTEN state
     * silently ignores the SYN, so a client that connect()s before the server
     * happens to be inside accept() retransmits until it gives up. That is not
     * a corner case: it is what every listen/connect/accept sequence in one
     * task does, and it is why loopback connect() failed with ECONNREFUSED.
     *
     * nx_tcp_server_socket_accept() with NX_NO_WAIT moves LISTEN ->
     * SYN_RECEIVED and returns NX_IN_PROGRESS without blocking, which is
     * exactly the arming step. The real accept() below then either finds the
     * socket ESTABLISHED already or suspends on it.
     */
    status = nx_tcp_server_socket_accept(&incoming->as_Nx.tcp, NX_NO_WAIT);
    if (status != NX_IN_PROGRESS && status != NX_SUCCESS)
        AMI_WARN("bsdsocket: arming accept on port %ld failed (%ld)",
                 (long)sock->as_LocalPort, (long)status);

    bsd_nx_leave(SocketBase);

    sock->as_Incoming   = incoming;
    sock->as_ListenPort = sock->as_LocalPort;
    sock->as_Backlog    = (UINT)backlog;
    sock->as_Flags     |= ASF_LISTENING;

    return 0;
}

LONG bsd_accept(register LONG sock_fd          __asm("d0"),
                register struct sockaddr *addr __asm("a0"),
                register socklen_t *addrlen    __asm("a1"),
                register struct AmiSocketBase *SocketBase __asm("a6"))
{
    AmiSocket  *sock = bsd_lookup(SocketBase, sock_fd);
    AmiSocket  *incoming;
    NX_IP      *ip = netstack_ip();
    NXD_ADDRESS peer;
    ULONG       peer_port = 0;
    UINT        status;
    LONG        fd;

    if (sock == NULL)
        return bsd_fail(SocketBase, AMI_EBADF);

    if (ip == NULL)
        return bsd_fail(SocketBase, AMI_ENETDOWN);

    if ((sock->as_Flags & ASF_LISTENING) == 0)
        return bsd_fail(SocketBase, AMI_EINVAL);

    if (bsd_nx_enter(SocketBase) != 0)
        return bsd_fail(SocketBase, AMI_ENETDOWN);

    /*
     * A listener with no spare parked on it is a listener that lost one, not
     * a listener the application got wrong -- see bsd_listen_rearm(). Try to
     * give it one back before answering, so a single failed re-arm costs one
     * accept() rather than the socket.
     */
    if (sock->as_Incoming == NULL && !bsd_listen_rearm(SocketBase, sock))
    {
        bsd_nx_leave(SocketBase);
        return bsd_fail(SocketBase, AMI_ENOBUFS);
    }

    incoming = sock->as_Incoming;

    status = nx_tcp_server_socket_accept(
        &incoming->as_Nx.tcp,
        bsd_wait_option(sock, sock->as_RcvTimeout));

    if (status == NX_NOT_CONNECTED || status == NX_IN_PROGRESS ||
        status == NX_NO_PACKET)
    {
        bsd_nx_leave(SocketBase);
        return bsd_fail(SocketBase, AMI_EWOULDBLOCK);
    }

    if (status == NX_WAIT_ABORTED)
    {
        bsd_nx_leave(SocketBase);
        return bsd_fail(SocketBase, AMI_EINTR);
    }

    if (status != NX_SUCCESS)
    {
        bsd_nx_leave(SocketBase);
        return bsd_fail(SocketBase, bsd_errno_from_nx(status));
    }

    /*
     * nxd_ (not nx_) peer info: the v4-only entry point reports 0 for a peer
     * that connected over IPv6, so a dual-stack listener would hand accept()
     * a sockaddr full of zeroes.
     */
    nxd_tcp_socket_peer_info_get(&incoming->as_Nx.tcp, &peer, &peer_port);

#ifdef AMINETXDUO_IPV6
    /*
     * IPV6_V6ONLY enforcement lives here and can live nowhere else.
     *
     * NetX Duo's listen is registered against a port, not an address family,
     * so a V6ONLY socket still has an IPv4 SYN answered for it down in the TCP
     * state machine -- by the time this code runs the handshake is complete.
     * The only honest thing left is to close that connection and go back to
     * waiting, which is what a V6ONLY listener on any other stack looks like
     * from the client's side (a connection that is accepted and immediately
     * reset, rather than a SYN that goes unanswered).
     *
     * Reported as EWOULDBLOCK rather than looping internally: a blocking
     * accept() that silently swallowed connections would hide the fact that
     * something is knocking on the v4 side, and a non-blocking one must return
     * anyway. The caller retries, exactly as it would after any spurious
     * wakeup.
     */
    if ((sock->as_Flags & ASF_V6ONLY) != 0 &&
        peer.nxd_ip_version == NX_IP_VERSION_V4)
    {
        AMI_DEBUG("bsdsocket: V6ONLY listener on port %ld refused an IPv4 peer",
                  (long)sock->as_ListenPort);

        nx_tcp_socket_disconnect(&incoming->as_Nx.tcp, NX_NO_WAIT);
        nx_tcp_server_socket_unaccept(&incoming->as_Nx.tcp);
        nx_tcp_server_socket_relisten(ip, sock->as_ListenPort,
                                      &incoming->as_Nx.tcp);
        (VOID)nx_tcp_server_socket_accept(&incoming->as_Nx.tcp, NX_NO_WAIT);

        bsd_nx_leave(SocketBase);

        return bsd_fail(SocketBase, AMI_EWOULDBLOCK);
    }
#endif

    /* Promote the parked socket to a descriptor of its own. ASF_SERVER stays
     * set: the port still has to go back through unaccept() at close. */
    incoming->as_Flags &= ~(ASF_INCOMING | ASF_ACCEPTPEND);
    incoming->as_Flags |= ASF_CONNECTED | ASF_BOUND;
    incoming->as_Parent = NULL;
    incoming->as_Owner  = SocketBase;

    incoming->as_PeerAddr  = peer;
    incoming->as_PeerPort  = (UINT)peer_port;
    incoming->as_LocalPort = sock->as_ListenPort;

    fd = bsd_fd_alloc(SocketBase, incoming);
    if (fd < 0)
    {
        /* Put the socket back on the port rather than losing the listener. */
        nx_tcp_socket_disconnect(&incoming->as_Nx.tcp, NX_NO_WAIT);
        nx_tcp_server_socket_unaccept(&incoming->as_Nx.tcp);
        nx_tcp_server_socket_relisten(ip, sock->as_ListenPort,
                                      &incoming->as_Nx.tcp);
        (VOID)nx_tcp_server_socket_accept(&incoming->as_Nx.tcp, NX_NO_WAIT);

        incoming->as_Flags &= ~ASF_CONNECTED;
        incoming->as_Flags |= ASF_INCOMING;
        incoming->as_Parent = sock;

        bsd_nx_leave(SocketBase);

        return bsd_fail(SocketBase, AMI_EMFILE);
    }

    sock->as_Incoming = NULL;
    sock->as_Flags   &= ~ASF_ACCEPTPEND;

    /*
     * Park a fresh socket so the port keeps accepting. The connection this
     * call is returning is already the caller's, so a re-arm that does not
     * take is NOT this accept()'s failure -- it is the next one's problem,
     * and bsd_listen_rearm() is what the next one calls.
     */
    (VOID)bsd_listen_rearm(SocketBase, sock);

    bsd_nx_leave(SocketBase);

    if (addr != NULL && addrlen != NULL)
        bsd_sockaddr_put(incoming, addr, addrlen, &incoming->as_PeerAddr,
                         incoming->as_PeerPort);

    return fd;
}

/* The body of connect(), run inside a ThreadX context bracket. */
static LONG bsd_connect_locked(struct AmiSocketBase *SocketBase,
                               AmiSocket *sock, const NXD_ADDRESS *addr,
                               UINT port)
{
    UINT status;

    if ((sock->as_Flags & ASF_RAW) != 0)
    {
        /* Same meaning as on a datagram socket: a default destination. */
        sock->as_PeerAddr = *addr;
        sock->as_PeerPort = port;
        sock->as_Flags   |= ASF_CONNECTED;

        return 0;
    }

    if ((sock->as_Flags & ASF_UDP) != 0)
    {
        /*
         * A connected UDP socket is just a default destination here; NetX Duo
         * has no UDP connect, so record it and let send()/recv() use it.
         */
        if ((sock->as_Flags & ASF_NXBOUND) == 0)
        {
            status = nx_udp_socket_bind(&sock->as_Nx.udp, NX_ANY_PORT,
                                        NX_NO_WAIT);
            if (status != NX_SUCCESS)
                return bsd_fail(SocketBase, bsd_errno_from_nx(status));

            nx_udp_socket_port_get(&sock->as_Nx.udp, &sock->as_LocalPort);
            sock->as_Flags |= ASF_NXBOUND | ASF_BOUND;
        }

        sock->as_PeerAddr = *addr;
        sock->as_PeerPort = port;
        sock->as_Flags   |= ASF_CONNECTED;

        return 0;
    }

    if ((sock->as_Flags & ASF_CONNECTED) != 0)
        return bsd_fail(SocketBase, AMI_EISCONN);

    if ((sock->as_Flags & ASF_LISTENING) != 0)
        return bsd_fail(SocketBase, AMI_EINVAL);

    if ((sock->as_Flags & ASF_CONNECTING) != 0)
    {
        /*
         * A second connect() on a non-blocking socket reports the outcome of
         * the first, as BSD requires.
         */
        if (sock->as_Nx.tcp.nx_tcp_socket_state == NX_TCP_ESTABLISHED)
        {
            sock->as_Flags &= ~ASF_CONNECTING;
            sock->as_Flags |= ASF_CONNECTED;
            return 0;
        }

        return bsd_fail(SocketBase, AMI_EALREADY);
    }

    if ((sock->as_Flags & ASF_NXBOUND) == 0)
    {
        status = nx_tcp_client_socket_bind(
            &sock->as_Nx.tcp,
            (sock->as_LocalPort != 0) ? sock->as_LocalPort : NX_ANY_PORT,
            NX_NO_WAIT);
        if (status != NX_SUCCESS)
            return bsd_fail(SocketBase, bsd_errno_from_nx(status));

        nx_tcp_client_socket_port_get(&sock->as_Nx.tcp, &sock->as_LocalPort);
        sock->as_Flags |= ASF_NXBOUND | ASF_BOUND;
    }

    /*
     * ASF_CONNECTING goes on BEFORE the call, not after.
     *
     * The establish / disconnect-complete callbacks (select.c) can fire from
     * *inside* nx_tcp_client_socket_connect(): a loopback SYN is queued to the
     * IP thread, which outranks us, and tx_mutex_put() inside the connect is a
     * scheduling point -- so on loopback the entire SYN -> RST -> reset
     * sequence can finish before the connect returns NX_IN_PROGRESS to us.
     * Setting the flag afterwards meant the callback saw a socket that was not
     * yet "connecting", filed the RST as an ordinary EOF, and left SO_ERROR at
     * zero. Setting it first makes the callback authoritative and this code
     * merely read its answer.
     */
    sock->as_PeerAddr = *addr;
    sock->as_PeerPort = port;
    sock->as_Flags   |= ASF_CONNECTING;

    /*
     * nxd_, not nx_: the v4-only wrapper builds an NXD_ADDRESS tagged
     * NX_IP_VERSION_V4 and calls exactly this, so going straight to it costs
     * nothing in the floor build and is the only way to reach an IPv6 peer.
     */
    status = nxd_tcp_client_socket_connect(
        &sock->as_Nx.tcp, (NXD_ADDRESS *)addr, port,
        bsd_wait_option(sock, sock->as_SndTimeout));

    if (status == NX_SUCCESS)
    {
        sock->as_Flags |= ASF_CONNECTED;
        sock->as_Flags &= ~ASF_CONNECTING;
        return 0;
    }

    if (status == NX_IN_PROGRESS)
    {
        /* Already established inside the call? Then the connect is done, and
           BSD says a non-blocking connect that completes at once returns 0. */
        if ((sock->as_Flags & ASF_CONNECTED) != 0)
        {
            sock->as_Flags &= ~ASF_CONNECTING;
            return 0;
        }

        /* Already failed inside the call? The callback cleared the flag and
           left the reason in as_SoError. */
        if ((sock->as_Flags & ASF_CONNECTING) == 0)
        {
            if (sock->as_SoError == 0)
                sock->as_SoError = AMI_ECONNREFUSED;

            return bsd_fail(SocketBase, sock->as_SoError);
        }

        return bsd_fail(SocketBase, AMI_EINPROGRESS);
    }

    sock->as_Flags &= ~ASF_CONNECTING;

    if (status == NX_WAIT_ABORTED)
        return bsd_fail(SocketBase, AMI_EINTR);

    /* A failed connect leaves the reason behind for getsockopt(SO_ERROR),
       which is how a non-blocking caller finds out what went wrong. */
    sock->as_SoError = (status == NX_NOT_CONNECTED)
                           ? AMI_ECONNREFUSED
                           : bsd_errno_from_nx(status);

    return bsd_fail(SocketBase, sock->as_SoError);
}

LONG bsd_connect(register LONG sock_fd          __asm("d0"),
                 register struct sockaddr *name __asm("a0"),
                 register socklen_t namelen     __asm("d1"),
                 register struct AmiSocketBase *SocketBase __asm("a6"))
{
    AmiSocket  *sock = bsd_lookup(SocketBase, sock_fd);
    NXD_ADDRESS addr;
    ULONG       scope = 0;
    UINT        port = 0;
    LONG        result;

    if (sock == NULL)
        return bsd_fail(SocketBase, AMI_EBADF);

    /* The same, for connect() -- see the note in bsd_bind(). */
    if (bsd_netmon_have(MHT_Connect))
    {
        struct ConnectMonitorMsg cmm;
        LONG                     denied;

        bsd_bzero(&cmm, sizeof(cmm));
        cmm.cmm_Size    = (LONG)sizeof(cmm);
        cmm.cmm_Caller  = bsd_netmon_caller(SocketBase);
        cmm.cmm_Socket  = sock_fd;
        cmm.cmm_Name    = name;
        cmm.cmm_NameLen = (LONG)namelen;

        denied = bsd_netmon_dispatch(MHT_Connect, &cmm);
        if (denied > 0)
            return bsd_fail(SocketBase, denied);
    }

    if (bsd_sockaddr_get(SocketBase, name, namelen, &addr, &port, &scope) != 0)
        return -1;

#ifdef AMINETXDUO_IPV6
    if ((sock->as_Flags & ASF_INET6) != 0)
    {
        if (addr.nxd_ip_version != NX_IP_VERSION_V6)
            return bsd_fail(SocketBase, AMI_EAFNOSUPPORT);

        /* ::ffff:a.b.c.d on a dual-stack socket becomes a real IPv4 connect;
           on a V6ONLY socket it is refused. */
        if (!bsd_addr_normalise(sock, &addr))
            return bsd_fail(SocketBase, AMI_ENETUNREACH);

        sock->as_ScopeId = scope;
    }
    else if (addr.nxd_ip_version == NX_IP_VERSION_V6)
    {
        return bsd_fail(SocketBase, AMI_EAFNOSUPPORT);
    }
#endif

    if (bsd_nx_enter(SocketBase) != 0)
        return bsd_fail(SocketBase, AMI_ENETDOWN);

    result = bsd_connect_locked(SocketBase, sock, &addr, port);

    bsd_nx_leave(SocketBase);

    return result;
}

LONG bsd_shutdown(register LONG sock_fd __asm("d0"),
                  register LONG how     __asm("d1"),
                  register struct AmiSocketBase *SocketBase __asm("a6"))
{
    AmiSocket *sock = bsd_lookup(SocketBase, sock_fd);

    if (sock == NULL)
        return bsd_fail(SocketBase, AMI_EBADF);

    if (how < 0 || how > 2)
        return bsd_fail(SocketBase, AMI_EINVAL);

    if (how == 0 || how == 2)
        sock->as_Flags |= ASF_RDSHUT;

    if (how == 1 || how == 2)
    {
        sock->as_Flags |= ASF_WRSHUT;

        if ((sock->as_Flags & (ASF_TCP | ASF_CONNECTED)) ==
            (ASF_TCP | ASF_CONNECTED))
        {
            if (bsd_nx_enter(SocketBase) != 0)
                return bsd_fail(SocketBase, AMI_ENETDOWN);

            bsd_tcp_send_fin(sock);

            bsd_nx_leave(SocketBase);
        }
    }

    return 0;
}

LONG bsd_CloseSocket(register LONG sock_fd __asm("d0"),
                     register struct AmiSocketBase *SocketBase __asm("a6"))
{
    AmiSocket *sock = bsd_lookup(SocketBase, sock_fd);

    if (sock == NULL)
        return bsd_fail(SocketBase, AMI_EBADF);

    bsd_fd_free(SocketBase, sock_fd);

    /*
     * The descriptor is gone whatever happens next, so the caller always sees
     * success; the teardown itself needs ThreadX context, and if the stack is
     * already down there is nothing left to hand the socket back to.
     */
    if (bsd_nx_enter(SocketBase) == 0)
    {
        bsd_socket_release(SocketBase, sock);

        /* The one place that is guaranteed to run again while a long-lived
           program is still opening and closing connections. */
        bsd_closing_sweep();

        bsd_nx_leave(SocketBase);
    }
    else
    {
        AMI_WARN("bsdsocket: CloseSocket(%ld) with the kernel down; leaking",
                 (long)sock_fd);
    }

    return 0;
}

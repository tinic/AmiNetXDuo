/*
 * AmiNetXDuo -- NetX Duo tuning for the AmigaOS floor target.
 *
 * Target (docs/RESEARCH.md 9, decision 1): 68020, OS 3.1, 4 MB.  NetX Duo's
 * shipped defaults assume an embedded target with a static budget and no
 * competition for RAM; on an Amiga the stack shares memory with everything
 * else on the machine and must stay modest.  Everything here is a conscious
 * departure from the default -- if a value is not listed, the default stands.
 *
 * Sizes that the rest of AmiNetXDuo shares (packet payload, pool bounds) live
 * in include/aminetxduo/netstack.h and are mirrored, not redefined, here.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef NX_USER_H
#define NX_USER_H


/* ---------------------------------------------------------------- timing -- */

/*
 * One tick is 20 ms.  THIS MUST EQUAL TX_TIMER_TICKS_PER_SECOND in
 * port/threadx-amiga/inc/tx_port.h: NetX Duo expresses every one of its own
 * rates as a divisor of this, so a disagreement does not fail -- it silently
 * scales every TCP timer by the ratio.
 *
 * 50 Hz rather than 100 because that is what the platform has always run
 * (AmiTCP and its descendants: `#define hz (50)`) and an order of magnitude
 * more than the BSD protocol timers above us consume -- pfslowtimo at hz/2
 * (500 ms) and pffasttimo at hz/5 (200 ms).  The tick task takes its wakeups
 * from timer.device UNIT_VBLANK and its TIME from ReadEClock(), so the rate is
 * honest on PAL, NTSC, RTG and accelerated systems alike.
 */

#define NX_IP_PERIODIC_RATE                     50


/* --------------------------------------------------------------- packets -- */

/*
 * Payload of a pool packet.  Must match AMI_POOL_PAYLOAD in
 * include/aminetxduo/netstack.h (1568 = 1500 MTU + 14 Ethernet + slack,
 * longword aligned).  NetX Duo itself takes this from nx_packet_pool_create();
 * it is recorded here so a driver can size a bounce buffer without pulling in
 * the AmiNetXDuo headers.
 */
#define NX_AMIGA_POOL_PAYLOAD                   1568

/*
 * Leave NX_PHYSICAL_HEADER at its default of 16 rather than trimming it to the
 * 14 bytes an Ethernet header actually needs.  With a longword-aligned payload
 * the 14-byte header then starts at +2, which puts the IP header on a longword
 * boundary -- worth having on 68020, where a longword access at an odd-word
 * address costs an extra bus cycle, and mandatory the day this runs on a
 * strict-alignment host.  The SANA-II shim relies on the same offset.
 */

/* Packets may still be chained: a TCP receive of more than one payload's worth
   arrives as a chain, and NX_DISABLE_PACKET_CHAIN would silently truncate it. */


/* ----------------------------------------------------------- interfaces --- */

/*
 * Two physical interfaces: one SANA-II device plus room for a second (a PPP
 * or SLIP unit alongside Ethernet is a normal Amiga configuration).  Each
 * costs an NX_INTERFACE in every NX_IP, so this is not free -- raise it only
 * with a measurement.  The loopback interface is separate and always present.
 */
#define NX_MAX_PHYSICAL_INTERFACES              2


/* ------------------------------------------------------------------ ARP --- */

/*
 * Queue at most two packets per unresolved ARP entry (default 4).  Each queued
 * packet is a whole pool buffer held hostage while the ARP resolves, and on a
 * 16-packet floor pool losing eight of them to one unreachable host is not
 * acceptable.
 */
#define NX_ARP_MAX_QUEUE_DEPTH                  2

/*
 * Retry an unanswered ARP 8 times rather than 18.  At the default one-second
 * update rate that is 8 s before giving up instead of 18 s, which is closer to
 * what Amiga software (and users) expect from a stack.
 */
#define NX_ARP_MAXIMUM_RETRIES                  8


/* ------------------------------------------------------------------ TCP --- */

/*
 * Cap the transmit queue at 8 packets per socket (default 20).  20 in-flight
 * packets is 20 * 1568 = 30 KB of pool per socket, which on the floor target
 * is more than the entire pool.
 */
#define NX_TCP_MAXIMUM_TX_QUEUE                 8

/*
 * How many ports may be listened on at once (default 10).
 *
 * This is a hard ceiling on listen(), not a soft one: the eleventh
 * nx_tcp_server_socket_listen() returns NX_MAX_LISTEN, which bsdsocket
 * reports as ENOBUFS, and no amount of closing other kinds of socket helps.
 * Ten was enough while the only server in the tree was a test; it is not
 * enough for the tools this stack exists to run.  `ssh -L` opens one listener
 * per forward, an ftp client opens one per active-mode transfer, and `nc -l`
 * plus anything else at all is already two.
 *
 * Cost is 44 bytes per entry (NX_TCP_LISTEN, with extended notify on) inside
 * the single NX_IP, so 10 -> 32 is under a kilobyte of BSS, once.
 */
#define NX_MAX_LISTEN_REQUESTS                  32

/*
 * Turn on the extended notify callbacks.
 *
 * Without this, NX_DISABLE_EXTENDED_NOTIFY_SUPPORT is what nx_api.h defines
 * for us, and nx_tcp_socket_establish_notify() /
 * nx_tcp_socket_disconnect_complete_notify() compile to a stub returning
 * NX_NOT_SUPPORTED.  bsdsocket.library then has no way to be told that a
 * non-blocking connect() completed or that a disconnect finished, and has to
 * derive both by reading nx_tcp_socket_state on every readiness poll --
 * i.e. WaitSelect() polls where it should be sleeping.
 *
 * With it, four call sites in the TCP state machine reach us directly:
 *   nx_tcp_socket_state_syn_sent.c      establish  (client connect complete)
 *   nx_tcp_socket_state_syn_received.c  establish  (server handshake done)
 *   nx_tcp_socket_connection_reset.c    disconnect (RST -- connect refused)
 *   nx_tcp_socket_state_fin_wait{1,2}/closing/last_ack.c
 *                                       disconnect (orderly close complete)
 *
 * Cost: four function pointers (16 bytes) per NX_TCP_SOCKET, and the two
 * extra branches per received segment that guard them.  It also changes the
 * NX_TCP_SOCKET layout, so it is an ABI break for anything compiled against
 * the old header -- everything here is built from one tree, but that is why
 * this is not a per-file define.
 */
#define NX_ENABLE_EXTENDED_NOTIFY_SUPPORT

/*
 * ACK EVERY SECOND FULL-SIZED SEGMENT (RFC 1122, 4.2.3.2).
 *
 * NetX Duo does not do this by default -- NX_TCP_ACK_EVERY_N_PACKETS is not
 * defined anywhere in the vendored tree, so the whole `need_ack` block in
 * nx_tcp_socket_state_data_check.c is compiled out and the rule is simply
 * absent.  What is left is two triggers, and NEITHER of them is per-segment:
 *
 *   1. a window-update ACK, sent only once the receive window has re-opened
 *      by HALF OF nx_tcp_socket_rx_window_default
 *      (nx_tcp_socket_state_data_check.c:1135, nx_tcp_socket_receive.c:212);
 *   2. the 200 ms delayed-ACK timer.
 *
 * So the interval between acknowledgements is PROPORTIONAL TO THE WINDOW, and
 * when the application cannot consume half a window inside 200 ms the timer
 * becomes the pacer.  That makes the obvious tuning knob actively harmful:
 * measured in tests/trace/ on the A1200 profile, 524288 bytes over the wire,
 * raising the receive window from 8 KB to 32 KB and changing nothing else
 *
 *      161 KB/s -> 89 KB/s,        ACK delay p50 6.7 ms -> 71.4 ms
 *                                  p90 8.7 ms -> 187.4 ms
 *                                  26 of 59 ACKs in the 200 ms delayed bucket
 *                                  a 14-deep run of duplicate ACKs
 *                                  one 1361 ms gap between data segments
 *
 * with ZERO retransmissions throughout -- nothing was lost, the sender was
 * simply waiting.  With this defined the same 32 KB build returns to
 * 179 KB/s and ACK delay p50 2.0 ms, 148 of 208 ACKs inside 2 ms.
 *
 * At the current 8 KB window it changes throughput by ~0% -- half of 8192 is
 * already about three segments -- but it cuts ACK latency from a 6.7 ms
 * median to 2.0 ms, which is what every request/response exchange pays
 * (HTTP, DNS, and each leg of a TLS handshake), and it removes the trap.
 *
 * Cost: one ULONG per NX_TCP_SOCKET (already in the struct, already
 * initialised by nx_tcp_socket_create.c:154) and one comparison per received
 * data segment.
 */
#define NX_TCP_ACK_EVERY_N_PACKETS              2

/*
 * RETRANSMIT WITH EXPONENTIAL BACKOFF (RFC 6298 5.5), AND THE TRAP IN DOING IT.
 *
 * NetX Duo's retransmission interval is
 *
 *     timeout = nx_tcp_socket_timeout_rate << (retries * NX_TCP_RETRY_SHIFT)
 *
 * (nx_tcp_socket_retransmit.c:188, nx_tcp_fast_periodic_processing.c:150).
 * NX_TCP_RETRY_SHIFT defaults to 0, so the shift is a no-op and the interval is
 * NX_IP_PERIODIC_RATE / NX_TCP_TRANSMIT_TIMER_RATE -- one second, FOREVER.
 * tests/tcpdrill measured it directly: SYN retransmissions at 890 ms and then
 * 1002 ms.  There is no RTT estimator anywhere in the vendored tree either, so
 * the interval is not merely constant, it is constant at a number nobody chose
 * for this path.
 *
 * A shift of 1 doubles it each time, which is what RFC 6298 5.5 asks for.
 * NX_TCP_MAXIMUM_RETRIES CANNOT BE LEFT ALONE WHILE DOING THAT, and this is the
 * whole reason the two are set together.  There is NO CEILING in NetX Duo --
 * the expression above is a plain shift with no clamp, and NX_TCP_MAXIMUM_RETRIES
 * is the only thing bounding it.  At the default of 10 the intervals run
 * 1, 2, 4 ... 1024 seconds and the socket does not give up for
 * 2^11 - 1 = 2047 s.  A one-second stall would have become a THIRTY-FOUR MINUTE
 * one.
 *
 * Six retries with a shift of 1 gives
 *
 *     1  2  4  8  16  32  64 seconds     abandon at 127 s
 *
 * which lands on three numbers that are each defensible rather than round:
 *
 *   * the largest single interval is 64 s, against RFC 6298 2.5's rule that a
 *     maximum RTO "MAY be placed provided it is at least 60 seconds";
 *   * the connection is abandoned at 127 s, against RFC 1122 4.2.3.5's R2 of
 *     "at least 100 seconds" for data;
 *   * and it is 12.7x the 10 s this stack gave up after before, not 200x.
 *
 * The last point is the one that would otherwise bite: the same counter bounds
 * SYN retransmission, so every connect() to a host that is not answering now
 * blocks for 127 s rather than 10 before the stack resets the socket.  That is
 * what TCP is supposed to do and it is what every other stack does, but it is a
 * visible behaviour change and it is why the number is 6 and not 8.
 */
/* #ifndef, so an arm that answers "was it this?" can be built from one tree with
   -DNX_TCP_RETRY_SHIFT=0 -DNX_TCP_MAXIMUM_RETRIES=10 and no edit here. */
#ifndef NX_TCP_RETRY_SHIFT
#define NX_TCP_RETRY_SHIFT                      1
#endif
#ifndef NX_TCP_MAXIMUM_RETRIES
#define NX_TCP_MAXIMUM_RETRIES                  6
#endif

/*
 * TCP KEEPALIVE, BECAUSE setsockopt(SO_KEEPALIVE) WAS ALREADY SAYING YES.
 *
 * src/bsdsocket/options.c accepted SO_KEEPALIVE, stored it in a socket flag and
 * reported it back through getsockopt() -- and nothing anywhere acted on it,
 * because NX_TCP_ENABLE_KEEPALIVE (the old spelling) was not defined and the
 * whole of nx_tcp_periodic_processing.c's keepalive block was compiled out.  An
 * API that answers correctly with no implementation behind it is the exact
 * shape of the last four defects found in this tree, and it is worse than an
 * API that returns ENOPROTOOPT: a program that asks for keepalive and is told
 * yes has no way to find out that its half-open connections will never be
 * reaped.
 *
 * With this, an ESTABLISHED socket that has been idle for
 * NX_TCP_KEEPALIVE_INITIAL seconds sends a probe -- an ACK carrying
 * tx_sequence - 1, which is a deliberately backward sequence number the peer
 * must answer (nx_tcp_periodic_processing.c:125) -- and after
 * NX_TCP_KEEPALIVE_RETRIES unanswered probes at NX_TCP_KEEPALIVE_RETRY seconds
 * the socket is reset.  The defaults are BSD's and are left alone: 7200 s
 * (two hours) initial, 75 s retry, 10 retries.
 *
 * ONE THING HAD TO CHANGE IN OUR CODE, and it is not optional.
 * nx_tcp_socket_create.c:166 sets nx_tcp_socket_keepalive_enabled = NX_TRUE
 * UNCONDITIONALLY under this define, so turning it on alone would put every
 * socket on keepalive whether the application asked or not -- which is not what
 * SO_KEEPALIVE means and not what BSD does.  src/bsdsocket/socket.c clears it
 * at create and options.c is what sets it, so the default is off and the option
 * is the only way on.
 *
 * Cost: three ULONGs per NX_TCP_SOCKET, and one decrement per socket per second
 * in the IP thread's one-second periodic -- which already walks the same list.
 */
#define NX_ENABLE_TCP_KEEPALIVE

/*
 * REJECT A SYN THAT ADVERTISES AN ABSURD MSS.
 *
 * Without this, nx_tcp_packet_process.c takes whatever MSS a peer's SYN
 * carries.  A peer advertising 1 makes every segment we send one byte of
 * payload with forty bytes of header, from a socket the application believes is
 * working; NX_TCP_MAXIMUM_TX_QUEUE (8) then bounds us at eight bytes in flight.
 * That is a denial of service that costs the other end one packet, and nothing
 * in the trace would look like an error.
 *
 * With it, a SYN whose MSS is below NX_TCP_MSS_MINIMUM (128, the default, kept)
 * is answered with a RESET and counted in nx_ip_tcp_invalid_packets.  A peer
 * that offers NO MSS option at all is unaffected -- the code substitutes the
 * interface-derived default before this check -- so nothing conformant is
 * refused.
 *
 * Cost: one comparison per incoming SYN.
 */
#define NX_ENABLE_TCP_MSS_CHECK

/*
 * RFC 1323 / 7323 WINDOW SCALING IS OFF, AND IT WAS MEASURED RATHER THAN
 * ASSUMED.  docs/RESEARCH.md 28.2 is the write-up; this is the short version,
 * because "NX_ENABLE_TCP_WINDOW_SCALING exists and we do not define it" reads
 * as an oversight and is not one.
 *
 * Build with -DAMINETXDUO_TCP_WINDOW_SCALING=ON to turn it on.  What that gets
 * you, measured on the A1200 profile over 524,288 bytes, three arms out of one
 * tree:
 *
 *   loopback   351 KB/s -> 351 KB/s.  Not "about the same": the two traces
 *              agree segment for segment -- 128 segments, the same 25088 and
 *              16725 advertised windows, 8192 bytes in flight against 8533,
 *              the same p50 gap to 0.1 ms, 315838 vs 315873 bytes/s.
 *   wire       unchanged, and unchanged for a reason the trace states rather
 *              than a coincidence: the peer never holds more than 2880 bytes
 *              outstanding against the window it is already offered (16.5).
 *
 * IT CANNOT DO ANYTHING ELSE HERE, and this is structural rather than a
 * property of one workload.  The scale factor NetX Duo computes is the
 * smallest shift that brings the receive window under 65536
 * (nx_tcp_packet_send_syn.c:292).  ami_bsd_tcp_window() draws every window
 * from one eighth of the packet pool, and the pool tops out at
 * AMI_POOL_MAX_PACKETS (256), so the largest window ANY socket can be given is
 * 256/8 * 1568 = 50,176 bytes -- with the ceiling removed entirely.  50,176 is
 * under 65,536, so THE NEGOTIATED SCALE IS ALWAYS ZERO.  The SYN confirms it:
 * with this on, `tcpdump -r` reads `options [mss 1460,wscale 0,eol]`.
 *
 * The option itself is free -- NetX Duo builds a fixed eight-byte option area
 * and the scale replaces the `nop,nop,nop,eol` padding, so the SYN is 24 bytes
 * either way and no other segment carries options at all.  What is not free is
 * 12 bytes per NX_TCP_SOCKET and a shift on every segment sent, retransmitted
 * and acknowledged, for a factor that is always zero.
 *
 * AND IT REMOVES A GUARD.  nxe_tcp_socket_create.c:170 rejects a window above
 * 65535 with NX_OPTION_ERROR while this is off, and accepts anything under
 * 2^30 while it is on.  That guard is worth keeping, because a window that
 * large is actively harmful on this machine and the trace says so: pinning
 * AMINETXDUO_TCP_WINDOW at 65536 -- the smallest value that makes the scale
 * non-zero, and the arm that proves the mechanism works, `wscale 1` in the SYN
 * -- took the wire from 172 KB/s to 32 KB/s, with 15 retransmitted segments
 * and a nine-deep duplicate-ACK run where the shipped configuration has none
 * of either.  There is no SACK in the vendored tree (16.7), so a burst loss
 * inside a big window costs a full go-back-N, and 24.3's pool budget is the
 * arithmetic that stops it happening.  With scaling off, that misconfiguration
 * fails at socket() instead of running five times slower.
 *
 * So this stays off until something changes the arithmetic: SACK, or a pool
 * budget that can offer one socket more than 64 KB.  Neither is close.
 */
#ifdef AMINETXDUO_TCP_WINDOW_SCALING
#define NX_ENABLE_TCP_WINDOW_SCALING
#endif


/* ------------------------------------------------------------- SOCK_RAW -- */

/*
 * What bsdsocket.library's SOCK_RAW is built on (src/bsdsocket/raw.c).
 *
 * NX_ENABLE_IP_RAW_PACKET_FILTER adds the `nx_ip_raw_packet_filter` hook to
 * NX_IP and makes nx_ip_raw_packet_filter_set() something other than a stub
 * returning NX_NOT_SUPPORTED.
 *
 * NX_ENABLE_IP_RAW_PACKET_ALL_STACK is the one that matters, and it is not
 * documented in nx_user_sample.h -- it appears only in
 * nx_ip_dispatch_process.c.  Without it the raw hook is consulted ONLY in the
 * "protocol I do not recognise" branch, after TCP, UDP, ICMP and IGMP have all
 * been dispatched, so a raw ICMP socket could never see an echo reply and
 * `ping`, `traceroute` and bsdsocktest tests 3 and 132-136 would be
 * unreachable by construction.  With it, the filter is called first for every
 * inbound IP packet, and normal dispatch continues unless the filter claims
 * the packet.
 *
 * Our filter never claims one -- it copies what a raw socket asked for and
 * declines -- so ICMP echo replies still reach nx_icmp_ping(), echo requests
 * are still answered, and TCP and UDP are untouched.  It is installed only
 * while at least one SOCK_RAW descriptor is open, so the per-packet cost on a
 * machine that has none is a NULL pointer test.
 *
 * NX_ENABLE_IP_RAW_PACKET_FILTER changes the layout of NX_IP, so like
 * NX_ENABLE_IP_PACKET_FILTER below it has to be seen by every translation
 * unit and therefore lives here rather than on a target.
 */
#define NX_ENABLE_IP_RAW_PACKET_FILTER
#define NX_ENABLE_IP_RAW_PACKET_ALL_STACK


/* ---------------------------------------------------------- capture ------ */

/*
 * The IP-level capture hook, and the ONLY way loopback traffic can be traced.
 *
 * The SANA-II taps in src/sana2/ see every frame that crosses a wire, which is
 * everything -- except loopback, because NetX Duo's loopback interface has
 * `nx_interface_link_driver_entry == NX_NULL` (nx_ip_create.c:157) and
 * _nx_ip_driver_packet_send() shortcuts a loopback destination straight into
 * _nx_ip_packet_deferred_receive().  No driver is called, so no tap on the
 * driver can ever fire, and the fastest path in the stack -- the one every
 * throughput number in docs/RESEARCH.md 11 was measured on -- would have been
 * the one path with no instrument on it.
 *
 * This adds two function pointers to NX_IP and two branches per packet in each
 * direction.  Because it changes the NX_IP layout it MUST be seen by every
 * translation unit, which is why it lives here and not on a target.
 */
#define NX_ENABLE_IP_PACKET_FILTER


/* ------------------------------------------------------------- routing --- */

/*
 * COMPILE THE IPv4 ROUTING TABLE IN.
 *
 * NX_IP_ROUTING_TABLE_SIZE below is inert on its own, and used to be set here
 * without this -- which reads as though routes existed and is the exact shape
 * of thing that costs an afternoon.  Without the enable:
 *
 *   * NX_IP has no nx_ip_routing_table[] and no
 *     nx_ip_routing_table_entry_count (nx_api.h:2972);
 *   * nx_ip_static_route_add()/delete() compile to stubs returning
 *     NX_NOT_SUPPORTED (nx_ip_static_route_{add,delete}.c);
 *   * _nx_ip_route_find() skips the table lookup entirely
 *     (nx_ip_route_find.c:150), so the only next hops that exist are the
 *     directly-attached prefix of each interface and the single default
 *     gateway.
 *
 * One gateway is enough for a machine on one Ethernet, which is why this was
 * never noticed.  It is not enough for the two configurations Amiga users
 * actually build: a second interface (PPP or SLIP alongside Ethernet -- the
 * reason NX_MAX_PHYSICAL_INTERFACES is 2 above) reachable only through its own
 * next hop, and a VPN or a second subnet behind a router that is not the
 * default one.  Both need "this prefix goes via that address", which is a
 * route and cannot be expressed as a gateway.
 *
 * With it, _nx_ip_route_find() consults the table BEFORE falling back to the
 * default gateway and picks the longest prefix that matches, so a route is
 * capable of overriding the gateway for part of the address space -- which is
 * what makes it worth having rather than a second name for one.
 *
 * Cost: NX_IP_ROUTING_TABLE_SIZE * sizeof(NX_IP_ROUTING_ENTRY) (16 bytes each,
 * so 64) plus a count, inside the single NX_IP, and a walk of at most four
 * entries per transmitted packet whose destination is not on-link.  It changes
 * the layout of NX_IP, so like the packet filters above it must be seen by
 * every translation unit and belongs here rather than on a target.
 *
 * What this switches on above it: NETSTATUS_ROUTES now reports the table as
 * well as the interface prefixes and the gateway, NETCTRL_ROUTE_ADD/DELETE
 * stop returning ENOSYS, NETSTATUS_SYS_ROUTING is set, and AddNetRoute /
 * DeleteNetRoute exist at all (src/tools/).
 */
#define NX_ENABLE_IP_STATIC_ROUTING

/*
 * Four static routes (default 8).  Roadshow-era configurations have a default
 * gateway and occasionally one or two additions.
 */
#define NX_IP_ROUTING_TABLE_SIZE                4


/* -------------------------------------------------------------- resolver -- */

/*
 * CACHE DNS ANSWERS.
 *
 * addons/dns has had this all along; nxd_dns.h ships the define commented out
 * and nothing here uncommented it, so every lookup went to the wire -- every
 * one, including the second lookup of a name resolved a moment earlier, which
 * is what a shell session, an FTP transfer and a redirect-following fetch all
 * do.
 *
 * With it, _nx_dns_host_resource_data_by_name_get() and
 * _nx_dns_host_by_address_get_internal() consult the cache BEFORE binding a
 * socket, so a hit costs a mutex and a linear walk and puts no packet on the
 * wire at all.  It caches what the server said, honours the TTL the server
 * sent (aged from tx_time_get() against NX_IP_PERIODIC_RATE), and replaces the
 * least recently used record when it is full -- so a cache too small for a
 * workload degrades to the behaviour it replaced rather than to a failure.
 * Forward (A, AAAA) and reverse (PTR) lookups share it.
 *
 * Cost inside NX_DNS: a pointer, a size and three counters.  THE CACHE ITSELF
 * IS THE CALLER'S -- src/netstack/netstack_dns.c owns the buffer and states
 * there how big it is and why.  Without a call to nx_dns_cache_initialize()
 * this define is inert: the code compiles in, dns_ptr->nx_dns_cache stays
 * NULL, and every lookup goes to the wire exactly as before.
 *
 * This does not change what DEVS:Internet/hosts does.  netstack_resolve()
 * still consults the file first and never reaches NetX Duo for a name that is
 * in it, so a hosts entry cannot be shadowed by a cached answer.
 */
#define NX_DNS_CACHE_ENABLE


/* ---------------------------------------------------------------- IPv6 ---- */

/*
 * IPv6 is a build option, not a default (docs/RESEARCH.md 9).  The root
 * CMakeLists keeps the nx_icmpv6/nx_ipv6/nx_nd objects out of the floor build;
 * disabling it here as well stops the dual-stack code paths being compiled
 * into the IPv4 objects.  Define AMINETXDUO_IPV6 to build the dual stack.
 */
#ifndef AMINETXDUO_IPV6
#define NX_DISABLE_IPV6
#else

/*
 * The dual stack, sized for the same 68020/4 MB floor as everything else.
 * NetX Duo's IPv6 defaults assume an embedded target that has nothing else to
 * spend RAM on; every table below is a fixed array inside the single NX_IP, so
 * these are the difference between an NX_IP that costs ~3 KB extra and one
 * that costs ~9 KB.  Measured with sizeof(NX_IP) at build time.
 */

/*
 * Neighbour cache (default 16).  This is the IPv6 equivalent of the ARP cache,
 * and it is sized by the same argument: an Amiga on a home LAN talks to a
 * router and a handful of hosts.  Each ND_CACHE_ENTRY carries a 16-byte
 * address, a MAC, timers and a queued-packet pointer.
 */
#define NX_IPV6_NEIGHBOR_CACHE_SIZE             8

/*
 * Destination cache (default 8).  One entry per off-link destination in use;
 * four matches NX_IP_ROUTING_TABLE_SIZE above, which is the IPv4 equivalent
 * decision.
 */
#define NX_IPV6_DESTINATION_TABLE_SIZE          4

/*
 * Default router list (default 8).  RFC 4861 requires at least one; a home
 * link with two advertising routers is already unusual.
 */
#define NX_IPV6_DEFAULT_ROUTER_TABLE_SIZE       2

/* On-link prefix list (default 8).  One RA typically advertises one prefix. */
#define NX_IPV6_PREFIX_LIST_TABLE_SIZE          4

/*
 * Addresses per NX_IP.  The default is NX_MAX_PHYSICAL_INTERFACES * 3, which
 * with two interfaces is 6: link-local + one autoconfigured global + one
 * static per interface.  That is exactly the budget AmiNetXDuo needs, so the
 * default stands and is spelled out rather than redefined.
 *
 *   NX_MAX_IPV6_ADDRESSES  == NX_MAX_PHYSICAL_INTERFACES * 3 == 6
 *
 * ::1 lives in a slot of its own (NX_LOOPBACK_IPV6_ENABLED) and is not
 * counted here; nxd_ipv6_enable() configures it unconditionally, which is why
 * loopback IPv6 works with no interface at all.
 */

/*
 * Duplicate Address Detection stays ON (NX_DISABLE_IPV6_DAD is NOT defined).
 *
 * It costs three neighbour solicitations and roughly one second per address
 * before that address becomes usable, which is a real delay at startup.  It is
 * kept because the alternative is silently sharing an address with another
 * host on the link -- and because it is the one part of neighbour discovery
 * that exercises solicited-node multicast on every boot, so turning it off
 * would also remove the only routine test of the S2_ADDMULTICASTADDRESS path
 * in src/sana2/.
 */

/*
 * Router solicitation stays ON (NX_DISABLE_ICMPV6_ROUTER_SOLICITATION is NOT
 * defined): stateless autoconfiguration is the configuration mode this port
 * expects to be used on a real network, and it starts with an RS.
 */

/*
 * Make stateless autoconfiguration switchable per interface.
 *
 * Without this, SLAAC is UNCONDITIONAL: nx_icmpv6_process_ra.c forms a global
 * address from any advertised prefix and does not consult a status flag, and
 * nxd_ipv6_stateless_address_autoconfig_{enable,disable}() compile to stubs
 * returning NX_NOT_SUPPORTED.  CONFIGURE6=LINKLOCAL and CONFIGURE6=STATIC
 * would then be lies -- an interface configured either way would still take a
 * global address off the wire the moment a router advertised one.
 *
 * Cost: one ULONG per NX_INTERFACE (8 bytes across the two we allow) and one
 * comparison per prefix option in a received router advertisement.  The
 * default when the field is zeroed is ENABLED (0), which is why AUTO works
 * without calling enable() at all -- but src/netstack/netstack_ipv6.c calls it
 * anyway, so the intent is in the code rather than in the initialisation.
 */
#define NX_IPV6_STATELESS_AUTOCONFIG_CONTROL

/*
 * Deliberately NOT set, and why:
 *
 *   NX_ENABLE_IPV6_MULTICAST     -- the application-level
 *                                   nxd_ipv6_multicast_interface_join() API.
 *                                   Neighbour discovery does not need it:
 *                                   solicited-node group joins go through
 *                                   _nx_ipv6_multicast_join() from
 *                                   nxd_ipv6_address_set(), which reaches the
 *                                   driver as NX_LINK_MULTICAST_JOIN either
 *                                   way.  Nothing in bsdsocket.library exposes
 *                                   IPv6 multicast membership yet, so this
 *                                   would be code with no caller.
 *   NX_ENABLE_IPV6_PATH_MTU_DISCOVERY -- adds a periodic sweep of the
 *                                   destination table for a benefit that only
 *                                   shows up on paths with a smaller MTU than
 *                                   the 1500 a SANA-II Ethernet device
 *                                   reports.
 *   NX_IPSEC_ENABLE              -- out of scope; §9 decision 4 lists the four
 *                                   optional subsystems and this is not one.
 */

#endif /* AMINETXDUO_IPV6 */


/*
 * Deliberately NOT set, and why:
 *
 *   NX_DISABLE_ERROR_CHECKING   -- saves code, but the whole point of the
 *                                  bring-up milestones is to catch our own
 *                                  misuse.  Revisit for the release build.
 *   NX_DISABLE_PACKET_CHAIN     -- would break TCP receives larger than one
 *                                  payload.
 *   NX_DISABLE_FRAGMENTATION    -- fragmentation is already off unless
 *                                  nx_ip_fragment_enable() is called.
 *   NX_ENABLE_INTERFACE_CAPABILITY -- checksum offload; no SANA-II device
 *                                  exposes it.
 *   NX_TCP_ENABLE_KEEPALIVE     -- off by default; bsdsocket's SO_KEEPALIVE
 *                                  will need it, so it turns on with the
 *                                  socket layer, not before.
 */

#endif /* NX_USER_H */

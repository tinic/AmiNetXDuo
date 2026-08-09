/*
 * AmiNetXDuo, NetX Duo tuning for the AmigaOS floor target.
 *
 * Target (docs/RESEARCH.md 9, decision 1): 68020, OS 3.1, 4 MB.  NetX Duo's
 * defaults assume an embedded target with a static memory budget; on an Amiga
 * the stack shares memory with everything else on the machine.  Every value
 * here departs from the default; if a value is not listed, the default stands.
 *
 * Sizes shared with the rest of AmiNetXDuo (packet payload, pool bounds) live
 * in include/aminetxduo/netstack.h and are mirrored, not redefined, here.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef NX_USER_H
#define NX_USER_H


/* ---------------------------------------------------------------- timing, */

/*
 * One tick is 20 ms.  Must equal TX_TIMER_TICKS_PER_SECOND in
 * port/threadx-amiga/inc/tx_port.h: NetX Duo expresses its own rates as
 * divisors of this, so a disagreement silently scales every TCP timer by the
 * ratio.
 *
 * 50 Hz rather than 100 to match the platform (AmiTCP and its descendants:
 * `#define hz (50)`), an order of magnitude above the BSD protocol timers
 * above us, pfslowtimo at hz/2 (500 ms), pffasttimo at hz/5 (200 ms).  The
 * tick task wakes on timer.device UNIT_VBLANK and takes its time from
 * ReadEClock(), so the rate holds on PAL, NTSC, RTG and accelerated systems.
 */

#define NX_IP_PERIODIC_RATE                     50


/* --------------------------------------------------------------- packets, */

/*
 * Payload of a pool packet.  Must match AMI_POOL_PAYLOAD in
 * include/aminetxduo/netstack.h (1568 = 1500 MTU + 14 Ethernet + slack,
 * longword aligned).  NetX Duo takes this from nx_packet_pool_create(); it is
 * recorded here so a driver can size a bounce buffer without pulling in the
 * AmiNetXDuo headers.
 */
#define NX_AMIGA_POOL_PAYLOAD                   1568

/*
 * NX_PHYSICAL_HEADER stays at its default of 16 rather than the 14 bytes an
 * Ethernet header needs.  With a longword-aligned payload the 14-byte header
 * starts at +2, putting the IP header on a longword boundary: on 68020 a
 * longword access at an odd-word address costs an extra bus cycle, and a
 * strict-alignment host would require it.  The SANA-II shim relies on the same
 * offset.
 */

/* Packets may be chained: a TCP receive of more than one payload's worth
   arrives as a chain, and NX_DISABLE_PACKET_CHAIN would silently truncate it. */


/* ----------------------------------------------------------- interfaces --- */

/*
 * Two physical interfaces: one SANA-II device plus room for a second, which is
 * a normal Amiga configuration.  Each costs an NX_INTERFACE in every NX_IP;
 * raise it only with a measurement.  The loopback interface is separate and
 * always present.
 */
#define NX_MAX_PHYSICAL_INTERFACES              2


/* ------------------------------------------------------------------ ARP --- */

/*
 * Queue at most two packets per unresolved ARP entry (default 4).  Each queued
 * packet is a whole pool buffer held while the ARP resolves; on a 16-packet
 * floor pool, losing eight to one unreachable host is too many.
 */
#define NX_ARP_MAX_QUEUE_DEPTH                  2

/*
 * Retry an unanswered ARP 8 times rather than 18.  At the default one-second
 * update rate that is 8 s before giving up instead of 18 s.
 */
#define NX_ARP_MAXIMUM_RETRIES                  8


/* ------------------------------------------------------------------ TCP --- */

/*
 * Cap the transmit queue at 8 packets per socket (default 20).  20 in-flight
 * packets is 20 * 1568 = 30 KB of pool per socket, more than the entire floor
 * pool.
 */
#define NX_TCP_MAXIMUM_TX_QUEUE                 8

/*
 * How many ports may be listened on at once (default 10).
 *
 * A hard ceiling on listen(): the eleventh nx_tcp_server_socket_listen()
 * returns NX_MAX_LISTEN, which bsdsocket reports as ENOBUFS, and closing other
 * kinds of socket does not help.  `ssh -L` opens one listener per forward, an
 * ftp client one per active-mode transfer, and `nc -l` plus anything else is
 * already two.
 *
 * Cost is 44 bytes per entry (NX_TCP_LISTEN, with extended notify on) inside
 * the single NX_IP, so 10 -> 32 is under a kilobyte of BSS, once.
 */
#define NX_MAX_LISTEN_REQUESTS                  32

/*
 * Turn on the extended notify callbacks.
 *
 * Without this, nx_api.h defines NX_DISABLE_EXTENDED_NOTIFY_SUPPORT for us and
 * nx_tcp_socket_establish_notify() /
 * nx_tcp_socket_disconnect_complete_notify() compile to a stub returning
 * NX_NOT_SUPPORTED.  bsdsocket.library then cannot be told that a non-blocking
 * connect() completed or that a disconnect finished, and has to derive both by
 * reading nx_tcp_socket_state on every readiness poll, so WaitSelect() polls
 * where it could sleep.
 *
 * With it, four call sites in the TCP state machine reach us directly:
 *   nx_tcp_socket_state_syn_sent.c      establish  (client connect complete)
 *   nx_tcp_socket_state_syn_received.c  establish  (server handshake done)
 *   nx_tcp_socket_connection_reset.c    disconnect (RST, connect refused)
 *   nx_tcp_socket_state_fin_wait{1,2}/closing/last_ack.c
 *                                       disconnect (orderly close complete)
 *
 * Cost: four function pointers (16 bytes) per NX_TCP_SOCKET, and the two extra
 * branches per received segment that guard them.  It changes the
 * NX_TCP_SOCKET layout, so it is an ABI break for anything compiled against the
 * old header, and cannot be a per-file define.
 */
#define NX_ENABLE_EXTENDED_NOTIFY_SUPPORT

/*
 * ACK every second full-sized segment (RFC 1122, 4.2.3.2).
 *
 * NX_TCP_ACK_EVERY_N_PACKETS is not defined anywhere in the vendored tree, so
 * by default the `need_ack` block in nx_tcp_socket_state_data_check.c is
 * compiled out.  Two triggers remain, neither per-segment: a window-update ACK
 * once the receive window has re-opened by half of
 * nx_tcp_socket_rx_window_default (nx_tcp_socket_state_data_check.c:1135,
 * nx_tcp_socket_receive.c:212), and the 200 ms delayed-ACK timer.  ACK interval
 * is therefore proportional to the window, and the timer paces it whenever the
 * application cannot consume half a window inside 200 ms, so raising the
 * window alone is harmful.  tests/trace/, A1200 profile, 524288 bytes over the
 * wire, 8 KB -> 32 KB receive window and nothing else changed: 161 -> 89 KB/s,
 * ACK delay p50 6.7 -> 71.4 ms, p90 8.7 -> 187.4 ms, 26 of 59 ACKs in the
 * 200 ms delayed bucket, a 14-deep duplicate-ACK run, one 1361 ms gap between
 * data segments, and no retransmissions.  With this defined the same 32 KB
 * build returns to 179 KB/s and p50 2.0 ms, 148 of 208 ACKs inside 2 ms.
 *
 * At the current 8 KB window throughput is unchanged, half of 8192 is already
 * about three segments, but ACK latency drops from a 6.7 ms median to 2.0 ms,
 * which every request/response exchange pays (HTTP, DNS, each leg of a TLS
 * handshake).
 *
 * Cost: one ULONG per NX_TCP_SOCKET (already in the struct, already initialised
 * by nx_tcp_socket_create.c:154) and one comparison per received data segment.
 */
#define NX_TCP_ACK_EVERY_N_PACKETS              2

/*
 * Retransmit with exponential backoff (RFC 6298 5.5).
 *
 *     timeout = nx_tcp_socket_timeout_rate << (retries * NX_TCP_RETRY_SHIFT)
 *
 * (nx_tcp_socket_retransmit.c:188, nx_tcp_fast_periodic_processing.c:150).
 * NX_TCP_RETRY_SHIFT defaults to 0, so the shift is a no-op and the interval is
 * NX_IP_PERIODIC_RATE / NX_TCP_TRANSMIT_TIMER_RATE, one second, forever.
 * tests/tcpdrill measured SYN retransmissions at 890 ms and then 1002 ms.
 * There is no RTT estimator anywhere in the vendored tree either.
 *
 * The two settings go together because the expression has no clamp, so
 * NX_TCP_MAXIMUM_RETRIES is the only bound.  A shift of 1 with the default of
 * 10 retries runs 1, 2, 4 ... 1024 seconds and does not give up for
 * 2^11 - 1 = 2047 s.  Six retries gives 1 2 4 8 16 32 64 seconds, abandoning at
 * 127 s: the largest single interval satisfies RFC 6298 2.5's "at least 60
 * seconds" maximum RTO, and 127 s satisfies RFC 1122 4.2.3.5's R2 of "at least
 * 100 seconds" for data.  The same counter bounds SYN retransmission, so
 * connect() to a host that is not answering now blocks for 127 s rather than
 * 10, a visible behaviour change, and why the number is 6 and not 8.
 *
 * The six was unreachable at first, for a reason outside this file:
 * _nx_tcp_fast_periodic_processing() tests the limit against
 * nx_tcp_socket_zero_window_probe_failure instead of the retry count whenever
 * the socket is marked as probing a zero window, and
 * nx_tcp_socket_send_internal() set that mark for any send it could not queue,
 * including one blocked by the congestion window or the transmit queue depth.
 * A caller that keeps offering data, bsd_wait_sliced(), every 200 ms,
 * re-armed the mark faster than the ladder doubled, so the limit was never
 * read, and an impaired link retransmitted at +1, +2, +4 ... +128 s without
 * giving up.  Both are fixed in the vendored fork;
 * tests/netstack/host/test_tcp_retries_host.c covers it in 0.3 s without a
 * network.
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
 * R2 for a connection request, RFC 1122 4.2.3.5 MUST-23: at least 3 minutes.
 * The six above give 127 s, which satisfies the same section's R2 for data,
 * "at least 100 seconds", and not MUST-23.
 *
 * Seven with the shift capped at 6 gives 1 2 4 8 16 32 64 64 seconds of waiting
 * seven retransmissions, the last at 127 s, and abandons at 191 s.  The cap
 * is what keeps this off 255: without it the seventh interval alone is 128
 * seconds.  Data keeps the six and its 127 s, because a transfer that has
 * stopped should report sooner than a connection that has not started.
 *
 * WHAT A USER SEES.  connect() to a host that is not answering now blocks for
 * 191 s rather than 127.  It is interruptible throughout, bsd_wait_sliced()
 * samples the break signal every 200 ms, and SO_SNDTIMEO bounds it for a
 * program that would rather not wait.  Linux's tcp_syn_retries default is 6,
 * which is the 127 s this replaces; FreeBSD gives up sooner still.
 *
 * -DNX_TCP_SYN_MAXIMUM_RETRIES=6 puts it back where it was.
 */
#ifndef NX_TCP_SYN_MAXIMUM_RETRIES
#define NX_TCP_SYN_MAXIMUM_RETRIES              7
#endif

/*
 * The round-trip time estimator of RFC 6298 2 and 3, which the ladder above
 * previously had to do without: nx_tcp_socket_timeout_rate was assigned
 * _nx_tcp_transmit_timer_rate once at socket create and never moved again, so
 * every socket on every path waited the same second before deciding a segment
 * was lost.
 *
 * With this, one segment per window is timed, the acknowledgment covering it
 * gives R, and SRTT/RTTVAR produce the base the ladder shifts.  Karn's
 * algorithm discards the sample when the segment was retransmitted, so an
 * ambiguous acknowledgment never moves the estimate.
 *
 * On this lab's links the estimate is under 2.4's one-second floor and the
 * result is the second we already had; what it buys is the long path, where a
 * fixed second retransmits data that was never lost.
 *
 * Costs 20 bytes per NX_TCP_SOCKET and one comparison per acknowledgment.
 *
 * NX_TCP_RTO_MINIMUM_MS is 2.4's floor and stays at the conformant 1000.
 * NX_TCP_RTO_MAXIMUM_MS is 2.5's ceiling, which the ladder then shifts above.
 *
 * Build with -DAMINETXDUO_TCP_RTT=OFF to take it out.
 */
#ifndef AMINETXDUO_TCP_RTT_OFF
#define NX_ENABLE_TCP_RTT_ESTIMATOR
#endif

/*
 * TCP keepalive, because setsockopt(SO_KEEPALIVE) was already accepting it.
 * src/bsdsocket/options.c stored the flag and reported it back through
 * getsockopt(), but NX_TCP_ENABLE_KEEPALIVE (the old spelling) was not defined,
 * so the keepalive block in nx_tcp_periodic_processing.c was compiled out and
 * a program told yes never had its half-open connections reaped.
 *
 * With this, an ESTABLISHED socket idle for NX_TCP_KEEPALIVE_INITIAL seconds
 * sends a probe, an ACK carrying tx_sequence - 1, a backward sequence number
 * the peer must answer (nx_tcp_periodic_processing.c:125), and after
 * NX_TCP_KEEPALIVE_RETRIES unanswered probes at NX_TCP_KEEPALIVE_RETRY seconds
 * the socket is reset.  BSD's defaults are kept: 7200 s initial, 75 s retry,
 * 10 retries.
 *
 * nx_tcp_socket_create.c:166 sets nx_tcp_socket_keepalive_enabled = NX_TRUE
 * unconditionally under this define, which would put every socket on keepalive
 * whether the application asked or not.  src/bsdsocket/socket.c therefore
 * clears it at create and options.c sets it, so the default is off and
 * SO_KEEPALIVE is the only way on.
 *
 * Cost: three ULONGs per NX_TCP_SOCKET, and one decrement per socket per second
 * in the IP thread's one-second periodic, which already walks the same list.
 */
#define NX_ENABLE_TCP_KEEPALIVE

/*
 * Refuse a datagram whose SOURCE address cannot be one.
 *
 * RFC 1122 3.2.1.3: a source of the subnet broadcast, of the network address,
 * or in class D is invalid, and nx_ipv4_packet_receive.c:344-371 tests exactly
 * those three, behind this define, which nothing in this port set. The check
 * was dead code in every build we have ever shipped, while
 * docs/CONFORMANCE.md listed martian-source filtering as verified conformant.
 * The claim is now true.
 *
 * It is guarded on nx_interface_address_mapping_needed, so it applies to the
 * Ethernet interfaces and not to loopback, which is where a source of our own
 * address legitimately arrives.
 *
 * Cost: three compares on the receive path, only for a source that is not an
 * ordinary host address.
 */
#define NX_ENABLE_SOURCE_ADDRESS_CHECK

/*
 * Reject a SYN that advertises an absurd MSS.
 *
 * Without this, nx_tcp_packet_process.c takes whatever MSS a peer's SYN
 * carries.  A peer advertising 1 makes every segment one byte of payload with
 * forty bytes of header, and NX_TCP_MAXIMUM_TX_QUEUE (8) then bounds us at
 * eight bytes in flight, a denial of service costing the other end one
 * packet, with nothing in the trace looking like an error.
 *
 * With it, a SYN whose MSS is below NX_TCP_MSS_MINIMUM (128, the default, kept)
 * is answered with a reset and counted in nx_ip_tcp_invalid_packets.  A peer
 * that offers no MSS option is unaffected: the code substitutes the
 * interface-derived default before this check.
 *
 * Cost: one comparison per incoming SYN.
 */
#define NX_ENABLE_TCP_MSS_CHECK

/*
 * RFC 1323 / 7323 section 2, window scaling.
 *
 * It used to be off with two conditions on it: until SACK existed, or until a
 * pool budget could offer one socket more than 64 KB.  Both are met.
 * AMINETXDUO_TCP_SACK has been on by default since the receive side landed,
 * and BSD_TCP_WINDOW_POOL_SHARE is 4, which makes the largest window this
 * stack can produce (AMI_POOL_MAX_PACKETS / 4) * AMI_POOL_PAYLOAD = 100,352
 * bytes.  At the eighth it was, that number was 50,176 -- under the 65,535 the
 * header field holds -- so the negotiated scale was zero on every machine and
 * the option did nothing at all, which is what the earlier measurement of it
 * was measuring.
 *
 * WHY THE TWO CONDITIONS WERE THERE.  Pinning AMINETXDUO_TCP_WINDOW at 65536
 * with no SACK in the tree took the wire from 172 KB/s to 32 KB/s, with 15
 * retransmitted segments and a nine-deep duplicate-ACK run: a burst loss
 * inside a big window cost a full go-back-N, and the receiver had no way to
 * say which segment was missing.  It does now.
 *
 * The option is not free: 12 bytes per NX_TCP_SOCKET and a shift on every
 * segment sent, retransmitted and acknowledged.  On the SYN it takes a word
 * that was padding, and pushes SACK-Permitted into a third -- see
 * NX_TCP_RWIN_OPTION, whose own padding byte used to end the option list and
 * take SACK-Permitted and the timestamp off the wire with it.
 *
 * It also removes a guard: nxe_tcp_socket_create.c:170 rejects a window above
 * 65535 with NX_OPTION_ERROR while this is off and accepts anything under 2^30
 * while it is on.  The pool budget is what bounds it now.
 *
 * Build with -DAMINETXDUO_TCP_WINDOW_SCALING=OFF to take it out.
 */
#ifdef AMINETXDUO_TCP_WINDOW_SCALING
#define NX_ENABLE_TCP_WINDOW_SCALING
#endif

/*
 * RFC 1323 section 3, timestamps.
 *
 * Two things come with it and one bill.  RTTM samples every segment rather
 * than one per window and survives a retransmission, which Karn's algorithm
 * otherwise discards; and PAWS rejects a segment whose timestamp went
 * backwards, which is what protects a sequence space that wraps.
 *
 * The bill is twelve bytes off every segment carrying data, so a 1500-byte
 * link goes from a 1460 MSS to 1448, and both ends build and parse the option
 * on every packet.  At Amiga link rates the sequence space takes tens of
 * minutes to wrap, so PAWS is not what is being bought here; RTTM is.
 *
 * MEASURED, A1200 bridged to a real peer, 1 MB, five reps a boot and three
 * boots an arm, the arms alternating:
 *
 *     read    on 392.7 KB/s (390-396)   off 391.3 KB/s (387-402)
 *     write   on 413.7 KB/s (413-415)   off 431.7 KB/s (430-433)
 *
 * Read is the figure, and it does not move.  The write arm loses about four
 * per cent, which is where the twelve bytes and the per-segment option build
 * show up: that arm leaves the CPU 22% idle against the read arm's 60%.
 * bsdsocket.library goes 308,288 to 310,488 bytes.
 *
 * Build with -DAMINETXDUO_TCP_TIMESTAMP=OFF to take it out.
 */
#ifndef AMINETXDUO_TCP_TIMESTAMP_OFF
#define NX_ENABLE_TCP_TIMESTAMP
#endif


/* ----------------------------------------------------------------- SACK, */

/*
 * RFC 2018 selective acknowledgment, receive side.
 *
 * The SYN offers SACK-Permitted and a SYN-ACK repeats it only when the peer's
 * SYN carried it.  An acknowledgment that leaves a hole then carries the blocks
 * describing what is held above it, so the peer retransmits the hole rather
 * than everything after it.  Without this a single loss inside a window costs a
 * go-back-N, which is what keeps the receive window ceiling where it is.
 *
 * Sender-side processing of blocks the peer sends us is not implemented, so
 * writes recover exactly as they did.
 *
 * Costs one byte plus three of padding and one ULONG per NX_TCP_SOCKET, and on
 * an in-order stream one comparison per acknowledgment: the builder reads the
 * queue tail, sees nothing above the receive sequence, and returns.
 *
 * Build with -DAMINETXDUO_TCP_SACK=OFF to take it out.
 */
#ifndef AMINETXDUO_TCP_SACK_OFF
#define NX_ENABLE_TCP_SACK
#endif


/* ------------------------------------------------------------- SOCK_RAW, */

/*
 * What bsdsocket.library's SOCK_RAW is built on (src/bsdsocket/raw.c).
 *
 * NX_ENABLE_IP_RAW_PACKET_FILTER adds the `nx_ip_raw_packet_filter` hook to
 * NX_IP and makes nx_ip_raw_packet_filter_set() something other than a stub
 * returning NX_NOT_SUPPORTED.
 *
 * NX_ENABLE_IP_RAW_PACKET_ALL_STACK is not documented in nx_user_sample.h,
 * it appears only in nx_ip_dispatch_process.c.  Without it the raw hook is
 * consulted only in the "protocol I do not recognise" branch, after TCP, UDP,
 * ICMP and IGMP have all been dispatched, so a raw ICMP socket could never see
 * an echo reply and `ping`, `traceroute` and bsdsocktest tests 3 and 132-136
 * would be unreachable.  With it, the filter is called first for every inbound
 * IP packet, and normal dispatch continues unless the filter claims the packet.
 *
 * Our filter never claims one, it copies what a raw socket asked for and
 * declines, so ICMP echo replies still reach nx_icmp_ping(), echo requests
 * are still answered, and TCP and UDP are untouched.  It is installed only
 * while at least one SOCK_RAW descriptor is open, so the per-packet cost on a
 * machine that has none is a NULL pointer test.
 *
 * NX_ENABLE_IP_RAW_PACKET_FILTER changes the layout of NX_IP, so like
 * NX_ENABLE_IP_PACKET_FILTER below it has to be seen by every translation unit
 * and lives here rather than on a target.
 */
#define NX_ENABLE_IP_RAW_PACKET_FILTER
#define NX_ENABLE_IP_RAW_PACKET_ALL_STACK


/* ---------------------------------------------------------- capture ------ */

/*
 * The IP-level capture hook, and the only way loopback traffic can be traced.
 *
 * The SANA-II taps in src/sana2/ see every frame that crosses a wire, but not
 * loopback: NetX Duo's loopback interface has
 * `nx_interface_link_driver_entry == NX_NULL` (nx_ip_create.c:157) and
 * _nx_ip_driver_packet_send() shortcuts a loopback destination straight into
 * _nx_ip_packet_deferred_receive().  No driver is called, so no tap on the
 * driver can fire, and the fastest path in the stack, the one every
 * throughput number in docs/RESEARCH.md 11 was measured on, would have no
 * instrument on it.
 *
 * This adds two function pointers to NX_IP and two branches per packet in each
 * direction.  Because it changes the NX_IP layout it must be seen by every
 * translation unit, so it lives here and not on a target.
 */
#define NX_ENABLE_IP_PACKET_FILTER


/* ------------------------------------------------------------------- IP --- */

/*
 * The IP identification field: NX_ENABLE_IP_ID_RANDOMIZATION is off, and the
 * free half of it is done in src/netstack/ instead.
 *
 * Without the define, nx_ip_header_add.c:151 uses `ip_ptr -> nx_ip_packet_id++`
 * a global counter that nx_ip_create.c zeroes at startup and increments once
 * per transmitted IP datagram.  That is (1) a fingerprint, since the rate it
 * climbs at is a machine-wide packet counter readable from any single flow, and
 * (2) RFC 6274 5.1's idle scan, where an off-path attacker reads the ID this
 * machine answers with to learn how many packets it sent in between, using it
 * as a zombie to port-scan a third party.
 *
 * The define fixes both and costs 5% of loopback.  Measured, two arms out of
 * one tree (docs/RESEARCH.md 29.4), A1200, 524288 bytes:
 *
 *                       counter      randomised
 *      loopback          347 KB/s     329 KB/s      -5.2%
 *      loopback, capturing  305         290         -4.9%
 *      wire                171          167         -2.3%
 *
 * The two paths differ by the ratio of datagrams they send, loopback about
 * 130 a second, the wire path about 70, and 5.2/2.3 is that ratio, so roughly
 * 400 us per transmitted datagram.  The cost is NX_RAND, which nx_port.h maps
 * to ami_random_rand(): a SHA-256 hash DRBG with a Forbid()/Permit() pair per
 * draw and a SHA-256 pair per 32 bytes of output, one refill every eight calls.
 * That is right for TLS key material, ECDHE privates and TCP sequence numbers
 * and far too expensive for a 16-bit header field once per packet, and NetX Duo
 * uses the same NX_RAND macro everywhere, so a cheaper source cannot be picked
 * for this one field.
 *
 * Instead, src/netstack/ seeds nx_ip_packet_id from the DRBG once, when the
 * NX_IP is created: a single draw at startup, nothing per packet.  That removes
 * (1), since the counter no longer starts at zero, so its absolute value says
 * nothing about uptime or about how much this machine has sent.  It does not
 * remove (2): the delta between two observations is still a packet count, and
 * idle scan works on the delta.
 *
 * Build with -DAMINETXDUO_IP_ID_RANDOMIZATION to turn the define on and pay the
 * 5%.  On a network where an idle scan is a real threat that is a good trade;
 * on the 14 MHz floor target it is not the default.
 */
#ifdef AMINETXDUO_IP_ID_RANDOMIZATION
#define NX_ENABLE_IP_ID_RANDOMIZATION
#endif


/* ------------------------------------------------------------- routing --- */

/*
 * Compile the IPv4 routing table in.
 *
 * NX_IP_ROUTING_TABLE_SIZE below is inert on its own, and used to be set here
 * without this, which reads as though routes existed.  Without the enable:
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
 * One gateway is enough for a machine on one Ethernet, but not for a second
 * interface (the reason NX_MAX_PHYSICAL_INTERFACES is 2 above) reachable only
 * through its own next hop, or for a VPN or second subnet behind a router that
 * is not the default one.  Both need "this prefix goes via that address", which
 * cannot be expressed as a gateway.  With the enable, _nx_ip_route_find()
 * consults the table before falling back to the default gateway and picks the
 * longest matching prefix, so a route can override the gateway for part of the
 * address space.
 *
 * Cost: NX_IP_ROUTING_TABLE_SIZE * sizeof(NX_IP_ROUTING_ENTRY) (16 bytes each,
 * so 64) plus a count, inside the single NX_IP, and a walk of at most four
 * entries per transmitted packet whose destination is not on-link.  It changes
 * the layout of NX_IP, so like the packet filters above it must be seen by
 * every translation unit and belongs here rather than on a target.
 *
 * It also switches on: NETSTATUS_ROUTES reporting the table as well as the
 * interface prefixes and the gateway, NETCTRL_ROUTE_ADD/DELETE no longer
 * returning ENOSYS, NETSTATUS_SYS_ROUTING, and AddNetRoute / DeleteNetRoute
 * existing at all (src/tools/).
 */
#define NX_ENABLE_IP_STATIC_ROUTING

/*
 * Four static routes (default 8).  Roadshow-era configurations have a default
 * gateway and occasionally one or two additions.
 */
#define NX_IP_ROUTING_TABLE_SIZE                4


/* -------------------------------------------------------------- resolver, */

/*
 * Cache DNS answers.  addons/dns has had this all along, but nxd_dns.h ships
 * the define commented out and nothing here uncommented it, so every lookup
 * went to the wire, including the second lookup of a name resolved a moment
 * earlier, which is what a shell session, an FTP transfer and a
 * redirect-following fetch all do.
 *
 * With it, _nx_dns_host_resource_data_by_name_get() and
 * _nx_dns_host_by_address_get_internal() consult the cache before binding a
 * socket, so a hit costs a mutex and a linear walk and puts no packet on the
 * wire.  It honours the TTL the server sent (aged from tx_time_get() against
 * NX_IP_PERIODIC_RATE) and replaces the least recently used record when full,
 * so a cache too small for a workload degrades to the previous behaviour rather
 * than to a failure.  Forward (A, AAAA) and reverse (PTR) lookups share it.
 *
 * Cost inside NX_DNS: a pointer, a size and three counters.  The cache buffer
 * belongs to the caller, src/netstack/netstack_dns.c allocates it and states
 * there how big it is and why.  Without a call to nx_dns_cache_initialize()
 * this define is inert: dns_ptr->nx_dns_cache stays NULL and every lookup goes
 * to the wire as before.
 *
 * DEVS:Internet/hosts is unaffected.  netstack_resolve() consults the file
 * first and never reaches NetX Duo for a name that is in it, so a hosts entry
 * cannot be shadowed by a cached answer.
 */
#define NX_DNS_CACHE_ENABLE

/*
 * One pass over the server list per call, instead of three.
 *
 * _nx_dns_host_resource_data_by_name_get() takes the wait_option it is given as
 * a PER-QUERY timeout, then spends it NX_DNS_MAX_RETRIES times over every
 * configured server, doubling between rounds.  With the default 3 and the
 * thirty seconds bsdsocket.library asks for, one gethostbyname() against five
 * unreachable servers is 5 * (30 + 60 + 64) seconds, and the DNS mutex is
 * held for all of it, so every other task's lookup queues behind it.
 *
 * With 1 the call is one round, bounded by wait_option * servers, and the
 * retransmission ladder moves to src/netstack/netstack_retry.c where the break
 * signal can be sampled between rounds.  The wire behaviour is the same query
 * sequence; what changes is who is driving it.
 */
#define NX_DNS_MAX_RETRIES                      1


/* ------------------------------------------------------------------ DHCP, */

/*
 * ARP-probe the address the server hands out, and DHCPDECLINE if somebody
 * answers (RFC 2131 4.4.1, RFC 5227 2.1).
 *
 * addons/dhcp ships the whole of this, _nx_dhcp_ip_conflict() and
 * _nx_dhcp_interface_decline(), behind the define, and nxd_dhcp_client.h
 * leaves it commented out, so without a line here no probe and no DECLINE can
 * leave the machine and a duplicate address is taken silently.  AutoIP next
 * door does probe (nx_auto_ip.c), so link-local was compliant while DHCP was
 * not.
 *
 * The probes go out alongside the address rather than in front of it, so the
 * boot cost is nothing: NetX Duo used to hold the address off the interface
 * for NX_DHCP_ARP_PROBE_WAIT plus NX_DHCP_ARP_PROBE_NUM intervals of
 * NX_DHCP_ARP_PROBE_MIN..MAX, which is 3 to 6 seconds charged to whoever
 * brought the stack up -- AddNetInterface in the Startup-Sequence, since
 * bsdsocket.library brings the stack up on the first OpenLibrary().  The
 * timings below are unchanged and are still what goes on the wire.
 */
#define NX_DHCP_CLIENT_SEND_ARP_PROBE


/* ---------------------------------------------------------------- IPv6 ---- */

/*
 * IPv6 is a build option, not a default (docs/RESEARCH.md 9).  The root
 * CMakeLists keeps the nx_icmpv6/nx_ipv6/nx_nd objects out of the floor build;
 * disabling it here as well keeps the dual-stack code paths out of the IPv4
 * objects.  Define AMINETXDUO_IPV6 to build the dual stack.
 */
#ifndef AMINETXDUO_IPV6
#define NX_DISABLE_IPV6
#else

/*
 * The dual stack, sized for the same 68020/4 MB floor as everything else.
 * Every table below is a fixed array inside the single NX_IP, so these are the
 * difference between an NX_IP that costs ~3 KB extra and one that costs ~9 KB.
 * Measured with sizeof(NX_IP) at build time.
 */

/*
 * Neighbour cache (default 16).  The IPv6 equivalent of the ARP cache, sized by
 * the same argument: an Amiga on a home LAN talks to a router and a handful of
 * hosts.  Each ND_CACHE_ENTRY carries a 16-byte address, a MAC, timers and a
 * queued-packet pointer.
 */
#define NX_IPV6_NEIGHBOR_CACHE_SIZE             8

/*
 * Destination cache (default 8).  One entry per off-link destination in use;
 * four matches NX_IP_ROUTING_TABLE_SIZE above, the IPv4 equivalent.
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
 * static per interface.  That is the budget AmiNetXDuo needs, so the default
 * stands and is spelled out rather than redefined.
 *
 *   NX_MAX_IPV6_ADDRESSES  == NX_MAX_PHYSICAL_INTERFACES * 3 == 6
 *
 * ::1 lives in a slot of its own (NX_LOOPBACK_IPV6_ENABLED) and is not
 * counted here; nxd_ipv6_enable() configures it unconditionally, which is why
 * loopback IPv6 works with no interface at all.
 */

/*
 * Duplicate Address Detection stays on (NX_DISABLE_IPV6_DAD is not defined).
 * Kept because the alternative is silently sharing an address with another host
 * on the link, and because it is the one part of neighbour discovery that
 * exercises solicited-node multicast on every boot, the only routine test of
 * the S2_ADDMULTICASTADDRESS path in src/sana2/.
 *
 * Nobody waits for it.  An address is TENTATIVE while the solicitations go
 * out, and the answer arrives through the notification below rather than on
 * the thread that configured the address; see ami_ns6_address_changed().
 *
 * ONE solicitation, not NetX Duo's three.  RFC 4862 5.4.5 makes an address
 * valid RetransTimer after its last solicitation goes unanswered, and
 * _nx_icmpv6_perform_DAD() runs off the IP thread's one-second periodic, so
 * each transmit is a second and the wait after the last one is a second more:
 * three transmits is four seconds per address before anything may use it.  A
 * machine gets two of them, the link-local and the address a router
 * advertisement forms, so it was eight seconds of a bring-up.
 *
 * 1 is RFC 4862's own DupAddrDetectTransmits default, not a corner cut; three
 * is NetX Duo's.  A duplicate answers the first solicitation as readily as the
 * third -- the retransmissions are there for a lost packet, on a link where a
 * neighbour's reply is a single unicast frame it did not have to ask for.
 */
#ifndef NX_IPV6_DAD_TRANSMITS
#define NX_IPV6_DAD_TRANSMITS                   1
#endif

/*
 * Report what duplicate address detection decides, and what a router hands
 * out, instead of polling for it.
 *
 * Without this the notify field is not in NX_IP at all and
 * nxd_ipv6_address_change_notify() compiles to a stub returning
 * NX_NOT_SUPPORTED, which leaves an address's fate readable only by watching
 * nxd_ipv6_address_state change.  Watching it is what AddNetInterface used to
 * do, for three seconds per address, on the Startup-Sequence's thread.
 *
 * It also covers what no watcher at bring-up could see: an address formed
 * later from a router advertisement, which arrives long after the command that
 * configured the interface has returned.
 */
#define NX_ENABLE_IPV6_ADDRESS_CHANGE_NOTIFY

/*
 * Router solicitation stays on (NX_DISABLE_ICMPV6_ROUTER_SOLICITATION is not
 * defined): stateless autoconfiguration is the configuration mode this port
 * expects on a real network, and it starts with an RS.
 */

/*
 * Make stateless autoconfiguration switchable per interface.
 *
 * Without this, SLAAC is unconditional: nx_icmpv6_process_ra.c forms a global
 * address from any advertised prefix and does not consult a status flag, and
 * nxd_ipv6_stateless_address_autoconfig_{enable,disable}() compile to stubs
 * returning NX_NOT_SUPPORTED.  CONFIGURE6=LINKLOCAL and CONFIGURE6=STATIC would
 * then be wrong, an interface configured either way would still take a global
 * address off the wire the moment a router advertised one.
 *
 * Cost: one ULONG per NX_INTERFACE (8 bytes across the two we allow) and one
 * comparison per prefix option in a received router advertisement.  The default
 * when the field is zeroed is enabled (0), which is why AUTO works without
 * calling enable() at all; src/netstack/netstack_ipv6.c calls it anyway, so the
 * intent is in the code rather than in the initialisation.
 */
#define NX_IPV6_STATELESS_AUTOCONFIG_CONTROL

/*
 * IPV6_JOIN_GROUP and IPV6_LEAVE_GROUP, over
 * nxd_ipv6_multicast_interface_join()/_leave().  src/bsdsocket/mcast.c is the
 * caller; AMINETXDUO_MULTICAST is the same switch the IPv4 side answers to, so
 * the two families arrive and leave together and the floor drawer, which turns
 * it off, is not asked to carry either.
 *
 * Neighbour discovery does not need this.  Solicited-node joins go through
 * _nx_ipv6_multicast_join() from nxd_ipv6_address_set(), which reaches the
 * driver as NX_LINK_MULTICAST_JOIN and never touches the table below.
 *
 * WHAT IT COSTS.
 *
 * It grows NX_IP by nx_ipv6_multicast_entry[7] plus a count, 172 bytes,
 * spent whether or not anything joins, where nx_igmp_enable() grows it by
 * nothing, the IPv4 table being unconditional already.  With the 384-byte
 * membership table in mcast.c that is 556 bytes, and the whole feature is
 * 3,288 bytes of code on the default drawer, measured stripped.  The floor
 * drawer pays nothing: it has this switch off and AMINETXDUO_IPV6 off, and
 * either alone compiles all of it out.
 *
 * And there is still no MLD in this tree.  nx_mld.h exists and is a stub that
 * declares nothing; no nx_mld_*.c exists; no Multicast Listener Report is ever
 * built or sent.  So a join registers 33:33:xx:xx:xx:xx with the interface and
 * tells the stack to accept the group, and announces nothing on the wire.
 *
 * What decided it was the third fact, which is that without this define the
 * receive path drops every non-solicited-node IPv6 multicast datagram outright
 * (nx_ipv6_packet_receive.c, the NX_ENABLE_IPV6_MULTICAST arm around the join
 * list).  There is no partial capability to preserve: it is 172 bytes for
 * group reception, or no group reception.
 *
 * What silence on the wire costs is a question of scope, not of switches.
 * RFC 4541 section 3 requires an MLD snooping switch to forward FF02::/16 on
 * every port whatever its membership table says, precisely so that a node
 * which has not reported still receives neighbour discovery and the
 * link-scope service protocols; ff02::fb, ff02::c and ff02::1:3 are what an
 * Amiga program joins and all three are inside it.  A querying switch
 * therefore prunes none of them, and a router forwards none of them either,
 * so a report would change nothing for the groups that are used.
 *
 * Above link-local scope it would change everything: ff05:: and ff0e:: are
 * forwarded on membership and nothing here reports any, so a join of one of
 * those receives only what is already on the link.  IPV6_JOIN_GROUP does not
 * refuse them, refusing would break the on-link half, which works.
 *
 * MLD is a protocol, not a define: nx_mld.h is a 48-line stub that declares
 * nothing and there is no nx_mld_*.c to enable, so wanting it means writing
 * MLDv1, query reception, per-group report timers with the RFC 2710 random
 * delay, and a done message on leave, into the NetX fork.  That is a piece
 * of work for a scope no Amiga program asks for, and it is not a prerequisite
 * for this.
 */
#ifdef AMINETXDUO_MULTICAST
#define NX_ENABLE_IPV6_MULTICAST
#endif

/*
 * Path MTU Discovery.
 *
 * A SANA-II Ethernet device reports 1500 and the stack believes it.  When the
 * path is narrower than that, a tunnel, a bridge with a smaller segment in
 * it, a router that has been configured down, IPv6 gives the sender no way
 * to find out except this: routers do not fragment, they return an ICMPv6
 * Packet Too Big and drop the packet.  Without this define the message is not
 * even dispatched (nx_icmpv6_packet_process.c), so the stack retransmits the
 * same oversized packet forever.  What the application sees is a connection
 * that establishes, exchanges small packets, and then stalls dead on the first
 * full-size segment.
 *
 * The alternative was to cap every IPv6 send at the 1280 that RFC 8200
 * guarantees, which needs no code at all and cannot stall.  It was measured
 * against the same rig: 5.4% slower at the median, 9.2% at the mean, and 13%
 * more packets for the same bytes, on every IPv6 path, permanently,
 * including the ordinary one where 1500 works and nothing was ever wrong.
 *
 * WHAT IT COSTS.
 *
 * 1,104 bytes of code and 36 bytes of RAM per NX_IP: two ULONGs added to each
 * NX_IPV6_DESTINATION_ENTRY (path MTU and its ageing timer, 8 bytes across the
 * four entries configured above) plus the periodic-update hook in NX_IP.  The
 * sweep it installs runs once a second over four table entries and does
 * nothing at all until an entry sits below the link MTU.  The floor drawer
 * pays none of it: AMINETXDUO_IPV6 is off there.
 *
 * The receive path is only safe to enable with the RFC 8201 4 checks in
 * nx_icmpv6_process_packet_too_big.c, the minimum-MTU floor above all, since
 * without it one forged Packet Too Big carrying an MTU of 68 pins a
 * destination there for as long as the entry lives.  That was a defect the
 * vendored stack shipped with, harmless only because the message was never
 * dispatched.  Do not turn this on in a tree that does not have the fix.
 *
 * Reassembly is a separate matter and is still missing: an inbound fragment is
 * dropped unless nx_ip_fragment_enable() has been called.  Path MTU Discovery
 * makes that less likely to be reached, since a peer told the correct MTU has
 * less reason to fragment, but it does not fix it.
 */
#define NX_ENABLE_IPV6_PATH_MTU_DISCOVERY

/*
 * Recursive DNS servers out of a router advertisement, RFC 8106.
 *
 * The one thing standing between an IPv6-only link and a usable machine.  A
 * link with no IPv4 on it configures addresses and a default route from the
 * advertisement and stops there: there is no DHCPv6 in this build
 * (src/netstack/netstack_ipv6.c says why), and DEVS:Internet/name_resolution
 * parses a nameserver as a dotted quad, so an IPv6 resolver cannot even be
 * written down.  The machine comes up routable and cannot resolve a name.
 *
 * The option costs one else-if in nx_icmpv6_process_ra.c's option walk, which
 * skipped type 25 with everything else it does not know, and a callback field
 * on NX_IP.  src/netstack/netstack_dns.c takes it from there; what it does not
 * do is call the DNS client from the IP thread, which would deadlock against
 * a query already holding that client's mutex.
 */
#define NX_ENABLE_IPV6_RDNSS

/*
 * Not set, and why:
 *
 *   NX_IPSEC_ENABLE, out of scope; §9 decision 4 lists the four
 *                                   optional subsystems and this is not one.
 */

#endif /* AMINETXDUO_IPV6 */


/* ------------------------------------------------------------- loopback, */

/*
 * Do not checksum a packet that never leaves memory.
 *
 * This is the checksum-offload switch, and no SANA-II device offers offload.
 * The loopback interface does: nx_ip_create.c:169 sets every checksum bit in
 * nx_interface_capability_flag on NX_LOOPBACK_INTERFACE, unconditionally,
 * under this define and only under it.  So the define's effect here is not
 * about hardware at all, it is 127.0.0.1, where the sender computes a
 * checksum over a buffer and the receiver verifies it against the same bytes
 * in the same RAM, having crossed no wire that could have corrupted them.
 * BSD has treated lo0 this way for decades.
 *
 * One of the two checksums goes, not both: _nx_ip_driver_packet_send()
 * fills the field in on the looped-back copy so the packet on the receive
 * side is well formed, and it is the verification that is skipped.
 * tests/perf/perf_test.c counts it, 316 checksum calls over 518 KB become
 * 158 over 259 KB for the same 256 KB transfer.
 *
 * Measured, A1200 profile, 256 KB, two runs per arm agreeing to the KB/s:
 *
 *                            off        on
 *      loopback, drain      603      682 KB/s     +13.1%
 *      loopback, +extract   535      595          +11.2%
 *      simulated wire       229      224           -2.2%
 *
 * The wire loses because the branches are compiled in everywhere while the
 * flag is zero on every real interface, and because NX_PACKET grows by the
 * capability field.  A 2.2% cost on the wire against 12% on loopback is the
 * trade, and it is taken on a machine where `TCP:`, local services and the
 * conformance suite's throughput test all run over 127.0.0.1.  Reverting is
 * one line if a wire measurement ever says otherwise.
 *
 * It changes the layout of NX_INTERFACE and NX_PACKET, so like the packet
 * filters it must be seen by every translation unit and belongs here.  No
 * driver work is needed: nothing in the vendored tree asks a driver for its
 * capabilities, and nx_ip_interface_attach.c:154 zeroes the flag for every
 * attached interface, so a SANA-II device claims nothing by accident.
 */
#define NX_ENABLE_INTERFACE_CAPABILITY

/*
 * Deliver a broadcast this host sends to this host's own sockets too.
 *
 * Ethernet is simplex: a card does not hear its own transmissions, so a
 * broadcast leaving the A2065 reaches every machine on the LAN except this
 * one.  4.4BSD copies it back in ether_output() and Linux in ip_mc_output(),
 * which is why a discovery protocol that broadcasts a query and answers it
 * from a server on the same machine works everywhere else.  Fitz is one:
 * `fitz query` broadcasts LIST to 255.255.255.255:17710 and its own
 * `fitz serve` binds that port, so without this the machine running both is
 * the one machine that cannot see its own share.
 *
 * Upstream NetX Duo only loops back a unicast to our own address and, if the
 * application asked for it with nx_igmp_loopback_enable(), a multicast.  The
 * define is ours (third_party/netxduo, branch amiga-ipv4-broadcast-loopback)
 * and is off upstream, so nothing else changes by enabling it here.
 *
 * Cost: one _nx_packet_copy() from the default pool per broadcast sent, held
 * until the receive side is done with it, an AMI_POOL_PAYLOAD packet, 1568
 * bytes of payload plus the NX_PACKET header, and a memcpy of the datagram.
 * Broadcasts are name lookups and DHCP, not a data path.  On a copy failure
 * the packet still goes out on the wire and only the local copy is lost.
 *
 * It does not change the layout of NX_IP or NX_PACKET, but the switch belongs
 * with the rest of the stack's configuration rather than on one target.
 */
#define NX_ENABLE_IP_BROADCAST_LOOPBACK


/*
 * Not set, and why:
 *
 *   NX_DISABLE_ERROR_CHECKING, saves code, and the _nxe_ wrappers are 30%
 *                                  of an nx_packet_allocate/release pair (90
 *                                  us against 63).  It is still not worth it:
 *                                  NetX Duo's own internals call _nx_ and
 *                                  never see a wrapper, so only our call
 *                                  sites pay, and an arm built with it
 *                                  measured 580/216 KB/s against 584/216,
 *                                  no change outside the noise.  The bring-up
 *                                  milestones exist to catch our own misuse
 *                                  and now cost nothing measurable.
 *   NX_DISABLE_PACKET_CHAIN, would break TCP receives larger than one
 *                                  payload.
 *   NX_DISABLE_FRAGMENTATION, src/netstack/ calls
 *                                  nx_ip_fragment_enable(), so inbound
 *                                  reassembly is wanted and this would
 *                                  compile it out.  What it holds is bounded
 *                                  by NX_IP_FRAGMENT_POOL_RESERVE in the
 *                                  fork rather than by the pool alone.
 *   NX_TCP_ACK_TIMER_RATE 25, a 40 ms delayed ACK rather than 200 ms,
 *                                  which is what AmiTCP_NG 4.1.4 did.  It
 *                                  needs NX_TCP_FAST_TIMER_RATE raised with
 *                                  it, since the fast periodic is what
 *                                  decrements the timeout, and that runs the
 *                                  whole socket list 2.5x as often.  Measured
 *                                  as a cost with no return: 592/524/224
 *                                  against 603/535/229 KB/s.  The latency it
 *                                  would buy is already bought by
 *                                  NX_TCP_ACK_EVERY_N_PACKETS above, which
 *                                  put ACK delay at a 2.0 ms median.
 *   NX_TCP_MAXIMUM_TX_QUEUE 16 , twice the in-flight depth.  Also a cost:
 *                                  592/528/224 against 603/535/229.  On a
 *                                  machine that is CPU-bound rather than
 *                                  window-bound (docs/RESEARCH.md 64) more
 *                                  packets in flight is more pool held for
 *                                  the same throughput.
 */


/*
 * Five more surveyed and rejected; docs/RESEARCH.md 29.3 has the working.
 *
 *   NX_DISABLE_ARP_AUTO_ENTRY
 *       Every ARP we see creates a cache entry (nx_arp_packet_receive.c:490),
 *       the classic poisoning surface, but disabling that does not close it:
 *       nx_arp_packet_receive.c updates an existing entry from any ARP it sees
 *       whether this is defined or not, and this define does not touch that
 *       path.  The auto entry is only created for a sender we have no entry
 *       for, from a broadcast ARP request, on a home LAN, the gateway asking
 *       for us, so turning it off costs an ARP request, a queued packet out
 *       of AMI_ARP_MAX_QUEUE_DEPTH (2) and a round trip on the next outbound
 *       packet to an unresolved next hop.  What would change the answer: an ARP
 *       cache that distinguishes "learned from a request addressed to us" from
 *       "learned from anything on the wire", or
 *       NX_ENABLE_ARP_MAC_CHANGE_NOTIFICATION below growing a consumer that can
 *       refuse a change.  Neither exists in the vendored tree.
 *
 *   NX_ENABLE_ARP_MAC_CHANGE_NOTIFICATION
 *       Adds a callback when a cached entry's MAC changes, a gateway being
 *       replaced, or impersonated.  It is a notification only:
 *       nx_arp_packet_receive.c:418 calls it after writing the new address, so
 *       nothing it does can refuse the change, and no handler in this tree
 *       would act on it.  It would add a function pointer to NX_IP and a branch
 *       per received ARP for a callback that logs.  Revisit when something can
 *       act, a static-ARP pin for the gateway, or a warning in ShowNetStatus.
 *
 *   NX_ENABLE_PACKET_DEBUG_INFO
 *       Records the file and line each packet was allocated at, in the
 *       NX_PACKET itself.  Relevant to the packet-ownership defects this
 *       project keeps finding, but rejected as a permanent setting: two
 *       pointers in every one of up to 256 pool packets, on a pool sized from
 *       AvailMem on a 4 MB machine.  It belongs behind a build option next to
 *       the debug log level, and that option does not exist yet.
 *
 *   NX_ENABLE_DUAL_PACKET_POOL
 *       Lets TCP allocate control packets (SYN, ACK, FIN, RST, no payload)
 *       from a second, smaller pool so a full data pool cannot stop the stack
 *       acknowledging.  docs/RESEARCH.md 24.8 established that there is one
 *       pool here for a reason, and a second takes memory permanently away from
 *       the 4 MB floor to protect against an exhaustion that 24.3's arithmetic
 *       already prevents.  An ACK that cannot be allocated is also a symptom of
 *       a data pool already empty, and the data is what was lost.
 *
 *   NX_ENABLE_LOW_WATERMARK
 *       docs/RESEARCH.md 24.7 found this and reported it rather than switching
 *       it on.  It needs three things together: the define, an
 *       nx_packet_pool_low_watermark_set() call from src/netstack/
 *       (nx_packet_pool_create() never touches the field, so a zeroed watermark
 *       means the guard is compiled in and can never fire), and
 *       NX_TCP_MAXIMUM_RX_QUEUE raised, because at its default of 20 and
 *       1440-byte wire segments it binds at about 28 KB, before a 32 KB window
 *       does, and the tail-drop it would then perform costs a retransmission
 *       this stack has no SACK to recover cheaply.  It also changes IPv4
 *       fragment reassembly and UDP receive, not only TCP.  That is a piece of
 *       work with its own measurement, not a line here.
 */

#endif /* NX_USER_H */

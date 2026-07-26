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


/* ------------------------------------------------------------- routing --- */

/*
 * Four static routes (default 8).  Roadshow-era configurations have a default
 * gateway and occasionally one or two additions.
 */
#define NX_IP_ROUTING_TABLE_SIZE                4


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

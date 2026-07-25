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
#endif


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

/*
 * bsdsocket.library, the receive window the packet pool can back.
 *
 * A socket may only advertise what the pool it draws from can hold.  Kept
 * here, next to the pool arithmetic it reads, because the two are one
 * mechanism: raising the pool floor in v0.25.5 was a change to this window.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_BSDSOCKET_WINDOW_H
#define AMINETXDUO_BSDSOCKET_WINDOW_H

#include <exec/types.h>

#include "aminetxduo/pool.h"

#ifndef BSD_TCP_WINDOW
#define BSD_TCP_WINDOW        8192
#endif

#ifndef BSD_TCP_WINDOW_POOL_SHARE
#define BSD_TCP_WINDOW_POOL_SHARE   8
#endif

/*
 * TWO WINDOWS PER SOCKET: the one it opens with, and the one it may grow to.
 *
 * A window is what the peer may put in flight at once, and the right size
 * depends on the path, which a socket does not know when it is created.  On
 * a LAN the round trip is under a millisecond and a fast peer sends a whole
 * window back to back; the card's receive ring has to absorb that burst at
 * wire speed while the CPU drains it at its own -- measured 2026-09-15 on
 * the emulated X-Surf 100 (A3000, 128 MB): a 262,144-byte window received
 * at 24-28 Mbit/s where 100,352 received at 39-40, three interleaved rounds
 * each, the bigger window losing to its own bursts.  Across the Internet the
 * same window is what the transfer rate IS: at 22 ms, 100,352 bytes cannot
 * exceed 36 Mbit/s and bursts are paced by the far end's link anyway.
 *
 * So a socket is created at BSD_TCP_WINDOW_LAN -- exactly the window every
 * socket had before this, the pool's eighth-share budget capped at what a
 * 512-packet pool gave -- and carries BSD_TCP_WINDOW_MAX as the size the
 * fork negotiates its window scale for.  When the socket's own connect
 * handshake measures a round trip of BSD_TCP_WINDOW_GROW_RTT_MS or more
 * (select.c, the establish notify), the window grows to the maximum in one
 * step, before any data has arrived.  A passive socket (accept) has no
 * handshake this side times and stays at the LAN window; a machine whose
 * budget is under the LAN window never sees any of this.
 *
 * Without the scale option the field itself is the ceiling: sixteen bits on
 * the wire, 65535 (nxe_tcp_socket_create.c:170), and there is nothing to
 * grow to.
 *
 * 262,144 is 100 Mbit/s over 21 ms and the GENET's 128 x 2 KB ring, a claim
 * about links and rings, not a derivation from the pool.  The 10 ms
 * threshold is a coarse but safe line: the real A1200's LAN round trip is
 * 1-5 ms, the emulator's under 1, the Internet's 20 and up.
 */
#ifndef BSD_TCP_WINDOW_LAN
#define BSD_TCP_WINDOW_LAN      100352UL    /* (512 / 8) * 1568, the old ceiling */
#endif

#ifndef BSD_TCP_WINDOW_MAX
#define BSD_TCP_WINDOW_MAX      262144UL
#endif

#ifndef BSD_TCP_WINDOW_GROW_RTT_MS
#define BSD_TCP_WINDOW_GROW_RTT_MS  10UL
#endif

/*
 * THE LAN WINDOW ON A GIGABIT LINK, measured 2026-09-15 on the A1200 +
 * PiStorm32 through anxgenet.device, iperf into the Amiga, three rounds a
 * boot.  The first table, before the receive offload:
 *
 *     window      receive
 *      50,176     134 Mbit/s
 *      75,264     134
 *     100,352      62-68
 *
 * A 1 Gbit peer puts the whole window on the wire at once, and what was
 * lost above 75 KB was NOT the driver's ring: it was the READ QUEUE.  The
 * shim posted 32 CMD_READs, the driver emptied its 128-frame ring in one
 * bottom half, a frame into a posted read each, and every frame past the
 * 32nd of a burst went nowhere -- found with the receive offload's own
 * counter.  Two things changed that day: the shim posts 128 reads on a
 * gigabit wire (sana2_internal.h, AMI_SANA2_RX_MAX_DEPTH), and the driver
 * holds a burst's tail in its ring for the reader instead of dropping it
 * (genet.c, the held pass; ANXD_CMD_RX_POLL).  With both, receive offload
 * on, same machine, same test, no frame lost at any row:
 *
 *     window      receive
 *      65,535     186 / 186 / 186
 *     100,352     197 / 197 / 197
 *     262,144     202 / 202 / 202
 *
 * So a socket that comes up on a link of BSD_TCP_WINDOW_FAST_BPS or more
 * settles at its maximum, exactly as a long path does: the ring backs the
 * burst, the window is the rate.  A socket on a slower link keeps
 * BSD_TCP_WINDOW_LAN, which the 100 Mbit X-Surf 100 (emulated) took at
 * 39-40 Mbit/s where 262,144 gave 27 -- that card has no ring behind it.
 * BSD_TCP_WINDOW_FAST, the 65,535 the first table forced, is gone.
 */
#ifndef BSD_TCP_WINDOW_FAST_BPS
#define BSD_TCP_WINDOW_FAST_BPS     1000000000UL
#endif

/*
 * The window a socket settles at once its handshake is done.  `created` is
 * what it opened with, `maximum` what it may grow to, `bps` the link it came
 * up on (0 = unknown), `rtt_ms` the handshake's round trip (0 = not
 * measured, which a passive socket cannot).  Pure arithmetic, host-tested.
 */
ULONG ami_bsd_tcp_window_settle(ULONG created, ULONG maximum, ULONG bps,
                                ULONG rtt_ms);

/*
 * The window a card can absorb: `hw_bytes` is what the driver holds from
 * the wire with nobody draining it (ANXD_CMD_RX_CAPACITY, 0 = not stated),
 * `mss` the segment the peer will send.  A peer on the same LAN puts the
 * whole window on the wire at once, and on a card that cannot pause the
 * wire every byte of it has to fit in that memory while the CPU catches up
 * -- a 25 MHz 68030 behind an X-Surf 100's 13 KB ring drained 100,352
 * bytes at 2.8 Mbit/s, 42 overruns and 42 chip resets in ten seconds.
 * Whole frames: the ring stores each segment with its headers in 256-byte
 * pages, so the number is frames-that-fit times the payload of one,
 * floored at two segments so the connection keeps moving.  Pure
 * arithmetic, host-tested.
 */
ULONG ami_bsd_tcp_window_fit(ULONG window, ULONG hw_bytes, ULONG mss);

#ifndef BSD_TCP_WINDOW_CEILING
#ifdef AMINETXDUO_TCP_WINDOW_SCALING
#define BSD_TCP_WINDOW_CEILING  BSD_TCP_WINDOW_MAX
#else
#define BSD_TCP_WINDOW_CEILING  65535UL
#endif
#endif

/*
 * The bytes the pool sets aside for TCP receive: one share of it.  This is
 * the number a window has to fit inside; anything larger is advertised and
 * cannot be stored.
 */
ULONG ami_bsd_tcp_budget(ULONG pool_packets, ULONG payload);

/*
 * That budget divided between the sockets that will draw on it, floored at
 * BSD_TCP_WINDOW.  `consumers` is how many sockets are already live, so the
 * caller's own is the +1.  _for() is the window a socket opens with, capped
 * at BSD_TCP_WINDOW_LAN and the wire's ceiling; _max_for() is the one it may
 * grow to, capped at BSD_TCP_WINDOW_CEILING, and never below _for().
 */
ULONG ami_bsd_tcp_window_for(ULONG pool_packets, ULONG payload,
                             ULONG consumers);
ULONG ami_bsd_tcp_window_max_for(ULONG pool_packets, ULONG payload,
                                 ULONG consumers);

#endif /* AMINETXDUO_BSDSOCKET_WINDOW_H */

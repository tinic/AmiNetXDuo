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

#ifndef BSD_TCP_WINDOW_CEILING
#ifdef AMINETXDUO_TCP_WINDOW_SCALING
#define BSD_TCP_WINDOW_CEILING                                                \
    (((ULONG)AMI_POOL_MAX_PACKETS / (ULONG)BSD_TCP_WINDOW_POOL_SHARE) *        \
     (ULONG)AMI_POOL_PAYLOAD)
#else
/*
 * Without the option, the field itself is the ceiling. The window goes on the
 * wire in sixteen bits and there is nothing to scale it by, so 65535 is what
 * the wire format allows rather than a policy. nxe_tcp_socket_create.c:170
 */
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
 * That budget divided between the sockets that will draw on it, clamped to
 * BSD_TCP_WINDOW..BSD_TCP_WINDOW_CEILING.  `consumers` is how many sockets
 * are already live, so the caller's own is the +1.
 */
ULONG ami_bsd_tcp_window_for(ULONG pool_packets, ULONG payload,
                             ULONG consumers);

#endif /* AMINETXDUO_BSDSOCKET_WINDOW_H */

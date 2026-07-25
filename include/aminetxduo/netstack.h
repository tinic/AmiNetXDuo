/*
 * AmiNetXDuo -- the stack singleton.
 *
 * One AmiNetStack exists per machine. It owns the ThreadX kernel, the NetX Duo
 * NX_IP instance, the packet pool, the DHCP/DNS clients and the SANA-II
 * interfaces. bsdsocket.library brings it up on first OpenLibrary() and tears
 * it down when the last opener closes and the interfaces go offline.
 *
 * Threading: every entry point here may be called from any Exec task. Startup
 * and shutdown are serialised by an internal semaphore and are idempotent.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_NETSTACK_H
#define AMINETXDUO_NETSTACK_H

#include <exec/types.h>
#include "aminetxduo/config.h"

#include "tx_api.h"
#include "nx_api.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AmiNetStack AmiNetStack;

#define AMI_NET_OK              0
#define AMI_NET_ERR_NOMEM      (-1)
#define AMI_NET_ERR_NODEV      (-2)   /* SANA-II device would not open        */
#define AMI_NET_ERR_CONFIG     (-3)
#define AMI_NET_ERR_KERNEL     (-4)   /* ThreadX/NetX Duo refused to start    */
#define AMI_NET_ERR_STATE      (-5)

/*
 * Bring the stack up (idempotent, reference-counted). Reads the config, starts
 * ThreadX, creates the packet pool and NX_IP, attaches interfaces, runs DHCP
 * where configured. Blocks until the first interface has an address or the
 * DHCP timeout expires.
 */
LONG netstack_startup(VOID);

/* Drop a reference; the stack goes down when the count reaches zero. */
VOID netstack_shutdown(VOID);

/* The singleton, or NULL if the stack is not up. */
AmiNetStack   *netstack_get(VOID);

/* Accessors -- all return NULL when the stack is down. */
NX_IP          *netstack_ip(VOID);
NX_PACKET_POOL *netstack_pool(VOID);
const AmiConfig *netstack_config(VOID);

/*
 * Packet pool sizing. The 68020/4 MB floor (docs/RESEARCH.md §9) means we
 * cannot use NetX Duo's embedded defaults blindly -- these are computed from
 * AvailMem() at startup and clamped to the range below.
 */
#define AMI_POOL_PAYLOAD        1568        /* 1500 MTU + 14 eth + slack, 4-aligned */
#define AMI_POOL_MIN_PACKETS    16
#define AMI_POOL_MAX_PACKETS    256

/*
 * Interface handles. Index 0..count-1 in config order; the loopback interface
 * is always present and is not counted here.
 */
UWORD   netstack_interface_count(VOID);
LONG    netstack_interface_up(UWORD index);
LONG    netstack_interface_down(UWORD index);
BOOL    netstack_interface_is_up(UWORD index);

/*
 * Resolver. Implemented over NetX Duo addons/dns; used by gethostbyname and
 * friends in bsdsocket.library. Blocking, with the timeout in ticks.
 */
LONG    netstack_resolve(const char *name, ULONG *addr_out, ULONG timeout_ticks);
LONG    netstack_resolve_reverse(ULONG addr, char *name_out, ULONG name_len,
                                 ULONG timeout_ticks);

#ifdef __cplusplus
}
#endif

#endif /* AMINETXDUO_NETSTACK_H */

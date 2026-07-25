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

/* ------------------------------------------------------ the ThreadX bracket
 *
 * NetX Duo checks who is calling: roughly forty of its entry points are
 * wrapped in NX_THREADS_ONLY_CALLER_CHECKING and return NX_CALLER_ERROR
 * unless tx_thread_identify() is non-NULL. An Exec Task that ThreadX has
 * never adopted fails every one of them, so *everything* that touches a NetX
 * Duo API -- the netstack itself, bsdsocket.library, the tools -- has to
 * bracket the call.
 *
 * This is public rather than private to src/netstack/ precisely so there is
 * exactly one bracket: bsdsocket.library used to carry a second, equivalent
 * implementation on the port's tx_amiga.h because it could not reach this one.
 *
 * `caller` is caller-owned storage that must stay valid until
 * ami_netstack_leave(). Brackets nest: a nested enter() finds the task is
 * already a ThreadX thread, borrows the context and leaves it alone.
 *
 * Nothing inside a bracket may block on anything except ThreadX -- an adopted
 * task holds the ThreadX baton, so an exec Wait() inside one stops the IP
 * thread and every other stack user until it returns.
 */
typedef struct AmiNetCaller
{
    TX_THREAD   nc_Thread;
    BOOL        nc_Adopted;
} AmiNetCaller;

LONG ami_netstack_enter(AmiNetCaller *caller);
VOID ami_netstack_leave(AmiNetCaller *caller);

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

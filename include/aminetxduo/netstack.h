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
 * Resolver failures, which are separate because they are the ones an ordinary
 * user meets: a mistyped host name is not a hardware fault and must not be
 * reported as one. netstack_resolve() used to answer every failure with
 * AMI_NET_ERR_NODEV, so a typo read as "the SANA-II device would not open"
 * and sent people to look at their Ethernet card.
 *
 * bsdsocket.library's h_errno mapping treats everything except
 * AMI_NET_ERR_STATE as HOST_NOT_FOUND, which stays correct for all three.
 */
#define AMI_NET_ERR_NONAME     (-6)   /* the name does not exist              */
#define AMI_NET_ERR_NOSERVER   (-7)   /* no name server is configured         */
#define AMI_NET_ERR_TIMEOUT    (-8)   /* the name server did not answer       */
#define AMI_NET_ERR_BUSY       (-9)   /* still carrying connections           */

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
    TX_THREAD    nc_Thread;
    BOOL         nc_Adopted;    /* inside a bracket right now                */
    BOOL         nc_Live;       /* nc_Thread exists and is dormant (cached)  */
    struct Task *nc_Task;       /* whose it is; only that task may use it    */
} AmiNetCaller;

LONG ami_netstack_enter(AmiNetCaller *caller);
VOID ami_netstack_leave(AmiNetCaller *caller);

/* --------------------------------------------------- the cached bracket ---
 *
 * The pair above adopts on enter and orphans on leave, which is right for a
 * caller whose AmiNetCaller lives on the stack and wrong for one that makes
 * thousands of calls: measured on a 14 MHz 68020 (tests/perf/bracket_test.c)
 * an adopt/orphan pair costs ~600 us and the same handoff over a TX_THREAD
 * that is merely dormant costs ~270 us. That difference is per CALL, not per
 * byte, which is why it showed up as this stack losing a bulk transfer to
 * Roadshow while winning the connect and the first byte (docs/RESEARCH.md
 * §29.3, §32.11).
 *
 * The pair below keeps the TX_THREAD across brackets. Three obligations come
 * with it, and none of them are optional:
 *
 *   1. `caller` must be ZEROED once and must outlive every bracket. A
 *      TX_THREAD that goes out of scope while ThreadX still lists it is a
 *      corrupted kernel.
 *   2. Every call must come from the SAME Exec Task. A different task is
 *      detected and served by the per-call path instead, never by borrowing.
 *   3. ami_netstack_release() must be called before the storage goes away,
 *      from the owning task where possible.
 *
 * Everything else is unchanged: the bracket still takes and gives back the
 * ThreadX baton per call, and NX_THREADS_ONLY_CALLER_CHECKING still sees a
 * real TX_THREAD. What is cached is the registration, not the baton.
 */
LONG ami_netstack_enter_cached(AmiNetCaller *caller);
VOID ami_netstack_leave_cached(AmiNetCaller *caller);
VOID ami_netstack_release(AmiNetCaller *caller);

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

/* ------------------------------------------------- interfaces at run time --
 *
 * Adding and removing an interface AFTER netstack_startup() has run. This is
 * what bsdsocket.library's AddInterfaceTagList() and RemoveInterface() are
 * built on, and it is the only path by which ns_Iface[] changes once the
 * stack is up -- which is the whole reason it lives here rather than in the
 * library: an interface attached to NetX Duo but unknown to the netstack
 * would not have its SANA-II device closed by netstack_shutdown().
 *
 * netstack_interface_add() opens the SANA-II device named in *cfg, binds it
 * and attaches it to the running NX_IP. `cfg` is COPIED into the netstack's
 * own configuration, because NetX Duo keeps the name pointer rather than the
 * name; *index_out receives the slot, which is the lowest free one.
 *
 * netstack_interface_remove() is the counterpart. It refuses an interface
 * that still carries TCP connections with AMI_NET_ERR_BUSY unless `force`,
 * and refuses with AMI_NET_ERR_STATE if the SANA-II device will not give its
 * read requests back -- in that case nothing is freed and the slot stays
 * occupied, because a device holding pointers into freed memory is worse than
 * an interface that will not go away until the next NetShutdown.
 */
LONG    netstack_interface_add(const AmiIfConfig *cfg, UWORD *index_out);
LONG    netstack_interface_remove(UWORD index, BOOL force);

/* ------------------------------------------------------------------ IPv6 --
 *
 * Present only in an AMINETXDUO_IPV6 build. The floor build has no IPv6 at
 * all, so callers ask with #ifdef rather than at run time -- there is no
 * "IPv6 is compiled in but turned off" state to represent.
 *
 * See docs/RESEARCH.md §9 for the configuration model: an interface always
 * gets its fe80::/64 link-local address (no router, no server, no config
 * needed), and CONFIGURE6 in DEVS:NetInterfaces/<name> decides whether it also
 * gets a stateless-autoconfigured or a static global one.
 */
#ifdef AMINETXDUO_IPV6

/* TRUE once nxd_ipv6_enable() has succeeded on the singleton's NX_IP. */
BOOL netstack_ipv6_enabled(VOID);

/*
 * The interface's addresses, in NetX Duo's four-host-order-ULONG form.
 * `slot` walks this interface's addresses from 0; returns FALSE when there
 * are no more. *prefix_out and *state_out may be NULL.
 *
 * State is one of NX_IPV6_ADDR_STATE_* -- an address that is still TENTATIVE
 * is undergoing duplicate address detection and must not be used as a source.
 */
BOOL netstack_ipv6_address_get(UWORD interface_index, UWORD slot,
                               ULONG addr_out[4], ULONG *prefix_out,
                               ULONG *state_out);

/*
 * The best source address for talking to `dest`, which is what an AF_INET6
 * socket bound to in6addr_any reports from getsockname(). Returns FALSE when
 * the interface has no usable (non-tentative) address of the right scope.
 */
BOOL netstack_ipv6_source_for(const ULONG dest[4], ULONG addr_out[4]);

/*
 * AAAA lookup. Same contract as netstack_resolve(), with one difference worth
 * knowing: DEVS:Internet/hosts is not consulted, because the netdb store has
 * no way to hold an IPv6 address. See the comment in netstack_dns.c.
 */
LONG netstack_resolve6(const char *name, ULONG addr_out[4],
                       ULONG timeout_ticks);

#endif /* AMINETXDUO_IPV6 */

/*
 * Resolver. Implemented over NetX Duo addons/dns; used by gethostbyname and
 * friends in bsdsocket.library. Blocking, with the timeout in ticks.
 *
 * In an AMINETXDUO_MDNS build a name ending in ".local" is sent to the RFC
 * 6762 responder instead of to the unicast name servers, and never to both.
 * Callers do not need to know: the branch is inside netstack_resolve().
 */
LONG    netstack_resolve(const char *name, ULONG *addr_out, ULONG timeout_ticks);
LONG    netstack_resolve_reverse(ULONG addr, char *name_out, ULONG name_len,
                                 ULONG timeout_ticks);

#ifdef AMINETXDUO_MDNS
/*
 * The name this machine actually answers to on the local wire, WITHOUT the
 * ".local" -- or NULL if mDNS is not running or lost every probe.
 *
 * Not necessarily the configured HOSTNAME: RFC 6762 9 renames on a collision,
 * so anything that shows this to a user must show what was claimed rather
 * than what was asked for.
 */
const char *netstack_mdns_hostname(VOID);
#endif

#ifdef __cplusplus
}
#endif

#endif /* AMINETXDUO_NETSTACK_H */

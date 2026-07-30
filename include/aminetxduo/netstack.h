/*
 * AmiNetXDuo -- the stack singleton.
 *
 * One AmiNetStack exists per machine. It holds the ThreadX kernel, the NetX Duo
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
 * Resolver failures are separate because a mistyped host name is not a hardware
 * fault and must not be reported as one. netstack_resolve() used to answer
 * every failure with AMI_NET_ERR_NODEV, so a typo read as "the SANA-II device
 * would not open".
 *
 * bsdsocket.library's h_errno mapping treats everything except
 * AMI_NET_ERR_STATE as HOST_NOT_FOUND, which stays correct for all three.
 */
#define AMI_NET_ERR_NONAME     (-6)   /* the name does not exist              */
#define AMI_NET_ERR_NOSERVER   (-7)   /* no name server is configured         */
#define AMI_NET_ERR_TIMEOUT    (-8)   /* the name server did not answer       */
#define AMI_NET_ERR_BUSY       (-9)   /* still carrying connections           */

/*
 * The device opened and then would not answer S2_DEVICEQUERY,
 * S2_GETSTATIONADDRESS or S2_CONFIGINTERFACE. Separate from AMI_NET_ERR_NODEV
 * because the advice is the opposite: the card is present and the driver is
 * loaded, so seating and unit numbers are not the thing to check.
 */
#define AMI_NET_ERR_DEVBAD    (-10)

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
 * NetX Duo checks who is calling: roughly forty of its entry points are wrapped
 * in NX_THREADS_ONLY_CALLER_CHECKING and return NX_CALLER_ERROR unless the
 * caller is the ThreadX baton holder (port/netxduo-amiga/inc/nx_port.h). An Exec
 * Task that ThreadX has never adopted fails every one of them, so everything
 * that touches a NetX Duo API -- the netstack itself, bsdsocket.library, the
 * tools -- has to bracket the call.
 *
 * This is public rather than private to src/netstack/ so there is exactly one
 * bracket: bsdsocket.library used to carry a second, equivalent implementation
 * on the port's tx_amiga.h because it could not reach this one.
 *
 * `caller` is caller-supplied storage that must stay valid until
 * ami_netstack_leave(). Brackets nest: a nested enter() finds the calling task
 * already holds the baton, borrows the context and leaves it alone. "Already
 * holds" and not "somebody holds" -- the baton is one global pointer, so a
 * second task must acquire it rather than read another's as its own.
 *
 * Nothing inside a bracket may block on anything except ThreadX. An adopted
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

/*
 * The same pair with the AmiNetCaller allocated rather than declared.
 *
 * A TX_THREAD is ~230 bytes and these brackets sit under bsdsocket.library
 * vectors, which run on the CALLER's stack: an AmigaDOS Shell gives 4 KB, with
 * no guard page and no MMU, so an oversized frame does not fault -- it
 * overwrites what is below it and the machine dies later somewhere unrelated.
 * A file-scope AmiNetCaller would not do, because two tasks can be inside the
 * bracket at once and each needs its own registration.
 *
 * Returns NULL if the kernel is not running or memory is short; the AllocMem
 * is invisible next to the network round trip every user of this pair makes.
 */
AmiNetCaller *ami_netstack_enter_alloc(VOID);
VOID          ami_netstack_leave_free(AmiNetCaller *caller);

/* --------------------------------------------------- the cached bracket ---
 *
 * The pair above adopts on enter and orphans on leave, which is right for a
 * caller whose AmiNetCaller lives on the stack and wrong for one that makes
 * thousands of calls: measured on a 14 MHz 68020 (tests/perf/bracket_test.c) an
 * adopt/orphan pair costs ~600 us and the same handoff over a TX_THREAD that is
 * merely dormant costs ~270 us. That difference is per call, not per byte,
 * which is why it showed up as this stack losing a bulk transfer to Roadshow
 * while winning the connect and the first byte (docs/RESEARCH.md §29.3,
 * §32.11).
 *
 * The pair below keeps the TX_THREAD across brackets. Three requirements come
 * with it:
 *
 *   1. `caller` must be zeroed once and must outlive every bracket. A TX_THREAD
 *      that goes out of scope while ThreadX still lists it is a corrupted
 *      kernel.
 *   2. Every call must come from the same Exec Task. A different task is
 *      detected and served by the per-call path instead, never by borrowing.
 *   3. ami_netstack_release() must be called before the storage goes away, from
 *      the owning task where possible.
 *
 * Everything else is unchanged: the bracket still acquires and releases the
 * ThreadX baton per call, and NX_THREADS_ONLY_CALLER_CHECKING still sees a real
 * TX_THREAD. What is cached is the registration, not the baton.
 */
LONG ami_netstack_enter_cached(AmiNetCaller *caller);
VOID ami_netstack_leave_cached(AmiNetCaller *caller);
VOID ami_netstack_release(AmiNetCaller *caller);

/*
 * Bracket counters. A freeze in here leaves nothing behind -- no Enforcer hit,
 * and a log that never reached disk -- so the evidence has to survive in memory
 * for a debugger or NETSTATUS_HEALTH to pick up.
 *
 *   bs_Live / bs_LiveMax   tasks inside a bracket now, and the most ever
 *   bs_Full                times the table had no slot (the fail-open path)
 *   bs_Transitions         release/acquire pairs completed
 *   bs_StateMax            highest _tx_thread_system_state seen on entry
 *   bs_BatonMoved          times release() found the baton was not ours
 */
typedef struct AmiBatonStats
{
    ULONG bs_Live;
    ULONG bs_LiveMax;
    ULONG bs_Full;
    ULONG bs_Transitions;
    ULONG bs_StateMax;
    ULONG bs_BatonMoved;
} AmiBatonStats;

extern AmiBatonStats ami_baton_stats;

/* Accessors -- all return NULL when the stack is down. */
NX_IP          *netstack_ip(VOID);
NX_PACKET_POOL *netstack_pool(VOID);
const AmiConfig *netstack_config(VOID);

/*
 * Packet pool sizing. NetX Duo's embedded defaults do not suit the 68020/4 MB
 * floor (docs/RESEARCH.md §9), so these are computed from AvailMem() at startup
 * and clamped to the range below.
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
 * Adding and removing an interface after netstack_startup() has run. This is
 * what bsdsocket.library's AddInterfaceTagList() and RemoveInterface() are
 * built on, and the only path by which ns_Iface[] changes once the stack is up.
 * It lives here rather than in the library because an interface attached to
 * NetX Duo but unknown to the netstack would not have its SANA-II device closed
 * by netstack_shutdown().
 *
 * netstack_interface_add() opens the SANA-II device named in *cfg, binds it and
 * attaches it to the running NX_IP. `cfg` is copied into the netstack's own
 * configuration, because NetX Duo keeps the name pointer rather than the name;
 * *index_out receives the slot, which is the lowest free one.
 *
 * netstack_interface_remove() is the counterpart. It refuses an interface that
 * still carries TCP connections with AMI_NET_ERR_BUSY unless `force`, and
 * refuses with AMI_NET_ERR_STATE if the SANA-II device will not give its read
 * requests back. In that case nothing is freed and the slot stays occupied: a
 * device holding pointers into freed memory is worse than an interface that
 * will not go away until the next NetShutdown.
 */
LONG    netstack_interface_add(const AmiIfConfig *cfg, UWORD *index_out);
LONG    netstack_interface_remove(UWORD index, BOOL force);

/* ------------------------------------------- DHCP on one interface --------
 *
 * What bsdsocket.library's BeginInterfaceConfig() drives. The netstack holds
 * the single NX_DHCP -- there can only be one, because there is only one UDP
 * port 68 -- so an allocation asked for by an application goes through the same
 * client the boot-time configuration uses, on the interface it names and no
 * other.
 *
 * These are not one blocking call. The published API's timeout is mandatory and
 * DHCP retries forever, so somebody has to hold a deadline; that is the caller,
 * which has a Process and can Delay(), where the netstack has neither
 * dos.library nor any business blocking.
 *
 *   netstack_interface_dhcp_start()   enable and start, creating the client
 *                                     if the machine booted without one.
 *   netstack_interface_dhcp_state()   AMI_DHCP_* -- poll it.
 *   netstack_interface_dhcp_lease()   what the server said. BOUND only.
 *   netstack_interface_dhcp_stop()    give up, or let go of the lease.
 */
#define AMI_DHCP_IDLE       0       /* not started, or stopped              */
#define AMI_DHCP_WORKING    1       /* discovering, requesting, probing     */
#define AMI_DHCP_BOUND      2       /* the interface has a lease            */

/* How many of each address list a lease is reported with. The DHCP option
   can carry more; nothing on this machine has room to use more. */
#define AMI_DHCP_MAX_ADDRS  8

typedef struct AmiDhcpLease {
    ULONG   adl_Address;
    ULONG   adl_NetMask;
    ULONG   adl_Server;
    ULONG   adl_LeaseSeconds;           /* 0xFFFFFFFF means infinite        */

    ULONG   adl_Router[AMI_DHCP_MAX_ADDRS];
    UWORD   adl_RouterCount;
    ULONG   adl_Dns[AMI_DHCP_MAX_ADDRS];
    UWORD   adl_DnsCount;
    ULONG   adl_StaticRoute[AMI_DHCP_MAX_ADDRS];
    UWORD   adl_StaticRouteCount;

    char    adl_HostName[AMI_CFG_NAME_LEN];
    char    adl_DomainName[AMI_CFG_NAME_LEN];
} AmiDhcpLease;

LONG    netstack_interface_dhcp_start(UWORD index, ULONG requested_address);
LONG    netstack_interface_dhcp_state(UWORD index);
LONG    netstack_interface_dhcp_lease(UWORD index, AmiDhcpLease *out);
LONG    netstack_interface_dhcp_stop(UWORD index, BOOL release);

/* ------------------------------------------------------------------ IPv6 --
 *
 * Present only in an AMINETXDUO_IPV6 build. The floor build has no IPv6 at all,
 * so callers ask with #ifdef rather than at run time: there is no "IPv6 is
 * compiled in but turned off" state to represent.
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
 * State is one of NX_IPV6_ADDR_STATE_*. An address that is still TENTATIVE is
 * undergoing duplicate address detection and must not be used as a source.
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
 * AAAA lookup. Same semantics as netstack_resolve(), with one difference:
 * DEVS:Internet/hosts is not consulted, because the netdb store has no way to
 * hold an IPv6 address. See the comment in netstack_dns.c.
 */
LONG netstack_resolve6(const char *name, ULONG addr_out[4],
                       ULONG timeout_ticks);

/* ------------------------------------------------------------ routing ---
 *
 * Where an IPv6 packet goes is decided by two lists and no routing table:
 * the on-link prefixes, and the default routers.  There is nothing in NetX
 * Duo that maps a prefix to a next hop, so these take one or the other:
 *
 *   next_hop given, prefix_len 0, dest ::   a default router on
 *                                           `interface_index`
 *   next_hop NULL or ::                     an on-link prefix; the packet goes
 *                                           straight to the destination
 *   next_hop given with a prefix            NX_NOT_SUPPORTED -- there is
 *                                           nowhere to put it
 *
 * These return NetX Duo's own status rather than an AMI_NET_* code because
 * every caller has a different word for each of NX_ENTRY_NOT_FOUND,
 * NX_DUPLICATED_ENTRY, NX_NO_MORE_ENTRIES and NX_IP_ADDRESS_ERROR.
 *
 * Both are what CONFIGURE6/GATEWAY6 goes through at bring-up, so a route
 * asked for in DEVS:NetInterfaces and one asked for by AddNetRoute are the
 * same call.
 */
UINT netstack_ipv6_route_add(const ULONG dest[4], ULONG prefix_len,
                             const ULONG next_hop[4], UWORD interface_index);
UINT netstack_ipv6_route_delete(const ULONG dest[4], ULONG prefix_len,
                                const ULONG next_hop[4]);

#endif /* AMINETXDUO_IPV6 */

/*
 * Changing the resolver while the stack runs, for AddDomainNameServer() and the
 * two beside it. Each updates both the NetX Duo DNS client and the stored
 * configuration, because different things read them: the client resolves, the
 * configuration is what every report describes.
 *
 * Adding a server that is already present succeeds and changes nothing.
 * netstack_set_domain_name(NULL) or "" clears the domain; a name too long to
 * store is refused rather than truncated.
 */
LONG netstack_dns_server_add(ULONG address);
LONG netstack_dns_server_remove(ULONG address);
LONG netstack_set_domain_name(const char *name);

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
 * The name this machine answers to on the local wire, without the ".local", or
 * NULL if mDNS is not running or lost every probe.
 *
 * Not necessarily the configured HOSTNAME: RFC 6762 9 renames on a collision,
 * so anything that shows this to a user must show what was claimed rather than
 * what was asked for.
 */
const char *netstack_mdns_hostname(VOID);
#endif

#ifdef __cplusplus
}
#endif

#endif /* AMINETXDUO_NETSTACK_H */

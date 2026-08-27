/* AmiNetXDuo, the stack singleton.  Every entry point is callable from any
 * Exec task; startup and shutdown are serialised and idempotent.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_NETSTACK_H
#define AMINETXDUO_NETSTACK_H

#include <exec/types.h>
#include "aminetxduo/config.h"
#include "aminetxduo/pool.h"

#include "tx_api.h"
#include "nx_api.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AmiNetStack AmiNetStack;

#define AMI_NET_OK              0
#define AMI_NET_ERR_NOMEM      (-1)
#define AMI_NET_ERR_NODEV      (-2)   /* SANA-II device did not open          */
#define AMI_NET_ERR_CONFIG     (-3)
#define AMI_NET_ERR_KERNEL     (-4)   /* ThreadX/NetX Duo refused to start    */
#define AMI_NET_ERR_STATE      (-5)

/* Resolver failures, separate so that a mistyped host name is not reported as
   a hardware fault. */
#define AMI_NET_ERR_NONAME     (-6)   /* the name does not exist              */
#define AMI_NET_ERR_NOSERVER   (-7)   /* no name server is configured         */
#define AMI_NET_ERR_TIMEOUT    (-8)   /* the name server did not answer       */
#define AMI_NET_ERR_BUSY       (-9)   /* still carrying connections           */

/* The device opened and then did not answer S2_DEVICEQUERY,
   S2_GETSTATIONADDRESS or S2_CONFIGINTERFACE: it is present and its driver is
   loaded, so seating and unit numbers are not the thing to check. */
#define AMI_NET_ERR_DEVBAD    (-10)

/* The caller's give_up said stop, see netstack_resolve_until().  Nothing
   failed and nothing was learnt; the caller decides what to report. */
#define AMI_NET_ERR_ABORTED   (-11)

/* Every NetX Duo interface slot (AMI_CFG_MAX_ATTACHED) is taken.  Describing
   more interfaces than can be attached is allowed, so this says the network is
   running and full, where AMI_NET_ERR_STATE says it is not running. */
#define AMI_NET_ERR_NOSLOT    (-12)

/* Bring the stack up: idempotent and reference-counted.  Blocks until the
   first interface has an address or the DHCP timeout expires. */
LONG netstack_startup(VOID);

/* Drop a reference. The stack goes down when the count reaches zero. */
VOID netstack_shutdown(VOID);

/* TRUE only when no stack or ThreadX Task can still execute this hunk. */
BOOL netstack_can_unload(VOID);

/* The singleton, or NULL if the stack is not up. */
AmiNetStack   *netstack_get(VOID);

/* ------------------------------------------------------ the ThreadX bracket
   Every NetX Duo call must be inside one, and `caller` must stay valid until
   ami_netstack_leave().  Brackets nest per task.  Nothing inside a bracket may
   block on anything but ThreadX: an exec Wait() there stops the whole stack. */
typedef struct AmiNetCaller
{
    TX_THREAD    nc_Thread;
    BOOL         nc_Adopted;    /* inside a bracket right now                */
    BOOL         nc_Live;       /* cached thread exists, or adoption started */
    struct Task *nc_Task;       /* whose it is; only that task may use it    */
} AmiNetCaller;

LONG ami_netstack_enter(AmiNetCaller *caller);
VOID ami_netstack_leave(AmiNetCaller *caller);

/* The same pair with the AmiNetCaller allocated: a TX_THREAD is ~230 bytes and
   these run on the caller's stack, which is 4 KB in a Shell with no guard page.
   NULL when the kernel is not running or memory is short. */
AmiNetCaller *ami_netstack_enter_alloc(VOID);
VOID          ami_netstack_leave_free(AmiNetCaller *caller);

/* --------------------------------------------------- the cached bracket ---
   Keeps the TX_THREAD across brackets: `caller` must be zeroed once and must
   outlive every bracket, every call must come from the same Exec Task, and
   ami_netstack_release() must run before the storage goes away. */
LONG ami_netstack_enter_cached(AmiNetCaller *caller);
VOID ami_netstack_leave_cached(AmiNetCaller *caller);

#ifdef AMINETXDUO_GREEN_REALM
/* Enter only if the baton is immediately takeable.  AMI_NET_OK leaves the
 * bracket open exactly as ami_netstack_enter_cached() would (leave with
 * ami_netstack_leave_cached()); AMI_NET_ERR_BUSY means nothing happened. */
LONG ami_netstack_try_enter_cached(AmiNetCaller *caller);
#endif
VOID ami_netstack_release(AmiNetCaller *caller);

/* Bracket counters, kept in memory because a freeze in here leaves no log.
   bs_StateShared counts times another task had the interrupt state raised
   while this one was running: it must stay zero. */
typedef struct AmiBatonStats
{
    ULONG bs_Live;
    ULONG bs_LiveMax;
    ULONG bs_Full;
    ULONG bs_Transitions;
    ULONG bs_StateMax;
    ULONG bs_BatonMoved;
    ULONG bs_StateShared;
} AmiBatonStats;

extern AmiBatonStats ami_baton_stats;

/* All return NULL when the stack is down.  netstack_config() points at the
   LIVE configuration: a caller reporting cfg->hostname or cfg->resolver calls
   netstack_dns_absorb_pending() once first. */
NX_IP          *netstack_ip(VOID);
NX_PACKET_POOL *netstack_pool(VOID);
const AmiConfig *netstack_config(VOID);

/* Offer a name at `source` (AmiHostnameSource).  It goes through
   ami_config_hostname_offer(), so a source weaker than the one that named the
   machine is refused with AMI_NET_ERR_CONFIG.  AMI_NET_ERR_STATE with the
   stack down. */
LONG netstack_hostname_offer(UWORD source, const char *name);

/* Is this machine answering .local on that NX interface index?  The effective
   state, not the MDNS= request: an interface the responder refused is FALSE. */
BOOL netstack_iface_mdns(UWORD nx_index);

/* Start or stop answering .local on that interface, now.  Neither direction
   waits, and FALSE keeps the module so its own thread can send the RFC 6762
   10.1 goodbye.  AMI_NET_ERR_STATE/NOMEM/KERNEL, and AMI_NET_ERR_NODEV in a
   build without AMINETXDUO_MDNS. */
LONG netstack_iface_mdns_set(UWORD nx_index, BOOL enable);

/* The configuration of the interface at that NX index.  NOT
   cfg->interfaces[nx_index]: that subscript is the configuration order. */
const AmiIfConfig *netstack_iface_config(UWORD nx_index);

/* Sample the packet pool's counters into AmiMemStats (aminetxduo/compat.h),
   carrying the low-water mark of what is free.  Sampled and not exact: NetX
   Duo also allocates packets from its own internals.  Does nothing when there
   is no pool. */
VOID netstack_pool_sample(VOID);

/* Packet pool sizing lives in aminetxduo/pool.h, included above:
   AMI_POOL_PAYLOAD, AMI_POOL_MIN_PACKETS, AMI_POOL_MAX_PACKETS and the
   arithmetic that turns AvailMem() into a packet count. */

/* Interface handles.  Index 0..count-1 in configuration order; the loopback
   interface is always present and is not counted here. */
UWORD   netstack_interface_count(VOID);
LONG    netstack_interface_up(UWORD index);
/* Down and offline are two states, not one: down stops the stack, offline also
   sends S2_OFFLINE and takes the wire away from every other client of the
   device. Roadshow's SM_Down/SM_Offline, and Online/Offline in the tools. */
LONG    netstack_interface_down(UWORD index);
LONG    netstack_interface_stack_down(UWORD index);
BOOL    netstack_interface_is_up(UWORD index);

/* ------------------------------------------------- interfaces at run time,
   the only path by which ns_Iface[] changes once the stack is up.  `cfg` is
   copied: NetX Duo keeps the name pointer, not the name.  remove() answers
   AMI_NET_ERR_BUSY on live TCP unless `force`, and AMI_NET_ERR_STATE with
   nothing freed if the device will not give its read requests back.  A claim
   blocks removal and slot reuse, and `force` cannot override it. */
LONG    netstack_interface_add(const AmiIfConfig *cfg, UWORD *index_out);
LONG    netstack_interface_start(const AmiIfConfig *cfg, UWORD *index_out);
LONG    netstack_interface_remove(UWORD index, BOOL force);
/* Resolve the name under the same add/remove lock as the removal. */
LONG    netstack_interface_remove_named(const char *name, BOOL force);
LONG    netstack_interface_claim(const char *name, UWORD *index_out);
VOID    netstack_interface_release(UWORD index);

/* ------------------------------------------- DHCP on one interface --------
   One NX_DHCP for the machine, because there is one UDP port 68.  Not one
   blocking call: the caller holds the deadline and polls
   netstack_interface_dhcp_state() for AMI_DHCP_*. */
#define AMI_DHCP_IDLE       0       /* not started, or stopped              */
#define AMI_DHCP_WORKING    1       /* discovering, requesting, probing     */
#define AMI_DHCP_BOUND      2       /* the interface has a lease            */

/* How many of each address list a lease is reported with. The DHCP option
   can carry more. Nothing on this machine has room to use more. */
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
/* Extend the lease this interface already holds; the address stays unless the
   server changes it.  AMI_NET_ERR_STATE when there is no lease to extend,
   which is not turned into a fresh allocation. */
LONG    netstack_interface_dhcp_renew(UWORD index);

/* NetX Duo's own NX_DHCP_STATE_* for this interface, which the AMI_DHCP_*
   values collapse: BOUND, RENEWING and REBINDING are all AMI_DHCP_BOUND.
   Zero (NOT_STARTED) with the stack down or no client created. */
UWORD   netstack_interface_dhcp_raw_state(UWORD index);
LONG    netstack_interface_dhcp_stop(UWORD index, BOOL release);

/* ------------------------------------------------------------------ IPv6,
   present only in an AMINETXDUO_IPV6 build, so callers ask with #ifdef: there
   is no state in which IPv6 is compiled in but turned off. */
#ifdef AMINETXDUO_IPV6

/* TRUE once nxd_ipv6_enable() has succeeded on the singleton's NX_IP. */
BOOL netstack_ipv6_enabled(VOID);

/* The interface's addresses, in NetX Duo's four-host-order-ULONG form.
   `slot` walks from 0 and returns FALSE when there are no more; *prefix_out
   and *state_out may be NULL.  State is NX_IPV6_ADDR_STATE_*, and a TENTATIVE
   address must not be used as a source. */
BOOL netstack_ipv6_address_get(UWORD interface_index, UWORD slot,
                               ULONG addr_out[4], ULONG *prefix_out,
                               ULONG *state_out);

/* How the address in `slot` was obtained, NX_IPV6_ADDRESS_* in nx_api.h. */
BOOL netstack_ipv6_address_origin(UWORD interface_index, UWORD slot,
                                  ULONG *origin_out);

/* TRUE when some interface holds a global unicast address (2000::/3, RFC 4291
   2.5.4) that has finished duplicate address detection.
   netstack_ipv6_enabled() is weaker and is not a substitute: every interface
   gets a link-local whether or not a router has ever spoken to it. */
BOOL netstack_ipv6_have_global(VOID);

/* The best source address for talking to `dest`.  interface_index is a
   zero-based NetX interface, or -1 to let the route choose.  FALSE when that
   interface has no usable (non-tentative) address of the right scope. */
BOOL netstack_ipv6_source_for(const ULONG dest[4], LONG interface_index,
                              ULONG addr_out[4]);

/* AAAA lookup, as netstack_resolve() including the search list, except that
   DEVS:Internet/hosts is not consulted: the netdb store holds no IPv6. */
LONG netstack_resolve6(const char *name, ULONG addr_out[4],
                       ULONG timeout_ticks);

/* ------------------------------------------------------------ routing ---
   Two lists and no routing table: a next hop with prefix_len 0 and dest :: is
   a default router on `interface_index`; no next hop is an on-link prefix; a
   next hop WITH a prefix is NX_NOT_SUPPORTED.  These return NetX Duo's own
   status rather than an AMI_NET_* code. */
UINT netstack_ipv6_route_add(const ULONG dest[4], ULONG prefix_len,
                             const ULONG next_hop[4], UWORD interface_index);
UINT netstack_ipv6_route_delete(const ULONG dest[4], ULONG prefix_len,
                                const ULONG next_hop[4]);

/* ------------------------------------------------------------- DHCPv6 --- */

typedef struct AmiDhcp6Status {
    UWORD   ad6_State;              /* AMI_DHCP_*                           */
    UWORD   ad6_RawState;           /* NX_DHCPV6_STATE_*, 0 = no client     */
    /* FALSE when the client only sent an Information-Request: options, no
       address, and so nothing that could be released. */
    BOOL    ad6_Stateful;
    ULONG   ad6_Address[4];         /* host byte order; AMI_DHCP_BOUND only */
    ULONG   ad6_PreferredSeconds;
    ULONG   ad6_ValidSeconds;
    ULONG   ad6_T1;
    ULONG   ad6_T2;
} AmiDhcp6Status;

/* This stack runs one DHCPv6 client, so every interface but the one it was
   started on answers AMI_DHCP_IDLE.  AMI_NET_ERR_STATE only on a bad
   argument; *out is filled either way. */
LONG netstack_interface_dhcp6_status(UWORD interface_index,
                                     AmiDhcp6Status *out);

/* Give a stateful lease back (RFC 8415 18.2.7) while the stack keeps running.
   Blocks for up to AMI_DHCPV6_RELEASE_TICKS, so never from a NetX callback.
   AMI_NET_ERR_STATE when this interface holds no DHCPv6 lease. */
LONG netstack_interface_dhcp6_release(UWORD interface_index);

#endif /* AMINETXDUO_IPV6 */

/* Put resolver changes recorded by DHCP -- and, in an IPv6 build, router
   advertisements and DHCPv6 -- into the DNS client and the reported
   configuration.  Must be called from a caller task, never from a NetX
   callback.  Cheap and safe when there is nothing pending. */
VOID netstack_dns_absorb_pending(VOID);

/* Changing the resolver while the stack runs; each updates both the DNS client
   and the stored configuration.  Adding a server already present succeeds and
   changes nothing, netstack_set_domain_name(NULL) or "" clears the domain, a
   name too long is refused rather than truncated, and the search list is left
   alone. */
LONG netstack_dns_server_add(ULONG address);
LONG netstack_dns_server_remove(ULONG address);
#ifdef AMINETXDUO_IPV6
/* The same pair for an IPv6 server, host byte order, four words.  Nesting is
   the IPv4 rule: two adds need two removes, and a server a router
   advertisement or DHCPv6 also names outlives the caller's own reference.
   AMI_NET_ERR_NONAME when there is no such server to remove. */
LONG netstack_dns_server6_add(const ULONG address[4]);
LONG netstack_dns_server6_remove(const ULONG address[4]);
#endif
LONG netstack_set_domain_name(const char *name);
/* Coherent reads of the live resolver configuration. The raw AmiConfig is
   still suitable for immutable startup fields, but not for this mutable part. */
LONG netstack_resolver_snapshot(AmiResolverConfig *out);
LONG netstack_domain_name_get(char *out, ULONG out_size);

/* Blocking; timeout_ticks is the whole lookup, not one query.  A name with no
   dot is tried as given and then under each search domain -- name_resolution
   SEARCH, else its DOMAIN, then the lease's option 119 and 15 -- and a name
   with a dot is never suffixed.  In an AMINETXDUO_MDNS build a ".local" name
   goes to the responder and never to the unicast servers. */
LONG    netstack_resolve(const char *name, ULONG *addr_out, ULONG timeout_ticks);
LONG    netstack_resolve_reverse(ULONG addr, char *name_out, ULONG name_len,
                                 ULONG timeout_ticks);

/* The same three, told how to give up early.  `give_up` is asked BETWEEN
   queries, never during one, and runs outside the ThreadX bracket so it may
   call exec; TRUE ends the lookup with AMI_NET_ERR_ABORTED.  A NULL `give_up`
   is the plain call above. */
typedef BOOL (*AmiNetGiveUpFn)(VOID *arg);

LONG    netstack_resolve_until(const char *name, ULONG *addr_out,
                               ULONG timeout_ticks,
                               AmiNetGiveUpFn give_up, VOID *give_up_arg);
LONG    netstack_resolve_reverse_until(ULONG addr, char *name_out,
                                       ULONG name_len, ULONG timeout_ticks,
                                       AmiNetGiveUpFn give_up,
                                       VOID *give_up_arg);
#ifdef AMINETXDUO_IPV6
LONG    netstack_resolve6_until(const char *name, ULONG addr_out[4],
                                ULONG timeout_ticks,
                                AmiNetGiveUpFn give_up, VOID *give_up_arg);
#endif

#ifdef AMINETXDUO_MDNS
/* The name this machine answers to on the local wire, without the ".local", or
 * NULL if mDNS is not running or lost every probe.  Not necessarily the
 * configured HOSTNAME: RFC 6762 9 renames on a collision. */
const char *netstack_mdns_hostname(VOID);

/* The widths of one browse result.  They must match NETSTATUS_SVC_*_LEN in
 * <aminetxduo/netstatus.h>, which src/bsdsocket/netstatus.c _Static_asserts. */
#define AMI_MDNS_SVC_NAME_LEN       64
#define AMI_MDNS_SVC_TYPE_LEN       24
#define AMI_MDNS_SVC_HOST_LEN       64
#define AMI_MDNS_SVC_TXT_LEN        192

/* One row of a browse, in the stack's own terms.  ams_Name empty means the row
 * is a service type from the DNS-SD meta-query, with no instance behind it. */
typedef struct AmiMdnsService
{
    UWORD   ams_Index;                  /* interface it was heard on         */
    UWORD   ams_Port;
    ULONG   ams_Address;                /* host order, 0 when not known      */
    BOOL    ams_Local;                  /* our own advertisement             */
    BOOL    ams_TextCut;
    char    ams_Name[AMI_MDNS_SVC_NAME_LEN];
    char    ams_Type[AMI_MDNS_SVC_TYPE_LEN];
    char    ams_Host[AMI_MDNS_SVC_HOST_LEN];
    char    ams_Text[AMI_MDNS_SVC_TXT_LEN];
} AmiMdnsService;

/* `type` is a DNS-SD service type such as "_http._tcp", or NULL for the
 * _services._dns-sd._udp.local meta-query.  Each brackets itself.  Start and
 * stop do not wait, so the caller owns the collection window and must not hold
 * the ThreadX baton across it.  Collect resolves SRV targets that arrived
 * without an address record, at most two seconds for the whole walk. */
LONG    netstack_mdns_browse_start(const char *type);
LONG    netstack_mdns_browse_stop(const char *type);
UWORD   netstack_mdns_browse_collect(const char *type, AmiMdnsService *out,
                                     UWORD max, UWORD *available);
#endif

#ifdef __cplusplus
}
#endif

#endif /* AMINETXDUO_NETSTACK_H */

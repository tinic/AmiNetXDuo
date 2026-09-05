/*
 * AmiNetXDuo, netstack singleton internals.  Private to src/netstack/.
 * Include order matters: tx_api.h and nx_api.h come before any exec header,
 * because <exec/types.h> turns VOID into a macro and breaks the tx typedefs.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_NETSTACK_INTERNAL_H
#define AMINETXDUO_NETSTACK_INTERNAL_H

#include "tx_api.h"
#include "nx_api.h"

#include "nxd_dhcp_client.h"
#ifdef AMINETXDUO_IPV6
#include "nxd_dhcpv6_client.h"
#endif
#include "nxd_dns.h"
#include "nx_auto_ip.h"
#ifdef AMINETXDUO_MDNS
#include "nxd_mdns.h"
#endif

#include <exec/types.h>

#include "netstack_dns_handoff.h"
#include "netstack_dns_lease.h"
#include "netstack_dns_domain.h"
#include "netstack_gateway.h"
#include "netstack_dhcp_hostname.h"
#ifdef AMINETXDUO_IPV6
#include "netstack_ra.h"
#endif

#include "aminetxduo/netstack.h"
#include "aminetxduo/config.h"
#include "aminetxduo/compat.h"
#include "aminetxduo/sana2.h"

/* ------------------------------------------------------------- tunables, */

#include "../thread_priorities.h"

#define AMI_IP_STACK_SIZE           4096
#define AMI_ARP_CACHE_SIZE          1024

/*
 * 4096 and not the 2048 the NetX Duo samples use: this thread reaches a
 * SANA-II device through ami_sana2_driver_entry and makes Exec DoIO/SendIO
 * calls, which run on this stack, on a machine with no guard page.
 */
#define AMI_AUTOIP_STACK_SIZE       4096

/* AMI_POOL_MEM_DIVISOR, AMI_POOL_WORKING_PACKETS and AMI_POOL_MEM_DIVISOR_LOW
   are in aminetxduo/pool.h with the arithmetic that reads them. */

#ifdef AMINETXDUO_IPV6
/*
 * The DHCPv6 client's own thread stack.  4096 for the reason
 * AMI_AUTOIP_STACK_SIZE is: 812 bytes measured, plus headroom for the Exec
 * calls its send path reaches on a target with no guard page.
 */
#define AMI_DHCPV6_STACK_SIZE       4096

/*
 * And the deferred-work thread's, which is small because that thread wakes on
 * an event flag and calls into the client: 604 bytes measured, and no Exec
 * calls, so it needs none of the headroom the SANA-II paths do.
 */
#define AMI_DHCPV6_WORK_STACK_SIZE  2048

/*
 * How long a shutdown waits for the DHCPv6 Release to be answered.  A machine
 * being switched off must not be held up by a server that has gone away, and
 * RFC 8415 18.2.7 lets a client not wait at all.
 */
#define AMI_DHCPV6_RELEASE_TICKS    (2UL * (ULONG)NX_IP_PERIODIC_RATE)

#endif

/*
 * DNS answer cache size: about fifty cached names.  Undersizing costs only a
 * DNS query, because NetX Duo replaces the least recently used record rather
 * than failing; oversizing costs resident memory forever.
 */
#define AMI_DNS_CACHE_BYTES         2048

#ifdef AMINETXDUO_MDNS
/*
 * The two mDNS cache sizes.  The local cache holds what this machine claims,
 * an A record per interface plus four records per declared service; the peer
 * cache holds what has been learnt and evicts the oldest when it is full.
 */
#define AMI_MDNS_LOCAL_CACHE_BYTES  \
    (1024 + AMI_CFG_MAX_SD_SERVICES * 384)
/* Overridable: 32 KB holds a hundred-odd learnt records, which is a network
   far larger than an Amiga is on.  The cache evicts the oldest when it is
   full, so a smaller one forgets sooner and loses nothing else. */
#ifndef AMI_MDNS_PEER_CACHE_BYTES
#define AMI_MDNS_PEER_CACHE_BYTES   8192
#endif
#endif

/*
 * Three DHCP options NetX Duo does not name: RFC 2132 3.17 for the domain
 * name, 3.12 for the classful static route list, RFC 3397 for the domain
 * search list.
 */
#define AMI_DHCP_OPTION_DOMAIN          15
#define AMI_DHCP_OPTION_STATIC_ROUTE    33
#define AMI_DHCP_OPTION_SEARCH         119

/* How long netstack_startup() blocks waiting for the first address. */
#define AMI_DHCP_TIMEOUT_TICKS      (30UL * (ULONG)NX_IP_PERIODIC_RATE)
#define AMI_LINK_TIMEOUT_TICKS      (10UL * (ULONG)NX_IP_PERIODIC_RATE)

/*
 * How long to wait after the RFC 3927 fallback fires.  PROBE_WAIT plus
 * PROBE_NUM probes plus a claim fits well inside fifteen seconds.
 */
#define AMI_AUTOIP_TIMEOUT_TICKS    (15UL * (ULONG)NX_IP_PERIODIC_RATE)

/*
 * Only for the case where the address-arrival semaphore was not created.  Set
 * clear of the one-tick floor: each poll walks every interface through
 * nx_ip_interface_address_get(), which takes the IP protection mutex.
 */
#define AMI_ADDRESS_POLL_TICKS      ((ULONG)NX_IP_PERIODIC_RATE / 10UL)

/* --------------------------------------------------------------- the state */

struct AmiNetStack
{
    ULONG               ns_Refs;

    AmiConfig           ns_Config;

    NX_PACKET_POOL      ns_Pool;
    APTR                ns_PoolMemory;
    ULONG               ns_PoolBytes;
    ULONG               ns_PoolPackets;

    NX_IP               ns_Ip;
    APTR                ns_IpStack;
    APTR                ns_ArpCache;
    BOOL                ns_IpCreated;

    AmiSana2If         *ns_Iface[AMI_CFG_MAX_ATTACHED];

    /* Operations that resolved an interface name and still use its numeric
       NetX slot.  A claimed slot cannot be removed and reused underneath the
       operation; ami_ns_lock serialises updates to this table. */
    UWORD               ns_IfaceClaims[AMI_CFG_MAX_ATTACHED];

    /* MDNS= for each interface, by NX interface index rather than by
       configuration index: ns_Iface[] is filled in open order and an interface
       that fails to open takes no slot, so the two are not the same number. */
    BOOL                ns_IfaceMdns[AMI_CFG_MAX_ATTACHED];

    /* Whether the service_discovery services have been registered on that
       interface.  nx_mdns_disable() only suspends the local records, so adding
       them again on an off/on pair announces each service twice. */
    BOOL                ns_IfaceMdnsSvc[AMI_CFG_MAX_ATTACHED];

    /* Which configuration slot each opened interface came from.  ns_Iface[] is
       in open order, so this is the only mapping back: an interface that failed
       to open advanced the configuration index and not the NX one. */
    UWORD               ns_IfaceCfg[AMI_CFG_MAX_ATTACHED];
    UWORD               ns_IfaceCount;

    /*
     * WHO ASKED FOR THIS SLOT.  Set when something NAMED the interface, clear
     * when the start-up pass took the slot on its own.  A slot nobody asked
     * for yields to a newcomer somebody did; ami_ns_yield_candidate() picks.
     */
    BOOL                ns_IfaceWanted[AMI_CFG_MAX_ATTACHED];

    NX_DHCP             ns_Dhcp;
    BOOL                ns_DhcpCreated;
    BOOL                ns_DhcpStarted;

    /*
     * The name the client announces as option 12.  nx_dhcp_create() keeps the
     * pointer rather than a copy, so it needs storage that outlives the
     * NX_DHCP, and storage of its own: ns_Config.hostname is written later.
     */
    char                ns_DhcpName[AMI_CFG_NAME_LEN];

    /* Last DHCP state and last address seen per interface.  NetX Duo's
       callbacks report only the new value, so the previous one is kept here to
       let the notifications report transitions such as a lost lease. */
    UBYTE               ns_DhcpState[AMI_CFG_MAX_ATTACHED];
    ULONG               ns_LastAddress[AMI_CFG_MAX_ATTACHED];
    AmiNsDhcpHostnameState ns_DhcpHostname;

    /* Another host answered an ARP for an address of this machine.  Counted
       rather than only logged, because the log is off in a shipping build and
       this is the one fault where the machine works and the network does not. */
    ULONG               ns_AddrConflicts;
    ULONG               ns_LastConflictAddr;

    /* Posted by ami_ns_address_changed() when an interface gains or loses an
       address, so ami_ns_wait_for_address() can block instead of polling. */
    TX_SEMAPHORE        ns_AddrArrived;
    BOOL                ns_AddrArrivedReady;

    /* The one-second heartbeat that carries ami_second_notify().  The flag is
       needed because four of ami_ns_destroy()'s seven call sites run before
       ThreadX exists, so the delete cannot be inferred from position. */
    TX_TIMER            ns_Second;
    BOOL                ns_SecondCreated;

    NX_AUTO_IP          ns_AutoIp;
    APTR                ns_AutoIpStack;
    BOOL                ns_AutoIpCreated;
    BOOL                ns_AutoIpRunning;

    NX_DNS              ns_Dns;
    BOOL                ns_DnsCreated;

    /* A BOUND notification can run on the DHCP client's own ThreadX task.
       It records the interface here; the next caller-thread resolver
       operation imports option 6 under the ordinary caller bracket. */
    AmiNsDnsPending     ns_DhcpDnsPending;
    AmiNsDhcpDnsLease   ns_DhcpDnsLease;
    AmiNsDhcpSearchLease ns_DhcpSearchLease;
    AmiNsDhcpDomainState ns_DhcpDomain;

#ifdef AMINETXDUO_IPV6
    /*
     * Recursive DNS servers a router advertised, RFC 8106.  ami_ns6_rdnss()
     * runs on the IP thread and must not call into the DNS client, so it only
     * writes here and the next lookup takes what it finds.  DNSSL is the same.
     */
    AmiNsRaPending      ns_Ra;
    char                ns_DnsslApplied[AMI_CFG_MAX_SEARCH]
                                       [AMI_CFG_NAME_LEN];
    UWORD               ns_DnsslAppliedCount;
    char                ns_DnsslDefault[AMI_CFG_NAME_LEN];
#endif
#ifdef NX_DNS_CACHE_ENABLE
    /* Inline rather than separately allocated, and explicitly aligned:
       nx_dns_cache_initialize() rejects a buffer that is not longword aligned,
       and m68k gives a ULONG only two-byte alignment by default. */
    ULONG               ns_DnsCache[AMI_DNS_CACHE_BYTES / sizeof(ULONG)]
                            __attribute__((aligned(4)));
#endif

#ifdef AMINETXDUO_MDNS
    /*
     * The responder, its thread stack and its two caches.  Both caches are
     * longword-aligned because the module lays resource records out from both
     * ends of the buffer and m68k gives a UCHAR array no alignment.
     */
    NX_MDNS             ns_Mdns;
    APTR                ns_MdnsStack;
    BOOL                ns_MdnsCreated;
    BOOL                ns_MdnsClaimed;     /* probing finished, name is ours */
    char                ns_MdnsLabel[NX_MDNS_HOST_NAME_MAX];  /* as configured */
    UCHAR               ns_MdnsLocalCache[AMI_MDNS_LOCAL_CACHE_BYTES]
                            __attribute__((aligned(4)));
    UCHAR               ns_MdnsPeerCache[AMI_MDNS_PEER_CACHE_BYTES]
                            __attribute__((aligned(4)));
#endif

#ifdef AMINETXDUO_IPV6
    BOOL                ns_Ipv6Enabled;

    /*
     * DHCPv6.  netstack_dhcpv6.c owns all of it.  The NX_DHCPV6 is inline,
     * while both stacks are allocated, because they exist only on a machine
     * that asked for DHCPv6 and one that did not must not carry 6 KB.
     */
    NX_DHCPV6           ns_Dhcpv6;
    APTR                ns_Dhcpv6Stack;
    APTR                ns_Dhcpv6WorkStack;
    TX_THREAD           ns_Dhcpv6Work;
    TX_EVENT_FLAGS_GROUP ns_Dhcpv6Events;
    BOOL                ns_Dhcpv6Created;
    BOOL                ns_Dhcpv6Started;
    BOOL                ns_Dhcpv6EventsReady;
    BOOL                ns_Dhcpv6WorkReady;
    /* TRUE for the IA_NA path, FALSE for an Information-Request. */
    BOOL                ns_Dhcpv6Stateful;
    /*
     * A router advertisement has already asked for DHCPv6 once.  Every
     * advertisement repeats the M and O flags, so without this each would
     * restart the exchange.  The IP thread writes; it and the worker read.
     */
    volatile BOOL       ns_Dhcpv6Asked;
    /* A Reply has landed and may carry name servers.  Same two-phase rule as
       ns_Ra: the client's thread writes, a caller thread absorbs. */
    volatile BOOL       ns_Dhcpv6DnsPending;
    /* TRUE only while the retained NetX option buffers describe a live,
       coherent exchange. Link-down and lease loss clear it before publishing
       an empty replacement through ns_Dhcpv6DnsPending. */
    volatile BOOL       ns_Dhcpv6OptionsValid;
    /* nx_dhcpv6_inform_req_responses as it stood at the last state change: the
       client's own counter is cumulative and never reset, so only the
       difference is about the exchange that just ended.  Its thread owns it. */
    ULONG               ns_Dhcpv6InformSeen;
    /* The name servers the last Reply named.  Reconciliation has to know which
       entries in resolver.nameserver6[] came from which source, or each absorb
       would withdraw the other's; netstack_dns.c states which source wins. */
    NXD_ADDRESS         ns_Dhcpv6Dns[AMI_RDNSS_MAX];
    UWORD               ns_Dhcpv6DnsCount;
    /* Search suffixes whose reference ownership was acquired from the last
       coherent DHCPv6 Domain Search List. This is separate from the resolver
       list so a replacement Reply can release only DHCPv6's references. */
    char                ns_Dhcpv6SearchApplied[AMI_CFG_MAX_SEARCH]
                                             [AMI_CFG_NAME_LEN];
    UWORD               ns_Dhcpv6SearchAppliedCount;
    UBYTE               ns_Dhcpv6Iface;
    UBYTE               ns_Dhcpv6State;     /* NX_DHCPV6_STATE_*             */
#endif
};

#ifdef AMINETXDUO_IPV6
/* netstack_ipv6.c.  Both callers must hold a ThreadX bracket.
   _configure_one() is idempotent: an interface that already has its
   link-local is left alone. */
LONG ami_netstack_ipv6_enable(AmiNetStack *ns);
VOID ami_netstack_ipv6_configure(AmiNetStack *ns);
VOID ami_netstack_ipv6_interface_up(AmiNetStack *ns, UWORD interface_index);

/*
 * netstack_dhcpv6.c.  _configure() does nothing unless some interface asked
 * for CONFIGURE6=DHCP or AUTO.  _release() gives the address back and must run
 * while the interface can still transmit.
 */
VOID ami_netstack_dhcpv6_configure(AmiNetStack *ns);
VOID ami_netstack_dhcpv6_release(AmiNetStack *ns);
VOID ami_netstack_dhcpv6_pause(AmiNetStack *ns);
VOID ami_netstack_dhcpv6_resume(AmiNetStack *ns, UWORD interface_index);
VOID ami_netstack_dhcpv6_destroy(AmiNetStack *ns);
VOID ami_netstack_dhcpv6_address_notify(NX_IP *ip_ptr, UINT status,
                                        UINT interface_index,
                                        UINT address_index, ULONG *address);

/* netstack_ipv6.c, called from netstack_dhcpv6.c only.  See its comment. */
VOID ami_netstack_ipv6_reclaim_notify(AmiNetStack *ns);
VOID ami_netstack_ipv6_configure_one(AmiNetStack *ns, UWORD index);
#endif

/* --------------------------------------------------------- bring-up marks */

/*
 * One line per bring-up milestone, "netstack: mark <event> <ms> ms", stamped
 * with ami_millis().  tests/ipv6/run-bringup.sh reads it and the event names
 * are the keys of that script.
 */
VOID ami_netstack_mark(const char *event);

/* ------------------------------------------------------------- baton hooks */

/*
 * Registered with the SANA-II shim through ami_sana2_set_block_hooks(). See
 * netstack_baton.c: these make it safe for a ThreadX thread to block in exec
 * Wait() for an IORequest.
 */
VOID ami_netstack_baton_release(VOID);
VOID ami_netstack_baton_acquire(VOID);
BOOL ami_netstack_baton_abandon(TX_THREAD *thread);

/*
 * The public anchor for the baton counters and for the tick task counters,
 * published for as long as the stack is up. netstack_baton.c, and
 * include/aminetxduo/health.h for what reads it.
 */
VOID ami_netstack_health_publish(VOID);
VOID ami_netstack_health_unpublish(VOID);

/*
 * Through a pointer rather than by name: tests/bracket compiles
 * netstack_baton.c on its own against threadx_port, without the rest of the
 * stack.  NULL until the pool exists, and NULL again before it goes.
 */
VOID ami_netstack_baton_set_sampler(VOID (*fn)(VOID));

/* The per-operation half of netstack_pool_sample(): the running minimum only,
   which is the one figure that cannot be read back later. */
VOID netstack_pool_mark_low(VOID);

/* Wipe the slot table. Only valid once ThreadX has stopped.  netstack_baton.c
   says what it is for. */
VOID ami_netstack_baton_reset(VOID);

#ifdef AMINETXDUO_BPF
/* netstack_capture.c, registers the interfaces with src/bpf/ and installs
   the IP-level filter, the only way to trace loopback. */
VOID ami_netstack_capture_start(AmiNetStack *ns);
VOID ami_netstack_capture_stop(AmiNetStack *ns);

/* The same, for one interface that appeared or went away after start-up. */
VOID ami_netstack_capture_attach_one(AmiNetStack *ns, UWORD index);
VOID ami_netstack_capture_detach_one(AmiNetStack *ns, UWORD index);

/* Pin the SANA-II allocation behind a BPF capture cookie while it is used. */
LONG ami_netstack_interface_claim_cookie(APTR cookie, UWORD *index_out);
#endif

/* ---------------------------------------------------------------- resolver */

LONG ami_netstack_dns_start(AmiNetStack *ns);
VOID ami_netstack_dns_stop(AmiNetStack *ns);
/* FALSE when it kept the last coherent option set rather than acting on a
   partial read: the caller re-marks the interface so it is tried again. */
BOOL ami_netstack_dns_dhcp_reconcile(AmiNetStack *ns, UWORD interface_index);
VOID ami_netstack_dns_dhcp_changed(AmiNetStack *ns, UWORD interface_index);

/* Bounded string copy, always NUL-terminating. netstack_dns.c. */
VOID ami_ns_copy_name(char *dst, const char *src, ULONG size);

#ifdef AMINETXDUO_IPV6
/* nx_ipv6_rdnss_notify, on the IP thread. netstack_dns.c. */
VOID ami_ns6_rdnss(NX_IP *ip_ptr, UINT interface_index, ULONG *dns_address,
                   ULONG lifetime);

/* nx_ipv6_dnssl_notify, on the IP thread. netstack_dns.c. */
VOID ami_ns6_dnssl(NX_IP *ip_ptr, UINT interface_index, UCHAR *domains,
                   UINT length, ULONG lifetime);
#endif

/* One DHCP option that is text, from the lease of one interface. Not
   NUL-terminated on the wire.  `out` always is. netstack.c. */
VOID ami_ns_dhcp_text(AmiNetStack *ns, UWORD index, UINT option,
                      char *out, ULONG outlen);

#ifdef AMINETXDUO_MDNS
/* netstack_mdns.c, the RFC 6762 responder, and the ".local" branch
   netstack_resolve() takes before it reaches the unicast DNS client. */
LONG ami_netstack_mdns_start(AmiNetStack *ns);
VOID ami_netstack_mdns_stop(AmiNetStack *ns);
/* One interface, either way, while the stack runs.  netstack_iface_mdns_set()
   in <aminetxduo/netstack.h> is the published spelling. */
LONG ami_netstack_mdns_iface_set(AmiNetStack *ns, UWORD index, BOOL enable);
LONG ami_netstack_mdns_resolve(const char *name, ULONG *addr_out,
                               ULONG timeout_ticks);
/* The browse is public to bsdsocket.library.  See <aminetxduo/netstack.h>. */
#endif

/* ------------------------------------------------------------ AMITCP port,
 *
 * netstack_rexx.c, the AMITCP public port and the ARexx host servicing it.
 * Without AMINETXDUO_AREXX there is no host and netstack.c opens the port on
 * its own, so `WaitForPort AMITCP` still returns and a script still blocks.
 */
#ifdef AMINETXDUO_AREXX
VOID ami_netstack_rexx_start(VOID);
VOID ami_netstack_rexx_stop(VOID);

/* Called around every SANA-II OpenDevice, via ami_sana2_set_open_hooks(). */
VOID ami_netstack_rexx_suspend(VOID);
VOID ami_netstack_rexx_resume(VOID);
#endif

/* The singleton, without the "is it up" filtering the public accessor does. */
AmiNetStack *ami_netstack_raw(VOID);

/*
 * Green builds route WaitIO()/WaitPort() through checked wrappers which convert
 * only operations that would block.  Probe builds additionally intercept raw
 * Wait().  All converted blocking calls are counted in gs_stray_wait.
 */
#ifdef AMINETXDUO_GREEN_REALM
/* <proto/exec.h> forced first: the NDK's inline Wait macro must expand (once,
   behind its guard) BEFORE ours is defined, or a TU including it later has
   ours silently replaced -- the NDK path is -isystem, so it never warns.  */
#include <proto/exec.h>
BYTE ami_green_checked_waitio(struct IORequest *request);
struct Message *ami_green_checked_waitport(struct MsgPort *port);

#undef WaitIO
#define WaitIO(request) ami_green_checked_waitio(request)
#undef WaitPort
#define WaitPort(port) ami_green_checked_waitport(port)

#ifdef AMINETXDUO_RXPROBE
ULONG ami_green_checked_wait(ULONG sigmask);
#undef Wait
#define Wait(sigmask) ami_green_checked_wait(sigmask)
#endif
#endif

#endif /* AMINETXDUO_NETSTACK_INTERNAL_H */

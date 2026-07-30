/*
 * AmiNetXDuo -- netstack singleton internals.
 *
 * Private to src/netstack/. Include order matters: tx_api.h and nx_api.h come
 * before any exec header, because <exec/types.h> turns VOID into a macro and
 * that breaks the ThreadX typedefs.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_NETSTACK_INTERNAL_H
#define AMINETXDUO_NETSTACK_INTERNAL_H

#include "tx_api.h"
#include "nx_api.h"

#include "nxd_dhcp_client.h"
#include "nxd_dns.h"
#include "nx_auto_ip.h"
#ifdef AMINETXDUO_MDNS
#include "nxd_mdns.h"
#endif

#include <exec/types.h>

#include "aminetxduo/netstack.h"
#include "aminetxduo/config.h"
#include "aminetxduo/compat.h"
#include "aminetxduo/sana2.h"

/* ------------------------------------------------------------- tunables -- */

#include "../thread_priorities.h"

#define AMI_IP_STACK_SIZE           4096
#define AMI_ARP_CACHE_SIZE          1024
#define AMI_AUTOIP_STACK_SIZE       2048

/* Fraction of AvailMem() the packet pool may claim (1/AMI_POOL_MEM_DIVISOR). */
#define AMI_POOL_MEM_DIVISOR        16

/*
 * DNS answer cache size.
 *
 * NetX Duo's cache is one buffer with resource records growing up from the
 * bottom and the strings they name growing down from the top.  On this target
 * an NX_DNS_RR is 20 bytes and a name costs ((len & ~3) + 8) in the string
 * table, so a cached A record for a 15-character host name such as
 * "www.example.com" is 20 + 20 = 40 bytes; eight bytes of the buffer are the
 * two end pointers.  2048 bytes is therefore about fifty cached names.
 *
 * Real workloads are far smaller: a shell session resolves one host, `fetch`
 * following redirects two or three, the tests/curl suite one peer.  The
 * largest consumer is the reverse lookups ShowNetStatus NAMES and netstat do,
 * one per peer address on screen, bounded by TOOL_MAX_SOCK (32).
 *
 * Undersizing costs only a DNS query, since NetX Duo replaces the least
 * recently used record rather than failing; oversizing costs resident memory
 * forever.  2048 bytes is 0.05% of the 4 MB floor target, and a fifth of the
 * 9,792-byte packet pool the DNS client already carries inside the same
 * NX_DNS (10,112 bytes) for its own queries, measured on this toolchain.
 */
#define AMI_DNS_CACHE_BYTES         2048

#ifdef AMINETXDUO_MDNS
/*
 * The two mDNS cache sizes.
 *
 * Same layout as the DNS answer cache above: fixed records growing up from the
 * bottom of one buffer, the strings they name growing down from the top, the
 * two end pointers in between.  An NX_MDNS_RR is larger than an NX_DNS_RR --
 * it carries the record's state machine, its retransmit counters and its
 * interface index as well as the data -- so the arithmetic differs.
 *
 * The local cache holds what this machine claims: the A record for
 * <host>.local per interface, plus four records per declared service -- an
 * SRV, a TXT and two PTRs, one of them the _services._dns-sd._udp enumeration
 * -- and the names they point at.  An NX_MDNS_RR is 56 bytes and the names run
 * to about 90 more, so 384 per service covers it with room over; the base
 * kilobyte is the host's own name.  If it will not hold our own name, the
 * machine has no name; if it will not hold a service, that service is not
 * advertised and ami_ns_mdns_services() says which.
 *
 * The peer cache holds what has been learnt; full means the oldest record is
 * evicted, nothing fails.  Every .local lookup lands here, so it is sized
 * against the same workload as AMI_DNS_CACHE_BYTES -- one or two names per
 * shell session, reverse lookups behind `netstat` and ShowNetStatus NAMES
 * bounded by TOOL_MAX_SOCK (32) -- and undersizing costs only a query on the
 * wire.
 *
 * Both are inline in the AmiNetStack for the reason ns_DnsCache is: identical
 * lifetime, small, and an allocation that could fail would need a "no mDNS"
 * path.  Together they are 6 KB.
 */
#define AMI_MDNS_LOCAL_CACHE_BYTES  \
    (1024 + AMI_CFG_MAX_SD_SERVICES * 384)
#define AMI_MDNS_PEER_CACHE_BYTES   2048
#endif

/* How long netstack_startup() blocks waiting for the first address. */
#define AMI_DHCP_TIMEOUT_TICKS      (30UL * (ULONG)NX_IP_PERIODIC_RATE)
#define AMI_LINK_TIMEOUT_TICKS      (10UL * (ULONG)NX_IP_PERIODIC_RATE)

/*
 * How long to wait after the RFC 3927 fallback fires. The probe/announce
 * sequence is PROBE_WAIT (0-1 s) + PROBE_NUM probes (1-2 s each) + a claim,
 * so 15 s is ample.
 */
#define AMI_AUTOIP_TIMEOUT_TICKS    (15UL * (ULONG)NX_IP_PERIODIC_RATE)

/*
 * Granularity of the address-arrival poll, and so how long a DHCP lease sits
 * unnoticed after it arrives. A tenth of a second cost 67 ms of a 980 ms
 * AddNetInterface once the client's startup delay had gone. One tick is the
 * floor; each poll is two loads and a compare per interface, so a full
 * thirty-second wait for a server that never answers costs 1,500 of them.
 */
#define AMI_ADDRESS_POLL_TICKS      1UL

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

    AmiSana2If         *ns_Iface[AMI_CFG_MAX_INTERFACES];
    UWORD               ns_IfaceCount;

    NX_DHCP             ns_Dhcp;
    BOOL                ns_DhcpCreated;
    BOOL                ns_DhcpStarted;

    /*
     * Last DHCP state and last address seen per interface. NetX Duo's
     * callbacks report only the new value, so the previous one is kept here to
     * let the notifications report transitions such as a lost lease.
     */
    UBYTE               ns_DhcpState[AMI_CFG_MAX_INTERFACES];
    ULONG               ns_LastAddress[AMI_CFG_MAX_INTERFACES];

    /*
     * Posted by ami_ns_address_changed() when an interface gains or loses an
     * address, so ami_ns_wait_for_address() can block instead of polling.
     */
    TX_SEMAPHORE        ns_AddrArrived;
    BOOL                ns_AddrArrivedReady;

    NX_AUTO_IP          ns_AutoIp;
    APTR                ns_AutoIpStack;
    BOOL                ns_AutoIpCreated;
    BOOL                ns_AutoIpRunning;

    NX_DNS              ns_Dns;
    BOOL                ns_DnsCreated;
#ifdef NX_DNS_CACHE_ENABLE
    /* Inline rather than separately allocated: small, same lifetime as the
       NX_DNS it belongs to, and an allocation that can fail would need a "no
       cache" path.  Explicitly aligned because nx_dns_cache_initialize()
       rejects a buffer that is not longword aligned and m68k gives a ULONG
       only two-byte alignment by default. */
    ULONG               ns_DnsCache[AMI_DNS_CACHE_BYTES / sizeof(ULONG)]
                            __attribute__((aligned(4)));
#endif

#ifdef AMINETXDUO_MDNS
    /*
     * The responder, its thread stack and its two caches. Sized in
     * netstack_mdns.c; both caches are inline for the reason ns_DnsCache is,
     * and longword-aligned because the module lays resource records out from
     * both ends of the buffer and m68k gives a UCHAR array no alignment.
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
#endif
};

#ifdef AMINETXDUO_IPV6
/* netstack_ipv6.c -- run from ami_ns_create_ip()/ami_ns_configure_addresses()
   at the points marked there, and from nowhere else. */
LONG ami_netstack_ipv6_enable(AmiNetStack *ns);
VOID ami_netstack_ipv6_configure(AmiNetStack *ns);
#endif

/* ------------------------------------------------------------- baton hooks */

/*
 * Registered with the SANA-II shim through ami_sana2_set_block_hooks(). See
 * netstack_baton.c: these make it safe for a ThreadX thread to block in exec
 * Wait() for an IORequest.
 */
VOID ami_netstack_baton_release(VOID);
VOID ami_netstack_baton_acquire(VOID);

/*
 * Bracket counters. A freeze in here leaves nothing behind -- no Enforcer hit,
 * and a log that never reached disk -- so the evidence has to survive in memory
 * for a debugger or a later reader to pick up.
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

/* ---------------------------------------------------------- adoption glue --
 *
 * AmiNetCaller / ami_netstack_enter() / ami_netstack_leave() are public; they
 * live in include/aminetxduo/netstack.h so bsdsocket.library and the tools
 * share this bracket rather than growing their own.
 */

#ifdef AMINETXDUO_BPF
/* netstack_capture.c -- registers the interfaces with src/bpf/ and installs
   the IP-level filter, the only way to trace loopback. */
VOID ami_netstack_capture_start(AmiNetStack *ns);
VOID ami_netstack_capture_stop(AmiNetStack *ns);

/* The same, for one interface that appeared or went away after start-up. */
VOID ami_netstack_capture_attach_one(AmiNetStack *ns, UWORD index);
VOID ami_netstack_capture_detach_one(AmiNetStack *ns, UWORD index);
#endif

/* ---------------------------------------------------------------- resolver */

LONG ami_netstack_dns_start(AmiNetStack *ns);
VOID ami_netstack_dns_stop(AmiNetStack *ns);

#ifdef AMINETXDUO_MDNS
/* netstack_mdns.c -- the RFC 6762 responder, and the ".local" branch
   netstack_resolve() takes before it reaches the unicast DNS client. */
LONG ami_netstack_mdns_start(AmiNetStack *ns);
VOID ami_netstack_mdns_stop(AmiNetStack *ns);
BOOL ami_netstack_mdns_is_local(const char *name);
LONG ami_netstack_mdns_resolve(const char *name, ULONG *addr_out,
                               ULONG timeout_ticks);
#endif

/* ------------------------------------------------------------ AMITCP port --
 *
 * netstack_rexx.c -- the AMITCP public port and the ARexx host servicing it.
 * The port is what `WaitForPort AMITCP` waits on; the host is why a script that
 * addresses it gets an answer instead of blocking (docs/RESEARCH.md 75.7).
 */
VOID ami_netstack_rexx_start(VOID);
VOID ami_netstack_rexx_stop(VOID);

/* Called around every SANA-II OpenDevice, via ami_sana2_set_open_hooks(). */
VOID ami_netstack_rexx_suspend(VOID);
VOID ami_netstack_rexx_resume(VOID);

/* The singleton, without the "is it up" filtering the public accessor does. */
AmiNetStack *ami_netstack_raw(VOID);

#endif /* AMINETXDUO_NETSTACK_INTERNAL_H */

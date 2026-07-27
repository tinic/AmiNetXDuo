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

/*
 * ThreadX priorities, lowest number wins. The SANA-II readers (priority 2,
 * src/sana2/sana2_internal.h) must outrank the IP thread so a burst drains
 * into the pool rather than being dropped on the wire, and the IP thread must
 * outrank everything that consumes packets.
 */
#define AMI_IP_THREAD_PRIORITY      1
#define AMI_AUTOIP_PRIORITY         3
#define AMI_CALLER_PRIORITY        16      /* adopted application tasks      */

#define AMI_IP_STACK_SIZE           4096
#define AMI_ARP_CACHE_SIZE          1024
#define AMI_AUTOIP_STACK_SIZE       2048

/* Fraction of AvailMem() the packet pool may claim (1/AMI_POOL_MEM_DIVISOR). */
#define AMI_POOL_MEM_DIVISOR        16

/*
 * THE DNS ANSWER CACHE, and how this number was picked.
 *
 * NetX Duo's cache is one buffer with resource records growing up from the
 * bottom and the strings they name growing down from the top.  On this target
 * an NX_DNS_RR is 20 bytes and a name costs ((len & ~3) + 8) in the string
 * table, so a cached A record for a typical host name -- "www.example.com",
 * 15 characters -- is 20 + 20 = 40 bytes, and eight bytes of the buffer are
 * the two end pointers.  2048 bytes is therefore about FIFTY cached names.
 *
 * Fifty against what?  A shell session resolves one host and then talks to it;
 * `fetch` following redirects resolves two or three; the whole tests/curl
 * suite names one peer.  The largest real consumer is not forward lookups at
 * all but the reverse ones ShowNetStatus NAMES and netstat do, one per peer
 * address on screen, and that is bounded by TOOL_MAX_SOCK (32).  Fifty covers
 * both at once with room left, and there is no workload on an Amiga that
 * wants five hundred.
 *
 * Being wrong is cheap in one direction and not the other, which is why the
 * number leans small.  Too small costs a DNS query -- exactly what happened
 * before the cache existed, so the worst case is the old behaviour; NetX Duo
 * replaces the least recently used record rather than failing.  Too large
 * costs resident memory on a 4 MB machine forever.  2048 bytes is 0.05% of
 * the floor target's RAM, and a fifth of the 9,792-byte packet pool the DNS
 * client already carries inside the same NX_DNS (10,112 bytes) for its own
 * queries -- measured on this toolchain, not estimated.
 */
#define AMI_DNS_CACHE_BYTES         2048

#ifdef AMINETXDUO_MDNS
/*
 * THE TWO mDNS CACHES, and how these numbers were picked.
 *
 * The module keeps records in the same layout the DNS answer cache above
 * describes: fixed records growing up from the bottom of one buffer, the
 * strings they name growing down from the top, the two end pointers in
 * between.  An NX_MDNS_RR is larger than an NX_DNS_RR -- it carries the
 * record's state machine, its retransmit counters and its interface index as
 * well as the data -- so the arithmetic is not the same, but the shape is.
 *
 * LOCAL holds what this machine CLAIMS.  That is exactly one thing today: the
 * A record for <host>.local, per interface.  It is sized for a handful so that
 * adding a service later (a PTR, an SRV and a TXT, which is what one service
 * costs) does not send somebody back here, and it is the cache in which a
 * failure actually matters -- if it will not hold our own name, the machine
 * does not have a name.
 *
 * PEER holds what has been LEARNT, and is a cache in the ordinary sense: full
 * means the oldest record goes, not that anything fails.  Every .local lookup
 * lands here, so it is sized against the same workload AMI_DNS_CACHE_BYTES is
 * -- a shell session resolves one or two names, the reverse lookups behind
 * `netstat` and ShowNetStatus NAMES are bounded by TOOL_MAX_SOCK (32) -- and
 * being wrong is cheap in the same direction: too small costs a query on the
 * wire, which is what a machine without the cache does every time.
 *
 * Both are inside the AmiNetStack rather than separately allocated, for the
 * reason ns_DnsCache is: identical lifetime, small, and an allocation that
 * could fail would need a "no mDNS" path for no benefit.  Together they are
 * 3 KB -- 0.07% of the 4 MB floor target.
 */
#define AMI_MDNS_LOCAL_CACHE_BYTES  1024
#define AMI_MDNS_PEER_CACHE_BYTES   2048
#endif

/* How long netstack_startup() blocks waiting for the first address. */
#define AMI_DHCP_TIMEOUT_TICKS      (30UL * (ULONG)NX_IP_PERIODIC_RATE)
#define AMI_LINK_TIMEOUT_TICKS      (10UL * (ULONG)NX_IP_PERIODIC_RATE)

/*
 * And how long after the RFC 3927 fallback fires. The whole probe/announce
 * sequence is PROBE_WAIT (0-1 s) + PROBE_NUM probes (1-2 s each) + a claim,
 * so 15 s is generous; the previous code waited another AMI_DHCP_TIMEOUT_TICKS
 * here, which is thirty seconds of nothing after a thirty-second wait that
 * had already failed.
 */
#define AMI_AUTOIP_TIMEOUT_TICKS    (15UL * (ULONG)NX_IP_PERIODIC_RATE)

/* Granularity of the "has anything got an address yet?" poll. */
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

    AmiSana2If         *ns_Iface[AMI_CFG_MAX_INTERFACES];
    UWORD               ns_IfaceCount;

    NX_DHCP             ns_Dhcp;
    BOOL                ns_DhcpCreated;
    BOOL                ns_DhcpStarted;

    /*
     * The last DHCP state seen per interface, and the last address seen on
     * it. Both exist so the notification callbacks can say what CHANGED --
     * "the lease was lost" is a transition, not a state, and NetX Duo's
     * callbacks report only the new value.
     */
    UBYTE               ns_DhcpState[AMI_CFG_MAX_INTERFACES];
    ULONG               ns_LastAddress[AMI_CFG_MAX_INTERFACES];

    NX_AUTO_IP          ns_AutoIp;
    APTR                ns_AutoIpStack;
    BOOL                ns_AutoIpCreated;
    BOOL                ns_AutoIpRunning;

    NX_DNS              ns_Dns;
    BOOL                ns_DnsCreated;
#ifdef NX_DNS_CACHE_ENABLE
    /* Inline rather than a separate allocation: it is small, it has the same
       lifetime as the NX_DNS it belongs to, and an allocation that can fail
       would need a "no cache" path for no benefit.  Explicitly aligned because
       nx_dns_cache_initialize() rejects a buffer that is not longword aligned
       and m68k gives a ULONG only two bytes by default. */
    ULONG               ns_DnsCache[AMI_DNS_CACHE_BYTES / sizeof(ULONG)]
                            __attribute__((aligned(4)));
#endif

#ifdef AMINETXDUO_MDNS
    /*
     * The responder, its thread stack and its two caches. Sized and justified
     * in netstack_mdns.c; both caches are inline for the reason ns_DnsCache
     * is, and longword-aligned because the module lays resource records out
     * from both ends of the buffer and m68k gives a UCHAR array no alignment
     * at all.
     */
    NX_MDNS             ns_Mdns;
    APTR                ns_MdnsStack;
    BOOL                ns_MdnsCreated;
    BOOL                ns_MdnsClaimed;     /* probing finished, name is ours */
    char                ns_MdnsLabel[NX_MDNS_HOST_NAME_MAX];  /* as CONFIGURED */
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
 * netstack_baton.c -- these are what make it legal for a ThreadX thread to
 * block in exec Wait() for an IORequest.
 */
VOID ami_netstack_baton_release(VOID);
VOID ami_netstack_baton_acquire(VOID);

/* ---------------------------------------------------------- adoption glue --
 *
 * AmiNetCaller / ami_netstack_enter() / ami_netstack_leave() are PUBLIC --
 * they live in include/aminetxduo/netstack.h so bsdsocket.library and the
 * tools share this bracket rather than growing their own.
 */

#ifdef AMINETXDUO_BPF
/* netstack_capture.c -- registers the interfaces with src/bpf/ and installs
   the IP-level filter that is the only way loopback can be traced. */
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

/* The singleton, without the "is it up" filtering the public accessor does. */
AmiNetStack *ami_netstack_raw(VOID);

#endif /* AMINETXDUO_NETSTACK_INTERNAL_H */

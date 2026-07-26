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
#endif

/* ---------------------------------------------------------------- resolver */

LONG ami_netstack_dns_start(AmiNetStack *ns);
VOID ami_netstack_dns_stop(AmiNetStack *ns);

/* The singleton, without the "is it up" filtering the public accessor does. */
AmiNetStack *ami_netstack_raw(VOID);

#endif /* AMINETXDUO_NETSTACK_INTERNAL_H */

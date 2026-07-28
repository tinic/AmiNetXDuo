/*
 * AmiNetXDuo tools -- reading the RUNNING NetX Duo instance.
 *
 * Only ShowNetStatus and netstat need this; the rest of the tools stay clear
 * of the stack's internals entirely.
 *
 *   This file used to hand each command an NX_IP * from netstack_ip() and let
 *   it walk NetX Duo's tables itself, after adopting the Process as a
 *   TX_THREAD.  That could never work in a shipped build and did not:
 *
 *     * A Shell command links its OWN copy of ThreadX and NetX Duo.  Its
 *       kernel was never entered and its NX_IP owns no interfaces.  The stack
 *       that is running lives inside bsdsocket.library, in that library's own
 *       copy of the same archives.
 *     * No tool links aminetxduo_netstack, so netstack_ip() resolved to
 *       src/tools/netstack_weak.c's weak stub and returned NULL -- in every
 *       build that was ever shipped.
 *
 *   So `netstat`, `ping` and ShowNetStatus's live path reached
 *   tool_require_stack(), got NULL, and printed "the network is up, but this
 *   command cannot read it".  Correct English, and a lie about what the
 *   commands could do.  docs/RESEARCH.md 21.
 *
 *   The snapshot now comes from bsdsocket.library through NetStackQuery()
 *   (include/aminetxduo/netstatus.h), which is the same idiom NetTrace
 *   already used for the capture engine.  The structures below are unchanged
 *   in shape -- they are what the two commands print from -- but they are
 *   filled by copying scalars across a library boundary rather than by
 *   walking another task's memory.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_TOOLS_NX_H
#define AMINETXDUO_TOOLS_NX_H

#include "tools.h"
#include "aminetxduo/sana2.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Suppress the failure messages the three calls below print by default.
 *
 * netstat wants them: it has nothing else to say and no report to spoil.
 * ShowNetStatus does not: it is printing a table, it words the "up but
 * unreadable" case itself and in its own place, and an error block halfway
 * through a report is worse than no error block at all.
 */
VOID tool_nx_quiet(BOOL quiet);

/* ----------------------------------------------------------- snapshots -- */

#define TOOL_MAX_IF     NX_MAX_PHYSICAL_INTERFACES
#define TOOL_MAX_SOCK   32

typedef struct ToolIfInfo
{
    UWORD           nx_index;
    BOOL            attached;        /* NX_INTERFACE is valid                */
    BOOL            link_up;
    ULONG           address;         /* host byte order                      */
    ULONG           netmask;
    ULONG           mtu;
    UBYTE           mac[AMI_ETH_ADDR_SIZE];
    char            nx_name[NETSTATUS_NAME_LEN];
    /*
     * The driver the RUNNING stack has open, which is not always the driver
     * the config file names: the file can be edited after the stack starts,
     * and an interface brought up by hand need never have been in it at all.
     * Reporting the file's answer to "what is this running on" is the same
     * mistake ShowNetStatus made with the name servers.
     *
     * Empty when the interface has no SANA-II device -- a loopback or an
     * unattached slot -- so an empty string is a fact, not a failure.
     */
    char            nx_device[NETSTATUS_DEVICE_LEN];
    ULONG           nx_unit;
    /* From the SANA-II shim, when the interface has one attached. */
    BOOL            have_sana2;
    BOOL            sana2_online;
    ULONG           bps;
    AmiSana2Stats   stats;
} ToolIfInfo;

typedef struct ToolSockInfo
{
    BOOL    is_tcp;
    UWORD   local_port;
    UWORD   peer_port;
    ULONG   peer_address;
    UINT    state;                   /* TCP only */
    ULONG   queued;                  /* UDP receive queue depth */
} ToolSockInfo;

typedef struct ToolSnapshot
{
    ToolIfInfo      iface[TOOL_MAX_IF];
    UWORD           iface_count;
    ToolSockInfo    sock[TOOL_MAX_SOCK];
    UWORD           sock_count;
    BOOL            sock_truncated;
    ULONG           gateway;
    BOOL            have_gateway;
    /* What this machine answers to on the local network, with the ".local".
       Empty when the build has no responder or it has not claimed one yet. */
    BOOL            have_mdns;
    char            mdns_name[NETSTATUS_NAME_LEN];
} ToolSnapshot;

/*
 * One question to the running library. `want_sockets` is honoured only when
 * the caller needs the connection table, because that is one more call across
 * the boundary. Returns 0, or a negative code after printing a message.
 */
LONG tool_snapshot(ToolSnapshot *out, BOOL want_sockets);

/* -------------------------------------------------- protocol counters -- */

/*
 * The per-protocol counters and the ARP cache, in one place so that no two
 * commands can report different numbers for the same thing: ShowNetStatus and
 * netstat both take this snapshot and only the layout of what they print
 * differs.
 *
 * A `have_*` flag is FALSE when the protocol in question is not enabled in the
 * running stack, which is not the same as "all its counters are zero" and must
 * not be printed as though it were.
 */

#define TOOL_MAX_ARP    32

typedef struct ToolArpEntry
{
    ULONG   address;                 /* host byte order                      */
    UBYTE   mac[AMI_ETH_ADDR_SIZE];
    BOOL    is_static;
    BOOL    resolved;                /* a hardware address has been learnt   */
    UWORD   retries;                 /* requests sent while unresolved       */
    UWORD   nx_index;                /* the interface it was learnt on       */
} ToolArpEntry;

typedef struct ToolStats
{
    BOOL            have_ip;
    ULONG           ip_packets_sent;
    ULONG           ip_bytes_sent;
    ULONG           ip_packets_received;
    ULONG           ip_bytes_received;
    ULONG           ip_invalid;
    ULONG           ip_receive_dropped;
    ULONG           ip_checksum_errors;
    ULONG           ip_send_dropped;
    ULONG           ip_fragments_sent;
    ULONG           ip_fragments_received;

    BOOL            have_icmp;
    ULONG           icmp_pings_sent;
    ULONG           icmp_ping_timeouts;
    ULONG           icmp_threads_suspended;
    ULONG           icmp_responses;
    ULONG           icmp_checksum_errors;
    ULONG           icmp_unhandled;

    BOOL            have_tcp;
    ULONG           tcp_packets_sent;
    ULONG           tcp_bytes_sent;
    ULONG           tcp_packets_received;
    ULONG           tcp_bytes_received;
    ULONG           tcp_invalid;
    ULONG           tcp_receive_dropped;
    ULONG           tcp_checksum_errors;
    ULONG           tcp_connections;
    ULONG           tcp_disconnections;
    ULONG           tcp_connections_dropped;
    ULONG           tcp_retransmits;

    BOOL            have_udp;
    ULONG           udp_packets_sent;
    ULONG           udp_bytes_sent;
    ULONG           udp_packets_received;
    ULONG           udp_bytes_received;
    ULONG           udp_invalid;
    ULONG           udp_receive_dropped;
    ULONG           udp_checksum_errors;

    BOOL            have_arp;
    ULONG           arp_requests_sent;
    ULONG           arp_requests_received;
    ULONG           arp_responses_sent;
    ULONG           arp_responses_received;
    ULONG           arp_dynamic_entries;
    ULONG           arp_static_entries;
    ULONG           arp_aged_entries;
    ULONG           arp_invalid_messages;
    ToolArpEntry    arp[TOOL_MAX_ARP];
    UWORD           arp_count;
    BOOL            arp_truncated;

    BOOL            have_pool;
    ULONG           pool_total;
    ULONG           pool_free;
    ULONG           pool_payload;
    ULONG           pool_empty_requests;
    ULONG           pool_empty_suspensions;
    ULONG           pool_invalid_releases;
} ToolStats;

/*
 * Everything above, in one round trip per table. Returns 0, or a negative
 * code after printing a message.
 */
LONG tool_stats(ToolStats *out);

/* --------------------------------------------------------------- DHCP -- */

/*
 * What the server said, per interface.  Nothing kept this before: the lease
 * was applied at bring-up and discarded, so a machine that got its address by
 * DHCP could not say who gave it out or for how long -- the two questions
 * that matter when the address changes underneath something.
 */

typedef struct ToolDhcpInfo
{
    UWORD   nx_index;
    UWORD   state;                   /* NETSTATUS_DHCP_*                     */
    ULONG   address;                 /* host byte order                      */
    ULONG   netmask;
    ULONG   server;                  /* 0 when it did not identify itself    */
    ULONG   lease_seconds;           /* 0 = not stated                       */
    ULONG   router[NETSTATUS_DHCP_ADDRS];
    UWORD   router_count;
    ULONG   dns[NETSTATUS_DHCP_ADDRS];
    UWORD   dns_count;
    ULONG   static_route[NETSTATUS_DHCP_ADDRS];
    UWORD   static_route_count;
    char    host_name[NETSTATUS_NAME_LEN];
    char    domain_name[NETSTATUS_NAME_LEN];
} ToolDhcpInfo;

typedef struct ToolDhcp
{
    ToolDhcpInfo    iface[TOOL_MAX_IF];
    UWORD           count;
} ToolDhcp;

/*
 * Returns 0, or a negative code.  A stack too old to know the selector is not
 * an error worth a message -- callers treat it as "no lease detail".
 */
LONG tool_dhcp(ToolDhcp *out);

/* ------------------------------------------------------------- routes -- */

#define TOOL_MAX_ROUTE  8

typedef struct ToolRoute
{
    ULONG   destination;
    ULONG   netmask;
    ULONG   gateway;                 /* 0 = directly attached                */
    UWORD   flags;                   /* NETSTATUS_RT_*                       */
    UWORD   nx_index;
} ToolRoute;

typedef struct ToolRoutes
{
    ToolRoute   route[TOOL_MAX_ROUTE];
    UWORD       count;
    BOOL        truncated;
    /*
     * FALSE when NX_ENABLE_IP_STATIC_ROUTING is not in the running stack's
     * build -- the directly-attached prefixes and the default gateway are
     * still reported and are still real, but there is no table to add to.
     */
    BOOL        static_routing;
} ToolRoutes;

LONG tool_routes(ToolRoutes *out);

/*
 * The table, printed. Both commands that print one call this, so they cannot
 * disagree about what the stack's routes are.
 *
 * `fmt` is how an address becomes text. NULL means ami_config_format_ip(), the
 * dotted quad; ShowNetStatus passes its own so that NAMES still turns a
 * gateway into a host name here as it does everywhere else in that report.
 */
typedef VOID (*ToolAddrText)(ULONG addr, char *buf, ULONG buflen);

VOID tool_print_routes(const ToolRoutes *routes, const AmiConfig *cfg,
                       ToolAddrText fmt);

/* The configured name of an nx_ip_interface[] slot, or "?". */
const char *tool_iface_name(const AmiConfig *cfg, UWORD index);

/* TCP state number -> the name netstat prints. */
const char *tool_tcp_state_name(UINT state);

#ifdef __cplusplus
}
#endif

#endif /* AMINETXDUO_TOOLS_NX_H */

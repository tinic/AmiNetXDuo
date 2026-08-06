/*
 * AmiNetXDuo tools, reading the running NetX Duo instance.
 *
 * Only ShowNetStatus and netstat need this; the rest of the tools stay clear
 * of the stack's internals.
 *
 * The snapshot comes from bsdsocket.library through NetStackQuery()
 * (include/aminetxduo/netstatus.h), the same idiom NetTrace uses for the
 * capture engine. The structures below are filled by copying scalars across a
 * library boundary, not by walking another task's memory.
 *
 * The earlier design handed each command an NX_IP * from netstack_ip() and let
 * it walk NetX Duo's tables. That cannot work in a shipped build: a Shell
 * command links its own copy of ThreadX and NetX Duo, its kernel is never
 * entered and its NX_IP owns no interfaces, while the running stack lives
 * inside bsdsocket.library's own copy of the same archives. No tool links
 * aminetxduo_netstack, so netstack_ip() resolved to src/tools/netstack_weak.c's
 * weak stub and returned NULL in every shipped build, leaving `netstat`, `ping`
 * and ShowNetStatus's live path printing "the network is up, but this command
 * cannot read it". docs/RESEARCH.md 21.
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
 * netstat wants them; ShowNetStatus does not, since it words the "up but
 * unreadable" case itself and an error block halfway through its table would
 * break the report.
 */
VOID tool_nx_quiet(BOOL quiet);

/* ----------------------------------------------------------- snapshots, */

#define TOOL_MAX_IF     NX_MAX_PHYSICAL_INTERFACES
#define TOOL_MAX_SOCK   32

/*
 * IPv6 addresses across all interfaces.  Each gets a link-local one and may
 * hold a configured or advertised global one alongside it, so two per
 * interface plus room to see a third arrive.
 */
#define TOOL_MAX_ADDR6  (TOOL_MAX_IF * 3)

typedef struct ToolIfInfo
{
    UWORD           nx_index;
    BOOL            attached;        /* NX_INTERFACE is valid                */
    BOOL            link_up;
    BOOL            mdns;            /* answering .local on this wire        */
    ULONG           address;         /* host byte order                      */
    ULONG           netmask;
    ULONG           mtu;
    UBYTE           mac[AMI_ETH_ADDR_SIZE];
    char            nx_name[NETSTATUS_NAME_LEN];
    /*
     * The driver the running stack has open, which is not always the driver
     * the config file names: the file can be edited after the stack starts,
     * and an interface brought up by hand need never have been in it.
     *
     * Empty when the interface has no SANA-II device, a loopback or an
     * unattached slot, so an empty string is a fact, not a failure.
     */
    char            nx_device[NETSTATUS_DEVICE_LEN];
    ULONG           nx_unit;
    /* From the SANA-II shim, when the interface has one attached. */
    BOOL            have_sana2;
    BOOL            sana2_online;
    ULONG           bps;
    AmiSana2Stats   stats;
} ToolIfInfo;

/*
 * One IPv6 address of one interface, already in text.
 *
 * The text is made while the library is open, because the only RFC 5952
 * formatter on the machine is bsdsocket.library's inet_ntop(), a command
 * built from an IPv4-only tree has none of its own, and the tools are one
 * binary whichever way the library was built.
 */
typedef struct ToolAddr6Info
{
    UWORD   nx_index;
    UWORD   state;                   /* NETSTATUS_IP6_*                      */
    ULONG   prefix;
    char    text[48];
} ToolAddr6Info;

/* "valid", "tentative"; NULL when the state needs no comment. */
const char *tool_addr6_state(UWORD state);

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
    /*
     * Empty on a machine whose library has no IPv6, and empty on one that has
     * it but has brought no interface up.  A library too old to know the
     * selector answers with an error, which is also empty and not a failure.
     */
    ToolAddr6Info   addr6[TOOL_MAX_ADDR6];
    UWORD           addr6_count;
    ToolSockInfo    sock[TOOL_MAX_SOCK];
    UWORD           sock_count;
    BOOL            sock_truncated;
    ULONG           gateway;
    BOOL            have_gateway;
    /* What this machine answers to on the local network, with the ".local".
       Empty when the build has no responder or it has not claimed one yet. */
    BOOL            have_mdns;
    char            mdns_name[NETSTATUS_NAME_LEN];
    /* AmiHostnameSource: which place named this machine. AMI_HOSTNAME_NONE
       when nothing did, or when the library is too old to say. */
    UWORD           host_source;
} ToolSnapshot;

/*
 * One question to the running library. Set `want_sockets` only when the
 * connection table is needed; it costs another call across the boundary.
 * Returns 0, or a negative code after printing a message.
 */
LONG tool_snapshot(ToolSnapshot *out, BOOL want_sockets);

/* -------------------------------------------------- protocol counters, */

/*
 * The per-protocol counters and the ARP cache in one place, so ShowNetStatus
 * and netstat cannot report different numbers for the same thing; only their
 * layout differs.
 *
 * A `have_*` flag is FALSE when the protocol is not enabled in the running
 * stack. That is not the same as "all its counters are zero" and must not be
 * printed as though it were.
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

/* ------------------------------------------------------- neighbours, */

/*
 * The IPv6 half of the address cache. There is no ARP in IPv6: neighbour
 * discovery does the same job and NetX Duo keeps its answers in a separate
 * table, so this is a separate snapshot rather than more rows in the one
 * above.
 *
 * The address arrives as text for ToolAddr6Info's reason: the library's
 * inet_ntop() is the only RFC 5952 formatter a command built from an
 * IPv4-only tree can reach.
 *
 * Empty on a machine whose library has no IPv6, and on one too old to know
 * the selector. Neither is a failure.
 */

#define TOOL_MAX_ND     16

typedef struct ToolNeighbour
{
    char    text[48];
    ULONG   addr[4];                 /* the same address, for comparing      */
    UBYTE   mac[AMI_ETH_ADDR_SIZE];
    UWORD   state;                   /* NETSTATUS_ND_*                       */
    UWORD   flags;                   /* NETSTATUS_ND_STATIC / _ROUTER        */
    UWORD   nx_index;
    UWORD   solicitations;           /* sent while unresolved                */
    UWORD   queued;                  /* packets held for the answer          */
} ToolNeighbour;

typedef struct ToolNeighbours
{
    ToolNeighbour   entry[TOOL_MAX_ND];
    UWORD           count;
    BOOL            truncated;
} ToolNeighbours;

LONG tool_neighbours(ToolNeighbours *out);

/* "REACHABLE", "STALE"; never NULL. */
const char *tool_nd_state_name(UWORD state);

/* What that state means, one sentence, or NULL when it needs no comment. */
const char *tool_nd_state_note(UWORD state);

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

    /* NETSTATUS_SYSTEM fills these, and NETSTATUS_HEALTH fills them again from
       the record the health mark points at, so -h and -s -h agree. pool_low is
       only in the health half. */
    BOOL            have_pool;
    ULONG           pool_total;
    ULONG           pool_free;
    ULONG           pool_low;
    ULONG           pool_payload;
    ULONG           pool_empty_requests;
    ULONG           pool_empty_suspensions;
    ULONG           pool_invalid_releases;

    /* NETSTATUS_HEALTH: whether the machine was ever held, not how much moved.
       FALSE against a library that predates the selector. health_mark is the
       address these came from when tool_health_mark() read them, and 0 when the
       library answered instead. */
    BOOL            have_health;
    ULONG           health_mark;
    ULONG           tick_ticks;
    ULONG           tick_clipped;
    ULONG           tick_lost;
    ULONG           tick_service_us;
    ULONG           tick_uptime_ms;
    ULONG           tick_worst_stall_ms;
    ULONG           tick_worst_service_us;
    ULONG           tick_over_budget;
    ULONG           tick_deferred;
    ULONG           tick_skew;
    ULONG           tick_skew_peak;
    ULONG           baton_live;
    ULONG           baton_live_max;
    ULONG           baton_full;
    ULONG           baton_transitions;
    ULONG           baton_state_max;
    ULONG           baton_moved;
    ULONG           baton_state_shared;

    /* What the stack owns right now, and the most it ever owned. A suspected
       leak is only answerable against these: AvailMem falls for every program
       on the machine and cannot say whose. */
    ULONG           alloc_live;
    ULONG           alloc_peak;
    ULONG           alloc_refused;
    ULONG           sockets;
    ULONG           sockets_peak;
    ULONG           opens;
} ToolStats;

/*
 * Everything above, in one round trip per table. Returns 0, or a negative
 * code after printing a message.
 */
LONG tool_stats(ToolStats *out);

/*
 * Only the health fields, and off the published mark rather than through the
 * library: no OpenLibrary(), no allocation, no lock taken, so it answers on a
 * machine that is already in trouble. TRUE if the mark was there.
 */
BOOL tool_health_mark(ToolStats *out);

/* --------------------------------------------------------------- DHCP, */

/*
 * What the server said, per interface. The lease used to be applied at
 * bring-up and discarded, so a DHCP-addressed machine could not say who gave
 * out its address or for how long.
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
 * an error; callers treat it as "no lease detail".
 */
LONG tool_dhcp(ToolDhcp *out);

/* ------------------------------------------------------------- routes, */

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
     * build, the directly-attached prefixes and the default gateway are
     * still reported and are still real, but there is no table to add to.
     */
    BOOL        static_routing;
} ToolRoutes;

LONG tool_routes(ToolRoutes *out);

/*
 * The IPv6 routes: the on-link prefixes and the default routers, in the order
 * the stack consults them. Separate from ToolRoutes because IPv6 has no
 * netmask and no single default gateway, there may be several default
 * routers, each with its own lifetime.
 *
 * Text rather than words, for ToolAddr6Info's reason.
 */
#define TOOL_MAX_ROUTE6 12

typedef struct ToolRoute6
{
    char    dest[48];
    char    next_hop[48];            /* empty = on link, no next hop         */
    /* The same destination in words, because text cannot be masked and a
       caller asking "is this address inside this prefix" needs to. */
    ULONG   dest_words[4];
    ULONG   prefix;
    ULONG   lifetime;                /* NETSTATUS_RT6_FOREVER = never expires */
    UWORD   flags;                   /* NETSTATUS_RT6_*                      */
    UWORD   nx_index;
} ToolRoute6;

typedef struct ToolRoutes6
{
    ToolRoute6  route[TOOL_MAX_ROUTE6];
    UWORD       count;
    BOOL        truncated;
} ToolRoutes6;

LONG tool_routes6(ToolRoutes6 *out);

/* The IPv6 table, printed. Prints nothing at all when there are no routes,
   so an IPv4-only machine's report is unchanged. */
VOID tool_print_routes6(const ToolRoutes6 *routes, const AmiConfig *cfg);

/*
 * The table, printed. Both commands that print one call this, so they cannot
 * disagree about the stack's routes.
 *
 * `fmt` turns an address into text. NULL means ami_config_format_ip(), the
 * dotted quad; ShowNetStatus passes its own so NAMES turns a gateway into a
 * host name here as it does elsewhere in that report.
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

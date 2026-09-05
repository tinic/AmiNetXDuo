/*
 * AmiNetXDuo, NetX Duo tuning for the AmigaOS floor target.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef NX_USER_H
#define NX_USER_H


/* ---------------------------------------------------------------- timing, */

/* Must equal TX_TIMER_TICKS_PER_SECOND in port/threadx-amiga/inc/tx_port.h.
   NetX Duo expresses its own rates as divisors of this, so a disagreement
   silently scales every TCP timer by the ratio. */

#define NX_IP_PERIODIC_RATE                     50


/* ------------------------------------------------------------- receive --- */

/*
 * The SANA-II reader runs IP input itself, under nx_ip_protection, rather than
 * queueing every frame to the IP thread (src/sana2/sana2_rx.c,
 * ami_sana2_rx_input).  TCP is the one protocol that would still cross:
 * nx_tcp_packet_receive.c queues the segment and wakes the IP thread for any
 * caller that is not the IP thread itself, which on a bulk read is a context
 * switch per segment onto a path the reader is already holding the lock for.
 *
 * _nx_ip_input_thread names the driver thread that is inside IP input right
 * now.  It is written only by that thread, and only between taking the mutex
 * and giving it back, so this comparison answers "am I the thread that already
 * holds what the IP thread would hold".  Everything else, the IP thread and
 * every ISR included, takes the stock path.
 */
struct TX_THREAD_STRUCT;
extern struct TX_THREAD_STRUCT *_nx_ip_input_thread;

#define NX_TCP_PACKET_RECEIVE_DIRECT(ip_ptr, packet_ptr)                      \
    ((_nx_ip_input_thread != 0) &&                                            \
     (_tx_thread_current_ptr == _nx_ip_input_thread))


/* --------------------------------------------------------------- packets, */

/* Must match AMI_POOL_PAYLOAD in include/aminetxduo/netstack.h. */
#define NX_AMIGA_POOL_PAYLOAD                   1568

/* NX_PHYSICAL_HEADER stays at its default 16, not Ethernet's 14: it puts the
   IP header on a longword boundary, and the SANA-II shim relies on the same
   offset. */

/* Packets may be chained: a TCP receive of more than one payload's worth
   arrives as a chain, and NX_DISABLE_PACKET_CHAIN would silently truncate it. */


/* ----------------------------------------------------------- interfaces --- */

/* One number, two names: CMakeLists.txt sets this and AMI_CFG_MAX_ATTACHED
   together from -DAMINETXDUO_MAX_INTERFACES, and a _Static_assert in
   src/netstack/netstack.c refuses a tree where they disagree.  Global: it
   changes the NX_IP layout, so every translation unit must see one value. */
#ifndef NX_MAX_PHYSICAL_INTERFACES
#define NX_MAX_PHYSICAL_INTERFACES              4
#endif


/* ------------------------------------------------------------------ ARP --- */

#define NX_ARP_MAX_QUEUE_DEPTH                  2

/* Seconds.  Governs unresolved entries only: NX_ARP_EXPIRATION_RATE is 0 here,
   so a resolved entry carries no next-update time and is never re-probed. */
#define NX_ARP_UPDATE_RATE                      1

#define NX_ARP_MAXIMUM_RETRIES                  8


/* ------------------------------------------------------------------ TCP --- */

#define NX_TCP_MAXIMUM_TX_QUEUE                 8

/* Compiles the per-socket receive-queue cap; src/bsdsocket/socket.c must size
   nx_tcp_socket_receive_queue_maximum from each socket's window or a sub-MSS
   peer pins the whole pool.  The pool-wide low watermark half stays inert. */
#define NX_ENABLE_LOW_WATERMARK

/* One entry per simultaneously LISTENING port, 44 bytes each, and the array
   lives inside NX_IP (nx_api.h:3368) -- so this is resident RAM, not a
   per-caller cost.  A second listen on a port already listening is rejected
   with NX_DUPLICATE_LISTEN and consumes nothing, and past the last entry
   nx_tcp_server_socket_listen returns NX_MAX_LISTEN, which errno.c maps to
   ENOBUFS.  32 was 1,408 bytes; 16 is 704 and still more listening ports at
   once than an Amiga has daemons.  NOTE: this define is unconditional, so
   -DNX_MAX_LISTEN_REQUESTS on the command line is silently ignored -- edit
   it here.  */
#define NX_MAX_LISTEN_REQUESTS                  16

/* Changes the NX_TCP_SOCKET layout: an ABI break for anything compiled against
   the old header, so it cannot be a per-file define. */
#define NX_ENABLE_EXTENDED_NOTIFY_SUPPORT

/* The backoff expression has no clamp, so NX_TCP_MAXIMUM_RETRIES is its only
   bound; the two must be changed together. */
#ifndef NX_TCP_RETRY_SHIFT
#define NX_TCP_RETRY_SHIFT                      1
#endif
#ifndef NX_TCP_MAXIMUM_RETRIES
#define NX_TCP_MAXIMUM_RETRIES                  6
#endif

#ifndef NX_TCP_SYN_MAXIMUM_RETRIES
#define NX_TCP_SYN_MAXIMUM_RETRIES              7
#endif

#ifndef AMINETXDUO_TCP_RTT_OFF
#define NX_ENABLE_TCP_RTT_ESTIMATOR
#endif

/* nx_tcp_socket_create.c sets nx_tcp_socket_keepalive_enabled unconditionally
   under this define.  src/bsdsocket/socket.c must clear it at create, or every
   socket is on keepalive whether the application asked or not. */
#define NX_ENABLE_TCP_KEEPALIVE

#define NX_ENABLE_SOURCE_ADDRESS_CHECK

#define NX_ENABLE_TCP_MSS_CHECK

/* Off, nxe_tcp_socket_create.c rejects a window above 65535; on, it accepts
   anything under 2^30.  BSD_TCP_WINDOW_CEILING has an arm for each. */
#ifdef AMINETXDUO_TCP_WINDOW_SCALING
#define NX_ENABLE_TCP_WINDOW_SCALING
#endif

#ifndef AMINETXDUO_TCP_TIMESTAMP_OFF
#define NX_ENABLE_TCP_TIMESTAMP
#endif


/* ----------------------------------------------------------------- SACK, */

/* Receive side only: blocks a peer sends us are not processed, so writes
   recover exactly as they did without it. */
#ifndef AMINETXDUO_TCP_SACK_OFF
#define NX_ENABLE_TCP_SACK
#endif


/* ------------------------------------------------- SEND SIDE LOSS RECOVERY, */

#ifndef AMINETXDUO_TCP_EARLY_RETRANSMIT_OFF
#define NX_ENABLE_TCP_EARLY_RETRANSMIT
#endif

/* Needs NX_ENABLE_TCP_RTT_ESTIMATOR, which supplies the two round trips. */
#ifndef AMINETXDUO_TCP_LOSS_PROBE_OFF
#define NX_ENABLE_TCP_LOSS_PROBE
#endif


/* ------------------------------------------------------------- SOCK_RAW, */

/* ALL_STACK is required, not optional: without it the raw hook runs only in the
   unrecognised-protocol branch, so a raw ICMP socket never sees an echo reply.
   FILTER changes the NX_IP layout and must be seen by every translation unit. */
#define NX_ENABLE_IP_RAW_PACKET_FILTER
#define NX_ENABLE_IP_RAW_PACKET_ALL_STACK


/* ---------------------------------------------------------- capture ------ */

/* The only tap loopback traffic can be seen through; the SANA-II taps cannot
   see it.  Changes the NX_IP layout, so every translation unit must see it. */
#define NX_ENABLE_IP_PACKET_FILTER


/* ------------------------------------------------------------------- IP --- */

#ifdef AMINETXDUO_IP_ID_RANDOMIZATION
#define NX_ENABLE_IP_ID_RANDOMIZATION
#endif


/* ------------------------------------------------------------- routing --- */

/* NX_IP_ROUTING_TABLE_SIZE below is inert without this, and the add/delete
   entry points are stubs returning NX_NOT_SUPPORTED.  Changes the NX_IP
   layout, so every translation unit must see it. */
#define NX_ENABLE_IP_STATIC_ROUTING

#define NX_IP_ROUTING_TABLE_SIZE                4


/* -------------------------------------------------------------- resolver, */

/* Inert until nx_dns_cache_initialize() is called; the cache buffer belongs to
   the caller (src/netstack/netstack_dns.c). */
#define NX_DNS_CACHE_ENABLE

/* wait_option is a PER-QUERY timeout spent this many times over every
   configured server with the DNS mutex held.  The retransmission ladder lives
   in src/netstack/netstack_retry.c instead, where the break signal is sampled. */
#define NX_DNS_MAX_RETRIES                      1


/* ------------------------------------------------------------------ DHCP, */

/* Without this nx_dhcp_create() enrolls interface 0 before the application says
   which interfaces use DHCP.  netstack.c must therefore enable every DHCP
   interface explicitly; the public disable API also clears IP parameters. */
#define NX_DHCP_CLIENT_DISABLE_DEFAULT_INTERFACE

#define NX_DHCP_CLIENT_SEND_ARP_PROBE


/* ---------------------------------------------------------------- IPv6 ---- */

/* The root CMakeLists keeps the nx_icmpv6/nx_ipv6/nx_nd objects out of the
   floor build; disabling it here too keeps the dual-stack paths out of the
   IPv4 objects. */
#ifndef AMINETXDUO_IPV6
#define NX_DISABLE_IPV6
#else

/* Every table below is a fixed array inside the single NX_IP. */

#define NX_IPV6_NEIGHBOR_CACHE_SIZE             8

#define NX_IPV6_DESTINATION_TABLE_SIZE          16

#define NX_IPV6_DEFAULT_ROUTER_TABLE_SIZE       2

#define NX_IPV6_PREFIX_LIST_TABLE_SIZE          4

/* Nothing waits for DAD: an address is TENTATIVE while the solicitations go
   out and the answer arrives through the change notify below, not on the
   thread that configured it.  1 is RFC 4862's own DupAddrDetectTransmits. */
#ifndef NX_IPV6_DAD_TRANSMITS
#define NX_IPV6_DAD_TRANSMITS                   1
#endif

#define NX_ENABLE_IPV6_ADDRESS_CHANGE_NOTIFY

/* Without this SLAAC is unconditional and the enable/disable entry points are
   stubs, so CONFIGURE6=LINKLOCAL and CONFIGURE6=STATIC cannot be honoured. */
#define NX_IPV6_STATELESS_AUTOCONFIG_CONTROL

/* Without it the receive path drops every non-solicited-node IPv6 multicast
   datagram outright.  Neighbour discovery does not go through this table. */
#ifdef AMINETXDUO_MULTICAST
#define NX_ENABLE_IPV6_MULTICAST
#endif

/* Host side only.  Rides on AMINETXDUO_IPV6 and not on AMINETXDUO_MULTICAST:
   the joins that must be reported are solicited-node, made inside NetX Duo.
   src/netstack/netstack.c must call nx_mld_enable() for any of it to run. */
#ifdef AMINETXDUO_IPV6
#define NX_ENABLE_MLD
#endif

/* Only safe with the RFC 8201 4 checks in nx_icmpv6_process_packet_too_big.c,
   the minimum-MTU floor above all: without it one forged Packet Too Big pins a
   destination at MTU 68 for the life of the entry.  Not for a tree without it. */
#define NX_ENABLE_IPV6_PATH_MTU_DISCOVERY

#define NX_ENABLE_IPV6_RDNSS

#define NX_ENABLE_IPV6_DNSSL

/* -------------------------------------------------------------- DHCPv6, */

/* nxd_dhcpv6_client.h defaults this to 2, which is AMI_IP_THREAD_PRIORITY: the
   client thread would preempt the thread it waits on.  The literal, not the
   AMI_ name; a _Static_assert in src/netstack/netstack_dhcpv6.c ties the two. */
#define NX_DHCPV6_THREAD_PRIORITY               4

#define NX_DHCPV6_MAX_IA_ADDRESS                1

/* Matches AMI_RDNSS_MAX; both feed one list of AMI_CFG_MAX_NAMESERVERS. */
#define NX_DHCPV6_NUM_DNS_SERVERS               4

#define NX_DHCPV6_DOMAIN_NAME_BUFFER_SIZE       256

#endif /* AMINETXDUO_IPV6 */


/* ------------------------------------------------------------- loopback, */

/* The checksum-offload switch.  No SANA-II device offers offload; loopback
   claims every capability bit unconditionally under this define.  Changes the
   NX_INTERFACE and NX_PACKET layouts, so every translation unit must see it. */
#define NX_ENABLE_INTERFACE_CAPABILITY

/* Ours, not upstream's: third_party/netxduo branch
   amiga-ipv4-broadcast-loopback, where it is off by default. */
#define NX_ENABLE_IP_BROADCAST_LOOPBACK


/* nxd_mdns.h computes this as 10 * NX_IP_PERIODIC_RATE / 1000, which is 0 at
   50 Hz, and _nx_mdns_timer_set() does nothing when handed 0: every query for
   a unique record would be answered never. */
#define NX_MDNS_RESPONSE_UNIQUE_DELAY           1

#endif /* NX_USER_H */

/* AmiNetXDuo, asking the running stack what it is doing.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_NETSTATUS_H
#define AMINETXDUO_NETSTATUS_H

#include <exec/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ LVOs.
   The -0x360 slot before these is unconditional even in a non-TLS build, so
   both are at the same offset in every configuration. */
#define AMI_NETSTATUS_QUERY_LVO     (-0x366)
#define AMI_NETSTATUS_CONTROL_LVO   (-0x36c)

#define AMI_NETSTATUS_MAGIC         0x414E5351UL    /* 'ANSQ' */
/* Bump on any change to a record or control-block shape: the version checks in
   src/bsdsocket/netstatus.c are exact equality in both directions, so two
   different shapes under one version number cannot be told apart. */
#define AMI_NETSTATUS_VERSION       12

/* Fixed widths every record shares. */
#define NETSTATUS_NAME_LEN      32
#define NETSTATUS_DEVICE_LEN    32
#define NETSTATUS_MAC_SIZE      6
/* A host name, RFC 1123 2.1's 63-character label and a NUL, which is also what
   AMI_CFG_NAME_LEN (aminetxduo/config.h) sizes cfg->hostname at. */
#define NETSTATUS_HOSTNAME_LEN  64

/* The DNS-SD widths.  NETSTATUS_SVC_TXT_LEN is shorter than a TXT record may
   legally be; a longer one is truncated, not dropped (NETSTATUS_SVC_TXTCUT). */
#define NETSTATUS_SVC_NAME_LEN  64
#define NETSTATUS_SVC_TYPE_LEN  24
#define NETSTATUS_SVC_HOST_LEN  64
#define NETSTATUS_SVC_TXT_LEN   192

/* Callers MUST check lib_Revision >= this before any netstatus call: an older
   library has no such vector and the jump lands past the table terminator.
   Bump when a netstatus vector is added or AMI_NETSTATUS_VERSION moves. */
#define AMI_NETSTATUS_MIN_REVISION  8

/* ------------------------------------------------------------ selectors --- */
#define NETSTATUS_SYSTEM        1   /* one NetStatusSystem                   */
#define NETSTATUS_INTERFACES    2   /* NetStatusInterface[]                  */
#define NETSTATUS_STATS         3   /* one NetStatusStats                    */
#define NETSTATUS_ARP           4   /* NetStatusArp[]                        */
#define NETSTATUS_ROUTES        5   /* NetStatusRoute[]                      */
#define NETSTATUS_SOCKETS       6   /* NetStatusSocket[]                     */
#define NETSTATUS_DHCP          7   /* NetStatusDhcp[]                       */
#define NETSTATUS_ADDRESSES6    8   /* NetStatusAddress6[]                   */
#define NETSTATUS_ROUTES6       9   /* NetStatusRoute6[]                     */
#define NETSTATUS_NEIGHBOURS   10   /* NetStatusNeighbour[]                  */
#define NETSTATUS_HEALTH       11   /* one NetStatusHealth                   */
#define NETSTATUS_SERVICES     12   /* NetStatusService[]                    */
#define NETSTATUS_OPENERS      13   /* NetStatusOpener[]                     */
#define NETSTATUS_TCPSTALL     14   /* NetStatusTcpStall[]                   */
#define NETSTATUS_DEST6        15   /* NetStatusDest6[]                      */
#define NETSTATUS_EVENTS       16   /* NetStatusEvent[]                      */
#define NETSTATUS_RXBUDGET     17   /* one NetStatusRxBudget                 */
#define NETSTATUS_DHCP6        18   /* NetStatusDhcp6[]                      */

/* Every buffer starts with this.  Truncation is detectable rather than silent:
   nsh_Count < nsh_Available. */
typedef struct NetStatusHeader
{
    ULONG   nsh_Magic;          /* in:  AMI_NETSTATUS_MAGIC                  */
    UWORD   nsh_Version;        /* in:  the caller's AMI_NETSTATUS_VERSION   */
    UWORD   nsh_Type;           /* out: the selector this answers            */
    UWORD   nsh_EntrySize;      /* out: sizeof one entry, the library's own  */
    UWORD   nsh_Count;          /* out: entries written                      */
    UWORD   nsh_Available;      /* out: entries the library had              */
    UWORD   nsh_Reserved;
} NetStatusHeader;

/* Where the entries start, for a caller that would rather not do the sum. */
#define NETSTATUS_ENTRIES(hdr)  ((APTR)((UBYTE *)(hdr) + sizeof(NetStatusHeader)))

/* --------------------------------------------------- NETSTATUS_SYSTEM --- */

/* nss_Flags */
#define NETSTATUS_SYS_UP        0x0001UL /* there is a live NX_IP            */
#define NETSTATUS_SYS_GATEWAY   0x0002UL /* nss_Gateway is meaningful        */
#define NETSTATUS_SYS_IPV6      0x0004UL /* built with AMINETXDUO_IPV6       */
/* NX_ENABLE_IP_STATIC_ROUTING.  Clear means the static routing table is not in
   this build and NETCTRL_ROUTE_ADD/DELETE answer ENOSYS. */
#define NETSTATUS_SYS_ROUTING   0x0008UL

/* AMINETXDUO_MDNS.  Set with an empty nss_MdnsName means the responder is
   there but has not claimed a name yet. */
#define NETSTATUS_SYS_MDNS      0x0010UL

typedef struct NetStatusSystem
{
    ULONG   nss_Flags;
    ULONG   nss_Gateway;                /* IPv4 default gateway, host order  */
    ULONG   nss_InterfaceCount;         /* NX_IP interface slots             */
    ULONG   nss_PoolTotal;
    ULONG   nss_PoolFree;
    ULONG   nss_PoolPayload;
    ULONG   nss_PoolEmptyRequests;
    ULONG   nss_PoolEmptySuspensions;
    ULONG   nss_PoolInvalidReleases;
    /* What this machine calls itself on the local network, with the ".local".
       Empty unless NETSTATUS_SYS_MDNS. */
    char    nss_MdnsName[NETSTATUS_NAME_LEN];
    /* AmiHostnameSource (aminetxduo/config.h).  Zero is AMI_HOSTNAME_NONE and
       is also what a library predating the field answers. */
    ULONG   nss_HostSource;
    /* Programs holding the library open, and the library's own open count.
       They differ by the reference NETCTRL_STACK_HOLD took. */
    ULONG   nss_Openers;
    ULONG   nss_OpenCnt;
} NetStatusSystem;

/* ----------------------------------------------- NETSTATUS_INTERFACES --- */

/* nsi_Flags */
#define NETSTATUS_IF_ATTACHED   0x0001  /* the NX_INTERFACE slot is valid    */
#define NETSTATUS_IF_LINKUP     0x0002  /* NetX Duo believes the link is up  */
#define NETSTATUS_IF_SANA2      0x0004  /* a SANA-II device is attached      */
#define NETSTATUS_IF_ONLINE     0x0008  /* and that device reports online    */
#define NETSTATUS_IF_NAMED      0x0010  /* nsi_Name came from the config     */
#define NETSTATUS_IF_MDNS       0x0020  /* answering .local here             */

typedef struct NetStatusInterface
{
    UWORD   nsi_Index;                  /* NX_IP interface index             */
    UWORD   nsi_Flags;
    ULONG   nsi_Address;                /* host byte order                   */
    ULONG   nsi_NetMask;
    ULONG   nsi_MTU;
    ULONG   nsi_Speed;                  /* bits/s; 0 when the driver has none */
    UBYTE   nsi_HwAddress[NETSTATUS_MAC_SIZE];
    UBYTE   nsi_Pad[2];
    char    nsi_Name[NETSTATUS_NAME_LEN];       /* "eth0", NUL-terminated    */
    char    nsi_Device[NETSTATUS_DEVICE_LEN];   /* "a2065.device"            */
    ULONG   nsi_Unit;

    /* From the SANA-II shim, all zero unless NETSTATUS_IF_SANA2 is set. */
    ULONG   nsi_PacketsIn;
    ULONG   nsi_PacketsOut;
    ULONG   nsi_BadData;
    ULONG   nsi_Overruns;
    ULONG   nsi_UnknownTypes;
    ULONG   nsi_Reconfigurations;
    ULONG   nsi_TxErrors;
    ULONG   nsi_RxErrors;
    ULONG   nsi_AllocFailures;
    /* The four causes behind nsi_RxErrors, which sum to it. */
    ULONG   nsi_RxErrRunt;
    ULONG   nsi_RxErrVerify;
    ULONG   nsi_RxErrLength;
    ULONG   nsi_RxErrIo;
    /* Frames filled by the copy/direct hooks, and those summed in the fill. */
    ULONG   nsi_RxCopyHook;
    ULONG   nsi_RxCopySummed;
    /* Of nsi_RxCopyHook, fills that came through the private direct-receive
       pair.  The summed counter cannot answer this. */
    ULONG   nsi_RxDirectFill;
    /* anxnet.device recovery counters; zero for other SANA-II drivers. */
    ULONG   nsi_TickPolls;
    ULONG   nsi_RxKicks;
} NetStatusInterface;

/* ----------------------------------------------- NETSTATUS_ADDRESSES6 --- */

/* One entry per IPv6 address per interface, in NetX Duo's own order.  An
   IPv4-only build answers with no entries rather than an error. */

/* nsn_State, NX_IPV6_ADDR_STATE_*.  A TENTATIVE address is still running
   duplicate address detection and must not be used as a source. */
#define NETSTATUS_IP6_TENTATIVE     1
#define NETSTATUS_IP6_PREFERRED     2
#define NETSTATUS_IP6_DEPRECATED    3
#define NETSTATUS_IP6_VALID         4

/* nsn_Origin, how the address was obtained. */
#define NETSTATUS_IP6_ORIGIN_NONE      0
#define NETSTATUS_IP6_ORIGIN_MANUAL    1
#define NETSTATUS_IP6_ORIGIN_SLAAC     2
#define NETSTATUS_IP6_ORIGIN_DHCPV6    3

typedef struct NetStatusAddress6
{
    UWORD   nsn_Interface;              /* NX_IP interface index             */
    UWORD   nsn_State;                  /* NETSTATUS_IP6_*                   */
    ULONG   nsn_Address[4];             /* host byte order, four words       */
    ULONG   nsn_PrefixLength;
    ULONG   nsn_Origin;                 /* NETSTATUS_IP6_ORIGIN_*            */
} NetStatusAddress6;

/* ----------------------------------------------------- NETSTATUS_DHCP --- */

/* What the DHCP server said, per interface.  The offered lists are reported
   whether or not they were used. */

/* nsd_State */
#define NETSTATUS_DHCP_OFF      0   /* not using DHCP on this interface     */
#define NETSTATUS_DHCP_WORKING  1   /* asking: discovering, requesting, ARP */
#define NETSTATUS_DHCP_BOUND    2   /* it has a lease; the rest is filled   */

#define NETSTATUS_DHCP_ADDRS    8   /* of each list; the option may hold more */

/* nsd_LeaseSeconds when the server said the lease never expires. */
#define NETSTATUS_DHCP_FOREVER  0xFFFFFFFFUL

/* nsd_RawState, NX_DHCP_STATE_* verbatim.  nsd_State collapses BOUND,
   RENEWING and REBINDING into one, so watching a renewal needs this.  Zero is
   NOT_STARTED and is also what a library predating the field answers. */
#define NETSTATUS_DHCPRAW_NOT_STARTED   0
#define NETSTATUS_DHCPRAW_BOOT          1
#define NETSTATUS_DHCPRAW_INIT          2
#define NETSTATUS_DHCPRAW_SELECTING     3
#define NETSTATUS_DHCPRAW_REQUESTING    4
#define NETSTATUS_DHCPRAW_BOUND         5
#define NETSTATUS_DHCPRAW_RENEWING      6
#define NETSTATUS_DHCPRAW_REBINDING     7
#define NETSTATUS_DHCPRAW_FORCERENEW    8
#define NETSTATUS_DHCPRAW_PROBING       9

typedef struct NetStatusDhcp
{
    UWORD   nsd_Index;                  /* NX_IP interface index             */
    UWORD   nsd_State;                  /* NETSTATUS_DHCP_*                  */

    /* All below are meaningful only in NETSTATUS_DHCP_BOUND. */
    ULONG   nsd_Address;                /* host byte order                   */
    ULONG   nsd_NetMask;
    ULONG   nsd_Server;                 /* who answered; 0 if it did not say */
    ULONG   nsd_LeaseSeconds;           /* 0 = not stated                    */

    ULONG   nsd_Router[NETSTATUS_DHCP_ADDRS];
    UWORD   nsd_RouterCount;
    ULONG   nsd_Dns[NETSTATUS_DHCP_ADDRS];
    UWORD   nsd_DnsCount;
    ULONG   nsd_StaticRoute[NETSTATUS_DHCP_ADDRS];
    UWORD   nsd_StaticRouteCount;
    UWORD   nsd_RawState;               /* NETSTATUS_DHCPRAW_*               */

    char    nsd_HostName[NETSTATUS_NAME_LEN];
    char    nsd_DomainName[NETSTATUS_NAME_LEN];
} NetStatusDhcp;

/* ---------------------------------------------------- NETSTATUS_DHCP6 --- */

/* One row per interface, the same as NETSTATUS_DHCP, and nsd6_State reuses
   the NETSTATUS_DHCP_* triple.  This stack runs ONE DHCPv6 client, so at most
   one row is ever anything but NETSTATUS_DHCP_OFF. */

/* nsd6_RawState, NX_DHCPV6_STATE_* verbatim.  Zero is what a library
   predating the selector, or one built without IPv6, answers. */
#define NETSTATUS_DHCP6RAW_NONE         0
#define NETSTATUS_DHCP6RAW_INIT         1
#define NETSTATUS_DHCP6RAW_SOLICIT      2
#define NETSTATUS_DHCP6RAW_REQUEST      3
#define NETSTATUS_DHCP6RAW_RENEW        4
#define NETSTATUS_DHCP6RAW_REBIND       5
#define NETSTATUS_DHCP6RAW_DECLINE      6
#define NETSTATUS_DHCP6RAW_CONFIRM      7
#define NETSTATUS_DHCP6RAW_INFORM       8
#define NETSTATUS_DHCP6RAW_RELEASE      9
#define NETSTATUS_DHCP6RAW_BOUND       15

typedef struct NetStatusDhcp6
{
    UWORD   nsd6_Index;                 /* NX_IP interface index             */
    UWORD   nsd6_State;                 /* NETSTATUS_DHCP_*                  */
    UWORD   nsd6_RawState;              /* NETSTATUS_DHCP6RAW_*              */

    /* 0 when the client only ever sent an Information-Request: it has
       options but no address, so there is no lease to release. */
    UWORD   nsd6_Stateful;

    /* All below are meaningful only in NETSTATUS_DHCP_BOUND. */
    ULONG   nsd6_Address[4];            /* host byte order, four words       */
    ULONG   nsd6_PreferredSeconds;      /* 0 = not stated                    */
    ULONG   nsd6_ValidSeconds;
    ULONG   nsd6_T1;                    /* renew at, RFC 8415 21.4           */
    ULONG   nsd6_T2;                    /* rebind at                         */
} NetStatusDhcp6;

/* ---------------------------------------------------- NETSTATUS_STATS --- */

/* nsx_Have, a protocol NetX Duo was not built with is not "all zero". */
#define NETSTATUS_HAVE_IP       0x0001UL
#define NETSTATUS_HAVE_ICMP     0x0002UL
#define NETSTATUS_HAVE_TCP      0x0004UL
#define NETSTATUS_HAVE_UDP      0x0008UL
#define NETSTATUS_HAVE_ARP      0x0010UL

typedef struct NetStatusStats
{
    ULONG   nsx_Have;

    ULONG   nsx_IpPacketsSent;
    ULONG   nsx_IpBytesSent;
    ULONG   nsx_IpPacketsReceived;
    ULONG   nsx_IpBytesReceived;
    ULONG   nsx_IpInvalid;
    ULONG   nsx_IpReceiveDropped;
    ULONG   nsx_IpChecksumErrors;
    ULONG   nsx_IpSendDropped;
    ULONG   nsx_IpFragmentsSent;
    ULONG   nsx_IpFragmentsReceived;

    ULONG   nsx_IcmpPingsSent;
    ULONG   nsx_IcmpPingTimeouts;
    ULONG   nsx_IcmpThreadsSuspended;
    ULONG   nsx_IcmpResponses;
    ULONG   nsx_IcmpChecksumErrors;
    ULONG   nsx_IcmpUnhandled;

    ULONG   nsx_TcpPacketsSent;
    ULONG   nsx_TcpBytesSent;
    ULONG   nsx_TcpPacketsReceived;
    ULONG   nsx_TcpBytesReceived;
    ULONG   nsx_TcpInvalid;
    ULONG   nsx_TcpReceiveDropped;
    ULONG   nsx_TcpChecksumErrors;
    ULONG   nsx_TcpConnections;
    ULONG   nsx_TcpDisconnections;
    ULONG   nsx_TcpConnectionsDropped;
    ULONG   nsx_TcpRetransmits;

    ULONG   nsx_UdpPacketsSent;
    ULONG   nsx_UdpBytesSent;
    ULONG   nsx_UdpPacketsReceived;
    ULONG   nsx_UdpBytesReceived;
    ULONG   nsx_UdpInvalid;
    ULONG   nsx_UdpReceiveDropped;
    ULONG   nsx_UdpChecksumErrors;

    ULONG   nsx_ArpRequestsSent;
    ULONG   nsx_ArpRequestsReceived;
    ULONG   nsx_ArpResponsesSent;
    ULONG   nsx_ArpResponsesReceived;
    ULONG   nsx_ArpDynamicEntries;
    ULONG   nsx_ArpStaticEntries;
    ULONG   nsx_ArpAgedEntries;
    ULONG   nsx_ArpInvalidMessages;
} NetStatusStats;

/* --------------------------------------------------- NETSTATUS_HEALTH --- */

/* Answered with the stack up or down.  nsl_PoolFree and nsl_PoolLow are
   sampled, not exact.  nsl_BatonStateShared must stay zero.  nsl_TickSkewPeak
   has a floor of one wakeup's worth of ticks: a healthy PAL machine reads 2. */
typedef struct NetStatusHealth
{
    ULONG   nsl_TickTicks;
    /* Wakeups that delivered more than one tick.  Counted unconditionally in
       tx_initialize_low_level.c and, before this, printed only by a serial
       dump no shipped build compiles. */
    ULONG   nsl_TickCatchups;
    ULONG   nsl_TickClipped;
    ULONG   nsl_TickLost;
    ULONG   nsl_TickServiceUs;
    ULONG   nsl_TickUptimeMs;
    ULONG   nsl_TickWorstStallMs;
    ULONG   nsl_TickWorstServiceUs;
    ULONG   nsl_TickOverBudget;
    ULONG   nsl_TickDeferred;
    ULONG   nsl_TickSkew;
    ULONG   nsl_TickSkewPeak;

    ULONG   nsl_BatonLive;
    ULONG   nsl_BatonLiveMax;
    ULONG   nsl_BatonFull;
    ULONG   nsl_BatonTransitions;
    ULONG   nsl_BatonStateMax;
    ULONG   nsl_BatonMoved;
    ULONG   nsl_BatonStateShared;

    ULONG   nsl_AllocLive;          /* ami_alloc() blocks not yet freed      */
    ULONG   nsl_AllocPeak;
    ULONG   nsl_AllocRefused;       /* allocations that came back NULL       */
    ULONG   nsl_Sockets;            /* AmiSocket structures alive            */
    ULONG   nsl_SocketsPeak;
    ULONG   nsl_Opens;              /* programs holding the library open     */
    ULONG   nsl_PoolTotal;          /* packets in the pool; 0 = no pool      */
    ULONG   nsl_PoolFree;
    ULONG   nsl_PoolLow;            /* fewest ever seen free                 */
    ULONG   nsl_PoolPayload;        /* bytes per packet                      */
    ULONG   nsl_PoolEmpty;          /* requests that found the pool empty    */
    ULONG   nsl_PoolWaited;         /* ... and suspended waiting             */
    ULONG   nsl_PoolBadRelease;
} NetStatusHealth;

/* ------------------------------------------------------ NETSTATUS_ARP --- */

/* nsa_Flags */
#define NETSTATUS_ARP_STATIC    0x0001
#define NETSTATUS_ARP_RESOLVED  0x0002  /* a hardware address has been learnt */

typedef struct NetStatusArp
{
    ULONG   nsa_Address;                /* host byte order                   */
    UBYTE   nsa_HwAddress[NETSTATUS_MAC_SIZE];
    UWORD   nsa_Flags;
    UWORD   nsa_Retries;                /* requests sent while unresolved    */
    UWORD   nsa_Interface;              /* the interface it was learnt on    */
} NetStatusArp;

/* ----------------------------------------------- NETSTATUS_NEIGHBOURS --- */

/* IPv6's answer to the ARP cache, from nx_ipv6_nd_cache[].  An IPv4-only
   build answers with no entries rather than an error. */

/* nsn6_State, ND_CACHE_STATE_*, spelled out so a caller need not include
   nx_nd_cache.h.  INVALID entries are not reported at all. */
#define NETSTATUS_ND_INCOMPLETE 1   /* asked, nothing back yet              */
#define NETSTATUS_ND_REACHABLE  2   /* answered within the reachable time   */
#define NETSTATUS_ND_STALE      3   /* answered once, not checked since     */
#define NETSTATUS_ND_DELAY      4   /* something was sent; waiting to probe */
#define NETSTATUS_ND_PROBE      5   /* being re-checked now                 */
#define NETSTATUS_ND_CREATED    6   /* the slot exists, nothing asked yet   */

/* nsn6_Flags */
#define NETSTATUS_ND_STATIC     0x0001  /* configured, never times out       */
#define NETSTATUS_ND_ROUTER     0x0002  /* it is a router for this machine   */

typedef struct NetStatusNeighbour
{
    ULONG   nsn6_Address[4];            /* host byte order, four words       */
    UBYTE   nsn6_HwAddress[NETSTATUS_MAC_SIZE];
    UWORD   nsn6_State;                 /* NETSTATUS_ND_*                    */
    UWORD   nsn6_Flags;
    UWORD   nsn6_Interface;             /* the interface it was learnt on    */
    UWORD   nsn6_Solicitations;         /* sent while unresolved             */
    UWORD   nsn6_Queued;                /* packets held for the answer       */
} NetStatusNeighbour;

/* --------------------------------------------------- NETSTATUS_ROUTES --- */

/* nsr_Flags, the BSD spelling, because that is what netstat -r prints. */
#define NETSTATUS_RT_UP         0x0001
#define NETSTATUS_RT_GATEWAY    0x0002  /* nsr_Gateway is a next hop         */
#define NETSTATUS_RT_HOST       0x0004  /* a /32                             */
#define NETSTATUS_RT_STATIC     0x0008  /* added by hand, not derived        */

typedef struct NetStatusRoute
{
    ULONG   nsr_Destination;            /* host byte order                   */
    ULONG   nsr_NetMask;
    ULONG   nsr_Gateway;                /* 0 = directly attached             */
    UWORD   nsr_Flags;
    UWORD   nsr_Interface;
} NetStatusRoute;

/* -------------------------------------------------- NETSTATUS_ROUTES6 --- */

/* The two lists NetX Duo routes IPv6 from -- on-link prefixes first, then
   default routers -- and not a destination table.  fe80::/64 is never an
   entry, so there is none to report and none to remove.  An IPv4-only build
   answers with no entries. */

/* nsr6_Flags, the same bits and the same letters as nsr_Flags. */
#define NETSTATUS_RT6_UP        0x0001
#define NETSTATUS_RT6_GATEWAY   0x0002  /* nsr6_NextHop is a next hop        */
#define NETSTATUS_RT6_HOST      0x0004  /* a /128                            */
#define NETSTATUS_RT6_STATIC    0x0008  /* added by hand, not advertised     */

/* nsr6_Lifetime for an entry nothing will time out. */
#define NETSTATUS_RT6_FOREVER   0xFFFFFFFFUL

typedef struct NetStatusRoute6
{
    ULONG   nsr6_Destination[4];        /* host byte order, four words       */
    ULONG   nsr6_PrefixLength;
    ULONG   nsr6_NextHop[4];            /* all zero = on link                */
    ULONG   nsr6_Lifetime;              /* seconds left                      */
    UWORD   nsr6_Flags;
    UWORD   nsr6_Interface;             /* NX_IP interface index             */
} NetStatusRoute6;

/* ---------------------------------------------------- NETSTATUS_DEST6 --- */

/* The destination cache, nx_ipv6_destination_table[].  nsd6_Age counts table
   uses since the entry was last chosen, not seconds; nsd6_Capacity is the same
   number in every row.  An IPv4-only build answers with no entries. */

/* nsd6_Flags */
#define NETSTATUS_DEST6_ONLINK  0x0001  /* next hop is the destination itself */
#define NETSTATUS_DEST6_ROUTER  0x0002  /* the next hop is a router           */

/* nsd6_PathMtu when the build has no path MTU discovery. */
#define NETSTATUS_DEST6_NO_MTU  0UL

typedef struct NetStatusDest6
{
    ULONG   nsd6_Destination[4];        /* host byte order, four words       */
    ULONG   nsd6_NextHop[4];            /* host byte order; == destination
                                           when the destination is on link   */
    ULONG   nsd6_Age;                   /* table uses since last chosen      */
    ULONG   nsd6_PathMtu;               /* 0 = not discovered / not built in */
    ULONG   nsd6_Capacity;              /* slots in the table, every row     */
    UWORD   nsd6_NdState;               /* NETSTATUS_ND_* of the next hop's
                                           cache entry; 0 = no entry linked  */
    UWORD   nsd6_Flags;
    UWORD   nsd6_Interface;             /* NX_IP interface index             */
    UWORD   nsd6_Pad;
} NetStatusDest6;

/* -------------------------------------------------- NETSTATUS_SOCKETS --- */

/* nso_Flags */
#define NETSTATUS_SOCK_TCP      0x0001  /* clear means UDP                   */

typedef struct NetStatusSocket
{
    UWORD   nso_Flags;
    UWORD   nso_LocalPort;
    UWORD   nso_PeerPort;
    UWORD   nso_State;                  /* TCP only; NX_TCP_* state number   */
    ULONG   nso_PeerAddress;            /* TCP only, host byte order         */
    ULONG   nso_Queued;                 /* UDP only: datagrams waiting       */
} NetStatusSocket;

/* NetX Duo's own NX_TCP_* values, spelled out so a caller need not include
   nx_api.h; _Static_asserts in src/bsdsocket/netstatus.c hold them to it. */
#define NETSTATUS_TCP_CLOSED        1
#define NETSTATUS_TCP_LISTEN        2
#define NETSTATUS_TCP_SYN_SENT      3
#define NETSTATUS_TCP_SYN_RECEIVED  4
#define NETSTATUS_TCP_ESTABLISHED   5
#define NETSTATUS_TCP_CLOSE_WAIT    6
#define NETSTATUS_TCP_FIN_WAIT_1    7
#define NETSTATUS_TCP_FIN_WAIT_2    8
#define NETSTATUS_TCP_CLOSING       9
#define NETSTATUS_TCP_TIMED_WAIT    10
#define NETSTATUS_TCP_LAST_ACK      11

/* ------------------------------------------------- NETSTATUS_TCPSTALL ---
   A separate table rather than more fields on NetStatusSocket: every consumer
   checks nsh_EntrySize for exact equality, so growing that record is an ABI
   break.  The identifying tuple is repeated so the two tables can be joined
   without trusting the order. */
typedef struct NetStatusTcpStall
{
    UWORD   nst_LocalPort;
    UWORD   nst_PeerPort;
    ULONG   nst_PeerAddress;            /* host byte order                   */
    ULONG   nst_Stalled;                /* ms since the peer last ACKed      */
    ULONG   nst_Retransmits;            /* consecutive, 0 after any progress */
    ULONG   nst_Rto;                    /* ms left on the retransmit timer   */
    ULONG   nst_UserTimeout;            /* ms, TCP_USER_TIMEOUT; 0 = unset   */
} NetStatusTcpStall;

/* ------------------------------------------------- NETSTATUS_SERVICES ---
   A read of the whole mDNS cache, of every type: match on nsv_Type rather than
   expecting the type last browsed for.  Resolving SRV targets that arrived
   without an address costs up to two seconds.  Empty on a build without
   AMINETXDUO_MDNS. */

/* nsv_Flags */
/* Clear means a service-type row from the meta-query: nsv_Name, nsv_Host,
   nsv_Address, nsv_Port and nsv_Text are then empty. */
#define NETSTATUS_SVC_INSTANCE  0x0001
#define NETSTATUS_SVC_ADDRESS   0x0002  /* nsv_Address is an answer, not zero */
#define NETSTATUS_SVC_TXT       0x0004  /* a TXT record was seen              */
#define NETSTATUS_SVC_TXTCUT    0x0008  /* and it did not fit nsv_Text        */
/* This machine's own advertisement, read back out of the local cache. */
#define NETSTATUS_SVC_LOCAL     0x0010

typedef struct NetStatusService
{
    UWORD   nsv_Flags;
    UWORD   nsv_Index;                  /* interface it was heard on         */
    UWORD   nsv_Port;                   /* SRV port, host order              */
    UWORD   nsv_Pad;
    ULONG   nsv_Address;                /* IPv4, host order                  */
    char    nsv_Name[NETSTATUS_SVC_NAME_LEN];   /* instance, RFC 6763 4.1.1  */
    char    nsv_Type[NETSTATUS_SVC_TYPE_LEN];   /* "_http._tcp"              */
    char    nsv_Host[NETSTATUS_SVC_HOST_LEN];   /* SRV target, with .local   */
    char    nsv_Text[NETSTATUS_SVC_TXT_LEN];    /* "key=value;key=value"     */
} NetStatusService;

/* --------------------------------------------------- NETSTATUS_OPENERS ---
   One row per OpenLibrary() of bsdsocket.library that has not been given back.
   The reference NETCTRL_STACK_HOLD takes belongs to no program and is not a
   row here; nss_Openers is the row count and nss_OpenCnt the library's own. */

/* nso_Flags */
#define NETSTATUS_OPENER_SELF   0x0001  /* the base this query came through  */
/* Opened the library and exited without closing it: nothing will ever be
   delivered to it, and nso_Task is what it used to be. */
#define NETSTATUS_OPENER_GONE   0x0002

typedef struct NetStatusOpener
{
    UWORD   nso_Flags;
    UWORD   nso_Sockets;                /* how many it has open              */
    ULONG   nso_Task;                   /* struct Task, a number to print    */
    ULONG   nso_BreakMask;              /* SBTC_BREAKMASK, what to signal it */
    char    nso_Name[NETSTATUS_NAME_LEN];   /* the task's, empty if unnamed  */
} NetStatusOpener;

/* ---------------------------------------------------- NETSTATUS_EVENTS ---
   Numeric codes only; the sentence for each lives in src/tools/tool_events.c.
   The codes are a wire format between two binaries: never reused, never
   renumbered.  A first entry whose nse_Seq > 1 means older ones were lost. */

/* nse_Index for an event about the machine rather than one interface. */
#define NETEVENT_NOINDEX        0xffffu

/* --- the stack ---------------------------------------------------------- */
#define NETEVENT_BRINGUP         1  /* came up; value = interfaces opened    */
#define NETEVENT_SHUTDOWN        2  /* teardown began; value = interfaces it
                                       still held                            */
#define NETEVENT_NOTIFY          3  /* value = programs signalled            */
#define NETEVENT_RELEASE         4  /* value = openers left                  */

/* --- bring-up ----------------------------------------------------------- */
/* nse_Index on the next two is the configuration slot rather than an interface
   index: a device that never opens never becomes an interface. */
#define NETEVENT_DEVICE_OPEN    10  /* OpenDevice() refused; value = its
                                       error code                            */
#define NETEVENT_DEVICE_REFUSED 11  /* it opened and then refused a SANA-II
                                       command; value = the AMI_NET_ERR_*    */
#define NETEVENT_ATTACH_FAILED  12  /* nx_ip_interface_attach() refused;
                                       value = the NX_ status                */
#define NETEVENT_LINK_DOWN      13  /* attached with the link down           */
#define NETEVENT_ONLINE_FAILED  14  /* the interface would not go online;
                                       value = the NX_ status                */
/* nse_Index is the slot that would have been next, which is also the count
   already up. */
#define NETEVENT_ATTACH_LIMIT   15  /* no NetX Duo slot left; value = how many
                                       interfaces were asked for             */
/* nse_Index is the slot that yielded.  An interface somebody named never
   yields; that case is NETEVENT_ATTACH_LIMIT. */
#define NETEVENT_ATTACH_YIELD   16  /* an unasked-for interface gave up its
                                       slot; value = interfaces described    */

/*
 * The interface is UP and its default route is not.
 *
 * nx_ip_gateway_address_set() refuses a next hop on no interface's network,
 * which is what a mistyped GATEWAY line looks like.  Bringing the interface
 * up anyway is what start-up has always done, and taking it down instead cost
 * a user a working card and told them about the SANA-II device.  A new event
 * id and nothing else: the record shape is unchanged, so AMI_NETSTATUS_VERSION
 * and AMI_NETSTATUS_MIN_REVISION stay where they are, and an older command
 * prints the row as an unnamed code rather than refusing the table.
 */
#define NETEVENT_GATEWAY_REFUSED 17 /* the default route was refused;
                                       value = NetX Duo status               */

/* --- the wire ----------------------------------------------------------- */
/* OUT_OF_SERVICE marks the interface offline, which makes the next
   ami_sana2_offline() a no-op -- and that no-op is OFFLINE_SKIPPED. */
#define NETEVENT_OUT_OF_SERVICE 20  /* a reader saw S2ERR_OUTOFSERVICE and
                                       marked the link down                  */
#define NETEVENT_OFFLINE_SKIPPED 21 /* S2_OFFLINE not issued: already offline */
#define NETEVENT_OFFLINE_FAILED 22  /* the device refused S2_OFFLINE;
                                       value = the SANA-II wire error        */

/* --- teardown ----------------------------------------------------------- */
#define NETEVENT_IFACE_RETAINED 30  /* value = NETEVENT_HELD_*               */
#define NETEVENT_STACK_RETAINED 31  /* value = interfaces retained           */

/* NETEVENT_IFACE_RETAINED values, and they combine. */
#define NETEVENT_HELD_RX        0x0001UL
#define NETEVENT_HELD_TX        0x0002UL

/* --- expunge ------------------------------------------------------------ */
#define NETEVENT_EXPUNGE_DECLINED 40 /* value = NETEVENT_EXP_*               */

#define NETEVENT_EXP_OPEN       1   /* somebody still has it open            */
#define NETEVENT_EXP_KERNEL     2   /* ThreadX would not stop                */
#define NETEVENT_EXP_TCP        3   /* the TCP: handler is alive             */
#define NETEVENT_EXP_ADDRALLOC  4   /* an address allocation is running      */
#define NETEVENT_EXP_NETMON     5   /* a monitoring hook is installed        */

typedef struct NetStatusEvent
{
    UWORD   nse_Code;                   /* NETEVENT_*                        */
    UWORD   nse_Index;                  /* interface, or NETEVENT_NOINDEX    */
    ULONG   nse_Value;                  /* the code says what it means       */
    ULONG   nse_Tick;                   /* ms since the stack started; 0 when
                                           there was no clock yet            */
    ULONG   nse_Seq;                    /* 1 for the first ever recorded     */
} NetStatusEvent;

/* -------------------------------------------------- NETSTATUS_RXBUDGET ---
   Filled only by a library built with AMINETXDUO_RXPROBE; any other build
   answers the selector with every count zero. */
#define NETSTATUS_BUDGET_BUCKETS  20

typedef struct NetStatusBudgetLeg
{
    ULONG   nbl_Count;
    ULONG   nbl_Sum;                    /* E-Clock ticks                     */
    ULONG   nbl_Max;
    ULONG   nbl_Hist[NETSTATUS_BUDGET_BUCKETS];  /* [i] holds < 2^i ticks    */
} NetStatusBudgetLeg;

/* The baton holder ring: who held the ThreadX baton longer than
   nrb_HoldThreshold.  Site values mirror AMI_HOLD_SITE_* in
   aminetxduo/budget.h and must stay in step. */
#define NETSTATUS_HOLDSITE_YIELD    1   /* blocked inside ThreadX/NetX       */
#define NETSTATUS_HOLDSITE_SUSPEND  2   /* adopted caller's call returning   */
#define NETSTATUS_HOLDSITE_DISCARD  3   /* adopted thread teardown           */
#define NETSTATUS_HOLDSITE_ORPHAN   4   /* adopted thread teardown           */
#define NETSTATUS_HOLDSITE_BRACKET  5   /* about to Wait() on an IORequest   */
#define NETSTATUS_HOLDSITE_REAP     6   /* scheduler reclaimed from a zombie */

#define NETSTATUS_HOLD_RING   16
#define NETSTATUS_HOLD_NAME   16

typedef struct NetStatusHold
{
    ULONG   nsh_Seq;                    /* running count of slow holds;
                                           0 = empty slot                    */
    ULONG   nsh_Ticks;                  /* E-Clock ticks held                */
    ULONG   nsh_Thread;                 /* the TX_THREAD's address           */
    UWORD   nsh_Site;                   /* NETSTATUS_HOLDSITE_*              */
    UWORD   nsh_State;                  /* tx_thread_state at release        */
    char    nsh_Name[NETSTATUS_HOLD_NAME];  /* thread name, copied at record
                                               time, always NUL-terminated   */
} NetStatusHold;

typedef struct NetStatusRxBudget
{
    ULONG               nrb_EClockRate; /* ticks per second, for conversion  */
    NetStatusBudgetLeg  nrb_Drain;      /* reply dequeued -> handed to IP    */
    NetStatusBudgetLeg  nrb_Baton;      /* bsd_nx_enter, asking -> holding   */
    NetStatusBudgetLeg  nrb_Settle;     /* handed to IP -> receive notify    */
    NetStatusBudgetLeg  nrb_Fetch;      /* receive notify -> recv() dequeue  */
    NetStatusBudgetLeg  nrb_Ack;        /* CMD_WRITE BeginIO -> reply reaped */
    /* The CPU an ACK costs to emit, in three pieces whose sum is the old
       nrb_Push: src/sana2/sana2_tx.c says exactly what each spans. */
    NetStatusBudgetLeg  nrb_Reap;       /* TX completion reap walk           */
    NetStatusBudgetLeg  nrb_Stuff;      /* slot claim + framing, to BeginIO  */
    NetStatusBudgetLeg  nrb_Post;       /* BeginIO enter -> return (the copy
                                           hook and FIFO stuffing run inside) */
    /* The direct-completion fork (AMINETXDUO_RX_DIRECT_COMPLETE): recv()
       requests the IP thread completed into the caller's buffer, against the
       packets the classic blocking dequeue fetched. */
    ULONG               nrb_RxDirect;
    ULONG               nrb_RxFallback;
    /* The holder instrument: every baton hold is measured, holds over
       nrb_HoldThreshold (E-Clock ticks, ~50 ms) are counted, maxed and
       ringed with the holder's identity. */
    ULONG               nrb_HoldTotal;
    ULONG               nrb_HoldSlow;
    ULONG               nrb_HoldMax;    /* E-Clock ticks                     */
    ULONG               nrb_HoldThreshold;
    NetStatusHold       nrb_Hold[NETSTATUS_HOLD_RING];
    /* nrb_Settle dissected: three chained sub-legs whose sum is nrb_Settle.
       Appended here, not beside nrb_Settle, so every earlier offset holds. */
    NetStatusBudgetLeg  nrb_Defer;      /* deliver -> IP thread pickup       */
    NetStatusBudgetLeg  nrb_Demux;      /* pickup -> the segment's socket    */
    NetStatusBudgetLeg  nrb_State;      /* socket entry -> receive notify    */
    /* The green realm's scheduling census (AMINETXDUO_GREEN_REALM builds; all
       zero from a baton build).  Appended at the end, so offsets hold. */
    ULONG               nrb_GreenSwitches;
    ULONG               nrb_GreenExternal;
    ULONG               nrb_GreenIdleWaits;
    ULONG               nrb_GreenWaitFast;
    ULONG               nrb_GreenWaitSlow;
    ULONG               nrb_GreenStray;     /* MUST be zero; a nonzero count
                                               is an unconverted Exec block
                                               inside the realm              */
    /* The request gate: brackets migrated into the realm against brackets
       that fell back to the adopted-baton path.  Appended, offsets hold. */
    ULONG               nrb_GateCalls;
    ULONG               nrb_GateFallback;
    /* Exec signal bits allocated on the realm Task, of its 16 allocatable. */
    ULONG               nrb_RealmSigBits;
    /* Brackets that took an idle realm's baton directly; with nrb_GateCalls
       and nrb_GateFallback these partition the brackets.  Offsets hold. */
    ULONG               nrb_GateFast;
    /* The transmit half of a received segment: its socket entry to the driver
       call carrying the ACK.  With nrb_Reap/Stuff/Post it prices the whole TX
       leg the receive pays for.  Appended, offsets hold. */
    NetStatusBudgetLeg  nrb_Xmit;
} NetStatusRxBudget;

/* ------------------------------------------------------------- control,
   the mutating half, on its own LVO.  Every operation takes the same argument
   block; fields an operation does not name MUST be zero, so giving one a
   meaning later cannot change what an older caller asked for. */
#define NETCTRL_INTERFACE_UP    1   /* nsc_Index                             */
#define NETCTRL_INTERFACE_DOWN  2   /* nsc_Index                             */
#define NETCTRL_GATEWAY_SET     3   /* nsc_Gateway                           */
#define NETCTRL_GATEWAY_CLEAR   4   /*,                                    */
/* No nsc_Index: NetX Duo derives the interface from the next hop, which must
   be on an interface's own subnet or the call fails EINVAL.  The same
   destination and mask replaces rather than duplicates; ENOBUFS past the
   table size. */
#define NETCTRL_ROUTE_ADD       5   /* nsc_Destination/NetMask/Gateway       */
#define NETCTRL_ROUTE_DELETE    6   /* nsc_Destination/NetMask               */
#define NETCTRL_ARP_ADD         7   /* nsc_Destination, nsc_HwAddress, Index */
#define NETCTRL_ARP_DELETE      8   /* nsc_Destination                       */
#define NETCTRL_ARP_FLUSH       9   /*,                                    */

/* nsc_PrefixLength 0 with a next hop is a default router; any prefix length
   with no next hop is an on-link prefix; a prefix length WITH a next hop is
   refused EINVAL.  nsc_Index is required for a link-local next hop and ignored
   for a prefix.  Both invalidate the IPv6 destination cache. */
#define NETCTRL_ROUTE6_ADD     10   /* nsc_Destination6/PrefixLength/Gateway6/Index */
#define NETCTRL_ROUTE6_DELETE  11   /* nsc_Destination6/PrefixLength/Gateway6 */

#define NETCTRL_ND_ADD         12   /* nsc_Destination6, nsc_HwAddress, Index */
#define NETCTRL_ND_DELETE      13   /* nsc_Destination6                      */

/* nsc_Name is a service type; empty is the RFC 6763 9 meta-query.  BROWSE
   registers a continuous query and returns at once.  BROWSE_STOP is not
   optional: an unretired query asks the network forever and occupies the peer
   cache.  ENOSYS on a build without AMINETXDUO_MDNS. */
#define NETCTRL_MDNS_BROWSE      14 /* nsc_Name, empty for the meta-query    */
#define NETCTRL_MDNS_BROWSE_STOP 15 /* nsc_Name, the same one                */

/* Closes the SANA-II device, detaches the interface and releases the slot.
   EBUSY while TCP connections are routed out of it unless NETCTRL_F_FORCE,
   which resets them.  Removing one interface does not renumber the others. */
#define NETCTRL_INTERFACE_REMOVE 16 /* nsc_Index, nsc_Flags                  */

/* nsc_Name is a file in DEVS:NetInterfaces.  This reads a file, so it must be
   called from a Process.  The address is not waited for: poll
   NETSTATUS_INTERFACES' nsi_Address, or give up. */
#define NETCTRL_INTERFACE_ADD   17  /* nsc_Name                              */

/* The library takes a reference of its own, so a command that started the
   network can CloseLibrary() without taking it down.  Idempotent.
   NETCTRL_STACK_RELEASE gives it back; ENETDOWN if there is no stack. */
#define NETCTRL_STACK_HOLD      18  /*,                                    */

/* nsc_Destination, nsc_NetMask and nsc_Gateway are each applied only when
   their NETCTRL_F_ bit is set, because 0.0.0.0 is a legitimate request.  The
   gateway is set last; a call that fails on it keeps the address it applied. */
#define NETCTRL_INTERFACE_CONFIGURE 19 /* nsc_Index/Destination/NetMask/Gateway */

/* All three take nsc_Index; START also reads nsc_Destination under
   NETCTRL_F_ADDRESS, which is a wish rather than a demand.  RENEW and RELEASE
   answer ENOTCONN with no lease.  None of them waits for the exchange. */
#define NETCTRL_DHCP_START      20  /* nsc_Index                             */
#define NETCTRL_DHCP_RENEW      21  /* nsc_Index                             */
#define NETCTRL_DHCP_RELEASE    22  /* nsc_Index                             */

/* Offers nsc_HostName at the rank of ENV:HOSTNAME (AmiHostnameSource), so a
   stronger source holding the name refuses it with EPERM.  EINVAL for an empty
   name, ENETDOWN with no stack.  Nothing here writes a file. */
#define NETCTRL_HOSTNAME_SET    23  /* nsc_HostName                          */

/* NETCTRL_F_MDNS in nsc_Flags is the direction, set on and clear off, with no
   third state meaning leave it unchanged.  Neither direction waits.  ENXIO,
   ENETDOWN, EIO, ENOMEM, and ENOSYS without AMINETXDUO_MDNS. */
#define NETCTRL_INTERFACE_MDNS  24  /* nsc_Index, NETCTRL_F_MDNS             */

/* NOTIFY signals each opener SIGBREAKF_CTRL_C plus its SBTC_BREAKMASK bit and
   does nothing else, skipping the caller's own base and openers whose task has
   exited.  RELEASE gives back the reference NETCTRL_STACK_HOLD took; it is
   idempotent and is refused when the caller is the only opener left. */
#define NETCTRL_STACK_NOTIFY    25  /* out: nsc_Count                        */
#define NETCTRL_STACK_RELEASE   26  /*,                                     */

/* Flags for nsc_Flags. Zero unless an operation above says otherwise. */
#define NETCTRL_F_FORCE         0x00000001
/* Which of NETCTRL_INTERFACE_CONFIGURE's three fields were given at all. */
#define NETCTRL_F_ADDRESS       0x00000002
#define NETCTRL_F_NETMASK       0x00000004
#define NETCTRL_F_GATEWAY       0x00000008
/* NETCTRL_INTERFACE_MDNS: set is on, clear is off. */
#define NETCTRL_F_MDNS          0x00000010

typedef struct NetStatusControl
{
    ULONG   nsc_Magic;                  /* in: AMI_NETSTATUS_MAGIC           */
    UWORD   nsc_Version;                /* in: the caller's version          */
    UWORD   nsc_Index;                  /* interface index                   */
    ULONG   nsc_Destination;            /* host byte order                   */
    ULONG   nsc_NetMask;
    ULONG   nsc_Gateway;
    UBYTE   nsc_HwAddress[NETSTATUS_MAC_SIZE];
    UWORD   nsc_Pad;
    ULONG   nsc_Destination6[4];        /* host byte order, four words       */
    ULONG   nsc_Gateway6[4];            /* all zero = no next hop            */
    ULONG   nsc_PrefixLength;
    char    nsc_Name[NETSTATUS_SVC_TYPE_LEN];
    ULONG   nsc_Flags;                  /* NETCTRL_F_*                       */
    /* out: how many the operation acted on; NETCTRL_STACK_NOTIFY's number of
       programs signalled. */
    ULONG   nsc_Count;
    ULONG   nsc_Reserved[2];
    /* A host name, for NETCTRL_HOSTNAME_SET.  Its own field because nsc_Name
       is 24 bytes, the width of a DNS-SD service type, and a host name is up
       to 63 (RFC 1123 2.1). */
    char    nsc_HostName[NETSTATUS_HOSTNAME_LEN];
} NetStatusControl;

#ifdef __cplusplus
}
#endif

#endif /* AMINETXDUO_NETSTATUS_H */

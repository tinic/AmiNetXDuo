/*
 * AmiNetXDuo -- asking the RUNNING stack what it is doing.
 *
 * WHY THIS EXISTS
 *
 *   The whole ThreadX/NetX Duo stack is a singleton inside
 *   bsdsocket.library's segment. A Shell command that links libnetxduo.a gets
 *   a SECOND set of NetX Duo globals, a second NX_IP that owns no interfaces
 *   and a ThreadX kernel that was never entered -- so every question it asks
 *   is answered by the wrong stack, and every NetX Duo call that suspends
 *   "the calling thread" reaches for a scheduler that is not running.
 *
 *   That is not a hypothetical. src/tools/netstack_weak.c supplies weak
 *   netstack_get()/netstack_ip() stubs that return NULL, no command links
 *   src/netstack, and so in every shipped build `netstat`, `ping` and
 *   ShowNetStatus's live path read NULL and print "the network is up, but
 *   this command cannot read it" -- a message that reads like a pass. See
 *   docs/RESEARCH.md 19.6 and 21.
 *
 *   src/tools/nettrace.c is the one command that already solved this: it
 *   reaches the capture engine through the eight published bpf_* LVOs rather
 *   than by linking src/bpf/, "because a tool that linked the archive would
 *   get its OWN copy of the channel table and capture nothing at all". This
 *   header is the same answer for the rest of the stack.
 *
 * A SNAPSHOT, NOT A POINTER
 *
 *   The obvious shape -- hand the caller the live NX_IP * -- is the wrong
 *   one. AmigaOS has no memory protection, so a pointer into another task's
 *   structures stays dereferenceable long after the stack has gone down, and
 *   this project has already shipped one use-after-free of exactly that kind
 *   (a teardown path that freed a reply port and the stack a thread was still
 *   running on). Worse, walking NetX Duo's tables takes the ThreadX baton,
 *   which a Shell command must not hold while it prints.
 *
 *   So the library COPIES. NetStackQuery() takes the baton, fills the
 *   caller's buffer with plain scalars, and gives the baton back before
 *   returning. Nothing the caller holds afterwards points into the stack.
 *
 * VERSIONING, AND WHY THE MAGIC IS NOT CEREMONY
 *
 *   These two slots sit past every offset any published bsdsocket ABI names
 *   -- past AmiTCP V3, past AmiTCP V4, past Roadshow's extension set and past
 *   the six reserved-for-expansion slots that clib/bsdsocket_protos.h
 *   documents after getnameinfo(). The only way to reach them is
 *   deliberately. But if some future vendor ever allocates the same offset
 *   for something else, a caller of THAT function arrives here with whatever
 *   it happened to have in its registers, and this call must do nothing
 *   rather than something. Wrong magic, wrong version or a buffer too small
 *   for its own header: the library writes nothing and fails.
 *
 *   nsh_Version is the CALLER's, and the library refuses a version it does
 *   not know. nsh_EntrySize is the LIBRARY's, so a caller can check that the
 *   struct it was compiled against is the struct it was handed.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_NETSTATUS_H
#define AMINETXDUO_NETSTATUS_H

#include <exec/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ LVOs --
 *
 * -0x360 is bsd_ObtainNetXDuoContext (aminetxduo/nxcontext.h), which exists
 * only in an AMINETXDUO_TLS build -- but its SLOT is unconditional, so these
 * two are at the same offset in every configuration. A network command that
 * works only in a TLS build is not a command.
 */
#define AMI_NETSTATUS_QUERY_LVO     (-0x366)
#define AMI_NETSTATUS_CONTROL_LVO   (-0x36c)

#define AMI_NETSTATUS_MAGIC         0x414E5351UL    /* 'ANSQ' */
#define AMI_NETSTATUS_VERSION       1

/* ------------------------------------------------------------ selectors --
 *
 * One selector per table. Adding a table later costs a selector, not an LVO,
 * which is why this is a selector at all.
 */
#define NETSTATUS_SYSTEM        1   /* one NetStatusSystem                   */
#define NETSTATUS_INTERFACES    2   /* NetStatusInterface[]                  */
#define NETSTATUS_STATS         3   /* one NetStatusStats                    */
#define NETSTATUS_ARP           4   /* NetStatusArp[]                        */
#define NETSTATUS_ROUTES        5   /* NetStatusRoute[]                      */
#define NETSTATUS_SOCKETS       6   /* NetStatusSocket[]                     */

/*
 * Every buffer starts with this. The caller fills nsh_Magic and nsh_Version;
 * the library fills the rest and writes as many entries after it as will fit,
 * reporting in nsh_Available how many it had. Truncation is therefore
 * detectable rather than silent -- nsh_Count < nsh_Available.
 */
typedef struct NetStatusHeader
{
    ULONG   nsh_Magic;          /* in:  AMI_NETSTATUS_MAGIC                  */
    UWORD   nsh_Version;        /* in:  the caller's AMI_NETSTATUS_VERSION   */
    UWORD   nsh_Type;           /* out: the selector this answers            */
    UWORD   nsh_EntrySize;      /* out: sizeof one entry, as the LIBRARY sees it */
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
/*
 * NX_ENABLE_IP_STATIC_ROUTING. When this is CLEAR, NETSTATUS_ROUTES answers
 * with the default gateway alone and NETCTRL_ROUTE_ADD/DELETE fail with
 * ENOSYS, because NetX Duo's routing table is not compiled into this build at
 * all -- port/netxduo-amiga/inc/nx_user.h sets NX_IP_ROUTING_TABLE_SIZE,
 * which reads as though it were enabled, and is inert without the enable.
 */
#define NETSTATUS_SYS_ROUTING   0x0008UL

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
    ULONG   nss_Reserved[7];
} NetStatusSystem;

/* ----------------------------------------------- NETSTATUS_INTERFACES --- */

#define NETSTATUS_NAME_LEN      32
#define NETSTATUS_DEVICE_LEN    32
#define NETSTATUS_MAC_SIZE      6

/* nsi_Flags */
#define NETSTATUS_IF_ATTACHED   0x0001  /* the NX_INTERFACE slot is valid    */
#define NETSTATUS_IF_LINKUP     0x0002  /* NetX Duo believes the link is up  */
#define NETSTATUS_IF_SANA2      0x0004  /* a SANA-II device is attached      */
#define NETSTATUS_IF_ONLINE     0x0008  /* and that device reports online    */
#define NETSTATUS_IF_NAMED      0x0010  /* nsi_Name came from the config     */

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

    /* From the SANA-II shim; all zero unless NETSTATUS_IF_SANA2 is set. */
    ULONG   nsi_PacketsIn;
    ULONG   nsi_PacketsOut;
    ULONG   nsi_BadData;
    ULONG   nsi_Overruns;
    ULONG   nsi_UnknownTypes;
    ULONG   nsi_Reconfigurations;
    ULONG   nsi_TxErrors;
    ULONG   nsi_RxErrors;
    ULONG   nsi_AllocFailures;
} NetStatusInterface;

/* ---------------------------------------------------- NETSTATUS_STATS --- */

/* nsx_Have -- a protocol NetX Duo was not built with is not "all zero". */
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

/* --------------------------------------------------- NETSTATUS_ROUTES --- */

/* nsr_Flags -- the BSD spelling, because that is what netstat -r prints. */
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

/*
 * TCP state numbers, spelled out so a caller need not include nx_api.h to
 * name them. These are NetX Duo's own values (nx_api.h NX_TCP_*), and the
 * _Static_asserts in src/bsdsocket/netstatus.c hold them to it.
 */
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

/* ------------------------------------------------------------- control --
 *
 * NetStackControl() is the mutating half, and is deliberately a separate LVO
 * from the reading half so that a caller which only reads cannot get one of
 * these by mistyping a selector.
 *
 * Every operation takes the same argument block. Which fields matter depends
 * on the operation and is stated per operation below; the rest must be zero,
 * so that adding a meaning to one later cannot change what an older caller
 * asked for.
 */
#define NETCTRL_INTERFACE_UP    1   /* nsc_Index                             */
#define NETCTRL_INTERFACE_DOWN  2   /* nsc_Index                             */
#define NETCTRL_GATEWAY_SET     3   /* nsc_Gateway                           */
#define NETCTRL_GATEWAY_CLEAR   4   /* --                                    */
#define NETCTRL_ROUTE_ADD       5   /* nsc_Destination/NetMask/Gateway/Index */
#define NETCTRL_ROUTE_DELETE    6   /* nsc_Destination/NetMask               */
#define NETCTRL_ARP_ADD         7   /* nsc_Destination, nsc_HwAddress, Index */
#define NETCTRL_ARP_DELETE      8   /* nsc_Destination                       */
#define NETCTRL_ARP_FLUSH       9   /* --                                    */

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
    ULONG   nsc_Reserved[4];
} NetStatusControl;

#ifdef __cplusplus
}
#endif

#endif /* AMINETXDUO_NETSTATUS_H */

/*
 * AmiNetXDuo, asking the running stack what it is doing.
 *
 *   The whole ThreadX/NetX Duo stack is a singleton inside bsdsocket.library's
 *   segment. A Shell command that links libnetxduo.a gets a second set of NetX
 *   Duo globals, a second NX_IP with no interfaces and a ThreadX kernel that
 *   was never entered, so every question it asks is answered by the wrong
 *   stack, and every NetX Duo call that suspends the calling thread reaches for
 *   a scheduler that is not running.
 *
 *   src/tools/netstack_weak.c supplies weak netstack_get()/netstack_ip() stubs
 *   that return NULL, and no command links src/netstack, so in every shipped
 *   build `netstat`, `ping` and ShowNetStatus's live path read NULL and report
 *   the network as up but unreadable, which reads like a pass. See
 *   docs/RESEARCH.md 19.6 and 21.
 *
 *   src/tools/nettrace.c already solved this: it reaches the capture engine
 *   through the eight published bpf_* LVOs rather than by linking src/bpf/,
 *   because a tool that linked the archive would get its own copy of the
 *   channel table and capture nothing. This header is the same answer for the
 *   rest of the stack.
 *
 *   It returns a snapshot, not a pointer. AmigaOS has no memory protection, so
 *   a live NX_IP * into another task's structures stays dereferenceable long
 *   after the stack has gone down, and this project has already shipped one
 *   use-after-free of that kind (a teardown path that freed a reply port and
 *   the stack a thread was still running on). Walking NetX Duo's tables also
 *   takes the ThreadX baton, which a Shell command must not hold while it
 *   prints. So the library copies. NetStackQuery() acquires the baton, fills
 *   the caller's buffer with plain scalars, and releases the baton before
 *   returning. Nothing the caller holds afterwards points into the stack.
 *
 *   These two slots sit past every offset any published bsdsocket ABI names,
 *   past AmiTCP V3, past AmiTCP V4, past Roadshow's extension set and past the
 *   six reserved-for-expansion slots that clib/bsdsocket_protos.h documents
 *   after getnameinfo(). The only way to reach them is on purpose. If some
 *   future vendor allocates the same offset for something else, a caller of
 *   that function arrives here with whatever it had in its registers, and this
 *   call must then do nothing. Wrong magic, wrong version or a buffer too small
 *   for its own header: the library writes nothing and fails.
 *
 *   nsh_Version is the caller's, and the library refuses a version it does not
 *   know. nsh_EntrySize is the library's, so a caller can check that the struct
 *   it was compiled against is the struct it was handed.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_NETSTATUS_H
#define AMINETXDUO_NETSTATUS_H

#include <exec/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ LVOs,
 *
 * -0x360 is bsd_ObtainNetXDuoContext (aminetxduo/nxcontext.h), which exists
 * only in an AMINETXDUO_TLS build, but its slot is unconditional, so these two
 * are at the same offset in every configuration.
 */
#define AMI_NETSTATUS_QUERY_LVO     (-0x366)
#define AMI_NETSTATUS_CONTROL_LVO   (-0x36c)

#define AMI_NETSTATUS_MAGIC         0x414E5351UL    /* 'ANSQ' */
/*
 * 10 since NETCTRL_STACK_NOTIFY, NETCTRL_STACK_RELEASE and NETSTATUS_OPENERS,
 * which also named nsc_Count out of the reserved words.
 *
 * 9 was NETCTRL_INTERFACE_MDNS. 8 was NETCTRL_INTERFACE_CONFIGURE, the DHCP
 * three and NETCTRL_HOSTNAME_SET, which also grew the control block by
 * nsc_HostName. A caller and a library that disagree fail every call rather
 * than half of them, which is why the commands and the library ship together.
 *
 * This is the compatibility mechanism for a record that grows. The size check in
 * bsd_NetStackQuery() is not: it rejects a buffer too small for the record, and
 * a caller that agrees on the version agrees on the record, so a matched caller
 * never meets it. It is there for the arrival that agreed on nothing.
 *
 * This number and the paragraphs above it move together. Adding an operation
 * without moving this leaves two different header shapes both claiming the same
 * version, and the checks at src/bsdsocket/netstatus.c:1200 and :1448 are exact
 * equality in both directions, so they cannot tell them apart. That has already
 * happened twice. NETCTRL_INTERFACE_ADD and NETCTRL_STACK_HOLD were both added
 * under 7 while the comment still said 6. Then the DHCP three (b40dc23) and
 * NETCTRL_HOSTNAME_SET (f629b38) were added with this constant left at 8, and
 * the sentence above was rewritten afterwards to cover them, which makes the
 * two agree now and did not make the libraries in between distinguishable.
 */
#define AMI_NETSTATUS_VERSION       10

/* Fixed widths every record shares.  Up here rather than beside the first
   record that uses one, because NetStatusSystem needs NETSTATUS_NAME_LEN and
   is declared before the interface table. */
#define NETSTATUS_NAME_LEN      32
#define NETSTATUS_DEVICE_LEN    32
#define NETSTATUS_MAC_SIZE      6
/* A host name, RFC 1123 2.1's 63-character label and a NUL, which is also what
   AMI_CFG_NAME_LEN (aminetxduo/config.h) sizes cfg->hostname at. */
#define NETSTATUS_HOSTNAME_LEN  64

/*
 * The DNS-SD widths, from RFC 6763 and the mDNS module's own limits: a service
 * type is "<sn>._tcp" with <sn> up to 15 characters, an instance name is one
 * DNS label, a target host is a name, and a TXT record is a set of key=value
 * strings the module hands back semicolon-separated.
 *
 * NETSTATUS_SVC_TXT_LEN is shorter than a TXT record can legally be. A record
 * past it is truncated rather than dropped, and NETSTATUS_SVC_TXTCUT says so,
 * because the keys a reader wants are at the front.
 */
#define NETSTATUS_SVC_NAME_LEN  64
#define NETSTATUS_SVC_TYPE_LEN  24
#define NETSTATUS_SVC_HOST_LEN  64
#define NETSTATUS_SVC_TXT_LEN   192

/*
 * The library revision that first had these slots, and the one check a caller
 * cannot skip.
 *
 * lib_Version stays 4: it is the AmiTCP V4 ABI number every caller passes to
 * OpenLibrary(), and moving it would lock out every program that asks for 4.
 * lib_Revision is ours, so it is what identifies which of our libraries this
 * is.
 *
 * The identity check is not enough on its own. A caller that finds a
 * bsdsocket.library whose lib_IdString says AmiNetXDuo will jump to -0x366, and
 * in the published v0.2.0 library that offset is past the end of the vector
 * table, where MakeLibrary() put the (APTR)-1 terminator. That is a guru.
 *
 * Bump this when a caller of this interface needs a newer library: when a
 * netstatus vector is added, or when AMI_NETSTATUS_VERSION moves. Not merely
 * because BSD_LIB_REVISION did. A revision that adds vectors no netstatus
 * caller touches still answers everything here, and refusing it would be a
 * wrong diagnosis.
 *
 * 6 because AMI_NETSTATUS_VERSION is 10 and revision 6 is the first library
 * that speaks it. A revision-5 library answers 9, which the exact-equality
 * check below refuses on every call. Without this the refusal arrives as
 * EINVAL from whichever call happened to be first, and reads like the feature
 * being absent rather than like half an install.
 *
 * The version check inside the library catches a mismatched pair too, but only
 * after the call, where it is indistinguishable from the feature being absent.
 * ShowNetServices read it as the library having no mDNS and told the reader to
 * stop looking. This check runs before any call and says to finish the install.
 */
#define AMI_NETSTATUS_MIN_REVISION  6

/* ------------------------------------------------------------ selectors,
 *
 * One selector per table, so adding a table later costs a selector, not an LVO.
 */
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
/*
 * 14 arrived without moving AMI_NETSTATUS_VERSION, which is right for this kind
 * of addition. The rule above is about record shapes: a control block that
 * grows, or a table whose entry grows, gives two libraries the same version
 * number and different bytes, and the exact-equality check cannot tell them
 * apart. A new selector returning a record no older library ever wrote changes
 * no shape. A version-10 library that predates it answers EINVAL, which is a
 * clean refusal a caller can act on, and src/tools/netstat.c acts on it by
 * leaving the column out. Bumping instead would have forced every command to
 * ship with the library for a column.
 */
#define NETSTATUS_TCPSTALL     14   /* NetStatusTcpStall[]                   */

/* 15 the same way, and for the same reason: a new record no older library
   ever wrote. A version-10 library that predates it answers EINVAL and the
   section is left out. */
#define NETSTATUS_DEST6        15   /* NetStatusDest6[]                      */

/* 16 the same way, and for the same reason: a new record no older library ever
   wrote. AMI_NETSTATUS_VERSION and AMI_NETSTATUS_MIN_REVISION both stay where
   they are. A version-10 library that predates it answers EINVAL, and
   ShowNetStatus leaves the section out rather than reporting a fault. */
#define NETSTATUS_EVENTS       16   /* NetStatusEvent[]                      */
#define NETSTATUS_RXBUDGET     17   /* one NetStatusRxBudget                 */

/*
 * Every buffer starts with this. The caller fills nsh_Magic and nsh_Version.
 * The library fills the rest and writes as many entries after it as will fit,
 * reporting in nsh_Available how many it had. Truncation is therefore
 * detectable rather than silent: nsh_Count < nsh_Available.
 */
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
/*
 * NX_ENABLE_IP_STATIC_ROUTING, which the shipped build defines. Set means NetX
 * Duo's static routing table is compiled in: NETSTATUS_ROUTES reports it
 * alongside the interface prefixes and the default gateway, and
 * NETCTRL_ROUTE_ADD/DELETE work. Clear means the table does not exist in this
 * build, NETSTATUS_ROUTES has only the prefixes and the gateway to report, and
 * NETCTRL_ROUTE_ADD/DELETE fail with ENOSYS. Ask before adding a route rather
 * than reading ENOSYS as a failure.
 */
#define NETSTATUS_SYS_ROUTING   0x0008UL

/*
 * AMINETXDUO_MDNS, and nss_MdnsName is then the name this machine answers to on
 * the local network.  Clear means the build has no responder and the field is
 * empty.  Set with an empty field means the responder is there but has not
 * claimed a name yet.
 */
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
    /*
     * What this machine calls itself on the local network, with the ".local":
     * the name somebody at another machine types to reach it.  Nothing else
     * reports it.  Empty unless NETSTATUS_SYS_MDNS.
     */
    char    nss_MdnsName[NETSTATUS_NAME_LEN];
    /*
     * Which of the places that can name a machine named this one:
     * AmiHostnameSource (aminetxduo/config.h). A name is not
     * self-explaining. A remnant ENV:HOSTNAME kept a renamed machine
     * answering to its old name and nothing said so, and this is the only
     * field that says.
     *
     * Zero is AMI_HOSTNAME_NONE and means either that nothing named the
     * machine or that the library predates the field, so a reader reports
     * the source as not stated rather than guessing. It is taken out of
     * nss_Reserved, which every library has always zeroed, so no version and
     * no minimum revision move for it.
     */
    ULONG   nss_HostSource;
    /*
     * How many programs have the library open, and what the library's own
     * open count is. They differ by the reference NETCTRL_STACK_HOLD took,
     * which belongs to no program and is what keeps the network standing
     * after the command that started it has exited. Both out of nss_Reserved.
     */
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
       pair: the device drained the wire straight into the packet.  The
       summed counter cannot answer this -- both fill paths fuse a sum. */
    ULONG   nsi_RxDirectFill;
} NetStatusInterface;

/* ----------------------------------------------- NETSTATUS_ADDRESSES6 --- */

/*
 * The IPv6 addresses each interface holds, one entry per address, in the
 * order NetX Duo keeps them on the interface's own list.  An interface with
 * IPv6 running always has at least its fe80::/64 link-local address.  A global
 * one arrives from CONFIGURE6, by advertisement or by hand.
 *
 * An IPv4-only build answers this selector with no entries rather than an
 * error, so a caller needs no build-time test to ask.
 *
 * The address is four host-order ULONGs, NetX Duo's own form, which is what
 * netstack_ipv6_address_get() and ami_config_format_ip6() both speak.
 */

/* nsn_State, NX_IPV6_ADDR_STATE_*, spelled out so a caller need not include
   nx_api.h.  A TENTATIVE address is still running duplicate address detection
   and must not be used as a source. */
#define NETSTATUS_IP6_TENTATIVE     1
#define NETSTATUS_IP6_PREFERRED     2
#define NETSTATUS_IP6_DEPRECATED    3
#define NETSTATUS_IP6_VALID         4

/* nsn_Origin, how the address was obtained.  A report that cannot say this
   cannot tell an advertised address from one a DHCPv6 server leased. */
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

/*
 * What the DHCP server said, per interface, which nothing kept until now.  The
 * lease was applied at bring-up and then thrown away, so a machine that got its
 * address by DHCP could not say who gave it that address or how long it is good
 * for.
 *
 * The offered lists are reported whether or not they were used.  A server that
 * hands out a name server this machine ignored is usually the explanation.
 */

/* nsd_State */
#define NETSTATUS_DHCP_OFF      0   /* not using DHCP on this interface     */
#define NETSTATUS_DHCP_WORKING  1   /* asking: discovering, requesting, ARP */
#define NETSTATUS_DHCP_BOUND    2   /* it has a lease; the rest is filled   */

#define NETSTATUS_DHCP_ADDRS    8   /* of each list; the option may hold more */

/* nsd_LeaseSeconds when the server said the lease never expires. */
#define NETSTATUS_DHCP_FOREVER  0xFFFFFFFFUL

/*
 * nsd_RawState, NX_DHCP_STATE_* verbatim, spelled out so a caller need not
 * include nxd_dhcp_client.h. nsd_State collapses six of these into
 * NETSTATUS_DHCP_WORKING and three into NETSTATUS_DHCP_BOUND, which is what a
 * caller waiting for an address wants and is not enough for a caller watching a
 * renewal: BOUND, RENEWING and REBINDING are all NETSTATUS_DHCP_BOUND, so a
 * lease being extended and a lease sitting still read the same.
 *
 * Named out of what was nsd_Pad, which every library has always zeroed, so the
 * record is the size it always was and nsd_State means exactly what it did.
 * Zero is NX_DHCP_STATE_NOT_STARTED and is also what a library predating the
 * field answers, and the two mean the same thing here.
 */
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

/*
 * Whether the machine was ever held and what the stack currently owns, rather
 * than how much traffic moved.  The tick and baton halves are the ThreadX tick
 * task's own accounting and the baton bracket's counters.  Neither touches NetX
 * Duo, so this selector answers with the stack up or down.  The memory half is
 * AmiMemStats (aminetxduo/compat.h), which the published health mark points at
 * as well, so `netstat -h` on a wedged machine and this call report the same
 * record.
 *
 * The memory half is what makes a suspected leak answerable.  AvailMem falls
 * for every program on the machine.  nsl_AllocLive is ours alone, nsl_Sockets
 * counts the AmiSocket structures the library owns (docs/RESEARCH.md 37.5 was
 * 776 of them), and the nsl_Pool fields are the packet pool, which drains for
 * different reasons and wants a different fix.  Each has a high-water mark
 * beside it, because a single reading cannot say whether a number is climbing.
 *
 * nsl_PoolFree and nsl_PoolLow are sampled rather than exact: NetX Duo
 * allocates packets from its own internals as well as from ours, so there is
 * no one place to count them.  netstack_pool_sample() refreshes them on the way
 * out of every stack operation, and this call refreshes them before answering.
 *
 * nsl_TickWorstStallMs is the longest the tick task ever went between wakeups,
 * and nsl_TickWorstServiceUs is what the wakeup before that one spent.  Large
 * next to a small service figure says the tick task was not dispatched, not
 * that it was slow.  Both cover every wakeup, not only the ones that clipped,
 * so they are non-zero on a healthy machine and the pair explains a skew peak
 * rather than sitting at 0 beside it.  nsl_BatonMoved or nsl_BatonFull
 * non-zero says the bracket lost track of a thread.
 *
 * nsl_BatonStateShared counts the times a task inside the bracket found
 * _tx_thread_system_state already raised by another one. That window makes
 * every other task look like an ISR to ThreadX, so a blocking service
 * entered on one returns without blocking and leaves the caller linked into
 * a suspension list it is no longer on. It must stay zero.
 *
 * nsl_TickSkew is how far behind real time the timer wheel is, in ticks: what
 * it has yet to be given plus what nsl_TickLost took off it for good. The
 * ThreadX clock is not in it, that comes from the E-Clock and is true either
 * way, so this measures how late timers are running and nothing else.
 * nsl_TickSkewPeak is sampled before a backlog is worked off, so it moves on a
 * machine where nothing was ever lost.
 *
 * nsl_TickSkewPeak has a floor of one wakeup's worth of ticks: a wakeup is
 * sampled owing every tick period that elapsed since the last one, and the
 * VBlank source wakes once every TX_AMIGA_VBLANK_DIVIDER frames, 2 on a stock
 * build. A healthy PAL machine therefore reads 2 having never been late. The
 * floor is not subtracted, because what one wakeup owes depends on the source
 * that woke it. Read it against nsl_TickWorstStallMs: a peak worth no more
 * than that stall at 20 ms a tick is one late wakeup, and a larger one is
 * lateness that accumulated across several.
 *
 * nsl_TickDeferred is ticks the budget put off to a later wakeup, which the
 * wheel does get. nsl_TickLost is ticks it never gets.
 */
typedef struct NetStatusHealth
{
    ULONG   nsl_TickTicks;
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

/*
 * IPv6's answer to the ARP cache.  There is no ARP: RFC 4861 neighbour
 * discovery does the same job over ICMPv6, and NetX Duo keeps its result in
 * nx_ipv6_nd_cache[], NX_IPV6_NEIGHBOR_CACHE_SIZE entries
 * (port/netxduo-amiga/inc/nx_user.h).
 *
 * An ARP entry is resolved or it is not.  A neighbour entry says how the stack
 * currently believes the address behaves, and the five states separate an
 * address nothing has answered for, one that answered once and has not been
 * checked since, and one being checked now.
 *
 * An IPv4-only build answers with no entries rather than an error.
 */

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

/*
 * IPv6 has no destination-to-next-hop table.  NetX Duo decides where a packet
 * goes from two lists, and this selector reports both in the order
 * _nx_ipv6_packet_send() consults them:
 *
 *   1. the on-link prefixes, nx_ipv6_prefix_list_ptr, plus the prefix of
 *      every manually configured address, which is what _nxd_ipv6_search_onlink()
 *      looks at.  A destination inside one of these is reached directly and
 *      nsr6_NextHop is all zero.
 *   2. the default routers, nx_ipv6_default_router_table, destination ::/0.
 *      Everything with nowhere better to go is handed to one of these.
 *
 * A stateless-autoconfigured address is deliberately not reported from its own
 * prefix: a router advertisement can set A without L, in which case the address
 * exists and the prefix is not on link.  The prefix-list entry the same
 * advertisement makes is reported instead, so this table says where packets go
 * rather than which addresses exist.
 *
 * fe80::/64 is not in it either.  _nxd_ipv6_search_onlink() answers 1 for every
 * link-local address before it looks at any list, so there is no entry to
 * report and none to remove.
 *
 * An IPv4-only build answers with no entries rather than an error.
 */

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

/*
 * The destination cache, nx_ipv6_destination_table[].  Where NETSTATUS_ROUTES6
 * reports the two lists a route is derived from, this reports what the stack
 * decided, per destination, and it is the first thing
 * _nx_ipv6_packet_send() looks at.
 *
 * It is reported because a full one used to be silent.  A miss on a full table
 * released the packet and returned, out of a VOID function, so a machine that
 * had reached its capacity of distinct destinations stopped sending to new ones
 * with no error anywhere.  Two slots go before any user command, to the
 * link-local addresses of the routers this machine answered.  Entries are given
 * up least-recently-used now, but a reader still has to be able to ask which
 * destinations are in use and whether the table is full.
 *
 * nsd6_Age is that answer: uses of the table since this entry was last chosen,
 * so 0 is the entry just used and the largest is the next one to be evicted.
 * It counts uses, not seconds.  The table has no clock but its own.
 *
 * nsd6_Capacity is the same number in every row, because a count of entries
 * does not say whether the table is full and nothing else here carries the
 * build's NX_IPV6_DESTINATION_TABLE_SIZE.
 *
 * An IPv4-only build answers with no entries rather than an error.
 */

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

/* ------------------------------------------------- NETSTATUS_TCPSTALL ---
 *
 * One row per TCP socket, in the same order NETSTATUS_SOCKETS walks them, but
 * a separate table rather than four more fields on NetStatusSocket: that
 * record is 16 fixed bytes and every consumer checks nsh_EntrySize for exact
 * equality, so growing it is an ABI break for a diagnostic.
 *
 * The identifying tuple is repeated here so the two tables can be joined
 * without trusting the order, which two calls a moment apart do not promise.
 *
 * This is what a stalled connection looks like from outside: nst_Stalled
 * climbing while nst_Retransmits climbs with it and nst_Rto doubles. An
 * established socket with nothing outstanding reads zero in all three.
 */
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

/* ------------------------------------------------- NETSTATUS_SERVICES,
 *
 * What a browse has heard so far. This is a read of the mDNS cache:
 * NETCTRL_MDNS_BROWSE starts the query, answers arrive on the responder's own
 * thread over the following seconds, and this selector says what has landed by
 * the time it is called. Calling it twice gives two different answers, and
 * neither is a complete list of the network, because mDNS has no end of
 * results.
 *
 * One thing it does put on the wire: a service whose SRV record arrived without
 * an address record beside it has its target resolved here, because a row with
 * a host name and no address is a service that cannot be used. That is the only
 * reason this selector can take a moment, and it is bounded at two seconds
 * however many such rows the cache holds.
 *
 * It answers with the whole cache, of every type, and not with the type the
 * caller last browsed for: one cache, any number of readers, so a filter here
 * would depend on who else was running. Match on nsv_Type.
 *
 * Empty on a build without AMINETXDUO_MDNS, which NETSTATUS_SYS_MDNS reports.
 */

/* nsv_Flags */
/*
 * Clear means this row is a service type and nothing more: the answer came
 * from the _services._dns-sd._udp.local enumeration, so something on the
 * network offers that type but no instance of it has been asked for yet.
 * nsv_Name, nsv_Host, nsv_Address, nsv_Port and nsv_Text are then empty.
 */
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

/* --------------------------------------------------- NETSTATUS_OPENERS,
 *
 * The programs using the network: one row per OpenLibrary() of
 * bsdsocket.library that has not been given back, which is the number the
 * stack goes down at when it reaches zero.
 *
 * It is here so a shutdown can name the program that did not let go, rather
 * than reporting a count the reader cannot act on.
 *
 * The reference NETCTRL_STACK_HOLD takes belongs to no program and is not a
 * row here. NETSTATUS_SYSTEM's nss_Openers is the row count and nss_OpenCnt
 * the library's own, so a caller that wants the difference can have it without
 * reading the table.
 */

/* nso_Flags */
#define NETSTATUS_OPENER_SELF   0x0001  /* the base this query came through  */
/*
 * Opened the library and exited without closing it. The base is on the list
 * for good and its task is gone, so nothing will ever be delivered to it and
 * a shutdown will never see it leave. nso_Task is what it used to be.
 */
#define NETSTATUS_OPENER_GONE   0x0002

typedef struct NetStatusOpener
{
    UWORD   nso_Flags;
    UWORD   nso_Sockets;                /* how many it has open              */
    ULONG   nso_Task;                   /* struct Task, a number to print    */
    ULONG   nso_BreakMask;              /* SBTC_BREAKMASK, what to signal it */
    char    nso_Name[NETSTATUS_NAME_LEN];   /* the task's, empty if unnamed  */
} NetStatusOpener;

/* ---------------------------------------------------- NETSTATUS_EVENTS,
 *
 * THE FAULT THIS EXISTS FOR.  Every diagnostic in the library is an AMI_ERROR,
 * an AMI_WARN or an AMI_INFO, and all three compile to `do { if (0) ... }
 * while (0)` unless AMINETXDUO_LOG is defined, which it is not in any shipped
 * binary and cannot be: the strings are 12,820 bytes on the 68000 tier
 * (aminetxduo/compat.h).  "sana2: leaking the interface, the device still
 * holds requests inside it" was written so that a user who hit it could quote
 * it, and no user has ever been able to.  Two of the paths that decide a
 * shutdown are silent for exactly this reason.
 *
 * So the library records a number.  A small ring in BSS, no allocation, no
 * strings, and the sentence that goes with each number lives in
 * src/tools/tool_events.c, which is a Shell command and can afford it.  The
 * user never sees a code.
 *
 * This is not a new mechanism.  anxnet.device already does exactly this
 * (aminetxduo/anxdiag.h, read by CheckNetDevice), for the same reason and in
 * the same dialect: a code, a qualifier saying which thing it is about, and a
 * value whose meaning the code decides.  Two mechanisms that do the same thing
 * differently would be worse than either, so this is that one with a time and
 * a sequence number added, and the transport is this interface rather than a
 * second public semaphore because the library has one already.
 *
 * IT ANSWERS WITH THE STACK DOWN.  The ring is the library's own memory, not
 * NetX Duo's, so this selector is served without the ThreadX baton and without
 * a live NX_IP -- which is the whole point, since the events worth reading are
 * the ones a teardown left behind.  tool_netstatus_open() looks for a resident
 * library rather than opening one, so reading the ring does not restart the
 * stack it is reporting on.
 *
 * WHAT IS LOST IS VISIBLE.  The ring holds AMINETXDUO_EVENT_RING entries and
 * overwrites its oldest.  nse_Seq counts every event ever recorded, so an
 * answer whose first entry has nse_Seq > 1 is how a reader knows entries went
 * past, and which ones.  nsh_Available is what the ring holds, not what the
 * machine did.
 *
 * The numbers are a wire format between two binaries and are never reused or
 * renumbered.  A code the tool does not know is printed as itself rather than
 * dropped, so an older ShowNetStatus against a newer library still says
 * something true.
 */

/* nse_Index for an event about the machine rather than one interface. */
#define NETEVENT_NOINDEX        0xffffu

/* --- the stack ---------------------------------------------------------- */
#define NETEVENT_BRINGUP         1  /* came up; value = interfaces opened    */
#define NETEVENT_SHUTDOWN        2  /* teardown began; value = interfaces it
                                       still held                            */
/*
 * NETCTRL_STACK_NOTIFY and NETCTRL_STACK_RELEASE, the pair NetShutdown is made
 * of.  Recorded because a shutdown that did not finish is the reported
 * complaint and which half ran is the first thing to establish: the value on
 * NOTIFY is how many programs were signalled, and on RELEASE how many still
 * had the library open after the hold was given back.  Non-zero there is a
 * program that did not let go, which NETSTATUS_OPENERS then names.
 */
#define NETEVENT_NOTIFY          3  /* value = programs signalled            */
#define NETEVENT_RELEASE         4  /* value = openers left                  */

/* --- bring-up ----------------------------------------------------------- */
/*
 * nse_Index on the first two is the configuration slot rather than an
 * interface index: a device that does not open never becomes an interface and
 * has no index of the other kind.
 */
#define NETEVENT_DEVICE_OPEN    10  /* OpenDevice() refused; value = its
                                       error code                            */
#define NETEVENT_DEVICE_REFUSED 11  /* it opened and then refused a SANA-II
                                       command; value = the AMI_NET_ERR_*    */
#define NETEVENT_ATTACH_FAILED  12  /* nx_ip_interface_attach() refused;
                                       value = the NX_ status                */
/*
 * Attached, and nx_interface_link_up is false: the attach drives
 * NX_LINK_ENABLE through the driver, and a device that would not go online or
 * whose readers would not start leaves the interface in the stack with nothing
 * on the wire.  Nothing reports it at the moment it happens.
 */
#define NETEVENT_LINK_DOWN      13  /* attached with the link down           */
/*
 * The two bring-up failures that used to leave the ring saying nothing, so a
 * command that read it after AddNetInterface refused could find no record of
 * the stage that refused.
 *
 * ONLINE_FAILED is netstack_interface_up(): NX_LINK_ENABLE through the driver,
 * which is the S2_ONLINE the device is sent.  Its value is the NX_ status,
 * because that is what the call returns and the SANA-II error underneath it
 * has already been consumed by the shim.
 *
 * ATTACH_LIMIT is the honest refusal that the config layer used to pre-empt by
 * refusing to PARSE a third interface (aminetxduo/config.h).  NetX Duo has
 * AMI_CFG_MAX_ATTACHED interface slots and no more; the value is how many
 * interfaces were described, and nse_Index is the slot that would have been
 * next, which is also the count already up.  A reader can therefore say
 * "three described, two online" without re-reading the drawer.
 */
#define NETEVENT_ONLINE_FAILED  14  /* the interface would not go online;
                                       value = the NX_ status                */
#define NETEVENT_ATTACH_LIMIT   15  /* no NetX Duo slot left; value = how many
                                       interfaces were asked for             */
/*
 * The other side of ATTACH_LIMIT, and the reason a machine with four interface
 * files can still bring up the fourth.
 *
 * An interface the start-up pass brought up because it found a file in
 * DEVS:NetInterfaces was never asked for in particular, and there may be more
 * files than slots.  When a user then names one that has no slot, the
 * unasked-for one gives its slot up and goes back to being merely defined.
 * That is a real change to what the machine has on the wire, so it is
 * recorded: nse_Index is the slot that yielded, and the value is how many
 * interfaces the stack knows of.  An interface somebody DID name never yields;
 * that case is ATTACH_LIMIT above and is refused by name.
 */
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
/*
 * The mechanism behind "the card's LED still blinks after NetShutdown", in two
 * halves that have to be read together.
 *
 * OUT_OF_SERVICE is a reader taking S2ERR_OUTOFSERVICE and clearing
 * iface->online, which is right -- the wire has gone -- and is also what makes
 * the next ami_sana2_offline() a no-op, because that returns early on an
 * interface already marked offline.  SKIPPED is that no-op.  The two in
 * sequence say S2_OFFLINE was never issued to a device that is still running
 * its receiver, and the LED is the device's rather than the stack's.
 */
#define NETEVENT_OUT_OF_SERVICE 20  /* a reader saw S2ERR_OUTOFSERVICE and
                                       marked the link down                  */
#define NETEVENT_OFFLINE_SKIPPED 21 /* S2_OFFLINE not issued: already offline */
#define NETEVENT_OFFLINE_FAILED 22  /* the device refused S2_OFFLINE;
                                       value = the SANA-II wire error        */

/* --- teardown ----------------------------------------------------------- */
/*
 * ami_sana2_close() refusing to close and free, because the device still owns
 * a request that points into the allocation.  The value says which side, which
 * is the difference between a driver that ignores AbortIO() on reads and one
 * that lost a write.  This one code with its interface index settles the
 * question on its own.
 */
#define NETEVENT_IFACE_RETAINED 30  /* value = NETEVENT_HELD_*               */
/* And the consequence: the packet pool and the whole stack allocation are kept
   as well, because an orphaned request reaches into both. */
#define NETEVENT_STACK_RETAINED 31  /* value = interfaces retained           */

/* NETEVENT_IFACE_RETAINED values, and they combine. */
#define NETEVENT_HELD_RX        0x0001UL
#define NETEVENT_HELD_TX        0x0002UL

/* --- expunge ------------------------------------------------------------ */
/*
 * The library declining to be unloaded.  One code with a reason rather than
 * five codes, the shape ANXDIAG_ATTACH_FAIL uses for the same kind of answer:
 * a reader wants to know that the segment stayed, and then which of the five
 * holds it.
 */
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

/* -------------------------------------------------- NETSTATUS_RXBUDGET,
 *
 * The receive step budget: one frame's journey from the SANA-II reader to
 * recv(), as E-Clock aggregates per hop.  Filled only by a library built with
 * AMINETXDUO_RXPROBE; any other build answers the selector with every count
 * zero, so a tool can always ask and honestly print "not instrumented".
 * aminetxduo/budget.h defines the legs and src/common/budget.c the method.
 */
#define NETSTATUS_BUDGET_BUCKETS  20

typedef struct NetStatusBudgetLeg
{
    ULONG   nbl_Count;
    ULONG   nbl_Sum;                    /* E-Clock ticks                     */
    ULONG   nbl_Max;
    ULONG   nbl_Hist[NETSTATUS_BUDGET_BUCKETS];  /* [i] holds < 2^i ticks    */
} NetStatusBudgetLeg;

/*
 * The baton holder ring: who held the ThreadX baton longer than ~50 ms, for
 * how long, and where the hold ended.  The waiter's nrb_Baton leg records
 * acquisition latency; this is the other side of the same coin.  Site values
 * mirror AMI_HOLD_SITE_* in aminetxduo/budget.h and must stay in step.
 */
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
    /* The settle leg dissected: three chained sub-legs between the same two
       stamps, so their sum is nrb_Settle told in parts.  Appended here, not
       beside nrb_Settle, so every earlier offset holds and an older tool
       still reads what it knows.  src/common/budget.c owns the definitions. */
    NetStatusBudgetLeg  nrb_Defer;      /* deliver -> IP thread pickup       */
    NetStatusBudgetLeg  nrb_Demux;      /* pickup -> the segment's socket    */
    NetStatusBudgetLeg  nrb_State;      /* socket entry -> receive notify    */
    /* The green realm's scheduling census (AMINETXDUO_GREEN_REALM builds;
       all zero from a baton build).  Appended at the end for the same
       offset-stability reason as the settle sub-legs.  What each counts:
       port/threadx-amiga/inc/tx_amiga.h, TX_AMIGA_GREEN_STATS. */
    ULONG               nrb_GreenSwitches;
    ULONG               nrb_GreenExternal;
    ULONG               nrb_GreenIdleWaits;
    ULONG               nrb_GreenWaitFast;
    ULONG               nrb_GreenWaitSlow;
    ULONG               nrb_GreenStray;     /* MUST be zero; a nonzero count
                                               is an unconverted Exec block
                                               inside the realm              */
    /* The request gate (the adopted-caller boundary): brackets migrated
       into the realm against brackets that fell back to the adopted-baton
       path.  Appended at the end, offsets hold. */
    ULONG               nrb_GateCalls;
    ULONG               nrb_GateFallback;
    /* The signal-bit audit: Exec signal bits allocated on the realm Task,
       of its 16 allocatable -- the one budget every green thread's MsgPort
       and AllocSignal draws from. */
    ULONG               nrb_RealmSigBits;
    /* The gate's free-baton fast path: brackets that took an idle realm's
       baton directly, against nrb_GateCalls that submitted through the
       gate and nrb_GateFallback that fell back to the adopted path.  The
       three partition the brackets; the fast share is what attributes the
       physical baton-leg numbers.  Appended at the end, offsets hold. */
    ULONG               nrb_GateFast;
} NetStatusRxBudget;

/* ------------------------------------------------------------- control,
 *
 * NetStackControl() is the mutating half, on a separate LVO from the reading
 * half so that a caller which only reads cannot get one of these by mistyping a
 * selector.
 *
 * Every operation takes the same argument block. Which fields matter is stated
 * per operation below. The rest must be zero, so that giving one a meaning
 * later cannot change what an older caller asked for.
 */
#define NETCTRL_INTERFACE_UP    1   /* nsc_Index                             */
#define NETCTRL_INTERFACE_DOWN  2   /* nsc_Index                             */
#define NETCTRL_GATEWAY_SET     3   /* nsc_Gateway                           */
#define NETCTRL_GATEWAY_CLEAR   4   /*,                                    */
/*
 * ROUTE_ADD takes nsc_Destination/NetMask/Gateway and not nsc_Index: NetX Duo
 * derives the interface from the next hop, which must be on an interface's own
 * subnet or the call fails with EINVAL. An entry with the same destination and
 * mask has its next hop replaced rather than being duplicated, and the table
 * holds NX_IP_ROUTING_TABLE_SIZE entries. ENOBUFS past that.
 */
#define NETCTRL_ROUTE_ADD       5   /* nsc_Destination/NetMask/Gateway       */
#define NETCTRL_ROUTE_DELETE    6   /* nsc_Destination/NetMask               */
#define NETCTRL_ARP_ADD         7   /* nsc_Destination, nsc_HwAddress, Index */
#define NETCTRL_ARP_DELETE      8   /* nsc_Destination                       */
#define NETCTRL_ARP_FLUSH       9   /*,                                    */

/*
 * The IPv6 pair. One operation covers both of the mechanisms NETSTATUS_ROUTES6
 * reports, because a caller writes the same thing either way, a destination,
 * a prefix length and somewhere to send it:
 *
 *   nsc_PrefixLength 0 with a next hop        a default router
 *   any prefix length with no next hop        an on-link prefix
 *   any prefix length with a next hop         refused: EINVAL. NetX Duo has
 *                                             no destination-to-next-hop table
 *                                             for IPv6 and cannot store one.
 *
 * nsc_Index names the interface. It is required for a link-local next hop,
 * because fe80::/64 exists on every interface and the address alone does not
 * say which. It is ignored for a prefix, which is a property of the machine's
 * whole prefix list rather than of one interface.
 *
 * Both invalidate the IPv6 destination cache, which is a per-destination
 * memory of where packets went last time and would otherwise keep sending
 * them the old way after the route that decided it has changed.
 */
#define NETCTRL_ROUTE6_ADD     10   /* nsc_Destination6/PrefixLength/Gateway6/Index */
#define NETCTRL_ROUTE6_DELETE  11   /* nsc_Destination6/PrefixLength/Gateway6 */

#define NETCTRL_ND_ADD         12   /* nsc_Destination6, nsc_HwAddress, Index */
#define NETCTRL_ND_DELETE      13   /* nsc_Destination6                      */

/*
 * The mDNS browse pair, nsc_Name naming a service type such as "_http._tcp".
 * An empty nsc_Name is the RFC 6763 9 meta-query: it asks what service types
 * exist rather than for instances of one, and is how a browser finds out what
 * there is to browse for.
 *
 * BROWSE registers a continuous query (RFC 6762 5.2, exponential backoff) and
 * returns at once, it does not wait for an answer, because mDNS has none to
 * wait for. The caller sleeps for as long as it is prepared to wait, then reads
 * NETSTATUS_SERVICES.
 *
 * BROWSE_STOP retires the query. A caller that forgets leaves the machine
 * asking the network the same question every few minutes for as long as the
 * stack is up, so it is not optional. The query also occupies the peer cache
 * that the answers have to land in.
 *
 * ENOSYS on a build without AMINETXDUO_MDNS.
 */
#define NETCTRL_MDNS_BROWSE      14 /* nsc_Name, empty for the meta-query    */
#define NETCTRL_MDNS_BROWSE_STOP 15 /* nsc_Name, the same one                */

/*
 * Take an interface out of the stack entirely: the SANA-II device is closed,
 * the NetX Duo interface is detached and the configuration slot is released.
 * NETCTRL_INTERFACE_DOWN stops the traffic and keeps all of that.
 *
 * Refused with EBUSY while anything is still using the interface, counted as
 * TCP connections routed out of it. NETCTRL_F_FORCE overrides, and every such
 * connection is reset. An index is a handle a caller can hold, so removing one
 * interface does not renumber the others.
 */
#define NETCTRL_INTERFACE_REMOVE 16 /* nsc_Index, nsc_Flags                  */

/*
 * Put one back, or bring a new one up: nsc_Name is a file in
 * DEVS:NetInterfaces, and the interface arrives configured the way that file
 * says, as it would have on a boot. That includes the address or the DHCP
 * exchange that fetches one, and the default gateway, which
 * NETCTRL_INTERFACE_REMOVE took away with the interface that carried it.
 *
 * This reads a file, so it must be called from a Process. ENOENT when there is
 * no such file or it cannot be parsed, EEXIST when the stack already has an
 * interface of that name, ENOSPC when every interface slot is taken, ENXIO or
 * EIO when the SANA-II device did not open or did not answer.
 *
 * The address is not waited for. A lease takes seconds to arrive and the
 * caller is the one with a Process to wait in. Read NETSTATUS_INTERFACES until
 * nsi_Address is set, or give up, as AddNetInterface does with its TIMEOUT.
 */
#define NETCTRL_INTERFACE_ADD   17  /* nsc_Name                              */

/*
 * Ask the library to hold the running stack itself.
 *
 * The stack is a singleton inside bsdsocket.library: it comes up on the first
 * OpenLibrary() and goes down when the last opener closes. A command that
 * starts the network and then exits therefore has a problem, since its own
 * close is the last one. AddNetInterface used to solve it by never closing,
 * which kept the network up and left a base behind on every invocation.
 *
 * This says the same thing without the base: after it returns, the library
 * holds a reference of its own and the caller can CloseLibrary() normally. It
 * is idempotent and costs nothing on the second call, so a command can ask
 * every time without accumulating anything.
 *
 * There is no matching release. The reference is permanent for the life of the
 * library, which is what a machine whose interfaces came up at boot wants. An
 * expunge is declined while it is held, as it was while the old open was
 * leaked. ENETDOWN if there is no stack to hold.
 */
#define NETCTRL_STACK_HOLD      18  /*,                                    */

/*
 * Re-address a running interface: what ConfigureNetInterface does, and the
 * thing that until now needed NETCTRL_INTERFACE_REMOVE and _ADD in a pair.
 *
 * nsc_Index names the interface. nsc_Destination is the new address,
 * nsc_NetMask the new mask and nsc_Gateway the new default gateway, and each is
 * applied only when its NETCTRL_F_ bit is set in nsc_Flags: 0.0.0.0 is a thing
 * a caller can legitimately ask for, so it cannot double as a request to leave
 * that field alone.
 *
 * The address and the mask are set together in one NetX Duo call even when only
 * one of them was given, because nx_ip_interface_address_set() takes both and
 * an interface must never be seen carrying a new address with its old mask. A
 * mask of zero on an interface that has none becomes the classful default, the
 * same rule ConfigureInterfaceTagList() applies to an address given without
 * one, and the same code.
 *
 * The gateway is the machine's, not the interface's: NetX Duo keeps one
 * nx_ip_gateway_address for the whole NX_IP. It is set last, after the address,
 * because nx_ip_gateway_address_set() refuses a next hop that is not on some
 * interface's subnet, and the subnet it has to be on is usually the one this
 * call has just changed. A gateway of 0.0.0.0 clears it.
 *
 * EADDRNOTAVAIL for an address NetX Duo would not take, EINVAL for a gateway it
 * would not take, ENXIO for an interface index that is not attached. Nothing is
 * applied by a call that fails on the address. A call that sets the address and
 * then fails on the gateway reports the gateway and keeps the address, which is
 * the half that was asked for first and the half a caller can see.
 */
#define NETCTRL_INTERFACE_CONFIGURE 19 /* nsc_Index/Destination/NetMask/Gateway */

/*
 * The DHCP client on one interface, aimed at one interface rather than at the
 * machine. Before these, the only way to make a lease happen again was
 * Offline/Online or NetShutdown, which restarts every interface there is.
 *
 * All three take nsc_Index. START also reads nsc_Destination.
 *
 *   START    ask for a lease on an interface that has none. This is what
 *            follows a RELEASE, and what a machine moved to another network
 *            needs. EBUSY while an allocation is already in progress on that
 *            interface. Already bound is not busy and starts again.
 *
 *            NETCTRL_F_ADDRESS makes nsc_Destination the address to ask the
 *            server for. It is a wish rather than a demand: DISCOVER is still
 *            sent, so a server that will not give that address offers a
 *            different one instead of answering NAK.
 *
 *   RENEW    extend the lease this interface already has, without giving up
 *            the address: a DHCPREQUEST to the server that granted it, which
 *            is RFC 2131 4.4.5's renewing state entered early. The address does
 *            not change unless the server says it does. ENOTCONN when the
 *            interface has no lease to extend, which is a different thing from
 *            failing and is why it is not silently turned into a START -- the
 *            caller asked to keep an address it does not have.
 *
 *   RELEASE  DHCPRELEASE to the server and stop the client. The interface
 *            keeps the address it was given until something takes it away:
 *            NetX Duo's client does not unconfigure the interface, and this
 *            does not either, because an interface that loses its address the
 *            instant a lease is dropped also loses the route the reply would
 *            have come back on. ENOTCONN when there is no lease.
 *
 * None of them waits. A lease takes seconds and the caller is the one with a
 * Process to wait in, the same division NETCTRL_INTERFACE_ADD draws: poll
 * NETSTATUS_DHCP's nsd_State until it is NETSTATUS_DHCP_BOUND, or give up.
 */
#define NETCTRL_DHCP_START      20  /* nsc_Index                             */
#define NETCTRL_DHCP_RENEW      21  /* nsc_Index                             */
#define NETCTRL_DHCP_RELEASE    22  /* nsc_Index                             */

/*
 * Offer nsc_HostName to the running stack as this machine's name, at the rank
 * of ENV:HOSTNAME.
 *
 * An offer and not an assignment. The name comes from four places, ranked
 * (AmiHostnameSource in aminetxduo/config.h): an interface file's ID=, then
 * ENV:HOSTNAME, then DHCP option 12, then DEVS:Internet/name_resolution.
 * ami_config_hostname_offer() takes a name only from a source at least as
 * strong as the one that named the machine already, and this goes through it,
 * so a machine named by its DHCP server keeps that name and this call is
 * refused with EPERM. The caller can then report which source holds the name,
 * rather than a name that appears to have been set and is not the one
 * gethostname() answers.
 *
 * The rank is fixed at ENV:HOSTNAME because that is the file the caller writes.
 * A caller that could pick its own rank could name the machine at
 * name_resolution rank without anything in DEVS:Internet saying so, and the
 * next boot would undo it with no record of what had happened.
 *
 * This changes the running stack only. Nothing here writes a file. The command
 * that calls it writes ENV:HOSTNAME and ENVARC:HOSTNAME itself, and the two
 * halves are separate because one of them works with the stack down.
 *
 * EINVAL for an empty name, EPERM when a stronger source holds, ENETDOWN when
 * there is no stack. The name is not checked here beyond being non-empty:
 * ami_config_hostname_offer() checks only the sources that were never required
 * to be host names, and the caller is the one that can say which rule a name
 * broke.
 */
#define NETCTRL_HOSTNAME_SET    23  /* nsc_HostName                          */

/*
 * Answer .local on one interface, or stop. nsc_Index names it and
 * NETCTRL_F_MDNS in nsc_Flags is the direction: set turns the responder on,
 * clear turns it off. A switch and not a field, so there is no third state
 * that means leave it unchanged.
 *
 * This is what MDNS= in DEVS:NetInterfaces asked for at boot, asked for again
 * while the machine is running. Until this existed the flag NETSTATUS_IF_MDNS
 * reports was set by a runtime interface add and never acted on, so an
 * interface joined no group, probed for no name and answered nothing while
 * saying it did.
 *
 * Turning it on creates the responder if no interface had asked for one yet --
 * the module is not created at boot when nothing wants it, which is where its
 * saving is -- then joins 224.0.0.251 on that interface and probes for
 * <HOSTNAME>.local there (RFC 6762 8). The services in
 * DEVS:Internet/service_discovery are registered on the interface the first
 * time it is enabled and not again, so an off/on pair re-announces them rather
 * than duplicating them.
 *
 * Turning it off sends the RFC 6762 10.1 goodbye, the records re-announced with
 * a TTL of zero so every cache on the link drops the name at once, then leaves
 * the group. The responder object stays: the goodbye is transmitted by its own
 * thread over the following 750 ms, and deleting it here would guarantee the
 * goodbye never left.
 *
 * Neither direction waits. Probing is three packets 250 ms apart, so a name is
 * claimed about a second after the call returns. NETSTATUS_INTERFACES' MDNS
 * flag is true from the moment the responder is enabled, and
 * NETSTATUS_SYSTEM's nss_MdnsName is what says the probe finished.
 *
 * ENXIO for an interface index that is not attached, ENETDOWN with no stack,
 * EIO when the module refused, ENOMEM when the responder could not be created,
 * ENOSYS on a build without AMINETXDUO_MDNS. Turning on what is already on and
 * off what is already off both succeed.
 */
#define NETCTRL_INTERFACE_MDNS  24  /* nsc_Index, NETCTRL_F_MDNS             */

/*
 * Tell every program using the network that it is stopping, and give back the
 * reference that keeps the network standing. The pair NetShutdown is made of.
 *
 * NOTIFY sends each opener SIGBREAKF_CTRL_C, and its SBTC_BREAKMASK bit as
 * well when it moved that off Ctrl-C. This is the existing convention rather
 * than a new one: AmiTCP's api_sendbreaktotasks() signalled SIGBREAKF_CTRL_C
 * to every task holding a SocketBase, AmiTCP_NG still does, and Roadshow's
 * manual describes NetShutdown as telling "every network program currently
 * running to let go of the network resources and exit". A program blocked in
 * recv() or WaitSelect() comes back with EINTR and one waiting on its own
 * signals wakes up, which is what both already do when the user presses
 * Ctrl-C. nsc_Count comes back with how many were signalled.
 *
 * It does not close anybody's sockets, unblock the library, or touch a task
 * beyond that one signal. A program that ignores it keeps its sockets and
 * keeps the library open, which is what every program did before this existed.
 * The caller learns that from NETSTATUS_OPENERS afterwards and can say which
 * program it was.
 *
 * The caller's own base is skipped, and so is an opener whose task has since
 * exited (its base is on the list for good, and Signal() on a freed Task is a
 * write into whatever holds that memory now).
 *
 * RELEASE gives back the reference NETCTRL_STACK_HOLD took, so the last
 * CloseLibrary() shuts the stack down instead of finding the library holding
 * itself up. Until this existed there was no way to give it back, and the
 * documented sequence was NetShutdown followed by a reboot. It is idempotent,
 * a stack that is not held is not an error, and it refuses when the caller is
 * the only opener left: dropping it there would tear the stack down inside the
 * call rather than at the close, with the caller's own base still live.
 *
 * Neither waits. The grace period belongs to the caller, which is the only one
 * that knows how long it is prepared to wait and is the one that has to report
 * what did not let go.
 */
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
    /*
     * Named out of what was reserved, which is what reserved was for: every
     * caller had to zero it, so no caller that predates this asks for a flag
     * by accident.
     */
    ULONG   nsc_Flags;                  /* NETCTRL_F_*                       */
    /* out: how many the operation acted on. NETCTRL_STACK_NOTIFY's number of
       programs signalled, and named out of the same reserved words. */
    ULONG   nsc_Count;
    ULONG   nsc_Reserved[2];
    /*
     * A host name, for NETCTRL_HOSTNAME_SET, and a field of its own rather than
     * nsc_Name above: that one is 24 bytes because a DNS-SD service type is
     * "<sn>._tcp" with <sn> up to 15 characters, and a host name is up to 63
     * (RFC 1123 2.1, one label), so putting one in the other would cap a host
     * name at a width that has nothing to do with host names.
     *
     * At the end of the block, and the block therefore grows. That is why
     * AMI_NETSTATUS_VERSION moved: the library refuses any version but its own
     * and checks the caller's size against its own sizeof, so a caller built
     * against the shorter block cannot reach this and cannot be misread as
     * having filled it.
     */
    char    nsc_HostName[NETSTATUS_HOSTNAME_LEN];
} NetStatusControl;

#ifdef __cplusplus
}
#endif

#endif /* AMINETXDUO_NETSTATUS_H */

/*
 * AmiNetXDuo, asking the running stack what it is doing.
 *
 *   The whole ThreadX/NetX Duo stack is a singleton inside bsdsocket.library's
 *   segment. A Shell command that links libnetxduo.a gets a second set of NetX
 *   Duo globals, a second NX_IP with no interfaces and a ThreadX kernel that
 *   was never entered, so every question it asks is answered by the wrong
 *   stack, and every NetX Duo call that suspends "the calling thread" reaches
 *   for a scheduler that is not running.
 *
 *   src/tools/netstack_weak.c supplies weak netstack_get()/netstack_ip() stubs
 *   that return NULL, and no command links src/netstack, so in every shipped
 *   build `netstat`, `ping` and ShowNetStatus's live path read NULL and print
 *   "the network is up, but this command cannot read it", which reads like a
 *   pass. See docs/RESEARCH.md 19.6 and 21.
 *
 *   src/tools/nettrace.c already solved this: it reaches the capture engine
 *   through the eight published bpf_* LVOs rather than by linking src/bpf/,
 *   "because a tool that linked the archive would get its OWN copy of the
 *   channel table and capture nothing at all". This header is the same answer
 *   for the rest of the stack.
 *
 *   It returns a snapshot, not a pointer. AmigaOS has no memory protection, so
 *   a live NX_IP * into another task's structures stays dereferenceable long
 *   after the stack has gone down, and this project has already shipped one
 *   use-after-free of that kind (a teardown path that freed a reply port and
 *   the stack a thread was still running on). Walking NetX Duo's tables also
 *   takes the ThreadX baton, which a Shell command must not hold while it
 *   prints.
 *
 *   So the library copies. NetStackQuery() acquires the baton, fills the caller's
 *   buffer with plain scalars, and releases the baton before returning. Nothing
 *   the caller holds afterwards points into the stack.
 *
 *   These two slots sit past every offset any published bsdsocket ABI names,
 *   past AmiTCP V3, past AmiTCP V4, past Roadshow's extension set and past the
 *   six reserved-for-expansion slots that clib/bsdsocket_protos.h documents
 *   after getnameinfo(). The only way to reach them is on purpose. But if some
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
 * 8 since NETCTRL_INTERFACE_CONFIGURE. A caller and a library that disagree
 * fail every call rather than half of them, which is why the commands and the
 * library ship together.
 *
 * This is the compatibility mechanism for a record that grows. The size check in
 * bsd_NetStackQuery() is not: it rejects a buffer too small for the record, and
 * a caller that agrees on the version agrees on the record, so a matched caller
 * never meets it. It is there for the arrival that agreed on nothing.
 *
 * THE NUMBER AND THE SENTENCE ABOVE IT MOVE TOGETHER. Adding an operation
 * without moving this leaves two different header shapes both claiming the same
 * version, and the checks at src/bsdsocket/netstatus.c:1193 and :1407 are exact
 * equality in both directions, so they cannot tell them apart. That has already
 * happened once: NETCTRL_INTERFACE_ADD and NETCTRL_STACK_HOLD were both added
 * under 7, and the comment here still said 6 while the constant said 7.
 */
#define AMI_NETSTATUS_VERSION       8

/* Fixed widths every record shares.  Up here rather than beside the first
   record that uses one, because NetStatusSystem needs NETSTATUS_NAME_LEN and
   is declared before the interface table. */
#define NETSTATUS_NAME_LEN      32
#define NETSTATUS_DEVICE_LEN    32
#define NETSTATUS_MAC_SIZE      6

/*
 * The DNS-SD widths, from RFC 6763 and the mDNS module's own limits: a service
 * type is "<sn>._tcp" with <sn> up to 15 characters, an instance name is one
 * DNS label, a target host is a name, and a TXT record is a set of key=value
 * strings the module hands back semicolon-separated.
 *
 * NETSTATUS_SVC_TXT_LEN is shorter than a TXT record may legally be. A record
 * past it is truncated rather than dropped, and NETSTATUS_SVC_TXTCUT says so,
 * because a printer that publishes 400 bytes of options is still a printer and
 * the interesting keys are at the front.
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
 * table, where MakeLibrary() put the (APTR)-1 terminator. That is a guru, which
 * is a worse answer than the message this interface exists to stop printing.
 *
 * Bump this when a *caller of this interface* needs a newer library: when a
 * netstatus vector is added, or when AMI_NETSTATUS_VERSION moves. Not merely
 * because BSD_LIB_REVISION did: a revision that adds vectors no netstatus
 * caller touches still answers everything here, and refusing it would be a
 * wrong diagnosis.
 *
 * 4 because AMI_NETSTATUS_VERSION is 8 and revision 4 is the first library
 * that speaks it. A revision-3 library answers 7, which the exact-equality
 * check below refuses on every call; without this the refusal arrives as
 * EINVAL from whichever call happened to be first, and reads like the feature
 * being absent rather than like half an install.
 *
 * The version check inside the library catches a mismatched pair too, but only
 * after the call, where it is indistinguishable from the feature being absent
 * ShowNetServices read it as "this library has no mDNS" and told the reader
 * to stop looking. This check runs before any call and says the true thing:
 * finish the install.
 */
#define AMI_NETSTATUS_MIN_REVISION  4

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

/*
 * Every buffer starts with this. The caller fills nsh_Magic and nsh_Version;
 * the library fills the rest and writes as many entries after it as will fit,
 * reporting in nsh_Available how many it had. Truncation is therefore
 * detectable rather than silent: nsh_Count < nsh_Available.
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
 * empty; set with an empty field means the responder is there but has not
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
     * AmiHostnameSource (aminetxduo/config.h). A name is not self-explaining
     * a remnant ENV:HOSTNAME kept a renamed machine answering to its old
     * name and nothing said so, and this is the only field that says.
     *
     * Zero is AMI_HOSTNAME_NONE and means either that nothing named the
     * machine or that the library predates the field, so a reader reports
     * "not stated" rather than guessing. It is taken out of nss_Reserved,
     * which every library has always zeroed, so no version and no minimum
     * revision move for it.
     */
    ULONG   nss_HostSource;
    ULONG   nss_Reserved[2];
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
    /* The four causes behind nsi_RxErrors, which sum to it. */
    ULONG   nsi_RxErrRunt;
    ULONG   nsi_RxErrVerify;
    ULONG   nsi_RxErrLength;
    ULONG   nsi_RxErrIo;
    /* Whether the driver uses our copy hook, and whether it could sum. */
    ULONG   nsi_RxCopyHook;
    ULONG   nsi_RxCopySummed;
} NetStatusInterface;

/* ----------------------------------------------- NETSTATUS_ADDRESSES6 --- */

/*
 * The IPv6 addresses each interface holds, one entry per address, in the
 * order NetX Duo keeps them on the interface's own list.  An interface with
 * IPv6 running always has at least its fe80::/64 link-local address; a global
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

typedef struct NetStatusAddress6
{
    UWORD   nsn_Interface;              /* NX_IP interface index             */
    UWORD   nsn_State;                  /* NETSTATUS_IP6_*                   */
    ULONG   nsn_Address[4];             /* host byte order, four words       */
    ULONG   nsn_PrefixLength;
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
    UWORD   nsd_Pad;

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
 * task's own accounting and the baton bracket's counters; neither touches NetX
 * Duo, so this selector answers with the stack up or down.  The memory half is
 * AmiMemStats (aminetxduo/compat.h), which the published health mark points at
 * as well, so `netstat -h` on a wedged machine and this call report the same
 * record.
 *
 * The memory half is what makes a suspected leak answerable.  AvailMem falls
 * for every program on the machine; nsl_AllocLive is ours alone, nsl_Sockets
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
 * nsl_TickWorstStallMs large next to nsl_TickWorstServiceUs small says the
 * tick task was not dispatched, not that it was slow.  nsl_BatonMoved or
 * nsl_BatonFull non-zero says the bracket lost track of a thread.
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
 * way, so this is a measure of how late timers are running and of nothing
 * else. nsl_TickSkewPeak is sampled before a backlog is worked off, so it moves
 * on a machine where nothing was ever lost.
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
 * The state is the part worth reporting.  An ARP entry is resolved or it is
 * not; a neighbour entry says how the stack currently believes the address
 * behaves, and the five states separate "nobody has answered" from "it
 * answered once and we have not checked since" from "we are checking now".
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
 * A stateless-autoconfigured address is deliberately NOT reported from its own
 * prefix: a router advertisement may set A without L, in which case the address
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

/* ------------------------------------------------- NETSTATUS_SERVICES,
 *
 * What a browse has heard so far. This is a read of the mDNS cache:
 * NETCTRL_MDNS_BROWSE starts the query, answers arrive on the responder's own
 * thread over the following seconds, and this selector says what has landed by
 * the time it is called. Calling it twice gives two different answers, and
 * neither is "everything on the network", mDNS has no end of results.
 *
 * One thing it does put on the wire: a service whose SRV record arrived without
 * an address record beside it has its target resolved here, because a row with
 * a host name and no address is a service that cannot be used. That is the only
 * reason this selector can take a moment, and it is bounded at two seconds
 * however many such rows the cache holds.
 *
 * It answers with the WHOLE cache, of every type, and not with the type the
 * caller last browsed for: one cache, any number of readers, so a filter here
 * would depend on who else was running. Match on nsv_Type.
 *
 * Empty on a build without AMINETXDUO_MDNS, which NETSTATUS_SYS_MDNS reports.
 */

/* nsv_Flags */
/*
 * Clear means this row is a service TYPE and nothing more: the answer came
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

/* ------------------------------------------------------------- control,
 *
 * NetStackControl() is the mutating half, on a separate LVO from the reading
 * half so that a caller which only reads cannot get one of these by mistyping a
 * selector.
 *
 * Every operation takes the same argument block. Which fields matter is stated
 * per operation below; the rest must be zero, so that giving one a meaning
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
 * holds NX_IP_ROUTING_TABLE_SIZE entries; ENOBUFS past that.
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
 * fe80::/64 exists on every interface, so the address alone does not say which
 * and ignored for a prefix, which is a property of the machine's whole
 * prefix list rather than of one interface.
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
 * stack is up, so it is not optional; the query also occupies the peer cache
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
 * TCP connections routed out of it; NETCTRL_F_FORCE overrides, and every such
 * connection is reset. An index is a handle a caller may hold, so removing one
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
 * EIO when the SANA-II device would not open or would not answer.
 *
 * The address is not waited for. A lease takes seconds to arrive and the
 * caller is the one with a Process to wait in; read NETSTATUS_INTERFACES until
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
 * holds a reference of its own and the caller may CloseLibrary() normally. It
 * is idempotent and costs nothing on the second call, so a command may ask
 * every time without accumulating anything.
 *
 * There is no matching release. The reference is permanent for the life of the
 * library, which is what a machine whose interfaces came up at boot wants; an
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
 * a caller may legitimately ask for, so it cannot double as "leave this one
 * alone".
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
 * applied by a call that fails on the address; a call that sets the address and
 * then fails on the gateway reports the gateway and keeps the address, which is
 * the half that was asked for first and the half a caller can see.
 */
#define NETCTRL_INTERFACE_CONFIGURE 19 /* nsc_Index/Destination/NetMask/Gateway */

/* Flags for nsc_Flags. Zero unless an operation above says otherwise. */
#define NETCTRL_F_FORCE         0x00000001
/* Which of NETCTRL_INTERFACE_CONFIGURE's three fields were given at all. */
#define NETCTRL_F_ADDRESS       0x00000002
#define NETCTRL_F_NETMASK       0x00000004
#define NETCTRL_F_GATEWAY       0x00000008

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
     * by accident. The block is the size it always was.
     */
    ULONG   nsc_Flags;                  /* NETCTRL_F_*                       */
    ULONG   nsc_Reserved[3];
} NetStatusControl;

#ifdef __cplusplus
}
#endif

#endif /* AMINETXDUO_NETSTATUS_H */

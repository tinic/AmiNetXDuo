/*
 * bsdsocket.library -- the Roadshow interface API.
 *
 *   ObtainInterfaceList()       the names of the interfaces the stack has
 *   ReleaseInterfaceList()
 *   QueryInterfaceTagList()     everything else, one IFQ_* tag at a time
 *   ConfigureInterfaceTagList() address, mask, MTU and up/down
 *
 * WRITTEN FROM THE AUTODOC, NOT FROM THE NAMES
 *
 * The contract implemented here is Olaf Barthel's, from the bsdsocket.library
 * autodoc that ships in NDK 3.2 (SANA+RoadshowTCP-IP/doc/bsdsocket.doc), read
 * together with libraries/bsdsocket.h from the same NDK. Both are freely
 * distributable and are used here as an ABI reference only; no Roadshow,
 * AmiTCP, AROSTCP or Miami code was consulted, and none of it is present.
 * Where the contract is not obvious from the name, the autodoc's own words
 * are quoted below, because guessing an ABI is how this project lost time
 * twice already (see the header of roadshow.c).
 *
 * FOUR THINGS THE DOCUMENT SETTLED THAT WOULD OTHERWISE HAVE BEEN GUESSES
 *
 *   1. ObtainInterfaceList() returns a list of plain Nodes carrying NOTHING
 *      but a name: "Pointer to a 'struct List', whose individual Nodes
 *      contain the names of the respective interfaces (found in
 *      node->ln_Name)". Not a Roadshow-private node type with fields after
 *      it. A caller walks it with ln_Name and nothing else, so that is all
 *      that is published here.
 *
 *   2. Every IFQ_* tag's ti_Data is a POINTER TO CALLER STORAGE, never the
 *      value. The autodoc types each tag as "(LONG *)", "(ULONG *)",
 *      "(struct sockaddr *)", "(SBQUAD_T *)" and so on, which is the half
 *      that libraries/bsdsocket.h alone does not say.
 *
 *   3. "0 for success, -1 for failure; the failure code will be stored in the
 *      'errno' variable" -- so this is a bsd_fail() call site and not a BOOL
 *      one. RemoveInterface(), one page further on, is the opposite (TRUE for
 *      success), which is exactly the kind of inconsistency that cannot be
 *      inferred.
 *
 *   4. IFC_State takes FOUR values where IFQ_State returns two, and the extra
 *      pair means something specific: SM_Online is "SM_Up, but send S2_ONLINE
 *      to the device first, and if that fails do nothing else". One block of
 *      #defines in libraries/bsdsocket.h, two different vocabularies.
 *
 * WHAT IS NOT ANSWERED, AND WHY
 *
 * On the QUERY side, a tag this stack has no true value for is LEFT ALONE:
 * the caller's storage is not written at all. Writing an invented zero would be indistinguishable
 * from a measured zero, and a monitor that shows "0 packets dropped" when
 * nothing counts drops is worse than one that shows the caller's own default.
 * Each such tag is listed at its case below with the reason.
 *
 * On the CONFIGURE side the same fact produces the opposite behaviour: a tag
 * this stack cannot honour makes the whole call fail with EOPNOTSUPP, and
 * nothing in the list is applied. Silently ignoring a configuration tag would
 * report success for a change that never happened, which is the one answer a
 * configuration call must never give.
 *
 * SPDX-License-Identifier: MIT
 */

#include "bsdsocket_vectors.h"

#include "aminetxduo/config.h"
#include "aminetxduo/sana2.h"

#include "interfaces.h"

#include <net/if.h>
#include <sys/sockio.h>

#include "aminetxduo/bpf.h"   /* AMI_STATIC_ASSERT */

#include <proto/exec.h>

/*
 * How long ConfigureInterfaceTagList() will wait for a name in IFC_Address to
 * resolve. The same thirty seconds resolver.c gives gethostbyname(), because
 * it is the same lookup and a caller cannot tell which one it is waiting for.
 */
#define BSD_IF_RESOLVE_TIMEOUT  (30UL * (ULONG)NX_IP_PERIODIC_RATE)

/* ------------------------------------------------------------- tag storage */

/* The storage a tag points at, or NULL when the caller passed none. */
static APTR bsd_tag_storage(const struct TagItem *item)
{
    return (APTR)item->ti_Data;
}

static VOID bsd_put_long(const struct TagItem *item, LONG value)
{
    LONG *out = (LONG *)bsd_tag_storage(item);

    if (out != NULL)
        *out = value;
}

static VOID bsd_put_ulong(const struct TagItem *item, ULONG value)
{
    ULONG *out = (ULONG *)bsd_tag_storage(item);

    if (out != NULL)
        *out = value;
}

/*
 * IFQ_Address, IFQ_BroadcastAddress and IFQ_DestinationAddress are typed
 * "struct sockaddr *" and IFQ_NetMask "struct sockaddr_in *"; there is one
 * shape behind all four, and no length argument anywhere in the call, so the
 * caller has supplied storage for a whole sockaddr_in and that is what goes
 * in it.
 */
static VOID bsd_put_sockaddr_in(const struct TagItem *item, ULONG host_addr)
{
    struct sockaddr_in *out = (struct sockaddr_in *)bsd_tag_storage(item);

    if (out == NULL)
        return;

    bsd_bzero(out, sizeof(*out));
    out->sin_len            = (UBYTE)sizeof(struct sockaddr_in);
    out->sin_family         = AF_INET;
    out->sin_addr.s_addr    = (in_addr_t)BSD_HTONL(host_addr);
}

/* ---------------------------------------------------------- interface names */

static const AmiIfConfig *bsd_if_config(UINT index)
{
    const AmiConfig *cfg = netstack_config();

    if (cfg == NULL)
        return NULL;

    if (index >= cfg->interface_count || index >= (UINT)AMI_CFG_MAX_INTERFACES)
        return NULL;

    if (!cfg->interfaces[index].configured)
        return NULL;

    return &cfg->interfaces[index];
}

/*
 * The name of physical interface `index`, or FALSE when that slot is not in
 * use. Same rule as netstatus.c: the name from DEVS:NetInterfaces if there is
 * one, otherwise the name NetX Duo gave the slot -- so the two APIs cannot
 * disagree about what an interface is called.
 *
 * The loopback interface is deliberately not here. It lives past the physical
 * slots (NX_LOOPBACK_INTERFACE), has no SANA-II device to answer any of the
 * IFQ_ tags about, and the only name it has is NetX Duo's "Internal IP
 * Loopback" -- twenty characters, where the published API caps a name at
 * fifteen. Handing back a name QueryInterfaceTagList() would then reject is
 * worse than not listing it.
 */
static BOOL bsd_if_name_of(NX_IP *ip, UINT index, char *out, ULONG outlen)
{
    const NX_INTERFACE *nxif = &ip->nx_ip_interface[index];
    const AmiIfConfig  *cfg;

    if (outlen > 0)
        out[0] = '\0';

    if (nxif->nx_interface_valid == 0)
        return FALSE;

    cfg = bsd_if_config(index);

    if (cfg != NULL && cfg->name[0] != '\0')
        bsd_strncpy(out, cfg->name, outlen);
    else
        bsd_strncpy(out, (const char *)nxif->nx_interface_name, outlen);

    return (out[0] != '\0') ? TRUE : FALSE;
}

/*
 * Names are compared without regard to case. They come from file names in
 * DEVS:NetInterfaces, and AmigaDOS file names are case-insensitive, so a user
 * who typed `NetMon ETH0` has named the interface in the file called "eth0".
 */
static BOOL bsd_name_equal(const char *a, const char *b)
{
    ULONG i;

    for (i = 0; ; i++)
    {
        char ca = a[i];
        char cb = b[i];

        if (ca >= 'A' && ca <= 'Z')
            ca = (char)(ca + ('a' - 'A'));
        if (cb >= 'A' && cb <= 'Z')
            cb = (char)(cb + ('a' - 'A'));

        if (ca != cb)
            return FALSE;
        if (ca == '\0')
            return TRUE;
    }
}

/* The physical slot called `name`, or -1. Published in interfaces.h, because
   addralloc.c has to ask the same question under the same naming rule. */
LONG bsd_if_index_of(NX_IP *ip, const char *name)
{
    char  have[BSD_IFNAME_SIZE];
    UINT  i;

    for (i = 0; i < (UINT)NX_MAX_PHYSICAL_INTERFACES; i++)
    {
        if (!bsd_if_name_of(ip, i, have, sizeof(have)))
            continue;

        if (bsd_name_equal(have, name))
            return (LONG)i;
    }

    return -1;
}

/* --------------------------------------------------- ObtainInterfaceList -- */

/*
 * One allocation holds the List, the Nodes and the name strings, so
 * ReleaseInterfaceList() is a single free of the block the list header sits
 * at the top of -- the same arrangement bsd_ObtainDomainNameServerList() uses,
 * and for the same reason: the published free takes only the list pointer.
 */
typedef struct BsdIfList
{
    struct List bil_List;
    struct Node bil_Node[NX_MAX_PHYSICAL_INTERFACES];
    char        bil_Name[NX_MAX_PHYSICAL_INTERFACES][BSD_IFNAME_SIZE];
} BsdIfList;

struct List *bsd_ObtainInterfaceList(
    register struct AmiSocketBase *SocketBase __asm("a6"))
{
    NX_IP     *ip = netstack_ip();
    BsdIfList *out;
    UINT       i;

    if (ip == NULL)
    {
        (VOID)bsd_fail(SocketBase, AMI_ENETDOWN);
        return NULL;
    }

    /*
     * "The result can be NULL if there was not enough memory available to
     * fill it. If no interfaces have been added yet, you will receive an
     * empty list." So an empty list is a success and NULL is reserved for
     * the allocation failure -- the block is allocated before anything is
     * counted, which also keeps the allocation outside the walk below.
     */
    out = (BsdIfList *)ami_alloc(sizeof(BsdIfList));
    if (out == NULL)
    {
        (VOID)bsd_fail(SocketBase, AMI_ENOBUFS);
        return NULL;
    }

    /* NewList(), open-coded: amiga.lib is not available to a shared library. */
    out->bil_List.lh_Head     = (struct Node *)&out->bil_List.lh_Tail;
    out->bil_List.lh_Tail     = NULL;
    out->bil_List.lh_TailPred = (struct Node *)&out->bil_List.lh_Head;
    out->bil_List.lh_Type     = NT_UNKNOWN;

    for (i = 0; i < (UINT)NX_MAX_PHYSICAL_INTERFACES; i++)
    {
        struct Node *node = &out->bil_Node[i];

        if (!bsd_if_name_of(ip, i, out->bil_Name[i], BSD_IFNAME_SIZE))
            continue;

        node->ln_Type = NT_UNKNOWN;
        node->ln_Pri  = 0;
        node->ln_Name = out->bil_Name[i];

        AddTail(&out->bil_List, node);
    }

    return &out->bil_List;
}

VOID bsd_ReleaseInterfaceList(register struct List *list __asm("a0"),
                              register struct AmiSocketBase *SocketBase __asm("a6"))
{
    (VOID)SocketBase;

    /* "This can be NULL in which case this routine will do nothing." */
    if (list != NULL)
        ami_free(list);
}

/* ------------------------------------------------ QueryInterfaceTagList -- */

/*
 * Everything the tag loop can be asked for, gathered in one pass so the
 * ThreadX bracket is held once and briefly. netstatus.c's rule 1 applies:
 * nothing that leaves here is a pointer into the stack -- with one deliberate
 * exception, bii_Device, which IFQ_DeviceName is documented to return as a
 * pointer ("A pointer to the name will be returned") and which points into
 * the AmiConfig inside the netstack singleton. That storage outlives every
 * caller, because the stack is torn down only when the last opener has gone.
 */
typedef struct BsdIfInfo
{
    const char     *bii_Device;         /* NULL when the slot is unconfigured */
    ULONG           bii_Unit;

    ULONG           bii_Address;
    ULONG           bii_NetMask;
    ULONG           bii_Broadcast;
    ULONG           bii_MTU;
    ULONG           bii_HardwareMTU;
    ULONG           bii_BPS;

    BOOL            bii_LinkUp;
    BOOL            bii_HaveSana;
    LONG            bii_BindType;

    UBYTE           bii_HwAddress[AMI_ETH_ADDR_SIZE];

    AmiSana2Stats   bii_Stats;
    AmiSana2Info    bii_Info;

    /* Stack-wide, not per interface -- see the IFQ_IPDrops case. */
    ULONG           bii_IpDrops;
    ULONG           bii_ArpDrops;
    BOOL            bii_HaveIpDrops;
    BOOL            bii_HaveArpDrops;
} BsdIfInfo;

static VOID bsd_if_gather(NX_IP *ip, UINT index, BsdIfInfo *info)
{
    NX_INTERFACE      *nxif = &ip->nx_ip_interface[index];
    const AmiIfConfig *cfg  = bsd_if_config(index);
    AmiSana2If        *sana;
    ULONG              scratch[10];

    bsd_bzero(info, sizeof(*info));

    info->bii_Address = nxif->nx_interface_ip_address;
    info->bii_NetMask = nxif->nx_interface_ip_network_mask;
    info->bii_MTU     = nxif->nx_interface_ip_mtu_size;
    info->bii_LinkUp  = (nxif->nx_interface_link_up != 0) ? TRUE : FALSE;

    /* The all-ones host part of this interface's prefix, which is the only
       broadcast address an interface configured from a mask can have. */
    if (info->bii_NetMask != 0)
        info->bii_Broadcast = (info->bii_Address & info->bii_NetMask) |
                              ~info->bii_NetMask;

    if (cfg != NULL)
    {
        info->bii_Device = cfg->device;
        info->bii_Unit   = cfg->unit;

        /*
         * IFABT_Unknown is documented as "has not been bound or is in
         * transitional state", which is precisely a DHCP interface that has
         * not been given a lease yet, so the address test comes first.
         * LINKLOCAL is RFC 3927 self-assignment -- an automated process whose
         * answer can change on the next collision, which is IFABT_Dynamic by
         * the autodoc's own definition.
         */
        if (info->bii_Address == 0)
            info->bii_BindType = IFABT_Unknown;
        else if (cfg->iptype == AMI_IPTYPE_STATIC)
            info->bii_BindType = IFABT_Static;
        else
            info->bii_BindType = IFABT_Dynamic;
    }
    else
    {
        info->bii_BindType = IFABT_Unknown;
    }

    /* ami_sana2_attach() parks the AmiSana2If in the interface's additional
       link info; that is how the driver's own facts are reached. */
    sana = (AmiSana2If *)nxif->nx_interface_additional_link_info;
    if (sana != NULL)
    {
        info->bii_HaveSana    = TRUE;
        info->bii_HardwareMTU = ami_sana2_get_mtu(sana);
        info->bii_BPS         = ami_sana2_get_bps(sana);

        ami_sana2_get_mac(sana, info->bii_HwAddress);
        ami_sana2_get_stats(sana, &info->bii_Stats);
        ami_sana2_get_info(sana, &info->bii_Info);
    }

    if (nx_ip_info_get(ip, &scratch[0], &scratch[1], &scratch[2], &scratch[3],
                       &scratch[4], &scratch[5], &scratch[6], &scratch[7],
                       &scratch[8], &scratch[9]) == NX_SUCCESS)
    {
        /* ip_receive_packets_dropped + ip_send_packets_dropped: the autodoc
           asks for "the total number of all IP packets dropped" without
           saying which direction, so both. */
        info->bii_IpDrops     = scratch[5] + scratch[7];
        info->bii_HaveIpDrops = TRUE;
    }

    if (nx_arp_info_get(ip, &scratch[0], &scratch[1], &scratch[2], &scratch[3],
                        &scratch[4], &scratch[5], &scratch[6],
                        &scratch[7]) == NX_SUCCESS)
    {
        info->bii_ArpDrops     = scratch[7];    /* arp_invalid_messages */
        info->bii_HaveArpDrops = TRUE;
    }
}

LONG bsd_QueryInterfaceTagList(register STRPTR name __asm("a0"),
                               register struct TagItem *tags __asm("a1"),
                               register struct AmiSocketBase *SocketBase __asm("a6"))
{
    NX_IP          *ip = netstack_ip();
    struct TagItem *cursor;
    struct TagItem *item;
    BsdIfInfo       info;
    LONG            index;

    if (name == NULL)
        return bsd_fail(SocketBase, AMI_EINVAL);

    /* "This name cannot be longer than 15 characters." A longer one cannot
       name an interface, so it is a bad argument rather than a miss. */
    if (bsd_strlen((const char *)name) >= (ULONG)BSD_IFNAME_SIZE)
        return bsd_fail(SocketBase, AMI_EINVAL);

    if (ip == NULL)
        return bsd_fail(SocketBase, AMI_ENETDOWN);

    index = bsd_if_index_of(ip, (const char *)name);
    if (index < 0)
    {
        /*
         * The autodoc gives no errno for "no such interface". ENXIO is what
         * the rest of this library already answers for one (netstatus.c maps
         * NX_INVALID_INTERFACE to it, and 4.4BSD's SIOCGIF* ioctls use it),
         * so a caller sees one code for the condition however it asked.
         */
        return bsd_fail(SocketBase, AMI_ENXIO);
    }

    /* An empty tag list is a legitimate way to ask "does this interface
       exist?" and must not be an error. */
    if (tags == NULL)
        return 0;

    if (bsd_nx_enter(SocketBase) != 0)
        return bsd_fail(SocketBase, AMI_ENETDOWN);

    bsd_if_gather(ip, (UINT)index, &info);

    bsd_nx_leave(SocketBase);

    /* Out of the bracket before a single byte of caller memory is touched:
       the answers are all in `info` now, and writing into an application
       buffer can fault. */
    cursor = tags;
    while ((item = bsd_next_tag(&cursor)) != NULL)
    {
        switch (item->ti_Tag)
        {
            /* -------------------------------------------- the SANA-II half */

            case IFQ_DeviceName:
                /* "A pointer to the name will be returned" -- so ti_Data is
                   a STRPTR *, not a buffer to copy into. */
                if (info.bii_Device != NULL)
                {
                    STRPTR *out = (STRPTR *)bsd_tag_storage(item);

                    if (out != NULL)
                        *out = (STRPTR)info.bii_Device;
                }
                break;

            case IFQ_DeviceUnit:
                if (info.bii_Device != NULL)
                    bsd_put_long(item, (LONG)info.bii_Unit);
                break;

            case IFQ_HardwareAddressSize:
                /* "the number returned will be the number of bits" -- from
                   S2_DEVICEQUERY, not from assuming Ethernet. */
                if (info.bii_HaveSana)
                    bsd_put_long(item, (LONG)info.bii_Info.address_bits);
                break;

            case IFQ_HardwareAddress:
                /* "these are bytes, not a NUL-terminated string", and at most
                   sixteen of them. Six is what an Ethernet address is and six
                   is what the shim keeps. */
                if (info.bii_HaveSana)
                {
                    UBYTE *out = (UBYTE *)bsd_tag_storage(item);

                    if (out != NULL)
                        bsd_bcopy(info.bii_HwAddress, out, AMI_ETH_ADDR_SIZE);
                }
                break;

            case IFQ_HardwareType:
                if (info.bii_HaveSana)
                    bsd_put_long(item, (LONG)info.bii_Info.hardware_type);
                break;

            case IFQ_BPS:
                if (info.bii_HaveSana)
                    bsd_put_long(item, (LONG)info.bii_BPS);
                break;

            case IFQ_MTU:
                bsd_put_long(item, (LONG)info.bii_MTU);
                break;

            case IFQ_HardwareMTU:
                /*
                 * The autodoc adds that this "will also magically update the
                 * MTU size used by the TCP/IP stack". There is nothing to
                 * update: this stack takes the driver's MTU at attach time
                 * and never runs with a different one, so the two numbers are
                 * the same number and the side effect is a no-op rather than
                 * something skipped.
                 */
                if (info.bii_HaveSana)
                    bsd_put_long(item, (LONG)info.bii_HardwareMTU);
                break;

            /* ------------------------------------------------- the counters */

            case IFQ_PacketsReceived:
                if (info.bii_HaveSana)
                    bsd_put_ulong(item, info.bii_Stats.packets_received);
                break;

            case IFQ_PacketsSent:
                if (info.bii_HaveSana)
                    bsd_put_ulong(item, info.bii_Stats.packets_sent);
                break;

            case IFQ_BadData:
                if (info.bii_HaveSana)
                    bsd_put_ulong(item, info.bii_Stats.bad_data);
                break;

            case IFQ_Overruns:
                if (info.bii_HaveSana)
                    bsd_put_ulong(item, info.bii_Stats.overruns);
                break;

            case IFQ_UnknownTypes:
                if (info.bii_HaveSana)
                    bsd_put_ulong(item, info.bii_Stats.unknown_types);
                break;

            case IFQ_InputErrors:
                if (info.bii_HaveSana)
                    bsd_put_long(item, (LONG)info.bii_Stats.rx_errors);
                break;

            case IFQ_OutputErrors:
                if (info.bii_HaveSana)
                    bsd_put_long(item, (LONG)info.bii_Stats.tx_errors);
                break;

            case IFQ_InputDrops:
                /* "received, but were dropped before they could be
                   processed" -- which is exactly a failed NX_PACKET
                   allocation on the receive path. */
                if (info.bii_HaveSana)
                    bsd_put_long(item, (LONG)info.bii_Stats.alloc_failures);
                break;

            case IFQ_IPDrops:
                if (info.bii_HaveIpDrops)
                    bsd_put_long(item, (LONG)info.bii_IpDrops);
                break;

            case IFQ_ARPDrops:
                if (info.bii_HaveArpDrops)
                    bsd_put_long(item, (LONG)info.bii_ArpDrops);
                break;

            /* ---------------------------------------------- the I/O requests */

            case IFQ_NumReadRequests:
                if (info.bii_HaveSana)
                    bsd_put_long(item, (LONG)info.bii_Info.read_requests);
                break;

            case IFQ_NumReadRequestsPending:
                if (info.bii_HaveSana)
                    bsd_put_long(item, (LONG)info.bii_Info.read_pending);
                break;

            case IFQ_NumWriteRequests:
                if (info.bii_HaveSana)
                    bsd_put_long(item, (LONG)info.bii_Info.write_requests);
                break;

            case IFQ_NumWriteRequestsPending:
                if (info.bii_HaveSana)
                    bsd_put_long(item, (LONG)info.bii_Info.write_pending);
                break;

            /* --------------------------------------------------- the addresses */

            case IFQ_Address:
                bsd_put_sockaddr_in(item, info.bii_Address);
                break;

            case IFQ_NetMask:
                bsd_put_sockaddr_in(item, info.bii_NetMask);
                break;

            case IFQ_BroadcastAddress:
                bsd_put_sockaddr_in(item, info.bii_Broadcast);
                break;

            case IFQ_PrimaryDNSAddress:
            case IFQ_SecondaryDNSAddress:
                /*
                 * "Try to obtain the address ... if known to this interface.
                 * If the address is not known, then the IP address filled in
                 * by this tag will be zero." These are the addresses a PPP
                 * peer hands over through the SANA-IIR4 extensions; nothing
                 * this stack drives supplies them per interface, so zero is
                 * the documented answer rather than a gap. The resolver's own
                 * name servers are NOT put here: they are stack-wide, and
                 * reporting them as belonging to one interface would be a
                 * different fact with the same shape.
                 */
                bsd_put_sockaddr_in(item, 0);
                break;

            /* -------------------------------------------------------- state */

            case IFQ_State:
                /* "the values returned can be either 'SM_Down' or 'SM_Up'" --
                   SM_Online/SM_Offline are IFC_State's, not this tag's. */
                bsd_put_long(item, info.bii_LinkUp ? SM_Up : SM_Down);
                break;

            case IFQ_AddressBindType:
                bsd_put_long(item, info.bii_BindType);
                break;

            case IFQ_Metric:
                /* Zero, and true rather than invented: every interface here
                   is a directly attached one and this stack has no routing
                   protocol to give any of them a cost. */
                bsd_put_long(item, 0);
                break;

            case IFQ_GetDebugMode:
                /* Likewise: there is no per-interface debug mode to be in. */
                bsd_put_long(item, FALSE);
                break;

            /*
             * ---------------------------------------------------------------
             * LEFT ALONE ON PURPOSE. Each of these has a well-defined meaning
             * that this stack keeps no true value for, and the caller's
             * storage is better left holding the caller's own default than an
             * invented zero that reads like a measurement.
             *
             *   IFQ_LastStart               nothing records when an interface
             *                               was last brought online.
             *   IFQ_DestinationAddress      the point-to-point partner. There
             *                               are no point-to-point interfaces
             *                               here; the SANA-II shim is
             *                               Ethernet-shaped throughout.
             *   IFQ_AddressLeaseExpires     the DHCP client does not surface
             *                               the lease deadline, and all-zero
             *                               is documented to mean "lasts
             *                               infinitely" -- the one wrong
             *                               answer for a leased address.
             *   IFQ_GetBytesIn/Out          no byte counters exist per
             *                               interface. The IP-level totals
             *                               are stack-wide and would be a
             *                               different number on a machine
             *                               with two cards.
             *   IFQ_OutputDrops             transmit failures are counted as
             *                               errors, not split into dropped
             *                               and failed.
             *   IFQ_InputMulticasts,
             *   IFQ_OutputMulticasts        multicast frames are not counted
             *                               separately from the rest.
             *   IFQ_GetSANA2CopyStats       the copy hooks in sana2_copy.c
             *                               are not instrumented.
             *   IFQ_MaxReadRequests,
             *   IFQ_MaxWriteRequests        the autodoc types these two
             *                               "(LONG)" where every one of their
             *                               neighbours is "(LONG *)". On a
             *                               query that has no other way to
             *                               return anything, a bare LONG can
             *                               only be a typo -- but writing
             *                               through a ti_Data the caller
             *                               passed as a scalar would corrupt
             *                               its memory, so this one stays
             *                               unanswered until the ambiguity
             *                               does not matter.
             * ---------------------------------------------------------------
             */

            default:
                /* Every other tag, including anybody's private ones: ignored,
                   which is what a tag list is for. */
                break;
        }
    }

    return 0;
}

/* --------------------------------------------- ConfigureInterfaceTagList -- */

/*
 * TWO PASSES, AND THE REASON IS THE FAILURE MODE
 *
 * The autodoc says nothing about what happens to a tag list whose fourth tag
 * is refused. Applying tags as they are read would leave the interface half
 * configured -- new address, old mask, still down -- which is the one state
 * from which a user cannot tell what went wrong. So the whole list is parsed
 * and validated first, and NOTHING is applied unless all of it can be:
 * a refused call leaves the interface exactly as it was.
 *
 * It also has to be that way for correctness rather than only for tidiness.
 * IFC_Address and IFC_NetMask arrive as two tags and NetX Duo changes both in
 * one call; applying them separately would put a mismatched pair on the
 * interface for as long as it took to read the next tag.
 *
 * Resolving a host name blocks and allocating nothing may happen inside the
 * ThreadX bracket, so pass one runs entirely outside it and pass two holds it
 * only for the address change.
 */
typedef struct BsdIfConfigReq
{
    BOOL    bcr_HaveAddress;
    ULONG   bcr_Address;
    BOOL    bcr_HaveNetMask;
    ULONG   bcr_NetMask;
    BOOL    bcr_HaveMTU;
    ULONG   bcr_MTU;
    BOOL    bcr_HaveState;
    LONG    bcr_State;
} BsdIfConfigReq;

/*
 * "a NUL-terminated string which can hold a host name to be resolved or an IP
 * address in dotted-decimal notation (per RFC1700)". Dotted-quad first,
 * because a machine being configured may well have no working resolver yet --
 * and because a name that happens to parse as an address must not be sent to
 * a name server.
 */
static BOOL bsd_if_parse_address(const char *text, ULONG *out)
{
    if (text == NULL || text[0] == '\0')
        return FALSE;

    if (ami_config_parse_ip(text, out))
        return TRUE;

    /* netstack_resolve() consults DEVS:Internet/hosts before the network, so
       a host named there resolves with no interface up at all. */
    return (netstack_resolve(text, out, BSD_IF_RESOLVE_TIMEOUT) == AMI_NET_OK)
               ? TRUE : FALSE;
}

/*
 * The mask for an address that arrived without one. The classful default is
 * not a good netmask in 2026, but it is the one an address alone implies, and
 * the alternative -- refusing IFC_Address unless IFC_NetMask came with it --
 * would reject a tag list the published API says is legal.
 */
static ULONG bsd_if_classful_mask(ULONG addr)
{
    if ((addr & 0x80000000UL) == 0)
        return 0xFF000000UL;                    /* class A */
    if ((addr & 0xC0000000UL) == 0x80000000UL)
        return 0xFFFF0000UL;                    /* class B */

    return 0xFFFFFF00UL;                        /* class C and everything else */
}

/*
 * Pass one. Returns 0, or -1 with errno set and *req untouched from the
 * caller's point of view -- nothing has been applied yet either way.
 */
static LONG bsd_if_parse_config(struct AmiSocketBase *SocketBase,
                                struct TagItem *tags, BsdIfConfigReq *req)
{
    struct TagItem *cursor = tags;
    struct TagItem *item;

    bsd_bzero(req, sizeof(*req));

    while ((item = bsd_next_tag(&cursor)) != NULL)
    {
        const char *text = (const char *)item->ti_Data;

        switch (item->ti_Tag)
        {
            case IFC_Address:
                if (!bsd_if_parse_address(text, &req->bcr_Address))
                    return bsd_fail(SocketBase, AMI_EINVAL);
                req->bcr_HaveAddress = TRUE;
                break;

            case IFC_NetMask:
                /* "this must be a NUL-terminated string" and nothing else --
                   a mask is not a host name, so no resolver here. */
                if (text == NULL ||
                    !ami_config_parse_ip(text, &req->bcr_NetMask))
                    return bsd_fail(SocketBase, AMI_EINVAL);
                req->bcr_HaveNetMask = TRUE;
                break;

            case IFC_LimitMTU:
                if (item->ti_Data == 0)
                    return bsd_fail(SocketBase, AMI_EINVAL);
                req->bcr_MTU     = (ULONG)item->ti_Data;
                req->bcr_HaveMTU = TRUE;
                break;

            case IFC_State:
                switch ((LONG)item->ti_Data)
                {
                    case SM_Up:
                    case SM_Down:
                    case SM_Online:
                    case SM_Offline:
                        req->bcr_State     = (LONG)item->ti_Data;
                        req->bcr_HaveState = TRUE;
                        break;

                    default:
                        return bsd_fail(SocketBase, AMI_EINVAL);
                }
                break;

            case IFC_Complete:
                /*
                 * "Indicate that the configuration for this interface is now
                 * complete. This has the effect of causing the default route
                 * configuration file to be read and processed for the first
                 * time."
                 *
                 * Accepted, and a no-op, which is the truthful answer rather
                 * than a shrug: this stack reads the whole of
                 * DEVS:NetInterfaces and DEVS:Internet at startup and defers
                 * nothing, so there is no first time left to cause.
                 */
                break;

            case IFC_SetDebugMode:
                /* There is no per-interface debug mode, so turning it off is
                   something this stack can honestly do and turning it on is
                   not. IFQ_GetDebugMode answers FALSE for the same reason. */
                if (item->ti_Data != 0)
                    return bsd_fail(SocketBase, AMI_EOPNOTSUPP);
                break;

            /*
             * ---------------------------------------------------------------
             * REFUSED, each for a reason that is a property of this stack
             * rather than of the caller. EOPNOTSUPP and not ENOSYS: the
             * vector exists and works, this particular thing does not exist
             * to be configured.
             *
             *   IFC_DestinationAddress  a point-to-point partner. The SANA-II
             *   IFC_GetPeerAddress      shim is Ethernet-shaped throughout and
             *                           these need SANA-IIR4 besides, which
             *                           the autodoc itself warns about.
             *   IFC_GetDNS              same.
             *   IFC_BroadcastAddress    NetX Duo derives the broadcast address
             *                           from the address and the mask; there
             *                           is no separate one to set.
             *   IFC_AddAliasAddress     one IPv4 address per interface.
             *   IFC_DeleteAliasAddress
             *   IFC_Metric              no routing protocol, so no cost for
             *                           one to carry.
             *   IFC_AssociatedRoute     these mark an interface so that
             *   IFC_AssociatedDNS       going down tears something else down
             *                           with it. Accepting the mark without
             *                           the teardown would be a flag nothing
             *                           reads.
             *   IFC_ReleaseAddress      the DHCP client has no release path;
             *                           accepting this would leave the lease
             *                           held on the server.
             * ---------------------------------------------------------------
             */
            case IFC_DestinationAddress:
            case IFC_BroadcastAddress:
            case IFC_Metric:
            case IFC_AddAliasAddress:
            case IFC_DeleteAliasAddress:
            case IFC_GetPeerAddress:
            case IFC_GetDNS:
            case IFC_AssociatedRoute:
            case IFC_AssociatedDNS:
            case IFC_ReleaseAddress:
                return bsd_fail(SocketBase, AMI_EOPNOTSUPP);

            default:
                /* Anything this API never defined, including private tags:
                   ignored, as a tag list requires. */
                break;
        }
    }

    return 0;
}

LONG bsd_ConfigureInterfaceTagList(register STRPTR name __asm("a0"),
                                   register struct TagItem *tags __asm("a1"),
                                   register struct AmiSocketBase *SocketBase __asm("a6"))
{
    NX_IP          *ip = netstack_ip();
    BsdIfConfigReq  req;
    NX_INTERFACE   *nxif;
    AmiSana2If     *sana;
    LONG            index;
    UINT            status;

    if (name == NULL)
        return bsd_fail(SocketBase, AMI_EINVAL);

    if (bsd_strlen((const char *)name) >= (ULONG)BSD_IFNAME_SIZE)
        return bsd_fail(SocketBase, AMI_EINVAL);

    if (ip == NULL)
        return bsd_fail(SocketBase, AMI_ENETDOWN);

    index = bsd_if_index_of(ip, (const char *)name);
    if (index < 0)
        return bsd_fail(SocketBase, AMI_ENXIO);

    /* An empty list is a legal no-op, same as it is for the query. */
    if (tags == NULL)
        return 0;

    if (bsd_if_parse_config(SocketBase, tags, &req) != 0)
        return -1;

    nxif = &ip->nx_ip_interface[index];
    sana = (AmiSana2If *)nxif->nx_interface_additional_link_info;

    /* --- pass two, in the order a configuration has to happen in --------- */

    if (req.bcr_HaveMTU)
    {
        /*
         * "Before the maximum transmission unit size is limited, the hardware
         * MTU settings will be reread and taken into account." The driver's
         * MTU is a fact read at open time and it does not change, so taking it
         * into account means clamping to it: this tag can only make the MTU
         * smaller, and a request for more than the hardware can carry becomes
         * the hardware's own number rather than an error.
         */
        ULONG limit = req.bcr_MTU;
        ULONG hardware = (sana != NULL) ? ami_sana2_get_mtu(sana) : 0;

        if (hardware != 0 && limit > hardware)
            limit = hardware;

        if (bsd_nx_enter(SocketBase) != 0)
            return bsd_fail(SocketBase, AMI_ENETDOWN);

        status = nx_ip_interface_mtu_set(ip, (UINT)index, limit);

        bsd_nx_leave(SocketBase);

        if (status != NX_SUCCESS)
            return bsd_fail(SocketBase, AMI_EINVAL);
    }

    if (req.bcr_HaveAddress || req.bcr_HaveNetMask)
    {
        ULONG address = req.bcr_HaveAddress ? req.bcr_Address
                                            : nxif->nx_interface_ip_address;
        ULONG mask    = req.bcr_HaveNetMask ? req.bcr_NetMask
                                            : nxif->nx_interface_ip_network_mask;

        if (mask == 0)
            mask = bsd_if_classful_mask(address);

        if (bsd_nx_enter(SocketBase) != 0)
            return bsd_fail(SocketBase, AMI_ENETDOWN);

        status = nx_ip_interface_address_set(ip, (UINT)index, address, mask);

        bsd_nx_leave(SocketBase);

        if (status != NX_SUCCESS)
            return bsd_fail(SocketBase, AMI_EADDRNOTAVAIL);
    }

    if (req.bcr_HaveState)
    {
        LONG rc;

        /*
         * Last, so that {IFC_Address, IFC_State SM_Up} does what it reads
         * like. netstack_interface_up()/down() take the ThreadX bracket
         * themselves and stop the SANA-II readers as well as telling NetX Duo,
         * so they are called OUTSIDE ours -- the same rule netstatus.c
         * follows for NETCTRL_INTERFACE_UP.
         *
         * SM_Up and SM_Online, and SM_Down and SM_Offline, do the same thing
         * here, and that is not a shortcut. The autodoc's distinction is
         * whether the SANA-II device is told S2_ONLINE/S2_OFFLINE as well as
         * the stack; this driver's NX_LINK_ENABLE already issues S2_ONLINE and
         * starts the readers, and NX_LINK_DISABLE issues S2_OFFLINE. There is
         * no way to move the stack's view without moving the device's, so the
         * two spellings describe one transition.
         */
        if (req.bcr_State == SM_Up || req.bcr_State == SM_Online)
            rc = netstack_interface_up((UWORD)index);
        else
            rc = netstack_interface_down((UWORD)index);

        if (rc != AMI_NET_OK)
            return bsd_fail(SocketBase, AMI_ENXIO);
    }

    return 0;
}

/* ------------------------------------------------ AddInterfaceTagList --- */

/*
 * "This function makes another device available for network access. Each such
 * device must be assigned a unique interface name and refer to a SANA-II
 * device name and unit number."
 *
 * The work is netstack_interface_add()'s, and it lives there rather than here
 * for a reason worth stating: half of it is NetX Duo's (attach an interface
 * to a running NX_IP) and half is the netstack's (open the SANA-II device,
 * register it for capture, take a configuration slot). An interface that got
 * only the first half would be invisible to netstack_shutdown() and its
 * device would never be closed.
 *
 * THE TAGS THAT ARE HONOURED are the ones that describe something this stack
 * has. Every one that does not is refused, for the same reason
 * ConfigureInterfaceTagList() refuses its own: an interface brought up with
 * a packet filter mode or a read-request count that was quietly ignored is
 * not the interface the caller asked for, and it would take a packet capture
 * to find out.
 */
static LONG bsd_if_parse_add(struct AmiSocketBase *SocketBase,
                             struct TagItem *tags, AmiIfConfig *cfg)
{
    struct TagItem *cursor = tags;
    struct TagItem *item;

    while ((item = bsd_next_tag(&cursor)) != NULL)
    {
        switch (item->ti_Tag)
        {
            case IFA_LimitMTU:
                /* Applied after the attach, once the driver's own MTU is
                   known -- "you can request that a smaller size is used". */
                if (item->ti_Data == 0)
                    return bsd_fail(SocketBase, AMI_EINVAL);
                cfg->mtu = (ULONG)item->ti_Data;
                break;

            case IFA_SetDebugMode:
                /* There is no debug mode, so turning it off is something this
                   stack can honestly do and turning it on is not. */
                if (item->ti_Data != 0)
                    return bsd_fail(SocketBase, AMI_EOPNOTSUPP);
                break;

            case IFA_Multicast:
            case IFA_PointToPoint:
                /*
                 * "Not normally necessary since the stack can figure this out
                 * all by itself" -- and it does: the SANA-II shim reads the
                 * wire type from S2_DEVICEQUERY. Accepted when it agrees with
                 * what the hardware says, which for every device this stack
                 * drives means multicast TRUE and point-to-point FALSE.
                 */
                if ((item->ti_Tag == IFA_Multicast) != (item->ti_Data != 0))
                    return bsd_fail(SocketBase, AMI_EOPNOTSUPP);
                break;

            /*
             * -----------------------------------------------------------
             * REFUSED, each because the thing it configures does not exist
             * here rather than because the caller got it wrong:
             *
             *   IFA_IPType, IFA_ARPType     the shim's EtherTypes are the
             *                               RFC 894 ones and are not
             *                               configurable.
             *   IFA_NumReadRequests         the RX depth is computed from the
             *   IFA_NumWriteRequests        packet pool at open time
             *   IFA_NumARPRequests          (sana2_rx.c) and the TX ring is a
             *                               compile-time array.
             *   IFA_PacketFilterMode        src/bpf/ captures what the stack
             *                               sees; there is no promiscuous
             *                               mode to select.
             *   IFA_DownGoesOffline         down ALWAYS goes offline here --
             *                               NX_LINK_DISABLE issues S2_OFFLINE
             *                               -- so FALSE cannot be honoured
             *                               and TRUE is not a choice.
             *   IFA_ReportOffline           nothing notifies.
             *   IFA_RequiresInitDelay       no settle delay is implemented.
             *   IFA_CopyMode                the copy hooks are chosen by what
             *                               the device asked for.
             *   IFA_HardwareAddress         the shim reads the station
             *                               address; it does not set one.
             * -----------------------------------------------------------
             */
            case IFA_IPType:
            case IFA_ARPType:
            case IFA_NumReadRequests:
            case IFA_NumWriteRequests:
            case IFA_NumARPRequests:
            case IFA_PacketFilterMode:
            case IFA_DownGoesOffline:
            case IFA_ReportOffline:
            case IFA_RequiresInitDelay:
            case IFA_CopyMode:
            case IFA_HardwareAddress:
                return bsd_fail(SocketBase, AMI_EOPNOTSUPP);

            default:
                break;
        }
    }

    return 0;
}

LONG bsd_AddInterfaceTagList(register STRPTR name __asm("a0"),
                             register STRPTR device __asm("a1"),
                             register LONG unit __asm("d0"),
                             register struct TagItem *tags __asm("a2"),
                             register struct AmiSocketBase *SocketBase __asm("a6"))
{
    NX_IP       *ip = netstack_ip();
    AmiIfConfig  cfg;
    UWORD        index = 0;
    LONG         rc;

    if (name == NULL || device == NULL)
        return bsd_fail(SocketBase, AMI_EINVAL);

    /* "This name cannot be longer than 15 characters." */
    if (name[0] == '\0' ||
        bsd_strlen((const char *)name) >= (ULONG)BSD_IFNAME_SIZE)
        return bsd_fail(SocketBase, AMI_EINVAL);

    if (device[0] == '\0' ||
        bsd_strlen((const char *)device) >= (ULONG)AMI_CFG_PATH_LEN)
        return bsd_fail(SocketBase, AMI_EINVAL);

    if (unit < 0)
        return bsd_fail(SocketBase, AMI_EINVAL);

    if (ip == NULL)
        return bsd_fail(SocketBase, AMI_ENETDOWN);

    if (bsd_if_index_of(ip, (const char *)name) >= 0)
        return bsd_fail(SocketBase, AMI_EEXIST);

    bsd_bzero(&cfg, sizeof(cfg));
    bsd_strncpy(cfg.name, (const char *)name, sizeof(cfg.name));
    bsd_strncpy(cfg.device, (const char *)device, sizeof(cfg.device));
    cfg.unit   = (ULONG)unit;
    cfg.iptype = AMI_IPTYPE_STATIC;
    cfg.up     = FALSE;

    /*
     * The address is deliberately NOT set here, and cannot be: the tag list
     * this call takes has no address tag in it. An interface arrives with no
     * address and is given one by ConfigureInterfaceTagList(), which is what
     * the two calls' tag sets say and is why they are two calls.
     */
    if (tags != NULL && bsd_if_parse_add(SocketBase, tags, &cfg) != 0)
        return -1;

    rc = netstack_interface_add(&cfg, &index);
    if (rc != AMI_NET_OK)
    {
        switch (rc)
        {
            case AMI_NET_ERR_NODEV:  return bsd_fail(SocketBase, AMI_ENXIO);
            case AMI_NET_ERR_NOMEM:  return bsd_fail(SocketBase, AMI_ENOBUFS);
            case AMI_NET_ERR_CONFIG: return bsd_fail(SocketBase, AMI_EEXIST);
            /* No free slot: NX_MAX_PHYSICAL_INTERFACES is 2, so a user meets
               this rather than only a bug. */
            default:                 return bsd_fail(SocketBase, AMI_ENOSPC);
        }
    }

    /* IFA_LimitMTU, now that the driver's own MTU is known and the interface
       exists to apply it to. The clamp is ConfigureInterfaceTagList()'s. */
    if (cfg.mtu != 0)
    {
        struct TagItem mtu_tags[2];

        mtu_tags[0].ti_Tag  = IFC_LimitMTU;
        mtu_tags[0].ti_Data = cfg.mtu;
        mtu_tags[1].ti_Tag  = TAG_DONE;
        mtu_tags[1].ti_Data = 0;

        (VOID)bsd_ConfigureInterfaceTagList(name, mtu_tags, SocketBase);
    }

    return 0;
}

/* ---------------------------------------------------- RemoveInterface --- */

/*
 * "success -- TRUE for success, 0 for failure" -- the OPPOSITE of every other
 * call in this file, which are all 0 for success and -1 for failure. One page
 * apart in the same document. This is why the return convention is read
 * rather than inferred.
 *
 * AND THE TWO SOURCES DISAGREE ABOUT THE TYPE. The autodoc's synopsis is
 * "BOOL RemoveInterface(STRPTR name,BOOL force)"; clib/bsdsocket_protos.h in
 * the same NDK says "LONG RemoveInterface(STRPTR interface_name, LONG force)".
 * The header wins on the TYPE, because the header is what a caller compiles
 * against -- and the autodoc wins on the VALUES, because the header says
 * nothing about them. LONG 1 for success and 0 for failure satisfies both
 * readings; 0-for-success, which every neighbouring call uses, satisfies
 * neither and would report failure as success to every BOOL test.
 */
LONG bsd_RemoveInterface(register STRPTR name __asm("a0"),
                         register LONG force __asm("d0"),
                         register struct AmiSocketBase *SocketBase __asm("a6"))
{
    NX_IP *ip = netstack_ip();
    LONG   index;
    LONG   rc;

    if (name == NULL ||
        bsd_strlen((const char *)name) >= (ULONG)BSD_IFNAME_SIZE)
    {
        (VOID)bsd_fail(SocketBase, AMI_EINVAL);
        return 0;
    }

    if (ip == NULL)
    {
        (VOID)bsd_fail(SocketBase, AMI_ENETDOWN);
        return 0;
    }

    index = bsd_if_index_of(ip, (const char *)name);
    if (index < 0)
    {
        (VOID)bsd_fail(SocketBase, AMI_ENXIO);
        return 0;
    }

    rc = netstack_interface_remove((UWORD)index, (force != 0) ? TRUE : FALSE);
    if (rc == AMI_NET_OK)
        return 1;

    /*
     * "RemoveInterface() will refuse to remove an interface which is still in
     * use. Use a 'force' parameter of TRUE to make it remove the interface
     * anyway." EBUSY is that refusal, and it is the one a caller can do
     * something about.
     *
     * The other failure is a SANA-II device that would not give its read
     * requests back. The autodoc's own wording for `force` -- "memory may
     * remain allocated until you shut down the network" -- describes exactly
     * that state, and this stack declines to enter it: the requests point
     * into the interface, so freeing it would hand the device memory the
     * system has taken back. The interface stays, down and registered, and
     * the caller is told with EBUSY as well, because the remedy is the same
     * one the autodoc names -- shut the network down.
     */
    (VOID)bsd_fail(SocketBase,
                   (rc == AMI_NET_ERR_BUSY || rc == AMI_NET_ERR_STATE)
                       ? AMI_EBUSY : AMI_EINVAL);

    return 0;
}

/*
 * BeginInterfaceConfig() and AbortInterfaceConfig() are in addralloc.c, with
 * the message they traffic in. They are not here because what they mostly do
 * is validate and reply a struct AddressAllocationMessage, which is that
 * file's subject rather than this one's.
 */

/* ---------------------------------------------------- the BSD ioctl half -- */

/*
 * SIOCGIFCONF and the SIOCGIF* family, which exist here for one reason:
 * libpcap's pcap_findalldevs() is how `tcpdump -D` and a bare `tcpdump` with
 * no -i discover what they can capture on, and it asks through these and
 * nothing else. Capture on a NAMED interface already worked; it was only
 * being ASKED which names exist that returned ENOSYS, so tcpdump exited 20
 * with an empty file (docs/RESEARCH.md 60).
 *
 * THE ENCODINGS ARE THE NDK'S, not invented here, because guessing an ABI has
 * cost this project time twice. sys/sockio.h gives SIOCGIFCONF as
 * _IOWR('i',36,struct ifconf) and the rest as _IOWR('i',n,struct ifreq); the
 * static assertions below fail the build if those sizes ever stop being 8 and
 * 32, which is the only way the layout below can silently go wrong.
 *
 * THE ONE SUBTLETY IS sa_len. fad-gifc walks the SIOCGIFCONF result by
 * striding sizeof(ifr_name) + ifr_addr.sa_len rather than sizeof(struct
 * ifreq), so an entry whose sockaddr says 0 makes the walk stride 16 and read
 * the second half of the entry it has already read as a name. Every sockaddr
 * written here therefore carries its length, and because sockaddr_in and
 * sockaddr are both 16 bytes on this NDK the stride comes out at 32 either
 * way -- which is what makes the bug invisible until somebody looks.
 *
 * ONLY PHYSICAL INTERFACES ARE LISTED. NetX Duo puts loopback past the
 * physical range, and there is no BPF channel that can bind to it: offering a
 * name that cannot then be captured on would turn one honest failure into two
 * confusing ones.
 */

AMI_STATIC_ASSERT(sizeof(struct ifreq) == 32,  "struct ifreq is the NDK's");
AMI_STATIC_ASSERT(sizeof(struct ifconf) == 8,  "struct ifconf is the NDK's");
AMI_STATIC_ASSERT(sizeof(struct sockaddr_in) == 16, "sockaddr_in is 4.4BSD's");
AMI_STATIC_ASSERT(IOCPARM_LEN(SIOCGIFCONF) == 8,   "SIOCGIFCONF parameter");
AMI_STATIC_ASSERT(IOCPARM_LEN(SIOCGIFADDR) == 32,  "SIOCGIFADDR parameter");

/* One address into a caller's sockaddr slot, with the length BSD requires. */
static VOID bsd_if_put_addr(struct sockaddr *sa, ULONG addr)
{
    struct sockaddr_in sin;

    bsd_bzero(&sin, sizeof(sin));
    sin.sin_len         = (UBYTE)sizeof(sin);
    sin.sin_family      = AF_INET;
    sin.sin_port        = 0;
    sin.sin_addr.s_addr = (ULONG)htonl(addr);

    bsd_bcopy(&sin, sa, sizeof(sin));
}

/*
 * What an interface looks like to code that thinks in BSD flags. RUNNING and
 * UP are distinct on purpose: UP is "configured and meant to be carrying
 * traffic", RUNNING is "the link is actually there", and libpcap prints the
 * difference. Every interface here is a SANA-II Ethernet device, so BROADCAST
 * and MULTICAST are unconditional -- there is no SLIP or PPP in this stack to
 * make them conditional on.
 */
static UWORD bsd_if_flags(const BsdIfInfo *info)
{
    UWORD flags = (UWORD)(IFF_BROADCAST | IFF_MULTICAST | IFF_SIMPLEX);

    if (info->bii_Address != 0)
        flags |= (UWORD)IFF_UP;

    if (info->bii_LinkUp)
        flags |= (UWORD)IFF_RUNNING;

    return flags;
}

LONG bsd_if_ioctl(ULONG req, APTR argp,
                  struct AmiSocketBase *SocketBase)
{
    NX_IP *ip;
    UINT   i;

    if (argp == NULL)
        return bsd_fail(SocketBase, AMI_EFAULT);

    ip = netstack_ip();
    if (ip == NULL)
        return bsd_fail(SocketBase, AMI_ENETDOWN);

    /* ----------------------------------------------------- SIOCGIFCONF -- */
    if (req == (ULONG)SIOCGIFCONF)
    {
        struct ifconf *ifc    = (struct ifconf *)argp;
        struct ifreq  *out    = ifc->ifc_req;
        LONG           room   = ifc->ifc_len;
        LONG           used   = 0;

        /*
         * A NULL buffer is the caller asking how much room it needs, which
         * fad-gifc does not do but other callers of this ioctl have always
         * been allowed to. Counting costs one pass and removes a way to
         * crash.
         */
        if (out == NULL)
            room = 0;

        if (bsd_nx_enter(SocketBase) != 0)
            return bsd_fail(SocketBase, AMI_ENETDOWN);

        for (i = 0; i < (UINT)NX_MAX_PHYSICAL_INTERFACES; i++)
        {
            char name[BSD_IFNAME_SIZE];

            if (!bsd_if_name_of(ip, i, name, sizeof(name)))
                continue;

            if (used + (LONG)sizeof(struct ifreq) <= room)
            {
                struct ifreq *ifr = (struct ifreq *)((UBYTE *)out + used);

                bsd_bzero(ifr, sizeof(*ifr));
                bsd_strncpy((char *)ifr->ifr_name, name,
                            sizeof(ifr->ifr_name));
                bsd_if_put_addr(&ifr->ifr_addr,
                                ip->nx_ip_interface[i].nx_interface_ip_address);
            }

            /*
             * Counted whether or not it fitted: that is what makes the
             * grow-and-retry loop every caller of this ioctl writes actually
             * terminate.
             */
            used += (LONG)sizeof(struct ifreq);
        }

        bsd_nx_leave(SocketBase);

        ifc->ifc_len = used;
        return 0;
    }

    /* ------------------------------------------------------- SIOCGIF* -- */
    {
        struct ifreq *ifr = (struct ifreq *)argp;
        BsdIfInfo     info;
        char          name[BSD_IFNAME_SIZE];
        LONG          index;

        /* ifr_name arrives from the caller and need not be terminated. */
        bsd_strncpy(name, (const char *)ifr->ifr_name, sizeof(name));
        name[sizeof(name) - 1] = '\0';

        index = bsd_if_index_of(ip, name);
        if (index < 0)
            return bsd_fail(SocketBase, AMI_ENXIO);

        if (bsd_nx_enter(SocketBase) != 0)
            return bsd_fail(SocketBase, AMI_ENETDOWN);

        bsd_if_gather(ip, (UINT)index, &info);

        bsd_nx_leave(SocketBase);

        /* Out of the bracket before the caller's memory is touched, for the
           reason bsd_QueryInterfaceTagList() gives at its own gather. */
        switch (req)
        {
            case SIOCGIFFLAGS:
                ifr->ifr_flags = (WORD)bsd_if_flags(&info);
                return 0;

            case SIOCGIFADDR:
                bsd_if_put_addr(&ifr->ifr_addr, info.bii_Address);
                return 0;

            case SIOCGIFNETMASK:
                bsd_if_put_addr(&ifr->ifr_addr, info.bii_NetMask);
                return 0;

            case SIOCGIFBRDADDR:
                bsd_if_put_addr(&ifr->ifr_addr, info.bii_Broadcast);
                return 0;

            default:
                break;
        }
    }

    return bsd_fail(SocketBase, AMI_ENOSYS);
}

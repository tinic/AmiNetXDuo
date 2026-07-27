/*
 * bsdsocket.library -- the interface QUERY half of the Roadshow interface API.
 *
 *   ObtainInterfaceList()       the names of the interfaces the stack has
 *   ReleaseInterfaceList()
 *   QueryInterfaceTagList()     everything else, one IFQ_* tag at a time
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
 * THREE THINGS THE DOCUMENT SETTLED THAT WOULD OTHERWISE HAVE BEEN GUESSES
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
 * WHAT IS NOT ANSWERED, AND WHY
 *
 * A tag this stack has no true value for is LEFT ALONE: the caller's storage
 * is not written at all. Writing an invented zero would be indistinguishable
 * from a measured zero, and a monitor that shows "0 packets dropped" when
 * nothing counts drops is worse than one that shows the caller's own default.
 * Each such tag is listed at its case below with the reason.
 *
 * SPDX-License-Identifier: MIT
 */

#include "bsdsocket_vectors.h"

#include "aminetxduo/config.h"
#include "aminetxduo/sana2.h"

#include <proto/exec.h>

/*
 * "This name cannot be longer than 15 characters" -- QueryInterfaceTagList()
 * and AddInterfaceTagList() both say so, so 15 plus a NUL is the whole name
 * storage this API ever needs.
 */
#define BSD_IFNAME_SIZE     16

/* ------------------------------------------------------------- tag walking */

/*
 * Hand-rolled NextTagItem(). utility.library is not open in a shared library
 * that may be called before anything else has opened it, and the four control
 * tags are three lines of arithmetic; errno.c does the same for
 * SocketBaseTagList().
 */
static struct TagItem *bsd_next_tag(struct TagItem **cursor)
{
    struct TagItem *item = *cursor;

    while (item != NULL)
    {
        switch (item->ti_Tag)
        {
            case TAG_DONE:
                *cursor = NULL;
                return NULL;

            case TAG_IGNORE:
                item++;
                continue;

            case TAG_MORE:
                item = (struct TagItem *)item->ti_Data;
                continue;

            case TAG_SKIP:
                item += 1 + (LONG)item->ti_Data;
                continue;

            default:
                *cursor = item + 1;
                return item;
        }
    }

    *cursor = NULL;
    return NULL;
}

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

/* The physical slot called `name`, or -1. */
static LONG bsd_if_index_of(NX_IP *ip, const char *name)
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

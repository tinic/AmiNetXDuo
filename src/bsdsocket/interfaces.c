/*
 * bsdsocket.library, the Roadshow interface API.
 *
 *   ObtainInterfaceList()       the names of the interfaces the stack has
 *   ReleaseInterfaceList()
 *   QueryInterfaceTagList()     everything else, one IFQ_* tag at a time
 *   ConfigureInterfaceTagList() address, mask, MTU and up/down
 *
 * Behaviour follows the bsdsocket.library autodoc in NDK 3.2
 * (SANA+RoadshowTCP-IP/doc/bsdsocket.doc) read together with
 * libraries/bsdsocket.h from the same NDK. Both are freely distributable and
 * are used as an ABI reference only; no Roadshow, AmiTCP, AROSTCP or Miami
 * code was consulted, and none of it is present. Where the behaviour is not
 * obvious from the name, the autodoc's own words are quoted below (see the
 * header of roadshow.c for why the ABI is read rather than inferred).
 *
 * Four points the document settled:
 *
 *   1. ObtainInterfaceList() returns plain Nodes carrying only a name:
 *      "Pointer to a 'struct List', whose individual Nodes contain the names
 *      of the respective interfaces (found in node->ln_Name)". Not a
 *      Roadshow-private node type with fields after it, so ln_Name is all
 *      that is published here.
 *
 *   2. Every IFQ_* tag's ti_Data is a pointer to caller storage, never the
 *      value. The autodoc types each tag as "(LONG *)", "(ULONG *)",
 *      "(struct sockaddr *)", "(SBQUAD_T *)" and so on;
 *      libraries/bsdsocket.h alone does not say this.
 *
 *   3. "0 for success, -1 for failure; the failure code will be stored in the
 *      'errno' variable", so this is a bsd_fail() call site and not a BOOL
 *      one. RemoveInterface(), one page further on, is the opposite (TRUE for
 *      success).
 *
 *   4. IFC_State takes four values where IFQ_State returns two. SM_Online is
 *      "SM_Up, but send S2_ONLINE to the device first, and if that fails do
 *      nothing else". One block of #defines in libraries/bsdsocket.h, two
 *      different vocabularies.
 *
 * On the query side, a tag this stack has no true value for is left alone:
 * the caller's storage is not written at all. An invented zero would be
 * indistinguishable from a measured zero. Each such tag is listed at its case
 * below with the reason.
 *
 * On the configure and add sides there are two classes of tag this stack does
 * not implement, and they are answered differently. Both functions are "a
 * simplified, specialized front end to the underlying interface socket API",
 * and the autodoc documents no error for an unsupported tag at all:
 *
 *   Advisory, it tunes how the stack goes about its work and changes nothing
 *   a caller can observe through this API or on the wire. Accepted and
 *   ignored: refusing would fail the whole call over a tag whose only effect
 *   is one this stack was never going to have, and real callers pass these as
 *   a matter of course.
 *
 *   Behavioural, its whole purpose is to change what the interface does, so
 *   a caller that saw success would be misled. Refused with EOPNOTSUPP, and
 *   nothing in the list is applied. A value that names what this stack already
 *   does is not a change, and is accepted.
 *
 * Each tag is filed under one of the two at its case below.
 *
 * ---------------------------------------------------------------------------
 * Why bsd_if_index_of() and ObtainInterfaceList() read nx_ip_interface[]
 * without the ThreadX bracket QueryInterfaceTagList() takes.
 *
 * The bracket is a ThreadX-context lock, not a lock on this array.
 * bsd_nx_enter() adopts the calling Task and takes the ThreadX baton, which
 * QueryInterfaceTagList() needs because bsd_if_gather() calls nx_ip_info_get()
 * and nx_arp_info_get(), and those take the NX_IP protection mutex. Reading
 * NX_INTERFACE fields needs no such thing.
 *
 * What the baton would buy, if taken, is exclusion against the other ThreadX
 * threads. Every writer of nx_ip_interface[] while the stack is up is one:
 * nx_ip_interface_attach(), nx_ip_interface_detach(),
 * nx_ip_interface_address_set() from the DHCP client, and nx_interface_link_up
 * from the SANA-II reader. And there are no torn reads to protect against in
 * the first place, one CPU, and Exec switches tasks only between
 * instructions, so every aligned load here is atomic.
 *
 * What it would not buy is exclusion against interface add and removal, which
 * is the case that matters. netstack_interface_add() and
 * netstack_interface_remove() do most of their work outside the baton on
 * purpose, because opening and closing a SANA-II device is Exec I/O:
 * ami_sana2_close(), which frees the AmiSana2If, runs after the bracketed
 * detach, and ns_Iface[] and AmiIfConfig.configured are written outside it. A
 * bracket here would serialise against nothing that matters and cost ~270 us a
 * call.
 *
 * So the reads stay unbracketed, and the two things that were genuinely wrong
 * are fixed where they are:
 *
 *   - bsd_if_name_of() dereferenced nx_interface_name after testing
 *     nx_interface_valid. A removal between the two left it NULL.
 *   - ConfigureInterfaceTagList() read nx_interface_additional_link_info and
 *     the address/mask pair outside the bracket and used them inside it. The
 *     first is a use-after-free once the removal reaches ami_sana2_close(); the
 *     second reverts whichever half of the pair a DHCP bind had just changed.
 *     Both reads now happen inside the bracket that uses them.
 *
 * What remains is netstack.c's and is recorded in docs/BACKLOG.md: nothing
 * serialises two tasks calling AddInterfaceTagList() at once, so both can pick
 * the same free slot.
 * ---------------------------------------------------------------------------
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
#include "aminetxduo/ifindex.h"

#include <proto/exec.h>

/*
 * How long ConfigureInterfaceTagList() waits for a name in IFC_Address to
 * resolve. Thirty seconds, matching what resolver.c gives gethostbyname():
 * it is the same lookup.
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
 * "struct sockaddr *" and IFQ_NetMask "struct sockaddr_in *". One shape
 * behind all four, and no length argument anywhere in the call, so the caller
 * has supplied storage for a whole sockaddr_in and that is what goes in it.
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
 * one, otherwise the name NetX Duo gave the slot, so the two APIs agree about
 * what an interface is called.
 *
 * Loopback is not listed. It lives past the physical slots
 * (NX_LOOPBACK_INTERFACE), has no SANA-II device to answer the IFQ_ tags
 * about, and its only name is NetX Duo's "Internal IP Loopback", twenty
 * characters, where the API caps a name at fifteen, so
 * QueryInterfaceTagList() would reject the name it was just handed.
 */
/*
 * Loopback, in RFC 3493 terms.
 *
 * NetX Duo parks it at nx_ip_interface[NX_LOOPBACK_INTERFACE], one past the
 * physical slots, and this library numbers an interface by its slot plus one,
 * the convention if_nametoindex() and GetRouteInfo()'s rtm_index already use.
 * So loopback is NX_LOOPBACK_INTERFACE + 1 and nothing else has to move.
 *
 * The name is ours, not NetX Duo's: its own is "Internal IP Loopback", twenty
 * characters where IF_NAMESIZE allows fifteen.  "lo0" is what BSD calls it and
 * what a program that hardcodes a loopback name will try.
 *
 * This is the RFC 3493 trio only.  ObtainInterfaceList(),
 * QueryInterfaceTagList() and SIOCGIFCONF are about SANA-II interfaces a
 * caller can configure, loopback is not one, and bsd_if_name_of() and
 * bsd_if_index_of() below stay physical-only for them.
 */
#if (NX_MAX_IP_INTERFACES > NX_MAX_PHYSICAL_INTERFACES)
#define BSD_HAVE_LOOPBACK_IF    1
#define BSD_LOOPBACK_IFINDEX    (NX_LOOPBACK_INTERFACE + 1)
#define BSD_LOOPBACK_IFNAME     "lo0"
#else
#define BSD_HAVE_LOOPBACK_IF    0
#endif

static BOOL bsd_if_name_of(NX_IP *ip, UINT index, char *out, ULONG outlen)
{
    const NX_INTERFACE *nxif = &ip->nx_ip_interface[index];
    const AmiIfConfig  *cfg;
    const char         *nxname;

    if (outlen > 0)
        out[0] = '\0';

    if (nxif->nx_interface_valid == 0)
        return FALSE;

    cfg = bsd_if_config(index);

    if (cfg != NULL && cfg->name[0] != '\0')
    {
        bsd_strncpy(out, cfg->name, outlen);
        return (out[0] != '\0') ? TRUE : FALSE;
    }

    /*
     * The fallback, and the one read in this file that a concurrent removal
     * can invalidate. nx_ip_interface_detach() memsets the whole NX_INTERFACE,
     * so nx_interface_name goes NULL; nx_ip_interface_attach() sets
     * nx_interface_valid before it sets the name, so the reverse window exists
     * too. Neither is closed by the ThreadX bracket, see the file header,
     * so the pointer is loaded once and tested. What it points at when it is
     * not NULL is the netstack's own AmiIfConfig.name, which outlives every
     * caller of this library.
     */
    nxname = (const char *)nxif->nx_interface_name;
    if (nxname == NULL)
        return FALSE;

    bsd_strncpy(out, nxname, outlen);

    return (out[0] != '\0') ? TRUE : FALSE;
}

/*
 * Names are compared case-insensitively: they come from file names in
 * DEVS:NetInterfaces, and AmigaDOS file names are case-insensitive, so
 * `NetMon ETH0` names the interface in the file called "eth0".
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

BOOL bsd_if_name_by_index(NX_IP *ip, ULONG index, char *out, ULONG outlen)
{
    if (outlen > 0)
        out[0] = '\0';

    if (ip == NULL || index == 0UL)
        return FALSE;

#if BSD_HAVE_LOOPBACK_IF
    if (index == (ULONG)BSD_LOOPBACK_IFINDEX)
    {
        if (ip->nx_ip_interface[NX_LOOPBACK_INTERFACE].nx_interface_valid == 0)
            return FALSE;

        bsd_strncpy(out, BSD_LOOPBACK_IFNAME, outlen);
        return (out[0] != '\0') ? TRUE : FALSE;
    }
#endif

    if (index > (ULONG)NX_MAX_PHYSICAL_INTERFACES)
        return FALSE;

    return bsd_if_name_of(ip, (UINT)(index - 1UL), out, outlen);
}

/* The physical slot called `name`, or -1. Published in interfaces.h because
   addralloc.c asks the same question under the same naming rule. */
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

/* --------------------------------------------------- ObtainInterfaceList, */

/*
 * One allocation holds the List, the Nodes and the name strings, so
 * ReleaseInterfaceList() is a single free of the block the list header sits at
 * the top of. Same arrangement as bsd_ObtainDomainNameServerList(), for the
 * same reason: the published free takes only the list pointer.
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

    /*
     * "The result can be NULL if there was not enough memory available to
     * fill it. If no interfaces have been added yet, you will receive an
     * empty list." NULL has exactly one documented meaning here, so a stack
     * that is not running gets the empty list rather than a second one: a
     * caller that saw NULL could not tell "out of memory" from "nothing
     * running", and the doc gives it no errno to ask with either.
     *
     * The block is allocated up front, before anything is counted.
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

    /* Nothing running is nothing to list, which is the empty list. */
    if (ip == NULL)
        return &out->bil_List;

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

/* ------------------------------------------------ QueryInterfaceTagList, */

/*
 * Everything the tag loop can be asked for, gathered in one pass so the
 * ThreadX bracket is held once and briefly. netstatus.c's rule 1 applies:
 * nothing that leaves here is a pointer into the stack, with one exception.
 * bii_Device points into the AmiConfig inside the netstack singleton, because
 * IFQ_DeviceName is documented to return a pointer ("A pointer to the name
 * will be returned"). That storage outlives every caller: the stack is torn
 * down only when the last opener has gone.
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

    BOOL            bii_LinkUp;         /* the wire */
    BOOL            bii_AdminUp;        /* the stack's intent */
    BOOL            bii_HaveSana;
    LONG            bii_BindType;

    UBYTE           bii_HwAddress[AMI_ETH_ADDR_SIZE];

    AmiSana2Stats   bii_Stats;
    AmiSana2Info    bii_Info;

    /* Stack-wide, not per interface, see the IFQ_IPDrops case. */
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

    /* All-ones host part of this interface's prefix: the only broadcast
       address an interface configured from a mask can have. */
    if (info->bii_NetMask != 0)
        info->bii_Broadcast = (info->bii_Address & info->bii_NetMask) |
                              ~info->bii_NetMask;

    if (cfg != NULL)
    {
        info->bii_Device = cfg->device;
        info->bii_Unit   = cfg->unit;

        /*
         * IFABT_Unknown is documented as "has not been bound or is in
         * transitional state", which covers a DHCP interface with no lease
         * yet, so the address test comes first. LINKLOCAL is RFC 3927
         * self-assignment, an automated process whose answer can change on the
         * next collision, so IFABT_Dynamic by the autodoc's definition.
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

    /* ami_sana2_attach() stores the AmiSana2If in the interface's additional
       link info; that is how the driver's own values are reached. */
    sana = (AmiSana2If *)nxif->nx_interface_additional_link_info;
    if (sana != NULL)
    {
        info->bii_HaveSana    = TRUE;
        info->bii_AdminUp     = ami_sana2_admin_up(sana);
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
       name an interface, so EINVAL rather than a lookup miss. */
    if (bsd_strlen((const char *)name) >= (ULONG)BSD_IFNAME_SIZE)
        return bsd_fail(SocketBase, AMI_EINVAL);

    if (ip == NULL)
        return bsd_fail(SocketBase, AMI_ENETDOWN);

    index = bsd_if_index_of(ip, (const char *)name);
    if (index < 0)
    {
        /*
         * The autodoc gives no errno for "no such interface". ENXIO is what
         * the rest of this library answers (netstatus.c maps
         * NX_INVALID_INTERFACE to it, and 4.4BSD's SIOCGIF* ioctls use it),
         * so a caller sees one code for the condition however it asked.
         */
        return bsd_fail(SocketBase, AMI_ENXIO);
    }

    /* An empty tag list asks "does this interface exist?", which is legal. */
    if (tags == NULL)
        return 0;

    if (bsd_nx_enter(SocketBase) != 0)
        return bsd_fail(SocketBase, AMI_ENETDOWN);

    bsd_if_gather(ip, (UINT)index, &info);

    bsd_nx_leave(SocketBase);

    /* Out of the bracket before any caller memory is touched: the answers are
       all in `info` now, and writing into an application buffer can fault. */
    cursor = tags;
    while ((item = bsd_next_tag(&cursor)) != NULL)
    {
        switch (item->ti_Tag)
        {
            /* -------------------------------------------- the SANA-II half */

            case IFQ_DeviceName:
                /* "A pointer to the name will be returned": ti_Data is a
                   STRPTR *, not a buffer to copy into. */
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
                /* "the number returned will be the number of bits", taken
                   from S2_DEVICEQUERY rather than assuming Ethernet. */
                if (info.bii_HaveSana)
                    bsd_put_long(item, (LONG)info.bii_Info.address_bits);
                break;

            case IFQ_HardwareAddress:
                /* "these are bytes, not a NUL-terminated string", at most
                   sixteen. "Other network interfaces may use hardware
                   addresses which could be shorter than six bytes", so only as
                   many bytes as IFQ_HardwareAddressSize reports: a caller that
                   sized its buffer from that tag has no more room. Ethernet
                   says 48 bits and gets the six the shim keeps. */
                if (info.bii_HaveSana)
                {
                    UBYTE *out   = (UBYTE *)bsd_tag_storage(item);
                    ULONG  bytes = info.bii_Info.address_bits / 8;

                    if (bytes > (ULONG)AMI_ETH_ADDR_SIZE)
                        bytes = (ULONG)AMI_ETH_ADDR_SIZE;

                    if (out != NULL && bytes != 0)
                        bsd_bcopy(info.bii_HwAddress, out, bytes);
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
                 * update: this stack takes the driver's MTU at attach time and
                 * never runs with a different one, so the two numbers are the
                 * same and the side effect is a no-op.
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
                   processed": a failed NX_PACKET allocation on the receive
                   path. */
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
                 * the documented answer. The resolver's own name servers are
                 * not put here: they are stack-wide, not per interface.
                 */
                bsd_put_sockaddr_in(item, 0);
                break;

            /* -------------------------------------------------------- state */

            case IFQ_State:
                /*
                 * "the values returned can be either 'SM_Down' or 'SM_Up'";
                 * SM_Online/SM_Offline are IFC_State's, not this tag's.
                 *
                 * Administrative state, not link state. IFC_State defines the
                 * two: SM_Up is "the stack will attempt to transmit messages
                 * through this interface. However, the underlying SANA-II
                 * device driver may not be connected to the network yet", and
                 * SM_Down is the same sentence negated. So a cable pulled out
                 * from under a configured interface is still SM_Up, that
                 * clears nx_interface_link_up (sana2_rx.c, on S2ERR_OUTOFSERVICE)
                 * and nothing else, and it is IFF_RUNNING that reports it.
                 *
                 * An interface with no SANA-II shim has no recorded intent, so
                 * the link is the only answer available.
                 */
                bsd_put_long(item,
                             (info.bii_HaveSana ? info.bii_AdminUp
                                                : info.bii_LinkUp)
                                 ? SM_Up : SM_Down);
                break;

            case IFQ_AddressBindType:
                bsd_put_long(item, info.bii_BindType);
                break;

            case IFQ_Metric:
                /* Zero, and measured rather than invented: every interface
                   here is directly attached and this stack has no routing
                   protocol to give any of them a cost. */
                bsd_put_long(item, 0);
                break;

            case IFQ_GetDebugMode:
                /* Likewise: there is no per-interface debug mode. */
                bsd_put_long(item, FALSE);
                break;

            /*
             * ---------------------------------------------------------------
             * Not written. This stack has no true value for any of these, and
             * the caller's own default is better than an invented zero that
             * reads like a measurement.
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
             *                               infinitely", which is wrong for a
             *                               leased address.
             *   IFQ_GetBytesIn/Out          no per-interface byte counters
             *                               exist. The IP-level totals are
             *                               stack-wide, so they would differ
             *                               on a machine with two cards.
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
             *                               "(LONG)" where every neighbour is
             *                               "(LONG *)". On a query with no
             *                               other way to return anything a
             *                               bare LONG can only be a typo, but
             *                               writing through a ti_Data the
             *                               caller passed as a scalar would
             *                               corrupt its memory, so these stay
             *                               unanswered.
             * ---------------------------------------------------------------
             */

            default:
                /* Every other tag, including private ones: ignored, as a tag
                   list requires. */
                break;
        }
    }

    return 0;
}

/* --------------------------------------------- ConfigureInterfaceTagList, */

/*
 * The autodoc says nothing about what happens to a tag list whose fourth tag
 * is refused. Applying tags as they are read would leave the interface half
 * configured, new address, old mask, still down. So the whole list is parsed
 * and validated first and nothing is applied unless all of it can be; a
 * refused call leaves the interface exactly as it was.
 *
 * This is also needed for correctness: IFC_Address and IFC_NetMask arrive as
 * two tags and NetX Duo changes both in one call, so applying them separately
 * would put a mismatched pair on the interface until the next tag was read.
 *
 * Resolving a host name blocks, and nothing may be allocated inside the
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
 * address in dotted-decimal notation (per RFC1700)". Dotted-quad is tried
 * first: a machine being configured may have no working resolver yet, and a
 * name that parses as an address must not be sent to a name server.
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
 * not a good netmask in 2026, but it is the one an address alone implies;
 * refusing IFC_Address unless IFC_NetMask came with it would reject a tag
 * list the published API says is legal.
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
 * The address half of pass two, on its own so that NETCTRL_INTERFACE_CONFIGURE
 * and ConfigureNetInterface reach it as well.
 *
 * Both halves belong in one bracket: one of the pair given leaves the other as
 * it stands, so this is a read-modify-write, and a DHCP bind landing between
 * the read and the write would be reverted by the write.
 *
 * The classful fallback is here rather than at either caller for the reason
 * tests/tools/run-ifreadd.sh exists: a mask guessed from the class of the
 * address where the caller supplied one is a silently wrong /8 on a machine
 * that had a /24, and two copies of this rule would be two chances to make
 * that mistake again.
 */
LONG bsd_if_set_address(struct AmiSocketBase *SocketBase, LONG index,
                        BOOL have_address, ULONG address,
                        BOOL have_netmask, ULONG netmask)
{
    NX_IP        *ip = netstack_ip();
    NX_INTERFACE *nxif;
    UINT          status;

    if (ip == NULL)
        return bsd_fail(SocketBase, AMI_ENETDOWN);

    if (bsd_nx_enter(SocketBase) != 0)
        return bsd_fail(SocketBase, AMI_ENETDOWN);

    nxif = &ip->nx_ip_interface[index];

    if (!have_address)
        address = nxif->nx_interface_ip_address;
    if (!have_netmask)
        netmask = nxif->nx_interface_ip_network_mask;

    if (netmask == 0)
        netmask = bsd_if_classful_mask(address);

    status = nx_ip_interface_address_set(ip, (UINT)index, address, netmask);

    bsd_nx_leave(SocketBase);

    if (status != NX_SUCCESS)
        return bsd_fail(SocketBase, AMI_EADDRNOTAVAIL);

    return 0;
}

/*
 * Pass one. Returns 0, or -1 with errno set; nothing has been applied to the
 * interface either way.
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
                /* "this must be a NUL-terminated string" and nothing else; a
                   mask is not a host name, so no resolver here. */
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
                 * Accepted and a no-op: this stack reads all of
                 * DEVS:NetInterfaces and DEVS:Internet at startup and defers
                 * nothing, so there is no first time left to cause.
                 */
                break;

            /*
             * Behavioural, and BOOL. FALSE asks for nothing, which is what
             * happens here, so it is accepted; TRUE asks for something this
             * stack does not do.
             *
             *   IFC_SetDebugMode        there is no per-interface debug mode.
             *                           IFQ_GetDebugMode answers FALSE for the
             *                           same reason.
             *   IFC_GetPeerAddress      a point-to-point partner, over the
             *   IFC_GetDNS              SANA-IIR4 extensions the autodoc itself
             *                           warns few drivers have. The shim is
             *                           Ethernet-shaped throughout.
             *   IFC_AssociatedRoute     these mark an interface so that going
             *   IFC_AssociatedDNS       down tears something else down with it.
             *                           A mark nothing reads is not the mark.
             *   IFC_ReleaseAddress      STILL REFUSED, and the reason has
             *                           changed. It used to be that the DHCP
             *                           client had no release path at all, so
             *                           accepting would have left the lease
             *                           held. There is one now
             *                           (NETCTRL_DHCP_RELEASE, over
             *                           netstack_interface_dhcp_stop(index,
             *                           TRUE)), and wiring this tag to it is a
             *                           change to a published vector's
             *                           behaviour that nothing yet asks for.
             *                           Left as it was on purpose rather than
             *                           by oversight; ConfigureNetInterface
             *                           RELEASE is the way to reach it.
             */
            case IFC_SetDebugMode:
            case IFC_GetPeerAddress:
            case IFC_GetDNS:
            case IFC_AssociatedRoute:
            case IFC_AssociatedDNS:
            case IFC_ReleaseAddress:
                if (item->ti_Data != 0)
                    return bsd_fail(SocketBase, AMI_EOPNOTSUPP);
                break;

            case IFC_Metric:
                /*
                 * "The routing metric of this interface, as used by the routing
                 * protocol." Same rule, one tag along: zero is what IFQ_Metric
                 * reports and what every directly attached interface here
                 * costs, so setting zero changes nothing and is accepted.
                 * Anything else asks for a cost this stack has no routing
                 * protocol to spend, and would read back as zero.
                 */
                if ((LONG)item->ti_Data != 0)
                    return bsd_fail(SocketBase, AMI_EOPNOTSUPP);
                break;

            /*
             * ---------------------------------------------------------------
             * Behavioural, and there is no value that means "no change", so
             * refused outright. EOPNOTSUPP and not ENOSYS: the vector exists
             * and works.
             *
             *   IFC_DestinationAddress  the far end of a point-to-point link,
             *                           which none of these interfaces is.
             *   IFC_BroadcastAddress    NetX Duo derives the broadcast address
             *                           from the address and the mask; there
             *                           is no separate one to set, and
             *                           IFQ_BroadcastAddress would contradict
             *                           whatever was accepted here.
             *   IFC_AddAliasAddress     one IPv4 address per interface.
             *   IFC_DeleteAliasAddress
             * ---------------------------------------------------------------
             */
            case IFC_DestinationAddress:
            case IFC_BroadcastAddress:
            case IFC_AddAliasAddress:
            case IFC_DeleteAliasAddress:
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

    /* An empty list is a legal no-op, same as for the query. */
    if (tags == NULL)
        return 0;

    if (bsd_if_parse_config(SocketBase, tags, &req) != 0)
        return -1;

    nxif = &ip->nx_ip_interface[index];

    /* --- pass two, in the order a configuration has to happen in --------- */

    /*
     * SM_Online goes first, alone among the states. "If the command succeeds,
     * the other necessary configuration operations will take place. If it
     * fails, then this function will return with an error code set and no
     * further configuration will have been done", so the S2_ONLINE has to be
     * tried before anything else is changed, or a device that refuses it
     * leaves behind an MTU and an address the call reported as a failure.
     */
    if (req.bcr_HaveState && req.bcr_State == SM_Online &&
        netstack_interface_up((UWORD)index) != AMI_NET_OK)
        return bsd_fail(SocketBase, AMI_ENXIO);

    if (req.bcr_HaveMTU)
    {
        /*
         * "Before the maximum transmission unit size is limited, the hardware
         * MTU settings will be reread and taken into account." The driver's
         * MTU is read at open time and does not change, so taking it into
         * account means clamping to it: this tag can only make the MTU
         * smaller, and a request for more than the hardware can carry becomes
         * the hardware's own number rather than an error.
         *
         * The AmiSana2If is fetched inside the bracket and used inside the same
         * one. Read outside it, a removal that had got as far as
         * ami_sana2_close() would have freed it; inside, the interface cannot
         * be detached under us, and a detach that already happened leaves
         * nx_interface_additional_link_info NULL, which is the no-clamp case.
         */
        AmiSana2If *sana;
        ULONG       limit = req.bcr_MTU;
        ULONG       hardware;

        if (bsd_nx_enter(SocketBase) != 0)
            return bsd_fail(SocketBase, AMI_ENETDOWN);

        sana     = (AmiSana2If *)nxif->nx_interface_additional_link_info;
        hardware = (sana != NULL) ? ami_sana2_get_mtu(sana) : 0;

        if (hardware != 0 && limit > hardware)
            limit = hardware;

        status = nx_ip_interface_mtu_set(ip, (UINT)index, limit);

        bsd_nx_leave(SocketBase);

        if (status != NX_SUCCESS)
            return bsd_fail(SocketBase, AMI_EINVAL);
    }

    if (req.bcr_HaveAddress || req.bcr_HaveNetMask)
    {
        if (bsd_if_set_address(SocketBase, index,
                               req.bcr_HaveAddress, req.bcr_Address,
                               req.bcr_HaveNetMask, req.bcr_NetMask) != 0)
            return -1;
    }

    if (req.bcr_HaveState && req.bcr_State != SM_Online)
    {
        LONG rc;

        /*
         * Last, so that {IFC_Address, IFC_State SM_Up} does what it reads
         * like. netstack_interface_*() take the ThreadX bracket themselves and
         * stop the SANA-II readers as well as telling NetX Duo, so they are
         * called outside ours, the same rule netstatus.c follows for
         * NETCTRL_INTERFACE_UP.
         *
         * The four states are three transitions. SM_Up and SM_Online both
         * bring the link up, and NX_LINK_ENABLE issues the S2_ONLINE either
         * way: a device that is not on the network cannot carry the stack's
         * traffic, so there is no useful "up but offline".
         *
         * Down and offline really are different, and the autodoc says so:
         * SM_Down is "the stack will no longer attempt to transmit messages
         * through this interface. However, the underlying SANA-II device
         * driver may still be connected to the network", where SM_Offline is
         * "same as 'SM_Down', but also sends an 'S2_OFFLINE' command". A unit
         * shared with Envoy or ACS stays on the wire for them.
         */
        if (req.bcr_State == SM_Up)
            rc = netstack_interface_up((UWORD)index);
        else if (req.bcr_State == SM_Down)
            rc = netstack_interface_stack_down((UWORD)index);
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
 * The work is netstack_interface_add()'s: half of it is NetX Duo's (attach an
 * interface to a running NX_IP) and half is the netstack's (open the SANA-II
 * device, register it for capture, take a configuration slot). An interface
 * that got only the first half would be invisible to netstack_shutdown() and
 * its device would never be closed.
 *
 * The tag policy is the file header's. Roadshow callers pass the tuning tags
 * as a matter of course, so refusing them refused the interface itself.
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
                   known: "you can request that a smaller size is used". */
                if (item->ti_Data == 0)
                    return bsd_fail(SocketBase, AMI_EINVAL);
                cfg->mtu = (ULONG)item->ti_Data;
                break;

            case IFA_DownGoesOffline:
                /* "bringing the interface 'down' ... will cause the associated
                   SANA-II device driver to be switched offline. Default is
                   FALSE." Honoured either way; see IFC_State's SM_Down. */
                cfg->down_goes_offline = (item->ti_Data != 0) ? TRUE : FALSE;
                break;

            case IFA_SetDebugMode:
                /* Behavioural, BOOL: there is no debug mode, so turning it off
                   is possible and turning it on is not. */
                if (item->ti_Data != 0)
                    return bsd_fail(SocketBase, AMI_EOPNOTSUPP);
                break;

            case IFA_Multicast:
            case IFA_PointToPoint:
                /*
                 * "Not normally necessary since the stack can figure this out
                 * all by itself": the SANA-II shim reads the wire type from
                 * S2_DEVICEQUERY. Accepted when it agrees with what the
                 * hardware says, which for every device this stack drives
                 * means multicast TRUE and point-to-point FALSE.
                 */
                if ((item->ti_Tag == IFA_Multicast) != (item->ti_Data != 0))
                    return bsd_fail(SocketBase, AMI_EOPNOTSUPP);
                break;

            /*
             * Three more that "override" a default this stack already has. The
             * documented default is what happens here, so asking for it is a
             * request that is met; anything else is not available.
             */
            case IFA_IPType:            /* "Default is 2048." */
                if ((ULONG)item->ti_Data != (ULONG)AMI_ETHERTYPE_IPV4)
                    return bsd_fail(SocketBase, AMI_EOPNOTSUPP);
                break;

            case IFA_ARPType:           /* "Default is 2054." */
                if ((ULONG)item->ti_Data != (ULONG)AMI_ETHERTYPE_ARP)
                    return bsd_fail(SocketBase, AMI_EOPNOTSUPP);
                break;

            case IFA_ReportOffline:     /* "Default is FALSE." Nothing
                                           notifies, so FALSE it is. */
                if (item->ti_Data != 0)
                    return bsd_fail(SocketBase, AMI_EOPNOTSUPP);
                break;

            case IFA_PacketFilterMode:
                /*
                 * Behavioural: the mode decides what a capture can see, and a
                 * tcpdump told it was promiscuous while it sees local traffic
                 * only reports a quiet wire. PFM_Local is both the documented
                 * default and what src/bpf/ does, it taps what the stack
                 * sees. The two promiscuous modes need the device opened
                 * exclusively, which the shim never does, and PFM_Nothing asks
                 * for capture to be off on this interface, which there is no
                 * way to arrange.
                 */
                if ((LONG)item->ti_Data != PFM_Local)
                    return bsd_fail(SocketBase, AMI_EOPNOTSUPP);
                break;

            case IFA_HardwareAddress:
                /* Behavioural, and the most visible kind: the station address
                   is what every other host on the segment sees. The shim reads
                   it from S2_DEVICEQUERY and does not set one. */
                return bsd_fail(SocketBase, AMI_EOPNOTSUPP);

            /*
             * -----------------------------------------------------------
             * Advisory. Each of these tunes how the work is done and leaves
             * nothing for a caller to observe through this API or on the wire,
             * so they are accepted and ignored. Roadshow tools pass them as a
             * matter of course, and refusing meant no interface at all.
             *
             *   IFA_NumReadRequests         queue depths. The RX depth is sized
             *   IFA_NumWriteRequests        from the packet pool at open time
             *   IFA_NumARPRequests          (sana2_rx.c) and the TX ring is a
             *                               compile-time array; a different
             *                               depth is throughput under burst,
             *                               not behaviour.
             *   IFA_CopyMode                CM_FastWordCopy is "the faster data
             *                               copying code"; the bytes that come
             *                               out are the same either way.
             *   IFA_RequiresInitDelay       a settle delay for devices that
             *                               lose the first frame. Defaults to
             *                               TRUE and has never been implemented
             *                               here, so refusing TRUE would refuse
             *                               the documented default.
             * -----------------------------------------------------------
             */
            case IFA_NumReadRequests:
            case IFA_NumWriteRequests:
            case IFA_NumARPRequests:
            case IFA_CopyMode:
            case IFA_RequiresInitDelay:
                break;

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

#ifdef AMINETXDUO_IPV6
    /* This tag list has no IPv6 tag, which is the same case as a
       DEVS:NetInterfaces file with no CONFIGURE6 line, so it gets the same
       answer config_parse.c gives that case. A bzeroed AmiIfConfig would say
       OFF, and the interface would come up with no link-local address at all. */
    cfg.ip6type = AMI_IP6TYPE_AUTO;
    cfg.prefix6 = 64;
#endif

    /*
     * No address is set here, and none can be: this call's tag list has no
     * address tag. An interface arrives with no address and is given one by
     * ConfigureInterfaceTagList().
     */
    if (tags != NULL && bsd_if_parse_add(SocketBase, tags, &cfg) != 0)
        return -1;

    rc = netstack_interface_add(&cfg, &index);
    if (rc != AMI_NET_OK)
    {
        switch (rc)
        {
            case AMI_NET_ERR_NODEV:  return bsd_fail(SocketBase, AMI_ENXIO);
            case AMI_NET_ERR_DEVBAD: return bsd_fail(SocketBase, AMI_EIO);
            case AMI_NET_ERR_NOMEM:  return bsd_fail(SocketBase, AMI_ENOBUFS);
            case AMI_NET_ERR_CONFIG: return bsd_fail(SocketBase, AMI_EEXIST);
            /* No free slot: NX_MAX_PHYSICAL_INTERFACES is 2, so a user can
               hit this without there being a bug. */
            default:                 return bsd_fail(SocketBase, AMI_ENOSPC);
        }
    }

    /* IFA_LimitMTU, now that the driver's MTU is known and the interface
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
 * "success, TRUE for success, 0 for failure": the opposite of every other
 * call in this file, which are all 0 for success and -1 for failure. One page
 * apart in the same document.
 *
 * The two sources also disagree about the type. The autodoc's synopsis is
 * "BOOL RemoveInterface(STRPTR name,BOOL force)"; clib/bsdsocket_protos.h in
 * the same NDK says "LONG RemoveInterface(STRPTR interface_name, LONG force)".
 * The header decides the type, since that is what a caller compiles against;
 * the autodoc decides the values, since the header says nothing about them.
 * LONG 1 for success and 0 for failure satisfies both readings. 0-for-success,
 * which every neighbouring call uses, satisfies neither and would report
 * failure as success to every BOOL test.
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
     * anyway." EBUSY is that refusal.
     *
     * The other failure is a SANA-II device that would not give its read
     * requests back. The autodoc's wording for `force`, "memory may remain
     * allocated until you shut down the network", describes that state, and
     * this stack will not enter it: the requests point into the interface, so
     * freeing it would leave the device holding memory the system has taken
     * back. The interface stays, down and registered, and the caller gets
     * EBUSY here too, because the remedy is the one the autodoc names: shut
     * the network down.
     */
    (VOID)bsd_fail(SocketBase,
                   (rc == AMI_NET_ERR_BUSY || rc == AMI_NET_ERR_STATE)
                       ? AMI_EBUSY : AMI_EINVAL);

    return 0;
}

/*
 * BeginInterfaceConfig() and AbortInterfaceConfig() are in addralloc.c, with
 * the message they use: most of what they do is validate and reply a struct
 * AddressAllocationMessage, which is that file's subject.
 */

/* ---------------------------------------------------- the BSD ioctl half, */

/*
 * SIOCGIFCONF and the SIOCGIF* family exist here for libpcap's
 * pcap_findalldevs(), which is how `tcpdump -D` and a bare `tcpdump` with no
 * -i discover what they can capture on; it asks through these and nothing
 * else. Capture on a named interface already worked, only asking which
 * names exist returned ENOSYS, so tcpdump exited 20 with an empty file
 * (docs/RESEARCH.md 60).
 *
 * The encodings are the NDK's, not invented here. sys/sockio.h gives
 * SIOCGIFCONF as _IOWR('i',36,struct ifconf) and the rest as
 * _IOWR('i',n,struct ifreq); the static assertions below fail the build if
 * those sizes ever stop being 8 and 32, which is the only way the layout
 * below can silently go wrong.
 *
 * Watch sa_len. fad-gifc walks the SIOCGIFCONF result by striding
 * sizeof(ifr_name) + ifr_addr.sa_len rather than sizeof(struct ifreq), so an
 * entry whose sockaddr says 0 makes the walk stride 16 and read the second
 * half of the entry it has already read as a name. Every sockaddr written
 * here therefore carries its length; since sockaddr_in and sockaddr are both
 * 16 bytes on this NDK, the stride comes out at 32 either way, so the bug is
 * easy to miss.
 *
 * Only physical interfaces are listed. NetX Duo puts loopback past the
 * physical range and no BPF channel can bind to it, so listing it would offer
 * a name that cannot be captured on.
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
 * What an interface looks like to code that thinks in BSD flags. UP is
 * "configured and meant to be carrying traffic", RUNNING is "the link is
 * actually there"; libpcap prints the difference, so the two are kept
 * distinct. Same pair IFQ_State reports on and IFF_RUNNING does not, and from
 * the same two fields. Every interface here is a SANA-II Ethernet device, so
 * BROADCAST and MULTICAST are unconditional.
 */
static UWORD bsd_if_flags(const BsdIfInfo *info)
{
    UWORD flags = (UWORD)(IFF_BROADCAST | IFF_MULTICAST | IFF_SIMPLEX);

    if (info->bii_HaveSana ? info->bii_AdminUp : (info->bii_Address != 0))
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

    /* ----------------------------------------------------- SIOCGIFCONF, */
    if (req == (ULONG)SIOCGIFCONF)
    {
        struct ifconf *ifc    = (struct ifconf *)argp;
        struct ifreq  *out    = ifc->ifc_req;
        LONG           room   = ifc->ifc_len;
        LONG           used   = 0;

        /*
         * A NULL buffer asks how much room is needed. fad-gifc does not do
         * this, but other callers of this ioctl always could. Counting costs
         * one pass and removes a way to crash.
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
             * Counted whether or not it fitted, so that the grow-and-retry
             * loop callers of this ioctl write can terminate.
             */
            used += (LONG)sizeof(struct ifreq);
        }

        bsd_nx_leave(SocketBase);

        ifc->ifc_len = used;
        return 0;
    }

    /* ------------------------------------------------------- SIOCGIF*, */
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

        /* Out of the bracket before the caller's memory is touched; see
           bsd_QueryInterfaceTagList() at its own gather. */
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

/* ------------------------------------------- RFC 3493 4: interface names, */

/*
 * Indices here are 1-based, as the RFC requires and as GetRouteInfo()'s
 * rtm_index now is. bsd_if_index_of() answers in NetX's 0-based array terms,
 * so every crossing between the two is a +1 or a -1 and there are only the
 * four below. include/aminetxduo/ifindex.h is the published half.
 *
 * Loopback is in this trio and in nothing else, see BSD_LOOPBACK_IFINDEX at
 * the top of the file for why, and for why its name is "lo0".
 */

ULONG bsd_if_nametoindex(register const char *ifname __asm("a0"),
                         register struct AmiSocketBase *SocketBase __asm("a6"))
{
    NX_IP *ip = netstack_ip();
    LONG   index;

    (VOID)SocketBase;

    /* "otherwise, it shall return zero. No errors are defined.", so no
       bsd_fail() anywhere in here, not even for a NULL name. */
    if (ifname == NULL || ip == NULL)
        return 0UL;

#if BSD_HAVE_LOOPBACK_IF
    if (bsd_name_equal(BSD_LOOPBACK_IFNAME, ifname) &&
        ip->nx_ip_interface[NX_LOOPBACK_INTERFACE].nx_interface_valid != 0)
        return (ULONG)BSD_LOOPBACK_IFINDEX;
#endif

    index = bsd_if_index_of(ip, ifname);

    return (index < 0) ? 0UL : (ULONG)(index + 1);
}

char *bsd_if_indextoname(register ULONG ifindex __asm("d0"),
                         register char *ifname __asm("a0"),
                         register struct AmiSocketBase *SocketBase __asm("a6"))
{
    NX_IP *ip = netstack_ip();

    if (ifname == NULL)
    {
        (VOID)bsd_fail(SocketBase, AMI_EFAULT);
        return NULL;
    }

    if (ip == NULL)
    {
        (VOID)bsd_fail(SocketBase, AMI_ENXIO);
        return NULL;
    }

    if (!bsd_if_name_by_index(ip, ifindex, ifname, (ULONG)IF_NAMESIZE))
    {
        (VOID)bsd_fail(SocketBase, AMI_ENXIO);
        return NULL;
    }

    return ifname;
}

/*
 * One allocation, as ObtainInterfaceList() and ObtainDomainNameServerList()
 * do: the terminator needs a slot of its own, and the names sit in the same
 * block so if_freenameindex(), which is given only the array pointer, can
 * be a single free.
 */
typedef struct BsdIfNameIndex
{
    struct if_nameindex bin_Entry[NX_MAX_IP_INTERFACES + 1];
    char                bin_Name[NX_MAX_IP_INTERFACES][IF_NAMESIZE];
} BsdIfNameIndex;

struct if_nameindex *bsd_if_nameindex(register struct AmiSocketBase *SocketBase
                                          __asm("a6"))
{
    NX_IP          *ip = netstack_ip();
    BsdIfNameIndex *out;
    UWORD           used = 0;
    UINT            i;

    if (ip == NULL)
    {
        (VOID)bsd_fail(SocketBase, AMI_ENXIO);
        return NULL;
    }

    out = (BsdIfNameIndex *)ami_alloc(sizeof(BsdIfNameIndex));
    if (out == NULL)
    {
        (VOID)bsd_fail(SocketBase, AMI_ENOMEM);
        return NULL;
    }

    for (i = 0; i < (UINT)NX_MAX_PHYSICAL_INTERFACES; i++)
    {
        if (!bsd_if_name_of(ip, i, out->bin_Name[used], (ULONG)IF_NAMESIZE))
            continue;

        out->bin_Entry[used].if_index = (ULONG)(i + 1);
        out->bin_Entry[used].if_name  = out->bin_Name[used];
        used++;
    }

#if BSD_HAVE_LOOPBACK_IF
    /* Last, as it is in the array and as `ifconfig -a` prints it. */
    if (ip->nx_ip_interface[NX_LOOPBACK_INTERFACE].nx_interface_valid != 0)
    {
        bsd_strncpy(out->bin_Name[used], BSD_LOOPBACK_IFNAME, IF_NAMESIZE);
        out->bin_Entry[used].if_index = (ULONG)BSD_LOOPBACK_IFINDEX;
        out->bin_Entry[used].if_name  = out->bin_Name[used];
        used++;
    }
#endif

    /* "an if_index of 0 and an if_name of NULL". ami_alloc() zeroes, so this
       is already true of every slot past `used`; written out because the
       terminator is contract, not a side effect of the allocator. */
    out->bin_Entry[used].if_index = 0UL;
    out->bin_Entry[used].if_name  = NULL;

    return out->bin_Entry;
}

VOID bsd_if_freenameindex(register struct if_nameindex *ptr __asm("a0"),
                          register struct AmiSocketBase *SocketBase __asm("a6"))
{
    (VOID)SocketBase;

    /* bin_Entry is the first member, so the array pointer is the block. */
    if (ptr != NULL)
        ami_free(ptr);
}

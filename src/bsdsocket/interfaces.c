/*
 * bsdsocket.library, the Roadshow interface API.
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

#define BSD_IF_RESOLVE_TIMEOUT  (30UL * (ULONG)NX_IP_PERIODIC_RATE)

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

static const AmiIfConfig *bsd_if_config(UINT index)
{
    if (index >= (UINT)AMI_CFG_MAX_ATTACHED)
        return NULL;

    return netstack_iface_config((UWORD)index);
}

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

    if (list != NULL)
        ami_free(list);
}

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

    if (info->bii_NetMask != 0)
        info->bii_Broadcast = (info->bii_Address & info->bii_NetMask) |
                              ~info->bii_NetMask;

    if (cfg != NULL)
    {
        info->bii_Device = cfg->device;
        info->bii_Unit   = cfg->unit;

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
       link info. That is how the driver's own values are reached. */
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
        return bsd_fail(SocketBase, AMI_ENXIO);
    }

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
                if (info.bii_HaveSana)
                    bsd_put_long(item, (LONG)info.bii_Info.address_bits);
                break;

            case IFQ_HardwareAddress:
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
                if (info.bii_HaveSana)
                    bsd_put_long(item, (LONG)info.bii_HardwareMTU);
                break;

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
                bsd_put_sockaddr_in(item, 0);
                break;

            case IFQ_State:
                bsd_put_long(item,
                             (info.bii_HaveSana ? info.bii_AdminUp
                                                : info.bii_LinkUp)
                                 ? SM_Up : SM_Down);
                break;

            case IFQ_AddressBindType:
                bsd_put_long(item, info.bii_BindType);
                break;

            case IFQ_Metric:
                bsd_put_long(item, 0);
                break;

            case IFQ_GetDebugMode:
                bsd_put_long(item, FALSE);
                break;

            default:
                break;
        }
    }

    return 0;
}

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

static BOOL bsd_if_parse_address(const char *text, ULONG *out)
{
    if (text == NULL || text[0] == '\0')
        return FALSE;

    if (ami_config_parse_ip(text, out))
        return TRUE;

    return (netstack_resolve(text, out, BSD_IF_RESOLVE_TIMEOUT) == AMI_NET_OK)
               ? TRUE : FALSE;
}

static ULONG bsd_if_classful_mask(ULONG addr)
{
    if ((addr & 0x80000000UL) == 0)
        return 0xFF000000UL;                    /* class A */
    if ((addr & 0xC0000000UL) == 0x80000000UL)
        return 0xFFFF0000UL;                    /* class B */

    return 0xFFFFFF00UL;                        /* class C and everything else */
}

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
 * Pass one. Returns 0, or -1 with errno set. Nothing is applied to the
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
                break;

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
                if ((LONG)item->ti_Data != 0)
                    return bsd_fail(SocketBase, AMI_EOPNOTSUPP);
                break;

            case IFC_DestinationAddress:
            case IFC_BroadcastAddress:
            case IFC_AddAliasAddress:
            case IFC_DeleteAliasAddress:
                return bsd_fail(SocketBase, AMI_EOPNOTSUPP);

            default:
                break;
        }
    }

    return 0;
}

LONG bsd_ConfigureInterfaceTagList(register STRPTR name __asm("a0"),
                                   register struct TagItem *tags __asm("a1"),
                                   register struct AmiSocketBase *SocketBase __asm("a6"))
{
    NX_IP          *ip;
    BsdIfConfigReq  req;
    NX_INTERFACE   *nxif;
    UWORD           index;
    LONG            rc;
    LONG            result = 0;
    UINT            status;

    if (name == NULL)
        return bsd_fail(SocketBase, AMI_EINVAL);

    if (bsd_strlen((const char *)name) >= (ULONG)BSD_IFNAME_SIZE)
        return bsd_fail(SocketBase, AMI_EINVAL);

    if (netstack_ip() == NULL)
        return bsd_fail(SocketBase, AMI_ENETDOWN);

    rc = netstack_interface_claim((const char *)name, &index);
    if (rc != AMI_NET_OK)
    {
        if (rc == AMI_NET_ERR_STATE)
            return bsd_fail(SocketBase, AMI_ENETDOWN);
        return bsd_fail(SocketBase, AMI_ENXIO);
    }

    ip = netstack_ip();

    if (tags == NULL)
        goto out;

    if (bsd_if_parse_config(SocketBase, tags, &req) != 0)
    {
        result = -1;
        goto out;
    }

    nxif = &ip->nx_ip_interface[index];

    if (req.bcr_HaveState && req.bcr_State == SM_Online &&
        netstack_interface_up(index) != AMI_NET_OK)
    {
        result = bsd_fail(SocketBase, AMI_ENXIO);
        goto out;
    }

    if (req.bcr_HaveMTU)
    {
        AmiSana2If *sana;
        ULONG       limit = req.bcr_MTU;
        ULONG       hardware;

        if (bsd_nx_enter(SocketBase) != 0)
        {
            result = bsd_fail(SocketBase, AMI_ENETDOWN);
            goto out;
        }

        sana     = (AmiSana2If *)nxif->nx_interface_additional_link_info;
        hardware = (sana != NULL) ? ami_sana2_get_mtu(sana) : 0;

        if (hardware != 0 && limit > hardware)
            limit = hardware;

        status = nx_ip_interface_mtu_set(ip, (UINT)index, limit);

        bsd_nx_leave(SocketBase);

        if (status != NX_SUCCESS)
        {
            result = bsd_fail(SocketBase, AMI_EINVAL);
            goto out;
        }
    }

    if (req.bcr_HaveAddress || req.bcr_HaveNetMask)
    {
        if (bsd_if_set_address(SocketBase, index,
                               req.bcr_HaveAddress, req.bcr_Address,
                               req.bcr_HaveNetMask, req.bcr_NetMask) != 0)
        {
            result = -1;
            goto out;
        }
    }

    if (req.bcr_HaveState && req.bcr_State != SM_Online)
    {
        if (req.bcr_State == SM_Up)
            rc = netstack_interface_up(index);
        else if (req.bcr_State == SM_Down)
            rc = netstack_interface_stack_down(index);
        else
            rc = netstack_interface_down(index);

        if (rc != AMI_NET_OK)
        {
            result = bsd_fail(SocketBase, AMI_ENXIO);
            goto out;
        }
    }

out:
    netstack_interface_release(index);
    return result;
}

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
                if (item->ti_Data == 0)
                    return bsd_fail(SocketBase, AMI_EINVAL);
                cfg->mtu = (ULONG)item->ti_Data;
                break;

            case IFA_DownGoesOffline:
                cfg->down_goes_offline = (item->ti_Data != 0) ? TRUE : FALSE;
                break;

            case IFA_RequiresInitDelay:
                /* Passed through to ami_sana2_open(), which applies the
                   documented one-second delay after configuring the device
                   and before starting readers or sending the first packet. */
                cfg->requires_init_delay =
                    (item->ti_Data != 0) ? TRUE : FALSE;
                break;

            case IFA_SetDebugMode:
                if (item->ti_Data != 0)
                    return bsd_fail(SocketBase, AMI_EOPNOTSUPP);
                break;

            case IFA_Multicast:
            case IFA_PointToPoint:
                if ((item->ti_Tag == IFA_Multicast) != (item->ti_Data != 0))
                    return bsd_fail(SocketBase, AMI_EOPNOTSUPP);
                break;

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
                if ((LONG)item->ti_Data != PFM_Local)
                    return bsd_fail(SocketBase, AMI_EOPNOTSUPP);
                break;

            case IFA_HardwareAddress:
            {
                const UBYTE *address = (const UBYTE *)item->ti_Data;
                UWORD        i;

                if (address == NULL)
                    return bsd_fail(SocketBase, AMI_EINVAL);

                for (i = 0; i < (UWORD)AMI_CFG_MAC_SIZE; i++)
                    cfg->hw_address[i] = address[i];
                cfg->have_hw_address = TRUE;
                break;
            }

            case IFA_NumReadRequests:
            case IFA_NumWriteRequests:
            case IFA_NumARPRequests:
            case IFA_CopyMode:
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
    cfg.ip6type = AMI_IP6TYPE_AUTO;
    cfg.prefix6 = 64;
#endif

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
            default:                 return bsd_fail(SocketBase, AMI_ENOSPC);
        }
    }

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

/*
 * "success, TRUE for success, 0 for failure": the opposite of every other
 * call in this file, which are all 0 for success and -1 for failure. One page
 * apart in the same document.
 */
LONG bsd_RemoveInterface(register STRPTR name __asm("a0"),
                         register LONG force __asm("d0"),
                         register struct AmiSocketBase *SocketBase __asm("a6"))
{
    LONG   rc;

    if (name == NULL ||
        bsd_strlen((const char *)name) >= (ULONG)BSD_IFNAME_SIZE)
    {
        (VOID)bsd_fail(SocketBase, AMI_EINVAL);
        return 0;
    }

    if (netstack_ip() == NULL)
    {
        (VOID)bsd_fail(SocketBase, AMI_ENETDOWN);
        return 0;
    }

    /* Physical slots are reusable, so resolve and remove under one lock. */
    rc = netstack_interface_remove_named((const char *)name,
                                         (force != 0) ? TRUE : FALSE);
    if (rc == AMI_NET_OK)
        return 1;

    if (rc == AMI_NET_ERR_NONAME)
    {
        (VOID)bsd_fail(SocketBase, AMI_ENXIO);
        return 0;
    }

    (VOID)bsd_fail(SocketBase,
                   (rc == AMI_NET_ERR_BUSY || rc == AMI_NET_ERR_STATE)
                       ? AMI_EBUSY : AMI_EINVAL);

    return 0;
}

/*
 * Every sockaddr written into a SIOCGIFCONF entry must carry sa_len: libpcap's
 * fad-gifc strides by sizeof(ifr_name) + ifr_addr.sa_len, not sizeof(ifreq).
 */

AMI_STATIC_ASSERT(sizeof(struct ifreq) == 32,  "struct ifreq is the NDK's");
AMI_STATIC_ASSERT(sizeof(struct ifconf) == 8,  "struct ifconf is the NDK's");
AMI_STATIC_ASSERT(sizeof(struct sockaddr_in) == 16, "sockaddr_in is 4.4BSD's");
AMI_STATIC_ASSERT(IOCPARM_LEN(SIOCGIFCONF) == 8,   "SIOCGIFCONF parameter");
AMI_STATIC_ASSERT(IOCPARM_LEN(SIOCGIFADDR) == 32,  "SIOCGIFADDR parameter");

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

    if (req == (ULONG)SIOCGIFCONF)
    {
        struct ifconf *ifc    = (struct ifconf *)argp;
        struct ifreq  *out    = ifc->ifc_req;
        LONG           room     = ifc->ifc_len;
        LONG           required = 0;
        LONG           written  = 0;

        if (bsd_nx_enter(SocketBase) != 0)
            return bsd_fail(SocketBase, AMI_ENETDOWN);

        for (i = 0; i < (UINT)NX_MAX_PHYSICAL_INTERFACES; i++)
        {
            char name[BSD_IFNAME_SIZE];

            if (!bsd_if_name_of(ip, i, name, sizeof(name)))
                continue;

            if (out != NULL &&
                written + (LONG)sizeof(struct ifreq) <= room)
            {
                struct ifreq *ifr =
                    (struct ifreq *)((UBYTE *)out + written);

                bsd_bzero(ifr, sizeof(*ifr));
                bsd_strncpy((char *)ifr->ifr_name, name,
                            sizeof(ifr->ifr_name));
                bsd_if_put_addr(&ifr->ifr_addr,
                                ip->nx_ip_interface[i].nx_interface_ip_address);
                written += (LONG)sizeof(struct ifreq);
            }

            required += (LONG)sizeof(struct ifreq);
        }

        bsd_nx_leave(SocketBase);

        ifc->ifc_len = (out == NULL) ? required : written;
        return 0;
    }

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

/*
 * Indices here are 1-based, as the RFC requires and as GetRouteInfo()'s
 * rtm_index now is. bsd_if_index_of() answers in NetX's 0-based array terms,
 * so every crossing between the two is a +1 or a -1 and there are only the
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
    if (ip->nx_ip_interface[NX_LOOPBACK_INTERFACE].nx_interface_valid != 0)
    {
        bsd_strncpy(out->bin_Name[used], BSD_LOOPBACK_IFNAME, IF_NAMESIZE);
        out->bin_Entry[used].if_index = (ULONG)BSD_LOOPBACK_IFINDEX;
        out->bin_Entry[used].if_name  = out->bin_Name[used];
        used++;
    }
#endif

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

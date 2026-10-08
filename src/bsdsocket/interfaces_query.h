/* Private interface-query snapshot and scalar replies. SPDX-License-Identifier: MIT */
#ifndef AMINETXDUO_INTERFACES_QUERY_H
#define AMINETXDUO_INTERFACES_QUERY_H

#include "aminetxduo/sana2.h"
#include <libraries/bsdsocket.h>
#include <stddef.h>

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

enum
{
    BSD_IFQ_ALWAYS = 1,
    BSD_IFQ_DEVICE = 2,
    BSD_IFQ_SANA   = 4,
    BSD_IFQ_IP     = 8,
    BSD_IFQ_ARP    = 16
};

/* Only LONG/ULONG fields belong here. Signed and unsigned corresponding
   types may alias, and both caller representations receive the same bits. */
static const struct BsdIfScalar
{
    UBYTE tag;
    UBYTE require;
    UWORD offset;
}
bsd_if_scalars[] =
{
#define IF_SCALAR(tag, field, require) \
    { (UBYTE)(IFQ_##tag - IFQ_BASE), BSD_IFQ_##require, \
      (UWORD)offsetof(BsdIfInfo, field) }
    IF_SCALAR(DeviceUnit, bii_Unit, DEVICE),
    IF_SCALAR(HardwareAddressSize, bii_Info.address_bits, SANA),
    IF_SCALAR(HardwareType, bii_Info.hardware_type, SANA),
    IF_SCALAR(BPS, bii_BPS, SANA),
    IF_SCALAR(MTU, bii_MTU, ALWAYS),
    IF_SCALAR(HardwareMTU, bii_HardwareMTU, SANA),
    IF_SCALAR(PacketsReceived, bii_Stats.packets_received, SANA),
    IF_SCALAR(PacketsSent, bii_Stats.packets_sent, SANA),
    IF_SCALAR(BadData, bii_Stats.bad_data, SANA),
    IF_SCALAR(Overruns, bii_Stats.overruns, SANA),
    IF_SCALAR(UnknownTypes, bii_Stats.unknown_types, SANA),
    IF_SCALAR(InputErrors, bii_Stats.rx_errors, SANA),
    IF_SCALAR(OutputErrors, bii_Stats.tx_errors, SANA),
    IF_SCALAR(InputDrops, bii_Stats.alloc_failures, SANA),
    IF_SCALAR(IPDrops, bii_IpDrops, IP),
    IF_SCALAR(ARPDrops, bii_ArpDrops, ARP),
    IF_SCALAR(NumReadRequests, bii_Info.read_requests, SANA),
    IF_SCALAR(NumReadRequestsPending, bii_Info.read_pending, SANA),
    IF_SCALAR(NumWriteRequests, bii_Info.write_requests, SANA),
    IF_SCALAR(NumWriteRequestsPending, bii_Info.write_pending, SANA),
    IF_SCALAR(AddressBindType, bii_BindType, ALWAYS),
#undef IF_SCALAR
};

_Static_assert(sizeof(BsdIfInfo) <= 65535UL,
               "interface snapshot offsets must fit in UWORD");
_Static_assert(IFQ_ARPDrops - IFQ_BASE <= 255UL,
               "scalar interface tag offsets must fit in UBYTE");

static ULONG bsd_if_query_available(const BsdIfInfo *info)
{
    return BSD_IFQ_ALWAYS |
           (info->bii_Device != NULL ? BSD_IFQ_DEVICE : 0) |
           (info->bii_HaveSana ? BSD_IFQ_SANA : 0) |
           (info->bii_HaveIpDrops ? BSD_IFQ_IP : 0) |
           (info->bii_HaveArpDrops ? BSD_IFQ_ARP : 0);
}

/* TRUE means this is a scalar tag, even if no value is available. Full
   ULONG comparison prevents unknown tags aliasing a truncated table key. */
static BOOL bsd_if_query_scalar(const BsdIfInfo *info,
                                const struct TagItem *item, ULONG available)
{
    ULONG i;

    for (i = 0; i < sizeof(bsd_if_scalars) / sizeof(bsd_if_scalars[0]); i++)
    {
        const struct BsdIfScalar *field = &bsd_if_scalars[i];

        if (item->ti_Tag == (ULONG)IFQ_BASE + field->tag)
        {
            ULONG *out = (ULONG *)item->ti_Data;

            if (out != NULL && (available & field->require) != 0)
                *out = *(const ULONG *)((const char *)info + field->offset);
            return TRUE;
        }
    }
    return FALSE;
}

#endif

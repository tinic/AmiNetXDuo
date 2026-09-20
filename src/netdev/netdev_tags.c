/*
 * anxnet.device: OpenDevice tag parsing and private ABI negotiation.
 *
 * Kept out of the romtag translation unit so the version, size and feature
 * rules can be host-tested without linking a device image.
 *
 * SPDX-License-Identifier: MIT
 */

#include "netdev_internal.h"
#include "aminetxduo/anxnet.h"
#include "aminetxduo/anxs2ext.h"

VOID netdev_take_tags(const struct TagItem *tags, NetdevOpener *op,
                      const char **pin, AnxdS2Extension **ext_answer)
{
    while (tags != NULL)
    {
        ULONG tag = tags->ti_Tag;

        if (tag == TAG_DONE)
            break;
        if (tag == TAG_MORE)
        {
            tags = (const struct TagItem *)tags->ti_Data;
            continue;
        }
        if (tag == TAG_IGNORE)
        {
            tags++;
            continue;
        }
        if (tag == TAG_SKIP)
        {
            tags += 1 + (LONG)tags->ti_Data;
            continue;
        }

        if (tag == S2_CopyToBuff)
            op->op_CopyTo = (APTR)tags->ti_Data;
        else if (tag == ANXD_S2_EXTENSION && tags->ti_Data != 0)
        {
            AnxdS2Extension *ext = (AnxdS2Extension *)tags->ti_Data;

            if (ext->Version == ANXD_S2_ABI_VERSION &&
                ext->Size >= (UWORD)sizeof(*ext))
            {
                ULONG request = ext->Request;

                ext->Accepted = 0;
                op->op_Anxd = 1;
                *ext_answer = ext;

                if ((request & ANXD_S2F_RX_DIRECT) != 0 &&
                    ext->RxDirect != NULL && ext->RxFilled != NULL)
                {
                    op->op_RxDirect = (APTR)ext->RxDirect;
                    op->op_RxFilled = (APTR)ext->RxFilled;
                    op->op_RxLinkHdr =
                        (UBYTE)((request & ANXD_S2F_RX_LINK_HDR) != 0);
                }
                if ((request & ANXD_S2F_RX_VERIFIED) != 0)
                    op->op_RxFlags = ANXD_S2_RXF_VERIFIED;
                if ((request & ANXD_S2F_TX_CSUM_TCP) != 0)
                    op->op_TxCsum |= ANXD_S2_TXF_TCP;
                if ((request & ANXD_S2F_TX_CSUM_UDP) != 0)
                    op->op_TxCsum |= ANXD_S2_TXF_UDP;
            }
        }
        else if (tag == S2_CopyFromBuff)
            op->op_CopyFrom = (APTR)tags->ti_Data;
        else if (tag == S2_PacketFilter)
            op->op_Filter = (APTR)tags->ti_Data;
        else if (tag == ANXD_S2_CARD_TYPE)
            *pin = (const char *)tags->ti_Data;

        tags++;
    }
}

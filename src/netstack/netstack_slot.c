/*
 * AmiNetXDuo, which interface slot a newcomer lands in.
 *
 * SPDX-License-Identifier: MIT
 */

#include "netstack_slot.h"

LONG ami_ns_slot_vacant(const AmiNsSlot *slot, UWORD count)
{
    UWORD i;

    if (slot == NULL)
        return -1;

    for (i = 0; i < count; i++)
    {
        if (!slot[i].attached && !slot[i].held && slot[i].claims == 0)
            return (LONG)i;
    }

    return -1;
}

LONG ami_ns_slot_yield_candidate(const AmiNsSlot *slot, UWORD count)
{
    LONG i;

    if (slot == NULL)
        return -1;

    for (i = (LONG)count - 1; i >= 0; i--)
    {
        if (slot[i].held && !slot[i].wanted && slot[i].claims == 0)
            return i;
    }

    return -1;
}

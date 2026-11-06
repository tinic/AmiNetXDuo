/*
 * AmiNetXDuo SANA-II callback ABI trampolines.
 *
 * SPDX-License-Identifier: MIT
 */

#include "netdev_internal.h"
#include "aminetxduo/foreign_call.h"

/* The opener's S2_CopyToBuff / S2_CopyFromBuff, with a6 clobbered
   (aminetxduo/foreign_call.h). */
BOOL netdev_copy_call(APTR fn, APTR to, APTR from, ULONG len)
{
    if (fn == NULL)
        return FALSE;

    return (BOOL)(ami_call_copy(fn, to, from, len) != 0);
}

/* The opener's S2_PacketFilter Hook. */
BOOL netdev_hook_call(APTR hook, APTR object, APTR message)
{
    if (hook == NULL)
        return TRUE;

    return (BOOL)(ami_call_hook((struct Hook *)hook, object, message) != 0);
}

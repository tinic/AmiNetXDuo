/*
 * AmiNetXDuo -- SANA-II buffer-management hooks.
 *
 * OpenDevice() is handed a tag list carrying these two functions. The device
 * calls them, in m68k register convention (a0 = to, a1 = from, d0 = length),
 * whenever it needs to move packet data in or out of "the abstract data
 * structure" -- which for us is an NX_PACKET, reached through the AmiRxSlot or
 * AmiTxSlot that ios2_Data points at. That is the whole point of the tags:
 * one copy, straight into the packet, no bounce buffer.
 *
 * Constraints from copybuff.doc: these run at interrupt level. No exec memory
 * calls, no logging, no stack checking, nothing that Forbid()s. They are pure
 * pointer arithmetic and a copy loop.
 *
 * SPDX-License-Identifier: MIT
 */

#include "sana2_internal.h"

/*
 * Longword copy loop. Deliberately local rather than newlib's memcpy: this is
 * called from an interrupt, and a shared library should not be relying on the
 * C library's choice of implementation there. The 68020 handles misaligned
 * longword access, but it costs an extra bus cycle, so align the destination
 * first and fall back to bytes when source and destination disagree.
 */
VOID ami_sana2_copy_bytes(UCHAR *to, const UCHAR *from, ULONG len)
{
    if (((ULONG)to & 1UL) == ((ULONG)from & 1UL))
    {
        /* Bring both onto an even address, then onto a longword boundary. */
        while (len != 0 && (((ULONG)to & 3UL) != 0))
        {
            *to++ = *from++;
            len--;
        }

        while (len >= 16)
        {
            *(ULONG *)to       = *(const ULONG *)from;
            *(ULONG *)(to + 4) = *(const ULONG *)(from + 4);
            *(ULONG *)(to + 8) = *(const ULONG *)(from + 8);
            *(ULONG *)(to + 12) = *(const ULONG *)(from + 12);
            to   += 16;
            from += 16;
            len  -= 16;
        }

        while (len >= 4)
        {
            *(ULONG *)to = *(const ULONG *)from;
            to   += 4;
            from += 4;
            len  -= 4;
        }
    }

    while (len != 0)
    {
        *to++ = *from++;
        len--;
    }
}

/*
 * S2_CopyToBuff: the device hands us `len` bytes of freshly received frame in
 * contiguous memory. `to` is our CMD_READ's ios2_Data, i.e. the AmiRxSlot,
 * whose NX_PACKET was allocated and positioned before the read was posted.
 *
 * In cooked mode dst already points 14 bytes into the packet, leaving room for
 * the Ethernet header sana2_rx.c synthesises from ios2_SrcAddr/DstAddr/
 * PacketType. In raw mode dst is the start of the frame.
 */
BOOL ami_sana2_copy_to_buff(register APTR to    __asm("a0"),
                            register APTR from  __asm("a1"),
                            register ULONG len  __asm("d0"))
{
    AmiRxSlot *slot = (AmiRxSlot *)to;

    if (slot == NULL || from == NULL)
        return FALSE;

    if (slot->packet == NULL || slot->dst == NULL || len > slot->capacity)
        return FALSE;

    ami_sana2_copy_bytes(slot->dst, (const UCHAR *)from, len);
    slot->copied = len;

    return TRUE;
}

/*
 * S2_CopyFromBuff: the device wants `len` bytes of the frame we are sending.
 * `from` is our CMD_WRITE's ios2_Data, i.e. the AmiTxSlot.
 *
 * A device may take the frame in one call or in several, and may restart the
 * whole transfer if it has to retry the wire. The cursor below handles the
 * chunked case; the reset conditions handle the retry case: any request that
 * cannot be satisfied from where the cursor stands rewinds to the start of the
 * packet first. For the overwhelmingly common single-buffer, single-call frame
 * both are no-ops and this degenerates to one memcpy from the prepend pointer.
 */
BOOL ami_sana2_copy_from_buff(register APTR to   __asm("a0"),
                              register APTR from __asm("a1"),
                              register ULONG len __asm("d0"))
{
    AmiTxSlot *slot = (AmiTxSlot *)from;
    UCHAR     *out  = (UCHAR *)to;
    NX_PACKET *cur;
    ULONG      off;

    if (slot == NULL || out == NULL || slot->packet == NULL)
        return FALSE;

    if (len > slot->total)
        return FALSE;

    /* Rewind on a restart, or when the cursor cannot cover the request. */
    if (slot->cursor == NULL || slot->consumed >= slot->total ||
        len > (slot->total - slot->consumed))
    {
        slot->cursor     = slot->packet;
        slot->cursor_off = 0;
        slot->consumed   = 0;
    }

    cur = slot->cursor;
    off = slot->cursor_off;

    while (len != 0 && cur != NULL)
    {
        ULONG have = (ULONG)(cur->nx_packet_append_ptr - cur->nx_packet_prepend_ptr);
        ULONG take;

        if (off >= have)
        {
#ifndef NX_DISABLE_PACKET_CHAIN
            cur = cur->nx_packet_next;
#else
            cur = NX_NULL;
#endif
            off = 0;
            continue;
        }

        take = have - off;
        if (take > len)
            take = len;

        ami_sana2_copy_bytes(out, cur->nx_packet_prepend_ptr + off, take);

        out            += take;
        off            += take;
        len            -= take;
        slot->consumed += take;
    }

    slot->cursor     = cur;
    slot->cursor_off = off;

    return (len == 0) ? TRUE : FALSE;
}

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

#include "net68k.h"

/*
 * The copy loop, still deliberately not newlib's memcpy: this runs at
 * interrupt level and a shared library should not depend on the C library's
 * choice of implementation there.
 *
 *     if (((ULONG)to & 1UL) == ((ULONG)from & 1UL))
 *
 * -- i.e. it took its fast path only when source and destination agreed mod 2,
 * and copied One byte per iteration when they did not.  Measured on the
 * emulated 68020 over 1460 bytes (tests/perf/perf_test.c):
 *
 *     parities agree     240.3 ns/B
 *     parities differ   1203.4 ns/B      -- a 5.0x cliff
 *
 * sitting on every frame whose driver buffer happened to land on the wrong
 * parity.  Nothing in SANA-II promises anything about a device's buffer
 * parity, so avoiding that was luck rather than design.  Worth recording
 * because the cliff was expected to be in newlib's memcpy and is not: the
 * libm020 memcpy this toolchain links costs 216 ns/B aligned and 252-260
 * misaligned, an 18% penalty with no cliff at all.
 *
 * n68k_copy_bytes() has no such condition either: it brings the DESTINATION
 * to a longword boundary and then moves longwords whatever the source is
 * doing, which is what the 68020 supports.  179.5 ns/B when the parities
 * agree (1.34x on what used to be the fast path) and 228.7 when they differ
 * (5.3x on what used to be the slow one), because movem.l moves eight
 * longwords per instruction pair.  See src/net68k/n68k_copy.S.
 */
VOID ami_sana2_copy_bytes(UCHAR *to, const UCHAR *from, ULONG len)
{
    n68k_copy_bytes(to, from, len);
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

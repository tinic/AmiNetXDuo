/*
 * AmiNetXDuo, SANA-II buffer-management hooks.
 *
 * OpenDevice() is handed a tag list carrying these two functions. The device
 * calls them, in m68k register convention (a0 = to, a1 = from, d0 = length),
 * whenever it needs to move packet data in or out of "the abstract data
 * structure", here an NX_PACKET reached through the AmiRxSlot or AmiTxSlot
 * that ios2_Data points at. One copy, straight into the packet, no bounce
 * buffer.
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
 * The copy loop is not newlib's memcpy: this runs at interrupt level and a
 * shared library should not depend on the C library's implementation there.
 *
 * The earlier loop took its fast path only when source and destination agreed
 * mod 2, and copied one byte per iteration otherwise.  Measured on the
 * emulated 68020 over 1460 bytes (tests/perf/perf_test.c):
 *
 *     parities agree     240.3 ns/B
 *     parities differ   1203.4 ns/B, a 5.0x cliff
 *
 * on every frame whose driver buffer landed on the wrong parity, and SANA-II
 * promises nothing about a device's buffer parity.  For comparison, the
 * libm020 memcpy this toolchain links costs 216 ns/B aligned and 252-260
 * misaligned: an 18% penalty with no cliff.
 *
 * n68k_copy_bytes() brings the destination to a longword boundary and then
 * moves longwords whatever the source is doing, which is what the 68020
 * supports: 179.5 ns/B when the parities agree (1.34x the old fast path) and
 * 228.7 when they differ (5.3x the old slow path), because movem.l moves
 * eight longwords per instruction pair.  See src/net68k/n68k_copy.S.
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
 * A device may take the frame in one call or several, and may restart the
 * whole transfer if it has to retry the wire. The cursor below handles the
 * chunked case; the reset conditions handle the retry case, rewinding to the
 * start of the packet for any request the cursor cannot satisfy from where it
 * stands. For the common single-buffer, single-call frame both are no-ops and
 * this reduces to one copy from the prepend pointer.
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

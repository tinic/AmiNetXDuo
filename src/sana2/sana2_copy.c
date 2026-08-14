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

/* _nx_ip_packet_checksum_compute(): the deferred path a declined fusion
   hands the packet to. */
#include "nx_ip.h"

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
    N68K_COPY_BYTES(to, from, len);
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

#ifdef AMINETXDUO_RX_VERIFY
    /*
     * Sum while copying, when both ends are WORD aligned.  The device has to
     * be copied out of anyway, so the sum comes out of loads already being
     * paid for; ami_sana2_rx_deliver() then verifies from it instead of
     * walking the frame a second time.
     *
     * WORD, not longword, and the difference is the whole path.  The two ends
     * are permanently two bytes out of phase: ami_sana2_rx_arm() puts the
     * destination at data_start + AMI_SANA2_RX_PAD + AMI_ETH_HEADER_SIZE,
     * 2 + 14, which is longword aligned and is what the pad is for, while the
     * device hands over its payload from behind a 14-byte header in an
     * aligned ring entry, two off.  A gate of `& 3` therefore never passed in
     * cooked mode: tools/profiler over an A600 fitz transfer, 2026-08-06, put
     * n68k_copy_bytes() at 12.9% of the machine and n68k_copy_sum_longwords()
     * at zero samples, with the frame walked again afterwards for a sum the
     * copy could have produced.
     *
     * A 68000 raises an address error on an ODD word or longword access, not
     * on a merely 4-misaligned one, and its bus is 16 bits wide, so a
     * longword move is two word cycles whatever its alignment.  68020 and
     * 68030 take misaligned accesses in hardware.  So `& 1` is the real
     * requirement, and if a device ever hands over an odd pointer this still
     * declines to the byte copy.
     *
     * The answer goes in the SLOT, not in the packet: the drain holds the same
     * slot when it delivers, and the slot is re-armed before it is reused, so
     * there is no lifetime here that is not already the reader's.
     */
    slot->summed = FALSE;

    /* The slot knows its reader and the reader knows the interface, which is
       where a counter a user can read has to live. */
    if (slot->owner != NULL && slot->owner->iface != NULL)
        slot->owner->iface->stats.rx_copy_hook++;

    if ((((ALIGN_TYPE)slot->dst | (ALIGN_TYPE)from) & 1) == 0)
    {
        ULONG   words =  len >> 2;
        ULONG   tail  =  len & 3UL;

        slot->sum = N68K_COPY_SUM_LONGWORDS((ULONG *)slot->dst,
                                            (const ULONG *)from, words);

        if (tail != 0UL)
        {
            const UCHAR    *s =  (const UCHAR *)from + (words << 2);
            UCHAR          *d =  slot->dst + (words << 2);
            ULONG           i;
            union { ULONG l; UCHAR b[4]; } w;

            /*
             * Past `len` the device's buffer holds the previous frame, so the
             * last longword is built from the bytes that arrived and padded
             * with zeroes, which is what a walk does with a partial trailing
             * longword.  Assembled through the byte array: what matters is
             * where the bytes sit, not what they are worth.
             */
            w.l = 0UL;
            for (i = 0UL; i < tail; i++)
            {
                d[i]   = s[i];
                w.b[i] = s[i];
            }

            slot->sum += w.l;
            if (slot->sum < w.l)
                slot->sum++;                    /* end-around carry */
        }

        slot->summed = TRUE;
        if (slot->owner != NULL && slot->owner->iface != NULL)
            slot->owner->iface->stats.rx_copy_summed++;
        slot->copied = len;

        return TRUE;
    }
#endif

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
/*
 * Copy one whole TCP frame into `out`, summing as it goes, and write the
 * checksum into the device's buffer.  TRUE if it did; FALSE leaves the packet
 * untouched for the caller to hand to NetX Duo.
 *
 * The pseudo-header is the source and destination addresses, the protocol and
 * the TCP length, which is what RFC 793 puts in front of the segment; the
 * segment itself is summed with its checksum field still zero, which is the
 * identity for a ones-complement sum and is what the send path left there.
 */
/*
 * Copy `len` bytes and return the ones-complement accumulator of what was
 * copied, in the convention n68k_copy_sum_longwords() uses: the caller folds,
 * and a partial trailing longword is padded with zeroes exactly as a walk
 * over the same bytes pads it.
 */
static ULONG ami_sana2_copy_sum(UCHAR *to, const UCHAR *from, ULONG len)
{
    union { ULONG l; UCHAR b[4]; } w;
    ULONG words, tail, sum, i, k, n;

    if ((((ALIGN_TYPE)to | (ALIGN_TYPE)from) & 1) != 0)
    {
        /* Odd on one side, where a 68000 permits no word access at all.  The
           pools this driver copies between are longword aligned, so this is
           unreachable today and exists so the answer does not depend on that
           staying true. */
        ami_sana2_copy_bytes(to, from, len);

        sum = 0UL;
        for (i = 0UL; i < len; i += 4UL)
        {
            n = len - i;
            if (n > 4UL)
                n = 4UL;

            w.l = 0UL;
            for (k = 0UL; k < n; k++)
                w.b[k] = from[i + k];

            sum += w.l;
            if (sum < w.l)
                sum++;
        }
        return sum;
    }

    words = len >> 2;
    tail  = len & 3UL;

    sum = N68K_COPY_SUM_LONGWORDS((ULONG *)(void *)to,
                                  (const ULONG *)(const void *)from, words);

    if (tail != 0UL)
    {
        const UCHAR *sp = from + (words << 2);
        UCHAR       *dp = to + (words << 2);

        w.l = 0UL;
        for (i = 0UL; i < tail; i++)
        {
            dp[i]  = sp[i];
            w.b[i] = sp[i];
        }

        sum += w.l;
        if (sum < w.l)
            sum++;
    }

    return sum;
}

static BOOL ami_sana2_tx_fuse_checksum(AmiTxSlot *slot, UCHAR *out, ULONG len)
{
    NX_PACKET *pkt = slot->packet;
    const UCHAR *ip;
    ULONG        ihl, total, tcp_len, sum;
    UCHAR       *csum;

    if (slot->iface == NULL || slot->hdr_len != 0)
        return FALSE;                   /* the frame would start at Ethernet */

    if (len != slot->total)
        return FALSE;                   /* not the whole frame in one call   */

#ifndef NX_DISABLE_PACKET_CHAIN
    if (pkt->nx_packet_next != NX_NULL)
        return FALSE;                   /* chained: one contiguous run only  */
#endif

    ip = (const UCHAR *)pkt->nx_packet_prepend_ptr;

    if (len < 40 || (ip[0] & 0xF0) != 0x40)
        return FALSE;                   /* not IPv4                          */

    ihl = (ULONG)(ip[0] & 0x0F) * 4UL;
    if (ihl < 20 || ihl + 20 > len)
        return FALSE;

    if (ip[9] != 6)                     /* not TCP                           */
        return FALSE;

    if ((((ULONG)ip[6] << 8) | ip[7]) & 0x3FFF)
        return FALSE;                   /* a fragment has no whole segment   */

    total = ((ULONG)ip[2] << 8) | ip[3];
    if (total > len || total < ihl + 20)
        return FALSE;                   /* padded or malformed               */

    tcp_len = total - ihl;

    /* Copy and sum in one pass; the IP header is copied but not summed. */
    ami_sana2_copy_bytes(out, ip, ihl);
    sum = ami_sana2_copy_sum(out + ihl, ip + ihl, tcp_len);

    /* Anything past the datagram is padding the device wants but the
       checksum does not cover. */
    if (len > total)
        ami_sana2_copy_bytes(out + total, ip + total, len - total);

    /* Fold what the copy accumulated to sixteen bits before the
       pseudo-header goes on top of it. */
    while (sum >> 16)
        sum = (sum & 0xFFFFUL) + (sum >> 16);

    /* The pseudo-header: addresses, protocol, TCP length. */
    sum += ((ULONG)ip[12] << 8) | ip[13];
    sum += ((ULONG)ip[14] << 8) | ip[15];
    sum += ((ULONG)ip[16] << 8) | ip[17];
    sum += ((ULONG)ip[18] << 8) | ip[19];
    sum += 6UL;
    sum += tcp_len;

    while (sum >> 16)
        sum = (sum & 0xFFFFUL) + (sum >> 16);

    sum = (~sum) & 0xFFFFUL;
    if (sum == 0UL)
        sum = 0xFFFFUL;                 /* TCP has no "no checksum" value    */

    csum = out + ihl + 16;
    csum[0] = (UCHAR)(sum >> 8);
    csum[1] = (UCHAR)(sum & 0xFF);

    slot->consumed   = len;
    slot->cursor     = pkt;
    slot->cursor_off = len;

    return TRUE;
}

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

    /*
     * The transmit checksum, out of the loads the copy is about to do.
     *
     * Only at the start of a frame, and only when this one call takes all of
     * it from one unchained packet: the answer has to be written into the
     * device's buffer at a fixed offset, and a chunked or chained copy cannot
     * promise that the chunk holding the field is still addressable when the
     * last byte arrives.  Everything else is handed to NetX Duo's own
     * deferred path, which fills the field into the packet BEFORE the copy
     * reads it, so the slow case is correct by construction rather than by
     * this function getting it right.
     */
    if (slot->consumed == 0 && slot->packet != NULL &&
        (slot->packet->nx_packet_interface_capability_flag &
         NX_INTERFACE_CAPABILITY_TCP_TX_CHECKSUM) != 0)
    {
        if (ami_sana2_tx_fuse_checksum(slot, out, len))
        {
            slot->packet->nx_packet_interface_capability_flag &=
                (ULONG)(~NX_INTERFACE_CAPABILITY_TCP_TX_CHECKSUM);

            /* It copied the whole frame and moved the cursor to the end of it.
               Falling through to the walk below would find nothing left to
               copy, exhaust the chain with `len` still unsatisfied and report
               a failed copy for a frame that is sitting complete in the
               device's buffer. */
            return TRUE;
        }
        else
        {
            /* NetX Duo fills it into the packet and clears the flag. */
            _nx_ip_packet_checksum_compute(slot->packet);
        }
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

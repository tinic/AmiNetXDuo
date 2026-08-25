/*
 * AmiNetXDuo, SANA-II buffer-management hooks.  The device calls them in m68k
 * register convention (a0 = to, a1 = from, d0 = length) to move packet data in
 * and out of the NX_PACKET reached through ios2_Data.
 *
 * copybuff.doc: these run at interrupt level.  No exec memory calls, no
 * logging, no stack checking, nothing that Forbid()s.
 *
 * SPDX-License-Identifier: MIT
 */

#include "sana2_internal.h"

#include "net68k.h"

#include "aminetxduo/anxs2ext.h"

/* _nx_ip_packet_checksum_compute(): the deferred path a declined fusion
   hands the packet to. */
#include "nx_ip.h"

/*
 * The copy loop is not newlib's memcpy: this runs at interrupt level, and a
 * shared library must not depend on the C library's implementation there.
 * n68k_copy_bytes() brings the destination to a longword boundary and then
 * moves longwords whatever the source is doing.  See src/net68k/n68k_copy.S.
 */
VOID ami_sana2_copy_bytes(UCHAR *to, const UCHAR *from, ULONG len)
{
    N68K_COPY_BYTES(to, from, len);
}

/*
 * S2_CopyToBuff.  `to` is this CMD_READ's ios2_Data, that is the AmiRxSlot,
 * whose NX_PACKET was allocated and positioned before the read was posted.  In
 * cooked mode dst already points 14 bytes into the packet, leaving room for the
 * synthesised Ethernet header; in raw mode dst is the start of the frame.
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
     * Sum while copying, when both ends are WORD aligned.  WORD, not longword:
     * the two ends are permanently two bytes out of phase, so a `& 3` gate
     * never passes in cooked mode.  A 68000 raises an address error on an odd
     * word or longword access but not on a merely 4-misaligned one, so `& 1` is
     * the real requirement.  The answer goes in the slot, not in the packet.
     */
    slot->summed = FALSE;

    /* The slot knows its reader and the reader knows the interface, where a
       counter a user can read has to live. */
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
             * with zeroes, which is what a walk does.  Assembled through the
             * byte array, so the result depends on where the bytes sit.
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
 * Copy `len` bytes and return the ones-complement accumulator of what was
 * copied, in n68k_copy_sum_longwords()'s convention: the caller folds, and a
 * partial trailing longword is padded with zeroes as a walk over it pads.
 */
static ULONG ami_sana2_copy_sum(UCHAR *to, const UCHAR *from, ULONG len)
{
    union { ULONG l; UCHAR b[4]; } w;
    ULONG words, tail, sum, i, k, n;

    if ((((ALIGN_TYPE)to | (ALIGN_TYPE)from) & 1) != 0)
    {
        /* Odd on one side, where a 68000 permits no word access at all.  The
           pools this driver copies between are longword aligned, so this is
           unreachable today. */
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

/*
 * Copy one whole TCP frame into `out`, summing as it goes, and write the
 * checksum into the device's buffer.  FALSE leaves the packet untouched for the
 * caller to hand to NetX Duo.  The pseudo-header is RFC 793's; the segment is
 * summed with its checksum field zero, the identity for a ones-complement sum.
 */
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

    /* Copy and sum in one pass. The IP header is copied but not summed. */
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

/*
 * S2_CopyFromBuff.  `from` is this CMD_WRITE's ios2_Data, the AmiTxSlot.  A
 * device may take the frame in one call or several and may restart the whole
 * transfer on a retry, so the cursor rewinds to the start of the packet for any
 * request it cannot satisfy from its current position.
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

    /*
     * The transmit checksum, out of the loads the copy is about to do.  Only at
     * the start of a frame, and only when this one call takes all of it from
     * one unchained packet: the field is written at a fixed offset, and a
     * chunked or chained copy cannot promise the chunk holding it is still
     * addressable when the last byte arrives.
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
               Falling through to the walk would exhaust the chain with `len`
               still unsatisfied and report a failed copy for a complete frame. */
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

/* ---- the private direct-receive pair, aminetxduo/anxs2ext.h ---------- */

/*
 * ANXD_S2_RX_DIRECT.  The device asks where this CMD_READ's payload would land.
 * A slot that cannot answer safely declines and the device takes the staging
 * path.  Interrupt level, copybuff.doc constraints.
 */
UBYTE *ami_sana2_rx_direct(APTR ios2_data, ULONG len)
{
    AmiRxSlot *slot = (AmiRxSlot *)ios2_data;

    if (slot == NULL || slot->packet == NULL || slot->dst == NULL)
        return NULL;
    if (len > slot->capacity)
        return NULL;

    return slot->dst;
}

/*
 * ANXD_S2_RX_FILLED.  The device wrote the payload itself, straight off the
 * hardware.  `summed` carries whether `sum` is the longword ones-complement
 * running sum the verifier expects; without it the verifier walks the frame.
 */
VOID ami_sana2_rx_filled(APTR ios2_data, ULONG len, ULONG sum, UBYTE summed)
{
    AmiRxSlot *slot = (AmiRxSlot *)ios2_data;

    (VOID)len;

    if (slot == NULL)
        return;

    /* The deliver path reads slot->copied as the received length, and a
       zero there means the hook never ran and the frame is refused.  This
       is the direct path's version of the hook running. */
    slot->copied = len;

    /* Count completion, not the earlier claim: a core may claim a slot and then
       put it back when its hardware drain fails.  These ABI-stable counter
       names predate the direct pair, so "copy hook" means either fill path. */
    if (slot->owner != NULL && slot->owner->iface != NULL)
    {
        slot->owner->iface->stats.rx_copy_hook++;
        slot->owner->iface->stats.rx_direct_fill++;
        if (summed != 0)
            slot->owner->iface->stats.rx_copy_summed++;
    }

#ifdef AMINETXDUO_RX_VERIFY
    slot->sum    = sum;
    slot->summed = (BOOL)(summed != 0);
#else
    (VOID)sum;
    (VOID)summed;
#endif
}

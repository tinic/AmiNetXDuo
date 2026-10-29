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

#ifdef AMINETXDUO_RX_VERIFY
/* Defined below; the copy hook needs it for the odd-address case. */
static ULONG ami_sana2_copy_sum(UCHAR *to, const UCHAR *from, ULONG len);
#endif

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
    AmiRxSlot  *slot = (AmiRxSlot *)to;
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
     *
     * THAT PHASE COSTS 30-37% OF THIS COPY AND IT IS 15% OF RECEIVE.
     * bench/sumbench.c timed its variants with src[] and dst[] as ULONG
     * arrays -- both ends aligned, which is not what this hook is ever handed.
     * Timed at the real phase (source at 2 mod 4) on an A1200 under Amiberry,
     * 200 x 1460 bytes:
     *
     *     variant     aligned      src +2       cost
     *     lm14        148.3 ns/B   203.4 ns/B   +37.2%
     *     ldmovem     159.9        207.9        +30.0%
     *     addx14      182.2        231.4        +27.0%
     *     lmsep       187.1        241.2        +28.9%
     *
     * So "per-byte is measured-optimal" was measured on input this code never
     * sees.  `dst` is nx_packet_data_start + PAD(2) + ETH(14), 0 mod 4; `from`
     * is the device's payload pointer, 2 mod 4, and we do not own it.
     *
     * AND IT IS STRUCTURAL.  BOTH WAYS OUT WERE TRIED AND BOTH LOSE.
     *
     * Matching the phase does not work on paper: dropping AMI_SANA2_RX_PAD to
     * 0 puts both ends at 2 mod 4, which a two-byte lead-in could align -- and
     * drags the IP header off its longword and misaligns the packet->app copy,
     * another 9.9% of receive.  The two ends differ by a fixed two bytes, so
     * no lead-in can ever align both.
     *
     * Avoiding the misaligned read does not work in fact.  bench/sumvar3.S's
     * v_shift2 reads ALIGNED longwords from `from - 2` and shift-combines each
     * adjacent pair into the longword a misaligned read would have produced,
     * so the bus never sees a misaligned access.  It is correct -- the bench
     * checks its sum and every written longword against the C reference over
     * the same bytes -- and it is far slower:
     *
     *     at src +2      lm14+2  199.1 ns/B      shift2  425.5 ns/B
     *
     * The misaligned read costs 49 ns/B over aligned lm14.  Shift-combining
     * spends more than four times that avoiding it.  v_shift2 is a plain
     * per-longword loop where lm14 batches seven loads with movem, so some of
     * that gap is batching rather than shifting -- but a movem-batched shift
     * variant would still have to fit four extra instructions per longword
     * inside 49 ns/B, and it cannot.
     *
     * SO IT IS NOT RECOVERABLE IN SOFTWARE -- AND IT WAS RECOVERED ANYWAY, IN
     * THE DEVICE.  Everything above is still true of the copy; what stopped
     * being true is the premise that `from` arrives at 2 mod 4.  Neither
     * attempt above considered moving the SOURCE, and the Am7990 takes a
     * byte-granular buffer address: starting the a2065's receive buffers TWO
     * BYTES IN puts `frame + 14`, which is what this hook is handed, at 0 mod
     * 4 against a destination already there (lance.c:70).  Measured +3.09% rx,
     * six alternating rounds.
     *
     * So on the a2065 path this copy now runs at the ALIGNED row of the table
     * above, and the 30-37% is a cost other drivers may still pay rather than
     * a standing property of the receive path.  Read the table as what a
     * misaligned source costs, not as what this hook costs today.
     */
    slot->summed  = FALSE;
    slot->rxflags = 0;

    /* The counter a user can read lives on the interface; the slot carries a
       pointer straight to it (sana2_internal.h), because this runs at
       interrupt level on every frame. */
    if (slot->stats != NULL)
        slot->stats->rx_copy_hook++;

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
        if (slot->stats != NULL)
            slot->stats->rx_copy_summed++;
        slot->copied = len;

        return TRUE;
    }

    /*
     * ODD ON ONE SIDE, WHICH ONLY A DEVICE WE DO NOT OWN CAN PRODUCE.
     *
     * Our own cores hand this hook an even payload pointer and slot->dst is
     * nx_packet_data_start + PAD + ETH, so the branch above takes every frame
     * and nothing here runs.  A THIRD-PARTY SANA-II driver is under no such
     * obligation, and x-surf-100.device is the one that matters: it is not our
     * code and this reader is the whole of our contact with it.
     *
     * What this used to do was copy and give up on the sum, leaving
     * slot->summed FALSE -- and n68k_rx_verify_sum() then walks the WHOLE
     * payload a second time to get what the copy had already read once.  Two
     * passes over every frame, on the one path we cannot measure here.
     *
     * ami_sana2_copy_sum() copies each byte and accumulates it immediately, in
     * the same convention the branch above uses, so the sum is the sum and the
     * second pass is gone.  It is safe at any parity because this branch uses
     * byte accesses; on a 68000 an odd word or longword access is an address
     * error rather than merely a slow access.
     */
    slot->sum    = ami_sana2_copy_sum(slot->dst, (const UCHAR *)from, len);
    slot->summed = TRUE;
    if (slot->stats != NULL)
    {
        slot->stats->rx_copy_summed++;
        /* AND SEPARATELY, because rx_copy_summed stopped being able to say
           it.  Making this branch accumulate its own sum was right -- it
           removed the second walk -- but it also made summed == hook whether
           the branch runs or not, which is exactly the measurement the
           0.26.3 field report needed.  Counted here, so `netstat -s` can
           answer it. */
        slot->stats->rx_copy_unaligned++;
    }
    slot->copied = len;

    return TRUE;
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
        /* Odd on one side, where a 68000 permits no word access at all.  Our
           own pools are longword aligned, so this is reached only through the
           copy hook above, and only for a third-party device that hands us an
           odd payload pointer. */
        sum = 0UL;
        for (i = 0UL; i < len; i += 4UL)
        {
            n = len - i;
            if (n > 4UL)
                n = 4UL;

            w.l = 0UL;
            for (k = 0UL; k < n; k++)
            {
                UCHAR byte = from[i + k];

                to[i + k] = byte;
                w.b[k]    = byte;
            }

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
/*
 * `ip` starts a whole IPv4 TCP segment of `len` bytes, not a fragment:
 * *ihl_out its header length, *total_out the datagram's total length, at
 * most `len` (the rest is padding).  What both checksum paths ask first.
 */
/* 68020+ permits odd-address word loads. GCC otherwise expands even the
   aligned C form into byte reads here; name the one load explicitly. A packed
   alias-safe operand describes arbitrary packet alignment to the compiler.
   68000 builds and other hosts retain portable byte reads. */
static inline ULONG __attribute__((always_inline)) ami_sana2_tx_be16(
    const UCHAR *p)
{
#if defined(__mc68020__)
    struct __attribute__((packed, may_alias)) NetWord { unsigned short value; };
    ULONG word = 0;
    __asm__("move.w %1,%0" : "+d"(word)
            : "m"(((const struct NetWord *)p)->value) : "cc");
    return word;
#else
    return ((ULONG)p[0] << 8) | p[1];
#endif
}

static inline BOOL __attribute__((always_inline)) ami_sana2_tx_tcp_segment(
    const UCHAR *ip, ULONG len, ULONG *ihl_out, ULONG *total_out)
{
    ULONG ihl, total;

    if (len < 40 || (ip[0] & 0xF0) != 0x40)
        return FALSE;                   /* not IPv4                          */

    ihl = (ULONG)(ip[0] & 0x0F) * 4UL;
    if (ihl < 20 || ihl + 20 > len)
        return FALSE;

    if (ip[9] != 6)                     /* not TCP                           */
        return FALSE;

    if (ami_sana2_tx_be16(ip + 6) & 0x3FFF)
        return FALSE;                   /* a fragment has no whole segment   */

    total = ami_sana2_tx_be16(ip + 2);
    if (total > len || total < ihl + 20)
        return FALSE;                   /* padded or malformed               */

    *ihl_out   = ihl;
    *total_out = total;
    return TRUE;
}

/* The pseudo-header of RFC 793, folded to sixteen bits and not complemented:
   what a checksum over the segment starts from. */
/* Both helpers are always_inline, not left to the heuristic: at -Os the LTO
   inliner put them out of line the day the fork grew by a hundred lines,
   right after ami_sana2_copy_to_buff() with no symbol of their own, and the
   hot-call gate read their four call sites as calls into it
   (tools/check-hot-calls.sh); `inline` alone did not bring them back. */
static inline ULONG __attribute__((always_inline)) ami_sana2_tx_pseudo_header(
    const UCHAR *ip, ULONG tcp_len)
{
    ULONG sum;

    sum  = ami_sana2_tx_be16(ip + 12);
    sum += ami_sana2_tx_be16(ip + 14);
    sum += ami_sana2_tx_be16(ip + 16);
    sum += ami_sana2_tx_be16(ip + 18);
    sum += 6UL + tcp_len;
    /* Four address words plus an IPv4 segment length fit in 19 bits. */
    sum = (sum & 0xFFFFUL) + (sum >> 16);
    sum = (sum & 0xFFFFUL) + (sum >> 16);
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
    if (!ami_sana2_tx_tcp_segment(ip, len, &ihl, &total))
        return FALSE;

    tcp_len = total - ihl;

    /* Copy and sum in one pass. The IP header is copied but not summed. */
    ami_sana2_copy_bytes(out, ip, ihl);
    sum = ami_sana2_copy_sum(out + ihl, ip + ihl, tcp_len);

    /* Anything past the datagram is padding the device wants but the
       checksum does not cover. */
    if (len > total)
        ami_sana2_copy_bytes(out + total, ip + total, len - total);

    /* Fold what the copy accumulated to sixteen bits, the pseudo-header on
       top of it, folded again. */
    while (sum >> 16)
        sum = (sum & 0xFFFFUL) + (sum >> 16);
    sum += ami_sana2_tx_pseudo_header(ip, tcp_len);
    while (sum >> 16)
        sum = (sum & 0xFFFFUL) + (sum >> 16);

    sum = (~sum) & 0xFFFFUL;
    if (sum == 0UL)
        sum = 0xFFFFUL;                 /* TCP has no "no checksum" value    */

    csum = out + ihl + 16;
    csum[0] = (UCHAR)(sum >> 8);
    csum[1] = (UCHAR)(sum & 0xFF);

    /*
     * Into the packet as well.  The caller clears the packet's checksum flag
     * after this, so every later copy of it takes the plain walk: a device
     * that copies the frame again (anxnet.device rebuilds a write its FIFO
     * had no room for) would otherwise send the zero NetX Duo left in the
     * field, and the peer drops the segment without a word.  Measured on a
     * real A1200 with a 3c589: every third frame of a burst lost, 22% of
     * all TCP segments retransmitted, 0.17 Mbit/s.
     */
    csum = (UCHAR *)pkt->nx_packet_prepend_ptr + ihl + 16;
    csum[0] = (UCHAR)(sum >> 8);
    csum[1] = (UCHAR)(sum & 0xFF);

    slot->consumed   = len;
    slot->cursor     = pkt;
    slot->cursor_off = len;

    return TRUE;
}

/*
 * The card finishes the negotiated TCP checksum: a cooked IPv4 TCP
 * segment that is not a fragment, its TCP header whole in the first buffer
 * and its checksum field the zero NetX Duo left for the interface, gets the
 * pseudo-header sum written there, folded and not complemented, and the
 * packet's checksum flag cleared so every copy of it from here on is the
 * plain walk.  FALSE leaves the packet as it was, for the fused copy or the
 * stack's own walk.  A retransmission zeroes the field and sets the flag
 * again (nx_tcp_socket_retransmit.c), so it arrives here as new.
 */
BOOL ami_sana2_tx_pseudo_sum(NX_PACKET *pkt)
{
    UCHAR *ip  = (UCHAR *)pkt->nx_packet_prepend_ptr;
    ULONG  len = pkt->nx_packet_length;
    ULONG  ihl, total, sum;
    UCHAR *csum;

    if (!ami_sana2_tx_tcp_segment(ip, len, &ihl, &total) || total != len)
        return FALSE;                   /* padding is the fusion's case      */

    if ((ULONG)(pkt->nx_packet_append_ptr - ip) < ihl + 20)
        return FALSE;                   /* the field is not in this buffer   */

    sum     = ami_sana2_tx_pseudo_header(ip, total - ihl);
    csum    = ip + ihl + 16;
    csum[0] = (UCHAR)(sum >> 8);
    csum[1] = (UCHAR)(sum & 0xFF);

    pkt->nx_packet_interface_capability_flag &=
        (ULONG)(~NX_INTERFACE_CAPABILITY_TCP_TX_CHECKSUM);
    return TRUE;
}

/* ------------------------------------------------------------- TCP GSO */

/*
 * A GSO child's frame is its parent's headers patched for the segment, the
 * payload slice, and on a short Ethernet frame zeroes up to 46 bytes.  The
 * parent is only ever read: TCP may hand it back for retransmission, and the
 * other children are still being cut from it.
 */
#define AMI_TCP_FIN     0x01
#define AMI_TCP_SYN     0x02
#define AMI_TCP_RST     0x04
#define AMI_TCP_PSH     0x08
#define AMI_TCP_URG     0x20
#define AMI_TCP_CWR     0x80

/* What one child's headers say that the parent's do not. */
typedef struct AmiGsoFields
{
    ULONG   seq;
    ULONG   seg_len;
    UWORD   ip_len;
    UWORD   ip_id;
    UWORD   ip_sum;
    UBYTE   flags;
} AmiGsoFields;

static inline ULONG __attribute__((always_inline)) ami_sana2_gso_fold(ULONG sum)
{
    while (sum >> 16)
        sum = (sum & 0xFFFFUL) + (sum >> 16);
    return sum;
}

BOOL ami_sana2_gso_parse(NX_PACKET *packet, ULONG mtu, AmiTxGso *g)
{
    const NX_TCP_GSO_INFO *info;
    const UCHAR           *ip;
    const UCHAR           *tcp;
    ULONG                  first, ihl, thl, payload, segs;
    ULONG                  room, base, end;
    UBYTE                  mode = 0;

    /* The record sits in front of the headroom, never inside it: four
       bytes of version 1, the whole of version 2. */
    room = (ULONG)(packet->nx_packet_prepend_ptr -
                   packet->nx_packet_data_start);
    if (room < 4UL)
        return FALSE;

    info = (const NX_TCP_GSO_INFO *)(const VOID *)packet->nx_packet_data_start;
    if (info->mss == 0)
        return FALSE;
    if (info->version == 1)
    {
        if (info->flags != 0)
            return FALSE;
    }
    else if (info->version == NX_TCP_GSO_VERSION &&
             room >= (ULONG)sizeof(NX_TCP_GSO_INFO))
    {
        if (info->flags == NX_TCP_GSO_REPAIR)
            mode = AMI_SANA2_GSO_REPAIR;
        else if (info->flags != 0 || info->offset != 0 || info->length != 0)
            return FALSE;
    }
    else
    {
        return FALSE;
    }

    /* Both headers whole in the first buffer: every child copies them from
       there. */
    ip    = packet->nx_packet_prepend_ptr;
    first = (ULONG)(packet->nx_packet_append_ptr - ip);
    if (first < 40 || (ip[0] & 0xF0) != 0x40 || ip[9] != 6)
        return FALSE;

    ihl = (ULONG)(ip[0] & 0x0F) * 4UL;
    if (ihl < 20 || ihl + 20 > first)
        return FALSE;
    if (ami_sana2_tx_be16(ip + 6) & 0x3FFF)
        return FALSE;                   /* a fragment                        */

    tcp = ip + ihl;
    thl = (ULONG)(tcp[12] >> 4) * 4UL;
    if (thl < 20 || ihl + thl > first)
        return FALSE;

    /* A segment that opens, resets or points into the data is never cut. */
    if (tcp[13] & (AMI_TCP_SYN | AMI_TCP_RST | AMI_TCP_URG))
        return FALSE;

    /* The IPv4 total length bounds the payload to sixteen bits. */
    if (packet->nx_packet_length <= ihl + thl ||
        packet->nx_packet_length > 0xFFFFUL)
        return FALSE;
    payload = packet->nx_packet_length - ihl - thl;

    if (mtu != 0 && ihl + thl + info->mss > mtu)
        return FALSE;

    if (mode != 0)
    {
        /* One frame, inside the payload and no larger than a segment. */
        base = info->offset;
        end  = base + info->length;
        if (info->length == 0 || info->length > info->mss || end > payload)
            return FALSE;
        if (end == payload)
            mode |= AMI_SANA2_GSO_TAIL;
        segs = 1;
    }
    else
    {
        base = 0;
        end  = payload;
        segs = (payload + info->mss - 1UL) / info->mss;
        if (segs > AMI_SANA2_GSO_MAX_SEGS)
            return FALSE;
    }

    g->cur_buf   = packet;
    g->cur_start = -(LONG)(ihl + thl);
    g->base      = (UWORD)base;
    g->end       = (UWORD)end;
    g->mode      = mode;
    g->mss       = info->mss;
    g->ihl       = (UBYTE)ihl;
    g->thl       = (UBYTE)thl;
    g->segs      = (UBYTE)segs;
    return TRUE;
}

/*
 * The children launch in order, one launcher at a time (sana2_tx.c): this
 * launch is the block's next child, and the block's cursor only moves
 * forward, so the whole block costs one walk of the chain.
 */
ULONG ami_sana2_gso_prepare(AmiTxSlot *slot, NX_PACKET *parent)
{
    AmiTxGso  *g = &slot->iface->gso[slot->gso - 1];
    ULONG      seg_off;
    ULONG      seg_len;
    NX_PACKET *buf;
    LONG       start;

    slot->seg_index = g->next_seg++;
    seg_off = ami_sana2_gso_slice(g, slot->seg_index, &seg_len);

    if (g->cur_buf == NULL || (LONG)seg_off < g->cur_start)
    {
        g->cur_buf   = parent;
        g->cur_start = -(LONG)(g->ihl + g->thl);
    }

    buf   = g->cur_buf;
    start = g->cur_start;
    while (buf != NULL)
    {
        LONG have = (LONG)(buf->nx_packet_append_ptr -
                           buf->nx_packet_prepend_ptr);

        if ((LONG)seg_off < start + have)
            break;
        start += have;
#ifndef NX_DISABLE_PACKET_CHAIN
        buf = buf->nx_packet_next;
#else
        buf = NX_NULL;
#endif
    }

    /* NULL: the chain is shorter than the block claims, and the copy
       refuses the child. */
    if (buf != NULL)
    {
        g->cur_buf   = buf;
        g->cur_start = start;
    }
    /* The parent now, not at the launch's common tail: the capture tap
       reads the child's headers from it before that. */
    slot->packet      = parent;
    slot->seg_buf     = buf;
    slot->seg_buf_off = (buf != NULL) ? (ULONG)((LONG)seg_off - start) : 0UL;
    slot->seg_summed  = FALSE;

    return (ULONG)g->ihl + g->thl + seg_len;
}

static VOID ami_sana2_gso_fields(const AmiTxGso *g, UWORD seg,
                                 const UCHAR *ip, AmiGsoFields *f)
{
    const UCHAR *tcp     = ip + g->ihl;
    ULONG        seg_off = ami_sana2_gso_slice(g, seg, &f->seg_len);
    ULONG        old_len = ami_sana2_tx_be16(ip + 2);
    ULONG        old_id  = ami_sana2_tx_be16(ip + 4);
    ULONG        sum;
    UBYTE        flags   = tcp[13];

    f->ip_len = (UWORD)(g->ihl + g->thl + f->seg_len);
    f->ip_id  = (UWORD)(old_id + seg);  /* the core reserved k from here  */

    /* RFC 1624 eqn. 3, HC' = ~(~HC + ~m + m'), over the two words that
       change.  The template's sum is whole: the interface never asks for
       the IPv4 header checksum. */
    sum  = (~ami_sana2_tx_be16(ip + 10)) & 0xFFFFUL;
    sum += ((~old_len) & 0xFFFFUL) + f->ip_len;
    sum += ((~old_id) & 0xFFFFUL) + f->ip_id;
    f->ip_sum = (UWORD)((~ami_sana2_gso_fold(sum)) & 0xFFFFUL);

    f->seq = ((ami_sana2_tx_be16(tcp + 4) << 16) |
              ami_sana2_tx_be16(tcp + 6)) + seg_off;

    /* FIN and PUSH belong to the end of the block, CWR to its start.  A
       repair is neither: PUSH only where it ends the payload, never FIN or
       CWR. */
    if ((g->mode & AMI_SANA2_GSO_REPAIR) != 0)
    {
        flags &= (UBYTE)~(AMI_TCP_FIN | AMI_TCP_CWR);
        if ((g->mode & AMI_SANA2_GSO_TAIL) == 0)
            flags &= (UBYTE)~AMI_TCP_PSH;
    }
    else
    {
        if ((ULONG)seg + 1UL < g->segs)
            flags &= (UBYTE)~(AMI_TCP_FIN | AMI_TCP_PSH);
        if (seg != 0)
            flags &= (UBYTE)~AMI_TCP_CWR;
    }
    f->flags = flags;
}

/* One header byte, if it falls inside the [from, to) piece at `out`. */
static inline VOID __attribute__((always_inline)) ami_sana2_gso_poke(
    UCHAR *out, ULONG from, ULONG to, ULONG at, ULONG value)
{
    if (at >= from && at < to)
        out[at - from] = (UCHAR)value;
}

/* Bytes [from, to) of the child's headers: the template, patched. */
static VOID ami_sana2_gso_emit(const AmiTxGso *g, const UCHAR *ip,
                               const AmiGsoFields *f, ULONG tcp_sum,
                               UCHAR *out, ULONG from, ULONG to)
{
    ULONG t = g->ihl;

    ami_sana2_copy_bytes(out, ip + from, to - from);

    ami_sana2_gso_poke(out, from, to, 2,      f->ip_len >> 8);
    ami_sana2_gso_poke(out, from, to, 3,      f->ip_len);
    ami_sana2_gso_poke(out, from, to, 4,      f->ip_id >> 8);
    ami_sana2_gso_poke(out, from, to, 5,      f->ip_id);
    ami_sana2_gso_poke(out, from, to, 10,     f->ip_sum >> 8);
    ami_sana2_gso_poke(out, from, to, 11,     f->ip_sum);
    ami_sana2_gso_poke(out, from, to, t + 4,  f->seq >> 24);
    ami_sana2_gso_poke(out, from, to, t + 5,  f->seq >> 16);
    ami_sana2_gso_poke(out, from, to, t + 6,  f->seq >> 8);
    ami_sana2_gso_poke(out, from, to, t + 7,  f->seq);
    ami_sana2_gso_poke(out, from, to, t + 13, f->flags);
    ami_sana2_gso_poke(out, from, to, t + 16, tcp_sum >> 8);
    ami_sana2_gso_poke(out, from, to, t + 17, tcp_sum);
}

/*
 * Copy (when `to` is not NULL) and sum one run, as if it started at an even
 * offset of the segment: big-endian 16-bit words, folded.  The longword
 * routines where both ends allow it, bytes otherwise and for the tail.
 *
 * Summed alone (`to` NULL: a copy in pieces with no card to finish the sum),
 * an odd `from` takes its first byte by hand and the rest from the even
 * address after it.  The rest starts one byte into the run, so its sum comes
 * out byte-swapped, and swapping a ones-complement sum back is exact (RFC
 * 1071, byte order independence).
 */
static ULONG ami_sana2_gso_copy_sum(UCHAR *to, const UCHAR *from, ULONG len)
{
    ULONG sum  = 0UL;
    ULONG lead = 0UL;
    ULONG i    = 0UL;
    BOOL  odd  = FALSE;

    if (to == NULL)
    {
        ULONG words;

        if (((ALIGN_TYPE)from & 1) != 0 && len != 0)
        {
            lead = (ULONG)from[0] << 8;
            from++;
            len--;
            odd = TRUE;
        }

        words = len >> 2;
        if (words != 0)
            sum = N68K_SUM_LONGWORDS((const ULONG *)(const void *)from,
                                     words);
        i = words << 2;
    }
    else if ((((ALIGN_TYPE)to | (ALIGN_TYPE)from) & 1) == 0)
    {
        ULONG words = len >> 2;

        sum = N68K_COPY_SUM_LONGWORDS((ULONG *)(void *)to,
                                      (const ULONG *)(const void *)from,
                                      words);
        i = words << 2;
    }

    for (; i < len; i++)
    {
        UCHAR b = from[i];
        ULONG v = (i & 1UL) ? (ULONG)b : ((ULONG)b << 8);

        if (to != NULL)
            to[i] = b;
        sum += v;
        if (sum < v)
            sum++;                      /* end-around carry                  */
    }

    sum = ami_sana2_gso_fold(sum);
    if (odd)
        sum = ami_sana2_gso_fold(lead +
                                 (((sum << 8) | (sum >> 8)) & 0xFFFFUL));

    return sum;
}

/*
 * `len` payload bytes from the cursor, copied to `to` and/or summed into
 * *sum (folded, from an even segment offset).  Moves the cursor; FALSE when
 * the chain ends first.  Each buffer is visited once.
 */
static BOOL ami_sana2_gso_walk(NX_PACKET **cur_io, ULONG *off_io, UCHAR *to,
                               ULONG len, ULONG *sum)
{
    NX_PACKET *cur = *cur_io;
    ULONG      off = *off_io;
    ULONG      acc = 0UL;
    ULONG      at  = 0UL;

    while (len != 0 && cur != NULL)
    {
        ULONG have = (ULONG)(cur->nx_packet_append_ptr -
                             cur->nx_packet_prepend_ptr);
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

        if (sum != NULL)
        {
            ULONG s = ami_sana2_gso_copy_sum(to, cur->nx_packet_prepend_ptr +
                                                 off, take);

            /* A run that starts at an odd offset sums byte-swapped. */
            if (at & 1UL)
                s = ((s << 8) | (s >> 8)) & 0xFFFFUL;
            acc += s;
        }
        else
        {
            ami_sana2_copy_bytes(to, cur->nx_packet_prepend_ptr + off, take);
        }

        if (to != NULL)
            to += take;
        off += take;
        len -= take;
        at  += take;
    }

    *cur_io = cur;
    *off_io = off;
    if (sum != NULL)
        *sum = acc;

    return (len == 0) ? TRUE : FALSE;
}

/* The child's TCP checksum: its own header (seq and flags patched, the
   field zero), the payload sum and the pseudo-header.  always_inline for
   the reason the pseudo-header's is: left to LTO it went out of line right
   after ami_sana2_copy_to_buff() with no symbol of its own, and the
   hot-call gate read its two call sites as calls into that. */
static inline ULONG __attribute__((always_inline)) ami_sana2_gso_finish(
    const AmiTxGso *g, const UCHAR *ip, const AmiGsoFields *f,
    ULONG payload_sum)
{
    const UCHAR *tcp = ip + g->ihl;
    ULONG        sum = ami_sana2_gso_fold(payload_sum);
    ULONG        i;

    for (i = 0; i < g->thl; i += 2)
    {
        if (i == 4)
            sum += f->seq >> 16;
        else if (i == 6)
            sum += f->seq & 0xFFFFUL;
        else if (i == 12)
            sum += ((ULONG)tcp[12] << 8) | f->flags;
        else if (i != 16)
            sum += ami_sana2_tx_be16(tcp + i);
    }
    sum += ami_sana2_tx_pseudo_header(ip, g->thl + f->seg_len);

    sum = (~ami_sana2_gso_fold(sum)) & 0xFFFFUL;
    return (sum == 0UL) ? 0xFFFFUL : sum;
}

/* The checksum field as the child carries it: the pseudo-header sum for a
   card that finishes it, else the whole sum, walked once and kept. */
static ULONG ami_sana2_gso_tcp_field(AmiTxSlot *slot, const AmiTxGso *g,
                                     const UCHAR *ip, const AmiGsoFields *f,
                                     BOOL *ok)
{
    *ok = TRUE;

    if ((slot->tx_flags & ANXD_S2_TXF_TCP) != 0)
        return ami_sana2_tx_pseudo_header(ip, g->thl + f->seg_len);

    if (!slot->seg_summed)
    {
        NX_PACKET *cur = slot->seg_buf;
        ULONG      off = slot->seg_buf_off;
        ULONG      sum;

        if (!ami_sana2_gso_walk(&cur, &off, NULL, f->seg_len, &sum))
        {
            *ok = FALSE;
            return 0UL;
        }
        slot->seg_sum    = (UWORD)ami_sana2_gso_finish(g, ip, f, sum);
        slot->seg_summed = TRUE;
    }

    return slot->seg_sum;
}

ULONG ami_sana2_gso_header(AmiTxSlot *slot, UCHAR *out)
{
    const AmiTxGso *g  = &slot->iface->gso[slot->gso - 1];
    const UCHAR    *ip = slot->packet->nx_packet_prepend_ptr;
    ULONG           hlen = (ULONG)g->ihl + g->thl;
    AmiGsoFields    f;
    ULONG           csum;
    BOOL            ok;

    ami_sana2_gso_fields(g, slot->seg_index, ip, &f);
    csum = ami_sana2_gso_tcp_field(slot, g, ip, &f, &ok);
    ami_sana2_gso_emit(g, ip, &f, csum, out, 0, hlen);

    return hlen;
}

/*
 * S2_CopyFromBuff for a GSO child.  The same contract as the ordinary walk:
 * any number of calls, a restart rewinds, and here to the start of the
 * child's slice, never of the parent.
 */
static BOOL __attribute__((noinline)) ami_sana2_gso_copy(AmiTxSlot *slot,
                                                         UCHAR *out, ULONG len)
{
    const AmiTxGso *g    = &slot->iface->gso[slot->gso - 1];
    const UCHAR    *ip   = slot->packet->nx_packet_prepend_ptr;
    ULONG           hlen = (ULONG)g->ihl + g->thl;
    AmiGsoFields    f;
    ULONG           data_end, pos, n, csum;
    BOOL            ok;

    if (slot->cursor == NULL || slot->consumed >= slot->total ||
        len > (slot->total - slot->consumed))
    {
        slot->cursor     = slot->seg_buf;
        slot->cursor_off = slot->seg_buf_off;
        slot->consumed   = 0;
    }

    if (slot->seg_buf == NULL)
        return FALSE;                   /* the chain ends before the slice   */

    ami_sana2_gso_fields(g, slot->seg_index, ip, &f);
    data_end = hlen + f.seg_len;

    /* The whole frame in one call and no card to finish the sum: the sum
       comes out of the copy's own loads, as the ordinary fusion's does. */
    if ((slot->tx_flags & ANXD_S2_TXF_TCP) == 0 && slot->consumed == 0 &&
        len == slot->total && !slot->seg_summed)
    {
        ULONG sum;

        ami_sana2_gso_emit(g, ip, &f, 0UL, out, 0, hlen);
        if (!ami_sana2_gso_walk(&slot->cursor, &slot->cursor_off, out + hlen,
                                f.seg_len, &sum))
            return FALSE;
        for (pos = data_end; pos < len; pos++)
            out[pos] = 0;

        csum = ami_sana2_gso_finish(g, ip, &f, sum);
        out[g->ihl + 16] = (UCHAR)(csum >> 8);
        out[g->ihl + 17] = (UCHAR)csum;

        slot->consumed = len;
        return TRUE;
    }

    /* In pieces: the field is written before the bytes it covers. */
    csum = ami_sana2_gso_tcp_field(slot, g, ip, &f, &ok);
    if (!ok)
        return FALSE;

    pos = slot->consumed;

    if (len != 0 && pos < hlen)
    {
        n = hlen - pos;
        if (n > len)
            n = len;
        ami_sana2_gso_emit(g, ip, &f, csum, out, pos, pos + n);
        out += n;
        len -= n;
        pos += n;
    }

    if (len != 0 && pos < data_end)
    {
        n = data_end - pos;
        if (n > len)
            n = len;
        if (!ami_sana2_gso_walk(&slot->cursor, &slot->cursor_off, out, n,
                                NULL))
        {
            slot->consumed = pos;
            return FALSE;
        }
        out += n;
        len -= n;
        pos += n;
    }

    /* The pad: the rewind rule above keeps it inside slot->total. */
    for (; len != 0; len--, pos++)
        *out++ = 0;

    slot->consumed = pos;
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

    /* A GSO child is cut from its parent as it is copied. */
    if (slot->gso != AMI_SANA2_GSO_NONE)
        return ami_sana2_gso_copy(slot, out, len);

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
            /* NetX Duo fills it into the packet and clears the flag.  It
               takes the segment length from nx_packet_length, which by now
               counts the zeroes ami_sana2_tx_pad() appended: without them
               off, a padded bare ACK goes out 6 short in its checksum. */
            ULONG length = slot->packet->nx_packet_length;

            slot->packet->nx_packet_length = length - slot->pad_len;
            _nx_ip_packet_checksum_compute(slot->packet);
            slot->packet->nx_packet_length = length;
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
 * RxDirect.  The device asks where this CMD_READ's payload would land.
 * A slot that cannot answer safely declines and the device takes the staging
 * path.  Interrupt level, copybuff.doc constraints.
 */
AMIGA_ASM_ARGS UBYTE *ami_sana2_rx_direct(APTR ios2_data, ULONG len)
{
    AmiRxSlot *slot = (AmiRxSlot *)ios2_data;

    if (slot == NULL || slot->packet == NULL || slot->dst == NULL)
        return NULL;
    if (len > slot->capacity)
        return NULL;

    return slot->dst;
}

/*
 * RxFilled.  The device wrote the payload itself, straight off the
 * hardware.  `flags` is the ANXD_S2_RXF_* byte: SUMMED says `sum` is the
 * longword ones-complement running sum the verifier expects (without it the
 * verifier walks the frame); VERIFIED is kept whole for the
 * reader, which only sees them from a device that accepted RX_VERIFIED.
 */
AMIGA_ASM_ARGS VOID ami_sana2_rx_filled(APTR ios2_data, ULONG len, ULONG sum,
                                        UBYTE flags)
{
    AmiRxSlot *slot = (AmiRxSlot *)ios2_data;

    (VOID)len;

    if (slot == NULL)
        return;

    /* The deliver path reads slot->copied as the received length, and a
       zero there means the hook never ran and the frame is refused.  This
       is the direct path's version of the hook running. */
    slot->copied = len;

    /* ONLY when the device answered the tag.  Reaching this hook says the
       direct path ran, not that the driver knows about the header, and a
       third-party one that implements the published pair without the tag
       would otherwise be credited with fourteen bytes it never wrote. */
    if (slot->owner != NULL && slot->owner->iface != NULL &&
        slot->owner->iface->link_hdr_ok)
        slot->hdr_written = TRUE;

    /* Count completion, not the earlier claim: a core may claim a slot and then
       put it back when its hardware drain fails.  These ABI-stable counter
       names predate the direct pair, so "copy hook" means either fill path. */
    if (slot->stats != NULL)
    {
        slot->stats->rx_copy_hook++;
        slot->stats->rx_direct_fill++;
        if ((flags & ANXD_S2_RXF_SUMMED) != 0)
            slot->stats->rx_copy_summed++;
    }

#ifdef AMINETXDUO_RX_VERIFY
    slot->sum     = sum;
    slot->summed  = (BOOL)((flags & ANXD_S2_RXF_SUMMED) != 0);
    slot->rxflags = flags;
#else
    (VOID)sum;
    (VOID)flags;
#endif
}

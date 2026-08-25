/*
 * AmiNetXDuo, the SANA-II buffer-management hooks, on the host.
 *
 * SPDX-License-Identifier: MIT
 */

#include "sana2_internal.h"

#include <stdio.h>
#include <string.h>


static unsigned long h_checks;
static unsigned long h_failures;

static void h_check(int ok, const char *what)
{
    h_checks++;
    if (!ok)
    {
        h_failures++;
        printf("  FAIL %s\n", what);
    }
}


/* The real one is src/net68k/n68k_copy.S. Everything below is about which
   bytes are asked for, not how they are moved. */
VOID n68k_copy_bytes(UCHAR *to, const UCHAR *from, ULONG len)
{
    if (len != 0)
        memcpy(to, from, (size_t)len);
}

/* The contract from src/net68k/n68k_checksum.c, not a memcpy: the transmit
   checksum is only correct if this is, and a stub that copied without summing
   would make every checksum assertion in this file pass for free. */
ULONG n68k_copy_sum_longwords(ULONG *to, const ULONG *from, ULONG count)
{
    ULONG acc = 0;

    while (count != 0UL)
    {
        ULONG w = *from++;

        *to++ = w;

        acc += w;
        if (acc < w)
            acc++;

        count--;
    }

    return acc;
}

/* NetX Duo's deferred checksum path, which ami_sana2_copy_from_buff() hands a
   packet the fusion declined.  Counted rather than performed: what the tests
   below assert is WHICH packets end up here. */
unsigned long h_deferred_checksums;

VOID _nx_ip_packet_checksum_compute(NX_PACKET *packet_ptr)
{
    (VOID)packet_ptr;
    h_deferred_checksums++;
}


#define FRAME_LEN   300

static UCHAR frame[FRAME_LEN];

static void frame_init(void)
{
    ULONG i;

    for (i = 0; i < FRAME_LEN; i++)
        frame[i] = (UCHAR)((i * 7 + 1) & 0xFF);
}

#define MAX_SEGS    6

static NX_PACKET seg[MAX_SEGS];
static UCHAR     segdata[MAX_SEGS][FRAME_LEN];

static NX_PACKET *chain(const ULONG *lens, ULONG nsegs)
{
    ULONG i;
    ULONG at = 0;

    memset(seg, 0, sizeof(seg));

    for (i = 0; i < nsegs; i++)
    {
        memcpy(segdata[i], frame + at, (size_t)lens[i]);

        seg[i].nx_packet_prepend_ptr = segdata[i];
        seg[i].nx_packet_append_ptr  = segdata[i] + lens[i];
        seg[i].nx_packet_next        = (i + 1 < nsegs) ? &seg[i + 1] : NX_NULL;

        at += lens[i];
    }

    return &seg[0];
}

static void tx_slot_init(AmiTxSlot *slot, NX_PACKET *head, ULONG total)
{
    memset(slot, 0, sizeof(*slot));
    slot->packet = head;
    slot->total  = total;
}


static void test_copy_to_buff(void)
{
    AmiRxSlot slot;
    NX_PACKET pkt;
    UCHAR     dst[FRAME_LEN + 16];

    printf("sana2: S2_CopyToBuff\n");

    memset(&pkt, 0, sizeof(pkt));

    memset(&slot, 0, sizeof(slot));
    slot.packet   = &pkt;
    slot.dst      = dst;
    slot.capacity = 100;

    h_check(ami_sana2_copy_to_buff(NULL, frame, 10) == FALSE,
            "a NULL slot is refused");
    h_check(ami_sana2_copy_to_buff(&slot, NULL, 10) == FALSE,
            "a NULL source is refused");

    slot.packet = NX_NULL;
    h_check(ami_sana2_copy_to_buff(&slot, frame, 10) == FALSE,
            "a slot with no packet is refused");
    slot.packet = &pkt;

    slot.dst = NULL;
    h_check(ami_sana2_copy_to_buff(&slot, frame, 10) == FALSE,
            "a slot with no destination is refused");
    slot.dst = dst;

    memset(dst, 0xCD, sizeof(dst));
    h_check(ami_sana2_copy_to_buff(&slot, frame, 101) == FALSE,
            "a frame one byte over capacity is refused");
    h_check(dst[0] == 0xCD, "and nothing was written before it was refused");
    h_check(slot.copied == 0, "and `copied` was not advanced");

    /* Exactly capacity is not an overrun. */
    memset(dst, 0xCD, sizeof(dst));
    h_check(ami_sana2_copy_to_buff(&slot, frame, 100) == TRUE,
            "a frame of exactly capacity is taken");
    h_check(memcmp(dst, frame, 100) == 0, "and it arrives unchanged");
    h_check(dst[100] == 0xCD, "and nothing past capacity was touched");
    h_check(slot.copied == 100, "and `copied` is the length");

    /* A runt, and the degenerate zero. SANA-II allows both. */
    slot.copied = 999;
    h_check(ami_sana2_copy_to_buff(&slot, frame, 0) == TRUE,
            "a zero-length copy succeeds");
    h_check(slot.copied == 0, "and reports zero bytes rather than the last count");
}

static void test_rx_direct(void)
{
    AmiSana2If iface;
    AmiSana2Rx owner;
    AmiRxSlot  slot;
    NX_PACKET  pkt;
    UCHAR      dst[16];

    printf("sana2: private direct receive hooks\n");

    memset(&iface, 0, sizeof(iface));
    memset(&owner, 0, sizeof(owner));
    memset(&slot, 0, sizeof(slot));
    memset(&pkt, 0, sizeof(pkt));
    owner.iface   = &iface;
    slot.owner    = &owner;
    slot.packet   = &pkt;
    slot.dst      = dst;
    slot.capacity = sizeof(dst);

    h_check(ami_sana2_rx_direct(NULL, 1) == NULL,
            "a NULL direct slot is refused");
    h_check(ami_sana2_rx_direct(&slot, sizeof(dst) + 1) == NULL,
            "a direct frame over capacity is refused");
    h_check(ami_sana2_rx_direct(&slot, sizeof(dst)) == dst,
            "a direct frame at capacity gets the exact destination");
    h_check(iface.stats.rx_copy_hook == 0,
            "a claim alone is not counted as a completed fill");

    ami_sana2_rx_filled(&slot, 7, 0x12345678UL, 1);
    h_check(slot.copied == 7, "direct completion records its length");
    h_check(iface.stats.rx_copy_hook == 1,
            "direct completion counts as one receive fill");
    h_check(iface.stats.rx_copy_summed == 1,
            "a fused direct completion counts as summed");
#ifdef AMINETXDUO_RX_VERIFY
    h_check(slot.sum == 0x12345678UL && slot.summed != FALSE,
            "direct completion carries its verifier sum");
#endif

    ami_sana2_rx_filled(&slot, 5, 0, 0);
    h_check(iface.stats.rx_copy_hook == 2,
            "a non-fused direct completion is still a receive fill");
    h_check(iface.stats.rx_copy_summed == 1,
            "a non-fused direct completion is not counted as summed");
}


static void test_from_buff_guards(void)
{
    AmiTxSlot  slot;
    NX_PACKET *head;
    UCHAR      out[FRAME_LEN];
    ULONG      lens[1];

    printf("sana2: S2_CopyFromBuff rejects what it cannot answer\n");

    lens[0] = FRAME_LEN;
    head    = chain(lens, 1);

    tx_slot_init(&slot, head, FRAME_LEN);

    h_check(ami_sana2_copy_from_buff(out, NULL, 10) == FALSE,
            "a NULL slot is refused");
    h_check(ami_sana2_copy_from_buff(NULL, &slot, 10) == FALSE,
            "a NULL destination is refused");

    slot.packet = NX_NULL;
    h_check(ami_sana2_copy_from_buff(out, &slot, 10) == FALSE,
            "a slot with no packet is refused");
    slot.packet = head;

    (void)ami_sana2_copy_from_buff(out, &slot, 40);
    h_check(slot.consumed == 40, "a good call advances the cursor");
    h_check(ami_sana2_copy_from_buff(out, &slot, FRAME_LEN + 1) == FALSE,
            "a request larger than the frame is refused");
    h_check(slot.consumed == 40, "and the cursor is left where it was");
}

static void test_from_buff_whole(void)
{
    AmiTxSlot  slot;
    UCHAR      out[FRAME_LEN];
    ULONG      lens[1];

    printf("sana2: S2_CopyFromBuff, one segment in one call\n");

    lens[0] = FRAME_LEN;
    tx_slot_init(&slot, chain(lens, 1), FRAME_LEN);

    memset(out, 0, sizeof(out));
    h_check(ami_sana2_copy_from_buff(out, &slot, FRAME_LEN) == TRUE,
            "the whole frame is handed over");
    h_check(memcmp(out, frame, FRAME_LEN) == 0, "and it is the frame");
    h_check(slot.consumed == FRAME_LEN, "and the whole frame is consumed");
}

static void test_from_buff_chunked(void)
{
    AmiTxSlot  slot;
    UCHAR      out[FRAME_LEN];
    ULONG      lens[1];
    ULONG      at = 0;

    printf("sana2: S2_CopyFromBuff, one segment in unequal chunks\n");

    lens[0] = FRAME_LEN;
    tx_slot_init(&slot, chain(lens, 1), FRAME_LEN);

    memset(out, 0, sizeof(out));

    /* 7 + 1 + 200 + 92: a small chunk, the degenerate single byte, a large
       one and the remainder. Nothing here is a multiple of anything. */
    h_check(ami_sana2_copy_from_buff(out + at, &slot, 7) == TRUE, "chunk of 7");
    at += 7;
    h_check(ami_sana2_copy_from_buff(out + at, &slot, 1) == TRUE, "chunk of 1");
    at += 1;
    h_check(ami_sana2_copy_from_buff(out + at, &slot, 200) == TRUE, "chunk of 200");
    at += 200;
    h_check(ami_sana2_copy_from_buff(out + at, &slot, 92) == TRUE, "chunk of 92");
    at += 92;

    h_check(at == FRAME_LEN, "the chunks account for the frame");
    h_check(memcmp(out, frame, FRAME_LEN) == 0,
            "and reassemble into it in order");
    h_check(slot.consumed == FRAME_LEN, "and the slot agrees");
}

static void test_from_buff_chain(void)
{
    AmiTxSlot  slot;
    UCHAR      out[FRAME_LEN];
    ULONG      lens[4];

    printf("sana2: S2_CopyFromBuff walks a packet chain\n");

    lens[0] = 100;
    lens[1] = 0;
    lens[2] = 150;
    lens[3] = 50;

    tx_slot_init(&slot, chain(lens, 4), FRAME_LEN);

    memset(out, 0, sizeof(out));
    h_check(ami_sana2_copy_from_buff(out, &slot, FRAME_LEN) == TRUE,
            "one call crosses all four links");
    h_check(memcmp(out, frame, FRAME_LEN) == 0,
            "and the links come out in order, the empty one skipped");
    h_check(slot.consumed == FRAME_LEN, "and the whole frame is consumed");
}

static void test_from_buff_chain_chunked(void)
{
    AmiTxSlot  slot;
    UCHAR      out[FRAME_LEN];
    ULONG      lens[3];
    ULONG      at = 0;

    printf("sana2: S2_CopyFromBuff, chunk boundaries against link boundaries\n");

    lens[0] = 100;
    lens[1] = 100;
    lens[2] = 100;

    tx_slot_init(&slot, chain(lens, 3), FRAME_LEN);

    memset(out, 0, sizeof(out));

    /* 100 lands exactly on a link boundary; 130 lands 30 bytes into the next
       link; 70 finishes it. Both cases in one transfer. */
    h_check(ami_sana2_copy_from_buff(out + at, &slot, 100) == TRUE,
            "a chunk ending exactly on a link boundary");
    at += 100;
    h_check(ami_sana2_copy_from_buff(out + at, &slot, 130) == TRUE,
            "a chunk ending inside the next link");
    at += 130;
    h_check(ami_sana2_copy_from_buff(out + at, &slot, 70) == TRUE,
            "the remainder");
    at += 70;

    h_check(at == FRAME_LEN, "the chunks account for the frame");
    h_check(memcmp(out, frame, FRAME_LEN) == 0, "and reassemble into it");
}

static void test_from_buff_restart(void)
{
    AmiTxSlot  slot;
    UCHAR      out[FRAME_LEN];
    ULONG      lens[3];

    printf("sana2: S2_CopyFromBuff rewinds when the device starts again\n");

    lens[0] = 100;
    lens[1] = 100;
    lens[2] = 100;

    tx_slot_init(&slot, chain(lens, 3), FRAME_LEN);
    h_check(ami_sana2_copy_from_buff(out, &slot, 150) == TRUE,
            "150 bytes of a first attempt");
    h_check(slot.consumed == 150, "and the cursor is 150 in");

    memset(out, 0, sizeof(out));
    h_check(ami_sana2_copy_from_buff(out, &slot, FRAME_LEN) == TRUE,
            "the whole frame, asked for again");
    h_check(memcmp(out, frame, FRAME_LEN) == 0,
            "starts at the beginning of the frame, not 150 bytes in");
    h_check(slot.consumed == FRAME_LEN, "and consumes it exactly once");

    /* A finished transfer asked for again: consumed >= total is the trigger. */
    memset(out, 0, sizeof(out));
    h_check(ami_sana2_copy_from_buff(out, &slot, 40) == TRUE,
            "a fresh attempt after a completed one");
    h_check(memcmp(out, frame, 40) == 0, "begins at the frame again");
    h_check(slot.consumed == 40, "and the count restarts with it");

    memset(out, 0, sizeof(out));
    h_check(ami_sana2_copy_from_buff(out, &slot, 261) == TRUE,
            "a request one byte longer than the remainder");
    h_check(memcmp(out, frame, 261) == 0, "is answered from the start");
    h_check(slot.consumed == 261, "and the count follows it");

    /* Exactly the remainder is a continuation, not a restart. */
    tx_slot_init(&slot, chain(lens, 3), FRAME_LEN);
    (void)ami_sana2_copy_from_buff(out, &slot, 40);
    memset(out, 0, sizeof(out));
    h_check(ami_sana2_copy_from_buff(out, &slot, FRAME_LEN - 40) == TRUE,
            "a request of exactly the remainder");
    h_check(memcmp(out, frame + 40, FRAME_LEN - 40) == 0,
            "continues where the last call stopped");
    h_check(slot.consumed == FRAME_LEN, "and finishes the frame");
}

static void test_from_buff_short_chain(void)
{
    AmiTxSlot  slot;
    UCHAR      out[FRAME_LEN];
    ULONG      lens[2];

    printf("sana2: S2_CopyFromBuff fails rather than short-copy\n");

    lens[0] = 100;
    lens[1] = 100;

    tx_slot_init(&slot, chain(lens, 2), FRAME_LEN);

    memset(out, 0, sizeof(out));
    h_check(ami_sana2_copy_from_buff(out, &slot, FRAME_LEN) == FALSE,
            "a chain 100 bytes shorter than `total` is a failure");
    h_check(memcmp(out, frame, 200) == 0,
            "what the chain did hold was still copied");
}


static void test_from_buff_fused_checksum(void)
{
    static AmiSana2If iface;
    static UCHAR      dgram[60];
    AmiTxSlot         slot;
    NX_PACKET         pkt;
    UCHAR             out[sizeof(dgram)];
    ULONG             i;
    ULONG             deferred_before = h_deferred_checksums;

    printf("sana2: S2_CopyFromBuff, TCP checksum fused into the copy\n");

    /* 20 bytes of IPv4 header, 20 of TCP, 20 of payload. */
    memset(dgram, 0, sizeof(dgram));
    dgram[0]  = 0x45;                       /* version 4, ihl 5             */
    dgram[2]  = 0;
    dgram[3]  = (UCHAR)sizeof(dgram);       /* total length                 */
    dgram[9]  = 6;                          /* TCP                          */
    dgram[12] = 10;  dgram[13] = 0; dgram[14] = 0; dgram[15] = 1;
    dgram[16] = 10;  dgram[17] = 0; dgram[18] = 0; dgram[19] = 2;
    dgram[20] = 0x30; dgram[21] = 0x39;     /* source port                  */
    dgram[22] = 0x00; dgram[23] = 0x50;     /* destination port             */
    dgram[32] = 0x50;                       /* data offset 5                */
    for (i = 40; i < sizeof(dgram); i++)
        dgram[i] = (UCHAR)((i * 11 + 3) & 0xFF);

    memset(&iface, 0, sizeof(iface));
    iface.raw_mode = FALSE;

    memset(&pkt, 0, sizeof(pkt));
    pkt.nx_packet_prepend_ptr = dgram;
    pkt.nx_packet_append_ptr  = dgram + sizeof(dgram);
    pkt.nx_packet_next        = NX_NULL;
    pkt.nx_packet_interface_capability_flag =
        NX_INTERFACE_CAPABILITY_TCP_TX_CHECKSUM;

    tx_slot_init(&slot, &pkt, (ULONG)sizeof(dgram));
    slot.iface = &iface;

    memset(out, 0, sizeof(out));
    h_check(ami_sana2_copy_from_buff(out, &slot, (ULONG)sizeof(dgram)) == TRUE,
            "a fused frame is handed over, not reported as a failed copy");
    h_check(slot.consumed == (ULONG)sizeof(dgram),
            "and the whole frame is consumed");
    h_check(h_deferred_checksums == deferred_before,
            "and NetX Duo was not asked to do it again");

    h_check(memcmp(out, dgram, 36) == 0 &&
            memcmp(out + 38, dgram + 38, sizeof(dgram) - 38) == 0,
            "and the bytes are the datagram");
    h_check(out[36] != 0 || out[37] != 0, "and the checksum field was filled");

    /* The capability flag is cleared, so a retransmission of the same packet
       is not summed a second time over a segment that now carries one. */
    h_check((pkt.nx_packet_interface_capability_flag &
             NX_INTERFACE_CAPABILITY_TCP_TX_CHECKSUM) == 0,
            "and the packet no longer claims the checksum is owed");
}


int main(void)
{
    frame_init();

    test_copy_to_buff();
    test_rx_direct();
    test_from_buff_guards();
    test_from_buff_whole();
    test_from_buff_chunked();
    test_from_buff_chain();
    test_from_buff_chain_chunked();
    test_from_buff_restart();
    test_from_buff_short_chain();
    test_from_buff_fused_checksum();

    printf("%lu checks, %lu failures, %s\n", h_checks, h_failures,
           (h_failures == 0) ? "PASS" : "FAIL");

    return (h_failures == 0) ? 0 : 1;
}

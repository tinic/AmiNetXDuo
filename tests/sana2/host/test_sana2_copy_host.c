/*
 * AmiNetXDuo -- the SANA-II buffer-management hooks, on the host.
 *
 * S2_CopyToBuff and S2_CopyFromBuff are the two functions a driver calls with
 * our packet in its hands, at interrupt level, and nothing in the tree tested
 * either of them. tests/tcpdrill's tapdev.c calls them, but it asserts about
 * TCP frames on the wire: a cursor that rewound one segment too far would show
 * up there as a retransmission, if it showed up at all.
 *
 * What is under test is the arithmetic, and it is real: src/sana2/sana2_copy.c
 * is compiled into this binary. Only n68k_copy_bytes() is stubbed, with
 * memcpy -- the copy primitive is priced by tests/perf and has its own
 * assembly; the cursor around it has nothing.
 *
 * The awkward part is S2_CopyFromBuff. A device may take a frame in one call
 * or in several, and may restart the whole transfer when it has to retry the
 * wire, with no signal that it has done so. The rewind conditions are what
 * distinguish the two, and getting them wrong sends a frame that is part
 * header and part middle -- at interrupt level, on a machine with no MMU.
 *
 * SPDX-License-Identifier: MIT
 */

#include "sana2_internal.h"

#include <stdio.h>
#include <string.h>


/* --------------------------------------------------------------- harness -- */

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


/* ----------------------------------------------------------------- stubs -- */

/* The real one is src/net68k/n68k_copy.S. Everything below is about which
   bytes are asked for, not how they are moved. */
VOID n68k_copy_bytes(UCHAR *to, const UCHAR *from, ULONG len)
{
    if (len != 0)
        memcpy(to, from, (size_t)len);
}


/* ------------------------------------------------------------- the frame -- */

/*
 * A recognisable 300-byte frame: byte i is (i * 7 + 1) & 0xff, so a copy that
 * is off by one segment or one chunk cannot pass by looking plausible.
 */
#define FRAME_LEN   300

static UCHAR frame[FRAME_LEN];

static void frame_init(void)
{
    ULONG i;

    for (i = 0; i < FRAME_LEN; i++)
        frame[i] = (UCHAR)((i * 7 + 1) & 0xFF);
}

/*
 * NX_PACKETs built by hand. The hooks read exactly three fields --
 * nx_packet_prepend_ptr, nx_packet_append_ptr and nx_packet_next -- so a real
 * packet pool would add nothing but a dependency.
 */
#define MAX_SEGS    6

static NX_PACKET seg[MAX_SEGS];
static UCHAR     segdata[MAX_SEGS][FRAME_LEN];

/*
 * Cut `frame` into `nsegs` links of the given lengths and return the head.
 * A length of 0 makes an empty link, which is the case the `off >= have` skip
 * in the walk exists for.
 */
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


/* ------------------------------------------------------- S2_CopyToBuff --- */

/*
 * Receive. The device has a contiguous frame and writes it where the posted
 * CMD_READ said to. The one thing that must never happen is a write past
 * `capacity`: that is our packet buffer, and past its end is somebody else's.
 */
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

    /* Nothing to copy into, nothing to copy from. */
    h_check(ami_sana2_copy_to_buff(NULL, frame, 10) == FALSE,
            "a NULL slot is refused");
    h_check(ami_sana2_copy_to_buff(&slot, NULL, 10) == FALSE,
            "a NULL source is refused");

    /* A slot the reader never finished arming. */
    slot.packet = NX_NULL;
    h_check(ami_sana2_copy_to_buff(&slot, frame, 10) == FALSE,
            "a slot with no packet is refused");
    slot.packet = &pkt;

    slot.dst = NULL;
    h_check(ami_sana2_copy_to_buff(&slot, frame, 10) == FALSE,
            "a slot with no destination is refused");
    slot.dst = dst;

    /* The overrun, which is the whole point of the capacity field. */
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


/* ----------------------------------------------------- S2_CopyFromBuff --- */

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

    /*
     * More than the frame holds. Refused before the cursor is touched: a
     * device that asks for too much is confused, and moving the cursor for it
     * would corrupt the transfer it is confused about.
     */
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

    /*
     * Four links, one of them empty. A TCP send larger than one packet buffer
     * arrives as a chain, and nothing stops a link being empty, so the walk
     * has to step over one rather than stop at it.
     */
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

/*
 * The retry case, and the reason the rewind conditions exist.
 *
 * A device that has to retry the wire starts the transfer again from the top
 * and says nothing. All the hook sees is a request it cannot satisfy from
 * where the cursor stands, so that is what it treats as a restart.
 */
static void test_from_buff_restart(void)
{
    AmiTxSlot  slot;
    UCHAR      out[FRAME_LEN];
    ULONG      lens[3];

    printf("sana2: S2_CopyFromBuff rewinds when the device starts again\n");

    lens[0] = 100;
    lens[1] = 100;
    lens[2] = 100;

    /* Half a transfer, then the device gives up and asks for the whole frame. */
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

    /*
     * The narrow one: 40 bytes taken, 260 left, and the device asks for 261.
     * It cannot be a continuation, so it is a restart -- and the answer must
     * be the first 261 bytes of the frame, not the last 261.
     */
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

/*
 * A chain that does not hold what `total` claims. The device is told the frame
 * is FRAME_LEN long by ios2_DataLength, so it may ask for that much; the walk
 * runs out of links and must say so rather than report success on a short
 * frame, which would put uninitialised bytes on the wire.
 */
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


/* ------------------------------------------------------------------ main -- */

int main(void)
{
    frame_init();

    test_copy_to_buff();
    test_from_buff_guards();
    test_from_buff_whole();
    test_from_buff_chunked();
    test_from_buff_chain();
    test_from_buff_chain_chunked();
    test_from_buff_restart();
    test_from_buff_short_chain();

    printf("%lu checks, %lu failures -- %s\n", h_checks, h_failures,
           (h_failures == 0) ? "PASS" : "FAIL");

    return (h_failures == 0) ? 0 : 1;
}

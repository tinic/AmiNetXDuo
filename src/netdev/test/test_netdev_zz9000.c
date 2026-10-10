/*
 * The ZZ9000 payload copies read the Zorro window longword aligned.
 *
 * The frame sits at +4 in a receive slot, so its payload begins 2 mod 4.
 * zz_copy_payload_sum has always peeled one word so the bulk reads aligned
 * addresses off the bus and the fast-RAM destination takes the misalignment;
 * the hardware-checksum fast path and the staging copy used to hand the same
 * 2 mod 4 source to the plain longword copy, two Zorro word cycles per
 * longword.  This fixture replaces the two bulk routines with recording ones
 * and drives both zz_copy_payload directly and zz_rint on both receive
 * paths, asserting the bytes and the alignment of every bulk source.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <exec/types.h>

/* The stubs below carry AMIGA_ASM_ARGS, so the pin has to be visible
   before them, not after the driver it is included from. */
#include "aminetxduo/asm_abi.h"

#include "netdev_nic.h"
#include "netdev_clock.h"

#ifndef MEMF_PUBLIC
#define MEMF_PUBLIC (1UL << 0)
#endif
#ifndef MEMF_CLEAR
#define MEMF_CLEAR  (1UL << 16)
#endif
static APTR test_alloc_mem(ULONG bytes, ULONG flags)
{
    (VOID)bytes;
    (VOID)flags;
    return NULL;
}
#define AllocMem test_alloc_mem

/* The bulk routines, recording.  Both copy exactly what they are asked to,
   so the byte comparisons below test the callers' bookkeeping, and both
   remember every source address so the alignment claim is checked rather
   than believed. */
static ULONG bulk_calls;
static ULONG bulk_misaligned;       /* sources not 0 mod 4 */
static ULONG bulk_longs;
static int   bulk_last_to_misaligned; /* the latest bulk destination */

AMIGA_ASM_ARGS VOID n68k_copy_longs(volatile void *to, const volatile void *from, ULONG longs)
{
    bulk_calls++;
    bulk_longs += longs;
    if (((uintptr_t)from & 3u) != 0)
        bulk_misaligned++;
    bulk_last_to_misaligned = (((uintptr_t)to & 3u) != 0);
    memcpy((void *)to, (const void *)from, longs << 2);
}

AMIGA_ASM_ARGS ULONG n68k_copy_longs_sum(void *to, const volatile void *from, ULONG longs)
{
    const UBYTE *s = (const UBYTE *)from;
    ULONG sum = 0;
    ULONG i;

    bulk_calls++;
    bulk_longs += longs;
    if (((uintptr_t)s & 3u) != 0)
        bulk_misaligned++;
    bulk_last_to_misaligned = (((uintptr_t)to & 3u) != 0);
    memcpy(to, s, longs << 2);
    for (i = 0; i < (longs << 2); i += 4)
    {
        ULONG w;
        memcpy(&w, s + i, sizeof(w));
        sum += w;
        if (sum < w)
            sum++;
    }
    return sum;
}

/* zz_intr()'s bounded wait for a header the ARM has counted.  One spin,
   then done; which form was armed is counted, because the measuring one
   under the service pass's Disable() is F-311. */
static ULONG waits_measuring;
static ULONG waits_isr;

VOID netdev_wait_begin(NetdevWait *w, ULONG us, ULONG spins)
{
    (VOID)us;
    (VOID)spins;
    waits_measuring++;
    w->nw_Spins = 1;
}

VOID netdev_wait_begin_isr(NetdevWait *w, ULONG us, ULONG spins)
{
    (VOID)us;
    (VOID)spins;
    waits_isr++;
    w->nw_Spins = 1;
}

BOOL netdev_wait_done(NetdevWait *w)
{
    return (BOOL)(w->nw_Spins-- == 0);
}

static UWORD test_rx_header_word(const volatile UBYTE *p);
#define ZZ_RX_HEADER_WORD(p) test_rx_header_word(p)
#include "zz9000.c"

static union
{
    ULONG align;
    UBYTE bytes[0x10000];
} board;

static NetdevNic nic;
static ZzCore    core;
static int       failures;
static ULONG     checks;

static VOID expect(int ok, const char *what)
{
    checks++;
    if (!ok)
    {
        printf("FAIL %s\n", what);
        failures++;
    }
}

/* The copies take a lone last byte out of a word read, as the far side
   allows no byte access, and place its HIGH half: on the 68k that is the
   byte at the address, on this host the one after it.  The comparison
   follows the word so the fixture checks the copy and not the host. */
static int same_bytes(const UBYTE *dst, const UBYTE *src, UWORD len)
{
    UWORD i;

    for (i = 0; i < (UWORD)(len & ~1u); i++)
        if (dst[i] != src[i])
            return 0;
    if (len & 1)
    {
        UWORD w;
        memcpy(&w, src + len - 1, sizeof(w));
        if (dst[len - 1] != (UBYTE)(w >> 8))
            return 0;
    }
    return 1;
}

static VOID bulk_reset(VOID)
{
    bulk_calls = 0;
    bulk_misaligned = 0;
    bulk_longs = 0;
}

/* ---------------------------------------------------- the helper alone --- */

/* A window whose payload begins 2 mod 4, as the card's does; a destination
   at both phases; guard bytes on every side. */
static ULONG fold_sum(ULONG sum)
{
    while (sum >> 16) sum = (sum & 0xffffu) + (sum >> 16);
    return sum;
}

static VOID payload_copy_every_length(VOID)
{
    static union { ULONG align; UBYTE b[1544]; } win;
    static union { ULONG align; UBYTE b[1544]; } out;
    unsigned cases = 0;
    for (unsigned srcphase = 0; srcphase <= 2; srcphase += 2)
      for (unsigned dstphase = 0; dstphase <= 2; dstphase += 2)
        for (unsigned kind = 0; kind < 3; kind++)
          for (unsigned len = 0; len <= 1514; len++) {
            const volatile UBYTE *src = win.b + 4 + srcphase;
            UBYTE *dst = out.b + 4 + dstphase;
            for (unsigned i = 0; i < sizeof(win.b); i++) win.b[i] = (UBYTE)(i * 37u + (len & 255));
            memset(out.b, 0xee, sizeof(out.b));
            bulk_reset();
            ULONG sum = 0;
            if (kind == 0) zz_copy_payload(dst, src, (UWORD)len);
            else if (kind == 1) sum = zz_copy_payload_sum(dst, src, (UWORD)len);
            else zz_copy_frame(dst, src, (UWORD)len);
            expect(same_bytes(dst, (const UBYTE *)src, (UWORD)len), "matrix: correct bytes");
            int guards = 1;
            for (unsigned i = 0; i < 4 + dstphase; i++) if (out.b[i] != 0xee) guards = 0;
            for (unsigned i = 4 + dstphase + len; i < sizeof(out.b); i++) if (out.b[i] != 0xee) guards = 0;
            expect(guards, "matrix: destination guards");
            expect(bulk_misaligned == 0, "matrix: all bulk Zorro sources aligned");
            unsigned peel = srcphase && len >= 2 ? 2 : 0;
            expect(bulk_longs == (len - peel) / 4, "matrix: correct bulk length");
            if (kind == 1) {
                ULONG reference = 0;
                for (unsigned i = 0; i + 2 <= len; i += 2) {
                    UWORD w; memcpy(&w, (const UBYTE *)src + i, 2);
                    reference += w;
                    reference = fold_sum(reference);
                }
                if (len & 1) {
                    UWORD w; memcpy(&w, (const UBYTE *)src + len - 1, 2);
                    reference += w & 0xff00u;
                }
                expect(fold_sum(sum) == fold_sum(reference), "matrix: folded checksum agrees with word reference");
            }
            cases++;
          }
    printf("PASS copy matrix: %u cases, all3 helpers/source+destination phases/length0..1514\n",cases);
}

/* A full 32-bit bulk sum plus either tail must fold its end-around carry. */
static VOID summed_tail_carries(VOID)
{
    static union { ULONG align; UBYTE b[12]; } win;
    static union { ULONG align; UBYTE b[12]; } out;
    UWORD tail;

    memset(win.b, 0, sizeof(win.b));
    memset(win.b + 4, 0xff, 4);       /* bulk sum is 0xffffffff */

    tail = 1;
    memcpy(win.b + 8, &tail, sizeof(tail));
    expect(zz_copy_payload_sum(out.b, win.b + 2, 8) == 1,
           "word tail folds carry after full bulk sum");

    tail = 0x0100;
    memcpy(win.b + 8, &tail, sizeof(tail));
    expect(zz_copy_payload_sum(out.b, win.b + 2, 7) == 0x0100,
           "odd-byte tail folds carry after full bulk sum");
}

/* -------------------------------------------------------- through rint --- */

/* Longword aligned like an opener's buffer; a bare UBYTE array is not
   (macOS/clang placed it 2 mod 4). */
static union
{
    ULONG align;
    UBYTE bytes[NETDEV_RXBUF_MAX + 8];
} claimed_buf;
static UBYTE *claim_dst;
static UBYTE  claim_wanted;
static ULONG  claim_sum;
static UBYTE  claim_flags;
static int    claim_done;
static UBYTE  received[NETDEV_RXBUF_MAX];
static UWORD  received_len;

static UBYTE *claim(APTR arg, const UBYTE *hdr, UWORD frame_len, APTR *token,
                    UBYTE *wanted)
{
    (VOID)arg;
    (VOID)hdr;
    (VOID)frame_len;
    *token  = (APTR)0x1234;
    *wanted = claim_wanted;
    return claim_dst;
}

static VOID claimed(APTR arg, APTR token, ULONG sum, UBYTE flags)
{
    (VOID)arg;
    expect(token == (APTR)0x1234, "claim token comes back");
    claim_sum   = sum;
    claim_flags = flags;
    claim_done  = 1;
}

static VOID receive(APTR arg, const UBYTE *frame, UWORD len)
{
    (VOID)arg;
    memcpy(received, frame, len);
    received_len = len;
}

/* A broadcast IPv4/TCP frame of `total` IP bytes the GEM says it verified. */
static UWORD present_tcp_frame_at(UWORD total, UWORD shift)
{
    volatile UWORD *length = (volatile UWORD *)(volatile void *)
                             (board.bytes + ZZ_RX_WINDOW);
    volatile UWORD *serial = length + 1;
    UBYTE *frame = board.bytes + ZZ_RX_WINDOW + ZZ_RX_PAD + shift;
    UWORD  len = (UWORD)(NETDEV_HDR_LEN + total);
    UWORD  i;

    for (i = 0; i < 6; i++)
        frame[i] = 0xff;
    for (; i < 12; i++)
        frame[i] = (UBYTE)(0x10 + i);
    frame[12] = 0x08;
    frame[13] = 0x00;
    for (i = 14; i < len; i++)
        frame[i] = (UBYTE)(i * 7u);
    frame[14] = 0x45;                   /* IPv4, no options */
    frame[16] = (UBYTE)(total >> 8);
    frame[17] = (UBYTE)total;
    frame[20] = 0;                      /* no MF, offset 0 */
    frame[21] = 0;
    frame[23] = 6;                      /* TCP */

    *length = (UWORD)(len | (shift ? ZZ_RX_LEN_OFFSET2 : 0u));
    *serial = 0x0042;
    *(volatile UWORD *)(volatile void *)(board.bytes + ZZ_REG_RX_META) =
        (UWORD)(ZZ_RXM_PRESENT | ZZ_RXM_TCP);
    return len;
}

static UWORD present_tcp_frame(UWORD total)
{
    return present_tcp_frame_at(total, 0);
}

static VOID fresh_unit(VOID)
{
    memset(&board, 0, sizeof(board));
    memset(&nic, 0, sizeof(nic));
    memset(&core, 0, sizeof(core));
    nic.board = board.bytes;
    nic.core  = &core;
    nic.rx    = receive;
    core.rx_meta = 1;
    claim_done = 0;
    received_len = 0;
    bulk_reset();
}

static VOID verified_claim_path_reads_aligned(VOID)
{
    const UBYTE *frame = board.bytes + ZZ_RX_WINDOW + ZZ_RX_PAD;
    UWORD total = 200;                  /* 20 IP + 180 TCP, odd tail below */
    UWORD len;

    fresh_unit();
    nic.rx_claim   = claim;
    nic.rx_claimed = claimed;
    claim_dst      = claimed_buf.bytes;       /* longword aligned, as an opener's is */
    claim_wanted   = ANXD_S2_RXF_VERIFIED;
    len = present_tcp_frame(total);

    expect(zz_rint(&nic), "verified: the frame is consumed");
    expect(claim_done, "verified: the claim completes");
    expect((claim_flags & ANXD_S2_RXF_VERIFIED) != 0,
           "verified: the GEM's verdict is passed on");
    expect(nic.core_stat[ZZ_ST_HW_VERIFIED] == 1, "verified: counted as such");
    expect(same_bytes(claimed_buf.bytes, frame + NETDEV_HDR_LEN, total),
           "verified: the payload bytes arrive");
    expect(bulk_misaligned == 0, "verified: every bulk read was aligned");
    expect(bulk_calls == 2, "verified: header bulk and payload bulk");
    expect(*(volatile UWORD *)(volatile void *)(board.bytes + ZZ_REG_RX_ACK)
           == 0x0042, "verified: the slot is acknowledged");
    (VOID)len;
}

static VOID summed_claim_path_still_aligned(VOID)
{
    const UBYTE *frame = board.bytes + ZZ_RX_WINDOW + ZZ_RX_PAD;
    UWORD total = 201;                  /* odd, so the tail byte is exercised */

    fresh_unit();
    nic.rx_claim   = claim;
    nic.rx_claimed = claimed;
    claim_dst      = claimed_buf.bytes;
    claim_wanted   = 0;                 /* nobody negotiated VERIFIED */
    present_tcp_frame(total);

    expect(zz_rint(&nic), "summed: the frame is consumed");
    expect(claim_done, "summed: the claim completes");
    expect((claim_flags & ANXD_S2_RXF_SUMMED) != 0, "summed: flagged SUMMED");
    expect(same_bytes(claimed_buf.bytes, frame + NETDEV_HDR_LEN, total),
           "summed: the payload bytes arrive");
    expect(bulk_misaligned == 0, "summed: every bulk read was aligned");
}

static VOID staging_path_reads_aligned(VOID)
{
    const UBYTE *frame = board.bytes + ZZ_RX_WINDOW + ZZ_RX_PAD;
    UWORD total = 199;
    UWORD len;

    fresh_unit();
    nic.rx_claim = NULL;                /* the plain SANA-II path */
    len = present_tcp_frame(total);

    expect(zz_rint(&nic), "staging: the frame is consumed");
    expect(received_len == len, "staging: the whole frame is delivered");
    expect(same_bytes(received, frame, len), "staging: the bytes match");
    expect(bulk_misaligned == 0, "staging: every bulk read was aligned");
    expect(bulk_calls == 2, "staging: header bulk and payload bulk");
}

/* The ARM's serial skips 0 and 1.  A stale window can present a frame that
   the driver acknowledged before the most recent one, not just an exact
   repeat of the last serial.  Never deliver it or move the watermark back. */
static VOID present_serial(UWORD serial)
{
    UWORD *slot = (UWORD *)(void *)(board.bytes + ZZ_RX_WINDOW);
    UBYTE *frame = board.bytes + ZZ_RX_WINDOW + ZZ_RX_PAD;

    memset(frame, 0, NETDEV_HDR_LEN + 40);
    memset(frame, 0xff, 6);             /* broadcast passes zz_rx_wanted */
    slot[0] = NETDEV_HDR_LEN + 40;
    slot[1] = serial;
}

static VOID stale_serial_recovery(VOID)
{
    fresh_unit();
    nic.core_stat[ZZ_ST_SERIAL] = 0x42;
    present_serial(0x42);
    expect(zz_rint(&nic), "repeat serial: pass acknowledges the slot");
    expect(received_len == 0, "repeat serial: frame not delivered twice");
    expect(nic.core_stat[ZZ_ST_ACK_RECOVER] == 1,
           "repeat serial: legacy advance still used");
    expect(*(UWORD *)(void *)(board.bytes + ZZ_REG_RX_ACK) == 1,
           "repeat serial: legacy acknowledge written");

    fresh_unit();
    nic.core_stat[ZZ_ST_SERIAL] = 0x42;
    present_serial(0x40);
    expect(zz_rint(&nic), "stale serial: pass acknowledges the slot");
    expect(received_len == 0, "stale serial: frame not delivered twice");
    expect(nic.core_stat[ZZ_ST_SERIAL] == 0x42,
           "stale serial: watermark does not rewind");
    expect(nic.core_stat[ZZ_ST_ACK_RECOVER] == 1,
           "stale serial: legacy advance used once");
    expect(*(UWORD *)(void *)(board.bytes + ZZ_REG_RX_ACK) == 1,
           "stale serial: legacy acknowledge written");
    expect(nic.core_stat[ZZ_ST_GAPS] == 0,
           "stale serial: no forward ring gap recorded");

    fresh_unit();
    nic.core_stat[ZZ_ST_SERIAL] = 0x42;
    present_serial(0x45);
    expect(zz_rint(&nic), "forward gap: pass acknowledges the slot");
    expect(received_len != 0, "forward gap: new frame delivered");
    expect(nic.core_stat[ZZ_ST_SERIAL] == 0x45,
           "forward gap: watermark advances");
    expect(nic.core_stat[ZZ_ST_GAPS] == 1,
           "forward gap: missing serials recorded");
    expect(*(UWORD *)(void *)(board.bytes + ZZ_REG_RX_ACK) == 0x45,
           "forward gap: exact serial acknowledged");

    fresh_unit();
    nic.core_stat[ZZ_ST_SERIAL] = 0xffff;
    present_serial(2);
    expect(zz_rint(&nic), "wrapped successor: pass acknowledges the slot");
    expect(received_len != 0, "wrapped successor: new frame delivered");
    expect(nic.core_stat[ZZ_ST_SERIAL] == 2,
           "wrapped successor: watermark advances");
    expect(nic.core_stat[ZZ_ST_GAPS] == 0,
           "wrapped successor: no ring gap recorded");
    expect(*(UWORD *)(void *)(board.bytes + ZZ_REG_RX_ACK) == 2,
           "wrapped successor: exact serial acknowledged");

    fresh_unit();
    nic.core_stat[ZZ_ST_SERIAL] = 2;
    present_serial(0xffff);
    expect(zz_rint(&nic), "stale at wrap: pass acknowledges the slot");
    expect(received_len == 0, "stale at wrap: old frame not delivered");
    expect(nic.core_stat[ZZ_ST_SERIAL] == 2,
           "stale at wrap: watermark does not rewind");
    expect(*(UWORD *)(void *)(board.bytes + ZZ_REG_RX_ACK) == 1,
           "stale at wrap: legacy acknowledge written");
}

/* F-311: the ARM says a frame waits, the window says none, in the soft
   interrupt after the ISR: the stale-header spin runs under Disable() and
   must arm the wait that never calibrates the beam. */
static VOID stale_header_wait_never_calibrates(VOID)
{
    fresh_unit();
    nic.running = TRUE;
    ZZ(&nic)->after_isr = 1;
    *(volatile UWORD *)(volatile void *)(board.bytes + ZZ_REG_RX_STATUS) = 1;
    waits_measuring = 0;
    waits_isr = 0;

    (VOID)zz_intr(&nic);
    expect(nic.core_stat[ZZ_ST_SOFT_EMPTY] == 1,
           "stale header: the soft-interrupt empty path ran");
    expect(waits_isr == 1, "stale header: the interrupt-level wait is armed");
    expect(waits_measuring == 0,
           "stale header: the calibrating wait is never armed under Disable()");
    expect(nic.core_stat[ZZ_ST_LATE_MISS] == 1,
           "stale header: an empty window is still a miss");
}

/* A stuck/stale presented serial is deliberately left in this host window.
 * The firmware would advance it on the recovery ack; here it proves that a
 * single masked software-interrupt pass stops after the specified number of
 * acknowledged slots. */
static VOID receive_pass_is_bounded(VOID)
{
    fresh_unit();
    nic.running = TRUE;
    present_serial(0x42);

    expect(zz_intr(&nic), "drain: pending receive counted as work");
    expect(nic.core_stat[ZZ_ST_BURST_MAX] == ZZ_RX_DRAIN_MAX,
           "drain: one pass stops at the ZZ9000 budget");
    expect(nic.rx_packets == 1,
           "drain: repeated serial is not delivered twice");
    expect(nic.core_stat[ZZ_ST_ACK_RECOVER] == ZZ_RX_DRAIN_MAX - 1,
           "drain: repeated serial is recovered only within the pass");
}

/* Reset is called by the VBlank watchdog, not by the ARM firmware.  Its
   register write cannot cancel GEM DMA from a still-busy TX window slot. */
static VOID reset_preserves_live_tx_slots(VOID)
{
    UBYTE frame[60];
    ULONG i;

    fresh_unit();
    memset(frame, 0x3c, sizeof(frame));
    memset(board.bytes + ZZ_TX_WINDOW, 0xa5, ZZ_TX_WINDOW_LEN);
    nic.running = TRUE;
    nic.txb_cnt = ZZ_TX_SLOTS;
    nic.txb_inuse = ZZ_TX_SLOTS;
    nic.tx_next = ZZ_TX_SLOTS;         /* slot 0 is the oldest in flight */
    nic.tx_done = 100;
    *(UWORD *)(void *)(board.bytes + ZZ_REG_TX_STATUS) =
        (UWORD)(ZZ_TXS_PRESENT | 100);

    zz_reset(&nic);
    expect(nic.txb_inuse == ZZ_TX_SLOTS,
           "reset: outstanding DMA slots are not declared free");
    expect(nic.tx_done == 100, "reset: completion baseline is not lost");
    expect(zz_tx(&nic, frame, sizeof(frame)) == DP8390_TX_BUSY,
           "reset: no send over an outstanding DMA slot");
    for (i = 0; i < ZZ_TX_WINDOW_LEN; i++)
        if (board.bytes[ZZ_TX_WINDOW + i] != 0xa5)
            break;
    expect(i == ZZ_TX_WINDOW_LEN, "reset: oldest TX window is unchanged");

    *(UWORD *)(void *)(board.bytes + ZZ_REG_TX_STATUS) =
        (UWORD)(ZZ_TXS_PRESENT | 101);
    expect(zz_tx_reclaim(&nic), "reset: a real firmware completion retires a slot");
    expect(nic.txb_inuse == ZZ_TX_SLOTS - 1,
           "reset: only the completed slot is free");
    expect(zz_tx(&nic, frame, sizeof(frame)) == 0,
           "reset: send resumes after that completion");
    expect(nic.tx_next == ZZ_TX_SLOTS + 1,
           "reset: slot cursor continues from its original origin");
    expect(nic.txb_inuse == ZZ_TX_SLOTS,
           "reset: replacement frame is counted in flight");
}

/* The firmware's completion count has its own 15-bit origin.  It counts a
   dropped submission too, and tx_next is only a window-slot cursor. */
static VOID tx_counter_reclaim(VOID)
{
    fresh_unit();
    nic.txb_inuse = 4;
    nic.tx_done = 0x7ffe;
    *(UWORD *)(void *)(board.bytes + ZZ_REG_TX_STATUS) =
        (UWORD)(ZZ_TXS_PRESENT | 1);
    expect(zz_tx_reclaim(&nic), "TX count: three completions cross wrap");
    expect(nic.txb_inuse == 1, "TX count: one slot remains in flight");
    expect(nic.tx_done == 1, "TX count: wrapped firmware count retained");
    expect(nic.tx_completed == 3, "TX count: three frames completed");

    fresh_unit();
    nic.txb_inuse = 2;
    nic.tx_done = 100;
    *(UWORD *)(void *)(board.bytes + ZZ_REG_TX_STATUS) =
        (UWORD)(ZZ_TXS_PRESENT | 104);
    expect(zz_tx_reclaim(&nic), "TX count: surplus firmware completions seen");
    expect(nic.txb_inuse == 0, "TX count: retire no more than are in flight");
    expect(nic.tx_completed == 2, "TX count: completed accounting is clamped");
    expect(nic.tx_done == 104, "TX count: raw firmware baseline resynchronised");
    nic.txb_inuse = 1;
    expect(!zz_tx_reclaim(&nic),
           "TX count: surplus completions cannot retire a later send");
    expect(nic.txb_inuse == 1, "TX count: later send stays in flight");
}

static VOID tx_offset2_negotiation(VOID)
{
    UBYTE frame[64];
    UBYTE *direct;
    UWORD command;

    fresh_unit();
    nic.running = TRUE;
    nic.txb_cnt = ZZ_TX_SLOTS;
    expect(nic.tx_at == NULL,
           "TX offset2: old firmware keeps the staging path");

    nic.tx_at = zz_tx_at;
    direct = nic.tx_at(&nic);
    expect(direct == board.bytes + ZZ_TX_WINDOW + 2,
           "TX offset2: direct frame starts two bytes into slot 0");
    memset(direct, 0x5a, sizeof(frame));
    bulk_reset();
    expect(zz_tx(&nic, direct, sizeof(frame)) == 0,
           "TX offset2: direct frame is accepted");
    command = *(UWORD *)(void *)(board.bytes + ZZ_REG_TX);
    expect((command & (ZZ_TX_ASYNC | ZZ_TX_OFFSET2 | ZZ_TX_LEN_MASK)) ==
           (ZZ_TX_ASYNC | ZZ_TX_OFFSET2 | sizeof(frame)),
           "TX offset2: command selects shifted DMA source");
    expect((command & ZZ_TX_CSUM) == 0,
           "TX offset2: without negotiated offload no checksum consent (ANX-004)");
    expect(bulk_calls == 0, "TX offset2: no staging-to-window copy");
    expect(nic.core_stat[ZZ_ST_TX_DIRECT] == 1,
           "TX offset2: direct frame is observable in device statistics");
    expect(nic.tx_at(&nic) == board.bytes + ZZ_TX_WINDOW +
                            ZZ_TX_WINDOW_LEN + 2,
           "TX offset2: next slot is selected");

    memset(frame, 0x3c, sizeof(frame));
    bulk_reset();
    expect(zz_tx(&nic, frame, sizeof(frame)) == 0,
           "TX offset2: staged request is accepted");
    command = *(UWORD *)(void *)(board.bytes + ZZ_REG_TX);
    expect((command & ZZ_TX_OFFSET2) == 0,
           "TX offset2: staged request uses the legacy slot origin");
    expect(bulk_calls != 0, "TX offset2: staged request copies normally");
    expect(nic.core_stat[ZZ_ST_TX_DIRECT] == 1,
           "TX offset2: staged frame does not increment direct count");

    nic.txb_inuse = ZZ_TX_SLOTS;
    expect(nic.tx_at(&nic) == NULL,
           "TX offset2: a full ring does not expose an owned slot");
}

static VOID tx_offset2_checksum_owner(VOID)
{
    UBYTE *direct;
    UBYTE frame[60];

    fresh_unit();
    nic.running = TRUE;
    nic.txb_cnt = ZZ_TX_SLOTS;
    nic.tx_csum = ANXD_S2_TXF_TCP;
    nic.tx_at = zz_tx_at;
    direct = nic.tx_at(&nic);
    memset(direct, 0, sizeof(frame));
    direct[12] = 0x08;          /* Ethernet IPv4 */
    direct[14] = 0x45;          /* IPv4, 20-byte header */
    direct[17] = 40;            /* 20 IP + 20 TCP */
    direct[23] = 6;             /* TCP */
    direct[46] = 0x50;          /* TCP data offset = 5 */
    direct[50] = 0x12;
    direct[51] = 0x34;
    memcpy(frame, direct, sizeof(frame));

    expect(zz_tx(&nic, direct, sizeof(frame)) == 0,
           "TX checksum: shifted frame accepted");
    expect((*(UWORD *)(void *)(board.bytes + ZZ_REG_TX) & ZZ_TX_CSUM) != 0,
           "TX checksum: a negotiated shifted frame carries the consent bit");
    expect(direct[50] == 0x12 && direct[51] == 0x34,
           "TX checksum: driver leaves shifted checksum for ARM preparation");
    expect(nic.core_stat[ZZ_ST_TX_CSUM] == 0,
           "TX checksum: shifted frame has no 68k checksum preparation");

    expect(zz_tx(&nic, frame, sizeof(frame)) == 0,
           "TX checksum: staged frame accepted");
    expect(board.bytes[ZZ_TX_WINDOW + ZZ_TX_WINDOW_LEN + 50] == 0 &&
           board.bytes[ZZ_TX_WINDOW + ZZ_TX_WINDOW_LEN + 51] == 0,
           "TX checksum: staged frame keeps the driver zeroing path");
    expect(nic.core_stat[ZZ_ST_TX_CSUM] == 1,
           "TX checksum: count only fields actually prepared by the 68k");
}

/* ANX-019: the capacity comes from the firmware's REG_ZZ_ETH_RX_FRAMES, not
   from the async-send bit or RX_STATUS's transient reservation count. */
static VOID rx_capacity_from_firmware_register(VOID)
{
    expect(zz_rx_capacity((UWORD)(ZZ_RXF_PRESENT | 56u)) == 56UL * 1514UL,
           "RX capacity: firmware says 56 frames");
    expect(zz_rx_capacity((UWORD)(ZZ_RXF_PRESENT | 32u)) == 32UL * 1514UL,
           "RX capacity: firmware says 32 frames");
    expect(zz_rx_capacity(0u) == 32UL * 1514UL,
           "RX capacity: firmware without the register -> the ring's 32");
    expect(zz_rx_capacity((UWORD)ZZ_RXF_PRESENT) == 32UL * 1514UL,
           "RX capacity: a present but zero count is not trusted");
}

/* The default layout: the payload copy reads aligned and writes 2 mod 4. */
static VOID default_layout_payload_dst_is_shifted(VOID)
{
    fresh_unit();
    nic.rx_claim   = claim;
    nic.rx_claimed = claimed;
    claim_dst      = claimed_buf.bytes;
    claim_wanted   = ANXD_S2_RXF_VERIFIED;
    present_tcp_frame(200);
    expect(zz_rint(&nic), "default layout: the frame is consumed");
    expect(bulk_last_to_misaligned,
           "default layout: the payload bulk writes 2 mod 4 (what RX offset2 removes)");
}

/* A frame the firmware shifted two bytes: payload and destination are both
   longword aligned, nothing is peeled, the bytes and the length are right. */
static VOID rx_offset2_payload_aligned_both_sides(VOID)
{
    const UBYTE *frame = board.bytes + ZZ_RX_WINDOW + ZZ_RX_PAD + 2;
    UWORD total;

    for (total = 199; total <= 202; total++)
    {
        UWORD wanted;

        for (wanted = 0; wanted < 2; wanted++)
        {
            fresh_unit();
            nic.rx_claim   = claim;
            nic.rx_claimed = claimed;
            claim_dst      = claimed_buf.bytes;
            claim_wanted   = wanted ? ANXD_S2_RXF_VERIFIED : 0;
            present_tcp_frame_at(total, 2);
            expect(zz_rint(&nic), "RX offset2: the frame is consumed");
            expect(claim_done, "RX offset2: the claim completes");
            expect(same_bytes(claimed_buf.bytes, frame + NETDEV_HDR_LEN, total),
                   "RX offset2: the payload bytes arrive");
            expect(bulk_misaligned == 0, "RX offset2: every bulk read was aligned");
            expect(!bulk_last_to_misaligned,
                   "RX offset2: the payload bulk writes longword aligned");
            expect(nic.rx_errors == 0, "RX offset2: the flag is not taken for length");
        }
    }

    fresh_unit();
    nic.rx_claim = NULL;
    present_tcp_frame_at(199, 2);
    expect(zz_rint(&nic), "RX offset2 staging: the frame is consumed");
    expect(received_len == NETDEV_HDR_LEN + 199, "RX offset2 staging: the whole frame");
    expect(same_bytes(received, frame, received_len), "RX offset2 staging: the bytes match");
    expect(bulk_misaligned == 0, "RX offset2 staging: every bulk read was aligned");
}

/* zz_init asks for the layout only when the firmware said it can. */
static VOID rx_offset2_requested_only_when_offered(VOID)
{
    fresh_unit();
    core.rx_off2 = 0;
    zz_init(&nic);
    expect(*(volatile UWORD *)(volatile void *)(board.bytes + ZZ_REG_ETH_CONFIG) == 0,
           "RX offset2: not requested from firmware that does not offer it");
    fresh_unit();
    core.rx_off2 = 1;
    zz_init(&nic);
    expect(*(volatile UWORD *)(volatile void *)(board.bytes + ZZ_REG_ETH_CONFIG) ==
           (UWORD)(ZZ_CFG_RX_OFFSET2 | 1u),
           "RX offset2: requested when offered");
}

/* Either firmware generation's capability bit turns RX offset2 on. */
static VOID rx_offset2_capability_sources(VOID)
{
    expect(zz_rx_off2_capable(0, FALSE, 0) == 0,
           "RX offset2: no capability on firmware without either bit");
    expect(zz_rx_off2_capable(ZZ_CFG_CAP_HASH, TRUE, ZZ_RXM_PRESENT) == 0,
           "RX offset2: hash and RX_META without the bits are not it");
    expect(zz_rx_off2_capable(ZZ_CFG_CAP_RX_OFFSET2, FALSE, 0) == 1,
           "RX offset2: upstream ETH_CONFIG bit 2");
    expect(zz_rx_off2_capable(0, TRUE, ZZ_RXM_RX_OFFSET2) == 1,
           "RX offset2: fork RX_META bit 12 with async TX");
    expect(zz_rx_off2_capable(0, FALSE, ZZ_RXM_RX_OFFSET2) == 0,
           "RX offset2: RX_META bit 12 alone is not trusted");
}

/* Review regression: restore the layout before handing the board back. */
static VOID rx_offset2_stop_restores_default(VOID)
{
    fresh_unit();
    core.rx_off2 = 1;
    zz_init(&nic);
    zz_stop(&nic);
    expect(*(volatile UWORD *)(volatile void *)(board.bytes + ZZ_REG_ETH_CONFIG) ==
           (UWORD)ZZ_CFG_RX_OFFSET2,
           "RX offset2: stop restores default firmware layout");
    fresh_unit();
    core.rx_off2 = 0;
    zz_stop(&nic);
    expect(*(volatile UWORD *)(volatile void *)(board.bytes + ZZ_REG_ETH_CONFIG) == 0,
           "RX offset2: stop does not command unsupported firmware");
}

/* Publish the already-filled slot after the first header read completes. */
static UWORD publish_len;
static int publication_pending;
static UWORD test_rx_header_word(const volatile UBYTE *p)
{
    UWORD got = *(const volatile UWORD *)(const volatile void *)p;
    if (publication_pending)
    {
        publication_pending = 0;
        *(volatile UWORD *)(volatile void *)(board.bytes + ZZ_RX_WINDOW) = publish_len;
        *(volatile UWORD *)(volatile void *)(board.bytes + ZZ_RX_WINDOW + 2) = 0x0042;
    }
    return got;
}

static VOID publication_between_header_reads(VOID)
{
    UWORD shift;
    for (shift = 0; shift <= 2; shift += 2)
    {
        UWORD len;
        fresh_unit();
        len = present_tcp_frame_at(46, shift);
        publish_len = (UWORD)(len | (shift ? ZZ_RX_LEN_OFFSET2 : 0));
        *(volatile UWORD *)(volatile void *)(board.bytes + ZZ_RX_WINDOW) = 0;
        *(volatile UWORD *)(volatile void *)(board.bytes + ZZ_RX_WINDOW + 2) = 0;
        publication_pending = 1;
        expect(!zz_rint(&nic), "publication: first empty serial defers frame");
        expect(nic.core_stat[ZZ_ST_OVERSIZE] == 0 && nic.rx_errors == 0,
               "publication: no spurious size rejection");
        expect(*(volatile UWORD *)(volatile void *)(board.bytes + ZZ_REG_RX_ACK) == 0,
               "publication: no ACK of frame not yet read");
        expect(zz_rint(&nic), "publication: next poll consumes published frame");
        expect(received_len == len, "publication: published frame delivered once");
        expect(nic.core_stat[ZZ_ST_OVERSIZE] == 0 && nic.rx_errors == 0,
               "publication: second poll has no size rejection");
    }
    fresh_unit();
    *(volatile UWORD *)(volatile void *)(board.bytes + ZZ_RX_WINDOW) = 1;
    *(volatile UWORD *)(volatile void *)(board.bytes + ZZ_RX_WINDOW + 2) = 0x0042;
    expect(zz_rint(&nic), "publication: actual invalid length still consumed");
    expect(nic.core_stat[ZZ_ST_OVERSIZE] == 1 && nic.rx_errors == 1,
           "publication: actual invalid length still counted");
}

int main(void)
{
    publication_between_header_reads();
    rx_offset2_stop_restores_default();
    default_layout_payload_dst_is_shifted();
    rx_offset2_payload_aligned_both_sides();
    rx_offset2_requested_only_when_offered();
    rx_offset2_capability_sources();
    payload_copy_every_length();
    summed_tail_carries();
    verified_claim_path_reads_aligned();
    summed_claim_path_still_aligned();
    staging_path_reads_aligned();
    stale_serial_recovery();
    receive_pass_is_bounded();
    stale_header_wait_never_calibrates();
    reset_preserves_live_tx_slots();
    tx_counter_reclaim();
    tx_offset2_negotiation();
    tx_offset2_checksum_owner();
    rx_capacity_from_firmware_register();

    printf("%s: zz9000 payload alignment, %lu checks, %d failure%s\n",
           failures == 0 ? "PASS" : "FAIL", (unsigned long)checks, failures,
           failures == 1 ? "" : "s");
    return failures != 0;
}

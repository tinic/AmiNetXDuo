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

VOID n68k_copy_longs(volatile void *to, const volatile void *from, ULONG longs)
{
    bulk_calls++;
    bulk_longs += longs;
    if (((uintptr_t)from & 3u) != 0)
        bulk_misaligned++;
    memcpy((void *)to, (const void *)from, longs << 2);
}

ULONG n68k_copy_longs_sum(void *to, const volatile void *from, ULONG longs)
{
    const UBYTE *s = (const UBYTE *)from;
    ULONG sum = 0;
    ULONG i;

    bulk_calls++;
    bulk_longs += longs;
    if (((uintptr_t)s & 3u) != 0)
        bulk_misaligned++;
    memcpy(to, s, longs << 2);
    for (i = 0; i < (longs << 2); i += 2)
    {
        ULONG w = ((ULONG)s[i] << 8) | s[i + 1];
        sum += w;
    }
    return sum;
}

/* zz_intr()'s bounded wait for a header the ARM has counted.  No case
   here drives it -- the fixture exercises the copies, not the stale
   header spin -- but zz9000.c references it, and until the sanitize arm
   linked without dead-stripping, nothing said so.  One spin, then done. */
VOID netdev_wait_begin(NetdevWait *w, ULONG us, ULONG spins)
{
    (VOID)us;
    (VOID)spins;
    w->nw_Spins = 1;
}

BOOL netdev_wait_done(NetdevWait *w)
{
    return (BOOL)(w->nw_Spins-- == 0);
}

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
static VOID payload_copy_every_length(VOID)
{
    static union { ULONG align; UBYTE b[128]; } win;
    static union { ULONG align; UBYTE b[128]; } out;
    UWORD len;
    UWORD phase;

    for (phase = 0; phase <= 2; phase += 2)
    {
        for (len = 0; len <= 70; len++)
        {
            const volatile UBYTE *src = win.b + 2;      /* 2 mod 4 */
            UBYTE *dst = out.b + 4 + phase;
            UWORD i;
            int   bytes_ok;
            int   guards_ok = 1;
            char  what[96];

            for (i = 0; i < sizeof(win.b); i++)
                win.b[i] = (UBYTE)(0x40 + i);
            memset(out.b, 0xee, sizeof(out.b));
            bulk_reset();

            zz_copy_payload(dst, src, len);

            bytes_ok = same_bytes(dst, (const UBYTE *)src, len);
            for (i = 0; i < 4 + phase; i++)
                if (out.b[i] != 0xee)
                    guards_ok = 0;
            for (i = (UWORD)(4 + phase + len); i < sizeof(out.b); i++)
                if (out.b[i] != 0xee)
                    guards_ok = 0;

            snprintf(what, sizeof(what), "len %u dst %u mod 4: bytes", len, (unsigned)phase);
            expect(bytes_ok, what);
            snprintf(what, sizeof(what), "len %u dst %u mod 4: guards", len, (unsigned)phase);
            expect(guards_ok, what);
            snprintf(what, sizeof(what), "len %u dst %u mod 4: bulk source aligned", len,
                    (unsigned)phase);
            expect(bulk_misaligned == 0, what);

            /* The bulk carries exactly the longwords between the first word
               and the tail, and is not called for fewer than four. */
            snprintf(what, sizeof(what), "len %u: bulk longwords", len);
            expect(bulk_longs == (len >= 2 ? (ULONG)((len - 2) >> 2) : 0), what);
            snprintf(what, sizeof(what), "len %u: bulk calls", len);
            expect(bulk_calls == (len >= 6 ? 1UL : 0UL), what);
        }
    }
}

/* -------------------------------------------------------- through rint --- */

static UBYTE  claimed_buf[NETDEV_RXBUF_MAX + 8];
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
static UWORD present_tcp_frame(UWORD total)
{
    volatile UWORD *length = (volatile UWORD *)(volatile void *)
                             (board.bytes + ZZ_RX_WINDOW);
    volatile UWORD *serial = length + 1;
    UBYTE *frame = board.bytes + ZZ_RX_WINDOW + ZZ_RX_PAD;
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

    *length = len;
    *serial = 0x0042;
    *(volatile UWORD *)(volatile void *)(board.bytes + ZZ_REG_RX_META) =
        (UWORD)(ZZ_RXM_PRESENT | ZZ_RXM_TCP);
    return len;
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
    claim_dst      = claimed_buf;       /* longword aligned, as an opener's is */
    claim_wanted   = ANXD_S2_RXF_VERIFIED;
    len = present_tcp_frame(total);

    expect(zz_rint(&nic), "verified: the frame is consumed");
    expect(claim_done, "verified: the claim completes");
    expect((claim_flags & ANXD_S2_RXF_VERIFIED) != 0,
           "verified: the GEM's verdict is passed on");
    expect(nic.core_stat[ZZ_ST_HW_VERIFIED] == 1, "verified: counted as such");
    expect(same_bytes(claimed_buf, frame + NETDEV_HDR_LEN, total),
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
    claim_dst      = claimed_buf;
    claim_wanted   = 0;                 /* nobody negotiated VERIFIED */
    present_tcp_frame(total);

    expect(zz_rint(&nic), "summed: the frame is consumed");
    expect(claim_done, "summed: the claim completes");
    expect((claim_flags & ANXD_S2_RXF_SUMMED) != 0, "summed: flagged SUMMED");
    expect(same_bytes(claimed_buf, frame + NETDEV_HDR_LEN, total),
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

int main(void)
{
    payload_copy_every_length();
    verified_claim_path_reads_aligned();
    summed_claim_path_still_aligned();
    staging_path_reads_aligned();

    printf("%s: zz9000 payload alignment, %lu checks, %d failure%s\n",
           failures == 0 ? "PASS" : "FAIL", (unsigned long)checks, failures,
           failures == 1 ? "" : "s");
    return failures != 0;
}

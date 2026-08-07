/*
 * AmiNetXDuo, the shipped copy-and-sum against its C contract, on target.
 *
 * bench/sumbench.c times candidates; this links the routine the tree actually
 * builds, so the arm a given -mcpu selects is the arm under test.  It exists
 * for 68040 and 68060, which no drill reaches: this lab has no such hardware
 * and Amiberry's cycle accounting is off above the 68020, but the ANSWERS are
 * still the machine's, and a wrong answer is what matters here.
 *
 * PHASE.  The receive path never has both ends longword aligned -- the
 * destination is data_start + 2 + 14 and the device's payload sits two off --
 * so the interesting case is a WORD aligned pair that disagree mod 4.  A
 * 68000 raises an address error on an odd address only, so the run itself is
 * the check that the relaxed gate is legal on the part that is strictest.
 *
 * SPDX-License-Identifier: MIT
 */

#include <exec/types.h>

#include <stdio.h>

extern ULONG n68k_copy_sum_longwords(ULONG *to, const ULONG *from, ULONG count);

static ULONG v_reference(ULONG *to, const ULONG *from, ULONG count)
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

#define BUFW    512
#define GUARD   0xDEADBEEFUL

/* Byte arrays, so a case can start the pair at any even offset. */
static UBYTE src_raw[BUFW * 4 + 8];
static UBYTE dst_raw[BUFW * 4 + 8];
static UBYTE ref_raw[BUFW * 4 + 8];

static ULONG rng = 0x2545f491UL;

static ULONG rnd(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

static ULONG failures;

static void run(const char *what, ULONG doff, ULONG soff)
{
    ULONG n;
    ULONG bad = 0;

    for (n = 0; n <= 72UL; n++)
    {
        ULONG *src = (ULONG *)(void *)(src_raw + soff);
        ULONG *dst = (ULONG *)(void *)(dst_raw + doff);
        ULONG *ref = (ULONG *)(void *)(ref_raw + doff);
        ULONG  i, want, got;

        for (i = 0; i < n; i++)
            src[i] = (rnd() << 8) ^ rnd();

        for (i = 0; i <= n; i++)
        {
            dst[i] = GUARD;
            ref[i] = GUARD;
        }

        want = v_reference(ref, src, n);
        got  = n68k_copy_sum_longwords(dst, src, n);

        if (got != want)
        {
            printf("FAIL %s n=%lu sum %08lx want %08lx\n", what,
                   (unsigned long)n, (unsigned long)got,
                   (unsigned long)want);
            bad++;
            continue;
        }

        for (i = 0; i < n; i++)
        {
            if (dst[i] != ref[i])
            {
                printf("FAIL %s n=%lu word %lu copied wrong\n", what,
                       (unsigned long)n, (unsigned long)i);
                bad++;
                break;
            }
        }

        if (dst[n] != GUARD)
        {
            printf("FAIL %s n=%lu wrote past the end\n", what,
                   (unsigned long)n);
            bad++;
        }
    }

    printf("  %-28s 73 counts, %lu failed\n", what, (unsigned long)bad);
    failures += bad;
}

int main(void)
{
    /* dst is longword aligned on the real path and the source is two off,
       which is the pair the gate now admits.  The others bracket it. */
    run("both longword aligned", 0, 0);
    run("dst aligned, src +2", 0, 2);
    run("dst +2, src aligned", 2, 0);
    run("both +2", 2, 2);

    printf("%s\n", failures == 0UL ? "PASS" : "FAIL");
    return failures ? 20 : 0;
}

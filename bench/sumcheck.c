/*
 * AmiNetXDuo, the shipped copy-and-sum against its C contract, on target.
 *
 * bench/sumbench.c times candidates; this links the routine the tree actually
 * builds, so the arm a given -mcpu selects is the arm under test.  It exists
 * for 68040 and 68060, which no drill reaches: this lab has no such hardware
 * and Amiberry's cycle accounting is off above the 68020, but the ANSWERS are
 * still the machine's, and a wrong answer is what matters here.
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

static ULONG src[BUFW];
static ULONG dst[BUFW + 1];
static ULONG ref[BUFW + 1];

static ULONG rng = 0x2545f491UL;

static ULONG rnd(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

int main(void)
{
    ULONG n;
    ULONG failures = 0;

    /* 0 to 72 covers the block loop, its remainder and the boundary; the
       guard longword past the end catches a block that writes one too many. */
    for (n = 0; n <= 72UL; n++)
    {
        ULONG i, want, got;

        for (i = 0; i < n; i++)
            src[i] = (rnd() << 8) ^ rnd();

        for (i = 0; i <= n; i++)
        {
            dst[i] = 0xDEADBEEFUL;
            ref[i] = 0xDEADBEEFUL;
        }

        want = v_reference(ref, src, n);
        got  = n68k_copy_sum_longwords(dst, src, n);

        if (got != want)
        {
            printf("FAIL n=%lu sum %08lx want %08lx\n",
                   (unsigned long)n, (unsigned long)got,
                   (unsigned long)want);
            failures++;
            continue;
        }

        for (i = 0; i < n; i++)
        {
            if (dst[i] != ref[i])
            {
                printf("FAIL n=%lu word %lu copied wrong\n",
                       (unsigned long)n, (unsigned long)i);
                failures++;
                break;
            }
        }

        if (dst[n] != 0xDEADBEEFUL)
        {
            printf("FAIL n=%lu wrote past the end\n", (unsigned long)n);
            failures++;
        }
    }

    printf("73 counts, %lu failed\n", (unsigned long)failures);
    printf("%s\n", failures == 0UL ? "PASS" : "FAIL");

    return failures ? 20 : 0;
}

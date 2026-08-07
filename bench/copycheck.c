/*
 * AmiNetXDuo, n68k_copy_bytes() against its contract, on target.
 *
 * This routine is AMINETXDUO_NET68K_MEMCPY: every copy in the stack goes
 * through it, so a wrong answer is not a network bug, it is everything.  The
 * short path added for small copies takes an early exit before the register
 * frame, and the lengths and alignments below cross that boundary from both
 * sides.
 *
 * A 68000 raises an address error on an odd word or longword access, so the
 * offsets 0..3 on each side are the whole point rather than a formality: this
 * running at all on the A600 arm is half the result.
 *
 * SPDX-License-Identifier: MIT
 */

#include <exec/types.h>

#include <stdio.h>

extern VOID n68k_copy_bytes(UBYTE *to, const UBYTE *from, ULONG len);

#define PAD     8
#define MAXLEN  80
#define GUARD   0xA5

static UBYTE src_raw[MAXLEN + 2 * PAD];
static UBYTE dst_raw[MAXLEN + 2 * PAD];

static ULONG rng = 0x13579bdfUL;

static ULONG rnd(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

int main(void)
{
    ULONG failures = 0;
    ULONG cases = 0;
    ULONG len, soff, doff, i;

    for (len = 0UL; len <= (ULONG)MAXLEN; len++)
    {
        for (soff = 0UL; soff < 4UL; soff++)
        {
            for (doff = 0UL; doff < 4UL; doff++)
            {
                UBYTE       *dst = (UBYTE *)(dst_raw + PAD + doff);
                const UBYTE *src = (const UBYTE *)(src_raw + PAD + soff);

                for (i = 0UL; i < (ULONG)(MAXLEN + 2 * PAD); i++)
                {
                    src_raw[i] = (UBYTE)(rnd() >> 11);
                    dst_raw[i] = (UBYTE)GUARD;
                }

                n68k_copy_bytes(dst, src, len);
                cases++;

                for (i = 0UL; i < len; i++)
                {
                    if (dst[i] != src[i])
                    {
                        printf("FAIL len=%lu s+%lu d+%lu at %lu\n",
                               (unsigned long)len, (unsigned long)soff,
                               (unsigned long)doff, (unsigned long)i);
                        failures++;
                        break;
                    }
                }

                /* Nothing before or after what was asked for. */
                if (dst[-1] != (UBYTE)GUARD || dst[len] != (UBYTE)GUARD)
                {
                    printf("FAIL len=%lu s+%lu d+%lu wrote outside\n",
                           (unsigned long)len, (unsigned long)soff,
                           (unsigned long)doff);
                    failures++;
                }
            }
        }
    }

    printf("%lu cases, %lu failed\n%s\n",
           (unsigned long)cases, (unsigned long)failures,
           failures ? "FAIL" : "PASS");

    return failures ? 20 : 0;
}

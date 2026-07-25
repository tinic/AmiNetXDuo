/*
 * AmiNetXDuo -- 64-bit unsigned division, because the toolchain has none.
 *
 * THE PROBLEM
 *
 *   $AMIGA_TOOLCHAIN_ROOT/lib/gcc/m68k-amigaos/15.2.0/libgcc.a is a ZERO BYTE
 *   file in this toolchain.  Nothing else in the tree exports __udivdi3 either
 *   (checked: libc.a, libm020/libc.a, libnix*.a, libamiga.a).  So the moment
 *   any translation unit divides a 64-bit value the link fails with
 *
 *       undefined reference to `__udivdi3'
 *
 *   and GCC emits that call for `unsigned long long / unsigned long long`
 *   whatever the optimisation level, because the 68020's divu.l only covers
 *   64/32 -> 32 and the compiler cannot prove the operands fit.
 *
 *   nx_crypto needs it in exactly one place: nx_crypto_huge_number.c's
 *   long-division quotient estimate, where HN_UBASE2 is `unsigned long long`
 *   (NX_CRYPTO_HUGE_NUMBER_BITS == 32).
 *
 * THE ALTERNATIVE WE DID NOT TAKE
 *
 *   -DNX_CRYPTO_HUGE_NUMBER_BITS=16 drops the huge-number digit to a USHORT
 *   and HN_UBASE2 to a ULONG, which removes every 64-bit operation and links
 *   clean.  It also halves the digit width, so a 2048-bit modular
 *   exponentiation does ~4x the multiplies -- on the one target where public
 *   key arithmetic is the whole question.  Supplying the helper is a few dozen
 *   instructions; halving the radix is a 4x tax on the thing being measured.
 *
 * SCOPE
 *
 *   This lives under src/tls/ because nx_crypto is currently the only consumer.
 *   If anything else in AmiNetXDuo ever divides a 64-bit value, move it to
 *   src/common/ rather than duplicating it -- a second definition would be a
 *   duplicate symbol, not a harmless copy.
 *
 * SPDX-License-Identifier: MIT
 */

typedef unsigned long long  u64;
typedef unsigned long       u32;

/*
 * 64/32 -> 32 with remainder, on hardware that has it.
 *
 * divu.l <ea>,Dr:Dq divides the 64-bit value Dr:Dq by a 32-bit divisor,
 * leaving the quotient in Dq and the remainder in Dr.  It is only defined when
 * the quotient fits in 32 bits, i.e. when hi < divisor -- every caller below
 * guarantees that before calling.
 */
#if defined(__mc68020__) || defined(__mc68030__) || defined(__mc68040__) || \
    defined(__mc68060__)
#define AMI_HAVE_DIVUL  1
#endif

static u32 ami_divu64_32(u32 hi, u32 lo, u32 divisor, u32 *remainder)
{
#ifdef AMI_HAVE_DIVUL

    __asm__ ("divu.l %2,%0:%1"
             : "+d" (hi), "+d" (lo)
             : "d"  (divisor));

    *remainder = hi;
    return(lo);

#else

u32     quotient = 0;
u32     rem = hi;
int     bit;

    /*
     * Restoring shift-subtract, 32 iterations.  Only reached on a plain 68000,
     * which this project does not target (docs/RESEARCH.md 9 decision 1) -- it
     * exists so the file is not silently wrong if someone builds -m68000.
     */
    for (bit = 31; bit >= 0; bit--)
    {
        u32 carry = rem >> 31;

        rem = (rem << 1) | ((lo >> bit) & 1UL);
        if (carry || (rem >= divisor))
        {
            rem -= divisor;
            quotient |= (1UL << bit);
        }
    }

    *remainder = rem;
    return(quotient);

#endif
}

u64 __udivdi3(u64 numerator, u64 denominator);
u64 __umoddi3(u64 numerator, u64 denominator);

static u64 ami_udivmoddi4(u64 numerator, u64 denominator, u64 *remainder)
{

u32     n_hi = (u32)(numerator >> 32);
u32     n_lo = (u32)numerator;
u32     d_hi = (u32)(denominator >> 32);
u32     d_lo = (u32)denominator;
u32     q_hi;
u32     q_lo;
u32     rem;


    if (denominator == 0)
    {
        /*
         * Undefined behaviour in C, and there is no sane answer.  Return all
         * ones rather than trapping: a divide-by-zero exception here would
         * Guru the machine in the middle of a modular exponentiation with no
         * clue as to why, and nx_crypto never divides by zero.
         */
        if (remainder != 0)
        {
            *remainder = 0;
        }
        return(~(u64)0);
    }

    if (d_hi == 0)
    {
        if (n_hi < d_lo)
        {
            /* Quotient fits in 32 bits: one instruction on 68020+. */
            q_lo = ami_divu64_32(n_hi, n_lo, d_lo, &rem);
            if (remainder != 0)
            {
                *remainder = (u64)rem;
            }
            return((u64)q_lo);
        }

        /*
         * Two steps: the high half first, then the low half carrying the
         * remainder in.  Each step's quotient fits in 32 bits by construction.
         */
        q_hi = ami_divu64_32(0, n_hi, d_lo, &rem);
        q_lo = ami_divu64_32(rem, n_lo, d_lo, &rem);

        if (remainder != 0)
        {
            *remainder = (u64)rem;
        }
        return(((u64)q_hi << 32) | (u64)q_lo);
    }

    /*
     * Divisor is >= 2^32, so the quotient is < 2^32.  Shift-subtract over the
     * 32 candidate bits.  nx_crypto reaches this only when the trial divisor
     * genuinely exceeds a word, which is rare -- it is not worth a Knuth
     * algorithm D implementation.
     */
    {
        u64 rem64 = 0;
        u32 quotient = 0;
        int bit;

        for (bit = 63; bit >= 0; bit--)
        {
            rem64 = (rem64 << 1) | ((numerator >> bit) & 1ULL);
            if (rem64 >= denominator)
            {
                rem64 -= denominator;
                if (bit < 32)
                {
                    quotient |= (1UL << bit);
                }
            }
        }

        if (remainder != 0)
        {
            *remainder = rem64;
        }
        return((u64)quotient);
    }
}

u64 __udivdi3(u64 numerator, u64 denominator)
{

    return ami_udivmoddi4(numerator, denominator, 0);
}

u64 __umoddi3(u64 numerator, u64 denominator)
{

u64     remainder;


    (void) ami_udivmoddi4(numerator, denominator, &remainder);
    return(remainder);
}

/*
 * AmiNetXDuo -- 64-bit division helpers, because the toolchain has none.
 *
 * THE PROBLEM
 *
 *   $AMIGA_TOOLCHAIN_ROOT/lib/gcc/m68k-amigaos/15.2.0/libgcc.a is a ZERO BYTE
 *   file in this toolchain.  Nothing else in the tree exports __udivdi3
 *   either (checked: libc.a, libm020/libc.a, libnix*.a, libamiga.a).  So the
 *   moment any translation unit divides a 64-bit value the link fails with
 *
 *       undefined reference to `__udivdi3'
 *
 *   and GCC emits that call for `unsigned long long / unsigned long long`
 *   whatever the optimisation level, because the 68020's divu.l only covers
 *   64/32 -> 32 and the compiler cannot prove the operands fit.
 *
 * WHO NEEDS IT
 *
 *   nx_crypto        nx_crypto_huge_number.c's long-division quotient
 *                    estimate, where HN_UBASE2 is `unsigned long long`
 *                    (NX_CRYPTO_HUGE_NUMBER_BITS == 32).  Reached from
 *                    src/tls/ and from src/crypto68k/.
 *   newlib printf    every vfprintf/vfiprintf variant references __udivdi3
 *                    and __umoddi3 for %lld and for the float paths, so any
 *                    stock C program that calls printf() fails to link.  That
 *                    is the conformance suite (tests/conformance/build.sh);
 *                    AmiNetXDuo's own code never hits it because it formats
 *                    through dos.library.
 *   E-Clock maths    src/tls/tls_amiga.c and tests/crypto68k/c68k_timer.c
 *                    convert tick deltas to microseconds with a 64-bit
 *                    intermediate on their report paths.
 *
 *   This file used to exist three times over (src/tls/tls_udivdi3.c,
 *   tests/conformance/compat/libgcc64.c, plus a second CMake target compiling
 *   the first one again for crypto68k).  One copy, here, is what the original
 *   header comment asked for the moment a second component needed it.
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
 * LINKING
 *
 *   This is built as its OWN static library (aminetxduo_m68k_rt), not folded
 *   into libaminetxduo_common.a: static archives resolve left to right in one
 *   pass, so a definition sitting in a library CMake happens to place before
 *   its consumer would not be found.  Never link two copies -- the
 *   definitions are identical and would collide.
 *
 * SPDX-License-Identifier: MIT
 */

typedef unsigned long long  u64;
typedef long long           s64;
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

u64 __udivmoddi4(u64 numerator, u64 denominator, u64 *remainder);
u64 __udivdi3(u64 numerator, u64 denominator);
u64 __umoddi3(u64 numerator, u64 denominator);
s64 __divdi3(s64 numerator, s64 denominator);
s64 __moddi3(s64 numerator, s64 denominator);

/*
 * libgcc spells this one __udivmoddi4 and it is a public entry point in its
 * own right -- GCC emits a call to it directly when it wants both halves.
 */
u64 __udivmoddi4(u64 numerator, u64 denominator, u64 *remainder)
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

    return __udivmoddi4(numerator, denominator, 0);
}

u64 __umoddi3(u64 numerator, u64 denominator)
{

u64     remainder;


    (void) __udivmoddi4(numerator, denominator, &remainder);
    return(remainder);
}

/*
 * The signed pair.  AmiNetXDuo's own code has no use for them, but newlib's
 * integer printf does (%lld with a negative value), so the conformance suite
 * needs them present.  Truncating division, as C99 requires: the quotient
 * rounds toward zero and the remainder takes the numerator's sign.
 */
s64 __divdi3(s64 numerator, s64 denominator)
{

int     negate = 0;
u64     quotient;


    if (numerator < 0)
    {
        numerator = -numerator;
        negate ^= 1;
    }
    if (denominator < 0)
    {
        denominator = -denominator;
        negate ^= 1;
    }

    quotient = __udivmoddi4((u64)numerator, (u64)denominator, 0);

    return(negate ? -(s64)quotient : (s64)quotient);
}

s64 __moddi3(s64 numerator, s64 denominator)
{

int     negate = 0;
u64     remainder = 0;


    if (numerator < 0)
    {
        numerator = -numerator;
        negate = 1;
    }
    if (denominator < 0)
    {
        denominator = -denominator;
    }

    (void) __udivmoddi4((u64)numerator, (u64)denominator, &remainder);

    return(negate ? -(s64)remainder : (s64)remainder);
}

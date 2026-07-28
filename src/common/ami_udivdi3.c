/*
 * AmiNetXDuo -- the compiler runtime this toolchain does not ship.
 *
 * The set depends on the target, and grew when the 68000 and 68060 builds were
 * added (docs/RESEARCH.md 45):
 *
 *   every target   __udivdi3 __umoddi3 __divdi3 __moddi3 __udivmoddi4
 *   68000, 68060   __muldi3 as well
 *   68000 only     __mulsi3 __udivsi3 __umodsi3 __divsi3 __modsi3
 *
 * The 68020 build needs no multiply helper because mulu.l gives it 32x32 -> 64
 * in one instruction.  The 68060 dropped that form; the 68000 never had any
 * 32-bit multiply.  Both new targets linked with one undefined symbol,
 * __muldi3, and nothing else.
 *
 *   $AMIGA_TOOLCHAIN_ROOT/lib/gcc/m68k-amigaos/15.2.0/libgcc.a is a zero-byte
 *   file in this toolchain, and nothing else in the tree exports __udivdi3
 *   (checked: libc.a, libm020/libc.a, libnix*.a, libamiga.a).  So the moment
 *   any translation unit divides a 64-bit value the link fails with
 *
 *       undefined reference to `__udivdi3'
 *
 *   GCC emits that call for `unsigned long long / unsigned long long` at every
 *   optimisation level, because the 68020's divu.l only covers 64/32 -> 32 and
 *   the compiler cannot prove the operands fit.
 *
 *   nx_crypto        nx_crypto_huge_number.c's long-division quotient
 *                    estimate, where HN_UBASE2 is `unsigned long long`
 *                    (NX_CRYPTO_HUGE_NUMBER_BITS == 32).  Reached from
 *                    src/tls/ and from src/crypto68k/.
 *   newlib printf    every vfprintf/vfiprintf variant references __udivdi3
 *                    and __umoddi3 for %lld and for the float paths, so any
 *                    stock C program that calls printf() fails to link.  That
 *                    is the conformance suite (tests/conformance/build.sh);
 *                    AmiNetXDuo's own code formats through dos.library and
 *                    never hits it.
 *   E-Clock maths    src/tls/tls_amiga.c and tests/crypto68k/c68k_timer.c
 *                    convert tick deltas to microseconds with a 64-bit
 *                    intermediate on their report paths.
 *
 *   This file replaces three earlier copies (src/tls/tls_udivdi3.c,
 *   tests/conformance/compat/libgcc64.c, and a second CMake target compiling
 *   the first one again for crypto68k).
 *
 *   -DNX_CRYPTO_HUGE_NUMBER_BITS=16 drops the huge-number digit to a USHORT
 *   and HN_UBASE2 to a ULONG, which removes every 64-bit operation and links
 *   clean.  It also halves the digit width, so a 2048-bit modular
 *   exponentiation does ~4x the multiplies, on the one target where public-key
 *   arithmetic is the whole question.  These helpers are a few dozen
 *   instructions by comparison.
 *
 *   Built as its own static library (aminetxduo_m68k_rt) rather than folded
 *   into libaminetxduo_common.a: static archives resolve left to right in one
 *   pass, so a definition in a library CMake places before its consumer would
 *   not be found.  Never link two copies; the definitions are identical and
 *   would collide.
 *
 * SPDX-License-Identifier: MIT
 */

typedef unsigned long long  u64;
typedef long long           s64;
typedef unsigned long       u32;
typedef unsigned short      u16;

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
     * Restoring shift-subtract, 32 iterations.  Reached on a plain 68000,
     * which has no divu.l at all.  It could use the two divu.w steps
     * ami_udivsi3() below does, but this is the 64-bit path: the high half
     * does not fit the 32/16 form and the general case would need four steps
     * plus the fits-in-16-bits test, for a routine the 68000 build reaches
     * only through printf and the E-Clock conversions.  Correct and dull.
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
 * libgcc spells this one __udivmoddi4, and it is a public entry point: GCC
 * calls it directly when it wants both halves.
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
 * The signed pair.  AmiNetXDuo's own code has no use for them; newlib's
 * integer printf does (%lld with a negative value), so the conformance suite
 * needs them.  Truncating division, as C99 requires: the quotient rounds
 * toward zero and the remainder takes the numerator's sign.
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

/* ------------------------------------------------------------ __muldi3 --
 *
 * 64x64 -> 64.  Needed by the 68000 and 68060 builds and by no other, for
 * opposite reasons:
 *
 *   68020/68030/68040   mulu.l Dn,Dh:Dl gives 32x32 -> 64 in one instruction,
 *                       so GCC composes the 64-bit product inline and never
 *                       calls this.
 *   68060               that 64-bit-result form is one of the integer
 *                       instructions the 68060 dropped -- it traps to vector
 *                       61 and the 68060 support code emulates it -- so GCC
 *                       will not emit it and calls here instead.
 *   68000               no 32-bit multiply of any kind; only mulu.w.
 *
 * nx_crypto's huge-number limb multiply is the caller that matters: on a
 * 68060 this routine is the bignum inner loop, reached from every RSA and EC
 * operation.
 */

/*
 * 32x32 -> 64 from four mulu.w.  The operands are declared u16 so that GCC
 * uses the widening umulhisi3 pattern (one mulu.w) rather than promoting to
 * int and calling __mulsi3, which would recurse through the routine below.
 */
static u64 ami_umul32_wide(u32 a, u32 b)
{

u16     a_lo = (u16)a;
u16     a_hi = (u16)(a >> 16);
u16     b_lo = (u16)b;
u16     b_hi = (u16)(b >> 16);
u32     ll = (u32)a_lo * (u32)b_lo;
u32     lh = (u32)a_lo * (u32)b_hi;
u32     hl = (u32)a_hi * (u32)b_lo;
u32     hh = (u32)a_hi * (u32)b_hi;
u32     mid;


    /* The two middle partial products, added at bit 16, carrying upward. */
    mid = (ll >> 16) + (u32)(u16)lh + (u32)(u16)hl;

    return(((u64)(hh + (lh >> 16) + (hl >> 16) + (mid >> 16)) << 32) |
           (u64)((mid << 16) | (u32)(u16)ll));
}

u64 __muldi3(u64 a, u64 b);

u64 __muldi3(u64 a, u64 b)
{

u32     a_lo = (u32)a;
u32     b_lo = (u32)b;
u64     product = ami_umul32_wide(a_lo, b_lo);


    /*
     * The cross terms only contribute to the high half, so their own high
     * halves fall off the end of a 64-bit result and the truncating 32x32
     * -> 32 product is all that is wanted.  ami_umul32_wide's low half is
     * that product, and going through it rather than writing `a_lo * b_hi`
     * keeps this file free of __mulsi3 calls on a 68000.
     */
    product += (u64)(u32)ami_umul32_wide(a_lo, (u32)(b >> 32)) << 32;
    product += (u64)(u32)ami_umul32_wide((u32)(a >> 32), b_lo) << 32;

    return(product);
}

/* ------------------------------------------------ the 68000 32-bit set --
 *
 * mulu.l, divu.l and divs.l do not exist before the 68020, so on a plain
 * 68000 every 32-bit `*`, `/` and `%` in C becomes a libgcc call, into the
 * zero-byte libgcc described at the top of this file.  Compiled away on every
 * other target, where GCC emits the instruction.
 *
 * newlib's `.` multilib was built for a 68000 and has the same references, so
 * without all five the 68000 build of anything that calls printf does not
 * link.
 */
#if !defined(__mc68020__) && !defined(__mc68030__) && \
    !defined(__mc68040__) && !defined(__mc68060__)

u32 __mulsi3(u32 a, u32 b);
u32 __udivsi3(u32 numerator, u32 denominator);
u32 __umodsi3(u32 numerator, u32 denominator);
long __divsi3(long numerator, long denominator);
long __modsi3(long numerator, long denominator);

u32 __mulsi3(u32 a, u32 b)
{

    return((u32)ami_umul32_wide(a, b));
}

/*
 * 32/32 -> 32 with remainder.
 *
 * divu.w divides a 32-bit register by a 16-bit operand, leaving the quotient
 * in the low word and the remainder in the high word, and sets V without
 * changing the register if the quotient will not fit in 16 bits.  Two of them
 * cover every divisor below 65536, which is nearly every division a network
 * stack or a printf performs; the step-2 quotient cannot overflow because
 * step 1 leaves a remainder strictly below the divisor.
 */
static u32 ami_udivmodsi(u32 numerator, u32 denominator, u32 *remainder)
{

u32     quotient = 0;
u32     rem = 0;
int     bit;


    if (denominator == 0)
    {
        /* Same reasoning as __udivmoddi4: a Guru here explains nothing. */
        if (remainder != 0)
        {
            *remainder = 0;
        }
        return(~(u32)0);
    }

    if (denominator < 0x10000UL)
    {
    u32 step = numerator >> 16;
    u32 q_hi;

        __asm__ ("divu.w %1,%0" : "+d" (step) : "dmi" (denominator));
        q_hi = (u32)(u16)step;

        step = ((step >> 16) << 16) | (numerator & 0xFFFFUL);
        __asm__ ("divu.w %1,%0" : "+d" (step) : "dmi" (denominator));

        if (remainder != 0)
        {
            *remainder = step >> 16;
        }
        return((q_hi << 16) | (u32)(u16)step);
    }

    /*
     * Divisor of 65536 or more, so the quotient is below 65536 and sixteen
     * restoring steps are enough.  The shift cannot lose a bit off the top:
     * the running remainder stays below the divisor, which is at most 2^32-1,
     * so testing the bit shifted out is what keeps this exact.
     */
    for (bit = 31; bit >= 16; bit--)
    {
    u32 carry = rem >> 31;

        rem = (rem << 1) | ((numerator >> bit) & 1UL);
        if (carry || rem >= denominator)
        {
            rem -= denominator;
        }
    }
    for (bit = 15; bit >= 0; bit--)
    {
    u32 carry = rem >> 31;

        rem = (rem << 1) | ((numerator >> bit) & 1UL);
        if (carry || rem >= denominator)
        {
            rem -= denominator;
            quotient |= (1UL << bit);
        }
    }

    if (remainder != 0)
    {
        *remainder = rem;
    }
    return(quotient);
}

u32 __udivsi3(u32 numerator, u32 denominator)
{

    return(ami_udivmodsi(numerator, denominator, 0));
}

u32 __umodsi3(u32 numerator, u32 denominator)
{

u32     remainder;


    (void) ami_udivmodsi(numerator, denominator, &remainder);
    return(remainder);
}

/* Truncating, as C99 requires: quotient toward zero, remainder takes the
 * numerator's sign. */
long __divsi3(long numerator, long denominator)
{

int     negate = 0;
u32     quotient;


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

    quotient = ami_udivmodsi((u32)numerator, (u32)denominator, 0);

    return(negate ? -(long)quotient : (long)quotient);
}

long __modsi3(long numerator, long denominator)
{

int     negate = 0;
u32     remainder = 0;


    if (numerator < 0)
    {
        numerator = -numerator;
        negate = 1;
    }
    if (denominator < 0)
    {
        denominator = -denominator;
    }

    (void) ami_udivmodsi((u32)numerator, (u32)denominator, &remainder);

    return(negate ? -(long)remainder : (long)remainder);
}

#endif  /* plain 68000 */

/* ------------------------------------------------------ 64-bit shifts --- */

/*
 * These appeared when the tree moved to -Os (docs/RESEARCH.md 57).
 *
 * At -O3 GCC expands 64-bit shifts inline; at -Os it calls out to libgcc,
 * which is the zero-byte file described at the top of this file. The 68020
 * build linked until the optimisation level changed and then failed on one
 * symbol, __lshrdi3, from ami_udivdi3.c itself.
 *
 * All three are provided rather than only the one that was missing, since
 * which of them a given -Os build calls for depends on the code the optimiser
 * sees.
 *
 * The shape is libgcc's: a shift count of 0 returns the value unchanged, a
 * count of 32 or more moves whole words, and a count of 64 or more is
 * undefined in C but must not fault here. The arithmetic version replicates
 * the sign bit; the logical one does not.
 */

u64 __lshrdi3(u64 value, int count);
u64 __ashldi3(u64 value, int count);
s64 __ashrdi3(s64 value, int count);

u64 __lshrdi3(u64 value, int count)
{
    if (count <= 0)
        return value;
    if (count >= 64)
        return 0;
    if (count >= 32)
        return (u64)((u32)(value >> 32) >> (count - 32));

    return (value >> count);
}

u64 __ashldi3(u64 value, int count)
{
    if (count <= 0)
        return value;
    if (count >= 64)
        return 0;
    if (count >= 32)
        return ((u64)((u32)value << (count - 32))) << 32;

    return (value << count);
}

s64 __ashrdi3(s64 value, int count)
{
    if (count <= 0)
        return value;

    /* Saturate to the sign, which is what an arithmetic shift converges to. */
    if (count >= 64)
        return (value < 0) ? (s64)-1 : (s64)0;

    if (count >= 32)
    {
        long high = (long)(u32)((u64)value >> 32);

        return (s64)(high >> (count - 32));
    }

    return (value >> count);
}

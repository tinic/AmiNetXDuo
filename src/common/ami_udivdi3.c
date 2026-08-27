/*
 * AmiNetXDuo, CPU-dispatched compiler runtime helpers.
 *
 * Built as its own static library (aminetxduo_m68k_rt); never link two copies,
 * the definitions are identical and collide.
 *
 * SPDX-License-Identifier: MIT
 */

typedef unsigned long long  u64;
typedef long long           s64;
typedef unsigned short      u16;

/*
 * 32 bits on every AmigaOS target.  The host tier compiles this file to check
 * the portable arms against the machine's own 64-bit division, and an LP64
 * host's `long` is not 32 bits.
 */
#if defined(__LP64__) || defined(_WIN64)
typedef unsigned int        u32;
typedef int                 s32;
#else
typedef unsigned long       u32;
typedef long                s32;
#endif

/*
 * Whether this file may contain 68k assembly.  Off on the host, where the
 * portable arms are the ones under test and the only reference worth having
 * -- the machine's own 64-bit divide -- is a compiler builtin.
 */
#if defined(__m68k__)
#define AMI_RT_ASM      1
#endif

/*
 * `.chip` has no push and pop: the directive closing an inline-asm block must
 * name the part the rest of the file is compiled for, not 68000.
 */
#if defined(__mc68060__)
#define AMI_CHIP_HOME   "\n\t.chip 68060"
#elif defined(__mc68040__)
#define AMI_CHIP_HOME   "\n\t.chip 68040"
#elif defined(__mc68030__)
#define AMI_CHIP_HOME   "\n\t.chip 68030"
#elif defined(__mc68020__)
#define AMI_CHIP_HOME   "\n\t.chip 68020"
#else
#define AMI_CHIP_HOME   "\n\t.chip 68000"
#endif
#define AMI_CHIP_020    ".chip 68020\n\t"

/* Compiler-generated libcalls do not exist in GIMPLE, so WPA would delete
   these bodies before LTRANS creates the references.  Each entry point needs
   its own named section for --gc-sections to work without -ffunction-sections. */
#ifdef AMI_RT_ASM
#define AMI_LATE_LIBCALL(sym) __attribute__((used, section(".text." #sym)))
#else
/* Mach-O names a section segment-first, and the host tier has no
   --gc-sections pass to satisfy. */
#define AMI_LATE_LIBCALL(sym) __attribute__((used))
#endif

static int ami_rt_020;
static int ami_rt_mulul;

void ami_rt_cpu_select(int have_68020, int have_mulul);
void ami_rt_cpu_select(int have_68020, int have_mulul)
{

    ami_rt_020   = (have_68020 != 0) ? 1 : 0;
    ami_rt_mulul = (have_mulul != 0) ? 1 : 0;
}

/*
 * 64/32 -> 32 with remainder.  divu.l Dr:Dq is only defined when the quotient
 * fits in 32 bits (hi < divisor); every caller guarantees that first.
 * Excludes the 68060, which traps on the 64-bit-result form.
 */
#if defined(__mc68020__) || defined(__mc68030__) || defined(__mc68040__)
#if !defined(__mc68060__)
#define AMI_HAVE_DIVUL  1
#endif
#endif

static u64 ami_umul32_wide(u32 a, u32 b);
static u32 ami_clz32(u32 value);

#ifndef AMI_HAVE_DIVUL

/* ----------------------------------------------------- the soft divide ---
 *
 * For the parts with no 64-bit DIVU.L: a plain 68000, and the 68060, which
 * traps that form.  Knuth's algorithm D in base 65536, which is the largest
 * base a DIVU.W can estimate a digit in.
 */

/*
 * 16x16 -> 32, the low words of both operands.  WRITTEN AS ASSEMBLY, and not
 * for speed: GCC's widening MULU.W pattern wants a pair of zero-extends, and
 * after the loop below is strength-reduced there are none left, so the C form
 * became a 32x32 multiply and a call to __mulsi3 -- four MULU.W and a jsr in
 * place of one instruction.
 */
static u32 ami_umul16(u32 a, u32 b)
{
#ifdef AMI_RT_ASM

u32     product = a;


    __asm__ ("mulu.w %1,%0" : "+d" (product) : "dmi" (b));

    return(product);

#else

u16     x = (u16)a;
u16     y = (u16)b;


    return((u32)x * (u32)y);

#endif
}

/* q*v modulo 2^32 for a q below 65536: two MULU.W where the wide form is
   four. */
static u32 ami_umul16x32(u32 q, u32 v)
{

    return(ami_umul16(q, v) + (ami_umul16(q, v >> 16) << 16));
}

/*
 * One DIVU.W: 32/16, a 16-bit quotient and a 16-bit remainder.  Valid only
 * when (numerator >> 16) < divisor, which is what stops the quotient
 * overflowing the sixteen bits the instruction writes back.
 */
static u32 ami_divu32_16_step(u32 numerator, u32 divisor, u32 *remainder)
{
#ifdef AMI_RT_ASM

u32     step = numerator;


    __asm__ ("divu.w %1,%0" : "+d" (step) : "dmi" (divisor));

    *remainder = step >> 16;
    return((u32)(u16)step);

#else

    *remainder = numerator % divisor;
    return(numerator / divisor);

#endif
}

/*
 * 32/16 with a full 32-bit quotient, divisor below 65536.  Two steps, because
 * the digit estimate below can reach 65537 and one DIVU.W cannot say so.
 */
static u32 ami_divu32_16(u32 numerator, u32 divisor, u32 *remainder)
{

u32     q_hi;
u32     rem;


#ifdef AMI_RT_ASM
    /* A 68060 arrives here: it has DIVU.L's 32-bit form and not the 64-bit
       one, so one instruction pair beats two DIVU.W.  Same pair as
       __umodsi3()'s 68020 arm. */
    if (ami_rt_020 != 0)
    {
    u32     q = numerator;
    u32     back;

        __asm__ (AMI_CHIP_020 "divu.l %1,%0" AMI_CHIP_HOME
                 : "+d" (q) : "d" (divisor));

        back = q;
        __asm__ (AMI_CHIP_020 "muls.l %1,%0" AMI_CHIP_HOME
                 : "+d" (back) : "d" (divisor));

        *remainder = numerator - back;
        return(q);
    }
#endif

    q_hi = ami_divu32_16_step(numerator >> 16, divisor, &rem);

    return((q_hi << 16) |
           ami_divu32_16_step((rem << 16) | (numerator & 0xFFFFUL), divisor,
                              remainder));
}

/*
 * 64/32 -> 32 with remainder, with no 64-bit divide instruction.  The caller's
 * precondition hi < divisor is what makes the quotient fit in 32 bits; it is
 * required and unchecked.
 *
 * Two digits of quotient, each estimated once and then corrected at most
 * twice.  A divisor below 65536 skips the estimate: hi is already the running
 * remainder there, and two DIVU.W finish the divide.  That is the case the
 * 32-step restoring loop this replaces was slowest at, relative to what the
 * hardware can do.
 */
static u32 ami_divu64_32_soft(u32 hi, u32 lo, u32 divisor, u32 *remainder)
{

u32     s;
u32     v1;
u32     v0;
u32     un32;
u32     un10;
u32     un1;
u32     un0;
u32     un21;
u32     q1;
u32     q0;
u32     rhat;


    if (divisor < 0x10000UL)
    {
        q1 = ami_divu32_16_step((hi << 16) | (lo >> 16), divisor, &rhat);
        q0 = ami_divu32_16_step((rhat << 16) | (lo & 0xFFFFUL), divisor,
                                remainder);

        return((q1 << 16) | q0);
    }

    /* Normalised so the divisor's top bit is set: that is what bounds the
       estimate below to two corrections.  hi < divisor, so the shift cannot
       carry a bit out of hi. */
    s       = ami_clz32(divisor);
    divisor = divisor << s;
    v1      = divisor >> 16;
    v0      = divisor & 0xFFFFUL;

    un32 = (hi << s) | ((s != 0UL) ? (lo >> (32 - s)) : 0UL);
    un10 = lo << s;
    un1  = un10 >> 16;
    un0  = un10 & 0xFFFFUL;

    /* ---- the high digit --------------------------------------------- */
    q1 = ami_divu32_16(un32, v1, &rhat);

    for (;;)
    {
        /* The second test is only reached with q1 below 65536, so neither
           side of it can overflow 32 bits; and rhat below 65536 is what the
           loop's own exit keeps true. */
        if (((q1 >> 16) != 0UL) ||
            (ami_umul16(q1, v0) > ((rhat << 16) | un1)))
        {
            q1--;
            rhat += v1;
            if ((rhat >> 16) == 0UL)
                continue;
        }
        break;
    }

    un21 = (un32 << 16) + un1 - ami_umul16x32(q1, divisor);

    /* ---- the low digit ---------------------------------------------- */
    q0 = ami_divu32_16(un21, v1, &rhat);

    for (;;)
    {
        if (((q0 >> 16) != 0UL) ||
            (ami_umul16(q0, v0) > ((rhat << 16) | un0)))
        {
            q0--;
            rhat += v1;
            if ((rhat >> 16) == 0UL)
                continue;
        }
        break;
    }

    /* The normalisation shift comes back out of the remainder, never out of
       the quotient. */
    *remainder = ((un21 << 16) + un0 - ami_umul16x32(q0, divisor)) >> s;

    return((q1 << 16) | q0);
}

#endif /* !AMI_HAVE_DIVUL */

static u32 ami_divu64_32(u32 hi, u32 lo, u32 divisor, u32 *remainder)
{
#ifdef AMI_HAVE_DIVUL

    __asm__ ("divu.l %2,%0:%1"
             : "+d" (hi), "+d" (lo)
             : "d"  (divisor));

    *remainder = hi;
    return(lo);

#else

    /* Caller precondition hi < divisor is required and unchecked here. */
#ifdef AMI_RT_ASM
    if (ami_rt_mulul != 0)
    {
        __asm__ (AMI_CHIP_020 "divu.l %2,%0:%1" AMI_CHIP_HOME
                 : "+d" (hi), "+d" (lo)
                 : "d"  (divisor));

        *remainder = hi;
        return(lo);
    }
#endif

    return(ami_divu64_32_soft(hi, lo, divisor, remainder));

#endif
}

/* Not __builtin_clz: on a -m68000 unit that lowers to __clzsi2, a libgcc
   member tools/check-rt-helpers.sh fails an image for taking. */
static u32 ami_clz32(u32 value)
{

u32     n = 0;


    if ((value & 0xFFFF0000UL) == 0) { n += 16; value <<= 16; }
    if ((value & 0xFF000000UL) == 0) { n += 8;  value <<= 8;  }
    if ((value & 0xF0000000UL) == 0) { n += 4;  value <<= 4;  }
    if ((value & 0xC0000000UL) == 0) { n += 2;  value <<= 2;  }
    if ((value & 0x80000000UL) == 0) { n += 1; }

    return(n);
}

u64 __udivmoddi4(u64 numerator, u64 denominator, u64 *remainder);
u64 __udivdi3(u64 numerator, u64 denominator);
u64 __umoddi3(u64 numerator, u64 denominator);
s64 __divdi3(s64 numerator, s64 denominator);
s64 __moddi3(s64 numerator, s64 denominator);

AMI_LATE_LIBCALL(__udivmoddi4) u64 __udivmoddi4(u64 numerator, u64 denominator,
                                   u64 *remainder)
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
        /* Must not trap: a divide-by-zero exception here Gurus the machine. */
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
            q_lo = ami_divu64_32(n_hi, n_lo, d_lo, &rem);
            if (remainder != 0)
            {
                *remainder = (u64)rem;
            }
            return((u64)q_lo);
        }

        q_hi = ami_divu64_32(0, n_hi, d_lo, &rem);
        q_lo = ami_divu64_32(rem, n_lo, d_lo, &rem);

        if (remainder != 0)
        {
            *remainder = (u64)rem;
        }
        return(((u64)q_hi << 32) | (u64)q_lo);
    }

    /* Divisor >= 2^32: normalise, one 64/32 estimate, one correction. */
    if (d_hi > n_hi)
    {
        if (remainder != 0)
        {
            *remainder = numerator;
        }
        return((u64)0);
    }

    {
    u32     bm = ami_clz32(d_hi);
    u32     n_2;
    u32     m_hi;
    u32     m_lo;
    u64     m;
    u32     b;


        if (bm == 0)
        {
            /* d_hi's top bit is set and d_hi <= n_hi, so the quotient is 0 or
               1 and the estimate below would need a divisor it cannot have. */
            if (n_hi > d_hi || n_lo >= d_lo)
            {
                q_lo = 1;
                m_hi = n_hi - d_hi - (n_lo < d_lo);
                n_lo = n_lo - d_lo;
                n_hi = m_hi;
            }
            else
            {
                q_lo = 0;
            }

            if (remainder != 0)
            {
                *remainder = ((u64)n_hi << 32) | (u64)n_lo;
            }
            return((u64)q_lo);
        }

        b    = 32UL - bm;
        d_hi = (d_hi << bm) | (d_lo >> b);
        d_lo = d_lo << bm;
        n_2  = n_hi >> b;
        n_hi = (n_hi << bm) | (n_lo >> b);
        n_lo = n_lo << bm;

        /* n_2 < d_hi holds by construction, which is what the 64/32 form
           needs. */
        q_lo = ami_divu64_32(n_2, n_hi, d_hi, &n_hi);

        m    = ami_umul32_wide(q_lo, d_lo);
        m_hi = (u32)(m >> 32);
        m_lo = (u32)m;

        if (m_hi > n_hi || (m_hi == n_hi && m_lo > n_lo))
        {
            q_lo--;
            m_hi = m_hi - d_hi - (m_lo < d_lo);
            m_lo = m_lo - d_lo;
        }

        if (remainder != 0)
        {
            n_hi = n_hi - m_hi - (n_lo < m_lo);
            n_lo = n_lo - m_lo;
            *remainder = ((u64)(n_hi >> bm) << 32) |
                         (u64)((n_hi << b) | (n_lo >> bm));
        }
        return((u64)q_lo);
    }
}

AMI_LATE_LIBCALL(__udivdi3) u64 __udivdi3(u64 numerator, u64 denominator)
{

    return __udivmoddi4(numerator, denominator, 0);
}

AMI_LATE_LIBCALL(__umoddi3) u64 __umoddi3(u64 numerator, u64 denominator)
{

u64     remainder;


    (void) __udivmoddi4(numerator, denominator, &remainder);
    return(remainder);
}

/* Truncating division, as C99 requires: quotient toward zero, remainder takes
 * the sign of the numerator. */
AMI_LATE_LIBCALL(__divdi3) s64 __divdi3(s64 numerator, s64 denominator)
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

AMI_LATE_LIBCALL(__moddi3) s64 __moddi3(s64 numerator, s64 denominator)
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

/*
 * 32x32 -> 64.  The u16 operands are required: they select GCC's widening
 * umulhisi3 pattern.  A promotion to int calls __mulsi3, which recurses here.
 */
static u64 ami_umul32_wide(u32 a, u32 b)
{
#if (defined(__mc68020__) || defined(__mc68030__) || defined(__mc68040__)) && \
    !defined(__mc68060__)

u32     hi;
u32     lo = a;


    __asm__ ("mulu.l %2,%1:%0"
             : "=d" (lo), "=d" (hi)
             : "dmi" (b), "0" (lo));

    return(((u64)hi << 32) | (u64)lo);

#else

#ifdef AMI_RT_ASM
    /* ami_rt_mulul is set from AttnFlags: 68020 and up, but NOT the 68060,
       which dropped the 64-bit-result MULU.L. */
    if (ami_rt_mulul != 0)
    {
    u32     w_hi;
    u32     w_lo = a;

        __asm__ (AMI_CHIP_020 "mulu.l %2,%1:%0" AMI_CHIP_HOME
                 : "=d" (w_lo), "=d" (w_hi)
                 : "dmi" (b), "0" (w_lo));

        return(((u64)w_hi << 32) | (u64)w_lo);
    }
#endif

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


    mid = (ll >> 16) + (u32)(u16)lh + (u32)(u16)hl;

    return(((u64)(hh + (lh >> 16) + (hl >> 16) + (mid >> 16)) << 32) |
           (u64)((mid << 16) | (u32)(u16)ll));
    }

#endif
}

/*
 * 32x32 -> the low 32 only.
 *
 * A whole MULU.W cheaper than ami_umul32_wide() on a part without MULU.L: the
 * high halves' product lands entirely above bit 31 and is thrown away, so
 * three of the four partial products are all a caller who shifts the answer
 * left can use.
 */
static __inline__ __attribute__((always_inline)) u32 ami_umul32_low(u32 a, u32 b)
{
#if (defined(__mc68020__) || defined(__mc68030__) || defined(__mc68040__)) && \
    !defined(__mc68060__)

u32     product = a;


    __asm__ ("mulu.l %1,%0" : "+d" (product) : "dmi" (b));

    return(product);

#else

#ifdef AMI_RT_ASM
    /* The 68020 up, the 68060 included: the 32-bit-result MULU.L is the form
       none of them dropped.  Signed or unsigned makes no difference to the
       low 32 bits, which is why __mulsi3() spells it the same way. */
    if (ami_rt_020 != 0)
    {
    u32     product = a;

        __asm__ (AMI_CHIP_020 "muls.l %1,%0" AMI_CHIP_HOME
                 : "+d" (product) : "d" (b));

        return(product);
    }
#endif

    {
u16     a_lo = (u16)a;
u16     a_hi = (u16)(a >> 16);
u16     b_lo = (u16)b;
u16     b_hi = (u16)(b >> 16);


    return(((u32)a_lo * (u32)b_lo) +
           ((((u32)a_lo * (u32)b_hi) + ((u32)a_hi * (u32)b_lo)) << 16));
    }

#endif
}

u64 __muldi3(u64 a, u64 b);

AMI_LATE_LIBCALL(__muldi3) u64 __muldi3(u64 a, u64 b)
{

u32     a_lo = (u32)a;
u32     b_lo = (u32)b;
u64     product = ami_umul32_wide(a_lo, b_lo);


    /* Both cross terms are shifted 32 left, so only their low halves survive
       and ami_umul32_low() is the right multiply for them -- ami_umul32_wide()
       computed a high half that was discarded, three MULU.W wasted per call on
       a 68000.  Neither goes through `a_lo * b_hi`: that lowers to __mulsi3,
       which is in this file. */
    product += (u64)(ami_umul32_low(a_lo, (u32)(b >> 32)) +
                     ami_umul32_low((u32)(a >> 32), b_lo)) << 32;

    return(product);
}

/* ------------------------------------------------ the 68000 32-bit set --- */
#if !defined(__mc68020__) && !defined(__mc68030__) && \
    !defined(__mc68040__) && !defined(__mc68060__)

u32 __mulsi3(u32 a, u32 b);
u32 __udivsi3(u32 numerator, u32 denominator);
u32 __umodsi3(u32 numerator, u32 denominator);
s32 __divsi3(s32 numerator, s32 denominator);
s32 __modsi3(s32 numerator, s32 denominator);


AMI_LATE_LIBCALL(__mulsi3) u32 __mulsi3(u32 a, u32 b)
{

#ifdef AMI_RT_ASM
    if (ami_rt_020 != 0)
    {
        /* Signed or unsigned makes no difference to the low 32 bits. */
        __asm__ (AMI_CHIP_020 "muls.l %1,%0" AMI_CHIP_HOME
                 : "+d" (a) : "d" (b));
        return(a);
    }
#endif

    return((u32)ami_umul32_wide(a, b));
}

/*
 * 32/32 -> 32 with remainder: the soft 64-bit divide with a zero high word.
 * It reduces to the same two DIVU.W for a divisor below 65536, and for a
 * larger one it is the same two-digit estimate rather than sixteen restoring
 * steps.
 */
static u32 ami_udivmodsi(u32 numerator, u32 denominator, u32 *remainder)
{

u32     quotient;
u32     rem;


    if (denominator == 0)
    {
        if (remainder != 0)
        {
            *remainder = 0;
        }
        return(~(u32)0);
    }

    quotient = ami_divu64_32_soft(0, numerator, denominator, &rem);

    if (remainder != 0)
    {
        *remainder = rem;
    }
    return(quotient);
}

AMI_LATE_LIBCALL(__udivsi3) u32 __udivsi3(u32 numerator, u32 denominator)
{

#ifdef AMI_RT_ASM
    if (ami_rt_020 != 0 && denominator != 0)
    {
        __asm__ (AMI_CHIP_020 "divu.l %1,%0" AMI_CHIP_HOME
                 : "+d" (numerator) : "d" (denominator));
        return(numerator);
    }
#endif

    return(ami_udivmodsi(numerator, denominator, 0));
}

AMI_LATE_LIBCALL(__umodsi3) u32 __umodsi3(u32 numerator, u32 denominator)
{

u32     remainder;


#ifdef AMI_RT_ASM
    if (ami_rt_020 != 0 && denominator != 0)
    {
    u32     q = numerator;

        __asm__ (AMI_CHIP_020 "divu.l %1,%0" AMI_CHIP_HOME
                 : "+d" (q) : "d" (denominator));
        __asm__ (AMI_CHIP_020 "muls.l %1,%0" AMI_CHIP_HOME
                 : "+d" (q) : "d" (denominator));
        return(numerator - q);
    }
#endif

    (void) ami_udivmodsi(numerator, denominator, &remainder);
    return(remainder);
}

/* Truncating, as C99 requires: quotient toward zero, remainder takes the sign
 * of the numerator. */
AMI_LATE_LIBCALL(__divsi3) s32 __divsi3(s32 numerator, s32 denominator)
{

int     negate = 0;
u32     quotient;


#ifdef AMI_RT_ASM
    if (ami_rt_020 != 0 && denominator != 0)
    {
    s32     q = numerator;

        __asm__ (AMI_CHIP_020 "divs.l %1,%0" AMI_CHIP_HOME
                 : "+d" (q) : "d" (denominator));
        return(q);
    }
#endif

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

    return(negate ? -(s32)quotient : (s32)quotient);
}

AMI_LATE_LIBCALL(__modsi3) s32 __modsi3(s32 numerator, s32 denominator)
{

int     negate = 0;
u32     remainder = 0;


#ifdef AMI_RT_ASM
    if (ami_rt_020 != 0 && denominator != 0)
    {
    s32     q = numerator;

        __asm__ (AMI_CHIP_020 "divs.l %1,%0" AMI_CHIP_HOME
                 : "+d" (q) : "d" (denominator));
        __asm__ (AMI_CHIP_020 "muls.l %1,%0" AMI_CHIP_HOME
                 : "+d" (q) : "d" (denominator));
        return(numerator - q);
    }
#endif

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

    return(negate ? -(s32)remainder : (s32)remainder);
}

#endif  /* plain 68000 */

/* ------------------------------------------------------ 64-bit shifts --- */

/*
 * Every shift below must stay a 32-bit shift.  A 64-bit shift by a variable
 * count is what GCC lowers to a call to these functions, so `value >> count`
 * on a u64 compiles into infinite recursion.  Gated by check-rt-recursion.sh.
 */

u64 __lshrdi3(u64 value, int count);
u64 __ashldi3(u64 value, int count);
s64 __ashrdi3(s64 value, int count);

AMI_LATE_LIBCALL(__lshrdi3) u64 __lshrdi3(u64 value, int count)
{

u32     hi = (u32)(value >> 32);
u32     lo = (u32)value;


    if (count <= 0)
        return value;
    if (count >= 64)
        return 0;
    if (count >= 32)
        return (u64)(hi >> (count - 32));

    return (((u64)(hi >> count) << 32) |
            (u64)((lo >> count) | (hi << (32 - count))));
}

AMI_LATE_LIBCALL(__ashldi3) u64 __ashldi3(u64 value, int count)
{

u32     hi = (u32)(value >> 32);
u32     lo = (u32)value;


    if (count <= 0)
        return value;
    if (count >= 64)
        return 0;
    if (count >= 32)
        return ((u64)(lo << (count - 32)) << 32);

    return (((u64)((hi << count) | (lo >> (32 - count))) << 32) |
            (u64)(lo << count));
}

AMI_LATE_LIBCALL(__ashrdi3) s64 __ashrdi3(s64 value, int count)
{

s32     hi = (s32)(u32)((u64)value >> 32);
u32     lo = (u32)(u64)value;


    if (count <= 0)
        return value;

    if (count >= 64)
        return (value < 0) ? (s64)-1 : (s64)0;

    if (count >= 32)
        return (s64)(hi >> (count - 32));

    return (s64)(((u64)(u32)(hi >> count) << 32) |
                 (u64)((lo >> count) | ((u32)hi << (32 - count))));
}

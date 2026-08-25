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
typedef unsigned long       u32;
typedef unsigned short      u16;

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
#define AMI_LATE_LIBCALL(sym) __attribute__((used, section(".text." #sym)))

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

    /* Caller precondition hi < divisor is required and unchecked here. */
    if (ami_rt_mulul != 0)
    {
        __asm__ (AMI_CHIP_020 "divu.l %2,%0:%1" AMI_CHIP_HOME
                 : "+d" (hi), "+d" (lo)
                 : "d"  (divisor));

        *remainder = hi;
        return(lo);
    }

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

static u64 ami_umul32_wide(u32 a, u32 b);

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

u64 __muldi3(u64 a, u64 b);

AMI_LATE_LIBCALL(__muldi3) u64 __muldi3(u64 a, u64 b)
{

u32     a_lo = (u32)a;
u32     b_lo = (u32)b;
u64     product = ami_umul32_wide(a_lo, b_lo);


    /* Cross terms go through ami_umul32_wide(), not `a_lo * b_hi`, to keep
       this file free of __mulsi3 calls on a 68000. */
    product += (u64)(u32)ami_umul32_wide(a_lo, (u32)(b >> 32)) << 32;
    product += (u64)(u32)ami_umul32_wide((u32)(a >> 32), b_lo) << 32;

    return(product);
}

/* ------------------------------------------------ the 68000 32-bit set --- */
#if !defined(__mc68020__) && !defined(__mc68030__) && \
    !defined(__mc68040__) && !defined(__mc68060__)

u32 __mulsi3(u32 a, u32 b);
u32 __udivsi3(u32 numerator, u32 denominator);
u32 __umodsi3(u32 numerator, u32 denominator);
long __divsi3(long numerator, long denominator);
long __modsi3(long numerator, long denominator);


AMI_LATE_LIBCALL(__mulsi3) u32 __mulsi3(u32 a, u32 b)
{

    if (ami_rt_020 != 0)
    {
        /* Signed or unsigned makes no difference to the low 32 bits. */
        __asm__ (AMI_CHIP_020 "muls.l %1,%0" AMI_CHIP_HOME
                 : "+d" (a) : "d" (b));
        return(a);
    }

    return((u32)ami_umul32_wide(a, b));
}

/*
 * 32/32 -> 32 with remainder.  Two divu.w cover every divisor below 65536;
 * the step-2 quotient cannot overflow because step 1 leaves a remainder
 * strictly below the divisor.
 */
static u32 ami_udivmodsi(u32 numerator, u32 denominator, u32 *remainder)
{

u32     quotient = 0;
u32     rem = 0;
int     bit;


    if (denominator == 0)
    {
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

    /* Divisor >= 65536, so the quotient is below 65536 and sixteen restoring
       steps are enough. */
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

AMI_LATE_LIBCALL(__udivsi3) u32 __udivsi3(u32 numerator, u32 denominator)
{

    if (ami_rt_020 != 0 && denominator != 0)
    {
        __asm__ (AMI_CHIP_020 "divu.l %1,%0" AMI_CHIP_HOME
                 : "+d" (numerator) : "d" (denominator));
        return(numerator);
    }

    return(ami_udivmodsi(numerator, denominator, 0));
}

AMI_LATE_LIBCALL(__umodsi3) u32 __umodsi3(u32 numerator, u32 denominator)
{

u32     remainder;


    if (ami_rt_020 != 0 && denominator != 0)
    {
    u32     q = numerator;

        __asm__ (AMI_CHIP_020 "divu.l %1,%0" AMI_CHIP_HOME
                 : "+d" (q) : "d" (denominator));
        __asm__ (AMI_CHIP_020 "muls.l %1,%0" AMI_CHIP_HOME
                 : "+d" (q) : "d" (denominator));
        return(numerator - q);
    }

    (void) ami_udivmodsi(numerator, denominator, &remainder);
    return(remainder);
}

/* Truncating, as C99 requires: quotient toward zero, remainder takes the sign
 * of the numerator. */
AMI_LATE_LIBCALL(__divsi3) long __divsi3(long numerator, long denominator)
{

int     negate = 0;
u32     quotient;


    if (ami_rt_020 != 0 && denominator != 0)
    {
    long    q = numerator;

        __asm__ (AMI_CHIP_020 "divs.l %1,%0" AMI_CHIP_HOME
                 : "+d" (q) : "d" (denominator));
        return(q);
    }

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

AMI_LATE_LIBCALL(__modsi3) long __modsi3(long numerator, long denominator)
{

int     negate = 0;
u32     remainder = 0;


    if (ami_rt_020 != 0 && denominator != 0)
    {
    long    q = numerator;

        __asm__ (AMI_CHIP_020 "divs.l %1,%0" AMI_CHIP_HOME
                 : "+d" (q) : "d" (denominator));
        __asm__ (AMI_CHIP_020 "muls.l %1,%0" AMI_CHIP_HOME
                 : "+d" (q) : "d" (denominator));
        return(numerator - q);
    }

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

long    hi = (long)(u32)((u64)value >> 32);
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

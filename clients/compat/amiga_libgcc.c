/*
 * clients/compat, compiler-runtime helpers needed by the ported clients beyond
 * the 64-bit arithmetic already in src/common.
 *
 * __ctzdi2, __popcountdi2, __floatdidf, __fixdfdi, __fixunsdfdi,
 * __unordsf2, __atomic_exchange_4 and nothing else; the division helpers stay in
 * src/common/ami_udivdi3.c, which clients/amiga-client.sh compiles alongside
 * this file.
 *
 * __floatdidf and __fixdfdi go through mathieeedoubbas.library, which is not in
 * Kickstart 3.1 ROM and must be in LIBS:.  __atomic_exchange_4 uses Disable()
 * rather than Forbid(), because it must be safe against an interrupt.
 *
 * SPDX-License-Identifier: MIT
 */

#include <exec/types.h>
#include <proto/exec.h>

typedef unsigned long long u64;
typedef long long          s64;
typedef unsigned long      u32;

int __ctzdi2(u64 x);
int __popcountdi2(u64 x);
double __floatdidf(s64 x);
s64 __fixdfdi(double x);
u64 __fixunsdfdi(double x);
int __unordsf2(float a, float b);
u32 __atomic_exchange_4(volatile void *ptr, u32 val, int memorder);


/* Count trailing zeros.  GCC leaves x == 0 undefined and every caller in a
   client guards it; this returns 64 anyway. */
int __ctzdi2(u64 x)
{
    u32 lo = (u32)x;
    u32 hi = (u32)(x >> 32);
    u32 w;
    int n;

    if (lo != 0)
    {
        w = lo;
        n = 0;
    }
    else if (hi != 0)
    {
        w = hi;
        n = 32;
    }
    else
    {
        return 64;
    }

    while ((w & 1UL) == 0UL)
    {
        w >>= 1;
        n++;
    }

    return n;
}

int __popcountdi2(u64 x)
{
    int n = 0;

    while (x != 0ULL)
    {
        x &= x - 1ULL;          /* clears the lowest set bit */
        n++;
    }

    return n;
}

/*
 * 64-bit signed integer -> double.
 *
 * Split rather than a single conversion because there is no 64-bit path in
 * the toolchain to call: the halves go through __floatsidf/__floatunsidf,
 * which libc.a does have.  2^32 is exact in a double, and so is each half, so
 * the only rounding is the final add, matching what the hardware would do.
 */
double __floatdidf(s64 x)
{
    s64  hi = x >> 32;
    u32  lo = (u32)x;

    return (double)(long)hi * 4294967296.0 + (double)lo;
}

/*
 * double -> 64-bit signed integer, truncating toward zero.
 *
 * Out of range is undefined behaviour in the C standard and GCC's own libgcc
 * saturates, so this saturates too: a client that asks --max-filesize 1e30
 * gets a clamp and not a random number.
 */
s64 __fixdfdi(double x)
{
    int  negative = 0;
    u32  hi;
    u32  lo;
    double t;

    if (x != x)                 /* NaN */
        return 0;

    if (x < 0.0)
    {
        negative = 1;
        x = -x;
    }

    if (x >= 9223372036854775808.0)     /* 2^63 */
        return negative ? (-9223372036854775807LL - 1LL) : 9223372036854775807LL;

    if (x < 1.0)
        return 0;

    t  = x / 4294967296.0;
    hi = (u32)(unsigned long)t;
    lo = (u32)(unsigned long)(x - (double)hi * 4294967296.0);

    {
        s64 v = (s64)(((u64)hi << 32) | (u64)lo);
        return negative ? -v : v;
    }
}

/* Same conversion for an unsigned result.  Keep the two 32-bit casts
   explicit: expressing this as a direct (u64)x would call this function
   recursively. */
u64 __fixunsdfdi(double x)
{
    u32 hi;
    u32 lo;
    double t;

    if (!(x > 0.0))             /* zero, negative, or NaN */
        return 0;
    if (x >= 18446744073709551616.0)   /* 2^64 */
        return ~0ULL;

    t  = x / 4294967296.0;
    hi = (u32)(unsigned long)t;
    lo = (u32)(unsigned long)(x - (double)hi * 4294967296.0);
    return ((u64)hi << 32) | (u64)lo;
}

/* libgcc's copy in the pinned toolchain carries DWARF sections that the Amiga
   HUNK writer mistakes for loadable segments.  Test IEEE-754 bits directly,
   both to avoid that malformed executable and to avoid recursively asking the
   compiler for the very unordered comparison being implemented. */
int __unordsf2(float a, float b)
{
    union { float f; u32 u; } ua;
    union { float f; u32 u; } ub;

    ua.f = a;
    ub.f = b;
    return ((ua.u & 0x7fffffffUL) > 0x7f800000UL ||
            (ub.u & 0x7fffffffUL) > 0x7f800000UL) ? 1 : 0;
}

/*
 * The only atomic a curl or wget build reaches.  memorder is ignored: this
 * machine has one core, no store buffer and no cache coherency protocol, so
 * every ordering is the same ordering.
 */
u32 __atomic_exchange_4(volatile void *ptr, u32 val, int memorder)
{
    volatile u32 *p = (volatile u32 *)ptr;
    u32 old;

    (void)memorder;

    Disable();
    old = *p;
    *p  = val;
    Enable();

    return old;
}

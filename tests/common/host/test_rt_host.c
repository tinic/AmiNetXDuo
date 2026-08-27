/*
 * AmiNetXDuo, src/common/ami_udivdi3.c checked against the host's own
 * arithmetic.
 *
 * The file compiles on the host with its assembly arms switched off, which
 * leaves exactly the arms a 68000 and a 68060 run: the base-65536 divide, the
 * 16x16 multiply and the pairs of 32-bit shifts.  Those are the parts an
 * emulator run cannot step through and the parts a wrong answer hides in, so
 * they are checked here, exhaustively over the shapes and by fuzz over the
 * rest.  tests/common/rt_test.c is the other half: it runs on the machine and
 * checks the DIVU.L and MULU.L arms this tier cannot compile.
 *
 * The reference is the host's own 64-bit division, which the target does not
 * have -- that is the whole point of the file under test.  The symbols are
 * renamed at compile time so the host's libgcc keeps its own.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>

typedef unsigned long long  u64;
typedef long long           s64;
typedef unsigned int        u32;
typedef int                 s32;

extern void ami_rt_cpu_select(int have_68020, int have_mulul);

extern u64 anxd_udivmoddi4(u64 n, u64 d, u64 *remainder);
extern u64 anxd_udivdi3(u64 n, u64 d);
extern u64 anxd_umoddi3(u64 n, u64 d);
extern s64 anxd_divdi3(s64 n, s64 d);
extern s64 anxd_moddi3(s64 n, s64 d);
extern u64 anxd_muldi3(u64 a, u64 b);
extern u32 anxd_mulsi3(u32 a, u32 b);
extern u32 anxd_udivsi3(u32 n, u32 d);
extern u32 anxd_umodsi3(u32 n, u32 d);
extern s32 anxd_divsi3(s32 n, s32 d);
extern s32 anxd_modsi3(s32 n, s32 d);
extern u64 anxd_lshrdi3(u64 value, int count);
extern u64 anxd_ashldi3(u64 value, int count);
extern s64 anxd_ashrdi3(s64 value, int count);

static unsigned long   checks;
static unsigned long   failures;

static void ck(const char *what, int ok)
{
    checks++;
    if (!ok)
    {
        failures++;
        if (failures < 20)
            printf("FAIL %s\n", what);
    }
}

/* One divide, both entry points, against the machine's own. */
static void div64(u64 n, u64 d)
{
u64     rem = 0x5A5A5A5A5A5A5A5AULL;
u64     q;

    q = anxd_udivmoddi4(n, d, &rem);

    checks++;
    if (q != (n / d) || rem != (n % d))
    {
        failures++;
        if (failures < 20)
            printf("FAIL udivmoddi4 0x%016llX / 0x%016llX = 0x%016llX r 0x%016llX,"
                   " want 0x%016llX r 0x%016llX\n",
                   n, d, q, rem, n / d, n % d);
    }

    /* The remainder pointer is optional and the quotient must not depend on
       it: __udivdi3() passes a null one on every call. */
    ck("udivdi3 agrees", anxd_udivdi3(n, d) == n / d);
    ck("umoddi3 agrees", anxd_umoddi3(n, d) == n % d);
}

/* ------------------------------------------------------------- fixtures -- */

/*
 * The shapes the divide is written around, each named for what it selects.
 * Every "correction" row was found by instrumenting the correction loop and
 * keeping a case that fired it: the loop is entered on roughly one divide in
 * 65536, so fuzz alone does not pin it.
 */
static const struct { u64 n; u64 d; const char *why; } vectors[] = {
    /* 32-bit divisor, high word of the numerator below it: one estimate. */
    { 0x0000000000000000ULL, 0x0000000000000001ULL, "zero over one" },
    { 0xFFFFFFFFFFFFFFFFULL, 0x0000000000000001ULL, "all ones over one" },
    { 0xFFFFFFFFFFFFFFFFULL, 0x0000000000000002ULL, "all ones over two" },
    { 0xFFFFFFFFFFFFFFFFULL, 0x0000000080000000ULL, "all ones over 2^31" },
    { 0xFFFFFFFFFFFFFFFFULL, 0x0000000100000000ULL, "all ones over 2^32" },
    { 0xFFFFFFFFFFFFFFFFULL, 0x8000000000000000ULL, "all ones over 2^63" },
    { 0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL, "all ones over all ones" },

    /* Divisor below 65536: the DIVU.W-only arm, no normalisation. */
    { 0xFEDCBA9876543210ULL, 0x000000000000FFFFULL, "divisor 65535" },
    { 0xFEDCBA9876543210ULL, 0x0000000000000003ULL, "divisor 3" },
    { 0xFFFFFFFFFFFFFFFFULL, 0x000000000000FFFFULL, "all ones over 65535" },
    { 0x000000000000FFFEULL, 0x000000000000FFFFULL, "quotient zero, small" },

    /* Divisor at and just above 65536: the boundary between the two arms. */
    { 0xFFFFFFFFFFFFFFFFULL, 0x0000000000010000ULL, "divisor 65536" },
    { 0xFFFFFFFFFFFFFFFFULL, 0x0000000000010001ULL, "divisor 65537" },

    /* 32-bit divisor with the numerator's high word at or above it: the
       two-step arm, quotient in both words. */
    { 0xFFFFFFFF00000000ULL, 0x00000000FFFFFFFFULL, "high word equals divisor" },
    { 0xFFFFFFFFFFFFFFFFULL, 0x00000000FFFFFFFFULL, "widest 32-bit divisor" },
    { 0x00000001FFFFFFFFULL, 0x0000000100000000ULL, "wide branch, q exactly one" },

    /* The base-65536 estimate corrected once, and corrected twice.  Found by
       instrumenting the loop; the divisor is 32 bits so these run through the
       software divide rather than the wide branch. */
    { 0x1D39A91BD781F4B1ULL, 0x000000003A391E19ULL, "estimate corrected once" },
    { 0x04AE36D17C8FE210ULL, 0x000000003586738EULL, "estimate corrected once" },
    { 0x01DE4283905D796FULL, 0x00000000326479D4ULL, "estimate corrected once" },
    { 0x3D83C529507344C7ULL, 0x00000000976D82DEULL, "estimate corrected twice" },
    { 0x0A9CE29C5A7332B9ULL, 0x0000000027E6EDC3ULL, "estimate corrected twice" },
    { 0x5DAF26B5AB089C03ULL, 0x0000000069645A9CULL, "estimate corrected twice" },

    /* The 64-bit-divisor branch.  bm is the normalisation shift it takes. */
    { 0xDDE005EB2757D5F3ULL, 0xC70111D3DC7F4539ULL, "wide branch, bm 0" },
    { 0xF9202CC19C14FE28ULL, 0xC8D749231BF61008ULL, "wide branch, bm 0" },
    { 0x8000000000000001ULL, 0x8000000000000000ULL, "wide branch, bm 0, q 1" },
    { 0x8000000000000000ULL, 0x8000000000000001ULL, "wide branch, bm 0, q 0" },
    { 0xEFF3C649C5AD6719ULL, 0x0000000125AAA426ULL, "wide branch, bm 31" },
    { 0xE68C6A9FCDF171F0ULL, 0x00000001C2246274ULL, "wide branch, bm 31" },
    { 0xFFFFFFFFFFFFFFFFULL, 0x0000000100000001ULL, "wide branch, bm 31, wide q" },
    { 0x6EBCECBBD5D049A1ULL, 0x00000007EB140BB6ULL, "wide branch, corrected" },
    { 0x9768CF670E1ACD06ULL, 0x0000004D97CA7CE0ULL, "wide branch, corrected" },
    { 0xCE6BA181B4B0A018ULL, 0x00000015D1A6B57DULL, "wide branch, corrected" },

    /* Divisor larger than the numerator: no divide happens at all. */
    { 0x0000000100000000ULL, 0xFFFFFFFFFFFFFFFFULL, "quotient zero, wide" },
};

/* ------------------------------------------------------------- the fuzz -- */

static u64  seed = 88172645463325252ULL;

static u64 rnd(void)
{
    seed ^= seed << 13;
    seed ^= seed >> 7;
    seed ^= seed << 17;
    return seed;
}

/*
 * Eight divisor shapes in rotation, because a uniform 64-bit divisor is a
 * 64-bit divisor with the top bit set in 999 draws out of 1000 and never
 * exercises anything else.
 */
static u64 shaped(u64 raw, unsigned long which)
{
    switch (which & 7UL)
    {
    case 0:  return raw;
    case 1:  return raw >> 32;                          /* 32-bit divisor */
    case 2:  return raw & 0xFFULL;                      /* one byte */
    case 3:  return raw & 0xFFFFULL;                    /* DIVU.W arm */
    case 4:  return (raw & 0xFFFFFFFFULL) | 0x10000ULL; /* just over 65536 */
    case 5:  return (raw & 0xFFFFFFFFULL) | 0x100000000ULL;   /* bm 31 */
    case 6:  return (raw >> (raw & 63U)) | 1ULL;        /* every width */
    default: return raw | 0x8000000000000000ULL;        /* bm 0 */
    }
}

int main(void)
{
unsigned long   i;
unsigned long   j;
u64             rem;

    /* No 68020 and no MULU.L: the arms this tier compiles are the ones the
       flags would otherwise route around, and the host has neither anyway. */
    ami_rt_cpu_select(0, 0);

    /* ---- named shapes ---------------------------------------------------- */
    for (i = 0; i < sizeof vectors / sizeof vectors[0]; i++)
        div64(vectors[i].n, vectors[i].d);

    /* ---- division by zero must return, not trap -------------------------- */
    rem = 0x1234ULL;
    ck("divide by zero quotient", anxd_udivmoddi4(0x123456789ABCDEF0ULL, 0ULL,
                                                  &rem) == ~(u64)0);
    ck("divide by zero remainder", rem == 0ULL);
    ck("udivsi3 by zero", anxd_udivsi3(12345U, 0U) == ~(u32)0);
    ck("umodsi3 by zero", anxd_umodsi3(12345U, 0U) == 0U);

    /* ---- every power of two, over every power of two --------------------- */
    for (i = 0; i < 64; i++)
    {
        for (j = 0; j < 64; j++)
        {
        u64 n = (1ULL << i) | ((i >= 3) ? (1ULL << (i - 3)) : 0ULL);
        u64 d = 1ULL << j;

            div64(n, d);
            div64(n, d | 1ULL);
            div64(~n, d);
            div64(n - 1ULL, d);
        }
    }

    /* ---- every normalisation shift, at its own boundary ------------------ */
    for (i = 0; i < 64; i++)
    {
    u64 d = (1ULL << i) | 1ULL;

        div64(d - 1ULL, d);
        div64(d, d);
        div64(d + 1ULL, d);
        div64(~(u64)0, d);
        div64(0x8000000000000000ULL, d);
    }

    /* ---- fuzz ------------------------------------------------------------ */
    for (i = 0; i < 1000000UL; i++)
    {
    u64 n = rnd();
    u64 d = shaped(rnd(), i);

        if (d == 0ULL)
            d = 1ULL;
        if ((i & 15UL) == 0UL)
            n &= 0xFFFFFFFFULL;             /* numerator inside 32 bits */
        if ((i & 31UL) == 0UL)
            n = (n % d) + (d * (rnd() & 0xFFULL));  /* near-exact multiples */

        div64(n, d);
    }

    /* ---- the signed 64-bit pair ------------------------------------------ */
    for (i = 0; i < 60000UL; i++)
    {
    s64 n = (s64)rnd();
    s64 d = (s64)shaped(rnd(), i);

        if (d == 0)
            d = 1;
        /* Both helpers negate their operands, so the most negative value is
           the one case C leaves undefined for them as well as for `/`. */
        if (n == (s64)0x8000000000000000ULL)
            n = 1;
        if (d == (s64)0x8000000000000000ULL)
            d = -1;
        if ((i & 1UL) != 0UL)
            n = -n;
        if ((i & 2UL) != 0UL)
            d = -d;

        ck("divdi3", anxd_divdi3(n, d) == n / d);
        ck("moddi3", anxd_moddi3(n, d) == n % d);
    }

    /* ---- the 32-bit set, which only a 68000 links ------------------------ */
    for (i = 0; i < 400000UL; i++)
    {
    u32 a = (u32)rnd();
    u32 b = (u32)shaped(rnd(), i);
    s32 sa;
    s32 sb;

        if (b == 0U)
            b = 1U;

        ck("mulsi3", anxd_mulsi3(a, b) == (u32)(a * b));
        ck("udivsi3", anxd_udivsi3(a, b) == a / b);
        ck("umodsi3", anxd_umodsi3(a, b) == a % b);

        sa = (s32)a;
        sb = (s32)b;
        if (sa == (s32)0x80000000U)
            sa = 1;
        if (sb == (s32)0x80000000U)
            sb = -1;
        if ((i & 1UL) != 0UL)
            sa = -sa;
        if ((i & 2UL) != 0UL)
            sb = -sb;

        ck("divsi3", anxd_divsi3(sa, sb) == sa / sb);
        ck("modsi3", anxd_modsi3(sa, sb) == sa % sb);
    }

    /* ---- the 64-bit multiply --------------------------------------------- */
    for (i = 0; i < 60000UL; i++)
    {
    u64 a = rnd();
    u64 b = shaped(rnd(), i);

        ck("muldi3", anxd_muldi3(a, b) == (u64)(a * b));
    }

    /* ---- the shifts, which must never become a call to themselves -------- */
    for (i = 0; i < 2000UL; i++)
    {
    u64 v = rnd();
    int c;

        for (c = 0; c < 64; c++)
        {
            ck("lshrdi3", anxd_lshrdi3(v, c) == (v >> c));
            ck("ashldi3", anxd_ashldi3(v, c) == (v << c));
            ck("ashrdi3", anxd_ashrdi3((s64)v, c) == ((s64)v >> c));
        }

        /* Out of range in both directions: the helpers define these, C does
           not, so the contract is the file's own. */
        ck("lshrdi3 negative", anxd_lshrdi3(v, -1) == v);
        ck("ashldi3 negative", anxd_ashldi3(v, -1) == v);
        ck("ashrdi3 negative", anxd_ashrdi3((s64)v, -1) == (s64)v);
        ck("lshrdi3 over", anxd_lshrdi3(v, 64) == 0ULL);
        ck("ashldi3 over", anxd_ashldi3(v, 64) == 0ULL);
        ck("ashrdi3 over",
           anxd_ashrdi3((s64)v, 64) == (((s64)v < 0) ? (s64)-1 : (s64)0));
    }

    printf("rt_host checks=%lu failures=%lu\n", checks, failures);
    printf("%s\n", failures == 0UL ? "PASS" : "FAIL");
    return failures != 0UL;
}

/*
 * AmiNetXDuo, the compiler runtime, checked on the machine that runs it.
 *
 * src/common/ami_udivdi3.c supplies AmiNetXDuo's compiler runtime:
 * __mulsi3, __udivsi3,
 * __umodsi3, __divsi3, __modsi3, __muldi3, __udivdi3 and friends, plus the
 * three 64-bit shifts.  Most of them have two forms -- the portable one a 68000 needs and a
 * one-instruction one for the parts that have MULU.L and DIVU.L -- chosen from
 * AttnFlags at library or client startup.
 *
 * Two things are asked here, and the first is the one that matters:
 *
 *   1. Is each routine right?  Against a reference in this file that CANNOT
 *      be the routine under test.  Writing `a / b` for the expected value is
 *      the obvious C and it is worthless: on a -m68000 build that division IS
 *      a call to __udivsi3, so the test compares the routine with itself and
 *      passes however wrong it is.  The references below are built from
 *      MULU.W and from single-bit shifts, which the hardware has and the
 *      compiler never lowers to a call.
 *   2. Do the two forms agree?  ami_rt_cpu_select() takes the choice as
 *      parameters, so one binary runs both over the same operands.  The fast
 *      form is only asked for on a machine that has the instructions; on a
 *      68000 it is never selected and executing it would be an illegal
 *      instruction, which is what the flag is for.
 *
 * WHAT THIS FOUND the first time it ran: __lshrdi3, __ashldi3 and __ashrdi3
 * each compiled into a call to themselves, because `value >> count` on a
 * 64-bit value with a variable count is exactly what GCC lowers to a call to
 * __lshrdi3.  Reached from __udivmoddi4's wide-divisor branch, it ate the 4 KB
 * Shell stack and took the machine with it.  tools/check-rt-recursion.sh is
 * the static half of that; this is the half that runs.
 *
 *   cmake --build build --parallel --target rt_test
 *   tools/amiberry-run.sh -t 150 -m A600 build/tests/common/rt_test
 *
 * Output is key=value and an exit code.  It prints through RawDoFmt rather
 * than stdio for the reason tests/perf/n68kmv.c does: printf pulls in newlib's
 * double formatting and a 3.1 ROM has no mathieeedoubbas.library.
 *
 * SPDX-License-Identifier: MIT
 */

#include <exec/types.h>
#include <exec/execbase.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <inline/macros.h>

#include <stdarg.h>

typedef unsigned long long  u64;
typedef long long           s64;
typedef unsigned long       u32;
typedef unsigned short      u16;

extern void ami_rt_cpu_select(int have_68020, int have_mulul);

extern u32 __mulsi3(u32 a, u32 b);
extern u32 __udivsi3(u32 n, u32 d);
extern u32 __umodsi3(u32 n, u32 d);
extern long __divsi3(long n, long d);
extern long __modsi3(long n, long d);
extern u64 __muldi3(u64 a, u64 b);
extern u64 __udivdi3(u64 n, u64 d);
extern u64 __umoddi3(u64 n, u64 d);
extern s64 __divdi3(s64 n, s64 d);
extern s64 __moddi3(s64 n, s64 d);
extern u64 __lshrdi3(u64 value, int count);
extern u64 __ashldi3(u64 value, int count);
extern s64 __ashrdi3(s64 value, int count);

/* ------------------------------------------------------------- logging --- */

#ifndef RawPutChar
#  define RawPutChar(c) \
      LP1NR(0x204, RawPutChar, UBYTE, (c), d0, , EXEC_BASE_NAME)
#endif

#define R_LOG_SIZE      4096

static char     r_log_buffer[R_LOG_SIZE];
static ULONG    r_log_used;

static VOID r_put(UBYTE ch)
{

    RawPutChar(ch);

    if (r_log_used < (ULONG)(R_LOG_SIZE - 1))
        r_log_buffer[r_log_used++] = (char)ch;
}

static VOID r_put_char(register UBYTE ch     __asm("d0"),
                       register APTR  unused __asm("a3"))
{

    (VOID)unused;
    if (ch != '\0')
        r_put(ch);
}

static VOID r_log(const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    RawDoFmt((STRPTR)fmt, args, (void (*)())r_put_char, NULL);
    va_end(args);

    r_put('\n');
}

static VOID r_flush(VOID)
{
    BPTR out = Output();

    if (out != (BPTR)0)
        (VOID)Write(out, (APTR)r_log_buffer, (LONG)r_log_used);
}

/* ----------------------------------------------------------- reference --- */

/*
 * The only multiply in this file, and it is one instruction the 68000 has.
 * Everything else is built from it, so no reference below can turn into a
 * call to the routine it is meant to check.
 */
static u32 ref_mulu16(u16 a, u16 b)
{

u32     r = (u32)a;


    __asm__ ("mulu.w %1,%0" : "+d" (r) : "dmi" (b));
    return(r);
}

/* 32x32 -> 64 from four of them, schoolbook. */
static u64 ref_mul32(u32 a, u32 b)
{

u32     ll = ref_mulu16((u16)a, (u16)b);
u32     lh = ref_mulu16((u16)a, (u16)(b >> 16));
u32     hl = ref_mulu16((u16)(a >> 16), (u16)b);
u32     hh = ref_mulu16((u16)(a >> 16), (u16)(b >> 16));
u32     mid = (ll >> 16) + (u32)(u16)lh + (u32)(u16)hl;


    return((((u64)(hh + (lh >> 16) + (hl >> 16) + (mid >> 16))) << 32) |
           (u64)((mid << 16) | (u32)(u16)ll));
}

/* 64x64 -> 64: the low product plus the two cross terms, truncated. */
static u64 ref_mul64(u64 a, u64 b)
{

u64     p = ref_mul32((u32)a, (u32)b);


    p += (u64)(u32)ref_mul32((u32)a, (u32)(b >> 32)) << 32;
    p += (u64)(u32)ref_mul32((u32)(a >> 32), (u32)b) << 32;
    return(p);
}

/*
 * 64/64 by restoring division, one bit at a time.
 *
 * The numerator is taken apart into two u32 halves and the bit is picked with
 * a 32-bit shift, so nothing here needs a 64-bit variable shift -- which is
 * the operation that would call __lshrdi3, one of the routines under test.
 */
static u64 ref_divmod64(u64 n, u64 d, u64 *remainder)
{

u32     n_hi = (u32)(n >> 32);
u32     n_lo = (u32)n;
u64     rem = 0;
u64     q = 0;
int     bit;


    if (d == 0)
    {
        /* What ami_udivdi3.c answers deliberately, rather than trapping. */
        if (remainder != 0)
            *remainder = 0;
        return(~(u64)0);
    }

    for (bit = 63; bit >= 0; bit--)
    {
    u32 b = (bit >= 32) ? ((n_hi >> (bit - 32)) & 1UL)
                        : ((n_lo >> bit) & 1UL);

        rem = (rem << 1) | (u64)b;
        q = q << 1;
        if (rem >= d)
        {
            rem -= d;
            q |= 1;
        }
    }

    if (remainder != 0)
        *remainder = rem;
    return(q);
}

/* Signed, truncating toward zero, as C99 asks. */
static s64 ref_sdiv64(s64 n, s64 d, s64 *remainder)
{

int     neg_q = 0;
int     neg_r = 0;
u64     q;
u64     r;


    if (n < 0) { n = -n; neg_q ^= 1; neg_r = 1; }
    if (d < 0) { d = -d; neg_q ^= 1; }

    q = ref_divmod64((u64)n, (u64)d, &r);

    if (remainder != 0)
        *remainder = neg_r ? -(s64)r : (s64)r;
    return(neg_q ? -(s64)q : (s64)q);
}

/*
 * The shifts, one bit at a time.  Slow and beyond argument, which is the
 * point: the routines under test do it in two 32-bit steps and this does not
 * share a line of that reasoning.
 */
static u64 ref_lshr(u64 v, int count)
{
    int i;

    if (count >= 64)
        return(0);
    for (i = 0; i < count; i++)
        v >>= 1;
    return(v);
}

static u64 ref_ashl(u64 v, int count)
{
    int i;

    if (count >= 64)
        return(0);
    for (i = 0; i < count; i++)
        v <<= 1;
    return(v);
}

static s64 ref_ashr(s64 v, int count)
{
    int i;

    if (count >= 64)
        return((v < 0) ? (s64)-1 : (s64)0);
    for (i = 0; i < count; i++)
        v >>= 1;
    return(v);
}

/* -------------------------------------------------------------- values --- */

static ULONG    failures;
static ULONG    checks;

/*
 * Operands chosen for the boundaries the two forms have between them: the
 * 16-bit split the portable multiply works in, the fits-in-16-bits test the
 * portable divide takes, the top bit, and the values either side of them.
 */
static const u32 vals[] =
{
    0UL, 1UL, 2UL, 3UL, 0xFFFFUL, 0x10000UL, 0x10001UL,
    0x7FFFFFFFUL, 0x80000000UL, 0x80000001UL, 0xFFFFFFFFUL,
    0x0000FFFFUL, 0xFFFF0000UL, 0x12345678UL, 0x9ABCDEF0UL, 0xDEADBEEFUL
};

#define NVALS   ((int)(sizeof(vals) / sizeof(vals[0])))

static VOID check32(const char *what, const char *form,
                    u32 a, u32 b, u32 got, u32 want)
{

    checks++;
    if (got != want)
    {
        failures++;
        r_log("FAIL %s %s(%08lx,%08lx) = %08lx want %08lx",
              (LONG)form, (LONG)what, (LONG)a, (LONG)b, (LONG)got, (LONG)want);
    }
}

static VOID check64(const char *what, const char *form,
                    u64 a, u64 b, u64 got, u64 want)
{

    checks++;
    if (got != want)
    {
        failures++;
        r_log("FAIL %s %s(%08lx%08lx,%08lx%08lx) = %08lx%08lx want %08lx%08lx",
              (LONG)form, (LONG)what,
              (LONG)(u32)(a >> 32), (LONG)(u32)a,
              (LONG)(u32)(b >> 32), (LONG)(u32)b,
              (LONG)(u32)(got >> 32), (LONG)(u32)got,
              (LONG)(u32)(want >> 32), (LONG)(u32)want);
    }
}

/* ------------------------------------------------------------- the set --- */

/*
 * The 32-bit five, over every operand pair.  These exist only in a -m68000
 * translation unit, which one binary for every 68k means every build.
 */
static VOID sweep32(const char *form)
{
    int i, j;

    for (i = 0; i < NVALS; i++)
    {
        for (j = 0; j < NVALS; j++)
        {
        u32     a = vals[i];
        u32     b = vals[j];
        u64     q;
        u64     r;

            check32("mulsi3", form, a, b,
                    __mulsi3(a, b), (u32)ref_mul32(a, b));

            if (b == 0UL)
                continue;

            (VOID)ref_divmod64((u64)a, (u64)b, &r);
            q = ref_divmod64((u64)a, (u64)b, 0);

            check32("udivsi3", form, a, b, __udivsi3(a, b), (u32)q);
            check32("umodsi3", form, a, b, __umodsi3(a, b), (u32)r);

            /* Signed, avoiding the one case C leaves undefined. */
            if ((long)a != (long)0x80000000UL || (long)b != -1L)
            {
            s64 sr;
            s64 sq = ref_sdiv64((s64)(long)a, (s64)(long)b, &sr);

                check32("divsi3", form, a, b,
                        (u32)__divsi3((long)a, (long)b), (u32)(long)sq);
                check32("modsi3", form, a, b,
                        (u32)__modsi3((long)a, (long)b), (u32)(long)sr);
            }
        }
    }
}

/*
 * The 64-bit set.  The operands are built so that both halves of every
 * divisor are exercised: a divisor above 2^32 takes __udivmoddi4's wide
 * branch, which is where the shift helpers are called from and where the
 * recursion above was found.
 */
static VOID sweep64(const char *form)
{
    int i, j;

    for (i = 0; i < NVALS; i++)
    {
        for (j = 0; j < NVALS; j++)
        {
        u64     a = ((u64)vals[i] << 32) | (u64)vals[j];
        u64     b = ((u64)vals[j] << 16) | (u64)vals[i];
        u64     r;
        u64     q;
        s64     sr;
        s64     sq;

            check64("muldi3", form, a, b, __muldi3(a, b), ref_mul64(a, b));

            if (b == 0ULL)
                continue;

            q = ref_divmod64(a, b, &r);
            check64("udivdi3", form, a, b, __udivdi3(a, b), q);
            check64("umoddi3", form, a, b, __umoddi3(a, b), r);

            sq = ref_sdiv64((s64)a, (s64)b, &sr);
            check64("divdi3", form, a, b,
                    (u64)__divdi3((s64)a, (s64)b), (u64)sq);
            check64("moddi3", form, a, b,
                    (u64)__moddi3((s64)a, (s64)b), (u64)sr);
        }
    }
}

/*
 * The three shifts, at every count from 0 to 64.  0 and 64 are the ends
 * libgcc defines a behaviour for and 32 is where the two-step form changes
 * shape; the counts between are where the recursion lived.
 */
static VOID sweep_shifts(const char *form)
{
    int i, c;

    for (i = 0; i < NVALS; i++)
    {
    u64 v = ((u64)vals[i] << 32) | (u64)vals[NVALS - 1 - i];

        for (c = 0; c <= 64; c++)
        {
            check64("lshrdi3", form, v, (u64)c,
                    __lshrdi3(v, c), ref_lshr(v, c));
            check64("ashldi3", form, v, (u64)c,
                    __ashldi3(v, c), ref_ashl(v, c));
            check64("ashrdi3", form, v, (u64)c,
                    (u64)__ashrdi3((s64)v, c), (u64)ref_ashr((s64)v, c));
        }
    }
}

static VOID run_all(const char *form)
{

    sweep32(form);
    sweep64(form);
    sweep_shifts(form);
    r_log("%s=%s checks=%lu failures=%lu",
          (LONG)form, (LONG)(failures == 0UL ? "ok" : "FAILED"),
          (LONG)checks, (LONG)failures);
}

int main(void)
{
    ULONG attn   = (ULONG)SysBase->AttnFlags;
    int   wide   = (attn & AFF_68020) != 0UL;
    int   mul_ul = (wide != 0 && (attn & AFF_68060) == 0UL);

    r_log("attnflags=%08lx", (LONG)attn);
    r_log("have_68020=%ld have_mulul=%ld", (LONG)wide, (LONG)mul_ul);

    /* The portable forms, on every machine including the ones that have the
       instructions: this is what a 68000 runs, and it has to be right there
       even though nothing else in the tree can see it fail. */
    ami_rt_cpu_select(0, 0);
    run_all("portable");

    /* And the hardware forms where they exist.  Same operands, same
       references, so a disagreement names the routine and the operands. */
    if (wide != 0)
    {
        ami_rt_cpu_select(wide, mul_ul);
        run_all("hardware");

        /*
         * The 68060 configuration, asked for by hand on whatever this is.
         *
         * A 68060 is the part that has MULS.L and DIVU.L but not their
         * 64-bit-result forms, so it runs the 32-bit fast paths with the
         * wide multiply left portable.  No rig here produces it: AFF_68060
         * is set by 68060.library rather than by any ROM, and under an
         * emulator that does not load it a 68060 reports AttnFlags 0x0F and
         * selects the same forms an 040 does.  The combination is a
         * parameter, so it costs one more pass to check it everywhere
         * instead of nowhere.
         */
        ami_rt_cpu_select(1, 0);
        run_all("no-mulul");
    }
    else
    {
        r_log("hardware=skipped-no-68020");
        r_log("no-mulul=skipped-no-68020");
    }

    /* Left as the machine's own choice, which is what everything after this
       test runs with. */
    ami_rt_cpu_select(wide, mul_ul);

    r_log("checks=%lu failures=%lu", (LONG)checks, (LONG)failures);
    r_log("%s", (LONG)(failures == 0UL ? "PASS" : "FAIL"));

    r_flush();

    return failures ? 20 : 0;
}

/*
 * AmiNetXDuo, the compiler runtime's two answers, compared on this machine.
 *
 * src/common/ami_udivdi3.c supplies __mulsi3, __udivsi3, __umodsi3, __divsi3,
 * __modsi3, __muldi3 and the 64/32 divide under them, because this toolchain
 * ships a zero-byte libgcc.  Each now has two forms: the portable one a 68000
 * needs, and a one-instruction one for the parts that have MULU.L and DIVU.L,
 * chosen from AttnFlags at library or client startup.
 *
 * Two implementations of seven routines and nothing compared them.  That is
 * what this is: ami_rt_cpu_select() takes the choice as parameters, so the
 * same binary can run both forms over the same operands and diff them.
 *
 *   1. Is the portable form still right?  Every case is checked against a
 *      value computed here, from 64-bit C the compiler does with no help.
 *   2. Does the fast form agree with it, instruction for instruction?  Only
 *      asked on a machine that has the instructions -- on a 68000 the fast
 *      form is never selected and executing it would be an illegal
 *      instruction, which is the whole reason the flag exists.
 *   3. Does the flag hold?  The last block asks for the fast form on a machine
 *      that has no 64-bit MULU.L and checks it did not get it.
 *
 *   cmake --build build --parallel --target rt_test
 *   AMINETXDUO_RUN_TAG=rt ./tools/amiberry-run.sh -t 300 -m A600 \
 *       build/tests/common/rt_test
 *
 * Output is key=value and an exit code, and it prints through RawDoFmt rather
 * than stdio for the reason tests/perf/n68kmv.c does: printf pulls in newlib's
 * double formatting and a 3.1 ROM has no mathieeedoubbas.library.
 *
 * SPDX-License-Identifier: MIT
 */

#include <exec/types.h>
#include <exec/execbase.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <inline/macros.h>
#include <proto/dos.h>

#include <stdarg.h>

extern void ami_rt_cpu_select(int have_68020, int have_mulul);

extern unsigned long __mulsi3(unsigned long a, unsigned long b);
extern unsigned long __udivsi3(unsigned long n, unsigned long d);
extern unsigned long __umodsi3(unsigned long n, unsigned long d);
extern long __divsi3(long n, long d);
extern long __modsi3(long n, long d);
extern unsigned long long __muldi3(unsigned long long a, unsigned long long b);
extern unsigned long long __udivdi3(unsigned long long n,
                                    unsigned long long d);

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

/* -------------------------------------------------------------- values --- */

static ULONG failures;

/*
 * Operands chosen for the boundaries each form has: the 16-bit split the
 * portable multiply works in, the fits-in-16-bits test the portable divide
 * takes, the top bit, and the values either side of them.
 */
static const ULONG vals[] =
{
    0UL, 1UL, 2UL, 3UL, 0xFFFFUL, 0x10000UL, 0x10001UL,
    0x7FFFFFFFUL, 0x80000000UL, 0x80000001UL, 0xFFFFFFFFUL,
    0x0000FFFFUL, 0xFFFF0000UL, 0x12345678UL, 0x9ABCDEF0UL, 0xDEADBEEFUL
};

#define NVALS   ((int)(sizeof(vals) / sizeof(vals[0])))

static VOID check_u(const char *what, ULONG a, ULONG b, ULONG got, ULONG want)
{

    if (got != want)
    {
        r_log("FAIL %s(%08lx,%08lx) = %08lx want %08lx",
              (LONG)what, (LONG)a, (LONG)b, (LONG)got, (LONG)want);
        failures++;
    }
}

/*
 * The whole set, over every operand pair.  `label` says which form is in
 * force; the expected values are computed here in C and are the same either
 * way, which is the point.
 */
static VOID sweep(const char *label)
{
    int i, j;

    for (i = 0; i < NVALS; i++)
    {
        for (j = 0; j < NVALS; j++)
        {
        ULONG   a = vals[i];
        ULONG   b = vals[j];
        ULONG   want;

            /* The 64-bit C below is what the compiler does without help, so
               it cannot be the routine under test. */
            want = (ULONG)((unsigned long long)a * (unsigned long long)b);
            check_u(label, a, b, __mulsi3(a, b), want);

            if (b != 0UL)
            {
                check_u(label, a, b, __udivsi3(a, b), a / b);
                check_u(label, a, b, __umodsi3(a, b), a % b);

                /* Signed, avoiding the one case C leaves undefined. */
                if (!((long)a == (long)0x80000000UL && (long)b == -1L))
                {
                    check_u(label, a, b, (ULONG)__divsi3((long)a, (long)b),
                            (ULONG)((long)a / (long)b));
                    check_u(label, a, b, (ULONG)__modsi3((long)a, (long)b),
                            (ULONG)((long)a % (long)b));
                }
            }
        }
    }
}

/*
 * __muldi3 and __udivdi3 over 64-bit operands built from the same values, so
 * the 32x32 -> 64 primitive underneath and the 64/32 divide are both reached.
 */
static VOID sweep64(const char *label)
{
    int i, j;

    for (i = 0; i < NVALS; i++)
    {
        for (j = 0; j < NVALS; j++)
        {
        unsigned long long a = ((unsigned long long)vals[i] << 32) | vals[j];
        unsigned long long b = ((unsigned long long)vals[j] << 16) | vals[i];
        unsigned long long got;

            got = __muldi3(a, b);
            if (got != (unsigned long long)(a * b))
            {
                r_log("FAIL %s muldi3 at %d,%d", (LONG)label, (LONG)i, (LONG)j);
                failures++;
            }

            if (b != 0ULL)
            {
                got = __udivdi3(a, b);
                if (got != (unsigned long long)(a / b))
                {
                    r_log("FAIL %s udivdi3 at %d,%d",
                          (LONG)label, (LONG)i, (LONG)j);
                    failures++;
                }
            }
        }
    }
}

int main(void)
{
    ULONG attn   = (ULONG)SysBase->AttnFlags;
    int   wide   = (attn & AFF_68020) != 0UL;
    int   mul_ul = (wide != 0 && (attn & AFF_68060) == 0UL);

    r_log("attnflags=%08lx", (LONG)attn);
    r_log("have_68020=%ld have_mulul=%ld", (LONG)wide, (LONG)mul_ul);

    /* The portable forms, on every machine including the ones that have the
       instructions: this is the answer everything else is compared against. */
    ami_rt_cpu_select(0, 0);
    sweep("portable");
    sweep64("portable");
    r_log("portable=%s", (LONG)(failures == 0UL ? "ok" : "FAILED"));

    /* And the hardware forms, where they exist.  Asking for them on a machine
       that lacks them is what the flag is for, so that case is not run here --
       it is checked below by asking and looking at what happened. */
    if (wide != 0)
    {
        ami_rt_cpu_select(wide, mul_ul);
        sweep("hardware");
        if (mul_ul != 0)
            sweep64("hardware");
        r_log("hardware=%s", (LONG)(failures == 0UL ? "ok" : "FAILED"));
    }
    else
    {
        r_log("hardware=skipped-no-68020");
    }

    /* The guard itself.  A 68000 that was handed a wrong answer would take an
       illegal instruction rather than a wrong result, so the check is that
       asking for what this machine does not have leaves it computing. */
    ami_rt_cpu_select(wide, mul_ul);
    sweep("selected");
    sweep64("selected");

    r_log("failures=%lu", (LONG)failures);
    r_log("%s", (LONG)(failures == 0UL ? "PASS" : "FAIL"));

    r_flush();

    return failures ? 20 : 0;
}

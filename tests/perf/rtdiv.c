/*
 * AmiNetXDuo, what libgcc's 64-bit helpers cost against src/common's.
 *
 * SPDX-License-Identifier: MIT
 */

#include <exec/types.h>
#include <dos/dos.h>

#include <proto/dos.h>
#include <proto/exec.h>

#include "aminetxduo/compat.h"
#include "rtgout.h"

typedef unsigned long long u64;

#define RTDIV_ITERS     100000UL
#define RTDIV_WIDE      20000UL

#define RTDIV_STEP      0x9e3779b97f4a7c15ULL
#define RTDIV_SEED      0x0123456789abcdefULL

/*
 * The iteration count, off the command line, for the machines where the
 * default does not finish inside a harness timeout.  A 68000 has no divide
 * instruction wider than DIVU.W, so one 64-bit divide there is thousands of
 * cycles and the default is a quarter of an hour; on an A600 pass 5000.
 *
 *   AMINETXDUO_RUN_TAG=rtd tools/amiberry-run.sh -t 300 -m A600 \
 *       -a 5000 build/cm/tests/perf/rtdiv_anxd
 *
 * GetArgStr(), not argv: an AmigaDOS command started the way the harness
 * starts one sees argc == 1.
 */
static ULONG rt_iters(void)
{
    const char *args = (const char *)GetArgStr();
    ULONG       n    = 0UL;

    if (args == NULL)
        return (RTDIV_ITERS);

    while (*args == ' ' || *args == '\t')
        args++;

    while (*args >= '0' && *args <= '9')
    {
        n = (n * 10UL) + (ULONG)(*args - '0');
        args++;
    }

    return ((n != 0UL) ? n : RTDIV_ITERS);
}

static u64 rt_div_narrow(ULONG iters)
{
    u64   acc = 0ULL;
    u64   n   = RTDIV_SEED;
    ULONG i;

    for (i = 0; i < iters; i++)
    {
        acc += n / (u64)(i | 1UL);
        n   += RTDIV_STEP;
    }

    return acc;
}

static u64 rt_mod_narrow(ULONG iters)
{
    u64   acc = 0ULL;
    u64   n   = RTDIV_SEED;
    ULONG i;

    for (i = 0; i < iters; i++)
    {
        acc += n % (u64)(i | 1UL);
        n   += RTDIV_STEP;
    }

    return acc;
}

/*
 * A divisor below 65536, which is the shape the software divide finishes in
 * two DIVU.W with no estimate at all.  Separate from the narrow arm above,
 * whose divisors run past 65536 two thirds of the way through and so price
 * the two arms of ami_divu64_32_soft() mixed together.
 */
static u64 rt_div_small(ULONG iters)
{
    u64   acc = 0ULL;
    u64   n   = RTDIV_SEED;
    ULONG i;

    for (i = 0; i < iters; i++)
    {
        acc += n / (u64)((i & 0xFFFUL) | 1UL);
        n   += RTDIV_STEP;
    }

    return acc;
}

static u64 rt_div_wide(ULONG iters)
{
    u64   acc = 0ULL;
    u64   n   = RTDIV_SEED;
    ULONG i;

    for (i = 0; i < iters; i++)
    {
        acc += n / ((u64)(i | 1UL) << 33);
        n   += RTDIV_STEP;
    }

    return acc;
}

static u64 rt_mod_wide(ULONG iters)
{
    u64   acc = 0ULL;
    u64   n   = RTDIV_SEED;
    ULONG i;

    for (i = 0; i < iters; i++)
    {
        acc += n % ((u64)(i | 1UL) << 33);
        n   += RTDIV_STEP;
    }

    return acc;
}

static u64 rt_mul_kernel(ULONG iters)
{
    u64   acc = 0ULL;
    u64   n   = RTDIV_SEED;
    ULONG i;

    for (i = 0; i < iters; i++)
    {
        acc += n * (u64)(i | 1UL);
        n   += RTDIV_STEP;
    }

    return acc;
}

static VOID rt_report(const char *name, ULONG iters, ULONG ms, u64 acc)
{
    ULONG args[5];

    args[0] = (ULONG)name;
    args[1] = iters;
    args[2] = ms;
    args[3] = (ULONG)(acc >> 32);
    args[4] = (ULONG)acc;

    rtg_say("rtdiv_%s iters=%lu ms=%lu acc=%08lx%08lx\n", args);
}

int main(int argc, char **argv)
{
    ULONG t0;
    ULONG iters;
    ULONG wide;
    u64   acc;

    (VOID)argc;
    (VOID)argv;

    iters = rt_iters();
    wide  = iters / (RTDIV_ITERS / RTDIV_WIDE);   /* the default ratio, kept */
    if (wide == 0UL)
        wide = 1UL;

    (VOID)ami_millis();                 /* opens timer.device, sets TimerBase */

    t0  = ami_millis();
    acc = rt_div_narrow(iters);
    rt_report("udivdi3_narrow", iters, ami_millis() - t0, acc);

    t0  = ami_millis();
    acc = rt_mod_narrow(iters);
    rt_report("umoddi3_narrow", iters, ami_millis() - t0, acc);

    t0  = ami_millis();
    acc = rt_div_small(iters);
    rt_report("udivdi3_small", iters, ami_millis() - t0, acc);

    t0  = ami_millis();
    acc = rt_mul_kernel(iters);
    rt_report("muldi3", iters, ami_millis() - t0, acc);

    t0  = ami_millis();
    acc = rt_div_wide(wide);
    rt_report("udivdi3_wide", wide, ami_millis() - t0, acc);

    t0  = ami_millis();
    acc = rt_mod_wide(wide);
    rt_report("umoddi3_wide", wide, ami_millis() - t0, acc);

    return RETURN_OK;
}

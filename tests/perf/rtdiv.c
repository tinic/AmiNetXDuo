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
    u64   acc;

    (VOID)argc;
    (VOID)argv;

    (VOID)ami_millis();                 /* opens timer.device, sets TimerBase */

    t0  = ami_millis();
    acc = rt_div_narrow(RTDIV_ITERS);
    rt_report("udivdi3_narrow", RTDIV_ITERS, ami_millis() - t0, acc);

    t0  = ami_millis();
    acc = rt_mod_narrow(RTDIV_ITERS);
    rt_report("umoddi3_narrow", RTDIV_ITERS, ami_millis() - t0, acc);

    t0  = ami_millis();
    acc = rt_div_small(RTDIV_ITERS);
    rt_report("udivdi3_small", RTDIV_ITERS, ami_millis() - t0, acc);

    t0  = ami_millis();
    acc = rt_mul_kernel(RTDIV_ITERS);
    rt_report("muldi3", RTDIV_ITERS, ami_millis() - t0, acc);

    t0  = ami_millis();
    acc = rt_div_wide(RTDIV_WIDE);
    rt_report("udivdi3_wide", RTDIV_WIDE, ami_millis() - t0, acc);

    t0  = ami_millis();
    acc = rt_mod_wide(RTDIV_WIDE);
    rt_report("umoddi3_wide", RTDIV_WIDE, ami_millis() - t0, acc);

    return RETURN_OK;
}

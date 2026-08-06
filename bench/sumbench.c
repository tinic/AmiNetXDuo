/*
 * AmiNetXDuo, a bench for the copy-and-sum variants, outside the stack.
 *
 * The routine is a pure function of two buffers and a count, so iterating on
 * it through a library build and a fitz transfer is all cost and no signal:
 * one emulator boot here times every variant against the same buffers and
 * checks each against the C reference first, because a faster wrong answer is
 * not a result.
 *
 * Timing is ReadEClock, the same clock tests/perf/perf_test.c uses, divided
 * once at the end so the tick granularity does not land on each iteration.
 *
 * SPDX-License-Identifier: MIT
 */

#include <exec/types.h>
#include <devices/timer.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/timer.h>

#include <stdio.h>
#include <string.h>

struct Device      *TimerBase;
static struct IORequest  timer_req;
static struct MsgPort   *timer_port;

extern ULONG v_addadd(ULONG *to, const ULONG *from, ULONG count);
extern ULONG v_addx(ULONG *to, const ULONG *from, ULONG count);
extern ULONG v_discrete(ULONG *to, const ULONG *from, ULONG count);
extern ULONG v_disc16(ULONG *to, const ULONG *from, ULONG count);
extern ULONG v_disc4(ULONG *to, const ULONG *from, ULONG count);
extern ULONG v_ldmovem(ULONG *to, const ULONG *from, ULONG count);
extern ULONG v_addx14(ULONG *to, const ULONG *from, ULONG count);
extern ULONG v_lm14(ULONG *to, const ULONG *from, ULONG count);
extern ULONG v_lmsep(ULONG *to, const ULONG *from, ULONG count);

/* The contract, from src/net68k/n68k_checksum.c. */
static ULONG v_reference(ULONG *to, const ULONG *from, ULONG count)
{
    ULONG acc = 0;

    while (count != 0UL)
    {
        ULONG w = *from++;

        *to++ = w;

        acc += w;
        if (acc < w)
            acc++;

        count--;
    }

    return acc;
}

#define BUFW    512                     /* longwords */

static ULONG  src[BUFW];
static ULONG  dst[BUFW + 1];
static ULONG  ref[BUFW + 1];

static ULONG  rng = 0x2545f491UL;

static ULONG rnd(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

static ULONG eclock(void)
{
    struct EClockVal ev;

    ReadEClock(&ev);
    return ev.ev_lo;
}

static ULONG failures;

/*
 * Every count from 0 to 72 exercises the block loop, its remainder and the
 * boundary between them; the guard longword past the end catches a block that
 * writes one too many.
 */
static void check(const char *name,
                  ULONG (*fn)(ULONG *, const ULONG *, ULONG))
{
    ULONG n;

    for (n = 0; n <= 72UL; n++)
    {
        ULONG i, want, got;

        for (i = 0; i < n; i++)
            src[i] = (rnd() << 8) ^ rnd();

        for (i = 0; i <= n; i++)
        {
            dst[i] = 0xDEADBEEFUL;
            ref[i] = 0xDEADBEEFUL;
        }

        want = v_reference(ref, src, n);
        got  = fn(dst, src, n);

        if (got != want)
        {
            printf("FAIL %s: n=%lu sum %08lx want %08lx\n",
                   name, (unsigned long)n, (unsigned long)got,
                   (unsigned long)want);
            failures++;
            return;
        }

        for (i = 0; i < n; i++)
        {
            if (dst[i] != ref[i])
            {
                printf("FAIL %s: n=%lu word %lu copied wrong\n",
                       name, (unsigned long)n, (unsigned long)i);
                failures++;
                return;
            }
        }

        if (dst[n] != 0xDEADBEEFUL)
        {
            printf("FAIL %s: n=%lu wrote past the end\n",
                   name, (unsigned long)n);
            failures++;
            return;
        }
    }

    printf("  ok   %-12s 73 counts against the reference\n", name);
}

static void bench(const char *name,
                  ULONG (*fn)(ULONG *, const ULONG *, ULONG),
                  ULONG words, ULONG reps)
{
    ULONG t0, ticks, i, bytes;
    ULONG ns;

    t0 = eclock();
    for (i = 0; i < reps; i++)
        (void)fn(dst, src, words);
    ticks = eclock() - t0;

    bytes = words * 4UL * reps;

    /* 709379 ticks a second; ns per byte, divided once. */
    ns = (ULONG)(((double)ticks * 1000000000.0) / 709379.0 / (double)bytes);

    printf("  %-12s %6lu ticks  %4lu ns/B  (%lu x %lu B)\n",
           name, (unsigned long)ticks, (unsigned long)ns,
           (unsigned long)reps, (unsigned long)(words * 4UL));
}

int main(void)
{
    ULONG i;
    ULONG words = 365;                  /* 1460 bytes, one MSS */
    ULONG reps  = 200;

    timer_port = CreateMsgPort();
    if (timer_port == NULL)
        return 20;

    timer_req.io_Message.mn_ReplyPort = timer_port;
    if (OpenDevice("timer.device", UNIT_ECLOCK, &timer_req, 0) != 0)
    {
        printf("no timer.device\n");
        return 20;
    }
    TimerBase = timer_req.io_Device;

    for (i = 0; i < BUFW; i++)
        src[i] = (rnd() << 8) ^ rnd();

    printf("copy-and-sum variants\n\n");

    check("addadd", v_addadd);
    check("addx", v_addx);
    check("discrete", v_discrete);
    check("disc16", v_disc16);
    check("disc4", v_disc4);
    check("ldmovem", v_ldmovem);
    check("addx14", v_addx14);
    check("lm14", v_lm14);
    check("lmsep", v_lmsep);

    printf("\n");

    bench("reference", v_reference, words, reps);
    bench("addadd", v_addadd, words, reps);
    bench("addx", v_addx, words, reps);
    bench("discrete", v_discrete, words, reps);
    bench("disc16", v_disc16, words, reps);
    bench("disc4", v_disc4, words, reps);
    bench("ldmovem", v_ldmovem, words, reps);
    bench("addx14", v_addx14, words, reps);
    bench("lm14", v_lm14, words, reps);
    bench("lmsep", v_lmsep, words, reps);

    printf("\n%s\n", failures == 0 ? "PASS" : "FAIL");

    CloseDevice(&timer_req);
    DeleteMsgPort(timer_port);

    return failures ? 20 : 0;
}

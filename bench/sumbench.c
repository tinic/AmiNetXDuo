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
extern ULONG v_shift2(ULONG *to, const ULONG *from, ULONG count);

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

/*
 * THE PHASE PRODUCTION ACTUALLY RUNS AT, which every arm above misses.
 * src[] and dst[] are ULONG arrays, so bench() times both ends longword
 * aligned.  The shipping path does not: ami_sana2_copy_to_buff() gates the
 * fused copy on `& 1` rather than `& 3` and says why -- "the two ends are
 * permanently two bytes out of phase" -- because dst is
 * data_start + PAD(2) + ETH(14), 0 mod 4, while `from` is the device's payload
 * pointer at 2 mod 4.  So the routine that is 15% of receive reads MISALIGNED
 * longwords on every frame and no bench here has ever timed that.
 *
 * Same buffers, source offset by one word.  The count drops by one longword so
 * the read stays inside src[].
 */
/*
 * TENTHS OF A NANOSECOND PER BYTE, IN INTEGER ARITHMETIC.
 *
 * This used to be double, and that made the bench unrunnable on the rig: the
 * link pulls mathieeedoubbas.library and the test image does not carry it, so
 * every run died with "mathieeedoubbas.library failed to load" before printing
 * a number.  Which is presumably why it was only ever run by hand somewhere
 * that had one.
 *
 * The eclock is 709379 ticks a second, so ns per byte is
 * ticks * 1e9 / 709379 / bytes, i.e. ticks * 1409.6 / bytes.  In tenths that
 * is ticks * 14096 / bytes, and ticks * 14096 overflows 32 bits for a run of
 * any length, so the multiply is done in 64 bits -- integer, which libgcc
 * supplies without a math library.
 */
static ULONG ns_tenths_per_byte(ULONG ticks, ULONG bytes)
{
    if (bytes == 0UL)
        return 0UL;

    return (ULONG)(((unsigned long long)ticks * 14096ULL) /
                   (unsigned long long)bytes);
}

/*
 * v_shift2 is only correct at the phase it exists for, so it cannot go through
 * check(), which feeds an aligned source.  Same contract: the answer and every
 * byte written must match the C reference over the SAME bytes.
 */
static void check_skewed(const char *name,
                         ULONG (*fn)(ULONG *, const ULONG *, ULONG))
{
    const ULONG *from = (const ULONG *)(const APTR)((const UBYTE *)src + 2);
    ULONG n = 64UL;
    ULONG want, got, i;
    int   bad = 0;

    for (i = 0; i < n + 1UL; i++)
        ref[i] = 0xDEADBEEFUL;
    for (i = 0; i < n + 1UL; i++)
        dst[i] = 0xDEADBEEFUL;

    want = v_reference(ref, from, n);
    got  = fn(dst, from, n);

    if (got != want)
    {
        printf("  FAIL %-12s sum %08lx, reference %08lx\n",
               name, (unsigned long)got, (unsigned long)want);
        bad = 1;
    }

    for (i = 0; i < n; i++)
    {
        if (dst[i] != ref[i])
        {
            printf("  FAIL %-12s longword %lu is %08lx, reference %08lx\n",
                   name, (unsigned long)i, (unsigned long)dst[i],
                   (unsigned long)ref[i]);
            bad = 1;
            break;
        }
    }

    if (dst[n] != 0xDEADBEEFUL)
    {
        printf("  FAIL %-12s wrote past its count\n", name);
        bad = 1;
    }

    if (bad)
        failures++;
    else
        printf("  ok   %-12s %lu skewed longwords against the reference\n",
               name, (unsigned long)n);
}

static void bench_skewed(const char *name,
                         ULONG (*fn)(ULONG *, const ULONG *, ULONG),
                         ULONG words, ULONG reps)
{
    const ULONG *from = (const ULONG *)(const APTR)((const UBYTE *)src + 2);
    ULONG t0, ticks, i, bytes;
    ULONG ns;

    words = (words > 1UL) ? (words - 1UL) : 1UL;

    t0 = eclock();
    for (i = 0; i < reps; i++)
        (void)fn(dst, from, words);
    ticks = eclock() - t0;

    bytes = words * 4UL * reps;
    ns = ns_tenths_per_byte(ticks, bytes);

    printf("  %-12s %6lu ticks  %3lu.%lu ns/B  (src +2, %lu x %lu B)\n",
           name, (unsigned long)ticks,
           (unsigned long)(ns / 10UL), (unsigned long)(ns % 10UL),
           (unsigned long)reps, (unsigned long)(words * 4UL));
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
    ns = ns_tenths_per_byte(ticks, bytes);

    printf("  %-12s %6lu ticks  %3lu.%lu ns/B  (%lu x %lu B)\n",
           name, (unsigned long)ticks,
           (unsigned long)(ns / 10UL), (unsigned long)(ns % 10UL),
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

    check_skewed("shift2", v_shift2);

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

    printf("\n  -- source at 2 mod 4, the phase the stack actually feeds --\n");
    bench_skewed("ldmovem+2", v_ldmovem, words, reps);
    bench_skewed("addx14+2", v_addx14, words, reps);
    bench_skewed("lm14+2", v_lm14, words, reps);
    bench_skewed("lmsep+2", v_lmsep, words, reps);
    bench_skewed("reference+2", v_reference, words, reps);
    bench_skewed("shift2", v_shift2, words, reps);

    printf("\n%s\n", failures == 0 ? "PASS" : "FAIL");

    CloseDevice(&timer_req);
    DeleteMsgPort(timer_port);

    return failures ? 20 : 0;
}

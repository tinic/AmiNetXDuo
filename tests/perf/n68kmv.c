/*
 * AmiNetXDuo, the multiversioned inner loops on the machine that is running.
 *
 * An AMINETXDUO_CPU=any build carries four assemblies of the checksum and the
 * copy and picks one from AttnFlags (src/net68k/n68k_cpu.c).  Three questions
 * follow from that and this answers all three in one boot:
 *
 *   1. Was the right one picked?  It prints the class it selected and the
 *      AttnFlags it read, so a wrong answer is visible rather than merely
 *      slow.
 *   2. Do they all still agree?  Every variant is checked against the C
 *      reference before anything is timed, because a faster wrong answer is
 *      not a result.  The forms written for a 68020 and up read a longword
 *      from an odd address on purpose, so on a 68000 they are checked at
 *      matched parity only -- there they would be an address error, which is
 *      the whole reason the 68000 has a variant of its own.
 *   3. What do they cost HERE?  n68k_copy.S and n68k_checksum.S both carry a
 *      68060 form that was reasoned about and never measured, because cycle
 *      accounting is off above the 68020 in every emulator this project uses.
 *      On a real 68040 or 68060 this program settles those two bets, and the
 *      dispatched entry is timed beside the variant it lands on, so what the
 *      indirection costs is a number too.
 *
 *   cmake --build build/any --parallel --target n68kmv
 *   AMINETXDUO_RUN_TAG=mv ./tools/amiberry-run.sh -t 300 -m A1200 \
 *       build/any/tests/perf/n68kmv
 *
 * SPDX-License-Identifier: MIT
 */

#include <exec/types.h>
#include <exec/execbase.h>
#include <devices/timer.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/timer.h>

#include <stdio.h>

/* Declared here rather than from net68k.h, which reaches nx_api.h and its own
   typedef of VOID: bench/copycheck.c takes the same way out. */
extern ULONG n68k_sum_longwords(const ULONG *p, ULONG count);
extern ULONG n68k_copy_sum_longwords(ULONG *to, const ULONG *from,
                                     ULONG count);
extern VOID  n68k_copy_bytes(UCHAR *to, const UCHAR *from, ULONG len);
extern VOID  n68k_cpu_select(ULONG attnflags);

struct Device           *TimerBase;
static struct IORequest  timer_req;
static struct MsgPort   *timer_port;

extern ULONG n68k_sum_longwords_mv0(const ULONG *p, ULONG count);
extern ULONG n68k_sum_longwords_mv20(const ULONG *p, ULONG count);
extern ULONG n68k_sum_longwords_mv40(const ULONG *p, ULONG count);
extern ULONG n68k_sum_longwords_mv60(const ULONG *p, ULONG count);

extern ULONG n68k_copy_sum_longwords_mv0(ULONG *to, const ULONG *from,
                                         ULONG count);
extern ULONG n68k_copy_sum_longwords_mv20(ULONG *to, const ULONG *from,
                                          ULONG count);
extern ULONG n68k_copy_sum_longwords_mv40(ULONG *to, const ULONG *from,
                                          ULONG count);
extern ULONG n68k_copy_sum_longwords_mv60(ULONG *to, const ULONG *from,
                                          ULONG count);

extern VOID n68k_copy_bytes_mv0(UCHAR *to, const UCHAR *from, ULONG len);
extern VOID n68k_copy_bytes_mv20(UCHAR *to, const UCHAR *from, ULONG len);
extern VOID n68k_copy_bytes_mv40(UCHAR *to, const UCHAR *from, ULONG len);
extern VOID n68k_copy_bytes_mv60(UCHAR *to, const UCHAR *from, ULONG len);

extern ULONG (*n68k_vec_sum)(const ULONG *, ULONG);
extern ULONG (*n68k_vec_copy_sum)(ULONG *, const ULONG *, ULONG);
extern VOID  (*n68k_vec_copy)(UCHAR *, const UCHAR *, ULONG);

#define BUFW    512                     /* longwords */

static ULONG  src[BUFW];
static ULONG  dst[BUFW + 1];
static ULONG  ref[BUFW + 1];

/* Longword aligned, so the offsets below are the whole of what varies: an
   array of UCHAR is only guaranteed to start on a word. */
static UCHAR  bsrc[320] __attribute__((aligned(4)));
static UCHAR  bdst[320] __attribute__((aligned(4)));
static UCHAR  bref[320] __attribute__((aligned(4)));

static ULONG  rng = 0x2545f491UL;
static ULONG  failures;

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

/* The contract, from src/net68k/n68k_checksum.c. */
static ULONG sum_reference(const ULONG *p, ULONG count)
{
    ULONG acc = 0;

    while (count != 0UL)
    {
        ULONG w = *p++;

        acc += w;
        if (acc < w)
            acc++;

        count--;
    }

    return acc;
}

static ULONG copy_sum_reference(ULONG *to, const ULONG *from, ULONG count)
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

static void check_sum(const char *name, ULONG (*fn)(const ULONG *, ULONG))
{
    ULONG n;

    for (n = 0; n <= 72UL; n++)
    {
        ULONG i;

        for (i = 0; i < n; i++)
            src[i] = (rnd() << 8) ^ rnd();

        if (fn(src, n) != sum_reference(src, n))
        {
            printf("FAIL sum %s at n=%lu\n", name, (unsigned long)n);
            failures++;
            return;
        }
    }
}

static void check_copy_sum(const char *name,
                           ULONG (*fn)(ULONG *, const ULONG *, ULONG))
{
    ULONG n;

    for (n = 0; n <= 72UL; n++)
    {
        ULONG i, want, got;

        for (i = 0; i < n; i++)
            src[i] = (rnd() << 8) ^ rnd();
        for (i = 0; i <= n; i++)
            dst[i] = ref[i] = 0xDEADBEEFUL;

        want = copy_sum_reference(ref, src, n);
        got  = fn(dst, src, n);

        if (got != want || dst[n] != 0xDEADBEEFUL)
        {
            printf("FAIL copysum %s at n=%lu\n", name, (unsigned long)n);
            failures++;
            return;
        }

        for (i = 0; i < n; i++)
        {
            if (dst[i] != ref[i])
            {
                printf("FAIL copysum %s at n=%lu word %lu\n",
                       name, (unsigned long)n, (unsigned long)i);
                failures++;
                return;
            }
        }
    }
}

/*
 * `pairs` is 4 on a 68000 -- matched parity only, the other twelve offsets are
 * an address error there by design -- and 16 everywhere else.
 */
static void check_copy(const char *name,
                       VOID (*fn)(UCHAR *, const UCHAR *, ULONG),
                       int all_offsets)
{
    /* 0..80 covers the byte tail, the 32-byte threshold and the alignment
       run; the rest reach the 128-byte block loop and its remainder. */
    static const ULONG extra[] = { 96, 127, 128, 129, 160, 255, 256, 300 };
    ULONG so, dof, k, n;

    for (so = 0; so < 4UL; so++)
    {
        for (dof = 0; dof < 4UL; dof++)
        {
            if (!all_offsets && ((so ^ dof) & 1UL) != 0UL)
                continue;

            for (k = 0; k <= 80UL + (ULONG)(sizeof(extra) / sizeof(extra[0]));
                 k++)
            {
                ULONG i;

                n = (k <= 80UL) ? k : extra[k - 81UL];

                for (i = 0; i < sizeof(bsrc); i++)
                {
                    bdst[i] = 0xA5;
                    bref[i] = 0xA5;
                }

                for (i = 0; i < n; i++)
                    bref[dof + i] = bsrc[so + i];

                fn(&bdst[dof], &bsrc[so], n);

                for (i = 0; i < sizeof(bsrc); i++)
                {
                    if (bdst[i] != bref[i])
                    {
                        printf("FAIL copy %s s%lu d%lu n=%lu at %lu\n",
                               name, (unsigned long)so, (unsigned long)dof,
                               (unsigned long)n, (unsigned long)i);
                        failures++;
                        return;
                    }
                }
            }
        }
    }
}

static void bench_sum(const char *name, ULONG (*fn)(const ULONG *, ULONG),
                      ULONG words, ULONG reps)
{
    ULONG t0, ticks, i;

    t0 = eclock();
    for (i = 0; i < reps; i++)
        (void)fn(src, words);
    ticks = eclock() - t0;

    printf("  sum       %-8s %6lu ticks  %4lu ns/B\n", name,
           (unsigned long)ticks,
           (unsigned long)(((double)ticks * 1000000000.0) / 709379.0 /
                           (double)(words * 4UL * reps)));
}

static void bench_copy(const char *name,
                       VOID (*fn)(UCHAR *, const UCHAR *, ULONG),
                       ULONG len, ULONG reps)
{
    ULONG t0, ticks, i;

    t0 = eclock();
    for (i = 0; i < reps; i++)
        fn(bdst, bsrc, len);
    ticks = eclock() - t0;

    printf("  copy%-4lu  %-8s %6lu ticks  %4lu ns/B\n",
           (unsigned long)len, name, (unsigned long)ticks,
           (unsigned long)(((double)ticks * 1000000000.0) / 709379.0 /
                           (double)(len * reps)));
}

static const char *selected(void)
{

    if (n68k_vec_copy == n68k_copy_bytes_mv60)
        return "60";
    if (n68k_vec_copy == n68k_copy_bytes_mv40)
        return "40";
    if (n68k_vec_copy == n68k_copy_bytes_mv20)
        return "20";
    if (n68k_vec_copy == n68k_copy_bytes_mv0)
        return "0";

    return "none";
}

int main(void)
{
    ULONG attn = (ULONG)SysBase->AttnFlags;
    int   wide = (attn & AFF_68020) != 0UL;
    ULONG i;

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

    /* Nothing has opened bsdsocket.library here, so nothing has done this
       yet: this program is the one caller that has to select for itself. */
    n68k_cpu_select(attn);

    printf("attnflags=%08lx\n", (unsigned long)attn);
    printf("selected=%s\n", selected());

    for (i = 0; i < BUFW; i++)
        src[i] = (rnd() << 8) ^ rnd();
    for (i = 0; i < sizeof(bsrc); i++)
        bsrc[i] = (UCHAR)rnd();

    check_sum("mv0",  n68k_sum_longwords_mv0);
    check_sum("mv20", n68k_sum_longwords_mv20);
    check_sum("mv40", n68k_sum_longwords_mv40);
    check_sum("mv60", n68k_sum_longwords_mv60);

    check_copy_sum("mv0",  n68k_copy_sum_longwords_mv0);
    check_copy_sum("mv20", n68k_copy_sum_longwords_mv20);
    check_copy_sum("mv40", n68k_copy_sum_longwords_mv40);
    check_copy_sum("mv60", n68k_copy_sum_longwords_mv60);

    check_copy("mv0", n68k_copy_bytes_mv0, 1);
    if (wide)
    {
        check_copy("mv20", n68k_copy_bytes_mv20, 1);
        check_copy("mv40", n68k_copy_bytes_mv40, 1);
        check_copy("mv60", n68k_copy_bytes_mv60, 1);
    }
    else
    {
        check_copy("mv20", n68k_copy_bytes_mv20, 0);
        check_copy("mv40", n68k_copy_bytes_mv40, 0);
        check_copy("mv60", n68k_copy_bytes_mv60, 0);
    }

    printf("checked=%s\n", wide ? "all16" : "matched-parity");
    printf("\n");

    bench_sum("mv0",  n68k_sum_longwords_mv0,  365, 200);
    bench_sum("mv20", n68k_sum_longwords_mv20, 365, 200);
    bench_sum("mv40", n68k_sum_longwords_mv40, 365, 200);
    bench_sum("mv60", n68k_sum_longwords_mv60, 365, 200);
    bench_sum("chosen", n68k_sum_longwords, 365, 200);

    /* 288 bytes is a copy the bulk loop cares about, 20 is the TCP header
       that Profile found is most of what memcpy() is asked for.  The chosen
       row is the same routine reached through the trampoline, so the two
       differ by the dispatch and nothing else. */
    bench_copy("mv0",  n68k_copy_bytes_mv0,  288, 400);
    bench_copy("mv20", n68k_copy_bytes_mv20, 288, 400);
    bench_copy("mv40", n68k_copy_bytes_mv40, 288, 400);
    bench_copy("mv60", n68k_copy_bytes_mv60, 288, 400);
    bench_copy("chosen", n68k_copy_bytes, 288, 400);

    bench_copy("mv0",  n68k_copy_bytes_mv0,  20, 4000);
    bench_copy("mv20", n68k_copy_bytes_mv20, 20, 4000);
    bench_copy("mv40", n68k_copy_bytes_mv40, 20, 4000);
    bench_copy("mv60", n68k_copy_bytes_mv60, 20, 4000);
    bench_copy("chosen", n68k_copy_bytes, 20, 4000);

    printf("\nfailures=%lu\n", (unsigned long)failures);
    printf("%s\n", failures == 0UL ? "PASS" : "FAIL");

    CloseDevice(&timer_req);
    DeleteMsgPort(timer_port);

    return failures ? 20 : 0;
}

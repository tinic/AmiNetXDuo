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
 * Output goes through RawDoFmt and Write(), not stdio, for the reason
 * cpucal.c does the same: printf drags in newlib's double formatting, the
 * startup then opens mathieeedoubbas.library, and a 3.1 ROM has no such
 * library -- the program exits 20 before main() with nothing measured.
 *
 * SPDX-License-Identifier: MIT
 */

#include <exec/types.h>
#include <exec/execbase.h>
#include <dos/dos.h>
#include <devices/timer.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/timer.h>

#include <stdarg.h>

#include "aminetxduo/compat.h"

/* Declared here rather than from net68k.h, which reaches nx_api.h and its own
   typedef of VOID: bench/copycheck.c takes the same way out. */
extern ULONG n68k_sum_longwords(const ULONG *p, ULONG count);
extern VOID  n68k_copy_bytes(UBYTE *to, const UBYTE *from, ULONG len);
extern VOID  n68k_cpu_select(ULONG attnflags);

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

extern VOID n68k_copy_bytes_mv0(UBYTE *to, const UBYTE *from, ULONG len);
extern VOID n68k_copy_bytes_mv20(UBYTE *to, const UBYTE *from, ULONG len);
extern VOID n68k_copy_bytes_mv40(UBYTE *to, const UBYTE *from, ULONG len);
extern VOID n68k_copy_bytes_mv60(UBYTE *to, const UBYTE *from, ULONG len);

extern VOID (*n68k_vec_copy)(UBYTE *, const UBYTE *, ULONG);

extern struct Device *TimerBase;        /* src/common/compat.c owns it */

/* ------------------------------------------------------------- logging --- */

#ifndef RawPutChar
#  define RawPutChar(c) \
      LP1NR(0x204, RawPutChar, UBYTE, (c), d0, , EXEC_BASE_NAME)
#endif

#define M_LOG_SIZE      8192

static char     m_log_buffer[M_LOG_SIZE];
static ULONG    m_log_used;

static VOID m_put(UBYTE ch)
{

    RawPutChar(ch);

    if (m_log_used < (ULONG)(M_LOG_SIZE - 1))
        m_log_buffer[m_log_used++] = (char)ch;
}

static VOID m_put_char(register UBYTE ch     __asm("d0"),
                       register APTR  unused __asm("a3"))
{

    (VOID)unused;
    if (ch != '\0')
        m_put(ch);
}

static VOID m_log(const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    RawDoFmt((STRPTR)fmt, args, (void (*)())m_put_char, NULL);
    va_end(args);

    m_put('\n');
}

static VOID m_flush(VOID)
{
    BPTR out = Output();

    if (out != (BPTR)0)
        (VOID)Write(out, (APTR)m_log_buffer, (LONG)m_log_used);
}

/* ------------------------------------------------------------ the data --- */

#define BUFW    512                     /* longwords */

static ULONG  src[BUFW];
static ULONG  dst[BUFW + 1];
static ULONG  ref[BUFW + 1];

/* Longword aligned, so the offsets below are the whole of what varies: an
   array of UBYTE is only guaranteed to start on a word. */
static UBYTE  bsrc[320] __attribute__((aligned(4)));
static UBYTE  bdst[320] __attribute__((aligned(4)));
static UBYTE  bref[320] __attribute__((aligned(4)));

static ULONG  rng = 0x2545f491UL;
static ULONG  failures;

static ULONG rnd(VOID)
{

    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;

    return rng;
}

static ULONG eclock(VOID)
{
    struct EClockVal ev;

    (VOID)ReadEClock(&ev);

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

/* ---------------------------------------------------------- the checks --- */

static VOID check_sum(const char *name, ULONG (*fn)(const ULONG *, ULONG))
{
    ULONG n;

    for (n = 0; n <= 72UL; n++)
    {
        ULONG i;

        for (i = 0; i < n; i++)
            src[i] = (rnd() << 8) ^ rnd();

        if (fn(src, n) != sum_reference(src, n))
        {
            m_log("FAIL sum %s at n=%lu", (LONG)name, (LONG)n);
            failures++;
            return;
        }
    }
}

static VOID check_copy_sum(const char *name,
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

        want = copy_sum_reference(ref, src, n);
        got  = fn(dst, src, n);

        if (got != want || dst[n] != 0xDEADBEEFUL)
        {
            m_log("FAIL copysum %s at n=%lu", (LONG)name, (LONG)n);
            failures++;
            return;
        }

        for (i = 0; i < n; i++)
        {
            if (dst[i] != ref[i])
            {
                m_log("FAIL copysum %s at n=%lu word %lu",
                      (LONG)name, (LONG)n, (LONG)i);
                failures++;
                return;
            }
        }
    }
}

/*
 * `all_offsets` is false on a 68000: only the four matched-parity pairs are
 * legal there for the forms that carry no parity guard, and the other twelve
 * are an address error by design rather than a defect.
 */
static VOID check_copy(const char *name,
                       VOID (*fn)(UBYTE *, const UBYTE *, ULONG),
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
                        m_log("FAIL copy %s s%lu d%lu n=%lu at %lu",
                              (LONG)name, (LONG)so, (LONG)dof, (LONG)n,
                              (LONG)i);
                        failures++;
                        return;
                    }
                }
            }
        }
    }
}

/* ----------------------------------------------------------- the bench --- */
/*
 * The E-Clock is 709379 Hz, so a tick is 1409.68 ns and the 1410 below is
 * 0.02% out.  Integer throughout: a double here is mathieeedoubbas.library.
 *
 * Best of three, because the first pass through a routine pays for a cold
 * instruction cache and the host the emulator runs on has its own load.
 *
 * The copy rows for mv20 and mv40 are the SAME instructions at different
 * addresses -- n68k_copy.S has nothing to say about a 68040 -- so the gap
 * between those two is this instrument's floor, and no gap smaller than it
 * is a fact about a variant.  On the A1200 profile it was 7%, which is more
 * than the whole of what the dispatch costs.
 */

#define ROUNDS  3

static VOID bench_sum(const char *name, ULONG (*fn)(const ULONG *, ULONG),
                      ULONG words, ULONG reps)
{
    ULONG best = 0xFFFFFFFFUL;
    ULONG r;

    for (r = 0; r < ROUNDS; r++)
    {
        ULONG t0, ticks, i;

        t0 = eclock();
        for (i = 0; i < reps; i++)
            (VOID)fn(src, words);
        ticks = eclock() - t0;

        if (ticks < best)
            best = ticks;
    }

    m_log("sum %s ticks=%lu nsB=%lu", (LONG)name, (LONG)best,
          (LONG)((best * 1410UL) / (words * 4UL * reps)));
}

static VOID bench_copy(const char *name,
                       VOID (*fn)(UBYTE *, const UBYTE *, ULONG),
                       ULONG len, ULONG reps)
{
    ULONG best = 0xFFFFFFFFUL;
    ULONG r;

    for (r = 0; r < ROUNDS; r++)
    {
        ULONG t0, ticks, i;

        t0 = eclock();
        for (i = 0; i < reps; i++)
            fn(bdst, bsrc, len);
        ticks = eclock() - t0;

        if (ticks < best)
            best = ticks;
    }

    m_log("copy%lu %s ticks=%lu nsB=%lu", (LONG)len, (LONG)name,
          (LONG)best, (LONG)((best * 1410UL) / (len * reps)));
}

/*
 * The decode, on every machine, from AttnFlags this one does not have.
 *
 * Without this the 68060 branch of n68k_cpu_select() is unreachable in the
 * lab: AFF_68060 is set by 68060.library and not by any ROM here, so `-c
 * 68060` under Amiberry reads 0x0F and lands on the 68040 forms.  A typo in
 * that branch would ship.  Feeding the flag values directly costs nothing and
 * covers all four, and the machine's own answer is put back afterwards.
 *
 * Cumulative, as Exec sets them: a 68060 also raises 010, 020, 030 and 040.
 */
static VOID check_decode(VOID)
{
    static const struct { ULONG attn; VOID (*want)(UBYTE *, const UBYTE *,
                                                   ULONG); const char *name; }
    cases[] =
    {
        { 0x00UL,                            n68k_copy_bytes_mv0,  "0"  },
        { AFF_68010,                         n68k_copy_bytes_mv0,  "0"  },
        { AFF_68010 | AFF_68020,             n68k_copy_bytes_mv20, "20" },
        { AFF_68010 | AFF_68020 | AFF_68030, n68k_copy_bytes_mv20, "20" },
        { AFF_68010 | AFF_68020 | AFF_68030 |
          AFF_68040,                         n68k_copy_bytes_mv40, "40" },
        { AFF_68010 | AFF_68020 | AFF_68030 |
          AFF_68040 | AFF_68060,             n68k_copy_bytes_mv60, "60" },
    };
    ULONG i;

    for (i = 0; i < (ULONG)(sizeof(cases) / sizeof(cases[0])); i++)
    {
        n68k_cpu_select(cases[i].attn);

        if (n68k_vec_copy != cases[i].want)
        {
            m_log("FAIL decode %08lx wanted mv%s",
                  (LONG)cases[i].attn, (LONG)cases[i].name);
            failures++;
        }
    }
}

static const char *selected(VOID)
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

    (VOID)ami_millis();                 /* opens timer.device, sets TimerBase */
    if (TimerBase == NULL)
    {
        m_log("no timer.device");
        m_flush();
        return 20;
    }

    /* Nothing has opened bsdsocket.library here, so nothing has selected yet:
       this program is the one caller that has to do it for itself. */
    check_decode();                     /* all four branches, on any machine */
    n68k_cpu_select(attn);              /* and then this machine's own */

    m_log("attnflags=%08lx", (LONG)attn);
    m_log("selected=%s", (LONG)selected());

    for (i = 0; i < BUFW; i++)
        src[i] = (rnd() << 8) ^ rnd();
    for (i = 0; i < sizeof(bsrc); i++)
        bsrc[i] = (UBYTE)rnd();

    check_sum("mv0",  n68k_sum_longwords_mv0);
    check_sum("mv20", n68k_sum_longwords_mv20);
    check_sum("mv40", n68k_sum_longwords_mv40);
    check_sum("mv60", n68k_sum_longwords_mv60);

    check_copy_sum("mv0",  n68k_copy_sum_longwords_mv0);
    check_copy_sum("mv20", n68k_copy_sum_longwords_mv20);
    check_copy_sum("mv40", n68k_copy_sum_longwords_mv40);
    check_copy_sum("mv60", n68k_copy_sum_longwords_mv60);

    check_copy("mv0",  n68k_copy_bytes_mv0,  1);
    check_copy("mv20", n68k_copy_bytes_mv20, wide);
    check_copy("mv40", n68k_copy_bytes_mv40, wide);
    check_copy("mv60", n68k_copy_bytes_mv60, wide);

    m_log("checked=%s", (LONG)(wide ? "all16" : "matched-parity"));

    bench_sum("mv0",    n68k_sum_longwords_mv0,  365, 200);
    bench_sum("mv20",   n68k_sum_longwords_mv20, 365, 200);
    bench_sum("mv40",   n68k_sum_longwords_mv40, 365, 200);
    bench_sum("mv60",   n68k_sum_longwords_mv60, 365, 200);
    bench_sum("chosen", n68k_sum_longwords,      365, 200);

    /* 288 bytes is a copy the bulk loop cares about; 20 is the TCP header,
       which Profile found is most of what memcpy() is asked for.  The chosen
       row is the same routine reached through the trampoline, so those two
       rows differ by the dispatch and nothing else. */
    bench_copy("mv0",    n68k_copy_bytes_mv0,  288, 400);
    bench_copy("mv20",   n68k_copy_bytes_mv20, 288, 400);
    bench_copy("mv40",   n68k_copy_bytes_mv40, 288, 400);
    bench_copy("mv60",   n68k_copy_bytes_mv60, 288, 400);
    bench_copy("chosen", n68k_copy_bytes,      288, 400);

    bench_copy("mv0",    n68k_copy_bytes_mv0,  20, 4000);
    bench_copy("mv20",   n68k_copy_bytes_mv20, 20, 4000);
    bench_copy("mv40",   n68k_copy_bytes_mv40, 20, 4000);
    bench_copy("mv60",   n68k_copy_bytes_mv60, 20, 4000);
    bench_copy("chosen", n68k_copy_bytes,      20, 4000);

    m_log("failures=%lu", (LONG)failures);
    m_log("%s", (LONG)(failures == 0UL ? "PASS" : "FAIL"));

    m_flush();

    return failures ? 20 : 0;
}

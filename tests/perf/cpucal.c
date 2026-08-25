/*
 * cpucal -- runs instruction sequences whose cost on real silicon is published
 * and reports what the emulator charges for them.  Primary results are ratios
 * between kernels measured in the same run, which are clock-independent; the
 * implied clock at the end assumes ADD.L costs its published two cycles.
 *
 * SPDX-License-Identifier: MIT
 */

#include <exec/types.h>
#include <exec/execbase.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <devices/timer.h>
#include <proto/exec.h>
#include <inline/macros.h>
#include <proto/dos.h>
#include <proto/timer.h>

#include <stdarg.h>
#include <string.h>

#include "aminetxduo/compat.h"


/* ------------------------------------------------------------- logging --- */

#ifndef RawPutChar
#  define RawPutChar(c) \
      LP1NR(0x204, RawPutChar, UBYTE, (c), d0, , EXEC_BASE_NAME)
#endif

#define C_LOG_SIZE      16384

static char     c_log_buffer[C_LOG_SIZE];
static ULONG    c_log_used;

static VOID c_put(UBYTE ch)
{
    RawPutChar(ch);

    if (c_log_used < (ULONG)(C_LOG_SIZE - 1))
    {
        c_log_buffer[c_log_used++] = (char)ch;
    }
}

static VOID c_put_char(register UBYTE ch     __asm("d0"),
                       register APTR  unused __asm("a3"))
{
    (VOID)unused;
    if (ch != '\0')
    {
        c_put(ch);
    }
}

static VOID c_log(const char *fmt, ...)
{
va_list args;

    va_start(args, fmt);
    RawDoFmt((STRPTR)fmt, args, (void (*)())c_put_char, NULL);
    va_end(args);

    c_put('\n');
}

static VOID c_flush(VOID)
{
BPTR    out;

    out = Output();
    if (out != (BPTR)0)
    {
        (VOID)Write(out, (APTR)c_log_buffer, (LONG)c_log_used);
    }
}


/* -------------------------------------------------------------- timing --- */

extern struct Device *TimerBase;        /* src/common/compat.c owns it */

static ULONG    c_rate;                 /* E-Clock ticks per second       */
static ULONG    c_tick_ns;              /* nanoseconds per tick           */
static ULONG    c_bracket;              /* cost of one measurement, ticks */

static ULONG c_now(VOID)
{
/* Zeroed for -fanalyzer, which cannot see through ReadEClock(). */
struct EClockVal ev = { 0UL, 0UL };

    (VOID)ReadEClock(&ev);

    return(ev.ev_lo);
}

static ULONG c_elapsed(ULONG start, ULONG stop)
{
ULONG   d = stop - start;

    return((d > c_bracket) ? (d - c_bracket) : 0UL);
}

static VOID c_timer_init(VOID)
{
struct EClockVal ev = { 0UL, 0UL };
ULONG            i, t0, t1, total;

    (VOID)ami_millis();                 /* opens timer.device, sets TimerBase */

    c_rate    = ReadEClock(&ev);
    c_tick_ns = 1000000000UL / c_rate;

    total = 0UL;
    for (i = 0UL; i < 256UL; i++)
    {
        t0 = c_now();
        t1 = c_now();
        total += (t1 - t0);
    }
    c_bracket = total / 256UL;
}


/* ------------------------------------------------------------- kernels --- */

extern VOID cal_empty (ULONG reps);
extern VOID cal_add   (ULONG reps);
extern VOID cal_move  (ULONG reps);
extern VOID cal_addx  (ULONG reps);
extern VOID cal_mulu  (ULONG reps);
extern VOID cal_mulu64(ULONG reps);
extern VOID cal_read  (APTR buf, ULONG longs, ULONG reps);
extern VOID cal_write (APTR buf, ULONG longs, ULONG reps);
extern VOID cal_m2m   (APTR dst, APTR src, ULONG longs, ULONG reps);
extern VOID cal_movem (APTR dst, APTR src, ULONG longs, ULONG reps);

#define K_EMPTY     0
#define K_ADD       1
#define K_MOVE      2
#define K_ADDX      3
#define K_MULU      4
#define K_MULU64    5
#define K_READ      6
#define K_WRITE     7
#define K_M2M       8
#define K_MOVEM     9

static APTR     c_buf_a;
static APTR     c_buf_b;
static ULONG    c_window;               /* longwords swept per rep */

static VOID c_run(ULONG kind, ULONG reps)
{
    switch (kind)
    {
    case K_EMPTY:  cal_empty(reps);                                  break;
    case K_ADD:    cal_add(reps);                                    break;
    case K_MOVE:   cal_move(reps);                                   break;
    case K_ADDX:   cal_addx(reps);                                   break;
    case K_MULU:   cal_mulu(reps);                                   break;
    case K_MULU64: cal_mulu64(reps);                                 break;
    case K_READ:   cal_read(c_buf_a, c_window, reps);                break;
    case K_WRITE:  cal_write(c_buf_a, c_window, reps);               break;
    case K_M2M:    cal_m2m(c_buf_a, c_buf_b, c_window, reps);        break;
    case K_MOVEM:  cal_movem(c_buf_a, c_buf_b, c_window, reps);      break;
    default:                                                         break;
    }
}

/*
 * Returns picoseconds per unit (an instruction for the register kernels, a
 * byte for the memory ones).  No 64-bit divide is linkable here, so the
 * scaling divides the unit count by 1000, which needs >= 1000 units.
 */
#define C_TARGET_TICKS  70000UL         /* ~100 ms of E-Clock */
#define C_MIN_UNITS     100000UL
#define C_MAX_REPS      0x08000000UL

static ULONG c_measure_ps(ULONG kind, ULONG units_per_rep, ULONG *reps_out)
{
ULONG   reps = 1UL;
ULONG   t0, ticks = 0UL;
ULONG   units;

    for (;;)
    {
        t0    = c_now();
        c_run(kind, reps);
        ticks = c_elapsed(t0, c_now());

        units = reps * units_per_rep;   /* both bounded; see C_MAX_REPS use */

        if ((ticks >= C_TARGET_TICKS && units >= C_MIN_UNITS) ||
            reps >= C_MAX_REPS)
        {
            break;
        }

        if (ticks > 16UL)
        {
            ULONG want = reps * (C_TARGET_TICKS / ticks + 1UL);

            if (want * units_per_rep < C_MIN_UNITS)
            {
                want = (C_MIN_UNITS / units_per_rep) + 1UL;
            }

            reps = (want > C_MAX_REPS || want < reps) ? C_MAX_REPS : want;
        }
        else
        {
            reps = (reps > (C_MAX_REPS / 64UL)) ? C_MAX_REPS : reps * 64UL;
        }
    }

    if (reps_out != NULL)
    {
        *reps_out = reps;
    }

    units = reps * units_per_rep;
    if (units < 1000UL)
    {
        return(0UL);                    /* cannot be scaled exactly; say so */
    }

    return((ticks * c_tick_ns) / (units / 1000UL));
}


/* ------------------------------------------------------------ reporting -- */

/* The loop's own cost per body slot; subtracted to leave the instruction. */
static ULONG    c_empty_ps;
static ULONG    c_add_ps;               /* one ADD.L, ps, the yardstick */

static VOID c_print_reg(const char *what, ULONG kind, ULONG real_020,
                        ULONG real_030)
{
ULONG   reps = 0UL;
ULONG   raw  = c_measure_ps(kind, 16UL, &reps);
ULONG   ps   = (raw > c_empty_ps) ? (raw - c_empty_ps) : 0UL;
ULONG   ratio_x100;

    if (c_add_ps == 0UL)
    {
        c_add_ps = ps;                  /* ADD.L is measured first */
    }

    ratio_x100 = (c_add_ps != 0UL) ? ((ps * 100UL) / c_add_ps) : 0UL;

    c_log("  %-22s %6ld.%03ld ns  implied %4ld.%02ld cycles   "
          "real 68020 %2ld, 68030 %2ld",
          (LONG)what,
          (LONG)(ps / 1000UL), (LONG)(ps % 1000UL),
          (LONG)((ratio_x100 * 2UL) / 100UL),
          (LONG)((ratio_x100 * 2UL) % 100UL),
          (LONG)real_020, (LONG)real_030);
}

/* Sweeps c_window longwords per rep, rounded down to the 16-longword inner
   block.  "Bytes" counts payload only, matching the rest of tests/perf/. */
static ULONG c_print_mem(const char *what, ULONG kind)
{
ULONG   reps  = 0UL;
ULONG   bytes = (c_window & ~15UL) * 4UL;
ULONG   ps_per_byte;
ULONG   kbs;

    if (bytes == 0UL)
    {
        return(0UL);
    }

    ps_per_byte = c_measure_ps(kind, bytes, &reps);

    /* KB/s = 1e12 / ps_per_byte / 1024, divided first to stay in 32 bits. */
    kbs = (ps_per_byte != 0UL)
              ? ((1000000000UL / ps_per_byte) * 1000UL / 1024UL)
              : 0UL;

    c_log("  %-30s %4ld.%03ld ns/B   %7ld KB/s",
          (LONG)what,
          (LONG)(ps_per_byte / 1000UL), (LONG)(ps_per_byte % 1000UL),
          (LONG)kbs);

    return(ps_per_byte);
}


/* ------------------------------------------------------------------ main -- */

#define C_BIG_LONGS     8192UL          /* 32 KB: far past any 68030 cache   */
#define C_SMALL_LONGS   16UL            /* 64 B: inside a 256 B data cache   */

static VOID c_mem_suite(const char *label, APTR a, APTR b)
{
ULONG   big_read, small_read;

    c_log("");
    c_log(" , %s --", (LONG)label);

    c_buf_a  = a;
    c_buf_b  = b;

    c_window = C_SMALL_LONGS;
    small_read = c_print_mem("read  64 B window  (cached)", K_READ);
    (VOID)c_print_mem("write 64 B window  (cached)", K_WRITE);
    (VOID)c_print_mem("m2m   64 B window  (cached)", K_M2M);

    c_window = C_BIG_LONGS;
    big_read = c_print_mem("read  32 KB window (bus)", K_READ);
    (VOID)c_print_mem("write 32 KB window (bus)", K_WRITE);
    (VOID)c_print_mem("m2m   32 KB window (bus)", K_M2M);
    (VOID)c_print_mem("movem 32 KB window (bus)", K_MOVEM);

    if (small_read != 0UL)
    {
        c_log("    32 KB / 64 B read ratio: %ld.%02ldx  "
              "(~0.9x = no data cache, i.e. a 68020)",
              (LONG)((big_read * 100UL / small_read) / 100UL),
              (LONG)((big_read * 100UL / small_read) % 100UL));
    }
}

int main(void)
{
struct ExecBase *sys = (struct ExecBase *)SysBase;
APTR    fast = NULL;
APTR    chip = NULL;
ULONG   flags;
ULONG   cache;
ULONG   t_start, t_end;
ULONG   reps;

    c_timer_init();

    t_start = c_now();

    c_log("AmiNetXDuo, CPU/memory calibration probe");
    c_log("");

    flags = (ULONG)sys -> AttnFlags;
    c_log("AttnFlags 0x%04lx, 68010:%ld 68020:%ld 68030:%ld 68040:%ld "
          "FPU:%ld",
          flags,
          (LONG)((flags & AFF_68010) ? 1 : 0),
          (LONG)((flags & AFF_68020) ? 1 : 0),
          (LONG)((flags & AFF_68030) ? 1 : 0),
          (LONG)((flags & AFF_68040) ? 1 : 0),
          (LONG)((flags & (AFF_68881 | AFF_68882)) ? 1 : 0));

    cache = CacheControl(0UL, 0UL);
    c_log("CacheControl 0x%08lx, I-cache:%ld D-cache:%ld copyback:%ld",
          cache,
          (LONG)((cache & CACRF_EnableI) ? 1 : 0),
          (LONG)((cache & CACRF_EnableD) ? 1 : 0),
          (LONG)((cache & CACRF_CopyBack) ? 1 : 0));

    c_log("E-Clock %ld Hz, %ld ns/tick, bracket %ld ticks",
          (LONG)c_rate, (LONG)c_tick_ns, (LONG)c_bracket);
    c_log("memory: chip %ld KB free, fast %ld KB free",
          (LONG)(AvailMem(MEMF_CHIP) / 1024UL),
          (LONG)(AvailMem(MEMF_FAST) / 1024UL));

    /* ------------------------------------------------- register kernels -- */

    c_empty_ps = c_measure_ps(K_EMPTY, 16UL, &reps);
    c_log("");
    c_log("-- register instructions, %ld ps/slot of loop overhead subtracted -",
          (LONG)c_empty_ps);
    c_log("");

    c_add_ps = 0UL;
    c_print_reg("ADD.L  Dn,Dm",   K_ADD,     2UL,  2UL);
    c_print_reg("MOVE.L Dn,Dm",   K_MOVE,    2UL,  2UL);
    c_print_reg("ADDX.L Dn,Dm",   K_ADDX,    2UL,  2UL);
    c_print_reg("MULU.L Dn,Dm",   K_MULU,   43UL, 44UL);
    c_print_reg("MULU.L Dn,Dh:Dl",K_MULU64, 45UL, 44UL);

    if (c_add_ps != 0UL)
    {
        c_log("");
        c_log("  implied clock, if ADD.L costs its published 2 cycles: "
              "%ld.%02ld MHz",
              (LONG)((2000000UL / c_add_ps) ),
              (LONG)((2000000UL * 100UL / c_add_ps) % 100UL));
    }

    /* --------------------------------------------------- memory kernels -- */

    c_log("");
    c_log("-- memory ------------------------------------------------------");

    fast = AllocMem(96UL * 1024UL, MEMF_FAST | MEMF_CLEAR);
    chip = AllocMem(96UL * 1024UL, MEMF_CHIP | MEMF_CLEAR);

    if (fast != NULL)
    {
        c_mem_suite("Fast RAM", fast, (APTR)((ULONG)fast + 48UL * 1024UL));
    }
    else
    {
        c_log("  (no Fast RAM available)");
    }

    if (chip != NULL)
    {
        c_mem_suite("Chip RAM", chip, (APTR)((ULONG)chip + 48UL * 1024UL));
    }
    else
    {
        c_log("  (no Chip RAM available)");
    }

    if ((flags & AFF_68030) != 0UL && fast != NULL)
    {
        ULONG before = CacheControl(0UL, 0UL);

        (VOID)CacheControl(CACRF_EnableD, CACRF_EnableD);
        c_log("");
        c_log("  CacheControl now 0x%08lx (asked for D-cache on)",
              CacheControl(0UL, 0UL));
        c_mem_suite("Fast RAM, D-cache forced on", fast,
                    (APTR)((ULONG)fast + 48UL * 1024UL));
        (VOID)CacheControl(before, ~0UL);
    }

    if (fast != NULL)
    {
        FreeMem(fast, 96UL * 1024UL);
    }
    if (chip != NULL)
    {
        FreeMem(chip, 96UL * 1024UL);
    }

    t_end = c_now();

    c_log("");
    c_log("emulated wall time for this probe: %ld ms",
          (LONG)((t_end - t_start) / (c_rate / 1000UL)));
    c_log("(compare against the host seconds the harness reports: a model "
          "running the CPU unthrottled finishes in far less emulated time "
          "than host time)");

    c_flush();

    return(0);
}

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
#include <libraries/configvars.h>
#include <proto/exec.h>
#include <inline/macros.h>
#include <proto/dos.h>
#include <proto/timer.h>
#include <proto/expansion.h>

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
#define K_CHIPREAD  10
#define K_INTENA    11
#define K_FORBID    12

/*
 * WHAT THE EMULATOR CHARGES FOR TOUCHING SOMETHING THAT IS NOT MEMORY.
 *
 * This campaign has been misled twice by assuming an access is cheap because
 * it is one instruction.  NETDEV_TIME's own self-calibration put a VHPOSR read
 * at 4.9 US -- against an ADD.L at a few nanoseconds -- which made every span
 * it measured useless and was only noticed because the report prints its own
 * probe cost.  The receive path takes one Disable()/Enable() pair per frame in
 * netdev_queue_read() (netdev_cmds.c:299), and nothing here knew what that
 * costs.
 *
 * These are C rather than kernels in cpucal.S because the per-rep cost is
 * microseconds: the loop is noise beside the body, and Disable() needs a6 the
 * compiler is already managing.
 */
static volatile UWORD c_chip_sink;

static VOID cal_chipread(ULONG reps)
{
volatile const UWORD   *vh = (volatile const UWORD *)0xdff006UL;   /* VHPOSR */
ULONG                   i;
UWORD                   acc = 0U;

    for (i = 0UL; i < reps; i++)
        acc = (UWORD)(acc + *vh);

    c_chip_sink = acc;
}

static VOID cal_intena(ULONG reps)
{
ULONG   i;

    /* Each pair re-enables, so interrupts are serviced between iterations and
       a long loop starves nothing. */
    for (i = 0UL; i < reps; i++)
    {
        Disable();
        Enable();
    }
}

static VOID cal_forbid(ULONG reps)
{
ULONG   i;

    for (i = 0UL; i < reps; i++)
    {
        Forbid();
        Permit();
    }
}

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
    case K_CHIPREAD: cal_chipread(reps);                             break;
    case K_INTENA:   cal_intena(reps);                               break;
    case K_FORBID:   cal_forbid(reps);                               break;
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

/*
 * ZORRO BOARD RAM, WHICH IS WHERE THE RECEIVE PATH'S BIGGEST COPY READS FROM.
 *
 * `_n68k_copy_sum_longwords` is the largest row in the receive profile and its
 * SOURCE is the a2065's on-board SRAM, not Fast RAM -- the LANCE writes the
 * frame there and the copy hook reads it in place (lance.c:426).  Everything
 * this tree has said about that copy being instruction-bound was reasoned from
 * Fast RAM figures.  If board RAM is several times slower, the copy is bus
 * bound and there is nothing in it; if it is not, the arithmetic stands.
 *
 * READ ONLY.  A board's address space is its hardware: sweeping it with writes
 * would be poking registers on whatever card happens to be in the slot.  A
 * read is what the receive path does anyway, and read bandwidth is the number
 * the question turns on.
 *
 * NOT A MEMORY BOARD, WHICH THE FIRST VERSION OF THIS PICKED.  "The first
 * board with at least 64 KB" found the 8 MB Zorro II RAM card at 0x00200000
 * and measured it at 77.4 ns/B -- identical to Fast RAM, because that is what
 * it is.  The a2065 carries 32 KB of SRAM and was excluded by the size floor
 * it did not meet.
 *
 * ERTF_MEMLIST is the bit Expansion sets on a board whose space it added to
 * the free memory list, so skipping it leaves the cards that are hardware.
 * The floor drops to 16 KB for the same reason.
 *
 * No such board, no line -- a machine configured without one is not a failure,
 * it just cannot answer.
 */
static APTR c_board_find(ULONG *size_out)
{
struct ConfigDev   *cd = NULL;

    if (ExpansionBase == NULL)
        return NULL;

    while ((cd = FindConfigDev(cd, -1, -1)) != NULL)
    {
        if (cd->cd_BoardAddr == NULL || cd->cd_BoardSize < 16384UL)
            continue;

        if ((cd->cd_Rom.er_Type & ERTF_MEMLIST) != 0)
            continue;               /* RAM, and already measured as Fast */

        *size_out = cd->cd_BoardSize;
        return cd->cd_BoardAddr;
    }

    return NULL;
}

/*
 * The I/O kernels, in nanoseconds and in ADD.L units.  No "real 68020 cycles"
 * column: what these cost on silicon is a bus property and what they cost here
 * is an emulator property, and the whole point is that the second is not the
 * first.
 */
static VOID c_print_io(const char *what, ULONG kind)
{
ULONG   reps = 0UL;
ULONG   raw  = c_measure_ps(kind, 16UL, &reps);
ULONG   ps   = (raw > c_empty_ps) ? (raw - c_empty_ps) : 0UL;
ULONG   adds = (c_add_ps != 0UL) ? (ps / c_add_ps) : 0UL;

    c_log("  %-22s %6ld.%03ld us  = %6ld ADD.L",
          (LONG)what,
          (LONG)(ps / 1000000UL), (LONG)((ps / 1000UL) % 1000UL),
          (LONG)adds);
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
    {
    ULONG   m2m   = c_print_mem("m2m   32 KB window (bus)", K_M2M);
    ULONG   movem = c_print_mem("movem 32 KB window (bus)", K_MOVEM);

    /*
     * THE RATIO BETWEEN TWO SEQUENCES HERE IS AN EMULATOR PROPERTY, AND IT
     * DOES NOT TRANSFER TO SILICON.  movem.l moves eight longwords for one
     * instruction fetch and a fixed setup cost paid once, which is why
     * n68k_copy.S uses it; an emulator that charges per instruction executed
     * rather than per bus cycle can make the sequence with FEWER instructions
     * the SLOWER one, and on this rig it does.
     *
     * That is worth knowing and it is not a reason to change the copy: the
     * library ships to real 68020s, where the published costs say movem wins.
     * The line below states which way this machine leans so nobody reads the
     * two numbers above as a verdict on the instruction.
     */
    if (m2m != 0UL && movem != 0UL)
    {
        /* AND THE RATIO IS NOT STABLE RUN TO RUN EITHER.  Two runs of this
           binary an hour apart read m2m 117.7 then 144.6 ns/B -- twenty-three
           per cent apart -- while movem moved 0.6 per cent.  So the ratio
           flipped from 1.10 to 0.90 with no change to anything.  Read ONE
           run's ratio as an observation about that run and nothing more; the
           rate arm that chased the first one measured +0.36 per cent, inside
           the noise, which is what "no difference" looks like. */
        c_log("    movem/m2m %ld.%02ldx on THIS emulator, THIS run -- neither "
              "a fact about the silicon nor stable between runs",
              (LONG)((movem * 100UL / m2m) / 100UL),
              (LONG)((movem * 100UL / m2m) % 100UL));
    }
    }

    {
    ULONG   bsize = 0UL;
    APTR    board = c_board_find(&bsize);

    if (board != NULL)
    {
        APTR    save = c_buf_a;
        ULONG   fast_read;

        c_log("");
        ULONG   win = C_BIG_LONGS;

        /* A 32 KB card cannot be swept with a 32 KB window and a guard: take
           half the board, so the sweep stays inside it whatever it is. */
        if ((bsize / 8UL) < win)
            win = bsize / 8UL;

        c_log(" , Zorro board (not memory) at 0x%08lx, %ld KB --", (LONG)board,
              (LONG)(bsize / 1024UL));
        fast_read = big_read;
        c_buf_a   = board;
        c_window  = win;
        (VOID)c_print_mem("read  window (bus)", K_READ);
        c_window  = C_BIG_LONGS;
        c_buf_a   = save;
        (VOID)fast_read;
    }
    else
    {
        c_log("");
        c_log(" , no Zorro board with 64 KB or more: nothing to sweep --");
    }
    }

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

    c_log("");
    c_log("what the emulator charges for a NON-MEMORY access:");
    c_print_io("VHPOSR read",          K_CHIPREAD);
    c_print_io("Disable()/Enable()",   K_INTENA);
    c_print_io("Forbid()/Permit()",    K_FORBID);

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

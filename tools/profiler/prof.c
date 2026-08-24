/*
 * Profile, timer-driven PC sampling.  See prof.h for why the sample is taken
 * in the autovector rather than in an Exec interrupt server.
 *
 * This file does the parts that need AmigaOS rather than 68k: getting an audio
 * channel without stealing one the program under test wants, swapping the
 * vector, and recording everything the host needs to turn a raw address back
 * into a name.
 *
 * WHAT THE HOST NEEDS, and where each comes from:
 *
 *   1. Where the profiled program was loaded.  LoadSeg() puts each hunk
 *      wherever it likes, so a sampled PC means nothing until it is mapped
 *      back to the link-time addresses in the executable.  prof_set_target()
 *      walks the seglist the caller loaded and records every hunk's base and
 *      length in load order, which is the same order as the hunks in the file.
 *
 *   2. Which library or device anything else belongs to.  Every AmigaOS
 *      library is a jump table: LVO -6*n of a base is a JMP to the real entry
 *      point.  prof_scan_libs() resolves all of them for every library, device
 *      and resource on Exec's lists, so a PC in Kickstart can be named by the
 *      nearest preceding entry, "exec.library/Forbid" rather than
 *      "$00f8xxxx", and the hull of one library's targets becomes a named
 *      range, so a sample in a disk-loaded device with no symbols at all still
 *      gets a module.
 *
 *   3. Which task was running.  SysBase->ThisTask, for the cost of one
 *      indirection in the handler, plus a name table so the host can print
 *      "IP thread" rather than "$0021c4f8".
 *
 *   4. When.  ps_Time, so the gaps Disable() leaves are a measured quantity
 *      rather than a silent bias.  See prof_gap_summary().
 *
 * NO CIA TIMER.  Two CIA sources were tried and both died mid-run while still
 * producing enough correct samples to look convincing; the source table that
 * used to carry them as fallbacks is gone rather than kept, because a fallback
 * that is known to fail silently is worse than no fallback.  Four audio
 * channels are tried instead, and if none is free the tool refuses.
 *
 * SPDX-License-Identifier: MIT
 */

#include "prof.h"

#include <exec/execbase.h>
#include <exec/interrupts.h>
#include <exec/memory.h>
#include <exec/nodes.h>
#include <exec/lists.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <devices/audio.h>
#include <hardware/intbits.h>

#include <proto/exec.h>
#include <inline/macros.h>
#include <proto/dos.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>


/* ------------------------------------------------------------- logging --- */

/*
 * Straight to the serial port, which is what tools/amiberry-run.sh captures, and
 * optionally to stdout as well, which is what somebody sitting at a real
 * machine wants.  Both, because the tool has to be usable in an emulator
 * harness and on a desk.
 */
#ifndef RawPutChar
#  define RawPutChar(c) \
      LP1NR(0x204, RawPutChar, UBYTE, (c), d0, , EXEC_BASE_NAME)
#endif

static BOOL prof_console;

VOID prof_log_console(BOOL on)
{
    prof_console = on;
}

/* vsniprintf, not vsnprintf: the double formatter drags in
   mathieeedoubbas.library, which is not on every machine, and the program then
   never reaches main() at all.  Everything here formats through this one
   wrapper so that stays true by construction. */
static VOID prof_fmt(char *buf, ULONG size, const char *fmt, ...)
{
va_list ap;

    va_start(ap, fmt);
    (VOID)vsniprintf(buf, (size_t)size, fmt, ap);
    va_end(ap);
}

VOID prof_log(const char *fmt, ...)
{
char     line[256];
va_list  ap;
int      i;

    va_start(ap, fmt);
    (VOID)vsniprintf(line, sizeof(line) - 2, fmt, ap);
    va_end(ap);

    for (i = 0; line[i] != '\0'; i++)
    {
        RawPutChar((UBYTE)line[i]);
    }
    RawPutChar((UBYTE)'\n');

    if (prof_console)
    {
        i = (int)strlen(line);
        line[i]     = '\n';
        line[i + 1] = '\0';
        (VOID)Write(Output(), line, (LONG)(i + 1));
    }
}


/* ------------------------------------------------- shared with the .S ---- */

/* Non-static: prof_vector.S addresses all of these absolute. */
ULONG   prof_next;          /* write cursor                                 */
ULONG   prof_limit;         /* one past the last usable record              */
ULONG   prof_hits;          /* level-4 interrupts the vector saw            */
ULONG   prof_dropped;       /* hits that found the buffer full              */
ULONG   prof_chain;         /* the vector we displaced                      */
ULONG   prof_taskptr;       /* &SysBase->ThisTask                           */
ULONG   prof_ownticks;      /* interrupts from OUR source specifically      */
UWORD   prof_intf;          /* INTF_AUDx for the channel in use             */

extern VOID  prof_vector(VOID);
extern VOID  prof_audio_stub(VOID);
extern ULONG prof_read_vbr(VOID);


/* --------------------------------------------------------------- state --- */

#define PROF_MAX_SEGS   64
#define PROF_MAX_MARKS  64
#define PROF_MAX_LVO_PER_LIB 640

/* Eight windows of eight ticks each.  Both CIA-B timers passed a single
   400 ms window and then stopped, so a probe shorter than about a second, or
   one that only checks the average, accepts a source that will not survive the
   run. */
#define PROF_PROBE_WINDOWS  8UL
#define PROF_PROBE_TICKS    8UL

/* The chipset, by hand.  Nothing here opens graphics.library: the tool must
   run on a machine where the program under test may have taken the display. */
#define PROF_CUSTOM     0xDFF000UL
#define PROF_VPOSR      ((volatile ULONG *)(PROF_CUSTOM + 0x004UL))
#define PROF_DMACONR    ((volatile UWORD *)(PROF_CUSTOM + 0x002UL))
#define PROF_INTENAR    ((volatile UWORD *)(PROF_CUSTOM + 0x01CUL))
#define PROF_DMACON     ((volatile UWORD *)(PROF_CUSTOM + 0x096UL))
#define PROF_INTENA     ((volatile UWORD *)(PROF_CUSTOM + 0x09AUL))
#define PROF_INTREQ     ((volatile UWORD *)(PROF_CUSTOM + 0x09CUL))

/* AUDxLC / LEN / PER / VOL, sixteen bytes apart from $DFF0A0. */
#define PROF_AUDBASE    (PROF_CUSTOM + 0x0A0UL)
#define PROF_AUDLC(ch)  ((volatile ULONG *)(PROF_AUDBASE + 16UL * (ch) + 0UL))
#define PROF_AUDLEN(ch) ((volatile UWORD *)(PROF_AUDBASE + 16UL * (ch) + 4UL))
#define PROF_AUDPER(ch) ((volatile UWORD *)(PROF_AUDBASE + 16UL * (ch) + 6UL))
#define PROF_AUDVOL(ch) ((volatile UWORD *)(PROF_AUDBASE + 16UL * (ch) + 8UL))

#define PROF_DMAF_SETCLR 0x8000U
#define PROF_INTF_SETCLR 0x8000U
#define PROF_DMAF_AUD(ch)  ((UWORD)(1U << (ch)))
#define PROF_INTF_AUD(ch)  ((UWORD)(0x0080U << (ch)))   /* AUD0 is bit 7 */
#define PROF_INTB_AUD(ch)  (7 + (int)(ch))

/*
 * Colour clocks per second and lines per frame.
 *
 * The oscillator and the video mode are separate facts.  ex_EClockFrequency
 * identifies the motherboard clock, while Exec's VBlankFrequency describes
 * the mode currently producing vertical blanks.  A real NTSC A1200 can boot a
 * PAL display: its audio period still uses the NTSC colour clock, but the
 * probe sees 50 frames a second and a frame has 312 lines.  Treating the
 * E-clock as both facts made that machine reject every correctly programmed
 * audio channel as 4/3 too fast.
 *
 * Neither answer needs graphics.library, which is deliberately not opened on
 * a machine whose display the program under test may have taken.
 */
#define PROF_CCK_PAL    3546895UL
#define PROF_CCK_NTSC   3579545UL
#define PROF_ECLOCK_PAL 709379UL
#define PROF_LINES_PAL  312UL
#define PROF_LINES_NTSC 262UL
#define PROF_CCK_LINE   227UL

/* One watchdog window per this many video frames: about half a second, and
   512 of them is four minutes of run.  The table is allocated, so the size is
   memory somebody on the 1 MB floor does not get back, 6 KB against the
   24 KB a longer history would cost, and a run this tool is used on is
   seconds, not hours. */
#define PROF_WIN_FRAMES 25UL
#define PROF_MAX_WINS   512UL

static struct ProfSample   *prof_buf;
static ULONG                prof_bufsize;
static ULONG                prof_max;

static struct Interrupt     prof_irq;
static struct Interrupt     prof_vbl_irq;
static BOOL                 prof_vbl_held;
static BOOL                 prof_running;
static ULONG                prof_rate;
static ULONG                prof_vbr;
static ULONG               *prof_slot;      /* the vector table entry we own */
static const char          *prof_err = "";
static char                 prof_conf[96];
static ULONG                prof_framecck;
static ULONG                prof_cck;
static ULONG                prof_vblank_hz;
static BOOL                 prof_ntsc_video;

static struct ProfSeg       prof_segs[PROF_MAX_SEGS];
static ULONG                prof_nsegs;
static struct ProfMark      prof_marks[PROF_MAX_MARKS];
static ULONG                prof_nmarks;

/* Sized from what is actually on the machine, not from a #define: the floor
   for this tool is a 1 MB 68000, where 64 KB of unconditional BSS for an LVO
   table is a tenth of what a program has left. */
static struct ProfLib      *prof_libs;
static ULONG                prof_nlibs, prof_maxlibs;
static struct ProfLVO      *prof_lvos;
static ULONG                prof_nlvos, prof_maxlvos;
static struct ProfRange    *prof_ranges;
static ULONG                prof_nranges, prof_maxranges;
static struct ProfLibSeg   *prof_libsegs;
static ULONG                prof_nlibsegs, prof_maxlibsegs;

/*
 * The caller window.  prof_vector.S reads all five, so none of them is static
 * and none may be renamed without changing the assembly too.  A zero
 * prof_watch_lo disarms it, which is the state a run that never asked for one
 * stays in, at the cost of one compare per sample.
 */
ULONG                       prof_watch_lo;
ULONG                       prof_watch_hi;
ULONG                       prof_call_next;
ULONG                       prof_call_limit;
ULONG                       prof_ncalls;
/* Bytes from the vector's own SP to the SP the interrupted code was using,
   when that code was already in supervisor mode: our 16 bytes of registers
   plus the exception frame, which grew a word on the 68010. */
ULONG                       prof_frameadj;

/* prof_vector.S reaches into struct Task with these as literals, because an
   interrupt handler cannot call offsetof().  They are Exec ABI and have not
   moved since 1985, but a wrong one here reads a stack bound out of the
   wrong field and validates nothing, so they are checked at compile time. */
_Static_assert(offsetof(struct Task, tc_SPLower) == 58,
               "prof_vector.S has tc_SPLower at 58");
_Static_assert(offsetof(struct Task, tc_SPUpper) == 62,
               "prof_vector.S has tc_SPUpper at 62");

/* Snapshots refused because the stack could not be shown to be one. */
ULONG                       prof_call_refused;

static struct ProfCall     *prof_calls;
static ULONG                prof_maxcalls;

/* What was asked for, held until the library it names registers a seglist. */
static char                 prof_watch_lib[32];
static ULONG                prof_watch_off, prof_watch_len;

static VOID prof_watch_try(VOID);
static struct ProfTask     *prof_tasks;
static ULONG                prof_ntasks, prof_maxtasks;
static struct ProfWindow   *prof_wins;
static ULONG                prof_nwins;

/* Written by the vertical-blank server. */
static volatile ULONG       prof_vblcount;
static volatile ULONG       prof_winleft;

static char                 prof_srcname[32] = "none";
static ULONG                prof_ch = PROF_ANY_CHANNEL;


/* --------------------------------------------------------- small helpers -- */

static VOID prof_copyname(char *dst, const char *src, ULONG size)
{
ULONG i;

    for (i = 0UL; i + 1UL < size && src != NULL && src[i] != '\0'; i++)
    {
        dst[i] = src[i];
    }
    dst[i] = '\0';
}

static VOID prof_add_range(ULONG lo, ULONG hi, UWORD kind, UWORD index,
                           const char *name)
{
    if (prof_ranges == NULL || prof_nranges >= prof_maxranges || hi <= lo)
    {
        return;
    }
    prof_ranges[prof_nranges].pr_Lo    = lo;
    prof_ranges[prof_nranges].pr_Hi    = hi;
    prof_ranges[prof_nranges].pr_Kind  = kind;
    prof_ranges[prof_nranges].pr_Index = index;
    prof_copyname(prof_ranges[prof_nranges].pr_Name, name,
                  sizeof(prof_ranges[prof_nranges].pr_Name));
    prof_nranges++;
}

VOID prof_note_range(ULONG lo, ULONG hi, const char *name)
{
    prof_add_range(lo, hi, PRK_MEMORY, 0U, name);
}


/* ------------------------------------------------------ where code lives --- */

/*
 * Each segment is [size][next][code...], the size longword sitting four bytes
 * below BADDR() and covering the eight-byte header as well.  That is the same
 * walk for any seglist, the one LoadSeg() returned, or the one a running
 * process has in cli_Module.
 */
static ULONG prof_walk_seglist(BPTR seglist, UWORD kind, const char *name,
                               struct ProfSeg *out, ULONG maxout)
{
BPTR  seg;
ULONG n = 0UL;
char  label[32];

    for (seg = seglist; seg != (BPTR)0; seg = (BPTR)(((ULONG *)BADDR(seg))[0]))
    {
    ULONG *hdr  = (ULONG *)BADDR(seg);
    ULONG  base = (ULONG)&hdr[1];
    ULONG  size = hdr[-1] - 8UL;

        if (out != NULL && n < maxout)
        {
            out[n].psg_Base = base;
            out[n].psg_Size = size;
        }

        prof_fmt(label, (ULONG)sizeof(label), "%.20s:%ld", name, (long)n);
        prof_add_range(base, base + size, kind, (UWORD)n, label);

        n++;
        if (n >= 256UL)                 /* a runaway list, not a program */
        {
            break;
        }
    }
    return(n);
}

/*
 * Both of these are called BEFORE prof_start(), because the caller wants to
 * record what it loaded before anything of it runs, and the range table does
 * not exist until prof_start() has counted the machine and allocated one.  So
 * they remember the seglist and prof_start() does the walk.  The alternative,
 * allocating the tables here, would mean the caller had to get the order right
 * for a reason it cannot see.
 */
static BPTR        prof_target_seg;
static char        prof_target_name[24];
static BPTR        prof_self_seg;

VOID prof_set_target(BPTR seglist, const char *name)
{
    prof_target_seg = seglist;
    prof_copyname(prof_target_name, name != NULL ? name : "target",
                  sizeof(prof_target_name));
}

VOID prof_note_profiler_seglist(BPTR seglist)
{
    prof_self_seg = seglist;
}

static VOID prof_record_seglists(VOID)
{
    prof_nsegs = prof_walk_seglist(prof_target_seg, PRK_TARGET,
                                   prof_target_name, prof_segs, PROF_MAX_SEGS);
    if (prof_nsegs > PROF_MAX_SEGS)
    {
        prof_nsegs = PROF_MAX_SEGS;
    }
    (VOID)prof_walk_seglist(prof_self_seg, PRK_PROFILER, "Profile", NULL, 0UL);
}

/*
 * The seglist of the program that is running, not of the shell running it:
 * pr_CLI's cli_Module is the BPTR LoadSeg() returned for THIS executable.
 */
static BPTR prof_own_seglist(VOID)
{
struct Process              *me;
struct CommandLineInterface *cli;

    me = (struct Process *)FindTask(NULL);
    if (me == NULL || me->pr_Task.tc_Node.ln_Type != NT_PROCESS ||
        me->pr_CLI == (BPTR)0)
    {
        return((BPTR)0);
    }
    cli = (struct CommandLineInterface *)BADDR(me->pr_CLI);
    return(cli != NULL ? cli->cli_Module : (BPTR)0);
}

VOID prof_target_is_self(VOID)
{
    prof_set_target(prof_own_seglist(), "self");
}


/* --------------------------------------------- where Kickstart's code is --- */

/*
 * Find a library's ProfSegTag, if it has one.  prof.h states the convention
 * and why it is a scanned self-identifying record rather than an offset.
 *
 * Only the positive half is searched.  That is where a library's data is; the
 * negative half is the jump table, and instruction bytes that happened to
 * spell the magic would be a false positive to reject rather than an extra
 * place worth looking.
 *
 * THE STEP IS TWO BYTES, NOT FOUR.  A longword on m68k is aligned to two, so
 * a record of longwords sits at a four-byte offset only by luck, and it does
 * not: put this at the end of a struct with an odd number of words before it
 * and it lands two mod four.  A four-byte scan walks straight past it and the
 * library reports no tag at all, which looks exactly like a library that
 * carries none.  Cost of getting it right: twice around a loop over a
 * kilobyte or two.
 */
static const struct ProfSegTag *prof_find_segtag(const struct Library *lib)
{
const UWORD *p, *end;
ULONG        span;

    span = (ULONG)lib->lib_PosSize;
    if (span < (ULONG)sizeof(struct ProfSegTag))
    {
        return(NULL);
    }

    p   = (const UWORD *)lib;
    end = (const UWORD *)((const UBYTE *)lib + span
                          - (ULONG)sizeof(struct ProfSegTag));

    for (; p <= end; p++)
    {
    const struct ProfSegTag *t = (const struct ProfSegTag *)p;

        if (t->pst_Magic != PROF_SEGTAG_MAGIC)          { continue; }
        if (t->pst_Size  != (ULONG)sizeof(*t))          { continue; }
        if (t->pst_LibBase != (ULONG)lib)               { continue; }
        if (t->pst_SegList == 0UL)                      { continue; }
        if ((t->pst_Magic + t->pst_Size + t->pst_LibBase +
             t->pst_SegList + t->pst_Sum) != 0UL)       { continue; }

        return(t);
    }
    return(NULL);
}

/*
 * Walk a library's seglist and record its hunks, having first been told where
 * the seglist is by the library itself.
 *
 * THE ANSWER IS CHECKED AGAINST SOMETHING ALREADY KNOWN.  The jump-table
 * targets were resolved a moment ago from Exec's own structures, and every one
 * of them is code in this library, so the hull of them must lie inside the
 * hunks this walk produced.  A tag that points at some other program's
 * seglist, or at a freed one, or at eight bytes that merely look like a
 * segment header, fails that and the whole walk is discarded, the library
 * keeps its hull and stays named by module, which is where it started.
 *
 * The hunks go in only after the check passes, so a rejected library leaves
 * nothing behind.
 */
static VOID prof_scan_lib_seglist(struct Library *lib, UWORD idx,
                                  ULONG hull_lo, ULONG hull_hi)
{
const struct ProfSegTag *tag;
BPTR                     seg;
ULONG                    first = prof_nlibsegs;
ULONG                    n, count = 0UL;
BOOL                     lo_in = FALSE, hi_in = FALSE;

    if (prof_libsegs == NULL)
    {
        return;
    }

    tag = prof_find_segtag(lib);
    if (tag == NULL)
    {
        return;
    }

    for (seg = (BPTR)tag->pst_SegList; seg != (BPTR)0;
         seg = (BPTR)(((ULONG *)BADDR(seg))[0]))
    {
    ULONG *hdr  = (ULONG *)BADDR(seg);
    ULONG  base = (ULONG)&hdr[1];
    ULONG  size;

        /* An eight-byte header plus something, and not so much that the list
           is being read out of somebody else's memory. */
        if (hdr[-1] < 8UL || hdr[-1] > 16UL * 1024UL * 1024UL)
        {
            prof_nlibsegs = first;
            return;
        }
        size = hdr[-1] - 8UL;

        if (prof_nlibsegs >= prof_maxlibsegs)
        {
            prof_nlibsegs = first;
            return;
        }

        prof_libsegs[prof_nlibsegs].pls_Base   = base;
        prof_libsegs[prof_nlibsegs].pls_Size   = size;
        prof_libsegs[prof_nlibsegs].pls_LibIdx = idx;
        prof_libsegs[prof_nlibsegs].pls_Hunk   = (UWORD)count;
        prof_nlibsegs++;
        prof_watch_try();               /* the window may have been waiting */

        if (hull_lo >= base && hull_lo <  base + size) { lo_in = TRUE; }
        if (hull_hi >= base && hull_hi <  base + size) { hi_in = TRUE; }

        count++;
        if (count >= 64UL)              /* a runaway list, not a library */
        {
            prof_nlibsegs = first;
            return;
        }
    }

    /* No hull to check against means nothing checked it, which is not a
       standard this walk gets to skip. */
    if (count == 0UL || hull_hi <= hull_lo || !lo_in || !hi_in)
    {
        prof_nlibsegs = first;
        return;
    }

    for (n = first; n < prof_nlibsegs; n++)
    {
        prof_add_range(prof_libsegs[n].pls_Base,
                       prof_libsegs[n].pls_Base + prof_libsegs[n].pls_Size,
                       PRK_LIBSEG, idx, prof_libs[idx].pl_Name);
    }
}

/*
 * Resolve one jump table.  Entry n of a library sits at base - 6*n and is
 * normally `JMP abs.l`; a few are `JMP d16(PC)`.  Anything else is left out
 * rather than guessed at, a wrong target here would pull unrelated samples
 * onto a name, which is precisely the failure this whole tool exists to avoid.
 *
 * The hull of the targets becomes a named range, which is what gives a module
 * to a sample in a disk-loaded device that has no symbols anywhere.  It is a
 * hull and is labelled as one: it brackets the code, it does not measure it.
 */
static VOID prof_scan_one(struct Library *lib, UWORD type)
{
ULONG   n, count;
ULONG   lo = 0xFFFFFFFFUL, hi = 0UL;
UWORD   idx;

    if (lib == NULL || lib->lib_NegSize < 6)
    {
        return;
    }
    if (prof_nlibs >= prof_maxlibs)
    {
        return;
    }

    /* Already seen: prof_scan_libs() runs before and after the run, so a
       library the program opened mid-run is picked up without the ones that
       were there all along being counted twice. */
    for (n = 0UL; n < prof_nlibs; n++)
    {
        if (prof_libs[n].pl_Base == (ULONG)lib)
        {
            return;
        }
    }

    idx = (UWORD)prof_nlibs;

    prof_libs[idx].pl_Base    = (ULONG)lib;
    prof_libs[idx].pl_NegSize = lib->lib_NegSize;
    prof_libs[idx].pl_Type    = type;
    prof_copyname(prof_libs[idx].pl_Name, (const char *)lib->lib_Node.ln_Name,
                  sizeof(prof_libs[idx].pl_Name));
    prof_nlibs++;

    count = (ULONG)lib->lib_NegSize / 6UL;
    if (count > PROF_MAX_LVO_PER_LIB)
    {
        count = PROF_MAX_LVO_PER_LIB;
    }

    for (n = 1UL; n <= count; n++)
    {
    UBYTE *entry = (UBYTE *)lib - (6UL * n);
    UWORD  op    = *(UWORD *)entry;
    ULONG  target;

        if (op == 0x4EF9U)              /* JMP abs.l   */
        {
            target = *(ULONG *)(entry + 2);
        }
        else if (op == 0x4EFAU)         /* JMP d16(PC) */
        {
            target = (ULONG)(entry + 2) + (ULONG)(LONG)*(WORD *)(entry + 2);
        }
        else
        {
            continue;
        }

        if (target < lo) { lo = target; }
        if (target > hi) { hi = target; }

        if (prof_nlvos >= prof_maxlvos)
        {
            break;
        }

        prof_lvos[prof_nlvos].pv_Target = target;
        prof_lvos[prof_nlvos].pv_LibIdx = idx;
        prof_lvos[prof_nlvos].pv_LVO    = (UWORD)(6UL * n);
        prof_nlvos++;
    }

    /* The jump table itself belongs to the library too: Exec keeps inline code
       in some slots, Forbid() and Permit() among them, so a PC standing in
       the table resolves to no target at all and would otherwise go missing. */
    prof_add_range((ULONG)lib - lib->lib_NegSize, (ULONG)lib + lib->lib_PosSize,
                   PRK_LIB, idx, prof_libs[idx].pl_Name);

    /* The measured extent, if the library will say where its seglist is.
       Tried before the hull is added so it can use the hull to check. */
    prof_scan_lib_seglist(lib, idx, lo, hi);

    /*
     * Past the last entry point there is still code.  Two kilobytes is enough
     * to cover the tail of a function and small enough that it cannot swallow
     * a neighbouring module whole.
     *
     * THE CAP IS NOT DECORATION.  A hull is only a module's extent if the
     * entry points are all in the module, and that is an assumption, not a
     * fact, FS-UAE's uaehf.device points its jump table at trap addresses
     * scattered across memory, which gives a "hull" of several megabytes that
     * then swallows every other range inside it.  Half a megabyte is the whole
     * of Kickstart, so nothing genuine is lost by refusing anything wider, and
     * a library whose hull is refused still gets its jump table named and its
     * entry points resolved by the nearest-preceding rule.
     */
    if (hi > lo && (hi - lo) <= 512UL * 1024UL)
    {
        prof_add_range(lo, hi + 2048UL, PRK_LIB, idx, prof_libs[idx].pl_Name);
    }
}

static VOID prof_scan_list(struct List *list, UWORD type)
{
struct Node *n;

    for (n = list->lh_Head; n->ln_Succ != NULL; n = n->ln_Succ)
    {
        prof_scan_one((struct Library *)n, type);
    }
}

static VOID prof_scan_libs(VOID)
{
struct ExecBase *eb = (struct ExecBase *)SysBase;

    /* Forbid() rather than Disable(): walking three lists and reading a few
       thousand jump tables is far too long to hold interrupts off, and no
       interrupt adds or removes a library. */
    Forbid();
    prof_scan_list(&eb->LibList,      NT_LIBRARY);
    prof_scan_list(&eb->DeviceList,   NT_DEVICE);
    prof_scan_list(&eb->ResourceList, NT_RESOURCE);
    Permit();
}


/* -------------------------------------------------------------- the tasks -- */

static VOID prof_note_task(struct Task *t)
{
ULONG i;

    if (t == NULL || prof_tasks == NULL || prof_ntasks >= prof_maxtasks)
    {
        return;
    }
    for (i = 0UL; i < prof_ntasks; i++)
    {
        if (prof_tasks[i].pt_Task == (ULONG)t)
        {
            return;
        }
    }
    prof_tasks[prof_ntasks].pt_Task = (ULONG)t;
    prof_copyname(prof_tasks[prof_ntasks].pt_Name, (const char *)t->tc_Node.ln_Name,
                  sizeof(prof_tasks[prof_ntasks].pt_Name));
    prof_ntasks++;
}

static VOID prof_scan_tasks(VOID)
{
struct ExecBase *eb = (struct ExecBase *)SysBase;
struct Node     *n;

    Disable();
    prof_note_task(eb->ThisTask);
    for (n = eb->TaskReady.lh_Head; n->ln_Succ != NULL; n = n->ln_Succ)
    {
        prof_note_task((struct Task *)n);
    }
    for (n = eb->TaskWait.lh_Head; n->ln_Succ != NULL; n = n->ln_Succ)
    {
        prof_note_task((struct Task *)n);
    }
    Enable();
}


/* ---------------------------------------------------------- the watchdog -- */

/*
 * A vertical-blank server, which has nothing whatever to do with the sampling
 * source: the beam runs whether or not the audio channel is still ours.
 *
 * This is the running answer to the failure that killed both CIA sources.  A
 * timer that runs correctly for a second and then stops still produces real,
 * correctly sampled PCs, and eight samples rank functions perfectly happily.
 * prof_start() proves the rate over eight windows BEFORE the run; this proves
 * it over every window OF the run.  A program that takes the audio channel
 * back halfway through shows up here as a window with no hits in it, and the
 * report says so instead of ranking half a program as if it were all of it.
 */
static VOID prof_vbl_server(VOID)
{
    prof_vblcount++;

    if (prof_winleft != 0UL && --prof_winleft == 0UL)
    {
        prof_winleft = PROF_WIN_FRAMES;
        if (prof_wins != NULL && prof_nwins < PROF_MAX_WINS)
        {
            prof_wins[prof_nwins].pw_Frames = prof_vblcount;
            prof_wins[prof_nwins].pw_Hits   = prof_hits;
            prof_wins[prof_nwins].pw_Next   = prof_next;
            prof_nwins++;
        }
    }
}

static VOID prof_watchdog_start(VOID)
{
    prof_vblcount = 0UL;
    prof_winleft  = PROF_WIN_FRAMES;
    prof_nwins    = 0UL;

    memset(&prof_vbl_irq, 0, sizeof(prof_vbl_irq));
    prof_vbl_irq.is_Node.ln_Type = NT_INTERRUPT;
    prof_vbl_irq.is_Node.ln_Pri  = -60;     /* after everybody else's */
    prof_vbl_irq.is_Node.ln_Name = (char *)"Profile watchdog";
    prof_vbl_irq.is_Data         = NULL;
    prof_vbl_irq.is_Code         = (VOID (*)())prof_vbl_server;

    AddIntServer(INTB_VERTB, &prof_vbl_irq);
    prof_vbl_held = TRUE;
}

/*
 * Measure the video frame the sampler will use as its clock.
 *
 * ExecBase.VBlankFrequency is a boot-time description, not necessarily the
 * mode the chipset is producing now.  The physical A1200 that exposed this
 * runs a 312-line PAL display while VBlankFrequency still says 60; using that
 * byte made both the rate probe and every saved timestamp call the frame
 * 262 lines long.  The beam is the authority.  The watchdog has already been
 * installed, so wait for one boundary and scan the complete frame after it.
 * Interrupts remain enabled throughout.
 */
static VOID prof_video_measure(VOID)
{
    ULONG before = prof_vblcount;
    ULONG frame;
    ULONG max_vpos = 0UL;
    ULONG guard;

    /* More than a frame even on a 7 MHz 68000, but bounded: the probe below
       owns the diagnostic for a stopped vertical blank. */
    for (guard = 0UL; guard < 2000000UL && prof_vblcount == before; guard++)
        ;
    if (prof_vblcount == before)
        return;

    frame = prof_vblcount;
    for (guard = 0UL; guard < 2000000UL && prof_vblcount == frame; guard++)
    {
        ULONG raw = *PROF_VPOSR;
        ULONG vpos = (((raw >> 16) & 1UL) << 8) |
                     ((raw >> 8) & 0xFFUL);

        if (vpos > max_vpos)
            max_vpos = vpos;
    }
    if (prof_vblcount == frame)
        return;

    prof_ntsc_video = (BOOL)(max_vpos < PROF_LINES_NTSC);
    prof_vblank_hz  = prof_ntsc_video ? 60UL : 50UL;
    prof_framecck   = (prof_ntsc_video ? PROF_LINES_NTSC : PROF_LINES_PAL) *
                      PROF_CCK_LINE;
}

static VOID prof_watchdog_stop(VOID)
{
    if (prof_vbl_held)
    {
        RemIntServer(INTB_VERTB, &prof_vbl_irq);
        prof_vbl_held = FALSE;
    }
    prof_winleft = 0UL;
}


/* ------------------------------------------------------------- audio ------ */

/*
 * One audio channel as a bare interval timer: two bytes of silence on repeat,
 * volume zero, and the only thing wanted from it is the interrupt the DMA
 * raises each time it reloads the pointer.  Period is in colour clocks, so the
 * interval is 2 * AUDxPER / 3.546895 MHz with a one-word length.
 *
 * THE CHANNEL IS ALLOCATED THROUGH audio.device, which is the part that makes
 * this a general-purpose tool rather than our own harness.  A profiler that
 * runs arbitrary programs will sooner or later be pointed at one that wants
 * the audio hardware, and poking DMACON behind audio.device's back means the
 * two quietly fight: the program's sound stutters, our interrupt rate goes
 * wrong, and the profile is garbage that looks like a profile.  Allocating
 * makes the collision an answer, "channel 3 is in use, using channel 2", or
 * "all four channels are in use, refusing to sample", rather than a wrong
 * ranking.
 *
 * Channel 3 first because it is the one least often used alone: a program that
 * wants a single channel usually takes 0, and a program that wants stereo
 * takes 0 and 1.
 */
static struct MsgPort  *prof_audio_port;
static struct IOAudio  *prof_audio_req;
static BOOL             prof_audio_open;
static UWORD           *prof_audio_buf;      /* CHIP RAM, two bytes  */
static struct Interrupt *prof_audio_old;
static BOOL             prof_audio_held;
static UWORD            prof_audio_per;

/* Precedence.  Zero is the documented "normal application" level: a program
   that asks for the channel with a higher precedence takes it from us, which
   is right, the program under test matters more than the measurement, and
   the watchdog then reports that it happened rather than letting the profile
   pretend otherwise. */
#define PROF_AUDIO_PRI  0

static BOOL prof_audio_claim(ULONG ch)
{
UBYTE map[1];

    prof_audio_port = CreateMsgPort();
    if (prof_audio_port == NULL)
    {
        return(FALSE);
    }
    prof_audio_req = (struct IOAudio *)CreateIORequest(prof_audio_port,
                                                       sizeof(struct IOAudio));
    if (prof_audio_req == NULL)
    {
        DeleteMsgPort(prof_audio_port);
        prof_audio_port = NULL;
        return(FALSE);
    }

    map[0] = (UBYTE)(1U << ch);

    prof_audio_req->ioa_Request.io_Message.mn_Node.ln_Pri = PROF_AUDIO_PRI;
    prof_audio_req->ioa_Request.io_Command = ADCMD_ALLOCATE;
    prof_audio_req->ioa_Request.io_Flags   = ADIOF_NOWAIT;
    prof_audio_req->ioa_AllocKey           = 0;
    prof_audio_req->ioa_Data               = map;
    prof_audio_req->ioa_Length             = 1;

    if (OpenDevice((STRPTR)"audio.device", 0UL,
                   (struct IORequest *)prof_audio_req, 0UL) != 0)
    {
        DeleteIORequest((struct IORequest *)prof_audio_req);
        DeleteMsgPort(prof_audio_port);
        prof_audio_req  = NULL;
        prof_audio_port = NULL;
        return(FALSE);
    }

    prof_audio_open = TRUE;
    return(TRUE);
}

static VOID prof_audio_release(VOID)
{
    if (prof_audio_open)
    {
        CloseDevice((struct IORequest *)prof_audio_req);
        prof_audio_open = FALSE;
    }
    if (prof_audio_req != NULL)
    {
        DeleteIORequest((struct IORequest *)prof_audio_req);
        prof_audio_req = NULL;
    }
    if (prof_audio_port != NULL)
    {
        DeleteMsgPort(prof_audio_port);
        prof_audio_port = NULL;
    }
}

static BOOL prof_audio_start(ULONG ch, ULONG rate_hz)
{
    prof_audio_buf = (UWORD *)AllocMem(4UL, MEMF_CHIP | MEMF_CLEAR);
    if (prof_audio_buf == NULL)
    {
        prof_err = "no chip memory for the audio buffer";
        return(FALSE);
    }

    /* One word per pass: interval = 2 * PER colour clocks.  The floor is the
       shortest period the DMA can service, and it is Paula's, not ours. */
    prof_audio_per = (UWORD)(prof_cck / (2UL * rate_hz));
    if (prof_audio_per < 124U)
    {
        prof_audio_per = 124U;
    }

    prof_intf = PROF_INTF_AUD(ch);

    memset(&prof_irq, 0, sizeof(prof_irq));
    prof_irq.is_Node.ln_Type = NT_INTERRUPT;
    prof_irq.is_Node.ln_Pri  = 0;
    prof_irq.is_Node.ln_Name = (char *)"Profile";
    prof_irq.is_Data         = NULL;
    prof_irq.is_Code         = (VOID (*)())prof_audio_stub;

    /* After the allocation, so that whatever audio.device did on the way in is
       done with before we own the slot.  SetIntVector rather than
       AddIntServer: AUDx is a handler slot.  The stub clears INTREQ itself,
       see prof_vector.S.  Nothing latches the way a CIA does, so the
       acknowledgement cannot be lost. */
    prof_audio_old  = SetIntVector(PROF_INTB_AUD(ch), &prof_irq);
    prof_audio_held = TRUE;

    *PROF_DMACON     = PROF_DMAF_AUD(ch);       /* channel off while set up */
    *PROF_AUDLC(ch)  = (ULONG)prof_audio_buf;
    *PROF_AUDLEN(ch) = 1U;
    *PROF_AUDPER(ch) = prof_audio_per;
    *PROF_AUDVOL(ch) = 0U;

    *PROF_INTREQ = prof_intf;
    *PROF_INTENA = PROF_INTF_SETCLR | prof_intf;
    *PROF_DMACON = PROF_DMAF_SETCLR | PROF_DMAF_AUD(ch);

    return(TRUE);
}

static VOID prof_audio_stop(ULONG ch)
{
    *PROF_DMACON     = PROF_DMAF_AUD(ch);
    *PROF_INTENA     = PROF_INTF_AUD(ch);
    *PROF_INTREQ     = PROF_INTF_AUD(ch);
    *PROF_AUDVOL(ch) = 0U;

    if (prof_audio_held)
    {
        (VOID)SetIntVector(PROF_INTB_AUD(ch), prof_audio_old);
        prof_audio_held = FALSE;
    }

    if (prof_audio_buf != NULL)
    {
        FreeMem(prof_audio_buf, 4UL);
        prof_audio_buf = NULL;
    }

    prof_audio_release();
}


/* ------------------------------------------------- install and validate --- */

static VOID prof_install_vector(VOID)
{
    /* Autovector for level 4 is exception vector 24+4. */
    prof_slot = (ULONG *)(prof_vbr + 4UL * (24UL + 4UL));

    Disable();
    prof_chain = *prof_slot;
    *prof_slot = (ULONG)prof_vector;
    Enable();

    CacheClearU();
}

static VOID prof_remove_vector(VOID)
{
    if (prof_slot == NULL)
    {
        return;
    }
    Disable();
    *prof_slot = prof_chain;
    Enable();
    CacheClearU();
    prof_slot = NULL;
}

/*
 * Does this source actually sample at the rate it was told to, WHILE the
 * things a program does are being done?
 *
 * Every static test passes for timers that do not work.  Both CIA-B timers
 * were handed over by AddICRVector(), programmed cleanly, read back exactly
 * what was written, and then stopped, one at the first timer.device open and
 * one at the first line of output.  Both failures produced a handful of real,
 * correctly sampled PCs over more than a second, which is exactly the shape of
 * an answer and none of the substance.  So the probe does those things inside
 * its own window and measures what comes out.
 *
 * The reference is the vertical-blank count, not a timer.device clock: it is
 * the one clock on the machine that cannot be affected by whatever is wrong
 * with the sampling source.
 */
static BOOL prof_probe(VOID)
{
ULONG w, h0, f0, got, frames, expect;
ULONG total = 0UL, total_frames = 0UL;

    for (w = 0UL; w < PROF_PROBE_WINDOWS; w++)
    {
        h0 = prof_hits;
        f0 = prof_vblcount;

        /* Whatever a program does, the probe does: wait on the OS, write a
           line, and let time pass without spinning the CPU flat out. */
        Delay(PROF_PROBE_TICKS);
        if (w == 0UL)
        {
            prof_log("Profile: probing %s", prof_srcname);
        }

        got    = prof_hits - h0;
        frames = prof_vblcount - f0;

        total        += got;
        total_frames += frames;

        if (frames < 2UL)
        {
            prof_err = "the vertical blank is not running";
            return(FALSE);
        }

        /* One frame is 1/50 s PAL, 1/60 NTSC, so this many interrupts should
           have arrived in the frames the beam actually drew. */
        expect = prof_actual_rate() * frames / prof_vblank_hz;
        if (expect == 0UL)
        {
            prof_err = "the probe window was too short to measure";
            return(FALSE);
        }

        /* Every window, not the total.  A source that runs correctly for four
           windows and then stops has an average that still looks plausible,
           and that is precisely the failure this exists to catch. */
        if (got < expect - expect / 3UL || got > expect + expect / 3UL)
        {
            prof_log("Profile:   %s failed window %ld: %ld interrupts in"
                     " %ld frames, expected %ld", prof_srcname, (long)w,
                     (long)got, (long)frames, (long)expect);
            prof_err = "the source did not hold its programmed rate";
            return(FALSE);
        }
    }

    prof_log("Profile:   %s: %ld interrupts over %ld video frames",
             prof_srcname, (long)total, (long)total_frames);
    return(TRUE);
}


/* --------------------------------------------------------------- public --- */

const char *prof_error(VOID)  { return(prof_err); }
const char *prof_conflict(VOID) { return(prof_conf); }
/*
 * Find the library's hunk 0 and arm the window over it.  Called once when the
 * request arrives, in case the library is already open, and again from
 * prof_scan_lib_seglist() every time one registers.
 */
static VOID prof_watch_try(VOID)
{
ULONG i;

    if (prof_watch_lib[0] == '\0' || prof_watch_lo != 0UL)
    {
        return;                         /* nothing asked for, or already armed */
    }

    for (i = 0UL; i < prof_nlibsegs; i++)
    {
        struct ProfLib *lib;

        if (prof_libsegs[i].pls_Hunk != 0U)      /* hunk 0 is .text */
        {
            continue;
        }
        lib = &prof_libs[prof_libsegs[i].pls_LibIdx];
        if (strcmp(lib->pl_Name, prof_watch_lib) != 0)
        {
            continue;
        }

        /* hi before lo: the vector tests lo, so it must be the last write. */
        prof_watch_hi = prof_libsegs[i].pls_Base + prof_watch_off + prof_watch_len;
        prof_watch_lo = prof_libsegs[i].pls_Base + prof_watch_off;
        return;
    }
}

BOOL prof_watch(const char *libname, ULONG off, ULONG len, ULONG maxcalls)
{
    if (libname == NULL || len == 0UL || maxcalls == 0UL)
    {
        prof_err = "watch needs a library, a length and a count";
        return(FALSE);
    }

    if (prof_calls != NULL)
    {
        prof_err = "a caller window is already armed";
        return(FALSE);
    }

    prof_calls = (struct ProfCall *)AllocMem(maxcalls * (ULONG)sizeof(struct ProfCall),
                                             MEMF_ANY | MEMF_CLEAR);
    if (prof_calls == NULL)
    {
        prof_err = "no memory for the caller window";
        return(FALSE);
    }

    prof_maxcalls   = maxcalls;
    prof_ncalls     = 0UL;
    prof_call_next  = (ULONG)prof_calls;
    prof_call_limit = (ULONG)(prof_calls + maxcalls);

    strncpy(prof_watch_lib, libname, sizeof(prof_watch_lib) - 1U);
    prof_watch_lib[sizeof(prof_watch_lib) - 1U] = '\0';
    prof_watch_off = off;
    prof_watch_len = len;

    prof_watch_try();                   /* it may already be open */
    return(TRUE);
}

ULONG prof_call_count(VOID)   { return(prof_ncalls); }
ULONG prof_call_refused_count(VOID) { return(prof_call_refused); }
ULONG prof_watch_base(VOID)   { return(prof_watch_lo); }

ULONG prof_hit_count(VOID)    { return(prof_hits); }
ULONG prof_own_count(VOID)    { return(prof_ownticks); }
ULONG prof_drop_count(VOID)   { return(prof_dropped); }
ULONG prof_capacity(VOID)     { return(prof_max); }
ULONG prof_level(VOID)        { return(4UL); }
ULONG prof_channel(VOID)      { return(prof_ch); }
ULONG prof_frames(VOID)       { return(prof_vblcount); }
ULONG prof_frame_cck(VOID)    { return(prof_framecck); }
ULONG prof_color_clock(VOID)  { return(prof_cck); }
const char *prof_source(VOID) { return(prof_srcname); }

ULONG prof_actual_rate(VOID)
{
    return(prof_audio_per ? (prof_cck / (2UL * (ULONG)prof_audio_per)) : 0UL);
}

const struct ProfSample *prof_buffer(VOID) { return(prof_buf); }

const struct ProfMark *prof_mark_table(ULONG *count)
{
    if (count != NULL) { *count = prof_nmarks; }
    return(prof_marks);
}

const struct ProfRange *prof_range_table(ULONG *count)
{
    if (count != NULL) { *count = prof_nranges; }
    return(prof_ranges);
}

ULONG prof_odd_formats(VOID)
{
struct ExecBase *eb = (struct ExecBase *)SysBase;
ULONG            i, n, bad = 0UL;

    if (prof_buf == NULL || (eb->AttnFlags & AFF_68010) == 0)
    {
        return(0UL);
    }

    n = prof_stored();
    for (i = 0UL; i < n; i++)
    {
        if ((prof_buf[i].ps_Format & 0xF000U) != 0U)
        {
            bad++;
        }
    }
    return(bad);
}

ULONG prof_stored(VOID)
{
    if (prof_buf == NULL)
    {
        return(0UL);
    }
    return((prof_next - (ULONG)prof_buf) / (ULONG)sizeof(struct ProfSample));
}

VOID prof_mark(const char *label)
{
    if (prof_nmarks >= PROF_MAX_MARKS)
    {
        return;
    }
    prof_marks[prof_nmarks].pm_Index = prof_stored();
    prof_copyname(prof_marks[prof_nmarks].pm_Label, label,
                  sizeof(prof_marks[prof_nmarks].pm_Label));
    prof_nmarks++;
}

ULONG prof_worst_window(VOID)
{
ULONG i, worst = 100UL, per;

    if (prof_wins == NULL || prof_nwins < 2UL)
    {
        return(100UL);
    }

    /* Interrupts one window should hold: rate * frames / frames-per-second. */
    per = prof_actual_rate() * PROF_WIN_FRAMES / prof_vblank_hz;
    if (per == 0UL)
    {
        return(100UL);
    }

    for (i = 1UL; i < prof_nwins; i++)
    {
    ULONG got = prof_wins[i].pw_Hits - prof_wins[i - 1UL].pw_Hits;
    ULONG pct = got * 100UL / per;

        if (pct < worst)
        {
            worst = pct;
        }
    }
    return(worst);
}

/*
 * Time the run covered against time actually sampled, from ps_Time.
 *
 * This is the whole point of carrying a timestamp.  Disable() masks INTENA, so
 * a Disable()/Enable() pair takes no samples and its time is charged to
 * whatever runs next, and with the sample ordinal as the clock that is
 * invisible, because a dropped sample simply shifts every sample after it.
 * With a real clock, consecutive samples one interval apart missed nothing and
 * samples four intervals apart swallowed three, and the report can say how
 * much of the run it never saw.
 */
VOID prof_gap_summary(ULONG *total_cck, ULONG *sampled_cck,
                      ULONG *worst_gap_cck, ULONG *worst_gap_index)
{
ULONG i, n, prev = 0UL, total = 0UL, sampled = 0UL;
ULONG worst = 0UL, worst_at = 0UL, step;
ULONG nominal;

    if (total_cck != NULL)       { *total_cck = 0UL; }
    if (sampled_cck != NULL)     { *sampled_cck = 0UL; }
    if (worst_gap_cck != NULL)   { *worst_gap_cck = 0UL; }
    if (worst_gap_index != NULL) { *worst_gap_index = 0UL; }

    n = prof_stored();
    if (prof_buf == NULL || n < 2UL || prof_actual_rate() == 0UL)
    {
        return;
    }

    /* Two intervals: below that a gap is jitter, not a masked section. */
    nominal = 2UL * (prof_cck / prof_actual_rate());

    for (i = 0UL; i < n; i++)
    {
    ULONG raw  = prof_buf[i].ps_Time;
    ULONG vpos = (((raw >> 16) & 1UL) << 8) | ((raw >> 8) & 0xFFUL);
    ULONG hpos = raw & 0xFFUL;
    ULONG now  = vpos * PROF_CCK_LINE + hpos;   /* hpos is colour clocks */

        if (i == 0UL)
        {
            prev = now;
            continue;
        }

        step = (now >= prev) ? (now - prev) : (now + prof_framecck - prev);
        prev = now;

        total += step;
        if (step <= nominal)
        {
            sampled += step;
        }
        else if (step > worst)
        {
            worst    = step;
            worst_at = i;
        }
    }

    if (total_cck != NULL)       { *total_cck = total; }
    if (sampled_cck != NULL)     { *sampled_cck = sampled; }
    if (worst_gap_cck != NULL)   { *worst_gap_cck = worst; }
    if (worst_gap_index != NULL) { *worst_gap_index = worst_at; }
}


/* ------------------------------------------------------------- start/stop -- */

static VOID prof_free_tables(VOID)
{
    if (prof_libs != NULL)
    {
        FreeMem(prof_libs, prof_maxlibs * (ULONG)sizeof(struct ProfLib));
        prof_libs = NULL;
    }
    if (prof_lvos != NULL)
    {
        FreeMem(prof_lvos, prof_maxlvos * (ULONG)sizeof(struct ProfLVO));
        prof_lvos = NULL;
    }
    if (prof_ranges != NULL)
    {
        FreeMem(prof_ranges, prof_maxranges * (ULONG)sizeof(struct ProfRange));
        prof_ranges = NULL;
    }
    if (prof_libsegs != NULL)
    {
        FreeMem(prof_libsegs, prof_maxlibsegs * (ULONG)sizeof(struct ProfLibSeg));
        prof_libsegs = NULL;
    }
    if (prof_tasks != NULL)
    {
        FreeMem(prof_tasks, prof_maxtasks * (ULONG)sizeof(struct ProfTask));
        prof_tasks = NULL;
    }
    if (prof_wins != NULL)
    {
        FreeMem(prof_wins, PROF_MAX_WINS * (ULONG)sizeof(struct ProfWindow));
        prof_wins = NULL;
    }

    /* DISARM BEFORE FREEING.  prof_vector.S tests prof_watch_lo and then
       writes through prof_call_next; freeing first would leave an interrupt
       handler storing into memory Exec had handed to somebody else. */
    prof_watch_lo   = 0UL;
    prof_watch_hi   = 0UL;
    prof_call_next  = 0UL;
    prof_call_limit = 0UL;

    if (prof_calls != NULL)
    {
        FreeMem(prof_calls, prof_maxcalls * (ULONG)sizeof(struct ProfCall));
        prof_calls    = NULL;
        prof_maxcalls = 0UL;
    }
}

/*
 * Count what is on this machine and allocate exactly that, rather than
 * carrying a worst-case table in BSS.  On the 1 MB floor a 64 KB LVO table is
 * a tenth of what a program has to work with, and this tool has to be able to
 * profile a program on that machine without being the reason it fails.
 */
static BOOL prof_alloc_tables(VOID)
{
struct ExecBase *eb = (struct ExecBase *)SysBase;
struct Node     *n;
ULONG            nlib = 0UL, nlvo = 0UL, ntask = 8UL;
int              pass;

    Forbid();
    for (pass = 0; pass < 3; pass++)
    {
    struct List *l = (pass == 0) ? &eb->LibList
                   : (pass == 1) ? &eb->DeviceList
                                 : &eb->ResourceList;

        for (n = l->lh_Head; n->ln_Succ != NULL; n = n->ln_Succ)
        {
        struct Library *lib = (struct Library *)n;
        ULONG           c   = (ULONG)lib->lib_NegSize / 6UL;

            if (lib->lib_NegSize < 6) { continue; }
            if (c > PROF_MAX_LVO_PER_LIB) { c = PROF_MAX_LVO_PER_LIB; }
            nlib++;
            nlvo += c;
        }
    }
    for (n = eb->TaskReady.lh_Head; n->ln_Succ != NULL; n = n->ln_Succ) { ntask++; }
    for (n = eb->TaskWait.lh_Head;  n->ln_Succ != NULL; n = n->ln_Succ) { ntask++; }
    Permit();

    /* Headroom for what the program under test opens while it runs, the
       second scan at prof_stop() is what picks those up. */
    nlib  += 32UL;
    nlvo  += 2048UL;
    ntask += 32UL;

    prof_maxlibs   = nlib;
    prof_maxlvos   = nlvo;
    prof_maxtasks  = ntask;
    /* Hunks of libraries that cooperate.  Nothing sizes this from the machine
       the way the tables above are sized: almost no library carries the tag,
       and the ones that do have a handful of hunks each.  1.5 KB. */
    prof_maxlibsegs = 128UL;
    prof_maxranges  = nlib * 2UL + prof_maxlibsegs + 128UL;

    prof_libs   = (struct ProfLib *)AllocMem(prof_maxlibs * (ULONG)sizeof(struct ProfLib), MEMF_ANY | MEMF_CLEAR);
    prof_lvos   = (struct ProfLVO *)AllocMem(prof_maxlvos * (ULONG)sizeof(struct ProfLVO), MEMF_ANY | MEMF_CLEAR);
    prof_ranges = (struct ProfRange *)AllocMem(prof_maxranges * (ULONG)sizeof(struct ProfRange), MEMF_ANY | MEMF_CLEAR);
    prof_libsegs = (struct ProfLibSeg *)AllocMem(prof_maxlibsegs * (ULONG)sizeof(struct ProfLibSeg), MEMF_ANY | MEMF_CLEAR);
    prof_tasks  = (struct ProfTask *)AllocMem(prof_maxtasks * (ULONG)sizeof(struct ProfTask), MEMF_ANY | MEMF_CLEAR);
    prof_wins   = (struct ProfWindow *)AllocMem(PROF_MAX_WINS * (ULONG)sizeof(struct ProfWindow), MEMF_ANY | MEMF_CLEAR);

    {
        struct ExecBase *eb = (struct ExecBase *)SysBase;

        /* 16 bytes of saved registers, then SR and PC, then the format word
           the 68010 and up append. */
        prof_frameadj = ((eb->AttnFlags & AFF_68010) != 0) ? 24UL : 22UL;
    }

    if (prof_libs == NULL || prof_lvos == NULL || prof_ranges == NULL ||
        prof_libsegs == NULL || prof_tasks == NULL || prof_wins == NULL)
    {
        prof_free_tables();
        prof_err = "no memory for the symbol tables";
        return(FALSE);
    }
    return(TRUE);
}

BOOL prof_start(ULONG max_samples, ULONG rate_hz, ULONG channel)
{
struct ExecBase *eb = (struct ExecBase *)SysBase;
ULONG            ch, first, last, avail, cap;
BOOL             claimed = FALSE;

    if (prof_running)
    {
        prof_err = "already running";
        return(FALSE);
    }

    prof_err     = "";
    prof_conf[0] = '\0';
    prof_hits    = 0UL;
    prof_dropped = 0UL;
    prof_ownticks = 0UL;
    prof_nmarks  = 0UL;
    prof_nlibs   = 0UL;
    prof_nlvos   = 0UL;
    prof_nranges = 0UL;
    prof_nlibsegs = 0UL;
    prof_ntasks  = 0UL;

    /* The audio clock comes from the oscillator.  The frame rate and shape
       come from the video mode; they need not name the same standard. */
    prof_cck = (eb->ex_EClockFrequency > (PROF_ECLOCK_PAL + 2000UL))
                   ? PROF_CCK_NTSC : PROF_CCK_PAL;

    prof_vblank_hz = (ULONG)eb->VBlankFrequency;
    if (prof_vblank_hz < 45UL || prof_vblank_hz > 65UL)
    {
        /* A damaged or pre-initialisation ExecBase must not become a divide
           by zero.  The oscillator is the best fallback available here. */
        prof_vblank_hz =
            (eb->ex_EClockFrequency > (PROF_ECLOCK_PAL + 2000UL))
                ? 60UL : 50UL;
    }
    prof_ntsc_video = (BOOL)(prof_vblank_hz >= 55UL);
    prof_framecck = (prof_ntsc_video ? PROF_LINES_NTSC : PROF_LINES_PAL) *
                    PROF_CCK_LINE;

    if (rate_hz < 32UL)    { rate_hz = 32UL; }
    if (rate_hz > 14000UL) { rate_hz = 14000UL; }
    prof_rate = rate_hz;

    /*
     * The buffer is a ceiling and a fraction, not a number somebody typed.  A
     * profiler that makes the machine it is measuring run out of memory is
     * measuring itself; on the 1 MB floor, a third of what is free is the most
     * that can be taken without changing the answer.
     */
    avail = AvailMem(MEMF_ANY);
    cap   = (avail / 3UL) / (ULONG)sizeof(struct ProfSample);
    if (max_samples == 0UL || max_samples > cap)
    {
        max_samples = cap;
    }
    if (max_samples < 256UL)
    {
        prof_err = "not enough free memory to sample anything";
        return(FALSE);
    }

    if (!prof_alloc_tables())
    {
        return(FALSE);
    }

    prof_bufsize = max_samples * (ULONG)sizeof(struct ProfSample);
    prof_buf = (struct ProfSample *)AllocMem(prof_bufsize, MEMF_ANY | MEMF_CLEAR);
    if (prof_buf == NULL)
    {
        prof_free_tables();
        prof_err = "no memory for the sample buffer";
        return(FALSE);
    }
    prof_max   = max_samples;
    prof_next  = (ULONG)prof_buf;
    prof_limit = (ULONG)prof_buf + prof_bufsize;

    prof_record_seglists();
    prof_scan_libs();
    prof_scan_tasks();

    /* &SysBase->ThisTask, resolved once so the handler is one indirection. */
    prof_taskptr = (ULONG)&eb->ThisTask;

    /* The vector base.  Zero on a 68000, and MOVEC does not exist there,
       AttnFlags is the documented way to ask, and prof_read_vbr() is only
       reached when the answer is yes. */
    prof_vbr = 0UL;
    if ((eb->AttnFlags & AFF_68010) != 0)
    {
        prof_vbr = (ULONG)Supervisor((ULONG (*)())prof_read_vbr);
    }

    prof_watchdog_start();
    prof_video_measure();

    /* Channel 3 first, then 2, 1, 0, or exactly the one asked for. */
    if (channel <= 3UL)
    {
        first = last = channel;
    }
    else
    {
        first = 3UL;
        last  = 0UL;
    }

    for (ch = first; ; ch--)
    {
        if ((*PROF_DMACONR & PROF_DMAF_AUD(ch)) != 0U)
        {
            /* DMA already running on this channel: somebody is using it
               without having gone through audio.device, or is mid-playback. */
            if (prof_conf[0] == '\0')
            {
                prof_fmt(prof_conf, (ULONG)sizeof(prof_conf),
                         "audio channel %ld already has DMA running", (long)ch);
            }
        }
        else if (prof_audio_claim(ch))
        {
            prof_fmt(prof_srcname, (ULONG)sizeof(prof_srcname),
                     "audio channel %ld", (long)ch);
            if (!prof_audio_start(ch, rate_hz))
            {
                prof_audio_release();
            }
            else
            {
                prof_install_vector();
                if (prof_probe())
                {
                    prof_ch = ch;
                    claimed = TRUE;
                    break;
                }
                prof_remove_vector();
                prof_audio_stop(ch);
            }
        }
        else if (prof_conf[0] == '\0')
        {
            prof_fmt(prof_conf, (ULONG)sizeof(prof_conf),
                     "audio channel %ld is allocated by another program",
                     (long)ch);
        }

        if (ch == last)
        {
            break;
        }
    }

    if (!claimed)
    {
        prof_watchdog_stop();
        FreeMem(prof_buf, prof_bufsize);
        prof_buf = NULL;
        prof_free_tables();
        if (prof_err[0] == '\0')
        {
            prof_err = "no audio channel was free to sample with";
        }
        return(FALSE);
    }

    /*
     * A channel that was busy is history now, not a conflict: we moved, and
     * prof_source() already says where to.  Clearing both means PROFF_LOSTAUDIO
     * and prof_conflict() mean exactly one thing, that the run itself was
     * interfered with, rather than sometimes meaning "the first choice was
     * taken and everything after that was fine".
     */
    if (prof_conf[0] != '\0')
    {
        prof_log("Profile: %s, sampling on %s instead", prof_conf, prof_srcname);
        prof_conf[0] = '\0';
    }
    prof_err = "";

    /* Discard the probe's own samples and restart the watchdog's clock, so
       everything reported describes the run and not the setup. */
    prof_next     = (ULONG)prof_buf;
    prof_hits     = 0UL;
    prof_dropped  = 0UL;
    prof_ownticks = 0UL;
    prof_vblcount = 0UL;
    prof_nwins    = 0UL;
    prof_winleft  = PROF_WIN_FRAMES;

    prof_running = TRUE;
    return(TRUE);
}

VOID prof_stop(VOID)
{
    if (!prof_running)
    {
        return;
    }

    prof_remove_vector();

    /* Did we still have the channel at the end?  audio.device hands it to a
       higher-precedence claimant without asking, which is right, and the one
       thing that must not happen is a profile that does not mention it. */
    if ((*PROF_DMACONR & PROF_DMAF_AUD(prof_ch)) == 0U && prof_conf[0] == '\0')
    {
        prof_fmt(prof_conf, (ULONG)sizeof(prof_conf),
                 "audio channel %ld was taken back during the run",
                 (long)prof_ch);
    }

    prof_audio_stop(prof_ch);
    prof_watchdog_stop();

    /* Again, now that the program under test has opened everything it needed:
       a library loaded from disk mid-run is not on Exec's list at start. */
    prof_scan_libs();
    prof_scan_tasks();

    prof_running = FALSE;
}

VOID prof_free(VOID)
{
    if (prof_buf != NULL)
    {
        FreeMem(prof_buf, prof_bufsize);
        prof_buf = NULL;
    }
    prof_free_tables();
}


/* ---------------------------------------------------------------- output -- */

static BOOL prof_put(BPTR fh, const VOID *data, ULONG len)
{
    if (len == 0UL)
    {
        return(TRUE);
    }
    return((BOOL)(Write(fh, (APTR)data, (LONG)len) == (LONG)len));
}

BOOL prof_write(const char *path)
{
struct ProfHeader hdr;
struct ExecBase  *eb = (struct ExecBase *)SysBase;
BPTR              fh;
ULONG             stored, i;
BOOL              ok;

    if (prof_buf == NULL)
    {
        prof_err = "nothing sampled";
        return(FALSE);
    }

    stored = prof_stored();

    memset(&hdr, 0, sizeof(hdr));
    hdr.ph_Magic       = PROF_MAGIC;
    hdr.ph_Version     = PROF_VERSION;
    hdr.ph_RateHz      = prof_actual_rate();
    hdr.ph_Hits        = prof_hits;
    hdr.ph_Stored      = stored;
    hdr.ph_Dropped     = prof_dropped;
    hdr.ph_VBR         = prof_vbr;
    hdr.ph_AttnFlags   = (ULONG)eb->AttnFlags;
    hdr.ph_Level       = 4UL;
    hdr.ph_NumSegs     = prof_nsegs;
    hdr.ph_NumLibs     = prof_nlibs;
    hdr.ph_NumLVOs     = prof_nlvos;
    hdr.ph_NumMarks    = prof_nmarks;
    hdr.ph_ExecVersion = (ULONG)eb->LibNode.lib_Version;
    hdr.ph_NumRanges   = prof_nranges;
    hdr.ph_NumTasks    = prof_ntasks;
    hdr.ph_NumWindows  = prof_nwins;
    hdr.ph_FrameCCK    = prof_framecck;
    hdr.ph_ColorClock  = prof_cck;
    hdr.ph_Frames      = prof_vblcount;
    hdr.ph_Channel     = prof_ch;
    hdr.ph_WinFrames   = PROF_WIN_FRAMES;
    hdr.ph_NumLibSegs  = prof_nlibsegs;
    hdr.ph_NumCalls    = prof_ncalls;
    hdr.ph_CallWords   = (ULONG)PROF_CALL_WORDS;
    hdr.ph_WatchLo     = prof_watch_lo;
    hdr.ph_WatchHi     = prof_watch_hi;

    if ((eb->AttnFlags & AFF_68010) != 0) { hdr.ph_Flags |= PROFF_FMTVALID; }
    if (prof_dropped != 0UL)              { hdr.ph_Flags |= PROFF_OVERFLOW; }
    if (prof_ntsc_video)                  { hdr.ph_Flags |= PROFF_NTSC; }
    if (prof_conf[0] != '\0')             { hdr.ph_Flags |= PROFF_LOSTAUDIO; }
    if (prof_worst_window() < 70UL)       { hdr.ph_Flags |= PROFF_RATEDIP; }

    /* The frame-shape check.  Every interrupt exception on a 68010 and up
       should carry format $0; anything else means the frame was not the shape
       the vector assumed and the PCs in this file are not to be trusted. */
    if ((eb->AttnFlags & AFF_68010) != 0)
    {
        for (i = 0UL; i < stored; i++)
        {
            if ((prof_buf[i].ps_Format & 0xF000U) != 0U)
            {
                hdr.ph_Flags |= PROFF_ODDFORMAT;
                break;
            }
        }
    }

    fh = Open((STRPTR)path, MODE_NEWFILE);
    if (fh == (BPTR)0)
    {
        prof_err = "cannot open the profile file";
        return(FALSE);
    }

    ok  = prof_put(fh, &hdr, (ULONG)sizeof(hdr));
    ok &= prof_put(fh, prof_segs,  prof_nsegs  * (ULONG)sizeof(struct ProfSeg));
    ok &= prof_put(fh, prof_libs,  prof_nlibs  * (ULONG)sizeof(struct ProfLib));
    ok &= prof_put(fh, prof_lvos,  prof_nlvos  * (ULONG)sizeof(struct ProfLVO));
    ok &= prof_put(fh, prof_marks, prof_nmarks * (ULONG)sizeof(struct ProfMark));
    ok &= prof_put(fh, prof_ranges, prof_nranges * (ULONG)sizeof(struct ProfRange));
    ok &= prof_put(fh, prof_tasks, prof_ntasks * (ULONG)sizeof(struct ProfTask));
    ok &= prof_put(fh, prof_wins,  prof_nwins  * (ULONG)sizeof(struct ProfWindow));
    ok &= prof_put(fh, prof_buf,   stored      * (ULONG)sizeof(struct ProfSample));
    /* Last, after the samples: see the version note in prof.h.  A version-2
       reader stops at the end of the sample array and never sees these. */
    ok &= prof_put(fh, prof_libsegs, prof_nlibsegs * (ULONG)sizeof(struct ProfLibSeg));
    /* And the caller snapshots after those, for the same reason. */
    ok &= prof_put(fh, prof_calls, prof_ncalls * (ULONG)sizeof(struct ProfCall));

    Close(fh);

    if (!ok)
    {
        prof_err = "short write";
    }
    return(ok);
}

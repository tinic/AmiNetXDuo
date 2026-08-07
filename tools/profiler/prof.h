/*
 * Profile, timer-driven PC sampling for AmigaOS/m68k.
 *
 * Self-contained: this core needs nothing but exec, dos and the chipset, so
 * it can be lifted out of this tree whole.  tools/profiler/profile.c drives an
 * arbitrary program with it; nothing here knows what that program is.
 *
 * HOW THE PC IS OBTAINED, which is the whole difficulty.
 *
 * A timer raises an interrupt.  We do NOT read the exception frame from an
 * Exec interrupt server: by the time AddIntServer()'s dispatcher calls a
 * server it has pushed registers on top of the frame, and the distance from
 * SSP back to it is undocumented and version-dependent.  A wrong offset there
 * yields a plausible address that is not the PC, and nothing about the
 * resulting profile looks wrong.
 *
 * Instead prof_vector.S is installed directly in the 68k autovector, so it is
 * the first instruction executed after the CPU takes the exception and SP
 * points exactly at the frame the CPU just built:
 *
 *      0(SP)   SR      word
 *      2(SP)   PC      long
 *      6(SP)   format/vector word, 68010 and up only
 *
 * That layout is identical on every 68k.  The 68010+ frame is EIGHT bytes
 * rather than six, but the extra word is APPENDED; SR and PC do not move, so
 * the handler needs no CPU test to find the PC.  It records the format word
 * anyway and prof_write() checks that every sample carried format $0.
 *
 * WHAT RAISES THE INTERRUPT: an audio channel, at level 4.  A CIA timer is the
 * obvious choice and does not work, see the source table in prof.c for the
 * two separate ways both CIA-B timers were lost.  Audio DMA raises the
 * interrupt and nothing latches, so nothing wedges.  Level 4 also means a
 * sample can be taken inside a level-2 or level-3 handler, which is where a
 * SANA-II driver's receive path runs.
 *
 * WHICH CHANNEL: 3 by preference, then 2, 1, 0, and the channel is taken
 * through audio.device rather than by poking DMACON.  A general-purpose
 * profiler runs against programs that may want the audio hardware themselves,
 * so it has to arbitrate for it and be told when it loses, see prof_audio_*
 * in prof.c.
 *
 * WHAT IS INVISIBLE, stated here because a profiler that hides its blind
 * spots is worse than none:
 *
 *   Disable().  Exec's Disable() clears INTENA's master enable, so no sample
 *   is taken inside a Disable()/Enable() pair.  ps_Time makes those gaps
 *   MEASURABLE rather than silent: consecutive samples one interval apart
 *   missed nothing, samples four intervals apart swallowed three.
 *
 *   Level 5 and 6.  A level-4 interrupt cannot preempt a level-5 (disk sync,
 *   serial receive) or level-6 (CIA-B, external) handler.  That time is
 *   charged to whatever runs next, and shows up as a gap in ps_Time.
 *
 *   Forbid()/Permit() does NOT mask interrupts and is sampled normally.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef PROFILE_PROF_H
#define PROFILE_PROF_H

#include <exec/types.h>
#include <dos/dos.h>

/*
 * One sample, 16 bytes, four more than without a timestamp.
 *
 * ps_Time is the raw longword at $DFF004, VPOSR in the high word, VHPOSR in
 * the low.  A single `move.l` with no latch, no side effect and no chip that
 * has to be acknowledged, which is the same reasoning that chose audio over
 * the CIA in the first place.  The beam position is the only free-running
 * counter on a plain OCS 68000 machine that can be read that cheaply.
 *
 * The host decodes it: vpos = (V8 << 8) | V7..V0, hpos = the low byte, and the
 * time within a frame is vpos * 227 + hpos colour clocks.
 *
 * hpos IS ALREADY IN COLOUR CLOCKS.  The hardware reference names the field
 * H8..H1 because the internal counter runs at half a colour clock, so the
 * eight bits exposed are the colour clock number 0..226 and scaling them is
 * wrong.  It is worth stating because the wrong version is not obviously
 * wrong: it inflates a line to 452 clocks, so the position goes BACKWARDS at
 * every line boundary, every close pair of samples that straddles one reads as
 * a frame wrap, and the run appears to contain hundreds of milliseconds that
 * were never in it.  Caught by disagreeing with the vertical-blank count.
 *
 * It wraps once a frame, i.e. every ~20 ms PAL / ~17 ms NTSC, and consecutive
 * samples at any rate this tool will run at are far closer together than that,
 * so unwrapping is unambiguous.  A gap that swallowed a WHOLE frame would
 * alias, ph_Frames is the independent count that catches that.
 *
 * prof_vector.S knows these offsets.
 */
struct ProfSample
{
    ULONG   ps_PC;          /* 2(SP) of the exception frame                */
    UWORD   ps_SR;          /* 0(SP): S bit separates task from interrupt  */
    UWORD   ps_Format;      /* 6(SP): format/vector word, 68010+ only      */
    ULONG   ps_Task;        /* SysBase->ThisTask when the sample was taken */
    ULONG   ps_Time;        /* raw VPOSR:VHPOSR                            */
};

/* 'APR2'.  Deliberately not the 'APRF' that tests/perf/prof writes: the sample
   record grew a field, so an old reader fed a new file would report addresses
   that are half a PC and half a timestamp.  That is precisely the class of
   silently-plausible wrongness this tool exists to avoid, so the two formats
   do not share a magic number.

   Version 3 adds the library segment table and nothing else: the sample record
   is unchanged, which is why the magic is unchanged.  The new table is written
   AFTER the samples and counted by a header field that was reserved and zero
   in version 2, so a version-2 file read by this reader has none and a
   version-3 file read by an older one is exactly a version-2 file with bytes
   after the end.  Neither can misread the other's records. */
#define PROF_MAGIC      0x41505232UL
#define PROF_VERSION    3UL

#define PROFF_FMTVALID  0x00000001UL    /* CPU is 68010+, ps_Format means it  */
#define PROFF_OVERFLOW  0x00000002UL    /* buffer filled; samples were lost   */
#define PROFF_ODDFORMAT 0x00000004UL    /* a frame arrived that was not fmt 0 */
#define PROFF_NTSC      0x00000008UL    /* frame is 262 lines, not 312        */
#define PROFF_LOSTAUDIO 0x00000010UL    /* something took the channel back    */
#define PROFF_RATEDIP   0x00000020UL    /* a window ran well under rate       */

/* Everything is big-endian and naturally aligned, the file is written by the
   68k and read by tools/profiler/profreport.py. */
struct ProfHeader
{
    ULONG   ph_Magic;
    ULONG   ph_Version;
    ULONG   ph_Flags;
    ULONG   ph_RateHz;      /* programmed sample rate                       */
    ULONG   ph_Hits;        /* interrupts the vector saw                    */
    ULONG   ph_Stored;      /* samples in this file                         */
    ULONG   ph_Dropped;     /* hits that found the buffer full              */
    ULONG   ph_VBR;         /* vector base register                         */
    ULONG   ph_AttnFlags;   /* SysBase->AttnFlags                           */
    ULONG   ph_Level;       /* interrupt level interposed                   */
    ULONG   ph_NumSegs;
    ULONG   ph_NumLibs;
    ULONG   ph_NumLVOs;
    ULONG   ph_NumMarks;
    ULONG   ph_ExecVersion;
    ULONG   ph_NumRanges;
    ULONG   ph_NumTasks;
    ULONG   ph_NumWindows;
    ULONG   ph_FrameCCK;    /* colour clocks in one video frame             */
    ULONG   ph_ColorClock;  /* colour clocks per second                     */
    ULONG   ph_Frames;      /* video frames the vertical-blank server saw   */
    ULONG   ph_Channel;     /* audio channel used, or ~0 for a CIA source   */
    ULONG   ph_WinFrames;   /* frames per window in the table below         */
    ULONG   ph_NumLibSegs;  /* version 3; zero in a version-2 file          */
    ULONG   ph_NumCalls;    /* caller snapshots; zero unless WATCH was armed */
    ULONG   ph_CallWords;   /* stack longwords per snapshot                  */
    ULONG   ph_Reserved[2];
};

/* The profiled program's hunks, in load order, which is the order they appear
   in the executable.  The host cross-checks these against the file before it
   resolves a single address. */
struct ProfSeg  { ULONG psg_Base; ULONG psg_Size; };

/*
 * A caller snapshot: the sampled PC, and the top of the stack the interrupted
 * code was using.  Written last in the file, after the library segments, so a
 * reader that does not know about them stops before them.
 *
 * THE STACK IS NOT PARSED HERE.  What a return address looks like is a
 * question about the code that was linked, which is host-side knowledge; the
 * Amiga side copies longwords and says nothing about what they mean.  Stale
 * data below the live frame is expected and is the reader's problem.
 */
#define PROF_CALL_WORDS 6

struct ProfCall
{
    ULONG   pc_PC;                          /* the sampled PC               */
    ULONG   pc_Stack[PROF_CALL_WORDS];      /* from the interrupted SP up   */
};

struct ProfLib
{
    ULONG   pl_Base;
    UWORD   pl_NegSize;
    UWORD   pl_Type;        /* NT_LIBRARY / NT_DEVICE / NT_RESOURCE */
    char    pl_Name[32];
};

/* One resolved jump-table entry: where LVO -pv_LVO of library pv_LibIdx
   actually lands.  A sample in ROM is named by the nearest preceding one. */
struct ProfLVO  { ULONG pv_Target; UWORD pv_LibIdx; UWORD pv_LVO; };

/*
 * One hunk of a library that let its seglist be found, the same pair as
 * ProfSeg, plus which library and which hunk of it.  This is what turns a
 * sample inside a shared library from a module into a function: subtract
 * pls_Base and the remainder is a link-time offset into that hunk, which the
 * host looks up in the library's own symbols exactly as it does the target's.
 */
struct ProfLibSeg
{
    ULONG   pls_Base;
    ULONG   pls_Size;
    UWORD   pls_LibIdx;     /* index into the ProfLib table */
    UWORD   pls_Hunk;       /* load order, so 0 is .text    */
};

/*
 * HOW A LIBRARY LETS ITS SEGLIST BE FOUND.
 *
 * A library base is a private struct.  Exec publishes the two dozen bytes of
 * struct Library at the front of it and nothing after, so the seglist, which
 * every library keeps, because Expunge has to return it, sits at an offset
 * only that library's own source knows.  Reading it from a hard-coded offset
 * would be a profiler asserting a layout it cannot check, and the first field
 * anybody inserted would move it silently: the walk would still find eight
 * bytes that look like a segment header, still produce hunk bases, and still
 * resolve every address in the library to a wrong and entirely plausible name.
 *
 * So the library says where it is instead, in a record that identifies itself:
 *
 *   * Anywhere in the library's own positive half, at any word boundary, a
 *     longword on m68k is aligned to two bytes, so a record of longwords lands
 *     four-aligned only by luck.  The profiler scans [base, base + PosSize)
 *     for the magic; no offset is agreed in advance and none can go stale.
 *   * pst_LibBase must equal the base it was found in, so a copy of the record
 *     that has been moved, a cloned library base, a stale image in freed
 *     memory, is refused rather than believed.
 *   * pst_Sum makes the five longwords add to zero, so the magic appearing by
 *     accident in somebody's data is not enough.
 *
 * The profiler then checks the answer against something it already knows: the
 * hull of the library's own jump-table targets must lie inside the hunks the
 * seglist walk produced.  A seglist that fails that is discarded and the
 * library falls back to being named by module.  Which is also what a library
 * that carries no tag at all gets, most of them, since this is our
 * convention and not Exec's.  See prof_find_segtag() in prof.c.
 *
 * A library adopting this needs no header from here.  It is five longwords:
 * declare them, fill them in at init, and if the base is ever cloned fix up
 * pst_LibBase and pst_Sum in the clone.
 */
#define PROF_SEGTAG_MAGIC   0x50534731UL    /* 'PSG1' */

struct ProfSegTag
{
    ULONG   pst_Magic;      /* PROF_SEGTAG_MAGIC                            */
    ULONG   pst_Size;       /* sizeof(struct ProfSegTag), so it can grow    */
    ULONG   pst_LibBase;    /* the library base this record is embedded in  */
    ULONG   pst_SegList;    /* BPTR, as LoadSeg() returned it               */
    ULONG   pst_Sum;        /* chosen so the five longwords sum to zero     */
};

struct ProfMark { ULONG pm_Index; char pm_Label[28]; };

/*
 * A named address range, which is how a sample outside the profiled program
 * still gets a module.
 *
 * PRK_LIB ranges are the hull of every jump-table target a library resolved
 * to.  That is exact enough to be useful and is stated as a hull rather than
 * as a claim of extent: a disk-loaded device's entry points bracket its code,
 * a ROM library's bracket a region of Kickstart.  Where a sample falls in a
 * hull but near no entry point the host says the module and not the function,
 * which is the honest answer.
 *
 * PRK_LIBSEG is the other thing entirely: a hunk of a library that let its
 * seglist be found, which is a MEASURED extent rather than a bracket, and
 * carries an offset the host can look up in that library's symbols.
 */
#define PRK_TARGET      0       /* a hunk of the program being profiled     */
#define PRK_PROFILER    1       /* a hunk of Profile itself                 */
#define PRK_LIB         2       /* the code hull of a library/device        */
#define PRK_MEMORY      3       /* a MemHeader, for anything left over      */
#define PRK_LIBSEG      4       /* a hunk of a library, from its seglist    */

struct ProfRange
{
    ULONG   pr_Lo;
    ULONG   pr_Hi;              /* exclusive */
    UWORD   pr_Kind;
    UWORD   pr_Index;           /* hunk number, or index into the lib table */
    char    pr_Name[32];
};

/* Task pointer to name, so the host can put a name at the root of a folded
   stack rather than a hex address. */
struct ProfTask { ULONG pt_Task; char pt_Name[28]; };

/*
 * One watchdog window, written by a vertical-blank server that has nothing to
 * do with the sampling source.
 *
 * This is the running answer to the failure that killed both CIA sources: a
 * timer that runs correctly for a while and then stops, having produced enough
 * real samples to rank functions convincingly.  prof_start() proves the rate
 * over eight windows before the run; this proves it over every window OF the
 * run, against a clock the sampling source cannot influence.
 */
struct ProfWindow
{
    ULONG   pw_Frames;      /* video frames since the run started */
    ULONG   pw_Hits;        /* sampling interrupts since the run started */
    ULONG   pw_Next;        /* write cursor, for locating the window's samples */
};


/* ---------------------------------------------------------------- setup --- */

/*
 * Reserve up to `max_samples` records and start sampling at `rate_hz`.
 * `channel` is 0..3 to demand one audio channel, or PROF_ANY_CHANNEL to take
 * the first free one.  FALSE on failure, with prof_error() saying why.
 *
 * max_samples is a ceiling, not a promise: prof_start() will not take more
 * than a third of free memory, because a profiler that makes the machine it
 * is measuring swap or fail an allocation is measuring itself.  Ask
 * prof_capacity() for what it actually got.
 */
#define PROF_ANY_CHANNEL 0xFFFFFFFFUL

BOOL        prof_start(ULONG max_samples, ULONG rate_hz, ULONG channel);

/* Stop the timer and restore the autovector.  Idempotent. */
VOID        prof_stop(VOID);

/* Note a phase boundary at the current sample index. */
VOID        prof_mark(const char *label);

/* Write the profile.  Call after prof_stop(). */
BOOL        prof_write(const char *path);

VOID        prof_free(VOID);


/* -------------------------------------------------------- what is loaded --- */

/*
 * Record the hunks of a seglist as the PROFILED PROGRAM, the thing the host
 * cross-checks against the executable.  Call before prof_start(); Profile
 * calls it with what LoadSeg() returned.
 */
VOID        prof_set_target(BPTR seglist, const char *name);

/*
 * Record the calling process's own hunks as the profiled program.  For a
 * program that links this core in and profiles itself, which is what the
 * self-test does.
 */
VOID        prof_target_is_self(VOID);

/* Record a seglist as the PROFILER's own code, so its samples are named as
   overhead rather than blamed on the program under test. */
VOID        prof_note_profiler_seglist(BPTR seglist);

/* Name an arbitrary address range.  For anything the automatic scans cannot
   see. */
VOID        prof_note_range(ULONG lo, ULONG hi, const char *name);


/* --------------------------------------------------------------- results --- */

const struct ProfSample *prof_buffer(VOID);

/* Arm the caller window over [base+off, base+off+len) of the first hunk of
   the named library, which must already have registered its seglist.  Returns
   FALSE if the library is not known or the buffer cannot be had. */
BOOL prof_watch(const char *libname, ULONG off, ULONG len, ULONG maxcalls);

/* Snapshots captured, and the window actually armed (zero if none). */
ULONG prof_call_count(VOID);
ULONG prof_watch_base(VOID);
const struct ProfMark   *prof_mark_table(ULONG *count);
const struct ProfRange  *prof_range_table(ULONG *count);

/* Samples whose frame was not format $0.  Must be zero on a 68010 and up;
   always zero on a 68000, where there is no format word to read. */
ULONG       prof_odd_formats(VOID);

ULONG       prof_stored(VOID);
ULONG       prof_capacity(VOID);        /* records actually reserved       */
ULONG       prof_hit_count(VOID);       /* every interrupt on the level    */
ULONG       prof_own_count(VOID);       /* only the ones our source raised */
ULONG       prof_drop_count(VOID);
ULONG       prof_actual_rate(VOID);     /* what the hardware really gives  */
ULONG       prof_level(VOID);           /* 68k autovector level in use     */
ULONG       prof_channel(VOID);         /* audio channel, or ~0            */
ULONG       prof_frames(VOID);          /* video frames the run covered    */
const char *prof_error(VOID);
const char *prof_source(VOID);          /* "audio channel 3", say          */
const char *prof_conflict(VOID);        /* "" or what went wrong with it   */

/* Colour clocks in one video frame and per second, so a caller can turn
   ps_Time deltas into microseconds without opening graphics.library. */
ULONG       prof_frame_cck(VOID);
ULONG       prof_color_clock(VOID);

/*
 * Walk the windows and report the worst one, as a percentage of the rate the
 * source was programmed for.  100 means every window held rate.  Anything
 * much under that means the source was interfered with mid-run and the
 * profile is describing part of the program, which is the failure mode that
 * looks most like an answer.
 */
ULONG       prof_worst_window(VOID);

/* Total video frames covered, sampled frames, and the largest gap in frames,
   derived from ps_Time.  Any of the pointers may be NULL. */
VOID        prof_gap_summary(ULONG *total_cck, ULONG *sampled_cck,
                             ULONG *worst_gap_cck, ULONG *worst_gap_index);

/* RawDoFmt-free logging to the serial port, which is where fsuae-run.sh
   captures output from.  prof_log_console() sends it to stdout as well, which
   is what somebody sitting at a real machine wants. */
VOID        prof_log(const char *fmt, ...);
VOID        prof_log_console(BOOL on);

#endif /* PROFILE_PROF_H */

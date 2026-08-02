/*
 * AmiNetXDuo -- timer-driven PC sampling for AmigaOS/m68k.
 *
 * tests/perf/perf_test.c prices every primitive the data path touches and
 * multiplies by call counts.  It accounts for about 22% of a wire transfer and
 * leaves 78% inside NetX Duo with no name on it.  Arithmetic cannot say where
 * that goes; a ranked list of sampled program counters can.
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
 *      6(SP)   format/vector word   -- 68010 and up only
 *
 * That layout is identical on every 68k.  The 68010+ frame is EIGHT bytes
 * rather than six, but the extra word is APPENDED; SR and PC do not move, so
 * the handler needs no CPU test to find the PC.  It records the format word
 * anyway and prof_write() checks that every sample carried format $0, which
 * is what would catch a 68020 throwaway frame (format $1, pushed when SR's M
 * bit is set) or any other shape we did not expect.  On a 68000 that word is
 * stale stack and is ignored; PROFF_FMTVALID says which.
 *
 * WHAT RAISES THE INTERRUPT: audio channel 3, at level 4.  A CIA timer is the
 * obvious choice and does not work -- see the source table in prof.c for the
 * two separate ways both CIA-B timers were lost, and for why an interrupt
 * acknowledged through a chip that latches its own request cannot be trusted
 * at kHz rates on this OS.  Audio DMA raises the interrupt and Exec's
 * dispatcher clears INTREQ; nothing latches, so nothing wedges.  Level 4 also
 * means a sample can be taken inside a level-2 or level-3 handler, which is
 * where a SANA-II driver's receive path runs.
 *
 * The vector chains to the one it displaced with a push/RTS, restoring SP to
 * the frame first, so Exec's own dispatch runs exactly as before.
 *
 * WHAT IS INVISIBLE.  Exec's Disable() clears INTENA's master enable, so no
 * sample is taken inside a Disable()/Enable() pair; the interrupt lands
 * immediately after instead, charging that time to whatever runs next.
 * Forbid()/Permit() does NOT mask interrupts and is sampled normally, which
 * matters because that is where the 214 us ThreadX bracket lives.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_PROF_H
#define AMINETXDUO_PROF_H

#include <exec/types.h>

/* One sample.  Written by prof_vector.S, which knows these offsets. */
struct ProfSample
{
    ULONG   ps_PC;          /* 2(SP) of the exception frame                */
    UWORD   ps_SR;          /* 0(SP): S bit separates task from interrupt  */
    UWORD   ps_Format;      /* 6(SP): format/vector word, 68010+ only      */
    ULONG   ps_Task;        /* SysBase->ThisTask when the sample was taken */
};

#define PROF_MAGIC      0x41505246UL    /* 'APRF' */
#define PROF_VERSION    1UL

#define PROFF_FMTVALID  0x00000001UL    /* CPU is 68010+, ps_Format means it */
#define PROFF_OVERFLOW  0x00000002UL    /* buffer filled; samples were lost   */
#define PROFF_ODDFORMAT 0x00000004UL    /* a frame arrived that was not fmt 0 */

/* Everything is big-endian and naturally aligned -- the file is written by the
   68k and read by tools/prof-report.py. */
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
    ULONG   ph_Level;       /* interrupt level interposed (6 or 2)          */
    ULONG   ph_NumSegs;
    ULONG   ph_NumLibs;
    ULONG   ph_NumLVOs;
    ULONG   ph_NumMarks;
    ULONG   ph_ExecVersion;
    ULONG   ph_Reserved;
};

struct ProfSeg  { ULONG psg_Base; ULONG psg_Size; };

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

struct ProfMark { ULONG pm_Index; char pm_Label[28]; };


/* Reserve `max_samples` records and start sampling at `rate_hz`.  FALSE on
   failure, with prof_error() saying why. */
BOOL        prof_start(ULONG max_samples, ULONG rate_hz);

/* Stop the timer and restore the autovector.  Idempotent. */
VOID        prof_stop(VOID);

/* Note a phase boundary at the current sample index. */
VOID        prof_mark(const char *label);

/* Write the profile.  Call after prof_stop(). */
BOOL        prof_write(const char *path);

VOID        prof_free(VOID);

/* The recorded samples and the phase marks, for a caller that wants to check
   the profile on the Amiga rather than wait for the host tool. */
const struct ProfSample *prof_buffer(VOID);
const struct ProfMark   *prof_mark_table(ULONG *count);

/* Samples whose frame was not format $0.  Must be zero on a 68010 and up;
   always zero on a 68000, where there is no format word to read. */
ULONG       prof_odd_formats(VOID);

/* RawDoFmt-free logging to the serial port, which is where fsuae-run.sh
   captures output from.  Both profiler binaries need it. */
VOID        prof_log(const char *fmt, ...);
VOID        prof_log_flush(VOID);

ULONG       prof_stored(VOID);
ULONG       prof_hit_count(VOID);       /* every interrupt on the level    */
ULONG       prof_cia_count(VOID);       /* only the ones our timer raised  */
ULONG       prof_drop_count(VOID);
ULONG       prof_actual_rate(VOID);     /* what the hardware really gives  */
ULONG       prof_level(VOID);           /* 68k autovector level in use     */
const char *prof_error(VOID);
const char *prof_source(VOID);          /* "ciab.resource timer B", say */

#endif /* AMINETXDUO_PROF_H */

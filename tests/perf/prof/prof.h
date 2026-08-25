/*
 * AmiNetXDuo, timer-driven PC sampling for AmigaOS/m68k.
 *
 * prof_vector.S sits directly in the 68k autovector, so SP points at the CPU
 * exception frame: 0(SP) SR word, 2(SP) PC long, 6(SP) format/vector word on
 * 68010+ (appended; SR and PC do not move).
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

/* Everything is big-endian and naturally aligned, the file is written by the
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

struct ProfLVO  { ULONG pv_Target; UWORD pv_LibIdx; UWORD pv_LVO; };

struct ProfMark { ULONG pm_Index; char pm_Label[28]; };


BOOL        prof_start(ULONG max_samples, ULONG rate_hz);

VOID        prof_stop(VOID);

VOID        prof_mark(const char *label);

/* Call after prof_stop(). */
BOOL        prof_write(const char *path);

VOID        prof_free(VOID);

const struct ProfSample *prof_buffer(VOID);
const struct ProfMark   *prof_mark_table(ULONG *count);

ULONG       prof_odd_formats(VOID);

/* Logs to the serial port: that is where amiberry-run.sh captures output. */
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

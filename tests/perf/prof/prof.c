/*
 * AmiNetXDuo, timer-driven PC sampling.  See prof.h for why the sample is
 * taken in the autovector rather than in an Exec interrupt server.
 *
 * This file does the parts that need AmigaOS rather than 68k: getting a CIA
 * timer without stealing one the OS is using, swapping the vector, and
 * recording everything the host needs to turn a raw address back into a name.
 *
 * THE THREE THINGS THE HOST NEEDS, and where each comes from:
 *
 *   1. Where our own code was loaded.  LoadSeg() puts each hunk wherever it
 *      likes, so a sampled PC means nothing until it is mapped back to the
 *      link-time addresses in the executable.  prof_scan_segs() walks the
 *      seglist out of the CLI that started us and records every hunk's base
 *      and length in load order, which is the same order as the hunks in the
 *      file.
 *
 *   2. Which library or device a ROM sample belongs to.  Every AmigaOS
 *      library is a jump table: LVO -6*n of a base is a JMP to the real
 *      entry point.  prof_scan_libs() resolves all of them for every library,
 *      device and resource on Exec's lists, so a PC in Kickstart can be named
 *      by the nearest preceding entry, "exec.library/Forbid" rather than
 *      "$00f8xxxx".  This is not decoration.  A separate probe measured the
 *      ThreadX bracket at 214 us per socket call and that time is spent in
 *      Exec; a profile that lumped Kickstart into one bucket would hide the
 *      largest known cost in the system.
 *
 *   3. Which task was running.  ThreadX threads are Exec Tasks in this port,
 *      so SysBase->ThisTask splits the profile by thread for the cost of one
 *      indirection in the handler.
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
#include <hardware/cia.h>
#include <resources/cia.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/cia.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "aminetxduo/compat.h"   /* ami_millis(): the probe needs to poke timer.device */


/* ------------------------------------------------------------- logging --- */

/*
 * Straight to the serial port, which is what tools/amiberry-run.sh captures.
 * Same route perf_test.c uses and for the same reason: stdout goes to the
 * emulated screen and nowhere the host can read it.
 */
#ifndef RawPutChar
#  define RawPutChar(c) \
      LP1NR(0x204, RawPutChar, UBYTE, (c), d0, , EXEC_BASE_NAME)
#endif

VOID prof_log(const char *fmt, ...)
{
char     line[256];
va_list  ap;
int      i;

    va_start(ap, fmt);
    /* vsniprintf, not vsnprintf: the double formatter drags in
       mathieeedoubbas.library, which is not on the test hard drive, and the
       program then never reaches main() at all. */
    (VOID)vsniprintf(line, sizeof(line), fmt, ap);
    va_end(ap);

    for (i = 0; line[i] != '\0'; i++)
    {
        RawPutChar((UBYTE)line[i]);
    }
    RawPutChar((UBYTE)'\n');
}

VOID prof_log_flush(VOID)
{
    /* The serial port is written a character at a time, so there is nothing
       buffered; the entry point exists so callers do not have to know that. */
}


/* ------------------------------------------------- shared with the .S ---- */

/* Non-static: prof_vector.S addresses all of these absolute. */
ULONG   prof_next;          /* write cursor                                 */
ULONG   prof_limit;         /* one past the last usable record              */
ULONG   prof_hits;          /* level-N interrupts the vector saw            */
ULONG   prof_dropped;       /* hits that found the buffer full              */
ULONG   prof_chain;         /* the vector we displaced                      */
ULONG   prof_taskptr;       /* &SysBase->ThisTask                           */
ULONG   prof_ciaticks;      /* interrupts from OUR CIA timer specifically   */

extern VOID  prof_vector(VOID);
extern VOID  prof_cia_stub(VOID);
extern VOID  prof_audio_stub(VOID);
extern ULONG prof_read_vbr(VOID);


/* --------------------------------------------------------------- state --- */

#define PROF_MAX_LIBS   192
#define PROF_MAX_LVOS   8192
#define PROF_MAX_SEGS   64
#define PROF_MAX_MARKS  64
#define PROF_MAX_LVO_PER_LIB 640

#define PROF_CIAA       ((struct CIA *)0xBFE001UL)
#define PROF_CIAB       ((struct CIA *)0xBFD000UL)

#define PROF_ECLOCK_PAL 709379UL

/* The probe runs eight windows of 160 ms and requires EVERY one of them to
   come out at the programmed rate.  Both CIA-B timers pass a single 400 ms
   window and then stop, so a probe shorter than about a second, or one that
   only checks the average, accepts a timer that will not survive the run. */
#define PROF_PROBE_WINDOWS  8UL
#define PROF_PROBE_TICKS    8UL

#define PROF_SRC_AUDIO  0
#define PROF_SRC_CIA    1

struct ProfSource
{
    UWORD       src_Kind;
    const char *src_Resource;   /* CIA only */
    struct CIA *src_CIA;        /* CIA only */
    UWORD       src_Bit;        /* CIA: CIAICRB_TA / CIAICRB_TB            */
    UWORD       src_Level;      /* 68k autovector level                   */
    const char *src_Name;
};

/*
 * Audio first, and the CIA only as a fallback, because a CIA timer at kHz
 * rates does not survive on AmigaOS.  This was measured, at length:
 *
 *   AddICRVector() arbitrates the ICR *vector*, not the timer, so both CIA-B
 *   timers were handed over and programmed cleanly.  Timer B then stopped at
 *   the first ami_millis(), which opens timer.device's MICROHZ unit and takes
 *   the timer back.  Timer A ran at a correct 1000 Hz for anything from 0.4
 *   to 1.5 seconds and then stopped dead, vector still ours, INTENA's EXTER
 *   still set, control register still $01, counter still counting, with the
 *   chip's ICR reading $85: an interrupt raised and never acknowledged.
 *   Reading the ICR by hand restarted it immediately.
 *
 *   That is the Exec EXTER race, not a bug here.  Exec clears INTREQ's EXTER
 *   bit around the CIA ICR read that acknowledges the chip; a CIA interrupt
 *   landing in that window leaves INTREQ clear and the CIA's IR bit set, and
 *   because the CIA holds its interrupt line asserted until the ICR is read,
 *   no further edge is ever produced.  At 50 Hz the window is essentially
 *   never hit.  At 1000 Hz it is hit within a second or two, every run.
 *
 *   What made this worth chasing rather than shrugging at: every failure
 *   produced a handful of real, correctly sampled PCs.  Eight samples over
 *   1.4 seconds ranks functions perfectly happily, and the ranking is noise.
 *
 * Audio DMA has no second chip latching anything.  The chipset raises the
 * interrupt, Exec's dispatcher clears INTREQ, and that is the whole
 * acknowledgement path, so the race above cannot occur.  Level 4 also beats
 * the CIA-A fallback's level 2: it can sample inside a level-2 or level-3
 * handler, which is where a SANA-II driver's receive path runs.
 */
static const struct ProfSource prof_sources[] =
{
    { PROF_SRC_AUDIO, NULL, NULL, 0, 4, "audio channel 3" },
    { PROF_SRC_CIA, "ciab.resource", PROF_CIAB, CIAICRB_TA, 6, "ciab.resource timer A" },
    { PROF_SRC_CIA, "ciab.resource", PROF_CIAB, CIAICRB_TB, 6, "ciab.resource timer B" },
    { PROF_SRC_CIA, "ciaa.resource", PROF_CIAA, CIAICRB_TB, 2, "ciaa.resource timer B" },
    { PROF_SRC_CIA, "ciaa.resource", PROF_CIAA, CIAICRB_TA, 2, "ciaa.resource timer A" },
};
#define PROF_NUM_SOURCES (sizeof(prof_sources) / sizeof(prof_sources[0]))

static struct ProfSample   *prof_buf;
static ULONG                prof_bufsize;
static ULONG                prof_max;

static struct Interrupt     prof_irq;
static struct Library      *prof_res;
static const struct ProfSource *prof_src;
static ULONG               *prof_slot;      /* the vector table entry we own */
static BOOL                 prof_running;
static ULONG                prof_rate;
static ULONG                prof_latch;
static ULONG                prof_eclock;
static ULONG                prof_vbr;
static const char          *prof_err = "";
static UBYTE                prof_saved_cr;

static struct ProfSeg       prof_segs[PROF_MAX_SEGS];
static ULONG                prof_nsegs;
static struct ProfLib       prof_libs[PROF_MAX_LIBS];
static ULONG                prof_nlibs;
static struct ProfLVO       prof_lvos[PROF_MAX_LVOS];
static ULONG                prof_nlvos;
static struct ProfMark      prof_marks[PROF_MAX_MARKS];
static ULONG                prof_nmarks;


/* ------------------------------------------------------ where we loaded --- */

/*
 * The seglist of the program that is running, not of the shell running it:
 * pr_CLI's cli_Module is the BPTR LoadSeg() returned for THIS executable.
 * Each segment is [size][next][code...], the size longword sitting four bytes
 * below BADDR() and covering the eight-byte header as well.
 */
static VOID prof_scan_segs(VOID)
{
struct Process              *me;
struct CommandLineInterface *cli;
BPTR                         seg;

    prof_nsegs = 0UL;

    me = (struct Process *)FindTask(NULL);
    if (me == NULL || me->pr_Task.tc_Node.ln_Type != NT_PROCESS ||
        me->pr_CLI == (BPTR)0)
    {
        return;
    }

    cli = (struct CommandLineInterface *)BADDR(me->pr_CLI);
    if (cli == NULL)
    {
        return;
    }

    for (seg = cli->cli_Module;
         seg != (BPTR)0 && prof_nsegs < PROF_MAX_SEGS;
         seg = (BPTR)(((ULONG *)BADDR(seg))[0]))
    {
    ULONG *hdr = (ULONG *)BADDR(seg);

        prof_segs[prof_nsegs].psg_Base = (ULONG)&hdr[1];
        prof_segs[prof_nsegs].psg_Size = hdr[-1] - 8UL;
        prof_nsegs++;
    }
}


/* --------------------------------------------- where Kickstart's code is --- */

/*
 * Resolve one jump table.  Entry n of a library sits at base - 6*n and is
 * normally `JMP abs.l`; a few are `JMP d16(PC)`.  Anything else is left out
 * rather than guessed at, a wrong target here would pull unrelated samples
 * onto a name, which is precisely the failure this whole tool exists to avoid.
 */
static VOID prof_scan_one(struct Library *lib, UWORD type)
{
ULONG   n, count;
UWORD   idx;

    if (lib == NULL || lib->lib_NegSize < 6)
    {
        return;
    }
    if (prof_nlibs >= PROF_MAX_LIBS)
    {
        return;
    }

    idx = (UWORD)prof_nlibs;

    prof_libs[idx].pl_Base    = (ULONG)lib;
    prof_libs[idx].pl_NegSize = lib->lib_NegSize;
    prof_libs[idx].pl_Type    = type;
    prof_libs[idx].pl_Name[0] = '\0';
    if (lib->lib_Node.ln_Name != NULL)
    {
        strncpy(prof_libs[idx].pl_Name, lib->lib_Node.ln_Name,
                sizeof(prof_libs[idx].pl_Name) - 1);
        prof_libs[idx].pl_Name[sizeof(prof_libs[idx].pl_Name) - 1] = '\0';
    }
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

        if (prof_nlvos >= PROF_MAX_LVOS)
        {
            return;
        }

        prof_lvos[prof_nlvos].pv_Target = target;
        prof_lvos[prof_nlvos].pv_LibIdx = idx;
        prof_lvos[prof_nlvos].pv_LVO    = (UWORD)(6UL * n);
        prof_nlvos++;
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

    prof_nlibs = 0UL;
    prof_nlvos = 0UL;

    /* Forbid() rather than Disable(): walking three lists and reading a few
       thousand jump tables is far too long to hold interrupts off, and no
       interrupt adds or removes a library. */
    Forbid();
    prof_scan_list(&eb->LibList,      NT_LIBRARY);
    prof_scan_list(&eb->DeviceList,   NT_DEVICE);
    prof_scan_list(&eb->ResourceList, NT_RESOURCE);
    Permit();
}


/* --------------------------------------------------------- the CIA timer -- */

/* The control register of whichever timer a source names. */
static UBYTE prof_read_cr(const struct ProfSource *src)
{
    return((UBYTE)(src->src_Bit == CIAICRB_TA ? src->src_CIA->ciacra
                                              : src->src_CIA->ciacrb));
}

static VOID prof_timer_program(VOID)
{
struct CIA *cia = prof_src->src_CIA;

    if (prof_src->src_Bit == CIAICRB_TA)
    {
        cia->ciatalo = (UBYTE)(prof_latch & 0xFFUL);
        cia->ciatahi = (UBYTE)((prof_latch >> 8) & 0xFFUL);
        /* Preserve SPMODE and TODIN; RUNMODE 0 is continuous, INMODE 0 counts
           the E-clock, LOAD forces the latch in now. */
        cia->ciacra  = (UBYTE)((cia->ciacra & 0xC0U) |
                               CIACRAF_LOAD | CIACRAF_START);
    }
    else
    {
        cia->ciatblo = (UBYTE)(prof_latch & 0xFFUL);
        cia->ciatbhi = (UBYTE)((prof_latch >> 8) & 0xFFUL);
        /* CRB bits 5-6 are INMODE and must be cleared to count the E-clock;
           bit 7 is ALARM and belongs to the TOD, so it is kept. */
        cia->ciacrb  = (UBYTE)((cia->ciacrb & 0x80U) |
                               CIACRBF_LOAD | CIACRBF_START);
    }
}

static VOID prof_timer_stop(VOID)
{
struct CIA *cia = prof_src->src_CIA;

    if (prof_src->src_Bit == CIAICRB_TA)
    {
        cia->ciacra &= (UBYTE)~CIACRAF_START;
    }
    else
    {
        cia->ciacrb &= (UBYTE)~CIACRBF_START;
    }
}


/* ------------------------------------------------------------- audio ------ */

/*
 * Channel 3 as a bare interval timer: two bytes of silence on repeat, volume
 * zero, and the only thing wanted from it is the interrupt the DMA raises
 * each time it reloads the pointer.  Period is in colour clocks, so the
 * interval is 2 * AUDxPER * (1 / 3.546895 MHz) with a one-word length.
 *
 * The channel is taken directly rather than through audio.device.  Nothing in
 * this tree opens audio.device, the tool is not something the library ships,
 * and everything touched, DMACON, INTENA, the AUD3 interrupt vector, is
 * saved and put back by prof_audio_stop().
 */
#define PROF_CUSTOM     0xDFF000UL
#define PROF_DMACONR    ((volatile UWORD *)(PROF_CUSTOM + 0x002UL))
#define PROF_INTENAR    ((volatile UWORD *)(PROF_CUSTOM + 0x01CUL))
#define PROF_DMACON     ((volatile UWORD *)(PROF_CUSTOM + 0x096UL))
#define PROF_INTENA     ((volatile UWORD *)(PROF_CUSTOM + 0x09AUL))
#define PROF_INTREQ     ((volatile UWORD *)(PROF_CUSTOM + 0x09CUL))
#define PROF_AUD3LC     ((volatile ULONG *)(PROF_CUSTOM + 0x0D0UL))
#define PROF_AUD3LEN    ((volatile UWORD *)(PROF_CUSTOM + 0x0D4UL))
#define PROF_AUD3PER    ((volatile UWORD *)(PROF_CUSTOM + 0x0D6UL))
#define PROF_AUD3VOL    ((volatile UWORD *)(PROF_CUSTOM + 0x0D8UL))

#define PROF_DMAF_AUD3  0x0008U
#define PROF_DMAF_SETCLR 0x8000U
#define PROF_INTF_AUD3  0x0400U
#define PROF_INTF_SETCLR 0x8000U
#define PROF_INTB_AUD3  10

/* Colour clocks per second, PAL.  NTSC is 3579545; the difference moves the
   sample rate by 0.9%, which for a ranking is nothing, and prof_actual_rate()
   is derived from the same constant either way. */
#define PROF_COLORCLOCK 3546895UL

static UWORD               *prof_audio_buf;      /* CHIP RAM, two bytes    */
static struct Interrupt    *prof_audio_old;
static BOOL                 prof_audio_held;
static UWORD                prof_audio_per;

static BOOL prof_audio_start(ULONG rate_hz)
{
    prof_audio_buf = (UWORD *)AllocMem(4UL, MEMF_CHIP | MEMF_CLEAR);
    if (prof_audio_buf == NULL)
    {
        return(FALSE);
    }

    /* One word per pass: interval = 2 * PER colour clocks. */
    prof_audio_per = (UWORD)(PROF_COLORCLOCK / (2UL * rate_hz));
    if (prof_audio_per < 124U)
    {
        prof_audio_per = 124U;
    }

    memset(&prof_irq, 0, sizeof(prof_irq));
    prof_irq.is_Node.ln_Type = NT_INTERRUPT;
    prof_irq.is_Node.ln_Pri  = 0;
    prof_irq.is_Node.ln_Name = (STRPTR)"AmiNetXDuo prof";
    prof_irq.is_Data         = NULL;
    prof_irq.is_Code         = (VOID (*)())prof_audio_stub;

    /* SetIntVector rather than AddIntServer: AUD3 is a handler slot.  The
       stub clears INTREQ itself, see prof_vector.S.  Nothing latches the
       way a CIA does, so the acknowledgement cannot be lost. */
    prof_audio_old  = SetIntVector(PROF_INTB_AUD3, &prof_irq);
    prof_audio_held = TRUE;

    *PROF_DMACON  = PROF_DMAF_AUD3;             /* channel off while set up */
    *PROF_AUD3LC  = (ULONG)prof_audio_buf;
    *PROF_AUD3LEN = 1U;
    *PROF_AUD3PER = prof_audio_per;
    *PROF_AUD3VOL = 0U;

    *PROF_INTREQ  = PROF_INTF_AUD3;
    *PROF_INTENA  = PROF_INTF_SETCLR | PROF_INTF_AUD3;
    *PROF_DMACON  = PROF_DMAF_SETCLR | PROF_DMAF_AUD3;

    return(TRUE);
}

static VOID prof_audio_stop(VOID)
{
    *PROF_DMACON = PROF_DMAF_AUD3;
    *PROF_INTENA = PROF_INTF_AUD3;
    *PROF_INTREQ = PROF_INTF_AUD3;
    *PROF_AUD3VOL = 0U;

    if (prof_audio_held)
    {
        (VOID)SetIntVector(PROF_INTB_AUD3, prof_audio_old);
        prof_audio_held = FALSE;
    }

    if (prof_audio_buf != NULL)
    {
        FreeMem(prof_audio_buf, 4UL);
        prof_audio_buf = NULL;
    }
}


/* ------------------------------------------------- install and validate --- */

static BOOL prof_install(VOID);
static VOID prof_uninstall(VOID);
static BOOL prof_probe(VOID);

static BOOL prof_install(VOID)
{
    if (prof_src->src_Kind == PROF_SRC_CIA)
    {
        /* Masked while the timer is programmed. */
        AbleICR(prof_res, CIAICRF_TA << prof_src->src_Bit);
        prof_saved_cr = prof_read_cr(prof_src);
        prof_timer_program();
    }
    else if (!prof_audio_start(prof_rate))
    {
        return(FALSE);
    }

    /* Autovector for level N is exception vector 24+N. */
    prof_slot = (ULONG *)(prof_vbr + 4UL * (24UL + (ULONG)prof_src->src_Level));

    Disable();
    prof_chain = *prof_slot;
    *prof_slot = (ULONG)prof_vector;
    Enable();

    CacheClearU();

    if (prof_src->src_Kind == PROF_SRC_CIA)
    {
        AbleICR(prof_res, CIAICRF_SETCLR | (CIAICRF_TA << prof_src->src_Bit));
    }

    return(TRUE);
}

static VOID prof_uninstall(VOID)
{
    if (prof_src->src_Kind == PROF_SRC_CIA)
    {
        AbleICR(prof_res, CIAICRF_TA << prof_src->src_Bit);
        prof_timer_stop();
    }
    else
    {
        prof_audio_stop();
    }

    Disable();
    *prof_slot = prof_chain;
    Enable();

    CacheClearU();

    if (prof_src->src_Kind == PROF_SRC_CIA)
    {
    struct CIA *cia = prof_src->src_CIA;

        /* Best effort: the control register goes back, the latch cannot, a
           CIA timer reads back its counter, not what was written.  Which is
           why prof_start() will not touch one that was already running. */
        if (prof_src->src_Bit == CIAICRB_TA)
        {
            cia->ciacra = prof_saved_cr;
        }
        else
        {
            cia->ciacrb = prof_saved_cr;
        }

        RemICRVector(prof_res, (LONG)prof_src->src_Bit, &prof_irq);
    }
}

/*
 * Does this source actually sample at the rate it was told to, WHILE the
 * things the workload does are being done?
 *
 * Every static test passes for timers that do not work.  AddICRVector()
 * arbitrates the ICR vector and not the hardware, so both CIA-B timers were
 * handed over, programmed cleanly, read back exactly what was written, and
 * then stopped:
 *
 *   timer B  ran at a correct 1000 Hz until the first ami_millis(), which
 *            opens timer.device's MICROHZ unit and takes the timer back.
 *   timer A  ran at a correct 1000 Hz until the first line of serial output,
 *            and then never fired again, vector still ours, INTENA's EXTER
 *            still set, control register still $01, and no interrupts.
 *
 * Both failures produced a handful of real, correctly sampled PCs over more
 * than a second, which is exactly the shape of an answer and none of the
 * substance.  So the probe does both of those things inside its own window
 * and measures what comes out.  A source that cannot survive asking the time
 * and printing a line is rejected here rather than at the end of a
 * measurement, where nothing would have noticed.
 */
static BOOL prof_probe(VOID)
{
ULONG w, h, t, ms, got, expect, worst_num = 1UL, worst_den = 1UL;
ULONG total = 0UL, total_ms = 0UL, total_cia = 0UL;

    for (w = 0UL; w < PROF_PROBE_WINDOWS; w++)
    {
        h = prof_hits;
        t = ami_millis();

        /* Whatever the workload does, the probe does: ask the time, print a
           line, and burn CPU in a loop that never enters the OS. */
        Delay(PROF_PROBE_TICKS);
        (VOID)ami_millis();
        if (w == 0UL)
        {
            prof_log("prof: probing %s", prof_src->src_Name);
        }

        ms  = ami_millis() - t;
        got = prof_hits - h;

        total    += got;
        total_ms += ms;
        total_cia = prof_ciaticks;

        if (ms < 20UL)
        {
            return(FALSE);
        }

        expect = prof_actual_rate() * ms / 1000UL;
        if (expect == 0UL)
        {
            return(FALSE);
        }

        /* Every window, not the total.  A timer that runs correctly for four
           windows and then stops has an average that still looks plausible,
           and that is precisely the failure this exists to catch. */
        if (got < expect - expect / 3UL || got > expect + expect / 3UL)
        {
            prof_log("prof:   %s failed window %ld: %ld interrupts in %ld ms,"
                     " expected %ld", prof_src->src_Name, (long)w, (long)got,
                     (long)ms, (long)expect);
            return(FALSE);
        }

        if (got * worst_den < worst_num * expect)
        {
            worst_num = got;
            worst_den = expect;
        }
    }

    (VOID)total_cia;

    prof_log("prof:   %s: %ld interrupts in %ld ms over %ld windows",
             prof_src->src_Name, (long)total, (long)total_ms,
             (long)PROF_PROBE_WINDOWS);

    return(TRUE);
}


/* --------------------------------------------------------------- public --- */

const char *prof_error(VOID)  { return(prof_err); }
ULONG prof_hit_count(VOID)    { return(prof_hits); }
ULONG prof_cia_count(VOID)    { return(prof_ciaticks); }
ULONG prof_drop_count(VOID)   { return(prof_dropped); }
ULONG prof_actual_rate(VOID)
{
    if (prof_src != NULL && prof_src->src_Kind == PROF_SRC_AUDIO)
    {
        return(prof_audio_per ? (PROF_COLORCLOCK / (2UL * prof_audio_per)) : 0UL);
    }
    return(prof_latch ? (prof_eclock / (prof_latch + 1UL)) : 0UL);
}

ULONG prof_level(VOID)
{
    return(prof_src != NULL ? (ULONG)prof_src->src_Level : 0UL);
}

const char *prof_source(VOID)
{
    return(prof_src != NULL ? prof_src->src_Name : "none");
}

const struct ProfSample *prof_buffer(VOID) { return(prof_buf); }

const struct ProfMark *prof_mark_table(ULONG *count)
{
    if (count != NULL)
    {
        *count = prof_nmarks;
    }
    return(prof_marks);
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
    strncpy(prof_marks[prof_nmarks].pm_Label, label,
            sizeof(prof_marks[prof_nmarks].pm_Label) - 1);
    prof_marks[prof_nmarks].pm_Label[sizeof(prof_marks[prof_nmarks].pm_Label) - 1] = '\0';
    prof_nmarks++;
}

BOOL prof_start(ULONG max_samples, ULONG rate_hz)
{
struct ExecBase *eb = (struct ExecBase *)SysBase;
ULONG            i;

    if (prof_running)
    {
        prof_err = "already running";
        return(FALSE);
    }

    prof_err      = "";
    prof_hits     = 0UL;
    prof_dropped  = 0UL;
    prof_ciaticks = 0UL;
    prof_nmarks   = 0UL;

    prof_eclock = eb->ex_EClockFrequency;
    if (prof_eclock < 100000UL || prof_eclock > 2000000UL)
    {
        prof_eclock = PROF_ECLOCK_PAL;
    }

    if (rate_hz < 32UL)   { rate_hz = 32UL; }
    if (rate_hz > 20000UL) { rate_hz = 20000UL; }

    prof_latch = (prof_eclock / rate_hz);
    if (prof_latch > 0UL) { prof_latch--; }
    if (prof_latch < 16UL)    { prof_latch = 16UL; }
    if (prof_latch > 0xFFFFUL) { prof_latch = 0xFFFFUL; }
    prof_rate = rate_hz;

    prof_bufsize = max_samples * (ULONG)sizeof(struct ProfSample);
    prof_buf = (struct ProfSample *)AllocMem(prof_bufsize, MEMF_ANY | MEMF_CLEAR);
    if (prof_buf == NULL)
    {
        prof_err = "no memory for the sample buffer";
        return(FALSE);
    }
    prof_max   = max_samples;
    prof_next  = (ULONG)prof_buf;
    prof_limit = (ULONG)prof_buf + prof_bufsize;

    prof_scan_segs();
    prof_scan_libs();

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

    /*
     * Make the OS take what it wants before asking for what is left.
     * ami_millis() opens timer.device's MICROHZ unit, which claims a CIA
     * timer; do it here and AddICRVector() gives an honest answer, rather
     * than handing over a timer that is repossessed the first time anything
     * asks the time.
     */
    (VOID)ami_millis();

    prof_res = NULL;
    prof_src = NULL;

    for (i = 0UL; i < PROF_NUM_SOURCES; i++)
    {
    const struct ProfSource *cand = &prof_sources[i];

        prof_res = NULL;

        if (cand->src_Kind == PROF_SRC_CIA)
        {
        struct Library *res = (struct Library *)OpenResource((STRPTR)cand->src_Resource);

            if (res == NULL)
            {
                continue;
            }

            /* A running timer belongs to somebody, whether or not they took
               the ICR vector.  Reprogramming it would break them silently,
               there is no way to read a CIA latch back and put it as it
               was. */
            if ((prof_read_cr(cand) &
                 (UBYTE)(cand->src_Bit == CIAICRB_TA ? CIACRAF_START
                                                     : CIACRBF_START)) != 0U)
            {
                continue;
            }

            memset(&prof_irq, 0, sizeof(prof_irq));
            prof_irq.is_Node.ln_Type = NT_INTERRUPT;
            prof_irq.is_Node.ln_Pri  = 0;
            prof_irq.is_Node.ln_Name = (STRPTR)"AmiNetXDuo prof";
            prof_irq.is_Data         = NULL;
            prof_irq.is_Code         = (VOID (*)())prof_cia_stub;

            if (AddICRVector(res, (LONG)cand->src_Bit, &prof_irq) != NULL)
            {
                continue;               /* somebody holds the vector */
            }

            prof_res = res;
        }

        prof_src = cand;

        if (!prof_install())
        {
            if (cand->src_Kind == PROF_SRC_CIA)
            {
                RemICRVector(prof_res, (LONG)cand->src_Bit, &prof_irq);
            }
            prof_src = NULL;
            continue;
        }

        if (prof_probe())
        {
            break;
        }

        /* Programmed cleanly and did not tick at the rate it was told to.
           Something else owns the hardware; put it back and try the next. */
        prof_uninstall();
        prof_res = NULL;
        prof_src = NULL;
    }

    if (prof_src == NULL)
    {
        FreeMem(prof_buf, prof_bufsize);
        prof_buf = NULL;
        prof_err = "no CIA timer both free and sampling at the rate asked for";
        return(FALSE);
    }

    /* Discard the probe's own samples. */
    prof_next     = (ULONG)prof_buf;
    prof_hits     = 0UL;
    prof_dropped  = 0UL;
    prof_ciaticks = 0UL;

    prof_running = TRUE;
    return(TRUE);
}

VOID prof_stop(VOID)
{
    if (!prof_running)
    {
        return;
    }

    prof_uninstall();
    prof_running = FALSE;
}

VOID prof_free(VOID)
{
    if (prof_buf != NULL)
    {
        FreeMem(prof_buf, prof_bufsize);
        prof_buf = NULL;
    }
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
    hdr.ph_Level       = prof_src != NULL ? (ULONG)prof_src->src_Level : 0UL;
    hdr.ph_NumSegs     = prof_nsegs;
    hdr.ph_NumLibs     = prof_nlibs;
    hdr.ph_NumLVOs     = prof_nlvos;
    hdr.ph_NumMarks    = prof_nmarks;
    hdr.ph_ExecVersion = (ULONG)eb->LibNode.lib_Version;

    if ((eb->AttnFlags & AFF_68010) != 0)
    {
        hdr.ph_Flags |= PROFF_FMTVALID;
    }
    if (prof_dropped != 0UL)
    {
        hdr.ph_Flags |= PROFF_OVERFLOW;
    }

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
    ok &= prof_put(fh, prof_buf,   stored      * (ULONG)sizeof(struct ProfSample));

    Close(fh);

    if (!ok)
    {
        prof_err = "short write";
    }
    return(ok);
}

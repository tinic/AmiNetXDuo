/*
 * profspin, an ordinary program, for proving the profiler on something that
 * is not part of it.
 *
 * This links nothing from tools/profiler and is not recompiled for profiling:
 *
 *      Profile OUT=spin.prof profspin RANGES=spin.ranges
 *
 * RANGES is GROUND TRUTH FOR THE CHECKER, not anything the profiler consumes:
 * the exact byte range of each assembly kernel, taken from the linker's own
 * labels, and the wall clock it measured in each phase.  The kernels have no
 * calls in them, so during their phases every sample must land inside them.
 * tools/profiler/profreport.py --contain reads that file.
 *
 * There are no phase marks: the profiler cannot ask a program it did not build
 * to call anything, so the check is made over the whole run against the ratios
 * this program measured for itself.
 *
 * SPDX-License-Identifier: MIT
 */

#include <exec/types.h>
#include <dos/dos.h>
#include <devices/audio.h>

#include <proto/exec.h>
#include <proto/dos.h>

#include <stdio.h>
#include <string.h>

extern VOID spin_a(ULONG reps);
extern VOID spin_b(ULONG reps);
extern VOID spin_c(ULONG reps);
extern UBYTE spin_a_end, spin_b_end, spin_c_end;

#define TEMPLATE "RANGES/K,SCALE/K/N,AUDIO/K/N"

enum { OPT_RANGES, OPT_SCALE, OPT_AUDIO, OPT_COUNT };

#define PHASES 4

/* Roughly 6:3:1 of wall clock across the three kernels, and long enough that
   each is thousands of samples rather than hundreds.  The exact figures do not
   matter: the checker compares sampled share against measured share, it does
   not assume either. */
static const ULONG reps[PHASES] = { 2700000UL, 900000UL, 220000UL, 120000UL };
static const char *names[PHASES] = { "spin_a", "spin_b", "spin_c", "exec" };

static ULONG ms[PHASES];

/* DateStamp() rather than timer.device: ds_Tick is fiftieths of a second,
   which over phases of a second and more is plenty, and it needs no device
   open at all.  A program under a profiler should be as ordinary as possible. */
static ULONG now_ms(VOID)
{
struct DateStamp ds;

    DateStamp(&ds);
    return((ULONG)ds.ds_Minute * 60000UL + (ULONG)ds.ds_Tick * 20UL);
}

/*
 * Hold an audio channel for the whole run, so that "the profiler arbitrates
 * for the hardware rather than fighting a program for it" is something that
 * can be demonstrated instead of asserted:
 *
 *      Profile profspin AUDIO=3
 *
 * should report that channel 3 is taken and move to channel 2, and the
 * profile should be as good as the one without it.
 */
static struct MsgPort *aport;
static struct IOAudio *areq;
static BOOL            aopen;

static BOOL grab_channel(ULONG ch)
{
UBYTE map[1];

    aport = CreateMsgPort();
    if (aport == NULL) { return(FALSE); }
    areq = (struct IOAudio *)CreateIORequest(aport, sizeof(struct IOAudio));
    if (areq == NULL) { DeleteMsgPort(aport); aport = NULL; return(FALSE); }

    map[0] = (UBYTE)(1U << ch);
    areq->ioa_Request.io_Message.mn_Node.ln_Pri = 10;   /* above Profile's 0 */
    areq->ioa_Request.io_Command = ADCMD_ALLOCATE;
    areq->ioa_Request.io_Flags   = ADIOF_NOWAIT;
    areq->ioa_AllocKey           = 0;
    areq->ioa_Data               = map;
    areq->ioa_Length             = 1;

    if (OpenDevice((STRPTR)"audio.device", 0UL, (struct IORequest *)areq, 0UL) != 0)
    {
        DeleteIORequest((struct IORequest *)areq);
        DeleteMsgPort(aport);
        areq = NULL; aport = NULL;
        return(FALSE);
    }
    aopen = TRUE;
    return(TRUE);
}

static VOID drop_channel(VOID)
{
    if (aopen) { CloseDevice((struct IORequest *)areq); aopen = FALSE; }
    if (areq  != NULL) { DeleteIORequest((struct IORequest *)areq); areq = NULL; }
    if (aport != NULL) { DeleteMsgPort(aport); aport = NULL; }
}

int main(void)
{
struct RDArgs *rd;
LONG           opt[OPT_COUNT];
ULONG          scale = 1UL;
ULONG          p, i, t0;
BPTR           fh;
char           line[128];

    memset(opt, 0, sizeof(opt));
    rd = ReadArgs((STRPTR)TEMPLATE, opt, NULL);
    if (rd == NULL)
    {
        PrintFault(IoErr(), (STRPTR)"profspin");
        return(RETURN_ERROR);
    }

    if (opt[OPT_SCALE] != 0L)
    {
        scale = (ULONG)*(LONG *)opt[OPT_SCALE];
        if (scale == 0UL) { scale = 1UL; }
    }

    if (opt[OPT_AUDIO] != 0L)
    {
    ULONG ch = (ULONG)*(LONG *)opt[OPT_AUDIO];

        if (ch <= 3UL && grab_channel(ch))
        {
            Printf((STRPTR)"profspin: holding audio channel %ld\n", (long)ch);
        }
        else
        {
            Printf((STRPTR)"profspin: could NOT take audio channel %ld\n", (long)ch);
        }
    }

    for (p = 0UL; p < PHASES; p++)
    {
        t0 = now_ms();
        switch (p)
        {
            case 0UL: spin_a(reps[0] * scale); break;
            case 1UL: spin_b(reps[1] * scale); break;
            case 2UL: spin_c(reps[2] * scale); break;
            default:
                /* Forbid()/Permit() in a loop.  Those are ROM code reached
                   through exec.library's jump table, and Exec keeps them as
                   inline code in the table rather than as a JMP, so this
                   phase tests the other half of the attribution: that a
                   Kickstart sample is named from the slot it is standing in
                   rather than dropped. */
                for (i = 0UL; i < reps[3] * scale; i++)
                {
                    Forbid();
                    Permit();
                }
                break;
        }
        ms[p] = now_ms() - t0;
    }

    if (opt[OPT_RANGES] != 0L)
    {
        fh = Open((STRPTR)opt[OPT_RANGES], MODE_NEWFILE);
        if (fh == (BPTR)0)
        {
            PrintFault(IoErr(), (STRPTR)opt[OPT_RANGES]);
            FreeArgs(rd);
            return(RETURN_ERROR);
        }

#define EMIT(...) do { \
        int n_ = (int)sniprintf(line, sizeof(line), __VA_ARGS__); \
        (VOID)Write(fh, line, (LONG)n_); \
    } while (0)

        EMIT("range spin_a 0x%08lx 0x%08lx\n",
             (unsigned long)spin_a, (unsigned long)&spin_a_end);
        EMIT("range spin_b 0x%08lx 0x%08lx\n",
             (unsigned long)spin_b, (unsigned long)&spin_b_end);
        EMIT("range spin_c 0x%08lx 0x%08lx\n",
             (unsigned long)spin_c, (unsigned long)&spin_c_end);
        for (p = 0UL; p < PHASES; p++)
        {
            EMIT("ms %s %ld\n", names[p], (long)ms[p]);
        }
#undef EMIT

        Close(fh);
    }

    Printf((STRPTR)"profspin: %ld %ld %ld %ld ms\n",
           (long)ms[0], (long)ms[1], (long)ms[2], (long)ms[3]);

    drop_channel();
    FreeArgs(rd);
    return(RETURN_OK);
}

/*
 * Profile, sample any AmigaOS program.
 *
 *      Profile [RATE=n] [SAMPLES=n] [CHANNEL=n] [STACK=n] [OUT=file]
 *              [FOLDED=file] [TOP=n] [QUIET] <program> [arguments]
 *
 * The program is loaded, its hunk bases are recorded, it is run, and the whole
 * time it runs the level-4 autovector is sampling the program counter.  The
 * program needs no recompilation, no instrumentation and no cooperation; it
 * does not know it is being profiled.
 *
 * WHY RunCommand() AND NOT CreateNewProc() OR SystemTagList().
 *
 * The one thing the host cannot reconstruct on its own is where each hunk of
 * the program was loaded, so whatever runs the program has to hand us the
 * seglist.  That rules SystemTagList() out at once: it loads and runs and
 * unloads, and never shows a caller the seglist it used.
 *
 * That leaves LoadSeg() plus one of two ways to enter it.
 *
 *   RunCommand()      synchronous, in this process, returns the program's
 *                     return code, and takes the seglist WE loaded, so the
 *                     hunk table is recorded before the first instruction
 *                     runs and is exact by construction.  prof_start() before
 *                     and prof_stop() after bracket the run with nothing in
 *                     between: no signal to wait for, no window in which the
 *                     program has finished and the sampler is still going.
 *
 *   CreateNewProc()   asynchronous, in a new process.  Everything above has to
 *                     be rebuilt: NP_ExitCode or a signal to learn that it
 *                     finished, and a race at both ends between the child
 *                     starting and the sampler starting.  It buys one thing,
 *                     the program gets its own stack and its own process, so a
 *                     program that Exit()s does not take the profiler with it.
 *
 * RunCommand(), and the cost is stated rather than hidden: a program that
 * calls Exit() rather than returning from main() unwinds past us, and the
 * profile for that run is not written.  That is the same deal the Shell makes,
 * which is what a Shell command is written against, so in practice it is the
 * uncommon case, and it is loud when it happens rather than quiet.
 *
 * RunCommand() runs the program in THIS process, so a program that reads
 * cli_Module to find its own seglist sees Profile's, not its own.  Nothing in
 * the OS does that; a program that profiles itself might.
 *
 * SPDX-License-Identifier: MIT
 */

#include "prof.h"

#include <exec/execbase.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/rdargs.h>

#include <proto/exec.h>
#include <proto/dos.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define TEMPLATE \
    "RATE/K/N,SAMPLES/K/N,CHANNEL/K/N,STACK/K/N,OUT/K,FOLDED/K,TOP/K/N," \
    "QUIET/S,COMMAND/A/F"

enum { OPT_RATE, OPT_SAMPLES, OPT_CHANNEL, OPT_STACK, OPT_OUT, OPT_FOLDED,
       OPT_TOP, OPT_QUIET, OPT_COMMAND, OPT_COUNT };

/*
 * 1000 Hz on anything with a 68010 or better, 250 on a plain 68000.
 *
 * Measured, by having profspin time itself with the profiler attached and
 * without: 12.52 s against 13.00 s on a 14 MHz 68EC020 at 1000 Hz, and
 * 16.66 s against 17.30 s on a 7 MHz 68000 at 250 Hz.  Both 3.8%, which is
 * roughly 38 us an interrupt on the 020 and 150 us on the 68000, most of it
 * Exec's own level-4 dispatch rather than the eleven instructions in the
 * vector.  The 68000 is about four times slower and samples four times less
 * often, so the two defaults cost the same fraction.
 *
 * 1000 Hz on the 68000 would be 15%, which is no longer measuring the program.
 * RATE= overrides both.
 */
#define RATE_68000      250UL
#define RATE_68010UP    1000UL

#define DEFAULT_STACK   32768UL
#define DEFAULT_TOP     20L

static BOOL quiet;

static VOID say(const char *fmt, ...);


/* -------------------------------------------------------- finding it ------ */

/*
 * LoadSeg() does not search the command path; the Shell does that before it
 * calls LoadSeg().  Doing it here is what makes `Profile List` work rather
 * than only `Profile C:List`.
 *
 * As given first, so an absolute or relative path is never second-guessed,
 * then each directory on the Shell's own path, then C:.  A resident command is
 * loaded from disk instead of being taken from the resident list, which is
 * what we want, because a fresh copy is a seglist we know the extent of.
 */
struct PathEntry { BPTR pe_Next; BPTR pe_Lock; };

static BPTR load_command(const char *name, char *found, ULONG foundlen)
{
struct Process              *me;
struct CommandLineInterface *cli;
struct PathEntry            *pe;
BPTR                         seg, olddir;
char                         buf[256];

    seg = LoadSeg((STRPTR)name);
    if (seg != (BPTR)0)
    {
        (VOID)sniprintf(found, (size_t)foundlen, "%s", name);
        return(seg);
    }

    /* A path was given and it did not load; do not go looking elsewhere for
       something with the same last component. */
    if (strchr(name, ':') != NULL || strchr(name, '/') != NULL)
    {
        return((BPTR)0);
    }

    me  = (struct Process *)FindTask(NULL);
    cli = (me != NULL && me->pr_CLI != (BPTR)0)
        ? (struct CommandLineInterface *)BADDR(me->pr_CLI) : NULL;

    if (cli != NULL)
    {
        for (pe = (struct PathEntry *)BADDR(cli->cli_CommandDir);
             pe != NULL;
             pe = (struct PathEntry *)BADDR(pe->pe_Next))
        {
            olddir = CurrentDir(pe->pe_Lock);
            seg    = LoadSeg((STRPTR)name);
            (VOID)CurrentDir(olddir);
            if (seg != (BPTR)0)
            {
                (VOID)NameFromLock(pe->pe_Lock, (STRPTR)buf, (LONG)sizeof(buf));
                (VOID)sniprintf(found, (size_t)foundlen, "%s/%s", buf, name);
                return(seg);
            }
        }
    }

    (VOID)sniprintf(buf, sizeof(buf), "C:%s", name);
    seg = LoadSeg((STRPTR)buf);
    if (seg != (BPTR)0)
    {
        (VOID)sniprintf(found, (size_t)foundlen, "%s", buf);
    }
    return(seg);
}


/* ----------------------------------------------------- on-Amiga resolving -- */

/*
 * Module attribution, on the machine, with no host tool and no symbols.
 *
 * prof.c records a named range for every hunk of the program, every hunk of
 * Profile itself, and the code hull of every library, device and resource on
 * the machine.  That is enough to say WHICH module a sample is in without
 * knowing a single function name, which is exactly what somebody standing at a
 * real Amiga wants first: is this program's time its own, or Kickstart's, or
 * a device driver's.  Function names need the executable's symbols and stay
 * with tools/profiler/profreport.py on the host.
 *
 * Ranges overlap on purpose, a library's jump table and its code hull are
 * two ranges over the same module, so the narrowest containing range wins.
 */
static const struct ProfRange *ranges;
static ULONG                   nranges;
static UWORD                   order[512];    /* indices, sorted by pr_Lo */
static ULONG                   norder;

static VOID sort_ranges(VOID)
{
ULONG i, j;

    ranges = prof_range_table(&nranges);
    norder = nranges < 512UL ? nranges : 512UL;

    for (i = 0UL; i < norder; i++)
    {
        order[i] = (UWORD)i;
    }

    /* Insertion sort: a couple of hundred entries, once. */
    for (i = 1UL; i < norder; i++)
    {
    UWORD v = order[i];

        for (j = i; j > 0UL && ranges[order[j - 1UL]].pr_Lo > ranges[v].pr_Lo; j--)
        {
            order[j] = order[j - 1UL];
        }
        order[j] = v;
    }
}

static const struct ProfRange *find_range(ULONG pc)
{
LONG                     lo = 0L, hi = (LONG)norder - 1L, mid, k;
const struct ProfRange  *best = NULL;

    while (lo <= hi)
    {
        mid = (lo + hi) / 2L;
        if (ranges[order[mid]].pr_Lo <= pc) { lo = mid + 1L; }
        else                                { hi = mid - 1L; }
    }

    /* lo is one past the last range that starts at or below pc.  Overlapping
       ranges mean the containing one is not always the last, so walk back a
       bounded distance and keep the narrowest. */
    for (k = lo - 1L; k >= 0L && k > lo - 64L; k--)
    {
    const struct ProfRange *r = &ranges[order[k]];

        if (pc < r->pr_Hi)
        {
            if (best == NULL ||
                (r->pr_Hi - r->pr_Lo) < (best->pr_Hi - best->pr_Lo))
            {
                best = r;
            }
        }
    }
    return(best);
}


/* -------------------------------------------------------------- the report -- */

#define MAX_BUCKETS 128

static char  bname[MAX_BUCKETS][32];
static ULONG bcount[MAX_BUCKETS];
static ULONG nbuckets;

static VOID bump(const char *name, ULONG by)
{
ULONG i;

    for (i = 0UL; i < nbuckets; i++)
    {
        if (strcmp(bname[i], name) == 0)
        {
            bcount[i] += by;
            return;
        }
    }
    if (nbuckets >= MAX_BUCKETS)
    {
        bump("(other)", by);
        return;
    }
    (VOID)sniprintf(bname[nbuckets], sizeof(bname[0]), "%s", name);
    bcount[nbuckets] = by;
    nbuckets++;
}

static VOID report(LONG top)
{
const struct ProfSample *s = prof_buffer();
ULONG stored = prof_stored();
ULONG i, super = 0UL;
ULONG total_cck, sampled_cck, worst_cck, worst_at;
LONG  shown;

    if (stored == 0UL)
    {
        say("no samples, the program did not run long enough to be sampled");
        return;
    }

    sort_ranges();

    for (i = 0UL; i < stored; i++)
    {
    const struct ProfRange *r = find_range(s[i].ps_PC);

        if ((s[i].ps_SR & 0x2000U) != 0U)
        {
            super++;
        }
        bump(r != NULL ? r->pr_Name : "(unattributed)", 1UL);
    }

    say("");
    say("%-32s %8s %7s", "module", "samples", "share");
    say("--------------------------------------------------");

    for (shown = 0L; shown < top; shown++)
    {
    ULONG best = 0UL, bi = 0UL;

        for (i = 0UL; i < nbuckets; i++)
        {
            if (bcount[i] > best) { best = bcount[i]; bi = i; }
        }
        if (best == 0UL)
        {
            break;
        }
        say("%-32s %8ld %6ld%%", bname[bi], (long)best,
            (long)(best * 100UL / stored));
        bcount[bi] = 0UL;
    }

    say("--------------------------------------------------");
    say("%ld samples, %ld%% in task context, %ld%% in supervisor/interrupt",
        (long)stored, (long)((stored - super) * 100UL / stored),
        (long)(super * 100UL / stored));

    /*
     * How much of the run was never seen.
     *
     * Exec's Disable() clears INTENA's master enable, so a Disable()/Enable()
     * pair takes no samples at all and its time lands on whatever runs next.
     * With the sample ordinal as the clock that is invisible.  With ps_Time it
     * is a number: if this is a few percent every share above is fine, and if
     * it is thirty then every share above is a share of the part of the run
     * that happened to be visible, which is a different claim entirely.
     */
    prof_gap_summary(&total_cck, &sampled_cck, &worst_cck, &worst_at);
    if (total_cck != 0UL)
    {
    ULONG missing = total_cck - sampled_cck;
    ULONG pct     = missing / (total_cck / 1000UL ? total_cck / 1000UL : 1UL);
    ULONG cck_ms  = prof_color_clock() / 1000UL;

        say("%ld.%02ld s covered, %ld.%ld%% of it unsampled (interrupts masked)",
            (long)(total_cck / prof_color_clock()),
            (long)((total_cck % prof_color_clock()) / (prof_color_clock() / 100UL)),
            (long)(pct / 10UL), (long)(pct % 10UL));

        if (worst_cck > cck_ms && worst_at > 0UL)
        {
            say("longest gap %ld.%ld ms, between $%08lx and $%08lx",
                (long)(worst_cck / cck_ms),
                (long)((worst_cck % cck_ms) * 10UL / cck_ms),
                (unsigned long)s[worst_at - 1UL].ps_PC,
                (unsigned long)s[worst_at].ps_PC);
        }
    }

    if (prof_drop_count() != 0UL)
    {
        say("!! %ld samples dropped, the buffer filled, the tail is missing",
            (long)prof_drop_count());
    }
    if (prof_odd_formats() != 0UL)
    {
        say("!! %ld exception frames were not format $0, do not trust these PCs",
            (long)prof_odd_formats());
    }
    if (prof_worst_window() < 70UL)
    {
        say("!! one half-second window held only %ld%% of the programmed rate --"
            " something interfered with the sampling source",
            (long)prof_worst_window());
    }
    if (prof_conflict()[0] != '\0')
    {
        say("!! %s", prof_conflict());
        /*
         * Profile allocates its channel at the normal application precedence,
         * so a program that asks audio.device for the same one with a higher
         * precedence takes it, and that is the right way round.  A profiler
         * that wins the fight has changed the thing it is measuring; one that
         * loses it has only lost the measurement, and can say so.
         */
        say("!! run it again with CHANNEL=n to sample on one the program does"
            " not want");
    }
}


/* --------------------------------------------------- folded stacks, on-Amiga */

/*
 * One line per unique stack, "frame1;frame2;frame3 <count>".  speedscope,
 * flamegraph.pl and inferno all read it directly, so this is three viewers for
 * one trivial emitter, and emitting it here rather than only on the host
 * means somebody at a real Amiga can produce something a browser will draw
 * without a cross-development machine anywhere in the loop.
 *
 * THERE ARE NO CALL STACKS HERE.  A PC sampler records one address, so a
 * literal folded stack would be one frame deep and an icicle graph of it is a
 * bar chart with extra steps.  What is emitted instead is a synthetic
 * hierarchy that the sample already carries: task, then task-versus-interrupt
 * context, then module.  For "how much of this is Exec, and under which task"
 * that is more use than a real call graph would be, and it is honest, every
 * level of it is measured, none of it is inferred.
 *
 * The host tool adds a fourth level, the function, which needs the
 * executable's symbols.
 */
static BOOL write_folded(const char *path)
{
BPTR  fh;
ULONG i, stored = prof_stored();
const struct ProfSample *s = prof_buffer();

    fh = Open((STRPTR)path, MODE_NEWFILE);
    if (fh == (BPTR)0)
    {
        return(FALSE);
    }

    nbuckets = 0UL;
    for (i = 0UL; i < MAX_BUCKETS; i++)
    {
        bcount[i] = 0UL;
    }

    for (i = 0UL; i < stored; i++)
    {
    const struct ProfRange *r = find_range(s[i].ps_PC);
    char line[80];

        (VOID)sniprintf(line, sizeof(line), "%s;%s",
                        (s[i].ps_SR & 0x2000U) != 0U ? "interrupt" : "task",
                        r != NULL ? r->pr_Name : "(unattributed)");
        bump(line, 1UL);
    }

    for (i = 0UL; i < nbuckets; i++)
    {
    char line[96];
    int  n;

        n = (int)sniprintf(line, sizeof(line), "%s %ld\n", bname[i],
                           (long)bcount[i]);
        (VOID)Write(fh, line, (LONG)n);
    }

    Close(fh);
    return(TRUE);
}


/* ------------------------------------------------------------------ main -- */

static VOID say(const char *fmt, ...)
{
char    line[256];
va_list ap;

    if (quiet)
    {
        return;
    }
    va_start(ap, fmt);
    (VOID)vsniprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    prof_log("%s", line);
}

int main(void)
{
struct ExecBase *eb = (struct ExecBase *)SysBase;
struct RDArgs   *rd;
LONG             opt[OPT_COUNT];
BPTR             seg = (BPTR)0;
char             name[64], found[300], args[256];
const char      *cmdline, *p;
ULONG            rate, samples, channel, stack;
LONG             rc = RETURN_FAIL, top;
int              i;

    memset(opt, 0, sizeof(opt));
    rd = ReadArgs((STRPTR)TEMPLATE, opt, NULL);
    if (rd == NULL)
    {
        PrintFault(IoErr(), (STRPTR)"Profile");
        return(RETURN_ERROR);
    }

    quiet = (BOOL)(opt[OPT_QUIET] != 0L);
    prof_log_console((BOOL)!quiet);

    top     = opt[OPT_TOP]     ? *(LONG *)opt[OPT_TOP]     : DEFAULT_TOP;
    stack   = opt[OPT_STACK]   ? (ULONG)*(LONG *)opt[OPT_STACK]   : DEFAULT_STACK;
    samples = opt[OPT_SAMPLES] ? (ULONG)*(LONG *)opt[OPT_SAMPLES] : 0UL;
    channel = opt[OPT_CHANNEL] ? (ULONG)*(LONG *)opt[OPT_CHANNEL] : PROF_ANY_CHANNEL;
    rate    = opt[OPT_RATE]    ? (ULONG)*(LONG *)opt[OPT_RATE]
            : (((eb->AttnFlags & AFF_68010) != 0) ? RATE_68010UP : RATE_68000);

    /* COMMAND/A/F is the rest of the line: the program, then its arguments
       verbatim.  Splitting on the first space is all that is needed, the
       program under test parses the rest itself, exactly as the Shell would
       have handed it over. */
    cmdline = (const char *)opt[OPT_COMMAND];
    while (*cmdline == ' ' || *cmdline == '\t')
    {
        cmdline++;
    }
    for (p = cmdline, i = 0; *p != '\0' && *p != ' ' && *p != '\t' &&
                             i < (int)sizeof(name) - 1; p++, i++)
    {
        name[i] = *p;
    }
    name[i] = '\0';

    while (*p == ' ' || *p == '\t')
    {
        p++;
    }
    /* RunCommand() wants the argument string newline-terminated, the way a
       Shell hands one over; a command that reads with ReadArgs() needs it. */
    (VOID)sniprintf(args, sizeof(args), "%s\n", p);

    if (name[0] == '\0')
    {
        say("Profile: nothing to run");
        FreeArgs(rd);
        return(RETURN_ERROR);
    }

    seg = load_command(name, found, sizeof(found));
    if (seg == (BPTR)0)
    {
        PrintFault(IoErr(), (STRPTR)name);
        FreeArgs(rd);
        return(RETURN_ERROR);
    }

    say("Profile: %s", found);

    /* Before prof_start(), so the hunk table describes the program and is in
       the profile before a single instruction of it has run. */
    prof_set_target(seg, (const char *)FilePart((STRPTR)found));
    {
    struct Process              *me  = (struct Process *)FindTask(NULL);
    struct CommandLineInterface *cli = (me != NULL && me->pr_CLI != (BPTR)0)
        ? (struct CommandLineInterface *)BADDR(me->pr_CLI) : NULL;

        /* Our own hunks, so Profile's overhead is named as Profile's rather
           than blamed on the program under test. */
        if (cli != NULL)
        {
            prof_note_profiler_seglist(cli->cli_Module);
        }
    }

    if (!prof_start(samples, rate, channel))
    {
        say("Profile: cannot sample: %s", prof_error());
        if (prof_conflict()[0] != '\0')
        {
            say("Profile: %s", prof_conflict());
        }
        UnLoadSeg(seg);
        FreeArgs(rd);
        return(RETURN_FAIL);
    }

    say("Profile: %s at %ld Hz, %ld sample slots (%ld KB)",
        prof_source(), (long)prof_actual_rate(), (long)prof_capacity(),
        (long)(prof_capacity() * sizeof(struct ProfSample) / 1024UL));

    prof_mark("run");
    rc = RunCommand(seg, stack, (STRPTR)args, (LONG)strlen(args));
    prof_mark("end");

    prof_stop();
    UnLoadSeg(seg);

    say("Profile: %s returned %ld", name, (long)rc);

    report(top);

    if (opt[OPT_OUT] != 0L)
    {
        if (prof_write((const char *)opt[OPT_OUT]))
        {
            say("Profile: wrote %s (%ld samples)", (const char *)opt[OPT_OUT],
                (long)prof_stored());
        }
        else
        {
            say("Profile: could not write %s: %s", (const char *)opt[OPT_OUT],
                prof_error());
        }
    }

    if (opt[OPT_FOLDED] != 0L)
    {
        if (write_folded((const char *)opt[OPT_FOLDED]))
        {
            say("Profile: wrote %s", (const char *)opt[OPT_FOLDED]);
        }
        else
        {
            say("Profile: could not write %s", (const char *)opt[OPT_FOLDED]);
        }
    }

    prof_free();
    FreeArgs(rd);
    return(rc);
}

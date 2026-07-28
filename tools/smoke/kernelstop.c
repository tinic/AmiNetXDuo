/*
 * tx_amiga_kernel_stop() -- does the machine survive the program?
 *
 * The failure this exists to catch is not a wrong return code, it is a wild
 * jump one tick AFTER a clean "PASS": tx_amiga_kernel_start() leaves the tick
 * Task and the scheduler Task running with their entry points inside the
 * program's code hunk, and AmigaDOS frees that hunk the instant the program
 * exits.  A test that merely called stop and returned could not tell a working
 * shutdown from a broken one -- both print PASS, and only one of them takes the
 * machine down 20 ms later, by which time the harness has already recorded the
 * exit status.
 *
 * So this binary runs as TWO processes.
 *
 *   parent (no arguments -- what s/Startup-Sequence runs)
 *       System()s a SECOND COPY of itself.  AmigaDOS LoadSeg()s that copy into
 *       its own hunk and UnLoadSeg()s it when the child exits, so the child
 *       really does experience the thing a program experiences on exit, while
 *       the parent's own hunk stays put to report on it.  Afterwards the parent
 *
 *         - looks for surviving "ThreadX" / "ThreadX tick" Tasks by name;
 *         - Fills free memory with the 68000 ILLEGAL INSTRUCTION, so that a
 *           stale Task jumping into the child's freed hunk faults immediately
 *           and visibly instead of running whatever happens to be there;
 *         - stays alive for several seconds -- hundreds of ticks -- and only
 *           then reports.
 *
 *   child (one argument)
 *       Starts the kernel, does real work on it, tears the work down, calls
 *       tx_amiga_kernel_stop(), and exits normally.  It also checks the
 *       contract: that stop REFUSES while application threads exist, that the
 *       refusal leaves the kernel usable, that it is idempotent, and that
 *       start -> stop -> start -> stop works.
 *
 * Build:
 *
 *   cmake --build build/cm --parallel --target smoke_KernelStop
 *
 * The output name is load-bearing: the parent runs "SYS:KernelStop child",
 * so the binary has to be called KernelStop whatever the CMake target is
 * named.
 *
 * Run:
 *
 *   AMINETXDUO_RUN_TAG=kstop ./tools/fsuae-run.sh -t 120 \
 *       build/cm/tools/smoke/KernelStop
 *
 * SPDX-License-Identifier: MIT
 */

/* tx_api.h FIRST: exec/types.h does #define VOID void, which collides with
   tx_port.h's typedef void VOID if the Amiga headers land first. */
#include "tx_api.h"
#include "tx_amiga.h"

#include <exec/memory.h>
#include <exec/tasks.h>
#include <dos/dos.h>
#include <dos/dostags.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include "aminetxduo/compat.h"
#include "aminetxduo/crashguard.h"

static const char version_tag[] __attribute__((used)) =
    "$VER: KernelStop 1.0 (25.7.2026)";

#define CHILD_COMMAND   "SYS:KernelStop child <NIL: >>DH0:kstop.txt"

#define WORKERS         3
#define STACK_BYTES     4096

/* How long the parent keeps the machine running after the child's hunk is
   gone.  At 50 Hz a stale tick gets ~400 chances to jump into freed memory. */
#define SURVIVE_TICKS   400L

static LONG checks, failures;

/* ---- the child's ThreadX objects ---------------------------------------- */

static TX_THREAD        self;
static TX_THREAD        worker[WORKERS];
static APTR             worker_stack[WORKERS];
static TX_SEMAPHORE     ping;
static TX_TIMER         beat;
static volatile ULONG   beats;
static volatile ULONG   loops[WORKERS];
static volatile ULONG   stop_now;


static void check(const char *what, BOOL ok)
{
    checks++;
    if (!ok)
        failures++;
    Printf((CONST_STRPTR)"  %s %s\n", (LONG)(ok ? "ok  " : "FAIL"), (LONG)what);
    AMI_ERROR("  %s %s", (LONG)(ok ? "ok  " : "FAIL"), (LONG)what);
}

static void checkv(const char *what, BOOL ok, LONG value)
{
    checks++;
    if (!ok)
        failures++;
    Printf((CONST_STRPTR)"  %s %s (%ld)\n", (LONG)(ok ? "ok  " : "FAIL"), (LONG)what, value);
    AMI_ERROR("  %s %s (%ld)", (LONG)(ok ? "ok  " : "FAIL"), (LONG)what, value);
}


/* ------------------------------------------------------------------------ */
/* child                                                                     */
/* ------------------------------------------------------------------------ */

static VOID worker_entry(ULONG id)
{
    while (stop_now == 0)
    {
        loops[id]++;
        (VOID)tx_semaphore_get(&ping, 2);
        (VOID)tx_thread_sleep(1);
    }
}

static VOID beat_entry(ULONG id)
{
    (VOID)id;
    beats++;
    (VOID)tx_semaphore_put(&ping);
}

VOID tx_application_define(VOID *first_unused)
{
    (VOID)first_unused;
    (VOID)tx_semaphore_create(&ping, "ping", 0);
    (VOID)tx_timer_create(&beat, "beat", beat_entry, 0, 5, 5, TX_AUTO_ACTIVATE);
}

/* Everything the "real work" phase creates, and its teardown.  Split out
   because the child does it twice -- once per kernel lifetime. */

static void work_start(void)
{
    int i;

    stop_now = 0;
    for (i = 0; i < WORKERS; i++)
    {
        loops[i]        = 0;
        worker_stack[i] = ami_alloc(STACK_BYTES);
        if (worker_stack[i] == NULL)
            continue;
        (VOID)tx_thread_create(&worker[i], "kstop-worker", worker_entry, (ULONG)i,
                               worker_stack[i], STACK_BYTES, 12, 12,
                               TX_NO_TIME_SLICE, TX_AUTO_START);
    }
}

static void work_stop(void)
{
    int i;

    stop_now = 1;
    for (i = 0; i < WORKERS; i++)
    {
        if (worker_stack[i] == NULL)
            continue;
        (VOID)tx_thread_wait_abort(&worker[i]);
        (VOID)tx_thread_terminate(&worker[i]);
        (VOID)tx_thread_delete(&worker[i]);
        ami_free(worker_stack[i]);
        worker_stack[i] = NULL;
    }

    /* The kernel's own objects go too: a stop is only defined once the caller
       has taken back everything it created (see tx_amiga.h). */
    (VOID)tx_timer_deactivate(&beat);
    (VOID)tx_timer_delete(&beat);
    (VOID)tx_semaphore_delete(&ping);
}

/*
 * A ThreadX thread that blocks in EXEC rather than on its ThreadX run signal --
 * the SANA-II reader parked in WaitIO() on a device that ignores AbortIO().
 * The port cannot wake it, so tx_thread_delete() detaches it and declares a
 * zombie; the zombie keeps running on its stack until it unblocks on its own.
 * Stop must refuse for exactly as long as that lasts.
 */
static TX_THREAD        stuck;
static volatile ULONG   stuck_running;
static volatile ULONG   stuck_release;
static volatile ULONG   stuck_gone;

static VOID stuck_entry(ULONG id)
{
    (VOID)id;
    stuck_running = 1;
    while (stuck_release == 0)
        (VOID)Wait(SIGBREAKF_CTRL_F);
    stuck_gone = 1;
}

/* A ThreadX object graph is only really exercised if the clock moves. */
static BOOL clock_ran(ULONG ticks)
{
    ULONG t0 = tx_time_get();
    ULONG spin;

    for (spin = 0; spin < 400 && (tx_time_get() - t0) < ticks; spin++)
        (VOID)tx_thread_sleep(2);

    return (tx_time_get() - t0) >= ticks;
}

static int child_main(void)
{
    UINT  status;
    ULONG mem_before, mem_after;
    ULONG zombies;
    int   i;

    Printf((CONST_STRPTR)"KernelStop child: start / work / stop / exit\n");
    AMI_ERROR("=== KernelStop CHILD, hunk at %08lx", (LONG)child_main);

    ami_crash_set_reference((APTR)child_main, "child_main");
    (VOID)ami_crash_install();
    (VOID)ami_crash_install_alert_hook();

    mem_before = AvailMem(MEMF_PUBLIC);

    /* ---- stop before start is a no-op, not an error --------------------- */

    check("stop on a kernel that was never started returns TX_SUCCESS",
          tx_amiga_kernel_stop() == TX_SUCCESS);

    /* ---- lifetime 1 ------------------------------------------------------ */

    status = tx_amiga_kernel_start();
    check("kernel started", status == TX_SUCCESS);
    if (status != TX_SUCCESS)
        return RETURN_ERROR;

    check("kernel reports running", tx_amiga_kernel_running() == TX_TRUE);
    check("the tick Task exists", FindTask((STRPTR)"ThreadX tick") != NULL);
    check("the scheduler Task exists", FindTask((STRPTR)"ThreadX") != NULL);

    status = tx_amiga_adopt_thread(&self, "kstop-main", 16);
    check("adopted the calling Process", status == TX_SUCCESS);

    work_start();
    check("the ThreadX clock advances", clock_ran(50));
    checkv("the application timer fired", beats > 0, (LONG)beats);

    for (i = 0; i < WORKERS; i++)
    {
        if (worker_stack[i] != NULL && loops[i] == 0)
            break;
    }
    check("every worker thread ran", i == WORKERS);

    /* ---- the refusal contract, from an ADOPTED caller -------------------- */

    /*
     * Three worker threads are still alive.  Stop must refuse, must not take
     * the kernel down, and must not orphan us on the way out -- a refusal that
     * left the caller half-detached would be worse than no refusal at all.
     */
    status = tx_amiga_kernel_stop();
    check("stop refuses while application threads exist (TX_THREAD_ERROR)",
          status == TX_THREAD_ERROR);
    check("the refusal left us adopted", tx_amiga_adopted_thread() == &self);
    check("the refusal left the kernel running",
          tx_amiga_kernel_running() == TX_TRUE);
    check("the kernel still works after the refusal", clock_ran(20));
    check("the tick Task survived the refusal",
          FindTask((STRPTR)"ThreadX tick") != NULL);

    /* ---- now do it properly, still adopted ------------------------------- */

    work_stop();

    zombies = tx_amiga_zombie_tasks_live();
    checkv("no zombie Tasks are outstanding", zombies == 0, (LONG)zombies);

    /*
     * Called while still adopted, which is netstack_shutdown()'s position
     * exactly.  Stop has to orphan us itself.
     */
    status = tx_amiga_kernel_stop();
    check("stop from an adopted caller returned TX_SUCCESS", status == TX_SUCCESS);
    check("stop orphaned the caller", tx_amiga_adopted_thread() == TX_NULL);
    check("kernel no longer reports running",
          tx_amiga_kernel_running() != TX_TRUE);

    /* The two assertions the whole exercise is for. */
    check("the tick Task is GONE", FindTask((STRPTR)"ThreadX tick") == NULL);
    check("the scheduler Task is GONE", FindTask((STRPTR)"ThreadX") == NULL);

    check("adoption is refused once the kernel is down",
          tx_amiga_adopt_thread(&self, "kstop-main", 16) == TX_NOT_DONE);
    check("stop is idempotent", tx_amiga_kernel_stop() == TX_SUCCESS);

    /* ---- lifetime 2: restart -------------------------------------------- */

    beats  = 0;
    status = tx_amiga_kernel_start();
    check("the kernel RESTARTED after a stop", status == TX_SUCCESS);

    if (status == TX_SUCCESS)
    {
        check("the restarted kernel reports running",
              tx_amiga_kernel_running() == TX_TRUE);
        check("the restarted tick Task exists",
              FindTask((STRPTR)"ThreadX tick") != NULL);

        status = tx_amiga_adopt_thread(&self, "kstop-main-2", 16);
        check("adopted on the restarted kernel", status == TX_SUCCESS);

        work_start();
        check("the restarted clock advances", clock_ran(50));
        checkv("the restarted application timer fired", beats > 0, (LONG)beats);
        work_stop();

        (VOID)tx_amiga_orphan_thread(&self);

        /* ---- the zombie contract ---------------------------------------- */

        /*
         * The one case stop can never make safe by itself.  Run from a plain
         * Exec Task (orphaned just above), exactly like netstack_shutdown().
         */
        {
            APTR         sstack;
            struct Task *stask;
            ULONG        spin;

            sstack = ami_alloc(STACK_BYTES);
            status = (sstack != NULL)
                         ? tx_thread_create(&stuck, "kstop-stuck", stuck_entry, 0,
                                            sstack, STACK_BYTES, 12, 12,
                                            TX_NO_TIME_SLICE, TX_AUTO_START)
                         : TX_NO_MEMORY;
            check("a thread that blocks in Exec was created", status == TX_SUCCESS);

            for (spin = 0; spin < 500 && stuck_running == 0; spin++)
                Delay(2);
            check("it is blocked inside Exec, not on its run signal",
                  stuck_running != 0);

            stask = (struct Task *)stuck.tx_thread_amiga_task;

            (VOID)tx_thread_terminate(&stuck);
            (VOID)tx_thread_delete(&stuck);
            checkv("deleting it produced a live zombie",
                   tx_amiga_zombie_tasks_live() == 1,
                   (LONG)tx_amiga_zombie_tasks_live());

            check("stop REFUSES while a zombie is outstanding",
                  tx_amiga_kernel_stop() == TX_THREAD_ERROR);
            check("the refusal left the kernel running",
                  tx_amiga_kernel_running() == TX_TRUE);

            /* Let it go, and watch the live count come back down. */
            stuck_release = 1;
            if (stask != NULL)
                Signal(stask, SIGBREAKF_CTRL_F);
            for (spin = 0; spin < 500 && tx_amiga_zombie_tasks_live() != 0; spin++)
                Delay(2);
            check("the zombie unblocked", stuck_gone != 0);
            checkv("the live zombie count came back to zero",
                   tx_amiga_zombie_tasks_live() == 0,
                   (LONG)tx_amiga_zombie_tasks_live());
            checkv("but the cumulative count remembers it",
                   tx_amiga_zombie_tasks() == 1, (LONG)tx_amiga_zombie_tasks());

            Delay(25);                  /* only now is its stack dead */
            if (sstack != NULL)
                ami_free(sstack);
        }

        status = tx_amiga_kernel_stop();
        check("the restarted kernel stopped again", status == TX_SUCCESS);
        check("no tick Task after the second stop",
              FindTask((STRPTR)"ThreadX tick") == NULL);
        check("no scheduler Task after the second stop",
              FindTask((STRPTR)"ThreadX") == NULL);
    }

    zombies = tx_amiga_zombie_tasks_live();
    checkv("no zombie is left outstanding at exit", zombies == 0, (LONG)zombies);

    /* ---- did stop give the memory back? --------------------------------- */

    /*
     * Two full kernel lifetimes: two 8 KB scheduler stacks, two 4 KB tick
     * stacks, two 32 KB kernel memory blocks, plus a Task and a MemList for
     * every thread.  If stop leaked any of it, it shows up here.
     */
    mem_after = AvailMem(MEMF_PUBLIC);
    checkv("public memory came back (bytes still out)",
           (mem_before <= mem_after) || ((mem_before - mem_after) < 8192UL),
           (LONG)(mem_before - mem_after));

    Printf((CONST_STRPTR)"child: %ld checks, %ld failure(s)\n", checks, failures);
    AMI_ERROR("=== KernelStop CHILD done: %ld checks, %ld failures",
              checks, failures);

    ami_crash_remove_alert_hook();
    ami_crash_remove();

    /* And now the interesting part: this returns, AmigaDOS UnLoadSeg()s
       everything above, and the parent finds out whether that was safe. */
    return failures == 0 ? RETURN_OK : RETURN_ERROR;
}


/* ------------------------------------------------------------------------ */
/* parent                                                                    */
/* ------------------------------------------------------------------------ */

/*
 * Poison free memory with ILLEGAL (0x4AFC).
 *
 * The child's code hunk has just been freed, so it is back on the free list and
 * these allocations very probably land on top of it.  Any Task the port failed
 * to reap is parked with its return address inside that hunk; the next time it
 * runs it executes this instead, and takes an illegal-instruction exception
 * that ami_crash_install() catches and prints -- instead of running whatever
 * stale bytes happened to survive, which might do nothing at all and let a real
 * bug pass as a PASS.
 *
 * Chunks rather than one big block, and MEMF_PUBLIC only, so a machine short of
 * memory still gets most of the effect.
 */
#define POISON_CHUNKS   8
#define POISON_BYTES    (64UL * 1024UL)

static APTR poison[POISON_CHUNKS];

static void poison_free_memory(void)
{
    ULONG i, w;

    for (i = 0; i < POISON_CHUNKS; i++)
    {
        poison[i] = AllocMem(POISON_BYTES, MEMF_PUBLIC);
        if (poison[i] == NULL)
            continue;
        for (w = 0; w < POISON_BYTES / 2UL; w++)
            ((UWORD *)poison[i])[w] = 0x4AFC;    /* ILLEGAL */
    }
}

static void poison_release(void)
{
    ULONG i;

    for (i = 0; i < POISON_CHUNKS; i++)
    {
        if (poison[i] != NULL)
        {
            FreeMem(poison[i], POISON_BYTES);
            poison[i] = NULL;
        }
    }
}

static int parent_main(void)
{
    struct Process *me = (struct Process *)FindTask(NULL);
    APTR            old_window;
    BPTR            fh;
    LONG            rc;
    ULONG           mem_before, mem_after;

    Printf((CONST_STRPTR)"AmiNetXDuo -- tx_amiga_kernel_stop() lifecycle\n");
    AMI_ERROR("=== KernelStop PARENT, hunk at %08lx", (LONG)parent_main);

    ami_crash_set_reference((APTR)parent_main, "parent_main");
    check("crash guard installed", ami_crash_install());
    check("Alert (Guru) hook installed", ami_crash_install_alert_hook());

    /* Nobody is at the keyboard to answer a requester. */
    old_window       = me->pr_WindowPtr;
    me->pr_WindowPtr = (APTR)-1;

    /*
     * Runaway guard.  If the child ever fails to recognise itself it runs this
     * function instead, System()s another copy, and the emulator dies of
     * recursion rather than of anything to do with the kernel -- which is
     * exactly how the first version of this test failed.  One lock file turns
     * that into an immediate, legible error.
     */
    fh = Open((CONST_STRPTR)"DH0:kstop.lock", MODE_OLDFILE);
    if (fh != (BPTR)0)
    {
        Close(fh);
        PutStr((CONST_STRPTR)"KernelStop: ran as parent twice -- the child did "
                             "not recognise itself\n");
        AMI_ERROR("KernelStop: recursion guard tripped");
        return RETURN_FAIL;
    }
    fh = Open((CONST_STRPTR)"DH0:kstop.lock", MODE_NEWFILE);
    if (fh != (BPTR)0)
        Close(fh);

    fh = Open((CONST_STRPTR)"DH0:kstop.txt", MODE_NEWFILE);
    if (fh != (BPTR)0)
        Close(fh);

    mem_before = AvailMem(MEMF_PUBLIC);

    /* A SECOND COPY of this program, in its own hunk, which AmigaDOS unloads
       when it returns. */
    rc = SystemTags((CONST_STRPTR)CHILD_COMMAND, TAG_DONE);
    me->pr_WindowPtr = old_window;

    checkv("the child ran and exited normally", rc == RETURN_OK, rc);

    /* ---- the child's hunk is now freed.  Is anything still pointing at it? */

    check("no ThreadX tick Task outlived the child",
          FindTask((STRPTR)"ThreadX tick") == NULL);
    check("no ThreadX scheduler Task outlived the child",
          FindTask((STRPTR)"ThreadX") == NULL);
    check("no ThreadX System Timer Task outlived the child",
          FindTask((STRPTR)"System Timer Thread") == NULL);

    mem_after = AvailMem(MEMF_PUBLIC);
    checkv("the child gave its memory back (bytes still out)",
           (mem_before <= mem_after) || ((mem_before - mem_after) < 8192UL),
           (LONG)(mem_before - mem_after));

    /* ---- survive the child ---------------------------------------------- */

    Printf((CONST_STRPTR)"  .. holding the machine open for %ld ticks with freed memory "
           "poisoned\n", (LONG)SURVIVE_TICKS);
    AMI_ERROR("parent: poisoning freed memory and waiting %ld ticks",
              (LONG)SURVIVE_TICKS);

    poison_free_memory();
    Delay(SURVIVE_TICKS);
    poison_release();

    check("the machine is still alive well after the child unloaded", TRUE);

    Printf((CONST_STRPTR)"\n%ld checks, %ld failure(s) -- %s\n", checks, failures,
           (LONG)(failures == 0 ? "PASS" : "FAIL"));
    AMI_ERROR("=== KernelStop: %ld checks, %ld failures -- %s",
              checks, failures, (LONG)(failures == 0 ? "PASS" : "FAIL"));

    ami_crash_remove_alert_hook();
    ami_crash_remove();

    return failures == 0 ? RETURN_OK : RETURN_ERROR;
}


/*
 * Which half are we?
 *
 * NOT argc/argv: this program links dos.library directly rather than a C
 * runtime that parses a command line, and a System()ed copy arrives with
 * argc == 1 exactly like the Startup-Sequence's copy does -- which made the
 * first version of this test fork-bomb the emulator until it ran out of RAM.
 * GetArgStr() asks dos.library for the Shell argument string itself and is
 * true regardless of what the startup code did or did not do.
 */
static BOOL is_child(void)
{
    CONST_STRPTR args = (CONST_STRPTR)GetArgStr();

    return (args != NULL) && (args[0] == 'c') && (args[1] == 'h');
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    if (is_child())
        return child_main();

    return parent_main();
}

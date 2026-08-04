/*
 * Isolate the AN_FreeTwice (0x01000009) double free.
 *
 * Exercises the ThreadX thread lifecycle on Exec with the Alert hook armed, so
 * a double free names the offending task instead of just showing a Guru:
 *   - ThreadX-created threads that run to completion and are deleted
 *   - threads deleted while still suspended
 *   - adopt/orphan churn on the calling task
 *   - tx_thread_wait_abort() on an ADOPTED thread parked inside ThreadX,
 *     which is the mechanism bsdsocket.library's WaitSelect() needs in order
 *     to honour an Exec break signal
 *   - tx_thread_delete() of a thread that is blocked in Exec rather than
 *     parked on its run signal, i.e. a task the port cannot wake
 *
 * Deliberately small and self-contained: it links the port and ThreadX but
 * nothing else, so anything it catches belongs to the port's task lifecycle
 * rather than to the netstack, the SANA-II shim or the socket library.
 *
 * SPDX-License-Identifier: MIT
 */

/* tx_api.h FIRST: exec/types.h does #define VOID void, which collides with
   tx_port.h's typedef void VOID if the Amiga headers land first. */
#include "tx_api.h"
#include "tx_amiga.h"

#include <exec/memory.h>
#include <exec/tasks.h>
#include <devices/timer.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include "aminetxduo/compat.h"
#include "aminetxduo/crashguard.h"

#ifndef ROUNDS
#define ROUNDS          8
#endif
#define WORKERS         3
#define STACK_BYTES     4096

static TX_THREAD    worker[WORKERS];
static TX_THREAD    self;
static APTR         worker_stack[WORKERS];
static TX_SEMAPHORE never;
static ULONG        ran[WORKERS];
static LONG         checks, failures;

/* wait-abort phase */
static TX_THREAD        victim_thread;
static volatile ULONG   victim_ready;
static volatile ULONG   victim_done;
static volatile ULONG   victim_gone;
static volatile UINT    victim_status;
static volatile UINT    victim_adopted;

/* unwakeable-thread phase */
static TX_THREAD        stuck_thread;
static volatile ULONG   stuck_running;
static volatile ULONG   stuck_release;
static volatile ULONG   stuck_gone;

static void check(const char *what, BOOL ok)
{
    checks++;
    if (!ok)
        failures++;
    Printf((STRPTR)"  %s %s\n", (LONG)(ok ? "ok  " : "FAIL"), (LONG)what);
    AMI_ERROR("  %s %s", (LONG)(ok ? "ok  " : "FAIL"), (LONG)what);
}

/* Runs to completion, so ThreadX marks it TX_COMPLETED and the port reaps it. */
static VOID worker_exits(ULONG id)
{
    ran[id] = 1;
}

/* Never returns; deleted from underneath while suspended. */
static VOID worker_blocks(ULONG id)
{
    ran[id] = 1;
    (VOID)tx_semaphore_get(&never, TX_WAIT_FOREVER);
}

/* ------------------------------------------------------------------------ */
/* A raw Exec Task, created the way the port creates them: the MemList lives  */
/* in its OWN allocation, because RemTask()'s FreeEntry() frees the MemList   */
/* structure as well as the entries it describes.                            */
/* ------------------------------------------------------------------------ */

static struct Task *spawn_task(const char *name, VOID (*entry)(VOID),
                               APTR stack, ULONG stack_size)
{
    struct MemList *ml;
    struct Task    *task;

    ml = (struct MemList *)AllocMem(sizeof(struct MemList), MEMF_PUBLIC | MEMF_CLEAR);
    if (ml == NULL)
        return NULL;

    task = (struct Task *)AllocMem(sizeof(struct Task), MEMF_PUBLIC | MEMF_CLEAR);
    if (task == NULL)
    {
        FreeMem(ml, sizeof(struct MemList));
        return NULL;
    }

    ml->ml_NumEntries      = 1;
    ml->ml_ME[0].me_Addr   = (APTR)task;
    ml->ml_ME[0].me_Length = sizeof(struct Task);

    task->tc_Node.ln_Type = NT_TASK;
    task->tc_Node.ln_Pri  = 0;
    task->tc_Node.ln_Name = (char *)name;
    task->tc_SPLower      = stack;
    task->tc_SPUpper      = (APTR)((UBYTE *)stack + stack_size);
    task->tc_SPReg        = task->tc_SPUpper;

    task->tc_MemEntry.lh_Head     = (struct Node *)&task->tc_MemEntry.lh_Tail;
    task->tc_MemEntry.lh_Tail     = NULL;
    task->tc_MemEntry.lh_TailPred = (struct Node *)&task->tc_MemEntry.lh_Head;
    AddTail(&task->tc_MemEntry, (struct Node *)ml);

    if (AddTask(task, (APTR)entry, NULL) == NULL)
    {
        FreeMem(task, sizeof(struct Task));
        FreeMem(ml, sizeof(struct MemList));
        return NULL;
    }
    return task;
}


/*
 * Adopts itself, parks inside ThreadX forever, and expects main() to break it
 * out with tx_thread_wait_abort().
 */
static VOID victim_entry(VOID)
{
    if (tx_amiga_adopt_thread(&victim_thread, "victim", 12) != TX_SUCCESS)
    {
        victim_adopted = 0;
        victim_ready   = 1;
        victim_done    = 1;
        victim_gone    = 1;
        Forbid();
        RemTask(NULL);
    }

    victim_adopted = 1;
    victim_ready   = 1;

    victim_status = tx_semaphore_get(&never, TX_WAIT_FOREVER);
    victim_done   = 1;

    (VOID)tx_amiga_orphan_thread(&victim_thread);

    victim_gone = 1;
    Forbid();
    RemTask(NULL);
}


/*
 * A ThreadX-created thread that blocks in Exec (not on its ThreadX run
 * signal), exactly like a SANA-II reader parked in WaitIO() on a device that
 * ignores AbortIO().  The port cannot wake it, so tx_thread_delete() must not
 * hang the caller forever.
 */
static VOID stuck_worker(ULONG id)
{
    (VOID)id;
    stuck_running = 1;
    while (stuck_release == 0)
        (VOID)Wait(SIGBREAKF_CTRL_F | SIGBREAKF_CTRL_E);
    stuck_gone = 1;
}


VOID tx_application_define(VOID *first_unused)
{
    (VOID)first_unused;
    (VOID)tx_semaphore_create(&never, "never", 0);
}


int main(void)
{
    UINT  status;
    ULONG round;
    int   i;

    Printf((STRPTR)"AmiNetXDuo, ThreadX task lifecycle probe\n");

    ami_crash_set_reference((APTR)main, "main");
    (VOID)ami_crash_install();
    check("Alert (Guru) hook installed", ami_crash_install_alert_hook());

    status = tx_amiga_kernel_start();
    check("ThreadX kernel started", status == TX_SUCCESS);
    if (status != TX_SUCCESS)
        goto done;

    status = tx_amiga_adopt_thread(&self, "lifecycle-main", 16);
    check("adopted the calling task", status == TX_SUCCESS);

    for (round = 0; round < ROUNDS; round++)
    {
        Printf((STRPTR)"round %ld: create/exit/delete\n", (LONG)round);
        AMI_ERROR("=== round %ld phase a (exiters)", (LONG)round);

        /* (a) threads that run to completion, then get deleted. */
        for (i = 0; i < WORKERS; i++)
        {
            ran[i] = 0;
            worker_stack[i] = ami_alloc(STACK_BYTES);
            if (worker_stack[i] == NULL)
                continue;

            (VOID)tx_thread_create(&worker[i], "exiter", worker_exits, (ULONG)i,
                                   worker_stack[i], STACK_BYTES, 12, 12,
                                   TX_NO_TIME_SLICE, TX_AUTO_START);
        }

        (VOID)tx_thread_sleep(20);

        for (i = 0; i < WORKERS; i++)
        {
            if (worker_stack[i] == NULL)
                continue;
            (VOID)tx_thread_terminate(&worker[i]);
            (VOID)tx_thread_delete(&worker[i]);
            ami_free(worker_stack[i]);      /* our stack, ours to free */
            worker_stack[i] = NULL;
        }

        AMI_ERROR("=== round %ld phase b (blockers)", (LONG)round);

        /* (b) threads deleted while parked on a semaphore nobody posts. */
        for (i = 0; i < WORKERS; i++)
        {
            ran[i] = 0;
            worker_stack[i] = ami_alloc(STACK_BYTES);
            if (worker_stack[i] == NULL)
                continue;

            (VOID)tx_thread_create(&worker[i], "blocker", worker_blocks, (ULONG)i,
                                   worker_stack[i], STACK_BYTES, 12, 12,
                                   TX_NO_TIME_SLICE, TX_AUTO_START);
        }

        (VOID)tx_thread_sleep(20);

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

        AMI_ERROR("=== round %ld phase c (adopt churn)", (LONG)round);

        /* (c) adopt/orphan churn. */
        (VOID)tx_amiga_orphan_thread(&self);
        (VOID)tx_amiga_adopt_thread(&self, "lifecycle-main", 16);
    }

    check("survived the lifecycle rounds", TRUE);

    /* ---------------------------------------------------------------- (d) */
    /* tx_thread_wait_abort() on an ADOPTED thread parked inside ThreadX.    */

    AMI_ERROR("=== phase d (wait-abort on an adopted thread)");
    {
        APTR         vstack;
        struct Task *vtask;
        ULONG        spin;
        CHAR        *nm;
        UINT         st = 0, pr, th;
        ULONG        rc;
        ULONG        sl;
        TX_THREAD   *nt;
        TX_THREAD   *ns;

        vstack = ami_alloc(STACK_BYTES);
        vtask  = (vstack != NULL) ? spawn_task("victim-task", victim_entry,
                                               vstack, STACK_BYTES)
                                  : NULL;
        check("phase d: victim Exec Task created", vtask != NULL);

        if (vtask != NULL)
        {
            for (spin = 0; spin < 400 && victim_ready == 0; spin++)
                (VOID)tx_thread_sleep(1);
            check("phase d: victim adopted itself", victim_adopted != 0);

            /* Wait until it really is suspended in ThreadX, not just about to
               be, or the abort would race the suspend and prove nothing. */
            for (spin = 0; spin < 400; spin++)
            {
                if (tx_thread_info_get(&victim_thread, &nm, &st, &rc, &pr, &th,
                                       &sl, &nt, &ns) != TX_SUCCESS)
                    break;
                if (st == TX_SEMAPHORE_SUSP)
                    break;
                (VOID)tx_thread_sleep(1);
            }
            check("phase d: victim suspended on the semaphore",
                  st == TX_SEMAPHORE_SUSP);

            status = tx_thread_wait_abort(&victim_thread);
            check("phase d: tx_thread_wait_abort accepted", status == TX_SUCCESS);

            for (spin = 0; spin < 400 && victim_done == 0; spin++)
                (VOID)tx_thread_sleep(1);
            check("phase d: adopted thread woke up", victim_done != 0);
            check("phase d: it saw TX_WAIT_ABORTED",
                  victim_status == TX_WAIT_ABORTED);

            for (spin = 0; spin < 400 && victim_gone == 0; spin++)
                (VOID)tx_thread_sleep(1);
            check("phase d: victim orphaned and exited", victim_gone != 0);
            (VOID)tx_thread_sleep(20);
        }
        if (vstack != NULL)
            ami_free(vstack);
    }

    (VOID)tx_amiga_orphan_thread(&self);

    /* ---------------------------------------------------------------- (e) */
    /* tx_thread_delete() of a thread that is blocked in Exec, not on its    */
    /* run signal, the SANA-II reader stuck in WaitIO() case.  Run as a    */
    /* plain Exec task (orphaned above), exactly like netstack_shutdown().   */

    AMI_ERROR("=== phase e (deleting a thread blocked in Exec)");
    {
        APTR   sstack;
        ULONG  spin;
        ULONG  zombies_before;
        ULONG  t0, t1;
        struct Task *stask;

        sstack = ami_alloc(STACK_BYTES);
        status = (sstack != NULL)
                     ? tx_thread_create(&stuck_thread, "stuck", stuck_worker, 0,
                                        sstack, STACK_BYTES, 12, 12,
                                        TX_NO_TIME_SLICE, TX_AUTO_START)
                     : TX_NO_MEMORY;
        check("phase e: unwakeable thread created", status == TX_SUCCESS);

        for (spin = 0; spin < 500 && stuck_running == 0; spin++)
            Delay(2);
        check("phase e: it is blocked inside Exec", stuck_running != 0);

        stask = (struct Task *)stuck_thread.tx_thread_amiga_task;
        zombies_before = tx_amiga_zombie_tasks();

        t0 = ami_millis();
        (VOID)tx_thread_terminate(&stuck_thread);
        (VOID)tx_thread_delete(&stuck_thread);
        t1 = ami_millis();

        AMI_ERROR("phase e: terminate+delete took %ld ms, zombies %ld -> %ld",
                  (LONG)(t1 - t0), (LONG)zombies_before,
                  (LONG)tx_amiga_zombie_tasks());

        check("phase e: tx_thread_delete returned (did not hang)", TRUE);
        check("phase e: it returned in bounded time", (t1 - t0) < 10000UL);
        check("phase e: the task was reported as a zombie",
              tx_amiga_zombie_tasks() == zombies_before + 1UL);

        /* The rest of ThreadX must still work: the baton has to have been
           recovered from the zombie. */
        status = tx_amiga_adopt_thread(&self, "lifecycle-main", 16);
        check("phase e: ThreadX still alive (re-adopted)", status == TX_SUCCESS);
        (VOID)tx_thread_sleep(5);
        (VOID)tx_amiga_orphan_thread(&self);

        /* Now let the zombie go, and check it destroys itself cleanly. */
        stuck_release = 1;
        if (stask != NULL)
            Signal(stask, SIGBREAKF_CTRL_F);
        for (spin = 0; spin < 500 && stuck_gone == 0; spin++)
            Delay(2);
        check("phase e: the zombie unblocked", stuck_gone != 0);
        Delay(25);
        /* Its stack is only safe to free once it is really gone. */
        if (sstack != NULL)
            ami_free(sstack);
    }

done:
    Printf((STRPTR)"\n%ld checks, %ld failure(s)\n", checks, failures);
    ami_crash_remove_alert_hook();
    ami_crash_remove();
    return failures == 0 ? RETURN_OK : RETURN_ERROR;
}

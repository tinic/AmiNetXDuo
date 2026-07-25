/*
 * Isolate the AN_FreeTwice (0x01000009) double free.
 *
 * Exercises the ThreadX thread lifecycle on Exec with the Alert hook armed, so
 * a double free names the offending task instead of just showing a Guru:
 *   - ThreadX-created threads that run to completion and are deleted
 *   - threads deleted while still suspended
 *   - adopt/orphan churn on the calling task
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

#include <proto/exec.h>
#include <proto/dos.h>

#include "aminetxduo/compat.h"
#include "aminetxduo/crashguard.h"

#define ROUNDS          8
#define WORKERS         3
#define STACK_BYTES     4096

static TX_THREAD    worker[WORKERS];
static APTR         worker_stack[WORKERS];
static TX_SEMAPHORE never;
static ULONG        ran[WORKERS];
static LONG         checks, failures;

static void check(const char *what, BOOL ok)
{
    checks++;
    if (!ok)
        failures++;
    Printf("  %s %s\n", (LONG)(ok ? "ok  " : "FAIL"), (LONG)what);
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

    Printf("AmiNetXDuo -- ThreadX task lifecycle probe\n");

    ami_crash_set_reference((APTR)main, "main");
    (VOID)ami_crash_install();
    check("Alert (Guru) hook installed", ami_crash_install_alert_hook());

    status = tx_amiga_kernel_start();
    check("ThreadX kernel started", status == TX_SUCCESS);
    if (status != TX_SUCCESS)
        goto done;

    status = tx_amiga_adopt_thread(NULL, "lifecycle-main", 16);
    check("adopted the calling task", status == TX_SUCCESS);

    for (round = 0; round < ROUNDS; round++)
    {
        Printf("round %ld: create/exit/delete\n", (LONG)round);

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

        /* (c) adopt/orphan churn. */
        (VOID)tx_amiga_orphan_thread(NULL);
        (VOID)tx_amiga_adopt_thread(NULL, "lifecycle-main", 16);
    }

    check("survived the lifecycle rounds", TRUE);
    (VOID)tx_amiga_orphan_thread(NULL);

done:
    Printf("\n%ld checks, %ld failure(s)\n", checks, failures);
    ami_crash_remove_alert_hook();
    ami_crash_remove();
    return failures == 0 ? RETURN_OK : RETURN_ERROR;
}

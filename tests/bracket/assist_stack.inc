/* The production m68k trampoline, on real Exec/ThreadX stacks. Included
 * after bracket_test.c's task helpers so this remains one native harness.
 * The small ownership wrapper here only drives the primitive; production
 * opener lifetime/retirement is covered by rx_assist_ownership on the host.
 * No SANA-II driver, receive throughput or audio is exercised here.
 * SPDX-License-Identifier: MIT */
#include "aminetxduo/asm_abi.h"
#define ASSIST_BYTES 8192UL
#define ASSIST_GUARD 64UL
typedef AMIGA_ASM_ARGS ULONG (*AssistWork)(APTR);
extern AMIGA_ASM_ARGS ULONG bsd_rx_stack_call(struct StackSwapStruct *, AssistWork, APTR);
extern UINT tx_amiga_adopted_task_dead(TX_THREAD *);

typedef struct AssistContext
{
    struct StackSwapStruct swap;
    UBYTE *allocation;
    struct Task *owner;
    volatile UWORD busy;
} AssistContext;

static AssistContext assist_contexts[2];
static BtTask assist_workers[2];
static volatile UWORD assist_resumed[2];

/* A test owner keeps each work stack claimed across the scheduled handoff
 * below. The real opener guard has a separate host lifetime/retirement test. */
static BOOL assist_try(AssistContext *ctx, AssistWork work, APTR arg,
                       ULONG *result)
{
    Forbid();
    if (ctx->allocation == NULL || ctx->busy ||
        ctx->owner != FindTask(NULL))
    {
        Permit();
        return FALSE;
    }
    ctx->busy = 1;
    Permit();
    *result = bsd_rx_stack_call(&ctx->swap, work, arg);
    Forbid();
    ctx->busy = 0;
    Permit();
    return TRUE;
}

static AMIGA_ASM_ARGS ULONG assist_work(APTR arg)
{
    BtTask *bt = (BtTask *)arg;
    UWORD index = bt == &assist_workers[0] ? 0 : 1;
    AssistContext *ctx = &assist_contexts[index];
    struct Task *me = FindTask(NULL);
    volatile UBYTE frame[1536];
    ULONG nested = 0x12345678UL;
    UWORD i;

    if (me->tc_SPLower != ctx->allocation + ASSIST_GUARD ||
        (ULONG)me->tc_SPUpper - (ULONG)me->tc_SPLower != ASSIST_BYTES ||
        (ULONG)&frame[0] < (ULONG)me->tc_SPLower ||
        (ULONG)&frame[sizeof(frame) - 1] >= (ULONG)me->tc_SPUpper)
        bt->bt_Failures++;
    for (i = 0; i < sizeof(frame); i++)
        frame[i] = (UBYTE)(i ^ index);
    if (assist_try(ctx, assist_work, arg, &nested) || nested != 0x12345678UL)
        bt->bt_Failures++;

    /* Deliberately yield in the primitive test even though RX assistance
       must use a nonblocking lock. The alternate stack cannot be reused by
       another call if a lower-level routine ever hands the baton away. */
    ami_netstack_baton_release();
    bt->bt_Ready = 1;
    Wait(BT_SIG_ACQUIRE);
    ami_netstack_baton_acquire();
    if (!tx_amiga_caller_is_thread() || tx_amiga_adopted_task_dead(bt->bt_Thread))
        bt->bt_Failures++;
    for (i = 0; i < sizeof(frame); i++)
        if (frame[i] != (UBYTE)(i ^ index))
        {
            bt->bt_Failures++;
            break;
        }
    assist_resumed[index] = 1;
    return 0xdeadbeefUL + index;
}

static AMIGA_ASM_ARGS ULONG assist_quick(APTR arg)
{
    AssistContext *ctx = (AssistContext *)arg;
    struct Task *me = FindTask(NULL);
    volatile ULONG local = 0x76543210UL;

    return (me->tc_SPLower == ctx->allocation + ASSIST_GUARD &&
            (ULONG)&local >= (ULONG)me->tc_SPLower &&
            (ULONG)&local + sizeof(local) <= (ULONG)me->tc_SPUpper)
               ? local : 0;
}

static VOID assist_worker_entry(VOID)
{
    struct Task *me = FindTask(NULL);
    BtTask *bt = (BtTask *)me->tc_UserData;
    UWORD index = bt == &assist_workers[0] ? 0 : 1;
    AssistContext *ctx = &assist_contexts[index];
    APTR lower = me->tc_SPLower, upper = me->tc_SPUpper;
    ULONG result = 0;
    ULONG marker = 0xcafebabeUL;
    BYTE priority = me->tc_Node.ln_Pri;
    UWORD round;

    Wait(BT_SIG_GO);
    if ((ULONG)upper - (ULONG)lower != 4096UL ||
        tx_amiga_adopt_thread(&bt->bt_Thread, &bt->bt_Gen,
                             (CHAR *)"assist stack", 20, TX_FALSE) != TX_SUCCESS)
    {
        bt->bt_Failures++;
        bt_finish(bt);
        return;
    }
    ctx->owner = me;
    if (!assist_try(ctx, assist_work, bt, &result) ||
        result != 0xdeadbeefUL + index || marker != 0xcafebabeUL ||
        me->tc_SPLower != lower || me->tc_SPUpper != upper ||
        me->tc_Node.ln_Pri != priority || ctx->busy)
        bt->bt_Failures++;
    for (round = 0; round < 128; round++)
        if (!assist_try(ctx, assist_quick, ctx, &result) ||
            result != 0x76543210UL || me->tc_SPLower != lower ||
            me->tc_SPUpper != upper || ctx->busy)
        {
            bt->bt_Failures++;
            break;
        }
    if (tx_amiga_orphan_thread(bt->bt_Thread, bt->bt_Gen) != TX_SUCCESS)
        bt->bt_Failures++;
    bt->bt_Rounds = 1;
    bt_finish(bt);
}

static VOID bt_test_assist_stack(VOID)
{
    AssistContext absent = {0};
    ULONG unchanged = 0x10203040UL;
    UWORD i, j, spawned = 0;
    BOOL guards_ok = TRUE;
    BOOL ready = FALSE;

    t_log("ASSIST_STACK: two 4KiB callers, separate 8KiB work stacks\n", 0, 0);
    t_check(!assist_try(&absent, assist_work, NULL, &unchanged) &&
            unchanged == 0x10203040UL,
            "missing optional stack skips without changing the result", 0);
    /* Allocate both work stacks before starting any worker, so allocation
       failure has no suspended alternate-stack owner to unwind. */
    for (i = 0; i < 2; i++)
    {
        AssistContext *ctx = &assist_contexts[i];
        ctx->allocation = AllocMem(ASSIST_BYTES + 2 * ASSIST_GUARD,
                                   MEMF_PUBLIC | MEMF_CLEAR);
        t_check(ctx->allocation != NULL, "allocated reserved work stack", i);
        if (ctx->allocation == NULL)
            goto cleanup;
        for (j = 0; j < ASSIST_GUARD; j++)
        {
            ctx->allocation[j] = 0xa5;
            ctx->allocation[ASSIST_GUARD + ASSIST_BYTES + j] = 0x5a;
        }
        ctx->swap.stk_Lower = ctx->allocation + ASSIST_GUARD;
        ctx->swap.stk_Upper = (ULONG)ctx->swap.stk_Lower + ASSIST_BYTES;
        ctx->swap.stk_Pointer = (APTR)ctx->swap.stk_Upper;
    }
    for (i = 0; i < 2; i++)
    {
        assist_workers[i].bt_Parent = FindTask(NULL);
        t_check(bt_spawn(&assist_workers[i], assist_worker_entry,
                         "assist-stack", 0) != NULL,
                "created a 4KiB caller", i);
        if (assist_workers[i].bt_Task == NULL)
            goto cleanup;
        spawned++;
    }
    for (i = 0; i < 2; i++)
    {
        AssistContext *ctx = &assist_contexts[i];
        Signal(assist_workers[i].bt_Task, BT_SIG_GO);
        bt_wait_for(&assist_workers[i].bt_Ready,
                    "caller suspended while on its reserved stack");
        if (!assist_workers[i].bt_Ready)
            goto cleanup;
        t_check(ctx->busy && !tx_amiga_adopted_task_dead(assist_workers[i].bt_Thread),
                "swapped live caller remains alive across ThreadX release", i);
        t_check(!assist_try(ctx, assist_work, NULL, &unchanged) &&
                unchanged == 0x10203040UL,
                "another task cannot reuse an active reserved stack", i);
    }
    t_check(assist_contexts[0].busy && assist_contexts[1].busy,
            "two independent reserved stacks can be active concurrently", 0);
    ready = TRUE;

cleanup:
    /* Also unwind an earlier worker if creation of the second failed.
       Signal both gates: that worker may still be waiting for its first GO.
       Do not signal a stale Task pointer after bt_finish removed it. */
    for (i = 0; i < spawned; i++)
    {
        Forbid();
        if (!assist_workers[i].bt_Done)
            Signal(assist_workers[i].bt_Task, BT_SIG_GO | BT_SIG_ACQUIRE);
        Permit();
        bt_wait_for(&assist_workers[i].bt_Done, "reserved stack caller finished");
        if (ready)
            t_check(assist_resumed[i] && assist_workers[i].bt_Rounds == 1 &&
                    assist_workers[i].bt_Failures == 0,
                    "handoff, reentry guard, repeats, frame/result/bounds/priority restoration",
                    assist_workers[i].bt_Failures);
    }
    for (i = 0; i < 2; i++)
    {
        AssistContext *ctx = &assist_contexts[i];
        /* A broken trampoline can strand a live worker. Keep its memory
           intact on timeout; the failing emulator run is the recovery. */
        if (i < spawned && !assist_workers[i].bt_Done)
            continue;
        if (i < spawned)
            bt_reap(&assist_workers[i]);
        if (ctx->allocation == NULL)
            continue;
        for (j = 0; j < ASSIST_GUARD; j++)
            if (ctx->allocation[j] != 0xa5 ||
                ctx->allocation[ASSIST_GUARD + ASSIST_BYTES + j] != 0x5a)
                guards_ok = FALSE;
        FreeMem(ctx->allocation, ASSIST_BYTES + 2 * ASSIST_GUARD);
        ctx->allocation = NULL;
    }
    t_check(guards_ok, "both reserved-stack boundary guards survived", 0);
}

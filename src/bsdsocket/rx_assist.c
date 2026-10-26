/* Optional caller-assisted receive, on a stack owned by this opener.
 * SPDX-License-Identifier: MIT */

#include "bsdsocket_internal.h"
#include "aminetxduo/sana2.h"
#include "aminetxduo/exec_port.h"

struct BsdRxAssist
{
#ifdef __m68k__
    struct StackSwapStruct swap;
#endif
    struct Task *owner;
    BOOL busy;
    BOOL retire;
};

static AMIGA_ASM_ARGS ULONG bsd_rx_work(APTR arg)
{
    return (ULONG)ami_sana2_rx_assist((NX_IP *)arg, 32);
}

#ifdef __m68k__
typedef AMIGA_ASM_ARGS ULONG (*BsdRxWork)(APTR);
extern AMIGA_ASM_ARGS ULONG bsd_rx_stack_call(struct StackSwapStruct *,
                                             BsdRxWork, APTR);
#endif

UWORD bsd_rx_assist(struct AmiSocketBase *base)
{
    struct Task *me = FindTask(NULL);
    struct BsdRxAssist *ctx;
    NX_IP *ip;
    ULONG result;
    BOOL retire;

    /* Nested callbacks and foreign tasks cannot borrow an active stack.
       Allocation is lazy: ordinary blocking users pay no per-opener stack. */
    if (base->sb_NxNest != 1 || base->sb_NxTask != me || base->sb_Task != me)
        return 0;
    ip = netstack_ip();
    if (ip == NULL)
        return 0;

    ctx = base->sb_RxAssist;
    if (ctx == NULL)
    {
        ULONG bytes = ami_sana2_rx_assist_stack_size();

        ctx = ami_alloc_flags((ULONG)sizeof(*ctx) + bytes,
                               MEMF_PUBLIC | MEMF_CLEAR);
        if (ctx == NULL)
            return 0;
        ctx->owner = me;
#ifdef __m68k__
        ctx->swap.stk_Lower = (APTR)(ctx + 1);
        ctx->swap.stk_Upper = (ULONG)(ctx + 1) + bytes;
        ctx->swap.stk_Pointer = (APTR)ctx->swap.stk_Upper;
#endif
        base->sb_RxAssist = ctx;
    }

    Forbid();
    if (ctx->busy || ctx->retire || ctx->owner != me)
    {
        Permit();
        return 0;
    }
    ctx->busy = TRUE;
    Permit();

#ifdef __m68k__
    result = bsd_rx_stack_call(&ctx->swap, bsd_rx_work, ip);
#else
    /* Host tests exercise ownership; the native bracket test exercises the
       actual stack switch. Host stacks do not have the Amiga 4KiB limit. */
    result = bsd_rx_work(ip);
#endif

    Forbid();
    ctx->busy = FALSE;
    retire = ctx->retire;
    Permit();
    if (retire)
        ami_free(ctx);
    return (UWORD)result;
}

VOID bsd_rx_assist_release(struct AmiSocketBase *base)
{
    struct BsdRxAssist *ctx;

    Forbid();
    ctx = base->sb_RxAssist;
    base->sb_RxAssist = NULL;
    if (ctx != NULL && ctx->busy &&
        tx_amiga_exec_task_alive(ctx->owner) != TX_FALSE)
    {
        /* A close during service cannot free the stack under its owner.
           The returning owner releases it after switching back. */
        ctx->retire = TRUE;
        ctx = NULL;
    }
    Permit();
    ami_free(ctx);
}

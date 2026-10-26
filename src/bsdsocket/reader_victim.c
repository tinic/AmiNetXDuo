/* #353 diagnostic: ask only at budget exhaustion.  No retained Task pointer,
 * dispatch hook, opener index, allocation, or wait is needed by this walk.
 * SPDX-License-Identifier: MIT
 */
#ifdef AMINETXDUO_READER_VICTIM_HOST
#include "reader_victim_host.h"
#else
#include "bsdsocket_internal.h"
#include "tx_amiga.h"
#include <proto/exec.h>
#endif

#ifdef V6_GATED
#define BSD_VICTIM_VISITS 64U
#define BSD_VICTIM_WINDOW ((TX_TIMER_TICKS_PER_SECOND + 4UL) / 5UL)

/* checks, yes, capped, internal skipped, recent skipped, maximum visits */
ULONG bsd_reader_victim_stats[6];

BOOL bsd_reader_victim_ready(struct AmiSocketBase **master_slot)
{
    struct AmiSocketBase *master;
    struct Node *node;
    ULONG now;
    UINT left = BSD_VICTIM_VISITS, above = 0, zero = 0;
    BOOL answer = FALSE;

    /* Protect the root load as well as every scheduler/child-list read.
       Keep only the boolean after Enable(), including on the empty path. */
    Disable();
    bsd_reader_victim_stats[0]++;
    master = *master_slot;
    if (master == NULL)
        goto done;
    now = tx_amiga_time_get_locked();
    node = SysBase->TaskReady.lh_Head;
    for (;;)
    {
        struct Task *task;
        struct MinNode *child_node;
        BOOL recent = FALSE;

        if (left == 0)
            goto capped;
        left--;
        if (node == NULL || node->ln_Succ == NULL || node->ln_Pri < 0)
            goto done;
        if (node->ln_Pri > 0)
        {
            if (++above > 8U)
                goto capped;
            node = node->ln_Succ;
            continue;
        }
        if (++zero > 16U)
            goto capped;
        task = (struct Task *)node;  /* fresh in this Disable */
        if (tx_amiga_task_may_be_internal((VOID *)task) != TX_FALSE)
        {
            bsd_reader_victim_stats[3]++;
            node = node->ln_Succ;
            continue;
        }

        child_node = master->sb_Children.mlh_Head;
        for (;;)
        {
            struct AmiSocketBase *child;

            if (left == 0)
                goto capped;
            left--;
            if (child_node == NULL || child_node->mln_Succ == NULL)
                break;
            child = (struct AmiSocketBase *)((UBYTE *)child_node -
                                      offsetof(struct AmiSocketBase, sb_Node));
            /* Stored task addresses are compared, NEVER dereferenced. A
               reused address is conservatively protected for this window. */
            if (child->sb_LastNetTask == task &&
                (ULONG)(now - child->sb_LastNetCall) <= BSD_VICTIM_WINDOW)
            {
                recent = TRUE;
                break;
            }
            child_node = child_node->mln_Succ;
        }
        if (!recent)
        {
            answer = TRUE;
            bsd_reader_victim_stats[1]++;
            goto done;
        }
        bsd_reader_victim_stats[4]++;
        node = node->ln_Succ; /* a caller must not hide a later decoder */
    }
capped:
    bsd_reader_victim_stats[2]++; /* UNKNOWN: retain throughput */
done:
    if (BSD_VICTIM_VISITS - left > bsd_reader_victim_stats[5])
        bsd_reader_victim_stats[5] = BSD_VICTIM_VISITS - left;
    Enable();
    return answer;
}
#endif

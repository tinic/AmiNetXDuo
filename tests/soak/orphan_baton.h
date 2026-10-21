/* The post-orphan observation, also exercised with the real pool on the host.
 * SPDX-License-Identifier: MIT
 */
#ifndef SOAK_ORPHAN_BATON_H
#define SOAK_ORPHAN_BATON_H

#include "tx_api.h"
#include "tx_thread.h"
#include "tx_amiga.h"
#include <proto/exec.h>

extern TX_THREAD *_tx_thread_current_ptr;

static UINT soak_orphan_baton_violation(TX_THREAD *old_thread, ULONG old_generation)
{
    TX_THREAD *current;
    ULONG generation;
    UINT violation = TX_FALSE;

    /* Orphan already returned its slot. Do not change its scheduling context;
       only freeze this observation of the baton and the slot's new identity. */
    Forbid();
    current = *(TX_THREAD * volatile *) &_tx_thread_current_ptr;
    if (current == old_thread)
    {
        generation = tx_amiga_adopt_generation(current);
        /* An unchanged handle or a freed slot still holding the baton is a
           failure. Only a live replacement owned by another Task is allowed. */
        if ((generation == 0UL) || (generation == old_generation) ||
            (current->tx_thread_id != TX_THREAD_ID) ||
            (current->tx_thread_amiga_task == (VOID *) 0) ||
            (current->tx_thread_amiga_task == (VOID *) FindTask((STRPTR) 0)))
        {
            violation = TX_TRUE;
        }
    }
    Permit();
    return violation;
}

#endif

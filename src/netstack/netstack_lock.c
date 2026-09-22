/*
 * AmiNetXDuo, serialization of singleton and interface lifecycle.
 *
 * SPDX-License-Identifier: MIT
 */

#include "netstack_internal.h"

#include <exec/semaphores.h>
#include <proto/exec.h>

static struct SignalSemaphore ami_ns_lock;
static volatile BOOL          ami_ns_lock_ready;

static VOID ami_ns_lock_init(VOID)
{
    Forbid();
    if (!ami_ns_lock_ready)
    {
        InitSemaphore(&ami_ns_lock);
        ami_ns_lock_ready = TRUE;
    }
    Permit();
}

VOID ami_ns_lock_obtain(VOID)
{
    ami_ns_lock_init();
    ObtainSemaphore(&ami_ns_lock);
}

BOOL ami_ns_lock_attempt(VOID)
{
    ami_ns_lock_init();
    return AttemptSemaphore(&ami_ns_lock) ? TRUE : FALSE;
}

/*
 * Does the CALLING Task hold ami_ns_lock right now?
 *
 * This is the netstack half of the adoption pool's reserve.  A caller inside
 * ami_ns_lock must never park waiting for a slot: it would hold the lock for
 * as long as the pool stayed full, and a slot holder that then wanted the lock
 * would be a cycle.  Exec records the owner of an exclusive SignalSemaphore in
 * ss_Owner, so the question is answerable exactly, at every call site, without
 * anybody having to remember to pass a flag -- which is what the earlier
 * "cached adoptions may not have the reserve" rule got wrong: it keyed off the
 * wrong property and did nothing at all with AMINETXDUO_NXCACHE=OFF.
 *
 * ONE reserved slot is enough, and that is the whole proof: ami_ns_lock is
 * exclusive, so at most one Task holds it; a nested enter from that same Task
 * finds itself already adopted and claims nothing more.
 */
BOOL ami_ns_lock_held_by_me(VOID)
{
    BOOL held;

    if (!ami_ns_lock_ready)
        return FALSE;

    Forbid();
    held = (ami_ns_lock.ss_Owner == FindTask(NULL)) ? TRUE : FALSE;
    Permit();

    return held;
}

VOID ami_ns_lock_release(VOID)
{
    ReleaseSemaphore(&ami_ns_lock);
}

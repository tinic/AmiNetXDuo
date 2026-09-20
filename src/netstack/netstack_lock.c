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

VOID ami_ns_lock_release(VOID)
{
    ReleaseSemaphore(&ami_ns_lock);
}

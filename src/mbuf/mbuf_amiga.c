/*
 * AmiNetXDuo, the AmigaOS half of the mbuf emulation.
 *
 * Everything in src/mbuf/ that touches Exec lives here, so mbuf_alloc.c and
 * mbuf_ops.c stay host-buildable. That is two functions.
 *
 * Forbid()/Permit() rather than Disable()/Enable(). The mbuf vectors are
 * called from application tasks, never from an interrupt, and long Disable()
 * regions break serial, floppy and audio (docs/RESEARCH.md 6.2). Nothing
 * inside the bracket allocates, frees or blocks. ami_mbuf_grow() and
 * ami_mbuf_cluster_get() drop the lock across ami_alloc() for that reason.
 *
 * SPDX-License-Identifier: MIT
 */

#include "mbuf_internal.h"

#include <proto/exec.h>

VOID ami_mbuf_lock(VOID)
{
    Forbid();
}

VOID ami_mbuf_unlock(VOID)
{
    Permit();
}

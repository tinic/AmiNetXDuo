/*
 * Records the stack and the anx drivers publish under public semaphores, read
 * without opening anything.
 *
 * Both are found with FindSemaphore() and copied whole under Forbid(); the
 * semaphore is never obtained, because a diagnostic must not block on the
 * thing that can be broken.  The publisher removes the semaphore under
 * Forbid() before the memory can be freed, so the copy is always of a live
 * record.
 *
 * CheckNetDevice, ShowNetStatus EVENTS and CreateAmiNetXDuoStatusReport read
 * through here.  None of the three may load a driver or start the stack just
 * to find out what happened.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_TOOL_PASSIVE_H
#define AMINETXDUO_TOOL_PASSIVE_H

#include "tools.h"

#include "aminetxduo/anxdiag.h"
#include "aminetxduo/events.h"

#define TOOL_PASSIVE_OK          0
#define TOOL_PASSIVE_ABSENT      1      /* nothing published under the name */
#define TOOL_PASSIVE_BAD_VERSION 2      /* published, in a shape not ours    */

/*
 * The probe record `device` publishes ("anxnet.device", or a path to it).  On
 * TOOL_PASSIVE_BAD_VERSION only ad_Version and ad_Size of *out are set.
 */
UWORD tool_anxdiag_read(const char *device, AnxDiagMark *out);

/*
 * The event ring, oldest first, at most `max` rows.  Returns how many were
 * copied, or -1 with *status saying why not.  *mark gets the ring's header.
 */
LONG tool_events_read(AmiEventMark *mark, NetStatusEvent *out, ULONG max,
                      UWORD *status);

#endif /* AMINETXDUO_TOOL_PASSIVE_H */

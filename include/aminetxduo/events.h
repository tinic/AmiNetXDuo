/*
 * Recording an event.  The record itself, the codes and the sentence each code
 * stands for are in aminetxduo/netstatus.h and src/tools/tool_events.c; this is
 * only the two calls the library makes.
 *
 * A call to ami_event() must never carry a string, a format or a pointer to
 * either.  See src/common/events.c, and tools/check-no-diag-strings.sh, which
 * fails the build if a diagnostic sentence ever appears in a shipped image.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_EVENTS_H
#define AMINETXDUO_EVENTS_H

#include <exec/types.h>

#include "aminetxduo/netstatus.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * How many events are kept.  A build option, because someone counting the last
 * few bytes of Chip RAM should be able to shrink this rather than lose the
 * mechanism; the floor is in CMakeLists.txt.  It costs nothing in the file --
 * an AmigaOS hunk records BSS as a length -- so the only reason to move it is
 * the machine's memory.
 *
 * 32 covers a bring-up of four interfaces, a shutdown of the same, and the
 * expunge that follows, with room left over.
 */
#ifndef AMINETXDUO_EVENT_RING
#  define AMINETXDUO_EVENT_RING  32
#endif

/* index is an interface index, or NETEVENT_NOINDEX. */
VOID  ami_event(UWORD code, UWORD index, ULONG value);

/*
 * Oldest first.  Returns how many were written, and sets *held to how many the
 * ring holds, which is what a caller with too small a buffer needs.  How many
 * the machine recorded altogether is not reported here: the last entry's
 * nse_Seq is that number, and the first entry's says which ones went past.
 */
ULONG ami_event_snapshot(NetStatusEvent *out, ULONG room, ULONG *held);

#ifdef __cplusplus
}
#endif

#endif /* AMINETXDUO_EVENTS_H */

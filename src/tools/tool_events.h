/*
 * The words that go with the library's event codes, and the only way to reach
 * them.  The table is src/tools/tool_events.c; nothing else in any command
 * carries a sentence about an event.
 *
 * Its own header rather than a block in tools.h, because the host test
 * (src/common/test/test_events.c) links the table against the ring and must
 * declare these without pulling in AmigaOS.
 *
 * tool_event_text() is NULL for a code the table does not know, which a caller
 * prints as its number rather than dropping.  tool_event_detail() names what
 * nse_Value counts, or is NULL when it counts nothing.
 * tool_event_value_name() is for the codes whose value is one of a set rather
 * than a number, and is NULL otherwise.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_TOOL_EVENTS_H
#define AMINETXDUO_TOOL_EVENTS_H

#include <exec/types.h>

const char *tool_event_text(UWORD code);
const char *tool_event_detail(UWORD code);
const char *tool_event_value_name(UWORD code, ULONG value);

#endif

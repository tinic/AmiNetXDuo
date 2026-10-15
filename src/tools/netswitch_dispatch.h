/*
 * NetSwitch's control dispatch: which interface an action touches, and what
 * the app says when it cannot.  No AmigaOS calls, so
 * test/test_netswitch_dispatch.c runs it on the build host.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_NETSWITCH_DISPATCH_H
#define AMINETXDUO_NETSWITCH_DISPATCH_H

#include <stddef.h>

/*
 * One live interface as the dispatcher sees it: the fields the join and the
 * gates read from NETSTATUS_INTERFACES, kept a plain record (not the ABI
 * NetStatusInterface) so the host can drive the fault paths the emulator
 * cannot reach -- a failed read, a removed interface, a reused slot.
 */
typedef struct NsLive
{
    const char *name;   /* nsi_Name */
    int         named;  /* NETSTATUS_IF_NAMED: a real row, not padding */
    unsigned    index;  /* nsi_Index */
    int         up;     /* NETSTATUS_IF_LINKUP */
} NsLive;

/*
 * The name-based join of one definition against the live table.  Returns the
 * position in `live` of the entry whose name matches `def_name`
 * (case-insensitive, ASCII), or -1 when none does.  The match is by name,
 * never by index: a definition whose interface was removed, and a slot reused
 * by another interface, both read detached.  A padding slot (named == 0)
 * never matches, even when its name happens to line up.
 */
long ns_join_live(const char *def_name, const NsLive *live, unsigned count);

/*
 * The one-line error for a bulk failure: "<name>: <err text>", cut to cap the
 * way the rest of the app's last-error text is.  The name is never dropped:
 * it fits first and the errno text yields.
 */
void ns_named_error(char *out, size_t cap, const char *name, const char *err);

/*
 * Whether the controls may act after a live-table read.  No resident stack,
 * an empty list, and a failed read (rows stale) all refuse.
 */
int ns_can_act(int has_base, unsigned count, int query_failed);

/*
 * Take-all additionally needs at least one interface online.
 */
int ns_can_take_all(int has_base, int query_failed, unsigned online);

#endif

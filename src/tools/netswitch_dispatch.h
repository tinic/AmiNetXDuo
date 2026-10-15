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

#include "netswitch_text.h"     /* NsState */

/* The action a row asks for.  DOWN and UP are control_switch calls on the
   live index; ADD attaches a definition the stack has never got. */
typedef enum
{
    NS_OP_NONE = 0,
    NS_OP_DOWN,
    NS_OP_UP,
    NS_OP_ADD
} NsDispatchOp;

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

/* The action a state asks for: Go Offline on up, Go Online on down, attach
   on detached.  This is read from the state on screen, before any re-read. */
NsDispatchOp ns_op_for_state(NsState state);

/*
 * The action to take after the re-read, given the action the visible button
 * asked for and the fresh state.  Returns the intended action when the fresh
 * state still asks for it, or NS_OP_NONE when the state moved under the
 * click -- acting on the fresh state then would reverse what the user asked
 * for, so the click is refused and the repaint shows the new action.
 */
NsDispatchOp ns_dispatch_action(NsDispatchOp intended, NsState fresh_state);

/* One control call the click resolves to: the op and, for DOWN and UP, the
   live index it switches.  ADD acts by name, so its index is unused; NONE
   means the click is refused and no call is made. */
typedef struct NsCall
{
    NsDispatchOp op;
    unsigned     index;
} NsCall;

/*
 * The call to make, reconciled the same way as ns_dispatch_action but with
 * the index routed through too, so a test can assert the whole call -- DOWN
 * at index 0, UP at index 2, or no call at all -- rather than only the op.
 */
NsCall ns_resolve_call(NsDispatchOp intended, NsState fresh_state,
                       unsigned index);

/*
 * The action path's hooks into the live app.  No AmigaOS calls; the function
 * pointers carry the GUI's own refresh_rows(), control_switch() and
 * add_interface() so the whole path -- from the on-screen action to the
 * device call -- runs here with recording stubs.  The emulator arm cannot
 * fail a live-table read or remove an interface mid-click, so those call
 * effects are driven on the host instead.
 */
typedef struct NsActionHooks
{
    void *ctx;                          /* first argument to every hook */

    /* Re-read the live table and answer the selected row's fresh state and
       index.  Returns 1 when the read failed (the rows are stale and the
       action must be refused). */
    int (*reread)(void *ctx, NsState *state, unsigned *index);

    /* One control switch on a live index.  Returns errno text, or NULL. */
    const char *(*down)(void *ctx, unsigned index);
    const char *(*up)(void *ctx, unsigned index);

    /* Attach a definition by name and bring it up, as
       tool_stack_add_interface(..., NULL, TRUE) does.  Returns 0, the 4.4BSD
       EEXIST (17) when another tool attached it in the meantime, or another
       errno from that numbering. */
    long (*attach)(void *ctx, const char *name);

    /* Maps an attach errno to its text (tool_code_errno). */
    const char *(*errtext)(long err);
} NsActionHooks;

/*
 * What one action or bulk run did: either the device calls ran (acted) and
 * err is the error text (NULL on success), or the run was refused
 * (acted == 0) and hint says why.  The caller shows one of the two and
 * repaints only after a call: a refusal leaves the on-screen state alone.
 */
typedef struct NsActionResult
{
    int         acted;
    const char *hint;
    const char *err;
} NsActionResult;

/*
 * The production action path between the button and the device: re-read the
 * fresh state, reconcile it against the action the button asked for, refuse a
 * moved target, and otherwise make the one control or attach call.  The whole
 * path runs here so test/test_netswitch_dispatch.c can assert its call
 * effects -- no call after a failed read, no call for a moved target, one
 * call with the right op and index otherwise.
 */
NsActionResult ns_run_action(NsDispatchOp intended, const char *name,
                             const NsActionHooks *h);

/*
 * The take-all path's row view: the three fields its loop reads from a fresh
 * join, kept a plain record so the bulk decision runs on the host.
 */
typedef struct NsBulkRow
{
    const char *name;
    NsState     state;
    unsigned    index;
} NsBulkRow;

/*
 * The take-all path's hooks into the live app, same idea as NsActionHooks.
 */
typedef struct NsBulkHooks
{
    void *ctx;

    /* Re-read the live table and fill rows[] (count entries) with the fresh
       name/state/index of every configured interface.  Returns 1 when the
       read failed (no switch may go out). */
    int (*reread)(void *ctx, NsBulkRow *rows, unsigned count);

    /* Whether a row is the loopback definition the bulk skips. */
    int (*is_loopback)(void *ctx, const char *name);

    /* One control switch down.  Returns errno text, or NULL. */
    const char *(*down)(void *ctx, unsigned index);
} NsBulkHooks;

/*
 * The take-all path: re-read, then take every up, non-loopback row down,
 * continuing past a failure so a partial bulk still switches the rest.  The
 * first failure is reported as "<name>: <err>" (ns_named_error) into first
 * (cap), and *switched counts the switches that went out.  Returns the same
 * shape as ns_run_action: refused (acted == 0, hint) on a failed re-read, or
 * acted with err = the first-error line (NULL when every switch went down).
 */
NsActionResult ns_bulk_down(const NsBulkHooks *h, NsBulkRow *rows,
                            unsigned count, char *first, size_t cap,
                            unsigned *switched);

#endif

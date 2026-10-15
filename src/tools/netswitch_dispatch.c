/* SPDX-License-Identifier: MIT */
#include "netswitch_dispatch.h"

static int ascii_lower(int c)
{
    if (c >= 'A' && c <= 'Z')
        return c + ('a' - 'A');
    return c;
}

static int name_eq(const char *a, const char *b)
{
    while (*a != '\0' && *b != '\0')
    {
        if (ascii_lower((unsigned char)*a) != ascii_lower((unsigned char)*b))
            return 0;
        a++;
        b++;
    }
    return *a == *b;
}

long ns_join_live(const char *def_name, const NsLive *live, unsigned count)
{
    unsigned i;

    if (def_name == NULL || live == NULL)
        return -1;
    for (i = 0; i < count; i++)
    {
        if (!live[i].named)
            continue;
        if (name_eq(live[i].name, def_name))
            return (long)i;
    }
    return -1;
}

void ns_named_error(char *out, size_t cap, const char *name, const char *err)
{
    size_t n = 0, i;
    int    sep = 0;

    if (out == NULL || cap == 0)
        return;

    for (i = 0; name != NULL && name[i] != '\0' && n + 1 < cap; i++)
        out[n++] = name[i];

    /* ": " only when there is an errno text to follow it, and room for both. */
    if (err != NULL && err[0] != '\0' && n + 2 < cap)
    {
        out[n++] = ':';
        out[n++] = ' ';
        sep = 1;
    }

    if (sep)
        for (i = 0; err[i] != '\0' && n + 1 < cap; i++)
            out[n++] = err[i];

    out[n] = '\0';
}

int ns_can_act(int has_base, unsigned count, int query_failed)
{
    return has_base && count != 0 && !query_failed;
}

int ns_can_take_all(int has_base, int query_failed, unsigned online)
{
    return has_base && !query_failed && online != 0;
}

NsDispatchOp ns_op_for_state(NsState state)
{
    if (state == NS_STATE_UP)
        return NS_OP_DOWN;
    if (state == NS_STATE_DOWN)
        return NS_OP_UP;
    return NS_OP_ADD;                       /* detached */
}

NsDispatchOp ns_dispatch_action(NsDispatchOp intended, NsState fresh_state)
{
    if (ns_op_for_state(fresh_state) == intended)
        return intended;
    return NS_OP_NONE;
}

NsCall ns_resolve_call(NsDispatchOp intended, NsState fresh_state,
                       unsigned index)
{
    NsCall call;

    call.op    = ns_dispatch_action(intended, fresh_state);
    call.index = (call.op == NS_OP_DOWN || call.op == NS_OP_UP) ? index : 0;
    return call;
}

/* The attach hook returns tool_stack_add_interface()'s errno in 4.4BSD
   numbering (tool_util.c:tool_code_errno keeps the same table); EEXIST is the
   value it reports when another tool has already attached the definition in
   the meantime.  <errno.h> is not included so the file stays free of AmigaOS
   and host libc headers alike, and the number is fixed by that numbering. */
enum
{
    NS_ATTACH_EEXIST = 17
};

NsActionResult ns_run_action(NsDispatchOp intended, const char *name,
                             const NsActionHooks *h)
{
    NsActionResult out;
    NsState        st;
    unsigned       ix;
    NsCall         call;

    out.acted = 0;
    out.hint  = NULL;
    out.err   = NULL;

    if (h == NULL || h->reread == NULL)
    {
        /* No way to re-read the live table: refuse, never guess. */
        out.hint = ns_state_moved_error;
        return out;
    }

    if (h->reread(h->ctx, &st, &ix))
    {
        out.hint = ns_query_failed_hint;
        return out;
    }

    call = ns_resolve_call(intended, st, ix);
    if (call.op == NS_OP_NONE)
    {
        /* The state moved under the click: what the button said no longer
           applies, and acting on the fresh state would reverse the request. */
        out.hint = ns_state_moved_error;
        return out;
    }

    out.acted = 1;
    if (call.op == NS_OP_DOWN)
    {
        out.err = (h->down != NULL) ? h->down(h->ctx, call.index) : NULL;
    }
    else if (call.op == NS_OP_UP)
    {
        out.err = (h->up != NULL) ? h->up(h->ctx, call.index) : NULL;
    }
    else
    {
        long e = (h->attach != NULL) ? h->attach(h->ctx, name) : 0;

        if (e != 0 && e != NS_ATTACH_EEXIST)
        {
            out.err = (h->errtext != NULL) ? h->errtext(e) : NULL;
        }
        else if (e == NS_ATTACH_EEXIST)
        {
            /* Another tool attached it in the meantime: re-read for the
               index and bring it up. */
            if (h->reread(h->ctx, &st, &ix))
                out.err = ns_query_failed_hint;
            else if (st != NS_STATE_DETACHED)
                out.err = (h->up != NULL) ? h->up(h->ctx, ix) : NULL;
        }
        /* e == 0: attached and up; err stays NULL. */
    }

    return out;
}

NsActionResult ns_bulk_down(const NsBulkHooks *h, NsBulkRow *rows,
                            unsigned count, char *first, size_t cap,
                            unsigned *switched)
{
    NsActionResult out;
    unsigned       i, n = 0;
    const char    *err = NULL;

    out.acted = 0;
    out.hint  = NULL;
    out.err   = NULL;

    if (h == NULL || h->reread == NULL)
    {
        /* No way to re-read the live table: refuse, never guess. */
        out.hint = ns_query_failed_hint;
        return out;
    }

    if (h->reread(h->ctx, rows, count))
    {
        out.hint = ns_query_failed_hint;
        return out;
    }

    out.acted = 1;
    for (i = 0; rows != NULL && i < count; i++)
    {
        const char *e;

        if (rows[i].state != NS_STATE_UP)
            continue;
        if (h->is_loopback != NULL && h->is_loopback(h->ctx, rows[i].name))
            continue;

        n++;
        e = (h->down != NULL) ? h->down(h->ctx, rows[i].index) : NULL;
        if (e != NULL && err == NULL)
        {
            /* The first failure is the one reported; say which interface it
               was, not just the errno text.  The loop still runs on: a
               partial bulk must switch the rest. */
            ns_named_error(first, cap, rows[i].name, e);
            err = first;
        }
    }

    out.err = err;
    if (switched != NULL)
        *switched = n;
    return out;
}

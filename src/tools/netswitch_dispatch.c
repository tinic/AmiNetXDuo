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

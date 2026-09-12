/*
 * Shared wait policy for the AmigaDOS tools.
 *
 * SPDX-License-Identifier: MIT
 */

#include "tools.h"

VOID tool_wait_init(ToolWait *wait, ULONG seconds)
{
    wait->limit   = seconds;
    wait->elapsed = 0;
    wait->broken  = FALSE;
}

BOOL tool_wait_second(ToolWait *wait)
{
    if (wait->limit != 0 && wait->elapsed >= wait->limit)
        return FALSE;

    if (tool_delay_ticks((ULONG)TICKS_PER_SECOND))
    {
        wait->broken = TRUE;
        return FALSE;
    }

    wait->elapsed++;
    return TRUE;
}

BOOL tool_wait_dhcp_bound(struct Library *base, UWORD index, ToolWait *wait,
                          ULONG *addr_out)
{
    for (;;)
    {
        LONG state = tool_netstatus_dhcp_state(base, index, addr_out);

        if (state == NETSTATUS_DHCP_BOUND)
            return TRUE;
        if (state < 0)
            return FALSE;
        if (!tool_wait_second(wait))
            return FALSE;
    }
}

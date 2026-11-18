/*
 * tool_reboot, flush every volume and restart: see tool_reboot.h.
 *
 * SPDX-License-Identifier: MIT
 */

#include "tool_reboot.h"

#include <dos/dosextens.h>

/* The ports are collected under the DosList lock and the packets sent with it
   given back: DoPkt() waits, and the handler can want the DosList to answer. */
#define TOOL_FLUSH_MAX  16

VOID tool_reboot(VOID)
{
    struct MsgPort *ports[TOOL_FLUSH_MAX];
    struct DosList *dl;
    ULONG           n = 0;
    ULONG           i;

    dl = LockDosList(LDF_VOLUMES | LDF_READ);
    if (dl != NULL)
    {
        while (n < (ULONG)TOOL_FLUSH_MAX &&
               (dl = NextDosEntry(dl, LDF_VOLUMES | LDF_READ)) != NULL)
        {
            if (dl->dol_Task != NULL)
                ports[n++] = dl->dol_Task;
        }
        UnLockDosList(LDF_VOLUMES | LDF_READ);
    }

    for (i = 0; i < n; i++)
        (VOID)DoPkt(ports[i], ACTION_FLUSH, 0, 0, 0, 0, 0);

    /* Half a second for what is still leaving: httpd's close frame and FIN,
       so the console viewer is told it is a reboot, not a dropped link. */
    Delay(25);

    ColdReboot();
}

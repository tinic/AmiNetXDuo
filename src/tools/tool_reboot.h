/*
 * tool_reboot, a restart that is safe after disk writes.
 *
 * C:Reboot calls ColdReboot() at once, and a handler can still hold dirty
 * buffers then.  This asks every mounted volume to write them out first.
 * httpd's console `reset` and SyncReboot reach it.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_TOOL_REBOOT_H
#define AMINETXDUO_TOOL_REBOOT_H

#include "tools.h"

/* ACTION_FLUSH to every volume, half a second, then ColdReboot().  Returns
   only if ColdReboot() does, which it does not on a real machine. */
VOID tool_reboot(VOID);

#endif /* AMINETXDUO_TOOL_REBOOT_H */

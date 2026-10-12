/*
 * NetWirelessPrefs: stopping and starting WirelessManager.  See
 * netwireless_wm.c.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_NETWIRELESS_WM_H
#define AMINETXDUO_NETWIRELESS_WM_H

#include <exec/types.h>

#define NW_WM_COMMAND   "C:WirelessManager"

/* TRUE when it is running; path gets the command as it was started. */
BOOL nw_wm_running(char *path, ULONG cap);

/* Ctrl-C, again every 2.5 seconds, until it has gone; FALSE after ten
   seconds with it still there. */
BOOL nw_wm_stop(VOID);

/* In a background Shell with a 32 KB stack, input and output NIL:.  Clears
   the Disconnect mark. */
BOOL nw_wm_start(const char *command, const char *device, ULONG unit);

/* The Disconnect mark: ENV:WiFiOff (ENV: only, gone at the next boot), set
   by a Disconnect and cleared when WirelessManager is started here.  With
   it set and WirelessManager not running, the Wi-Fi is off for
   NetWireless and NetWirelessPrefs alike. */
VOID nw_wm_mark_off(VOID);
BOOL nw_wm_marked_off(VOID);

#endif

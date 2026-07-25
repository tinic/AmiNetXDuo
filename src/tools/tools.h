/*
 * AmiNetXDuo -- shared plumbing for the command-line tools.
 *
 * These are ordinary AmigaDOS commands, not library code: they may use
 * dos.library freely, they parse their arguments with ReadArgs() so they
 * behave like every other Shell command (including "?" for the template),
 * and they honour Ctrl-C.
 *
 * Include order matters. tx_api.h/nx_api.h must precede <exec/types.h>,
 * because exec turns VOID into a macro and that breaks tx_port.h's
 * `typedef void VOID`. Same rule as src/bsdsocket/bsdsocket_internal.h.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_TOOLS_H
#define AMINETXDUO_TOOLS_H

#include "tx_api.h"
#include "nx_api.h"

#include <exec/types.h>
#include <exec/nodes.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/rdargs.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include <stdarg.h>

#include "aminetxduo/config.h"
#include "aminetxduo/netstack.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Every tool defines this once; it prefixes diagnostics and is what
 * PrintFault() puts in front of the DOS error text.
 */
extern const char *const tool_name;

/*
 * Output goes through dos.library, so the format strings are RawDoFmt's, not
 * newlib's: every integer is 32 bits and must be written %ld / %lu / %lx.
 */
VOID tool_printf(const char *fmt, ...);
VOID tool_error(const char *fmt, ...);     /* "<tool>: ..." + newline        */
VOID tool_fault(LONG code);                /* PrintFault(code, tool_name)    */

/* ------------------------------------------------------------------ break */

/*
 * Clear a stale Ctrl-C left over from whatever ran before us. Every tool
 * calls this once on entry.
 */
VOID tool_break_arm(VOID);

/*
 * TRUE once the user has pressed Ctrl-C. Consumes the signal, so a caller
 * that wants to print a summary before quitting still can.
 */
BOOL tool_break(VOID);

/* Delay() in small slices so a break is noticed promptly. ticks are 1/50 s. */
BOOL tool_delay_ticks(ULONG ticks);        /* TRUE if interrupted            */

/* ------------------------------------------------------------- formatting */

VOID tool_format_mac(const UBYTE *mac, char *buf, ULONG buflen);
ULONG tool_broadcast(ULONG addr, ULONG mask);
UWORD tool_prefix_len(ULONG mask);

/* ------------------------------------------------------------------ stack */

const char *tool_net_error(LONG err);

/*
 * The running stack, or NULL after printing a sensible message. Never starts
 * it -- ShowNetStatus/netstat/ping/host must not bring the network up as a
 * side effect of being asked a question.
 */
AmiNetStack *tool_require_stack(VOID);

/* Interface index in config order, or -1 after printing a message. */
LONG tool_find_interface(const char *name);

/* "DEVS:NetInterfaces/eth0" -> "eth0"; "eth0" -> "eth0". */
const char *tool_basename(const char *path);

/* Was this started from Workbench? These commands are Shell-only. */
BOOL tool_from_workbench(int argc);

#ifdef __cplusplus
}
#endif

#endif /* AMINETXDUO_TOOLS_H */

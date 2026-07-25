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

#include "aminetxduo/compat.h"
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

/* ------------------------------------------------------------- diagnosis --
 *
 * tool_diag.c. Everything here works with the stack down, because that is the
 * state that needs explaining. See the comment at the top of that file for
 * what the messages are trying to achieve.
 */

#define TOOL_NAME_LEN       40      /* a device or interface file name       */
#define TOOL_MAX_DEVICES    12      /* as many drivers as we will list       */

typedef struct ToolDevice
{
    char name[TOOL_NAME_LEN];       /* "ariadne.device"                      */
    char where[TOOL_NAME_LEN];      /* "DEVS:Networks" or "already in memory" */
} ToolDevice;

/* Small string/file helpers the commands share with the explainers. */
int   tool_stricmp(const char *a, const char *b);
int   tool_stricmp_n(const char *a, const char *b, ULONG n);
VOID  tool_copy_string(char *dst, ULONG dstlen, const char *src);
VOID  tool_join_path(char *dst, ULONG dstlen, const char *dir, const char *name);
BOOL  tool_exists(const char *path);
ULONG tool_list_dir(const char *path, char names[][TOOL_NAME_LEN], ULONG max,
                    const char *suffix);

/*
 * Which SANA-II drivers this machine actually has: everything in
 * DEVS:Networks, plus any driver we know by name found elsewhere or already
 * in memory. Scanned once; the result is cached.
 */
ULONG             tool_scan_devices(VOID);
const ToolDevice *tool_scan_device(ULONG index);

/* Which of the usual drawers holds `device`, or NULL if it is nowhere. */
const char *tool_device_where(const char *device);

/*
 * Open and immediately close a SANA-II device, to find out whether it really
 * works. 0 means it opened; anything else is the OpenDevice() error.
 */
#define TOOL_PROBE_NO_NAME      (-100)
#define TOOL_PROBE_NO_MEMORY    (-101)
LONG tool_device_probe(const char *device, ULONG unit);

/* Indented advice under an error line, and a blank separator. */
VOID tool_advise(const char *text);
VOID tool_advise_blank(VOID);

/* Word-wrap to the width of a Shell window, indented. */
VOID tool_wrap(ULONG indent, const char *text);

/*
 * Print configuration-file problems -- with the file, the line number and a
 * suggestion -- while the config layer is parsing. Bracket a call to
 * ami_config_load()/ami_config_load_interface() with these.
 */
VOID tool_config_watch(VOID);
VOID tool_config_unwatch(VOID);

/* The explainers themselves. Each prints a block; none of them exits. */
VOID tool_explain_interface_file(const char *name);   /* file missing        */
VOID tool_explain_device(const char *device, ULONG unit);
VOID tool_explain_no_interfaces(VOID);                /* nothing configured  */
VOID tool_explain_dhcp(const char *name);             /* nobody answered     */
VOID tool_explain_resolve(const char *name, LONG err); /* a lookup failed    */
VOID tool_explain_no_stack(VOID);                     /* nothing is running  */

/*
 * TRUE when bsdsocket.library is in memory with at least one opener, which is
 * the only way one command can tell that another has the stack up. It does
 * NOT open the library: that would start the stack.
 */
BOOL tool_stack_library_running(VOID);

/* TRUE when bsdsocket.library exists at all (LIBS: or already in memory). */
BOOL tool_stack_installed(VOID);

/*
 * Start the network by opening bsdsocket.library, which brings the stack up
 * on its first open. The returned base is deliberately never closed by the
 * caller: that open reference is what keeps the network up after the command
 * exits. NULL means the library is missing or the stack refused to start.
 */
struct Library *tool_stack_start(VOID);

/*
 * Is that library AmiNetXDuo's? Every Amiga TCP/IP stack answers to the name
 * bsdsocket.library, so a machine that already has Roadshow or AmiTCP -- or an
 * emulator with its own -- will hand out somebody else's.
 */
BOOL tool_stack_is_ours(struct Library *base);
VOID tool_explain_foreign_stack(struct Library *base);

/*
 * What a running stack will tell an outsider: gethostid() and gethostname()
 * through the library's own vectors. FALSE when nothing is running -- this
 * never starts the stack.
 */
BOOL tool_stack_query(ULONG *addr_out, char *host, ULONG hostlen);

/*
 * Look a name up through the running stack's own gethostbyname()/
 * gethostbyaddr() vectors. This works when netstack_resolve() cannot be
 * reached -- the normal case, with the stack inside bsdsocket.library -- and
 * uses whatever name servers it is really using, DHCP-supplied included.
 * FALSE when nothing is running or the name is not known.
 */
BOOL tool_stack_lookup(const char *name, ULONG *addr_out);
BOOL tool_stack_lookup_addr(ULONG addr, char *name_out, ULONG name_len);

/*
 * The name servers the running stack is really using, as dotted quads --
 * including the ones a DHCP lease supplied, which no file on disk mentions.
 * Returns how many were written. 0 when nothing is running.
 */
ULONG tool_stack_name_servers(char out[][16], ULONG max);

/* "Usage: <tool> <template>" plus one line of what it is for. */
VOID tool_usage(const char *tmpl, const char *summary);

#ifdef __cplusplus
}
#endif

#endif /* AMINETXDUO_TOOLS_H */

/*
 * AmiNetXDuo -- shared plumbing for the command-line tools.
 *
 * These are AmigaDOS commands, not library code: they use dos.library freely,
 * parse arguments with ReadArgs() (so "?" prints the template like every other
 * Shell command), and honour Ctrl-C.
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
#include "aminetxduo/netstatus.h"

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
 * The running stack, or NULL after printing a message. Never starts it: a
 * status command must not bring the network up just for being asked.
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
 * tool_diag.c. Everything here works with the stack down, which is the state
 * that needs explaining.
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
 * The SANA-II drivers this machine has: everything in DEVS:Networks, plus any
 * driver known by name found elsewhere or already in memory. Scanned once and
 * cached.
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
 * Print configuration-file problems -- file, line number, suggestion -- while
 * the config layer is parsing. Bracket a call to ami_config_load() or
 * ami_config_load_interface() with these.
 */
VOID tool_config_watch(VOID);
VOID tool_config_unwatch(VOID);

/* The explainers. Each prints a block; none of them exits. */
VOID tool_explain_interface_file(const char *name);   /* file missing        */
VOID tool_explain_device(const char *device, ULONG unit);
VOID tool_explain_device_refused(const char *device, ULONG unit);
VOID tool_explain_no_interfaces(VOID);                /* nothing configured  */
VOID tool_explain_dhcp(const char *name);             /* nobody answered     */
VOID tool_explain_resolve(const char *name, LONG err); /* a lookup failed    */
VOID tool_explain_no_stack(VOID);                     /* nothing is running  */

/*
 * TRUE when bsdsocket.library is in memory with at least one opener -- the
 * only way one command can tell that another has the stack up. Does not open
 * the library, since that would start the stack.
 */
BOOL tool_stack_library_running(VOID);

/* TRUE when bsdsocket.library exists at all (LIBS: or already in memory). */
BOOL tool_stack_installed(VOID);

/*
 * Start the network by opening bsdsocket.library, which brings the stack up on
 * its first open. The caller must not close the returned base: that open
 * reference is what keeps the network up after the command exits. NULL means
 * the library is missing or the stack refused to start.
 */
struct Library *tool_stack_start(VOID);

/*
 * Is that library AmiNetXDuo's? Every Amiga TCP/IP stack answers to the name
 * bsdsocket.library, so a machine with Roadshow, AmiTCP or an emulator's own
 * stack will hand out somebody else's.
 */
BOOL tool_stack_is_ours(struct Library *base);
VOID tool_explain_foreign_stack(struct Library *base);

/*
 * gethostid() and gethostname() through the library's own vectors. FALSE when
 * nothing is running; never starts the stack.
 */
BOOL tool_stack_query(ULONG *addr_out, char *host, ULONG hostlen);

/*
 * Look a name up through the running stack's own gethostbyname() /
 * gethostbyaddr() vectors. Works when netstack_resolve() cannot be reached --
 * the normal case, with the stack inside bsdsocket.library -- and uses the
 * name servers it is really using, DHCP-supplied included. FALSE when nothing
 * is running or the name is not known.
 */
BOOL tool_stack_lookup(const char *name, ULONG *addr_out);
BOOL tool_stack_lookup_addr(ULONG addr, char *name_out, ULONG name_len);

/*
 * The name servers the running stack is really using, as dotted quads,
 * including DHCP-supplied ones that no file on disk mentions. Returns how many
 * were written; 0 when nothing is running.
 */
ULONG tool_stack_name_servers(char out[][16], ULONG max);

/* --------------------------------------------------- the running stack --
 *
 * Everything above this line works with the stack down. Everything below it
 * asks the running stack, through the two private LVOs in
 * include/aminetxduo/netstatus.h.
 *
 * A command that links libnetxduo.a gets its own NX_IP with no interfaces and
 * its own ThreadX kernel that was never started; the running stack lives inside
 * bsdsocket.library. netstack_ip() in a tool resolves to
 * src/tools/netstack_weak.c's stub and returns NULL in every shipped build,
 * which left `netstat`, `ping` and ShowNetStatus inert in v0.2.0 while printing
 * a message that read like a pass. docs/RESEARCH.md 21.
 *
 * The library copies what it is asked for, so nothing returned here points into
 * the stack and nothing can be left dangling by a teardown.
 */

/*
 * Open the running library, or NULL. Never starts the stack: it checks
 * tool_stack_library_running() first, so a status command cannot bring the
 * network up. `quiet` suppresses the explanation, for callers with their own.
 */
struct Library *tool_netstatus_open(BOOL quiet);
VOID            tool_netstatus_close(struct Library *base);

/*
 * One snapshot. `what` is a NETSTATUS_* selector; `buffer` starts with a
 * NetStatusHeader this fills in. Returns the number of entries written, or -1
 * having printed nothing -- the caller decides what a failure means, since "no
 * ARP entries" and "this is not our library" need different words.
 *
 * A buffer that holds the header but no entries asks how many there are:
 * nsh_Available comes back regardless of nsh_Count.
 */
/*
 * An IPv6 address, four host-order ULONGs, in RFC 5952 text, and the same
 * text back to four ULONGs.  Both answer the same on a library with IPv6 and
 * one without: they convert text, and whether the machine can reach the
 * address is a separate question each caller asks for itself.  Neither needs
 * the library open.  A "/N" suffix is a syntax error, as inet_pton() has it.
 */
VOID tool_format_ip6(const ULONG addr[4], char *buf, ULONG buflen);
BOOL tool_parse_ip6(const char *text, ULONG out[4]);

LONG tool_netstatus_query(struct Library *base, ULONG what,
                          APTR buffer, ULONG size, ULONG entry_size);

/*
 * The mutating half. The caller fills `ctl` except for the magic and version,
 * which this sets. Returns 0, or -1 with *errno_out (if given) set to the
 * library's errno -- ENOSYS for an operation this build cannot do.
 */
LONG tool_netstatus_control(struct Library *base, ULONG op,
                            NetStatusControl *ctl, LONG *errno_out);

/* Convenience: open, ask for NETSTATUS_SYSTEM, close. TRUE on success. */
BOOL tool_netstatus_system(NetStatusSystem *out);

/*
 * The message for "the stack is running but would not answer". Distinguishes a
 * foreign bsdsocket.library (Roadshow, AmiTCP, an emulator's own) from ours
 * being too old for these vectors; the two need different advice.
 */
VOID tool_explain_no_netstatus(struct Library *base);

/* "Usage: <tool> <template>" plus a one-line summary. */
VOID tool_usage(const char *tmpl, const char *summary);

#ifdef __cplusplus
}
#endif

#endif /* AMINETXDUO_TOOLS_H */

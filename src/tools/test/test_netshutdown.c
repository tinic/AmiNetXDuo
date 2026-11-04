/*
 * Execute the real NetShutdown and Wi-Fi exit requester against Exec/DOS
 * services. No target code is copied into the model. This proves ordering,
 * bounded waits and truthful verdicts, not physical Prism2 IRQ behavior.
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <exec/types.h>
#include <exec/execbase.h>
#include <exec/devices.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include "aminetxduo/netstatus.h"

/* Keep 32-bit Amiga words. BSTR/BPTR fields below contain small model tokens,
 * not truncated host pointers. RawDoFmt arguments are never interpreted. */
static APTR h_baddr(BPTR token);
#undef BADDR
#define BADDR(p) h_baddr(p)
#define AMINETXDUO_TOOLS_H
#define TOOL_VERSTAG(n) "$VER: " n " host-fixture"
#define NX_MAX_PHYSICAL_INTERFACES 4
#define GVF_GLOBAL_ONLY 256
#define SYS_Input (TAG_USER + 33)
#define SYS_Output (TAG_USER + 34)
#define SYS_Asynch (TAG_USER + 35)
#define SYS_UserShell (TAG_USER + 37)
struct RDArgs { int unused; };
struct RDArgs *ReadArgs(CONST_STRPTR text, LONG *args, struct RDArgs *rda);
VOID FreeArgs(struct RDArgs *rda);
LONG VPrintf(CONST_STRPTR fmt, APTR args);
ULONG MaxCli(VOID);
struct Process *FindCliProc(ULONG n);
LONG DoPkt(struct MsgPort *port, LONG action, ...);
LONG SetVar(CONST_STRPTR name, CONST_STRPTR value, LONG size, LONG flags);
LONG GetVar(CONST_STRPTR name, STRPTR value, LONG size, LONG flags);
LONG DeleteVar(CONST_STRPTR name, LONG flags);
LONG SystemTags(CONST_STRPTR command, ...);
VOID tool_error(const char *fmt, ...);
VOID tool_fault(LONG code);
VOID tool_usage(const char *usage, const char *description);
BOOL tool_from_workbench(int argc);
VOID tool_break_arm(VOID);
BOOL tool_delay_ticks(ULONG ticks);
BOOL tool_stack_library_running(VOID);
BOOL tool_stack_is_ours(struct Library *base);
struct Library *tool_netstatus_open(BOOL quiet);
VOID tool_netstatus_close(struct Library *base);
LONG tool_netstatus_query(struct Library *, ULONG, APTR, ULONG, ULONG);
LONG tool_netstatus_control(struct Library *, ULONG, NetStatusControl *, LONG *);
VOID tool_explain_no_netstatus(struct Library *base);
VOID tool_netstatus_devices(struct Library *base);
BOOL tool_if_device(char *dst, ULONG size, const NetStatusInterface *e);
const char *tool_basename(const char *name);
int tool_stricmp(const char *a, const char *b);

extern struct ExecBase *SysBase;

#include "../netwireless_wm.c"
#define main h_command
#include "../netshutdown.c"
#undef main

static unsigned checks, failures, cases;
#define CHECK(c, message) do { checks++; if (!(c)) { failures++; \
    printf("FAIL %s:%d: %s\n", __func__, __LINE__, message); } } while (0)

static struct ExecBase system_base;
struct ExecBase *SysBase = &system_base;
static struct Library library;
static struct Device device;
static struct RDArgs rdargs;
static struct Process process[4];
static struct CommandLineInterface cli[4];
static struct MsgPort port[2];
static UBYTE command_name[4][128];
static struct {
    BOOL alive[4], requested[4];
    ULONG exit_after[4];
    BOOL port_present[2];
    BOOL running, attached, library_present, foreign, retained;
    BOOL whole_name, missing_name, break_consumed;
    ULONG delays, break_at, signals, removes, notify, releases, closes;
    ULONG removed_while_clients, last_client_close, removed_at, ordinal;
    ULONG errors, still_open_errors, retained_errors, metadata_errors;
    LONG socket_holders;
    int forbidden, dos_locked;
    const char *driver, *resident_name;
} h;

static VOID list_one(struct List *list, struct Node *node)
{
    memset(list, 0, sizeof(*list));
    list->lh_Head = node;
    list->lh_TailPred = node;
    node->ln_Pred = (struct Node *)&list->lh_Head;
    node->ln_Succ = (struct Node *)&list->lh_Tail;
}

static VOID reset(VOID)
{
    memset(&h, 0, sizeof(h));
    memset(&system_base, 0, sizeof(system_base));
    memset(&library, 0, sizeof(library));
    memset(&device, 0, sizeof(device));
    memset(process, 0, sizeof(process));
    memset(cli, 0, sizeof(cli));
    memset(port, 0, sizeof(port));
    memset(command_name, 0, sizeof(command_name));
    h.running = h.attached = h.library_present = h.whole_name = TRUE;
    h.driver = "DEVS:Networks/prism2.device";
    h.resident_name = "Prism2.device";
    library.lib_Node.ln_Name = "bsdsocket.library";
    device.dd_Library.lib_Node.ln_Name = (char *)h.resident_name;
    device.dd_Library.lib_OpenCnt = 1; /* the stack's open */
    list_one(&system_base.DeviceList, &device.dd_Library.lib_Node);
    /* The product's static command state is intentionally NOT reset here:
       multiple calls exercise resident/repeated command execution. */
}

static VOID client(unsigned i, const char *command, BOOL public_port,
                   ULONG exit_after)
{
    size_t n = strlen(command);
    CHECK(i < 4 && n < sizeof(command_name[0]), "valid client model");
    command_name[i][0] = (UBYTE)n;
    memcpy(command_name[i] + 1, command, n);
    h.alive[i] = TRUE;
    h.exit_after[i] = exit_after;
    process[i].pr_CLI = (BPTR)(i * 2 + 1);
    cli[i].cli_CommandName = (BPTR)(i * 2 + 2);
    cli[i].cli_Module = 1;
    if (public_port) {
        CHECK(i < 2, "only commodity/preferences have named ports");
        port[i].mp_SigTask = &process[i].pr_Task;
        h.port_present[i] = TRUE;
        /* Workbench starts have no CLI. */
        process[i].pr_CLI = 0;
    }
    device.dd_Library.lib_OpenCnt++;
}

static int run(VOID)
{
    int rc = h_command(1, NULL);
    CHECK(h.forbidden == 0 && h.dos_locked == 0, "all locks released");
    return rc;
}

static APTR h_baddr(BPTR token)
{
    unsigned i;
    if (!token) return NULL;
    i = (unsigned)(token - 1) / 2;
    CHECK(token > 0 && i < 4, "valid BPTR token, never a truncated pointer");
    return token % 2 ? (APTR)&cli[i] : (APTR)command_name[i];
}
VOID Forbid(VOID) { h.forbidden++; }
VOID Permit(VOID) { CHECK(h.forbidden > 0, "balanced Permit"); h.forbidden--; }
ULONG MaxCli(VOID) { CHECK(h.forbidden > 0, "CLI lookup protected"); return 4; }
struct Process *FindCliProc(ULONG n)
{
    CHECK(h.forbidden > 0 && n > 0 && n <= 4, "protected process lookup");
    return h.alive[n - 1] ? &process[n - 1] : NULL;
}
struct MsgPort *FindPort(const UBYTE *name)
{
    CHECK(h.forbidden > 0, "port lookup protected");
    if (!strcmp((const char *)name, NW_STATUS_PORT)) return h.port_present[0] ? &port[0] : NULL;
    if (!strcmp((const char *)name, "NetWirelessPrefs")) return h.port_present[1] ? &port[1] : NULL;
    CHECK(FALSE, "unexpected port lookup"); return NULL;
}
VOID Signal(struct Task *task, ULONG bits)
{
    unsigned i;
    CHECK(h.forbidden > 0 && bits == SIGBREAKF_CTRL_C, "safe orderly exit signal");
    for (i = 0; i < 4; i++) if (task == &process[i].pr_Task) {
        h.requested[i] = TRUE; h.signals++; return;
    }
    CHECK(FALSE, "unknown task signalled");
}
BOOL tool_delay_ticks(ULONG ticks)
{
    unsigned i;
    CHECK(h.forbidden == 0 && ticks == TICKS_PER_SECOND, "wait outside Forbid");
    h.delays++;
    if (!h.break_consumed && h.break_at == h.delays) {
        h.break_consumed = TRUE; return TRUE;
    }
    for (i = 0; i < 4; i++) if (h.alive[i] && h.requested[i] &&
                               h.delays >= h.exit_after[i]) {
        /* Model CloseDevice before removing the public port/CLI. */
        CHECK(device.dd_Library.lib_OpenCnt > 0, "client owns device");
        device.dd_Library.lib_OpenCnt--;
        h.alive[i] = FALSE;
        if (i < 2) h.port_present[i] = FALSE;
        h.last_client_close = ++h.ordinal;
    }
    return FALSE;
}
struct Node *FindName(struct List *list, const char *name)
{
    CHECK(h.forbidden > 0 && list == &SysBase->LibList, "safe library lookup");
    CHECK(!strcmp(name, "bsdsocket.library"), "only our library queried");
    return h.library_present ? &library.lib_Node : NULL;
}
VOID RemLibrary(struct Library *base)
{
    CHECK(h.forbidden > 0 && base == &library, "safe library removal");
    h.library_present = FALSE;
}
BOOL tool_stack_is_ours(struct Library *base) { CHECK(base == &library, "known library"); return !h.foreign; }
BOOL tool_stack_library_running(VOID) { return h.running; }
struct Library *tool_netstatus_open(BOOL quiet) { (void)quiet; return &library; }
VOID tool_netstatus_close(struct Library *base) { CHECK(base == &library, "known base closed"); h.closes++; }
LONG tool_netstatus_query(struct Library *base, ULONG what, APTR out, ULONG bytes, ULONG stride)
{
    NetStatusHeader *hdr = out;
    (void)bytes;
    CHECK(base == &library && h.forbidden == 0, "status outside Forbid");
    memset(out, 0, bytes);
    if (what == NETSTATUS_OPENERS) {
        NetStatusOpener *o = (APTR)(hdr + 1);
        CHECK(stride == sizeof(*o), "real public opener layout");
        hdr->nsh_Available = (ULONG)(1 + h.socket_holders);
        o[0].nso_Flags = NETSTATUS_OPENER_SELF;
        if (h.socket_holders) strcpy(o[1].nso_Name, "stubborn");
        return 1 + h.socket_holders;
    }
    CHECK(what == NETSTATUS_INTERFACES && stride == sizeof(NetStatusInterface), "real public interface layout");
    if (h.attached) {
        NetStatusInterface *e = (APTR)(hdr + 1);
        e->nsi_Flags = NETSTATUS_IF_ATTACHED | NETSTATUS_IF_LINKUP | NETSTATUS_IF_SANA2 | NETSTATUS_IF_NAMED;
        strcpy(e->nsi_Name, "wifi0");
        strncpy(e->nsi_Device, h.driver, sizeof(e->nsi_Device) - 1);
        return 1;
    }
    return 0;
}
LONG tool_netstatus_control(struct Library *base, ULONG what, NetStatusControl *ctl, LONG *err)
{
    unsigned i;
    CHECK(base == &library && h.forbidden == 0, "control outside Forbid");
    if (err) *err = 0;
    if (what == NETCTRL_INTERFACE_REMOVE) {
        CHECK(ctl->nsc_Flags == NETCTRL_F_FORCE, "remove rather than logical Offline");
        h.removes++; h.removed_at = ++h.ordinal;
        for (i = 0; i < 4; i++) if (h.alive[i]) h.removed_while_clients++;
        h.attached = FALSE;
        if (h.retained) { if (err) *err = NSD_EINPROGRESS; return -1; }
        CHECK(device.dd_Library.lib_OpenCnt > 0, "stack owns unit until removal");
        device.dd_Library.lib_OpenCnt--;
    } else if (what == NETCTRL_STACK_NOTIFY) h.notify++;
    else if (what == NETCTRL_STACK_RELEASE) { h.releases++; h.running = FALSE; }
    else CHECK(FALSE, "unexpected control request");
    return 0;
}
VOID tool_netstatus_devices(struct Library *base) { CHECK(base == &library, "known path query"); }
BOOL tool_if_device(char *dst, ULONG size, const NetStatusInterface *e)
{
    const char *name = h.missing_name ? "" : (h.whole_name ? h.driver : e->nsi_Device);
    snprintf(dst, size, "%s", name); return h.whole_name;
}
const char *tool_basename(const char *name)
{
    const char *base = name;
    for (; *name; name++) if (*name == ':' || *name == '/') base = name + 1;
    return base;
}
int tool_stricmp(const char *a, const char *b)
{
    while (*a && nw_lower(*a) == nw_lower(*b)) { a++; b++; }
    return nw_lower(*a) - nw_lower(*b);
}
VOID tool_error(const char *fmt, ...)
{
    CHECK(h.forbidden == 0, "diagnostic outside Forbid"); h.errors++;
    if (strstr(fmt, "still open")) h.still_open_errors++;
    if (strstr(fmt, "holds requests")) h.retained_errors++;
    if (strstr(fmt, "cannot verify")) h.metadata_errors++;
}
LONG VPrintf(CONST_STRPTR fmt, APTR args) { (void)fmt; (void)args; CHECK(!h.forbidden, "output outside Forbid"); return 0; }
struct RDArgs *ReadArgs(CONST_STRPTR text, LONG *args, struct RDArgs *rda)
{ (void)text; (void)args; (void)rda; return &rdargs; } /* default TIMEOUT, no pointers */
VOID FreeArgs(struct RDArgs *rda) { CHECK(rda == &rdargs, "known arguments freed"); }
BOOL tool_from_workbench(int argc) { return argc == 0; }
VOID tool_break_arm(VOID) { }
VOID tool_explain_no_netstatus(struct Library *base) { (void)base; CHECK(FALSE, "unexpected missing status"); }
VOID tool_fault(LONG code) { (void)code; CHECK(FALSE, "unexpected DOS fault"); }
VOID tool_usage(const char *usage, const char *description) { (void)usage; (void)description; CHECK(FALSE, "unexpected usage"); }
LONG IoErr(VOID) { return 0; }
struct DosList *LockDosList(ULONG flags) { (void)flags; h.dos_locked++; return NULL; }
struct DosList *FindDosEntry(const struct DosList *list, const UBYTE *name, ULONG flags)
{ (void)list; (void)name; (void)flags; CHECK(h.dos_locked, "DOS list locked"); return NULL; }
VOID UnLockDosList(ULONG flags) { (void)flags; h.dos_locked--; }
LONG DoPkt(struct MsgPort *port, LONG action, ...) { (void)port; (void)action; CHECK(FALSE, "unexpected TCP packet"); return 0; }
/* The shutdown requester must not write preferences or restart a process. */
LONG Delay(ULONG ticks) { (void)ticks; CHECK(FALSE, "unexpected independent WM wait"); return 0; }
LONG SetVar(CONST_STRPTR name, CONST_STRPTR value, LONG size, LONG flags)
{ (void)name; (void)value; (void)size; (void)flags; CHECK(FALSE, "no preferences write"); return 0; }
LONG GetVar(CONST_STRPTR name, STRPTR value, LONG size, LONG flags)
{ (void)name; (void)value; (void)size; (void)flags; CHECK(FALSE, "no preferences read"); return -1; }
LONG DeleteVar(CONST_STRPTR name, LONG flags)
{ (void)name; (void)flags; CHECK(FALSE, "no preferences deletion"); return 0; }
BPTR Open(const char *name, LONG mode) { (void)name; (void)mode; CHECK(FALSE, "no files opened"); return 0; }
LONG Close(BPTR file) { (void)file; CHECK(FALSE, "no files closed"); return 0; }
LONG SystemTags(CONST_STRPTR text, ...) { (void)text; CHECK(FALSE, "no process started"); return -1; }

static VOID test_order(VOID)
{
    reset(); cases++;
    client(0, "NetWireless", TRUE, 1);
    client(1, "NetWirelessPrefs", TRUE, 2);
    client(2, "AmiNetXDuo:C/wirelessmanager", FALSE, 1);
    CHECK(run() == RETURN_OK, "cooperative Wi-Fi shutdown succeeds");
    CHECK(h.signals >= 3 && h.delays == 2, "all clients signalled and awaited");
    CHECK(h.removed_while_clients == 0 && h.removed_at > h.last_client_close,
          "all client device closes precede stack interface removal");
    CHECK(device.dd_Library.lib_OpenCnt == 0 && h.releases == 1 && h.closes == 1,
          "unit fully released and normal stack cleanup completed");
}
static VOID test_budget(VOID)
{
    ULONG waited = 0; BOOL broken = FALSE;
    reset(); cases++; client(0, "NetWireless", TRUE, 99);
    CHECK(!stop_wireless(0, &waited, &broken), "zero budget does not pretend exit");
    CHECK(h.delays == 0 && waited == 0 && !broken, "zero budget never sleeps");
    CHECK(!stop_wireless(2, &waited, &broken), "bounded wait expires");
    CHECK(h.delays == 2 && waited == 2 && !broken, "exact budget, no extra wait");
    reset(); cases++; client(0, "NetWireless", TRUE, 99);
    CHECK(run() == RETURN_WARN && h.delays == NSD_TIMEOUT, "unresponsive client is incomplete, bounded");
    CHECK(h.removes == 1 && h.still_open_errors == 1, "timeout still removes stack but reports remaining unit opener");
}
static VOID test_break(VOID)
{
    reset(); cases++; client(0, "NetWireless", TRUE, 99);
    h.break_at = 1; h.socket_holders = 1;
    CHECK(run() == RETURN_WARN, "Ctrl-C gives incomplete shutdown");
    CHECK(h.delays == 1 && h.removes == 1 && h.releases == 1,
          "Ctrl-C prevents subsequent grace waits without skipping teardown");
}
static VOID test_verdicts(VOID)
{
    unsigned i;
    reset(); cases++; h.retained = TRUE;
    CHECK(run() == RETURN_WARN && h.retained_errors == 1, "retained requests are not a hardware pass");
    CHECK(h.still_open_errors == 1, "retained unit opener remains visible");
    reset(); cases++; device.dd_Library.lib_OpenCnt++;
    CHECK(run() == RETURN_WARN && h.still_open_errors == 1, "unrecognised external device holder is incomplete");
    reset(); cases++; h.whole_name = FALSE;
    h.driver = "Work:long/path/which/cannot/fit/prism2.device";
    CHECK(run() == RETURN_WARN && h.metadata_errors == 1, "truncated path cannot prove hardware closure");
    reset(); cases++; h.missing_name = TRUE;
    CHECK(run() == RETURN_WARN && h.metadata_errors == 1, "missing path cannot prove hardware closure");
    reset(); cases++; h.foreign = TRUE; client(2, "WirelessManager", FALSE, 1);
    CHECK(run() == RETURN_WARN && h.signals == 0 && h.removes == 0, "foreign stack and its Wi-Fi left alone");
    /* A resident command sees changing device names across invocations. */
    for (i = 0; i < 6; i++) {
        reset(); cases++;
        h.driver = h.resident_name = i == 5 ? "other.device" : "prism2.device";
        device.dd_Library.lib_Node.ln_Name = (char *)h.resident_name;
        if (i == 5) device.dd_Library.lib_OpenCnt++;
        CHECK(run() == (i == 5 ? RETURN_WARN : RETURN_OK), "fresh device census on every invocation");
    }
}
static VOID test_identity(VOID)
{
    ULONG waited = 0; BOOL broken = FALSE;
    reset(); cases++;
    client(0, "NetWireless", FALSE, 1);
    client(1, "C:NetWirelessPrefs", FALSE, 1);
    client(2, "C:WIRELESSMANAGER", FALSE, 1);
    client(3, "C:WirelessManagerBackup", FALSE, 1);
    CHECK(stop_wireless(3, &waited, &broken), "legacy CLI clients stop too");
    CHECK(!h.alive[0] && !h.alive[1] && !h.alive[2] && h.alive[3] && !h.requested[3],
          "exact basename matching, not a prefix or case-sensitive guess");
    reset(); cases++; h.running = FALSE;
    client(0, "NetWireless", TRUE, 1);
    CHECK(run() == RETURN_WARN && !h.alive[0] && h.removes == 0,
          "Wi-Fi monitor stopped even when network was already down");
}
int main(VOID)
{
    test_order(); test_budget(); test_break(); test_verdicts(); test_identity();
    printf("netshutdown: %u cases, %u checks, %u failures\n", cases, checks, failures);
    return failures ? 1 : 0;
}

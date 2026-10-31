/* NetEnv: optional, configuration-only ENV: publisher. Polls a resident
 * AmiNetXDuo, closes its base before touching ENV: or waiting. No packets.
 * SPDX-License-Identifier: MIT */
#include "tools.h"
#include "netenv_model.h"
#include "aminetxduo/version.h"
#include <devices/timer.h>
#include <dos/var.h>
#include <exec/memory.h>
#include <libraries/commodities.h>
#include <workbench/startup.h>
#include <workbench/workbench.h>
#include <proto/commodities.h>
#include <proto/icon.h>
#include <string.h>

const char *const tool_name = "NetEnv";
static const char version_tag[] __attribute__((used)) = TOOL_VERSTAG("NetEnv");
struct Library *CxBase;
struct Library *IconBase;
#define TEMPLATE "INTERVAL/K/N,VERSION/S"
enum { ARG_INTERVAL, ARG_VERSION, ARG_COUNT };

static NeSnapshot snapshot;
static NeTable desired;
static NeOwned owned;
/* Static query storage: a Shell/WB tool may have only a 4 KB stack. */
static union {
    struct { NetStatusHeader h; NetStatusInterface e[NE_IF_MAX]; } iface;
    struct { NetStatusHeader h; NetStatusSystem e; } system;
    struct { NetStatusHeader h; NetStatusAddress6 e[NE_ADDR6_MAX]; } addr6;
    struct { NetStatusHeader h; NetStatusRoute e[NE_ROUTE_MAX]; } route;
    struct { NetStatusHeader h; NetStatusRoute6 e[NE_ROUTE_MAX]; } route6;
} answer;

static LONG query(struct Library *base, ULONG what, ULONG size, ULONG entry_size)
{
    LONG count = tool_netstatus_query(base, what, &answer, size, entry_size);
    NetStatusHeader *h = (NetStatusHeader *)&answer;
    if (count < 0 || h->nsh_Available != h->nsh_Count ||
        (ULONG)count > (size - sizeof(*h)) / entry_size) return -1;
    return count;
}

static int scoped6(const ULONG a[4])
{
    return (a[0] & 0xffc00000UL) == 0xfe800000UL;
}

static int collect(struct Library *base)
{
    LONG n, i;
    BOOL ipv6;
    n = query(base, NETSTATUS_SYSTEM, sizeof(answer.system), sizeof(NetStatusSystem));
    if (n != 1) return 0;
    if (!(answer.system.e.nss_Flags & NETSTATUS_SYS_UP)) {
        snapshot.state = "DOWN"; return 1;
    }
    ipv6 = (answer.system.e.nss_Flags & NETSTATUS_SYS_IPV6) != 0;
    n = query(base, NETSTATUS_INTERFACES, sizeof(answer.iface), sizeof(NetStatusInterface));
    if (n < 0) return 0;
    for (i = 0; i < n; i++) {
        const NetStatusInterface *in = &answer.iface.e[i];
        NeInterface *f;
        if ((in->nsi_Flags & (NETSTATUS_IF_ATTACHED | NETSTATUS_IF_SANA2)) !=
            (NETSTATUS_IF_ATTACHED | NETSTATUS_IF_SANA2) ||
            (in->nsi_Flags & NETSTATUS_IF_ALIAS)) continue;
        if (!memchr(in->nsi_Name, 0, sizeof(in->nsi_Name)) ||
            !ne_name_valid(in->nsi_Name)) return 0;
        f = &snapshot.iface[snapshot.interfaces++];
        strcpy(f->name, in->nsi_Name);
        f->index = in->nsi_Index;
        f->up = (in->nsi_Flags & (NETSTATUS_IF_LINKUP | NETSTATUS_IF_ONLINE)) ==
                 (NETSTATUS_IF_LINKUP | NETSTATUS_IF_ONLINE);
        f->address = in->nsi_Address;
        f->mask = in->nsi_NetMask;
    }
    n = query(base, NETSTATUS_ROUTES, sizeof(answer.route), sizeof(NetStatusRoute));
    if (n < 0) return 0;
    for (i = 0; i < n; i++) {
        const NetStatusRoute *in = &answer.route.e[i];
        NeRoute *r;
        if (in->nsr_Destination || in->nsr_NetMask || !in->nsr_Gateway ||
            !(in->nsr_Flags & NETSTATUS_RT_UP)) continue;
        r = &snapshot.route[snapshot.routes++];
        r->index = in->nsr_Interface;
        ami_config_format_ip(in->nsr_Gateway, r->gateway, sizeof(r->gateway));
    }
    if (!ipv6) return 1;
    n = query(base, NETSTATUS_ADDRESSES6, sizeof(answer.addr6), sizeof(NetStatusAddress6));
    if (n < 0) return 0;
    for (i = 0; i < n; i++) {
        const NetStatusAddress6 *in = &answer.addr6.e[i];
        NeAddress6 *a = &snapshot.addr6[snapshot.addresses6++];
        a->index = in->nsn_Interface;
        a->prefix = (unsigned)in->nsn_PrefixLength;
        /* Deprecated addresses remain valid for existing connections, but
           are not advertised as current addresses for new use. */
        a->ready = in->nsn_State == NETSTATUS_IP6_PREFERRED ||
                   in->nsn_State == NETSTATUS_IP6_VALID;
        a->scoped = scoped6(in->nsn_Address);
        tool_format_ip6(in->nsn_Address, a->text, sizeof(a->text));
    }
    n = query(base, NETSTATUS_ROUTES6, sizeof(answer.route6), sizeof(NetStatusRoute6));
    if (n < 0) return 0;
    for (i = 0; i < n; i++) {
        const NetStatusRoute6 *in = &answer.route6.e[i];
        NeRoute *r;
        if (in->nsr6_PrefixLength || in->nsr6_Destination[0] ||
            in->nsr6_Destination[1] || in->nsr6_Destination[2] ||
            in->nsr6_Destination[3] || !in->nsr6_Lifetime ||
            !(in->nsr6_Flags & NETSTATUS_RT6_UP) ||
            !(in->nsr6_NextHop[0] | in->nsr6_NextHop[1] |
              in->nsr6_NextHop[2] | in->nsr6_NextHop[3])) continue;
        if (snapshot.routes == NE_ROUTE_MAX) return 0;
        r = &snapshot.route[snapshot.routes++];
        r->index = in->nsr6_Interface; r->ipv6 = 1;
        r->scoped = scoped6(in->nsr6_NextHop);
        tool_format_ip6(in->nsr6_NextHop, r->gateway, sizeof(r->gateway));
    }
    return 1;
}

static void refresh(void)
{
    struct Library *base;
    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.state = "DOWN";
    base = tool_netstatus_open_resident();
    if (base) {
        snapshot.state = "UP";
        if (!collect(base)) snapshot.state = "ERROR";
        tool_netstatus_close(base);
    }
    if (!ne_build(&snapshot, &desired)) {
        memset(&snapshot, 0, sizeof(snapshot));
        snapshot.state = "ERROR";
        (void)ne_build(&snapshot, &desired);
    }
}

static int env_read(void *ctx, const char *name, char *value, size_t size)
{
    LONG n;
    (void)ctx;
    n = GetVar((CONST_STRPTR)name, (STRPTR)value, (LONG)size, GVF_GLOBAL_ONLY);
    if (n < 0 || (ULONG)n >= size) return 0;
    value[n] = 0;
    return 1;
}

static int env_write(void *ctx, const char *name, const char *value)
{
    char path[40] = "ENV:", *p;
    (void)ctx;
    if (value) {
        strcat(path, name);
        for (p = path + 4; *p; p++) {
            BPTR lock;
            if (*p != '/') continue;
            *p = 0;
            lock = Lock((CONST_STRPTR)path, ACCESS_READ);
            if (!lock) lock = CreateDir((CONST_STRPTR)path);
            *p = '/';
            if (!lock) return 0;
            UnLock(lock);
        }
        return SetVar((CONST_STRPTR)name, (CONST_STRPTR)value, -1, GVF_GLOBAL_ONLY) != 0;
    }
    if (!DeleteVar((CONST_STRPTR)name, GVF_GLOBAL_ONLY) &&
        IoErr() != ERROR_OBJECT_NOT_FOUND) return 0;
    /* Only empty directories, and only inside our namespace. */
    strcat(path, name);
    p = strrchr(path, '/');
    if (p) { *p = 0; (void)DeleteFile((CONST_STRPTR)path); }
    return 1;
}

/* Recover only the documented keys after an earlier abnormal termination.
 * Never recursively delete unknown files or touch ENVARC:. */
static void recover(void)
{
    static const char *const global[] = {
        "ANXD/STATE", "ANXD/INTERFACES", "ANXD/DEFAULT/INTERFACE",
        "ANXD/DEFAULT/ADDRESS", "ANXD/DEFAULT/NETMASK", "ANXD/DEFAULT/GATEWAY",
        "ANXD/DEFAULT6/INTERFACE", "ANXD/DEFAULT6/GATEWAY"
    };
    static const char *const fields[] = {"ADDRESS", "NETMASK", "STATE", "ADDR6"};
    char list[NE_IF_MAX * 16], key[32], *name, *end;
    struct FileInfoBlock *fib;
    BPTR directory;
    unsigned i;
    for (i = 0; i < sizeof(global)/sizeof(global[0]); i++) (void)ne_own(&owned, global[i]);
    directory = Lock((CONST_STRPTR)"ENV:ANXD/IF", ACCESS_READ);
    fib = (struct FileInfoBlock *)AllocDosObject(DOS_FIB, NULL);
    if (directory && fib && Examine(directory, fib)) {
        while (ExNext(directory, fib)) {
            const char *saved = (const char *)fib->fib_FileName;
            if (fib->fib_DirEntryType <= 0 || !ne_name_valid(saved)) continue;
            for (i = 0; i < 4; i++) {
                strcpy(key, "ANXD/IF/"); strcat(key, saved); strcat(key, "/"); strcat(key, fields[i]);
                if (!ne_own(&owned, key)) {
                    tool_error("too many old interface exports to recover");
                    break;
                }
            }
        }
    }
    if (fib) FreeDosObject(DOS_FIB, fib);
    if (directory) UnLock(directory);
    if (!env_read(NULL, "ANXD/INTERFACES", list, sizeof(list))) return;
    name = list;
    while (*name) {
        end = name;
        while (*end && *end != ' ') end++;
        if (*end) *end++ = 0;
        if (ne_name_valid(name)) for (i = 0; i < 4; i++) {
            strcpy(key, "ANXD/IF/"); strcat(key, name); strcat(key, "/"); strcat(key, fields[i]);
            (void)ne_own(&owned, key);
        }
        name = end;
    }
}

int main(int argc, char **argv)
{
    LONG args[ARG_COUNT] = {0};
    struct RDArgs *rda = NULL;
    struct MsgPort *port = NULL, *timer_port = NULL;
    struct timerequest *timer = NULL;
    struct DiskObject *icon = NULL;
    CxObj *broker = NULL;
    LONG interval = 2, rc = RETURN_FAIL;
    BOOL timer_open = FALSE, armed = FALSE, run = TRUE, enabled = TRUE, failed = FALSE;
    struct NewBroker nb = {
        NB_VERSION, (STRPTR)"NetEnv", (STRPTR)"NetEnv",
        (STRPTR)"Network configuration in ENV:", NBU_UNIQUE | NBU_NOTIFY,
        0, 0, NULL, 0
    };
    if (argc == 0) {
        struct WBStartup *wb = (struct WBStartup *)argv;
        IconBase = OpenLibrary((CONST_STRPTR)"icon.library", 37);
        if (IconBase && wb->sm_NumArgs) {
            BPTR old = CurrentDir(wb->sm_ArgList[0].wa_Lock);
            icon = GetDiskObject(wb->sm_ArgList[0].wa_Name);
            (void)CurrentDir(old);
            if (icon) {
                const char *text = (const char *)FindToolType(
                    (CONST_STRPTR *)icon->do_ToolTypes, (CONST_STRPTR)"INTERVAL");
                if (text && StrToLong((CONST_STRPTR)text, &interval) != (LONG)strlen(text)) interval = 0;
            }
        }
    } else {
        rda = ReadArgs((CONST_STRPTR)TEMPLATE, args, NULL);
        if (!rda) { tool_fault(IoErr()); goto out; }
        if (args[ARG_VERSION]) { tool_printf("%s\n", (LONG)(version_tag + 7)); rc = RETURN_OK; goto out; }
        if (args[ARG_INTERVAL]) interval = *(LONG *)args[ARG_INTERVAL];
    }
    if (interval < 1 || interval > 3600) { tool_error("INTERVAL must be 1 to 3600 seconds"); goto out; }
    CxBase = OpenLibrary((CONST_STRPTR)"commodities.library", 37);
    if (!CxBase || !(port = CreateMsgPort())) { tool_error("cannot create commodity"); goto out; }
    nb.nb_Port = port;
    broker = CxBroker(&nb, NULL);
    if (!broker) { tool_error("NetEnv is already running, or the broker could not be created"); goto out; }
    if (!(timer_port = CreateMsgPort()) ||
        !(timer = (struct timerequest *)CreateIORequest(timer_port, sizeof(*timer)))) {
        tool_error("cannot allocate timer request"); goto out;
    }
    if (OpenDevice((CONST_STRPTR)TIMERNAME, UNIT_VBLANK, (struct IORequest *)timer, 0)) {
        tool_error("cannot open timer.device"); goto out;
    }
    timer_open = TRUE;
    (void)ActivateCxObj(broker, 1);
    recover();
    rc = RETURN_OK;
    while (run) {
        ULONG signals;
        CxMsg *msg;
        if (enabled) refresh();
        else desired.count = desired.used = 0;
        {
            BOOL ok = ne_publish(&owned, &desired, env_read, env_write, NULL);
            if (!ok && !failed) tool_error("cannot update ENV:; will retry");
            if (!ok && enabled) (void)env_write(NULL, "ANXD/STATE", "ERROR");
            failed = !ok;
        }
        timer->tr_node.io_Command = TR_ADDREQUEST;
        timer->tr_time.tv_secs = (ULONG)interval;
        timer->tr_time.tv_micro = 0;
        SendIO((struct IORequest *)timer); armed = TRUE;
        /* Commodity messages do not bring the next timed refresh forward. */
        for (;;) {
            signals = Wait(SIGBREAKF_CTRL_C | (1UL << port->mp_SigBit) |
                           (1UL << timer_port->mp_SigBit));
            if (signals & SIGBREAKF_CTRL_C) run = FALSE;
            while ((msg = (CxMsg *)GetMsg(port))) {
                ULONG type = CxMsgType(msg); LONG id = CxMsgID(msg);
                ReplyMsg((struct Message *)msg);
                if (type == CXM_COMMAND) {
                    if (id == CXCMD_KILL) run = FALSE;
                    if (id == CXCMD_DISABLE) {
                        (void)ActivateCxObj(broker, 0);
                        enabled = FALSE; desired.count = desired.used = 0;
                        if (!ne_publish(&owned, &desired, env_read, env_write, NULL))
                            tool_error("cannot clear ENV:");
                    }
                    if (id == CXCMD_ENABLE) {
                        (void)ActivateCxObj(broker, 1);
                        enabled = TRUE;
                    }
                }
            }
            if (!run || (signals & (1UL << timer_port->mp_SigBit))) break;
        }
        if (!CheckIO((struct IORequest *)timer)) AbortIO((struct IORequest *)timer);
        (void)WaitIO((struct IORequest *)timer); armed = FALSE;
    }
    desired.count = desired.used = 0;
    if (!ne_publish(&owned, &desired, env_read, env_write, NULL)) {
        tool_error("could not remove all exported variables"); rc = RETURN_ERROR;
    }
    (void)DeleteFile((CONST_STRPTR)"ENV:ANXD/IF");
    (void)DeleteFile((CONST_STRPTR)"ENV:ANXD");
out:
    if (armed) {
        if (!CheckIO((struct IORequest *)timer)) AbortIO((struct IORequest *)timer);
        (void)WaitIO((struct IORequest *)timer);
    }
    if (timer_open) CloseDevice((struct IORequest *)timer);
    if (timer) DeleteIORequest((struct IORequest *)timer);
    if (timer_port) DeleteMsgPort(timer_port);
    if (broker) DeleteCxObjAll(broker);
    if (port) {
        struct Message *pending;
        while ((pending = GetMsg(port))) ReplyMsg(pending);
        DeleteMsgPort(port);
    }
    if (CxBase) CloseLibrary(CxBase);
    if (icon) FreeDiskObject(icon);
    if (IconBase) CloseLibrary(IconBase);
    if (rda) FreeArgs(rda);
    return (int)rc;
}

/*
 * CreateAmiNetXDuoStatusReport, a support snapshot for a bug report.
 *
 * PASSIVE, AND THAT IS THE CONTRACT.  It opens no device, loads no driver,
 * sends nothing, and starts no stack except in the NetShutdown race below.
 * Opening the resident bsdsocket.library still runs its per-opener setup,
 * which reads the netdb files and can load usergroup.library from LIBS:.
 * A NetShutdown that completes between the check and the open gives a
 * loopback-only start (tool_diag.c).
 * What it reads:
 *
 *   Exec's own lists      FindName() on LibList and DeviceList, FindPort(),
 *                         FindSemaphore(), all under Forbid()
 *   published records     the anx drivers' probe records and the stack's
 *                         event ring, through tool_passive.c
 *   the running stack     NetStackQuery(), through a bsdsocket.library that
 *                         is already resident, ours, and has its stack up --
 *                         tool_netstatus_open_resident(), never the opener
 *                         the other commands use
 *   the interface files   DEVS:NetInterfaces, printed through the allowlist
 *                         in statusreport_text.c and never copied out
 *
 * A value none of those can give is printed as `unavailable`, not guessed.
 * CheckNetDevice's other path, the one that opens a driver so that it
 * probes, must never be reached from here: it once loaded anxzz9000.device
 * onto a ZZ9000 its own driver held and took that interface down.
 * test/test_statusreport.c holds this file and the ones it links to that.
 *
 * SPDX-License-Identifier: MIT
 */

#include "tool_passive.h"

#include <stddef.h>

#include <exec/execbase.h>
#include <exec/memory.h>

#include "config_internal.h"
#include "statusreport_text.h"

#include "aminetxduo/version.h"

const char *const tool_name = "CreateAmiNetXDuoStatusReport";

static const char version_tag[] __attribute__((used)) =
    TOOL_VERSTAG("CreateAmiNetXDuoStatusReport");

#define TEMPLATE    "TO/K,NOFILE/S,ADDRESSES/S"

enum
{
    ARG_TO = 0,
    ARG_NOFILE,
    ARG_ADDRESSES,
    ARG_COUNT
};

#define SR_DEFAULT_FILE     "T:AmiNetXDuoStatusReport.txt"

extern struct ExecBase *SysBase;

/* ---------------------------------------------------------------- output --- */

static BPTR  sr_console;
static BPTR  sr_file;
static LONG  sr_file_error;
static SrTee sr_tee;

/* An SrPut: `handle` points at one of the two BPTRs above. */
static LONG sr_put(APTR handle, const char *line)
{
    if (FPuts(*(BPTR *)handle, (CONST_STRPTR)line) == 0)
        return 0;
    if (handle == (APTR)&sr_file && sr_file_error == 0)
        sr_file_error = IoErr();
    return -1;
}

static SrOut sr_out;
static char  sr_k[SR_KEY_MAX];

/* tool_break() consumes the signal, and the report asks between every
   section, so the first answer is kept. */
static BOOL sr_broken;

static BOOL sr_stop(VOID)
{
    if (!sr_broken && tool_break())
        sr_broken = TRUE;
    return sr_broken;
}

/* ---------------------------------------------------------------- system --- */

static VOID sr_system(VOID)
{
    struct Library *lib;
    ULONG           wb_ver = 0;
    ULONG           wb_rev = 0;
    BOOL            wb     = FALSE;

    sr_version(&sr_out, "system.exec",
               SysBase->LibNode.lib_Version, SysBase->LibNode.lib_Revision);
    sr_version(&sr_out, "system.kickstart",
               SysBase->LibNode.lib_Version, SysBase->SoftVer);

    /* version.library says which Workbench, and only when something has
       already loaded it: opening it would read it from LIBS:. */
    Forbid();
    lib = (struct Library *)FindName(&SysBase->LibList,
                                     (CONST_STRPTR)"version.library");
    if (lib != NULL)
    {
        wb_ver = lib->lib_Version;
        wb_rev = lib->lib_Revision;
        wb     = TRUE;
    }
    Permit();

    if (wb)
        sr_version(&sr_out, "system.workbench", wb_ver, wb_rev);
    else
        sr_str(&sr_out, "system.workbench", NULL);

    sr_str(&sr_out, "system.cpu", sr_cpu_name(SysBase->AttnFlags));
    sr_str(&sr_out, "system.fpu", sr_fpu_name(SysBase->AttnFlags));
    sr_hex(&sr_out, "system.attnflags", SysBase->AttnFlags);

    sr_ulong(&sr_out, "system.chip_free", AvailMem(MEMF_CHIP));
    sr_ulong(&sr_out, "system.chip_largest", AvailMem(MEMF_CHIP | MEMF_LARGEST));
    sr_ulong(&sr_out, "system.fast_free", AvailMem(MEMF_FAST));
    sr_ulong(&sr_out, "system.fast_largest", AvailMem(MEMF_FAST | MEMF_LARGEST));
}

/* ------------------------------------------------------------- libraries --- */

#define SR_ID_LEN   80

/* A resident module as Exec knows it, copied under Forbid(): it can expunge
   the moment Forbid() ends. */
typedef struct SrModule
{
    char  name[TOOL_NAME_LEN];
    char  id[SR_ID_LEN];
    UWORD version;
    UWORD revision;
    BOOL  ours;
} SrModule;

static VOID sr_copy_id(char *dst, const char *id)
{
    ULONG i = 0;

    if (id != NULL)
    {
        /* lib_IdString ends "\r\n" by convention. */
        for (; i + 1UL < SR_ID_LEN && id[i] != '\0' &&
               id[i] != '\r' && id[i] != '\n'; i++)
            dst[i] = id[i];
    }
    dst[i] = '\0';
}

static BOOL sr_find_library(const char *name, SrModule *m)
{
    struct Library *lib;
    BOOL            found = FALSE;

    Forbid();
    lib = (struct Library *)FindName(&SysBase->LibList, (CONST_STRPTR)name);
    if (lib != NULL)
    {
        tool_copy_string(m->name, sizeof(m->name), name);
        sr_copy_id(m->id, (const char *)lib->lib_IdString);
        m->version  = lib->lib_Version;
        m->revision = lib->lib_Revision;
        m->ours     = tool_stack_is_ours(lib);
        found       = TRUE;
    }
    Permit();

    return found;
}

static SrModule sr_lib;

static VOID sr_library(const char *name, BOOL say_ours)
{
    char part[TOOL_NAME_LEN];
    BOOL found = sr_find_library(name, &sr_lib);

    sr_key_part(part, sizeof(part), name);

    sr_key(sr_k, sizeof(sr_k), "library", part, "resident");
    sr_yesno(&sr_out, sr_k, found);

    sr_key(sr_k, sizeof(sr_k), "library", part, "version");
    if (found)
        sr_version(&sr_out, sr_k, sr_lib.version, sr_lib.revision);
    else
        sr_str(&sr_out, sr_k, NULL);

    sr_key(sr_k, sizeof(sr_k), "library", part, "id");
    sr_str(&sr_out, sr_k, (found && sr_lib.id[0] != '\0') ? sr_lib.id : NULL);

    if (say_ours && found)
    {
        sr_key(sr_k, sizeof(sr_k), "library", part, "ours");
        sr_yesno(&sr_out, sr_k, sr_lib.ours);
    }
}

/* --------------------------------------------------------- the live stack --- */

/* A counter row: where it is in the record and what the report calls it. */
typedef struct SrField
{
    const char *key;
    UWORD       offset;
} SrField;

#define SR_F(k, type, field)    { k, (UWORD)offsetof(type, field) }

static const SrField sr_if_fields[] =
{
    SR_F("mtu",            NetStatusInterface, nsi_MTU),
    SR_F("speed",          NetStatusInterface, nsi_Speed),
    SR_F("unit",           NetStatusInterface, nsi_Unit),
    SR_F("packets_in",     NetStatusInterface, nsi_PacketsIn),
    SR_F("packets_out",    NetStatusInterface, nsi_PacketsOut),
    SR_F("bad_data",       NetStatusInterface, nsi_BadData),
    SR_F("overruns",       NetStatusInterface, nsi_Overruns),
    SR_F("unknown_types",  NetStatusInterface, nsi_UnknownTypes),
    SR_F("reconfigurations", NetStatusInterface, nsi_Reconfigurations),
    SR_F("tx_errors",      NetStatusInterface, nsi_TxErrors),
    SR_F("rx_errors",      NetStatusInterface, nsi_RxErrors),
    SR_F("alloc_failures", NetStatusInterface, nsi_AllocFailures),
    SR_F("collisions",     NetStatusInterface, nsi_Collisions),
    SR_F("tx_underruns",   NetStatusInterface, nsi_TxUnderruns),
    SR_F("chip_resets",    NetStatusInterface, nsi_ChipResets),
    SR_F("tx_wedges",      NetStatusInterface, nsi_TxWedges),
    SR_F("driver_tx_errors", NetStatusInterface, nsi_DrvTxErrors),
    { NULL, 0 }
};

static const SrField sr_sys_fields[] =
{
    SR_F("interfaces",     NetStatusSystem, nss_InterfaceCount),
    SR_F("pool_total",     NetStatusSystem, nss_PoolTotal),
    SR_F("pool_free",      NetStatusSystem, nss_PoolFree),
    SR_F("pool_payload",   NetStatusSystem, nss_PoolPayload),
    SR_F("pool_empty_requests", NetStatusSystem, nss_PoolEmptyRequests),
    SR_F("pool_empty_suspensions", NetStatusSystem, nss_PoolEmptySuspensions),
    SR_F("pool_invalid_releases", NetStatusSystem, nss_PoolInvalidReleases),
    SR_F("host_source",    NetStatusSystem, nss_HostSource),
    SR_F("openers",        NetStatusSystem, nss_Openers),
    SR_F("open_count",     NetStatusSystem, nss_OpenCnt),
    { NULL, 0 }
};

static const SrField sr_stats_fields[] =
{
    SR_F("ip_packets_sent",       NetStatusStats, nsx_IpPacketsSent),
    SR_F("ip_packets_received",   NetStatusStats, nsx_IpPacketsReceived),
    SR_F("ip_invalid",            NetStatusStats, nsx_IpInvalid),
    SR_F("ip_receive_dropped",    NetStatusStats, nsx_IpReceiveDropped),
    SR_F("ip_checksum_errors",    NetStatusStats, nsx_IpChecksumErrors),
    SR_F("ip_send_dropped",       NetStatusStats, nsx_IpSendDropped),
    SR_F("icmp_checksum_errors",  NetStatusStats, nsx_IcmpChecksumErrors),
    SR_F("icmp_unhandled",        NetStatusStats, nsx_IcmpUnhandled),
    SR_F("tcp_packets_sent",      NetStatusStats, nsx_TcpPacketsSent),
    SR_F("tcp_packets_received",  NetStatusStats, nsx_TcpPacketsReceived),
    SR_F("tcp_invalid",           NetStatusStats, nsx_TcpInvalid),
    SR_F("tcp_receive_dropped",   NetStatusStats, nsx_TcpReceiveDropped),
    SR_F("tcp_checksum_errors",   NetStatusStats, nsx_TcpChecksumErrors),
    SR_F("tcp_connections",       NetStatusStats, nsx_TcpConnections),
    SR_F("tcp_disconnections",    NetStatusStats, nsx_TcpDisconnections),
    SR_F("tcp_connections_dropped", NetStatusStats, nsx_TcpConnectionsDropped),
    SR_F("tcp_retransmits",       NetStatusStats, nsx_TcpRetransmits),
    SR_F("udp_packets_sent",      NetStatusStats, nsx_UdpPacketsSent),
    SR_F("udp_packets_received",  NetStatusStats, nsx_UdpPacketsReceived),
    SR_F("udp_invalid",           NetStatusStats, nsx_UdpInvalid),
    SR_F("udp_receive_dropped",   NetStatusStats, nsx_UdpReceiveDropped),
    SR_F("udp_checksum_errors",   NetStatusStats, nsx_UdpChecksumErrors),
    SR_F("arp_requests_sent",     NetStatusStats, nsx_ArpRequestsSent),
    SR_F("arp_requests_received", NetStatusStats, nsx_ArpRequestsReceived),
    SR_F("arp_responses_sent",    NetStatusStats, nsx_ArpResponsesSent),
    SR_F("arp_responses_received", NetStatusStats, nsx_ArpResponsesReceived),
    SR_F("arp_invalid_messages",  NetStatusStats, nsx_ArpInvalidMessages),
    { NULL, 0 }
};

static const SrField sr_health_fields[] =
{
    SR_F("uptime_ms",        NetStatusHealth, nsl_TickUptimeMs),
    SR_F("tick_lost",        NetStatusHealth, nsl_TickLost),
    SR_F("tick_clipped",     NetStatusHealth, nsl_TickClipped),
    SR_F("tick_worst_stall_ms", NetStatusHealth, nsl_TickWorstStallMs),
    SR_F("tick_over_budget", NetStatusHealth, nsl_TickOverBudget),
    SR_F("alloc_live",       NetStatusHealth, nsl_AllocLive),
    SR_F("alloc_peak",       NetStatusHealth, nsl_AllocPeak),
    SR_F("alloc_refused",    NetStatusHealth, nsl_AllocRefused),
    SR_F("sockets",          NetStatusHealth, nsl_Sockets),
    SR_F("sockets_peak",     NetStatusHealth, nsl_SocketsPeak),
    SR_F("opens",            NetStatusHealth, nsl_Opens),
    SR_F("pool_low",         NetStatusHealth, nsl_PoolLow),
    SR_F("pool_empty",       NetStatusHealth, nsl_PoolEmpty),
    SR_F("pool_waited",      NetStatusHealth, nsl_PoolWaited),
    SR_F("pool_bad_release", NetStatusHealth, nsl_PoolBadRelease),
    { NULL, 0 }
};

static VOID sr_fields(const char *prefix, const SrField *f, const APTR rec)
{
    for (; f->key != NULL; f++)
    {
        sr_key(sr_k, sizeof(sr_k), prefix, f->key, NULL);
        sr_ulong(&sr_out, sr_k,
                 *(const ULONG *)((const UBYTE *)rec + f->offset));
    }
}

/* One buffer for every query, one query at a time.  Static: a Shell gives a
   command 4096 bytes of stack. */
static union
{
    struct { NetStatusHeader h; NetStatusSystem    e;    } sys;
    struct { NetStatusHeader h; NetStatusInterface e[NX_MAX_PHYSICAL_INTERFACES]; } ifs;
    struct { NetStatusHeader h; NetStatusDhcp      e[NX_MAX_PHYSICAL_INTERFACES]; } dhcp;
    struct { NetStatusHeader h; NetStatusStats     e;    } stats;
    struct { NetStatusHeader h; NetStatusHealth    e;    } health;
} sr_q;

/* The live interfaces' names by index, for the DHCP rows, which carry only
   the index. */
static char sr_live_name[NX_MAX_PHYSICAL_INTERFACES][NETSTATUS_NAME_LEN];

/* The drivers the running stack names, for the device section. */
#define SR_MAX_NAMED    16
static char  sr_named[SR_MAX_NAMED][TOOL_NAME_LEN];
static ULONG sr_named_count;

static VOID sr_name_device(const char *path)
{
    const char *base = tool_basename(path);
    ULONG       i;

    if (base == NULL || *base == '\0')
        return;

    for (i = 0; i < sr_named_count; i++)
    {
        if (tool_stricmp(sr_named[i], base) == 0)
            return;
    }
    if (sr_named_count < SR_MAX_NAMED)
        tool_copy_string(sr_named[sr_named_count++], TOOL_NAME_LEN, base);
}

static const char *sr_dhcp_state(UWORD state)
{
    switch (state)
    {
    case NETSTATUS_DHCP_OFF:     return "off";
    case NETSTATUS_DHCP_WORKING: return "working";
    case NETSTATUS_DHCP_BOUND:   return "bound";
    default:                     break;
    }
    return NULL;
}

static VOID sr_live(struct Library *base)
{
    static char host[NETSTATUS_HOSTNAME_LEN];
    char        part[NETSTATUS_NAME_LEN + 8];
    LONG        n;
    LONG        i;

    /* ------------------------------------------------- the whole stack */
    n = tool_netstatus_query(base, NETSTATUS_SYSTEM, &sr_q.sys,
                             sizeof(sr_q.sys), sizeof(NetStatusSystem));
    if (n > 0)
    {
        const NetStatusSystem *s = &sr_q.sys.e;

        sr_yesno(&sr_out, "stack.up",      (s->nss_Flags & NETSTATUS_SYS_UP) != 0);
        sr_yesno(&sr_out, "stack.ipv6",    (s->nss_Flags & NETSTATUS_SYS_IPV6) != 0);
        sr_yesno(&sr_out, "stack.routing", (s->nss_Flags & NETSTATUS_SYS_ROUTING) != 0);
        sr_yesno(&sr_out, "stack.mdns",    (s->nss_Flags & NETSTATUS_SYS_MDNS) != 0);
        sr_fields("stack", sr_sys_fields, (APTR)s);

        if (s->nss_Flags & NETSTATUS_SYS_GATEWAY)
            sr_addr_ipv4(&sr_out, "stack.gateway", s->nss_Gateway);
        if (s->nss_Flags & NETSTATUS_SYS_MDNS)
            sr_addr_str(&sr_out, "stack.mdns_name", s->nss_MdnsName);
    }
    else
    {
        sr_str(&sr_out, "stack.up", NULL);
    }

    if (sr_out.addresses)
    {
        if (tool_stack_hostname(base, host, sizeof(host)) == 0 && host[0] != '\0')
            sr_addr_str(&sr_out, "stack.hostname", host);
        else
            sr_addr_str(&sr_out, "stack.hostname", NULL);
    }

    /* ------------------------------------------------- the interfaces */
    n = tool_netstatus_query(base, NETSTATUS_INTERFACES, &sr_q.ifs,
                             sizeof(sr_q.ifs), sizeof(NetStatusInterface));
    if (n < 0)
    {
        sr_str(&sr_out, "live.interfaces", NULL);
    }
    else
    {
        sr_ulong(&sr_out, "live.interfaces", (ULONG)n);
        tool_netstatus_devices(base);

        for (i = 0; i < n && i < (LONG)NX_MAX_PHYSICAL_INTERFACES; i++)
        {
            const NetStatusInterface *e = &sr_q.ifs.e[i];
            char   name[NETSTATUS_NAME_LEN + 1];
            static char device[NETSTATUS_FILE_LEN];  /* 4 KB Shell stack */
            char   prefix[NETSTATUS_NAME_LEN + 8];

            tool_copy_string(name, sizeof(name), e->nsi_Name);
            tool_if_device(device, sizeof(device), e);

            if (name[0] != '\0')
                sr_key_part(part, sizeof(part), name);
            else
                sr_key_index(part, sizeof(part), "if", e->nsi_Index, NULL);

            if (e->nsi_Index < NX_MAX_PHYSICAL_INTERFACES)
                tool_copy_string(sr_live_name[e->nsi_Index],
                                 NETSTATUS_NAME_LEN, part);

            sr_key(prefix, sizeof(prefix), "live", part, NULL);

            sr_key(sr_k, sizeof(sr_k), prefix, "index", NULL);
            sr_ulong(&sr_out, sr_k, e->nsi_Index);
            sr_key(sr_k, sizeof(sr_k), prefix, "device", NULL);
            sr_str(&sr_out, sr_k, device[0] != '\0' ? device : NULL);
            sr_key(sr_k, sizeof(sr_k), prefix, "attached", NULL);
            sr_yesno(&sr_out, sr_k, (e->nsi_Flags & NETSTATUS_IF_ATTACHED) != 0);
            sr_key(sr_k, sizeof(sr_k), prefix, "link", NULL);
            sr_yesno(&sr_out, sr_k, (e->nsi_Flags & NETSTATUS_IF_LINKUP) != 0);
            sr_key(sr_k, sizeof(sr_k), prefix, "sana2", NULL);
            sr_yesno(&sr_out, sr_k, (e->nsi_Flags & NETSTATUS_IF_SANA2) != 0);
            sr_key(sr_k, sizeof(sr_k), prefix, "online", NULL);
            sr_yesno(&sr_out, sr_k, (e->nsi_Flags & NETSTATUS_IF_ONLINE) != 0);
            sr_key(sr_k, sizeof(sr_k), prefix, "priority", NULL);
            sr_long(&sr_out, sr_k, (LONG)e->nsi_Priority);
            sr_fields(prefix, sr_if_fields, (APTR)e);

            sr_key(sr_k, sizeof(sr_k), prefix, "address", NULL);
            sr_addr_ipv4(&sr_out, sr_k, e->nsi_Address);
            sr_key(sr_k, sizeof(sr_k), prefix, "netmask", NULL);
            sr_addr_ipv4(&sr_out, sr_k, e->nsi_NetMask);
            sr_key(sr_k, sizeof(sr_k), prefix, "hardwareaddress", NULL);
            sr_addr_mac(&sr_out, sr_k, e->nsi_HwAddress);

            if (device[0] != '\0')
                sr_name_device(device);
        }
    }

    /* ------------------------------------------------- the leases */
    n = tool_netstatus_query(base, NETSTATUS_DHCP, &sr_q.dhcp,
                             sizeof(sr_q.dhcp), sizeof(NetStatusDhcp));
    if (n < 0)
    {
        sr_str(&sr_out, "dhcp", NULL);
    }
    else
    {
        for (i = 0; i < n && i < (LONG)NX_MAX_PHYSICAL_INTERFACES; i++)
        {
            const NetStatusDhcp *d = &sr_q.dhcp.e[i];
            char   prefix[NETSTATUS_NAME_LEN + 8];

            if (d->nsd_Index < NX_MAX_PHYSICAL_INTERFACES &&
                sr_live_name[d->nsd_Index][0] != '\0')
                tool_copy_string(part, sizeof(part), sr_live_name[d->nsd_Index]);
            else
                sr_key_index(part, sizeof(part), "if", d->nsd_Index, NULL);

            sr_key(prefix, sizeof(prefix), "dhcp", part, NULL);

            sr_key(sr_k, sizeof(sr_k), prefix, "state", NULL);
            sr_str(&sr_out, sr_k, sr_dhcp_state(d->nsd_State));
            sr_key(sr_k, sizeof(sr_k), prefix, "raw_state", NULL);
            sr_ulong(&sr_out, sr_k, d->nsd_RawState);

            if (d->nsd_State != NETSTATUS_DHCP_BOUND)
                continue;

            sr_key(sr_k, sizeof(sr_k), prefix, "lease_seconds", NULL);
            sr_ulong(&sr_out, sr_k, d->nsd_LeaseSeconds);
            sr_key(sr_k, sizeof(sr_k), prefix, "address", NULL);
            sr_addr_ipv4(&sr_out, sr_k, d->nsd_Address);
            sr_key(sr_k, sizeof(sr_k), prefix, "server", NULL);
            sr_addr_ipv4(&sr_out, sr_k, d->nsd_Server);
            if (d->nsd_RouterCount > 0)
            {
                sr_key(sr_k, sizeof(sr_k), prefix, "router", NULL);
                sr_addr_ipv4(&sr_out, sr_k, d->nsd_Router[0]);
            }
            if (d->nsd_DnsCount > 0)
            {
                sr_key(sr_k, sizeof(sr_k), prefix, "dns", NULL);
                sr_addr_ipv4(&sr_out, sr_k, d->nsd_Dns[0]);
            }
        }
    }

    /* ------------------------------------------------- the counters */
    n = tool_netstatus_query(base, NETSTATUS_STATS, &sr_q.stats,
                             sizeof(sr_q.stats), sizeof(NetStatusStats));
    if (n > 0)
        sr_fields("stats", sr_stats_fields, (APTR)&sr_q.stats.e);
    else
        sr_str(&sr_out, "stats", NULL);

    n = tool_netstatus_query(base, NETSTATUS_HEALTH, &sr_q.health,
                             sizeof(sr_q.health), sizeof(NetStatusHealth));
    if (n > 0)
        sr_fields("health", sr_health_fields, (APTR)&sr_q.health.e);
    else
        sr_str(&sr_out, "health", NULL);
}

static VOID sr_stack(VOID)
{
    struct Library *base;
    BOOL            running = tool_stack_library_running();

    sr_yesno(&sr_out, "stack.running", running);

    /* Neither is in anything the library publishes; the file's $VER is,
       and reading that means the file, not the library in memory. */
    sr_str(&sr_out, "stack.profile", NULL);
    sr_str(&sr_out, "stack.build", NULL);

    base = running ? tool_netstatus_open_resident() : NULL;

    if (base == NULL)
    {
        /* Why not, in one word a script can test. */
        const char *why = "not_running";

        if (running)
        {
            if (!sr_find_library("bsdsocket.library", &sr_lib))
                why = "not_resident";
            else if (!sr_lib.ours)
                why = "foreign";
            else
                why = "too_old";
        }
        sr_str(&sr_out, "stack.status", why);
        sr_str(&sr_out, "stack.up", NULL);
        sr_str(&sr_out, "live.interfaces", NULL);
        sr_str(&sr_out, "dhcp", NULL);
        sr_str(&sr_out, "stats", NULL);
        sr_str(&sr_out, "health", NULL);
        return;
    }

    sr_str(&sr_out, "stack.status", "queried");
    sr_live(base);
    tool_netstatus_close(base);
}

/* ------------------------------------------------------ interface files --- */

#define SR_MAX_IFACES   16

static char sr_ifnames[SR_MAX_IFACES][TOOL_NAME_LEN];

static VOID sr_config(VOID)
{
    static char dir[AMI_CFG_PATH_LEN];
    static char path[AMI_CFG_PATH_LEN + TOOL_NAME_LEN];
    char        device[TOOL_NAME_LEN];
    const char *where;
    ULONG       count;
    ULONG       i;

    where = ami_cfg_resolve(AMI_CFG_DIR_NETINTERFACES, dir, sizeof(dir));

    if (!tool_exists(where))
    {
        sr_str(&sr_out, "config.interfaces", NULL);
        return;
    }

    count = tool_list_dir(where, sr_ifnames, SR_MAX_IFACES, NULL);
    sr_ulong(&sr_out, "config.interfaces", count);

    for (i = 0; i < count && !sr_stop(); i++)
    {
        char *text;

        tool_join_path(path, sizeof(path), where, sr_ifnames[i]);
        text = (char *)ami_cfg_read_file(path, NULL);

        (VOID)sr_interface_text(&sr_out, sr_ifnames[i], text,
                                device, sizeof(device));
        if (device[0] != '\0')
            sr_name_device(device);

        if (text != NULL)
            ami_free(text);
    }
}

/* -------------------------------------------------------------- drivers --- */

#define SR_MAX_DEVICES  48

static SrModule sr_devs[SR_MAX_DEVICES];
static ULONG    sr_dev_count;

static VOID sr_scan_devices(VOID)
{
    struct Node *node;

    sr_dev_count = 0;

    Forbid();
    for (node = SysBase->DeviceList.lh_Head;
         node->ln_Succ != NULL && sr_dev_count < SR_MAX_DEVICES;
         node = node->ln_Succ)
    {
        struct Library *lib = (struct Library *)node;
        SrModule       *m   = &sr_devs[sr_dev_count];

        if (node->ln_Name == NULL)
            continue;

        tool_copy_string(m->name, sizeof(m->name), (const char *)node->ln_Name);
        sr_copy_id(m->id, (const char *)lib->lib_IdString);
        m->version  = lib->lib_Version;
        m->revision = lib->lib_Revision;
        m->ours     = FALSE;
        sr_dev_count++;
    }
    Permit();
}

static BOOL sr_is_anx(const char *name)
{
    return (BOOL)(tool_stricmp_n(name, "anx", 3) == 0);
}

static AnxDiagMark sr_mark;

static VOID sr_probe_record(const char *prefix, const char *device)
{
    UWORD status = tool_anxdiag_read(device, &sr_mark);
    UWORD i;
    char  sub[SR_KEY_MAX];

    sr_key(sr_k, sizeof(sr_k), prefix, "probe", NULL);
    sr_str(&sr_out, sr_k, status == TOOL_PASSIVE_OK ? "ok" :
                          status == TOOL_PASSIVE_BAD_VERSION ? "bad_version" :
                          "absent");
    if (status != TOOL_PASSIVE_OK)
        return;

    sr_key(sr_k, sizeof(sr_k), prefix, "probe.units", NULL);
    sr_ulong(&sr_out, sr_k, sr_mark.ad_Units);
    sr_key(sr_k, sizeof(sr_k), prefix, "probe.dropped", NULL);
    sr_ulong(&sr_out, sr_k, sr_mark.ad_Dropped);
    sr_key(sr_k, sizeof(sr_k), prefix, "probe.steps", NULL);
    sr_ulong(&sr_out, sr_k, sr_mark.ad_Used);
    sr_key(sr_k, sizeof(sr_k), prefix, "probe.lost", NULL);
    sr_ulong(&sr_out, sr_k, sr_mark.ad_Lost);

    for (i = 0; i < sr_mark.ad_Cards && i < 16; i++)
    {
        char card[16];

        sr_mark.ad_Name[i][sizeof(sr_mark.ad_Name[i]) - 1] = '\0';
        sr_key_part(card, sizeof(card), sr_mark.ad_Name[i]);
        sr_key(sub, sizeof(sub), prefix, "probe", NULL);
        sr_key_index(sr_k, sizeof(sr_k), sub, i, "card");
        sr_str(&sr_out, sr_k, card);
    }

    /* "code 13 card 2 value $00000001", one line a step.  The value is
       printed only when statusreport_text.c says the code may carry it. */
    for (i = 0; i < sr_mark.ad_Used && i < ANXDIAG_STEPS; i++)
    {
        static char        line[64];
        const AnxDiagStep *st  = &sr_mark.ad_Step[i];
        LONG               cls = sr_anxdiag_value_class(st->ds_Code);

        line[0] = '\0';
        sr_cat(line, sizeof(line), "code ");
        sr_cat_ulong(line, sizeof(line), st->ds_Code);
        sr_cat(line, sizeof(line), " card ");
        if (st->ds_Card == (UWORD)ANXDIAG_NOCARD)
            sr_cat(line, sizeof(line), "-");
        else
            sr_cat_ulong(line, sizeof(line), st->ds_Card);
        sr_cat(line, sizeof(line), " value ");

        if (cls > 0 || (cls == 0 && sr_out.addresses))
            sr_cat_hex(line, sizeof(line), st->ds_Value);
        else if (cls == 0)
            /* Station-address or serial bytes, and ADDRESSES was not given:
               the step stays so the sequence reads, its value does not. */
            sr_cat(line, sizeof(line), "withheld");
        else
            sr_cat(line, sizeof(line), SR_UNAVAILABLE);

        sr_key(sub, sizeof(sub), prefix, "probe", NULL);
        sr_key_index(sr_k, sizeof(sr_k), sub, i, NULL);
        sr_str(&sr_out, sr_k, line);
    }
}

static BOOL sr_is_named(const char *name)
{
    ULONG i;

    for (i = 0; i < sr_named_count; i++)
    {
        if (tool_stricmp(sr_named[i], name) == 0)
            return TRUE;
    }
    return FALSE;
}

static VOID sr_devices(VOID)
{
    char  part[TOOL_NAME_LEN];
    char  prefix[TOOL_NAME_LEN + 8];
    ULONG i;
    ULONG j;

    sr_scan_devices();

    /* Resident: ours by name, and anything an interface names. */
    for (i = 0; i < sr_dev_count && !sr_stop(); i++)
    {
        const SrModule *m = &sr_devs[i];

        if (!sr_is_anx(m->name) && !sr_is_named(m->name))
            continue;

        sr_key_part(part, sizeof(part), m->name);
        sr_key(prefix, sizeof(prefix), "device", part, NULL);

        sr_key(sr_k, sizeof(sr_k), prefix, "resident", NULL);
        sr_yesno(&sr_out, sr_k, TRUE);
        sr_key(sr_k, sizeof(sr_k), prefix, "version", NULL);
        sr_version(&sr_out, sr_k, m->version, m->revision);
        sr_key(sr_k, sizeof(sr_k), prefix, "id", NULL);
        sr_str(&sr_out, sr_k, m->id[0] != '\0' ? m->id : NULL);

        if (sr_is_anx(m->name))
            sr_probe_record(prefix, m->name);
    }

    /* Named by an interface and not in memory: nothing to read but that. */
    for (j = 0; j < sr_named_count; j++)
    {
        BOOL resident = FALSE;

        for (i = 0; i < sr_dev_count; i++)
        {
            if (tool_stricmp(sr_devs[i].name, sr_named[j]) == 0)
                resident = TRUE;
        }
        if (resident)
            continue;

        sr_key_part(part, sizeof(part), sr_named[j]);
        sr_key(prefix, sizeof(prefix), "device", part, NULL);

        sr_key(sr_k, sizeof(sr_k), prefix, "resident", NULL);
        sr_yesno(&sr_out, sr_k, FALSE);
        sr_key(sr_k, sizeof(sr_k), prefix, "version", NULL);
        sr_str(&sr_out, sr_k, NULL);
    }
}

/* ---------------------------------------------------------------- events --- */

#define SR_EVENTS   32

static AmiEventMark   sr_ev_mark;
static NetStatusEvent sr_ev[SR_EVENTS];

static VOID sr_events(VOID)
{
    static char line[80];
    UWORD       status;
    LONG        n = tool_events_read(&sr_ev_mark, sr_ev, SR_EVENTS, &status);
    LONG        i;

    if (n < 0)
    {
        sr_str(&sr_out, "events", status == TOOL_PASSIVE_BAD_VERSION
                                  ? "bad_version" : NULL);
        return;
    }

    sr_ulong(&sr_out, "events.count", (ULONG)n);
    sr_ulong(&sr_out, "events.overwritten",
             (n > 0 && sr_ev[0].nse_Seq > 1UL) ? sr_ev[0].nse_Seq - 1UL : 0UL);

    /* "seq 4 ms 1234 code 12 index 0 value -3": the codes are NETEVENT_* in
       include/aminetxduo/netstatus.h, index - is the machine. */
    for (i = 0; i < n; i++)
    {
        const NetStatusEvent *e = &sr_ev[i];
        char                  key[24];

        line[0] = '\0';
        sr_cat(line, sizeof(line), "seq ");
        sr_cat_ulong(line, sizeof(line), e->nse_Seq);
        sr_cat(line, sizeof(line), " ms ");
        sr_cat_ulong(line, sizeof(line), e->nse_Tick);
        sr_cat(line, sizeof(line), " code ");
        sr_cat_ulong(line, sizeof(line), e->nse_Code);
        sr_cat(line, sizeof(line), " index ");
        if (e->nse_Index == (UWORD)NETEVENT_NOINDEX)
            sr_cat(line, sizeof(line), "-");
        else
            sr_cat_ulong(line, sizeof(line), e->nse_Index);
        sr_cat(line, sizeof(line), " value ");
        sr_cat_long(line, sizeof(line), (LONG)e->nse_Value);

        sr_key_index(key, sizeof(key), "event", (ULONG)i, NULL);
        sr_str(&sr_out, key, line);
    }
}

/* ------------------------------------------------------------------ main --- */

int main(int argc, char **argv)
{
    LONG           args[ARG_COUNT];
    struct RDArgs *rda;
    const char    *path = SR_DEFAULT_FILE;
    LONG           rc   = RETURN_OK;

    (VOID)argv;

    if (tool_from_workbench(argc))
        return RETURN_FAIL;

    tool_break_arm();

    args[ARG_TO]        = 0;
    args[ARG_NOFILE]    = 0;
    args[ARG_ADDRESSES] = 0;

    rda = ReadArgs((CONST_STRPTR)TEMPLATE, args, NULL);
    if (rda == NULL)
    {
        tool_fault(IoErr());
        tool_usage("[TO <file>] [NOFILE] [ADDRESSES]",
                   "Write a network status report for a bug report.");
        return RETURN_ERROR;
    }

    if (args[ARG_TO] != 0 && args[ARG_NOFILE] != 0)
    {
        tool_error("TO and NOFILE cannot be used together");
        FreeArgs(rda);
        return RETURN_ERROR;
    }

    if (args[ARG_TO] != 0)
        path = (const char *)args[ARG_TO];

    sr_file = 0;
    if (args[ARG_NOFILE] == 0)
    {
        sr_file = Open((CONST_STRPTR)path, MODE_NEWFILE);
        if (sr_file == 0)
        {
            LONG err = IoErr();

            tool_error("%s could not be written", (LONG)path);
            tool_fault(err);
            rc = RETURN_ERROR;
        }
    }

    sr_console          = Output();
    sr_file_error       = 0;
    sr_tee.put          = sr_put;
    sr_tee.console      = (APTR)&sr_console;
    sr_tee.file         = (sr_file != 0) ? (APTR)&sr_file : NULL;
    sr_tee.file_failed  = FALSE;

    sr_out.write     = sr_tee_write;
    sr_out.user      = (APTR)&sr_tee;
    sr_out.addresses = (BOOL)(args[ARG_ADDRESSES] != 0);
    sr_out.lines     = 0;

    sr_str(&sr_out, "report.command", tool_name);
    sr_str(&sr_out, "report.version", AMINETXDUO_VERSION);
    sr_str(&sr_out, "report.build", AMINETXDUO_VERSION_HASH[0] != '\0'
                                    ? AMINETXDUO_VERSION_HASH : NULL);
    sr_yesno(&sr_out, "report.addresses", sr_out.addresses);
    sr_str(&sr_out, "report.file", sr_file != 0 ? path : "none");

    sr_system();

    sr_library("bsdsocket.library", TRUE);
    sr_library("usergroup.library", FALSE);
    sr_library("tls.library", FALSE);

    if (!sr_stop())
        sr_stack();
    if (!sr_stop())
        sr_config();
    if (!sr_stop())
        sr_devices();
    if (!sr_stop())
        sr_events();

    if (sr_stop())
    {
        sr_str(&sr_out, "report.end", "interrupted");
        tool_fault(ERROR_BREAK);
        if (rc == RETURN_OK)
            rc = RETURN_WARN;
    }
    else
    {
        sr_str(&sr_out, "report.end", "complete");
    }

    if (sr_file != 0)
    {
        /* Close() flushes, so a full disk can show up only here. */
        BOOL closed = (BOOL)Close(sr_file);

        sr_file = 0;
        if (!closed && sr_file_error == 0)
            sr_file_error = IoErr();
        if (sr_tee.file_failed || !closed)
        {
            tool_error("%s is incomplete", (LONG)path);
            if (sr_file_error != 0)
                tool_fault(sr_file_error);
            rc = RETURN_ERROR;
        }
    }

    FreeArgs(rda);
    return (int)rc;
}

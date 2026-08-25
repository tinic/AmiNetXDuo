/*
 * GetNetStatus, report network readiness as a return code, for scripts.
 *
 *     GetNetStatus CHECK/K,QUIET/S,VERSION/S
 * SPDX-License-Identifier: MIT
 */

#include "tools_nx.h"
#include "aminetxduo/version.h"

const char *const tool_name = "GetNetStatus";

static const char version_tag[] __attribute__((used)) =
    TOOL_VERSTAG("GetNetStatus");

#define TEMPLATE    "CHECK/K,QUIET/S,VERSION/S"

enum
{
    ARG_CHECK = 0,
    ARG_QUIET,
    ARG_VERSION,
    ARG_COUNT
};

enum
{
    COND_INTERFACES = 0,
    COND_PTPINTERFACES,
    COND_BCASTINTERFACES,
    COND_RESOLVER,
    COND_ROUTES,
    COND_DEFAULTROUTE,
    COND_COUNT
};

/*
 * A table rather than a chain of comparisons: the same list parses CHECK,
 * prints the report and names the condition that failed.
 */
static const struct ConditionName
{
    const char *name;
    const char *asks;
} conditions[COND_COUNT] =
{
    { "INTERFACES",      "an interface is up and has an address"    },
    { "PTPINTERFACES",   "a point-to-point interface is up"         },
    { "BCASTINTERFACES", "an Ethernet interface is up"              },
    { "RESOLVER",        "a name server is in use"                  },
    { "ROUTES",          "there is somewhere to send packets"       },
    { "DEFAULTROUTE",    "there is a route off this network"        }
};

static BOOL gns_quiet;

static VOID say(const char *fmt, ...)
{
    va_list args;

    if (gns_quiet)
        return;

    va_start(args, fmt);
    VPrintf((CONST_STRPTR)fmt, (APTR)args);         /* (APTR): see tool_util.c */
    va_end(args);
}

/*
 * Everything the six conditions are answered from, gathered once. Static: a
 * ToolSnapshot is far more than a Shell command's 4 KB stack holds.
 */
static ToolSnapshot gns_snap;
static ToolRoutes   gns_routes;

static VOID measure(BOOL satisfied[COND_COUNT])
{
    char  servers[TOOL_NAME_SERVERS_MAX][AMI_CFG_IP6_STRLEN];
    UWORD i;

    for (i = 0; i < (UWORD)COND_COUNT; i++)
        satisfied[i] = FALSE;

    if (!tool_stack_library_running())
        return;

    tool_nx_quiet(TRUE);

    if (tool_snapshot(&gns_snap, FALSE) != 0)
        return;

    for (i = 0; i < gns_snap.iface_count; i++)
    {
        const ToolIfInfo *info = &gns_snap.iface[i];

        if (!info->attached || !info->link_up)
            continue;

        /*
         * A SANA-II interface with a hardware address is Ethernet.
         * PTPINTERFACES stays FALSE: see the note at the top.
         */
        satisfied[COND_BCASTINTERFACES] = TRUE;

        /* Either family. Roadshow's INTERFACES asks whether this machine can
           be reached, and an IPv6-only machine can. */
        if (tool_iface_has_address(&gns_snap, info))
            satisfied[COND_INTERFACES] = TRUE;
    }

    if (gns_snap.have_gateway && gns_snap.gateway != 0)
        satisfied[COND_DEFAULTROUTE] = TRUE;

    if (tool_routes(&gns_routes) == 0 && gns_routes.count > 0)
        satisfied[COND_ROUTES] = TRUE;
    else if (satisfied[COND_INTERFACES] || satisfied[COND_DEFAULTROUTE])
        satisfied[COND_ROUTES] = TRUE;

    if (tool_stack_name_servers(servers, (ULONG)TOOL_NAME_SERVERS_MAX) > 0)
        satisfied[COND_RESOLVER] = TRUE;
}

/*
 * CHECK is one string of comma-separated names. Returns the number of
 * conditions asked for, or -1 after saying which name was not one.
 */
static LONG select_conditions(const char *list, BOOL wanted[COND_COUNT])
{
    char  name[32];
    ULONG len   = 0;
    LONG  count = 0;
    UWORD i;

    for (i = 0; i < (UWORD)COND_COUNT; i++)
        wanted[i] = FALSE;

    for (;;)
    {
        char c = *list++;

        if (c != ',' && c != '\0' && c != ' ' && c != '\t')
        {
            if (len + 1 < sizeof(name))
                name[len++] = c;
            continue;
        }

        name[len] = '\0';

        if (len > 0)
        {
            BOOL known = FALSE;

            for (i = 0; i < (UWORD)COND_COUNT; i++)
            {
                if (tool_stricmp(name, conditions[i].name) != 0)
                    continue;

                if (!wanted[i])
                    count++;
                wanted[i] = TRUE;
                known     = TRUE;
                break;
            }

            if (!known)
            {
                tool_error("\"%s\" is not something this command can check",
                           (LONG)name);
                for (i = 0; i < (UWORD)COND_COUNT; i++)
                    tool_printf("      %s\n", (LONG)conditions[i].name);
                return -1;
            }

            len = 0;
        }

        if (c == '\0')
            break;
    }

    if (count == 0)
    {
        tool_error("CHECK was given nothing to check");
        return -1;
    }

    return count;
}

int main(int argc, char **argv)
{
    LONG           args[ARG_COUNT];
    struct RDArgs *rda;
    BOOL           satisfied[COND_COUNT];
    BOOL           wanted[COND_COUNT];
    const char    *check;
    BOOL           running;
    UWORD          missing = 0;
    UWORD          i;

    (VOID)argv;

    if (tool_from_workbench(argc))
        return RETURN_FAIL;

    tool_break_arm();

    args[ARG_CHECK]   = 0;
    args[ARG_QUIET]   = 0;
    args[ARG_VERSION] = 0;

    rda = ReadArgs((CONST_STRPTR)TEMPLATE, args, NULL);
    if (rda == NULL)
    {
        tool_fault(IoErr());
        tool_usage("[CHECK <condition>[,<condition>...]] [QUIET]",
                   "Returns 0 when the network is ready, 5 when it is not.");
        return RETURN_ERROR;
    }

    gns_quiet = (args[ARG_QUIET] != 0) ? TRUE : FALSE;
    check     = (const char *)args[ARG_CHECK];

    if (args[ARG_VERSION] != 0)
    {
        char id[64];

        if (tool_stack_version(id, sizeof(id)))
        {
            tool_printf("%s\n", (LONG)id);
            FreeArgs(rda);
            return RETURN_OK;
        }

        tool_error("the network library is not loaded, so it has no version "
                   "to report");

        FreeArgs(rda);
        return RETURN_WARN;
    }

    if (check != NULL)
    {
        if (select_conditions(check, wanted) < 0)
        {
            FreeArgs(rda);
            return RETURN_ERROR;
        }
    }
    else
    {
        /* No CHECK: report all six and answer with INTERFACES. */
        for (i = 0; i < (UWORD)COND_COUNT; i++)
            wanted[i] = FALSE;

        wanted[COND_INTERFACES] = TRUE;
    }

    running = tool_stack_library_running();

    /*
     * A running stack that is somebody else's cannot be asked. Reporting the
     * network as not ready would send the reader to fix a network that is
     * fine, so this is a failure to find out and returns RETURN_ERROR.
     */
    if (running)
    {
        /* FALSE: the return code says ready or not ready, and this path is
           neither. QUIET drops the report, not the reason there is none. */
        struct Library *base = tool_netstatus_open(FALSE);

        if (base == NULL)
        {
            FreeArgs(rda);
            return RETURN_ERROR;
        }

        tool_netstatus_close(base);
    }

    measure(satisfied);

    if (check == NULL)
    {
        say("The network is %s\n", (LONG)(running ? "running" : "not running"));

        for (i = 0; i < (UWORD)COND_COUNT; i++)
        {
            say("  %-16s %-4s  %s\n", (LONG)conditions[i].name,
                (LONG)(satisfied[i] ? "yes" : "no"),
                (LONG)conditions[i].asks);
        }
    }

    for (i = 0; i < (UWORD)COND_COUNT; i++)
    {
        if (!wanted[i] || satisfied[i])
            continue;

        missing++;

        if (check != NULL)
            say("%s: no, %s\n", (LONG)conditions[i].name,
                (LONG)conditions[i].asks);
    }

    FreeArgs(rda);

    return (missing > 0) ? RETURN_WARN : RETURN_OK;
}

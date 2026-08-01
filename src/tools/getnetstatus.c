/*
 * GetNetStatus -- report network readiness as a return code, for scripts.
 *
 *     GetNetStatus CHECK/K,QUIET/S
 *
 *     C:GetNetStatus CHECK=INTERFACES,DEFAULTROUTE QUIET
 *     IF WARN
 *         echo "The network is not ready; not starting the server."
 *         SKIP done
 *
 * Return codes:
 *
 *     0  (RETURN_OK)     every condition asked about is satisfied
 *     5  (RETURN_WARN)   at least one is not -- this is what IF WARN tests
 *    10  (RETURN_ERROR)  the command could not find out: a name in CHECK that
 *                        is not a condition, or a bsdsocket.library that is
 *                        not this stack's
 *
 * Nothing here starts the network; doing so would make the answer true.
 *
 * With no CHECK it lists every condition, says which are satisfied, and returns
 * the answer to INTERFACES alone -- an interface that is up and has an address.
 *
 * The conditions are Roadshow's, so a script written for one stack works on the
 * other. Two mean something specific here:
 *
 *   PTPINTERFACES is never satisfied. A point-to-point interface is SLIP or
 *   PPP; every interface this stack attaches is a SANA-II Ethernet device with
 *   a hardware address.
 *
 *   ROUTES is satisfied by the routes that exist rather than by a routing
 *   table: without NX_ENABLE_IP_STATIC_ROUTING the routes a machine has are
 *   the directly-attached prefix of each interface and the default gateway.
 *   docs/RESEARCH.md 22.5.
 *
 * SPDX-License-Identifier: MIT
 */

#include "tools_nx.h"
#include "aminetxduo/version.h"

const char *const tool_name = "GetNetStatus";

static const char version_tag[] __attribute__((used)) =
    TOOL_VERSTAG("GetNetStatus");

#define TEMPLATE    "CHECK/K,QUIET/S"

enum
{
    ARG_CHECK = 0,
    ARG_QUIET,
    ARG_COUNT
};

/* ------------------------------------------------------------ conditions -- */

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

/* --------------------------------------------------------------- the run -- */

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
    char  servers[AMI_CFG_MAX_NAMESERVERS][16];
    UWORD i;

    for (i = 0; i < (UWORD)COND_COUNT; i++)
        satisfied[i] = FALSE;

    if (!tool_stack_library_running())
        return;

    /*
     * Quietly: this command's output is a verdict, and an explanation block in
     * the middle of it would be read as one of the answers.
     */
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

        if (info->address != 0)
            satisfied[COND_INTERFACES] = TRUE;
    }

    if (gns_snap.have_gateway && gns_snap.gateway != 0)
        satisfied[COND_DEFAULTROUTE] = TRUE;

    if (tool_routes(&gns_routes) == 0 && gns_routes.count > 0)
        satisfied[COND_ROUTES] = TRUE;
    else if (satisfied[COND_INTERFACES] || satisfied[COND_DEFAULTROUTE])
        satisfied[COND_ROUTES] = TRUE;

    if (tool_stack_name_servers(servers, (ULONG)AMI_CFG_MAX_NAMESERVERS) > 0)
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
                tool_advise_blank();
                tool_advise("The conditions are:");
                for (i = 0; i < (UWORD)COND_COUNT; i++)
                    tool_printf("      %s\n", (LONG)conditions[i].name);
                tool_advise("Separate several of them with commas.");
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

    args[ARG_CHECK] = 0;
    args[ARG_QUIET] = 0;

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
     * A running stack that is somebody else's cannot be asked. Answering "not
     * ready" would send the reader to fix a network that is fine, so this is a
     * failure to find out and returns RETURN_ERROR.
     */
    if (running)
    {
        struct Library *base = tool_netstatus_open(gns_quiet);

        if (base == NULL)
        {
            FreeArgs(rda);
            return RETURN_ERROR;
        }

        tool_netstatus_close(base);
    }

    measure(satisfied);

    /* ---- the report ----------------------------------------------------- */

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
            say("%s: no -- %s\n", (LONG)conditions[i].name,
                (LONG)conditions[i].asks);
    }

    FreeArgs(rda);

    return (missing > 0) ? RETURN_WARN : RETURN_OK;
}

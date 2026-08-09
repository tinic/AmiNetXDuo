/*
 * NetShutdown, take every network interface down.
 *
 *     NetShutdown TIMEOUT/N,QUIET/S
 *
 * The counterpart of AddNetInterface. TIMEOUT is how many seconds to wait for
 * the interfaces to reach the down state, five by default; the transition is
 * synchronous so it is normally instant, and running out of the wait is
 * reported rather than hidden.
 *
 * It does not unload the stack, despite the name. The stack is a singleton
 * inside bsdsocket.library, coming up on that library's first OpenLibrary() and
 * going down when the last opener closes (src/bsdsocket/library.c).
 * AddNetInterface starts the network by opening the library and never closing
 * it, and that leaked reference keeps the interface up after the command exits.
 * No other command holds that reference, so no other command can drop it:
 * bsdsocket.library stays in memory with its ThreadX kernel running until a
 * reboot.
 *
 * What is stoppable is the traffic. Every interface is taken down through
 * NETCTRL_INTERFACE_DOWN, the same call Offline makes, reaching
 * nx_ip_driver_interface_direct_command(NX_LINK_DISABLE) and stopping the
 * SANA-II readers with it. Afterwards nothing is sent and nothing is received.
 *
 * SPDX-License-Identifier: MIT
 */

#include "tools.h"

const char *const tool_name = "NetShutdown";

static const char version_tag[] __attribute__((used)) =
    TOOL_VERSTAG("NetShutdown");

#define TEMPLATE    "TIMEOUT/N,QUIET/S"

enum
{
    ARG_TIMEOUT = 0,
    ARG_QUIET,
    ARG_COUNT
};

/* Seconds. Roadshow's default, and generous for a synchronous transition. */
#define NSD_TIMEOUT     5UL

static BOOL nsd_quiet;

static VOID say(const char *fmt, ...)
{
    va_list args;

    if (nsd_quiet)
        return;

    va_start(args, fmt);
    VPrintf((CONST_STRPTR)fmt, (APTR)args);         /* (APTR): see tool_util.c */
    va_end(args);
}

/* Static: this is most of a Shell command's 4 KB stack on its own. */
static struct
{
    NetStatusHeader     hdr;
    NetStatusInterface  e[NX_MAX_PHYSICAL_INTERFACES];
} nsd_ifaces;

/*
 * How many interfaces are up right now. The whole table is re-read each time
 * rather than cached, because the wait below is watching for it to change.
 */
static LONG count_up(struct Library *base)
{
    LONG n;
    LONG i;
    LONG up = 0;

    n = tool_netstatus_query(base, NETSTATUS_INTERFACES, &nsd_ifaces,
                             sizeof(nsd_ifaces), sizeof(NetStatusInterface));
    if (n < 0)
        return -1;

    /* nsh_Count is the library's number, not ours: bound it by the table. */
    for (i = 0; i < n && i < (LONG)NX_MAX_PHYSICAL_INTERFACES; i++)
    {
        /*
         * NETSTATUS_IF_LINKUP, not NETSTATUS_IF_ONLINE, for the reason onoff.c
         * gives: LINKUP is the flag NETCTRL_INTERFACE_DOWN clears, while the
         * SANA-II shim's online flag is a layer below and does not follow in
         * step. Waiting on that one would wait forever.
         */
        if (nsd_ifaces.e[i].nsi_Flags & NETSTATUS_IF_LINKUP)
            up++;
    }

    return up;
}

int main(int argc, char **argv)
{
    LONG             args[ARG_COUNT];
    struct RDArgs   *rda;
    struct Library  *base;
    NetStatusControl ctl;
    ULONG            timeout;
    ULONG            waited  = 0;
    LONG             stopped = 0;
    LONG             failed  = 0;
    LONG             n;
    LONG             i;
    ULONG            w;

    (VOID)argv;

    if (tool_from_workbench(argc))
        return RETURN_FAIL;

    tool_break_arm();

    args[ARG_TIMEOUT] = 0;
    args[ARG_QUIET]   = 0;

    rda = ReadArgs((CONST_STRPTR)TEMPLATE, args, NULL);
    if (rda == NULL)
    {
        tool_fault(IoErr());
        tool_usage("[TIMEOUT <secs>] [QUIET]",
                   "Take every network interface down.");
        return RETURN_ERROR;
    }

    nsd_quiet = (args[ARG_QUIET] != 0) ? TRUE : FALSE;
    timeout   = NSD_TIMEOUT;

    if (args[ARG_TIMEOUT] != 0)
    {
        LONG given = *(const LONG *)args[ARG_TIMEOUT];

        timeout = (given > 0) ? (ULONG)given : 0UL;
    }

    /* Nothing to stop; tool_netstatus_open() will not start the stack. */
    if (!tool_stack_library_running())
    {
        say("The network is not running, so there is nothing to stop.\n");
        FreeArgs(rda);
        return RETURN_WARN;
    }

    base = tool_netstatus_open(nsd_quiet);
    if (base == NULL)
    {
        FreeArgs(rda);
        return RETURN_FAIL;
    }

    n = tool_netstatus_query(base, NETSTATUS_INTERFACES, &nsd_ifaces,
                             sizeof(nsd_ifaces), sizeof(NetStatusInterface));
    if (n < 0)
    {
        if (!nsd_quiet)
        {
            tool_error("the network would not say which interfaces it has");
            tool_explain_no_netstatus(base);
        }
        tool_netstatus_close(base);
        FreeArgs(rda);
        return RETURN_FAIL;
    }

    for (i = 0; i < n && i < (LONG)NX_MAX_PHYSICAL_INTERFACES; i++)
    {
        const NetStatusInterface *nsi = &nsd_ifaces.e[i];
        const char               *name;
        LONG                      err = 0;

        if (!(nsi->nsi_Flags & NETSTATUS_IF_LINKUP))
            continue;

        name = (nsi->nsi_Flags & NETSTATUS_IF_NAMED) ? nsi->nsi_Name
                                                     : "an interface";

        for (w = 0; w < (ULONG)(sizeof(ctl) / sizeof(ULONG)); w++)
            ((ULONG *)&ctl)[w] = 0;

        ctl.nsc_Index = nsi->nsi_Index;

        if (tool_netstatus_control(base, NETCTRL_INTERFACE_DOWN, &ctl,
                                   &err) != 0)
        {
            if (!nsd_quiet)
                tool_error("%s would not go down", (LONG)name);
            failed++;
            continue;
        }

        say("%s: stopped\n", (LONG)name);
        stopped++;
    }

    if (stopped == 0 && failed == 0)
    {
        say("Every interface was already down.\n");
        tool_netstatus_close(base);
        FreeArgs(rda);
        return RETURN_OK;
    }

    /*
     * Wait for the table to agree. The transition is synchronous, so the first
     * look normally finds nothing left to wait for; the loop covers the case
     * TIMEOUT is about.
     */
    for (;;)
    {
        LONG still_up = count_up(base);

        if (still_up <= 0)
            break;

        if (waited >= timeout)
        {
            if (!nsd_quiet)
            {
                tool_error("%ld interface(s) were still up %lu seconds after "
                           "being told to stop", still_up, timeout);
            }
            tool_netstatus_close(base);
            FreeArgs(rda);
            return RETURN_WARN;
        }

        if (tool_delay_ticks((ULONG)TICKS_PER_SECOND))
        {
            tool_netstatus_close(base);
            FreeArgs(rda);
            tool_fault(ERROR_BREAK);
            return RETURN_WARN;
        }

        waited++;
    }

    tool_netstatus_close(base);

    if (failed > 0)
    {
        FreeArgs(rda);
        return RETURN_WARN;
    }

    say("\nThe network is stopped: nothing is sent and nothing is received.\n");
    say("bsdsocket.library stays in memory with the stack inside it. The\n");
    say("reference that keeps it there belongs to whatever started the\n");
    say("network, and only a reboot drops it. Online <interface> starts a\n");
    say("stopped interface again.\n");

    FreeArgs(rda);
    return RETURN_OK;
}

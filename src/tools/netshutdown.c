/*
 * NetShutdown -- stop the network.
 *
 * Every interface goes down, in one command, and nothing goes in or out of
 * this machine afterwards. It is the counterpart of AddNetInterface: that one
 * starts the network, this one stops it, and until now the only way to stop it
 * was to take each interface down by name or to reboot.
 *
 * TIMEOUT is how many seconds to wait for the interfaces to actually reach the
 * down state, five by default. Going down is normally instant -- the transition
 * is synchronous -- so the wait exists for the case where it is not, and
 * running out of it is reported rather than hidden.
 *
 * What it does not do, said out loud because the name promises more than this
 * machine can deliver.
 *
 * The stack is a singleton inside bsdsocket.library and it comes up on that
 * library's first OpenLibrary() and goes down when the last opener closes
 * (src/bsdsocket/library.c). AddNetInterface starts the network precisely by
 * opening the library and never closing it, and that deliberately leaked
 * reference is what keeps the interface up after the command exits. No other
 * command holds that reference, so no other command can drop it: bsdsocket.
 * library therefore stays in memory with its ThreadX kernel running, and only
 * a reboot clears it.
 *
 * What is stoppable is the traffic, and that is what this stops. Every
 * interface is taken down through NETCTRL_INTERFACE_DOWN -- the same call
 * Offline makes, which reaches nx_ip_driver_interface_direct_command(
 * NX_LINK_DISABLE) and stops the SANA-II readers with it. Afterwards nothing
 * is sent and nothing is received. Saying that plainly is better than a
 * command that claims to have shut the stack down and left it running.
 *
 * SPDX-License-Identifier: MIT
 */

#include "tools.h"

const char *const tool_name = "NetShutdown";

static const char version_tag[] __attribute__((used)) =
    "$VER: NetShutdown 1.0 (26.7.2026)";

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
 * How many interfaces are up right now, and their names when asked for. The
 * whole table is re-read each time rather than cached, because the wait below
 * is watching for it to change.
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

    for (i = 0; i < n; i++)
    {
        /*
         * NETSTATUS_IF_LINKUP and not NETSTATUS_IF_ONLINE, for the reason
         * onoff.c gives: LINKUP is the flag NETCTRL_INTERFACE_DOWN clears, and
         * the SANA-II shim's own online flag belongs to a layer below and does
         * not follow in step. Waiting on the other one would wait forever.
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

    /*
     * Nothing to stop. Roadshow exits at once and says so, and that is right:
     * a shutdown command that starts the network in order to have something to
     * shut down would be absurd, and tool_netstatus_open() will not do it.
     */
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

    for (i = 0; i < n; i++)
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
     * Wait for the table to agree. The transition is synchronous, so this
     * normally finds nothing left to wait for on its first look; the loop is
     * for the case where it does not, which is the case TIMEOUT is about.
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
                tool_advise_blank();
                tool_advise("The shutdown was asked for and has not been");
                tool_advise("cancelled; it may still finish on its own.");
                tool_advise("ShowNetStatus says where it got to.");
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
    say("bsdsocket.library stays in memory with the stack inside it -- the\n");
    say("reference that keeps it there belongs to whatever started the\n");
    say("network, and only a reboot drops it. Online <interface> starts a\n");
    say("stopped interface again.\n");

    FreeArgs(rda);
    return RETURN_OK;
}

/*
 * NetShutdown, stop the network and the programs using it.
 *
 *     NetShutdown TIMEOUT/N,QUIET/S
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

/* Seconds. Roadshow's default, and the whole budget: the programs' grace
   period and the wait for the interfaces come out of the same figure. */
#define NSD_TIMEOUT     5UL

/* How many holders to name. Past this the count is printed and the names are
   not. */
#define NSD_MAX_OPENERS 12

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

/* Static: these are most of a Shell command's 4 KB stack on their own. */
static struct
{
    NetStatusHeader     hdr;
    NetStatusInterface  e[NX_MAX_PHYSICAL_INTERFACES];
} nsd_ifaces;

static struct
{
    NetStatusHeader     hdr;
    NetStatusOpener     e[NSD_MAX_OPENERS];
} nsd_openers;

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

/*
 * The programs holding the library open, this one excluded. Fills the table
 * and returns how many there are, which can be more than it could list.
 */
static LONG others_holding(struct Library *base, LONG *listed)
{
    LONG n;
    LONG i;
    LONG others = 0;

    if (listed != NULL)
        *listed = 0;

    n = tool_netstatus_query(base, NETSTATUS_OPENERS, &nsd_openers,
                             sizeof(nsd_openers), sizeof(NetStatusOpener));
    if (n < 0)
        return -1;

    for (i = 0; i < n && i < NSD_MAX_OPENERS; i++)
    {
        if (nsd_openers.e[i].nso_Flags & NETSTATUS_OPENER_SELF)
            continue;

        if (listed != NULL)
            nsd_openers.e[(*listed)++] = nsd_openers.e[i];
    }

    /* nsh_Available is the true count. The table can hold fewer. */
    others = (LONG)nsd_openers.hdr.nsh_Available - 1;

    return (others > 0) ? others : 0;
}

/* Never with the DOS list locked: a handler answers on the Task whose port
   this is, and answering can mean taking that list. 1 gone, 0 refused
   (a file handle is still open), -1 there was no TCP: at all. */
static LONG stop_tcp_handler(VOID)
{
    struct DosList *dl;
    struct MsgPort *port = NULL;

    dl = LockDosList(LDF_DEVICES | LDF_READ);
    dl = FindDosEntry(dl, (STRPTR)"TCP", LDF_DEVICES);
    if (dl != NULL)
        port = dl->dol_Task;
    UnLockDosList(LDF_DEVICES | LDF_READ);

    if (port == NULL)
        return -1;

    return DoPkt(port, ACTION_DIE, 0, 0, 0, 0, 0) ? 1 : 0;
}

/* Name the holders the last others_holding() listed, one to a line, then the
   count of the ones it had no room for. */
static VOID name_them(LONG listed, LONG total)
{
    LONG i;

    for (i = 0; i < listed; i++)
    {
        const NetStatusOpener *o = &nsd_openers.e[i];
        const char            *name;

        name = (o->nso_Name[0] != '\0') ? o->nso_Name : "an unnamed program";

        if (o->nso_Flags & NETSTATUS_OPENER_GONE)
            say("  %s (exited without closing the library)\n", (LONG)name);
        else if (o->nso_Sockets != 0)
            say("  %s, %ld socket(s)\n", (LONG)name, (LONG)o->nso_Sockets);
        else
            say("  %s\n", (LONG)name);
    }

    if (total > listed)
        say("  and %ld more\n", (LONG)(total - listed));
}

int main(int argc, char **argv)
{
    LONG             args[ARG_COUNT];
    struct RDArgs   *rda;
    struct Library  *base;
    NetStatusControl ctl;
    ULONG            timeout;
    ULONG            waited   = 0;
    LONG             stopped  = 0;
    LONG             failed   = 0;
    LONG             holding;
    LONG             listed   = 0;
    BOOL             stopped_waiting = FALSE;
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
                   "Stop the network and the programs using it.");
        return RETURN_ERROR;
    }

    nsd_quiet = (args[ARG_QUIET] != 0) ? TRUE : FALSE;
    timeout   = NSD_TIMEOUT;

    if (args[ARG_TIMEOUT] != 0)
    {
        LONG given = *(const LONG *)args[ARG_TIMEOUT];

        if (given < 0)
        {
            tool_error("TIMEOUT cannot be negative");
            FreeArgs(rda);
            return RETURN_ERROR;
        }

        timeout = (ULONG)given;
    }

    /* Nothing to stop. tool_netstatus_open() does not start the stack. */
    if (!tool_stack_library_running())
    {
        say("The network is not running, so there is nothing to stop.\n");
        FreeArgs(rda);
        return RETURN_WARN;
    }

    /* FALSE: QUIET drops what was stopped, never why it could not be. */
    base = tool_netstatus_open(FALSE);
    if (base == NULL)
    {
        FreeArgs(rda);
        return RETURN_FAIL;
    }

    holding = others_holding(base, &listed);

    if (holding > 0)
    {
        LONG err = 0;

        say("%ld program(s) use the network:\n", holding);
        name_them(listed, holding);

        for (w = 0; w < (ULONG)(sizeof(ctl) / sizeof(ULONG)); w++)
            ((ULONG *)&ctl)[w] = 0;

        if (tool_netstatus_control(base, NETCTRL_STACK_NOTIFY, &ctl,
                                   &err) != 0)
        {
            /* An older library has no such operation. Everything below it
               still works, and this is the half that needs the pair to match,
               so say which half is missing rather than failing the command. */
            tool_error("this bsdsocket.library cannot tell them to stop, so "
                       "they will not be asked");
            holding = 0;
        }
        else
        {
            say("Asked them to stop, and will wait up to %lu second(s).\n",
                timeout);

            while ((holding = others_holding(base, &listed)) > 0)
            {
                if (waited >= timeout)
                    break;

                if (tool_delay_ticks((ULONG)TICKS_PER_SECOND))
                {
                    /* Roadshow's third outcome: the shutdown was already asked
                       for and cannot be recalled. */
                    stopped_waiting = TRUE;
                    break;
                }

                waited++;
            }
        }
    }

    n = tool_netstatus_query(base, NETSTATUS_INTERFACES, &nsd_ifaces,
                             sizeof(nsd_ifaces), sizeof(NetStatusInterface));
    if (n < 0)
    {
        tool_error("the network did not say which interfaces it has");
        tool_explain_no_netstatus(base);
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
            tool_error("%s did not go down", (LONG)name);
            failed++;
            continue;
        }

        say("%s: stopped\n", (LONG)name);
        stopped++;
    }

    /*
     * Wait for the table to agree. The transition is synchronous, so the first
     * look normally finds nothing left to wait for. The loop covers the case
     * TIMEOUT is about, out of whatever is left of it.
     */
    while (!stopped_waiting && (stopped != 0 || failed != 0))
    {
        LONG still_up = count_up(base);

        if (still_up <= 0)
            break;

        if (waited >= timeout)
        {
            tool_error("%ld interface(s) were still up %lu seconds after "
                       "the request to stop", still_up, timeout);
            failed++;
            break;
        }

        if (tool_delay_ticks((ULONG)TICKS_PER_SECOND))
        {
            stopped_waiting = TRUE;
            break;
        }

        waited++;
    }

    /* After step 1, which is what gets a TCP: file handle closed, and before
       step 3, whose last CloseLibrary() is the expunge this clears. */
    {
        LONG died = stop_tcp_handler();

        if (died > 0)
            say("TCP: stopped\n");
        else if (died == 0)
            say("TCP: is still open by a program and was left running\n");
    }

    /*
     * After the interfaces, not before. While the reference is held the stack
     * cannot go down, and once it is given back the last CloseLibrary() takes
     * the stack with it. That last close can be the one at the bottom of this
     * function.
     */
    for (w = 0; w < (ULONG)(sizeof(ctl) / sizeof(ULONG)); w++)
        ((ULONG *)&ctl)[w] = 0;

    (VOID)tool_netstatus_control(base, NETCTRL_STACK_RELEASE, &ctl, NULL);

    holding = others_holding(base, &listed);

    tool_netstatus_close(base);

    if (stopped_waiting)
    {
        say("\nThe wait is over. The network is stopped either way, and it\n"
            "will finish its shutdown when the programs below let go.\n");
    }

    if (holding > 0)
    {
        say("\n%ld program(s) did not let go of the network:\n", holding);
        name_them(listed, holding);
        say("Nothing is sent or received now. bsdsocket.library stays in\n"
            "memory until they close it, and the stack goes with the last\n"
            "one.\n");

        FreeArgs(rda);
        return RETURN_WARN;
    }

    if (failed > 0)
    {
        FreeArgs(rda);
        return RETURN_WARN;
    }

    if (stopped == 0)
        say("Every interface was already down.\n");

    say("\nThe network is stopped and nothing uses it now, so\n"
        "bsdsocket.library and the stack inside it went with the last\n"
        "program that closed it. Online <interface> or AddNetInterface\n"
        "starts the network again.\n");

    FreeArgs(rda);
    return (stopped_waiting) ? RETURN_WARN : RETURN_OK;
}

/*
 * Online / Offline -- switch a network interface up or down.
 *
 *     Online  NAME/A,UNIT/N,TIMEOUT/N
 *     Offline NAME/A,UNIT/N,TIMEOUT/N
 *
 * NAME IS TWO THINGS, and that is the whole design of this command.
 *
 * The interface these commands are expected to have takes a SANA-II DEVICE
 * DRIVER and a unit number -- "Offline a2065.device UNIT 0" -- because that is
 * the level at which a driver can be switched off to run hardware diagnostics
 * on the card. What this stack can act on, though, is a CONFIGURED INTERFACE:
 * a file in DEVS:NetInterfaces which names a driver and a unit, and which is
 * the handle everything else here uses -- ShowNetStatus prints it,
 * AddNetInterface takes it, and the netstack indexes by it.
 *
 * So NAME is resolved in that order, and both spellings work:
 *
 *   1. a configured interface, if DEVS:NetInterfaces/<NAME> parses. UNIT is
 *      then checked against the unit that file already names rather than being
 *      allowed to contradict it.
 *   2. otherwise a driver name, matched against the DEVICE and UNIT of every
 *      configured interface -- so "a2065.device" finds the interface that uses
 *      it, and the command operates on that.
 *   3. otherwise neither, and the message says so and lists what this machine
 *      does have, with the driver and unit of each. A driver name that no
 *      interface uses is the interesting case: the card may well be present,
 *      but nothing here has been told to use it, so there is nothing to switch.
 *
 * A name that is both -- an interface file called the same thing as a driver --
 * is taken as the interface, and says so.
 *
 * TIMEOUT is how many seconds to wait for the interface to reach the state
 * that was asked for, with 0 meaning wait for as long as it takes; Ctrl-C
 * aborts the wait either way. The transition here is synchronous, so the state
 * is normally reached before the wait begins and nothing is waited for.
 *
 * One source, two executables: TOOL_OFFLINE picks which. The asymmetry that
 * matters is that Online may have to start the network (the user may never
 * have run AddNetInterface), while Offline never does -- taking an interface
 * down on a machine with no stack is a no-op worth saying out loud rather than
 * a reason to boot the whole thing.
 *
 * Both resolve NAME before they touch anything, so that a mistyped name is
 * answered with "there is no such interface, here are the ones there are"
 * rather than with a fact about the stack.
 *
 * SPDX-License-Identifier: MIT
 */

#include "tools.h"

#ifdef TOOL_OFFLINE
const char *const tool_name = "Offline";
static const char version_tag[] __attribute__((used)) =
    "$VER: Offline 2.0 (26.7.2026)";
#else
const char *const tool_name = "Online";
static const char version_tag[] __attribute__((used)) =
    "$VER: Online 2.0 (26.7.2026)";
#endif

#define TEMPLATE    "NAME/A,UNIT/N,TIMEOUT/N"

enum
{
    ARG_NAME = 0,
    ARG_UNIT,
    ARG_TIMEOUT,
    ARG_COUNT
};

/* As many interface files as the drawer is scanned for. */
#define ONOFF_MAX_FILES     16

/*
 * Read DEVS:NetInterfaces/<name> purely to be able to talk about it. `loud`
 * FALSE is a probe: nothing is printed and the caller decides what the answer
 * means.
 */
static BOOL load_interface(const char *name, AmiIfConfig *ifc, BOOL loud)
{
    LONG err;

    if (loud)
        tool_config_watch();
    err = ami_config_load_interface(name, ifc);
    if (loud)
        tool_config_unwatch();

    if (err == AMI_CFG_OK)
        return TRUE;

    if (!loud)
        return FALSE;

    if (err == AMI_CFG_ERR_IO)
    {
        tool_error("there is no interface called \"%s\"", (LONG)name);
        tool_explain_interface_file(name);
    }
    else
    {
        tool_error("DEVS:NetInterfaces/%s cannot be used as it stands",
                   (LONG)name);
        tool_advise_blank();
        tool_advise("Fix the line named above, or run  NetSetup  to write the");
        tool_advise("file from scratch.");
    }

    return FALSE;
}

/*
 * Every interface file in the drawer, whether or not a stack is running --
 * netstack_config() is empty in the shipped build, where the stack lives in
 * bsdsocket.library, so the files are the only list there is.
 */
static ULONG list_interfaces(char names[][TOOL_NAME_LEN])
{
    return tool_list_dir("DEVS:NetInterfaces", names,
                         (ULONG)ONOFF_MAX_FILES, NULL);
}

/*
 * Find the configured interface that uses SANA-II driver `device` on `unit`.
 * The comparison is on the last path component of each, so
 * "DEVS:Networks/a2065.device" and "a2065.device" are the same driver.
 */
static BOOL find_by_device(const char *device, ULONG unit, char *name_out,
                           ULONG name_len, AmiIfConfig *ifc)
{
    static char names[ONOFF_MAX_FILES][TOOL_NAME_LEN];
    ULONG       count = list_interfaces(names);
    ULONG       i;

    for (i = 0; i < count; i++)
    {
        if (!load_interface(names[i], ifc, FALSE))
            continue;

        if (tool_stricmp(tool_basename(ifc->device), tool_basename(device)) != 0)
            continue;
        if (ifc->unit != unit)
            continue;

        tool_copy_string(name_out, name_len, names[i]);
        return TRUE;
    }

    return FALSE;
}

/* "a2065.device is not used by any interface" -- with the list, so it is act-on-able. */
static VOID explain_unknown_name(const char *given, ULONG unit, BOOL had_unit)
{
    static char        names[ONOFF_MAX_FILES][TOOL_NAME_LEN];
    static AmiIfConfig ifc;
    ULONG              count = list_interfaces(names);
    ULONG              i;
    ULONG              listed = 0;

    if (had_unit)
        tool_error("nothing here is \"%s\" on unit %lu", (LONG)given, unit);
    else
        tool_error("nothing here is called \"%s\"", (LONG)given);

    for (i = 0; i < count; i++)
    {
        if (!load_interface(names[i], &ifc, FALSE))
            continue;

        if (listed++ == 0)
        {
            tool_advise_blank();
            tool_advise("A name is either an interface or the driver one uses.");
            tool_advise("This machine has:");
        }

        tool_printf("      %-15s %s unit %ld\n", (LONG)names[i],
                    (LONG)ifc.device, (LONG)ifc.unit);
    }

    if (listed == 0)
    {
        tool_explain_interface_file(given);
        return;
    }

    tool_advise_blank();
    tool_advise("Either column works as the name; a driver that no interface");
    tool_advise("uses has nothing here to switch, even when the card is fitted.");
}

/* ------------------------------------------------- the running stack -----
 *
 * These two commands used to say, in as many words, that an interface could
 * not be switched while the stack ran -- "that needs a call the library does
 * not have yet". The library has it now: NetStackControl() at
 * AMI_NETSTATUS_CONTROL_LVO (include/aminetxduo/netstatus.h). docs/RESEARCH.md
 * 22.
 *
 * The index is looked up BY NAME out of the live snapshot rather than computed
 * from the order of DEVS:NetInterfaces. The two do agree -- src/netstack
 * attaches the configured interfaces in config order -- but agreeing is not
 * the same as being the same thing, and taking an interface down by index is
 * the wrong operation to be approximately right about.
 */

static struct
{
    NetStatusHeader     hdr;
    NetStatusInterface  e[NX_MAX_PHYSICAL_INTERFACES];
} onoff_ifaces;

/*
 * The NX interface index of `name` in the running stack, or -1. *online is
 * set when the answer is found.
 */
static LONG live_index(struct Library *base, const char *name, BOOL *online)
{
    LONG n;
    LONG i;

    if (online != NULL)
        *online = FALSE;

    n = tool_netstatus_query(base, NETSTATUS_INTERFACES, &onoff_ifaces,
                             sizeof(onoff_ifaces), sizeof(NetStatusInterface));
    if (n <= 0)
        return -1;

    for (i = 0; i < n; i++)
    {
        const NetStatusInterface *nsi = &onoff_ifaces.e[i];

        if (!(nsi->nsi_Flags & NETSTATUS_IF_NAMED))
            continue;

        if (tool_stricmp(nsi->nsi_Name, name) != 0)
            continue;

        /*
         * NETSTATUS_IF_LINKUP, not NETSTATUS_IF_ONLINE. This is the flag these
         * two commands set: NETCTRL_INTERFACE_UP/DOWN reach
         * nx_ip_driver_interface_direct_command(NX_LINK_ENABLE/DISABLE), which
         * is what nx_interface_link_up records. The SANA-II shim's own online
         * flag belongs to a layer below and does not follow in step, so
         * waiting on it would wait forever.
         */
        if (online != NULL)
            *online = (nsi->nsi_Flags & NETSTATUS_IF_LINKUP) ? TRUE : FALSE;

        return (LONG)nsi->nsi_Index;
    }

    return -1;
}

/*
 * Wait for the interface to reach the state that was asked for, reading the
 * live stack each time round. `seconds` 0 waits for as long as it takes.
 * FALSE means the time ran out, or -- with *broken set -- that Ctrl-C was
 * pressed.
 */
static BOOL wait_for_live_state(struct Library *base, const char *name,
                                BOOL want_up, ULONG seconds, BOOL *broken)
{
    ULONG waited = 0;

    for (;;)
    {
        BOOL now = FALSE;

        if (live_index(base, name, &now) < 0)
            return FALSE;

        if (now == want_up)
            return TRUE;

        if (seconds != 0 && waited >= seconds)
            return FALSE;

        if (tool_delay_ticks((ULONG)TICKS_PER_SECOND))
        {
            *broken = TRUE;
            return FALSE;
        }

        waited++;
    }
}

/*
 * Switch one interface of the RUNNING stack, and report what happened. Both
 * commands end up here whenever bsdsocket.library has the network up, which
 * on a machine that has run AddNetInterface is always.
 *
 * Owns the FreeArgs(): it is the tail of main() for this path, and threading
 * the return code back out through three more branches bought nothing.
 */
static LONG switch_live(const char *name, const AmiIfConfig *ifc, BOOL up,
                        ULONG timeout, struct RDArgs *rda)
{
    struct Library  *base;
    NetStatusControl ctl;
    BOOL             online = FALSE;
    BOOL             broken = FALSE;
    LONG             index;
    LONG             err = 0;
    LONG             rc  = RETURN_OK;
    ULONG            i;

    base = tool_netstatus_open(FALSE);
    if (base == NULL)
    {
        FreeArgs(rda);
        return RETURN_FAIL;
    }

    index = live_index(base, name, &online);
    if (index < 0)
    {
        tool_error("%s is configured but the running stack has no such "
                   "interface", (LONG)name);
        tool_advise_blank();
        tool_advise("The network was started before this interface file was");
        tool_advise("written, or with a different one. AddNetInterface adds an");
        tool_advise("interface to a stack that is already running.");
        tool_netstatus_close(base);
        FreeArgs(rda);
        return RETURN_ERROR;
    }

    if (online == up)
    {
        tool_printf("%s is already %s\n", (LONG)name,
                    (LONG)(up ? "online" : "offline"));
        tool_netstatus_close(base);
        FreeArgs(rda);
        return RETURN_OK;
    }

    for (i = 0; i < (ULONG)(sizeof(ctl) / sizeof(ULONG)); i++)
        ((ULONG *)&ctl)[i] = 0;

    ctl.nsc_Index = (UWORD)index;

    if (tool_netstatus_control(base,
                               up ? NETCTRL_INTERFACE_UP
                                  : NETCTRL_INTERFACE_DOWN,
                               &ctl, &err) != 0)
    {
        tool_error("%s would not go %s", (LONG)name,
                   (LONG)(up ? "online" : "offline"));

        if (up && ifc != NULL)
            tool_explain_device(ifc->device, ifc->unit);

        tool_netstatus_close(base);
        FreeArgs(rda);
        return RETURN_FAIL;
    }

    if (!wait_for_live_state(base, name, up, timeout, &broken) && !broken)
    {
        tool_error("%s was still %s %lu seconds after being asked to go %s",
                   (LONG)name, (LONG)(up ? "down" : "up"), timeout,
                   (LONG)(up ? "up" : "down"));
        rc = RETURN_WARN;
    }
    else if (!broken && up)
    {
        char  addr[16];
        ULONG live = 0;

        /* The live address, not the one in the file: see addnetinterface.c. */
        if (live_index(base, name, &online) >= 0)
        {
            LONG n;

            for (n = 0; n < (LONG)onoff_ifaces.hdr.nsh_Count; n++)
            {
                if (onoff_ifaces.e[n].nsi_Index == (UWORD)index)
                {
                    live = onoff_ifaces.e[n].nsi_Address;
                    break;
                }
            }
        }

        if (live != 0)
        {
            ami_config_format_ip(live, addr, sizeof(addr));
            tool_printf("%s is online, address %s\n", (LONG)name, (LONG)addr);
        }
        else
        {
            tool_printf("%s is online but has no address yet\n", (LONG)name);

            if (ifc != NULL && ifc->iptype == AMI_IPTYPE_DHCP)
                tool_explain_dhcp(name);
        }
    }
    else if (!broken)
    {
        tool_printf("%s is offline\n", (LONG)name);
    }

    tool_netstatus_close(base);
    FreeArgs(rda);

    if (broken || tool_break())
    {
        tool_fault(ERROR_BREAK);
        return RETURN_WARN;
    }

    return rc;
}

/*
 * Wait for the interface to reach the state that was asked for. `seconds` 0
 * waits for as long as it takes. FALSE means the time ran out, or -- with
 * *broken set -- that Ctrl-C was pressed.
 */
static BOOL wait_for_state(LONG index, BOOL want_up, ULONG seconds,
                           BOOL *broken)
{
    ULONG waited = 0;

    for (;;)
    {
        BOOL now = netstack_interface_is_up((UWORD)index) ? TRUE : FALSE;

        if (now == want_up)
            return TRUE;

        if (seconds != 0 && waited >= seconds)
            return FALSE;

        if (tool_delay_ticks((ULONG)TICKS_PER_SECOND))
        {
            *broken = TRUE;
            return FALSE;
        }

        waited++;
    }
}

int main(int argc, char **argv)
{
    LONG           args[ARG_COUNT];
    struct RDArgs *rda;
    AmiIfConfig    ifc;
    static char    resolved[TOOL_NAME_LEN];
    const char    *given;
    const char    *name;
    ULONG          unit;
    ULONG          timeout;
    BOOL           had_unit;
    BOOL           broken = FALSE;
    LONG           index;
    LONG           err;

    (VOID)argv;

    if (tool_from_workbench(argc))
        return RETURN_FAIL;

    tool_break_arm();

    args[ARG_NAME]    = 0;
    args[ARG_UNIT]    = 0;
    args[ARG_TIMEOUT] = 0;

    rda = ReadArgs((CONST_STRPTR)TEMPLATE, args, NULL);
    if (rda == NULL)
    {
        tool_fault(IoErr());
        tool_usage("<interface or driver> [UNIT <n>] [TIMEOUT <secs>]",
                   "eth0, or the driver it uses, e.g. a2065.device.");
        return RETURN_ERROR;
    }

    given    = tool_basename((const char *)args[ARG_NAME]);
    had_unit = (args[ARG_UNIT] != 0) ? TRUE : FALSE;
    unit     = had_unit ? (ULONG)*(const LONG *)args[ARG_UNIT] : 0UL;
    timeout  = (args[ARG_TIMEOUT] != 0)
                   ? (ULONG)*(const LONG *)args[ARG_TIMEOUT] : 0UL;

    /*
     * Resolve NAME: interface first, then driver. See the note at the top of
     * this file for why that order and not the other one.
     */
    if (load_interface(given, &ifc, FALSE))
    {
        tool_copy_string(resolved, sizeof(resolved), given);
        name = resolved;

        if (had_unit && ifc.unit != unit)
        {
            tool_error("%s is %s unit %ld, and unit %lu was asked for",
                       (LONG)name, (LONG)ifc.device, (LONG)ifc.unit, unit);
            tool_advise_blank();
            tool_advise("The unit an interface uses is set in its file, in");
            tool_advise("DEVS:NetInterfaces. Leave UNIT off to use that one.");
            FreeArgs(rda);
            return RETURN_ERROR;
        }

        /* Reload loudly, so a broken file is still reported with line numbers. */
        if (!load_interface(name, &ifc, TRUE))
        {
            FreeArgs(rda);
            return RETURN_FAIL;
        }

        /* The one ambiguous case, named rather than left to be guessed at. */
        {
            /* static: a second AmiIfConfig is most of a Shell command's 4K. */
            static AmiIfConfig other;
            static char        othername[TOOL_NAME_LEN];

            if (find_by_device(given, unit, othername, sizeof(othername),
                               &other) &&
                tool_stricmp(othername, name) != 0)
            {
                tool_printf("%s: taken as the interface name; the interface "
                            "that uses a driver\n", (LONG)name);
                tool_printf("  of that name is %s.\n", (LONG)othername);
            }
        }
    }
    else if (find_by_device(given, unit, resolved, sizeof(resolved), &ifc))
    {
        name = resolved;
        tool_printf("%s unit %lu is interface %s.\n", (LONG)given, unit,
                    (LONG)name);
    }
    else
    {
        explain_unknown_name(given, unit, had_unit);
        FreeArgs(rda);
        return RETURN_FAIL;
    }

#ifndef TOOL_OFFLINE
    err = netstack_startup();

    if (err == AMI_NET_ERR_STATE)
    {
        /*
         * No stack in this command: it lives in bsdsocket.library and comes
         * up on first open (tool_stack_start()). That brings every configured
         * interface up at once, which is what Online was asked for -- but it
         * also means a single interface cannot be toggled once the stack is
         * already running.
         */
        if (tool_stack_library_running())
        {
            /*
             * The stack is up inside bsdsocket.library. This used to be the
             * end of the road -- "individual interfaces cannot be taken up and
             * down while the stack runs; that needs a call the library does
             * not have yet". It has one now.
             */
            return switch_live(name, &ifc, TRUE, timeout, rda);
        }

        {
            struct Library *base;

            /* Up to half a minute while DHCP is asked; do not sit silent. */
            tool_printf("%s: starting the network...\n", (LONG)name);

            base = tool_stack_start();

            if (base == NULL)
            {
                tool_error("%s would not come online", (LONG)name);

                if (!tool_stack_installed())
                {
                    tool_advise_blank();
                    tool_advise("bsdsocket.library is not installed. The network");
                    tool_advise("stack lives in that library and belongs in LIBS:.");
                }
                else
                {
                    tool_explain_device(ifc.device, ifc.unit);
                }

                FreeArgs(rda);
                return RETURN_FAIL;
            }

            if (!tool_stack_is_ours(base))
            {
                tool_error("another TCP/IP stack is installed on this machine");
                tool_explain_foreign_stack(base);
                FreeArgs(rda);
                return RETURN_WARN;
            }
        }

        {
            ULONG addr = 0;
            char  text[16];

            if (tool_stack_query(&addr, NULL, 0) && addr != 0)
            {
                ami_config_format_ip(addr, text, sizeof(text));
                tool_printf("%s is online, address %s\n", (LONG)name, (LONG)text);
            }
            else
            {
                tool_printf("%s is online but has no address yet\n", (LONG)name);
                if (ifc.iptype == AMI_IPTYPE_DHCP)
                    tool_explain_dhcp(name);
            }
        }

        FreeArgs(rda);
        return RETURN_OK;
    }

    if (err != AMI_NET_OK)
    {
        tool_error("the network would not start: %s",
                   (LONG)tool_net_error(err));

        if (err == AMI_NET_ERR_NODEV)
            tool_explain_device(ifc.device, ifc.unit);

        FreeArgs(rda);
        return RETURN_FAIL;
    }
#else
    if (netstack_get() == NULL)
    {
        if (tool_stack_library_running())
            return switch_live(name, &ifc, FALSE, timeout, rda);

        tool_printf("%s is already offline: the network is not running.\n",
                    (LONG)name);

        FreeArgs(rda);
        return RETURN_WARN;
    }
#endif

    index = tool_find_interface(name);
    if (index < 0)
    {
        FreeArgs(rda);
        return RETURN_FAIL;
    }

#ifdef TOOL_OFFLINE
    if (!netstack_interface_is_up((UWORD)index))
    {
        tool_printf("%s is already offline\n", (LONG)name);
        FreeArgs(rda);
        return RETURN_OK;
    }

    err = netstack_interface_down((UWORD)index);
    if (err != AMI_NET_OK)
    {
        tool_error("cannot take %s offline: %s", (LONG)name,
                   (LONG)tool_net_error(err));
        FreeArgs(rda);
        return RETURN_FAIL;
    }

    if (!wait_for_state(index, FALSE, timeout, &broken) && !broken)
    {
        tool_error("%s was still up %lu seconds after being asked to go down",
                   (LONG)name, timeout);
        FreeArgs(rda);
        return RETURN_WARN;
    }

    if (!broken)
        tool_printf("%s is offline\n", (LONG)name);
#else
    if (netstack_interface_is_up((UWORD)index))
    {
        tool_printf("%s is already online\n", (LONG)name);
        FreeArgs(rda);
        return RETURN_OK;
    }

    err = netstack_interface_up((UWORD)index);
    if (err != AMI_NET_OK)
    {
        tool_error("%s would not come online: %s", (LONG)name,
                   (LONG)tool_net_error(err));
        tool_explain_device(ifc.device, ifc.unit);
        FreeArgs(rda);
        return RETURN_FAIL;
    }

    if (!wait_for_state(index, TRUE, timeout, &broken) && !broken)
    {
        tool_error("%s was still down %lu seconds after being asked to come up",
                   (LONG)name, timeout);
        FreeArgs(rda);
        return RETURN_WARN;
    }

    if (!broken)
    {
        NX_IP *ip = netstack_ip();
        char   addr[16];
        ULONG  live = 0;

        /* The live address, not the one in the file: see addnetinterface.c. */
        if (ip != NULL)
            live = ip->nx_ip_interface[index].nx_interface_ip_address;

        if (live != 0)
        {
            ami_config_format_ip(live, addr, sizeof(addr));
            tool_printf("%s is online, address %s\n", (LONG)name, (LONG)addr);
        }
        else
        {
            tool_printf("%s is online but has no address yet\n", (LONG)name);

            if (ifc.iptype == AMI_IPTYPE_DHCP)
                tool_explain_dhcp(name);
        }
    }
#endif

    if (broken || tool_break())
    {
        tool_fault(ERROR_BREAK);
        FreeArgs(rda);
        return RETURN_WARN;
    }

    FreeArgs(rda);
    return RETURN_OK;
}

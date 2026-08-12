/*
 * AddNetInterface, bring up the interface described by
 * DEVS:NetInterfaces/<name>.
 *
 *     AddNetInterface INTERFACE/M,QUIET/S,TIMEOUT/K/N
 *
 * This is the command S:User-Startup invokes, so it is the one that starts the
 * network:
 *
 *     C:AddNetInterface DEVS:NetInterfaces/eth0 QUIET
 *
 * INTERFACE is either a bare interface name or the full path to the interface
 * file, only the name matters, the directory is fixed, and more than one
 * may be given. Several names are sorted before they are used, so a list
 * brings interfaces up in a defined order rather than in the order typed; the
 * ceiling is AMI_CFG_MAX_INTERFACES, which is what the parsed configuration
 * holds.
 *
 * TIMEOUT is how long to wait, in seconds, for an interface that asks for its
 * address to be given one. It bounds the DHCP exchange, so ten seconds is both
 * the default and the floor: a shorter limit expires before the protocol can
 * finish and reports a failure that is not one.
 *
 * The files are read twice. The first pass only parses, and stops the command
 * before anything is started if any file cannot be used; without it, a list of
 * three interfaces with a typo in the third brings two up and then fails.
 *
 * This is also the first command a new user runs, so every failure below
 * prints what is wrong, where, and what to type next; the terse version still
 * goes to the serial log.
 *
 * QUIET drops that running commentary and nothing else. A boot that says
 * nothing when it works and still says why it did not is what User-Startup
 * wants; a boot that swallows "there is no interface called eth0" is a
 * machine with no network and no reason given.
 *
 * It does not shut the stack down again: tool_stack_start() asks the library to
 * hold the stack itself, which is what keeps the interface online after this
 * command exits, as in Roadshow, where the interface stays up until Offline or
 * a reboot. This command's own open is closed like anybody else's.
 *
 * SPDX-License-Identifier: MIT
 */

#include "tools.h"

const char *const tool_name = "AddNetInterface";

static const char version_tag[] __attribute__((used)) =
    TOOL_VERSTAG("AddNetInterface");

#define TEMPLATE    "INTERFACE/M,QUIET/S,TIMEOUT/K/N"

enum
{
    ARG_INTERFACE = 0,
    ARG_QUIET,
    ARG_TIMEOUT,
    ARG_COUNT
};

/* Seconds. Both the default and the floor, see the note at the top. */
#define ADDIF_TIMEOUT       10UL

/*
 * Below this much free, a failed start is a memory problem and no other
 * explanation is worth printing. It is not what the stack costs, 81 measured
 * that at 432-439 KB resident plus the packet pool. A 512 KB machine reads
 * about 73 KB here, measured, so the line sits well clear of both.
 */
#define ADDNETIF_MIN_FREE   (200UL * 1024UL)

/*
 * Out of memory, in the same words from both explainers below. `freemem` is
 * the AvailMem() the caller already tested, so the number printed is the one
 * that was judged.
 */
static VOID advise_out_of_memory(ULONG freemem)
{
    tool_printf("  %lu bytes are free; the stack needs about 450K.\n", freemem);
}

/*
 * Turn a stack error code into advice. `ifc` is the interface file already
 * parsed, which lets the device explanation name the driver and unit.
 */
static VOID explain_startup_failure(LONG err, const AmiIfConfig *ifc)
{
    switch (err)
    {
        case AMI_NET_ERR_NODEV:
            tool_explain_device(ifc->device, ifc->unit);
            break;

        case AMI_NET_ERR_DEVBAD:
            tool_explain_device_refused(ifc->device, ifc->unit);
            break;

        case AMI_NET_ERR_CONFIG:
            /*
             * The stack came up but no interface got an address. For DHCP
             * that is nearly always the cable or the server; for a static
             * interface it is the file.
             */
            if (ifc->iptype == AMI_IPTYPE_DHCP)
            {
                tool_explain_dhcp(ifc->name);
            }
            else
            {
                tool_printf("  %s has no usable address.\n", (LONG)ifc->name);
            }
            break;

        case AMI_NET_ERR_NOMEM:
            advise_out_of_memory(AvailMem(MEMF_PUBLIC));
            break;

        case AMI_NET_ERR_KERNEL:
            break;

        default:
            break;
    }
}

/* bsdsocket.library would not open. That has exactly two causes. */
static VOID explain_library_failure(const AmiIfConfig *ifc)
{
    if (!tool_stack_installed())
    {
        /* Same sentence Online prints for the same machine, onoff.c. */
        tool_printf("  LIBS:bsdsocket.library is not installed.\n");
        return;
    }

    /*
     * Before blaming the interface. A 512 KB machine fails here: the stack
     * logs its refusal to the serial port and puts nothing on screen, and the
     * device probe below would then send someone with no free memory to go and
     * look at their card. tests/tools/run-oommsg.sh is the run that proves
     * this branch is reached.
     */
    {
        ULONG freemem = AvailMem(MEMF_PUBLIC);

        if (freemem < ADDNETIF_MIN_FREE)
        {
            advise_out_of_memory(freemem);
            return;
        }
    }


    /* Probe the hardware rather than guess at it. */
    tool_explain_device(ifc->device, ifc->unit);
}

/*
 * Read DEVS:NetInterfaces/<name>, reporting what is wrong with it if anything
 * is. tool_config_watch() puts the parser's complaints, with line numbers, on
 * the screen instead of only on the serial port.
 */
static BOOL load_interface(const char *name, AmiIfConfig *ifc, BOOL again)
{
    LONG err;

    tool_config_watch();
    err = ami_config_load_interface(name, ifc);
    tool_config_unwatch();

    if (err == AMI_CFG_OK)
        return TRUE;

    /* A re-read of a file that already parsed once; the first read is what
       reports on it. Not QUIET: the message is an error and QUIET keeps
       those. */
    if (again)
        return FALSE;

    if (err == AMI_CFG_ERR_IO)
    {
        tool_error("there is no interface called \"%s\"", (LONG)name);
        tool_explain_interface_file(name);
    }
    else if (err == AMI_CFG_ERR_SYNTAX)
    {
        tool_error("DEVS:NetInterfaces/%s cannot be used as it stands",
                   (LONG)name);
    }
    else
    {
        tool_error("out of memory reading DEVS:NetInterfaces/%s", (LONG)name);
    }

    return FALSE;
}

/*
 * Sort the names, so a list of interfaces is brought up in an order that does
 * not depend on how it was typed. Insertion sort: the list is at most
 * AMI_CFG_MAX_INTERFACES long.
 */
static VOID sort_names(STRPTR *names, ULONG count)
{
    ULONG i;

    for (i = 1; i < count; i++)
    {
        STRPTR hold = names[i];
        ULONG  j    = i;

        while (j > 0 &&
               tool_stricmp(tool_basename((const char *)names[j - 1]),
                            tool_basename((const char *)hold)) > 0)
        {
            names[j] = names[j - 1];
            j--;
        }

        names[j] = hold;
    }
}

/* ------------------------------------------- the stack inside the library,
 *
 * Which is every shipped build. Opening bsdsocket.library starts the network
 * and brings up everything in DEVS:NetInterfaces, so a first add needs nothing
 * else; an add against a stack that is ALREADY running is a different job, and
 * one this command did not do at all until 0.20.1. It opened the library,
 * found it open, waited for an address the machine already had or never got,
 * and returned RETURN_OK having attached nothing. That is what a user saw
 * after RemoveNetInterface: the same name added back, success reported, no
 * interface. NETCTRL_INTERFACE_ADD is the call that does the work.
 */

/* Static: this table is most of a Shell command's 4 KB stack on its own. */
static struct
{
    NetStatusHeader     hdr;
    NetStatusInterface  e[NX_MAX_PHYSICAL_INTERFACES];
} addif_ifaces;

/*
 * What the running stack has by that name: its index, or -1 when there is no
 * such interface and -2 when the stack would not answer. `addr_out` receives
 * the address it holds, which is 0 until one arrives.
 */
static LONG running_index(struct Library *base, const char *name,
                          ULONG *addr_out)
{
    LONG n;
    LONG i;

    if (addr_out != NULL)
        *addr_out = 0;

    n = tool_netstatus_query(base, NETSTATUS_INTERFACES, &addif_ifaces,
                             sizeof(addif_ifaces), sizeof(NetStatusInterface));
    if (n < 0)
        return -2;

    /* nsh_Count is the library's number, not ours: bound it by the table it is
       being used to index, as removenetinterface.c and tool_nx.c do. */
    for (i = 0; i < n && i < (LONG)NX_MAX_PHYSICAL_INTERFACES; i++)
    {
        if (!(addif_ifaces.e[i].nsi_Flags & NETSTATUS_IF_NAMED))
            continue;

        if (tool_stricmp(addif_ifaces.e[i].nsi_Name, name) != 0)
            continue;

        if (addr_out != NULL)
            *addr_out = addif_ifaces.e[i].nsi_Address;

        return (LONG)addif_ifaces.e[i].nsi_Index;
    }

    return -1;
}

/*
 * Hand the name to the running stack. The library reads
 * DEVS:NetInterfaces/<name> itself and brings the interface up as a boot
 * would, so there is nothing to pass but the name. Returns 0, or the library's
 * errno.
 */
static LONG add_to_running_stack(struct Library *base, const char *name)
{
    NetStatusControl ctl;
    LONG             err = 0;
    ULONG            w;
    ULONG            i;

    for (w = 0; w < (ULONG)(sizeof(ctl) / sizeof(ULONG)); w++)
        ((ULONG *)&ctl)[w] = 0;

    for (i = 0; i + 1 < (ULONG)sizeof(ctl.nsc_Name) && name[i] != '\0'; i++)
        ctl.nsc_Name[i] = name[i];

    if (tool_netstatus_control(base, NETCTRL_INTERFACE_ADD, &ctl, &err) == 0)
        return 0;

    return (err != 0) ? err : EIO;
}

/* Why the add was refused, in the words the rest of this command uses. */
static VOID explain_add_failure(LONG err, const char *name,
                                const AmiIfConfig *ifc)
{
    switch (err)
    {
        case ENOENT:
            tool_explain_interface_file(name);
            break;

        case ENXIO:
            tool_explain_device(ifc->device, ifc->unit);
            break;

        case EIO:
            tool_explain_device_refused(ifc->device, ifc->unit);
            break;

        case EEXIST:
            tool_printf("  %s is already part of the running network.\n",
                        (LONG)name);
            break;

        case ENOSPC:
            tool_printf("  this stack holds %ld interfaces and they are all "
                        "in use; RemoveNetInterface frees one.\n",
                        (LONG)NX_MAX_PHYSICAL_INTERFACES);
            break;

        case ENOBUFS:
            advise_out_of_memory(AvailMem(MEMF_PUBLIC));
            break;

        default:
            break;
    }
}

/*
 * Wait up to `seconds` for that interface to be given an address. FALSE means
 * the time ran out, the interface went away, or Ctrl-C was pressed.
 */
static BOOL wait_for_running_address(struct Library *base, const char *name,
                                     ULONG seconds, ULONG *addr_out,
                                     BOOL *broken)
{
    ULONG waited = 0;

    for (;;)
    {
        ULONG addr = 0;

        if (running_index(base, name, &addr) < 0)
            return FALSE;

        if (addr != 0)
        {
            *addr_out = addr;
            return TRUE;
        }

        if (waited >= seconds)
            return FALSE;

        if (tool_delay_ticks((ULONG)TICKS_PER_SECOND))
        {
            *broken = TRUE;
            return FALSE;
        }

        waited++;
    }
}

/* The same wait, against an interface of a stack that is linked in here. */
static BOOL wait_for_interface_address(LONG index, ULONG seconds,
                                       ULONG *addr_out, BOOL *broken)
{
    ULONG waited = 0;

    for (;;)
    {
        NX_IP *ip   = netstack_ip();
        ULONG  addr = 0;

        if (ip != NULL)
            addr = ip->nx_ip_interface[index].nx_interface_ip_address;

        if (addr != 0)
        {
            *addr_out = addr;
            return TRUE;
        }

        if (waited >= seconds)
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
    LONG            args[ARG_COUNT];
    struct RDArgs  *rda;
    AmiIfConfig     ifc;
    STRPTR         *names;
    const char     *name;
    const char     *primary;
    ULONG           count = 0;
    ULONG           timeout;
    ULONG           allowance;
    BOOL            quiet;
    BOOL            dynamic = FALSE;
    BOOL            broken = FALSE;
    ULONG           n;
    LONG            index;
    LONG            err;
    LONG            rc = RETURN_OK;

    (VOID)argv;

    if (tool_from_workbench(argc))
        return RETURN_FAIL;

    tool_break_arm();

    args[ARG_INTERFACE] = 0;
    args[ARG_QUIET]     = 0;
    args[ARG_TIMEOUT]   = 0;

    rda = ReadArgs((CONST_STRPTR)TEMPLATE, args, NULL);
    if (rda == NULL)
    {
        tool_fault(IoErr());
        tool_usage("<interface name> [<interface name>...]",
                   "The name of a file in DEVS:NetInterfaces, e.g. eth0.");
        return RETURN_ERROR;
    }

    names   = (STRPTR *)args[ARG_INTERFACE];
    quiet   = (args[ARG_QUIET] != 0) ? TRUE : FALSE;
    timeout = ADDIF_TIMEOUT;

    if (args[ARG_TIMEOUT] != 0)
    {
        LONG given = *(const LONG *)args[ARG_TIMEOUT];

        timeout = (given > (LONG)ADDIF_TIMEOUT) ? (ULONG)given : ADDIF_TIMEOUT;
    }

    while (names != NULL && names[count] != NULL)
        count++;

    if (count == 0)
    {
        tool_error("which interface?");
        tool_usage("<interface name> [<interface name>...]",
                   "The name of a file in DEVS:NetInterfaces, e.g. eth0.");

        FreeArgs(rda);
        return RETURN_ERROR;
    }

    if (count > (ULONG)AMI_CFG_MAX_INTERFACES)
    {
        tool_error("this stack holds %ld interfaces, and %lu were named",
                   (LONG)AMI_CFG_MAX_INTERFACES, count);

        FreeArgs(rda);
        return RETURN_ERROR;
    }

    sort_names(names, count);
    primary = tool_basename((const char *)names[0]);

    /*
     * First pass: every named file must parse before anything is started, so
     * that a typo in User-Startup gives a message rather than a stack that
     * comes up with nothing on it.
     */
    for (n = 0; n < count; n++)
    {
        name = tool_basename((const char *)names[n]);

        if (!load_interface(name, &ifc, FALSE))
        {
            FreeArgs(rda);
            return RETURN_FAIL;
        }

        if (ifc.iptype != AMI_IPTYPE_STATIC)
            dynamic = TRUE;
    }

    /*
     * Printed only now that every file is known to be good: printing during
     * the parse pass would list two interfaces and then reject the third,
     * reading as though the first two had been started.
     */
    if (!quiet)
    {
        for (n = 0; n < count; n++)
        {
            name = tool_basename((const char *)names[n]);
            (VOID)load_interface(name, &ifc, TRUE);
            tool_printf("%s: %s unit %ld\n", (LONG)name, (LONG)ifc.device,
                        (LONG)ifc.unit);
        }
    }

    /*
     * TIMEOUT is an allowance for an address to be handed out, so there is
     * nothing to wait for when every named interface has its address in its
     * file. Waiting anyway would turn a misconfigured static interface into a
     * ten-second pause before the message that says so.
     */
    allowance = dynamic ? timeout : 0UL;

    /* Second pass. The first file's configuration is what the explainers use. */
    (VOID)load_interface(primary, &ifc, TRUE);
    name = primary;

    /*
     * netstack_startup() is idempotent and reference counted, and is the call
     * to use when the stack is linked into this command. It is not in the
     * shipped build, the stack has to outlive the command, so it lives in
     * bsdsocket.library, and AMI_NET_ERR_STATE is what says so.
     */
    err = netstack_startup();

    if (err == AMI_NET_ERR_STATE)
    {
        struct Library *base;
        BOOL            was_running = tool_stack_library_running();

        /*
         * Starting the network blocks until an address arrives or DHCP gives
         * up, up to half a minute. Say so first, or the pause looks like a
         * hung machine. Nothing is started when the stack is already up, so
         * the line is not printed then either.
         */
        if (!quiet && !was_running)
            tool_printf("%s: starting the network...\n", (LONG)name);

        base = tool_stack_start();

        if (base == NULL)
        {
            tool_error("the network would not start");
            explain_library_failure(&ifc);
            FreeArgs(rda);
            return RETURN_FAIL;
        }

        if (!tool_stack_is_ours(base))
        {
            tool_error("another TCP/IP stack is installed on this machine");
            tool_explain_foreign_stack(base);

            /*
             * Holding a reference to a library we are about to complain about
             * would only stop its owner unloading it.
             */
            tool_stack_release(base);
            FreeArgs(rda);
            return RETURN_WARN;
        }

        /*
         * The first open brought up every interface in DEVS:NetInterfaces at
         * once, so a name that was in there is already attached and this only
         * reads it back. A name that is not attached is the interesting case:
         * an interface added to the directory since the stack started, or one
         * taken out by RemoveNetInterface, and it is added here.
         *
         * Every name is checked, whether or not the stack was already running,
         * because the answer is the same question either way and a start that
         * came up without one of them is a failure this used to report as
         * success. This is also where TIMEOUT is spent: tool_stack_start() has
         * already waited for the DHCP exchange it starts, and the wait below
         * spends the rest of the allowance before deciding nothing answered.
         */
        for (n = 0; n < count; n++)
        {
            ULONG addr = 0;
            char  text[16];
            LONG  where;

            name = tool_basename((const char *)names[n]);
            (VOID)load_interface(name, &ifc, TRUE);

            where = running_index(base, name, &addr);

            if (where == -2)
            {
                tool_error("the network would not say which interfaces it "
                           "has");
                tool_explain_no_netstatus(base);
                tool_stack_release(base);
                FreeArgs(rda);
                return RETURN_FAIL;
            }

            if (where < 0)
            {
                LONG add_err = add_to_running_stack(base, name);

                if (add_err != 0)
                {
                    tool_error("%s could not be added to the running network",
                               (LONG)name);
                    explain_add_failure(add_err, name, &ifc);
                    rc = RETURN_FAIL;
                    continue;
                }
            }

            if (addr == 0)
                (VOID)wait_for_running_address(base, name, allowance, &addr,
                                               &broken);

            if (addr != 0)
            {
                if (!quiet)
                {
                    ami_config_format_ip(addr, text, sizeof(text));
                    tool_printf("%s: online, address %s\n", (LONG)name,
                                (LONG)text);
                }
            }
            else if (!ifc.up)
            {
                /*
                 * The file says STATE=down, so there is no address because
                 * none was asked for. Sending someone to check a cable they
                 * deliberately left unplugged is the same mistake as blaming a
                 * driver for a card that opened.
                 */
                if (!quiet)
                    tool_printf("%s: the network is running, and %s is "
                                "configured down\n", (LONG)name, (LONG)name);
            }
            else
            {
                /*
                 * Attached, and nothing gave it an address. WARN and not OK:
                 * a script that reads the return code is the reason this
                 * command exists in User-Startup.
                 */
                if (!quiet)
                {
                    tool_printf("%s: the network is running, but this machine "
                                "has no address yet\n", (LONG)name);

                    if (ifc.iptype == AMI_IPTYPE_DHCP)
                        tool_explain_dhcp(name);
                }

                if (rc == RETURN_OK)
                    rc = RETURN_WARN;
            }

            if (broken)
                break;
        }

        /*
         * The network is up and the library is holding it, so this open has
         * done its job. Before this was here, every AddNetInterface left one
         * behind: a base on the library's child list naming a Task that is
         * about to exit (tool_diag.c, tool_stack_start()).
         */
        if (broken)
        {
            tool_fault(ERROR_BREAK);
            tool_stack_release(base);
            FreeArgs(rda);
            return RETURN_WARN;
        }

        tool_stack_release(base);
        FreeArgs(rda);
        return (int)rc;
    }

    if (err != AMI_NET_OK)
    {
        tool_error("the network would not start: %s",
                   (LONG)tool_net_error(err));
        explain_startup_failure(err, &ifc);
        FreeArgs(rda);
        return RETURN_FAIL;
    }

    for (n = 0; n < count; n++)
    {
        name = tool_basename((const char *)names[n]);
        (VOID)load_interface(name, &ifc, TRUE);

        index = tool_find_interface(name);
        if (index < 0)
        {
            /*
             * The file parses but the running stack does not know the name:
             * the stack was already up when this interface file was written,
             * or the name was removed from it. Add it, which is the same work
             * NETCTRL_INTERFACE_ADD does for the library build above.
             */
            UWORD slot = 0;

            err = netstack_interface_start(&ifc, &slot);
            if (err != AMI_NET_OK)
            {
                tool_error("%s could not be added to the running network: %s",
                           (LONG)name, (LONG)tool_net_error(err));
                explain_startup_failure(err, &ifc);
                FreeArgs(rda);
                return RETURN_FAIL;
            }

            index = (LONG)slot;
        }

        if (!netstack_interface_is_up((UWORD)index))
        {
            err = netstack_interface_up((UWORD)index);
            if (err != AMI_NET_OK)
            {
                tool_error("%s would not come online: %s", (LONG)name,
                           (LONG)tool_net_error(err));
                tool_explain_device(ifc.device, ifc.unit);
                FreeArgs(rda);
                return RETURN_FAIL;
            }
        }

        {
            char  addr[16];
            char  mask[16];
            ULONG live_addr = 0;
            ULONG live_mask = 0;
            NX_IP *ip;

            /*
             * A DHCP interface's address is not the one in the config file:
             * the file says "DHCP" and has zeroes in it. Read the interface,
             * where the lease landed. Printing the file's fields here
             * produced "eth0: 0.0.0.0 netmask 0.0.0.0 (DHCP)" one line after
             * a successful lease. TIMEOUT is how long the lease is given to
             * arrive.
             */
            (VOID)wait_for_interface_address(index,
                                             (ifc.iptype != AMI_IPTYPE_STATIC)
                                                 ? timeout : 0UL,
                                             &live_addr, &broken);

            ip = netstack_ip();
            if (ip != NULL)
                live_mask =
                    ip->nx_ip_interface[index].nx_interface_ip_network_mask;

            /* Online with nothing on it is not a success; the library build
               above answers the same case the same way. */
            if (live_addr == 0 && ifc.up && rc == RETURN_OK)
                rc = RETURN_WARN;

            if (quiet)
            {
                /* nothing to say */
            }
            else if (live_addr == 0)
            {
                tool_printf("%s: online, but it has no address yet\n",
                            (LONG)name);

                if (ifc.iptype == AMI_IPTYPE_DHCP)
                    tool_explain_dhcp(name);
            }
            else
            {
                ami_config_format_ip(live_addr, addr, sizeof(addr));
                ami_config_format_ip(live_mask, mask, sizeof(mask));

                tool_printf("%s: %s netmask %s (%s)\n",
                            (LONG)name, (LONG)addr, (LONG)mask,
                            (LONG)(ifc.iptype == AMI_IPTYPE_DHCP      ? "DHCP" :
                                   ifc.iptype == AMI_IPTYPE_LINKLOCAL ? "link-local"
                                                                      : "static"));
            }
        }

        if (broken)
            break;
    }

    if (broken || tool_break())
    {
        tool_fault(ERROR_BREAK);
        rc = RETURN_WARN;
    }

    FreeArgs(rda);
    return (int)rc;
}

/*
 * AddNetInterface -- bring up the interface described by
 * DEVS:NetInterfaces/<name>.
 *
 * This is the command S:User-Startup invokes, so it is the one that starts the
 * network:
 *
 *     C:AddNetInterface DEVS:NetInterfaces/eth0 QUIET
 *
 * INTERFACE is either a bare interface name or the full path to the interface
 * file -- only the name matters, the directory is fixed -- and more than one
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
 * It does not shut the stack down again: the reference taken by
 * tool_stack_start() is what keeps the interface online after this command
 * exits, as in Roadshow, where the interface stays up until Offline or a
 * reboot.
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

/* Seconds. Both the default and the floor -- see the note at the top. */
#define ADDIF_TIMEOUT       10UL

/*
 * Below this much free, a failed start is a memory problem and no other
 * explanation is worth printing. It is not what the stack costs -- 81 measured
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
        return;
    }

    /*
     * Before blaming the interface. A 512 KB machine fails here: the stack
     * logs its refusal to the serial port and puts nothing on screen, and the
     * cable branch below would then send someone with no free memory to go and
     * look at their wiring. tests/tools/run-oommsg.sh is the run that proves
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

    /* Only when the card is fine: a missing driver is not a cable fault. */
    if (ifc->iptype == AMI_IPTYPE_DHCP &&
        tool_device_where(ifc->device) != NULL &&
        tool_device_probe(ifc->device, ifc->unit) == 0)
    {
    }
}

/*
 * Read DEVS:NetInterfaces/<name>, reporting what is wrong with it if anything
 * is. tool_config_watch() puts the parser's complaints, with line numbers, on
 * the screen instead of only on the serial port.
 */
static BOOL load_interface(const char *name, AmiIfConfig *ifc, BOOL quiet)
{
    LONG err;

    tool_config_watch();
    err = ami_config_load_interface(name, ifc);
    tool_config_unwatch();

    if (err == AMI_CFG_OK)
        return TRUE;

    if (quiet)
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

/*
 * Wait up to `seconds` for this machine to be given an address. `addr_out` is
 * where the answer lands; FALSE means the time ran out or Ctrl-C was pressed.
 * Returns at once when there is already an address, as on a static interface.
 */
static BOOL wait_for_address(ULONG seconds, ULONG *addr_out, BOOL *broken)
{
    ULONG waited = 0;

    for (;;)
    {
        ULONG addr = 0;

        if (tool_stack_query(&addr, NULL, 0) && addr != 0)
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
        if (!quiet)
        {
            tool_error("which interface?");
            tool_usage("<interface name> [<interface name>...]",
                       "The name of a file in DEVS:NetInterfaces, e.g. eth0.");
        }

        FreeArgs(rda);
        return RETURN_ERROR;
    }

    if (count > (ULONG)AMI_CFG_MAX_INTERFACES)
    {
        if (!quiet)
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

        if (!load_interface(name, &ifc, quiet))
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
     * shipped build -- the stack has to outlive the command, so it lives in
     * bsdsocket.library -- and AMI_NET_ERR_STATE is what says so.
     */
    err = netstack_startup();

    if (err == AMI_NET_ERR_STATE)
    {
        struct Library *base;

        /*
         * Starting the network blocks until an address arrives or DHCP gives
         * up, up to half a minute. Say so first, or the pause looks like a
         * hung machine.
         */
        if (!quiet)
            tool_printf("%s: starting the network...\n", (LONG)name);

        base = tool_stack_start();

        if (base == NULL)
        {
            if (!quiet)
            {
                tool_error("the network would not start");
                explain_library_failure(&ifc);
            }
            FreeArgs(rda);
            return RETURN_FAIL;
        }

        if (!tool_stack_is_ours(base))
        {
            if (!quiet)
            {
                tool_error("another TCP/IP stack is installed on this machine");
                tool_explain_foreign_stack(base);
            }

            /*
             * Closed, unlike the success path below.
             *
             * tool_stack_start() leaks its reference on purpose, because that
             * is what keeps OUR network up after this command exits. None of
             * that applies to somebody else's stack: nothing here is going to
             * use it, and holding a reference to a library we are about to
             * complain about only stops its owner unloading it.
             */
            CloseLibrary(base);
            FreeArgs(rda);
            return RETURN_WARN;
        }

        /*
         * Up, and every configured interface with it: the library reads them
         * all once, so a list of names amounts to a single start here.
         *
         * The stack is inside the library, so the address it was given has to
         * be asked for rather than read out of our own memory. This is where
         * TIMEOUT is spent: tool_stack_start() has already waited for the DHCP
         * exchange it starts, and this waits out the rest of the allowance
         * before deciding that nothing answered.
         */
        {
            ULONG addr = 0;
            BOOL  have = wait_for_address(allowance, &addr, &broken);

            if (!quiet)
            {
                char text[16];

                if (have)
                {
                    ami_config_format_ip(addr, text, sizeof(text));
                    tool_printf("%s: online, address %s\n", (LONG)name,
                                (LONG)text);
                }
                else
                {
                    if (!ifc.up)
                    {
                        /*
                         * The file says STATE=down, so there is no address
                         * because none was asked for. Sending someone to check
                         * a cable they deliberately left unplugged is the same
                         * mistake as blaming a driver for a card that opened.
                         */
                        tool_printf("%s: the network is running, and %s is "
                                    "configured down\n", (LONG)name, (LONG)name);
                        tool_printf("  is up and every command works; run  Online %s  to\n",
                                    (LONG)name);
                    }
                    else
                    {
                        tool_printf("%s: the network is running, but this machine "
                                    "has no address yet\n", (LONG)name);

                        if (ifc.iptype == AMI_IPTYPE_DHCP)
                            tool_explain_dhcp(name);
                    }
                }

                for (n = 1; n < count; n++)
                    tool_printf("%s: up, with the others\n",
                                (LONG)tool_basename((const char *)names[n]));
            }
        }

        if (broken)
        {
            tool_fault(ERROR_BREAK);
            FreeArgs(rda);
            return RETURN_WARN;
        }

        FreeArgs(rda);
        return RETURN_OK;
    }

    if (err != AMI_NET_OK)
    {
        if (!quiet)
        {
            tool_error("the network would not start: %s",
                       (LONG)tool_net_error(err));
            explain_startup_failure(err, &ifc);
        }
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
             * the stack was already up when this interface file was added.
             */
            FreeArgs(rda);
            return RETURN_FAIL;
        }

        if (!netstack_interface_is_up((UWORD)index))
        {
            err = netstack_interface_up((UWORD)index);
            if (err != AMI_NET_OK)
            {
                if (!quiet)
                {
                    tool_error("%s would not come online: %s", (LONG)name,
                               (LONG)tool_net_error(err));
                    tool_explain_device(ifc.device, ifc.unit);
                }
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

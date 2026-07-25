/*
 * AddNetInterface -- bring up the interface described by
 * DEVS:NetInterfaces/<name>.
 *
 * This is the command S:User-Startup invokes, so it is the one that actually
 * starts the network:
 *
 *     C:AddNetInterface DEVS:NetInterfaces/eth0 QUIET
 *
 * Roadshow accepts either a bare interface name or the full path to the
 * interface file; so do we -- the name is what matters and the directory is
 * fixed.
 *
 * It is also the first command a new user runs, and the first place they meet
 * anything they got wrong. Every failure below therefore prints what is
 * wrong, where, and what to type next; the terse version still goes to the
 * serial log for whoever is debugging the stack itself.
 *
 * It deliberately does NOT shut the stack down again. The reference taken by
 * tool_stack_start() is what keeps the interface online after this command
 * exits -- exactly Roadshow's model, where the interface stays up until
 * Offline or a reboot.
 *
 * SPDX-License-Identifier: MIT
 */

#include "tools.h"

const char *const tool_name = "AddNetInterface";

static const char version_tag[] __attribute__((used)) =
    "$VER: AddNetInterface 1.1 (25.7.2026)";

#define TEMPLATE    "NAME/A,QUIET/S"

enum
{
    ARG_NAME = 0,
    ARG_QUIET,
    ARG_COUNT
};

/*
 * Whatever the stack said, turned into something the person at the keyboard
 * can act on. `ifc` is the interface file we already parsed, which is what
 * lets the device explanation name the actual driver and unit.
 */
static VOID explain_startup_failure(LONG err, const AmiIfConfig *ifc)
{
    switch (err)
    {
        case AMI_NET_ERR_NODEV:
            tool_explain_device(ifc->device, ifc->unit);
            break;

        case AMI_NET_ERR_CONFIG:
            /*
             * The stack came up but no interface got an address. For a DHCP
             * interface that is nearly always the cable or the server; for a
             * static one it is the file.
             */
            if (ifc->iptype == AMI_IPTYPE_DHCP)
            {
                tool_explain_dhcp(ifc->name);
            }
            else
            {
                tool_advise_blank();
                tool_printf("  %s has no usable address.\n", (LONG)ifc->name);
                tool_advise_blank();
                tool_advise("The interface file says to use a fixed address, so");
                tool_advise("it needs an ADDRESS line and a NETMASK line. Run");
                tool_advise("NetSetup to set them, or add  CONFIGURE = DHCP  to");
                tool_advise("have an address handed out instead.");
            }
            break;

        case AMI_NET_ERR_NOMEM:
            tool_advise_blank();
            tool_advise("There was not enough free memory to start the network.");
            tool_advise("Close some programs and try again; the stack needs");
            tool_advise("roughly 200K free before it will start.");
            break;

        case AMI_NET_ERR_KERNEL:
            tool_advise_blank();
            tool_advise("The network could not be started at all. This is a");
            tool_advise("fault in the stack rather than in your configuration --");
            tool_advise("the serial debug log records what went wrong.");
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
        tool_advise_blank();
        tool_advise("bsdsocket.library is not installed.");
        tool_advise_blank();
        tool_advise("The network stack lives in that library, and it belongs in");
        tool_advise("LIBS:. The installer puts it there; if you copied files by");
        tool_advise("hand, LIBS:bsdsocket.library is the one that matters.");
        return;
    }

    tool_advise_blank();
    tool_advise("bsdsocket.library is installed, so the network stack is here;");
    tool_advise("it was the interface that would not come up.");

    /* Ask the hardware directly rather than speculating about it. */
    tool_explain_device(ifc->device, ifc->unit);

    /*
     * Only worth saying when the card itself was fine -- if the driver is
     * missing there is no point sending the reader to look at cables.
     */
    if (ifc->iptype == AMI_IPTYPE_DHCP &&
        tool_device_where(ifc->device) != NULL &&
        tool_device_probe(ifc->device, ifc->unit) == 0)
    {
        tool_advise_blank();
        tool_advise("The card is fine, so what failed was getting an address:");
        tool_advise("nothing answered. Check the cable, and that something on");
        tool_advise("this network hands out addresses.");
    }
}

int main(int argc, char **argv)
{
    LONG            args[ARG_COUNT];
    struct RDArgs  *rda;
    AmiIfConfig     ifc;
    const char     *name;
    BOOL            quiet;
    LONG            index;
    LONG            err;
    LONG            rc = RETURN_OK;

    (VOID)argv;

    if (tool_from_workbench(argc))
        return RETURN_FAIL;

    tool_break_arm();

    args[ARG_NAME]   = 0;
    args[ARG_QUIET]  = 0;

    rda = ReadArgs((CONST_STRPTR)TEMPLATE, args, NULL);
    if (rda == NULL)
    {
        tool_fault(IoErr());
        tool_usage("<interface name>",
                   "The name of a file in DEVS:NetInterfaces, e.g. eth0.");
        tool_advise("NetSetup writes that file for you if you have none.");
        return RETURN_ERROR;
    }

    name  = tool_basename((const char *)args[ARG_NAME]);
    quiet = (args[ARG_QUIET] != 0) ? TRUE : FALSE;

    /*
     * Parse the interface file first: a typo in User-Startup should produce a
     * clear message rather than a stack that comes up with nothing on it.
     * tool_config_watch() is what puts the parser's complaints -- with line
     * numbers -- on the screen instead of only on the serial port.
     */
    tool_config_watch();
    err = ami_config_load_interface(name, &ifc);
    tool_config_unwatch();

    if (err != AMI_CFG_OK)
    {
        if (err == AMI_CFG_ERR_IO)
        {
            tool_error("there is no interface called \"%s\"", (LONG)name);
            tool_explain_interface_file(name);
        }
        else if (err == AMI_CFG_ERR_SYNTAX)
        {
            /* tool_config_watch() has already printed the detail. */
            tool_error("DEVS:NetInterfaces/%s cannot be used as it stands",
                       (LONG)name);
            tool_advise_blank();
            tool_advise("Fix the line named above, or run  NetSetup  to write");
            tool_advise("the file from scratch.");
        }
        else
        {
            tool_error("out of memory reading DEVS:NetInterfaces/%s", (LONG)name);
        }

        FreeArgs(rda);
        return RETURN_FAIL;
    }

    if (!quiet)
    {
        tool_printf("%s: %s unit %ld\n", (LONG)name, (LONG)ifc.device,
                    (LONG)ifc.unit);
    }

    /*
     * netstack_startup() is idempotent and reference counted, and is the right
     * call when the stack is linked into this command. It is not in the
     * shipped build -- the stack has to outlive the command, so it lives in
     * bsdsocket.library -- and AMI_NET_ERR_STATE is exactly what says so.
     */
    err = netstack_startup();

    if (err == AMI_NET_ERR_STATE)
    {
        struct Library *base;

        /*
         * Starting the network blocks until an address arrives or DHCP gives
         * up, which is up to half a minute of nothing happening. Say so
         * first: a silent pause that long reads as a hung machine.
         */
        if (!quiet)
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
            FreeArgs(rda);
            return RETURN_WARN;
        }

        /*
         * Up. The stack is inside the library, so the address it was given
         * has to be asked for rather than read out of our own memory.
         */
        if (!quiet)
        {
            ULONG addr = 0;
            char  text[16];

            if (tool_stack_query(&addr, NULL, 0) && addr != 0)
            {
                ami_config_format_ip(addr, text, sizeof(text));
                tool_printf("%s: online, address %s\n", (LONG)name, (LONG)text);
            }
            else
            {
                tool_printf("%s: the network is running, but this machine has "
                            "no address yet\n", (LONG)name);

                if (ifc.iptype == AMI_IPTYPE_DHCP)
                    tool_explain_dhcp(name);
            }
        }

        FreeArgs(rda);
        return RETURN_OK;
    }

    if (err != AMI_NET_OK)
    {
        tool_error("the network would not start: %s", (LONG)tool_net_error(err));
        explain_startup_failure(err, &ifc);
        FreeArgs(rda);
        return RETURN_FAIL;
    }

    index = tool_find_interface(name);
    if (index < 0)
    {
        /*
         * The file parses but the running stack does not know the name --
         * the stack was already up when this interface file was added.
         */
        tool_advise_blank();
        tool_advise("The network was already running when this interface file");
        tool_advise("was added, and interfaces are read once at startup. Reboot,");
        tool_advise("or take the network down and start it again.");
        FreeArgs(rda);
        return RETURN_FAIL;
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

    if (!quiet)
    {
        NX_IP *ip = netstack_ip();
        char   addr[16];
        char   mask[16];
        ULONG  live_addr = 0;
        ULONG  live_mask = 0;

        /*
         * The address a DHCP interface ends up with is not the one in the
         * config file -- the file says "DHCP" and has zeroes in it. Read the
         * interface, which is where the lease landed; printing the file's
         * fields here is what produced "eth0: 0.0.0.0 netmask 0.0.0.0 (DHCP)"
         * one line after a successful lease.
         */
        if (ip != NULL)
        {
            live_addr = ip->nx_ip_interface[index].nx_interface_ip_address;
            live_mask = ip->nx_ip_interface[index].nx_interface_ip_network_mask;
        }

        if (live_addr == 0)
        {
            tool_printf("%s: online, but it has no address yet\n", (LONG)name);

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

    if (tool_break())
    {
        tool_fault(ERROR_BREAK);
        rc = RETURN_WARN;
    }

    FreeArgs(rda);
    return (int)rc;
}

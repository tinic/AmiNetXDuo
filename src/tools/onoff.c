/*
 * Online / Offline -- bring a named interface up or down.
 *
 *     Online  INTERFACE/A,QUIET/S
 *     Offline INTERFACE/A,QUIET/S
 *
 * One source, two executables: TOOL_OFFLINE picks which. The asymmetry that
 * matters is that Online may have to start the network (the user may never
 * have run AddNetInterface), while Offline never does -- taking an interface
 * down on a machine with no stack is a no-op worth saying out loud rather than
 * a reason to boot the whole thing.
 *
 * Both check the interface file before they touch anything, so that a
 * mistyped name is answered with "there is no such interface, here are the
 * ones there are" rather than with a fact about the stack.
 *
 * SPDX-License-Identifier: MIT
 */

#include "tools.h"

#ifdef TOOL_OFFLINE
const char *const tool_name = "Offline";
static const char version_tag[] __attribute__((used)) =
    "$VER: Offline 1.1 (25.7.2026)";
#else
const char *const tool_name = "Online";
static const char version_tag[] __attribute__((used)) =
    "$VER: Online 1.1 (25.7.2026)";
#endif

#define TEMPLATE    "INTERFACE/A,QUIET/S"

enum
{
    ARG_INTERFACE = 0,
    ARG_QUIET,
    ARG_COUNT
};

/*
 * Read DEVS:NetInterfaces/<name> purely to be able to talk about it. Returns
 * FALSE after explaining what is wrong with it, in which case the caller has
 * nothing left to do.
 */
static BOOL load_interface(const char *name, AmiIfConfig *ifc)
{
    LONG err;

    tool_config_watch();
    err = ami_config_load_interface(name, ifc);
    tool_config_unwatch();

    if (err == AMI_CFG_OK)
        return TRUE;

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

int main(int argc, char **argv)
{
    LONG           args[ARG_COUNT];
    struct RDArgs *rda;
    AmiIfConfig    ifc;
    const char    *name;
    BOOL           quiet;
    LONG           index;
    LONG           err;

    (VOID)argv;

    if (tool_from_workbench(argc))
        return RETURN_FAIL;

    tool_break_arm();

    args[ARG_INTERFACE] = 0;
    args[ARG_QUIET]     = 0;

    rda = ReadArgs((CONST_STRPTR)TEMPLATE, args, NULL);
    if (rda == NULL)
    {
        tool_fault(IoErr());
        tool_usage("<interface name>",
                   "The name of a file in DEVS:NetInterfaces, e.g. eth0.");
        return RETURN_ERROR;
    }

    name  = tool_basename((const char *)args[ARG_INTERFACE]);
    quiet = (args[ARG_QUIET] != 0) ? TRUE : FALSE;

    if (!load_interface(name, &ifc))
    {
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
            ULONG addr = 0;
            char  text[16];

            if (!quiet)
            {
                tool_printf("%s: the network is already running.\n", (LONG)name);

                if (tool_stack_query(&addr, NULL, 0) && addr != 0)
                {
                    ami_config_format_ip(addr, text, sizeof(text));
                    tool_printf("  This machine's address is %s.\n", (LONG)text);
                }

                tool_advise_blank();
                tool_advise("Individual interfaces cannot be taken up and down");
                tool_advise("while the stack runs; that needs a call the library");
                tool_advise("does not have yet. Reboot to change what is up.");
            }

            FreeArgs(rda);
            return RETURN_OK;
        }

        {
            struct Library *base;

            /* Up to half a minute while DHCP is asked; do not sit silent. */
            if (!quiet)
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

        if (!quiet)
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
        {
            tool_error("%s cannot be taken offline", (LONG)name);
            tool_advise_blank();
            tool_advise("The network is running inside bsdsocket.library, and");
            tool_advise("there is no call yet that lets a command take one of");
            tool_advise("its interfaces down. Rebooting is the way to stop it.");
            FreeArgs(rda);
            return RETURN_WARN;
        }

        if (!quiet)
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
        if (!quiet)
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

    if (!quiet)
        tool_printf("%s is offline\n", (LONG)name);
#else
    if (netstack_interface_is_up((UWORD)index))
    {
        if (!quiet)
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

    if (!quiet)
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

    if (tool_break())
    {
        tool_fault(ERROR_BREAK);
        FreeArgs(rda);
        return RETURN_WARN;
    }

    FreeArgs(rda);
    return RETURN_OK;
}

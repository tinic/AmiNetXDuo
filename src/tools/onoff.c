/*
 * Online / Offline -- bring a named interface up or down.
 *
 *     Online  INTERFACE/A,QUIET/S
 *     Offline INTERFACE/A,QUIET/S
 *
 * One source, two executables: TOOL_OFFLINE picks which. The asymmetry that
 * matters is that Online may have to start the stack (the user may never have
 * run AddNetInterface), while Offline never does -- taking an interface down
 * on a machine with no stack is a no-op worth saying out loud rather than a
 * reason to boot the whole thing.
 *
 * SPDX-License-Identifier: MIT
 */

#include "tools.h"

#ifdef TOOL_OFFLINE
const char *const tool_name = "Offline";
static const char version_tag[] __attribute__((used)) =
    "$VER: Offline 1.0 (24.7.2026)";
#else
const char *const tool_name = "Online";
static const char version_tag[] __attribute__((used)) =
    "$VER: Online 1.0 (24.7.2026)";
#endif

#define TEMPLATE    "INTERFACE/A,QUIET/S"

enum
{
    ARG_INTERFACE = 0,
    ARG_QUIET,
    ARG_COUNT
};

int main(int argc, char **argv)
{
    LONG           args[ARG_COUNT];
    struct RDArgs *rda;
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
        return RETURN_ERROR;
    }

    name  = tool_basename((const char *)args[ARG_INTERFACE]);
    quiet = (args[ARG_QUIET] != 0) ? TRUE : FALSE;

#ifndef TOOL_OFFLINE
    err = netstack_startup();
    if (err != AMI_NET_OK)
    {
        tool_error("cannot start the network stack: %s",
                   (LONG)tool_net_error(err));
        FreeArgs(rda);
        return RETURN_FAIL;
    }
#else
    if (netstack_get() == NULL)
    {
        tool_error("the network stack is not running, so %s is already offline",
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
        tool_error("cannot bring %s online: %s", (LONG)name,
                   (LONG)tool_net_error(err));
        FreeArgs(rda);
        return RETURN_FAIL;
    }

    if (!quiet)
    {
        const AmiConfig *cfg = netstack_config();
        char             addr[16];

        if (cfg != NULL && (UWORD)index < cfg->interface_count)
        {
            ami_config_format_ip(cfg->interfaces[index].address,
                                 addr, sizeof(addr));
            tool_printf("%s is online, address %s\n", (LONG)name, (LONG)addr);
        }
        else
        {
            tool_printf("%s is online\n", (LONG)name);
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

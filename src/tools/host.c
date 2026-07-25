/*
 * host -- forward and reverse DNS lookup.
 *
 *     host NAME/A,TIMEOUT/N/K
 *
 * A dotted quad is looked up backwards, anything else forwards; that is what
 * the Unix tool of the same name does and there is no reason to be different.
 * TIMEOUT is in seconds (default 10).
 *
 * SPDX-License-Identifier: MIT
 */

#include "tools.h"

const char *const tool_name = "host";

static const char version_tag[] __attribute__((used)) =
    "$VER: host 1.0 (24.7.2026)";

#define TEMPLATE    "NAME/A,TIMEOUT/N/K"

enum
{
    ARG_NAME = 0,
    ARG_TIMEOUT,
    ARG_COUNT
};

#define HOST_DEFAULT_TIMEOUT    10UL        /* seconds */
#define HOST_NAME_MAX           256

int main(int argc, char **argv)
{
    LONG           args[ARG_COUNT];
    struct RDArgs *rda;
    const char    *name;
    ULONG          timeout;
    ULONG          ticks;
    ULONG          addr = 0;
    LONG           err;
    char           text[HOST_NAME_MAX];

    (VOID)argv;

    if (tool_from_workbench(argc))
        return RETURN_FAIL;

    tool_break_arm();

    args[ARG_NAME]    = 0;
    args[ARG_TIMEOUT] = 0;

    rda = ReadArgs((CONST_STRPTR)TEMPLATE, args, NULL);
    if (rda == NULL)
    {
        tool_fault(IoErr());
        return RETURN_ERROR;
    }

    name    = (const char *)args[ARG_NAME];
    timeout = (args[ARG_TIMEOUT] != 0)
                  ? (ULONG)(*(LONG *)args[ARG_TIMEOUT])
                  : HOST_DEFAULT_TIMEOUT;
    if (timeout == 0)
        timeout = HOST_DEFAULT_TIMEOUT;

    /* netstack_resolve takes ThreadX ticks. */
    ticks = timeout * TX_TIMER_TICKS_PER_SECOND;

    if (tool_require_stack() == NULL)
    {
        FreeArgs(rda);
        return RETURN_ERROR;
    }

    if (ami_config_parse_ip(name, &addr))
    {
        err = netstack_resolve_reverse(addr, text, sizeof(text), ticks);
        if (err != AMI_NET_OK)
        {
            tool_error("no name for %s: %s", (LONG)name,
                       (LONG)tool_net_error(err));
            FreeArgs(rda);
            return RETURN_ERROR;
        }

        tool_printf("%s is %s\n", (LONG)name, (LONG)text);
    }
    else
    {
        err = netstack_resolve(name, &addr, ticks);
        if (err != AMI_NET_OK)
        {
            tool_error("cannot resolve \"%s\": %s", (LONG)name,
                       (LONG)tool_net_error(err));
            FreeArgs(rda);
            return RETURN_ERROR;
        }

        ami_config_format_ip(addr, text, sizeof(text));
        tool_printf("%s has address %s\n", (LONG)name, (LONG)text);
    }

    if (tool_break())
    {
        tool_fault(ERROR_BREAK);
        FreeArgs(rda);
        return RETURN_WARN;
    }

    FreeArgs(rda);
    return RETURN_OK;
}

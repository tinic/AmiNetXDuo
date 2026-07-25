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

    /*
     * The usual case is a stack running inside bsdsocket.library, which this
     * command cannot reach through netstack_resolve() -- but gethostbyname()
     * is a published vector and answers from the same resolver, DHCP-supplied
     * name servers included. Ask that first, and only fall back to the
     * in-process stack (which exists in a build that links one) after.
     */
    if (netstack_get() == NULL && tool_stack_library_running())
    {
        BOOL ok;

        if (ami_config_parse_ip(name, &addr))
        {
            ok = tool_stack_lookup_addr(addr, text, sizeof(text));
            if (ok)
                tool_printf("%s is %s\n", (LONG)name, (LONG)text);
            else
                tool_error("no name for %s", (LONG)name);
        }
        else
        {
            ok = tool_stack_lookup(name, &addr);
            if (ok)
            {
                ami_config_format_ip(addr, text, sizeof(text));
                tool_printf("%s has address %s\n", (LONG)name, (LONG)text);
            }
            else
            {
                tool_error("cannot resolve \"%s\"", (LONG)name);
            }
        }

        if (!ok)
        {
            /*
             * gethostbyname() reports failure without a reason a command can
             * read, and "not known" is much the likeliest one.
             */
            tool_explain_resolve(name, AMI_NET_ERR_NONAME);
        }

        FreeArgs(rda);
        return ok ? RETURN_OK : RETURN_ERROR;
    }

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
            tool_explain_resolve(name, err);
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
            tool_explain_resolve(name, err);
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

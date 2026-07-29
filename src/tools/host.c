/*
 * host -- resolve a name the way this machine's own programs would.
 *
 *     host NAME/A,TIMEOUT/N/K
 *
 * Asks the machine's resolver through bsdsocket.library's gethostbyname() /
 * gethostbyaddr(), so the answer comes from the whole chain, in the order the
 * chain uses it:
 *
 *     DEVS:Internet/hosts     names written down on this machine
 *     mDNS (.local)           names the local network answers for itself
 *     the resolver cache      anything looked up recently
 *     the name servers        DEVS:Internet/name_resolution, or the DHCP lease
 *
 * A dotted quad is looked up backwards and anything else forwards, as the Unix
 * tool of the same name does. TIMEOUT is accepted and ignored: the timeout is
 * the resolver's, set in DEVS:Internet/name_resolution.
 *
 * A forward lookup goes through getaddrinfo() with AF_UNSPEC, so a name with
 * an AAAA record reports it. gethostbyname() cannot: its hostent carries one
 * address family and this library only ever fills it with IPv4.
 *
 * nslookup differs in that it bypasses the resolver: it builds a DNS query and
 * sends it to a name server itself, so it reports what that server said -- no
 * hosts file, no mDNS, no cache -- and can ask for record types the resolver has
 * no call for (MX, TXT, NS, SRV, SOA) against a nominated server. Comparing the
 * two gives the diagnosis:
 *
 *     both agree                  the name is fine
 *     host works, nslookup fails  the answer came from this machine --
 *                                 DEVS:Internet/hosts, or mDNS, or the cache
 *     host fails, nslookup works  this machine's resolver configuration is
 *                                 wrong; not the network and not the name
 *     both fail                   the name, the servers, or the network
 *
 * Like every other client command, host calls tool_socket_open() and so starts
 * the stack if nothing else has. An earlier version instead checked whether
 * something else had started it and gave up if not.
 *
 * SPDX-License-Identifier: MIT
 */

#include "toolsock.h"

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
    LONG            args[ARG_COUNT];
    struct RDArgs  *rda;
    struct Library *sbase;
    const char     *name;
    ULONG           addr = 0;
    BOOL            ok;
    char            text[HOST_NAME_MAX];
    ToolAddrInfo    hints;
    ToolAddrInfo   *list = NULL;

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

    name = (const char *)args[ARG_NAME];

    /*
     * Starts the stack if nothing else has. The base is not closed: that open
     * reference keeps the network up afterwards, as in every other client
     * command.
     */
    sbase = tool_socket_open();
    if (sbase == NULL)
    {
        FreeArgs(rda);
        return RETURN_FAIL;
    }

    if (ami_config_parse_ip(name, &addr))
    {
        ok = tool_stack_lookup_addr(addr, text, sizeof(text));
        if (ok)
            tool_printf("%s is %s\n", (LONG)name, (LONG)text);
        else
            tool_error("no name for %s", (LONG)name);
    }
    else if (tool_sock_have_lvo(sbase, 0x330UL))
    {
        hints.ai_flags     = 0;
        hints.ai_family    = TOOL_AF_UNSPEC;
        hints.ai_socktype  = TOOL_SOCK_STREAM;
        hints.ai_protocol  = 0;
        hints.ai_addrlen   = 0;
        hints.ai_addr      = NULL;
        hints.ai_canonname = NULL;
        hints.ai_next      = NULL;

        ok = FALSE;

        if (tool_sock_getaddrinfo(sbase, name, NULL, &hints, &list) == 0)
        {
            ToolAddrInfo *ai;

            for (ai = list; ai != NULL; ai = ai->ai_next)
            {
                ToolAddr found;

                if (ai->ai_addr == NULL ||
                    !tool_sock_addr_get(ai->ai_addr, &found))
                    continue;

                tool_addr_text(sbase, &found, text, sizeof(text));
                tool_printf("%s has %saddress %s\n", (LONG)name,
                            (LONG)(TOOL_ADDR_IS6(&found) ? "IPv6 " : ""),
                            (LONG)text);
                ok = TRUE;
            }

            tool_sock_freeaddrinfo(sbase, list);
        }

        if (!ok)
            tool_error("cannot resolve \"%s\"", (LONG)name);
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
         * gethostbyname() fails without a reason a command can read, and the
         * two candidates need opposite actions from the user, so print both.
         * nslookup tells them apart.
         */
        tool_explain_resolve(name, AMI_NET_ERR_NONAME);
        tool_advise("nslookup will say whether the name servers answer.");
    }

    if (tool_break())
    {
        tool_fault(ERROR_BREAK);
        FreeArgs(rda);
        return RETURN_WARN;
    }

    FreeArgs(rda);
    return ok ? RETURN_OK : RETURN_ERROR;
}

/*
 * host, resolve a name the way this machine's own programs would.
 *
 *     host NAME/A,TIMEOUT/N/K,IPV4=-4/S,IPV6=-6/S
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
 * tool of the same name does. An IPv6 literal is refused with the reason: it
 * is an address, so it would be looked up backwards, and no call this command
 * can reach reverses one. nslookup does, by writing the ip6.arpa query itself.
 * TIMEOUT is accepted and ignored: the timeout is the resolver's, set in
 * DEVS:Internet/name_resolution.
 *
 * A forward lookup goes through getaddrinfo() with AF_UNSPEC, so a name with
 * an AAAA record reports it. gethostbyname() cannot: its hostent carries one
 * address family and this library only ever fills it with IPv4.
 *
 * -4 and -6 mean something different here than in the commands that open a
 * socket. Those pin the family they connect over. This command connects
 * to nothing, so the pair asks for one kind of record and not the other: -4
 * reports the A records, -6 the AAAA. It is what nslookup already does through
 * TYPE=A / TYPE=AAAA, which host has no equivalent of. Both together is an
 * argument error rather than a silent AF_UNSPEC, since asking for exactly one
 * of two things twice cannot be meant.
 *
 * Neither applies to a numeric argument: an address is looked up backwards and
 * already declares its own family. -6 on a dotted quad contradicts the
 * argument and is refused as such.
 *
 * nslookup differs in that it bypasses the resolver: it builds a DNS query and
 * sends it to a name server itself, so it reports what that server said, no
 * hosts file, no mDNS, no cache, and can ask for record types the resolver has
 * no call for (MX, TXT, NS, SRV, SOA) against a nominated server. Comparing the
 * two gives the diagnosis:
 *
 *     both agree                  the name is fine
 *     host works, nslookup fails  the answer came from this machine,
 *                                 DEVS:Internet/hosts, or mDNS, or the cache
 *     host fails, nslookup works  this machine's resolver configuration is
 *                                 wrong, not the network and not the name
 *     both fail                   the name, the servers, or the network
 *
 * Like every other client command, host calls tool_socket_open() and so starts
 * the stack if nothing else has. An earlier version instead checked whether
 * something else had started it and gave up if not.
 *
 * SPDX-License-Identifier: MIT
 */

#include "toolsock.h"
#include "aminetxduo/version.h"

const char *const tool_name = "host";

static const char version_tag[] __attribute__((used)) =
    TOOL_VERSTAG("host");

#define TEMPLATE    "NAME/A,TIMEOUT/N/K,IPV4=-4/S,IPV6=-6/S"

enum
{
    ARG_NAME = 0,
    ARG_TIMEOUT,
    ARG_IPV4,
    ARG_IPV6,
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
    LONG            family;
    BOOL            ok;
    BOOL            no_family = FALSE;
    char            text[HOST_NAME_MAX];
    ToolAddrInfo    hints;
    ToolAddrInfo   *list = NULL;
    ULONG           v6[4];

    (VOID)argv;

    if (tool_from_workbench(argc))
        return RETURN_FAIL;

    tool_break_arm();

    args[ARG_NAME]    = 0;
    args[ARG_TIMEOUT] = 0;
    args[ARG_IPV4]    = 0;
    args[ARG_IPV6]    = 0;

    rda = ReadArgs((CONST_STRPTR)TEMPLATE, args, NULL);
    if (rda == NULL)
    {
        tool_fault(IoErr());
        return RETURN_ERROR;
    }

    name = (const char *)args[ARG_NAME];

    if (!tool_arg_family(args[ARG_IPV4], args[ARG_IPV6], &family))
    {
        FreeArgs(rda);
        return RETURN_ERROR;
    }

    /*
     * An IPv6 literal, in either build. host answers through the machine's
     * resolver, and the resolver reverses IPv4 addresses only, there is no
     * ip6.arpa call behind gethostbyaddr() or getnameinfo() to ask. Handing
     * the literal to the forward path instead is what used to produce
     * "cannot resolve", with advice about spelling and name servers that has
     * nothing to do with an address.
     *
     * Checked before the library is opened, so a question host cannot answer
     * does not start the network to say so. A dotted quad never gets here:
     * the IPv6 grammar does not accept one on its own.
     */
    if (tool_parse_ip6(name, v6))
    {
        tool_error("\"%s\" is an address, not a name", (LONG)name);
        tool_printf("  nslookup asks the DNS for its ip6.arpa record.\n");
        FreeArgs(rda);
        return RETURN_ERROR;
    }

    /* A dotted quad is looked up backwards, and an address already says which
       family it is. -6 over one contradicts the argument. */
    if (family == TOOL_AF_INET6 && ami_config_parse_ip(name, &addr))
    {
        tool_error("%s is an IPv4 address, and -6 was given", (LONG)name);
        FreeArgs(rda);
        return RETURN_ERROR;
    }

    /*
     * Starts the stack if nothing else has, and is closed again below like
     * every other client command closes its own. Leaving it open cost one
     * cloned AmiSocketBase, 2568 bytes, on every single run.
     */
    sbase = tool_socket_open();
    if (sbase == NULL)
    {
        FreeArgs(rda);
        return RETURN_FAIL;
    }

    /* Same as tool_sock_resolve_af(): -6 on a machine with no IPv6 is a fact
       about the machine, not about the name. */
    if (family == TOOL_AF_INET6 && !tool_sock_have_ipv6(sbase))
    {
        tool_error("%s: this machine's network has no IPv6", (LONG)name);
        tool_no_ipv6_note();
        CloseLibrary(sbase);
        FreeArgs(rda);
        return RETURN_ERROR;
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
        hints.ai_family    = family;
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

        if (!ok && tool_sock_family_absent(sbase, name, family))
        {
            tool_sock_say_no_family(name, family);
            no_family = TRUE;
        }
        else if (!ok)
        {
            tool_error("cannot resolve \"%s\"", (LONG)name);
        }
    }
    else if (family == TOOL_AF_INET6)
    {
        /* gethostbyname() answers with an A and nothing else, so there is no
           way to ask this library for the AAAA. */
        ok = FALSE;
        tool_error("this bsdsocket.library has no getaddrinfo, so -6 cannot "
                   "be answered");
        no_family = TRUE;
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

    if (!ok && !no_family)
    {
        /*
         * gethostbyname() fails without a reason a command can read, and the
         * two candidates need opposite actions from the user, so print both.
         * nslookup tells them apart.
         */
        tool_explain_resolve(name, AMI_NET_ERR_NONAME);
    }

    if (tool_break())
    {
        tool_fault(ERROR_BREAK);
        CloseLibrary(sbase);
        FreeArgs(rda);
        return RETURN_WARN;
    }

    CloseLibrary(sbase);
    FreeArgs(rda);
    return ok ? RETURN_OK : RETURN_ERROR;
}

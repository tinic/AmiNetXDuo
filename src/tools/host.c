/*
 * host -- what does THIS MACHINE make of a name?
 *
 *     host NAME/A,TIMEOUT/N/K
 *
 * WHAT IT ACTUALLY DOES, because the name does not say
 *
 * It asks the machine's own resolver, through bsdsocket.library's
 * gethostbyname()/gethostbyaddr(), exactly as any program on this Amiga does.
 * So the answer comes from the whole chain, in the order the chain uses it:
 *
 *     DEVS:Internet/hosts     names written down on this machine
 *     mDNS (.local)           names the local network answers for itself
 *     the resolver cache      anything looked up recently
 *     the name servers        DEVS:Internet/name_resolution, or the DHCP lease
 *
 * That makes it the answer to "what will my programs get when they ask for
 * this?", which is the question worth asking first, because it is what
 * everything else on the machine is going to act on.
 *
 * A dotted quad is looked up backwards and anything else forwards, as the Unix
 * tool of the same name does. TIMEOUT is accepted and ignored: the resolver
 * owns the timeout and it is set in DEVS:Internet/name_resolution.
 *
 * HOW IT DIFFERS FROM nslookup, WHICH IS WHY BOTH EXIST
 *
 * nslookup does not use the resolver. It builds a DNS query and sends it to a
 * name server itself, so it reports what that SERVER said -- no hosts file, no
 * mDNS, no cache -- and it can ask for record types the resolver has no call
 * for (MX, TXT, NS, SRV, SOA) against a server you nominate.
 *
 * The difference between the two answers is the diagnosis:
 *
 *     both agree                  the name is fine
 *     host works, nslookup fails  the answer came from THIS machine --
 *                                 DEVS:Internet/hosts, or mDNS, or the cache
 *     host fails, nslookup works  this machine's resolver configuration is
 *                                 wrong; not the network and not the name
 *     both fail                   the name, the servers, or the network
 *
 * IT STARTS THE NETWORK, AND IT USED NOT TO
 *
 * host was the only client command that refused to. Every other one -- fetch,
 * nc, telnet, tftp, whois, traceroute, ping, nslookup -- calls
 * tool_socket_open(), and opening bsdsocket.library is what brings the stack
 * up. host instead checked whether somebody ELSE had already started it and
 * gave up if not, then advised running AddNetInterface: work it could have
 * done itself. Asking a question is not a reason to refuse to start.
 *
 * (It also carried a second, unreachable implementation over
 * netstack_resolve(). The tools do not link src/netstack -- they get the weak
 * stubs, whose netstack_resolve() is `moveq #-5,d0 / rts` -- so that half was
 * dead code in every build ever shipped. It is gone.)
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
     * Starts the stack if nothing else has. The base is deliberately not
     * closed -- that open reference is what keeps the network up afterwards,
     * which is the same bargain every other client command makes.
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
         * two candidates want opposite actions from the user, so say both
         * rather than pick one. nslookup is how you tell them apart.
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

/*
 * CensusProbe, the guest half of the allocation census. Built only when
 * AMINETXDUO_ALLOCCENSUS is on. TCP has to be off for the expunge to happen:
 * bsd_lib_expunge() declines while the handler Process is alive.
 *
 * SPDX-License-Identifier: MIT
 */

#include "toolsock.h"
#include "aminetxduo/version.h"
#include "aminetxduo/compat.h"

#include <exec/memory.h>
#include <proto/exec.h>

const char *const tool_name = "censusprobe";

static const char version_tag[] __attribute__((used)) =
    TOOL_VERSTAG("CensusProbe");

#define TEMPLATE    "HOST/K,ROUNDS/N/K,NOFLUSH/S"

enum
{
    ARG_HOST = 0,
    ARG_ROUNDS,
    ARG_NOFLUSH,
    ARG_COUNT
};

/* The house summary line, so tools/test-verdict.sh can read this run. */
static ULONG probe_checks;
static ULONG probe_failures;

static VOID probe_check(const char *what, BOOL ok)
{
    probe_checks++;
    if (!ok)
    {
        probe_failures++;
        tool_printf("censusprobe: FAIL %s\n", (LONG)what);
    }
}

/*
 * One pass of what an ordinary network command does: open the library, make
 * sockets of both kinds, ask the resolver something, give it all back.
 */
static VOID probe_round(const char *host)
{
    struct Library *sb;
    LONG            s;
    ToolServEnt    *se;
    ToolHostEnt    *he;

    sb = tool_socket_open();
    probe_check("OpenLibrary", sb != NULL);
    if (sb == NULL)
        return;

    s = tool_sock_socket(sb, TOOL_AF_INET, TOOL_SOCK_DGRAM, 0);
    probe_check("socket UDP", s >= 0);
    if (s >= 0)
        (VOID)tool_sock_close(sb, s);

    s = tool_sock_socket(sb, TOOL_AF_INET, TOOL_SOCK_STREAM, 0);
    probe_check("socket TCP", s >= 0);
    if (s >= 0)
        (VOID)tool_sock_close(sb, s);

    /* Reads DEVS:Internet/services, one of the tables the library parses once
       and holds until the expunge. */
    se = tool_sock_getservbyname(sb, "http", "tcp");
    probe_check("getservbyname http/tcp", se != NULL);

    /* DEVS:Internet/hosts, then the resolver. A miss is a fine outcome: the
       allocation path is the same. */
    he = tool_sock_gethostbyname(sb, host);
    (VOID)he;
    probe_checks++;

    /*
     * getaddrinfo() is the other resolver, with an allocation surface of its
     * own and a free the caller has to make.
     */
    if (tool_sock_have_lvo(sb, 0x32aUL))
    {
        ToolAddrInfo *ai = NULL;

        if (tool_sock_getaddrinfo(sb, host, "http", NULL, &ai) == 0 &&
            ai != NULL)
        {
            tool_sock_freeaddrinfo(sb, ai);
        }
        probe_checks++;
    }

    /* A datagram that goes out on the wire: the send path takes packets from
       the pool and mbufs from src/mbuf. */
    s = tool_sock_socket(sb, TOOL_AF_INET, TOOL_SOCK_DGRAM, 0);
    if (s >= 0)
    {
        ToolSockAddrAny to;

        /* Any address at all: nothing waits for a reply, and the allocation
           this probe is counting happens in the send.  10.0.2.3 is chosen
           because it is unroutable on every segment this runs on, so the
           datagram cannot reach a real name server. */
        (VOID)tool_sock_addr_v4(&to, 0x0A000203UL, 53);
        (VOID)tool_sock_sendto(sb, s, "\0\0\1\0\0\0\0\0\0\0\0\0", 12, &to);
        (VOID)tool_sock_close(sb, s);
        probe_checks++;
    }

    CloseLibrary(sb);
}

/*
 * `avail flush`: ask for a block nothing can satisfy. Exec answers by calling
 * every expunge vector and trying again, so the failure is the point.
 */
static VOID probe_flush(VOID)
{
    APTR huge = AllocMem(0x7FFFFFF0UL, MEMF_ANY);

    if (huge != NULL)
        FreeMem(huge, 0x7FFFFFF0UL);    /* only on a machine with 2 GB */
}

int main(void)
{
    LONG            args[ARG_COUNT];
    struct RDArgs  *rda;
    const char     *host   = "localhost";
    LONG            rounds = 1;
    BOOL            flush  = TRUE;
    LONG            i;

    for (i = 0; i < (LONG)ARG_COUNT; i++)
        args[i] = 0;

    rda = ReadArgs((CONST_STRPTR)TEMPLATE, args, NULL);
    if (rda != NULL)
    {
        if (args[ARG_HOST] != 0)
            host = (const char *)args[ARG_HOST];
        if (args[ARG_ROUNDS] != 0)
            rounds = *(LONG *)args[ARG_ROUNDS];
        if (args[ARG_NOFLUSH] != 0)
            flush = FALSE;
    }

    if (rounds < 1)
        rounds = 1;

    tool_printf("censusprobe: %ld round(s), host %s\n", rounds, (LONG)host);

    for (i = 0; i < rounds; i++)
        probe_round(host);

    /*
     * The library is closed now. This is the command's own census: src/common
     * is linked into every image separately, so the two never mix.
     */
    AMI_CENSUS_REPORT("cmd-censusprobe");

    if (flush)
    {
        probe_flush();
        tool_printf("censusprobe: flushed\n");
    }

    if (rda != NULL)
        FreeArgs(rda);

    tool_printf("censusprobe: %lu checks, %lu failures\n",
                (LONG)probe_checks, (LONG)probe_failures);

    return (probe_failures == 0) ? RETURN_OK : RETURN_FAIL;
}

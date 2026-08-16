/*
 * CensusProbe, the guest half of the allocation census.
 *
 *     CensusProbe HOST/K,ROUNDS/N/K,NOFLUSH/S
 *
 * Built only when AMINETXDUO_ALLOCCENSUS is on, and it is the reason the
 * census is testable rather than readable: something has to reach the two
 * moments the library reports at, and no existing command reaches the second
 * one.
 *
 *   bsd-stack-down   the last close, when the stack comes down.  Any command
 *                    that opens and closes bsdsocket.library reaches this.
 *   bsd-expunge      the segment being unloaded.  Nothing reaches this on its
 *                    own: AmigaOS expunges a library only when it is short of
 *                    memory, so `avail flush` asks for a block nothing can
 *                    satisfy in order to provoke the sweep, and that is what
 *                    the AllocMem() below is.
 *
 * The expunge is where the answer that matters comes from.  At the last close
 * the library is still loaded and is entitled to hold things -- the parsed
 * DEVS:Internet tables, deliberately, so a second program does not re-read
 * them.  Once the segment goes there is nothing left to own anything, and
 * whatever the census still holds is memory that survives until reboot.
 *
 * TCP has to be off for the expunge to happen.  bsd_lib_expunge() declines
 * while the handler Process is alive, and the handler is started by the first
 * open.  tools/alloc-census.sh writes DEVS:Internet/tcp_handler accordingly,
 * and this program says so when the flush does not take.
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
 * sockets of both kinds, ask the resolver something, give it all back.  This
 * walks the allocation sites a command walks, so the census has something to
 * hold.  Testing any of it is the job of tests/.
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

    /* Reads DEVS:Internet/services, which is one of the tables the library
       parses once and holds until the expunge. */
    se = tool_sock_getservbyname(sb, "http", "tcp");
    probe_check("getservbyname http/tcp", se != NULL);

    /* DEVS:Internet/hosts, then the resolver.  A miss is a fine outcome: the
       allocation path is the same and a run with no name server still walks
       it, which is what the census is watching. */
    he = tool_sock_gethostbyname(sb, host);
    (VOID)he;
    probe_checks++;

    /*
     * getaddrinfo() is the other resolver, with an allocation surface of its
     * own (src/bsdsocket/addrinfo.c) and a free the caller has to make.  A
     * caller that forgets freeaddrinfo() is the leak shape this whole
     * instrument is for, so it is walked here deliberately.
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
       the pool and mbufs from src/mbuf, neither of which the socket calls
       above reach. */
    s = tool_sock_socket(sb, TOOL_AF_INET, TOOL_SOCK_DGRAM, 0);
    if (s >= 0)
    {
        ToolSockAddrAny to;

        /* The SLIRP forwarder, which tests/netstack/devs says is 10.0.2.3.
           Nothing waits for a reply: the allocation is in the send. */
        (VOID)tool_sock_addr_v4(&to, 0x0A000203UL, 53);
        (VOID)tool_sock_sendto(sb, s, "\0\0\1\0\0\0\0\0\0\0\0\0", 12, &to);
        (VOID)tool_sock_close(sb, s);
        probe_checks++;
    }

    CloseLibrary(sb);
}

/*
 * `avail flush`: ask for a block nothing can satisfy.  Exec answers by calling
 * every library's and device's expunge vector and trying again, so the failure
 * is the point and the NULL is the expected return.
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
     * The library is closed now, so the reports it prints have already gone to
     * the serial port.  This is the command's own census: src/common is linked
     * into every image separately, so a command has a table of its own and the
     * two never mix.  Ownership is therefore answered per image, which is the
     * granularity it has.
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

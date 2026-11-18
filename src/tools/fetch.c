/*
 * fetch, retrieve an http:// or https:// URL.
 *
 *     fetch URL/A,TO/K,HEADERS/S,QUIET/S,NOVERIFY/S,TIMEOUT/N/K,
 * SPDX-License-Identifier: MIT
 */

#include "toolsock.h"

#include <exec/memory.h>
#include <exec/tasks.h>

#include "aminetxduo/version.h"

#include "fetchcore.h"

const char *const tool_name = "fetch";

static const char version_tag[] __attribute__((used)) =
    TOOL_VERSTAG("fetch");

#define TEMPLATE    "URL/A,TO/K,HEADERS/S,QUIET/S,NOVERIFY/S,TIMEOUT/N/K," \
                    "IPV4=-4/S,IPV6=-6/S"

enum
{
    ARG_URL = 0,
    ARG_TO,
    ARG_HEADERS,
    ARG_QUIET,
    ARG_NOVERIFY,
    ARG_TIMEOUT,
    ARG_IPV4,
    ARG_IPV6,
    ARG_COUNT
};

#define FETCH_DEFAULT_TIMEOUT   120UL       /* seconds */
#define FETCH_TIMEOUT_MAX       2147483L    /* signed milliseconds fit */


static BOOL url_parse(const char *url, FetchUrl *out)
{
    FetchUrlResult why = fetch_url_parse(url, out);

    if (why == FETCH_URL_OK)
        return TRUE;

    tool_error("\"%s\": %s", (LONG)url, (LONG)fetch_url_error(why));

    return FALSE;
}


/* The sink: a TO file, or standard output. */
struct FetchState
{
    BPTR        out;                /* 0 until the first body byte        */
    const char *to;                 /* the TO argument, or NULL           */
    BOOL        failed;
};

/* The destination, opened (and a TO file truncated) once.  FALSE when it
   will not open; the failure is reported and recorded. */
static BOOL fetch_open_out(struct FetchState *st)
{
    if (st->out != (BPTR)0)
        return TRUE;

    if (st->to != NULL)
    {
        st->out = Open((CONST_STRPTR)st->to, MODE_NEWFILE);
        if (st->out == (BPTR)0)
        {
            tool_fault(IoErr());
            st->failed = TRUE;
            return FALSE;
        }
    }
    else
    {
        st->out = Output();
    }

    return TRUE;
}

static BOOL fetch_sink_open(APTR ctx)
{
    return fetch_open_out((struct FetchState *)ctx);
}

static BOOL emit(APTR ctx, const UBYTE *data, LONG len)
{
    struct FetchState *st = (struct FetchState *)ctx;

    if (len <= 0)
        return TRUE;

    if (!fetch_open_out(st))
        return FALSE;

    if (Write(st->out, (APTR)data, len) != len)
    {
        tool_fault(IoErr());
        st->failed = TRUE;
        return FALSE;
    }

    return TRUE;
}

/*
 * What main() hands over to the transfer.  File-static because the transfer
 * runs on a different stack, see the StackSwap() note at the bottom.
 */
static struct FetchState fetch_init_state;
static FetchCore         fetch_core;

/* The status line, static for the reason the transfer's buffers are: the
   stack check counts fetch_run() on top of fetch_core_run(). */
static char              fetch_line[128];

static LONG fetch_run(VOID)
{
    struct FetchState     st = fetch_init_state;
    LONG                  rc;

    fetch_core.sink.ctx = &st;

    rc = fetch_core_run(&fetch_core);

    if (rc == RETURN_OK && !st.failed && fetch_core.status != 0)
    {
        fetch_head_first_line(&fetch_core.head, fetch_line,
                              sizeof(fetch_line));

        if (!fetch_core.quiet)
        {
            tool_printf("%s\n", (LONG)fetch_line);
            tool_printf("%lu bytes -> %s\n", fetch_core.total, (LONG)st.to);
        }

        /* A 4xx or 5xx is a successful transfer of a bad answer: the body is
           the server's explanation, so it is written, but the return code says
           the fetch did not get what was asked for. */
        if (fetch_core.status >= 400)
        {
            tool_error("%s", (LONG)fetch_line);
            rc = RETURN_WARN;
        }
    }

    if (st.failed)
        rc = RETURN_FAIL;

    if (st.out != (BPTR)0 && st.to != NULL)
        Close(st.out);

    return rc;
}


/*
 * Hazard: fetch_trampoline() must have no locals and no arguments, and must
 * stay noinline.  Between the two StackSwap() calls the stack pointer belongs
 * to the new stack, so a stack-based local of its own would read wrong memory.
 */
#define FETCH_STACK_SIZE    (64UL * 1024UL)

static struct StackSwapStruct fetch_sss;
static LONG                   fetch_result;

static __attribute__((noinline)) VOID fetch_trampoline(VOID)
{
    StackSwap(&fetch_sss);
    fetch_result = fetch_run();
    StackSwap(&fetch_sss);
}

int main(int argc, char **argv)
{
    LONG           args[ARG_COUNT];
    struct RDArgs *rda;
    APTR           stack;
    ULONG          timeout;

    (VOID)argv;

    if (tool_from_workbench(argc))
        return RETURN_FAIL;

    tool_break_arm();

    args[ARG_URL]      = 0;
    args[ARG_TO]       = 0;
    args[ARG_HEADERS]  = 0;
    args[ARG_QUIET]    = 0;
    args[ARG_NOVERIFY] = 0;
    args[ARG_TIMEOUT]  = 0;
    args[ARG_IPV4]     = 0;
    args[ARG_IPV6]     = 0;

    rda = ReadArgs((CONST_STRPTR)TEMPLATE, args, NULL);
    if (rda == NULL)
    {
        tool_fault(IoErr());
        tool_usage("[-4|-6] <url> [TO <file>]",
                   "An http: or https: URL.  Without TO the body goes to "
                   "standard output.");
        return RETURN_ERROR;
    }

    fetch_init_state.out      = (BPTR)0;
    fetch_init_state.to       = (const char *)args[ARG_TO];
    fetch_init_state.failed   = FALSE;

    fetch_core.headers    = (args[ARG_HEADERS]  != 0) ? TRUE : FALSE;
    fetch_core.quiet      = (args[ARG_QUIET]    != 0) ? TRUE : FALSE;
    fetch_core.noverify   = (args[ARG_NOVERIFY] != 0) ? TRUE : FALSE;
    fetch_core.sink.open  = (fetch_init_state.to != NULL) ? fetch_sink_open
                                                          : NULL;
    fetch_core.sink.write = emit;

    if (!tool_arg_family(args[ARG_IPV4], args[ARG_IPV6],
                         &fetch_core.family))
    {
        FreeArgs(rda);
        return RETURN_ERROR;
    }

    /*
     * Without TO the body goes to standard output, so a progress line would
     * corrupt it.  With TO that channel is free and the summary can print.
     */
    if (fetch_init_state.to == NULL)
        fetch_core.quiet = TRUE;

    timeout = FETCH_DEFAULT_TIMEOUT;

    if (args[ARG_TIMEOUT] != 0)
    {
        LONG seconds = *(LONG *)args[ARG_TIMEOUT];

        if (seconds < 0 || seconds > FETCH_TIMEOUT_MAX)
        {
            tool_error("TIMEOUT must be between 0 and %ld seconds",
                       (LONG)FETCH_TIMEOUT_MAX);
            FreeArgs(rda);
            return RETURN_ERROR;
        }

        timeout = (ULONG)seconds;
    }

    if (timeout == 0)
        timeout = FETCH_DEFAULT_TIMEOUT;
    fetch_core.timeout = timeout;

    if (!url_parse((const char *)args[ARG_URL], &fetch_core.url))
    {
        FreeArgs(rda);
        return RETURN_ERROR;
    }

    stack = AllocMem(FETCH_STACK_SIZE, MEMF_ANY);
    if (stack != NULL)
    {
        fetch_sss.stk_Lower   = stack;
        fetch_sss.stk_Upper   = (ULONG)stack + FETCH_STACK_SIZE;
        fetch_sss.stk_Pointer = (APTR)((ULONG)stack + FETCH_STACK_SIZE);

        fetch_trampoline();

        FreeMem(stack, FETCH_STACK_SIZE);
    }
    else
    {
        /* Out of memory for a stack.  http: fits on the caller's, so try
           rather than refuse.  https: would not have worked on a machine this
           short of memory anyway. */
        fetch_result = fetch_run();
    }

    FreeArgs(rda);
    return fetch_result;
}

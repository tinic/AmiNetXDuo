/*
 * The PC sampler over a REAL transfer: bsdsocket.library, the SANA-II reader
 * and whatever card the guest was booted with.
 *
 * tcpprof.c answers where the TCP/IP core spends its time, and it is honest
 * about the limit -- it creates both IP instances with _nx_ram_network_driver
 * and both phases run over that, so nothing it says applies to the driver
 * path.  The step budget (AMINETXDUO_RXPROBE, netstat -s) measures that path
 * but only as legs: drain, settle, demux, state.  A leg says how long, never
 * which function, and the largest leg on the receive path is the reader
 * running IP input inline.
 *
 * So this is the missing instrument: the same sampler, wrapped around a real
 * iperf receive through the shipping library.  It links src/tools' iperf core
 * rather than reimplementing a client, which keeps the thing being measured
 * identical to the thing the rate harness measures.
 *
 *   wireprof <peer> [port] [seconds]
 *
 * The far end is `iperf -c <guest>`, exactly as tests/tools/run-iperf.sh
 * arranges for tcp-rx.  Read the result with tools/prof-report.py, and build
 * with -DAMINETXDUO_LTO=OFF or the report blames crt0.o.
 *
 * SPDX-License-Identifier: MIT
 */

/* iperfcore.h FIRST, and no <exec/types.h> before it: it reaches tools.h and
   then tx_api.h, whose port typedefs VOID, and Exec typedefs it too.  Every
   command in src/tools includes it this way round for the same reason. */
#include "iperfcore.h"
#include "toolsock.h"

#include <proto/exec.h>
#include <proto/dos.h>

#include <stdlib.h>

#include "prof.h"

/* One second of samples at the rate tcpprof uses, times the longest run this
   is meant for.  The buffer is the only thing here that is large. */
#define W_RATE          1000UL
#define W_MAX_SAMPLES   40000UL
#define W_BUFLEN        8192UL
#define W_DEFAULT_SECS  8UL
#define W_DEFAULT_PORT  5001U

/* Every command in src/tools defines this; tool_util.c prints with it. */
const char *const tool_name = "wireprof";

int main(int argc, char **argv)
{
    struct Library *sb;
    IperfPlan       plan;
    IperfRun        run;
    IperfResult     res;
    const char     *why;
    UBYTE          *buf;
    LONG            rc = RETURN_FAIL;
    LONG            state;

    if (argc < 2)
    {
        prof_log("usage: wireprof <peer> [port] [seconds]");
        prof_log_flush();
        return (RETURN_ERROR);
    }

    sb = tool_socket_open();
    if (sb == NULL)
    {
        prof_log("FATAL: bsdsocket.library did not open");
        prof_log_flush();
        return (RETURN_FAIL);
    }

    iperf_plan_init(&plan);
    plan.dir      = IPERF_TCP_RX;
    plan.blocking = 1;            /* the shape an application actually uses */
    plan.port     = (UWORD)((argc > 2) ? atoi(argv[2]) : W_DEFAULT_PORT);
    plan.seconds  = (ULONG)((argc > 3) ? atoi(argv[3]) : W_DEFAULT_SECS);
    plan.kbytes   = 0;
    plan.buflen   = W_BUFLEN;

    if (!tool_sock_resolve_af(sb, argv[1], 0, &plan.peer))
    {
        prof_log("FATAL: cannot resolve %s", argv[1]);
        goto done;
    }

    why = iperf_plan_check(&plan);
    if (why != NULL)
    {
        prof_log("FATAL: %s", why);
        goto done;
    }

    buf = (UBYTE *)AllocVec(plan.buflen, MEMF_ANY);
    if (buf == NULL)
    {
        prof_log("FATAL: no %ld bytes for the payload buffer",
                 (long)plan.buflen);
        goto done;
    }

    if (iperf_begin(&run, sb, &plan, buf) != 0)
    {
        prof_log("FATAL: iperf_begin: %s",
                 (run.res.stage != NULL) ? run.res.stage : "?");
        iperf_end(&run, &res);
        FreeVec(buf);
        goto done;
    }

    /* Sampling starts AFTER the connection is up: the accept and the three-way
       handshake are not what this is for, and they would be attributed to the
       transfer phase. */
    if (!prof_start(W_MAX_SAMPLES, W_RATE))
    {
        prof_log("FATAL: prof_start: %s", prof_error());
        iperf_abort(&run);
        iperf_end(&run, &res);
        FreeVec(buf);
        goto done;
    }

    prof_mark("transfer");

    do
    {
        state = iperf_slice(&run);
    }
    while (state == IPERF_RUNNING);

    prof_mark("end");
    prof_stop();

    iperf_end(&run, &res);
    FreeVec(buf);

    prof_log("wireprof: %s %lu bytes in %lu ms, %lu bit/s, %lu samples",
             iperf_dir_name(res.dir), (unsigned long)res.bytes_lo,
             (unsigned long)res.ms, (unsigned long)res.bits,
             (unsigned long)prof_stored());

    if (state == IPERF_FAILED)
        prof_log("wireprof: the run FAILED at %s",
                 (res.stage != NULL) ? res.stage : "?");
    else if (!prof_write("profile-wire-real.bin"))
        prof_log("FATAL: prof_write: %s", prof_error());
    else
    {
        prof_log("wireprof: wrote profile-wire-real.bin");
        rc = RETURN_OK;
    }

    prof_free();

done:
    prof_log_flush();
    CloseLibrary(sb);
    return (rc);
}

/*
 * The throughput measurement itself, over bsdsocket.library, in slices.
 *
 * Two callers share this: the `iperf` command, which loops on it until it says
 * it has finished, and httpd, which is one select() loop with no threads under
 * it and calls it once per pass.  That is why nothing here blocks for longer
 * than IPERF_SLICE_MICROS: a handler inside httpd that blocked for the length
 * of a ten-second measurement would stop the server answering anyone at all,
 * and its own connection timeouts would then drop the clients that were
 * waiting.  Same shape as httpd's own CONN_WALK.
 *
 * bsdsocket.library and not NetX Duo directly, for the reason sntp.c gives
 * about the vendored SNTP add-on: a Shell command's linked copy of NetX Duo is
 * a different, empty one, and the vendored nx_iperf.c binds its sockets on an
 * NX_IP it expects to be handed.  Going through the library also means the
 * figure is the one an application gets, and that the same binary runs on
 * Roadshow and AmiTCP, so a user can measure us against them with one tool.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_IPERFCORE_H
#define AMINETXDUO_IPERFCORE_H

#include "toolsock.h"
#include "iperfwire.h"

#ifdef __cplusplus
extern "C" {
#endif

enum
{
    IPERF_TCP_TX = 0,               /* we send, the far end is `iperf -s`   */
    IPERF_TCP_RX,                   /* we receive, the far end is `iperf -c`*/
    IPERF_UDP_TX,
    IPERF_UDP_RX
};

/* What a slice answers. */
enum
{
    IPERF_RUNNING = 0,
    IPERF_DONE,
    IPERF_FAILED
};

/*
 * How long a slice can spend inside the library.  250 ms is httpd's own idle
 * select timeout, so a measurement adds no latency that a request did not
 * already face.  20 ms leaves the loop responsive without spinning.
 */
#define IPERF_SLICE_MICROS  20000UL

typedef struct IperfPlan
{
    UBYTE       dir;
    /*
     * TCP_RX only: receive with a BLOCKING socket instead of the nonblocking
     * recv + WaitSelect slices.  The standalone command sets it; a caller
     * that runs slices inside its own select loop (httpd) must not, which is
     * the whole reason the slices exist.  A blocking read is what an
     * ordinary application does, it is the shape the library can serve
     * without a wakeup round trip per chunk
     * (AMINETXDUO_RX_DIRECT_COMPLETE), and it is the shape worth measuring.
     */
    UBYTE       blocking;
    UWORD       port;
    ToolAddr    peer;               /* the client directions only           */
    ULONG       seconds;            /* 0 when kbytes decides instead        */
    ULONG       kbytes;             /* 0 when seconds decides               */
    ULONG       buflen;             /* payload per send                     */
    ULONG       rate_kbit;          /* UDP send pacing, 0 is unpaced        */
} IperfPlan;

typedef struct IperfResult
{
    UBYTE           dir;
    UBYTE           got_report;     /* UDP TX: the far end answered         */
    UWORD           pad;
    ULONG           bytes_hi;
    ULONG           bytes_lo;
    ULONG           packets;
    ULONG           ms;
    ULONG           bits;
    ULONG           lost;
    ULONG           outoforder;
    ToolAddr        peer;           /* who we ended up talking to           */
    UWORD           peer_port;
    UWORD           pad2;
    IperfWireReport report;         /* valid when got_report                */
    LONG            err;            /* 0, or a TOOL_E* from the library     */
    const char     *stage;          /* what was in progress when err was set*/
} IperfResult;

typedef struct IperfRun
{
    struct Library *sb;
    IperfPlan       plan;
    IperfResult     res;

    UBYTE           state;
    UBYTE           fin_tries;
    UBYTE           clock_on;       /* t_begin is a reading, not "unset"    */
    UBYTE           pad;

    /*
     * How many slices this run can take before the clock is asked whether it
     * is still moving.  A measurement whose clock stops advancing would never
     * reach its deadline and would send for ever.  The host-side iperf 2.2.1
     * used to pin this protocol down wedged on a failed clock_nanosleep() and
     * spun for seventeen hours.  A timing call that fails has to end the run
     * with a diagnosis rather than become a busy loop.
     *
     * The count is the trigger and t_guard is the evidence: how fast a
     * machine goes round the loop is not a measurement of anything, so the
     * budget running out asks the clock rather than answering for it.
     */
    ULONG           slices;
    ULONG           slice_max;
    ULONG           t_guard;        /* the clock when the budget last ran out */

    LONG            lsock;          /* listening socket, server directions  */
    LONG            sock;

    ULONG           t_begin;        /* ami_millis() when bytes started      */
    ULONG           t_deadline;     /* 0 when there is no time limit        */
    ULONG           t_lastact;
    ULONG           want_bytes_hi;
    ULONG           want_bytes_lo;

    long            seq;            /* UDP datagram id last sent            */
    long            expect;         /* UDP datagram id next expected        */

    ToolSockAddrAny from;
    UBYTE           have_from;
    UBYTE           pad3[3];
} IperfRun;

/*
 * A plan filled with the defaults, so a caller only sets what it means to
 * change.  dir and peer are left alone.
 */
VOID iperf_plan_init(IperfPlan *plan);

/*
 * Check a plan and say what is wrong with it, or NULL when nothing is.  The
 * text is a sentence fragment: "iperf: <text>" reads correctly.  Separate from
 * iperf_begin() so the command can refuse before it opens anything and httpd
 * can answer 400 without starting a measurement.
 */
const char *iperf_plan_check(const IperfPlan *plan);

/*
 * Open the sockets and start.  0 on success.  On failure the reason is in
 * run->res.err and run->res.stage and iperf_end() still has to be called.
 *
 * `buf` is the payload buffer, supplied by the caller so that neither this nor
 * httpd needs a static one.  It must be at least plan->buflen bytes and stay
 * alive until iperf_end().
 */
LONG iperf_begin(IperfRun *run, struct Library *sb, const IperfPlan *plan,
                 UBYTE *buf);

/* One slice.  IPERF_RUNNING, IPERF_DONE or IPERF_FAILED. */
LONG iperf_slice(IperfRun *run);

/*
 * Stop early, as a Ctrl-C does.  Whatever has been measured so far is kept, so
 * an interrupted run still reports a rate.
 */
VOID iperf_abort(IperfRun *run);

/* Close everything and finish the arithmetic.  Safe after a failed begin. */
VOID iperf_end(IperfRun *run, IperfResult *out);

/*
 * The result as one line a script can read, into `buf`:
 *
 *   dir=tcp-tx bytes=4194304 ms=10012 bits_per_sec=3351000 packets=512 \
 *   lost=0 peer=192.168.1.9 port=5001
 *
 * Fixed key order, every key always present, so a reader can index it.
 */
VOID iperf_result_line(const IperfResult *res, struct Library *sb,
                       char *buf, ULONG buflen);

/* "tcp-tx", for the line above and for a caller that wants to name it. */
const char *iperf_dir_name(UBYTE dir);

#ifdef __cplusplus
}
#endif

#endif /* AMINETXDUO_IPERFCORE_H */

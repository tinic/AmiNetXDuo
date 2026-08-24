/*
 * The throughput measurement, in slices.  See iperfcore.h for why it is shaped
 * this way and why it does not link NetX Duo.
 *
 * SPDX-License-Identifier: MIT
 */

#include "iperfcore.h"

/* The states a run passes through.  One machine covers all four directions,
   and which states are reachable depends on plan.dir. */
enum
{
    ST_IDLE = 0,
    ST_CONNECT,                     /* TCP TX: handshake in flight          */
    ST_ACCEPT,                      /* TCP RX: waiting for a caller         */
    ST_SEND,
    ST_RECV,
    ST_FIN,                         /* UDP TX: end marker, awaiting report  */
    ST_REPORT,                      /* UDP RX: answering the end marker     */
    ST_DONE,
    ST_FAILED
};

/*
 * How long a receiver waits with nothing arriving before it decides the far
 * end has stopped.  Two seconds: an iperf 2 client that loses its whole FIN
 * burst is gone, and a stream that has paused this long is not one anybody
 * wants a rate from.  Independent of the run's own -t, which is a ceiling on
 * the measurement and not on the peer.
 */
#define IPERF_IDLE_MS       2000UL

/* An unpaced sender still has to come back to the caller. */
#define IPERF_SEND_BURST    64

/*
 * How long a sender with nothing to do sleeps.  Short, because on a datagram
 * socket there is no event to wake it: it is a sleep and not a wait, and a
 * pause that clears in a millisecond must not be waited out for twenty.
 */
#define IPERF_PACE_MICROS   2000UL

/* iperf 2 repeats its end-of-test datagram this many times before giving up. */
#define IPERF_FIN_TRIES     10
#define IPERF_FIN_WAIT_MS   250UL

static UBYTE *iperf_buf;            /* the caller's, held for the run       */

/* ------------------------------------------------------------------ plan - */

VOID iperf_plan_init(IperfPlan *plan)
{
    plan->blocking  = 0;
    plan->port      = (UWORD)IPERF_PORT;
    plan->seconds   = 10;
    plan->kbytes    = 0;
    plan->buflen    = 0;            /* filled in by iperf_plan_check()      */
    plan->rate_kbit = 0;
}

const char *iperf_plan_check(const IperfPlan *plan)
{
    const char *why;

    if (plan->dir > IPERF_UDP_RX)
        return "no such direction";

    /* Every bound a run has is decided in one place, and it is host-tested.
       See the comment on iperf_limits_check() for why an unbounded run is
       refused rather than merely discouraged. */
    why = iperf_limits_check(plan->seconds, plan->kbytes, plan->buflen,
                             (ULONG)plan->port);
    if (why != NULL)
        return why;

    /* A UDP datagram has to hold the header the far end parses. */
    if ((plan->dir == IPERF_UDP_TX) && plan->buflen < (ULONG)IPERF_DG_TOTAL)
        return "a UDP datagram smaller than 36 bytes cannot carry the header";

    return NULL;
}

const char *iperf_dir_name(UBYTE dir)
{
    static const char *const names[4] =
        { "tcp-tx", "tcp-rx", "udp-tx", "udp-rx" };

    return (dir <= IPERF_UDP_RX) ? names[dir] : "?";
}

/* ------------------------------------------------------------- bookkeeping - */

static VOID iperf_fail(IperfRun *run, const char *stage, LONG err)
{
    run->res.err   = err;
    run->res.stage = stage;
    run->state     = ST_FAILED;
}

static BOOL iperf_udp(const IperfRun *run)
{
    return (run->plan.dir == IPERF_UDP_TX || run->plan.dir == IPERF_UDP_RX)
               ? TRUE : FALSE;
}

/*
 * The clock starts when the first byte moves, not when the socket opened: a
 * connect() on a slow link would otherwise be charged to the transfer.
 *
 * clock_on rather than `t_begin == 0`, because 0 is a reading the clock
 * really returns -- ami_millis() accumulates from the previous call and so
 * answers 0 the first time it is asked.  With 0 meaning "not started" the
 * first slice ran its whole burst before any target check could pass, and a
 * `-n 64` run moved 256 KB instead of 64.  The same conflation is what would
 * turn a timer that never starts into a transfer that never ends.
 */
static VOID iperf_clock_start(IperfRun *run)
{
    if (!run->clock_on)
    {
        run->clock_on  = 1;
        run->t_begin   = ami_millis();
        run->t_lastact = run->t_begin;

        if (run->plan.seconds != 0)
            run->t_deadline = run->t_begin + run->plan.seconds * 1000UL;
    }
}

/* TRUE once the run has moved everything it was asked to move. */
static BOOL iperf_target_met(const IperfRun *run, ULONG now)
{
    if (!run->clock_on)
        return FALSE;

    if (run->t_deadline != 0 && (LONG)(now - run->t_deadline) >= 0)
        return TRUE;

    if (run->plan.kbytes != 0)
    {
        if (run->res.bytes_hi > run->want_bytes_hi)
            return TRUE;
        if (run->res.bytes_hi == run->want_bytes_hi
            && run->res.bytes_lo >= run->want_bytes_lo)
            return TRUE;
    }

    return FALSE;
}

static VOID iperf_count(IperfRun *run, LONG n)
{
    iperf_add64(&run->res.bytes_hi, &run->res.bytes_lo, (ULONG)n);
    run->res.packets++;
    run->t_lastact = ami_millis();
}

/* ------------------------------------------------------------------ setup - */

static LONG iperf_open_client(IperfRun *run, LONG type)
{
    ToolSockAddrAny sa;
    LONG            nonblock = 1;

    run->sock = tool_sock_socket(run->sb,
                                 TOOL_ADDR_IS6(&run->plan.peer) ? TOOL_AF_INET6
                                                                : TOOL_AF_INET,
                                 type, 0);
    if (run->sock < 0)
    {
        iperf_fail(run, "socket", tool_sock_errno(run->sb));
        return -1;
    }

    (VOID)tool_sock_addr(&sa, &run->plan.peer, run->plan.port);

    /* Non-blocking from the start.  A slice that blocked would hold up
       httpd's whole loop, and the command wants Ctrl-C noticed. */
    (VOID)tool_sock_ioctl(run->sb, run->sock, TOOL_FIONBIO, &nonblock);

    if (type == TOOL_SOCK_DGRAM)
    {
        /* connect() on a datagram socket only remembers the destination,
           which is all this needs and lets send() be used throughout. */
        if (tool_sock_connect(run->sb, run->sock, &sa) != 0)
        {
            LONG e = tool_sock_errno(run->sb);

            if (e != TOOL_EINPROGRESS && e != TOOL_EWOULDBLOCK)
            {
                iperf_fail(run, "connect", e);
                return -1;
            }
        }
        run->state = ST_SEND;
        return 0;
    }

    if (tool_sock_connect(run->sb, run->sock, &sa) == 0)
    {
        run->state = ST_SEND;
        return 0;
    }

    {
        LONG e = tool_sock_errno(run->sb);

        if (e != TOOL_EINPROGRESS && e != TOOL_EWOULDBLOCK)
        {
            iperf_fail(run, "connect", e);
            return -1;
        }
    }

    run->state = ST_CONNECT;
    return 0;
}

static LONG iperf_open_server(IperfRun *run, LONG type)
{
    ToolSockAddrAny sa;
    LONG            on = 1;
    LONG            nonblock = 1;
    LONG            s;

    s = tool_sock_socket(run->sb, (LONG)run->plan.peer.ta_Family, type, 0);
    if (s < 0)
    {
        iperf_fail(run, "socket", tool_sock_errno(run->sb));
        return -1;
    }

    (VOID)tool_sock_setsockopt(run->sb, s, TOOL_SOL_SOCKET, TOOL_SO_REUSEADDR,
                               &on, (LONG)sizeof(on));

    (VOID)tool_sock_addr(&sa, &run->plan.peer, run->plan.port);

    if (tool_sock_bind(run->sb, s, &sa) != 0)
    {
        iperf_fail(run, "bind", tool_sock_errno(run->sb));
        (VOID)tool_sock_close(run->sb, s);
        return -1;
    }

    (VOID)tool_sock_ioctl(run->sb, s, TOOL_FIONBIO, &nonblock);

    if (type == TOOL_SOCK_DGRAM)
    {
        run->sock  = s;
        run->state = ST_RECV;
        return 0;
    }

    if (tool_sock_listen(run->sb, s, 4) != 0)
    {
        iperf_fail(run, "listen", tool_sock_errno(run->sb));
        (VOID)tool_sock_close(run->sb, s);
        return -1;
    }

    run->lsock = s;
    run->state = ST_ACCEPT;
    return 0;
}

LONG iperf_begin(IperfRun *run, struct Library *sb, const IperfPlan *plan,
                 UBYTE *buf)
{
    ULONG i;
    UBYTE *p = (UBYTE *)run;

    for (i = 0; i < sizeof(*run); i++)
        p[i] = 0;

    run->sb    = sb;
    run->plan  = *plan;
    run->lsock = -1;
    run->sock  = -1;
    run->state = ST_IDLE;
    run->expect = 1;
    run->res.dir = plan->dir;

    iperf_buf = buf;

    run->slice_max = iperf_slice_budget(plan->seconds, plan->kbytes,
                                        plan->buflen);

    /*
     * Force timer.device open before anything is timed, as ping.c does, and
     * then check the clock actually moves.  ami_millis() answers 0 when it
     * cannot open the timer, and a measurement without a clock is not one:
     * better to say so here than to run to a deadline that never arrives.
     */
    (VOID)ami_millis();
    {
        ULONG a = ami_millis();
        ULONG b;

        (VOID)tool_delay_ticks(2);
        b = ami_millis();

        if (a == b)
        {
            iperf_fail(run, "the system timer is not running", 0);
            return -1;
        }

        run->t_guard = b;
    }

    if (plan->kbytes != 0)
    {
        /* kbytes * 1024 in two halves, because a 4 GB run overflows one. */
        run->want_bytes_hi = plan->kbytes >> 22;
        run->want_bytes_lo = plan->kbytes << 10;
    }

    /* The payload, filled once.  A TCP sender rewrites the pattern's phase per
       send so a receiver reading the stream sees it continuous.  A UDP sender
       overwrites the first 36 bytes per datagram. */
    iperf_pattern_fill(buf, plan->buflen, 0);

    switch (plan->dir)
    {
    case IPERF_TCP_TX:
        return iperf_open_client(run, TOOL_SOCK_STREAM);
    case IPERF_UDP_TX:
        return iperf_open_client(run, TOOL_SOCK_DGRAM);
    case IPERF_TCP_RX:
        return iperf_open_server(run, TOOL_SOCK_STREAM);
    case IPERF_UDP_RX:
        return iperf_open_server(run, TOOL_SOCK_DGRAM);
    default:
        break;
    }

    iperf_fail(run, "direction", 0);
    return -1;
}

/* ------------------------------------------------------------------ slices - */

/*
 * Wait up to one slice for the socket to become ready.  Everything here is
 * non-blocking, so this is where a run that has nothing to do spends its time
 * rather than spinning.
 */
static LONG iperf_wait(IperfRun *run, LONG sock, BOOL forwrite, ULONG micros)
{
    ToolFdSet   set;
    ToolTimeval tv;

    tool_fd_zero(&set);
    tool_fd_add(&set, sock);

    tv.tv_secs  = (LONG)(micros / 1000000UL);
    tv.tv_micro = (LONG)(micros % 1000000UL);

    return tool_sock_select(run->sb, sock + 1,
                            forwrite ? NULL : &set,
                            forwrite ? &set : NULL, &tv);
}

/*
 * The same wait for a sender, on the side that can actually block it.
 *
 * select() answers "writable" for a datagram socket at once, and goes on
 * answering it while the driver queue is full -- a UDP send fails with
 * EWOULDBLOCK long before anything makes the socket unwritable.  Waiting for
 * writability there does not wait, it spins, and the run then goes round the
 * loop as fast as the machine can: the same three-second `iperf -u -b 2000`
 * ran 30,000 slices in 2.5 s on an A3000 and stayed well inside them on an
 * A1200.  Waiting on the read side does sleep, and still wakes early if the
 * far end says anything.  A stream socket is the other way round: writable
 * means the buffer drained, which is exactly the wake-up wanted.
 */
static LONG iperf_wait_send(IperfRun *run, ULONG micros)
{
    if (!iperf_udp(run))
        return iperf_wait(run, run->sock, TRUE, micros);

    if (micros > IPERF_PACE_MICROS)
        micros = IPERF_PACE_MICROS;

    return iperf_wait(run, run->sock, FALSE, micros);
}

static VOID iperf_slice_connect(IperfRun *run)
{
    LONG err    = 0;
    LONG errlen = (LONG)sizeof(err);
    LONG ready;

    ready = iperf_wait(run, run->sock, TRUE, IPERF_SLICE_MICROS);

    if (ready < 0)
    {
        iperf_fail(run, "connect", tool_sock_errno(run->sb));
        return;
    }

    if (ready == 0)
    {
        /* Nothing yet.  A connect that never completes is caught by the
           run's own deadline, not by a timeout of its own: an unreachable
           address takes the stack's whole retransmit schedule and the user
           asked for a run of a stated length. */
        if (run->t_deadline == 0)
            run->t_deadline = ami_millis() + run->plan.seconds * 1000UL
                            + IPERF_IDLE_MS;

        if ((LONG)(ami_millis() - run->t_deadline) >= 0)
            iperf_fail(run, "connect", TOOL_ETIMEDOUT);

        return;
    }

    if (tool_sock_getsockopt(run->sb, run->sock, TOOL_SOL_SOCKET,
                             TOOL_SO_ERROR, &err, &errlen) != 0)
        err = 0;

    if (err != 0)
    {
        iperf_fail(run, "connect", err);
        return;
    }

    /* The deadline set above was the connect's, not the transfer's. */
    run->t_deadline = 0;
    run->state      = ST_SEND;
}

static VOID iperf_slice_accept(IperfRun *run)
{
    ToolSockAddrAny from;
    LONG            nonblock = 1;
    LONG            ready;
    LONG            s;

    ready = iperf_wait(run, run->lsock, FALSE, IPERF_SLICE_MICROS);

    if (ready < 0)
    {
        iperf_fail(run, "accept", tool_sock_errno(run->sb));
        return;
    }

    if (ready == 0)
        return;

    s = tool_sock_accept(run->sb, run->lsock, &from);
    if (s < 0)
    {
        LONG e = tool_sock_errno(run->sb);

        if (e == TOOL_EWOULDBLOCK || e == TOOL_EINTR)
            return;

        iperf_fail(run, "accept", e);
        return;
    }

    /* A blocking plan leaves the accepted socket blocking: the data-phase
       recv() then parks in the library instead of polling it in slices. */
    if (!run->plan.blocking)
        (VOID)tool_sock_ioctl(run->sb, s, TOOL_FIONBIO, &nonblock);
    (VOID)tool_sock_addr_get(&from, &run->res.peer);
    run->res.peer_port = tool_sock_addr_port(&from);

    run->sock  = s;
    run->state = ST_RECV;
}

/*
 * How many bytes the run is allowed to have sent by now, when a rate was
 * asked for.  Pacing on the clock rather than on a per-datagram delay: a
 * Delay() of less than a tick is not one, and an Amiga's tick is 20 ms.
 */
static BOOL iperf_paced_out(IperfRun *run, ULONG now)
{
    unsigned long long allowed;
    unsigned long long sent;
    ULONG              elapsed;

    if (run->plan.rate_kbit == 0)
        return FALSE;

    elapsed = now - run->t_begin;
    allowed = (unsigned long long)run->plan.rate_kbit * (unsigned long long)elapsed
            / 8ULL;
    sent    = ((unsigned long long)run->res.bytes_hi << 32)
            | (unsigned long long)run->res.bytes_lo;

    return (sent >= allowed) ? TRUE : FALSE;
}

static VOID iperf_slice_send(IperfRun *run)
{
    ULONG now;
    LONG  burst;

    iperf_clock_start(run);
    now = ami_millis();

    if (iperf_target_met(run, now))
    {
        if (run->plan.dir == IPERF_UDP_TX)
        {
            run->state     = ST_FIN;
            run->fin_tries = 0;
            run->t_lastact = now;
        }
        else
        {
            run->res.ms = now - run->t_begin;
            run->state  = ST_DONE;
        }
        return;
    }

    if (iperf_paced_out(run, now))
    {
        /* Ahead of the asked-for rate.  Give the slice back rather than
           spinning on the clock. */
        (VOID)iperf_wait_send(run, IPERF_PACE_MICROS);
        return;
    }

    for (burst = 0; burst < IPERF_SEND_BURST; burst++)
    {
        LONG n;
        LONG len = (LONG)run->plan.buflen;

        if (run->plan.dir == IPERF_UDP_TX)
        {
            run->seq++;
            iperf_dg_put(iperf_buf, run->seq, now / 1000UL,
                         (now % 1000UL) * 1000UL);
        }

        n = tool_sock_send(run->sb, run->sock, iperf_buf, len);

        if (n > 0)
        {
            iperf_count(run, n);
        }
        else if (n < 0)
        {
            LONG e = tool_sock_errno(run->sb);

            if (e == TOOL_EWOULDBLOCK || e == TOOL_EINTR)
            {
                if (run->plan.dir == IPERF_UDP_TX)
                    run->seq--;     /* not sent, so not an id anyone saw    */

                (VOID)iperf_wait_send(run, IPERF_SLICE_MICROS);
                return;
            }

            iperf_fail(run, "send", e);
            return;
        }

        now = ami_millis();

        if (iperf_target_met(run, now) || iperf_paced_out(run, now))
            return;
    }
}

static VOID iperf_slice_recv(IperfRun *run)
{
    ULONG now = ami_millis();
    LONG  burst;
    /* Blocking reads park until data arrives, so a 64-read burst could hold
       a slice for seconds at Amiga rates.  Eight keeps the once-a-second
       sampler and the break check between slices responsive. */
    LONG  burst_max = run->plan.blocking ? 8 : IPERF_SEND_BURST;

    if (run->clock_on && iperf_target_met(run, now))
    {
        run->res.ms = now - run->t_begin;
        run->state  = ST_DONE;
        return;
    }

    for (burst = 0; burst < burst_max; burst++)
    {
        ToolSockAddrAny from;
        LONG            n;

        if (iperf_udp(run))
            n = tool_sock_recvfrom(run->sb, run->sock, iperf_buf,
                                   (LONG)run->plan.buflen, &from);
        else
            n = tool_sock_recv(run->sb, run->sock, iperf_buf,
                               (LONG)run->plan.buflen);

        if (n > 0)
        {
            if (iperf_udp(run))
            {
                long id;

                /* A short datagram has no iperf identity.  In particular it
                   must not claim an idle server, start its clock, or become
                   part of the measurement. */
                if (iperf_dg_get(iperf_buf, (ULONG)n, &id) != 0)
                    continue;

                if (run->have_from
                    && !tool_sock_addr_same(&from, &run->from))
                {
                    /* One UDP run belongs to the endpoint whose first
                       datagram opened it.  Counting another sender here
                       corrupts that peer's figures, and a forged negative
                       id would end its run and make us report the result to
                       the original endpoint. */
                    continue;
                }

                if (!run->have_from)
                {
                    run->from      = from;
                    run->have_from = 1;
                    (VOID)tool_sock_addr_get(&from, &run->res.peer);
                    run->res.peer_port = tool_sock_addr_port(&from);
                }

                iperf_clock_start(run);
                iperf_count(run, n);

                if (id < 0)
                {
                    /* The end marker.  Its own bytes are counted, which is
                       what iperf 2's server does and what makes the two
                       totals comparable. */
                    run->res.ms    = ami_millis() - run->t_begin;
                    run->state     = ST_REPORT;
                    run->fin_tries = 0;
                    return;
                }

                if (id == run->expect)
                {
                    run->expect++;
                }
                else if (id > run->expect)
                {
                    run->res.lost += (ULONG)(id - run->expect);
                    run->expect    = id + 1;
                }
                else
                {
                    run->res.outoforder++;
                }
            }
            else
            {
                iperf_clock_start(run);
                iperf_count(run, n);
            }

            now = ami_millis();

            if (iperf_target_met(run, now))
            {
                run->res.ms = now - run->t_begin;
                run->state  = ST_DONE;
                return;
            }

            continue;
        }

        if (n == 0 && !iperf_udp(run))
        {
            /* The sender closed.  That is the end of a TCP test, and the
               normal way `iperf -c` finishes. */
            run->res.ms = run->clock_on
                              ? (ami_millis() - run->t_begin) : 0;
            run->state  = ST_DONE;
            return;
        }

        {
            LONG e = tool_sock_errno(run->sb);

            if (n < 0 && e != TOOL_EWOULDBLOCK && e != TOOL_EINTR)
            {
                if (e == TOOL_ECONNRESET && run->clock_on)
                {
                    /* A peer that resets after sending is still a completed
                       measurement.  A reset before any byte is a failure. */
                    run->res.ms = ami_millis() - run->t_begin;
                    run->state  = ST_DONE;
                    return;
                }

                iperf_fail(run, "receive", e);
                return;
            }
        }

        /* Nothing there.  Wait, then decide whether the far end has gone. */
        (VOID)iperf_wait(run, run->sock, FALSE, IPERF_SLICE_MICROS);

        now = ami_millis();

        if (run->clock_on && (now - run->t_lastact) >= IPERF_IDLE_MS)
        {
            run->res.ms = run->t_lastact - run->t_begin;
            run->state  = ST_DONE;
        }

        return;
    }
}

/* UDP TX: send the end marker and wait for the far end's report. */
static VOID iperf_slice_fin(IperfRun *run)
{
    ULONG now = ami_millis();
    LONG  n;

    if (run->res.ms == 0)
        run->res.ms = now - run->t_begin;

    if (run->fin_tries >= IPERF_FIN_TRIES)
    {
        /* No answer.  Our own send-side figure still stands, and
           got_report says the far end never reported back. */
        run->state = ST_DONE;
        return;
    }

    if (run->fin_tries == 0 || (now - run->t_lastact) >= IPERF_FIN_WAIT_MS)
    {
        LONG sent;

        iperf_dg_put(iperf_buf, -run->seq, now / 1000UL,
                     (now % 1000UL) * 1000UL);
        sent = tool_sock_send(run->sb, run->sock, iperf_buf,
                              (LONG)run->plan.buflen);

        /*
         * The end marker is one of the test's datagrams, and iperf 2 counts
         * it on both sides: its receiver adds it to the total it reports
         * back, so a sender that left it out disagrees with every peer by
         * exactly one datagram.  Only the first attempt counts -- a retry is
         * recovery traffic, and the far end still only ever sees one marker,
         * so the two totals agree whether or not one was lost.
         */
        if (sent > 0 && run->fin_tries == 0)
        {
            iperf_add64(&run->res.bytes_hi, &run->res.bytes_lo, (ULONG)sent);
            run->res.packets++;
        }

        run->fin_tries++;
        run->t_lastact = now;
    }

    if (iperf_wait(run, run->sock, FALSE, IPERF_SLICE_MICROS) <= 0)
        return;

    n = tool_sock_recv(run->sb, run->sock, iperf_buf, (LONG)run->plan.buflen);

    if (n > 0 && iperf_report_get(iperf_buf, (ULONG)n, &run->res.report) == 0)
    {
        run->res.got_report = 1;
        run->res.lost       = run->res.report.lost;
        run->res.outoforder = run->res.report.outoforder;
        run->state          = ST_DONE;
    }
}

/* UDP RX: answer the end marker, the way iperf 2's server does. */
static VOID iperf_slice_report(IperfRun *run)
{
    IperfWireReport rep;
    ULONG           now = ami_millis();

    if (run->fin_tries >= IPERF_FIN_TRIES)
    {
        run->state = ST_DONE;
        return;
    }

    rep.flags       = IPERF_HEADER_VERSION1;
    rep.bytes_hi    = run->res.bytes_hi;
    rep.bytes_lo    = run->res.bytes_lo;
    rep.stop_sec    = run->res.ms / 1000UL;
    rep.stop_usec   = (run->res.ms % 1000UL) * 1000UL;
    rep.lost        = run->res.lost;
    rep.outoforder  = run->res.outoforder;
    /* The end marker is not one of the datagrams the test moved. */
    rep.datagrams   = (run->res.packets > 0) ? (run->res.packets - 1) : 0;
    rep.jitter_sec  = 0;
    rep.jitter_usec = 0;

    iperf_report_put(iperf_buf, &rep);

    (VOID)tool_sock_sendto(run->sb, run->sock, iperf_buf,
                           (LONG)IPERF_REPORT_LEN, &run->from);
    run->fin_tries++;
    run->t_lastact = now;

    /* Repeated because it is a datagram and the client is listening for
       exactly one.  iperf 2 does the same.  A slice between each so the
       burst does not hold the caller. */
    (VOID)iperf_wait(run, run->sock, FALSE, 20000UL);
}

LONG iperf_slice(IperfRun *run)
{
    /*
     * The bound for a clock that has stopped.  Everything else here decides it
     * has finished by comparing two readings, so a clock that stops advancing
     * means a run that never ends: it would send until something killed it,
     * and a caller with its output redirected would have an unbounded writer.
     *
     * Slices are the trigger and the clock is the authority.  Counting alone
     * made the bound a property of the machine: the same three-second UDP send
     * spent its 30,000 slices in 2.5 s on an A3000 and finished inside them on
     * an A1200, so the A3000 was told its clock had stopped by the same clock
     * that was printing a line a second.  A budget that runs out now reads the
     * clock, and only a reading that has not moved since the last one ends the
     * run -- which a stopped clock reaches one budget later and a running one
     * never does.
     */
    if (run->state != ST_DONE && run->state != ST_FAILED
        && ++run->slices > run->slice_max)
    {
        ULONG now = ami_millis();

        run->slices = 0;

        if (now == run->t_guard)
        {
            iperf_fail(run, "the clock stopped advancing", 0);
            return IPERF_FAILED;
        }

        run->t_guard = now;
    }

    switch (run->state)
    {
    case ST_CONNECT: iperf_slice_connect(run); break;
    case ST_ACCEPT:  iperf_slice_accept(run);  break;
    case ST_SEND:    iperf_slice_send(run);    break;
    case ST_RECV:    iperf_slice_recv(run);    break;
    case ST_FIN:     iperf_slice_fin(run);     break;
    case ST_REPORT:  iperf_slice_report(run);  break;
    default:                                   break;
    }

    if (run->state == ST_DONE)
        return IPERF_DONE;
    if (run->state == ST_FAILED)
        return IPERF_FAILED;

    return IPERF_RUNNING;
}

VOID iperf_abort(IperfRun *run)
{
    if (run->state == ST_DONE || run->state == ST_FAILED)
        return;

    if (run->clock_on && run->res.ms == 0)
        run->res.ms = ami_millis() - run->t_begin;

    run->state = ST_DONE;
}

VOID iperf_end(IperfRun *run, IperfResult *out)
{
    if (run->plan.dir == IPERF_TCP_TX && run->sock >= 0)
    {
        /* Half-close so the far end sees the end of the stream and reports.
           Without it `iperf -s` waits out its own idle timeout. */
        (VOID)tool_sock_shutdown(run->sb, run->sock, TOOL_SHUT_WR);
    }

    if (run->sock >= 0)
    {
        (VOID)tool_sock_close(run->sb, run->sock);
        run->sock = -1;
    }

    if (run->lsock >= 0)
    {
        (VOID)tool_sock_close(run->sb, run->lsock);
        run->lsock = -1;
    }

    if (run->res.ms == 0 && run->clock_on)
        run->res.ms = ami_millis() - run->t_begin;

    run->res.bits = iperf_bits_per_sec(run->res.bytes_hi, run->res.bytes_lo,
                                       run->res.ms);

    if (out != NULL)
        *out = run->res;

    iperf_buf = NULL;
}

/* ------------------------------------------------------------------ output - */

static ULONG line_put(char *buf, ULONG buflen, ULONG pos, const char *text)
{
    while (*text != '\0' && pos + 1 < buflen)
        buf[pos++] = *text++;

    buf[pos] = '\0';
    return pos;
}

static ULONG line_num(char *buf, ULONG buflen, ULONG pos, ULONG v)
{
    char  rev[12];
    ULONG n = 0;

    if (v == 0)
        rev[n++] = '0';

    while (v > 0 && n < sizeof(rev))
    {
        rev[n++] = (char)('0' + (v % 10UL));
        v /= 10UL;
    }

    while (n > 0 && pos + 1 < buflen)
        buf[pos++] = rev[--n];

    buf[pos] = '\0';
    return pos;
}

VOID iperf_result_line(const IperfResult *res, struct Library *sb,
                       char *buf, ULONG buflen)
{
    char  addr[TOOL_ADDR_STRLEN];
    ULONG p = 0;

    if (buflen == 0)
        return;

    buf[0] = '\0';

    tool_addr_text(sb, &res->peer, addr, sizeof(addr));

    p = line_put(buf, buflen, p, "dir=");
    p = line_put(buf, buflen, p, iperf_dir_name(res->dir));
    p = line_put(buf, buflen, p, " bytes=");
    /* The low half alone up to 4 GB.  Above that the high half is printed as a
       multiplier rather than a wrong number. */
    if (res->bytes_hi != 0)
    {
        p = line_num(buf, buflen, p, res->bytes_hi);
        p = line_put(buf, buflen, p, "*4294967296+");
    }
    p = line_num(buf, buflen, p, res->bytes_lo);
    p = line_put(buf, buflen, p, " ms=");
    p = line_num(buf, buflen, p, res->ms);
    p = line_put(buf, buflen, p, " bits_per_sec=");
    p = line_num(buf, buflen, p, res->bits);
    p = line_put(buf, buflen, p, " packets=");
    p = line_num(buf, buflen, p, res->packets);
    p = line_put(buf, buflen, p, " lost=");
    p = line_num(buf, buflen, p, res->lost);
    p = line_put(buf, buflen, p, " outoforder=");
    p = line_num(buf, buflen, p, res->outoforder);
    p = line_put(buf, buflen, p, " peerreport=");
    p = line_num(buf, buflen, p, (ULONG)res->got_report);
    p = line_put(buf, buflen, p, " peer=");
    p = line_put(buf, buflen, p, addr);
    p = line_put(buf, buflen, p, " port=");
    (VOID)line_num(buf, buflen, p, (ULONG)res->peer_port);
}

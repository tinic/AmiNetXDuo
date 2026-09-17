/*
 * AmiNetXDuo, the packet pool and the window it has to back.
 *
 * v0.25.5 raised the pool on a machine with no Fast RAM, because a sixteenth
 * of 1.2 MB free buys 47 packets, the eighth-share TCP budget off 47 packets
 * is 7,840 bytes, and the 8,192-byte floor window then overrode it: the
 * socket advertised a window the pool it came from could not hold, and the
 * transfer was spent at a zero window.  The two acknowledgement holes fixed
 * beside it are gated by tcp_persist, and the silly-window rule by tcp_sws.
 * The arithmetic that reopened the window was measured and nothing guarded
 * it, so a later edit to either half could put the two back out of step
 * without a single test noticing.
 *
 * Gated on the mechanism and not on a throughput number: the property is that
 * a window is never advertised past the budget that stores it, and it holds
 * on every machine size, every card and every emulator.
 *
 * SPDX-License-Identifier: MIT
 */

#include "aminetxduo/pool.h"
#include "bsdsocket_window.h"

#include <stdio.h>


/*
 * A1200, 2 MB of chip and no Fast RAM: about 1.2 MB free when the stack comes
 * up, and 1,672 bytes per packet once sizeof(NX_PACKET) and the alignment are
 * added to AMI_POOL_PAYLOAD on m68k.  These two reproduce the measured 47 and
 * 94 exactly, which is what makes this case the regression and not an
 * illustration.
 */
#define A1200_AVAIL     1258291UL
#define M68K_STRIDE     1672UL

static unsigned long h_checks;
static unsigned long h_failures;


static void h_check(int ok, const char *what)
{
    h_checks++;
    if (!ok)
    {
        h_failures++;
        printf("FAIL %s\n", what);
    }
}


static void h_checkf(int ok, const char *what,
                     unsigned long a, unsigned long b, unsigned long c)
{
    h_checks++;
    if (!ok)
    {
        h_failures++;
        printf("FAIL %s avail=%lu stride=%lu divisor=%lu\n", what, a, b, c);
    }
}


static ULONG h_clamp(ULONG packets)
{
    if (packets < (ULONG)AMI_POOL_MIN_PACKETS)
        packets = (ULONG)AMI_POOL_MIN_PACKETS;
    if (packets > (ULONG)AMI_POOL_MAX_PACKETS)
        packets = (ULONG)AMI_POOL_MAX_PACKETS;
    return packets;
}


/* Every machine size worth asking about, from a 512 KB A500 with almost
   nothing free up to 32 MB of Fast RAM. */
static const ULONG h_avail[] = {
    131072UL, 262144UL, 393216UL, 524288UL, 786432UL, 1048576UL,
    A1200_AVAIL, 1572864UL, 2097152UL, 3145728UL, 4194304UL, 6291456UL,
    8388608UL, 12582912UL, 16777216UL, 33554432UL
};

/* sizeof(NX_PACKET) differs between m68k and the host this runs on, and the
   IPv6 build adds to it, so nothing here may depend on one stride. */
static const ULONG h_stride[] = {
    1600UL, 1632UL, M68K_STRIDE, 1712UL, 1792UL, 1888UL
};

/* AMI_POOL_MEM_DIVISOR and the two ENV:ANXDPOOLDIV values the fix was
   confirmed against before it was written. */
static const ULONG h_divisor[] = { 16UL, 8UL, 4UL };

#define H_AVAIL_N   ((int)(sizeof(h_avail)   / sizeof(h_avail[0])))
#define H_STRIDE_N  ((int)(sizeof(h_stride)  / sizeof(h_stride[0])))
#define H_DIVISOR_N ((int)(sizeof(h_divisor) / sizeof(h_divisor[0])))


/*
 * THE DEFECT.  A pool an eighth of free memory could have backed the floor
 * window with must back it.  Pre-fix this is false at A1200_AVAIL: the eighth
 * buys 94 packets and a budget of 17,248, the sixteenth buys 47 and a budget
 * of 7,840, and 7,840 is below the 8,192 the socket went on to advertise.
 */
static void a_a_backable_floor_window_is_backed(void)
{
    int i, j, k;

    for (i = 0; i < H_AVAIL_N; i++)
        for (j = 0; j < H_STRIDE_N; j++)
            for (k = 0; k < H_DIVISOR_N; k++)
            {
                ULONG avail  = h_avail[i];
                ULONG stride = h_stride[j];
                ULONG afford = h_clamp((avail / (ULONG)AMI_POOL_MEM_DIVISOR_LOW) /
                                       stride);
                ULONG pool;

                if (ami_bsd_tcp_budget(afford, (ULONG)AMI_POOL_PAYLOAD) <
                    (ULONG)BSD_TCP_WINDOW)
                    continue;           /* the machine cannot afford it */

                pool = ami_ns_pool_packets_for(avail, h_divisor[k], stride);

                h_checkf(ami_bsd_tcp_budget(pool, (ULONG)AMI_POOL_PAYLOAD) >=
                         (ULONG)BSD_TCP_WINDOW,
                         "the pool cannot hold the window it advertises",
                         avail, stride, h_divisor[k]);
            }
}


/*
 * And the other side of it: the fix may not buy the window by spending memory
 * the machine has not got.  Never more than the larger of the configured
 * share and an eighth.
 */
static void b_never_more_than_an_eighth(void)
{
    int i, j, k;

    for (i = 0; i < H_AVAIL_N; i++)
        for (j = 0; j < H_STRIDE_N; j++)
            for (k = 0; k < H_DIVISOR_N; k++)
            {
                ULONG avail  = h_avail[i];
                ULONG stride = h_stride[j];
                ULONG plain  = (avail / h_divisor[k]) / stride;
                ULONG eighth = (avail / (ULONG)AMI_POOL_MEM_DIVISOR_LOW) / stride;
                ULONG most   = h_clamp((plain > eighth) ? plain : eighth);

                h_checkf(ami_ns_pool_packets_for(avail, h_divisor[k], stride) <= most,
                         "the pool took more than an eighth of free memory",
                         avail, stride, h_divisor[k]);
            }
}


/*
 * A MACHINE WITH FAST RAM IS UNTOUCHED.  Above the working floor the branch
 * is not taken and the arithmetic is bit for bit the plain share, which is
 * why this is not simply AMI_POOL_MEM_DIVISOR 8.
 */
static void c_a_machine_above_the_floor_is_untouched(void)
{
    int i, j, k;

    for (i = 0; i < H_AVAIL_N; i++)
        for (j = 0; j < H_STRIDE_N; j++)
            for (k = 0; k < H_DIVISOR_N; k++)
            {
                ULONG avail  = h_avail[i];
                ULONG stride = h_stride[j];
                ULONG plain  = (avail / h_divisor[k]) / stride;

                if (plain < (ULONG)AMI_POOL_WORKING_PACKETS)
                    continue;

                h_checkf(ami_ns_pool_packets_for(avail, h_divisor[k], stride) ==
                         h_clamp(plain),
                         "the pool moved on a machine above the working floor",
                         avail, stride, h_divisor[k]);
            }
}


/* The raise stops at the working floor: it buys a working pool, not a big
   one.  Below the floor the answer never exceeds AMI_POOL_WORKING_PACKETS. */
static void d_the_raise_stops_at_the_working_floor(void)
{
    int i, j;

    for (i = 0; i < H_AVAIL_N; i++)
        for (j = 0; j < H_STRIDE_N; j++)
        {
            ULONG avail  = h_avail[i];
            ULONG stride = h_stride[j];
            ULONG plain  = (avail / (ULONG)AMI_POOL_MEM_DIVISOR) / stride;

            if (plain >= (ULONG)AMI_POOL_WORKING_PACKETS)
                continue;

            h_checkf(ami_ns_pool_packets_for(avail, (ULONG)AMI_POOL_MEM_DIVISOR,
                                             stride) <=
                     (ULONG)AMI_POOL_WORKING_PACKETS,
                     "the raise went past the working floor",
                     avail, stride, (ULONG)AMI_POOL_MEM_DIVISOR);
        }
}


/* Free memory and the pool move the same way.  A branch that inverts this is
   how a machine with more RAM ends up with a smaller pool than one with
   less, and nothing on the wire would say so. */
static void e_more_memory_never_buys_less(void)
{
    int i, j, k;

    for (j = 0; j < H_STRIDE_N; j++)
        for (k = 0; k < H_DIVISOR_N; k++)
            for (i = 1; i < H_AVAIL_N; i++)
            {
                ULONG lo = ami_ns_pool_packets_for(h_avail[i - 1], h_divisor[k],
                                                   h_stride[j]);
                ULONG hi = ami_ns_pool_packets_for(h_avail[i], h_divisor[k],
                                                   h_stride[j]);

                h_checkf(hi >= lo, "more free memory bought a smaller pool",
                         h_avail[i], h_stride[j], h_divisor[k]);
            }
}


/*
 * Where the constant comes from: AMI_POOL_WORKING_PACKETS is the pool at
 * which the eighth-share budget covers the floor window twice over.  Lowering
 * it, raising BSD_TCP_WINDOW, or widening BSD_TCP_WINDOW_POOL_SHARE without
 * moving the other two puts the pool and the window back out of step, and
 * this is the check that says so.
 */
static void f_the_working_floor_covers_the_window_twice(void)
{
    h_check(ami_bsd_tcp_budget((ULONG)AMI_POOL_WORKING_PACKETS,
                               (ULONG)AMI_POOL_PAYLOAD) >=
            2UL * (ULONG)BSD_TCP_WINDOW,
            "the working floor no longer covers the floor window twice");

    /* And the floor window itself has to be reachable at that pool: a socket
       that is alone on the machine gets the floor and no less. */
    h_check(ami_bsd_tcp_window_for((ULONG)AMI_POOL_WORKING_PACKETS,
                                   (ULONG)AMI_POOL_PAYLOAD, 0UL) >=
            (ULONG)BSD_TCP_WINDOW,
            "one socket on a working pool is below the floor window");
}


/*
 * The measured machine, with the numbers from the v0.25.5 message.  47 and
 * 7,840 are what the sixteenth gives and what made the window unbacked; 94
 * and 17,248 are what ships.
 */
static void g_the_a1200_with_no_fast_ram(void)
{
    ULONG plain = (A1200_AVAIL / (ULONG)AMI_POOL_MEM_DIVISOR) / M68K_STRIDE;
    ULONG pool  = ami_ns_pool_packets_for(A1200_AVAIL,
                                          (ULONG)AMI_POOL_MEM_DIVISOR,
                                          M68K_STRIDE);

    h_check(plain == 47UL, "the sixteenth no longer buys the measured 47");
    h_check(ami_bsd_tcp_budget(plain, (ULONG)AMI_POOL_PAYLOAD) <
            (ULONG)BSD_TCP_WINDOW,
            "the sixteenth's budget is no longer short of the floor window");

    h_check(pool == 94UL, "the eighth no longer buys the measured 94");
    h_check(ami_bsd_tcp_budget(pool, (ULONG)AMI_POOL_PAYLOAD) >=
            (ULONG)BSD_TCP_WINDOW,
            "the shipped pool cannot hold the floor window");

    /* Two sockets sharing it still get a backed window, which is the case a
       browser and a download are. */
    h_check(2UL * ami_bsd_tcp_window_for(pool, (ULONG)AMI_POOL_PAYLOAD, 1UL) <=
            ami_bsd_tcp_budget(pool, (ULONG)AMI_POOL_PAYLOAD),
            "two sockets between them advertise past the budget");
}


/*
 * THE OTHER END OF THE RANGE, 2026-09-15.  A 32 MB, a 128 MB and a 1.8 GB
 * machine (an A3000 with Zorro III RAM, a PiStorm) were all the same machine
 * while the clamp was 512: a budget of 64 packets, one socket at 100,352
 * bytes, 18 Mbit/s at 22 ms whatever the link.  Now the divisor sizes the
 * pool until the clamp, the clamp sits past 107 MB free, and a socket has two
 * windows: the one it opens with, which is still the old ceiling at most
 * (bursts on a LAN), and the one it may grow to once its handshake has shown
 * a long path, bounded by BSD_TCP_WINDOW_MAX -- a claim about links, not
 * about memory.  Where it SETTLES once the link and the path are known is
 * ami_bsd_tcp_window_settle(), tested below.
 */
static void h_a_big_machine_is_bounded_by_the_link(void)
{
    static const ULONG big[] = { 33554432UL, 134217728UL, 1887436800UL };
    ULONG floor128 = (134217728UL / (ULONG)AMI_POOL_MEM_DIVISOR) / M68K_STRIDE;
    int   i, j;

    /* The clamp is out of the way of a 128 MB machine's own sixteenth, and
       the clamped pool's budget backs two sockets at the maximum. */
    h_check(floor128 > (ULONG)AMI_POOL_MAX_PACKETS,
            "a 128 MB machine no longer reaches the pool clamp");
    h_check(ami_bsd_tcp_budget((ULONG)AMI_POOL_MAX_PACKETS,
                               (ULONG)AMI_POOL_PAYLOAD) >=
            (ULONG)BSD_TCP_WINDOW_MAX,
            "the clamped pool's budget cannot back one socket at the maximum");
    /* A big pool gives TCP receive a quarter, a small one the eighth it had:
       the line is BSD_TCP_WINDOW_BIG_POOL, and 8 MB machines are under it. */
    h_check(ami_bsd_tcp_budget((ULONG)BSD_TCP_WINDOW_BIG_POOL,
                               (ULONG)AMI_POOL_PAYLOAD) ==
            ((ULONG)BSD_TCP_WINDOW_BIG_POOL / BSD_TCP_WINDOW_POOL_SHARE_BIG) *
                (ULONG)AMI_POOL_PAYLOAD,
            "a big pool does not give TCP receive the half share");
    h_check(ami_bsd_tcp_budget((ULONG)BSD_TCP_WINDOW_BIG_POOL - 1UL,
                               (ULONG)AMI_POOL_PAYLOAD) ==
            (((ULONG)BSD_TCP_WINDOW_BIG_POOL - 1UL) / BSD_TCP_WINDOW_POOL_SHARE) *
                (ULONG)AMI_POOL_PAYLOAD,
            "a pool under the line does not keep the eighth share");
    h_check(ami_ns_pool_packets_for(8388608UL, (ULONG)AMI_POOL_MEM_DIVISOR,
                                    M68K_STRIDE) < (ULONG)BSD_TCP_WINDOW_BIG_POOL,
            "an 8 MB machine's pool is over the big-pool line");

    for (i = 0; i < (int)(sizeof(big) / sizeof(big[0])); i++)
        for (j = 0; j < H_STRIDE_N; j++)
        {
            ULONG pool = ami_ns_pool_packets_for(big[i],
                                                 (ULONG)AMI_POOL_MEM_DIVISOR,
                                                 h_stride[j]);
            ULONG budget = ami_bsd_tcp_budget(pool, (ULONG)AMI_POOL_PAYLOAD);
            ULONG one  = ami_bsd_tcp_window_for(pool, (ULONG)AMI_POOL_PAYLOAD,
                                                0UL);
            ULONG two  = ami_bsd_tcp_window_for(pool, (ULONG)AMI_POOL_PAYLOAD,
                                                1UL);
            ULONG max1 = ami_bsd_tcp_window_max_for(pool,
                                                    (ULONG)AMI_POOL_PAYLOAD,
                                                    0UL);
            ULONG max2 = ami_bsd_tcp_window_max_for(pool,
                                                    (ULONG)AMI_POOL_PAYLOAD,
                                                    1UL);
            ULONG want1 = budget;
            ULONG want2 = budget / 2UL;

            /* The budget first, the link ceiling only where the budget is
               past it: a 32 MB machine at the widest stride buys 1,110
               packets, a budget of 217 KB, and that is its first socket's
               maximum. */
            if (want1 > (ULONG)BSD_TCP_WINDOW_CEILING)
                want1 = (ULONG)BSD_TCP_WINDOW_CEILING;
            if (want2 > (ULONG)BSD_TCP_WINDOW_CEILING)
                want2 = (ULONG)BSD_TCP_WINDOW_CEILING;

            h_checkf(pool <= (ULONG)AMI_POOL_MAX_PACKETS,
                     "the pool passed the clamp", big[i], h_stride[j], 16UL);

            /* What a socket OPENS with on a big machine is the old ceiling,
               whatever the budget: that is the window whose bursts a LAN
               was measured to take. */
            h_checkf(one == (ULONG)BSD_TCP_WINDOW_LAN,
                     "the first socket does not open at the LAN window",
                     big[i], h_stride[j], 16UL);
            h_checkf(two == (ULONG)BSD_TCP_WINDOW_LAN,
                     "a second socket does not open at the LAN window",
                     big[i], h_stride[j], 16UL);

            /* What it may GROW to is min(its share, the link ceiling). */
            h_checkf(max1 == want1,
                     "the first socket's maximum is not min(budget, link ceiling)",
                     big[i], h_stride[j], 16UL);
            h_checkf(max2 == want2,
                     "a second socket's maximum is not min(half budget, ceiling)",
                     big[i], h_stride[j], 16UL);
            h_checkf(max1 >= one && max2 >= two,
                     "a maximum below the window it opens with",
                     big[i], h_stride[j], 16UL);
            h_checkf(2UL * max2 <= budget,
                     "two grown sockets between them exceed the budget",
                     big[i], h_stride[j], 16UL);
            /* From 128 MB up, memory is no longer what bounds the first
               socket: its share of the clamped pool's half is past the
               ceiling. */
            if (big[i] >= 134217728UL)
                h_checkf(max1 == (ULONG)BSD_TCP_WINDOW_CEILING,
                         "a 128 MB machine's first maximum is not the link ceiling",
                         big[i], h_stride[j], 16UL);
        }

    /* 1.8 GB and 128 MB are the same pool: the clamp, not the memory. */
    h_check(ami_ns_pool_packets_for(1887436800UL, (ULONG)AMI_POOL_MEM_DIVISOR,
                                    M68K_STRIDE) ==
            ami_ns_pool_packets_for(134217728UL, (ULONG)AMI_POOL_MEM_DIVISOR,
                                    M68K_STRIDE),
            "past the clamp, more memory still buys packets");
    h_check(ami_ns_pool_packets_for(134217728UL, (ULONG)AMI_POOL_MEM_DIVISOR,
                                    M68K_STRIDE) == (ULONG)AMI_POOL_MAX_PACKETS,
            "a 128 MB machine does not sit exactly on the clamp");

    /* And both windows are a policy under the budget, never above it: on
       12 MB (a pool of 470, under the old clamp) the budget still decides,
       and the two windows are the same number -- nothing to grow to. */
    {
        ULONG pool = ami_ns_pool_packets_for(12582912UL,
                                             (ULONG)AMI_POOL_MEM_DIVISOR,
                                             M68K_STRIDE);
        ULONG one  = ami_bsd_tcp_window_for(pool, (ULONG)AMI_POOL_PAYLOAD, 0UL);
        ULONG max1 = ami_bsd_tcp_window_max_for(pool, (ULONG)AMI_POOL_PAYLOAD,
                                                0UL);

        h_check(one < (ULONG)BSD_TCP_WINDOW_LAN,
                "a 12 MB machine is not bounded by its budget any more");
        h_check(one == ami_bsd_tcp_budget(pool, (ULONG)AMI_POOL_PAYLOAD),
                "a 12 MB machine's first socket does not get its whole budget");
        h_check(max1 == one,
                "a 12 MB machine has something to grow to");
    }

    /* Every machine in the small table: the window it opens with is exactly
       what it was before any of this. */
    {
        int k;

        for (k = 0; k < H_AVAIL_N; k++)
        {
            ULONG pool = ami_ns_pool_packets_for(h_avail[k],
                                                 (ULONG)AMI_POOL_MEM_DIVISOR,
                                                 M68K_STRIDE);
            ULONG one  = ami_bsd_tcp_window_for(pool, (ULONG)AMI_POOL_PAYLOAD,
                                                0UL);
            ULONG old  = ami_bsd_tcp_budget(pool, (ULONG)AMI_POOL_PAYLOAD);

            if (old > 100352UL)
                old = 100352UL;
            if (old < (ULONG)BSD_TCP_WINDOW)
                old = (ULONG)BSD_TCP_WINDOW;

            h_checkf(one == old, "a machine up to 32 MB opens differently",
                     h_avail[k], M68K_STRIDE, 16UL);
        }
    }
}


/*
 * WHERE A SOCKET SETTLES, from the numbers in bsdsocket_window.h.  The window
 * it opened with (`created`) is the LAN window or the budget share, the
 * maximum is what the link ceiling allows, the link speed comes from the
 * interface the connection landed on, the round trip from its own handshake.
 */
static void i_the_window_settles_for_the_path_and_the_link(void)
{
    const ULONG lan   = (ULONG)BSD_TCP_WINDOW_LAN;      /* 100,352 */
    const ULONG max   = (ULONG)BSD_TCP_WINDOW_MAX;      /* 1,048,576 */
    const ULONG lmax  = (ULONG)BSD_TCP_WINDOW_MAX_LAN;  /* 262,144 */
    const ULONG gbit  = (ULONG)BSD_TCP_WINDOW_FAST_BPS;
    const ULONG tenm  = 10000000UL;
    const ULONG hundm = 100000000UL;
    const ULONG rtt   = (ULONG)BSD_TCP_WINDOW_GROW_RTT_MS;

    /* A long path grows to the maximum on any link, even a gigabit one. */
    h_check(ami_bsd_tcp_window_settle(lan, max, gbit, rtt) == max,
            "a long path on a gigabit link does not grow");
    h_check(ami_bsd_tcp_window_settle(lan, max, hundm, 22UL) == max,
            "a 22 ms path on 100 Mbit does not grow");
    h_check(ami_bsd_tcp_window_settle(lan, max, 0UL, 95UL) == max,
            "a long path on an unknown link does not grow");
    /* ... but never past what it was created with when there is no maximum
       (a peer with no scaling, a small machine). */
    h_check(ami_bsd_tcp_window_settle(lan, lan, gbit, rtt) == lan,
            "a long path grew past a maximum equal to the created window");
    h_check(ami_bsd_tcp_window_settle(8192UL, 8192UL, 0UL, 300UL) == 8192UL,
            "a small machine's floor window moved on a long path");

    /* A LAN round trip on a gigabit link: the size the burst was measured
       at -- the driver's ring and 128 posted reads back it (202 Mbit/s at
       262,144 against 186 at 65,535, no frame lost, bsdsocket_window.h) --
       and no further, whatever the maximum: there the window is one burst. */
    h_check(ami_bsd_tcp_window_settle(lan, max, gbit, rtt - 1UL) == lmax,
            "a gigabit LAN socket did not settle at the LAN maximum");
    h_check(ami_bsd_tcp_window_settle(lan, max, gbit, 0UL) == lmax,
            "a passive gigabit socket did not settle at the LAN maximum");
    h_check(ami_bsd_tcp_window_settle(50176UL, max, gbit, 0UL) == lmax,
            "a small created window on a gigabit LAN did not grow");
    h_check(ami_bsd_tcp_window_settle(lan, 200000UL, gbit, 0UL) == 200000UL,
            "a gigabit LAN socket grew past a maximum under the LAN maximum");
    /* ... while the same socket on a long path takes the whole maximum. */
    h_check(ami_bsd_tcp_window_settle(lan, max, gbit, rtt) == max &&
            max > lmax,
            "a long path over a gigabit link stopped at the LAN maximum");

    /* Whether the card must hold the window: a LAN round trip always, a
       slower card on any path; a gigabit card on a long path is drained at
       wire speed into posted reads while the far link paces the data. */
    h_check(ami_bsd_tcp_window_burst_bound(gbit, rtt - 1UL) == TRUE,
            "a gigabit LAN socket is not burst-bound");
    h_check(ami_bsd_tcp_window_burst_bound(gbit, 0UL) == TRUE,
            "a passive gigabit socket is not burst-bound");
    h_check(ami_bsd_tcp_window_burst_bound(gbit, rtt) == FALSE,
            "a gigabit card on a long path is burst-bound");
    h_check(ami_bsd_tcp_window_burst_bound(hundm, 26UL) == TRUE,
            "a 100 Mbit card on a long path is not burst-bound");
    h_check(ami_bsd_tcp_window_burst_bound(tenm, 300UL) == TRUE,
            "a 10 Mbit card on a long path is not burst-bound");
    h_check(ami_bsd_tcp_window_burst_bound(0UL, 95UL) == TRUE,
            "an unknown link on a long path is not burst-bound");
    h_check(ami_bsd_tcp_window_settle(lan, lan, gbit, 0UL) == lan,
            "a gigabit LAN socket grew past a maximum equal to its window");

    /* A LAN round trip on 10 or 100 Mbit: exactly what it opened with. */
    h_check(ami_bsd_tcp_window_settle(lan, max, hundm, 1UL) == lan,
            "a 100 Mbit LAN socket did not keep the LAN window");
    h_check(ami_bsd_tcp_window_settle(lan, max, tenm, 5UL) == lan,
            "a 10 Mbit LAN socket did not keep the LAN window");
    h_check(ami_bsd_tcp_window_settle(lan, max, 0UL, 0UL) == lan,
            "an unknown link on a LAN did not keep the LAN window");

    /* The card's own memory caps whatever settled: an X-Surf 100's 13 KB
       ring (52 pages) holds eight 1536-byte frames of 1460 payload each;
       the GENET's 256 KB holds more than the LAN window; a card that says
       nothing caps nothing; two segments is the floor. */
    h_check(ami_bsd_tcp_window_fit(lan, 52UL * 256UL, 1460UL) == 8UL * 1460UL,
            "a 13 KB ring did not cap the window at eight segments");
    h_check(ami_bsd_tcp_window_fit(lan, 128UL * 2048UL, 1460UL) == lan,
            "a 256 KB ring capped a window it can hold");
    h_check(ami_bsd_tcp_window_fit(lan, 0UL, 1460UL) == lan,
            "an unstated capacity capped the window");
    h_check(ami_bsd_tcp_window_fit(8192UL, 2048UL, 1460UL) == 2UL * 1460UL,
            "a one-frame ring did not floor the window at two segments");
    h_check(ami_bsd_tcp_window_fit(lan, 52UL * 256UL, 0UL) == lan,
            "a zero MSS was not left alone");
}


/*
 * The bytes the pool is sized from are the fastest memory class's, not
 * everything Exec has: the A3000 with 12 MB of motherboard RAM ahead of a
 * 256 MB Zorro III card, and the machines that must stay exactly as they
 * were -- one header, several at one priority, none at all.
 */
static void j_the_pool_is_sized_from_the_fastest_class(void)
{
    static const LONG  a3000_pri[]  = { 30, 20 };
    static const ULONG a3000_free[] = { 12UL << 20, 256UL << 20 };
    static const LONG  one_pri[]    = { 20 };
    static const ULONG one_free[]   = { 128UL << 20 };
    static const LONG  two_pri[]    = { 20, 20 };
    static const ULONG two_free[]   = { 16UL << 20, 16UL << 20 };
    static const LONG  card_pri[]   = { 30, 40 };
    static const ULONG card_free[]  = { 16UL << 20, 128UL << 20 };
    static const LONG  z2_pri[]     = { 0, 20 };
    static const ULONG z2_free[]    = { 8UL << 20, 64UL << 20 };
    ULONG stride = 1568UL + 64UL;

    h_check(ami_ns_pool_avail_of(a3000_pri, a3000_free, 2) == (12UL << 20),
            "the A3000 is sized from its 12 MB of motherboard RAM, not 268");
    h_check(ami_ns_pool_packets_for(12UL << 20, (ULONG)AMI_POOL_MEM_DIVISOR,
                                    stride) == ((12UL << 20) / 16UL) / stride,
            "and that is a few hundred packets, not the cap");
    h_check(ami_ns_pool_avail_of(one_pri, one_free, 1) == (128UL << 20),
            "one header: the number AvailMem() gave");
    h_check(ami_ns_pool_avail_of(two_pri, two_free, 2) == (32UL << 20),
            "two boards at one priority: summed, as before");
    h_check(ami_ns_pool_avail_of(card_pri, card_free, 2) == (128UL << 20),
            "a CPU card's RAM above the motherboard's: the card's");
    h_check(ami_ns_pool_avail_of(z2_pri, z2_free, 2) == (64UL << 20),
            "Zorro III RAM above Zorro II: the Zorro III");
    h_check(ami_ns_pool_avail_of(NULL, NULL, 0) == 0UL,
            "no Fast RAM at all: 0, and the caller asks AvailMem()");

    printf("  fastest class     A3000 12 MB -> %lu packets\n",
           (unsigned long)ami_ns_pool_packets_for(12UL << 20,
                                                  (ULONG)AMI_POOL_MEM_DIVISOR,
                                                  stride));
}

int main(void)
{
    printf("packet pool sizing and the window it backs, v0.25.5\n");

    a_a_backable_floor_window_is_backed();
    b_never_more_than_an_eighth();
    c_a_machine_above_the_floor_is_untouched();
    d_the_raise_stops_at_the_working_floor();
    e_more_memory_never_buys_less();
    f_the_working_floor_covers_the_window_twice();
    g_the_a1200_with_no_fast_ram();
    h_a_big_machine_is_bounded_by_the_link();
    i_the_window_settles_for_the_path_and_the_link();
    j_the_pool_is_sized_from_the_fastest_class();

    printf("%lu checks, %lu failures, %s\n",
           h_checks, h_failures, (h_failures == 0UL) ? "PASS" : "FAIL");

    return (h_failures == 0UL) ? 0 : 1;
}

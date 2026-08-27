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

    printf("%lu checks, %lu failures, %s\n",
           h_checks, h_failures, (h_failures == 0UL) ? "PASS" : "FAIL");

    return (h_failures == 0UL) ? 0 : 1;
}

/*
 * Does a periodic ThreadX timer keep its period?
 *
 * _tx_timer_interrupt() advances _tx_timer_current_ptr only when the slot it
 * points at is EMPTY, and here it is called from the tick task rather than
 * from an interrupt, so the timer thread may not have drained it in time.
 *
 * The wheel loses those ticks while delivered, clipped, lost and skew all stay
 * what they should be, so netstat -h reports nothing wrong.  That is why this
 * probe measures the period rather than the counters.
 *
 * Output is key=value.  Exit 0 if every arm held its period inside TOLERANCE,
 * 10 otherwise.
 *
 * SPDX-License-Identifier: MIT
 */

/* tx_api.h FIRST: exec/types.h does #define VOID void, which collides with
   tx_port.h's typedef void VOID if the Amiga headers land first. */
#include "tx_api.h"
#include "tx_amiga.h"

#include <proto/exec.h>
#include <proto/dos.h>

#include "aminetxduo/compat.h"

#define PERIOD          50      /* NX_DHCP_TIME_INTERVAL, one second */
#define SAMPLES         12
#define NOISE           3       /* other timers, as a running stack has */
#define TOLERANCE       2       /* ticks either way, for the sampling itself */

static TX_TIMER  probe;
static TX_TIMER  noise[NOISE];
static ULONG     stamp[SAMPLES];
static ULONG     taken;
static ULONG     noise_hits;

static VOID probe_entry(ULONG id)
{
    (VOID)id;

    if (taken < (ULONG)SAMPLES)
    {
        stamp[taken] = tx_time_get();
        taken++;
    }
}

static VOID noise_entry(ULONG id)
{
    (VOID)id;
    noise_hits++;
}

VOID tx_application_define(VOID *first_unused)
{
    (VOID)first_unused;

    (VOID)tx_timer_create(&probe, "drift probe", probe_entry, 0,
                          (ULONG)PERIOD, (ULONG)PERIOD, TX_NO_ACTIVATE);
}

/*
 * Timers that are not the one under test, at periods that share no factor with
 * it, so their slots come up on ticks the probe's does not.  A machine running
 * the stack has several: the IP thread's own one-second periodic, ARP, TCP,
 * and every tx_thread_sleep() in the tree takes a slot of its own.  With the
 * wheel stalling once per occupied slot rather than once per second, this arm
 * is the one that matches what DHCP saw.
 */
static void noise_start(void)
{
    UINT  i;
    ULONG period = 7UL;

    for (i = 0; i < (UINT)NOISE; i++)
    {
        (VOID)tx_timer_create(&noise[i], "drift noise", noise_entry, (ULONG)i,
                              period, period, TX_AUTO_ACTIVATE);
        period += 6UL;          /* 7, 13, 19 */
    }
}

static void noise_stop(void)
{
    UINT i;

    for (i = 0; i < (UINT)NOISE; i++)
    {
        (VOID)tx_timer_deactivate(&noise[i]);
        (VOID)tx_timer_delete(&noise[i]);
    }
}

/* One arm: run the probe timer SAMPLES times and report the deltas. */
static LONG arm(const char *name)
{
    ULONG i;
    ULONG deltas = 0UL;
    ULONG sum    = 0UL;
    ULONG min    = 0xFFFFFFFFUL;
    ULONG max    = 0UL;
    ULONG wall0;
    ULONG wall;

    taken = 0UL;

    wall0 = ami_millis();
    (VOID)tx_timer_activate(&probe);

    /* Delay(), not tx_thread_sleep(): a sleep is itself a wheel entry and
       would add a slot to the thing being measured. */
    while (taken < (ULONG)SAMPLES)
    {
        Delay(25);              /* half a second of DOS ticks */
    }

    (VOID)tx_timer_deactivate(&probe);
    wall = ami_millis() - wall0;

    for (i = 1UL; i < (ULONG)SAMPLES; i++)
    {
        ULONG d = stamp[i] - stamp[i - 1UL];

        deltas++;
        sum += d;
        if (d < min)
        {
            min = d;
        }
        if (d > max)
        {
            max = d;
        }
    }

    Printf((CONST_STRPTR)"arm=%s period=%lu n=%lu mean=%lu min=%lu max=%lu "
                         "wall_ms=%lu\n",
           (LONG)name, (LONG)PERIOD, (LONG)deltas, (LONG)(sum / deltas),
           (LONG)min, (LONG)max, (LONG)wall);

    /* The period in ticks the wheel actually delivered, against the period it
       was created with.  Over by more than TOLERANCE is the defect. */
    return (LONG)((sum / deltas) - (ULONG)PERIOD);
}

int main(int argc, char **argv)
{
    LONG solo;
    LONG loaded;
    LONG bad = 0;
    UINT status;

    TX_AMIGA_TICK_STATS s;

    (void)argc;
    (void)argv;

    Printf((CONST_STRPTR)"timerdrift: a periodic ThreadX timer, %lu ticks\n",
           (LONG)PERIOD);

    status = tx_amiga_kernel_start();
    if (status != TX_SUCCESS)
    {
        Printf((CONST_STRPTR)"tx_amiga_kernel_start() failed: %lu\n", (ULONG)status);
        return RETURN_FAIL;
    }

    /* The tick validates its wakeup source for ~250 ms before the first tick. */
    Delay(50);

    solo = arm("solo");

    noise_start();
    loaded = arm("loaded");
    noise_stop();

    tx_amiga_tick_stats(&s);

    status = tx_amiga_kernel_stop();
    if (status != TX_SUCCESS)
    {
        Printf((CONST_STRPTR)"tx_amiga_kernel_stop() returned %lu\n", (ULONG)status);
    }

    /* Printed beside the arms because they are what a reader checks first, and
       on the machine this was written for they say nothing is wrong. */
    Printf((CONST_STRPTR)"ticks=%lu uptime_ms=%lu clipped=%lu lost=%lu "
                         "deferred=%lu skew=%lu skew_peak=%lu\n",
           (LONG)s.tx_amiga_tick_delivered, (LONG)tx_amiga_uptime_ms(&s),
           (LONG)s.tx_amiga_tick_clipped, (LONG)s.tx_amiga_tick_lost,
           (LONG)s.tx_amiga_tick_deferred, (LONG)s.tx_amiga_tick_skew,
           (LONG)s.tx_amiga_tick_skew_peak);

    if (solo > (LONG)TOLERANCE || solo < -(LONG)TOLERANCE)
    {
        bad++;
    }
    if (loaded > (LONG)TOLERANCE || loaded < -(LONG)TOLERANCE)
    {
        bad++;
    }

    Printf((CONST_STRPTR)"solo_over=%ld loaded_over=%ld noise_hits=%lu "
                         "result=%s\n",
           solo, loaded, (LONG)noise_hits, (LONG)(bad ? "fail" : "pass"));

    return bad ? 10 : 0;
}

/*
 * WbJitter -- how long a Workbench-priority task waits for the CPU while the
 * stack moves data.
 *
 *   WbJitter [PRI/N/K] [SECS/N/K]
 *
 * AmiTCP_NG #9 is a Workbench that freezes for the length of a transfer on
 * Emu68.  Our reader runs at priority 1, which is Workbench's own, and the IP
 * thread at 2, so the question is measurable and this measures it: a task at
 * PRI (default 1, Workbench) asks for one tick with Delay(1) and reads the
 * E-clock each time it gets the CPU back.  A gap much longer than the 20 ms it
 * asked for is time Workbench would have spent not repainting, not taking a
 * click.  Run it once idle and once under an iperf or a fetch and compare.
 *
 * Output is key=value: samples, max_ms, the histogram of gaps, and
 * stall_ms, the total time spent inside gaps over 100 ms.
 *
 * MEASURED 2026-09-19 on the real A3000 (68030/25, X-Surf 100, the machine
 * this stack saturates at ~4 Mbit/s), RAM:WbJitter over the ssh door, iperf
 * to and from playhouse2 for 40 s around each 30 s sample:
 *
 *   arm                      samples  max_ms  gaps 40-100  stalls >100
 *   idle, PRI 1                  897      35            0            0
 *   idle, PRI 0                  598      40            0            0
 *   TX 3.99 Mbit/s, PRI 1        895      51           11            0
 *   RX 4.15 Mbit/s, PRI 1        897      45            4            0
 *   RX 4.19 Mbit/s, PRI 0        879      51          168            0
 *
 * So at the CPU wall Workbench waits 51 ms at most and a priority-0 program
 * sees a fifth of its ticks stretch to under 100 ms; nothing freezes.  The
 * emulated A1200 (tools/amiberry-run.sh -m A1200 -a "SECS 10") reads 250
 * samples, max 40 ms, idle -- the smoke that came before the machine.
 *
 * SPDX-License-Identifier: MIT
 */
#include <exec/types.h>
#include <exec/execbase.h>
#include <dos/dos.h>
#include <dos/rdargs.h>
#include <devices/timer.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/timer.h>

#include "aminetxduo/compat.h"

#define TEMPLATE "PRI/N/K,SECS/N/K"

/* Printf() wants CONST_STRPTR, an unsigned char pointer. */
#define P(fmt, ...) Printf((CONST_STRPTR)(fmt), ##__VA_ARGS__)

extern struct Device *TimerBase;        /* src/common/compat.c owns it */

static const ULONG bucket_ms[] = { 40, 100, 250, 500, 1000 };
#define BUCKETS (sizeof(bucket_ms) / sizeof(bucket_ms[0]) + 1)

static ULONG eclock_ms(const struct EClockVal *a, const struct EClockVal *b,
                       ULONG rate)
{
    ULONG lo = b->ev_lo - a->ev_lo;

    /* A gap never nears 2^32 E-clock ticks (an hour and a half); the low
       word carries it. */
    return lo / (rate / 1000UL);
}

int main(void)
{
    struct RDArgs    *rda;
    LONG              args[2] = { 0, 0 };
    LONG              pri  = 1;
    LONG              secs = 30;
    struct Task      *me   = FindTask(NULL);
    BYTE              old_pri;
    struct EClockVal  prev, now, start;
    ULONG             rate;
    ULONG             hist[BUCKETS];
    ULONG             samples  = 0;
    ULONG             max_ms   = 0;
    ULONG             stall_ms = 0;
    ULONG             i;
    ULONG             total_ms;
    BOOL              broken   = FALSE;

    rda = ReadArgs((CONST_STRPTR)TEMPLATE, args, NULL);
    if (rda == NULL)
    {
        PrintFault(IoErr(), (CONST_STRPTR)"WbJitter");
        return RETURN_ERROR;
    }
    if (args[0] != 0) pri  = *(LONG *)args[0];
    if (args[1] != 0) secs = *(LONG *)args[1];
    FreeArgs(rda);

    if (secs < 1)   secs = 1;
    if (secs > 600) secs = 600;
    if (pri < -5)   pri  = -5;
    if (pri > 5)    pri  = 5;

    (VOID)ami_millis();                 /* opens timer.device, sets TimerBase */
    if (TimerBase == NULL)
    {
        P("WbJitter: timer.device did not open\n");
        return RETURN_FAIL;
    }
    rate = ReadEClock(&start);
    if (rate == 0)
    {
        P("WbJitter: E-clock rate is 0\n");
        return RETURN_FAIL;
    }

    for (i = 0; i < BUCKETS; i++)
        hist[i] = 0;

    old_pri = SetTaskPri(me, pri);
    (VOID)ReadEClock(&prev);
    start = prev;

    for (;;)
    {
        ULONG gap;

        Delay(1);
        (VOID)ReadEClock(&now);
        gap  = eclock_ms(&prev, &now, rate);
        prev = now;
        samples++;

        if (gap > max_ms)
            max_ms = gap;
        if (gap > 100UL)
            stall_ms += gap;
        for (i = 0; i < BUCKETS - 1; i++)
            if (gap <= bucket_ms[i])
                break;
        hist[i]++;

        if (eclock_ms(&start, &now, rate) >= (ULONG)secs * 1000UL)
            break;
        if ((SetSignal(0, 0) & SIGBREAKF_CTRL_C) != 0)
        {
            broken = TRUE;
            break;
        }
    }

    (VOID)SetTaskPri(me, old_pri);
    total_ms = eclock_ms(&start, &now, rate);

    P("wbjitter pri=%ld secs=%ld eclock=%lu\n", pri, secs, rate);
    P("samples=%lu total_ms=%lu max_ms=%lu stall_ms=%lu stall_pct=%lu\n",
           samples, total_ms, max_ms, stall_ms,
           total_ms ? (stall_ms * 100UL) / total_ms : 0UL);
    P("gaps le40=%lu le100=%lu le250=%lu le500=%lu le1000=%lu gt1000=%lu\n",
           hist[0], hist[1], hist[2], hist[3], hist[4], hist[5]);
    if (broken)
        P("broken=1\n");

    ami_timer_close();
    return RETURN_OK;
}

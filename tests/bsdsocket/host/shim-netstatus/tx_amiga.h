/*
 * tx_amiga.h for the host tier.
 *
 * netstatus.c includes it for tx_amiga_tick_stats() and the tick counters it
 * copies into NETSTATUS_HEALTH. The real header is port/threadx-amiga/inc,
 * which cannot be on the include path here: it carries the Amiga tx_port.h,
 * which types LONG as `long` where this tier's ThreadX linux port types it as
 * `int`, and every structure the test links against would then have a
 * different shape than the one that ships (tests/bsdsocket/CMakeLists.txt).
 *
 * The member names and order are the real header's, so a rename there stops
 * the build here rather than silently zeroing a counter.
 *
 * IN A DIRECTORY OF ITS OWN, not in host/shim beside the rest. tests/tick puts
 * host/shim on its include path for exec/types.h and the real tx_amiga.h after
 * it, and a copy of this name in the shared directory shadows the real one for
 * that target: test_tick_conv lost tx_amiga_eclock_ms() to exactly that.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_HOST_TX_AMIGA_H
#define AMINETXDUO_HOST_TX_AMIGA_H

#include "tx_api.h"

typedef struct TX_AMIGA_TICK_STATS_STRUCT
{
    ULONG   tx_amiga_tick_unit;
    ULONG   tx_amiga_tick_fallback;
    ULONG   tx_amiga_tick_eclock_hz;
    ULONG   tx_amiga_tick_source_chz;
    ULONG   tx_amiga_tick_wakeups;
    ULONG   tx_amiga_tick_delivered;
    ULONG   tx_amiga_tick_empty;
    ULONG   tx_amiga_tick_catchups;
    ULONG   tx_amiga_tick_clipped;
    ULONG   tx_amiga_tick_lost;
    ULONG   tx_amiga_tick_service_us;
    ULONG   tx_amiga_tick_uptime_ms;
    ULONG   tx_amiga_tick_uptime_rem;
    ULONG   tx_amiga_tick_worst_stall_ms;
    ULONG   tx_amiga_tick_worst_service_us;
    ULONG   tx_amiga_tick_over_budget;
    ULONG   tx_amiga_tick_deferred;
    ULONG   tx_amiga_tick_skew;
    ULONG   tx_amiga_tick_skew_peak;
} TX_AMIGA_TICK_STATS;

VOID tx_amiga_tick_stats(TX_AMIGA_TICK_STATS *stats);

/* The green realm's census, mirrored for the same reason as the tick stats:
   netstatus.c copies it into NETSTATUS_RXBUDGET.  The host tier has no green
   realm, so the shim in host/shim-netstatus/tx_amiga_green_stub.c answers
   zeros, which is also what a baton build on the Amiga answers.  */
typedef struct TX_AMIGA_GREEN_STATS_STRUCT
{
    ULONG   gs_switches;
    ULONG   gs_external;
    ULONG   gs_idle_waits;
    ULONG   gs_wait_fast;
    ULONG   gs_wait_slow;
    ULONG   gs_stray_wait;
    ULONG   gs_gate_calls;
    ULONG   gs_gate_fallback;
    ULONG   gs_realm_sigbits;
    ULONG   gs_gate_fast;
} TX_AMIGA_GREEN_STATS;

VOID tx_amiga_green_stats(TX_AMIGA_GREEN_STATS *stats);

static __inline ULONG tx_amiga_uptime_ms(const TX_AMIGA_TICK_STATS *t)
{
    ULONG hz = t->tx_amiga_tick_eclock_hz;

    if (hz == 0)
        return t->tx_amiga_tick_uptime_ms;

    return t->tx_amiga_tick_uptime_ms +
           ((t->tx_amiga_tick_uptime_rem * 1000UL) / hz);
}

#endif /* AMINETXDUO_HOST_TX_AMIGA_H */

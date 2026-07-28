/*
 * AmiNetXDuo -- nx_secure glue: entropy seeding and an E-Clock microsecond
 * timer.
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * Use a private library base for the timer inlines.  src/common/compat.c
 * defines the conventional `TimerBase` for its own ami_millis(); with
 * -fno-common (the GCC 15 default) a second definition here would be a
 * duplicate symbol as soon as anything links both.  The NDK inlines are
 * parameterised for this.
 */
#define TIMER_BASE_NAME ami_tls_timer_base

#include <exec/types.h>
#include <exec/io.h>
#include <exec/lists.h>
#include <exec/memory.h>
#include <devices/timer.h>
#include <hardware/custom.h>
#include <proto/exec.h>
#include <proto/timer.h>

#include "aminetxduo/random.h"

#include "tls.h"

struct Device                     *ami_tls_timer_base;

static struct timerequest          ami_tls_req;
static struct MsgPort              ami_tls_port;
static ULONG                       ami_tls_hz;

BOOL ami_tls_timer_open(VOID)
{
    struct EClockVal ev;

    if (ami_tls_timer_base != NULL)
    {
        return TRUE;
    }

    ami_tls_port.mp_Node.ln_Type = NT_MSGPORT;
    ami_tls_port.mp_Flags        = PA_IGNORE;
    ami_tls_port.mp_SigTask      = FindTask(NULL);

    /* NewList() is amiga.lib; open-code it so this stays link-library free. */
    ami_tls_port.mp_MsgList.lh_Head     =
        (struct Node *)&ami_tls_port.mp_MsgList.lh_Tail;
    ami_tls_port.mp_MsgList.lh_Tail     = NULL;
    ami_tls_port.mp_MsgList.lh_TailPred =
        (struct Node *)&ami_tls_port.mp_MsgList.lh_Head;

    ami_tls_req.tr_node.io_Message.mn_Node.ln_Type = NT_MESSAGE;
    ami_tls_req.tr_node.io_Message.mn_ReplyPort    = &ami_tls_port;
    ami_tls_req.tr_node.io_Message.mn_Length       = sizeof(ami_tls_req);

    if (OpenDevice((STRPTR)TIMERNAME, UNIT_ECLOCK,
                   (struct IORequest *)&ami_tls_req, 0) != 0)
    {
        return FALSE;
    }

    ami_tls_timer_base = ami_tls_req.tr_node.io_Device;

    ami_tls_hz = ReadEClock(&ev);
    if (ami_tls_hz == 0)
    {
        ami_tls_hz = 709379UL;         /* PAL, if the device lies */
    }

    return TRUE;
}

BOOL ami_tls_timer_is_open(VOID)
{

    return((BOOL)(ami_tls_timer_base != NULL));
}

VOID ami_tls_timer_close(VOID)
{

    if (ami_tls_timer_base == NULL)
    {
        return;
    }

    CloseDevice((struct IORequest *)&ami_tls_req);
    ami_tls_timer_base = NULL;
}

ULONG ami_tls_eclock(VOID)
{
    struct EClockVal ev;

    if (!ami_tls_timer_open())
    {
        return 0;
    }

    ReadEClock(&ev);
    return ev.ev_lo;
}

ULONG ami_tls_eclock_hz(VOID)
{

    (VOID) ami_tls_timer_open();
    return ami_tls_hz;
}

ULONG ami_tls_eclock_micros(ULONG ticks)
{

    if (ami_tls_hz == 0)
    {
        (VOID) ami_tls_timer_open();
    }
    if (ami_tls_hz == 0)
    {
        return 0;
    }

    /*
     * 64-bit intermediate: at ~709 kHz a one-second measurement is ~709,000
     * ticks, and ticks * 1,000,000 overflows 32 bits after ~4,295 ticks (6 ms).
     * This is a report path, not a hot path, so the __udivdi3 call (out of
     * src/common/ami_udivdi3.c -- this toolchain's libgcc.a is empty) is
     * acceptable here.  It is not acceptable inside the timed region, so every
     * measurement below accumulates raw ticks and converts once.
     */
    return (ULONG)(((unsigned long long)ticks * 1000000ULL) /
                   (unsigned long long)ami_tls_hz);
}

ULONG ami_tls_seed_rng(VOID)
{
    ULONG eclock;

    ami_random_init();

    /*
     * The E-Clock and beam position are already sampled by the pool's own
     * collection; feeding them again credits nothing, and is done only so a
     * caller who opened the timer before the pool did contributes its phase.
     */
    eclock = ami_tls_eclock();
    ami_random_add_entropy(&eclock, sizeof(eclock), 0);

    return ami_random_entropy_bits();
}

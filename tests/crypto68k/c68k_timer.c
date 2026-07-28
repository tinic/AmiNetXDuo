/*
 * AmiNetXDuo -- crypto68k test: E-Clock timing.
 *
 * A private copy of the timer glue rather than a link against src/tls, which
 * only builds when AMINETXDUO_TLS is ON; this test has to stand alone, and
 * linking aminetxduo_tls would drag the whole of nx_secure in for four
 * functions.
 *
 * TIMER_BASE_NAME gives this file its own library base: src/common/compat.c
 * defines the conventional `TimerBase` for ami_millis(), and with -fno-common
 * (the GCC 15 default) a second definition would be a duplicate symbol.
 *
 * SPDX-License-Identifier: MIT
 */

#define TIMER_BASE_NAME c68k_timer_base

#include <exec/types.h>
#include <exec/io.h>
#include <exec/lists.h>
#include <devices/timer.h>
#include <proto/exec.h>
#include <proto/timer.h>

#include "c68k_timer.h"

struct Device              *c68k_timer_base;

static struct timerequest   c68k_req;
static struct MsgPort       c68k_port;
static ULONG                c68k_hz;


BOOL c68k_timer_open(VOID)
{

struct EClockVal    ev;


    if (c68k_timer_base != NULL)
    {
        return(TRUE);
    }

    c68k_port.mp_Node.ln_Type = NT_MSGPORT;
    c68k_port.mp_Flags        = PA_IGNORE;
    c68k_port.mp_SigTask      = FindTask(NULL);

    /* NewList() is amiga.lib; open-code it so this stays link-library free. */
    c68k_port.mp_MsgList.lh_Head     =
        (struct Node *)&c68k_port.mp_MsgList.lh_Tail;
    c68k_port.mp_MsgList.lh_Tail     = NULL;
    c68k_port.mp_MsgList.lh_TailPred =
        (struct Node *)&c68k_port.mp_MsgList.lh_Head;

    c68k_req.tr_node.io_Message.mn_Node.ln_Type = NT_MESSAGE;
    c68k_req.tr_node.io_Message.mn_ReplyPort    = &c68k_port;
    c68k_req.tr_node.io_Message.mn_Length       = sizeof(c68k_req);

    if (OpenDevice((STRPTR)TIMERNAME, UNIT_ECLOCK,
                   (struct IORequest *)&c68k_req, 0) != 0)
    {
        return(FALSE);
    }

    c68k_timer_base = c68k_req.tr_node.io_Device;

    c68k_hz = ReadEClock(&ev);
    if (c68k_hz == 0)
    {
        c68k_hz = 709379UL;             /* PAL, if the device lies */
    }

    return(TRUE);
}


VOID c68k_timer_close(VOID)
{

    if (c68k_timer_base == NULL)
    {
        return;
    }

    CloseDevice((struct IORequest *)&c68k_req);
    c68k_timer_base = NULL;
}


ULONG c68k_eclock(VOID)
{

struct EClockVal    ev;


    if (!c68k_timer_open())
    {
        return(0);
    }

    ReadEClock(&ev);
    return(ev.ev_lo);
}


ULONG c68k_eclock_hz(VOID)
{

    (VOID) c68k_timer_open();
    return(c68k_hz);
}


ULONG c68k_eclock_millis(ULONG ticks)
{

    if (c68k_hz == 0)
    {
        (VOID) c68k_timer_open();
    }
    if (c68k_hz == 0)
    {
        return(0);
    }

    /*
     * Milliseconds, not microseconds: an RSA-2048 private operation without
     * CRT runs for minutes on the floor target, and microseconds overflow a
     * ULONG after 4,295 seconds.  Everything measured here is >= 1 ms.
     *
     * The 64-bit divide is a __udivdi3 call, resolved out of
     * src/common/ami_udivdi3.c (this toolchain's libgcc.a is empty).  Fine on
     * a report path, not inside a timed region: every measurement accumulates
     * raw ticks and converts exactly once.
     */
    return((ULONG)(((unsigned long long)ticks * 1000ULL) /
                   (unsigned long long)c68k_hz));
}


ULONG c68k_eclock_micros(ULONG ticks)
{

    if (c68k_hz == 0)
    {
        (VOID) c68k_timer_open();
    }
    if (c68k_hz == 0)
    {
        return(0);
    }

    /*
     * ticks * 1,000,000 reaches ~1.1e14 for the longest operation here, which
     * is why the intermediate is 64-bit; the result fits a ULONG comfortably.
     */
    return((ULONG)(((unsigned long long)ticks * 1000000ULL) /
                   (unsigned long long)c68k_hz));
}

/*
 * AmiNetXDuo, shared AmigaOS glue.
 *
 * No dependencies beyond exec.library and timer.device: this code runs inside
 * a shared library, so it must not pull in the newlib stdio.
 *
 * SPDX-License-Identifier: MIT
 */

#include "aminetxduo/compat.h"

#include <exec/execbase.h>
#include <exec/semaphores.h>
#include <devices/timer.h>
#include <proto/exec.h>
#include <inline/macros.h>
#include <proto/timer.h>

/* ------------------------------------------------------------------ memory */

/* In a census build compat.h points both names at the tagging wrappers in
   alloccensus.c. These are the functions that those wrappers call. */
#undef ami_alloc
#undef ami_alloc_flags

/* The record that the health mark points at, in aminetxduo/compat.h. */
static AmiMemStats ami_mem;

APTR ami_alloc_flags(ULONG size, ULONG memf)
{
    APTR p;

    if (size == 0)
        return NULL;

    /* Exec picks by MemHeader priority, which puts Fast ahead of Chip already.
       A request for MEMF_FAST first, with a fallback, cannot change where
       anything lands. When Fast has room, exec uses it. When Fast has no room,
       the fallback goes to Chip, which is where exec puts it too. */
    p = AllocVec(size, memf);

    Forbid();
    if (p != NULL)
    {
        ami_mem.ms_Live++;
        if (ami_mem.ms_Live > ami_mem.ms_LiveMax)
            ami_mem.ms_LiveMax = ami_mem.ms_Live;
    }
    else
    {
        ami_mem.ms_Refused++;
    }
    Permit();

    return p;
}

APTR ami_alloc(ULONG size)
{
    return ami_alloc_flags(size, MEMF_PUBLIC | MEMF_CLEAR);
}

VOID ami_free(APTR ptr)
{
    if (ptr == NULL)
        return;

    /* Before the block goes: the census keeps its size, and it must never read
       that size from a freed pointer. */
    AMI_CENSUS_DROP(ptr);

    FreeVec(ptr);

    Forbid();
    ami_mem.ms_Live--;
    Permit();
}

ULONG ami_alloc_count(VOID)
{
    return ami_mem.ms_Live;
}

AmiMemStats *ami_mem_stats(VOID)
{
    return &ami_mem;
}

VOID ami_mem_socket_delta(LONG delta)
{
    Forbid();
    if (delta > 0)
    {
        ami_mem.ms_Sockets += (ULONG)delta;
        if (ami_mem.ms_Sockets > ami_mem.ms_SocketsMax)
            ami_mem.ms_SocketsMax = ami_mem.ms_Sockets;
    }
    else if (ami_mem.ms_Sockets >= (ULONG)(-delta))
    {
        ami_mem.ms_Sockets -= (ULONG)(-delta);
    }
    Permit();
}

VOID ami_mem_open_delta(LONG delta)
{
    Forbid();
    if (delta > 0)
        ami_mem.ms_Opens += (ULONG)delta;
    else if (ami_mem.ms_Opens >= (ULONG)(-delta))
        ami_mem.ms_Opens -= (ULONG)(-delta);
    Permit();
}

/* --------------------------------------------------------------- utilities */

BYTE ami_signal_alloc(VOID)
{
    return (BYTE)AllocSignal(-1);
}

VOID ami_signal_free(BYTE sig)
{
    if (sig >= 0)
        FreeSignal(sig);
}

/*
 * EClock-based millisecond counter. The EClock rate is machine dependent
 * (~709379 Hz PAL, ~715909 Hz NTSC), so read it once and scale.
 */
/* The inline ReadEClock() macro resolves the library base through TimerBase. */
struct Device             *TimerBase;

static struct timerequest  ami_timer_req;
static struct MsgPort      ami_timer_port;
static ULONG               ami_eclock_hz;
/* Running total and the reading that last advanced it, in ami_millis(). */
static struct EClockVal    ami_eclock_last;
static ULONG               ami_eclock_ms;
static ULONG               ami_eclock_rem;
static ULONG               ami_eclock_carry;   /* thousandths of a tick */

/*
 * One request, one port, opened lazily, so the open must be serialised.
 * ami_millis() runs on SANA-II reader Tasks (bpf_amiga.c) as well as on
 * application tasks. Two of them that race the TimerBase test both
 * OpenDevice() the same static timerequest. The second overwrites io_Device,
 * timer.device is left one open too many, and the request memory belongs to a
 * segment that can be unloaded.
 *
 * A semaphore, not Forbid(): OpenDevice() can Wait, which breaks a Forbid and
 * leaves a lock in name only. The one-time init of the semaphore is the
 * Forbid-and-flag shape that netstack.c uses for the same problem.
 */
static struct SignalSemaphore  ami_timer_lock;
static volatile BOOL           ami_timer_lock_ready;

/*
 * The "it is safe to use" flag, and not TimerBase, because TimerBase cannot be
 * both. The NDK ReadEClock() inline resolves the library base through
 * TimerBase, so TimerBase must be set before the rate is read. That leaves a
 * window in which a caller on the fast path below sees a non-NULL TimerBase
 * and an ami_eclock_hz still at zero, and ami_millis() divides by it. This
 * flag is set after the last field, and the fast path tests this flag.
 */
static volatile BOOL           ami_timer_ready;

static VOID ami_timer_lock_init(VOID)
{
    Forbid();
    if (!ami_timer_lock_ready)
    {
        InitSemaphore(&ami_timer_lock);
        ami_timer_lock_ready = TRUE;
    }
    Permit();
}

static BOOL ami_timer_init(VOID)
{
    struct EClockVal ev;
    ULONG            rate;

    if (ami_timer_ready)
        return TRUE;

    ami_timer_lock_init();
    ObtainSemaphore(&ami_timer_lock);

    /* Again inside the lock: any caller that was ahead has finished. */
    if (ami_timer_ready)
    {
        ReleaseSemaphore(&ami_timer_lock);
        return TRUE;
    }

    ami_timer_port.mp_Node.ln_Type = NT_MSGPORT;
    ami_timer_port.mp_Flags        = PA_IGNORE;
    /* NULL, not FindTask(NULL): exec never reads mp_SigTask on a PA_IGNORE
       port, and whichever task came first here exits long before the library
       does. A stale pointer here is a Signal() into freed memory for the next
       caller that gives this port a reason to signal. */
    ami_timer_port.mp_SigTask      = NULL;

    /* NewList() lives in amiga.lib. A shared library open-codes it. */
    ami_timer_port.mp_MsgList.lh_Head     =
        (struct Node *)&ami_timer_port.mp_MsgList.lh_Tail;
    ami_timer_port.mp_MsgList.lh_Tail     = NULL;
    ami_timer_port.mp_MsgList.lh_TailPred =
        (struct Node *)&ami_timer_port.mp_MsgList.lh_Head;

    ami_timer_req.tr_node.io_Message.mn_Node.ln_Type = NT_MESSAGE;
    ami_timer_req.tr_node.io_Message.mn_ReplyPort    = &ami_timer_port;
    ami_timer_req.tr_node.io_Message.mn_Length       = sizeof(ami_timer_req);

    if (OpenDevice((STRPTR)TIMERNAME, UNIT_ECLOCK,
                   (struct IORequest *)&ami_timer_req, 0) != 0)
    {
        ReleaseSemaphore(&ami_timer_lock);
        return FALSE;
    }

    /* Before ReadEClock(), which is an inline that goes through it. */
    TimerBase = ami_timer_req.tr_node.io_Device;

    /*
     * Scale to ticks-per-millisecond first: a 64-bit divide pulls __udivdi3
     * out of libgcc, which a shared library must not need. ~709 ticks/ms PAL,
     * ~716 NTSC, a ~0.1% rounding error.
     */
    rate = ReadEClock(&ev);
    ami_eclock_hz = (rate != 0UL) ? rate : 709379UL;
    ami_eclock_last = ev;
    ami_eclock_ms   = 0UL;
    ami_eclock_rem  = 0UL;
    ami_eclock_carry = 0UL;

    ami_timer_ready = TRUE;

    ReleaseSemaphore(&ami_timer_lock);

    return TRUE;
}

/*
 * Give timer.device its open back. Called from bsd_lib_expunge() by way of
 * bsd_runtime_close(): the request and the port are file-scope statics. Without
 * this call the segment goes to UnLoadSeg() with the device still open against
 * memory inside it, and one more open accumulates on every load and expunge
 * cycle.
 */
VOID ami_timer_close(VOID)
{
    if (!ami_timer_ready)
        return;

    ami_timer_lock_init();
    ObtainSemaphore(&ami_timer_lock);

    if (ami_timer_ready)
    {
        /* Unpublish first: a caller on the fast path must not enter
           ami_millis() between the CloseDevice() and the clear of the base. */
        ami_timer_ready = FALSE;
        CloseDevice((struct IORequest *)&ami_timer_req);
        TimerBase       = NULL;
        ami_eclock_hz   = 0UL;
    }

    ReleaseSemaphore(&ami_timer_lock);
}

/*
 * ami_millis() when the clock is already there, and 0 when it is not.
 *
 * ami_millis() opens timer.device on its first call and that ObtainSemaphore()
 * can Wait.  The event recorder (src/common/events.c) runs inside lib_expunge,
 * where Exec holds Forbid(), and a Wait there breaks the Forbid on a path whose
 * whole job is to decide whether the segment may be unloaded.  So this asks and
 * does not open.  A stack that has ever taken a timestamp -- which is every one
 * that got as far as bringing an interface up -- answers a real time.
 */
ULONG ami_millis_quick(VOID)
{
    return ami_timer_ready ? ami_millis() : 0UL;
}

/* The measured E-Clock rate, or 0 before timer.device answered.  A reader
   converting tick aggregates to time wants the measured number, not the PAL
   constant a NTSC machine would be wrong by. */
ULONG ami_eclock_rate(VOID)
{
    return ami_timer_ready ? ami_eclock_hz : 0UL;
}

#ifdef AMINETXDUO_RXPROBE
/*
 * The raw E-Clock low word, for the step budget's hop stamps
 * (aminetxduo/budget.h).  Raw and not ami_millis(): the hops are tens of
 * microseconds to a few milliseconds, which milliseconds cannot resolve, and
 * the budget subtracts two nearby readings so the ~100-minute wrap costs at
 * most one discarded sample.  It lives here because ami_timer_ready is the
 * one honest gate on ReadEClock() and this file owns it.  Zero doubles as
 * "no clock yet", and the budget's disarmed state is also zero, so a stamp
 * taken before the timer exists arms nothing.
 */
ULONG ami_budget_clock(VOID)
{
    struct EClockVal ev;

    if (!ami_timer_ready)
        return 0UL;

    (VOID)ReadEClock(&ev);

    return ev.ev_lo;
}
#endif

ULONG ami_millis(VOID)
{
    struct EClockVal ev;
    ULONG            delta;
    ULONG            gain;
    ULONG            num;
    ULONG            ms;

    if (!ami_timer_init())
        return 0;

    /*
     * Accumulated, not measured from a fixed base. ev_lo is 32 bits at
     * ~710 kHz and wraps every ~100 minutes, so a subtraction from a base
     * captured at init is right once and wrong afterwards. A machine up two
     * hours reports a few minutes. Each call adds the interval since the last
     * call, which is correct across a wrap, and carries the remainder.
     *
     * Divided by the rate and not by a ticks-per-millisecond value, because
     * 709379/1000 truncates to 709 and runs the clock 0.05% fast, 46 seconds
     * a day. That appears as drift against an accurate reference.
     *
     * Two calls more than ~100 minutes apart lose a whole wrap, and the
     * counter cannot survive that. Any caller that watches a clock calls more
     * often.
     */
    Forbid();

    ReadEClock(&ev);
    delta           = ev.ev_lo - ami_eclock_last.ev_lo;
    ami_eclock_last = ev;

    /* Whole seconds convert exactly and keep the operands of the fine step
       small. What is left is under one second, so rem * 1000 cannot overflow. */
    ami_eclock_ms  += (delta / ami_eclock_hz) * 1000UL;
    ami_eclock_rem += delta % ami_eclock_hz;
    if (ami_eclock_rem >= ami_eclock_hz)
    {
        ami_eclock_rem -= ami_eclock_hz;
        ami_eclock_ms  += 1000UL;
    }

    gain             = (ami_eclock_rem * 1000UL) / ami_eclock_hz;
    ami_eclock_ms   += gain;

    /* The ticks those milliseconds consumed, to a thousandth of a tick: the
       remainder is carried, or the truncation alone costs 3.5 s a day.  */
    num              = gain * ami_eclock_hz + ami_eclock_carry;
    ami_eclock_rem  -= num / 1000UL;
    ami_eclock_carry = num % 1000UL;
    ms               = ami_eclock_ms;

    Permit();

    return ms;
}

/* ---------------------------------------------------------- SANA-II opens */

#define AMI_SANA2_DEVS_SUBDIR   "Networks/"
#define AMI_SANA2_NAME_MAX      128

/*
 * Set by the netstack when it owns the AMITCP port. compat.c is linked into
 * commands that have no netstack, so this cannot be a direct call.
 */
static VOID (*ami_sana2_quiesce)(VOID);
static VOID (*ami_sana2_restore)(VOID);

VOID ami_sana2_set_open_hooks(VOID (*quiesce)(VOID), VOID (*restore)(VOID))
{
    ami_sana2_quiesce = quiesce;
    ami_sana2_restore = restore;
}

/* Same arrangement, for the address-change signal: see compat.h. */
static VOID (*ami_address_change_hook)(VOID);

VOID ami_set_address_change_hook(VOID (*hook)(VOID))
{
    ami_address_change_hook = hook;
}

VOID ami_address_change_notify(VOID)
{
    VOID (*hook)(VOID) = ami_address_change_hook;

    /* Read once: the library can deregister from another task while the IP
       thread is here. A NULL test on the global, followed by a call through
       it, is two reads of a value that changes between them. */
    if (hook != NULL)
        hook();
}

/* Same arrangement again, for the one-second heartbeat: see compat.h for the
   interrupt-level contract this one carries and the address-change hook does
   not. */
static VOID (*ami_second_hook)(VOID);

VOID ami_set_second_hook(VOID (*hook)(VOID))
{
    ami_second_hook = hook;
}

VOID ami_second_notify(VOID)
{
    VOID (*hook)(VOID) = ami_second_hook;

    /* Read once, for the reason given above ami_address_change_notify(): the
       library can deregister from another task while the tick is here. */
    if (hook != NULL)
        hook();
}

/* And again for the shutdown request, in compat.h. This one runs on an
   ordinary task, so it carries neither of the other two contracts. */
static VOID (*ami_shutdown_hook)(VOID);

VOID ami_set_shutdown_hook(VOID (*hook)(VOID))
{
    ami_shutdown_hook = hook;
}

VOID ami_shutdown_notify(VOID)
{
    VOID (*hook)(VOID) = ami_shutdown_hook;

    if (hook != NULL)
        hook();
}

static LONG ami_sana2_open_once(const char *name, ULONG unit,
                                struct IORequest *req)
{
    LONG status;

    /* If an iComp driver finds the AMITCP port here, it bypasses the copy
       callbacks. See ami_ns_port_suspend(). */
    if (ami_sana2_quiesce != NULL)
        ami_sana2_quiesce();

    status = (LONG)(BYTE)OpenDevice((CONST_STRPTR)name, unit, req, 0);

    if (ami_sana2_restore != NULL)
        ami_sana2_restore();

    return status;
}

LONG ami_sana2_open_device(const char *name, ULONG unit, struct IORequest *req)
{
    char  path[sizeof(AMI_SANA2_DEVS_SUBDIR) + AMI_SANA2_NAME_MAX];
    LONG  status;
    ULONG i;

    if (name == NULL || *name == '\0' || req == NULL)
        return -1;

    status = ami_sana2_open_once(name, unit, req);
    if (status == 0)
        return 0;

    /* A name that already carries a path was meant literally. */
    for (i = 0; name[i] != '\0'; i++)
    {
        if (name[i] == '/' || name[i] == ':')
            return status;
    }
    if (i >= AMI_SANA2_NAME_MAX)
        return status;

    for (i = 0; AMI_SANA2_DEVS_SUBDIR[i] != '\0'; i++)
        path[i] = AMI_SANA2_DEVS_SUBDIR[i];
    for (; *name != '\0'; name++)
        path[i++] = *name;
    path[i] = '\0';

    /* OpenDevice leaves io_Device set on a failed open of a device that does
       exist. The retry must not be read as a success against the old one. */
    req->io_Device = NULL;
    req->io_Unit   = NULL;

    return ami_sana2_open_once(path, unit, req);
}

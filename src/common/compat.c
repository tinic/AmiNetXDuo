/*
 * AmiNetXDuo -- shared AmigaOS glue.
 *
 * Dependency-free beyond exec.library and timer.device: this code runs inside
 * a shared library, so it must not drag in newlib's stdio.
 *
 * SPDX-License-Identifier: MIT
 */

#include "aminetxduo/compat.h"

#include <exec/execbase.h>
#include <devices/timer.h>
#include <proto/exec.h>
#include <proto/timer.h>

#include <stdarg.h>

#ifndef AMINETXDUO_LOG_LEVEL
#  ifdef AMINETXDUO_DEBUG
#    define AMINETXDUO_LOG_LEVEL AMI_LOG_DEBUG
#  else
#    define AMINETXDUO_LOG_LEVEL AMI_LOG_WARN
#  endif
#endif

/* ------------------------------------------------------------------ memory */

/* The record the health mark points at; aminetxduo/compat.h. */
static AmiMemStats ami_mem;

APTR ami_alloc_flags(ULONG size, ULONG memf)
{
    APTR p;

    if (size == 0)
        return NULL;

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

/* ----------------------------------------------------------------- logging */

/*
 * RawPutChar is an exec LVO (-516) that the NDK declares only in the assembler
 * headers, so wire it up here. Serial debug output is the one sink a shared
 * library can always reach.
 */
#ifndef RawPutChar
#  define RawPutChar(c) \
      LP1NR(0x204, RawPutChar, UBYTE, (c), d0, , EXEC_BASE_NAME)
#endif

/* RawDoFmt callback: one character to the serial debug port. */
static VOID put_char(register UBYTE c   __asm("d0"),
                     register APTR unused __asm("a3"))
{
    (VOID)unused;
    if (c != '\0')
        RawPutChar(c);
}

VOID ami_log(int level, const char *fmt, ...)
{
    static const char *const prefix[] = { "ERR ", "WARN", "INFO", "DBG ", "TRC " };
    va_list args;

    if (level > AMINETXDUO_LOG_LEVEL)
        return;
    if (level < AMI_LOG_ERROR || level > AMI_LOG_TRACE)
        level = AMI_LOG_INFO;

    RawPutChar('[');
    {
        const char *p = prefix[level];
        while (*p != '\0')
            RawPutChar(*p++);
    }
    RawPutChar(']');
    RawPutChar(' ');

    va_start(args, fmt);
    RawDoFmt((STRPTR)fmt, args, (void (*)())put_char, NULL);
    va_end(args);

    RawPutChar('\n');
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
/* Running total and the reading it was last advanced from; ami_millis(). */
static struct EClockVal    ami_eclock_last;
static ULONG               ami_eclock_ms;
static ULONG               ami_eclock_rem;
static ULONG               ami_eclock_carry;   /* thousandths of a tick */

/*
 * One IORequest and one accumulator for the machine, so the init has to be
 * one-shot. Unguarded it was a plain test-then-act: two tasks reaching
 * ami_millis() for the first time together both fell through and both
 * OpenDevice()d the same &ami_timer_req -- Exec's "reuse of an active
 * IORequest" -- and the second re-zeroed ami_eclock_ms under the first, which
 * a caller measuring `ami_millis() - start` sees as an unsigned wrap.
 *
 * Forbid() rather than a semaphore: this is reached from the SANA-II receive
 * path via ami_bpf_now(), where blocking is not allowed. timer.device's Open
 * is a table lookup and does not Wait, so the Forbid holds across it.
 */
static BOOL ami_timer_init(VOID)
{
    struct EClockVal ev;
    ULONG            rate;
    BOOL             ok;

    if (TimerBase != NULL)
        return TRUE;

    Forbid();

    if (TimerBase != NULL)
    {
        Permit();
        return TRUE;
    }

    ami_timer_port.mp_Node.ln_Type = NT_MSGPORT;
    ami_timer_port.mp_Flags        = PA_IGNORE;
    ami_timer_port.mp_SigTask      = FindTask(NULL);

    /* NewList() lives in amiga.lib; a shared library open-codes it. */
    ami_timer_port.mp_MsgList.lh_Head     =
        (struct Node *)&ami_timer_port.mp_MsgList.lh_Tail;
    ami_timer_port.mp_MsgList.lh_Tail     = NULL;
    ami_timer_port.mp_MsgList.lh_TailPred =
        (struct Node *)&ami_timer_port.mp_MsgList.lh_Head;

    ami_timer_req.tr_node.io_Message.mn_Node.ln_Type = NT_MESSAGE;
    ami_timer_req.tr_node.io_Message.mn_ReplyPort    = &ami_timer_port;
    ami_timer_req.tr_node.io_Message.mn_Length       = sizeof(ami_timer_req);

    ok = (OpenDevice((STRPTR)TIMERNAME, UNIT_ECLOCK,
                     (struct IORequest *)&ami_timer_req, 0) == 0) ? TRUE : FALSE;
    if (!ok)
    {
        Permit();
        return FALSE;
    }

    /* Before ReadEClock(), which is a proto/timer.h inline and calls through
       this very base. Publishing it half-initialised is what the Forbid is
       for -- no other task can observe the window. */
    TimerBase = ami_timer_req.tr_node.io_Device;

    /*
     * Scale to ticks-per-millisecond up front: a 64-bit divide would pull
     * __udivdi3 out of libgcc, which a shared library should not need.
     * ~709 ticks/ms PAL, ~716 NTSC, a ~0.1% rounding error.
     */
    rate = ReadEClock(&ev);
    ami_eclock_hz = (rate != 0UL) ? rate : 709379UL;
    ami_eclock_last = ev;
    ami_eclock_ms   = 0UL;
    ami_eclock_rem  = 0UL;
    ami_eclock_carry = 0UL;

    Permit();

    return TRUE;
}

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
     * Accumulated, not measured from a fixed base.  ev_lo is 32 bits at
     * ~710 kHz and wraps every ~100 minutes, so subtracting a base captured at
     * init is right once and wrong afterwards -- a machine up two hours would
     * report a few minutes.  Each call adds the interval since the last one,
     * which is correct across a wrap, and carries the remainder.
     *
     * Divided by the rate rather than by a ticks-per-millisecond, because
     * 709379/1000 truncates to 709 and runs the clock 0.05% fast -- 46 seconds
     * a day, which shows up as drift against anything honest.
     *
     * The one thing it cannot survive is two calls more than ~100 minutes
     * apart, which loses a whole wrap.  Anything watching a clock calls more
     * often than that.
     */
    Forbid();

    ReadEClock(&ev);
    delta           = ev.ev_lo - ami_eclock_last.ev_lo;
    ami_eclock_last = ev;

    /* Whole seconds convert exactly and keep the fine step's operands small;
       what is left is under one second, so rem * 1000 cannot overflow.  */
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

static LONG ami_sana2_open_once(const char *name, ULONG unit,
                                struct IORequest *req)
{
    LONG status;

    /* An iComp driver samples the AMITCP port here and bypasses our copy
       callbacks if it finds one -- see ami_ns_port_suspend(). */
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
       exist; the retry must not be read as a success against the old one. */
    req->io_Device = NULL;
    req->io_Unit   = NULL;

    return ami_sana2_open_once(path, unit, req);
}

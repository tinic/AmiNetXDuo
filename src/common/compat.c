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

static ULONG ami_outstanding;

APTR ami_alloc_flags(ULONG size, ULONG memf)
{
    APTR p;

    if (size == 0)
        return NULL;

    p = AllocVec(size, memf);
    if (p != NULL)
    {
        Forbid();
        ami_outstanding++;
        Permit();
    }

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
    ami_outstanding--;
    Permit();
}

ULONG ami_alloc_count(VOID)
{
    return ami_outstanding;
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
static ULONG               ami_eclock_per_ms;
static struct EClockVal    ami_eclock_base;

static BOOL ami_timer_init(VOID)
{
    struct EClockVal ev;
    ULONG            rate;

    if (TimerBase != NULL)
        return TRUE;

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

    if (OpenDevice((STRPTR)TIMERNAME, UNIT_ECLOCK,
                   (struct IORequest *)&ami_timer_req, 0) != 0)
        return FALSE;

    TimerBase = ami_timer_req.tr_node.io_Device;

    /*
     * Scale to ticks-per-millisecond up front: a 64-bit divide would pull
     * __udivdi3 out of libgcc, which a shared library should not need.
     * ~709 ticks/ms PAL, ~716 NTSC, a ~0.1% rounding error.
     */
    rate = ReadEClock(&ev);
    ami_eclock_per_ms = rate / 1000UL;
    if (ami_eclock_per_ms == 0)
        ami_eclock_per_ms = 1;
    ami_eclock_base = ev;

    return TRUE;
}

ULONG ami_millis(VOID)
{
    struct EClockVal ev;
    ULONG            ticks;

    if (!ami_timer_init())
        return 0;

    ReadEClock(&ev);

    /*
     * 32-bit low word is enough: at ~710 kHz it wraps every ~100 minutes, and
     * the subtraction below stays correct across a single wrap.
     */
    ticks = ev.ev_lo - ami_eclock_base.ev_lo;

    return ticks / ami_eclock_per_ms;
}

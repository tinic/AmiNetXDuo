/*
 * Forbid()/Disable() section census, the Amiga side.  AMINETXDUO_FORBIDSTATS
 * only; see aminetxduo/forbidstats.h.  AMI_FS_IMPL keeps the proto/exec.h
 * shim out, so the calls below are Exec's own.
 * SPDX-License-Identifier: MIT
 */
#define AMI_FS_IMPL

#include "aminetxduo/forbidstats.h"
#include "aminetxduo/compat.h"

#include <exec/execbase.h>
#include <exec/tasks.h>
#include <devices/timer.h>
#include <proto/exec.h>

#define __NOLIBBASE__
#define TIMER_BASE_NAME ami_fs_timer
#include <proto/timer.h>

#include "netdev_dtree.h"

static struct Device *volatile ami_fs_timer;
static volatile ULONG         *ami_fs_stc;     /* BCM system timer CLO */
static AmiFsState              ami_fs;

#define AMI_FS_CAL_N    256UL

static ULONG fs_eclock(VOID)
{
    struct EClockVal ev;

    (VOID)ReadEClock(&ev);
    return ev.ev_lo;
}

/* On an Emu68 the Pi's 1 MHz system timer is one load and no bus cycle, as
   in tools/forbidrec; elsewhere ReadEClock(), CIA reads under Disable(). */
static ULONG fs_now(VOID)
{
    if (ami_fs_stc != NULL)
        return __builtin_bswap32(*ami_fs_stc);
    return fs_eclock();
}

/* The window is believed only if it keeps time with the E-Clock over 5 ms. */
static ULONG fs_pick_clock(ULONG hz)
{
    ULONG           base;
    volatile ULONG *clo;
    ULONG           c0, c1, e0, e1, dus;

    ami_fs_stc = NULL;
    if (!netdev_dtree_bus_addr("/soc", 0x7e003000UL, &base))
        return hz;
    clo = (volatile ULONG *)(base + 4);
    c0  = __builtin_bswap32(*clo);
    e0  = fs_eclock();
    do
        e1 = fs_eclock();
    while (e1 - e0 < hz / 200UL);
    c1  = __builtin_bswap32(*clo);
    dus = (e1 - e0) * 1000UL / (hz / 1000UL);
    if (c1 - c0 > dus - dus / 20UL && c1 - c0 < dus + dus / 20UL)
    {
        ami_fs_stc = clo;
        return 1000000UL;
    }
    return hz;
}

static LONG fs_nest(ULONG kind)
{
    return (kind == AMI_FS_FORBID) ? (LONG)SysBase->TDNestCnt
                                   : (LONG)SysBase->IDNestCnt;
}

VOID ami_fs_entered(ULONG kind, const char *site)
{
    AmiFsKind *k = &ami_fs.st_Kind[kind];

    k->k_Pairs++;
    if (fs_nest(kind) != 0)
        return;
    if (ami_fs_timer == NULL)
    {
        k->k_Untimed++;
        return;
    }
    ami_fs_core_open(&ami_fs, kind, SysBase->ThisTask, fs_now(),
                     SysBase->DispCount, site);
}

VOID ami_fs_leaving(ULONG kind)
{
    if (fs_nest(kind) != 0 || ami_fs_timer == NULL)
        return;
    ami_fs_core_close(&ami_fs, kind, SysBase->ThisTask, fs_now(),
                      SysBase->DispCount);
}

VOID ami_fs_forbid(const char *site)
{
    Forbid();
    ami_fs_entered(AMI_FS_FORBID, site);
}

VOID ami_fs_permit(VOID)
{
    ami_fs_leaving(AMI_FS_FORBID);
    Permit();
}

VOID ami_fs_disable(const char *site)
{
    Disable();
    ami_fs_entered(AMI_FS_DISABLE, site);
}

VOID ami_fs_enable(VOID)
{
    ami_fs_leaving(AMI_FS_DISABLE);
    Enable();
}

VOID ami_fs_raw_permit(VOID)
{
    Permit();
}

/* Exec lets the nesting go while the task sleeps and puts it back when it
   runs again: the section ends at the Wait() and a new one starts after. */
const char *ami_fs_sleep(ULONG kind)
{
    /* Any depth: Exec lets go of all of it. */
    if (ami_fs_timer == NULL || fs_nest(kind) < 0)
        return NULL;
    return ami_fs_core_sleep(&ami_fs, kind, SysBase->ThisTask, fs_now(),
                             SysBase->DispCount);
}

VOID ami_fs_woken(ULONG kind, const char *site)
{
    if (site == NULL || ami_fs_timer == NULL || fs_nest(kind) < 0)
        return;
    ami_fs_core_open(&ami_fs, kind, SysBase->ThisTask, fs_now(),
                     SysBase->DispCount, site);
}

/*
 * From the tick task, once timer.device is open, at nest -1.  Resets the
 * counts: what came before had no clock.  Three loops on the real path:
 * outermost pairs give the inner overhead (an empty section's measured
 * length) and the cost per pair; pairs under an outer Forbid() give the
 * cost of a nested pair.
 */
VOID ami_fs_calibrate(struct Device *timer, ULONG hz)
{
    static const char cal_outer[] = "forbidstats:cal";
    static const char cal_nested[] = "forbidstats:cal-nested";
    ULONG             i;
    ULONG             t0;
    ULONG             t_outer;
    ULONG             t_nested;
    ULONG             t_empty;
    ULONG             held;
    ULONG             n;

    if (timer == NULL || hz == 0)
        return;

    ami_fs_timer = timer;
    hz = fs_pick_clock(hz);
    Disable();
    ami_fs_core_init(&ami_fs, hz, 0);
    Enable();

    if (SysBase->TDNestCnt >= 0)
        return;                     /* not at nest -1; leave ovh16 at 0 */

    t0 = fs_now();
    for (i = 0; i < AMI_FS_CAL_N; i++)
        (VOID)fs_now();
    t_empty = fs_now() - t0;        /* the loop and one clock read each */

    t0 = fs_now();
    for (i = 0; i < AMI_FS_CAL_N; i++)
    {
        ami_fs_forbid(cal_outer);
        ami_fs_permit();
    }
    t_outer = fs_now() - t0;

    Forbid();
    t0 = fs_now();
    for (i = 0; i < AMI_FS_CAL_N; i++)
    {
        ami_fs_forbid(cal_nested);
        ami_fs_permit();
    }
    t_nested = fs_now() - t0;
    Permit();

    Disable();
    held = 0;
    n    = 0;
    for (i = 0; i < AMI_FS_SITES; i++)
        if (ami_fs.st_Site[i].s_Site == cal_outer)
        {
            held = ami_fs.st_Site[i].s_Held;
            n    = ami_fs.st_Site[i].s_Count;
        }
    ami_fs_core_init(&ami_fs, hz, (n != 0) ? held * 16UL / n : 0);
    ami_fs.st_Calibrated = n;
    ami_fs.st_Outer16    = t_outer * 16UL / AMI_FS_CAL_N;
    ami_fs.st_Nested16   = t_nested * 16UL / AMI_FS_CAL_N;
    ami_fs.st_Clock16    = t_empty * 16UL / AMI_FS_CAL_N;
    Enable();

    AMI_INFO("forbidstats: hz=%ld stc=%ld ovh16=%ld outer16=%ld nested16=%ld "
             "clock16=%ld cal=%ld", (LONG)hz, (LONG)(ami_fs_stc != NULL),
             (LONG)ami_fs.st_Ovh16, (LONG)ami_fs.st_Outer16,
             (LONG)ami_fs.st_Nested16, (LONG)ami_fs.st_Clock16, (LONG)n);
}

/* Before the port lets go of timer.device. */
VOID ami_fs_timer_gone(VOID)
{
    Disable();
    ami_fs_timer = NULL;
    ami_fs.st_Kind[AMI_FS_FORBID].k_Open  = 0;
    ami_fs.st_Kind[AMI_FS_DISABLE].k_Open = 0;
    Enable();
}

VOID ami_fs_read(AmiFsState *out, ULONG *now_hi, ULONG *now_lo)
{
    struct EClockVal ev = { 0, 0 };

    Disable();
    if (ami_fs_timer != NULL)
        ev.ev_lo = fs_now();
    CopyMem((APTR)&ami_fs, (APTR)out, sizeof(ami_fs));
    ami_fs_core_reset_max(&ami_fs);
    Enable();

    *now_hi = ev.ev_hi;
    *now_lo = ev.ev_lo;
}

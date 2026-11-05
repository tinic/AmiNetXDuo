/*
 * Forbid()/Disable() section census, AMINETXDUO_FORBIDSTATS only.
 *
 * Counts every Forbid/Permit and Disable/Enable pair the library makes --
 * ThreadX's TX_DISABLE/TX_RESTORE and our direct calls, which
 * include/forbidstats/proto/exec.h rewrites in this build -- and times each
 * outermost section: the Pi's 1 MHz system timer on an Emu68, else the
 * E-Clock.  A Wait() inside a section ends it (Exec drops the nesting while
 * the task sleeps) and its return opens a new one.  A section Exec
 * dispatched inside anyway (DispCount moved: WaitPort(), DoIO()) slept where
 * the shim cannot see; it is counted as broken and kept out of the held.
 *
 * Times are ticks at st_Hz.  "16" fields are ticks x 16.  Held totals are raw:
 * subtract sections x ovh16 / 16 for the net figure.  Max and the per-site
 * max are net and reset on every read.  A shipped build carries none of this.
 * SPDX-License-Identifier: MIT
 */
#ifndef AMINETXDUO_FORBIDSTATS_H
#define AMINETXDUO_FORBIDSTATS_H

#include <exec/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AMI_FS_FORBID   0
#define AMI_FS_DISABLE  1
#define AMI_FS_KINDS    2

#define AMI_FS_SITES    512     /* power of two */
#define AMI_FS_BUCKETS  4       /* <10 us, <100 us, <1 ms, >= 1 ms */

#define AMI_FS_STR2(x)  #x
#define AMI_FS_STR(x)   AMI_FS_STR2(x)
#define AMI_FS_SITE     (__FILE_NAME__ ":" AMI_FS_STR(__LINE__))

typedef struct AmiFsKind
{
    ULONG       k_Pairs;        /* every enter, nested or not             */
    ULONG       k_Sections;     /* outermost sections timed               */
    ULONG       k_Untimed;      /* outermost sections before the clock    */
    ULONG       k_HeldLo;       /* raw ticks inside timed sections        */
    ULONG       k_HeldHi;
    ULONG       k_Max16;        /* longest net section since the last read */
    const char *k_MaxSite;
    ULONG       k_Hist[AMI_FS_BUCKETS];
    ULONG       k_Sleeps;       /* sections a Wait() ended                 */
    ULONG       k_Lost;         /* open section another task's enter found */
    ULONG       k_Unmatched;    /* outermost exit with nothing of ours open */
    ULONG       k_Broken;       /* Exec dispatched inside: slept unseen     */
    ULONG       k_BrokenHeld;   /* their raw ticks, kept out of the held    */

    APTR        k_Holder;       /* the open section                        */
    ULONG       k_T0;
    ULONG       k_Disp;         /* SysBase->DispCount at the open           */
    const char *k_Site;
    ULONG       k_Open;
} AmiFsKind;

typedef struct AmiFsSite
{
    const char *s_Site;         /* NULL = empty slot */
    ULONG       s_Kind;
    ULONG       s_Count;
    ULONG       s_Held;         /* raw ticks */
    ULONG       s_Max16;        /* net, since the last read */
    ULONG       s_Broken;
} AmiFsSite;

typedef struct AmiFsState
{
    ULONG       st_Hz;
    ULONG       st_Ovh16;       /* inner overhead per timed section        */
    ULONG       st_Outer16;     /* cost added per outermost pair           */
    ULONG       st_Nested16;    /* cost added per nested pair              */
    ULONG       st_Clock16;     /* one ReadEClock() and its loop           */
    ULONG       st_Calibrated;  /* sections the calibration measured       */
    ULONG       st_Lim16[AMI_FS_BUCKETS - 1];
    ULONG       st_SiteOverflow;
    AmiFsKind   st_Kind[AMI_FS_KINDS];
    AmiFsSite   st_Site[AMI_FS_SITES];
} AmiFsState;

/* Pure accounting, host-tested in src/common/test/test_forbidstats.c.
   Callers serialise: Forbid() for the forbid kind, Disable() for the other. */
VOID        ami_fs_core_init(AmiFsState *st, ULONG hz, ULONG ovh16);
VOID        ami_fs_core_open(AmiFsState *st, ULONG kind, APTR task,
                             ULONG now, ULONG disp, const char *site);
VOID        ami_fs_core_close(AmiFsState *st, ULONG kind, APTR task,
                              ULONG now, ULONG disp);
const char *ami_fs_core_sleep(AmiFsState *st, ULONG kind, APTR task,
                              ULONG now, ULONG disp);
VOID        ami_fs_core_reset_max(AmiFsState *st);

/* The Amiga side, src/common/forbidstats.c. */
VOID        ami_fs_entered(ULONG kind, const char *site); /* after the raise */
VOID        ami_fs_leaving(ULONG kind);                   /* before the drop */
VOID        ami_fs_forbid(const char *site);
VOID        ami_fs_permit(VOID);
VOID        ami_fs_disable(const char *site);
VOID        ami_fs_enable(VOID);
const char *ami_fs_sleep(ULONG kind);                   /* before Wait() */
VOID        ami_fs_woken(ULONG kind, const char *site);  /* after it      */
VOID        ami_fs_raw_permit(VOID);
struct Device;
VOID        ami_fs_calibrate(struct Device *timer, ULONG hz);
VOID        ami_fs_timer_gone(VOID);
/* Copies the state under Disable() and resets the max fields. */
VOID        ami_fs_read(AmiFsState *out, ULONG *now_hi, ULONG *now_lo);

#ifdef __cplusplus
}
#endif

#endif /* AMINETXDUO_FORBIDSTATS_H */

/*
 * AmiNetXDuo, shared AmigaOS glue, visible to every component.  Keep it small.
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_COMPAT_H
#define AMINETXDUO_COMPAT_H

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/tasks.h>
#include <exec/semaphores.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ memory */

/* All stack memory goes through these: AllocVec/FreeVec with
   MEMF_PUBLIC|MEMF_CLEAR, plus a guard band and leak counter in debug. */
APTR  ami_alloc(ULONG size);
APTR  ami_alloc_flags(ULONG size, ULONG memf);
VOID  ami_free(APTR ptr);
ULONG ami_alloc_count(VOID);          /* outstanding allocations */

/* The census takes over the two allocating names.  Only src/common/compat.c
   may undefine them, to define the functions themselves. */
#include "aminetxduo/alloccensus.h"

#ifdef AMINETXDUO_ALLOCCENSUS
#  define ami_alloc(size)               ami_alloc_tagged((size), AMI_CENSUS_SITE)
#  define ami_alloc_flags(size, memf)   ami_alloc_flags_tagged((size), (memf), \
                                                               AMI_CENSUS_SITE)
#endif

/*
 * One record, which the published health mark points at.  The pool fields are
 * a sample refreshed by netstack_pool_sample(), not a subscription: NetX Duo
 * also allocates packets from its own internals.
 */
typedef struct AmiMemStats
{
    ULONG   ms_Live;            /* ami_alloc() blocks not yet freed          */
    ULONG   ms_LiveMax;         /* the most there have ever been at once     */
    ULONG   ms_Refused;         /* ami_alloc() calls that came back NULL     */

    ULONG   ms_Sockets;         /* AmiSocket structures the library owns     */
    ULONG   ms_SocketsMax;
    ULONG   ms_Opens;           /* programs holding bsdsocket.library open   */

    ULONG   ms_PoolTotal;       /* packets in the pool; 0 means no pool yet  */
    ULONG   ms_PoolFree;        /* free at the last sample                   */
    ULONG   ms_PoolLow;         /* fewest ever seen free                     */
    ULONG   ms_PoolPayload;     /* bytes per packet                          */
    ULONG   ms_PoolEmpty;       /* requests that found the pool empty        */
    ULONG   ms_PoolWaited;      /* ... and suspended waiting for a packet    */
    ULONG   ms_PoolBadRelease;  /* packets released that were not allocated  */
} AmiMemStats;

/* The live record, never NULL. */
AmiMemStats *ami_mem_stats(VOID);

/* +1 / -1 as a socket or a library open comes and goes.  Both keep a peak. */
VOID ami_mem_socket_delta(LONG delta);
VOID ami_mem_open_delta(LONG delta);

/* ----------------------------------------------------------------- logging */

#define AMI_LOG_ERROR   0
#define AMI_LOG_WARN    1
#define AMI_LOG_INFO    2
#define AMI_LOG_DEBUG   3
#define AMI_LOG_TRACE   4

/*
 * Never call from an interrupt.  RawDoFmt, not printf: %ld/%lu/%lx/%s only,
 * every argument cast to LONG.  %c and bare %d/%u/%x take a WORD and silently
 * misalign everything after them; no %p, no %f, no width-from-argument.
 */
VOID ami_log(int level, const char *fmt, ...);

/*
 * How much of it comes out, AMI_LOG_ERROR..AMI_LOG_TRACE.  A RUNTIME dial and
 * not a build option: the fault tier is in every shipped image, and a machine
 * that has just faulted is turned up where it stands rather than being sent a
 * different binary.  Out of range is clamped.  AMINETXDUO_LOG_LEVEL is only
 * the value it starts at; ENV:ANXDLOGLEVEL is what a user sets, read once at
 * bring-up (src/config/config_file.c, src/netstack/netstack.c).
 */
VOID ami_log_level_set(int level);
int  ami_log_level(VOID);

/*
 * The compiler runtime's own CPU choice.  Pass non-zero when SysBase->AttnFlags
 * has AFF_68020; never calling it leaves the 68000 routines, correct anywhere.
 */
void ami_rt_cpu_select(int have_68020, int have_mulul);

#ifdef AMINETXDUO_DEBUG
#  define AMI_DEBUG(...)  ami_log(AMI_LOG_DEBUG, __VA_ARGS__)
#  define AMI_TRACE(...)  ami_log(AMI_LOG_TRACE, __VA_ARGS__)
#else
#  define AMI_DEBUG(...)  ((void)0)
#  define AMI_TRACE(...)  ((void)0)
#endif
/*
 * AMINETXDUO_LOG off compiles the three out; AMINETXDUO_LOG_LEVEL does not.
 * `if (0)` rather than `((void)0)` so arguments stay type-checked and used.
 *
 * bsdsocket.library and anxnet.device stay RESIDENT, and 27,948 bytes of
 * sentences in the library is 7.2 per cent of a machine's network stack held
 * for a serial port that is not connected.  The event ring is what a shipped
 * image carries instead: a code and a value in BSS, no image bytes, and
 * ShowNetStatus holds the words.
 */
#ifdef AMINETXDUO_LOG
#  define AMI_ERROR(...)  ami_log(AMI_LOG_ERROR, __VA_ARGS__)
#  define AMI_WARN(...)   ami_log(AMI_LOG_WARN,  __VA_ARGS__)
#  define AMI_INFO(...)   ami_log(AMI_LOG_INFO,  __VA_ARGS__)
#else
#  define AMI_ERROR(...)  do { if (0) ami_log(AMI_LOG_ERROR, __VA_ARGS__); } while (0)
#  define AMI_WARN(...)   do { if (0) ami_log(AMI_LOG_WARN,  __VA_ARGS__); } while (0)
#  define AMI_INFO(...)   do { if (0) ami_log(AMI_LOG_INFO,  __VA_ARGS__); } while (0)
#endif

/* --------------------------------------------------------------- utilities */

/* Signal-bit helpers: allocate/free a signal for the calling task, -1 on failure. */
BYTE ami_signal_alloc(VOID);
VOID ami_signal_free(BYTE sig);

/* Milliseconds since stack startup, from EClock. Monotonic, wraps at 2^32. */
ULONG ami_millis(VOID);

/* The same, for a caller that may not block: 0 rather than opening the timer.
   src/common/events.c says why. */
ULONG ami_millis_quick(VOID);
ULONG ami_eclock_rate(VOID);

/* Hands timer.device back the open ami_millis() took lazily. A library must
   call this before its segment is unloaded; a Shell command need not. */
VOID ami_timer_close(VOID);

/*
 * OpenDevice() for a SANA-II driver, with the DEVS:Networks retry.  The name is
 * tried as given first, so an absolute or resident one is unaffected.  0 on
 * success, otherwise the OpenDevice() error.
 */
struct IORequest;
LONG ami_sana2_open_device(const char *name, ULONG unit, struct IORequest *req);

/* Called around every SANA-II OpenDevice. The netstack registers a pair that
   takes the AMITCP port down and puts it back; a command that has no netstack
   registers nothing and the open is unchanged. */
VOID ami_sana2_set_open_hooks(VOID (*quiesce)(VOID), VOID (*restore)(VOID));

/*
 * An interface address changed.  The notify runs on the IP thread: a hook must
 * not block for long and must not call into NetX Duo.
 */
VOID ami_set_address_change_hook(VOID (*hook)(VOID));
VOID ami_address_change_notify(VOID);

/*
 * A once-a-second heartbeat, running on the ThreadX tick task inside a
 * Forbid(), which both ThreadX and NetX Duo count as interrupt level: a hook
 * may not block, wait on a semaphore or mutex, or call into NetX Duo at all.
 */
VOID ami_set_second_hook(VOID (*hook)(VOID));
VOID ami_second_notify(VOID);

/*
 * The network is being shut down.  The hook does NOT wait for anybody; the
 * caller polls if it cares.  Runs on whatever task asked for the shutdown, not
 * on the tick task, so it may take a semaphore.
 */
VOID ami_set_shutdown_hook(VOID (*hook)(VOID));
VOID ami_shutdown_notify(VOID);

#ifdef __cplusplus
}
#endif

#endif /* AMINETXDUO_COMPAT_H */

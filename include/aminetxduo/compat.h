/*
 * AmiNetXDuo -- shared AmigaOS glue.
 *
 * Everything in this header is available to every component. Keep it small: it
 * exists so the port layer, the SANA-II shim, bsdsocket.library and the tools
 * agree on memory allocation, logging and library bases.
 *
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

/*
 * All stack memory goes through these. They wrap AllocVec/FreeVec with
 * MEMF_PUBLIC|MEMF_CLEAR and, in debug builds, a guard band + leak counter.
 * NetX Duo and ThreadX are handed one pre-allocated region each at startup
 * (they do their own sub-allocation), so these are for our own structures.
 */
APTR  ami_alloc(ULONG size);
APTR  ami_alloc_flags(ULONG size, ULONG memf);
VOID  ami_free(APTR ptr);
ULONG ami_alloc_count(VOID);          /* outstanding allocations */

/*
 * What the stack currently owns, and the most it has ever owned.  A suspected
 * leak is answerable only against a number that belongs to us: AvailMem falls
 * for every program on the machine, and a user watching it cannot say whose.
 *
 * One record, so the published health mark (aminetxduo/health.h) can point at
 * it and a reader gets one instant.  It lives here because src/common is the
 * one place every component links -- src/bsdsocket fills the socket and open
 * counts, src/netstack samples the packet pool into it, and the bracket that
 * publishes the mark links neither of those.
 *
 * The pool fields are a sample, not a subscription: NetX Duo allocates packets
 * from its own internals as well as from ours, so there is no one place to
 * count them.  netstack_pool_sample() refreshes them; its header comment says
 * where from.
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
 * Serial/console debug output. Compiled out entirely in release builds except
 * for AMI_LOG_ERROR. Never call from an interrupt.
 *
 * Formatting goes through exec's RawDoFmt, which is not printf.
 *   * Use %ld / %lu / %lx / %s only -- all longword-sized. Cast every argument
 *     to LONG, including pointers and strings: ami_log(..., "%s", (LONG)str).
 *   * Do not use %c or bare %d/%u/%x: RawDoFmt consumes a *word* for those,
 *     while the C caller pushes a longword, so every argument after one is
 *     misaligned and prints garbage, silently -- the first few values look
 *     right and the rest are nonsense.
 *   * No %p, no %f, no field-width-from-argument.
 */
VOID ami_log(int level, const char *fmt, ...);

#ifdef AMINETXDUO_DEBUG
#  define AMI_DEBUG(...)  ami_log(AMI_LOG_DEBUG, __VA_ARGS__)
#  define AMI_TRACE(...)  ami_log(AMI_LOG_TRACE, __VA_ARGS__)
#else
#  define AMI_DEBUG(...)  ((void)0)
#  define AMI_TRACE(...)  ((void)0)
#endif
/*
 * AMINETXDUO_LOG off compiles the three out.  AMINETXDUO_LOG_LEVEL does not:
 * it is tested inside ami_log(), so the format strings are still linked and
 * still passed -- silencing the port costs nothing and saves nothing.
 *
 * `if (0)` rather than `((void)0)` so the arguments are still type-checked and
 * a variable used only in a log call is still used.  The optimiser drops the
 * branch and the strings with it; a build with them out is 12,820 bytes
 * smaller on the 68000 floor tier, which is the whole point.
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

/* Hands timer.device back the open ami_millis() took lazily. A library must
   call this before its segment is unloaded; a Shell command need not. */
VOID ami_timer_close(VOID);

/*
 * OpenDevice() for a SANA-II driver, with the DEVS:Networks retry.
 *
 * A bare device name reaches DOS as DEVS:<name>, and DEVS:Networks -- where
 * every third-party SANA-II driver is installed -- is not on that path. The
 * name is tried as given first, so an absolute or already-resident one is
 * unaffected. 0 on success, otherwise the OpenDevice() error.
 */
struct IORequest;
LONG ami_sana2_open_device(const char *name, ULONG unit, struct IORequest *req);

/* Called around every SANA-II OpenDevice. The netstack registers a pair that
   takes the AMITCP port down and puts it back; a command that has no netstack
   registers nothing and the open is unchanged. */
VOID ami_sana2_set_open_hooks(VOID (*quiesce)(VOID), VOID (*restore)(VOID));

#ifdef __cplusplus
}
#endif

#endif /* AMINETXDUO_COMPAT_H */

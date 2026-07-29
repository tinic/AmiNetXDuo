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
ULONG ami_alloc_count(VOID);          /* debug: outstanding allocations */

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
#define AMI_ERROR(...)    ami_log(AMI_LOG_ERROR, __VA_ARGS__)
#define AMI_WARN(...)     ami_log(AMI_LOG_WARN,  __VA_ARGS__)
#define AMI_INFO(...)     ami_log(AMI_LOG_INFO,  __VA_ARGS__)

/* --------------------------------------------------------------- utilities */

/* Signal-bit helpers: allocate/free a signal for the calling task, -1 on failure. */
BYTE ami_signal_alloc(VOID);
VOID ami_signal_free(BYTE sig);

/* Milliseconds since stack startup, from EClock. Monotonic, wraps at 2^32. */
ULONG ami_millis(VOID);

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

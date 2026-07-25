/*
 * AmiNetXDuo -- crash guard.
 *
 * AmigaOS has no memory protection, so a bad pointer produces a Guru and takes
 * the whole machine down. Under the test harness that means the emulator dies
 * with no output at all: no serial log, no exit status, nothing to debug from.
 *
 * The crash guard installs a tc_TrapCode handler that catches CPU exceptions,
 * dumps the exception number, PC, SR and every register to the serial debug
 * port, and then resumes in user mode at a bail-out routine so the process can
 * exit cleanly. A crash becomes a readable report plus a nonzero exit status
 * instead of a dead emulator.
 *
 * Use it in any test or tool:
 *
 *     ami_crash_set_reference((APTR)main, "main");
 *     ami_crash_install();
 *     ... code under test ...
 *     ami_crash_remove();
 *
 * WHAT IS RELIABLE: the report. A caught exception is dumped to the serial port
 * with the exception name, PC, SR and every register, and a one-line summary is
 * written to DH0:crash.txt, which the harness stages from a host directory and
 * prints after the run. That works.
 *
 * WHAT IS NOT: resuming. The handler patches the exception frame to return into
 * a user-mode bail-out routine, which then longjmp()s back to the install site
 * -- but that unwind has been observed under FS-UAE resuming *after* the call
 * site rather than at it, so ami_crash_install() cannot be relied on to return
 * a second time. Treat a caught crash as fatal: use the guard to find out what
 * died and where, not to keep running afterwards.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_CRASHGUARD_H
#define AMINETXDUO_CRASHGUARD_H

#include <exec/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Install the handler for the calling task. Returns TRUE on the initial call.
 * If a crash is caught, execution resumes here returning FALSE, with the report
 * already written to the serial port.
 */
BOOL ami_crash_install(VOID);

/* Restore the previous tc_TrapCode. Always call this before exiting. */
VOID ami_crash_remove(VOID);

/* Details of the last caught exception, valid after a FALSE return. */
typedef struct AmiCrashInfo
{
    ULONG   number;         /* Exec trap number = 68k exception vector      */
    ULONG   pc;             /* PC from the exception frame                  */
    UWORD   sr;             /* status register                              */
    UWORD   format;         /* 68010+ frame format/vector word              */
    ULONG   d[8];
    ULONG   a[7];           /* a0..a6                                       */
    ULONG   seg_base;       /* load address of the crashing code hunk, or 0 */
    BOOL    valid;
} AmiCrashInfo;

const AmiCrashInfo *ami_crash_info(VOID);

/*
 * Record the load address of the program's code hunk so the report can print
 * PC-relative offsets. Without it, a PC is only useful if you can guess where
 * the hunk was loaded. Pass any function in the code hunk (main is fine) --
 * the reporter prints both the raw PC and PC minus this reference.
 */
VOID ami_crash_set_reference(APTR code_address, const char *label);

/* The exception name for a trap number, e.g. 4 -> "illegal instruction". */
const char *ami_crash_name(ULONG number);

/*
 * Exec Alert (Guru) interception. A Guru is NOT a CPU exception -- Exec calls
 * its own Alert() when it detects corruption, so the trap handler never sees
 * it. These hook exec's Alert vector so a double free, corrupt memory list or
 * reused IORequest gets logged with a decoded reason before the Guru appears.
 *
 * This patches Exec machine-wide: use it in tests and tools under the
 * emulator, and ALWAYS remove it before exiting.
 */
BOOL ami_crash_install_alert_hook(VOID);
VOID ami_crash_remove_alert_hook(VOID);
const char *ami_crash_alert_name(ULONG num);

/* One-line summary of the last crash, e.g. "illegal instruction at PC=00216e64". */
VOID ami_crash_format(char *buf, ULONG len);

#ifdef __cplusplus
}
#endif

#endif /* AMINETXDUO_CRASHGUARD_H */

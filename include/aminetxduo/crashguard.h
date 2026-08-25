/*
 * AmiNetXDuo, crash guard: a tc_TrapCode handler that reports a caught CPU
 * exception.  Resuming is unreliable -- treat a caught crash as fatal.
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

/* Pass any function in the program's code hunk (main is fine) so the report
   can print PC-relative offsets. */
VOID ami_crash_set_reference(APTR code_address, const char *label);

/* The exception name for a trap number, e.g. 4 -> "illegal instruction". */
const char *ami_crash_name(ULONG number);

/*
 * Exec Alert (Guru) interception; the trap handler never sees an Alert.
 * Patches Exec machine-wide: always remove it before exiting.
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

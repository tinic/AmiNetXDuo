/*
 * bsdsocket.library, SBTC_LOG_HOOK: one hook for the machine, handed every
 * syslog() an application makes and, in a build with AMINETXDUO_LOG, every
 * line the stack logs.  Roadshow's NetLogViewer installs one and shows the
 * log in a window; it is the only way a machine with no serial capture reads
 * the log.
 *
 * Global, as Roadshow's is: the last SBTC_LOG_HOOK set is the one called.
 * The base that set it is remembered, so a program that exits without
 * clearing it does not leave a pointer into freed memory being called on
 * the next syslog().
 *
 * THE HOOK RUNS ON THE CALLER'S CONTEXT.  For syslog() that is the
 * application's task.  For the stack's own lines it is whichever task
 * logged: a NetX thread, or the port's timer task, which runs the tick under
 * Forbid() at priority 20.  A hook that Wait()s or takes a semaphore there
 * stalls every thread of the stack, so nothing is delivered under Forbid()
 * or Disable(), or from the timer task at all, and a hook that logs while it
 * runs is not re-entered.
 *
 * SBTC_LOG_FILE_NAME is answered in errno.c, as "no file": nothing in the
 * stack may do DOS file I/O from an IP thread, and there is no task to hand
 * the lines to.  The hook is where the log goes.
 *
 * SPDX-License-Identifier: MIT
 */

#include "bsdsocket_internal.h"
#include "tx_amiga.h"

#include <proto/dos.h>
#include <proto/exec.h>
#include <sys/syslog.h>

typedef LONG (*BsdLogHookFn)(register struct Hook *hook __asm("a0"),
                             register APTR reserved __asm("a2"),
                             register struct LogHookMessage *lhm __asm("a1"));

typedef union BsdLogHookEntry
{
    ULONG        (*blhe_Raw)(VOID);
    BsdLogHookFn   blhe_Fn;
} BsdLogHookEntry;

static struct Hook          *bsd_log_hook;
static struct AmiSocketBase *bsd_log_hook_owner;
static volatile BOOL         bsd_log_hook_busy;

struct Hook *bsd_log_hook_get(VOID)
{
    return bsd_log_hook;
}

BOOL bsd_log_hook_set(struct AmiSocketBase *base, struct Hook *hook)
{
    if (hook != NULL && hook->h_Entry == NULL)
        return FALSE;

    Forbid();
    bsd_log_hook       = hook;
    bsd_log_hook_owner = (hook != NULL) ? base : NULL;
    Permit();

    return TRUE;
}

BOOL bsd_log_hook_installed(VOID)
{
    return (bsd_log_hook != NULL) ? TRUE : FALSE;
}

/* A base closing with its hook still installed, bsd_child_destroy(). */
VOID bsd_log_hook_drop_owner(struct AmiSocketBase *base)
{
    Forbid();
    if (bsd_log_hook_owner == base)
    {
        bsd_log_hook       = NULL;
        bsd_log_hook_owner = NULL;
    }
    Permit();
}

static BOOL bsd_log_hook_context_ok(VOID)
{
    return (tx_amiga_exec_task_context() != TX_FALSE) ? TRUE : FALSE;
}

VOID bsd_log_hook_deliver(LONG priority, STRPTR tag, ULONG id,
                          const char *message)
{
    struct Hook          *hook = bsd_log_hook;
    struct LogHookMessage lhm;
    BsdLogHookEntry       entry;
    BOOL                  taken;

    if (hook == NULL || hook->h_Entry == NULL || message == NULL)
        return;
    if (!bsd_log_hook_context_ok())
        return;

    /* Not under Forbid() here, see above, so the flag is taken under one. */
    Forbid();
    taken = bsd_log_hook_busy ? FALSE : TRUE;
    bsd_log_hook_busy = TRUE;
    Permit();
    if (!taken)
        return;

    lhm.lhm_Size          = (LONG)sizeof(lhm);
    lhm.lhm_Priority      = priority;
    lhm.lhm_Date.ds_Days   = 0;
    lhm.lhm_Date.ds_Minute = 0;
    lhm.lhm_Date.ds_Tick   = 0;
    /* DateStamp() is dos.library; a Process, which every syslog() caller is.
       The stack's own threads are Tasks and get no date. */
    if (DOSBase != NULL && FindTask(NULL)->tc_Node.ln_Type == NT_PROCESS)
        DateStamp(&lhm.lhm_Date);
    lhm.lhm_Tag     = tag;
    lhm.lhm_ID      = id;
    lhm.lhm_Message = (STRPTR)message;

    entry.blhe_Raw = hook->h_Entry;
    (VOID)entry.blhe_Fn(hook, NULL, &lhm);

    bsd_log_hook_busy = FALSE;
}

/* ------------------------------------------------- the rendered form */

typedef struct BsdLogText
{
    char *at;
    char *end;
} BsdLogText;

static VOID bsd_log_text_char(register UBYTE c __asm("d0"),
                              register BsdLogText *text __asm("a3"))
{
    if (c != '\0' && text->at < text->end)
        *text->at++ = (char)c;
}

/* On the caller's stack, an application's: a Shell gives 4 KB. */
#define BSD_LOG_HOOK_TEXT   256

VOID bsd_log_hook_emit(LONG priority, STRPTR tag, ULONG id, const char *fmt,
                       APTR args)
{
    char       rendered[BSD_LOG_HOOK_TEXT];
    BsdLogText text;

    if (bsd_log_hook == NULL || fmt == NULL)
        return;
    if (!bsd_log_hook_context_ok())
        return;

    text.at  = rendered;
    text.end = rendered + sizeof(rendered) - 1;
    RawDoFmt((STRPTR)fmt, args, (void (*)())bsd_log_text_char, &text);
    *text.at = '\0';

    bsd_log_hook_deliver(priority, tag, id, rendered);
}

/* ----------------------------------------------------------- lifetime */

#ifdef AMINETXDUO_LOG
/* ami_serial_logv()'s line sink: the stack's own lines, LOG_KERN as
   Roadshow's are, with no tag. */
static VOID bsd_log_line(int level, const char *line, APTR ctx)
{
    static const LONG priority[] =
        { LOG_ERR, LOG_WARNING, LOG_INFO, LOG_DEBUG, LOG_DEBUG };

    (VOID)ctx;

    if (level < AMI_LOG_ERROR || level > AMI_LOG_TRACE)
        level = AMI_LOG_INFO;

    bsd_log_hook_deliver(LOG_KERN | priority[level], NULL, 0UL, line);
}
#endif

VOID bsd_log_hook_init(VOID)
{
    bsd_log_hook       = NULL;
    bsd_log_hook_owner = NULL;
    bsd_log_hook_busy  = FALSE;
#ifdef AMINETXDUO_LOG
    ami_log_line_sink_set(bsd_log_line, NULL);
#endif
}

VOID bsd_log_hook_exit(VOID)
{
#ifdef AMINETXDUO_LOG
    ami_log_line_sink_set(NULL, NULL);
#endif
    bsd_log_hook       = NULL;
    bsd_log_hook_owner = NULL;
}

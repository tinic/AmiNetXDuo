/*
 * src/bsdsocket/loghook.c on the host: the one SBTC_LOG_HOOK, what it is
 * handed, and the contexts it is not called on.
 *
 * The ROM's RawDoFmt is not here.  The one below passes the format through a
 * character at a time and ends with the NUL the ROM sends, which is the whole
 * of what the renderer relies on; what the ROM makes of a directive is the
 * ROM's.
 *
 * SPDX-License-Identifier: MIT
 */

#include "bsdsocket_internal.h"

#include <stdio.h>
#include <string.h>
#include <sys/syslog.h>

static unsigned long h_checks;
static unsigned long h_failures;

#define CHECK(cond, what)                                                     \
    do {                                                                      \
        h_checks++;                                                           \
        if (!(cond)) {                                                        \
            h_failures++;                                                     \
            printf("  FAIL %s\n", (what));                                   \
        }                                                                     \
    } while (0)

struct ExecBase   *SysBase;
struct DosLibrary *DOSBase;
VOID              *_tx_amiga_timer_task;

static struct ExecBase      h_exec;
static struct Task          h_process;  /* a syslog() caller               */
static struct Task          h_thread;   /* a NetX thread: a Task, no date  */
static struct Task          h_tick;     /* the port's timer task           */
static UBYTE                h_dos[32];  /* any non-NULL DOSBase will do */
static struct AmiSocketBase h_base_a;
static struct AmiSocketBase h_base_b;

/* Exec's nest counts rest at -1: nothing in effect. */
VOID Forbid(VOID) { h_exec.TDNestCnt++; }
VOID Permit(VOID) { h_exec.TDNestCnt--; }

struct Task *FindTask(const char *name)
{
    (VOID)name;
    return h_exec.ThisTask;
}

UINT tx_amiga_exec_task_context(VOID)
{
    return (SysBase != NULL && SysBase->TDNestCnt < 0 &&
            SysBase->IDNestCnt < 0 &&
            (VOID *)SysBase->ThisTask != _tx_amiga_timer_task)
               ? TX_TRUE : TX_FALSE;
}

static unsigned long h_stamps;

VOID DateStamp(struct DateStamp *ds)
{
    h_stamps++;
    ds->ds_Days   = 20000;
    ds->ds_Minute = 61;
    ds->ds_Tick   = 7;
}

typedef VOID (*HostPutCh)(UBYTE c, APTR data);

VOID RawDoFmt(const UBYTE *formatString, APTR dataStream,
              VOID (*putChProc)(VOID), APTR putChData)
{
    HostPutCh put = (HostPutCh)putChProc;

    (VOID)dataStream;

    for (; *formatString != '\0'; formatString++)
        put(*formatString, putChData);
    put('\0', putChData);
}

/* ------------------------------------------------------------- the hook */

static unsigned long         h_calls;
static struct LogHookMessage h_last;
static char                  h_last_text[400];
static int                   h_reenter;
static LONG                  h_hook_result;

static LONG h_hook_fn(struct Hook *hook, APTR reserved,
                      struct LogHookMessage *lhm)
{
    (VOID)hook;
    (VOID)reserved;

    h_calls++;
    h_last = *lhm;
    snprintf(h_last_text, sizeof(h_last_text), "%s", lhm->lhm_Message);

    /* A hook that logs: the inner line must not come back in. */
    if (h_reenter)
    {
        h_reenter = 0;
        bsd_log_hook_deliver(LOG_INFO, NULL, 0UL, "inner");
    }

    return h_hook_result;
}

static struct Hook h_hook;
static struct Hook h_blank;     /* no h_Entry */

/* h_Entry is `ULONG (*)()`, the shape a Hook carries; the real one is the
   installer's.  The union is how loghook.c itself goes the other way. */
typedef union HostHookEntry
{
    ULONG (*raw)(void);
    LONG  (*fn)(struct Hook *hook, APTR reserved, struct LogHookMessage *lhm);
} HostHookEntry;

static VOID reset(VOID)
{
    memset(&h_exec, 0, sizeof(h_exec));
    memset(&h_process, 0, sizeof(h_process));
    memset(&h_thread, 0, sizeof(h_thread));
    memset(&h_tick, 0, sizeof(h_tick));
    memset(&h_last, 0, sizeof(h_last));
    memset(&h_hook, 0, sizeof(h_hook));
    memset(&h_blank, 0, sizeof(h_blank));

    h_exec.TDNestCnt = -1;
    h_exec.IDNestCnt = -1;
    h_exec.ThisTask  = &h_process;
    h_process.tc_Node.ln_Type = NT_PROCESS;
    h_thread.tc_Node.ln_Type  = NT_TASK;
    h_tick.tc_Node.ln_Type    = NT_TASK;

    SysBase              = &h_exec;
    DOSBase              = (struct DosLibrary *)h_dos;
    _tx_amiga_timer_task = &h_tick;

    {
        HostHookEntry entry;

        entry.fn       = h_hook_fn;
        h_hook.h_Entry = entry.raw;
    }

    h_calls        = 0;
    h_stamps       = 0;
    h_reenter      = 0;
    h_hook_result  = 0;
    h_last_text[0] = '\0';

    bsd_log_hook_init();
}

static VOID test_set_get(VOID)
{
    printf("one hook for the machine, set and read back\n");
    reset();

    CHECK(bsd_log_hook_get() == NULL, "none after init");
    CHECK(!bsd_log_hook_installed(), "and none is installed");

    CHECK(bsd_log_hook_set(&h_base_a, &h_hook), "a hook with an entry is taken");
    CHECK(bsd_log_hook_get() == &h_hook, "and read back");
    CHECK(bsd_log_hook_installed(), "installed");

    CHECK(!bsd_log_hook_set(&h_base_a, &h_blank),
          "a hook with no h_Entry is refused");
    CHECK(bsd_log_hook_get() == &h_hook, "and the one there stays");

    CHECK(bsd_log_hook_set(&h_base_a, NULL), "NULL clears it");
    CHECK(bsd_log_hook_get() == NULL && !bsd_log_hook_installed(),
          "and nothing is installed");
    CHECK(h_exec.TDNestCnt == -1, "every Forbid() was matched");
}

static VOID test_deliver(VOID)
{
    printf("what the hook is handed\n");
    reset();
    bsd_log_hook_set(&h_base_a, &h_hook);

    bsd_log_hook_deliver(LOG_USER | LOG_NOTICE, (STRPTR)"fetch", 0x1234UL,
                         "hello");

    CHECK(h_calls == 1, "called once");
    CHECK(h_last.lhm_Size == (LONG)sizeof(struct LogHookMessage),
          "lhm_Size is the structure's");
    CHECK(h_last.lhm_Priority == (LOG_USER | LOG_NOTICE),
          "the priority as given, facility and all");
    CHECK(h_last.lhm_Tag != NULL && strcmp((const char *)h_last.lhm_Tag,
                                           "fetch") == 0,
          "the tag");
    CHECK(h_last.lhm_ID == 0x1234UL, "the identifier");
    CHECK(strcmp(h_last_text, "hello") == 0, "the message");
    CHECK(h_stamps == 1 && h_last.lhm_Date.ds_Days == 20000 &&
          h_last.lhm_Date.ds_Minute == 61 && h_last.lhm_Date.ds_Tick == 7,
          "a Process gets a DateStamp");

    /* A Task, a NetX thread: dos.library is not called for it. */
    h_exec.ThisTask = &h_thread;
    bsd_log_hook_deliver(LOG_KERN | LOG_INFO, NULL, 0UL, "from a thread");
    CHECK(h_calls == 2, "a Task's line is delivered");
    CHECK(h_stamps == 1, "with no DateStamp() call");
    CHECK(h_last.lhm_Date.ds_Days == 0 && h_last.lhm_Date.ds_Minute == 0 &&
          h_last.lhm_Date.ds_Tick == 0, "and a zero date");
    CHECK(h_last.lhm_Tag == NULL && h_last.lhm_ID == 0UL,
          "no tag and no identifier, as the stack's own lines have");

    /* No dos.library: the same. */
    h_exec.ThisTask = &h_process;
    DOSBase = NULL;
    bsd_log_hook_deliver(LOG_INFO, NULL, 0UL, "no dos");
    CHECK(h_calls == 3 && h_stamps == 1 && h_last.lhm_Date.ds_Days == 0,
          "no dos.library, no date, still delivered");

    bsd_log_hook_deliver(LOG_INFO, NULL, 0UL, NULL);
    CHECK(h_calls == 3, "a NULL message is not delivered");
    CHECK(h_exec.TDNestCnt == -1, "every Forbid() was matched");
}

static VOID test_contexts(VOID)
{
    printf("not under Forbid(), not under Disable(), not on the tick\n");
    reset();
    bsd_log_hook_set(&h_base_a, &h_hook);

    h_exec.TDNestCnt = 0;
    bsd_log_hook_deliver(LOG_INFO, NULL, 0UL, "forbidden");
    CHECK(h_calls == 0, "under Forbid() nothing is delivered");
    h_exec.TDNestCnt = -1;

    h_exec.IDNestCnt = 0;
    bsd_log_hook_deliver(LOG_INFO, NULL, 0UL, "disabled");
    CHECK(h_calls == 0, "under Disable() nothing is delivered");
    h_exec.IDNestCnt = -1;

    h_exec.ThisTask = &h_tick;
    bsd_log_hook_deliver(LOG_INFO, NULL, 0UL, "tick");
    CHECK(h_calls == 0, "on the port's timer task nothing is delivered");
    h_exec.ThisTask = &h_process;

    bsd_log_hook_deliver(LOG_INFO, NULL, 0UL, "clear");
    CHECK(h_calls == 1, "and on a plain task it is");

    /* The renderer takes the same road. */
    h_exec.ThisTask = &h_tick;
    bsd_log_hook_emit(LOG_INFO, NULL, 0UL, "tick %ld", NULL);
    CHECK(h_calls == 1, "nor is anything rendered on the tick");
    h_exec.ThisTask = &h_process;
}

static VOID test_reentry(VOID)
{
    printf("a hook that logs is not re-entered\n");
    reset();
    bsd_log_hook_set(&h_base_a, &h_hook);

    h_reenter = 1;
    bsd_log_hook_deliver(LOG_INFO, NULL, 0UL, "outer");
    CHECK(h_calls == 1, "the inner line is dropped");
    CHECK(strcmp(h_last_text, "outer") == 0, "and the outer one was the call");

    bsd_log_hook_deliver(LOG_INFO, NULL, 0UL, "after");
    CHECK(h_calls == 2, "the flag is clear once the hook returns");
    CHECK(h_exec.TDNestCnt == -1, "every Forbid() was matched");
}

static VOID test_owner(VOID)
{
    printf("the base that set the hook takes it with it\n");
    reset();
    bsd_log_hook_set(&h_base_a, &h_hook);

    bsd_log_hook_drop_owner(&h_base_b);
    CHECK(bsd_log_hook_get() == &h_hook, "another base closing changes nothing");

    bsd_log_hook_drop_owner(&h_base_a);
    CHECK(bsd_log_hook_get() == NULL, "the owner closing clears the hook");

    bsd_log_hook_deliver(LOG_INFO, NULL, 0UL, "gone");
    CHECK(h_calls == 0, "and nothing is called into freed memory");

    /* Re-set by another base, then cleared by the first: the first no longer
       owns anything. */
    bsd_log_hook_set(&h_base_b, &h_hook);
    bsd_log_hook_drop_owner(&h_base_a);
    CHECK(bsd_log_hook_get() == &h_hook, "the previous owner is not remembered");
    bsd_log_hook_drop_owner(&h_base_b);
    CHECK(bsd_log_hook_get() == NULL, "the present one is");
    CHECK(h_exec.TDNestCnt == -1, "every Forbid() was matched");
}

static VOID test_emit(VOID)
{
    char long_fmt[400];

    printf("the rendered form is bounded\n");
    reset();

    bsd_log_hook_emit(LOG_ERR, (STRPTR)"tag", 1UL, "no hook", NULL);
    CHECK(h_calls == 0, "nothing is rendered with no hook installed");

    bsd_log_hook_set(&h_base_a, &h_hook);
    bsd_log_hook_emit(LOG_USER | LOG_ERR, (STRPTR)"tag", 1UL,
                      "open %s: failed", NULL);
    CHECK(h_calls == 1, "rendered and delivered");
    CHECK(strcmp(h_last_text, "open %s: failed") == 0,
          "the format went through the formatter as given");
    CHECK(h_last.lhm_Priority == (LOG_USER | LOG_ERR) &&
          strcmp((const char *)h_last.lhm_Tag, "tag") == 0 &&
          h_last.lhm_ID == 1UL,
          "with the priority, tag and identifier");

    memset(long_fmt, 'x', sizeof(long_fmt) - 1);
    long_fmt[sizeof(long_fmt) - 1] = '\0';
    bsd_log_hook_emit(LOG_INFO, NULL, 0UL, long_fmt, NULL);
    CHECK(h_calls == 2, "a long line is delivered");
    CHECK(strlen(h_last_text) == 255, "cut to the buffer, NUL-terminated");

    bsd_log_hook_emit(LOG_INFO, NULL, 0UL, NULL, NULL);
    CHECK(h_calls == 2, "a NULL format is not rendered");
}

static VOID test_exit(VOID)
{
    printf("expunge clears the hook\n");
    reset();
    bsd_log_hook_set(&h_base_a, &h_hook);

    bsd_log_hook_exit();
    CHECK(bsd_log_hook_get() == NULL, "gone");
    bsd_log_hook_drop_owner(&h_base_a);
    CHECK(bsd_log_hook_get() == NULL, "and the owner with it");
}

int main(void)
{
    test_set_get();
    test_deliver();
    test_contexts();
    test_reentry();
    test_owner();
    test_emit();
    test_exit();

    printf("loghook_host checks=%lu failures=%lu\n", h_checks, h_failures);
    return h_failures == 0 ? 0 : 1;
}

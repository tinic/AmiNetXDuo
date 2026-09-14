/*
 * src/bsdsocket/syslog.c on the host.
 *
 * The public LVO receives RawDoFmt's packed argument stream, not the host's
 * va_list.  This test therefore records the final format and asserts that the
 * stream is passed through untouched; trying to render it with host printf
 * would test a different ABI.
 *
 * SPDX-License-Identifier: MIT
 */

#include "bsdsocket_vectors.h"

#include <stdio.h>
#include <stdlib.h>
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

static struct AmiSocketBase h_base;
static struct ExecBase h_exec;
static struct Task h_task;
static ULONG h_args[] = { 0x11111111UL, 0x22222222UL };

static int h_emits;
static int h_level;
static const void *h_seen_args;
static char h_format[1200];
static BOOL h_fail_alloc;
static unsigned long h_allocs;
static unsigned long h_frees;

APTR ami_alloc(ULONG size)
{
    if (h_fail_alloc)
        return NULL;
    h_allocs++;
    return calloc(1, (size_t)size);
}

VOID ami_free(APTR ptr)
{
    h_frees++;
    free(ptr);
}

const char *bsd_errno_string(LONG code)
{
    return code == AMI_EIO ? "Input/output error" : "Unknown error";
}

/* vsyslog() gates the sink on the runtime level as well as the build option;
   this test wants every priority through. */
int ami_log_level(VOID)
{
    return AMI_LOG_TRACE;
}

VOID ami_serial_logv(int level, const char *fmt, const void *args)
{
    h_emits++;
    h_level = level;
    h_seen_args = args;
    snprintf(h_format, sizeof(h_format), "%s", fmt);
}

/* loghook.c: whether a hook is installed, and what vsyslog() hands it. */
static BOOL        h_hook_on;
static int         h_hook_emits;
static LONG        h_hook_priority;
static const char *h_hook_tag;
static ULONG       h_hook_id;
static char        h_hook_fmt[1200];
static const void *h_hook_args;

BOOL bsd_log_hook_installed(VOID)
{
    return h_hook_on;
}

VOID bsd_log_hook_emit(LONG priority, STRPTR tag, ULONG id, const char *fmt,
                       APTR args)
{
    h_hook_emits++;
    h_hook_priority = priority;
    h_hook_tag      = (const char *)tag;
    h_hook_id       = id;
    h_hook_args     = args;
    snprintf(h_hook_fmt, sizeof(h_hook_fmt), "%s", fmt);
}

static VOID reset_sink(VOID)
{
    h_emits = 0;
    h_level = -1;
    h_seen_args = NULL;
    h_format[0] = '\0';
    h_fail_alloc = FALSE;
    h_hook_on = FALSE;
    h_hook_emits = 0;
    h_hook_priority = -1;
    h_hook_tag = NULL;
    h_hook_id = 0;
    h_hook_fmt[0] = '\0';
    h_hook_args = NULL;
}

static VOID reset_base(VOID)
{
    memset(&h_base, 0, sizeof(h_base));
    memset(&h_exec, 0, sizeof(h_exec));
    memset(&h_task, 0, sizeof(h_task));
    h_exec.ThisTask = &h_task;
    h_base.sb_SysBase = &h_exec;
    h_base.sb_Task = &h_task;
    h_base.sb_LogFacility = LOG_USER;
    h_base.sb_LogMask = 0xff;
    h_base.sb_Errno = AMI_EIO;
    reset_sink();
}

static VOID call_log(LONG priority, const char *fmt)
{
    bsd_vsyslog(priority, (STRPTR)fmt, (APTR)h_args, &h_base);
}

static VOID test_format_and_errno(VOID)
{
    printf("tag, arguments and %%m\n");
    reset_base();
    h_base.sb_LogTag = (STRPTR)"fetch";

    call_log(LOG_INFO, "open %s: %m");

    CHECK(h_emits == 1, "an enabled priority is emitted");
    CHECK(h_level == AMI_LOG_INFO, "LOG_INFO maps to the info sink");
    CHECK(strcmp(h_format, "fetch: open %s: Input/output error") == 0,
          "%m expands without consuming a RawDoFmt argument");
    CHECK(h_seen_args == (const void *)h_args,
          "the caller's packed argument stream is unchanged");
    CHECK(h_allocs == h_frees, "the temporary format is freed");
}

static VOID test_tag_is_data(VOID)
{
    printf("the log tag is not a format string\n");
    reset_base();
    h_base.sb_LogTag = (STRPTR)"fetch%20%s";

    call_log(LOG_NOTICE, "done %%m %m");

    CHECK(strcmp(h_format,
                 "fetch%%20%%s: done %%m Input/output error") == 0,
          "percent signs in the tag are quoted and %%m stays literal");
}

static VOID test_pid(VOID)
{
    printf("LOG_PID identifies the calling task\n");
    reset_base();
    h_exec.ThisTask = (struct Task *)(unsigned long)0x1234UL;
    h_base.sb_LogTag = (STRPTR)"inetd";
    h_base.sb_LogStat = LOG_PID;

    call_log(LOG_DAEMON | LOG_WARNING, "child failed");

    CHECK(strcmp(h_format, "inetd[1234]: child failed") == 0,
          "the current task address is the AmiTCP process identifier");
    CHECK(h_level == AMI_LOG_WARN, "LOG_WARNING maps to warning");
}

static VOID test_mask_and_priority(VOID)
{
    unsigned long before;

    printf("priority validation and the per-opener mask\n");
    reset_base();
    h_base.sb_LogMask = LOG_MASK(LOG_ERR);
    before = h_allocs;

    call_log(LOG_INFO, "hidden");
    CHECK(h_emits == 0, "a masked priority is silent");
    CHECK(h_allocs == before, "a masked message allocates nothing");

    call_log(LOG_ERR, "visible");
    CHECK(h_emits == 1 && h_level == AMI_LOG_ERROR,
          "an enabled error is emitted");

    reset_sink();
    h_base.sb_LogMask = 0xff;
    call_log(LOG_EMERG, "emergency");
    CHECK(h_emits == 1 && h_level == AMI_LOG_ERROR,
          "priority zero is the valid LOG_EMERG priority");

    reset_sink();
    before = h_allocs;
    call_log(0x40000000L | LOG_ERR, "invalid");
    CHECK(h_emits == 0, "unknown priority bits are rejected");
    CHECK(h_allocs == before, "an invalid priority allocates nothing");
}

static VOID test_low_memory_fallback(VOID)
{
    char long_message[300];

    printf("the low-memory fallback remains bounded\n");
    reset_base();
    memset(long_message, 'x', sizeof(long_message) - 1);
    long_message[sizeof(long_message) - 1] = '\0';
    h_base.sb_LogTag = (STRPTR)"worker";
    h_fail_alloc = TRUE;

    call_log(LOG_DEBUG, long_message);

    CHECK(h_emits == 1, "allocation failure still emits a message");
    CHECK(strlen(h_format) <= 127, "the fallback is NUL-terminated and bounded");
    CHECK(strncmp(h_format, "worker: ", 8) == 0,
          "the fallback retains the caller identity");
}

static VOID test_null_format(VOID)
{
    unsigned long before;

    printf("a bad format pointer is refused before allocation\n");
    reset_base();
    before = h_allocs;
    bsd_vsyslog(LOG_ERR, NULL, (APTR)h_args, &h_base);
    CHECK(h_emits == 0, "NULL format is silent");
    CHECK(h_allocs == before, "NULL format allocates nothing");
}

static VOID test_log_hook(VOID)
{
    printf("SBTC_LOG_HOOK gets the caller's text, the tag and the task apart\n");
    reset_base();
    h_base.sb_LogTag  = (STRPTR)"fetch";
    h_base.sb_LogStat = LOG_PID;
    h_exec.ThisTask   = (struct Task *)(unsigned long)0x1234UL;
    h_hook_on = TRUE;

    call_log(LOG_INFO, "open %s: %m");

    CHECK(h_hook_emits == 1, "an accepted message reaches the hook");
    CHECK(h_hook_priority == (LOG_USER | LOG_INFO),
          "with the opener's facility filled in");
    CHECK(h_hook_tag == (const char *)h_base.sb_LogTag,
          "the tag goes as itself, for lhm_Tag");
    CHECK(h_hook_id == 0x1234UL,
          "the task address is the identifier, the number LOG_PID prints");
    CHECK(strcmp(h_hook_fmt, "open %s: Input/output error") == 0,
          "the text is the caller's with %m expanded and no prefix");
    CHECK(h_hook_args == (const void *)h_args,
          "and the caller's argument stream goes with it");
    CHECK(strcmp(h_format, "fetch[1234]: open %s: Input/output error") == 0,
          "the serial line keeps its prefix");

    reset_sink();
    h_hook_on = TRUE;
    h_base.sb_LogMask = LOG_MASK(LOG_ERR);
    call_log(LOG_INFO, "hidden");
    CHECK(h_hook_emits == 0, "a masked priority does not reach the hook");

    reset_sink();
    call_log(LOG_ERR, "no hook");
    CHECK(h_hook_emits == 0 && h_emits == 1,
          "no hook installed: nothing is rendered for one, the sink still "
          "gets the line");
}

int main(void)
{
    test_format_and_errno();
    test_tag_is_data();
    test_pid();
    test_mask_and_priority();
    test_low_memory_fallback();
    test_null_format();
    test_log_hook();

    printf("syslog_host checks=%lu failures=%lu\n", h_checks, h_failures);
    return h_failures == 0 ? 0 : 1;
}

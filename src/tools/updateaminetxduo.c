/*
 * UpdateAmiNetXDuo, the updater (#497).
 *
 *     UpdateAmiNetXDuo CHECK/S,INSTALL/S,BOOT/S,CONFIRM/S,QUIET/S,ANSWER/K,
 *                      RESTART/S
 *
 * The contract, the return codes and every key=value line are in the header
 * of tests/updater/scenarios.tsv.  This is its CHECK: the settings, the
 * clock, the channel's update.manifest and its signature from the mirror,
 * and the version against the installed one.  CHECK writes nothing but its
 * log.  INSTALL, BOOT and CONFIRM, and the INSTALL and SKIP answers, are
 * refused as not implemented until their part of #497 lands.
 *
 *   rc 0   update=current, update=off, update=foreign_stack
 *      5   update=available (and update=later with ANSWER=LATER)
 *     10   refused=<why>; nothing changed
 *
 * Every line also goes to AmiNetXDuo:Installed/Update/log, when that drawer
 * exists: CHECK creates nothing.
 *
 * SPDX-License-Identifier: MIT
 */

#include "toolsock.h"

#include <exec/memory.h>
#include <exec/tasks.h>
#include <dos/dos.h>
#include <dos/dosextens.h>

#include "aminetxduo/version.h"

#include "fetchcore.h"
#include "updcheck.h"
#include "updkeys.h"

const char *const tool_name = "UpdateAmiNetXDuo";

static const char version_tag[] __attribute__((used)) =
    TOOL_VERSTAG("UpdateAmiNetXDuo");

#define TEMPLATE    "CHECK/S,INSTALL/S,BOOT/S,CONFIRM/S,QUIET/S,ANSWER/K," \
                    "RESTART/S"

enum
{
    ARG_CHECK = 0,
    ARG_INSTALL,
    ARG_BOOT,
    ARG_CONFIRM,
    ARG_QUIET,
    ARG_ANSWER,
    ARG_RESTART,
    ARG_COUNT
};

enum { ANSWER_NONE = 0, ANSWER_INSTALL, ANSWER_LATER, ANSWER_SKIP };

#define UPD_RC_AVAILABLE    5
#define UPD_RC_REFUSED      10

/* Placeholder until the mirror exists (#497 Q1/Q2): .invalid never
   resolves.  A build with no release key refuses before it is used. */
#define UPD_DEFAULT_MIRROR  "http://update.aminetxduo.invalid/"
#define UPD_NTP_SERVER      "pool.ntp.org"
#define UPD_TIMEOUT         60UL            /* seconds, per fetch          */
#define UPD_MANIFEST_MAX    (64UL * 1024UL)
#define UPD_SIG_MAX         1024UL
#define UPD_FILES_MAX       160
#define UPD_SETTINGS_MAX    1024
#define UPD_LOG_DIR         "AmiNetXDuo:Installed/Update"
#define UPD_LOG_FILE        "AmiNetXDuo:Installed/Update/log"

/* ------------------------------------------------------------ the lines --- */

/* What this run said, for the log.  A line that does not fit is still
   printed; the log gets the lines before it. */
static char  upd_said[2048];
static ULONG upd_said_len;

static VOID say_part(const char *s)
{
    ULONG n = 0;

    while (s[n] != '\0')
        n++;
    if (upd_said_len + n < sizeof(upd_said))
    {
        CopyMem((APTR)s, upd_said + upd_said_len, n);
        upd_said_len += n;
    }
    tool_printf("%s", (LONG)s);
}

/* One key=value line, from up to three parts. */
static VOID say(const char *a, const char *b, const char *c)
{
    say_part(a);
    if (b != NULL)
        say_part(b);
    if (c != NULL)
        say_part(c);
    say_part("\n");
}

static VOID upd_log_write(const char *word)
{
    struct Process *me = (struct Process *)FindTask(NULL);
    APTR            saved = me->pr_WindowPtr;
    struct DateStamp ds;
    char            date[11];
    BPTR            lock, fh;

    if (upd_said_len == 0)
        return;

    /* No "insert volume AmiNetXDuo:" requester without the assign.  The
       drawer is the installer's to make: CHECK creates nothing. */
    me->pr_WindowPtr = (APTR)-1L;
    lock = Lock((CONST_STRPTR)UPD_LOG_DIR, SHARED_LOCK);
    if (lock != (BPTR)0)
    {
        UnLock(lock);

        fh = Open((CONST_STRPTR)UPD_LOG_FILE, MODE_READWRITE);
        if (fh != (BPTR)0)
        {
            (VOID)Seek(fh, 0, OFFSET_END);
            DateStamp(&ds);
            upd_date((unsigned long)ds.ds_Days, date);
            (VOID)FPuts(fh, (CONST_STRPTR)"run=");
            (VOID)FPuts(fh, (CONST_STRPTR)word);
            (VOID)FPuts(fh, (CONST_STRPTR)" date=");
            (VOID)FPuts(fh, (CONST_STRPTR)date);
            (VOID)FPuts(fh, (CONST_STRPTR)" version=" AMINETXDUO_VERSION "\n");
            (VOID)Flush(fh);
            (VOID)Write(fh, upd_said, (LONG)upd_said_len);
            Close(fh);
        }
    }
    me->pr_WindowPtr = saved;
}

/* ------------------------------------------------------------ the fetch --- */

typedef struct UpdBuf
{
    UBYTE *data;
    ULONG  len;
    ULONG  size;
    ULONG  cap;
} UpdBuf;

static BOOL ub_write(APTR ctx, const UBYTE *d, LONG n)
{
    UpdBuf *b = (UpdBuf *)ctx;

    if (n <= 0)
        return TRUE;

    if (b->len + (ULONG)n > b->cap)
    {
        tool_error("the answer is larger than %lu bytes", b->cap);
        return FALSE;
    }

    if (b->len + (ULONG)n > b->size)
    {
        ULONG  size = b->size ? b->size : 4096UL;
        UBYTE *grown;

        while (size < b->len + (ULONG)n)
            size *= 2;
        if (size > b->cap)
            size = b->cap;

        grown = (UBYTE *)AllocVec(size, MEMF_ANY);
        if (grown == NULL)
        {
            tool_fault(ERROR_NO_FREE_STORE);
            return FALSE;
        }
        if (b->data != NULL)
        {
            CopyMem(b->data, grown, b->len);
            FreeVec(b->data);
        }
        b->data = grown;
        b->size = size;
    }

    CopyMem((APTR)d, b->data + b->len, (ULONG)n);
    b->len += (ULONG)n;

    return TRUE;
}

static VOID ub_free(UpdBuf *b)
{
    if (b->data != NULL)
        FreeVec(b->data);
    b->data = NULL;
    b->len = b->size = 0;
}

static FetchCore upd_fc;

/* 0 got it whole with a 200; the HTTP status otherwise; -1 no answer. */
static LONG upd_fetch(const char *url, UpdBuf *b)
{
    FetchUrlResult why = fetch_url_parse(url, &upd_fc.url);

    if (why != FETCH_URL_OK)
    {
        tool_error("\"%s\": %s", (LONG)url, (LONG)fetch_url_error(why));
        return -1;
    }

    upd_fc.timeout    = UPD_TIMEOUT;
    upd_fc.family     = TOOL_AF_UNSPEC;
    upd_fc.headers    = FALSE;
    upd_fc.quiet      = TRUE;
    upd_fc.noverify   = FALSE;
    upd_fc.sink.open  = NULL;
    upd_fc.sink.write = ub_write;
    upd_fc.sink.ctx   = b;
    b->len = 0;

    if (fetch_core_run(&upd_fc) != RETURN_OK)
        return -1;
    if (upd_fc.status != 200)
    {
        tool_error("%s: HTTP %ld", (LONG)url, upd_fc.status);
        return upd_fc.status > 0 ? upd_fc.status : -1;
    }

    return 0;
}

/* ------------------------------------------------------- what is here --- */

static char upd_settings_text[UPD_SETTINGS_MAX];

static VOID upd_read_settings(UpdSettings *s)
{
    static const char *const where[] = {
        "ENV:AmiNetXDuo/Update", "ENVARC:AmiNetXDuo/Update"
    };
    struct Process *me = (struct Process *)FindTask(NULL);
    APTR            saved = me->pr_WindowPtr;
    LONG            n = 0;
    ULONG           i;

    me->pr_WindowPtr = (APTR)-1L;
    for (i = 0; i < 2; i++)
    {
        BPTR fh = Open((CONST_STRPTR)where[i], MODE_OLDFILE);

        if (fh == (BPTR)0)
            continue;
        n = Read(fh, upd_settings_text, (LONG)sizeof(upd_settings_text));
        Close(fh);
        if (n < 0)
            n = 0;
        break;
    }
    me->pr_WindowPtr = saved;

    upd_settings_parse(upd_settings_text, (unsigned long)n, s);
}

static char upd_id[160];

/*
 * The flavour from the stack in memory, opened from LIBS: when it is not.
 * -2 when that stack is not ours, -1 when it says no flavour, -3 when there
 * is no bsdsocket.library at all.  *base is what to close.
 */
static int upd_installed_flavour(struct Library **base)
{
    struct Library *lib;
    int             ours = 0, found = 0;

    *base = NULL;
    Forbid();
    lib = (struct Library *)FindName(&SysBase->LibList,
                                     (CONST_STRPTR)"bsdsocket.library");
    if (lib != NULL)
    {
        found = 1;
        ours = tool_stack_is_ours(lib) ? 1 : 0;
        tool_copy_string(upd_id, sizeof(upd_id),
                         lib->lib_IdString ? (const char *)lib->lib_IdString : "");
    }
    Permit();

    if (!found)
    {
        lib = OpenLibrary((CONST_STRPTR)"bsdsocket.library", 4UL);
        if (lib == NULL)
            return -3;
        *base = lib;
        ours = tool_stack_is_ours(lib) ? 1 : 0;
        tool_copy_string(upd_id, sizeof(upd_id),
                         lib->lib_IdString ? (const char *)lib->lib_IdString : "");
    }

    if (!ours)
        return -2;

    return upd_flavour_from_id(upd_id);
}

/* ------------------------------------------------------------ the check --- */

typedef struct UpdRun
{
    BOOL  quiet;
    LONG  answer;
    char  word[8];
} UpdRun;

static UpdRun      upd_run_args;
static UpdSettings upd_set;
static UpdBuf      upd_manifest_buf, upd_sig_buf;
static char        upd_url_buf[UPD_MIRROR_MAX + 48];
static char        upd_ver_text[UPD_VER_MAX];

static LONG refuse(const char *why, const char *hint)
{
    say("refused=", why, NULL);
    if (hint != NULL)
        tool_hint("%s", (LONG)hint);
    return UPD_RC_REFUSED;
}

/* The clock, set by sntp when it was never set. */
static BOOL upd_clock(VOID)
{
    struct DateStamp ds;
    LONG             rc;

    DateStamp(&ds);
    if ((ULONG)ds.ds_Days >= UPD_CLOCK_FLOOR_DAYS)
        return TRUE;

    rc = tool_run_companion("sntp", UPD_NTP_SERVER, "TIMEOUT 10 QUIET", NULL);
    DateStamp(&ds);
    if (rc == RETURN_OK && (ULONG)ds.ds_Days >= UPD_CLOCK_FLOOR_DAYS)
    {
        say("sntp=ok", NULL, NULL);
        return TRUE;
    }

    say("sntp=failed", NULL, NULL);
    return FALSE;
}

static LONG upd_check(VOID)
{
    struct Library *base = NULL;
    UpdWant         want;
    UpdManifest     m;
    UpdFile        *files = NULL;
    UpdVerdict      v;
    int             flavour;
    LONG            got, rc;
    const char     *mirror;

    upd_read_settings(&upd_set);
    if (upd_run_args.quiet && !upd_set.auto_check)
    {
        say("update=off", NULL, NULL);
        return RETURN_OK;
    }

    if (!upd_ver_parse(AMINETXDUO_VERSION,
                       (unsigned long)(sizeof(AMINETXDUO_VERSION) - 1),
                       &want.installed))
        return refuse("manifest", "this build's own version does not parse");

    flavour = upd_installed_flavour(&base);
    if (flavour == -2)
    {
        if (base != NULL)
            CloseLibrary(base);
        say("update=foreign_stack", NULL, NULL);
        return RETURN_OK;
    }
    if (flavour == -3)
        return refuse("network", "no bsdsocket.library");
    if (flavour < 0)
    {
        if (base != NULL)
            CloseLibrary(base);
        return refuse("no_receipt", "which flavour is installed is unknown: "
                                    "run Install-AmiNetXDuo");
    }

    want.flavour = (unsigned char)flavour;
    want.channel = (unsigned char)(upd_set.channel >= 0
                       ? upd_set.channel
                       : (want.installed.pre[0] != '\0' ? UPD_CHANNEL_PRERELEASE
                                                        : UPD_CHANNEL_RELEASE));

    if (!upd_clock())
    {
        if (base != NULL)
            CloseLibrary(base);
        return refuse("clock", "the clock is not set: run sntp, or set it "
                               "with Date");
    }

    mirror = upd_set.mirror[0] != '\0' ? upd_set.mirror : UPD_DEFAULT_MIRROR;

    rc = UPD_RC_REFUSED;
    upd_manifest_buf.cap = UPD_MANIFEST_MAX;
    upd_sig_buf.cap      = UPD_SIG_MAX;

    if (!upd_url(upd_url_buf, sizeof(upd_url_buf), mirror, want.channel,
                 "update.manifest") ||
        upd_fetch(upd_url_buf, &upd_manifest_buf) != 0)
    {
        rc = refuse("network", NULL);
        goto done;
    }

    if (!upd_url(upd_url_buf, sizeof(upd_url_buf), mirror, want.channel,
                 "update.manifest.sig"))
    {
        rc = refuse("network", NULL);
        goto done;
    }
    got = upd_fetch(upd_url_buf, &upd_sig_buf);
    if (got == 404)
    {
        /* The manifest is there and its signature is not: unsigned. */
        rc = refuse("signature", "the mirror has no update.manifest.sig");
        goto done;
    }
    if (got != 0)
    {
        rc = refuse("network", NULL);
        goto done;
    }

    files = (UpdFile *)AllocVec((ULONG)(UPD_FILES_MAX * sizeof(UpdFile)),
                                MEMF_ANY);
    if (files == NULL)
    {
        tool_fault(ERROR_NO_FREE_STORE);
        rc = refuse("manifest", NULL);
        goto done;
    }

    v = upd_check_verdict(upd_manifest_buf.data, upd_manifest_buf.len,
                          (const char *)upd_sig_buf.data, upd_sig_buf.len,
                          upd_keys, upd_key_count, &want, &m,
                          files, UPD_FILES_MAX, NULL);

    switch (v)
    {
    case UPD_V_CURRENT:
        upd_ver_format(&want.installed, upd_ver_text);
        say("update=current version=", upd_ver_text, NULL);
        rc = RETURN_OK;
        break;

    case UPD_V_AVAILABLE:
        upd_ver_format(&m.version, upd_ver_text);
        say("update=available version=", upd_ver_text, NULL);
        rc = UPD_RC_AVAILABLE;
        if (upd_run_args.answer != ANSWER_NONE)
        {
            say("requester=offer", NULL, NULL);
            if (upd_run_args.answer == ANSWER_LATER)
                say("update=later version=", upd_ver_text, NULL);
            else
                rc = refuse("not_implemented", "only ANSWER=LATER is "
                                               "implemented yet");
        }
        break;

    case UPD_V_UNKNOWN_KEY:
        rc = refuse("unknown_key", "no signature is by a key this build "
                                   "knows");
        break;

    case UPD_V_SIGNATURE:
        rc = refuse("signature", "update.manifest does not match its "
                                 "signature");
        break;

    case UPD_V_MANIFEST:
        rc = refuse("manifest", NULL);
        break;
    }

done:
    if (files != NULL)
        FreeVec(files);
    ub_free(&upd_manifest_buf);
    ub_free(&upd_sig_buf);
    if (base != NULL)
        CloseLibrary(base);

    return rc;
}

static LONG upd_main(VOID)
{
    /* Another stack is active: nothing at all, not even the check. */
    if (tool_stack_foreign_loaded())
    {
        say("update=foreign_stack", NULL, NULL);
        return RETURN_OK;
    }

    if (upd_run_args.word[0] != 'C' || upd_run_args.word[1] != 'H')
        return refuse("not_implemented", "only CHECK is implemented yet");

    /* Fail safe: a build that holds no key accepts nothing. */
    if (upd_key_count == 0)
        return refuse("unknown_key", "this build holds no release key");

    return upd_check();
}

/*
 * Hazard: upd_trampoline() must have no locals and no arguments, and must
 * stay noinline; see fetch.c.  16 KB covers http: and the Ed25519 check;
 * https: (tls.library) gets fetch's 64 KB.
 */
static struct StackSwapStruct upd_sss;
static LONG                   upd_result;

static __attribute__((noinline)) VOID upd_trampoline(VOID)
{
    StackSwap(&upd_sss);
    upd_result = upd_main();
    StackSwap(&upd_sss);
}

static LONG upd_answer(const char *s)
{
    static const char *const words[] = { "INSTALL", "LATER", "SKIP" };
    ULONG w, i;

    for (w = 0; w < 3; w++)
    {
        for (i = 0; words[w][i] != '\0'; i++)
        {
            char c = s[i];

            if (c >= 'a' && c <= 'z')
                c = (char)(c - 'a' + 'A');
            if (c != words[w][i])
                break;
        }
        if (words[w][i] == '\0' && s[i] == '\0')
            return (LONG)(w + 1);
    }

    return -1;
}

int main(int argc, char **argv)
{
    LONG           args[ARG_COUNT];
    struct RDArgs *rda;
    APTR           stack;
    ULONG          stack_size;
    const char    *word;
    ULONG          i;

    (VOID)argv;

    if (tool_from_workbench(argc))
        return RETURN_FAIL;

    tool_break_arm();

    for (i = 0; i < ARG_COUNT; i++)
        args[i] = 0;

    rda = ReadArgs((CONST_STRPTR)TEMPLATE, args, NULL);
    if (rda == NULL)
    {
        tool_fault(IoErr());
        tool_usage("CHECK [QUIET] [ANSWER=INSTALL|LATER|SKIP]",
                   "Check the mirror for a newer AmiNetXDuo.  INSTALL, BOOT "
                   "and CONFIRM are the steps after it.");
        return RETURN_ERROR;
    }

    upd_run_args.quiet  = args[ARG_QUIET] != 0 ? TRUE : FALSE;
    upd_run_args.answer = ANSWER_NONE;
    if (args[ARG_ANSWER] != 0)
    {
        upd_run_args.answer = upd_answer((const char *)args[ARG_ANSWER]);
        if (upd_run_args.answer < 0)
        {
            tool_error("ANSWER is INSTALL, LATER or SKIP");
            FreeArgs(rda);
            return RETURN_ERROR;
        }
    }

    word = args[ARG_INSTALL] ? "INSTALL" : args[ARG_BOOT] ? "BOOT"
         : args[ARG_CONFIRM] ? "CONFIRM" : "CHECK";
    tool_copy_string(upd_run_args.word, sizeof(upd_run_args.word), word);
    FreeArgs(rda);

    if (upd_run_args.quiet)
        tool_quiet_begin();

    upd_result = RETURN_FAIL;
    stack_size = 16UL * 1024UL;
    {
        UpdSettings peek;

        /* The mirror decides the stack: tls.library wants fetch's. */
        upd_read_settings(&peek);
        if (peek.mirror[0] == 'h' && peek.mirror[4] == 's')
            stack_size = 64UL * 1024UL;
    }
    stack = AllocMem(stack_size, MEMF_ANY);
    if (stack != NULL)
    {
        upd_sss.stk_Lower   = stack;
        upd_sss.stk_Upper   = (ULONG)stack + stack_size;
        upd_sss.stk_Pointer = (APTR)((ULONG)stack + stack_size);

        upd_trampoline();

        FreeMem(stack, stack_size);
    }
    else
    {
        /* Too short of memory for a stack: http: fits on the caller's, as
           in fetch, so try rather than refuse. */
        upd_result = upd_main();
    }

    upd_log_write(upd_run_args.word);

    /* QUIET silences; it does not change the contract's return codes. */
    (VOID)tool_quiet_end(RETURN_OK);

    return (int)upd_result;
}

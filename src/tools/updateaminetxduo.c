/*
 * UpdateAmiNetXDuo, the updater (#497).
 *
 *     UpdateAmiNetXDuo CHECK/S,INSTALL/S,BOOT/S,CONFIRM/S,QUIET/S,ANSWER/K,
 *                      RESTART/S
 *
 * The contract, the return codes and every key=value line are in the header
 * of tests/updater/scenarios.tsv.  CHECK: the settings, the clock, the
 * channel's update.manifest and its signature from the mirror, and the
 * version against the installed one; it writes nothing but its log.
 * INSTALL (and CHECK ANSWER=INSTALL): CHECK, then the receipt's three-way
 * file selection, PREPARE (the container to the Update drawer, every
 * member to <live>.new, the journal) and SWAP (src/tools/updinstall.h).
 * BOOT, the first startup line: a journal finished or undone, the trial's
 * boots counted, and on the third boot without CONFIRM the update rolled
 * back.  CONFIRM, after S:Network-Startup: once an interface has an
 * address, the trial committed (the .old files, the receipt, the record
 * Uninstall-AmiNetXDuo runs).
 *
 * CHECK QUIET is the startup line, run in the background: nothing with
 * AUTO=OFF, at most one check a day (the state's last_check= and later=),
 * and an update offered with the Install / Later / Skip this version
 * requester once the Workbench screen is open.  Install installs it and
 * asks to restart; Later asks again the next day; Skip passes that version
 * by (skip=).  ANSWER= answers the requester without drawing it.
 *
 *   rc 0   update=current, update=off, update=notdue, update=foreign_stack,
 *          update=installed, update=skipped; BOOT and CONFIRM always, but
 *          for a journal they cannot read (5): a startup line never stops
 *          the startup
 *      5   update=available (and update=later with ANSWER=LATER)
 *     10   refused=<why>; nothing changed
 *     20   failed after the first live file was touched
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
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <proto/intuition.h>

#include "aminetxduo/version.h"

#include <string.h>

#include "c68k_sha256.h"
#include "fetchcore.h"
#include "tool_reboot.h"
#include "updcheck.h"
#include "updinstall.h"
#include "updkeys.h"
#include "updprefs.h"

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
#define UPD_PATH            300

/* The Update drawer, by its volume: upd_find_udir(). */
static char upd_udir[UPD_PATH];
static BOOL upd_find_udir(VOID);
static VOID upd_in_dir(char *out, const char *name);

/* ------------------------------------------------------------ the lines --- */

/* What this run said, for the log: whole lines, and truncated=yes after
   the last that fits (upd_said_add).  Every line is still printed. */
static char  upd_said[2048];
static ULONG upd_said_len;

/* One key=value line, from up to three parts. */
static VOID say(const char *a, const char *b, const char *c)
{
    upd_said_len = upd_said_add(upd_said, sizeof(upd_said), upd_said_len,
                                a, b, c);
    tool_printf("%s", (LONG)a);
    if (b != NULL)
        tool_printf("%s", (LONG)b);
    if (c != NULL)
        tool_printf("%s", (LONG)c);
    tool_printf("%s", (LONG)"\n");
}

static VOID upd_log_write(const char *word)
{
    struct Process *me = (struct Process *)FindTask(NULL);
    APTR            saved = me->pr_WindowPtr;
    struct DateStamp ds;
    char            date[11];
    BPTR            fh;
    static char     path[UPD_PATH];     /* main()'s, on the Shell's stack */

    if (upd_said_len == 0)
        return;

    /* No "insert volume AmiNetXDuo:" requester without the assign.  The
       drawer is the installer's to make: CHECK creates nothing. */
    me->pr_WindowPtr = (APTR)-1L;
    if (upd_find_udir())
    {
        upd_in_dir(path, UPD_N_LOG);
        fh = Open((CONST_STRPTR)path, MODE_READWRITE);
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
    BOOL   opened;              /* a final 2xx: the body is the answer    */
} UpdBuf;

/* Only a final 2xx opens the sink; an error status's body still reaches
   ub_write(), and is neither kept nor counted (upd_dl_chunk). */
static BOOL ub_open(APTR ctx)
{
    UpdBuf *b = (UpdBuf *)ctx;

    b->opened = TRUE;
    b->len = 0;
    return TRUE;
}

static BOOL ub_write(APTR ctx, const UBYTE *d, LONG n)
{
    UpdBuf *b = (UpdBuf *)ctx;

    if (n <= 0)
        return TRUE;

    switch (upd_dl_chunk(b->opened, b->len, (ULONG)n, b->cap))
    {
    case UPD_DL_OK:
        break;
    case UPD_DL_LONG:
        tool_error("the answer is larger than %lu bytes", b->cap);
        return FALSE;
    default:
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
    int            whole;

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
    upd_fc.sink.open  = ub_open;
    upd_fc.sink.write = ub_write;
    upd_fc.sink.ctx   = b;
    b->len = 0;
    b->opened = FALSE;

    /* The status first, as the container's (upd_fetch_result). */
    whole = fetch_core_run(&upd_fc) == RETURN_OK;
    if (upd_fc.status != 0 && upd_fc.status != 200)
        tool_error("%s: HTTP %ld", (LONG)url, upd_fc.status);
    return upd_fetch_result(upd_fc.status, whole);
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
    BOOL  restart;
    LONG  answer;
    char  word[8];
} UpdRun;

static UpdRun      upd_run_args;
static UpdSettings upd_set;
static UpdBuf      upd_manifest_buf, upd_sig_buf;
static char        upd_url_buf[UPD_MIRROR_MAX + 96];
static char        upd_ver_text[UPD_VER_MAX];

/* The last refusal, for the requester that says an install failed. */
static char upd_refused[48];

static LONG refuse(const char *why, const char *hint)
{
    say("refused=", why, NULL);
    tool_copy_string(upd_refused, sizeof(upd_refused), why);
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

/* ------------------------------------------------------------- install --- */

/*
 * INSTALL: PREPARE and SWAP (src/tools/updinstall.h has the rules, the
 * journal and the recovery).  Nothing live is touched before the journal
 * says "ready": every refusal before it removes what it staged and leaves
 * the machine as it was.
 */

#define UPD_RCPT_FILE_MAX   (32UL * 1024UL)
#define UPD_STATE_MAX       1024UL
#define UPD_VOL_MAX         8
#define UPD_VOL_MARGIN      16UL            /* blocks: journal, state, dirs */
#define UPD_RC_FAILED       20

typedef struct UpdPick
{
    unsigned char act;                  /* UpdAct                          */
    unsigned char why;                  /* UpdWhy                          */
    unsigned char vol;                  /* the live file's, upd_vols[]     */
    unsigned char has_old;              /* <live>.old exists               */
    unsigned char jn;                   /* its UpdInst.je[], when replaced */
    short         rcpt;                 /* receipt line, -1 none           */
    char         *real;                 /* the live file by its volume     */
    ULONG         old_size;
} UpdPick;

typedef struct UpdVol
{
    BPTR  node;                         /* id_VolumeNode                   */
    LONG  state;                        /* id_DiskState                    */
    ULONG free;                         /* blocks                          */
    ULONG bpb;
    ULONG need;
    char  name[64];                     /* a file on it, for the refusal   */
} UpdVol;

static UBYTE        upd_chunk[4096];
static C68K_SHA256  upd_sha;
static struct InfoData upd_info __attribute__((aligned(4)));
static UpdVol       upd_vols[UPD_VOL_MAX];
static ULONG        upd_nvols;
static char         upd_p1[UPD_PATH];
static char         upd_p2[UPD_PATH];
static char         upd_line[UPD_PATH + 64];

static UBYTE       *upd_rcpt_text;
static ULONG        upd_rcpt_len;
static UpdRcpt     *upd_rcpt;
static unsigned int upd_nrcpt;
static BOOL         upd_reboot;

/* s appended to b, which holds `cap` bytes; truncated, always ended. */
static VOID sb(char *b, ULONG cap, const char *s)
{
    ULONG n = 0;

    while (b[n] != '\0')
        n++;
    while (*s != '\0' && n + 1 < cap)
        b[n++] = *s++;
    b[n] = '\0';
}

static VOID sbn(char *b, ULONG cap, const char *s, ULONG len)
{
    ULONG n = 0, i;

    while (b[n] != '\0')
        n++;
    for (i = 0; i < len && n + 1 < cap; i++)
        b[n++] = s[i];
    b[n] = '\0';
}

static VOID sbu(char *b, ULONG cap, ULONG v)
{
    char  t[12];
    ULONG n = sizeof(t) - 1;

    t[n] = '\0';
    do
    {
        t[--n] = (char)('0' + v % 10);
        v /= 10;
    } while (v != 0);
    sb(b, cap, t + n);
}

/* upd_udir + "/" + name into out. */
static VOID upd_in_dir(char *out, const char *name)
{
    out[0] = '\0';
    sb(out, UPD_PATH, upd_udir);
    if (out[0] != '\0' && out[strlen(out) - 1] != ':')
        sb(out, UPD_PATH, "/");
    sb(out, UPD_PATH, name);
}

/* A whole file into memory, NUL-ended; NULL when it is not there, cannot
   be read or is larger than `cap`. */
static UBYTE *upd_slurp(const char *path, ULONG cap, ULONG *len)
{
    BPTR   fh = Open((CONST_STRPTR)path, MODE_OLDFILE);
    LONG   size;
    UBYTE *buf = NULL;

    *len = 0;
    if (fh == (BPTR)0)
        return NULL;
    (VOID)Seek(fh, 0, OFFSET_END);
    size = Seek(fh, 0, OFFSET_BEGINNING);
    if (size >= 0 && (ULONG)size <= cap)
    {
        buf = (UBYTE *)AllocVec((ULONG)size + 1, MEMF_ANY);
        if (buf != NULL && Read(fh, buf, size) != size)
        {
            FreeVec(buf);
            buf = NULL;
        }
        if (buf != NULL)
        {
            buf[size] = '\0';
            *len = (ULONG)size;
        }
    }
    Close(fh);
    return buf;
}

/* <dir>/Installed/Update, locked; 0 when it is not there. */
static BPTR upd_lock_udir(const char *dir, ULONG n)
{
    upd_p1[0] = '\0';
    sbn(upd_p1, UPD_PATH, dir, n);
    sb(upd_p1, UPD_PATH, upd_p1[strlen(upd_p1) - 1] == ':'
                         ? "Installed/Update" : "/Installed/Update");
    return Lock((CONST_STRPTR)upd_p1, SHARED_LOCK);
}

/*
 * The Update drawer: AmiNetXDuo:Installed/Update, or, in the system layout,
 * which assigns no AmiNetXDuo:, <DOCDIR>/Installed/Update with DOCDIR from
 * the docdir= line of S:AmiNetXDuo-Install, or from the
 * T:AmiNetXDuo-DrawerKept line of S:AmiNetXDuo-Files (the record
 * InstallSameFile DRAWER reads).  FALSE when there is none.  The caller has
 * the window pointer at -1.
 */
static BOOL upd_find_udir(VOID)
{
    static const char key[] = ">T:AmiNetXDuo-DrawerKept \"";
    static BOOL       done;
    BPTR              lock;
    UBYTE            *t;
    ULONG             len, i, j;

    if (done)
        return upd_udir[0] != '\0';
    done = TRUE;
    upd_udir[0] = '\0';

    lock = Lock((CONST_STRPTR)"AmiNetXDuo:Installed/Update", SHARED_LOCK);
    if (lock == (BPTR)0 &&
        (t = upd_slurp("S:AmiNetXDuo-Install", 4096UL, &len)) != NULL)
    {
        if (upd_state_get((const char *)t, len, "docdir", upd_p2, UPD_PATH - 20) &&
            upd_p2[0] != '\0')
            lock = upd_lock_udir(upd_p2, strlen(upd_p2));
        FreeVec(t);
    }
    if (lock == (BPTR)0)
    {
        t = upd_slurp("S:AmiNetXDuo-Files", 256UL * 1024UL, &len);
        if (t == NULL)
            return FALSE;
        for (i = 0; i + sizeof(key) - 1 <= len; i++)
            if (memcmp(t + i, key, sizeof(key) - 1) == 0)
            {
                i += sizeof(key) - 1;
                for (j = i; j < len && t[j] != '"' && t[j] != '\n'; j++)
                    ;
                if (j < len && t[j] == '"' && j > i && j - i < UPD_PATH - 20)
                    lock = upd_lock_udir((const char *)t + i, j - i);
                break;
            }
        FreeVec(t);
    }
    if (lock == (BPTR)0)
        return FALSE;
    if (!NameFromLock(lock, (STRPTR)upd_udir, sizeof(upd_udir)))
        upd_udir[0] = '\0';
    UnLock(lock);
    return upd_udir[0] != '\0';
}

static BOOL upd_write_file(const char *path, const char *data, ULONG len)
{
    BPTR fh = Open((CONST_STRPTR)path, MODE_NEWFILE);
    LONG wrote;

    if (fh == (BPTR)0)
        return FALSE;
    wrote = Write(fh, (APTR)data, (LONG)len);
    if (!Close(fh) || wrote != (LONG)len)
    {
        (VOID)DeleteFile((CONST_STRPTR)path);
        return FALSE;
    }
    return TRUE;
}

/* Gone, or was never there. */
static BOOL upd_remove(const char *path)
{
    if (DeleteFile((CONST_STRPTR)path))
        return TRUE;
    return IoErr() == ERROR_OBJECT_NOT_FOUND;
}

/* Not there, as opposed to there and unreadable. */
static BOOL upd_absent(const char *path)
{
    BPTR l = Lock((CONST_STRPTR)path, SHARED_LOCK);

    if (l != (BPTR)0)
    {
        UnLock(l);
        return FALSE;
    }
    return upd_ioerr_absent(IoErr()) ? TRUE : FALSE;
}

/* There, or it cannot be said that it is not: fails closed. */
static BOOL upd_exists(const char *path)
{
    return !upd_absent(path);
}

/* "<what> <path>" said, with a refusal's return code. */
static LONG refuse_at(const char *why, const char *path, const char *hint)
{
    upd_line[0] = '\0';
    sb(upd_line, sizeof(upd_line), why);
    sb(upd_line, sizeof(upd_line), " ");
    sb(upd_line, sizeof(upd_line), path);
    return refuse(upd_line, hint);
}

/* ---- the test stop hook ---- */

#ifdef UPD_TEST_KEY
static char  upd_stop_phase[12];
static ULONG upd_stop_n;
static char  upd_stop_file[128];
static ULONG upd_cuts[3];

/*
 * PREPARE's and SWAP's cut points are a sample, one at each boundary that
 * differs, so that the scenario that stops at every one of them (S40,
 * cut=each) is a handful of boots: every single operation is cut, and
 * recovered from, by test_updinstall on the host.  upd_samp[phase] holds
 * the raw step numbers that count, 1-based, set by upd_install(); the
 * rollback's (S42) are not sampled.
 *
 *   prepare  1 the first .new written   2 half of them   3 the last .new
 *            4 "ready" appended
 *   swap     1 the first .old moved aside (when there is one)
 *            2 half way: a live file at .old, its .new not live yet
 *            3 the last "done"           4 the journal renamed to trial
 */
#define UPD_SAMP_MAX    4
static ULONG upd_samp[2][UPD_SAMP_MAX];
static ULONG upd_nsamp[2];

static VOID upd_samp_add(ULONG phase, ULONG raw)
{
    ULONG i;

    for (i = 0; i < upd_nsamp[phase]; i++)
        if (upd_samp[phase][i] == raw)
            return;
    if (upd_nsamp[phase] < UPD_SAMP_MAX)
        upd_samp[phase][upd_nsamp[phase]++] = raw;
}

/* ENV:AmiNetXDuo/UpdateTestStop: "<prepare|swap|rollback> <n> <file>". */
static VOID upd_stop_read(VOID)
{
    static const char *const where[] = {
        "ENV:AmiNetXDuo/UpdateTestStop", "ENVARC:AmiNetXDuo/UpdateTestStop"
    };
    ULONG  len, i, k;
    UBYTE *t = NULL;

    upd_stop_n = 0;
    for (i = 0; i < 2 && t == NULL; i++)
        t = upd_slurp(where[i], 256, &len);
    if (t == NULL)
        return;
    for (i = 0, k = 0; i < len && t[i] > ' ' && k + 1 < sizeof(upd_stop_phase); i++)
        upd_stop_phase[k++] = (char)t[i];
    upd_stop_phase[k] = '\0';
    while (i < len && t[i] == ' ')
        i++;
    while (i < len && t[i] >= '0' && t[i] <= '9')
        upd_stop_n = upd_stop_n * 10 + (ULONG)(t[i++] - '0');
    while (i < len && t[i] == ' ')
        i++;
    for (k = 0; i < len && t[i] > ' ' && k + 1 < sizeof(upd_stop_file); i++)
        upd_stop_file[k++] = (char)t[i];
    upd_stop_file[k] = '\0';
    if (upd_stop_file[0] == '\0')
        upd_stop_n = 0;
    FreeVec(t);
}

/* A cut point of PREPARE (0), SWAP (1) or the rollback (2): the one named
   stops here, as a power cut would, after saying so. */
static VOID upd_cut(ULONG phase)
{
    static const char *const names[] = { "prepare", "swap", "rollback" };
    ULONG raw = ++upd_cuts[phase], n = raw, i;

    if (phase < 2)
    {
        for (i = 0; i < upd_nsamp[phase] && upd_samp[phase][i] != raw; i++)
            ;
        if (i == upd_nsamp[phase])
            return;
        n = i + 1;
    }

    if (upd_stop_n == n && tool_stricmp(upd_stop_phase, names[phase]) == 0)
    {
        upd_line[0] = '\0';
        sb(upd_line, sizeof(upd_line), "cut ");
        sbu(upd_line, sizeof(upd_line), n);
        sb(upd_line, sizeof(upd_line), "\n");
        (VOID)upd_write_file(upd_stop_file, upd_line, strlen(upd_line));
        Wait(0);
    }
}
#else
#define upd_stop_read() ((VOID)0)
#define upd_cut(p)      ((VOID)0)
#endif

/* ---- before the network ---- */

/*
 * Leftovers of a run that stopped: a journal without "ready" is undone (its
 * .new files deleted), a container and the staged receipt removed.  A
 * journal with "ready" is a swap half done, which BOOT finishes, and a
 * trial waits for CONFIRM, which needs receipt-next: both refused before
 * anything is removed.
 */
static BOOL upd_removed;

static LONG upd_clean_stale(VOID)
{
    struct FileInfoBlock *fib;
    BOOL                  removed = FALSE;
    UBYTE                *t;
    ULONG                 len, i;
    BPTR                  dir;

    /* Absent only for ERROR_OBJECT_NOT_FOUND, as the journal below. */
    upd_in_dir(upd_p1, UPD_N_STATE);
    t = upd_slurp(upd_p1, UPD_STATE_MAX, &len);
    i = t == NULL && upd_absent(upd_p1);
    upd_in_dir(upd_p1, UPD_N_TRIAL);
    i = (ULONG)upd_trial_refuse(upd_absent(upd_p1), (const char *)t,
                                t != NULL ? len : 0, (int)i);
    if (t != NULL)
        FreeVec(t);
    if (i)
        return refuse("pending", "an update is installed and not confirmed "
                                 "yet: restart the Amiga");

    /* Undone only when it is read whole and has no "ready"; gone only
       when it is not there.  A journal that cannot be read is refused. */
    upd_in_dir(upd_p1, UPD_N_JOURNAL);
    t = upd_slurp(upd_p1, 64UL * 1024UL, &len);
    {
        UpdJournal j;
        UpdJEntry *e = t != NULL ? (UpdJEntry *)AllocVec(UPD_FILES_MAX * sizeof(UpdJEntry),
                                                         MEMF_ANY) : NULL;

        switch (upd_stale_journal((const char *)t, len, t == NULL && upd_absent(upd_p1),
                                  &j, e, UPD_FILES_MAX))
        {
        case UPD_STALE_NONE:
            break;
        case UPD_STALE_UNDO:
            for (i = 0; i < j.nfiles; i++)
            {
                if (upd_sidecar(upd_p2, UPD_PATH, e[i].path, e[i].len, ".new"))
                    (VOID)upd_remove(upd_p2);
            }
            (VOID)upd_remove(upd_p1);
            removed = TRUE;
            break;
        default:
            if (e != NULL)
                FreeVec(e);
            if (t != NULL)
                FreeVec(t);
            return refuse("pending", "an update is half applied: restart the "
                                     "Amiga to finish it");
        }
        if (e != NULL)
            FreeVec(e);
        if (t != NULL)
            FreeVec(t);
    }

    upd_in_dir(upd_p1, UPD_N_JOURNAL_TMP);
    if (upd_exists(upd_p1))
        removed = upd_remove(upd_p1) || removed;
    upd_in_dir(upd_p1, UPD_N_RECEIPT_NEXT);
    if (upd_exists(upd_p1))
        removed = upd_remove(upd_p1) || removed;
    upd_in_dir(upd_p1, "prior-old");
    (VOID)DeleteFile((CONST_STRPTR)upd_p1);     /* only when empty */

    /* Every container: <name>.update.lha. */
    fib = (struct FileInfoBlock *)AllocDosObject(DOS_FIB, NULL);
    dir = Lock((CONST_STRPTR)upd_udir, SHARED_LOCK);
    if (fib != NULL && dir != (BPTR)0 && Examine(dir, fib))
    {
        BOOL again = TRUE;

        /* Deleting inside ExNext loses the place: start over after one. */
        while (again)
        {
            again = FALSE;
            if (!Examine(dir, fib))
                break;
            while (ExNext(dir, fib))
            {
                ULONG n = strlen((const char *)fib->fib_FileName);

                if (fib->fib_DirEntryType < 0 && n > sizeof(UPD_N_CONTAINER_EXT) - 1 &&
                    tool_stricmp((const char *)fib->fib_FileName + n -
                                 (sizeof(UPD_N_CONTAINER_EXT) - 1),
                                 UPD_N_CONTAINER_EXT) == 0)
                {
                    upd_in_dir(upd_p1, (const char *)fib->fib_FileName);
                    if (DeleteFile((CONST_STRPTR)upd_p1))
                    {
                        removed = TRUE;
                        again = TRUE;
                    }
                    break;
                }
            }
        }
    }
    if (dir != (BPTR)0)
        UnLock(dir);
    if (fib != NULL)
        FreeDosObject(DOS_FIB, fib);

    if (removed)
        say("staging=removed", NULL, NULL);
    upd_removed = removed;
    return -1;
}

/* ---- the state ---- */

static int ao_exists(void *c, const char *p);

static char  upd_st[UPD_STATE_MAX + 1];
static ULONG upd_st_len;

/*
 * The state into upd_st, empty when there is none.  A cut between the
 * remove of state and the rename of state.tmp leaves state.tmp alone, and
 * whole: `recover` renames it to state first (under the lock), else it is
 * read where it is.  FALSE, and upd_st empty, when a look or the read
 * fails or it is over the size: then nothing may be written from it.
 */
static BOOL upd_state_load_as(BOOL recover)
{
    UBYTE *t;
    ULONG  len = 0;
    int    plan;

    upd_st_len = 0;
    upd_st[0] = '\0';
    upd_in_dir(upd_p2, UPD_N_STATE);
    upd_in_dir(upd_p1, UPD_N_STATE_TMP);
    plan = upd_pair_plan(ao_exists(NULL, upd_p2), ao_exists(NULL, upd_p1));
    if (plan == UPD_PAIR_EMPTY)
        return TRUE;
    if (plan == UPD_PAIR_FAIL)
        return FALSE;
    if (plan == UPD_PAIR_RECOVER)
    {
        if (!recover)
            upd_in_dir(upd_p2, UPD_N_STATE_TMP);
        else if (!Rename((CONST_STRPTR)upd_p1, (CONST_STRPTR)upd_p2))
            return FALSE;
    }
    t = upd_slurp(upd_p2, UPD_STATE_MAX, &len);
    if (t == NULL)
        return FALSE;
    CopyMem(t, upd_st, len);
    upd_st_len = len;
    upd_st[len] = '\0';
    FreeVec(t);
    return TRUE;
}

static BOOL upd_state_load(VOID)
{
    return upd_state_load_as(TRUE);
}

/* A state value, "" when there is none. */
static const char *upd_state_value(const char *key)
{
    static char v[UPD_VER_MAX + 16];

    if (!upd_state_get(upd_st, upd_st_len, key, v, sizeof(v)))
        v[0] = '\0';
    return v;
}

static BOOL upd_phase_is(const char *phase)
{
    return tool_stricmp(upd_state_value("phase"), phase) == 0;
}

/* A trial, its commit or its rollback is under way. */
static BOOL upd_phase_open(VOID)
{
    return upd_phase_is("trial") || upd_phase_is("commit") ||
           upd_phase_is("rollback");
}

/* `n` bytes of `out` as the state: state.tmp, then renamed over state. */
static BOOL upd_state_write(const char *out, ULONG n)
{
    BOOL ok;

    upd_in_dir(upd_p1, "state.tmp");
    upd_in_dir(upd_p2, "state");
    ok = upd_write_file(upd_p1, out, n);
    if (ok)
        (VOID)upd_remove(upd_p2);
    ok = ok && Rename((CONST_STRPTR)upd_p1, (CONST_STRPTR)upd_p2);
    if (ok)
    {
        CopyMem((APTR)out, upd_st, n);
        upd_st_len = n;
        upd_st[n] = '\0';
    }
    return ok;
}

/* phase, version and boots set, flavour (NULL: the state's own) and every
   other line kept. */
static BOOL upd_state_put(const char *phase, const char *version, ULONG boots,
                          const char *flavour)
{
    static char out[UPD_STATE_MAX];
    static char fl[16];
    ULONG       n;

    /* A state that cannot be read is not written over: its keys (skip=,
       last_check=) would go. */
    if (!upd_state_load())
        return FALSE;
    if (flavour == NULL)
    {
        tool_copy_string(fl, sizeof(fl), upd_state_value("flavour"));
        flavour = fl;
    }
    n = upd_state_merge(upd_st, upd_st_len, phase, version, boots, flavour,
                        out, sizeof(out));
    return n != 0 && upd_state_write(out, n);
}

/*
 * The Update drawer's lock (src/tools/updinstall.h): one INSTALL, BOOT or
 * CONFIRM at a time, from before its cleanup to its end.  -1 when it is
 * held, or the drawer is read-only and nothing can be written to race
 * (the write-protection refusal comes later); else the refusal's rc.
 */
static BPTR upd_lock_fh;

/* upd_lock_why() reads these by number. */
typedef char upd_lock_codes[(ERROR_OBJECT_IN_USE == 202 &&
                             ERROR_DISK_WRITE_PROTECTED == 214 &&
                             ERROR_WRITE_PROTECTED == 223) ? 1 : -1];

static LONG upd_lock_take(VOID)
{
    if (upd_lock_fh != (BPTR)0)
        return -1;
    upd_in_dir(upd_p1, UPD_N_LOCK);
    upd_lock_fh = Open((CONST_STRPTR)upd_p1, MODE_NEWFILE);
    if (upd_lock_fh != (BPTR)0)
        return -1;
    switch (upd_lock_why(IoErr()))
    {
    case UPD_LOCK_BUSY:
        return refuse("busy", "another UpdateAmiNetXDuo is running");
    case UPD_LOCK_WP:
        return -1;
    default:
        return refuse_at("protected", upd_p1, "the lock file cannot be made");
    }
}

static VOID upd_lock_drop(VOID)
{
    if (upd_lock_fh == (BPTR)0)
        return;
    Close(upd_lock_fh);
    upd_lock_fh = (BPTR)0;
    upd_in_dir(upd_p1, UPD_N_LOCK);
    (VOID)DeleteFile((CONST_STRPTR)upd_p1);
}

/* The receipt and the Update drawer, before anything is fetched: an
   installation from before 1.0.0-rc5 has neither. */
static LONG upd_receipt_load(VOID)
{
    if (upd_rcpt != NULL)
        return -1;
    if (!upd_find_udir())
        return refuse("no_receipt", "no install receipt: run Install-AmiNetXDuo");

    upd_in_dir(upd_p1, UPD_N_RECEIPT);
    upd_rcpt_text = upd_slurp(upd_p1, UPD_RCPT_FILE_MAX, &upd_rcpt_len);
    upd_rcpt = (UpdRcpt *)AllocVec(UPD_RCPT_MAX * sizeof(UpdRcpt), MEMF_ANY);
    if (upd_rcpt_text == NULL || upd_rcpt == NULL ||
        !upd_receipt_parse((const char *)upd_rcpt_text, upd_rcpt_len, upd_rcpt,
                           UPD_RCPT_MAX, &upd_nrcpt))
    {
        if (upd_rcpt != NULL)
            FreeVec(upd_rcpt);
        upd_rcpt = NULL;
        return refuse("no_receipt", "no install receipt: run Install-AmiNetXDuo");
    }
    return -1;
}

/* INSTALL's start: the receipt, no trial under way, and what a run that
   stopped left behind removed. */
static LONG upd_install_begin(VOID)
{
    LONG rc = upd_receipt_load();

    if (rc >= 0)
        return rc;
    /* From before the cleanup to the end of the swap: BOOT and CONFIRM
       take the same lock. */
    rc = upd_lock_take();
    if (rc >= 0)
        return rc;
    if (!upd_state_load())
        return refuse("pending", "the update state cannot be read");
    if (upd_phase_open())
        return refuse("pending", "the update installed last is on trial: "
                                 "restart, and the next boot with the "
                                 "network up confirms it");
    upd_stop_read();
    return upd_clean_stale();
}

static VOID upd_install_end(VOID)
{
    upd_lock_drop();
    if (upd_rcpt != NULL)
        FreeVec(upd_rcpt);
    if (upd_rcpt_text != NULL)
        FreeVec(upd_rcpt_text);
    upd_rcpt = NULL;
    upd_rcpt_text = NULL;
}

/* ---- selection ---- */

/* The volume `lock` is on, in upd_vols[]; -1 when Info() fails. */
static LONG upd_vol_of(BPTR lock, const char *name)
{
    ULONG i;

    if (!Info(lock, &upd_info))
        return -1;
    for (i = 0; i < upd_nvols; i++)
        if (upd_vols[i].node == upd_info.id_VolumeNode)
            return (LONG)i;
    if (upd_nvols >= UPD_VOL_MAX)
        return -1;
    upd_vols[i].node  = upd_info.id_VolumeNode;
    upd_vols[i].state = upd_info.id_DiskState;
    upd_vols[i].free  = upd_free_blocks(upd_info.id_NumBlocks, upd_info.id_NumBlocksUsed);
    upd_vols[i].bpb   = (ULONG)upd_info.id_BytesPerBlock;
    upd_vols[i].need  = UPD_VOL_MARGIN;
    tool_copy_string(upd_vols[i].name, sizeof(upd_vols[i].name), name);
    upd_nvols++;
    return (LONG)i;
}

/* SHA-256 of a file and what the scan finds in it. */
static BOOL upd_hash_file(const char *path, UBYTE *sha, UpdScan *scan)
{
    BPTR fh = Open((CONST_STRPTR)path, MODE_OLDFILE);
    LONG n;

    if (fh == (BPTR)0)
        return FALSE;
    (VOID)c68k_sha256_initialize(&upd_sha, NX_CRYPTO_HASH_SHA256);
    upd_scan_init(scan);
    while ((n = Read(fh, upd_chunk, sizeof(upd_chunk))) > 0)
    {
        (VOID)c68k_sha256_update(&upd_sha, upd_chunk, (UINT)n);
        upd_scan_feed(scan, upd_chunk, (unsigned long)n);
    }
    Close(fh);
    if (n < 0)
        return FALSE;
    (VOID)c68k_sha256_digest_calculate(&upd_sha, sha, NX_CRYPTO_HASH_SHA256);
    return TRUE;
}

/* Protection bits that stop a write or a delete, on what `path` names. */
/* 1 a drawer, 0 a file, -1 not there or not to be examined. */
static LONG upd_kind(const char *path)
{
    BPTR                  l = Lock((CONST_STRPTR)path, SHARED_LOCK);
    struct FileInfoBlock *fib = (struct FileInfoBlock *)AllocDosObject(DOS_FIB, NULL);
    LONG                  k = -1;

    if (fib != NULL && l != (BPTR)0 && Examine(l, fib))
        k = fib->fib_DirEntryType > 0 ? 1 : 0;
    if (l != (BPTR)0)
        UnLock(l);
    if (fib != NULL)
        FreeDosObject(DOS_FIB, fib);
    return k;
}

/* Not there: FALSE.  Any other failure to look: protected. */
static BOOL upd_protected(const char *path, ULONG *size)
{
    BPTR                  l = Lock((CONST_STRPTR)path, SHARED_LOCK);
    BOOL                  p = l != (BPTR)0 || !upd_ioerr_absent(IoErr());
    struct FileInfoBlock *fib = (struct FileInfoBlock *)AllocDosObject(DOS_FIB, NULL);

    if (fib != NULL && l != (BPTR)0 && Examine(l, fib))
    {
        p = (fib->fib_Protection & (FIBF_WRITE | FIBF_DELETE)) != 0;
        if (size != NULL)
            *size = (ULONG)fib->fib_Size;
    }
    if (l != (BPTR)0)
        UnLock(l);
    if (fib != NULL)
        FreeDosObject(DOS_FIB, fib);
    return p;
}

/* ---- the swap's file operations ---- */

static char upd_journal_path[UPD_PATH];

/* Tri-state for the swap: a lock error is neither there nor not. */
static int ao_exists(void *c, const char *p)
{
    BPTR l = Lock((CONST_STRPTR)p, SHARED_LOCK);

    (VOID)c;
    if (l != (BPTR)0)
    {
        UnLock(l);
        return 1;
    }
    return upd_ioerr_absent(IoErr()) ? 0 : -1;
}
static int ao_rename(void *c, const char *a, const char *b)
{
    (VOID)c;
    return Rename((CONST_STRPTR)a, (CONST_STRPTR)b) ? 1 : 0;
}
static int ao_remove(void *c, const char *p) { (VOID)c; return upd_remove(p); }

static int ao_copy(void *c, const char *a, const char *b)
{
    BPTR in = Open((CONST_STRPTR)a, MODE_OLDFILE), out;
    LONG n = 0;
    int  ok = 0;

    (VOID)c;
    if (in == (BPTR)0)
        return 0;
    out = Open((CONST_STRPTR)b, MODE_NEWFILE);
    if (out != (BPTR)0)
    {
        while ((n = Read(in, upd_chunk, sizeof(upd_chunk))) > 0)
            if (Write(out, upd_chunk, n) != n)
            {
                n = -1;
                break;
            }
        ok = Close(out) && n == 0;
    }
    Close(in);
    return ok;
}

static int ao_journal(void *c, const char *line)
{
    BPTR fh = Open((CONST_STRPTR)upd_journal_path, MODE_READWRITE);
    LONG n = (LONG)strlen(line);
    int  ok;

    (VOID)c;
    if (fh == (BPTR)0)
        return 0;
    (VOID)Seek(fh, 0, OFFSET_END);
    ok = Write(fh, (APTR)line, n) == n && Write(fh, (APTR)"\n", 1) == 1;
    return Close(fh) && ok;
}

/* The phase the swap's cut points count in: 1 SWAP (and BOOT's forward),
   2 the rollback, 3 none (the commit). */
static ULONG upd_cut_phase = 1;

static int ao_cut(void *c)
{
    (VOID)c;
    if (upd_cut_phase < 3)
        upd_cut(upd_cut_phase);
    return 1;
}

static const UpdFsOps upd_amiga_ops = {
    NULL, ao_exists, ao_rename, ao_remove, ao_copy, ao_journal, ao_cut
};

/* ---- PREPARE: the container ---- */

typedef struct UpdSink
{
    const char *path;
    BPTR        fh;
    ULONG       total;
    ULONG       size;           /* the manifest's                          */
    BOOL        wfail;          /* a write failed: the disk                */
    BOOL        toolong;
} UpdSink;

static BOOL us_open(APTR ctx)
{
    UpdSink *s = (UpdSink *)ctx;

    s->fh = Open((CONST_STRPTR)s->path, MODE_NEWFILE);
    if (s->fh == (BPTR)0)
    {
        s->wfail = TRUE;
        return FALSE;
    }
    (VOID)c68k_sha256_initialize(&upd_sha, NX_CRYPTO_HASH_SHA256);
    return TRUE;
}

static BOOL us_write(APTR ctx, const UBYTE *d, LONG n)
{
    UpdSink *s = (UpdSink *)ctx;

    if (n <= 0)
        return TRUE;
    /* Not opened: an error status's body, which is no container. */
    switch (upd_dl_chunk(s->fh != (BPTR)0, s->total, (ULONG)n, s->size))
    {
    case UPD_DL_OK:
        break;
    case UPD_DL_LONG:
        s->toolong = TRUE;
        return FALSE;
    default:
        return FALSE;
    }
    if (Write(s->fh, (APTR)d, n) != n)
    {
        s->wfail = TRUE;
        return FALSE;
    }
    (VOID)c68k_sha256_update(&upd_sha, (UCHAR *)d, (UINT)n);
    s->total += (ULONG)n;
    return TRUE;
}

typedef struct UpdInst
{
    UpdManifest *m;
    UpdFile     *files;
    UpdPick     *pick;
    UpdJEntry   *je;
    char        *pool;
    const char  *mirror;
    unsigned int channel;
    unsigned int nrep, nold;
    ULONG        kept, skipped, missing;
    char         cname[UPD_NAME_MAX + 1];
    char         cpath[UPD_PATH];
    char         newv[UPD_VER_MAX];
} UpdInst;

/* Removes everything PREPARE staged: only the .new files this run wrote.
   The rest is this run's too: the cleanup before it removed or refused
   any other, under the drawer's lock. */
static VOID upd_unstage(UpdInst *in)
{
    upd_unstage_new(in->je, in->nrep, &upd_amiga_ops);
    (VOID)upd_remove(in->cpath);
    upd_in_dir(upd_p1, UPD_N_RECEIPT_NEXT);
    (VOID)upd_remove(upd_p1);
    upd_in_dir(upd_p1, UPD_N_JOURNAL_TMP);
    (VOID)upd_remove(upd_p1);
    (VOID)upd_remove(upd_journal_path);
    say("staging=removed", NULL, NULL);
}

static LONG upd_download(UpdInst *in)
{
    UpdSink       s;
    FetchUrlResult why;
    UBYTE         sha[32];
    LONG          rc;

    if (!upd_url(upd_url_buf, sizeof(upd_url_buf), in->mirror, in->channel,
                 in->cname))
        return refuse("network", NULL);
    why = fetch_url_parse(upd_url_buf, &upd_fc.url);
    if (why != FETCH_URL_OK)
        return refuse("network", NULL);

    s.path = in->cpath;
    s.fh = (BPTR)0;
    s.total = 0;
    s.size = in->m->container_size;
    s.wfail = s.toolong = FALSE;

    upd_fc.timeout    = UPD_TIMEOUT;
    upd_fc.family     = TOOL_AF_UNSPEC;
    upd_fc.headers    = FALSE;
    upd_fc.quiet      = TRUE;
    upd_fc.noverify   = FALSE;
    upd_fc.sink.open  = us_open;
    upd_fc.sink.write = us_write;
    upd_fc.sink.ctx   = &s;

    rc = fetch_core_run(&upd_fc);
    if (s.fh != (BPTR)0 && !Close(s.fh))
        s.wfail = TRUE;

    switch (upd_dl_verdict(upd_fc.status, rc == RETURN_OK, s.wfail, s.toolong,
                           s.total, s.size))
    {
    case UPD_DL_OK:
        break;
    case UPD_DL_SPACE:
        return refuse("space", "the container could not be written");
    case UPD_DL_LONG:
        return refuse("container_hash", "the container is longer than the "
                                        "manifest says");
    default:
        return refuse("network", "the container did not arrive whole");
    }
    (VOID)c68k_sha256_digest_calculate(&upd_sha, sha, NX_CRYPTO_HASH_SHA256);
    if (memcmp(sha, in->m->container_sha256, 32) != 0)
        return refuse("container_hash", "the container is not the one the "
                                        "manifest names");
    return -1;
}

/* Each member to <live>.new, checked against its file= line. */
static LONG upd_extract(UpdInst *in)
{
    /* The headers in the chunk buffer's top; the data copy below uses
       the rest of it only after they are parsed. */
    UBYTE       *hdr = upd_chunk + sizeof(upd_chunk) - UPD_LHA_EXT_MAX - UPD_LHA_HEAD_MAX - 2;
    UBYTE       *ext = upd_chunk + sizeof(upd_chunk) - UPD_LHA_EXT_MAX;
    UpdLha       lm;
    BPTR         fh = Open((CONST_STRPTR)in->cpath, MODE_OLDFILE);
    ULONG        got = 0;
    LONG         rc = -1;

    if (fh == (BPTR)0)
        return refuse("container_hash", NULL);

    for (;;)
    {
        long  hl;
        int   idx;
        ULONG left;
        UBYTE sha[32];
        BPTR  out;

        if (Read(fh, hdr, 1) != 1)
        {
            rc = refuse("member_hash", "the container ends early");
            break;
        }
        if (hdr[0] == 0)
            break;
        if (Read(fh, hdr + 1, (LONG)hdr[0] + 1) != (LONG)hdr[0] + 1 ||
            (hl = upd_lha_base(hdr, (unsigned long)hdr[0] + 2, &lm)) <= 0 ||
            Read(fh, ext, (LONG)(lm.skip - lm.size)) != (LONG)(lm.skip - lm.size) ||
            !upd_lha_ext(ext, lm.skip - lm.size, &lm) ||
            (idx = upd_member_file(lm.name, lm.name_len, in->files,
                                   in->m->nfiles)) < 0 ||
            lm.size != in->files[idx].size)
        {
            rc = refuse("member_hash", "a member is not one the manifest names");
            break;
        }
        /* Not replaced: skipped unhashed.  upd_download() checked the
           SHA-256 of the whole container, every byte of it, against the
           signed manifest. */
        if (in->pick[idx].act != UPD_ACT_REPLACE)
        {
            if (Seek(fh, (LONG)lm.size, OFFSET_CURRENT) < 0)
            {
                rc = refuse("member_hash", NULL);
                break;
            }
            continue;
        }

        /* Each selected file once: a second copy is refused before it
           overwrites the first. */
        if (in->je[in->pick[idx].jn].staged)
        {
            rc = refuse("member_hash", "a file is in the container twice");
            break;
        }
        (VOID)upd_sidecar(upd_p2, UPD_PATH, in->pick[idx].real,
                          strlen(in->pick[idx].real), ".new");
        out = Open((CONST_STRPTR)upd_p2, MODE_NEWFILE);
        if (out == (BPTR)0)
        {
            rc = refuse_at("space", upd_p2, NULL);
            break;
        }
        in->je[in->pick[idx].jn].staged = 1;
        (VOID)c68k_sha256_initialize(&upd_sha, NX_CRYPTO_HASH_SHA256);
        left = lm.size;
        while (left > 0)
        {
            LONG want = left > sizeof(upd_chunk) ? (LONG)sizeof(upd_chunk) : (LONG)left;

            if (Read(fh, upd_chunk, want) != want)
                break;
            if (Write(out, upd_chunk, want) != want)
            {
                left = ~0UL;
                break;
            }
            (VOID)c68k_sha256_update(&upd_sha, upd_chunk, (UINT)want);
            left -= (ULONG)want;
        }
        if (!Close(out) || left == ~0UL)
        {
            rc = refuse_at("space", upd_p2, NULL);
            break;
        }
        (VOID)c68k_sha256_digest_calculate(&upd_sha, sha, NX_CRYPTO_HASH_SHA256);
        if (left != 0 || memcmp(sha, in->files[idx].sha256, 32) != 0)
        {
            upd_line[0] = '\0';
            sb(upd_line, sizeof(upd_line), "member_hash ");
            sbn(upd_line, sizeof(upd_line), lm.name, lm.name_len);
            rc = refuse(upd_line, "a file in the container is not the one "
                                  "the manifest names");
            break;
        }
        got++;
        upd_cut(0);
    }
    Close(fh);
    if (rc < 0 && (got != in->nrep || !upd_all_staged(in->je, in->nrep)))
        rc = refuse("member_hash", "the container lacks a file the manifest "
                                   "names");
    return rc;
}

/* The receipt as it will be once the update is confirmed. */
static BOOL upd_receipt_next(UpdInst *in)
{
    ULONG  cap = upd_rcpt_len + 64, at = 0, r, f;
    char  *out = (char *)AllocVec(cap + 1, MEMF_ANY);
    char   hex[65];
    BOOL   ok;

    if (out == NULL)
        return FALSE;
    out[0] = '\0';
    for (r = 0; r < upd_nrcpt; r++)
    {
        const unsigned char *sha = upd_rcpt[r].sha256;
        BOOL                 drop = FALSE;

        for (f = 0; f < in->m->nfiles; f++)
            if (in->pick[f].rcpt == (short)r)
            {
                if (in->pick[f].act == UPD_ACT_MISSING)
                    drop = TRUE;
                else if (in->pick[f].act == UPD_ACT_REPLACE ||
                         in->pick[f].why == UPD_WHY_CURRENT)
                    sha = in->files[f].sha256;
            }
        if (drop)
            continue;
        upd_hex32(sha, hex);
        sb(out, cap, hex);
        sb(out, cap, " ");
        sbn(out, cap, upd_rcpt[r].path, upd_rcpt[r].len);
        sb(out, cap, "\n");
    }
    at = strlen(out);
    upd_in_dir(upd_p1, UPD_N_RECEIPT_NEXT);
    ok = upd_write_file(upd_p1, out, at);
    FreeVec(out);
    return ok;
}

static BOOL upd_journal_write(UpdInst *in, const UpdWant *want)
{
    ULONG cap = 256, i, at;
    char *t;
    BOOL  ok;
    char  from[UPD_VER_MAX];

    for (i = 0; i < in->m->nfiles; i++)
        if (in->pick[i].act == UPD_ACT_REPLACE)
            cap += strlen(in->pick[i].real) + 20;
    t = (char *)AllocVec(cap, MEMF_ANY);
    if (t == NULL)
        return FALSE;
    t[0] = '\0';
    upd_ver_format(&want->installed, from);
    sb(t, cap, "format=1\nversion=");
    sb(t, cap, in->newv);
    sb(t, cap, "\nfrom=");
    sb(t, cap, from);
    sb(t, cap, "\nflavour=");
    sb(t, cap, upd_flavour_name(want->flavour));
    sb(t, cap, "\n");
    for (i = 0; i < in->m->nfiles; i++)
        if (in->pick[i].act == UPD_ACT_REPLACE)
        {
            sb(t, cap, "file=");
            sb(t, cap, upd_why_name(in->pick[i].why));
            sb(t, cap, " ");
            sb(t, cap, in->pick[i].real);
            sb(t, cap, "\n");
        }
    at = strlen(t);
    upd_in_dir(upd_p1, UPD_N_JOURNAL_TMP);
    ok = upd_write_file(upd_p1, t, at) &&
         Rename((CONST_STRPTR)upd_p1, (CONST_STRPTR)upd_journal_path);
    FreeVec(t);
    return ok;
}

/* phase=trial with the rest of the state kept: state.tmp, then renamed. */
/* The state with phase=trial, merged before PREPARE (upd_state_next). */
static char  upd_state_out[UPD_STATE_MAX];
static ULONG upd_state_len;

/* state.tmp, then renamed: the old state goes only once it is written. */
static BOOL upd_state_trial(VOID)
{
    upd_in_dir(upd_p1, UPD_N_STATE_TMP);
    if (upd_state_len == 0 || !upd_write_file(upd_p1, upd_state_out, upd_state_len))
        return FALSE;
    upd_in_dir(upd_p2, UPD_N_STATE);
    (VOID)upd_remove(upd_p2);
    return Rename((CONST_STRPTR)upd_p1, (CONST_STRPTR)upd_p2);
}

static LONG upd_install(const char *mirror, const UpdWant *want,
                        UpdManifest *m, UpdFile *files)
{
    UpdInst      in;
    ULONG        i, pool_len = 0, upd_vol;
    LONG         rc = -1, v;
    int          layout = upd_receipt_layout(upd_rcpt, upd_nrcpt);
    unsigned int failed;
    char        *pp;
    BPTR         l;

    memset(&in, 0, sizeof(in));
    in.m = m;
    in.files = files;
    in.mirror = mirror;
    in.channel = want->channel;
    upd_ver_format(&m->version, in.newv);
    upd_nvols = 0;
    upd_in_dir(upd_journal_path, UPD_N_JOURNAL);

    in.pick = (UpdPick *)AllocVec(m->nfiles * sizeof(UpdPick) + 1, MEMF_ANY | MEMF_CLEAR);
    in.pool = (char *)AllocVec(m->nfiles * UPD_PATH + 1, MEMF_ANY);
    if (in.pick == NULL || in.pool == NULL)
    {
        tool_fault(ERROR_NO_FREE_STORE);
        rc = refuse("space", NULL);
        goto out;
    }
    pp = in.pool;

    l = Lock((CONST_STRPTR)upd_udir, SHARED_LOCK);
    upd_vol = l != (BPTR)0 ? (ULONG)upd_vol_of(l, upd_udir) : ~0UL;
    if (l != (BPTR)0)
        UnLock(l);
    if (upd_vol == ~0UL)
    {
        rc = refuse_at("writeprotected", upd_udir, NULL);
        goto out;
    }

    /* ---- which files, by the receipt and the disk ---- */
    for (i = 0; i < m->nfiles; i++)
    {
        UpdPick  *p = &in.pick[i];
        UpdSelIn  sel;
        UpdScan   scan;
        UBYTE     disk[32];
        int       r;

        p->rcpt = -1;
        if (!upd_dest(upd_p1, UPD_PATH, layout, files[i].cls, files[i].path,
                      files[i].path_len))
            continue;
        r = upd_receipt_find(upd_rcpt, upd_nrcpt, upd_p1, strlen(upd_p1));
        if (r < 0)
            continue;
        p->rcpt = (short)r;

        memset(&sel, 0, sizeof(sel));
        sel.receipt = upd_rcpt[r].sha256;
        sel.fresh   = files[i].sha256;
        sel.update  = &m->version;
        l = Lock((CONST_STRPTR)upd_p1, SHARED_LOCK);
        /* Missing is ERROR_OBJECT_NOT_FOUND alone: a file in use, or one
           on a volume that will not answer, is not deleted. */
        if (l == (BPTR)0 && !upd_ioerr_absent(IoErr()))
        {
            rc = refuse_at("protected", upd_p1, "it cannot be read");
            goto out;
        }
        if (l != (BPTR)0)
        {
            if (upd_hash_file(upd_p1, disk, &scan))
            {
                sel.disk = disk;
                sel.scan = &scan;
            }
            else
            {
                UnLock(l);
                rc = refuse_at("protected", upd_p1, "it cannot be read");
                goto out;
            }
        }
        upd_select(&sel, &p->act, &p->why);

        if (p->act == UPD_ACT_REPLACE)
        {
            v = upd_vol_of(l, upd_p1);
            if (v < 0 || !NameFromLock(l, (STRPTR)pp, UPD_PATH - 5))
            {
                UnLock(l);
                rc = refuse_at("protected", upd_p1, "its volume cannot be read");
                goto out;
            }
            p->vol  = (unsigned char)v;
            p->real = pp;
            pool_len = strlen(pp) + 1;
            pp += pool_len;
            in.nrep++;
        }
        if (l != (BPTR)0)
            UnLock(l);

        switch (p->act)
        {
        case UPD_ACT_KEEP:    in.kept++;    break;
        case UPD_ACT_SKIP:    in.skipped++; break;
        case UPD_ACT_MISSING: in.missing++; break;
        default: break;
        }
        if ((p->act == UPD_ACT_REPLACE && p->why == UPD_WHY_OTHERBUILD) ||
            p->act == UPD_ACT_KEEP || p->act == UPD_ACT_MISSING ||
            (p->act == UPD_ACT_SKIP && p->why == UPD_WHY_NEWER))
        {
            upd_line[0] = '\0';
            sb(upd_line, sizeof(upd_line), "file=");
            sb(upd_line, sizeof(upd_line), upd_act_name(p->act));
            sb(upd_line, sizeof(upd_line), " ");
            sb(upd_line, sizeof(upd_line), upd_why_name(p->why));
            sb(upd_line, sizeof(upd_line), " ");
            sb(upd_line, sizeof(upd_line), upd_p1);
            say(upd_line, NULL, NULL);
        }
    }

    /* Nothing to replace: no container, no journal, no trial, no
       restart, and the receipt as it is. */
    if (in.nrep == 0)
    {
        upd_line[0] = '\0';
        sb(upd_line, sizeof(upd_line), "update=unchanged version=");
        sb(upd_line, sizeof(upd_line), in.newv);
        sb(upd_line, sizeof(upd_line), " kept=");
        sbu(upd_line, sizeof(upd_line), in.kept);
        sb(upd_line, sizeof(upd_line), " skipped=");
        sbu(upd_line, sizeof(upd_line), in.skipped);
        sb(upd_line, sizeof(upd_line), " missing=");
        sbu(upd_line, sizeof(upd_line), in.missing);
        say(upd_line, NULL, NULL);
        rc = RETURN_OK;
        goto out;
    }

    /* ---- refused as a whole, before anything is staged ---- */
    if (upd_vols[upd_vol].state == ID_WRITE_PROTECTED)
    {
        rc = refuse_at("writeprotected", upd_udir, NULL);
        goto out;
    }
    /* The swap's entries, in file order: PREPARE marks what it stages. */
    in.je = (UpdJEntry *)AllocVec((in.nrep + 1) * sizeof(UpdJEntry), MEMF_ANY | MEMF_CLEAR);
    if (in.je == NULL)
    {
        tool_fault(ERROR_NO_FREE_STORE);
        rc = refuse("space", NULL);
        goto out;
    }
    for (i = 0, v = 0; i < m->nfiles; i++)
        if (in.pick[i].act == UPD_ACT_REPLACE)
        {
            in.pick[i].jn = (unsigned char)v;
            in.je[v].path = in.pick[i].real;
            in.je[v].len  = (unsigned short)strlen(in.pick[i].real);
            in.je[v].why  = in.pick[i].why;
            v++;
        }
    if (!upd_sidecars_distinct(in.je, in.nrep, &failed))
    {
        rc = refuse_at("protected", in.je[failed].path, "its name, cut to "
                       "fit 30 characters, is another file's");
        goto out;
    }
    for (i = 0; i < m->nfiles; i++)
    {
        UpdPick *p = &in.pick[i];

        if (p->act != UPD_ACT_REPLACE)
            continue;
        upd_dest(upd_p1, UPD_PATH, layout, files[i].cls, files[i].path,
                 files[i].path_len);
        /* The physical path, which an assign can make longer than the
           journal and the swap's names hold. */
        upd_in_dir(upd_p2, UPD_N_PRIOR);
        if (!upd_swap_fits(&in.je[p->jn], p->jn, upd_p2))
        {
            rc = refuse_at("protected", upd_p1, "its full path does not "
                                                "fit the journal");
            goto out;
        }
        if (upd_vols[p->vol].state == ID_WRITE_PROTECTED)
        {
            rc = refuse_at("writeprotected", upd_p1, NULL);
            goto out;
        }
        if (upd_protected(p->real, NULL))
        {
            rc = refuse_at("protected", upd_p1, "clear its protection bits "
                                                "(Protect <file> +wd)");
            goto out;
        }
        (VOID)upd_sidecar(upd_p2, UPD_PATH, p->real, strlen(p->real), ".new");
        /* Not ours: the cleanup took any an unfinished run left. */
        if (!upd_new_free(&in.je[p->jn], 1, &upd_amiga_ops, NULL))
        {
            rc = refuse_at("protected", upd_p2, "it is not the updater's: "
                                                "move it away");
            goto out;
        }
        (VOID)upd_sidecar(upd_p2, UPD_PATH, p->real, strlen(p->real), ".old");
        if (upd_exists(upd_p2))
        {
            if (upd_protected(upd_p2, &p->old_size))
            {
                rc = refuse_at("protected", upd_p2, "an existing .old cannot "
                                                    "be moved aside");
                goto out;
            }
            /* Where it goes, an earlier update's: the swap would stop. */
            upd_in_dir(upd_p1, UPD_N_PRIOR "/");
            sbu(upd_p1, UPD_PATH, (ULONG)p->jn + 1);
            if (upd_exists(upd_p1))
            {
                rc = refuse_at("protected", upd_p1, "an earlier update's "
                                                    "saved file is there");
                goto out;
            }
            p->has_old = 1;
            in.nold++;
            if (p->vol != upd_vol)
                upd_vols[upd_vol].need = upd_sat_add(upd_vols[upd_vol].need,
                                                     upd_blocks(p->old_size, upd_vols[upd_vol].bpb));
        }
        upd_vols[p->vol].need = upd_sat_add(upd_vols[p->vol].need,
                                            upd_blocks(files[i].size, upd_vols[p->vol].bpb));
    }
    upd_vols[upd_vol].need = upd_sat_add(upd_vols[upd_vol].need,
                                         upd_blocks(m->container_size, upd_vols[upd_vol].bpb));
    for (i = 0; i < upd_nvols; i++)
        if (upd_vols[i].need > upd_vols[i].free)
        {
            rc = refuse_at("space", upd_vols[i].name, "free space on that volume");
            goto out;
        }

    /* The state SWAP writes, merged now: one that cannot be read, or
       that would not fit, is refused rather than its keys dropped. */
    {
        UBYTE *old;
        ULONG  oldlen;

        upd_in_dir(upd_p2, UPD_N_STATE);
        old = upd_slurp(upd_p2, UPD_STATE_MAX, &oldlen);
        upd_state_len = upd_state_next(old != NULL ? (const char *)old : NULL,
                                       oldlen, old == NULL && upd_absent(upd_p2),
                                       in.newv, upd_flavour_name(want->flavour),
                                       upd_state_out, sizeof(upd_state_out));
        if (old != NULL)
            FreeVec(old);
        if (upd_state_len == 0)
        {
            rc = refuse_at("protected", upd_p2, "it cannot be read, or is too "
                                                "long to keep");
            goto out;
        }
    }

    /* prior-old, a drawer, before the first .new: each .old goes there. */
    if (in.nold != 0)
    {
        upd_in_dir(upd_p1, UPD_N_PRIOR);
        if (upd_absent(upd_p1))
        {
            l = CreateDir((CONST_STRPTR)upd_p1);
            if (l == (BPTR)0)
            {
                rc = refuse_at("protected", upd_p1, "it cannot be made");
                goto out;
            }
            UnLock(l);
        }
        else if (upd_kind(upd_p1) != 1)
        {
            rc = refuse_at("protected", upd_p1, "it is not a drawer");
            goto out;
        }
        for (i = 0; i < m->nfiles; i++)
            if (in.pick[i].act == UPD_ACT_REPLACE && in.pick[i].has_old)
            {
                upd_in_dir(upd_p1, UPD_N_PRIOR "/");
                sbu(upd_p1, UPD_PATH, (ULONG)in.pick[i].jn + 1);
                sb(upd_p1, UPD_PATH, ".tmp");
                if (upd_protected(upd_p1, NULL))
                {
                    rc = refuse_at("protected", upd_p1, NULL);
                    goto out;
                }
            }
    }

#ifdef UPD_TEST_KEY
    /* The sampled cut points, by the raw steps PREPARE and SWAP pass:
       PREPARE 1 the download, 2 the journal, 2+k member k, 3+nrep
       "ready"; SWAP per file [aside], live to .old, .new to live, done,
       then the state and the rename to trial. */
    if (in.nrep != 0)
    {
        ULONG raw = 0, k = 0, j, t;
        BOOL  aside = FALSE;

        upd_samp_add(0, 3);
        upd_samp_add(0, 2 + (in.nrep + 1) / 2);
        upd_samp_add(0, 2 + in.nrep);
        upd_samp_add(0, 3 + in.nrep);
        for (i = 0; i < m->nfiles; i++)
        {
            if (in.pick[i].act != UPD_ACT_REPLACE)
                continue;
            if (in.pick[i].has_old)
            {
                raw++;
                if (!aside)
                    upd_samp_add(1, raw);   /* the first .old aside        */
                aside = TRUE;
            }
            raw++;
            if (k == in.nrep / 2)
                upd_samp_add(1, raw);       /* live at .old, .new not live */
            raw += 2;
            k++;
        }
        upd_samp_add(1, raw);               /* the last "done"             */
        upd_samp_add(1, raw + 2);           /* the rename to trial         */
        /* In the order they come. */
        for (j = 1; j < upd_nsamp[1]; j++)
            for (t = j; t > 0 && upd_samp[1][t - 1] > upd_samp[1][t]; t--)
            {
                ULONG x = upd_samp[1][t];

                upd_samp[1][t] = upd_samp[1][t - 1];
                upd_samp[1][t - 1] = x;
            }
    }
    upd_line[0] = '\0';
    sb(upd_line, sizeof(upd_line), "steps.prepare=");
    sbu(upd_line, sizeof(upd_line), upd_nsamp[0]);
    sb(upd_line, sizeof(upd_line), " steps.swap=");
    sbu(upd_line, sizeof(upd_line), upd_nsamp[1]);
    say(upd_line, NULL, NULL);
#endif

    /* ---- PREPARE ---- */
    tool_copy_string(in.cname, sizeof(in.cname), "");
    sbn(in.cname, sizeof(in.cname), m->container, m->container_len);
    upd_in_dir(in.cpath, UPD_N_CONTAINER);

    if (in.nrep != 0)
    {
        rc = upd_download(&in);
        if (rc >= 0)
        {
            upd_unstage(&in);
            goto out;
        }
        upd_cut(0);
    }
    if (!upd_journal_write(&in, want))
    {
        upd_unstage(&in);
        rc = refuse("space", "the journal could not be written");
        goto out;
    }
    upd_cut(0);
    if (in.nrep != 0)
    {
        rc = upd_extract(&in);
        if (rc >= 0)
        {
            upd_unstage(&in);
            goto out;
        }
    }
    (VOID)upd_remove(in.cpath);
    if (!upd_receipt_next(&in) || !ao_journal(NULL, "ready"))
    {
        upd_unstage(&in);
        rc = refuse("space", "the journal could not be written");
        goto out;
    }
    upd_cut(0);

    /* ---- SWAP ---- */
    upd_in_dir(upd_p1, UPD_N_PRIOR);
    if (upd_forward(in.je, in.nrep, upd_p1, &upd_amiga_ops, &failed) != 1)
    {
        tool_error("the swap stopped at %s: restart, and the next boot "
                   "finishes it", (LONG)(failed < in.nrep ? in.je[failed].path : ""));
        rc = UPD_RC_FAILED;
        goto out;
    }
    if (!upd_state_trial())
    {
        tool_error("%s could not be written", (LONG)"state");
        rc = UPD_RC_FAILED;
        goto out;
    }
    upd_cut(1);
    upd_in_dir(upd_p1, UPD_N_TRIAL);
    (VOID)upd_remove(upd_p1);
    if (!Rename((CONST_STRPTR)upd_journal_path, (CONST_STRPTR)upd_p1))
    {
        tool_error("%s could not be renamed", (LONG)upd_journal_path);
        rc = UPD_RC_FAILED;
        goto out;
    }
    upd_cut(1);

    upd_line[0] = '\0';
    sb(upd_line, sizeof(upd_line), "update=installed version=");
    sb(upd_line, sizeof(upd_line), in.newv);
    sb(upd_line, sizeof(upd_line), " replaced=");
    sbu(upd_line, sizeof(upd_line), in.nrep);
    sb(upd_line, sizeof(upd_line), " kept=");
    sbu(upd_line, sizeof(upd_line), in.kept);
    sb(upd_line, sizeof(upd_line), " skipped=");
    sbu(upd_line, sizeof(upd_line), in.skipped);
    sb(upd_line, sizeof(upd_line), " missing=");
    sbu(upd_line, sizeof(upd_line), in.missing);
    say(upd_line, NULL, NULL);
    if (upd_run_args.restart)
        upd_reboot = TRUE;
    else
        say("restart=needed", NULL, NULL);
    rc = RETURN_OK;

out:
    if (in.je != NULL)
        FreeVec(in.je);
    if (in.pool != NULL)
        FreeVec(in.pool);
    if (in.pick != NULL)
        FreeVec(in.pick);
    return rc;
}

/* ----------------------------------------------------------- the answer --- */

struct IntuitionBase *IntuitionBase;

static char upd_today[11];          /* the clock's date, after upd_clock() */

static LONG upd_answer(const char *s);

/*
 * The state's last_check= set to today, and `line2` (later=, skip=) when
 * not NULL, every other line kept; under the drawer's lock, taken here when
 * this run does not hold it.  Nothing, and no line, when that cannot be: a
 * date not written is one more check, never a refusal.
 */
static VOID upd_state_note(const char *line2)
{
    static char line1[24];
    char       *tmp = (char *)upd_chunk;    /* free between the fetches  */
    char       *out = (char *)upd_chunk + UPD_STATE_MAX;
    BOOL        took = FALSE;
    ULONG       n;

    if (upd_today[0] == '\0' || !upd_find_udir())
        return;
    if (upd_lock_fh == (BPTR)0)
    {
        upd_in_dir(upd_p1, UPD_N_LOCK);
        upd_lock_fh = Open((CONST_STRPTR)upd_p1, MODE_NEWFILE);
        if (upd_lock_fh == (BPTR)0)
            return;
        took = TRUE;
    }
    if (upd_state_load())
    {
        line1[0] = '\0';
        sb(line1, sizeof(line1), "last_check=");
        sb(line1, sizeof(line1), upd_today);
        n = line2 != NULL
            ? upd_prefs_set2(upd_st, upd_st_len, line1, line2, tmp, out,
                             UPD_STATE_MAX)
            : upd_prefs_set(upd_st, upd_st_len, line1, out, UPD_STATE_MAX);
        if (n != 0)
            (VOID)upd_state_write(out, n);
    }
    if (took)
        upd_lock_drop();
}

/*
 * A requester on the Workbench screen, `a` and `b` for its two %s: 1 for
 * the first button, 2 the second, 0 the last, -1 none drawn.  The startup
 * check runs before LoadWB, so the screen is waited for, up to
 * UPD_SCREEN_WAIT seconds; without it nothing is drawn.
 */
#define UPD_SCREEN_WAIT     600UL

static LONG upd_ask(const char *text, const char *buttons, const char *a,
                    const char *b)
{
    struct EasyStruct es;
    struct Screen    *s;
    LONG              args[2];
    ULONG             t;

    if (IntuitionBase == NULL)
        IntuitionBase = (struct IntuitionBase *)
            OpenLibrary((CONST_STRPTR)"intuition.library", 37UL);
    if (IntuitionBase == NULL)
        return -1;
    for (t = 0; (s = LockPubScreen((CONST_STRPTR)"Workbench")) == NULL; t++)
        if (t >= UPD_SCREEN_WAIT || tool_delay_ticks(50))
            return -1;
    UnlockPubScreen(NULL, s);

    es.es_StructSize   = sizeof(es);
    es.es_Flags        = 0;
    es.es_Title        = (UBYTE *)"AmiNetXDuo";
    es.es_TextFormat   = (UBYTE *)text;
    es.es_GadgetFormat = (UBYTE *)buttons;
    args[0] = (LONG)a;
    args[1] = (LONG)b;
    return EasyRequestArgs(NULL, &es, NULL, (APTR)args);
}

#ifdef UPD_TEST_KEY
/* ENV:AmiNetXDuo/UpdateAnswer, "INSTALL|LATER|SKIP [RESTART]": the answers
   a QUIET run's requesters get, none of them drawn.  FALSE without one. */
static BOOL upd_hook_answer(LONG *answer, BOOL *restart)
{
    static const char *const where[] = {
        "ENV:AmiNetXDuo/UpdateAnswer", "ENVARC:AmiNetXDuo/UpdateAnswer"
    };
    char   w[2][12];
    ULONG  len, i, k, n;
    UBYTE *t = NULL;

    for (i = 0; i < 2 && t == NULL; i++)
        t = upd_slurp(where[i], 64, &len);
    if (t == NULL)
        return FALSE;
    for (i = 0, n = 0; n < 2; n++)
    {
        while (i < len && t[i] <= ' ')
            i++;
        for (k = 0; i < len && t[i] > ' ' && k + 1 < sizeof(w[n]); i++)
            w[n][k++] = (char)t[i];
        w[n][k] = '\0';
    }
    FreeVec(t);
    *answer = upd_answer(w[0]);
    if (*answer < 0)
        *answer = ANSWER_NONE;
    *restart = tool_stricmp(w[1], "RESTART") == 0;
    return TRUE;
}
#endif

/*
 * update=available, not INSTALL: the offer.  ANSWER= answers it; a QUIET
 * run asks with the requester (the test hook's answers in a test build).
 * Later: rc 5, and later= today; Skip: rc 0, and skip= the version; no
 * answer (no screen): rc 5, asked again at the next startup.  Install from
 * the requester asks to restart once it is installed, or says why not.
 */
static LONG upd_offer(const char *mirror, const UpdWant *want,
                      UpdManifest *m, UpdFile *files)
{
    LONG answer = upd_run_args.answer, rc;
    BOOL ask = upd_run_args.quiet && answer == ANSWER_NONE;
    BOOL drawn = ask, restart = FALSE, saved;
    char inst[UPD_VER_MAX];

    if (ask)
    {
#ifdef UPD_TEST_KEY
        drawn = !upd_hook_answer(&answer, &restart);
#endif
        if (drawn)
        {
            upd_ver_format(&want->installed, inst);
            switch (upd_ask("AmiNetXDuo %s is available (installed %s).",
                            "Install|Later|Skip this version", upd_ver_text,
                            inst))
            {
            case 1:  answer = ANSWER_INSTALL; break;
            case 2:  answer = ANSWER_LATER;   break;
            case 0:  answer = ANSWER_SKIP;    break;
            default: break;
            }
        }
    }
    if (answer == ANSWER_NONE)
        return UPD_RC_AVAILABLE;
    say("requester=offer", NULL, NULL);

    if (answer == ANSWER_LATER)
    {
        say("update=later version=", upd_ver_text, NULL);
        upd_line[0] = '\0';
        sb(upd_line, sizeof(upd_line), "later=");
        sb(upd_line, sizeof(upd_line), upd_today);
        upd_state_note(upd_line);
        return UPD_RC_AVAILABLE;
    }
    if (answer == ANSWER_SKIP)
    {
        say("update=skipped version=", upd_ver_text, NULL);
        upd_line[0] = '\0';
        sb(upd_line, sizeof(upd_line), "skip=");
        sb(upd_line, sizeof(upd_line), upd_ver_text);
        upd_state_note(upd_line);
        return RETURN_OK;
    }

    upd_state_note(NULL);
    rc = upd_install_begin();
    if (rc < 0)
    {
        /* From the requester the restart is asked after, not printed. */
        saved = upd_run_args.restart;
        upd_run_args.restart = upd_run_args.restart || ask;
        rc = upd_install(mirror, want, m, files);
        upd_run_args.restart = saved;
    }
    if (!ask)
        return rc;

    if (rc == RETURN_OK && upd_reboot)
    {
        if (drawn)
            restart = upd_ask("AmiNetXDuo %s is installed.\n"
                              "Restart the Amiga now to use it?",
                              "Restart now|Later", upd_ver_text, NULL) == 1;
        say("requester=restart", NULL, NULL);
        if (!restart)
        {
            upd_reboot = FALSE;
            say("restart=needed", NULL, NULL);
        }
    }
    else if (rc != RETURN_OK)
    {
        say("requester=refused", NULL, NULL);
        if (drawn && rc == UPD_RC_REFUSED)
            (VOID)upd_ask("AmiNetXDuo %s was not installed (%s).\n"
                          "Nothing was changed.",
                          "OK", upd_ver_text, upd_refused);
        else if (drawn)
            (VOID)upd_ask("The update to AmiNetXDuo %s stopped.\n"
                          "Restart the Amiga: the next start finishes it.",
                          "OK", upd_ver_text, NULL);
    }
    return rc;
}

static LONG upd_check(BOOL install)
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

    /* Installed before 1.0.0-rc5, or by hand: nothing to update from. */
    rc = upd_receipt_load();
    if (rc >= 0)
    {
        if (base != NULL)
            CloseLibrary(base);
        return rc;
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

    /* The day, from a clock that is set: the startup check's at most once. */
    {
        struct DateStamp ds;

        DateStamp(&ds);
        upd_date((unsigned long)ds.ds_Days, upd_today);
    }
    if (upd_run_args.quiet)
    {
        (VOID)upd_state_load_as(FALSE);
        if (!upd_due(upd_st, upd_st_len, upd_today))
        {
            if (base != NULL)
                CloseLibrary(base);
            say("update=notdue", NULL, NULL);
            return RETURN_OK;
        }
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
        if (upd_run_args.quiet)
            upd_state_note(NULL);
        rc = RETURN_OK;
        break;

    case UPD_V_AVAILABLE:
        upd_ver_format(&m.version, upd_ver_text);
        /* The version a rollback took back out, or one the user skipped:
           CHECK and the automatic paths pass it by, an explicit INSTALL
           installs it.  A state that cannot be read skips nothing. */
        (VOID)upd_state_load_as(FALSE);
        switch (upd_skip_verdict(upd_st, upd_st_len, upd_ver_text, install))
        {
        case UPD_SKIP_SKIPPED:
            say("update=skipped version=", upd_ver_text, NULL);
            if (upd_run_args.quiet)
                upd_state_note(NULL);
            rc = RETURN_OK;
            goto done;
        case UPD_SKIP_OVERRIDDEN:
            say("skip=overridden version=", upd_ver_text, NULL);
            break;
        default:
            break;
        }
        say("update=available version=", upd_ver_text, NULL);
        rc = install ? upd_install(mirror, &want, &m, files)
                     : upd_offer(mirror, &want, &m, files);
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

/* ------------------------------------------------------- BOOT, CONFIRM --- */

#define UPD_BOOTS_MAX       2               /* unconfirmed boots of a trial  */
#define UPD_CONFIRM_WAIT    30              /* seconds for an address        */
#define UPD_RECORD          "S:AmiNetXDuo-Files"
#define UPD_RECORD_MAX      (256UL * 1024UL)

static ULONG upd_atoul(const char *s)
{
    ULONG v = 0;

    while (*s >= '0' && *s <= '9' && v < 100000UL)
        v = v * 10 + (ULONG)(*s++ - '0');
    return v;
}

/* A journal or trial file read and parsed; text NULL when it is not there. */
typedef struct UpdJFile
{
    UBYTE      *text;
    ULONG       len;
    UpdJEntry  *e;
    UpdJournal  j;
    BOOL        ok;
    BOOL        unread;     /* there, and not read: never "not there" */
} UpdJFile;

/* BOOT's, the commit's and the rollback's, one at a time: static, as the
   startup line may run on the Shell's stack. */
static UpdJFile upd_jf;

static VOID upd_jfree(UpdJFile *f)
{
    if (f->e != NULL)
        FreeVec(f->e);
    if (f->text != NULL)
        FreeVec(f->text);
    f->e = NULL;
    f->text = NULL;
}

/* upd_journal_path read; ok when it parses.  A last line cut short is cut
   off the file first, so that what is appended starts a line of its own. */
static VOID upd_jload(UpdJFile *f)
{
    ULONG keep;

    memset(f, 0, sizeof(*f));
    f->text = upd_slurp(upd_journal_path, 64UL * 1024UL, &f->len);
    if (f->text == NULL)
    {
        /* Only ERROR_OBJECT_NOT_FOUND is "not there" (upd_stale_journal()'s
           rule): anything else is a file that could not be read. */
        f->unread = !upd_absent(upd_journal_path);
        return;
    }
    f->e = (UpdJEntry *)AllocVec(UPD_FILES_MAX * sizeof(UpdJEntry), MEMF_ANY | MEMF_CLEAR);
    if (f->e == NULL ||
        !upd_journal_parse((const char *)f->text, f->len, &f->j, f->e, UPD_FILES_MAX))
        return;
    keep = upd_journal_trim((const char *)f->text, f->len);
    if (keep != f->len)
    {
        BPTR fh = Open((CONST_STRPTR)upd_journal_path, MODE_OLDFILE);

        if (fh == (BPTR)0)
            return;
        if (SetFileSize(fh, (LONG)keep, OFFSET_BEGINNING) != (LONG)keep)
        {
            Close(fh);
            return;
        }
        Close(fh);
    }
    f->ok = TRUE;
}

static VOID upd_say_version(const char *what, const UpdVer *v)
{
    upd_ver_format(v, upd_ver_text);
    say(what, upd_ver_text, NULL);
}

/* The prior-old drawer, made when it is not there. */
static VOID upd_prior_dir(char *out)
{
    BPTR l;

    upd_in_dir(out, "prior-old");
    if (!upd_exists(out) && (l = CreateDir((CONST_STRPTR)out)) != (BPTR)0)
        UnLock(l);
}

/*
 * S:AmiNetXDuo-Files with this update's block: Uninstall-AmiNetXDuo deletes
 * the other build's copies the commit kept (<path>.old, <path>.otherbuild)
 * where they still carry the mark; a prior .old put back is not ours and is
 * not named.  The trust store is the one receipt file the installer's
 * record leaves to Installed/Remove, which deletes it only while it holds
 * the bytes of its copy in Installed: that copy becomes the store as it is
 * now, and the block deletes the store where it still holds those bytes.  Written as
 * .tmp and renamed; a .tmp a cut left is the record.
 */
/* The trust store's path as the receipt names it, into `out`, and its hash
   there into `hex`; FALSE when the receipt lists none. */
static BOOL upd_store_path(char *out, char *hex)
{
    UBYTE *t;
    ULONG  len, a, b;
    BOOL   found = FALSE;

    upd_in_dir(upd_line, "receipt");
    t = upd_slurp(upd_line, UPD_RCPT_FILE_MAX, &len);
    if (t == NULL)
        return FALSE;
    for (a = 0; a < len && !found; a = b + 1)
    {
        for (b = a; b < len && t[b] != '\n'; b++)
            ;
        if (b - a > 65 + 12 &&
            tool_stricmp_n((const char *)t + b - 12, "certificates", 12) == 0 &&
            (t[b - 13] == '/' || t[b - 13] == ':') && b - a - 65 < UPD_PATH)
        {
            out[0] = '\0';
            sbn(out, UPD_PATH, (const char *)t + a + 65, b - a - 65);
            CopyMem(t + a, hex, 64);
            hex[64] = '\0';
            found = TRUE;
        }
    }
    FreeVec(t);
    return found;
}

static __attribute__((noinline)) VOID upd_record(const UpdJFile *f)
{
    /* Static: CONFIRM may run on the Shell's stack. */
    static char             want[65], have[65], date[11];
    static UBYTE            sha[32];
    static UpdScan          scan;
    static struct DateStamp ds;
    ULONG  len = 0, cap, n, i;
    UBYTE *old;
    char  *sec, *out;

    if (!upd_exists(UPD_RECORD) && upd_exists(UPD_RECORD ".tmp"))
        (VOID)Rename((CONST_STRPTR)UPD_RECORD ".tmp", (CONST_STRPTR)UPD_RECORD);
    old = upd_slurp(UPD_RECORD, UPD_RECORD_MAX, &len);
    if (old == NULL)
        return;

    for (i = 0, cap = 200; i < f->j.nfiles; i++)
        if (f->e[i].why == UPD_WHY_OTHERBUILD)
            cap += 3 * UPD_PATH + 120;
    cap += 4 * UPD_PATH + 120;          /* the trust store's lines         */
    sec = (char *)AllocVec(cap, MEMF_ANY);
    out = (char *)AllocVec(len + cap, MEMF_ANY);
    if (sec != NULL && out != NULL)
    {
        /* InstallSameFile, beside the Update drawer in Installed. */
        upd_p2[0] = '\0';
        sb(upd_p2, UPD_PATH, upd_udir);
        for (i = strlen(upd_p2); i > 0 && upd_p2[i - 1] != '/' && upd_p2[i - 1] != ':'; i--)
            ;
        upd_p2[i] = '\0';
        sb(upd_p2, UPD_PATH, "InstallSameFile");

        DateStamp(&ds);
        upd_date((unsigned long)ds.ds_Days, date);
        upd_ver_format(&f->j.version, upd_ver_text);
        sec[0] = '\0';
        sb(sec, cap, UPD_REC_BEGIN "\n; ");
        sb(sec, cap, upd_ver_text);
        sb(sec, cap, ", installed by UpdateAmiNetXDuo on ");
        sb(sec, cap, date);
        sb(sec, cap, "\n");
        if (upd_store_path(upd_p1, want))
        {
            /* <Installed>/Devs/Internet/certificates, as it is now -- only
               while it is the receipt's: a store the user edited stays
               one Uninstall keeps. */
            static char snap[UPD_PATH];

            snap[0] = '\0';
            sbn(snap, UPD_PATH, upd_p2, strlen(upd_p2) - 15);
            sb(snap, UPD_PATH, "Devs/Internet/certificates");
            /* A cut after the old copy went and before the new one was
               renamed: the lone .tmp is the copy. */
            upd_line[0] = '\0';
            sb(upd_line, sizeof(upd_line), snap);
            sb(upd_line, sizeof(upd_line), ".tmp");
            if (upd_pair_plan(ao_exists(NULL, snap), ao_exists(NULL, upd_line)) ==
                UPD_PAIR_RECOVER)
                (VOID)Rename((CONST_STRPTR)upd_line, (CONST_STRPTR)snap);
            if (upd_exists(snap) && upd_hash_file(upd_p1, sha, &scan) &&
                (upd_hex32(sha, have), memcmp(have, want, 64) == 0))
            {
                upd_line[0] = '\0';
                sb(upd_line, sizeof(upd_line), snap);
                sb(upd_line, sizeof(upd_line), ".tmp");
                if (ao_copy(NULL, upd_p1, upd_line) && upd_remove(snap))
                    (VOID)Rename((CONST_STRPTR)upd_line, (CONST_STRPTR)snap);
            }
            sb(sec, cap, "If EXISTS \"");
            sb(sec, cap, upd_p1);
            sb(sec, cap, "\"\n  \"");
            sb(sec, cap, upd_p2);
            sb(sec, cap, "\" \"");
            sb(sec, cap, upd_p1);
            sb(sec, cap, "\" \"");
            sb(sec, cap, snap);
            sb(sec, cap, "\" BYTES\n  If NOT WARN\n    Delete \"");
            sb(sec, cap, upd_p1);
            sb(sec, cap, "\" QUIET\n  EndIf\nEndIf\n");
        }
        for (i = 0; i < f->j.nfiles; i++)
        {
            if (f->e[i].why != UPD_WHY_OTHERBUILD)
                continue;
            if (!upd_sidecar(upd_p1, UPD_PATH, f->e[i].path, f->e[i].len,
                             f->e[i].other ? ".otherbuild" : ".old"))
                continue;
            sb(sec, cap, "If EXISTS \"");
            sb(sec, cap, upd_p1);
            sb(sec, cap, "\"\n  \"");
            sb(sec, cap, upd_p2);
            sb(sec, cap, "\" \"");
            sb(sec, cap, upd_p1);
            sb(sec, cap, "\" AmiNetXDuo MARK\n  If NOT WARN\n    Delete \"");
            sb(sec, cap, upd_p1);
            sb(sec, cap, "\" QUIET\n  EndIf\nEndIf\n");
        }
        sb(sec, cap, UPD_REC_END "\n");
        n = upd_record_merge((const char *)old, len, sec, strlen(sec), out, len + cap);
        if (n != 0 && upd_write_file(UPD_RECORD ".tmp", out, n) &&
            DeleteFile((CONST_STRPTR)UPD_RECORD))
            (VOID)Rename((CONST_STRPTR)UPD_RECORD ".tmp", (CONST_STRPTR)UPD_RECORD);
    }
    if (out != NULL)
        FreeVec(out);
    if (sec != NULL)
        FreeVec(sec);
    FreeVec(old);
}

/*
 * S:AmiNetXDuo-Install's version= set to the version now installed (the
 * state's), every other line kept: .tmp, then renamed; a lone .tmp a cut
 * left is the file.  Not there: nothing to do.
 */
#define UPD_INSTALL_INFO "S:AmiNetXDuo-Install"

static VOID upd_install_info_version(VOID)
{
    static char out[4096 + UPD_VER_MAX + 16], line[UPD_VER_MAX + 16];
    UBYTE      *t;
    ULONG       len, n;

    switch (upd_pair_plan(ao_exists(NULL, UPD_INSTALL_INFO),
                          ao_exists(NULL, UPD_INSTALL_INFO ".tmp")))
    {
    case UPD_PAIR_RECOVER:
        if (!Rename((CONST_STRPTR)UPD_INSTALL_INFO ".tmp",
                    (CONST_STRPTR)UPD_INSTALL_INFO))
            return;
        break;
    case UPD_PAIR_READ:
        break;
    default:
        return;
    }
    t = upd_slurp(UPD_INSTALL_INFO, 4096UL, &len);
    if (t == NULL)
        return;
    line[0] = '\0';
    sb(line, sizeof(line), "version=");
    sb(line, sizeof(line), upd_state_value("version"));
    n = upd_prefs_set((const char *)t, len, line, out, sizeof(out));
    FreeVec(t);
    if (n != 0 && upd_write_file(UPD_INSTALL_INFO ".tmp", out, n) &&
        DeleteFile((CONST_STRPTR)UPD_INSTALL_INFO))
        (VOID)Rename((CONST_STRPTR)UPD_INSTALL_INFO ".tmp",
                     (CONST_STRPTR)UPD_INSTALL_INFO);
}

/* The trial committed: the .old rules, receipt-next as the receipt, the
   record, phase=confirmed.  Each step repeatable: BOOT runs it again after a
   cut, from phase=commit. */
static LONG upd_commit_run(VOID)
{
    UpdJFile    *fp = &upd_jf;
    unsigned int failed, i;
    LONG         rc = RETURN_OK;

    upd_in_dir(upd_journal_path, "trial");
    upd_jload(fp);
    if (fp->unread || (fp->text != NULL && !fp->ok))
    {
        upd_jfree(fp);
        return refuse("pending", "the trial file cannot be read");
    }
    if (fp->ok)
    {
        upd_prior_dir(upd_p1);
        upd_cut_phase = 3;
        if (upd_commit(fp->e, fp->j.nfiles, upd_p1, &upd_amiga_ops, &failed) != 1)
        {
            tool_error("the commit stopped at %s: the next boot finishes it",
                       (LONG)(failed < fp->j.nfiles ? fp->e[failed].path : ""));
            upd_jfree(fp);
            return RETURN_WARN;
        }
        for (i = 0; i < fp->j.nfiles; i++)
            if (upd_commit_stays(&fp->e[i]))
            {
                upd_line[0] = '\0';
                sb(upd_line, sizeof(upd_line), "file=keep otherbuild ");
                if (upd_sidecar(upd_p2, UPD_PATH, fp->e[i].path, fp->e[i].len, ".old"))
                    sb(upd_line, sizeof(upd_line), upd_p2);
                sb(upd_line, sizeof(upd_line), " prior=prior-old/");
                sbu(upd_line, sizeof(upd_line), i + 1);
                say(upd_line, NULL, NULL);
            }
    }

    /* The receipt goes only for a receipt-next that is surely there: a
       look that cannot be answered leaves both, for the next boot. */
    upd_in_dir(upd_p1, UPD_N_RECEIPT_NEXT);
    upd_in_dir(upd_p2, UPD_N_RECEIPT);
    switch (ao_exists(NULL, upd_p1))
    {
    case 0:
        break;
    case 1:
        if (upd_remove(upd_p2) && Rename((CONST_STRPTR)upd_p1, (CONST_STRPTR)upd_p2))
            break;
        /* fall through */
    default:
        tool_error("%s could not be renamed", (LONG)upd_p1);
        rc = RETURN_WARN;
        break;
    }
    /* The record from the receipt as it is now: not after a rename that
       failed, which the next boot does again first. */
    if (rc == RETURN_OK && fp->ok)
        upd_record(fp);
    if (rc == RETURN_OK)
        upd_install_info_version();

    tool_copy_string(upd_ver_text, sizeof(upd_ver_text), upd_state_value("version"));
    if (rc == RETURN_OK &&
        !upd_state_put("confirmed", upd_ver_text,
                       upd_atoul(upd_state_value("boots")), NULL))
        rc = RETURN_WARN;
    if (rc == RETURN_OK)
    {
        upd_in_dir(upd_p1, "trial");
        (VOID)upd_remove(upd_p1);
        upd_in_dir(upd_p1, "prior-old");
        (VOID)DeleteFile((CONST_STRPTR)upd_p1);    /* only when empty */
        say("confirm=confirmed version=", upd_ver_text, NULL);
    }
    upd_jfree(fp);
    return rc;
}

/* The trial rolled back: every .old live again, every prior .old back,
   receipt-next dropped (the receipt is the one before), phase=rolledback;
   CHECK skips that version from then on. */
static LONG upd_rollback_run(VOID)
{
    UpdJFile    *fp = &upd_jf;
    unsigned int failed;

    upd_in_dir(upd_journal_path, "trial");
    upd_jload(fp);
    if (fp->unread || (fp->text != NULL && !fp->ok))
    {
        upd_jfree(fp);
        return refuse("pending", "the trial file cannot be read");
    }
    if (fp->ok)
    {
        upd_prior_dir(upd_p1);
        upd_cut_phase = 2;
        if (upd_rollback(fp->e, fp->j.nfiles, upd_p1, &upd_amiga_ops, &failed) != 1)
        {
            tool_error("the rollback stopped at %s: the next boot finishes it",
                       (LONG)(failed < fp->j.nfiles ? fp->e[failed].path : ""));
            upd_jfree(fp);
            return RETURN_WARN;
        }
    }
    upd_jfree(fp);

    upd_in_dir(upd_p1, "receipt-next");
    (VOID)upd_remove(upd_p1);
    tool_copy_string(upd_ver_text, sizeof(upd_ver_text), upd_state_value("version"));
    if (!upd_state_put("rolledback", upd_ver_text,
                       upd_atoul(upd_state_value("boots")), NULL))
        return RETURN_WARN;
    upd_in_dir(upd_p1, "trial");
    (VOID)upd_remove(upd_p1);
    upd_in_dir(upd_p1, "prior-old");
    (VOID)DeleteFile((CONST_STRPTR)upd_p1);
    say("boot=rolledback version=", upd_ver_text, NULL);
    return RETURN_OK;
}

/*
 * BOOT, before anything opens bsdsocket.library: a journal with "ready"
 * finished (the swap that a cut stopped), one without it undone with the
 * rest of a stopped PREPARE, then the trial's boots counted.
 */
static LONG upd_boot(VOID)
{
    UpdJFile    *fp = &upd_jf;
    unsigned int failed;
    ULONG        boots;
    BOOL         stale = FALSE;
    LONG         rc;

    if (!upd_find_udir())
        return RETURN_OK;
    rc = upd_lock_take();
    if (rc >= 0)
        return rc;
    upd_stop_read();

    upd_in_dir(upd_journal_path, "journal");
    upd_jload(fp);
    if (fp->unread)
    {
        /* Not read is not absent: nothing is undone or counted on it. */
        upd_jfree(fp);
        return refuse("pending", "the journal cannot be read");
    }
    if (fp->ok && fp->j.ready)
    {
        upd_prior_dir(upd_p1);
        upd_cut_phase = 1;
        if (upd_forward(fp->e, fp->j.nfiles, upd_p1, &upd_amiga_ops, &failed) != 1)
        {
            tool_error("the swap could not be finished at %s",
                       (LONG)(failed < fp->j.nfiles ? fp->e[failed].path : ""));
            upd_jfree(fp);
            return RETURN_WARN;
        }
        upd_ver_format(&fp->j.version, upd_ver_text);
        if (upd_state_put("trial", upd_ver_text, 0, upd_flavour_name(fp->j.flavour)))
            upd_in_dir(upd_p2, "trial");
        else
            upd_p2[0] = '\0';
        if (upd_p2[0] == '\0' || !upd_remove(upd_p2) ||
            !Rename((CONST_STRPTR)upd_journal_path, (CONST_STRPTR)upd_p2))
        {
            tool_error("%s could not be renamed", (LONG)upd_journal_path);
            upd_jfree(fp);
            return RETURN_WARN;
        }
        upd_say_version("boot=forward version=", &fp->j.version);
    }
    upd_jfree(fp);

    if (!upd_state_load())
        return refuse("pending", "the update state cannot be read");
    if (upd_phase_is("trial"))
    {
        tool_copy_string(upd_ver_text, sizeof(upd_ver_text), upd_state_value("version"));
        boots = upd_atoul(upd_state_value("boots")) + 1;
        if (boots > UPD_BOOTS_MAX)
        {
            if (!upd_state_put("rollback", upd_ver_text, boots, NULL))
                return RETURN_WARN;
            return upd_rollback_run();
        }
        if (!upd_state_put("trial", upd_ver_text, boots, NULL))
            return RETURN_WARN;
        upd_line[0] = '\0';
        sb(upd_line, sizeof(upd_line), "boot=trial boots=");
        sbu(upd_line, sizeof(upd_line), boots);
        sb(upd_line, sizeof(upd_line), " version=");
        sb(upd_line, sizeof(upd_line), upd_ver_text);
        say(upd_line, NULL, NULL);
        return RETURN_OK;
    }
    if (upd_phase_is("rollback"))
        return upd_rollback_run();
    if (upd_phase_is("commit"))
        return upd_commit_run();

    /* A commit or rollback cut after its state and before its trial file
       went: the state says it is over, so the trial file goes now. */
    if (upd_phase_is("confirmed") || upd_phase_is("rolledback"))
    {
        upd_in_dir(upd_p1, "trial");
        if (upd_exists(upd_p1) && upd_remove(upd_p1))
            stale = TRUE;
    }
    if (upd_clean_stale() >= 0)
        return RETURN_WARN;
    if (upd_removed || stale)
        say("boot=undone", NULL, NULL);
    return RETURN_OK;
}

/*
 * One of our interfaces has an IPv4 address that is not loopback: SIOCGIFCONF
 * on the stack that is running.  Nothing is opened that is not open already:
 * a stack that did not start is not started here.
 */
#define UPD_SIOCGIFCONF     0xC0086924UL    /* _IOWR('i', 36, struct ifconf) */
#define UPD_IFREQ           32UL

static BOOL upd_online(VOID)
{
    static UBYTE    ifbuf[UPD_IFREQ * 8];
    struct { LONG len; APTR buf; } ifc;
    struct Library *lib;
    BOOL            up = FALSE;
    LONG            s, i;

    Forbid();
    lib = (struct Library *)FindName(&SysBase->LibList,
                                     (CONST_STRPTR)"bsdsocket.library");
    Permit();
    if (lib == NULL)
        return FALSE;
    lib = OpenLibrary((CONST_STRPTR)"bsdsocket.library", 4UL);
    if (lib == NULL)
        return FALSE;
    if (tool_stack_is_ours(lib) &&
        (s = tool_sock_socket(lib, 2 /* AF_INET */, 2 /* SOCK_DGRAM */, 0)) >= 0)
    {
        ifc.len = (LONG)sizeof(ifbuf);
        ifc.buf = ifbuf;
        if (tool_sock_ioctl(lib, s, UPD_SIOCGIFCONF, &ifc) == 0)
            for (i = 0; i + (LONG)UPD_IFREQ <= ifc.len; i += (LONG)UPD_IFREQ)
            {
                const UBYTE *a = ifbuf + i + 20;    /* sin_addr            */

                if ((a[0] | a[1] | a[2] | a[3]) != 0 && a[0] != 127)
                    up = TRUE;
            }
        (VOID)tool_sock_close(lib, s);
    }
    CloseLibrary(lib);
    return up;
}

/* CONFIRM, after S:Network-Startup: a trial whose network came up is
   committed; one whose did not stays on trial, and BOOT counts it. */
static LONG upd_confirm(VOID)
{
    ULONG t;
    LONG  rc;

    if (!upd_find_udir())
        return RETURN_OK;
    /* A peek, unlocked and writing nothing: what cannot be read is
       looked at again under the lock. */
    if (upd_state_load_as(FALSE) && !upd_phase_is("commit") &&
        !upd_phase_is("trial"))
        return RETURN_OK;               /* nothing to lock for           */
    rc = upd_lock_take();
    if (rc >= 0)
        return rc;
    if (!upd_state_load())
        return refuse("pending", "the update state cannot be read");
    if (upd_phase_is("commit"))
        return upd_commit_run();
    if (!upd_phase_is("trial"))
        return RETURN_OK;

    for (t = 0; !upd_online(); t++)
    {
        if (t >= UPD_CONFIRM_WAIT || tool_delay_ticks(50))
        {
            say("confirm=timeout version=", upd_state_value("version"), NULL);
            return RETURN_OK;
        }
    }
    tool_copy_string(upd_ver_text, sizeof(upd_ver_text), upd_state_value("version"));
    if (!upd_state_put("commit", upd_ver_text, upd_atoul(upd_state_value("boots")), NULL))
        return RETURN_WARN;
    return upd_commit_run();
}

/* BOOT and CONFIRM: no requester, and at most a warning for the startup. */
static LONG upd_startup_word(VOID)
{
    struct Process *me = (struct Process *)FindTask(NULL);
    APTR            saved = me->pr_WindowPtr;
    LONG            rc;

    me->pr_WindowPtr = (APTR)-1L;
    rc = upd_run_args.word[0] == 'B' ? upd_boot() : upd_confirm();
    upd_lock_drop();
    me->pr_WindowPtr = saved;
    return rc > RETURN_WARN ? RETURN_WARN : rc;
}

static LONG upd_main(VOID)
{
    /* The swap finished or undone whatever stack is to run: BOOT runs
       before any opens. */
    if (upd_run_args.word[0] == 'B')
        return upd_startup_word();

    /* Another stack is active: nothing at all, not even the check. */
    if (tool_stack_foreign_loaded())
    {
        say("update=foreign_stack", NULL, NULL);
        return RETURN_OK;
    }

    if (upd_run_args.word[0] == 'C' && upd_run_args.word[1] == 'O')
        return upd_startup_word();

    /* Fail safe: a build that holds no key accepts nothing. */
    if (upd_key_count == 0)
        return refuse("unknown_key", "this build holds no release key");

    {
        struct Process *me = (struct Process *)FindTask(NULL);
        APTR            saved = me->pr_WindowPtr;
        LONG            rc = -1;

        /* No "volume is write protected" or "insert volume" requester on
           any path to the Update drawer, CHECK ANSWER=INSTALL's too: every
           refusal is a line. */
        me->pr_WindowPtr = (APTR)-1L;
        if (upd_run_args.word[0] == 'I')
        {
            rc = upd_install_begin();
            if (rc < 0)
                rc = upd_check(TRUE);
        }
        else
            rc = upd_check(FALSE);
        upd_install_end();
        me->pr_WindowPtr = saved;
        return rc;
    }
}

/*
 * Hazard: upd_trampoline() must have no locals and no arguments, and must
 * stay noinline; see fetch.c.  Its size is upd_stack_size()'s: every
 * word that fetches gets fetch's 64 KB, as an http: mirror can redirect
 * to https: (tls.library).
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
        tool_usage("CHECK [QUIET] [ANSWER=INSTALL|LATER|SKIP] | INSTALL [RESTART]"
                   " | BOOT | CONFIRM",
                   "Check the mirror for a newer AmiNetXDuo, or install it.  "
                   "BOOT and CONFIRM are S:User-Startup's steps after it.");
        return RETURN_ERROR;
    }

    upd_run_args.quiet   = args[ARG_QUIET] != 0 ? TRUE : FALSE;
    upd_run_args.restart = args[ARG_RESTART] != 0 ? TRUE : FALSE;
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

    if ((args[ARG_CHECK] != 0) + (args[ARG_INSTALL] != 0)
        + (args[ARG_BOOT] != 0) + (args[ARG_CONFIRM] != 0) > 1)
    {
        tool_error("give one of CHECK, INSTALL, BOOT and CONFIRM");
        FreeArgs(rda);
        return RETURN_ERROR;
    }
    if (args[ARG_RESTART] != 0 && args[ARG_INSTALL] == 0)
    {
        tool_error("RESTART goes with INSTALL");
        FreeArgs(rda);
        return RETURN_ERROR;
    }

    word = args[ARG_INSTALL] ? "INSTALL" : args[ARG_BOOT] ? "BOOT"
         : args[ARG_CONFIRM] ? "CONFIRM" : "CHECK";
    tool_copy_string(upd_run_args.word, sizeof(upd_run_args.word), word);
    FreeArgs(rda);

    if (upd_run_args.quiet)
        tool_quiet_begin();

    upd_result = RETURN_FAIL;
    stack_size = upd_stack_size(upd_run_args.word);
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
    if (IntuitionBase != NULL)
        CloseLibrary((struct Library *)IntuitionBase);

    upd_log_write(upd_run_args.word);

    /* QUIET silences; it does not change the contract's return codes. */
    (VOID)tool_quiet_end(RETURN_OK);

    /* INSTALL RESTART, installed: every volume flushed first. */
    if (upd_reboot)
        tool_reboot();

    return (int)upd_result;
}

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
 * BOOT, CONFIRM and the SKIP answer are refused as not implemented until
 * their part of #497 lands.
 *
 *   rc 0   update=current, update=off, update=foreign_stack,
 *          update=installed
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

#include "aminetxduo/version.h"

#include <string.h>

#include "c68k_sha256.h"
#include "fetchcore.h"
#include "tool_reboot.h"
#include "updcheck.h"
#include "updinstall.h"
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
#define UPD_PATH            300

/* The Update drawer, by its volume: upd_find_udir(). */
static char upd_udir[UPD_PATH];
static BOOL upd_find_udir(VOID);
static VOID upd_in_dir(char *out, const char *name);

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
    BPTR            fh;
    static char     path[UPD_PATH];     /* main()'s, on the Shell's stack */

    if (upd_said_len == 0)
        return;

    /* No "insert volume AmiNetXDuo:" requester without the assign.  The
       drawer is the installer's to make: CHECK creates nothing. */
    me->pr_WindowPtr = (APTR)-1L;
    if (upd_find_udir())
    {
        upd_in_dir(path, "log");
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
    BOOL  restart;
    LONG  answer;
    char  word[8];
} UpdRun;

static UpdRun      upd_run_args;
static UpdSettings upd_set;
static UpdBuf      upd_manifest_buf, upd_sig_buf;
static char        upd_url_buf[UPD_MIRROR_MAX + 96];
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

/*
 * The Update drawer: AmiNetXDuo:Installed/Update, or, in the system layout,
 * which assigns no AmiNetXDuo:, <DOCDIR>/Installed/Update with DOCDIR from
 * the T:AmiNetXDuo-DrawerKept line of S:AmiNetXDuo-Files (the record
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
                {
                    upd_p1[0] = '\0';
                    sbn(upd_p1, UPD_PATH, (const char *)t + i, j - i);
                    sb(upd_p1, UPD_PATH, upd_p1[strlen(upd_p1) - 1] == ':'
                                         ? "Installed/Update" : "/Installed/Update");
                    lock = Lock((CONST_STRPTR)upd_p1, SHARED_LOCK);
                }
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

static BOOL upd_exists(const char *path)
{
    BPTR l = Lock((CONST_STRPTR)path, SHARED_LOCK);

    if (l == (BPTR)0)
        return FALSE;
    UnLock(l);
    return TRUE;
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
static ULONG upd_cuts[2];

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

/* A cut point of PREPARE (0) or SWAP (1): the one named stops here, as a
   power cut would, after saying so. */
static VOID upd_cut(ULONG phase)
{
    static const char *const names[] = { "prepare", "swap" };
    ULONG n = ++upd_cuts[phase];

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
static LONG upd_clean_stale(VOID)
{
    struct FileInfoBlock *fib;
    BOOL                  removed = FALSE;
    UBYTE                *t;
    ULONG                 len, i;
    BPTR                  dir;

    upd_in_dir(upd_p1, "state");
    t = upd_slurp(upd_p1, UPD_STATE_MAX, &len);
    upd_in_dir(upd_p1, "trial");
    i = (ULONG)upd_trial_active(upd_exists(upd_p1), (const char *)t,
                                t != NULL ? len : 0);
    if (t != NULL)
        FreeVec(t);
    if (i)
        return refuse("pending", "an update is installed and not confirmed "
                                 "yet: restart the Amiga");

    upd_in_dir(upd_p1, "journal");
    t = upd_slurp(upd_p1, 64UL * 1024UL, &len);
    if (t != NULL)
    {
        UpdJournal j;
        UpdJEntry *e = (UpdJEntry *)AllocVec(UPD_FILES_MAX * sizeof(UpdJEntry), MEMF_ANY);

        if (e == NULL || !upd_journal_parse((const char *)t, len, &j, e, UPD_FILES_MAX) ||
            j.ready)
        {
            if (e != NULL)
                FreeVec(e);
            FreeVec(t);
            return refuse("pending", "an update is half applied: restart the "
                                     "Amiga to finish it");
        }
        for (i = 0; i < j.nfiles; i++)
        {
            upd_p2[0] = '\0';
            sbn(upd_p2, UPD_PATH, e[i].path, e[i].len);
            sb(upd_p2, UPD_PATH, ".new");
            (VOID)upd_remove(upd_p2);
        }
        FreeVec(e);
        FreeVec(t);
        (VOID)upd_remove(upd_p1);
        removed = TRUE;
    }

    upd_in_dir(upd_p1, "journal.tmp");
    if (upd_exists(upd_p1))
        removed = upd_remove(upd_p1) || removed;
    upd_in_dir(upd_p1, "receipt-next");
    if (upd_exists(upd_p1))
        removed = upd_remove(upd_p1) || removed;

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

                if (fib->fib_DirEntryType < 0 && n > 11 &&
                    tool_stricmp((const char *)fib->fib_FileName + n - 11,
                                 ".update.lha") == 0)
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
    return -1;
}

/* The receipt and the Update drawer, before anything is fetched. */
static LONG upd_install_begin(VOID)
{
    if (!upd_find_udir())
        return refuse("no_receipt", "no install receipt: run Install-AmiNetXDuo");

    upd_in_dir(upd_p1, "receipt");
    upd_rcpt_text = upd_slurp(upd_p1, UPD_RCPT_FILE_MAX, &upd_rcpt_len);
    upd_rcpt = (UpdRcpt *)AllocVec(UPD_RCPT_MAX * sizeof(UpdRcpt), MEMF_ANY);
    if (upd_rcpt_text == NULL || upd_rcpt == NULL ||
        !upd_receipt_parse((const char *)upd_rcpt_text, upd_rcpt_len, upd_rcpt,
                           UPD_RCPT_MAX, &upd_nrcpt))
        return refuse("no_receipt", "no install receipt: run Install-AmiNetXDuo");

    upd_stop_read();
    return upd_clean_stale();
}

static VOID upd_install_end(VOID)
{
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
    upd_vols[i].free  = (ULONG)(upd_info.id_NumBlocks - upd_info.id_NumBlocksUsed);
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
static BOOL upd_protected(const char *path, ULONG *size)
{
    struct FileInfoBlock *fib = (struct FileInfoBlock *)AllocDosObject(DOS_FIB, NULL);
    BPTR                  l = Lock((CONST_STRPTR)path, SHARED_LOCK);
    BOOL                  p = FALSE;

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

static int ao_exists(void *c, const char *p) { (VOID)c; return upd_exists(p); }
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

static int ao_cut(void *c)
{
    (VOID)c;
    upd_cut(1);
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

/* Removes everything PREPARE staged. */
static VOID upd_unstage(UpdInst *in)
{
    ULONG i;

    for (i = 0; i < in->m->nfiles; i++)
        if (in->pick[i].act == UPD_ACT_REPLACE && in->pick[i].real != NULL)
        {
            upd_p2[0] = '\0';
            sb(upd_p2, UPD_PATH, in->pick[i].real);
            sb(upd_p2, UPD_PATH, ".new");
            (VOID)upd_remove(upd_p2);
        }
    (VOID)upd_remove(in->cpath);
    upd_in_dir(upd_p1, "receipt-next");
    (VOID)upd_remove(upd_p1);
    upd_in_dir(upd_p1, "journal.tmp");
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
        if (in->pick[idx].act != UPD_ACT_REPLACE)
        {
            if (Seek(fh, (LONG)lm.size, OFFSET_CURRENT) < 0)
            {
                rc = refuse("member_hash", NULL);
                break;
            }
            continue;
        }

        upd_p2[0] = '\0';
        sb(upd_p2, UPD_PATH, in->pick[idx].real);
        sb(upd_p2, UPD_PATH, ".new");
        (VOID)upd_remove(upd_p2);
        out = Open((CONST_STRPTR)upd_p2, MODE_NEWFILE);
        if (out == (BPTR)0)
        {
            rc = refuse_at("space", upd_p2, NULL);
            break;
        }
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
    if (rc < 0 && got != in->nrep)
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
    upd_in_dir(upd_p1, "receipt-next");
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
    upd_in_dir(upd_p1, "journal.tmp");
    ok = upd_write_file(upd_p1, t, at) &&
         Rename((CONST_STRPTR)upd_p1, (CONST_STRPTR)upd_journal_path);
    FreeVec(t);
    return ok;
}

/* phase=trial with the rest of the state kept: state.tmp, then renamed. */
static BOOL upd_state_trial(UpdInst *in, const UpdWant *want)
{
    static char out[UPD_STATE_MAX];
    UBYTE      *old;
    ULONG       oldlen, n;
    BOOL        ok;

    upd_in_dir(upd_p2, "state");
    old = upd_slurp(upd_p2, UPD_STATE_MAX, &oldlen);
    n = upd_state_merge(old != NULL ? (const char *)old : "", oldlen, "trial",
                        in->newv, 0, upd_flavour_name(want->flavour), out,
                        sizeof(out));
    if (old != NULL)
        FreeVec(old);
    if (n == 0)
        return FALSE;
    upd_in_dir(upd_p1, "state.tmp");
    ok = upd_write_file(upd_p1, out, n);
    (VOID)upd_remove(upd_p2);
    return ok && Rename((CONST_STRPTR)upd_p1, (CONST_STRPTR)upd_p2);
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
    upd_in_dir(upd_journal_path, "journal");

    in.pick = (UpdPick *)AllocVec(m->nfiles * sizeof(UpdPick) + 1, MEMF_ANY | MEMF_CLEAR);
    in.pool = (char *)AllocVec(m->nfiles * UPD_PATH, MEMF_ANY);
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

    /* ---- refused as a whole, before anything is staged ---- */
    if (upd_vols[upd_vol].state == ID_WRITE_PROTECTED)
    {
        rc = refuse_at("writeprotected", upd_udir, NULL);
        goto out;
    }
    for (i = 0, v = 0; i < m->nfiles; i++)
    {
        UpdPick  *p = &in.pick[i];
        UpdJEntry je;

        if (p->act != UPD_ACT_REPLACE)
            continue;
        upd_dest(upd_p1, UPD_PATH, layout, files[i].cls, files[i].path,
                 files[i].path_len);
        /* The physical path, which an assign can make longer than the
           journal and the swap's names hold. */
        je.path = p->real;
        je.len  = (unsigned short)strlen(p->real);
        upd_in_dir(upd_p2, "prior-old");
        if (!upd_swap_fits(&je, (unsigned int)v++, upd_p2))
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
        upd_p2[0] = '\0';
        sb(upd_p2, UPD_PATH, p->real);
        sb(upd_p2, UPD_PATH, ".new");
        if (upd_protected(upd_p2, NULL))
        {
            rc = refuse_at("protected", upd_p2, NULL);
            goto out;
        }
        upd_p2[strlen(upd_p2) - 4] = '\0';
        sb(upd_p2, UPD_PATH, ".old");
        if (upd_exists(upd_p2))
        {
            if (upd_protected(upd_p2, &p->old_size))
            {
                rc = refuse_at("protected", upd_p2, "an existing .old cannot "
                                                    "be moved aside");
                goto out;
            }
            p->has_old = 1;
            in.nold++;
            if (p->vol != upd_vol)
                upd_vols[upd_vol].need += upd_blocks(p->old_size, upd_vols[upd_vol].bpb);
        }
        upd_vols[p->vol].need += upd_blocks(files[i].size, upd_vols[p->vol].bpb);
    }
    upd_vols[upd_vol].need += upd_blocks(m->container_size, upd_vols[upd_vol].bpb);
    for (i = 0; i < upd_nvols; i++)
        if (upd_vols[i].need > upd_vols[i].free)
        {
            rc = refuse_at("space", upd_vols[i].name, "free space on that volume");
            goto out;
        }

#ifdef UPD_TEST_KEY
    upd_line[0] = '\0';
    sb(upd_line, sizeof(upd_line), "steps.prepare=");
    sbu(upd_line, sizeof(upd_line), (in.nrep != 0 ? 1 : 0) + 1 + in.nrep + 1);
    sb(upd_line, sizeof(upd_line), " steps.swap=");
    sbu(upd_line, sizeof(upd_line), upd_forward_steps(in.nrep, in.nold) + 2);
    say(upd_line, NULL, NULL);
#endif

    /* ---- PREPARE ---- */
    tool_copy_string(in.cname, sizeof(in.cname), "");
    sbn(in.cname, sizeof(in.cname), m->container, m->container_len);
    upd_in_dir(in.cpath, in.cname);

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
    in.je = (UpdJEntry *)AllocVec((in.nrep + 1) * sizeof(UpdJEntry), MEMF_ANY | MEMF_CLEAR);
    if (in.je == NULL)
    {
        tool_fault(ERROR_NO_FREE_STORE);
        rc = UPD_RC_FAILED;
        goto out;
    }
    for (i = 0, v = 0; i < m->nfiles; i++)
        if (in.pick[i].act == UPD_ACT_REPLACE)
        {
            in.je[v].path = in.pick[i].real;
            in.je[v].len  = (unsigned short)strlen(in.pick[i].real);
            in.je[v].why  = in.pick[i].why;
            v++;
        }
    upd_in_dir(upd_p1, "prior-old");
    if (in.nold != 0 && !upd_exists(upd_p1))
    {
        l = CreateDir((CONST_STRPTR)upd_p1);
        if (l != (BPTR)0)
            UnLock(l);
    }
    if (upd_forward(in.je, in.nrep, upd_p1, &upd_amiga_ops, &failed) != 1)
    {
        tool_error("the swap stopped at %s: restart, and the next boot "
                   "finishes it", (LONG)(failed < in.nrep ? in.je[failed].path : ""));
        rc = UPD_RC_FAILED;
        goto out;
    }
    if (!upd_state_trial(&in, want))
    {
        tool_error("%s could not be written", (LONG)"state");
        rc = UPD_RC_FAILED;
        goto out;
    }
    upd_cut(1);
    upd_in_dir(upd_p1, "trial");
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
        if (!install && upd_run_args.answer != ANSWER_NONE)
        {
            say("requester=offer", NULL, NULL);
            if (upd_run_args.answer == ANSWER_LATER)
                say("update=later version=", upd_ver_text, NULL);
            else if (upd_run_args.answer == ANSWER_INSTALL)
            {
                rc = upd_install_begin();
                if (rc < 0)
                    rc = upd_install(mirror, &want, &m, files);
            }
            else
                rc = refuse("not_implemented", "ANSWER=SKIP is not "
                                               "implemented yet");
        }
        else if (install)
            rc = upd_install(mirror, &want, &m, files);
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

    if (upd_run_args.word[0] != 'C' && upd_run_args.word[0] != 'I')
        return refuse("not_implemented", "BOOT and CONFIRM are not "
                                         "implemented yet");

    /* Fail safe: a build that holds no key accepts nothing. */
    if (upd_key_count == 0)
        return refuse("unknown_key", "this build holds no release key");

    if (upd_run_args.word[0] == 'I')
    {
        struct Process *me = (struct Process *)FindTask(NULL);
        APTR            saved = me->pr_WindowPtr;
        LONG            rc;

        /* No "volume is write protected" or "insert volume" requester:
           every refusal is a line. */
        me->pr_WindowPtr = (APTR)-1L;
        rc = upd_install_begin();
        if (rc < 0)
            rc = upd_check(TRUE);
        upd_install_end();
        me->pr_WindowPtr = saved;
        return rc;
    }

    {
        LONG rc = upd_check(FALSE);

        upd_install_end();
        return rc;
    }
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
        tool_usage("CHECK [QUIET] [ANSWER=INSTALL|LATER|SKIP] | INSTALL [RESTART]",
                   "Check the mirror for a newer AmiNetXDuo, or install it.  "
                   "BOOT and CONFIRM are the steps after it.");
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

    /* INSTALL RESTART, installed: every volume flushed first. */
    if (upd_reboot)
        tool_reboot();

    return (int)upd_result;
}

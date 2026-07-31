/*
 * FitzStress -- four concurrent AmigaDOS copies over ONE TCP connection.
 *
 * WHOSE TEST THIS IS
 *
 *   The author of Fitz reproduces freezes in Amiga TCP/IP stacks with four
 *   processes at once, described in his own words:
 *
 *     process 1  copies a 10 MB file from a fileserver to RAM:, endlessly
 *     process 2  does the same in the opposite direction
 *     process 3  copies a 100 MB file alternately fileserver -> harddisk
 *                and back
 *     process 4  copies an Amiga system installation to the fileserver
 *
 *   "This all goes through a single TCP connection for the mount, which exerts
 *   considerable stress on the TCP/IP stack and its internal buffers.  I've
 *   written the tests originally for Fitz, but it revealed more problems with
 *   TCP/IP stacks than with the filesystem."
 *
 * WHY IT IS NOT tests/endurance
 *
 *   Endurance opens N sockets and drives each one.  This opens ZERO: every
 *   byte goes through the Fitz handler's one socket, which is the property the
 *   whole test turns on.  Four DOS clients queue packets at one handler, the
 *   handler serialises them onto one connection, and the send and receive
 *   buffers of that connection carry a 10 MB read, a 10 MB write, a 100 MB
 *   transfer and a directory walk interleaved at DOS-packet granularity.  A
 *   window, a buffer or a retransmit queue that only misbehaves under that
 *   interleaving cannot be reached with a socket per workload.
 *
 *   This harness therefore opens no bsdsocket.library at all.  If it ever
 *   needs one, the test has stopped being this test.
 *
 * WHAT IT CHECKS
 *
 *   Correctness is not an afterthought here: another reading of the same
 *   reports is that the stack delivers wrong bytes before it stops delivering
 *   any, and a silent corruption would be worse than the hang.  So every byte
 *   that comes off the share is checked against a position-addressable
 *   pattern as it arrives --
 *
 *       byte(o) = pat[o & 8191] ^ (UBYTE)(o >> 13)
 *
 *   -- which catches a single altered byte, and, because the period is 2 MB,
 *   a repeated or dropped block as well.  The host side knows the same
 *   pattern and checks everything the guest wrote to the share.  The tree
 *   worker is checked by Fitz's own `comparetree ... crc`, run from the
 *   supervisor against a copy the worker has finished with -- at the end by
 *   default, because it is minutes of the same connection and no health
 *   sample is taken while it runs.
 *
 * WHAT IT RECORDS
 *
 *   DH0:stress-timeline.csv   one row per sample: per-worker iterations,
 *                             kilobytes, errors and mismatches, plus AvailMem
 *   DH0:stress-events.txt     every error, with the second it happened
 *   DH0:health.log            `netstat -h` blocks, the leak and pool series
 *   DH0:stress-summary.txt    totals, and which worker was where at the end
 *   serial                    one line per sample, so a machine whose
 *                             filesystem has stopped can still be seen to be
 *                             executing -- and a machine that stops emitting
 *                             them has stopped executing
 *
 *   A worker that stops advancing its phase stamp is the freeze, and the
 *   summary names it and the DOS call it was in.  The supervisor writes that
 *   summary on its deadline whether or not the workers came back, and then
 *   parks rather than exiting, because unloading this segment out from under
 *   a wedged child would turn a diagnosable freeze into a crash.
 *
 * SPDX-License-Identifier: MIT
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/dostags.h>
#include <proto/exec.h>
#include <proto/dos.h>

static const char version_tag[] __attribute__((used)) =
    "$VER: FitzStress 1.0 (29.7.2026)";

/* --------------------------------------------------------------- serial -- */

/* RawPutChar is exec LVO -516; the NDK declares it only for the assembler. */
#ifndef RawPutChar
#  define RawPutChar(c) \
      LP1NR(0x204, RawPutChar, UBYTE, (c), d0, , EXEC_BASE_NAME)
#endif

static VOID fs_putch(register UBYTE c __asm("d0"),
                     register APTR unused __asm("a3"))
{
    (VOID)unused;

    if (c != '\0')
        RawPutChar(c);
}

/* RawDoFmt, not printf: %ld/%lu/%s only, every argument longword-sized. */
static VOID fs_kprintf(const char *fmt, LONG *args)
{
    RawDoFmt((STRPTR)fmt, args, (void (*)())fs_putch, NULL);
}

/* ---------------------------------------------------------- the pattern -- */

/*
 * 8 KB of xorshift32 output, xored with the offset's high bits.  The host
 * side (tests/stress/pattern.py) generates the same bytes from the same seed,
 * so a file can be checked at either end without either end having to send a
 * checksum the stack under test would have to carry.
 */
#define PAT_SIZE    8192

static UBYTE fs_pat[PAT_SIZE];

static VOID fs_pat_init(ULONG seed)
{
    ULONG x = (seed != 0UL) ? seed : 2463534242UL;
    LONG  i;

    for (i = 0; i < PAT_SIZE; i++)
    {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        fs_pat[i] = (UBYTE)(x & 255UL);
    }
}

static VOID fs_pat_fill(UBYTE *buf, ULONG off, ULONG len)
{
    ULONG i;

    for (i = 0UL; i < len; i++)
    {
        ULONG o = off + i;

        buf[i] = (UBYTE)(fs_pat[o & (PAT_SIZE - 1UL)] ^ (UBYTE)(o >> 13));
    }
}

/* Returns the offset of the first byte that is wrong, or -1. */
static LONG fs_pat_check(const UBYTE *buf, ULONG off, ULONG len)
{
    ULONG i;

    for (i = 0UL; i < len; i++)
    {
        ULONG o = off + i;
        UBYTE want = (UBYTE)(fs_pat[o & (PAT_SIZE - 1UL)] ^ (UBYTE)(o >> 13));

        if (buf[i] != want)
            return (LONG)i;
    }

    return -1;
}

/* ------------------------------------------------------------- the clock -- */

/*
 * Wall clock, not accumulated sample steps.
 *
 * The supervisor's loop does more than Delay(): it runs netstat, and it can
 * run comparetree, which walks a 4 MB tree with CRC over the same connection
 * four workers are hammering and takes minutes.  A clock that adds `sample`
 * per iteration therefore reads far behind real time -- a run asked for 420
 * seconds sat there for 22 minutes and reported t=300 -- and the emulator's
 * deadline arrives long before the workload's own.
 */
static struct DateStamp g_start;

static VOID fs_clock_start(VOID)
{
    DateStamp(&g_start);
}

static ULONG fs_elapsed(VOID)
{
    struct DateStamp now;
    LONG             days, mins, ticks;

    DateStamp(&now);

    days  = now.ds_Days   - g_start.ds_Days;
    mins  = now.ds_Minute - g_start.ds_Minute;
    ticks = now.ds_Tick   - g_start.ds_Tick;

    ticks += (days * 1440L + mins) * (60L * 50L);

    return (ticks > 0L) ? (ULONG)(ticks / 50L) : 0UL;
}

/* ------------------------------------------------------------ the config -- */

#define CFG_PATH        "DH0:fitzstress.cfg"
#define TIMELINE_PATH   "DH0:stress-timeline.csv"
#define EVENTS_PATH     "DH0:stress-events.txt"
#define SUMMARY_PATH    "DH0:stress-summary.txt"
#define HEALTH_PATH     "DH0:health.log"
#define COMPARE_PATH    "DH0:compare.log"

#define MAXPATH         256

typedef struct
{
    char    cf_Share[MAXPATH];      /* the Fitz mount, e.g. FITZ:            */
    char    cf_Ram[MAXPATH];        /* RAM: -- worker 1's sink, 2's source   */
    char    cf_Disk[MAXPATH];       /* a real disk -- worker 3's sink        */
    char    cf_Tree[MAXPATH];       /* the system tree worker 4 copies       */
    ULONG   cf_SmallKB;             /* workers 1 and 2                       */
    ULONG   cf_BigKB;               /* worker 3                              */
    ULONG   cf_Seconds;
    ULONG   cf_Sample;
    ULONG   cf_IoBuf;               /* bytes per Read()/Write()              */
    ULONG   cf_Seed;
    /*
     * comparetree with CRC reads the whole tree back over the same connection
     * the four workers are on, and the supervisor is inside SystemTagList()
     * for as long as it takes -- minutes under load, during which no health
     * sample is taken.  So it is off by default and the run gets one at the
     * end, once the workers have stopped.  Content is still checked
     * continuously: workers 1 and 3 verify every byte off the share against
     * the pattern, and the host checks every surviving tree slot.
     */
    ULONG   cf_Compare;             /* run comparetree every Nth sample; 0 off*/
    ULONG   cf_Workers;             /* bitmask, for bisecting a freeze       */
} StressCfg;

static StressCfg g_cfg;

static ULONG fs_atoi(const char *s)
{
    ULONG v = 0UL;

    while (*s >= '0' && *s <= '9')
        v = v * 10UL + (ULONG)(*s++ - '0');

    return v;
}

static VOID fs_copystr(char *dst, const char *src, ULONG max)
{
    ULONG i = 0UL;

    while (src[i] != '\0' && i < max - 1UL)
    {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static BOOL fs_key(const char *line, const char *key, const char **val)
{
    ULONG i = 0UL;

    while (key[i] != '\0')
    {
        if (line[i] != key[i])
            return FALSE;
        i++;
    }

    if (line[i] != ' ' && line[i] != '\t')
        return FALSE;

    while (line[i] == ' ' || line[i] == '\t')
        i++;

    *val = line + i;

    return TRUE;
}

static VOID fs_cfg_defaults(VOID)
{
    fs_copystr(g_cfg.cf_Share, "FITZ:",       MAXPATH);
    fs_copystr(g_cfg.cf_Ram,   "RAM:",        MAXPATH);
    fs_copystr(g_cfg.cf_Disk,  "DH0:work",    MAXPATH);
    fs_copystr(g_cfg.cf_Tree,  "DH0:sysimage", MAXPATH);

    g_cfg.cf_SmallKB = 4096UL;
    g_cfg.cf_BigKB   = 40960UL;
    g_cfg.cf_Seconds = 1800UL;
    g_cfg.cf_Sample  = 30UL;
    g_cfg.cf_IoBuf   = 32768UL;
    g_cfg.cf_Seed    = 20260729UL;
    g_cfg.cf_Compare = 0UL;
    g_cfg.cf_Workers = 15UL;
}

static VOID fs_cfg_load(VOID)
{
    BPTR        fh;
    /* Static, not automatic: the supervisor runs as a Shell command and gets
       the Shell's 4K stack, which a few of these do not fit in together. */
    static char line[MAXPATH + 32];
    const char *v;

    fs_cfg_defaults();

    fh = Open((CONST_STRPTR)CFG_PATH, MODE_OLDFILE);
    if (fh == (BPTR)0)
        return;

    while (FGets(fh, (STRPTR)line, (ULONG)sizeof(line)) != NULL)
    {
        LONG n = 0;

        while (line[n] != '\0')
            n++;
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r' ||
                         line[n - 1] == ' ' || line[n - 1] == '\t'))
            line[--n] = '\0';

        if (line[0] == '\0' || line[0] == '#')
            continue;

        if      (fs_key(line, "share",   &v)) fs_copystr(g_cfg.cf_Share, v, MAXPATH);
        else if (fs_key(line, "ram",     &v)) fs_copystr(g_cfg.cf_Ram,   v, MAXPATH);
        else if (fs_key(line, "disk",    &v)) fs_copystr(g_cfg.cf_Disk,  v, MAXPATH);
        else if (fs_key(line, "tree",    &v)) fs_copystr(g_cfg.cf_Tree,  v, MAXPATH);
        else if (fs_key(line, "smallkb", &v)) g_cfg.cf_SmallKB = fs_atoi(v);
        else if (fs_key(line, "bigkb",   &v)) g_cfg.cf_BigKB   = fs_atoi(v);
        else if (fs_key(line, "seconds", &v)) g_cfg.cf_Seconds = fs_atoi(v);
        else if (fs_key(line, "sample",  &v)) g_cfg.cf_Sample  = fs_atoi(v);
        else if (fs_key(line, "iobuf",   &v)) g_cfg.cf_IoBuf   = fs_atoi(v);
        else if (fs_key(line, "seed",    &v)) g_cfg.cf_Seed    = fs_atoi(v);
        else if (fs_key(line, "compare", &v)) g_cfg.cf_Compare = fs_atoi(v);
        else if (fs_key(line, "workers", &v)) g_cfg.cf_Workers = fs_atoi(v);
    }

    Close(fh);
}

/* ------------------------------------------------------------- reporting -- */

/* Opened, appended and closed per line: a run that has to be killed keeps
   everything written up to the moment it was killed. */
static VOID fs_append(const char *path, const char *fmt, LONG *args)
{
    BPTR fh = Open((CONST_STRPTR)path, MODE_READWRITE);

    if (fh == (BPTR)0)
        return;

    Seek(fh, 0, OFFSET_END);
    VFPrintf(fh, (CONST_STRPTR)fmt, (APTR)args);
    Close(fh);
}

static VOID fs_truncate(const char *path)
{
    BPTR fh = Open((CONST_STRPTR)path, MODE_NEWFILE);

    if (fh != (BPTR)0)
        Close(fh);
}

/* ------------------------------------------------------------- the workers */

#define ROLE_DOWN   1       /* share -> RAM:                                  */
#define ROLE_UP     2       /* RAM: -> share                                  */
#define ROLE_BIG    3       /* share <-> disk, alternating                    */
#define ROLE_TREE   4       /* a directory tree -> share, cloned              */

/* Where a worker is right now.  A stamp that stops moving with a phase that
   stays put names the DOS call the machine died in. */
#define PH_IDLE     0
#define PH_OPEN_SRC 1
#define PH_OPEN_DST 2
#define PH_READ     3
#define PH_WRITE    4
#define PH_CLOSE    5
#define PH_DELETE   6
#define PH_SCAN     7
#define PH_MKDIR    8
#define PH_META     9
#define PH_SETTLE   10

static const char *const fs_phase_name[] =
{
    "idle", "open-src", "open-dst", "read", "write", "close",
    "delete", "scan", "mkdir", "meta", "settle"
};

typedef struct
{
    ULONG            w_Role;
    volatile ULONG   w_Iters;
    volatile ULONG   w_KB;
    volatile ULONG   w_Errs;
    volatile ULONG   w_Bad;         /* content mismatches                     */
    volatile ULONG   w_Rem;         /* bytes not yet rolled into w_KB         */
    volatile ULONG   w_Phase;
    volatile ULONG   w_Stamp;       /* bumped on every buffer, for the wedge  */
    volatile ULONG   w_Alive;
    volatile ULONG   w_Done;
    volatile ULONG   w_Ready;       /* worker 4: highest finished tree slot   */
    struct Process  *w_Proc;
    UBYTE           *w_Buf;
} StressWorker;

/* Static, not on the supervisor's stack: a wedged child outlives the
   supervisor's deadline and must not be reading freed memory. */
static StressWorker g_worker[4];
static volatile ULONG g_stop;
static volatile ULONG g_secs;

static VOID fs_event(ULONG role, const char *what, LONG a, LONG b)
{
    LONG args[5];

    args[0] = (LONG)g_secs;
    args[1] = (LONG)role;
    args[2] = (LONG)what;
    args[3] = a;
    args[4] = b;

    fs_append(EVENTS_PATH, "t=%lu w%lu %s a=%ld b=%ld\n", args);
}

/* ------------------------------------------------------------ path making -- */

static VOID fs_join(char *out, const char *base, const char *name)
{
    ULONG i = 0UL;
    ULONG j = 0UL;

    while (base[i] != '\0' && i < MAXPATH - 2UL)
    {
        out[i] = base[i];
        i++;
    }

    if (i > 0UL && out[i - 1UL] != ':' && out[i - 1UL] != '/')
        out[i++] = '/';

    while (name[j] != '\0' && i < MAXPATH - 1UL)
        out[i++] = name[j++];

    out[i] = '\0';
}

/* "<base>/<stem><n>" -- worker 4's numbered tree slots. */
static VOID fs_join_num(char *out, const char *base, const char *stem, ULONG n)
{
    char  tail[64];
    ULONG i = 0UL;
    ULONG d = 0UL;
    char  digits[12];

    while (stem[i] != '\0' && i < sizeof(tail) - 12UL)
    {
        tail[i] = stem[i];
        i++;
    }

    if (n == 0UL)
        digits[d++] = '0';
    while (n > 0UL)
    {
        digits[d++] = (char)('0' + (n % 10UL));
        n /= 10UL;
    }
    while (d > 0UL)
        tail[i++] = digits[--d];
    tail[i] = '\0';

    fs_join(out, base, tail);
}

/* ------------------------------------------------------------- file moves -- */

/*
 * One file, source to destination, through the worker's own buffer.
 *
 * `check` means the source is supposed to carry the pattern, and every buffer
 * that comes off it is checked before it is written on.  `fill` means there is
 * no source file at all: the buffer is generated, which is how the RAM: side
 * is seeded without a network in the way.
 *
 * Returns the bytes moved, or -1 on an I/O error (IoErr() is left set).
 */
static LONG fs_copy_file(StressWorker *w, const char *src, const char *dst,
                         ULONG bytes, BOOL check, BOOL fill)
{
    BPTR  in  = (BPTR)0;
    BPTR  out = (BPTR)0;
    ULONG off = 0UL;
    LONG  rc  = -1;
    ULONG chunk = g_cfg.cf_IoBuf;

    if (!fill)
    {
        w->w_Phase = PH_OPEN_SRC;
        in = Open((CONST_STRPTR)src, MODE_OLDFILE);
        if (in == (BPTR)0)
            return -1;
    }

    w->w_Phase = PH_OPEN_DST;
    out = Open((CONST_STRPTR)dst, MODE_NEWFILE);
    if (out == (BPTR)0)
    {
        if (in != (BPTR)0)
            Close(in);
        return -1;
    }

    while (off < bytes)
    {
        ULONG want = bytes - off;
        LONG  got;

        if (want > chunk)
            want = chunk;

        if (fill)
        {
            fs_pat_fill(w->w_Buf, off, want);
            got = (LONG)want;
        }
        else
        {
            w->w_Phase = PH_READ;
            got = Read(in, w->w_Buf, (LONG)want);
            if (got < 0)
                goto out;
            if (got == 0)
            {
                /* End of file before the length that was asked for.  Not an
                   I/O error, so IoErr() says nothing -- and a copy that
                   stopped early must not pass for a clean one, which is why
                   it is recorded here rather than left to the caller. */
                fs_event(w->w_Role, "short", (LONG)off, (LONG)bytes);
                goto out;
            }

            if (check)
            {
                LONG bad = fs_pat_check(w->w_Buf, off, (ULONG)got);

                if (bad >= 0)
                {
                    w->w_Bad++;
                    fs_event(w->w_Role, "content", (LONG)(off + (ULONG)bad),
                             (LONG)w->w_Buf[bad]);
                    /* Keep going: how much of the file is wrong is the
                       question, and stopping at the first byte cannot say. */
                }
            }
        }

        w->w_Phase = PH_WRITE;
        if (Write(out, w->w_Buf, got) != got)
            goto out;

        off += (ULONG)got;
        w->w_Stamp++;

        /* Carried, not truncated per buffer: worker 4 moves hundreds of files
           smaller than a kilobyte and a per-buffer division loses most of
           what it moved. */
        w->w_Rem += (ULONG)got;
        while (w->w_Rem >= 1024UL)
        {
            w->w_KB++;
            w->w_Rem -= 1024UL;
        }

        if (g_stop != 0UL)
            break;
    }

    rc = (LONG)off;

out:
    w->w_Phase = PH_CLOSE;
    if (out != (BPTR)0 && Close(out) == 0 && rc >= 0)
        rc = -1;
    if (in != (BPTR)0)
        Close(in);

    w->w_Phase = PH_IDLE;

    return rc;
}

/* ---------------------------------------------------------- the tree copy -- */

/*
 * `copy <tree> <share>/testsysN ALL QUIET CLONE`, written out rather than
 * shelled out: this rig's DH0: is a bare directory hard drive with no
 * Workbench on it, so there is no C:Copy to call.  CLONE is the part that
 * matters -- the comment, the protection bits and the datestamp are what
 * Fitz carries over the wire for every entry and what comparetree checks --
 * so all three are set, in that order, because SetProtection() can take the
 * write bit away and SetComment() would then fail.
 */
#define TREE_MAXDEPTH   12

typedef struct
{
    ULONG   tc_Files;
    ULONG   tc_Dirs;
    ULONG   tc_Errs;
} TreeCount;

static LONG fs_tree_copy(StressWorker *w, const char *src, const char *dst,
                         LONG depth, TreeCount *tc)
{
    BPTR                 lock;
    struct FileInfoBlock *fib;
    char                 *spath;
    char                 *dpath;
    LONG                  rc = 0;
    LONG                  dirprot = 0L;
    struct DateStamp      dirdate;
    char                  dircomment[80];

    if (depth > TREE_MAXDEPTH)
        return 0;

    w->w_Phase = PH_MKDIR;
    lock = CreateDir((CONST_STRPTR)dst);
    if (lock != (BPTR)0)
        UnLock(lock);
    else if (IoErr() != ERROR_OBJECT_EXISTS)
    {
        tc->tc_Errs++;
        fs_event(w->w_Role, "mkdir", IoErr(), depth);
        return -1;
    }
    tc->tc_Dirs++;

    w->w_Phase = PH_SCAN;
    lock = Lock((CONST_STRPTR)src, ACCESS_READ);
    if (lock == (BPTR)0)
        return -1;

    fib   = (struct FileInfoBlock *)AllocVec((ULONG)sizeof(*fib),
                                             MEMF_PUBLIC | MEMF_CLEAR);
    spath = (char *)AllocVec((ULONG)MAXPATH, MEMF_ANY);
    dpath = (char *)AllocVec((ULONG)MAXPATH, MEMF_ANY);

    if (fib == NULL || spath == NULL || dpath == NULL)
    {
        rc = -1;
        goto done;
    }

    if (Examine(lock, fib) == DOSFALSE)
    {
        rc = -1;
        goto done;
    }

    /* Examine() on a directory lock describes the DIRECTORY.  Its own three
       fields are taken here, before ExNext() overwrites them with the first
       entry, and applied at the bottom: a drawer's datestamp is one of the
       things comparetree checks and CLONE carries. */
    {
        ULONG c;

        dirprot = fib->fib_Protection;
        dirdate = fib->fib_Date;
        for (c = 0UL; c < sizeof(dircomment) - 1UL &&
                      fib->fib_Comment[c] != '\0'; c++)
            dircomment[c] = fib->fib_Comment[c];
        dircomment[c] = '\0';
    }

    while (ExNext(lock, fib) != DOSFALSE)
    {
        char  name[108];
        LONG  prot;
        struct DateStamp ds;
        char  comment[80];
        ULONG i;

        for (i = 0UL; i < sizeof(name) - 1UL && fib->fib_FileName[i] != '\0'; i++)
            name[i] = fib->fib_FileName[i];
        name[i] = '\0';

        for (i = 0UL; i < sizeof(comment) - 1UL && fib->fib_Comment[i] != '\0'; i++)
            comment[i] = fib->fib_Comment[i];
        comment[i] = '\0';

        prot = fib->fib_Protection;
        ds   = fib->fib_Date;

        fs_join(spath, src, name);
        fs_join(dpath, dst, name);

        if (fib->fib_DirEntryType > 0)
        {
            /* Every level has its own lock and its own fib, so descending
               does not disturb this level's ExNext() position.  The fields
               are copied out above anyway, because the recursion reuses the
               names while this frame still needs them. */
            LONG sub = fs_tree_copy(w, spath, dpath, depth + 1, tc);

            if (sub == -2)
            {
                rc = -2;
                goto done;
            }
            if (sub < 0)
                rc = -1;
        }
        else
        {
            if (fs_copy_file(w, spath, dpath, (ULONG)fib->fib_Size,
                             FALSE, FALSE) < 0)
            {
                tc->tc_Errs++;
                fs_event(w->w_Role, "treefile", IoErr(), 0);
                rc = -1;
            }
            else
            {
                tc->tc_Files++;

                w->w_Phase = PH_META;
                if (comment[0] != '\0')
                    (VOID)SetComment((CONST_STRPTR)dpath,
                                     (CONST_STRPTR)comment);
                (VOID)SetFileDate((CONST_STRPTR)dpath, &ds);
                (VOID)SetProtection((CONST_STRPTR)dpath, prot);
            }
        }

        w->w_Stamp++;

        if (g_stop != 0UL)
        {
            /* Abandoning the walk leaves a partial tree, and a partial tree
               compared against the source reads as hundreds of differences.
               It is not one: say so, so the caller does not publish it. */
            rc = -2;
            goto done;
        }
    }

    /* The drawer itself, after its contents: setting a datestamp first and
       then writing into it would lose the datestamp again. */
    w->w_Phase = PH_META;
    if (dircomment[0] != '\0')
        (VOID)SetComment((CONST_STRPTR)dst, (CONST_STRPTR)dircomment);
    (VOID)SetFileDate((CONST_STRPTR)dst, &dirdate);
    (VOID)SetProtection((CONST_STRPTR)dst, dirprot);

done:
    if (dpath != NULL) FreeVec(dpath);
    if (spath != NULL) FreeVec(spath);
    if (fib   != NULL) FreeVec(fib);
    UnLock(lock);

    w->w_Phase = PH_IDLE;

    return rc;
}

/*
 * Recursive delete, for the slots the tree worker retires.  Same depth cap.
 */
static VOID fs_tree_delete(StressWorker *w, const char *path, LONG depth)
{
    BPTR                  lock;
    struct FileInfoBlock *fib;
    char                 *sub;

    if (depth > TREE_MAXDEPTH)
        return;

    w->w_Phase = PH_SCAN;
    lock = Lock((CONST_STRPTR)path, ACCESS_READ);
    if (lock == (BPTR)0)
        return;

    fib = (struct FileInfoBlock *)AllocVec((ULONG)sizeof(*fib),
                                           MEMF_PUBLIC | MEMF_CLEAR);
    sub = (char *)AllocVec((ULONG)MAXPATH, MEMF_ANY);

    if (fib != NULL && sub != NULL && Examine(lock, fib) != DOSFALSE)
    {
        while (ExNext(lock, fib) != DOSFALSE)
        {
            LONG dir = fib->fib_DirEntryType;

            fs_join(sub, path, (const char *)fib->fib_FileName);

            if (dir > 0)
            {
                fs_tree_delete(w, sub, depth + 1);
            }
            else
            {
                w->w_Phase = PH_DELETE;
                (VOID)SetProtection((CONST_STRPTR)sub, 0L);
                (VOID)DeleteFile((CONST_STRPTR)sub);
            }
        }
    }

    if (sub != NULL) FreeVec(sub);
    if (fib != NULL) FreeVec(fib);
    UnLock(lock);

    w->w_Phase = PH_DELETE;
    (VOID)SetProtection((CONST_STRPTR)path, 0L);
    (VOID)DeleteFile((CONST_STRPTR)path);
    w->w_Phase = PH_IDLE;
}

/*
 * Give the source tree Amiga metadata to carry.
 *
 * mktree.py builds it on the host, where a protection bit and a file comment
 * have nowhere to live, so every entry would arrive with the same default
 * ----rwed and no comment -- and comparetree would then be checking two
 * fields that are constant, which is no check at all.  This walks the tree
 * once at startup and gives each file a comment of its own and a protection
 * word that varies.
 *
 * ONLY THE HIGH NIBBLE IS TOUCHED.  Archive, Pure, Script and Hold say
 * nothing to AmigaDOS about access; R, W, E and D are inverted-sense, so
 * setting FIBF_READ makes the file unreadable and the copy that is supposed
 * to be under test fails for a reason that has nothing to do with the stack.
 */
static VOID fs_tree_stamp(StressWorker *w, const char *path, LONG depth,
                          ULONG *idx)
{
    BPTR                  lock;
    struct FileInfoBlock *fib;
    char                 *sub;

    if (depth > TREE_MAXDEPTH)
        return;

    lock = Lock((CONST_STRPTR)path, ACCESS_READ);
    if (lock == (BPTR)0)
        return;

    fib = (struct FileInfoBlock *)AllocVec((ULONG)sizeof(*fib),
                                           MEMF_PUBLIC | MEMF_CLEAR);
    sub = (char *)AllocVec((ULONG)MAXPATH, MEMF_ANY);

    if (fib != NULL && sub != NULL && Examine(lock, fib) != DOSFALSE)
    {
        while (ExNext(lock, fib) != DOSFALSE)
        {
            LONG dir = fib->fib_DirEntryType;

            fs_join(sub, path, (const char *)fib->fib_FileName);

            if (dir > 0)
            {
                fs_tree_stamp(w, sub, depth + 1, idx);
            }
            else
            {
                char  note[64];
                LONG  prot = 0L;
                ULONG n = *idx;
                ULONG k = 0UL;
                const char *tag = "fitzstress-";

                while (tag[k] != '\0')
                {
                    note[k] = tag[k];
                    k++;
                }
                /* A comment whose length varies as well as its content: a
                   fixed-width one cannot catch an off-by-one in the field. */
                {
                    char digits[12];
                    ULONG d = 0UL;
                    ULONG v = n;

                    if (v == 0UL)
                        digits[d++] = '0';
                    while (v > 0UL)
                    {
                        digits[d++] = (char)('0' + (v % 10UL));
                        v /= 10UL;
                    }
                    while (d > 0UL)
                        note[k++] = digits[--d];
                }
                note[k] = '\0';

                if ((n & 1UL) != 0UL) prot |= FIBF_ARCHIVE;
                if ((n & 2UL) != 0UL) prot |= FIBF_PURE;
                if ((n & 4UL) != 0UL) prot |= FIBF_SCRIPT;

                (VOID)SetComment((CONST_STRPTR)sub, (CONST_STRPTR)note);
                (VOID)SetProtection((CONST_STRPTR)sub, prot);

                (*idx)++;
            }

            w->w_Stamp++;
        }
    }

    if (sub != NULL) FreeVec(sub);
    if (fib != NULL) FreeVec(fib);
    UnLock(lock);
}

/* ------------------------------------------------------- the four bodies -- */

/* 1: the share to RAM:, endlessly, checking every byte on the way in. */
static VOID fs_body_down(StressWorker *w)
{
    char src[MAXPATH];
    char dst[MAXPATH];

    fs_join(src, g_cfg.cf_Share, "down.bin");
    fs_join(dst, g_cfg.cf_Ram,   "fsdown.bin");

    while (g_stop == 0UL)
    {
        if (fs_copy_file(w, src, dst, g_cfg.cf_SmallKB * 1024UL,
                         TRUE, FALSE) < 0)
        {
            w->w_Errs++;
            fs_event(w->w_Role, "down", IoErr(), 0);
            Delay(50UL);
        }
        else
        {
            w->w_Iters++;
        }

        w->w_Phase = PH_DELETE;
        (VOID)DeleteFile((CONST_STRPTR)dst);
        w->w_Phase = PH_IDLE;
    }
}

/* 2: RAM: to the share, endlessly.  The host checks what landed. */
static VOID fs_body_up(StressWorker *w)
{
    char src[MAXPATH];
    char dst[MAXPATH];

    fs_join(src, g_cfg.cf_Ram,   "fsup.bin");
    fs_join(dst, g_cfg.cf_Share, "up.bin");

    /* The source is generated, not fetched: worker 2 must not depend on the
       network to have something to send. */
    if (fs_copy_file(w, NULL, src, g_cfg.cf_SmallKB * 1024UL,
                     FALSE, TRUE) < 0)
    {
        w->w_Errs++;
        fs_event(w->w_Role, "seedram", IoErr(), 0);
        return;
    }
    w->w_KB  = 0UL;                     /* the seed is local, not traffic */
    w->w_Rem = 0UL;

    while (g_stop == 0UL)
    {
        if (fs_copy_file(w, src, dst, g_cfg.cf_SmallKB * 1024UL,
                         FALSE, FALSE) < 0)
        {
            w->w_Errs++;
            fs_event(w->w_Role, "up", IoErr(), 0);
            Delay(50UL);
        }
        else
        {
            w->w_Iters++;
        }
    }

    w->w_Phase = PH_DELETE;
    (VOID)DeleteFile((CONST_STRPTR)src);
    w->w_Phase = PH_IDLE;
}

/* 3: the big file, share -> disk, then disk -> share, alternating. */
static VOID fs_body_big(StressWorker *w)
{
    char remote[MAXPATH];
    char local[MAXPATH];
    char back[MAXPATH];
    ULONG bytes = g_cfg.cf_BigKB * 1024UL;

    fs_join(remote, g_cfg.cf_Share, "big.bin");
    fs_join(local,  g_cfg.cf_Disk,  "big.bin");
    fs_join(back,   g_cfg.cf_Share, "bigback.bin");

    while (g_stop == 0UL)
    {
        /* down: checked byte for byte as it arrives */
        if (fs_copy_file(w, remote, local, bytes, TRUE, FALSE) < 0)
        {
            w->w_Errs++;
            fs_event(w->w_Role, "bigdown", IoErr(), 0);
            Delay(50UL);
            continue;
        }
        w->w_Iters++;

        if (g_stop != 0UL)
            break;

        /* up: the host checks bigback.bin against the same pattern */
        if (fs_copy_file(w, local, back, bytes, FALSE, FALSE) < 0)
        {
            w->w_Errs++;
            fs_event(w->w_Role, "bigup", IoErr(), 0);
            Delay(50UL);
            continue;
        }
        w->w_Iters++;

        w->w_Phase = PH_DELETE;
        (VOID)DeleteFile((CONST_STRPTR)local);
        w->w_Phase = PH_IDLE;
    }
}

/*
 * 4: the system tree to the share, cloned, into numbered slots.
 *
 * Three slots are kept, not one, because the supervisor runs comparetree
 * against the newest FINISHED slot while this worker is filling the next: a
 * single slot would be compared while it was being rewritten, and a
 * difference would say nothing.  Slot n-3 is retired when slot n is done, so
 * whatever the comparison is reading stays put for two more copies.
 */
static VOID fs_body_tree(StressWorker *w)
{
    char  dst[MAXPATH];
    ULONG slot = 0UL;

    while (g_stop == 0UL)
    {
        TreeCount tc;
        LONG      rc;

        tc.tc_Files = 0UL;
        tc.tc_Dirs  = 0UL;
        tc.tc_Errs  = 0UL;

        fs_join_num(dst, g_cfg.cf_Share, "testsys", slot);

        rc = fs_tree_copy(w, g_cfg.cf_Tree, dst, 0, &tc);

        if (rc == -2)
        {
            /* Told to stop part way.  Not an error, and not publishable. */
            break;
        }
        if (rc < 0)
        {
            w->w_Errs++;
            fs_event(w->w_Role, "tree", (LONG)tc.tc_Errs, (LONG)slot);
            /* A share that has gone away fails every entry instantly, and a
               loop with no pause in it fills the event file with the same
               line for as long as the run has left. */
            Delay(50UL);
        }
        else
        {
            w->w_Iters++;
            w->w_Ready = slot + 1UL;    /* +1 so 0 still means "none yet" */
        }

        if (slot >= 3UL)
        {
            char old[MAXPATH];

            fs_join_num(old, g_cfg.cf_Share, "testsys", slot - 3UL);
            fs_tree_delete(w, old, 0);
        }

        slot++;
    }
}

/* --------------------------------------------------------------- spawning -- */

static VOID fs_worker_entry(VOID)
{
    struct Process *me = (struct Process *)FindTask((STRPTR)0);
    StressWorker   *w;

    Wait(SIGF_SINGLE);

    w = (StressWorker *)me->pr_Task.tc_UserData;
    if (w == NULL)
        return;

    w->w_Buf = (UBYTE *)AllocVec(g_cfg.cf_IoBuf, MEMF_ANY);
    if (w->w_Buf == NULL)
    {
        w->w_Done = 1UL;
        return;
    }

    w->w_Alive = 1UL;

    switch (w->w_Role)
    {
        case ROLE_DOWN: fs_body_down(w); break;
        case ROLE_UP:   fs_body_up(w);   break;
        case ROLE_BIG:  fs_body_big(w);  break;
        case ROLE_TREE: fs_body_tree(w); break;
        default: break;
    }

    FreeVec(w->w_Buf);
    w->w_Buf   = NULL;
    w->w_Alive = 0UL;
    w->w_Done  = 1UL;
}

static struct Process *fs_spawn(StressWorker *w, const char *name)
{
    struct Process *p;

    Forbid();

    p = CreateNewProcTags(NP_Entry,     (ULONG)fs_worker_entry,
                          NP_Name,      (ULONG)name,
                          NP_Priority,  (ULONG)0,
                          NP_StackSize, (ULONG)32768,
                          NP_Cli,       (ULONG)FALSE,
                          TAG_DONE);

    if (p != NULL)
        p->pr_Task.tc_UserData = (APTR)w;

    Permit();

    if (p != NULL)
    {
        w->w_Proc = p;
        Signal(&p->pr_Task, SIGF_SINGLE);
    }

    return p;
}

/* ------------------------------------------------------------- supervisor -- */

static VOID fs_heartbeat(ULONG t)
{
    LONG args[16];
    LONG i;

    args[0] = (LONG)t;
    for (i = 0; i < 4; i++)
    {
        args[1 + i * 3] = (LONG)g_worker[i].w_Iters;
        args[2 + i * 3] = (LONG)g_worker[i].w_KB;
        args[3 + i * 3] = (LONG)g_worker[i].w_Stamp;
    }
    args[13] = (LONG)AvailMem(MEMF_PUBLIC);

    fs_kprintf("FS t=%ld w1=%ld/%ldk/%ld w2=%ld/%ldk/%ld "
               "w3=%ld/%ldk/%ld w4=%ld/%ldk/%ld free=%ld\n", args);
}

static VOID fs_timeline(ULONG t)
{
    LONG args[16];
    LONG i;

    args[0] = (LONG)t;
    for (i = 0; i < 4; i++)
    {
        args[1 + i * 3] = (LONG)g_worker[i].w_Iters;
        args[2 + i * 3] = (LONG)g_worker[i].w_KB;
        args[3 + i * 3] = (LONG)g_worker[i].w_Errs;
    }
    args[13] = (LONG)AvailMem(MEMF_PUBLIC);
    args[14] = (LONG)AvailMem(MEMF_PUBLIC | MEMF_LARGEST);

    fs_append(TIMELINE_PATH,
              "%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu\n",
              args);
}

/*
 * `netstat -h` opens no library, allocates nothing and takes no lock, which is
 * why it is safe to run from here while four processes are hammering the
 * machine -- and why it still answers when the rest of the stack does not.
 * The block is stamped so the series can be read against the timeline.
 */
static VOID fs_health(ULONG t)
{
    LONG args[2];

    args[0] = (LONG)t;
    args[1] = 0;
    fs_append(HEALTH_PATH, "\n===== t=%lu =====\n", args);

    (VOID)SystemTagList((CONST_STRPTR)"SYS:netstat -h >>" HEALTH_PATH
                        " <NIL:", NULL);
}

/*
 * Fitz's own comparetree, run the way its author runs it: every field it
 * carries over the wire, and the file content too.  Slot `ready-1` is the
 * newest one worker 4 has finished with.
 */
static VOID fs_compare(ULONG t)
{
    static char line[MAXPATH * 2 + 64];
    static char slotpath[MAXPATH];
    LONG args[2];
    ULONG ready = g_worker[3].w_Ready;
    ULONG i = 0UL;
    ULONG j;
    const char *p;

    if (ready == 0UL)
        return;

    fs_join_num(slotpath, g_cfg.cf_Share, "testsys", ready - 1UL);

    args[0] = (LONG)t;
    args[1] = (LONG)(ready - 1UL);
    fs_append(COMPARE_PATH, "\n===== t=%lu testsys%lu =====\n", args);

    p = "SYS:comparetree ";
    for (j = 0UL; p[j] != '\0'; j++) line[i++] = p[j];
    p = g_cfg.cf_Tree;
    for (j = 0UL; p[j] != '\0'; j++) line[i++] = p[j];
    line[i++] = ' ';
    p = slotpath;
    for (j = 0UL; p[j] != '\0'; j++) line[i++] = p[j];
    p = " CRC >>" COMPARE_PATH " <NIL:";
    for (j = 0UL; p[j] != '\0'; j++) line[i++] = p[j];
    line[i] = '\0';

    args[0] = SystemTagList((CONST_STRPTR)line, NULL);
    args[1] = 0;
    fs_append(COMPARE_PATH, "----- rc %ld -----\n", args);

    if (args[0] != 0)
        fs_event(4UL, "comparetree", args[0], (LONG)(ready - 1UL));
}

static VOID fs_summary(ULONG t, ULONG stuck)
{
    LONG args[8];
    LONG i;

    args[0] = (LONG)t;
    args[1] = (LONG)stuck;
    fs_append(SUMMARY_PATH, "seconds %lu\nstuck_workers %lu\n", args);

    for (i = 0; i < 4; i++)
    {
        StressWorker *w = &g_worker[i];
        ULONG ph = w->w_Phase;

        if (ph >= (ULONG)(sizeof(fs_phase_name) / sizeof(fs_phase_name[0])))
            ph = 0UL;

        args[0] = (LONG)(i + 1);
        args[1] = (LONG)w->w_Iters;
        args[2] = (LONG)w->w_KB;
        args[3] = (LONG)w->w_Errs;
        args[4] = (LONG)w->w_Bad;
        args[5] = (LONG)w->w_Alive;
        args[6] = (LONG)fs_phase_name[ph];
        args[7] = (LONG)w->w_Stamp;

        fs_append(SUMMARY_PATH,
                  "w%lu iters %lu kb %lu errs %lu bad %lu alive %lu "
                  "phase %s stamp %lu\n", args);
    }
}

int main(int argc, char **argv)
{
    struct Process *self = (struct Process *)FindTask(NULL);
    APTR            old_window;
    ULONG           t = 0UL;
    ULONG           sample_n = 0UL;
    ULONG           stuck = 0UL;
    LONG            i;
    LONG            args[4];
    ULONG           last_stamp[4];

    (VOID)argc;
    (VOID)argv;

    old_window = self->pr_WindowPtr;
    self->pr_WindowPtr = (APTR)-1;

    fs_cfg_load();
    fs_pat_init(g_cfg.cf_Seed);

    fs_truncate(TIMELINE_PATH);
    fs_truncate(EVENTS_PATH);
    fs_truncate(SUMMARY_PATH);
    fs_truncate(HEALTH_PATH);
    fs_truncate(COMPARE_PATH);

    fs_append(TIMELINE_PATH,
              "t_s,w1_it,w1_kb,w1_err,w2_it,w2_kb,w2_err,"
              "w3_it,w3_kb,w3_err,w4_it,w4_kb,w4_err,avail,largest\n", NULL);

    args[0] = (LONG)g_cfg.cf_Seconds;
    args[1] = (LONG)g_cfg.cf_SmallKB;
    args[2] = (LONG)g_cfg.cf_BigKB;
    args[3] = (LONG)g_cfg.cf_Workers;
    fs_append(SUMMARY_PATH,
              "requested_seconds %lu\nsmall_kb %lu\nbig_kb %lu\nworkers %lu\n",
              args);

    /* A working directory on the real disk for worker 3. */
    {
        BPTR l = CreateDir((CONST_STRPTR)g_cfg.cf_Disk);

        if (l != (BPTR)0)
            UnLock(l);
    }

    /* Before any worker starts, so the tree it copies has something to
       carry.  Local disk only -- no network is involved in this. */
    {
        ULONG idx = 0UL;

        fs_tree_stamp(&g_worker[3], g_cfg.cf_Tree, 0, &idx);
        args[0] = (LONG)idx;
        args[1] = 0;
        fs_append(SUMMARY_PATH, "tree_files_stamped %lu\n", args);
    }

    fs_health(0UL);

    for (i = 0; i < 4; i++)
    {
        static const char *const names[4] =
            { "FitzStress.down", "FitzStress.up",
              "FitzStress.big",  "FitzStress.tree" };

        g_worker[i].w_Role = (ULONG)(i + 1);
        last_stamp[i] = 0UL;

        if ((g_cfg.cf_Workers & (1UL << i)) == 0UL)
        {
            g_worker[i].w_Done = 1UL;
            continue;
        }

        if (fs_spawn(&g_worker[i], names[i]) == NULL)
        {
            g_worker[i].w_Done = 1UL;
            fs_event((ULONG)(i + 1), "spawn", IoErr(), 0);
        }
    }

    /* ---- the run ---------------------------------------------------- */

    fs_clock_start();

    while (t < g_cfg.cf_Seconds)
    {
        ULONG step = g_cfg.cf_Sample;

        if (step == 0UL)
            step = 30UL;

        Delay(step * 50UL);
        t = fs_elapsed();
        g_secs = t;
        sample_n++;

        fs_timeline(t);
        fs_heartbeat(t);
        fs_health(t);

        if (g_cfg.cf_Compare != 0UL && (sample_n % g_cfg.cf_Compare) == 0UL)
            fs_compare(t);

        if (SetSignal(0L, 0L) & SIGBREAKF_CTRL_C)
        {
            SetSignal(0L, SIGBREAKF_CTRL_C);
            break;
        }
    }

    /* ---- winding down ----------------------------------------------- */

    g_stop = 1UL;

    /* A worker is given four minutes to notice, which is longer than the
       slowest single buffer can take on this link.  Anything still moving is
       still working; anything not moving is the thing being hunted. */
    for (i = 0; i < 240; i++)
    {
        ULONG done = 0UL;
        LONG  k;

        for (k = 0; k < 4; k++)
            done += g_worker[k].w_Done;

        if (done == 4UL)
            break;

        Delay(50UL);
        g_secs = fs_elapsed();

        if ((i % 30) == 29)
            fs_heartbeat(g_secs);
    }
    t = fs_elapsed();

    for (i = 0; i < 4; i++)
    {
        if (g_worker[i].w_Done == 0UL)
        {
            stuck++;
            if (g_worker[i].w_Stamp == last_stamp[i])
                fs_event((ULONG)(i + 1), "wedged", (LONG)g_worker[i].w_Phase,
                         (LONG)g_worker[i].w_Stamp);
        }
        last_stamp[i] = g_worker[i].w_Stamp;
    }

    fs_health(t + 1UL);
    fs_compare(t + 1UL);
    fs_summary(t, stuck);
    fs_heartbeat(t + 1UL);

    self->pr_WindowPtr = old_window;

    if (stuck != 0UL)
    {
        /*
         * Do not return.  Returning unloads this segment, and a child still
         * inside fs_copy_file() would then execute freed memory -- turning a
         * freeze that can be read off the timeline into a crash that cannot.
         * The emulator's own deadline ends the run; everything worth having
         * is already on DH0:, and the heartbeat keeps saying whether the
         * machine is executing at all.
         */
        for (;;)
        {
            Delay(30UL * 50UL);
            g_secs += 30UL;
            fs_heartbeat(g_secs);
            fs_health(g_secs);
        }
    }

    return RETURN_OK;
}

/*
 * RsGapBoot: the boot-time half of docs/plans/roadshow-rx-gap.md.
 *
 * Stack-neutral: it never opens bsdsocket.library, so it runs before any
 * stack and says the same thing under both.  One subcommand per call:
 *
 *   SELECT    read S:rsgap-arm, DELETE it, then point RSGAPARM: at the arm's
 *             drawer and put that drawer's libs first in LIBS:.  No selector,
 *             or one that cannot be deleted, is AmiNetXDuo in door mode.
 *             RC 0 = a measurement boot, RC 5 = door mode.  Also rotates the
 *             previous boot's cur.* files to boot-<n>.* in DIR.
 *   WATCHDOG  started with Run before the stack.  ColdReboot() after SECS
 *             unless CANCELFILE appears (deleted, logged) or CTRL-C arrives.
 *             Logs to DIR/cur.wd.  Nothing here needs the network.
 *   RECORD    CPU, caches, boards, memory, loaded stack and driver, public
 *             ports and tasks, and the md5 of every FILES entry.
 *   NEXT      pop the first arm off S:rsgap-queue into S:rsgap-arm.
 *   REBOOT    ColdReboot() after a pause that lets the disk settle.
 *   HOLD      wait until the watchdog is gone, then SECS more (emulator).
 *
 * Every line printed is key=value, first key rec=.
 *
 * SPDX-License-Identifier: MIT
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/execbase.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <libraries/configvars.h>
#include <libraries/expansionbase.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/expansion.h>

#include <stdarg.h>
#include <string.h>

static const char version_tag[] __attribute__((used)) =
    "$VER: RsGapBoot 1.0 (24.9.2026)";

extern struct ExecBase *SysBase;
struct ExpansionBase *ExpansionBase;

#define TEMPLATE \
    "SELECT/S,WATCHDOG/S,RECORD/S,NEXT/S,REBOOT/S,HOLD/S,SECS/K/N," \
    "CANCELFILE/K,DIR/K,ARMS/K,FILES/M"

enum { ARG_SELECT, ARG_WATCHDOG, ARG_RECORD, ARG_NEXT, ARG_REBOOT, ARG_HOLD,
       ARG_SECS, ARG_CANCELFILE, ARG_DIR, ARG_ARMS, ARG_FILES, ARG_COUNT };

#define SELECTOR   "S:rsgap-arm"
#define QUEUE      "S:rsgap-queue"
#define DEF_DIR    "SYS:rsgap"
#define DEF_ARMS   "SYS:rsgap/arm"
#define WD_PORT    "rsgap.watchdog"

static char line[2048];

static VOID say(const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    VPrintf((CONST_STRPTR)fmt, (APTR)args);
    va_end(args);
}

static VOID path_join(char *out, LONG size, const char *dir, const char *leaf)
{
    strncpy(out, dir, size - 1);
    out[size - 1] = '\0';
    AddPart((STRPTR)out, (CONST_STRPTR)leaf, size);
}

static BOOL exists(const char *path)
{
    BPTR l = Lock((CONST_STRPTR)path, SHARED_LOCK);

    if (l == 0)
        return FALSE;
    UnLock(l);
    return TRUE;
}

/* Whole small file into buf; -1 when it cannot be opened. */
static LONG read_small(const char *path, char *buf, LONG size)
{
    BPTR fh = Open((CONST_STRPTR)path, MODE_OLDFILE);
    LONG n;

    if (fh == 0)
        return -1;
    n = Read(fh, buf, size - 1);
    Close(fh);
    if (n < 0)
        n = 0;
    buf[n] = '\0';
    return n;
}

static BOOL write_small(const char *path, const char *text, BOOL append)
{
    BPTR fh;
    LONG len = (LONG)strlen(text);
    BOOL ok;

    if (append)
    {
        fh = Open((CONST_STRPTR)path, MODE_READWRITE);
        if (fh != 0)
            Seek(fh, 0, OFFSET_END);
    }
    else
        fh = Open((CONST_STRPTR)path, MODE_NEWFILE);
    if (fh == 0)
        return FALSE;
    ok = Write(fh, (APTR)text, len) == len;
    return Close(fh) && ok;
}

static ULONG parse_ulong(const char *s)
{
    ULONG v = 0;

    while (*s >= '0' && *s <= '9')
        v = v * 10UL + (ULONG)(*s++ - '0');
    return v;
}

/* Copy a name into out with blanks, commas and '=' made harmless. */
static VOID clean(char *out, LONG size, const char *in)
{
    LONG i = 0;

    if (in == NULL)
        in = "-";
    while (*in && i < size - 1)
    {
        char c = *in++;

        if (c == ' ' || c == ',' || c == '=' || c == '\t')
            c = '_';
        else if (c == '\n' || c == '\r')
            break;
        out[i++] = c;
    }
    out[i] = '\0';
}

/* -------------------------------------------------------------- SELECT -- */

/* Put dir first in LIBS:, keeping every directory LIBS: already had. */
static BOOL libs_prepend(const char *dir)
{
    BPTR             first, keep[16];
    int              nkeep = 0, i;
    struct DosList  *dl;
    struct AssignList *al;

    first = Lock((CONST_STRPTR)dir, SHARED_LOCK);
    if (first == 0)
        return FALSE;

    dl = LockDosList(LDF_ASSIGNS | LDF_READ);
    dl = FindDosEntry(dl, (CONST_STRPTR)"LIBS", LDF_ASSIGNS);
    if (dl != NULL)
    {
        if (dl->dol_Lock != 0)
            keep[nkeep++] = DupLock(dl->dol_Lock);
        for (al = dl->dol_misc.dol_assign.dol_List; al && nkeep < 16;
             al = al->al_Next)
            keep[nkeep++] = DupLock(al->al_Lock);
    }
    UnLockDosList(LDF_ASSIGNS | LDF_READ);

    if (!AssignLock((CONST_STRPTR)"LIBS", first))
    {
        UnLock(first);
        for (i = 0; i < nkeep; i++)
            UnLock(keep[i]);
        return FALSE;
    }
    for (i = 0; i < nkeep; i++)
        if (keep[i] != 0 && !AssignAdd((CONST_STRPTR)"LIBS", keep[i]))
            UnLock(keep[i]);
    return TRUE;
}

static BOOL list_has(struct List *list, const char *name)
{
    struct Node *n;
    BOOL         found;

    Forbid();
    n = FindName(list, (CONST_STRPTR)name);
    found = n != NULL;
    Permit();
    return found;
}

static VOID rotate(const char *dir, ULONG boot, const char *ext)
{
    char from[256], to[256], leaf[32];

    strcpy(leaf, "cur.");
    strcat(leaf, ext);
    path_join(from, sizeof(from), dir, leaf);
    if (!exists(from))
        return;
    {
        char num[12];
        int  i = 11;
        ULONG v = boot;

        num[i] = '\0';
        do { num[--i] = (char)('0' + v % 10UL); v /= 10UL; } while (v);
        strcpy(leaf, "boot-");
        strcat(leaf, num + i);
        strcat(leaf, ".");
        strcat(leaf, ext);
    }
    path_join(to, sizeof(to), dir, leaf);
    DeleteFile((CONST_STRPTR)to);
    Rename((CONST_STRPTR)from, (CONST_STRPTR)to);
}

static int do_select(const char *dir, const char *arms)
{
    char        buf[64], countpath[256], cur[256], armdir[256], libs[256];
    const char *arm = "A", *mode = "door", *selector = "absent";
    const char *reason = "none";
    ULONG       boot = 0;
    BOOL        consumed = FALSE;
    BOOL        preload_bsd, preload_drv;
    BPTR        l;

    /* Before anything else: was a stack or the driver already started? */
    preload_bsd = list_has(&SysBase->LibList, "bsdsocket.library");
    preload_drv = list_has(&SysBase->DeviceList, "x-surf-100.device");

    UnLock(CreateDir((CONST_STRPTR)dir));
    path_join(countpath, sizeof(countpath), dir, "bootcount");
    if (read_small(countpath, buf, sizeof(buf)) > 0)
        boot = parse_ulong(buf);
    rotate(dir, boot, "kv");
    rotate(dir, boot, "wd");
    rotate(dir, boot, "log");
    boot++;
    {
        char num[16];
        int  i = 14;
        ULONG v = boot;

        num[15] = '\0';
        num[14] = '\n';
        do { num[--i] = (char)('0' + v % 10UL); v /= 10UL; } while (v);
        write_small(countpath, num + i, FALSE);
    }

    /* ONE-SHOT: read, then delete, then check it is gone -- all before the
       stack.  A selector that survives would pin the next boot to it. */
    if (read_small(SELECTOR, buf, sizeof(buf)) >= 0)
    {
        char *p = buf;

        selector = "present";
        while (*p == ' ' || *p == '\t' || *p == '\n')
            p++;
        DeleteFile((CONST_STRPTR)SELECTOR);
        consumed = !exists(SELECTOR);
        if (!consumed)
            reason = "undeletable";
        else if (*p == 'R' || *p == 'r')
            { arm = "R"; mode = "measure"; }
        else if (*p == 'A' || *p == 'a')
            { arm = "A"; mode = "measure"; }
        else
            reason = "badselector";
    }

    path_join(armdir, sizeof(armdir), arms, arm);
    path_join(libs, sizeof(libs), armdir, "libs");
    l = Lock((CONST_STRPTR)armdir, SHARED_LOCK);
    if (l == 0 || !AssignLock((CONST_STRPTR)"RSGAPARM", l))
    {
        if (l != 0)
            UnLock(l);
        reason = "noarmdir";
    }
    else if (!libs_prepend(libs))
        reason = "nolibs";

    path_join(cur, sizeof(cur), dir, "cur.kv");
    strcpy(line, "");
    {
        char num[12];
        int  i = 11;
        ULONG v = boot;

        num[i] = '\0';
        do { num[--i] = (char)('0' + v % 10UL); v /= 10UL; } while (v);
        strcat(line, "rec=select boot=");
        strcat(line, num + i);
    }
    strcat(line, " arm=");       strcat(line, arm);
    strcat(line, " mode=");      strcat(line, mode);
    strcat(line, " selector=");  strcat(line, selector);
    strcat(line, " consumed=");  strcat(line, consumed ? "1" : "0");
    strcat(line, " reason=");    strcat(line, reason);
    strcat(line, " bsdsocket_preloaded=");
    strcat(line, preload_bsd ? "1" : "0");
    strcat(line, " driver_preloaded=");
    strcat(line, preload_drv ? "1" : "0");
    strcat(line, "\n");
    write_small(cur, line, TRUE);
    say("%s", (LONG)line);

    if (strcmp(reason, "noarmdir") == 0 || strcmp(reason, "nolibs") == 0)
        return 20;
    return strcmp(mode, "measure") == 0 ? 0 : 5;
}

/* ------------------------------------------------------------ WATCHDOG -- */

static LONG elapsed_s(const struct DateStamp *a)
{
    struct DateStamp b;

    DateStamp(&b);
    return (b.ds_Days - a->ds_Days) * 86400L +
           (b.ds_Minute - a->ds_Minute) * 60L +
           (b.ds_Tick - a->ds_Tick) / TICKS_PER_SECOND;
}

static VOID wd_log(const char *dir, const char *text)
{
    char path[256];

    path_join(path, sizeof(path), dir, "cur.wd");
    write_small(path, text, TRUE);
}

static int do_watchdog(const char *dir, LONG secs, const char *cancelfile)
{
    struct MsgPort  *port;
    struct DateStamp start;
    char             msg[160];
    LONG             e;

    port = CreateMsgPort();
    if (port == NULL)
        return 20;
    port->mp_Node.ln_Name = (char *)WD_PORT;
    port->mp_Node.ln_Pri = 0;
    AddPort(port);

    DateStamp(&start);
    msg[0] = '\0';
    {
        struct { LONG s; const char *c; } a;
        a.s = secs;
        a.c = cancelfile ? cancelfile : "-";
        RawDoFmt((CONST_STRPTR)"rec=wd state=armed secs=%ld cancelfile=%s\n",
                 (APTR)&a, (void (*)())"\x16\xc0\x4e\x75", msg);
    }
    wd_log(dir, msg);

    for (;;)
    {
        Delay(TICKS_PER_SECOND);
        e = elapsed_s(&start);
        if (SetSignal(0, 0) & SIGBREAKF_CTRL_C)
        {
            RawDoFmt((CONST_STRPTR)"rec=wd state=cancelled via=break wd_fired=0 elapsed_s=%ld\n",
                     (APTR)&e, (void (*)())"\x16\xc0\x4e\x75", msg);
            wd_log(dir, msg);
            break;
        }
        if (cancelfile != NULL && exists(cancelfile))
        {
            DeleteFile((CONST_STRPTR)cancelfile);
            RawDoFmt((CONST_STRPTR)"rec=wd state=cancelled via=file wd_fired=0 elapsed_s=%ld\n",
                     (APTR)&e, (void (*)())"\x16\xc0\x4e\x75", msg);
            wd_log(dir, msg);
            break;
        }
        if (e >= secs)
        {
            RawDoFmt((CONST_STRPTR)"rec=wd state=fired wd_fired=1 elapsed_s=%ld\n",
                     (APTR)&e, (void (*)())"\x16\xc0\x4e\x75", msg);
            wd_log(dir, msg);
            Delay(3 * TICKS_PER_SECOND);        /* let the disk settle */
            ColdReboot();
        }
    }

    RemPort(port);
    DeleteMsgPort(port);
    return 0;
}

static int do_hold(LONG grace)
{
    for (;;)
    {
        struct MsgPort *p;

        Forbid();
        p = FindPort((CONST_STRPTR)WD_PORT);
        Permit();
        if (p == NULL)
            break;
        if (SetSignal(0, 0) & SIGBREAKF_CTRL_C)
            return 5;
        Delay(TICKS_PER_SECOND);
    }
    say("rec=hold watchdog=gone grace_s=%ld\n", grace);
    Delay(grace * TICKS_PER_SECOND);
    return 0;
}

/* ---------------------------------------------------------------- NEXT -- */

static int do_next(VOID)
{
    static char q[1024];
    char       *p, *rest;
    char        arm = 0;
    LONG        left = 0;

    if (read_small(QUEUE, q, sizeof(q)) < 0)
    {
        say("rec=next next_arm=none queue_left=0\n");
        return 0;
    }
    p = q;
    while (*p && arm == 0)
    {
        if (*p == 'R' || *p == 'r')
            arm = 'R';
        else if (*p == 'A' || *p == 'a')
            arm = 'A';
        p++;
    }
    while (*p && *p != '\n')
        p++;
    rest = p;
    for (p = rest; *p; p++)
        if (*p == 'R' || *p == 'r' || *p == 'A' || *p == 'a')
            left++;
    if (left > 0)
        write_small(QUEUE, rest, FALSE);
    else
        DeleteFile((CONST_STRPTR)QUEUE);
    if (arm != 0)
    {
        char sel[3];

        sel[0] = arm;
        sel[1] = '\n';
        sel[2] = '\0';
        if (!write_small(SELECTOR, sel, FALSE))
        {
            say("rec=next next_arm=none queue_left=%ld reason=nowrite\n", left);
            return 10;
        }
        say("rec=next next_arm=%s queue_left=%ld\n",
            (LONG)(arm == 'R' ? "R" : "A"), left);
    }
    else
        say("rec=next next_arm=none queue_left=0\n");
    return 0;
}

/* -------------------------------------------------------------- RECORD -- */

/* RFC 1321, compact. */
struct md5 { ULONG a, b, c, d, lo, hi; UBYTE buf[64]; };

#define F(x, y, z) ((z) ^ ((x) & ((y) ^ (z))))
#define G(x, y, z) ((y) ^ ((z) & ((x) ^ (y))))
#define H(x, y, z) ((x) ^ (y) ^ (z))
#define I(x, y, z) ((y) ^ ((x) | ~(z)))
#define STEP(f, a, b, c, d, x, t, s) \
    (a) += f((b), (c), (d)) + (x) + (t); \
    (a) = (((a) << (s)) | (((a) & 0xffffffffUL) >> (32 - (s)))); \
    (a) += (b);

static VOID md5_block(struct md5 *m, const UBYTE *p)
{
    ULONG x[16], a = m->a, b = m->b, c = m->c, d = m->d;
    int   i;

    for (i = 0; i < 16; i++)
        x[i] = (ULONG)p[i * 4] | ((ULONG)p[i * 4 + 1] << 8) |
               ((ULONG)p[i * 4 + 2] << 16) | ((ULONG)p[i * 4 + 3] << 24);

    STEP(F, a, b, c, d, x[0], 0xd76aa478, 7)  STEP(F, d, a, b, c, x[1], 0xe8c7b756, 12)
    STEP(F, c, d, a, b, x[2], 0x242070db, 17) STEP(F, b, c, d, a, x[3], 0xc1bdceee, 22)
    STEP(F, a, b, c, d, x[4], 0xf57c0faf, 7)  STEP(F, d, a, b, c, x[5], 0x4787c62a, 12)
    STEP(F, c, d, a, b, x[6], 0xa8304613, 17) STEP(F, b, c, d, a, x[7], 0xfd469501, 22)
    STEP(F, a, b, c, d, x[8], 0x698098d8, 7)  STEP(F, d, a, b, c, x[9], 0x8b44f7af, 12)
    STEP(F, c, d, a, b, x[10], 0xffff5bb1, 17) STEP(F, b, c, d, a, x[11], 0x895cd7be, 22)
    STEP(F, a, b, c, d, x[12], 0x6b901122, 7) STEP(F, d, a, b, c, x[13], 0xfd987193, 12)
    STEP(F, c, d, a, b, x[14], 0xa679438e, 17) STEP(F, b, c, d, a, x[15], 0x49b40821, 22)

    STEP(G, a, b, c, d, x[1], 0xf61e2562, 5)  STEP(G, d, a, b, c, x[6], 0xc040b340, 9)
    STEP(G, c, d, a, b, x[11], 0x265e5a51, 14) STEP(G, b, c, d, a, x[0], 0xe9b6c7aa, 20)
    STEP(G, a, b, c, d, x[5], 0xd62f105d, 5)  STEP(G, d, a, b, c, x[10], 0x02441453, 9)
    STEP(G, c, d, a, b, x[15], 0xd8a1e681, 14) STEP(G, b, c, d, a, x[4], 0xe7d3fbc8, 20)
    STEP(G, a, b, c, d, x[9], 0x21e1cde6, 5)  STEP(G, d, a, b, c, x[14], 0xc33707d6, 9)
    STEP(G, c, d, a, b, x[3], 0xf4d50d87, 14) STEP(G, b, c, d, a, x[8], 0x455a14ed, 20)
    STEP(G, a, b, c, d, x[13], 0xa9e3e905, 5) STEP(G, d, a, b, c, x[2], 0xfcefa3f8, 9)
    STEP(G, c, d, a, b, x[7], 0x676f02d9, 14) STEP(G, b, c, d, a, x[12], 0x8d2a4c8a, 20)

    STEP(H, a, b, c, d, x[5], 0xfffa3942, 4)  STEP(H, d, a, b, c, x[8], 0x8771f681, 11)
    STEP(H, c, d, a, b, x[11], 0x6d9d6122, 16) STEP(H, b, c, d, a, x[14], 0xfde5380c, 23)
    STEP(H, a, b, c, d, x[1], 0xa4beea44, 4)  STEP(H, d, a, b, c, x[4], 0x4bdecfa9, 11)
    STEP(H, c, d, a, b, x[7], 0xf6bb4b60, 16) STEP(H, b, c, d, a, x[10], 0xbebfbc70, 23)
    STEP(H, a, b, c, d, x[13], 0x289b7ec6, 4) STEP(H, d, a, b, c, x[0], 0xeaa127fa, 11)
    STEP(H, c, d, a, b, x[3], 0xd4ef3085, 16) STEP(H, b, c, d, a, x[6], 0x04881d05, 23)
    STEP(H, a, b, c, d, x[9], 0xd9d4d039, 4)  STEP(H, d, a, b, c, x[12], 0xe6db99e5, 11)
    STEP(H, c, d, a, b, x[15], 0x1fa27cf8, 16) STEP(H, b, c, d, a, x[2], 0xc4ac5665, 23)

    STEP(I, a, b, c, d, x[0], 0xf4292244, 6)  STEP(I, d, a, b, c, x[7], 0x432aff97, 10)
    STEP(I, c, d, a, b, x[14], 0xab9423a7, 15) STEP(I, b, c, d, a, x[5], 0xfc93a039, 21)
    STEP(I, a, b, c, d, x[12], 0x655b59c3, 6) STEP(I, d, a, b, c, x[3], 0x8f0ccc92, 10)
    STEP(I, c, d, a, b, x[10], 0xffeff47d, 15) STEP(I, b, c, d, a, x[1], 0x85845dd1, 21)
    STEP(I, a, b, c, d, x[8], 0x6fa87e4f, 6)  STEP(I, d, a, b, c, x[15], 0xfe2ce6e0, 10)
    STEP(I, c, d, a, b, x[6], 0xa3014314, 15) STEP(I, b, c, d, a, x[13], 0x4e0811a1, 21)
    STEP(I, a, b, c, d, x[4], 0xf7537e82, 6)  STEP(I, d, a, b, c, x[11], 0xbd3af235, 10)
    STEP(I, c, d, a, b, x[2], 0x2ad7d2bb, 15) STEP(I, b, c, d, a, x[9], 0xeb86d391, 21)

    m->a += a; m->b += b; m->c += c; m->d += d;
}

static VOID md5_init(struct md5 *m)
{
    m->a = 0x67452301; m->b = 0xefcdab89; m->c = 0x98badcfe; m->d = 0x10325476;
    m->lo = m->hi = 0;
}

static VOID md5_update(struct md5 *m, const UBYTE *p, ULONG n)
{
    ULONG used = m->lo & 63;

    if ((m->lo += n) < n)
        m->hi++;
    while (n > 0)
    {
        ULONG take = 64 - used;

        if (take > n)
            take = n;
        memcpy(m->buf + used, p, take);
        used += take; p += take; n -= take;
        if (used == 64)
        {
            md5_block(m, m->buf);
            used = 0;
        }
    }
}

static VOID md5_final(struct md5 *m, UBYTE out[16])
{
    static const UBYTE pad[64] = { 0x80 };
    UBYTE len[8];
    ULONG lo = m->lo << 3, hi = (m->hi << 3) | (m->lo >> 29);
    ULONG used = m->lo & 63;
    int   i;

    for (i = 0; i < 4; i++)
    {
        len[i] = (UBYTE)(lo >> (8 * i));
        len[i + 4] = (UBYTE)(hi >> (8 * i));
    }
    md5_update(m, pad, used < 56 ? 56 - used : 120 - used);
    md5_update(m, len, 8);
    for (i = 0; i < 4; i++)
    {
        out[i]      = (UBYTE)(m->a >> (8 * i));
        out[i + 4]  = (UBYTE)(m->b >> (8 * i));
        out[i + 8]  = (UBYTE)(m->c >> (8 * i));
        out[i + 12] = (UBYTE)(m->d >> (8 * i));
    }
}

static VOID record_md5(const char *path, UBYTE *buf, ULONG bufsize)
{
    struct md5 m;
    UBYTE      dig[16];
    char       hex[33], name[256];
    BPTR       fh;
    LONG       n;
    ULONG      size = 0;
    int        i;

    clean(name, sizeof(name), path);
    fh = Open((CONST_STRPTR)path, MODE_OLDFILE);
    if (fh == 0)
    {
        say("rec=md5 file=%s md5=missing size=0\n", (LONG)name);
        return;
    }
    md5_init(&m);
    while ((n = Read(fh, buf, bufsize)) > 0)
    {
        md5_update(&m, buf, (ULONG)n);
        size += (ULONG)n;
    }
    Close(fh);
    md5_final(&m, dig);
    for (i = 0; i < 16; i++)
    {
        hex[i * 2]     = "0123456789abcdef"[dig[i] >> 4];
        hex[i * 2 + 1] = "0123456789abcdef"[dig[i] & 15];
    }
    hex[32] = '\0';
    say("rec=md5 file=%s md5=%s size=%lu\n", (LONG)name, (LONG)hex, size);
}

static VOID append_names(char *out, LONG size, struct List *list)
{
    struct Node *n;
    char         nm[48];

    for (n = list->lh_Head; n->ln_Succ != NULL; n = n->ln_Succ)
    {
        clean(nm, sizeof(nm), n->ln_Name);
        if ((LONG)(strlen(out) + strlen(nm) + 2) >= size)
            break;
        if (out[0] != '\0')
            strcat(out, ",");
        strcat(out, nm);
    }
}

static VOID record_lib(struct List *list, const char *name, const char *key)
{
    struct Library *lib;
    char            id[96];
    int             count = 0;
    UWORD           ver = 0, rev = 0;
    struct Node    *n;

    id[0] = '\0';
    Forbid();
    for (n = list->lh_Head; n->ln_Succ != NULL; n = n->ln_Succ)
        if (n->ln_Name != NULL && strcmp(n->ln_Name, name) == 0)
        {
            lib = (struct Library *)n;
            if (count++ == 0)
            {
                ver = lib->lib_Version;
                rev = lib->lib_Revision;
                clean(id, sizeof(id), (const char *)lib->lib_IdString);
            }
        }
    Permit();
    say("rec=%s name=%s count=%ld version=%ld.%ld id=%s\n", (LONG)key,
        (LONG)name, (LONG)count, (LONG)ver, (LONG)rev,
        (LONG)(id[0] ? id : "-"));
}

static int do_record(char **files)
{
    UWORD              attn = SysBase->AttnFlags;
    ULONG              cache = CacheControl(0, 0);
    const char        *cpu = "68000";
    struct ConfigDev  *cd = NULL;
    struct MemHeader  *mh;
    struct DateStamp   ds;
    UBYTE             *buf;
    int                i = 0;

    if (attn & AFF_68060)      cpu = "68060";
    else if (attn & AFF_68040) cpu = "68040";
    else if (attn & AFF_68030) cpu = "68030";
    else if (attn & AFF_68020) cpu = "68020";
    else if (attn & AFF_68010) cpu = "68010";
    DateStamp(&ds);
    say("rec=sys cpu=%s attnflags=0x%04lx cachecontrol=0x%08lx exec=%ld.%ld "
        "fpu=%s wall_days=%ld wall_min=%ld wall_ticks=%ld\n",
        (LONG)cpu, (LONG)attn, cache,
        (LONG)SysBase->LibNode.lib_Version, (LONG)SysBase->LibNode.lib_Revision,
        (LONG)((attn & (AFF_68881 | AFF_68882 | AFF_FPU40)) ? "yes" : "no"),
        ds.ds_Days, ds.ds_Minute, ds.ds_Tick);

    ExpansionBase = (struct ExpansionBase *)OpenLibrary((CONST_STRPTR)"expansion.library", 33);
    if (ExpansionBase != NULL)
    {
        while ((cd = FindConfigDev(cd, -1, -1)) != NULL)
        {
            UBYTE type = cd->cd_Rom.er_Type & ERT_TYPEMASK;

            say("rec=board n=%ld mfr=%lu prod=%lu addr=0x%08lx size=0x%08lx "
                "zorro=%s\n", (LONG)i++, (ULONG)cd->cd_Rom.er_Manufacturer,
                (ULONG)cd->cd_Rom.er_Product, (ULONG)cd->cd_BoardAddr,
                cd->cd_BoardSize,
                (LONG)(type == ERT_ZORROIII ? "3" :
                       type == ERT_ZORROII ? "2" : "?"));
        }
        CloseLibrary((struct Library *)ExpansionBase);
    }

    {
        struct { char nm[48]; ULONG attr, lower, upper; } mem[16];
        int nmem = 0, k;

        Forbid();
        for (mh = (struct MemHeader *)SysBase->MemList.lh_Head;
             mh->mh_Node.ln_Succ != NULL && nmem < 16;
             mh = (struct MemHeader *)mh->mh_Node.ln_Succ, nmem++)
        {
            clean(mem[nmem].nm, sizeof(mem[nmem].nm), mh->mh_Node.ln_Name);
            mem[nmem].attr = mh->mh_Attributes;
            mem[nmem].lower = (ULONG)mh->mh_Lower;
            mem[nmem].upper = (ULONG)mh->mh_Upper;
        }
        Permit();
        for (k = 0; k < nmem; k++)
            say("rec=mem name=%s attr=0x%04lx lower=0x%08lx upper=0x%08lx "
                "fast=%s\n", (LONG)mem[k].nm, mem[k].attr, mem[k].lower,
                mem[k].upper, (LONG)((mem[k].attr & MEMF_FAST) ? "1" : "0"));
    }

    record_lib(&SysBase->LibList, "bsdsocket.library", "stacklib");
    record_lib(&SysBase->DeviceList, "x-surf-100.device", "driver");

    line[0] = '\0';
    Forbid();
    append_names(line, sizeof(line), &SysBase->PortList);
    Permit();
    say("rec=ports names=%s\n", (LONG)line);

    line[0] = '\0';
    Forbid();
    append_names(line, sizeof(line), &SysBase->TaskReady);
    append_names(line, sizeof(line), &SysBase->TaskWait);
    Permit();
    say("rec=tasks names=%s\n", (LONG)line);

    buf = AllocMem(8192, MEMF_ANY);
    if (buf != NULL)
    {
        for (; files != NULL && *files != NULL; files++)
            record_md5(*files, buf, 8192);
        FreeMem(buf, 8192);
    }
    return 0;
}

/* ---------------------------------------------------------------- main -- */

int main(VOID)
{
    LONG           args[ARG_COUNT];
    struct RDArgs *rda;
    const char    *dir, *arms;
    LONG           secs;
    int            rc = 20;

    memset(args, 0, sizeof(args));
    rda = ReadArgs((CONST_STRPTR)TEMPLATE, args, NULL);
    if (rda == NULL)
    {
        say("rec=error reason=badargs\n");
        return 20;
    }
    dir = args[ARG_DIR] ? (const char *)args[ARG_DIR] : DEF_DIR;
    arms = args[ARG_ARMS] ? (const char *)args[ARG_ARMS] : DEF_ARMS;
    secs = args[ARG_SECS] ? *(LONG *)args[ARG_SECS] : -1;

    if (args[ARG_SELECT])
        rc = do_select(dir, arms);
    else if (args[ARG_WATCHDOG])
        rc = do_watchdog(dir, secs > 0 ? secs : 600,
                         (const char *)args[ARG_CANCELFILE]);
    else if (args[ARG_RECORD])
        rc = do_record((char **)args[ARG_FILES]);
    else if (args[ARG_NEXT])
        rc = do_next();
    else if (args[ARG_HOLD])
        rc = do_hold(secs > 0 ? secs : 0);
    else if (args[ARG_REBOOT])
    {
        say("rec=reboot\n");
        Flush(Output());
        Delay(3 * TICKS_PER_SECOND);
        ColdReboot();
    }
    else
        say("rec=error reason=nosubcommand\n");

    FreeArgs(rda);
    return rc;
}

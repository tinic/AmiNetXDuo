/*
 * InstallSameFile, answer whether two AmigaDOS names resolve to one object.
 *
 * Commodore Installer has checksums but no file-identity primitive.  A
 * checksum cannot distinguish the drawer's library from a byte-identical
 * copy earlier in LIBS:, which is exactly the distinction a private install
 * has to make.  Keep the missing primitive here, using dos.library's
 * SameLock(), and keep it beside the Installer rather than installing it.
 *
 * Exit status is RETURN_OK only for the same object, RETURN_WARN for two
 * different objects, and RETURN_ERROR when either name cannot be locked.
 *
 * DEVICES: FIRST is a kept interface file, SECOND one or more Devs/Networks
 * drawers of this installation.  A DEVICE line naming one of our drivers by
 * an absolute path into one of them is rewritten to name it through
 * AmiNetXDuo:, as a fresh drawer install writes it; every other byte stays
 * (devicehome.c).  The Installer cannot read the line back, so this is the
 * one place a kept file can follow the assign.  RETURN_OK when the file is
 * as it should be, RETURN_ERROR when it could not be read or replaced, and
 * then it is unchanged.
 *
 * BYTES: RETURN_OK when FIRST and SECOND hold the same bytes, RETURN_WARN
 * when they differ, RETURN_ERROR when either cannot be read.  The Installer
 * has no compare of its own, and a backup the same as one already kept is
 * not kept twice.
 *
 * MARK: RETURN_OK when the file FIRST holds the bytes SECOND anywhere,
 * RETURN_WARN when it does not, RETURN_ERROR when it cannot be read.  Exact
 * bytes, case and all, in blocks: AmigaDOS Search ignores case and reads
 * lines, which in a program or the certificate store mean nothing.
 *
 * HOSTLINE: FIRST is a name_resolution file; the name the stack takes from it
 * (ami_cfg_resolver_hostname_file, the stack's own parser) is written to
 * SECOND as a hostname line, so the Installer can carry a name set by hand
 * over into the file it writes.  RETURN_OK with a line written, RETURN_WARN when
 * there is none, RETURN_ERROR when either file fails.
 *
 * DRAWER: FIRST is S:AmiNetXDuo-Files; the drawer the installation that
 * wrote it lives in (its T:AmiNetXDuo-DrawerKept line) goes into the global
 * variable SECOND, so the Installer can read it with getenv.  RETURN_OK with
 * it set, RETURN_WARN when the record names none, RETURN_ERROR when the file
 * cannot be read.
 *
 * RECEIPT: FIRST is the install receipt UpdateAmiNetXDuo INSTALL reads
 * (src/tools/updinstall.h), SECOND triples of SOURCE DEST NAME.  A SOURCE
 * drawer is each plain file in it but icons, at DEST/<file>, named
 * NAME<file>; a SOURCE file is DEST itself, named NAME.  Every DEST holding
 * the same bytes as its SOURCE -- the file this installation placed --
 * gets a "<sha256> <name>" line, once per name, after the lines FIRST
 * holds already; written to FIRST.tmp and renamed over FIRST.  RETURN_OK
 * with the receipt written, or with nothing to add and FIRST as it was;
 * RETURN_ERROR otherwise, and then FIRST is as it was.  A FIRST that
 * cannot be opened for any reason but not being there is an error.  The Installer deletes FIRST first and calls once per drawer, which
 * keeps each command line short.
 *
 * SPDX-License-Identifier: MIT
 */

/* First, as tools.h has it: c68k_sha256.h's UINT and UCHAR. */
#include "tx_api.h"

#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/rdargs.h>
#include <dos/var.h>
#include <proto/dos.h>
#include <proto/exec.h>

#include <string.h>

#include "aminetxduo/version.h"
#include "aminetxduo/config.h"
#include "devicehome.h"
#include "c68k_sha256.h"
#include "updinstall.h"

const char *const tool_name = "InstallSameFile";

static const char version_tag[] __attribute__((used)) =
    TOOL_VERSTAG("InstallSameFile");

#define TEMPLATE "FIRST/A,SECOND/A/M,DEVICES/S,HOSTLINE/S,BYTES/S,MARK/S,DRAWER/S," \
                 "RECEIPT/S"

/* An interface file is a few hundred bytes; the cap only bounds a stray. */
#define DEVICES_FILE_MAX 65536L
#define DEVICES_GROWTH   256L

enum
{
    ARG_FIRST = 0,
    ARG_SECOND,
    ARG_DEVICES,
    ARG_HOSTLINE,
    ARG_BYTES,
    ARG_MARK,
    ARG_DRAWER,
    ARG_RECEIPT,
    ARG_COUNT
};

static int same_object(const char *a, const char *b)
{
    BPTR first  = Lock((CONST_STRPTR)a, ACCESS_READ);
    BPTR second = Lock((CONST_STRPTR)b, ACCESS_READ);
    LONG same   = LOCK_DIFFERENT;

    if (first != 0 && second != 0)
        same = SameLock(first, second);
    if (second != 0)
        UnLock(second);
    if (first != 0)
        UnLock(first);
    if (first == 0 || second == 0)
        return -1;
    return same == LOCK_SAME;
}

static int ours(const char *dir, void *ctx)
{
    STRPTR *drawer;

    for (drawer = (STRPTR *)ctx; *drawer != NULL; drawer++)
        if (same_object(dir, (const char *)*drawer) == 1)
            return 1;
    return 0;
}

static ULONG cat(char *out, ULONG cap, const char *a, const char *b)
{
    ULONG n = 0;

    while (*a != '\0' && n + 1 < cap) out[n++] = *a++;
    while (*b != '\0' && n + 1 < cap) out[n++] = *b++;
    out[n] = '\0';
    return (*a == '\0' && *b == '\0') ? n : 0;
}

/* The kept file's protection bits and filenote go with it; a file that
   cannot be examined is replaced with the defaults, as before. */
static VOID keep_attributes(const char *from, const char *to)
{
    struct FileInfoBlock *fib;
    BPTR                  lock;

    fib = (struct FileInfoBlock *)AllocDosObject(DOS_FIB, NULL);
    if (fib == NULL)
        return;
    lock = Lock((CONST_STRPTR)from, ACCESS_READ);
    if (lock != 0)
    {
        if (Examine(lock, fib))
        {
            (VOID)SetProtection((CONST_STRPTR)to, fib->fib_Protection);
            (VOID)SetComment((CONST_STRPTR)to, (CONST_STRPTR)fib->fib_Comment);
        }
        UnLock(lock);
    }
    FreeDosObject(DOS_FIB, fib);
}

/* NetPrefs' order: the new bytes are whole on disk before the old name goes,
   and both spare names end in .info, which the interface scan skips. */
static BOOL replace_file(const char *path, const char *data, LONG length)
{
    char tmp[256];
    char old[256];
    BPTR fh;
    LONG wrote;

    if (cat(tmp, sizeof(tmp), path, ".new.info") == 0 ||
        cat(old, sizeof(old), path, ".old.info") == 0)
        return FALSE;

    (VOID)DeleteFile((CONST_STRPTR)tmp);
    fh = Open((CONST_STRPTR)tmp, MODE_NEWFILE);
    if (fh == 0)
        return FALSE;
    wrote = Write(fh, (APTR)data, length);
    Close(fh);
    if (wrote != length)
    {
        (VOID)DeleteFile((CONST_STRPTR)tmp);
        return FALSE;
    }
    keep_attributes(path, tmp);

    (VOID)DeleteFile((CONST_STRPTR)old);
    if (!Rename((CONST_STRPTR)path, (CONST_STRPTR)old))
    {
        (VOID)DeleteFile((CONST_STRPTR)tmp);
        return FALSE;
    }
    if (!Rename((CONST_STRPTR)tmp, (CONST_STRPTR)path))
    {
        (VOID)Rename((CONST_STRPTR)old, (CONST_STRPTR)path);
        (VOID)DeleteFile((CONST_STRPTR)tmp);
        return FALSE;
    }
    (VOID)DeleteFile((CONST_STRPTR)old);
    return TRUE;
}

static LONG rehome_devices(const char *path, STRPTR *drawers)
{
    BPTR   fh;
    LONG   size;
    LONG   got;
    char  *old;
    char  *out;
    size_t newlen = 0;
    long   rewritten;
    LONG   rc = RETURN_ERROR;

    fh = Open((CONST_STRPTR)path, MODE_OLDFILE);
    if (fh == 0)
        return RETURN_ERROR;
    (VOID)Seek(fh, 0, OFFSET_END);
    size = Seek(fh, 0, OFFSET_BEGINNING);      /* the previous position */
    if (size < 0 || size > DEVICES_FILE_MAX)
    {
        Close(fh);
        return RETURN_ERROR;
    }
    old = (char *)AllocVec((ULONG)(2 * size + DEVICES_GROWTH + 1), MEMF_ANY);
    if (old == NULL)
    {
        Close(fh);
        return RETURN_ERROR;
    }
    got = Read(fh, old, size);
    Close(fh);
    out = old + size;

    if (got == size)
    {
        rewritten = dh_rehome(old, (size_t)size, out,
                              (size_t)(size + DEVICES_GROWTH), &newlen,
                              ours, drawers);
        if (rewritten == 0)
            rc = RETURN_OK;
        else if (rewritten > 0 && replace_file(path, out, (LONG)newlen))
            rc = RETURN_OK;
    }
    FreeVec(old);
    return rc;
}

static LONG copy_hostline(const char *from, const char *to)
{
    char  name[AMI_CFG_NAME_LEN];
    char  line[2 * AMI_CFG_NAME_LEN + 16];
    BPTR  fh;
    LONG  n, got;

    if (!ami_cfg_resolver_hostname_file(from, name, sizeof(name)))
        return RETURN_WARN;
    n = (LONG)ami_cfg_hostname_line(name, line, sizeof(line));
    if (n == 0)
        return RETURN_ERROR;
    fh = Open((CONST_STRPTR)to, MODE_NEWFILE);
    if (fh == 0)
        return RETURN_ERROR;
    got = Write(fh, line, n);
    Close(fh);
    return (got == n) ? RETURN_OK : RETURN_ERROR;
}

static LONG same_bytes(const char *a, const char *b)
{
    static UBYTE ba[2048];
    static UBYTE bb[2048];
    BPTR fa = Open((CONST_STRPTR)a, MODE_OLDFILE);
    BPTR fb = Open((CONST_STRPTR)b, MODE_OLDFILE);
    LONG rc = RETURN_OK;

    if (fa == 0 || fb == 0)
        rc = RETURN_ERROR;
    while (rc == RETURN_OK)
    {
        LONG na = Read(fa, ba, sizeof(ba));
        LONG nb = Read(fb, bb, sizeof(bb));
        LONG i;

        if (na < 0 || nb < 0)
            rc = RETURN_ERROR;
        else if (na != nb)
            rc = RETURN_WARN;
        else if (na == 0)
            break;
        else
            for (i = 0; i < na; i++)
                if (ba[i] != bb[i])
                {
                    rc = RETURN_WARN;
                    break;
                }
    }
    if (fa != 0)
        Close(fa);
    if (fb != 0)
        Close(fb);
    return rc;
}

#define MARK_MAX 128L

static LONG has_mark(const char *file, const char *mark)
{
    static UBYTE buf[2048 + MARK_MAX];
    LONG         len = (LONG)strlen(mark);
    LONG         have = 0;
    LONG         rc = RETURN_WARN;
    BPTR         fh;

    if (len <= 0 || len > MARK_MAX)
        return RETURN_ERROR;
    fh = Open((CONST_STRPTR)file, MODE_OLDFILE);
    if (fh == 0)
        return RETURN_ERROR;
    for (;;)
    {
        LONG n = Read(fh, buf + have, 2048);
        LONG i;

        if (n < 0)
        {
            rc = RETURN_ERROR;
            break;
        }
        have += n;
        for (i = 0; i + len <= have; i++)
            if (buf[i] == (UBYTE)mark[0] &&
                memcmp(buf + i, mark, (size_t)len) == 0)
                break;
        if (i + len <= have)
        {
            rc = RETURN_OK;
            break;
        }
        if (n == 0)
            break;
        /* Keep the tail a mark could start in. */
        if (have > len - 1)
        {
            memmove(buf, buf + have - (len - 1), (size_t)(len - 1));
            have = len - 1;
        }
    }
    Close(fh);
    return rc;
}

static LONG record_drawer(const char *file, const char *var)
{
    static const char key[] = ">T:AmiNetXDuo-DrawerKept \"";
    BPTR  fh;
    LONG  size, got, i, j;
    char *buf;
    LONG  rc = RETURN_WARN;

    fh = Open((CONST_STRPTR)file, MODE_OLDFILE);
    if (fh == 0)
        return RETURN_ERROR;
    Seek(fh, 0, OFFSET_END);
    size = Seek(fh, 0, OFFSET_BEGINNING);
    if (size <= 0 || (buf = (char *)AllocVec((ULONG)size + 1, MEMF_ANY)) == NULL)
    {
        Close(fh);
        return size == 0 ? RETURN_WARN : RETURN_ERROR;
    }
    got = Read(fh, buf, size);
    Close(fh);
    if (got != size)
        rc = RETURN_ERROR;
    else
        for (i = 0; i + (LONG)sizeof(key) - 1 <= size; i++)
            if (memcmp(buf + i, key, sizeof(key) - 1) == 0)
            {
                i += (LONG)sizeof(key) - 1;
                for (j = i; j < size && buf[j] != '"' && buf[j] != '\n'; j++)
                    ;
                if (j < size && buf[j] == '"' && j > i &&
                    SetVar((CONST_STRPTR)var, (CONST_STRPTR)(buf + i), j - i,
                           GVF_GLOBAL_ONLY))
                    rc = RETURN_OK;
                break;
            }
    FreeVec(buf);
    return rc;
}

/* ---- RECEIPT ---- */

#define RECEIPT_MAX     (32L * 1024L)
#define RECEIPT_PATH    256

static char *rc_text;
static LONG  rc_len;
static char  rc_src[RECEIPT_PATH];
static char  rc_dst[RECEIPT_PATH];
static char  rc_name[RECEIPT_PATH];

static int rc_lower(int c)
{
    return (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c;
}

/* The receipt has a line for `name` already. */
static int rc_listed(const char *name)
{
    LONG i = 0, n = (LONG)strlen(name);

    while (i < rc_len)
    {
        LONG e = i, k;

        while (e < rc_len && rc_text[e] != '\n')
            e++;
        if (e - i - 65 == n)
        {
            for (k = 0; k < n; k++)
                if (rc_lower((unsigned char)rc_text[i + 65 + k]) !=
                    rc_lower((unsigned char)name[k]))
                    break;
            if (k == n)
                return 1;
        }
        i = e + 1;
    }
    return 0;
}

static int rc_hash(const char *path, UBYTE *sha)
{
    static UBYTE  buf[2048];
    C68K_SHA256   ctx;
    BPTR          fh = Open((CONST_STRPTR)path, MODE_OLDFILE);
    LONG          n;

    if (fh == 0)
        return 0;
    (VOID)c68k_sha256_initialize(&ctx, NX_CRYPTO_HASH_SHA256);
    while ((n = Read(fh, buf, sizeof(buf))) > 0)
        (VOID)c68k_sha256_update(&ctx, buf, (UINT)n);
    Close(fh);
    if (n < 0)
        return 0;
    (VOID)c68k_sha256_digest_calculate(&ctx, sha, NX_CRYPTO_HASH_SHA256);
    return 1;
}

/* One line for DEST named NAME when its bytes are SOURCE's.  0 on an
   error that must fail the receipt: no room, a placed file unreadable. */
static int rc_add(const char *src, const char *dst, const char *name)
{
    static const char hex[] = "0123456789abcdef";
    UBYTE sha[32];
    LONG  n = (LONG)strlen(name), i;

    if (same_bytes(src, dst) != RETURN_OK || rc_listed(name))
        return 1;
    if (!rc_hash(dst, sha) || rc_len + 66 + n > RECEIPT_MAX)
        return 0;
    for (i = 0; i < 32; i++)
    {
        rc_text[rc_len++] = hex[sha[i] >> 4];
        rc_text[rc_len++] = hex[sha[i] & 15];
    }
    rc_text[rc_len++] = ' ';
    memcpy(rc_text + rc_len, name, (size_t)n);
    rc_len += n;
    rc_text[rc_len++] = '\n';
    return 1;
}

static int rc_join(char *out, const char *a, const char *b)
{
    size_t n = strlen(a);

    if (n + strlen(b) + 2 > RECEIPT_PATH)
        return 0;
    strcpy(out, a);
    if (n > 0 && a[n - 1] != ':' && a[n - 1] != '/')
        strcat(out, "/");
    strcat(out, b);
    return 1;
}

static int rc_triple(const char *src, const char *dst, const char *name)
{
    struct FileInfoBlock *fib;
    BPTR                  lock = Lock((CONST_STRPTR)src, ACCESS_READ);
    int                   ok = 1;

    if (lock == 0)
        return 1;                       /* not in this archive            */
    fib = (struct FileInfoBlock *)AllocDosObject(DOS_FIB, NULL);
    if (fib == NULL || !Examine(lock, fib))
        ok = 0;
    else if (fib->fib_DirEntryType < 0)
        ok = rc_add(src, dst, name);
    else
        while (ok && ExNext(lock, fib))
        {
            size_t fl = strlen((const char *)fib->fib_FileName);

            if (fib->fib_DirEntryType >= 0 ||
                (fl > 5 && strcmp((const char *)fib->fib_FileName + fl - 5, ".info") == 0))
                continue;
            if (strlen(name) + fl + 1 > RECEIPT_PATH ||
                !rc_join(rc_src, src, (const char *)fib->fib_FileName) ||
                !rc_join(rc_dst, dst, (const char *)fib->fib_FileName))
            {
                ok = 0;
                break;
            }
            strcpy(rc_name, name);
            strcat(rc_name, (const char *)fib->fib_FileName);
            ok = rc_add(rc_src, rc_dst, rc_name);
        }
    if (fib != NULL)
        FreeDosObject(DOS_FIB, fib);
    UnLock(lock);
    return ok;
}

static LONG write_receipt(const char *file, STRPTR *second)
{
    char  tmp[RECEIPT_PATH];
    ULONG n = 0;
    LONG  rc = RETURN_ERROR;
    BPTR  fh;

    while (second[n] != NULL)
        n++;
    if (n == 0 || n % 3 != 0 || strlen(file) + 5 > sizeof(tmp))
        return RETURN_ERROR;
    rc_text = (char *)AllocVec((ULONG)RECEIPT_MAX, MEMF_ANY);
    if (rc_text == NULL)
        return RETURN_ERROR;
    rc_len = 0;
    fh = Open((CONST_STRPTR)file, MODE_OLDFILE);
    /* Only a FIRST that is not there starts empty: any other failure to
       open it would drop the drawers already in it. */
    if (fh == 0 &&
        upd_rcpt_write_plan(0, IoErr() == ERROR_OBJECT_NOT_FOUND, 0) == UPD_RW_ERROR)
    {
        FreeVec(rc_text);
        return RETURN_ERROR;
    }
    if (fh != 0)
    {
        rc_len = Read(fh, rc_text, RECEIPT_MAX);
        Close(fh);
        if (rc_len < 0 || rc_len >= RECEIPT_MAX ||
            (rc_len > 0 && rc_text[rc_len - 1] != '\n'))
        {
            FreeVec(rc_text);
            return RETURN_ERROR;
        }
    }
    for (n = 0; second[n] != NULL; n += 3)
        if (!rc_triple((const char *)second[n], (const char *)second[n + 1],
                       (const char *)second[n + 2]))
            break;
    strcpy(tmp, file);
    strcat(tmp, ".tmp");
    /* A drawer with none of this flavour's files (C/fat on a full
       install) adds nothing: done, and FIRST as it was. */
    if (second[n] == NULL && upd_rcpt_write_plan(1, 0, (ULONG)rc_len) == UPD_RW_NONE)
        rc = RETURN_OK;
    else if (second[n] == NULL &&
             (fh = Open((CONST_STRPTR)tmp, MODE_NEWFILE)) != 0)
    {
        LONG wrote = Write(fh, rc_text, rc_len);

        if (Close(fh) && wrote == rc_len)
        {
            (VOID)DeleteFile((CONST_STRPTR)file);
            if (Rename((CONST_STRPTR)tmp, (CONST_STRPTR)file))
                rc = RETURN_OK;
        }
        if (rc != RETURN_OK)
            (VOID)DeleteFile((CONST_STRPTR)tmp);
    }
    FreeVec(rc_text);
    return rc;
}

int main(int argc, char **argv)
{
    LONG           args[ARG_COUNT] = { 0 };
    struct RDArgs *rda;
    STRPTR        *second;
    LONG           rc;

    (VOID)argv;

    if (argc == 0)
        return RETURN_FAIL;

    rda = ReadArgs((CONST_STRPTR)TEMPLATE, args, NULL);
    if (rda == NULL)
        return RETURN_ERROR;

    second = (STRPTR *)args[ARG_SECOND];
    if (args[ARG_RECEIPT] != 0)
        rc = write_receipt((const char *)args[ARG_FIRST], second);
    else if (args[ARG_DRAWER] != 0)
        rc = (second[1] != NULL) ? RETURN_ERROR
                                 : record_drawer((const char *)args[ARG_FIRST],
                                                 (const char *)second[0]);
    else if (args[ARG_MARK] != 0)
        rc = (second[1] != NULL) ? RETURN_ERROR
                                 : has_mark((const char *)args[ARG_FIRST],
                                            (const char *)second[0]);
    else if (args[ARG_BYTES] != 0)
        rc = (second[1] != NULL) ? RETURN_ERROR
                                 : same_bytes((const char *)args[ARG_FIRST],
                                              (const char *)second[0]);
    else if (args[ARG_HOSTLINE] != 0)
        rc = (second[1] != NULL) ? RETURN_ERROR
                                 : copy_hostline((const char *)args[ARG_FIRST],
                                                 (const char *)second[0]);
    else if (args[ARG_DEVICES] != 0)
    {
        /* A DEVICE line naming a volume that is not mounted is simply not
           ours; it must not stop the Installer at "Please insert". */
        struct Process *me  = (struct Process *)FindTask(NULL);
        APTR            win = me->pr_WindowPtr;

        me->pr_WindowPtr = (APTR)-1;
        rc = rehome_devices((const char *)args[ARG_FIRST], second);
        me->pr_WindowPtr = win;
    }
    else if (second[1] != NULL)
        rc = RETURN_ERROR;
    else
    {
        int same = same_object((const char *)args[ARG_FIRST],
                               (const char *)second[0]);

        rc = same < 0 ? RETURN_ERROR : same ? RETURN_OK : RETURN_WARN;
    }

    FreeArgs(rda);
    return rc;
}

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
 * HOSTLINE: FIRST is a name_resolution file; the name the stack takes from it
 * (ami_cfg_resolver_hostname_file, the stack's own parser) is written to
 * SECOND as a hostname line, so the Installer can carry a name set by hand
 * over into the file it writes.  RETURN_OK with a line written, RETURN_WARN when
 * there is none, RETURN_ERROR when either file fails.
 *
 * SPDX-License-Identifier: MIT
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/rdargs.h>
#include <proto/dos.h>
#include <proto/exec.h>

#include "aminetxduo/version.h"
#include "aminetxduo/config.h"
#include "devicehome.h"

const char *const tool_name = "InstallSameFile";

static const char version_tag[] __attribute__((used)) =
    TOOL_VERSTAG("InstallSameFile");

#define TEMPLATE "FIRST/A,SECOND/A/M,DEVICES/S,HOSTLINE/S"

/* An interface file is a few hundred bytes; the cap only bounds a stray. */
#define DEVICES_FILE_MAX 65536L
#define DEVICES_GROWTH   256L

enum
{
    ARG_FIRST = 0,
    ARG_SECOND,
    ARG_DEVICES,
    ARG_HOSTLINE,
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
    if (args[ARG_HOSTLINE] != 0)
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

/*
 * AmigaDOS filesystem policy for httpd's WebDAV methods.  Keeping the scratch
 * objects in the caller makes the server's single-task ownership explicit;
 * this module neither allocates nor hides mutable state.
 * SPDX-License-Identifier: MIT
 */

#include "tools.h"
#include "httpfs.h"
#include "httpstr.h"

/* An AmigaDOS error as an HTTP status.  One table, so every write method gives
   the same answer to the same failure. */
ULONG http_fs_status(LONG err)
{
    switch (err)
    {
        case ERROR_OBJECT_NOT_FOUND:
        case ERROR_DIR_NOT_FOUND:           return 409;
        case ERROR_OBJECT_EXISTS:           return 405;
        case ERROR_DIRECTORY_NOT_EMPTY:
        case ERROR_OBJECT_IN_USE:           return 409;
        case ERROR_DISK_FULL:               return 507;
        /* A name the filesystem will not carry, too long for OFS, or a
           character it reserves.  Refused rather than truncated into a name
           that would collide with a file already there. */
        case ERROR_INVALID_COMPONENT_NAME:
        case ERROR_BAD_STREAM_NAME:         return 400;
        default:                            return 403;
    }
}

/* The drawer a path lives in: "Work:Public/a/b" is "Work:Public/a" and
   "RAM:foo" is "RAM:". */
BOOL http_fs_parent(const char *path, char *out, ULONG outlen)
{
    ULONG n = hs_len(path);
    ULONG cut = 0;
    ULONG i;
    BOOL  found = FALSE;

    for (i = 0; i < n; i++)
    {
        if (path[i] == '/')
        {
            cut   = i;
            found = TRUE;
        }
        else if (path[i] == ':')
        {
            cut   = i + 1UL;
            found = TRUE;
        }
    }

    if (!found || cut + 1UL >= outlen)
        return FALSE;

    for (i = 0; i < cut; i++)
        out[i] = path[i];
    out[cut] = '\0';

    return TRUE;
}

BOOL http_fs_name_differs(const struct FileInfoBlock *fib, const char *name)
{
    if (name[0] == '\0')
        return FALSE;

    return hs_equal((const char *)fib->fib_FileName, name) ? FALSE : TRUE;
}

BOOL http_fs_name_cut(struct FileInfoBlock *scratch, const char *path,
                      const char *name)
{
    BPTR lock;
    BOOL cut = FALSE;

    if (scratch == NULL || name[0] == '\0')
        return FALSE;

    lock = Lock((CONST_STRPTR)path, ACCESS_READ);
    if (lock == (BPTR)0)
        return FALSE;

    if (Examine(lock, scratch))
        cut = http_fs_name_differs(scratch, name);

    UnLock(lock);
    return cut;
}

/* Only called when nothing is at path: MODE_NEWFILE would otherwise truncate
   the existing file. */
BOOL http_fs_name_survives(struct FileInfoBlock *scratch, const char *path,
                           const char *name)
{
    BPTR probe;
    BOOL cut;

    if (name[0] == '\0')
        return TRUE;

    probe = Open((CONST_STRPTR)path, MODE_NEWFILE);
    if (probe == (BPTR)0)
        return TRUE;

    (VOID)Close(probe);
    cut = http_fs_name_cut(scratch, path, name);
    (VOID)DeleteFile((CONST_STRPTR)path);

    return cut ? FALSE : TRUE;
}

BOOL http_fs_entry_is_link(LONG type)
{
    return (type == ST_SOFTLINK || type == ST_LINKDIR) ? TRUE : FALSE;
}

VOID http_fs_etag(ULONG size, const struct DateStamp *ds,
                  char *out, ULONG outlen)
{
    ULONG used = 0;
    BOOL  ok;

    out[0] = '\0';
    ok = hs_append(out, outlen, &used, "\"");
    ok = ok && hs_append_num(out, outlen, &used, size);
    ok = ok && hs_append(out, outlen, &used, "-");
    ok = ok && hs_append_num(out, outlen, &used, (ULONG)ds->ds_Days);
    ok = ok && hs_append(out, outlen, &used, "-");
    ok = ok && hs_append_num(out, outlen, &used, (ULONG)ds->ds_Minute);
    ok = ok && hs_append(out, outlen, &used, "-");
    ok = ok && hs_append_num(out, outlen, &used, (ULONG)ds->ds_Tick);
    ok = ok && hs_append(out, outlen, &used, "\"");

    if (!ok)
        out[0] = '\0';
}

VOID http_fs_etag_of(struct FileInfoBlock *scratch, const char *path,
                     char *out, ULONG outlen)
{
    BPTR lock;

    out[0] = '\0';
    if (scratch == NULL)
        return;

    lock = Lock((CONST_STRPTR)path, ACCESS_READ);
    if (lock == (BPTR)0)
        return;

    if (Examine(lock, scratch) && scratch->fib_DirEntryType <= 0)
        http_fs_etag((ULONG)scratch->fib_Size, &scratch->fib_Date,
                     out, outlen);

    UnLock(lock);
}

LONG http_fs_kind(struct FileInfoBlock *scratch, const char *path)
{
    BPTR lock = Lock((CONST_STRPTR)path, ACCESS_READ);
    LONG kind = 0;

    if (lock == (BPTR)0)
        return -1;

    if (scratch != NULL && Examine(lock, scratch) &&
        scratch->fib_DirEntryType > 0)
        kind = 1;

    UnLock(lock);
    return kind;
}

ULONG http_fs_free_bytes(struct InfoData *scratch, const char *path)
{
    BPTR  lock;
    ULONG blocks;
    ULONG per;

    if (scratch == NULL)
        return 0;

    lock = Lock((CONST_STRPTR)path, ACCESS_READ);
    if (lock == (BPTR)0)
        return 0;

    if (!Info(lock, scratch))
    {
        UnLock(lock);
        return 0;
    }
    UnLock(lock);

    if (scratch->id_NumBlocks <= scratch->id_NumBlocksUsed ||
        scratch->id_BytesPerBlock <= 0)
        return 0;

    blocks = (ULONG)(scratch->id_NumBlocks - scratch->id_NumBlocksUsed);
    per    = (ULONG)scratch->id_BytesPerBlock;
    if (blocks > 0xffffffffUL / per)
        return 0xffffffffUL;

    return blocks * per;
}

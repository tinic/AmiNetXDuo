/*
 * Host test shim for config_file.c: the base proto/dos.h (Open/Read/Seek/
 * Close over shim_dos_files) plus a small volume model for Lock(), Examine()
 * and the DEVS: multi-assign.
 *
 * Every object is a full path on a volume ("SYS:Devs/NetInterfaces/genet").
 * Two assigns exist: AmiNetXDuo: (one directory or none) and DEVS: (an
 * ordered member list, as `Assign DEVS: x ADD` builds it).  A lock is an
 * index into the object table plus one, so SameLock() is an integer compare.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_TEST_FILESHIM_PROTO_DOS_H
#define AMINETXDUO_TEST_FILESHIM_PROTO_DOS_H

#include_next <proto/dos.h>

#include <dos/dosextens.h>
#include <stdio.h>
#include <strings.h>

#define SHIM_VOL_MAX        32
#define SHIM_DEVS_MAX       4

extern const char *shim_vol_obj[SHIM_VOL_MAX];   /* full paths */
extern int         shim_vol_dir[SHIM_VOL_MAX];   /* 1 drawer, 0 file */
extern int         shim_vol_count;
extern const char *shim_assign_anxd;             /* NULL: not assigned */
extern const char *shim_assign_devs[SHIM_DEVS_MAX];
extern int         shim_assign_devs_count;
extern int         shim_vol_locks;               /* outstanding */
extern int         shim_vol_unwindowed_locks;    /* with pr_WindowPtr -1 */
extern struct DevProc shim_devproc;
extern int            shim_devproc_member;
extern int            shim_devproc_out;

static inline void shim_vol_add(const char *path, int dir)
{
    if (shim_vol_count < SHIM_VOL_MAX)
    {
        shim_vol_obj[shim_vol_count] = path;
        shim_vol_dir[shim_vol_count] = dir;
        shim_vol_count++;
    }
}

static inline int shim_vol_find(const char *full)
{
    int i;

    for (i = 0; i < shim_vol_count; i++)
        if (strcasecmp(shim_vol_obj[i], full) == 0)
            return i;
    return -1;
}

/* `rest` below `root`, as AmigaDOS joins them: no '/' after a colon. */
static inline int shim_vol_find_in(const char *root, const char *rest)
{
    char   full[256];
    size_t n = strlen(root);

    if (*rest == '\0')
        return shim_vol_find(root);
    snprintf(full, sizeof(full), "%s%s%s", root,
             (n > 0 && root[n - 1] == ':') ? "" : "/", rest);
    return shim_vol_find(full);
}

static inline int shim_vol_has_prefix(const char *name, const char *assign,
                                      const char **rest)
{
    size_t n = strlen(assign);

    if (strncasecmp(name, assign, n) != 0 || name[n] != ':')
        return 0;
    *rest = name + n + 1;
    return 1;
}

static inline BPTR Lock(STRPTR name, LONG mode)
{
    struct Process *self = (struct Process *)shim_current_task;
    const char     *path = (const char *)name;
    const char     *rest;
    int             i, at = -1;

    (void)mode;
    if (self != NULL && self->pr_WindowPtr == (APTR)-1L)
        shim_vol_unwindowed_locks++;

    if (shim_vol_has_prefix(path, "AmiNetXDuo", &rest))
    {
        if (shim_assign_anxd != NULL)
            at = shim_vol_find_in(shim_assign_anxd, rest);
    }
    else if (shim_vol_has_prefix(path, "DEVS", &rest))
    {
        /* A multi-assign: each member is tried in turn. */
        for (i = 0; i < shim_assign_devs_count && at < 0; i++)
            at = shim_vol_find_in(shim_assign_devs[i], rest);
    }
    else
        at = shim_vol_find(path);

    if (at < 0)
        return (BPTR)0;
    shim_vol_locks++;
    return (BPTR)(at + 1);
}

static inline void UnLock(BPTR lock)
{
    if (lock != 0)
        shim_vol_locks--;
}

static inline LONG SameLock(BPTR a, BPTR b)
{
    return (a == b) ? LOCK_SAME : LOCK_DIFFERENT;
}

/* DEVS: only; the next member each call, NULL past the last. */
static inline struct DevProc *GetDeviceProc(STRPTR name, struct DevProc *dp)
{
    const char *rest;

    if (!shim_vol_has_prefix((const char *)name, "DEVS", &rest))
        return NULL;

    if (dp == NULL)
    {
        shim_devproc_member = 0;
        shim_devproc_out++;
    }
    else
        shim_devproc_member++;

    for (; shim_devproc_member < shim_assign_devs_count; shim_devproc_member++)
    {
        int at = shim_vol_find(shim_assign_devs[shim_devproc_member]);

        if (at < 0)
            continue;
        shim_devproc.dvp_Lock  = (BPTR)(at + 1);
        shim_devproc.dvp_Flags = (shim_assign_devs_count > 1) ? DVPF_ASSIGN : 0;
        return &shim_devproc;
    }

    shim_devproc_out--;
    return NULL;
}

static inline void FreeDeviceProc(struct DevProc *dp)
{
    if (dp != NULL)
        shim_devproc_out--;
}

/* Examine() starts the walk; ExNext() hands out the drawer's children. */
static inline LONG Examine(BPTR lock, struct FileInfoBlock *fib)
{
    fib->fib_DiskKey = 0;
    fib->fib_DirEntryType = shim_vol_dir[lock - 1] ? 2 : -3;
    snprintf(fib->fib_FileName, sizeof(fib->fib_FileName), "%s",
             shim_vol_obj[lock - 1]);
    return DOSTRUE;
}

static inline LONG ExNext(BPTR lock, struct FileInfoBlock *fib)
{
    const char *dir = shim_vol_obj[lock - 1];
    size_t      n   = strlen(dir);

    for (; fib->fib_DiskKey < shim_vol_count; fib->fib_DiskKey++)
    {
        const char *obj = shim_vol_obj[fib->fib_DiskKey];

        if (strncasecmp(obj, dir, n) == 0 && obj[n] == '/' &&
            strchr(obj + n + 1, '/') == NULL)
        {
            fib->fib_DirEntryType = shim_vol_dir[fib->fib_DiskKey] ? 2 : -3;
            snprintf(fib->fib_FileName, sizeof(fib->fib_FileName), "%s",
                     obj + n + 1);
            fib->fib_DiskKey++;
            return DOSTRUE;
        }
    }
    return DOSFALSE;
}

#endif

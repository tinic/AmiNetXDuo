/*
 * AmiNetXDuo host-test shim: the dos.library calls ug_db.c makes, over an
 * in-memory file table the test fills in.
 *
 * FOUR CALLS, and the Seek() contract is the subtle one.  ug_db.c sizes a
 * file with
 *
 *     Seek(fh, 0, OFFSET_END);
 *     size = Seek(fh, 0, OFFSET_BEGINNING);
 *
 * which works only because AmigaDOS Seek() returns the position it was at
 * BEFORE the seek, not the one it moved to.  A shim that returned the new
 * position would hand back 0 and the test would silently read nothing, so it
 * is reproduced exactly here.
 *
 * Open() also records whether pr_WindowPtr was -1 when it was called.  That
 * is not decoration: it is the only thing stopping a missing DEVS: from
 * putting a "please insert volume" requester on the caller's screen, and
 * nothing else in the tree checks it.
 *
 * The state lives in the test, through SHIM_DOS_DEFINE_STATE, because this
 * header is included by both the test and ug_db.c and file-scope statics
 * would give each translation unit its own copy.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_SHIM_PROTO_DOS_H
#define AMINETXDUO_SHIM_PROTO_DOS_H

#include <dos/dos.h>
#include <dos/dosextens.h>
#include <exec/types.h>

#include <string.h>

#define SHIM_DOS_MAX_FILES  8
#define SHIM_DOS_MAX_OPEN   4

struct ShimDosFile {
    const char *path;
    const char *data;
    long        len;
};

struct ShimDosHandle {
    int  used;
    int  file;
    long pos;
};

extern struct ShimDosFile   shim_dos_files[SHIM_DOS_MAX_FILES];
extern int                  shim_dos_file_count;
extern struct ShimDosHandle shim_dos_handles[SHIM_DOS_MAX_OPEN];

/* Observations a test can assert on. */
extern int shim_dos_opens;          /* Open() calls, hits and misses alike */
extern int shim_dos_closes;         /* every successful Open() must match  */
extern int shim_dos_open_unwindowed; /* Opens made with pr_WindowPtr == -1 */

#define SHIM_DOS_DEFINE_STATE                                                \
    struct ShimDosFile   shim_dos_files[SHIM_DOS_MAX_FILES];                 \
    int                  shim_dos_file_count;                                \
    struct ShimDosHandle shim_dos_handles[SHIM_DOS_MAX_OPEN];                \
    int                  shim_dos_opens;                                     \
    int                  shim_dos_closes;                                    \
    int                  shim_dos_open_unwindowed

/* Declared by the exec shim; Open() reads pr_WindowPtr through it. */
extern struct Task *shim_current_task;

static inline void shim_dos_reset(void)
{
    memset(shim_dos_files, 0, sizeof(shim_dos_files));
    memset(shim_dos_handles, 0, sizeof(shim_dos_handles));
    shim_dos_file_count      = 0;
    shim_dos_opens           = 0;
    shim_dos_closes          = 0;
    shim_dos_open_unwindowed = 0;
}

static inline void shim_dos_add_file(const char *path, const char *data, long len)
{
    if (shim_dos_file_count >= SHIM_DOS_MAX_FILES)
        return;

    shim_dos_files[shim_dos_file_count].path = path;
    shim_dos_files[shim_dos_file_count].data = data;
    shim_dos_files[shim_dos_file_count].len  = len;
    shim_dos_file_count++;
}

static inline BPTR Open(STRPTR path, LONG mode)
{
    struct Process *self = (struct Process *)shim_current_task;
    int i;

    (void)mode;
    shim_dos_opens++;

    if (self != NULL && self->pr_WindowPtr == (APTR)-1L)
        shim_dos_open_unwindowed++;

    for (i = 0; i < shim_dos_file_count; i++)
    {
        if (strcmp(shim_dos_files[i].path, (const char *)path) != 0)
            continue;

        {
            int h;

            for (h = 0; h < SHIM_DOS_MAX_OPEN; h++)
            {
                if (shim_dos_handles[h].used)
                    continue;

                shim_dos_handles[h].used = 1;
                shim_dos_handles[h].file = i;
                shim_dos_handles[h].pos  = 0;

                return (BPTR)(h + 1);       /* 0 is "could not open" */
            }
        }

        return (BPTR)0;                     /* out of handles */
    }

    return (BPTR)0;                         /* absent, the normal case */
}

static inline LONG Close(BPTR fh)
{
    int h = (int)fh - 1;

    if (h < 0 || h >= SHIM_DOS_MAX_OPEN || !shim_dos_handles[h].used)
        return 0;

    shim_dos_handles[h].used = 0;
    shim_dos_closes++;

    return -1;
}

/*
 * Returns the position BEFORE the seek.  See the note at the top: ug_db.c's
 * file sizing depends on it.
 */
static inline LONG Seek(BPTR fh, LONG position, LONG mode)
{
    int  h = (int)fh - 1;
    long was, len, want;

    if (h < 0 || h >= SHIM_DOS_MAX_OPEN || !shim_dos_handles[h].used)
        return -1;

    len = shim_dos_files[shim_dos_handles[h].file].len;
    was = shim_dos_handles[h].pos;

    if (mode == OFFSET_BEGINNING)
        want = position;
    else if (mode == OFFSET_END)
        want = len + position;
    else
        want = was + position;

    if (want < 0)
        want = 0;
    if (want > len)
        want = len;

    shim_dos_handles[h].pos = want;

    return (LONG)was;
}

static inline LONG Read(BPTR fh, APTR buffer, LONG length)
{
    int  h = (int)fh - 1;
    long len, avail;

    if (h < 0 || h >= SHIM_DOS_MAX_OPEN || !shim_dos_handles[h].used)
        return -1;

    if (length <= 0)
        return 0;

    len   = shim_dos_files[shim_dos_handles[h].file].len;
    avail = len - shim_dos_handles[h].pos;
    if (avail <= 0)
        return 0;
    if (length < avail)
        avail = length;

    memcpy(buffer, shim_dos_files[shim_dos_handles[h].file].data
                       + shim_dos_handles[h].pos,
           (size_t)avail);
    shim_dos_handles[h].pos += avail;

    return (LONG)avail;
}

#endif /* AMINETXDUO_SHIM_PROTO_DOS_H */

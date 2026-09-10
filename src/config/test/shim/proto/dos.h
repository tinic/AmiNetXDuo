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
#define SHIM_DOS_OPLOG      256
#define SHIM_DOS_WRITELOG   256

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

/*
 * An ORDER log, because for a console read the order is the property.  One
 * character per call: 'o' Open, 'c' Close, 'r' Read, 'w' Write, '1' SetMode
 * raw, '0' SetMode cooked.  A password prompt that turns echo off after the
 * first Read has already put a character on the screen, and a count of calls
 * cannot say so.
 */
extern char shim_dos_ops[SHIM_DOS_OPLOG];
extern int  shim_dos_oplen;

/* What Write() was handed, concatenated. */
extern char shim_dos_written[SHIM_DOS_WRITELOG];
extern int  shim_dos_writelen;

/* Set to make SetMode() fail, the case where raw mode is refused. */
extern int  shim_dos_setmode_fails;

#define SHIM_DOS_DEFINE_STATE                                                \
    struct ShimDosFile   shim_dos_files[SHIM_DOS_MAX_FILES];                 \
    int                  shim_dos_file_count;                                \
    struct ShimDosHandle shim_dos_handles[SHIM_DOS_MAX_OPEN];                \
    int                  shim_dos_opens;                                     \
    int                  shim_dos_closes;                                    \
    int                  shim_dos_open_unwindowed;                           \
    char                 shim_dos_ops[SHIM_DOS_OPLOG];                       \
    int                  shim_dos_oplen;                                     \
    char                 shim_dos_written[SHIM_DOS_WRITELOG];                \
    int                  shim_dos_writelen;                                  \
    int                  shim_dos_setmode_fails

/* Declared by the exec shim; Open() reads pr_WindowPtr through it. */
extern struct Task *shim_current_task;

static inline void shim_dos_op(char c)
{
    if (shim_dos_oplen < SHIM_DOS_OPLOG - 1)
        shim_dos_ops[shim_dos_oplen++] = c;
    shim_dos_ops[shim_dos_oplen] = '\0';
}

static inline void shim_dos_reset(void)
{
    memset(shim_dos_files, 0, sizeof(shim_dos_files));
    memset(shim_dos_handles, 0, sizeof(shim_dos_handles));
    memset(shim_dos_ops, 0, sizeof(shim_dos_ops));
    memset(shim_dos_written, 0, sizeof(shim_dos_written));
    shim_dos_file_count      = 0;
    shim_dos_opens           = 0;
    shim_dos_closes          = 0;
    shim_dos_open_unwindowed = 0;
    shim_dos_oplen           = 0;
    shim_dos_writelen        = 0;
    shim_dos_setmode_fails   = 0;
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
    shim_dos_op('o');

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
    shim_dos_op('c');

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

    shim_dos_op('r');
    memcpy(buffer, shim_dos_files[shim_dos_handles[h].file].data
                       + shim_dos_handles[h].pos,
           (size_t)avail);
    shim_dos_handles[h].pos += avail;

    return (LONG)avail;
}

static inline LONG Write(BPTR fh, APTR buffer, LONG length)
{
    int h = (int)fh - 1;
    int i;

    if (h < 0 || h >= SHIM_DOS_MAX_OPEN || !shim_dos_handles[h].used)
        return -1;
    if (length <= 0)
        return 0;

    shim_dos_op('w');

    for (i = 0; i < length && shim_dos_writelen < SHIM_DOS_WRITELOG - 1; i++)
        shim_dos_written[shim_dos_writelen++] = ((const char *)buffer)[i];
    shim_dos_written[shim_dos_writelen] = '\0';

    return length;
}

/* mode != 0 is raw: one character at a time, and the console stops echoing. */
static inline LONG SetMode(BPTR fh, LONG mode)
{
    int h = (int)fh - 1;

    if (h < 0 || h >= SHIM_DOS_MAX_OPEN || !shim_dos_handles[h].used)
        return DOSFALSE;

    if (shim_dos_setmode_fails)
        return DOSFALSE;

    shim_dos_op(mode != 0 ? '1' : '0');

    return DOSTRUE;
}

#endif /* AMINETXDUO_SHIM_PROTO_DOS_H */

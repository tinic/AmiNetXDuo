/*
 * Host test shim for config_file.c: the base dos/dos.h plus the names its
 * Lock()/Examine() and multi-assign walk need, at the NDK's values.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_TEST_FILESHIM_DOS_DOS_H
#define AMINETXDUO_TEST_FILESHIM_DOS_DOS_H

#include_next <dos/dos.h>

#define SHARED_LOCK     (-2)
#define ACCESS_READ     SHARED_LOCK

#define LOCK_SAME       0
#define LOCK_DIFFERENT  (-1)

struct FileInfoBlock {
    LONG  fib_DiskKey;
    LONG  fib_DirEntryType;     /* > 0 a drawer, < 0 a file */
    char  fib_FileName[108];
};

#endif

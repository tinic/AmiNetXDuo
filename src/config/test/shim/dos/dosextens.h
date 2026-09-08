/*
 * Host test shim, struct Process.
 *
 * ug_db.c needs exactly two things from it: the embedded Task, so it can
 * check ln_Type is NT_PROCESS before calling Open(), and pr_WindowPtr, which
 * it sets to -1 across the Open() so a missing DEVS: raises no requester on
 * the caller's screen.  Nothing else here is modelled.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_TEST_DOS_DOSEXTENS_H
#define AMINETXDUO_TEST_DOS_DOSEXTENS_H

#include <dos/dos.h>
#include <exec/tasks.h>
#include <exec/types.h>

struct Process {
    struct Task pr_Task;
    APTR        pr_WindowPtr;
};

#endif

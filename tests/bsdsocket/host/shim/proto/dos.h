/* <proto/dos.h> for the bsdsocket host tests: declarations only, same rule
   as proto/exec.h.  SPDX-License-Identifier: MIT */
#ifndef AMINETXDUO_BSD_TEST_PROTO_DOS_H
#define AMINETXDUO_BSD_TEST_PROTO_DOS_H
#include <exec/types.h>
#include <utility/tagitem.h>
#include <dos/dos.h>
#include <dos/dosextens.h>

LONG  IoErr(VOID);
VOID  SetIoErr(LONG result);
BPTR  Open(const char *name, LONG accessMode);
LONG  Close(BPTR file);
LONG  Read(BPTR file, APTR buffer, LONG length);
LONG  Write(BPTR file, const APTR buffer, LONG length);
BPTR  Input(VOID);
BPTR  Output(VOID);
LONG  Delay(ULONG ticks);
struct Process *FindTaskProcess(VOID);
struct Process *CreateNewProc(const struct TagItem *tags);

/* AmiTCP: -> SYS:, library_runtime.c's assign. */
BPTR  Lock(const UBYTE *name, LONG type);
VOID  UnLock(BPTR lock);
LONG  AssignLock(const UBYTE *name, BPTR lock);

/* The DOS device list, for the TCP: handler.  LockDosList() returns the list
   header, which is what FindDosEntry() walks. */
struct DosList *LockDosList(ULONG flags);
VOID  UnLockDosList(ULONG flags);
struct DosList *FindDosEntry(const struct DosList *dlist, const UBYTE *name,
                             ULONG flags);
struct DosList *MakeDosEntry(const UBYTE *name, LONG type);
VOID  FreeDosEntry(struct DosList *dlist);
LONG  AddDosEntry(struct DosList *dlist);
LONG  RemDosEntry(struct DosList *dlist);
#endif

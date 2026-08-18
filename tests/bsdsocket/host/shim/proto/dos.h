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
#endif

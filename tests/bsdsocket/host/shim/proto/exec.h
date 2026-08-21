/*
 * <proto/exec.h> for the bsdsocket host tests: declarations only.
 *
 * A test that reaches one of these provides it.  Nothing is defined here,
 * deliberately: a default implementation would let a test pass while calling
 * something it never meant to, and on a machine with no memory protection the
 * interesting failures are the ones where a call happened at all.
 *
 * The set is what src/bsdsocket actually calls, enumerated from the sources
 * rather than copied from the NDK, so a new call site fails to link here and
 * has to be considered.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef AMINETXDUO_BSD_TEST_PROTO_EXEC_H
#define AMINETXDUO_BSD_TEST_PROTO_EXEC_H

#include <exec/types.h>
#include <exec/lists.h>
#include <exec/libraries.h>
#include <exec/tasks.h>
#include <exec/ports.h>
#include <exec/semaphores.h>
#include <exec/io.h>

APTR  AllocMem(ULONG byteSize, ULONG requirements);
VOID  FreeMem(APTR memoryBlock, ULONG byteSize);
APTR  AllocVec(ULONG byteSize, ULONG requirements);
VOID  FreeVec(APTR memoryBlock);
VOID  CopyMem(const APTR source, APTR dest, ULONG size);

VOID  Forbid(VOID);
VOID  Permit(VOID);
VOID  Disable(VOID);
VOID  Enable(VOID);

VOID  InitSemaphore(struct SignalSemaphore *sigSem);
VOID  ObtainSemaphore(struct SignalSemaphore *sigSem);
VOID  ReleaseSemaphore(struct SignalSemaphore *sigSem);
ULONG AttemptSemaphore(struct SignalSemaphore *sigSem);

struct Task *FindTask(const char *name);
VOID  Signal(struct Task *task, ULONG signalSet);
ULONG Wait(ULONG signalSet);
ULONG SetSignal(ULONG newSignals, ULONG signalSet);
BYTE  AllocSignal(LONG signalNum);
VOID  FreeSignal(LONG signalNum);

struct MsgPort *CreateMsgPort(VOID);
VOID  DeleteMsgPort(struct MsgPort *port);
VOID  PutMsg(struct MsgPort *port, struct Message *message);
struct Message *GetMsg(struct MsgPort *port);
struct MsgPort *WaitPort(struct MsgPort *port);
VOID  ReplyMsg(struct Message *message);

/* CONST_STRPTR, as clib/exec_protos.h:115 spells it, and not `const char *`:
   the callers cast to STRPTR, which is unsigned, and the two differ under
   -Werror=pointer-sign. */
BYTE  OpenDevice(const UBYTE *devName, ULONG unit,
                 struct IORequest *ioRequest, ULONG flags);
VOID  CloseDevice(struct IORequest *ioRequest);
VOID  SendIO(struct IORequest *ioRequest);
LONG  AbortIO(struct IORequest *ioRequest);
LONG  WaitIO(struct IORequest *ioRequest);
struct IORequest *CheckIO(struct IORequest *ioRequest);
BYTE  DoIO(struct IORequest *ioRequest);

struct Library *OpenLibrary(const char *libName, ULONG version);
VOID  CloseLibrary(struct Library *library);

VOID  NewList(struct List *list);
VOID  AddTail(struct List *list, struct Node *node);
VOID  AddHead(struct List *list, struct Node *node);
VOID  Remove(struct Node *node);
struct Node *RemHead(struct List *list);
struct Node *FindName(struct List *list, const char *name);

VOID  CacheClearU(VOID);
VOID  SetTaskPri(struct Task *task, LONG priority);

#endif

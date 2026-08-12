/* <proto/exec.h> for the sana2 host tests: the calls src/sana2/sana2_tx.c and
   src/sana2/sana2_rx.c make, and nothing else.  The test binary defines them.

   The second block is the receive path's.  Nothing on the host can honour
   Wait() or a real reply port, and the tests do not try to: what they reach is
   ami_sana2_rx_deliver(), which takes a packet and no Exec at all.  These are
   declared because the rest of the translation unit has to compile, not
   because anything calls them.
   SPDX-License-Identifier: MIT */

#ifndef AMINETXDUO_SANA2_TEST_PROTO_EXEC_H
#define AMINETXDUO_SANA2_TEST_PROTO_EXEC_H

#include <exec/types.h>
#include <exec/lists.h>
#include <exec/ports.h>
#include <exec/io.h>

/* exec/nodes.h's node types; NT_MESSAGE is the only one this path sets. */
#ifndef NT_MESSAGE
#define NT_MESSAGE  5
#endif

VOID            Disable(VOID);
VOID            Enable(VOID);
VOID            Forbid(VOID);
VOID            Permit(VOID);
struct Message *GetMsg(struct MsgPort *port);
VOID            ReplyMsg(struct Message *msg);
VOID            SendIO(struct IORequest *req);
LONG            AbortIO(struct IORequest *req);

VOID            NewList(struct List *list);
VOID            AddTail(struct List *list, struct Node *node);
struct Node    *RemHead(struct List *list);

/* ---- the receive path's, sana2_rx.c ---- */

struct MsgPort *CreateMsgPort(VOID);
VOID            DeleteMsgPort(struct MsgPort *port);
LONG            DoIO(struct IORequest *req);
struct Task    *FindTask(STRPTR name);
BYTE            AllocSignal(LONG num);
VOID            FreeSignal(LONG num);
ULONG           Wait(ULONG mask);
VOID            Signal(struct Task *task, ULONG mask);
VOID            CloseDevice(struct IORequest *req);

#endif

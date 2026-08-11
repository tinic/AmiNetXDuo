/* <proto/exec.h> for the sana2 host tests: the calls src/sana2/sana2_tx.c
   makes, and nothing else.  The test binary defines them.
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

#endif

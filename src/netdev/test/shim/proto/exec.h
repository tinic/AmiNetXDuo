/* <proto/exec.h> for the netdev host test: the calls netdev_event.c makes,
   and nothing else.  The test binary defines them, and what it does with
   Disable()/Enable() is half the point -- an event posted from an interrupt
   with the list unprotected is the failure this test exists to catch.
   SPDX-License-Identifier: MIT */

#ifndef AMINETXDUO_NETDEV_TEST_PROTO_EXEC_H
#define AMINETXDUO_NETDEV_TEST_PROTO_EXEC_H

#include <exec/types.h>
#include <exec/lists.h>
#include <exec/nodes.h>
#include <exec/ports.h>

#ifndef NT_MESSAGE
#define NT_MESSAGE  5
#endif
#ifndef NT_REPLYMSG
#define NT_REPLYMSG 7
#endif

VOID         Disable(VOID);
VOID         Enable(VOID);
VOID         ReplyMsg(struct Message *msg);

VOID         NewList(struct List *list);
VOID         AddHead(struct List *list, struct Node *node);
VOID         AddTail(struct List *list, struct Node *node);
VOID         Remove(struct Node *node);
struct Node *RemHead(struct List *list);

#endif

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
#include <exec/semaphores.h>

#ifndef NT_MESSAGE
#define NT_MESSAGE  5
#endif
#ifndef NT_REPLYMSG
#define NT_REPLYMSG 7
#endif

VOID         Disable(VOID);
VOID         Enable(VOID);
VOID         ReplyMsg(struct Message *msg);

/*
 * The scheduler lock and the public semaphore list, for
 * src/netdev/test/test_netdev_diag.c.  Declared and not defined, like
 * everything else here: the record's whole publish contract is that a SECOND
 * driver finds the first one's semaphore and leaves it alone, so the test
 * needs a list it can inspect rather than a no-op.
 */
VOID         Forbid(VOID);
VOID         Permit(VOID);
VOID         InitSemaphore(struct SignalSemaphore *sem);
VOID         AddSemaphore(struct SignalSemaphore *sem);
VOID         RemSemaphore(struct SignalSemaphore *sem);
struct SignalSemaphore *FindSemaphore(STRPTR name);

VOID         NewList(struct List *list);
VOID         AddHead(struct List *list, struct Node *node);
VOID         AddTail(struct List *list, struct Node *node);
VOID         Remove(struct Node *node);
struct Node *RemHead(struct List *list);

#endif

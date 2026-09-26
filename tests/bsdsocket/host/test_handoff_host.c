/*
 * handoff.c on the host: listeners are ordinary releasable sockets, as they
 * are in AmiTCP and Roadshow.
 *
 * SPDX-License-Identifier: MIT
 */

#include "bsdsocket_vectors.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned long checks;
static unsigned long failures;

#define CHECK(c, what)                                                        \
    do {                                                                      \
        checks++;                                                             \
        if (!(c)) {                                                           \
            failures++;                                                       \
            printf("  FAIL %s\n", (what));                                  \
        }                                                                     \
    } while (0)

static AmiSocket *tables[3][4];
static struct AmiSocketBase master_base;
static struct AmiSocketBase source_base;
static struct AmiSocketBase target_base;

VOID AddTail(struct List *list, struct Node *node)
{
    node->ln_Succ = (struct Node *)&list->lh_Tail;
    node->ln_Pred = list->lh_TailPred;
    list->lh_TailPred->ln_Succ = node;
    list->lh_TailPred = node;
}

VOID Remove(struct Node *node)
{
    node->ln_Pred->ln_Succ = node->ln_Succ;
    node->ln_Succ->ln_Pred = node->ln_Pred;
}

struct Node *RemHead(struct List *list)
{
    struct Node *node = list->lh_Head;

    if (node->ln_Succ == NULL)
        return NULL;
    Remove(node);
    return node;
}

VOID ObtainSemaphore(struct SignalSemaphore *sem) { (VOID)sem; }
VOID ReleaseSemaphore(struct SignalSemaphore *sem) { (VOID)sem; }
VOID Forbid(VOID) { }
VOID Permit(VOID) { }

APTR ami_alloc(ULONG size) { return calloc(1, (size_t)size); }
VOID ami_free(APTR p) { free(p); }

LONG bsd_fail(struct AmiSocketBase *base, LONG error)
{
    base->sb_Errno = error;
    return -1;
}

AmiSocket *bsd_lookup(struct AmiSocketBase *base, LONG fd)
{
    if (fd < 0 || fd >= base->sb_TableSize)
        return NULL;
    return base->sb_Table[fd];
}

LONG bsd_fd_alloc(struct AmiSocketBase *base, AmiSocket *sock)
{
    LONG fd;

    for (fd = 0; fd < base->sb_TableSize; fd++)
    {
        if (base->sb_Table[fd] == NULL)
        {
            base->sb_Table[fd] = sock;
            return fd;
        }
    }
    return bsd_fail(base, AMI_EMFILE);
}

LONG bsd_fd_free(struct AmiSocketBase *base, LONG fd)
{
    if (fd < 0 || fd >= base->sb_TableSize)
        return bsd_fail(base, AMI_EBADF);
    base->sb_Table[fd] = NULL;
    return 0;
}

VOID bsd_socket_retain(AmiSocket *sock) { sock->as_RefCount++; }
VOID bsd_socket_release(struct AmiSocketBase *base, AmiSocket *sock)
{
    (VOID)base;
    if (sock->as_RefCount != 0)
        sock->as_RefCount--;
}

LONG bsd_nx_enter(struct AmiSocketBase *base) { (VOID)base; return 0; }
VOID bsd_nx_leave(struct AmiSocketBase *base) { (VOID)base; }

static VOID reset_fixture(AmiSocket *sock)
{
    memset(tables, 0, sizeof(tables));
    memset(&master_base, 0, sizeof(master_base));
    memset(&source_base, 0, sizeof(source_base));
    memset(&target_base, 0, sizeof(target_base));
    memset(sock, 0, sizeof(*sock));

    master_base.sb_Table = tables[0];
    source_base.sb_Table = tables[1];
    target_base.sb_Table = tables[2];
    master_base.sb_TableSize = source_base.sb_TableSize =
        target_base.sb_TableSize = 4;
    source_base.sb_Master = target_base.sb_Master = &master_base;
    bsd_handoff_init(&master_base);

    sock->as_Flags = ASF_TCP | ASF_LISTENING;
    sock->as_Domain = AF_INET;
    sock->as_Type = SOCK_STREAM;
    sock->as_Protocol = IPPROTO_TCP;
    sock->as_RefCount = 1;
    sock->as_Owner = &source_base;
    source_base.sb_Table[0] = sock;
}

static VOID test_release_listener(VOID)
{
    AmiSocket sock;
    LONG id;
    LONG fd;

    reset_fixture(&sock);
    id = bsd_ReleaseSocket(0, UNIQUE_ID, &source_base);
    CHECK(id > 65535, "ReleaseSocket accepts a listening descriptor");
    CHECK(source_base.sb_Table[0] == NULL,
          "and detaches it from the releasing descriptor table");
    CHECK(sock.as_Owner == NULL, "the parked listener has no stale owner");

    fd = bsd_ObtainSocket(id, AF_INET, SOCK_STREAM, 0, &target_base);
    CHECK(fd == 0, "the listener can be obtained by another base");
    CHECK(target_base.sb_Table[0] == &sock,
          "the obtained descriptor names the same listener");
    CHECK(sock.as_Owner == &target_base,
          "future accept events signal the obtaining base");
    CHECK((sock.as_Flags & ASF_LISTENING) != 0,
          "the listen state survives the handoff");
}

static VOID test_release_copy_listener(VOID)
{
    AmiSocket sock;
    LONG id;
    LONG fd;

    reset_fixture(&sock);
    id = bsd_ReleaseCopyOfSocket(0, UNIQUE_ID, &source_base);
    CHECK(id > 65535, "ReleaseCopyOfSocket accepts a listening descriptor");
    CHECK(source_base.sb_Table[0] == &sock,
          "and leaves the original descriptor installed");
    CHECK(sock.as_RefCount == 2, "the parked copy owns a second reference");

    fd = bsd_ObtainSocket(id, AF_INET, SOCK_STREAM, IPPROTO_TCP, &target_base);
    CHECK(fd == 0 && target_base.sb_Table[0] == &sock,
          "the copied listener can be obtained");
    CHECK((sock.as_Flags & ASF_LISTENING) != 0,
          "and remains a listener after the copy handoff");
}

/* The last opener's close: bsd_handoff_take() moves the registry off the
   master under sb_Lock, bsd_handoff_flush() releases it afterwards. */
static VOID test_take_and_flush(VOID)
{
    AmiSocket      sock;
    struct MinList moved;
    struct MinNode *first;
    struct MinNode *second;

    reset_fixture(&sock);
    CHECK(bsd_ReleaseCopyOfSocket(0, UNIQUE_ID, &source_base) > 65535 &&
              bsd_ReleaseCopyOfSocket(0, UNIQUE_ID, &source_base) > 65535,
          "two entries are parked");
    CHECK(sock.as_RefCount == 3, "each holding a reference");

    bsd_handoff_take(&master_base, &moved);
    CHECK(!bsd_handoff_pending(&master_base), "the master registry is empty");
    CHECK(master_base.sb_Handoffs.mlh_Head ==
                  (struct MinNode *)&master_base.sb_Handoffs.mlh_Tail &&
              master_base.sb_Handoffs.mlh_TailPred ==
                  (struct MinNode *)&master_base.sb_Handoffs.mlh_Head,
          "and a well-formed empty list");

    first  = moved.mlh_Head;
    second = first->mln_Succ;
    CHECK(first->mln_Pred == (struct MinNode *)&moved.mlh_Head,
          "the first moved entry points back at the new list head");
    CHECK(second->mln_Pred == first && moved.mlh_TailPred == second,
          "the second is the tail");
    CHECK(second->mln_Succ == (struct MinNode *)&moved.mlh_Tail &&
              moved.mlh_Tail == NULL,
          "and ends at the new list's tail");

    bsd_handoff_flush(&master_base, &moved, TRUE);
    CHECK(sock.as_RefCount == 1, "the flush released both references");
    CHECK(moved.mlh_Head == (struct MinNode *)&moved.mlh_Tail,
          "and emptied the moved list");
    CHECK(source_base.sb_Table[0] == &sock, "the original stays installed");
}

int main(void)
{
    test_release_listener();
    test_release_copy_listener();
    test_take_and_flush();
    printf("handoff: %lu checks, %lu failures\n", checks, failures);
    return failures ? 1 : 0;
}

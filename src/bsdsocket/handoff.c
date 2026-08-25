/*
 * bsdsocket.library, handing a socket from one task to another.
 * SPDX-License-Identifier: MIT
 */

#include "bsdsocket_vectors.h"

#include <proto/exec.h>

typedef struct BsdHandoff
{
    struct MinNode  bh_Node;
    LONG            bh_Id;
    AmiSocket      *bh_Socket;
    BOOL            bh_Claimed;
} BsdHandoff;

/* Open-coded NewList(). amiga.lib is not available to a shared library. */
VOID bsd_handoff_init(struct AmiSocketBase *master)
{
    master->sb_Handoffs.mlh_Head =
        (struct MinNode *)&master->sb_Handoffs.mlh_Tail;
    master->sb_Handoffs.mlh_Tail     = NULL;
    master->sb_Handoffs.mlh_TailPred =
        (struct MinNode *)&master->sb_Handoffs.mlh_Head;

    /*
     * UNIQUE_ID (-1) asks us to invent an id, so generated ones start clear of
     * it and of the small integers applications hand-pick.
     */
    master->sb_NextHandoffId = 0x10000;
}

static struct AmiSocketBase *bsd_master_of(struct AmiSocketBase *base)
{
    return (base->sb_Master != NULL) ? base->sb_Master : base;
}

/*
 * The id range carries meaning. ReleaseSocket's autodoc: "If the Id value is
 * between 0 and 65535 (inclusively), then the id is considered non-unique and
 * anyone can pick it up via ObtainSocket() by specifying the right combination
 * of socket type and protocol. If the Id value is greater than 65535 then it
 * must be unique number (this function will fail if it is not)."
 */
#define BSD_ID_NONUNIQUE_MAX  65535L

static BOOL bsd_handoff_id_is_unique(LONG id)
{
    return (id < 0 || id > BSD_ID_NONUNIQUE_MAX) ? TRUE : FALSE;
}

/*
 * Caller holds the master's semaphore.
 */
static BsdHandoff *bsd_handoff_match(struct AmiSocketBase *master, LONG id,
                                     LONG domain, LONG type, LONG protocol)
{
    struct MinNode *node;

    for (node = master->sb_Handoffs.mlh_Head;
         node->mln_Succ != NULL;
         node = node->mln_Succ)
    {
        BsdHandoff *entry = (BsdHandoff *)node;
        AmiSocket  *sock  = entry->bh_Socket;

        if (entry->bh_Claimed)
            continue;

        if (entry->bh_Id != id)
            continue;

        if (sock == NULL)
            continue;

        if ((LONG)sock->as_Domain != domain || (LONG)sock->as_Type != type)
            continue;

        /* Protocol 0 means "the default for this type", which is what the
           socket was created with, so it matches whatever is parked. */
        if (protocol != 0 && sock->as_Protocol != protocol)
            continue;

        return entry;
    }

    return NULL;
}

static BsdHandoff *bsd_handoff_find(struct AmiSocketBase *master, LONG id)
{
    struct MinNode *node;

    for (node = master->sb_Handoffs.mlh_Head;
         node->mln_Succ != NULL;
         node = node->mln_Succ)
    {
        BsdHandoff *entry = (BsdHandoff *)node;

        if (entry->bh_Id == id)
            return entry;
    }

    return NULL;
}

/* Caller holds the master's semaphore. */
static LONG bsd_handoff_new_id(struct AmiSocketBase *master)
{
    LONG id;

    do
    {
        id = master->sb_NextHandoffId++;

        if (master->sb_NextHandoffId <= 0)       /* wrapped past LONG_MAX */
            master->sb_NextHandoffId = 0x10000;
    }
    while (id == UNIQUE_ID || bsd_handoff_find(master, id) != NULL);

    return id;
}

/*
 * Park `sock` under `id`. Returns the id, or -1 with errno set.
 * `sock` must already carry the reference the registry is taking over.
 */
static LONG bsd_handoff_park(struct AmiSocketBase *base, AmiSocket *sock,
                             LONG id, LONG detach_fd)
{
    struct AmiSocketBase *master = bsd_master_of(base);
    BsdHandoff           *entry;

    entry = (BsdHandoff *)ami_alloc(sizeof(BsdHandoff));
    if (entry == NULL)
        return bsd_fail(base, AMI_ENOMEM);

    ObtainSemaphore(&master->sb_Lock);

    if (id == UNIQUE_ID)
    {
        id = bsd_handoff_new_id(master);
    }
    else if (bsd_handoff_id_is_unique(id) && bsd_handoff_find(master, id) != NULL)
    {
        ReleaseSemaphore(&master->sb_Lock);
        ami_free(entry);
        return bsd_fail(base, AMI_EINVAL);
    }

    entry->bh_Id      = id;
    entry->bh_Socket  = sock;
    entry->bh_Claimed = (detach_fd >= 0);

    AddTail((struct List *)&master->sb_Handoffs, (struct Node *)&entry->bh_Node);

    ReleaseSemaphore(&master->sb_Lock);

    /* Reserve the registry entry before asking caller code to free the fd.
       Claimed entries cannot be obtained, so uniqueness stays transactional
       without invoking an arbitrary callback under the registry lock. */
    if (detach_fd >= 0)
    {
        if (bsd_fd_free(base, detach_fd) != 0)
        {
            ObtainSemaphore(&master->sb_Lock);
            Remove((struct Node *)&entry->bh_Node);
            ReleaseSemaphore(&master->sb_Lock);
            ami_free(entry);
            return -1;
        }

        ObtainSemaphore(&master->sb_Lock);
        entry->bh_Claimed = FALSE;
        sock->as_Owner = NULL;
        ReleaseSemaphore(&master->sb_Lock);
    }

    /*
     * A fully released socket belongs to nobody until it is obtained. An
     * as_Owner left at the releasing base lets a receive callback Signal() a
     */
    return id;
}

/*
 * Release everything still parked. Called from bsd_lib_close() when the last
 * opener goes away. At that point no base exists that can obtain them, and the
 */
VOID bsd_handoff_flush(struct AmiSocketBase *base)
{
    struct AmiSocketBase *master = bsd_master_of(base);
    BOOL                  bracketed;

    if (master->sb_Handoffs.mlh_Head == NULL ||
        master->sb_Handoffs.mlh_Head->mln_Succ == NULL)
        return;                                 /* empty, nothing to do */

    bracketed = (bsd_nx_enter(base) == 0);

    for (;;)
    {
        BsdHandoff *entry =
            (BsdHandoff *)RemHead((struct List *)&master->sb_Handoffs);

        if (entry == NULL)
            break;

        if (bracketed)
        {
            bsd_socket_release(base, entry->bh_Socket);
        }
        else
        {
            AMI_WARN("bsdsocket: released socket id %ld abandoned. "
                     "The kernel is already down", (long)entry->bh_Id);
        }

        ami_free(entry);
    }

    if (bracketed)
        bsd_nx_leave(base);
}

LONG bsd_ReleaseSocket(register LONG sock_fd __asm("d0"),
                       register LONG id      __asm("d1"),
                       register struct AmiSocketBase *SocketBase __asm("a6"))
{
    AmiSocket *sock = bsd_lookup(SocketBase, sock_fd);
    LONG       result;

    if (sock == NULL)
        return bsd_fail(SocketBase, AMI_EBADF);

    /*
     * A listening descriptor owns a socket parked on the port and a listen
     * request registered against this NX_IP. Neither survives a handover to
     * another base, so a release of one is refused.
     */
    if ((sock->as_Flags & ASF_LISTENING) != 0)
        return bsd_fail(SocketBase, AMI_EOPNOTSUPP);

    result = bsd_handoff_park(SocketBase, sock, id, sock_fd);
    if (result < 0)
        return result;

    return result;
}

LONG bsd_ReleaseCopyOfSocket(register LONG sock_fd __asm("d0"),
                             register LONG id      __asm("d1"),
                             register struct AmiSocketBase *SocketBase __asm("a6"))
{
    AmiSocket *sock = bsd_lookup(SocketBase, sock_fd);
    LONG       result;

    if (sock == NULL)
        return bsd_fail(SocketBase, AMI_EBADF);

    if ((sock->as_Flags & ASF_LISTENING) != 0)
        return bsd_fail(SocketBase, AMI_EOPNOTSUPP);

    bsd_socket_retain(sock);

    result = bsd_handoff_park(SocketBase, sock, id, -1);
    if (result < 0)
    {
        /* Undo the reference reserved above. The caller's descriptor still
           owns the other one, so this cannot destroy the socket. */
        Forbid();
        sock->as_RefCount--;
        Permit();
        return result;
    }

    return result;
}

LONG bsd_ObtainSocket(register LONG id       __asm("d0"),
                      register LONG domain   __asm("d1"),
                      register LONG type     __asm("d2"),
                      register LONG protocol __asm("d3"),
                      register struct AmiSocketBase *SocketBase __asm("a6"))
{
    struct AmiSocketBase *master = bsd_master_of(SocketBase);
    BsdHandoff           *entry;
    AmiSocket            *sock;
    LONG                  fd;

    ObtainSemaphore(&master->sb_Lock);

    entry = bsd_handoff_match(master, id, domain, type, protocol);
    if (entry == NULL)
    {
        ReleaseSemaphore(&master->sb_Lock);
        return bsd_fail(SocketBase, AMI_EBADF);
    }

    sock = entry->bh_Socket;

    /* Keep the node in the registry until descriptor allocation and its
       callback succeed. A claimed node is skipped by another ObtainSocket(),
       but still makes a unique id unavailable to ReleaseSocket(). */
    entry->bh_Claimed = TRUE;

    ReleaseSemaphore(&master->sb_Lock);

    fd = bsd_fd_alloc(SocketBase, sock);
    if (fd < 0)
    {
        ObtainSemaphore(&master->sb_Lock);
        entry->bh_Claimed = FALSE;
        ReleaseSemaphore(&master->sb_Lock);
        return -1;
    }

    ObtainSemaphore(&master->sb_Lock);
    Remove((struct Node *)&entry->bh_Node);
    ReleaseSemaphore(&master->sb_Lock);

    ami_free(entry);

    /*
     * The socket is ours now: events go to this task. A socket obtained from
     * a ReleaseCopyOfSocket() therefore no longer signals the base that still
     */
    sock->as_Owner = SocketBase;

    return fd;
}

/*
 * ObtainServerSocket() and ProcessIsServer() answer "was this Process launched
 * by the stack's inetd, and if so what socket was it given?".
 */
LONG bsd_ObtainServerSocket(register struct AmiSocketBase *SocketBase __asm("a6"))
{
    return bsd_fail(SocketBase, AMI_ENOENT);
}

BOOL bsd_ProcessIsServer(register struct Process *pr __asm("a0"),
                         register struct AmiSocketBase *SocketBase __asm("a6"))
{
    (VOID)pr;
    (VOID)SocketBase;

    return FALSE;
}

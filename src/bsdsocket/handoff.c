/*
 * bsdsocket.library -- handing a socket from one task to another.
 *
 *   ReleaseSocket()        give a socket up; it is parked under an id
 *   ReleaseCopyOfSocket()  park a second reference and keep using the first
 *   ObtainSocket()         take a parked socket into this base's table
 *   ObtainServerSocket()   take the socket an inetd launched us with
 *   ProcessIsServer()      was this Process launched by an inetd?
 *
 * This is the Amiga-specific half of descriptor management and it has no
 * BSD counterpart. There are no file descriptors and no descriptor passing
 * over a socket (docs/RESEARCH.md S3.1): a SocketBase belongs to exactly one
 * task, its descriptor table is private, and the only way a socket crosses
 * between tasks is through the registry here. It is what makes an
 * inetd-style server possible -- the daemon accepts, releases, launches a
 * child and tells it the id; the child opens its own bsdsocket.library and
 * obtains it.
 *
 * WHERE THE REGISTRY LIVES
 *
 * In the MASTER base, guarded by its semaphore, because that is the one
 * object both tasks can see. A parked socket belongs to no base at all --
 * as_Owner is cleared, so the NetX Duo receive/disconnect callbacks find no
 * task to signal rather than signalling one that has closed the library. It
 * is restored on ObtainSocket().
 *
 * REFERENCE COUNTING
 *
 * AmiSocket already carries as_RefCount for Dup2Socket(). A release moves the
 * existing reference into the registry; a *copy* takes an extra one, so the
 * original descriptor stays fully usable and the socket survives whichever
 * of the two goes away first. Anything still parked when the last opener
 * closes the library is released there rather than leaked -- see
 * bsd_handoff_flush(), called from bsd_lib_close().
 *
 * SPDX-License-Identifier: MIT
 */

#include "bsdsocket_vectors.h"

#include <proto/exec.h>

typedef struct BsdHandoff
{
    struct MinNode  bh_Node;
    LONG            bh_Id;
    AmiSocket      *bh_Socket;
} BsdHandoff;

/* Open-coded NewList(); amiga.lib is not available to a shared library. */
VOID bsd_handoff_init(struct AmiSocketBase *master)
{
    master->sb_Handoffs.mlh_Head =
        (struct MinNode *)&master->sb_Handoffs.mlh_Tail;
    master->sb_Handoffs.mlh_Tail     = NULL;
    master->sb_Handoffs.mlh_TailPred =
        (struct MinNode *)&master->sb_Handoffs.mlh_Head;

    /*
     * UNIQUE_ID (-1) asks us to invent an id, so the generated ones start
     * clear of it and of the small integers applications hand-pick.
     */
    master->sb_NextHandoffId = 0x10000;
}

static struct AmiSocketBase *bsd_master_of(struct AmiSocketBase *base)
{
    return (base->sb_Master != NULL) ? base->sb_Master : base;
}

/* Caller holds the master's semaphore. */
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
                             LONG id, BOOL detach)
{
    struct AmiSocketBase *master = bsd_master_of(base);
    BsdHandoff           *entry;

    entry = (BsdHandoff *)ami_alloc(sizeof(BsdHandoff));
    if (entry == NULL)
        return bsd_fail(base, AMI_ENOBUFS);

    ObtainSemaphore(&master->sb_Lock);

    if (id == UNIQUE_ID)
    {
        id = bsd_handoff_new_id(master);
    }
    else if (bsd_handoff_find(master, id) != NULL)
    {
        ReleaseSemaphore(&master->sb_Lock);
        ami_free(entry);
        return bsd_fail(base, AMI_EEXIST);
    }

    entry->bh_Id     = id;
    entry->bh_Socket = sock;

    AddTail((struct List *)&master->sb_Handoffs, (struct Node *)&entry->bh_Node);

    /*
     * A fully released socket belongs to nobody until it is obtained. Leaving
     * as_Owner pointing at the releasing base would let a receive callback
     * Signal() a task that has since closed the library.
     *
     * A *copy* leaves as_Owner alone: the original descriptor is still live
     * in the releasing base and is still the one that should be woken.
     */
    if (detach)
        sock->as_Owner = NULL;

    ReleaseSemaphore(&master->sb_Lock);

    return id;
}

/*
 * Release everything still parked. Called from bsd_lib_close() when the last
 * opener goes away: at that point no base exists that could obtain them, so
 * the choice is "release now" or "leak until reboot".
 *
 * `base` is only used for the ThreadX bracket the teardown needs.
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
            AMI_WARN("bsdsocket: released socket id %ld abandoned; "
                     "the kernel is already down", (long)entry->bh_Id);
        }

        ami_free(entry);
    }

    if (bracketed)
        bsd_nx_leave(base);
}

/* ---------------------------------------------------------------- vectors -- */

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
     * request registered against this NX_IP; neither survives being handed to
     * another base, so releasing one is refused rather than silently broken.
     */
    if ((sock->as_Flags & ASF_LISTENING) != 0)
        return bsd_fail(SocketBase, AMI_EOPNOTSUPP);

    result = bsd_handoff_park(SocketBase, sock, id, TRUE);
    if (result < 0)
        return result;

    /* The descriptor is gone; the registry now holds its reference. */
    bsd_fd_free(SocketBase, sock_fd);

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

    /* The copy is a second reference to the same NX socket -- NetX Duo has no
       way to duplicate one, and BSD's own semantics are a shared file entry
       rather than an independent connection, so this is the faithful shape. */
    sock->as_RefCount++;

    result = bsd_handoff_park(SocketBase, sock, id, FALSE);
    if (result < 0)
    {
        sock->as_RefCount--;
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

    entry = bsd_handoff_find(master, id);
    if (entry == NULL)
    {
        ReleaseSemaphore(&master->sb_Lock);
        return bsd_fail(SocketBase, AMI_ENOENT);
    }

    sock = entry->bh_Socket;

    /*
     * The caller states what it expects to get. Refusing a mismatch is the
     * whole value of those three arguments: an inetd child that asks for a
     * SOCK_STREAM and is handed a datagram socket would otherwise fail much
     * later and much less clearly.
     */
    if (domain != (LONG)sock->as_Domain || type != (LONG)sock->as_Type ||
        (protocol != 0 && protocol != sock->as_Protocol))
    {
        ReleaseSemaphore(&master->sb_Lock);
        return bsd_fail(SocketBase, AMI_EINVAL);
    }

    Remove((struct Node *)&entry->bh_Node);

    ReleaseSemaphore(&master->sb_Lock);

    ami_free(entry);

    fd = bsd_fd_alloc(SocketBase, sock);
    if (fd < 0)
    {
        /* Put it back rather than losing it. */
        (VOID)bsd_handoff_park(SocketBase, sock, id, TRUE);
        return bsd_fail(SocketBase, AMI_EMFILE);
    }

    /*
     * The socket is ours now: events go to this task. A socket obtained from
     * a ReleaseCopyOfSocket() therefore stops signalling the base that still
     * holds the original descriptor -- one NX socket has one owner, and there
     * is no second control block to give the other half.
     */
    sock->as_Owner = SocketBase;

    return fd;
}

/*
 * ObtainServerSocket() and ProcessIsServer() answer "was this Process
 * launched by the stack's inetd, and if so what socket was it given?".
 *
 * AmiNetXDuo ships no inetd and nothing in it ever launches a server
 * process, so the answer is always "no". That is a real answer, not a stub:
 *
 *   - ProcessIsServer() returns FALSE. The generated table would otherwise
 *     have pointed it at a stub returning -1, and -1 is all-bits-set, which
 *     every BOOL test reads as TRUE -- i.e. the stub was telling callers that
 *     every Process on the machine is a server process.
 *   - ObtainServerSocket() returns -1, which is exactly what Roadshow
 *     documents for a process that is not a server process.
 *
 * The mechanism a real inetd would use is not missing: it is
 * ReleaseCopyOfSocket() plus ObtainSocket() above. What is missing is only
 * the daemon, and the private convention Roadshow uses to tell a child which
 * id to ask for.
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

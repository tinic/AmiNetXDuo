/*
 * bsdsocket.library, handing a socket from one task to another.
 *
 *   ReleaseSocket()        release a socket, which is parked under an id
 *   ReleaseCopyOfSocket()  park a second reference and keep using the first
 *   ObtainSocket()         take a parked socket into this base's table
 *   ObtainServerSocket()   take the socket an inetd launched us with
 *   ProcessIsServer()      was this Process launched by an inetd?
 *
 * The Amiga-specific half of descriptor management, with no BSD counterpart.
 * There are no file descriptors and no descriptor passing over a socket
 * (docs/RESEARCH.md S3.1). A SocketBase belongs to exactly one task and its
 * descriptor table is private. A socket crosses between tasks only through
 * the registry here. That is how an inetd-style server works: the daemon
 * accepts, releases, launches a child and tells it the id. The child opens its
 * own bsdsocket.library and obtains it.
 *
 * The registry lives in the master base, guarded by its semaphore, since that
 * is the one object both tasks can see. A parked socket belongs to no base.
 * as_Owner is cleared, so the NetX Duo receive/disconnect callbacks find no
 * task to signal, rather than one that closed the library. ObtainSocket()
 * restores it.
 *
 * AmiSocket already carries as_RefCount for Dup2Socket(). A release moves the
 * existing reference into the registry. A copy takes an extra one, so the
 * original descriptor stays usable and the socket survives whichever of the
 * two goes away first. Anything still parked when the last opener closes the
 * library is released there, see bsd_handoff_flush(), called from
 * bsd_lib_close().
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
 *
 * A duplicate rejected with EEXIST makes the whole non-unique range unusable.
 * A daemon uses that range when it hands each accepted connection to a child
 * under the same well-known id.
 */
#define BSD_ID_NONUNIQUE_MAX  65535L

static BOOL bsd_handoff_id_is_unique(LONG id)
{
    return (id < 0 || id > BSD_ID_NONUNIQUE_MAX) ? TRUE : FALSE;
}

/*
 * ObtainSocket "must be identified by an ID, a domain, type and protocol
 * number", so a non-unique id is only half the key. The rest is what the
 * caller asks for. A match on id alone hands back whichever socket is first
 * in the list.
 *
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
                             LONG id, BOOL detach)
{
    struct AmiSocketBase *master = bsd_master_of(base);
    BsdHandoff           *entry;

    /* ENOMEM, not ENOBUFS: "[ENOMEM] There is not enough memory left to put
       this socket onto the public list." */
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
        /* Only the >65535 range promises uniqueness, so only there is a
           duplicate an error. Below it, several sockets under one id is
           documented behaviour. The errno is the autodoc's: "[EINVAL] The Id
           number requested is not unique." */
        ReleaseSemaphore(&master->sb_Lock);
        ami_free(entry);
        return bsd_fail(base, AMI_EINVAL);
    }

    entry->bh_Id     = id;
    entry->bh_Socket = sock;

    AddTail((struct List *)&master->sb_Handoffs, (struct Node *)&entry->bh_Node);

    /*
     * A fully released socket belongs to nobody until it is obtained. An
     * as_Owner left at the releasing base lets a receive callback Signal() a
     * task that closed the library.
     *
     * A copy leaves as_Owner alone: the original descriptor is still live in
     * the releasing base and is still the one to wake.
     */
    if (detach)
        sock->as_Owner = NULL;

    ReleaseSemaphore(&master->sb_Lock);

    return id;
}

/*
 * Release everything still parked. Called from bsd_lib_close() when the last
 * opener goes away. At that point no base exists that can obtain them, and the
 * alternative is a leak until reboot.
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
            AMI_WARN("bsdsocket: released socket id %ld abandoned. "
                     "The kernel is already down", (long)entry->bh_Id);
        }

        ami_free(entry);
    }

    if (bracketed)
        bsd_nx_leave(base);
}

/* ---------------------------------------------------------------- vectors, */

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

    result = bsd_handoff_park(SocketBase, sock, id, TRUE);
    if (result < 0)
        return result;

    /* The descriptor is gone. The registry now holds its reference. */
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

    /* The copy is a second reference to the same NX socket: NetX Duo cannot
       duplicate one, and BSD semantics are a shared file entry rather than an
       independent connection. */
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

    /* bsd_handoff_match() already refuses a socket whose domain/type/protocol
       do not match what the caller asked for, so "no match" and "wrong kind"
       are one answer: "[EBADF] No socket with the given Id could be found." */
    entry = bsd_handoff_match(master, id, domain, type, protocol);
    if (entry == NULL)
    {
        ReleaseSemaphore(&master->sb_Lock);
        return bsd_fail(SocketBase, AMI_EBADF);
    }

    sock = entry->bh_Socket;

    Remove((struct Node *)&entry->bh_Node);

    ReleaseSemaphore(&master->sb_Lock);

    ami_free(entry);

    fd = bsd_fd_alloc(SocketBase, sock);
    if (fd < 0)
    {
        /* Put it back rather than lose it. */
        (VOID)bsd_handoff_park(SocketBase, sock, id, TRUE);
        return bsd_fail(SocketBase, AMI_EMFILE);
    }

    /*
     * The socket is ours now: events go to this task. A socket obtained from
     * a ReleaseCopyOfSocket() therefore no longer signals the base that still
     * holds the original descriptor. One NX socket has one owner, and there
     * is no second control block for the other half.
     */
    sock->as_Owner = SocketBase;

    return fd;
}

/*
 * ObtainServerSocket() and ProcessIsServer() answer "was this Process launched
 * by the stack's inetd, and if so what socket was it given?".
 *
 * AmiNetXDuo ships no inetd and never launches a server process, so the answer
 * is always no:
 *
 *   - ProcessIsServer() returns FALSE. A generic stub that returns -1 is
 *     all-bits-set, which every BOOL test reads as TRUE. That reports every
 *     Process on the machine as a server process.
 *   - ObtainServerSocket() returns -1, what Roadshow documents for a process
 *     that is not a server process.
 *
 * A real inetd uses ReleaseCopyOfSocket() plus ObtainSocket() above. Only the
 * daemon is missing, and Roadshow's private convention for the id a child
 * asks for.
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

/*
 * F-059: CloseSocket() and a closing base's bsd_close_all() without a ThreadX
 * bracket -- adoption refused with the kernel still running, or another Task
 * holding the base's bracket (F-042).  The socket is not released and leaks
 * with its NetX callbacks installed; those reach the base through as_Owner
 * (bsd_event_post), and the base can be closed and freed before they fire.
 * Ownership must pass to another holder, or to nobody.
 *
 * socket.c is #included; the bracket is refused throughout, so the release
 * arm below is linked but never run.  Also Dup2Socket()'s claim on its
 * target, which another task cannot close or take meanwhile (F-055), and the
 * allocation's own claim, which an FDCB callback re-entering the library or
 * resizing the table cannot pass, and a close another task wins (N-087).
 *
 * SPDX-License-Identifier: MIT
 */

#include "bsdsocket_vectors.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned long h_checks;
static unsigned long h_failures;

#define CHECK(cond, what)                                                     \
    do {                                                                      \
        h_checks++;                                                           \
        if (!(cond)) {                                                        \
            h_failures++;                                                     \
            printf("  FAIL %s\n", (what));                                    \
        }                                                                     \
    } while (0)

#define H_FDS  4

static struct AmiSocketBase h_master;
static struct AmiSocketBase h_base;
static struct AmiSocketBase h_other;
static AmiSocket           *h_table[H_FDS];
static AmiSocket           *h_other_table[H_FDS];
static AmiSocket            h_sock[2];
static struct Task          h_task_base, h_task_other;
static int                  h_signals;
static struct Task         *h_signalled;

/* ---- what the unbracketed arms reach ---- */

LONG bsd_nx_enter(struct AmiSocketBase *base) { (VOID)base; return -1; }
VOID bsd_nx_leave(struct AmiSocketBase *base) { (VOID)base; abort(); }

VOID Signal(struct Task *task, ULONG mask)
{
    (VOID)mask;
    h_signals++;
    h_signalled = task;
}

LONG bsd_fail(struct AmiSocketBase *base, LONG code)
{
    base->sb_Errno = code;
    return -1;
}

VOID AddTail(struct List *list, struct Node *node)
{
    node->ln_Succ = (struct Node *)&list->lh_Tail;
    node->ln_Pred = list->lh_TailPred;
    list->lh_TailPred->ln_Succ = node;
    list->lh_TailPred = node;
}

/* N-087: another task running at the next Forbid(), once. */
static void (*h_forbid_hook)(void);
static int    h_forbid_skip;           /* Forbid()s to let pass first */

VOID Forbid(VOID)
{
    void (*hook)(void) = h_forbid_hook;

    if (hook != NULL && h_forbid_skip > 0)
    {
        h_forbid_skip--;
        return;
    }

    if (hook != NULL)
    {
        h_forbid_hook = NULL;
        hook();
    }
}
VOID Permit(VOID) { }

VOID bsd_bzero(APTR p, ULONG size) { memset(p, 0, size); }
VOID bsd_bcopy(CONST_APTR src, APTR dst, ULONG size) { memcpy(dst, src, size); }

#include "socket.c"

/* The release arm, linked and never run: the bracket is always refused. */
VOID _nx_tcp_packet_send_fin(NX_TCP_SOCKET *s, ULONG seq) { (VOID)s; (VOID)seq; abort(); }
VOID _nx_tcp_packet_send_rst(NX_TCP_SOCKET *s, NX_TCP_HEADER *h) { (VOID)s; (VOID)h; abort(); }
UINT _nxe_packet_release(NX_PACKET **p) { (VOID)p; abort(); }
UINT _nxe_tcp_client_socket_unbind(NX_TCP_SOCKET *s) { (VOID)s; abort(); }
UINT _nxe_tcp_server_socket_unaccept(NX_TCP_SOCKET *s) { (VOID)s; abort(); }
UINT _nxe_tcp_server_socket_unlisten(NX_IP *ip, UINT port) { (VOID)ip; (VOID)port; abort(); }
UINT _nxe_tcp_socket_delete(NX_TCP_SOCKET *s) { (VOID)s; abort(); }
UINT _nxe_tcp_socket_disconnect(NX_TCP_SOCKET *s, ULONG w) { (VOID)s; (VOID)w; abort(); }
UINT _nxe_tcp_socket_receive_notify(NX_TCP_SOCKET *s,
                                    VOID (*fn)(NX_TCP_SOCKET *))
{ (VOID)s; (VOID)fn; abort(); }
UINT _nxe_udp_socket_delete(NX_UDP_SOCKET *s) { (VOID)s; abort(); }
UINT _nxe_udp_socket_unbind(NX_UDP_SOCKET *s) { (VOID)s; abort(); }
ULONG _tx_time_get(VOID) { abort(); }
UINT _txe_mutex_get(TX_MUTEX *m, ULONG w) { (VOID)m; (VOID)w; abort(); }
UINT _txe_mutex_put(TX_MUTEX *m) { (VOID)m; abort(); }
/* Only SBTC_DTABLESIZE's resize allocates here (N-087).  The slack keeps a
   store past a shrunk table inside the block, so the check reports it. */
static int h_alloc_ok;

/* The slack past a new table names h_poison, so a read past its end is a
   socket the test can see touched. */
static AmiSocket h_poison;

APTR ami_alloc(ULONG n)
{
    AmiSocket **block;
    ULONG       i;

    if (!h_alloc_ok)
        abort();
    block = (AmiSocket **)calloc(1, n + 64);
    if (block == NULL)
        abort();
    for (i = n / sizeof(AmiSocket *); i < (n + 64) / sizeof(AmiSocket *); i++)
        block[i] = &h_poison;
    return block;
}

/* A table the test watches: freed means poisoned and kept, so a store that
   lands in it after the swap is seen rather than undefined. */
static AmiSocket **h_watch;
static int         h_watch_freed;

VOID ami_free(APTR p)
{
    if (p == (APTR)h_table || p == (APTR)h_other_table)
        return;
    if (p != NULL && p == (APTR)h_watch)
    {
        int i;

        for (i = 0; i < H_FDS; i++)
            h_watch[i] = &h_poison;
        h_watch_freed = 1;
        return;
    }
    if (!h_alloc_ok)
        abort();
    free(p);
}
VOID ami_mem_socket_delta(LONG d) { (VOID)d; abort(); }
VOID bsd_mcast_close(AmiSocket *s) { (VOID)s; abort(); }
VOID bsd_raw_close(AmiSocket *s) { (VOID)s; abort(); }


static void h_link(struct AmiSocketBase *b)
{
    AddTail((struct List *)&h_master.sb_Children, (struct Node *)&b->sb_Node);
}

static void h_reset(void)
{
    struct List *l = (struct List *)&h_master.sb_Children;

    memset(&h_master, 0, sizeof(h_master));
    memset(&h_base, 0, sizeof(h_base));
    memset(&h_other, 0, sizeof(h_other));
    memset(h_table, 0, sizeof(h_table));
    memset(h_other_table, 0, sizeof(h_other_table));
    memset(h_sock, 0, sizeof(h_sock));

    l->lh_Head     = (struct Node *)&l->lh_Tail;
    l->lh_Tail     = NULL;
    l->lh_TailPred = (struct Node *)&l->lh_Head;
    h_link(&h_base);
    h_link(&h_other);

    h_base.sb_Master = h_other.sb_Master = &h_master;
    h_base.sb_Table = h_table;
    h_base.sb_TableSize = H_FDS;
    h_other.sb_Table = h_other_table;
    h_other.sb_TableSize = H_FDS;
    h_base.sb_Task = &h_task_base;
    h_other.sb_Task = &h_task_other;
    h_base.sb_EventSigMask = 1UL << 10;
    h_other.sb_EventSigMask = 1UL << 11;

    h_signals = 0;
    h_signalled = NULL;
}

/* SBTC_FDCALLBACK: the last action seen, and the answer to give. */
static LONG h_fdcb_action;
static LONG h_fdcb_answer;

static LONG h_fdcb(LONG fd, LONG action)
{
    (VOID)fd;
    h_fdcb_action = action;
    return h_fdcb_answer;
}

static void t_fd_claim(void)
{
    AmiSocket *prev = NULL;

    h_reset();
    h_base.sb_FDCallback = h_fdcb;
    h_fdcb_answer = 0;
    h_sock[0].as_RefCount = 1;
    h_sock[0].as_Owner = &h_base;
    h_table[1] = &h_sock[0];

    h_fdcb_action = -1;
    CHECK(bsd_fd_claim(&h_base, 1, &prev) == 0 && prev == &h_sock[0] &&
          h_table[1] == BSD_FD_BUSY && h_fdcb_action == FDCB_FREE,
          "a claim takes the socket out of the slot, after FDCB_FREE");

    /* Another task on the base, meanwhile. */
    CHECK(bsd_CloseSocket(1, &h_base) == -1 && h_base.sb_Errno == AMI_EBADF,
          "CloseSocket of a claimed descriptor is EBADF");
    CHECK(h_table[1] == BSD_FD_BUSY && h_sock[0].as_RefCount == 1 &&
          h_sock[0].as_Owner == &h_base,
          "and touches neither the slot nor the socket");
    CHECK(bsd_fd_claim(&h_base, 1, &prev) == -1 && h_base.sb_Errno == AMI_EBUSY &&
          prev == &h_sock[0],
          "a second claim is EBUSY");
    CHECK(bsd_fd_alloc(&h_base, &h_sock[1]) == 0 && h_table[1] == BSD_FD_BUSY,
          "an allocation passes it by");
    h_table[0] = NULL;

    h_fdcb_answer = 5;
    CHECK(bsd_fd_settle(&h_base, 1, &h_sock[1]) == -1 && h_base.sb_Errno == 5 &&
          h_table[1] == BSD_FD_BUSY,
          "a refused settle leaves the claim");
    h_fdcb_answer = 0;
    h_fdcb_action = -1;
    CHECK(bsd_fd_unclaim(&h_base, 1, &h_sock[0]) == 0 && h_table[1] == &h_sock[0] &&
          h_fdcb_action == FDCB_ALLOC,
          "unclaim puts the socket back with FDCB_ALLOC");

    CHECK(bsd_fd_claim(&h_base, 1, &prev) == 0, "claimed again");
    h_fdcb_answer = 5;
    CHECK(bsd_fd_unclaim(&h_base, 1, prev) == -1 && h_table[1] == NULL,
          "a refused FDCB_ALLOC leaves it empty, for the caller to release");

    h_fdcb_answer = 0;
    h_table[2] = &h_sock[0];
    h_fdcb_answer = 7;
    CHECK(bsd_fd_claim(&h_base, 2, &prev) == -1 && h_table[2] == &h_sock[0],
          "a vetoed FDCB_FREE leaves the slot as it was");
    h_fdcb_answer = 0;
    CHECK(bsd_fd_claim(&h_base, 3, &prev) == 0 && prev == NULL &&
          bsd_fd_settle(&h_base, 3, &h_sock[1]) == 0 && h_table[3] == &h_sock[1],
          "an empty slot is claimed and settled");
    CHECK(bsd_fd_claim(&h_base, H_FDS, &prev) == -1 && h_base.sb_Errno == AMI_EBADF,
          "a descriptor past the table is EBADF");

    h_base.sb_FDCallback = NULL;
}

static LONG h_free_guard(LONG fd, LONG action)
{
    if (action != FDCB_FREE)
        return 0;

    CHECK(h_table[fd] == BSD_FD_BUSY && bsd_lookup(&h_base, fd) == NULL,
          "a free callback cannot look up its closing descriptor");
    CHECK(bsd_fd_alloc(&h_base, &h_sock[1]) == 0 &&
          h_table[fd] == BSD_FD_BUSY,
          "callback allocation leaves the closing descriptor reserved");
    return h_fdcb_answer;
}

static void t_fd_free_guard(void)
{
    h_reset();
    h_table[1] = &h_sock[0];
    h_base.sb_FDCallback = h_free_guard;
    h_fdcb_answer = 7;
    CHECK(bsd_fd_free(&h_base, 1) == -1 && h_base.sb_Errno == 7 &&
          h_table[1] == &h_sock[0],
          "a callback veto restores the original descriptor");
    h_table[0] = NULL;
    h_fdcb_answer = 0;
    CHECK(bsd_fd_free(&h_base, 1) == 0 && h_table[1] == NULL,
          "an accepted free clears its claimed descriptor");
    h_table[0] = NULL;
    h_table[1] = BSD_FD_RESERVED;
    CHECK(bsd_fd_free(&h_base, 1) == 0 && h_table[1] == NULL,
          "a reserved external descriptor uses the same callback guard");
    h_base.sb_FDCallback = NULL;
}

/* ------------------------------------------------------------ N-087 --- */

static int h_reenter_armed;
static LONG h_inner_fd;
static LONG h_resize_to;
static LONG h_resize_rc;
static LONG h_check_refuse_fd;
static LONG h_alloc_fail;

static LONG h_fdcb_n087(LONG fd, LONG action)
{
    if (action == FDCB_CHECK && fd == h_check_refuse_fd)
        return 1;

    if (action == FDCB_ALLOC && h_alloc_fail != 0)
        return h_alloc_fail;

    if (action == FDCB_ALLOC && h_reenter_armed)
    {
        h_reenter_armed = 0;
        h_inner_fd = bsd_fd_alloc(&h_base, &h_sock[1]);
    }

    if (action == FDCB_ALLOC && h_resize_to != 0)
    {
        LONG to = h_resize_to;

        h_resize_to = 0;
        h_resize_rc = bsd_table_resize(&h_base, to);
    }

    return 0;
}

static void h_n087_reset(void)
{
    h_reset();
    h_base.sb_FDCallback = h_fdcb_n087;
    h_reenter_armed   = 0;
    h_inner_fd        = -1;
    h_resize_to       = 0;
    h_resize_rc       = 99;
    h_check_refuse_fd = -1;
    h_alloc_fail      = 0;
    h_alloc_ok        = 1;
}

static void t_fd_alloc_reentry(void)
{
    LONG fd;

    h_n087_reset();
    h_reenter_armed = 1;
    fd = bsd_fd_alloc(&h_base, &h_sock[0]);
    CHECK(fd >= 0 && h_inner_fd >= 0 && fd != h_inner_fd,
          "N-087: a callback's own allocation gets another descriptor");
    CHECK(fd >= 0 && h_inner_fd >= 0 &&
          h_base.sb_Table[fd] == &h_sock[0] &&
          h_base.sb_Table[h_inner_fd] == &h_sock[1],
          "N-087: both sockets stay reachable");
}

static void t_fd_alloc_resize(void)
{
    LONG fd;

    /* Shrink below the slot being allocated: refused, nothing past the end. */
    h_n087_reset();
    h_table[0] = &h_sock[1];
    h_resize_to = 1;
    fd = bsd_fd_alloc(&h_base, &h_sock[0]);
    CHECK(fd == 1 && h_resize_rc == -1 && h_base.sb_TableSize == H_FDS &&
          h_base.sb_Table == h_table && h_table[1] == &h_sock[0],
          "N-087: DTABLESIZE cannot shrink past a slot being allocated");
    if (h_base.sb_Table != h_table)
        free(h_base.sb_Table);

    /* Grow: the store lands in the new table. */
    h_n087_reset();
    h_table[0] = &h_sock[1];
    h_resize_to = 8;
    fd = bsd_fd_alloc(&h_base, &h_sock[0]);
    CHECK(fd == 1 && h_resize_rc == 0 && h_base.sb_TableSize == 8 &&
          h_base.sb_Table != h_table && h_base.sb_Table[1] == &h_sock[0] &&
          h_base.sb_Table[0] == &h_sock[1],
          "N-087: a table grown in the callback gets the store");
    if (h_base.sb_Table != h_table)
        free(h_base.sb_Table);
    h_base.sb_Table = h_table;
}

static void t_fd_alloc_cleanup(void)
{
    LONG fd;

    h_n087_reset();
    h_check_refuse_fd = 0;
    h_base.sb_Errno = 99;
    fd = bsd_fd_alloc(&h_base, &h_sock[0]);
    CHECK(fd == 1 && h_table[0] == BSD_FD_RESERVED &&
          h_table[1] == &h_sock[0] && h_base.sb_Errno == 99,
          "N-087: a CHECK refusal reserves the slot and goes on");

    h_n087_reset();
    h_alloc_fail = 9;
    fd = bsd_fd_alloc(&h_base, &h_sock[0]);
    CHECK(fd == -1 && h_base.sb_Errno == 9 && h_table[0] == NULL,
          "N-087: an ALLOC failure leaves the slot empty, its errno kept");

    h_n087_reset();
    h_alloc_fail = 9;
    CHECK(bsd_fd_reserve(&h_base, 2) == -1 && h_base.sb_Errno == 9 &&
          h_table[2] == NULL,
          "N-087: a reserve whose ALLOC fails leaves the slot empty");
    h_alloc_fail = 0;
    CHECK(bsd_fd_reserve(&h_base, 2) == 2 && h_table[2] == BSD_FD_RESERVED,
          "N-087: a reserve that succeeds holds the slot");
    h_base.sb_FDCallback = NULL;
}

/* The other task closes fd 0 between this task's lookup and its free. */
static void h_other_close(void)
{
    CHECK(bsd_CloseSocket(0, &h_base) == 0, "N-087: the other task's close wins");
}

static void t_close_race(void)
{
    LONG rc;

    h_reset();
    bsd_defer_head = NULL;
    h_sock[0].as_RefCount = 3;
    h_sock[0].as_Owner = &h_base;
    h_table[0] = &h_sock[0];
    h_other_table[1] = &h_sock[0];
    h_forbid_hook = h_other_close;
    h_base.sb_Errno = 0;
    rc = bsd_CloseSocket(0, &h_base);
    h_forbid_hook = NULL;
    CHECK(rc == -1 && h_base.sb_Errno == AMI_EBADF,
          "N-087: the losing close is EBADF");
    CHECK(h_sock[0].as_DeferRefs == 1,
          "N-087: the socket is released once, not twice");
    bsd_defer_head = NULL;
}

/* The other task closes fd 3 and shrinks the table to two, between this
   task's lookup and the bound in its take. */
static void h_other_close_shrink(void)
{
    CHECK(bsd_CloseSocket(3, &h_base) == 0, "N-087: the other task closes fd 3");
    CHECK(bsd_table_resize(&h_base, 2) == 0, "N-087: and shrinks the table to 2");
}

/* At each of the close's first Forbid()s in turn, so the take's bound is
   reached whichever lookups come before it. */
static void t_close_shrink_race(void)
{
    int skip;

    for (skip = 0; skip < 3; skip++)
    {
        LONG rc;
        char what[96];

        h_reset();
        h_alloc_ok = 1;
        bsd_defer_head = NULL;
        memset(&h_poison, 0, sizeof(h_poison));
        h_sock[0].as_RefCount = 3;
        h_sock[0].as_Owner = &h_base;
        h_table[3] = &h_sock[0];
        h_other_table[1] = &h_sock[0];
        h_forbid_skip = skip;
        h_forbid_hook = h_other_close_shrink;
        h_base.sb_Errno = 0;
        rc = bsd_CloseSocket(3, &h_base);
        h_forbid_hook = NULL;
        h_forbid_skip = 0;
        snprintf(what, sizeof(what),
                 "N-087: shrink at Forbid %d: the overtaken close is EBADF", skip);
        CHECK(rc == -1 && h_base.sb_Errno == AMI_EBADF, what);
        snprintf(what, sizeof(what),
                 "N-087: shrink at Forbid %d: nothing past the table is read", skip);
        CHECK(h_poison.as_DeferRefs == 0 && h_sock[0].as_DeferRefs == 1, what);
        if (h_base.sb_Table != h_table)
            free(h_base.sb_Table);
        h_base.sb_Table = h_table;
        bsd_defer_head = NULL;
    }
}

/* --------------------------------------- N-087: grow at a slot store --- */

static LONG h_grow_on;
static LONG h_grow_answer;

static void h_grow(void)
{
    CHECK(bsd_table_resize(&h_base, 8) == 0,
          "N-087: the other task grows the table at the store");
}

static LONG h_fdcb_grow(LONG fd, LONG action)
{
    (VOID)fd;
    if (action == h_grow_on)
    {
        h_forbid_skip = 0;
        h_forbid_hook = h_grow;
    }
    return h_grow_answer;
}

/* The base on a table of its own (h_watch), as h_table holds it now. */
static void h_grow_reset(void)
{
    h_reset();
    h_alloc_ok    = 1;
    bsd_defer_head = NULL;
    h_watch_freed = 0;
    h_watch = (AmiSocket **)ami_alloc(H_FDS * sizeof(AmiSocket *));
    h_base.sb_Table = h_watch;
    h_base.sb_FDCallback = h_fdcb_grow;
    h_grow_on     = -1;
    h_grow_answer = 0;
}

static int h_watch_untouched(void)
{
    int i;

    if (!h_watch_freed)
        return 0;
    for (i = 0; i < H_FDS; i++)
        if (h_watch[i] != &h_poison)
            return 0;
    return 1;
}

static void h_grow_done(void)
{
    h_forbid_hook = NULL;
    h_base.sb_FDCallback = NULL;
    if (h_base.sb_Table != h_watch)
        free(h_base.sb_Table);
    free(h_watch);
    h_watch = NULL;
    h_base.sb_Table = h_table;
    bsd_defer_head = NULL;
}

static void t_store_grow(void)
{
    AmiSocket *prev = NULL;

    /* settle */
    h_grow_reset();
    CHECK(bsd_fd_claim(&h_base, 1, &prev) == 0 && prev == NULL, "N-087: claimed");
    h_grow_on = FDCB_ALLOC;
    CHECK(bsd_fd_settle(&h_base, 1, &h_sock[1]) == 0, "N-087: settled");
    CHECK(h_base.sb_TableSize == 8 && h_base.sb_Table != h_watch &&
          h_base.sb_Table[1] == &h_sock[1] && h_watch_untouched(),
          "N-087: settle's store reaches the grown table, not the freed one");
    h_grow_done();

    /* unclaim, putting a socket back */
    h_grow_reset();
    h_watch[2] = &h_sock[0];
    CHECK(bsd_fd_claim(&h_base, 2, &prev) == 0 && prev == &h_sock[0],
          "N-087: claimed a held slot");
    h_grow_on = FDCB_ALLOC;
    CHECK(bsd_fd_unclaim(&h_base, 2, prev) == 0, "N-087: unclaimed");
    CHECK(h_base.sb_TableSize == 8 && h_base.sb_Table != h_watch &&
          h_base.sb_Table[2] == &h_sock[0] && h_watch_untouched(),
          "N-087: unclaim's store reaches the grown table");
    h_grow_done();

    /* the claim's veto */
    h_grow_reset();
    h_watch[3] = &h_sock[0];
    h_grow_on = FDCB_FREE;
    h_grow_answer = 7;
    CHECK(bsd_fd_claim(&h_base, 3, &prev) == -1 && h_base.sb_Errno == 7,
          "N-087: a vetoed claim");
    CHECK(h_base.sb_TableSize == 8 && h_base.sb_Table != h_watch &&
          h_base.sb_Table[3] == &h_sock[0] && h_watch_untouched(),
          "N-087: the veto's restore reaches the grown table");
    h_grow_done();

    /* bsd_close_all() emptying a slot whose free was vetoed */
    h_grow_reset();
    h_sock[0].as_RefCount = 1;
    h_sock[0].as_Owner = &h_base;
    h_watch[0] = &h_sock[0];
    h_grow_on = FDCB_FREE;
    h_grow_answer = 7;
    bsd_close_all(&h_base);
    CHECK(h_base.sb_TableSize == 8 && h_base.sb_Table != h_watch &&
          h_base.sb_Table[0] == NULL && h_watch_untouched(),
          "N-087: close_all's store reaches the grown table");
    h_grow_done();
    h_alloc_ok = 0;
}

int main(void)
{
    t_fd_claim();
    t_fd_free_guard();
    t_fd_alloc_reentry();
    t_fd_alloc_resize();
    t_fd_alloc_cleanup();
    t_close_race();
    t_close_shrink_race();
    t_store_grow();
    h_alloc_ok = 0;

    /* CloseSocket() with the bracket refused: the socket leaks, and its
       callbacks no longer reach this base. */
    h_reset();
    h_sock[0].as_RefCount = 1;
    h_sock[0].as_Owner = &h_base;
    h_table[0] = &h_sock[0];
    CHECK(bsd_CloseSocket(0, &h_base) == 0, "CloseSocket succeeds");
    CHECK(h_table[0] == NULL, "the descriptor is gone");
    CHECK(h_sock[0].as_RefCount == 1, "the socket is not released (it leaks)");
    CHECK(h_sock[0].as_Owner == NULL, "and no longer names this base");

    /* The same socket held by another opener too: that opener owns it. */
    h_reset();
    h_sock[0].as_RefCount = 2;
    h_sock[0].as_Owner = &h_base;
    h_table[0] = &h_sock[0];
    h_other_table[1] = &h_sock[0];
    CHECK(bsd_CloseSocket(0, &h_base) == 0, "CloseSocket of a shared socket");
    CHECK(h_sock[0].as_Owner == &h_other, "the other holder owns it");
    CHECK(h_signals == 1 && h_signalled == &h_task_other,
          "and is signalled once");

    /* A closing base with the bracket refused: every socket leaks, none
       keeps the base as its owner.  A Dup2Socket() alias frees last. */
    h_reset();
    h_sock[0].as_RefCount = 2;
    h_sock[0].as_Owner = &h_base;
    h_sock[1].as_RefCount = 1;
    h_sock[1].as_Owner = &h_base;
    h_table[0] = &h_sock[0];
    h_table[1] = &h_sock[1];
    h_table[2] = &h_sock[0];                    /* an alias of fd 0 */
    bsd_close_all(&h_base);
    CHECK(h_table[0] == NULL && h_table[1] == NULL && h_table[2] == NULL,
          "every descriptor is gone");
    CHECK(h_sock[0].as_Owner == NULL && h_sock[1].as_Owner == NULL,
          "and no leaked socket names the closing base");

    printf("unbracketed_close: %lu checks, %lu failures\n",
           h_checks, h_failures);
    return (h_failures != 0) ? 1 : 0;
}

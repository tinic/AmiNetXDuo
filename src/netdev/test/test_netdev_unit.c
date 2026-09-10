/*
 * What an opener's filter state and its departure do to a unit.
 *
 * THE HASH IS THE ONE THAT MATTERS.  A DP8390 and a LANCE both decide which
 * multicast frames reach the driver from a 64-bit hash the host programs, and
 * netdev_rebuild_filter() is what builds it.  Get it wrong in the quiet
 * direction -- a bit not set -- and the card silently drops the router's
 * neighbour solicitation to 33:33:ff:xx:xx:xx, so on-link IPv6 works (a router
 * advertisement comes back unicast) and off-link never resolves.  That is the
 * same failure the S2_ADDMULTICASTADDRESS group-bit divergence causes at the
 * other end of the same path, and emulator.yml:466-470 records it happening:
 * "x-surf, x-surf-100 Z2 and Z3 could not reach a single address past the
 * router: their drivers refuse every S2_ADDMULTICASTADDRESS, so the router's
 * neighbour solicitation for the guest was dropped by the card."
 *
 * These three functions lived in netdev_device.c, which cannot be compiled off
 * target -- a raw romtag asm at its head and <exec/execbase.h> at :19 -- so
 * none of them had ever run on a host.  netdev_unit.c is the split that let
 * them, at a measured 4 bytes of resident RAM.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <string.h>

#include <proto/exec.h>

#include "netdev_internal.h"
#include "netdev_mcaf.h"

static int failures;
static int checks;

static void expect(int ok, const char *what)
{
    checks++;
    if (ok)
        return;

    printf("FAIL %s\n", what);
    failures++;
}

static void expect_u32(const char *what, unsigned long got, unsigned long want)
{
    checks++;
    if (got == want)
        return;

    printf("FAIL %s: got 0x%lx, want 0x%lx\n", what, got, want);
    failures++;
}

/* ---------------------------------------------------------------- exec --- */

static int disable_depth;
static int replies;

VOID Disable(VOID)
{
    disable_depth++;
}

VOID Enable(VOID)
{
    disable_depth--;
    if (disable_depth < 0)
    {
        printf("FAIL Enable() without a matching Disable()\n");
        failures++;
        disable_depth = 0;
    }
}

/* A reply from a node still linked into a list is the one thing a driver must
   never do; Remove() nulls ln_Succ, so this catches it. */
VOID ReplyMsg(struct Message *msg)
{
    replies++;
    if (msg->mn_Node.ln_Succ != NULL)
    {
        printf("FAIL ReplyMsg() on a node still linked into a list\n");
        failures++;
    }
    msg->mn_Node.ln_Type = NT_REPLYMSG;
}

VOID NewList(struct List *l)
{
    l->lh_Head     = (struct Node *)&l->lh_Tail;
    l->lh_Tail     = NULL;
    l->lh_TailPred = (struct Node *)&l->lh_Head;
}

VOID AddTail(struct List *l, struct Node *n)
{
    n->ln_Succ              = (struct Node *)&l->lh_Tail;
    n->ln_Pred              = l->lh_TailPred;
    l->lh_TailPred->ln_Succ = n;
    l->lh_TailPred          = n;
}

VOID AddHead(struct List *l, struct Node *n)
{
    n->ln_Succ          = l->lh_Head;
    n->ln_Pred          = (struct Node *)&l->lh_Head;
    l->lh_Head->ln_Pred = n;
    l->lh_Head          = n;
}

VOID Remove(struct Node *n)
{
    n->ln_Pred->ln_Succ = n->ln_Succ;
    n->ln_Succ->ln_Pred = n->ln_Pred;
    n->ln_Succ = NULL;
    n->ln_Pred = NULL;
}

struct Node *RemHead(struct List *l)
{
    struct Node *n = l->lh_Head;

    if (n->ln_Succ == NULL)
        return NULL;
    Remove(n);
    return n;
}

/* ------------------------------------------------------------ the chip --- */

static int   setfilter_calls;
static int   setfilter_depth;      /* Disable() nesting when it was called */
static UBYTE setfilter_mar[8];

static VOID t_setfilter(NetdevNic *nic)
{
    setfilter_calls++;
    setfilter_depth = disable_depth;
    memcpy(setfilter_mar, nic->mar, sizeof(setfilter_mar));
}

/* Designated, so the table's shape is the header's business and not this
   file's: a positional initializer here went stale the moment a member was
   added, and said so only as a warning. */
static const struct NetdevNicOps t_ops = { .setfilter = t_setfilter };

/* ----------------------------------------------------------- the fixture - */

static NetdevUnit   unit;
static NetdevOpener op_a;
static NetdevOpener op_b;

static void reset(void)
{
    memset(&unit, 0, sizeof(unit));
    memset(&op_a, 0, sizeof(op_a));
    memset(&op_b, 0, sizeof(op_b));

    unit.nu_Nic.ops = &t_ops;
    NewList(&unit.nu_Writes);

    op_a.op_Hw = &unit;
    op_b.op_Hw = &unit;

    disable_depth = replies = 0;
    setfilter_calls = 0;
    setfilter_depth = -1;
    memset(setfilter_mar, 0, sizeof(setfilter_mar));
}

static void join(const UBYTE *addr, UWORD slot)
{
    memcpy(unit.nu_Mcast[slot].addr, addr, NETDEV_ADDR_LEN);
    unit.nu_Mcast[slot].refs = 1;
}

static int mar_is_all_ones(const UBYTE *mar)
{
    UWORD i;

    for (i = 0; i < 8u; i++)
    {
        if (mar[i] != 0xffu)
            return 0;
    }

    return 1;
}

static int mar_is_zero(const UBYTE *mar)
{
    UWORD i;

    for (i = 0; i < 8u; i++)
    {
        if (mar[i] != 0)
            return 0;
    }

    return 1;
}

/* ============================================================ the filter = */

/* No groups joined: the hash accepts nothing, and the chip is still told. */
static void a_no_groups_is_an_empty_hash(void)
{
    reset();
    netdev_rebuild_filter(&unit);

    expect(setfilter_calls == 1, "the chip is programmed even with no groups");
    expect(mar_is_zero(setfilter_mar), "and the hash accepts nothing");
    expect(mar_is_zero(unit.nu_Nic.mar), "the unit's copy agrees");
    expect_u32("Disable balanced", (unsigned long)disable_depth, 0);
    expect(setfilter_depth >= 1,
           "and setfilter ran inside Disable(), where the interrupt server"
           " cannot see a half-built hash");
}

/*
 * A joined group sets its own bit and leaves the rest alone.  Checked against
 * netdev_mar_set() over a fresh hash rather than against a constant, because
 * the bit a given address lands on is netdev_mcaf.c's business and is tested
 * there -- what THIS file owns is that the table is what gets hashed.
 */
static void b_a_joined_group_sets_its_own_bit(void)
{
    static const UBYTE ipv6_ns[6] = { 0x33, 0x33, 0xff, 0x12, 0x34, 0x56 };
    static const UBYTE ipv4_mc[6] = { 0x01, 0x00, 0x5e, 0x00, 0x00, 0x01 };
    UBYTE want[8];

    reset();
    join(ipv6_ns, 0);
    netdev_rebuild_filter(&unit);

    netdev_mar_clear(want);
    netdev_mar_set(want, ipv6_ns);
    expect(memcmp(setfilter_mar, want, sizeof(want)) == 0,
           "a solicited-node group is in the hash");
    expect(!mar_is_zero(setfilter_mar),
           "which is to say the card would accept it");

    /* Two groups, and both survive: an implementation that rebuilt from the
       last entry rather than all of them passes the single-group case. */
    reset();
    join(ipv6_ns, 0);
    join(ipv4_mc, 7);
    netdev_rebuild_filter(&unit);

    netdev_mar_clear(want);
    netdev_mar_set(want, ipv6_ns);
    netdev_mar_set(want, ipv4_mc);
    expect(memcmp(setfilter_mar, want, sizeof(want)) == 0,
           "two groups joined, both in the hash");
}

/* A slot with no references is not a member, whatever is left in its bytes. */
static void c_a_released_slot_is_not_a_member(void)
{
    static const UBYTE mc[6] = { 0x01, 0x00, 0x5e, 0x0a, 0x0b, 0x0c };

    reset();
    join(mc, 3);
    unit.nu_Mcast[3].refs = 0;          /* left behind by the last leave */

    netdev_rebuild_filter(&unit);

    expect(mar_is_zero(setfilter_mar),
           "a slot whose refcount reached zero is not hashed");
}

/*
 * Promiscuous and accept-all-multicast both mean "every group", and the hash
 * is the only thing that can say so to the chip.  An all-ones hash is not an
 * optimisation here: it is the difference between a bridge or a tcpdump
 * seeing traffic and seeing nothing.
 */
static void d_promiscuous_and_allmulti_accept_everything(void)
{
    static const UBYTE mc[6] = { 0x01, 0x00, 0x5e, 0x01, 0x02, 0x03 };

    reset();
    unit.nu_Nic.promisc = 1;
    netdev_rebuild_filter(&unit);
    expect(mar_is_all_ones(setfilter_mar),
           "promiscuous accepts every group");

    reset();
    unit.nu_AllMulti = 1;
    netdev_rebuild_filter(&unit);
    expect(mar_is_all_ones(setfilter_mar),
           "and so does one accept-all-multicast reference");

    /* Even with a specific group joined, all-multicast still means all. */
    reset();
    join(mc, 0);
    unit.nu_AllMulti = 2;
    netdev_rebuild_filter(&unit);
    expect(mar_is_all_ones(setfilter_mar),
           "all-multicast is not narrowed by an exact join beside it");

    /* And the last release of it goes back to the exact table. */
    unit.nu_AllMulti = 0;
    netdev_rebuild_filter(&unit);
    {
        UBYTE want[8];

        netdev_mar_clear(want);
        netdev_mar_set(want, mc);
        expect(memcmp(setfilter_mar, want, sizeof(want)) == 0,
               "and releasing it restores the exact table");
    }
}

/*
 * Every slot the table has, and the LAST one chosen so a short loop is
 * visible.  Filling all 32 with arbitrary addresses does not do it: 32
 * addresses over a 64-bit hash collide often, so dropping the last entry
 * frequently leaves the same MAR and a loop bound one short passes.  The last
 * address here is searched for -- it is one whose bit no other entry has set.
 */
static void e_the_whole_table_is_hashed(void)
{
    UBYTE want[8];
    UBYTE last[6];
    UWORD i;
    int   found = 0;

    reset();
    netdev_mar_clear(want);

    for (i = 0; i + 1u < (UWORD)NETDEV_MCAST_MAX; i++)
    {
        UBYTE a[6];

        a[0] = 0x01; a[1] = 0x00; a[2] = 0x5e;
        a[3] = 0x00; a[4] = (UBYTE)(i >> 8); a[5] = (UBYTE)i;
        join(a, i);
        netdev_mar_set(want, a);
    }

    /* An address whose hash bit is not already set by any of the others, so
       its absence from the hash is detectable. */
    for (i = 0; i < 4096u && !found; i++)
    {
        UBYTE probe[8];

        last[0] = 0x01; last[1] = 0x00; last[2] = 0x5e;
        last[3] = 0x7f; last[4] = (UBYTE)(i >> 8); last[5] = (UBYTE)i;

        memcpy(probe, want, sizeof(probe));
        netdev_mar_set(probe, last);
        if (memcmp(probe, want, sizeof(probe)) != 0)
            found = 1;
    }

    expect(found, "an address with a hash bit of its own exists to test with");
    if (!found)
        return;

    join(last, (UWORD)(NETDEV_MCAST_MAX - 1));
    netdev_mar_set(want, last);

    netdev_rebuild_filter(&unit);
    expect(memcmp(setfilter_mar, want, sizeof(want)) == 0,
           "every slot of the table is hashed, the last one included");
}

/* ============================================================ the reply == */

/*
 * IOF_QUICK means the caller did not go through a reply port, so replying
 * would push a message at a port that is not waiting for one.  Clearing it
 * means the caller IS waiting and a missing reply hangs it forever.  Both
 * directions, because each is a different bug.
 */
static void f_reply(void)
{
    struct IOSana2Req io;

    reset();
    memset(&io, 0, sizeof(io));
    io.ios2_Req.io_Flags = IOF_QUICK;

    netdev_reply(&io, S2ERR_BAD_STATE, S2WERR_NOT_CONFIGURED);

    expect_u32("a quick request carries the error",
               (unsigned long)(UBYTE)io.ios2_Req.io_Error,
               (unsigned long)(UBYTE)S2ERR_BAD_STATE);
    expect_u32("and the wire error", io.ios2_WireError,
               (unsigned long)S2WERR_NOT_CONFIGURED);
    expect(replies == 0, "and is NOT replied to");

    memset(&io, 0, sizeof(io));
    netdev_reply(&io, 0, 0);
    expect(replies == 1, "a request that is not quick IS replied to");
    expect_u32("with no error", (unsigned long)(UBYTE)io.ios2_Req.io_Error, 0);
}

/* ============================================================ the drop === */

/*
 * A caller flushes so that teardown is safe.  Its writes are queued on the
 * UNIT, not on the opener, so this is what stops one of its own requests
 * living on in the driver after it has gone -- and it must take ONLY its own.
 */
static void g_drop_writes_takes_only_one_openers(void)
{
    struct IOSana2Req a1, a2, b1;

    reset();

    memset(&a1, 0, sizeof(a1));
    memset(&a2, 0, sizeof(a2));
    memset(&b1, 0, sizeof(b1));

    a1.ios2_Req.io_Unit = (struct Unit *)&op_a.op_Unit;
    a2.ios2_Req.io_Unit = (struct Unit *)&op_a.op_Unit;
    b1.ios2_Req.io_Unit = (struct Unit *)&op_b.op_Unit;

    AddTail(&unit.nu_Writes, &a1.ios2_Req.io_Message.mn_Node);
    AddTail(&unit.nu_Writes, &b1.ios2_Req.io_Message.mn_Node);
    AddTail(&unit.nu_Writes, &a2.ios2_Req.io_Message.mn_Node);

    netdev_drop_writes(&unit, &op_a);

    expect_u32("both of that opener's writes came back",
               (unsigned long)replies, 2);
    expect_u32("aborted", (unsigned long)(UBYTE)a1.ios2_Req.io_Error,
               (unsigned long)(UBYTE)IOERR_ABORTED);
    expect_u32("both of them", (unsigned long)(UBYTE)a2.ios2_Req.io_Error,
               (unsigned long)(UBYTE)IOERR_ABORTED);

    /* The other opener's is untouched and still on the list -- completing
       somebody else's request is worse than leaving one behind. */
    expect_u32("the other opener's write was not completed",
               (unsigned long)(UBYTE)b1.ios2_Req.io_Error, 0);
    expect(unit.nu_Writes.lh_Head == &b1.ios2_Req.io_Message.mn_Node,
           "and is still on the unit's queue");
    expect(b1.ios2_Req.io_Message.mn_Node.ln_Succ ==
           (struct Node *)&unit.nu_Writes.lh_Tail,
           "as the only entry left");

    expect_u32("Disable balanced", (unsigned long)disable_depth, 0);

    /* Dropping again finds nothing, and an empty queue is not a special
       case that walks off the end. */
    replies = 0;
    netdev_drop_writes(&unit, &op_a);
    expect_u32("a second drop finds nothing", (unsigned long)replies, 0);

    netdev_drop_writes(&unit, &op_b);
    expect_u32("the other opener's drop takes its own",
               (unsigned long)replies, 1);
    expect(unit.nu_Writes.lh_Head->ln_Succ == NULL,
           "and the queue is empty");
}

int main(void)
{
    a_no_groups_is_an_empty_hash();
    b_a_joined_group_sets_its_own_bit();
    c_a_released_slot_is_not_a_member();
    d_promiscuous_and_allmulti_accept_everything();
    e_the_whole_table_is_hashed();
    f_reply();
    g_drop_writes_takes_only_one_openers();

    if (failures != 0)
    {
        printf("netdev_unit: %d of %d checks failed\n", failures, checks);
        return 1;
    }

    printf("netdev_unit: %d checks ok\n", checks);

    return 0;
}

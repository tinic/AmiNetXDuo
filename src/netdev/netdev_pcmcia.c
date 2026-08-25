/*
 * anxnet.device: claiming the A600/A1200 PCMCIA slot through card.resource.
 *
 * SPDX-License-Identifier: MIT
 */

#include "netdev_internal.h"
#include "netdev_cards.h"
#include "netdev_clock.h"
#include "netdev_macgen.h"
#include "el3.h"        /* el3_answers(), and no EtherLink III register */

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/nodes.h>
#include <exec/libraries.h>
#include <exec/tasks.h>
#include <resources/card.h>

#include <proto/exec.h>

struct Library *CardResource;

/*
 * card.resource stubs: no proto header works on both toolchains.  LVOs:
 * OwnCard -0x06(a1), ReleaseCard -0x0c(a1,d0), CardMiscControl -0x30(a1,d1),
 * CopyTuple -0x48(a1,a0,d1,d0).
 */

static struct CardHandle *pc_own_card(struct CardHandle *h)
{
    register struct Library    *_a6 __asm("a6") = CardResource;
    register struct CardHandle *_a1 __asm("a1") = h;
    register struct CardHandle *res __asm("d0");

    __asm __volatile ("jsr a6@(-0x6)"
                      : "=r" (res)
                      : "r" (_a6), "r" (_a1)
                      : "d1", "a0", "cc", "memory");

    return res;
}

static VOID pc_release_card(struct CardHandle *h, ULONG flags)
{
    register struct Library    *_a6 __asm("a6") = CardResource;
    register struct CardHandle *_a1 __asm("a1") = h;
    register ULONG              _d0 __asm("d0") = flags;

    __asm __volatile ("jsr a6@(-0xc)"
                      : "+r" (_d0)
                      : "r" (_a6), "r" (_a1)
                      : "d1", "a0", "cc", "memory");
}

static BOOL pc_reset_card(struct CardHandle *h)
{
    register struct Library    *_a6 __asm("a6") = CardResource;
    register struct CardHandle *_a1 __asm("a1") = h;
    register LONG               res __asm("d0");

    __asm __volatile ("jsr a6@(-0x42)"
                      : "=r" (res)
                      : "r" (_a6), "r" (_a1)
                      : "d1", "a0", "cc", "memory");

    return (BOOL)(res != 0);
}

static UBYTE pc_misc_control(struct CardHandle *h, UBYTE bits)
{
    register struct Library    *_a6 __asm("a6") = CardResource;
    register struct CardHandle *_a1 __asm("a1") = h;
    register ULONG              _d1 __asm("d1") = (ULONG)bits;
    register LONG               res __asm("d0");

    __asm __volatile ("jsr a6@(-0x30)"
                      : "=r" (res)
                      : "r" (_a6), "r" (_a1), "r" (_d1)
                      : "a0", "cc", "memory");

    return (UBYTE)res;
}

static BOOL pc_copy_tuple(struct CardHandle *h, UBYTE *buf, ULONG code,
                          ULONG size)
{
    register struct Library    *_a6 __asm("a6") = CardResource;
    register struct CardHandle *_a1 __asm("a1") = h;
    register UBYTE             *_a0 __asm("a0") = buf;
    register ULONG              _d1 __asm("d1") = code;
    register ULONG              _d0 __asm("d0") = size;
    register LONG               res __asm("d0");

    __asm __volatile ("jsr a6@(-0x48)"
                      : "=r" (res)
                      : "r" (_a6), "r" (_a1), "r" (_a0), "r" (_d1), "0" (_d0)
                      : "cc", "memory");

    return (BOOL)(res != 0);
}

#ifdef NETDEV_TRACE
static VOID pc_trace(const char *s, ULONG v)
{
    static const char hex[] = "0123456789abcdef";
    volatile UWORD   *serdat  = (volatile UWORD *)0xdff030;
    volatile UWORD   *serdatr = (volatile UWORD *)0xdff018;
    char              buf[12];
    const char       *p;
    int               i;

    for (i = 0; i < 8; i++)
        buf[i] = hex[(v >> ((7 - i) * 4)) & 0xf];
    buf[8] = '\r'; buf[9] = '\n'; buf[10] = '\0';

    for (p = s; *p != '\0'; p++)
    {
        ULONG guard = 200000;
        while ((*serdatr & 0x2000) == 0 && --guard != 0)
            ;
        *serdat = (UWORD)(0x100 | (UBYTE)*p);
    }
    for (p = buf; *p != '\0'; p++)
    {
        ULONG guard = 200000;
        while ((*serdatr & 0x2000) == 0 && --guard != 0)
            ;
        *serdat = (UWORD)(0x100 | (UBYTE)*p);
    }
}
#else
#define pc_trace(s, v)  ((VOID)0)
#endif

/* Attribute memory is byte-per-word: the card's byte n is at 2n. */
#define PC_ATTR_STRIDE      2

/* Card Information Structure tuples, PC Card standard, release 2. */
#define CISTPL_VERS_1       0x15
#define CISTPL_CONFIG       0x1a
#define CISTPL_CFTABLE      0x1b
#define CISTPL_MANFID       0x20
#define CISTPL_FUNCID       0x21
#define CISTPL_FUNCE        0x22
#define CIS_FUNC_LAN        6

#define CIS_FUNCE_LAN_NODE_ID   0x04

/* Configuration Option Register, offset from the configuration base the
   CISTPL_CONFIG tuple gives. */
#define PC_COR_OFF          0

static UBYTE pc_last_cr;

static BOOL pc_dp8390_answers(const NetdevCard *card)
{
    volatile UBYTE *cr =
        (volatile UBYTE *)(ULONG)(card->base + card->reg_off);
    UBYTE           v;

    *cr = 0x21;             /* ED_CR_STP | ED_CR_RD2 */
    v   = *cr;
    pc_trace("pc: cr ", (ULONG)v);
    pc_last_cr = v;
    /*
     * Mask the START bit: some clones (Netgear FA411) come out of reset with
     * CR bit 1 stuck and read 0x23 where the datasheet says 0x21.  0xff (bus
     * floating) and 0x00 (nothing decoding) still fail the comparison.
     */
    return (BOOL)((v & (UBYTE)~0x02u) == 0x21);
}

static BOOL pc_chip_answers(const NetdevCard *card)
{
    if (card->chip == NETDEV_CHIP_EL3)
        return el3_answers(card);

    return pc_dp8390_answers(card);
}

/*
 * No timer exists at romtag-init time, so the wait spins on attribute-memory
 * reads.  Each read is a real Gayle cycle that paces the socket: the read must
 * stay in the loop body.  The count argument is a floor, not the duration.
 */
static VOID pc_settle(ULONG us)
{
    volatile UBYTE *attr = (volatile UBYTE *)0x00a00000UL;
    NetdevWait      w;

    netdev_wait_begin(&w, us, us * 4u);

    do
        (VOID)*attr;
    while (!netdev_wait_done(&w));
}

/* 20 rounds of 2 ms, which is Linux's 40 ms for the same wait. */
#define PC_SETTLE_ROUNDS    20
#define PC_SETTLE_US        2000

static BOOL pc_chip_settles(const NetdevCard *card, UWORD *rounds)
{
    UWORD i;

    for (i = 0; i < PC_SETTLE_ROUNDS; i++)
    {
        if (pc_chip_answers(card))
        {
            *rounds = i;
            return TRUE;
        }
        pc_settle(PC_SETTLE_US);
    }

    *rounds = i;

    return FALSE;
}

static UBYTE pc_cis[80];
static UWORD pc_cis_len;
static UBYTE pc_node_id[NETDEV_ADDR_LEN];
static BOOL  pc_have_node_id;

static VOID pc_cis_keep(const UBYTE *buf, UWORD len)
{
    UWORD i;

    for (i = 0; i < len && pc_cis_len < (UWORD)sizeof(pc_cis); i++)
        pc_cis[pc_cis_len++] = buf[i];
}

/* CopyTuple() writes two header bytes then min(size, TPL_LINK) body bytes, so
   the buffer must be size + 2.  The autodoc asks for size + 8, used here. */
#define PC_TUPLE_BODY   56
#define PC_TUPLE_BUF    (PC_TUPLE_BODY + 8)

/* Read one tuple, keep it for the fingerprint, and hand it back.  buf is
   PC_TUPLE_BUF bytes.  CopyTuple() does not clear beyond the tuple body, so
   this clears the buffer and returns the clamped body length. */
static BOOL pc_cis_read(struct CardHandle *h, UBYTE code, UBYTE *buf,
                        UWORD *body_len)
{
    UWORD i;
    UWORD keep;

    for (i = 0; i < (UWORD)PC_TUPLE_BUF; i++)
        buf[i] = 0;
    if (body_len != NULL)
        *body_len = 0;

    if (!pc_copy_tuple(h, buf, (ULONG)code, (ULONG)PC_TUPLE_BODY))
        return FALSE;

    /* buf[1] is TPL_LINK, the body length.  Clamped to what was asked for and
       not to the buffer: the link can say more than CopyTuple() copied. */
    keep = (UWORD)buf[1];
    if (keep > (UWORD)PC_TUPLE_BODY)
        keep = (UWORD)PC_TUPLE_BODY;
    pc_cis_keep(buf + 2, keep);
    if (body_len != NULL)
        *body_len = keep;

    return TRUE;
}

UWORD netdev_pcmcia_fingerprint(UBYTE *buf, UWORD max)
{
    UWORD i;

    for (i = 0; i < pc_cis_len && i < max; i++)
        buf[i] = pc_cis[i];

    return i;
}

BOOL netdev_mac_cis_node_id(UBYTE *mac)
{
    UWORD i;

    if (!pc_have_node_id)
        return FALSE;

    for (i = 0; i < NETDEV_ADDR_LEN; i++)
        mac[i] = pc_node_id[i];

    return TRUE;
}

/* The handle stays owned for as long as the unit does: a release powers the
   socket down under a driver that is still using it. */
static struct CardHandle pc_handle;
static struct Interrupt  pc_removed;
static struct Interrupt  pc_inserted;
static struct Interrupt  pc_status;

/* One slot, one owner and one worker.  The callbacks only change the volatile
   half below; the worker owns every card.resource call after OwnCard(). */
#define PC_WORKER_STACK_SIZE  8192UL
#define PC_WORKER_SIGNAL      SIGF_SINGLE

static NetdevDevice     *pc_dev;
static NetdevUnit       *pc_unit;
static const NetdevCard *pc_card;
static struct Task      *pc_worker;
static APTR              pc_worker_stack;

static volatile UBYTE pc_present;
static volatile UBYTE pc_owned;
static volatile UBYTE pc_linked;
static volatile UBYTE pc_used;
static volatile UBYTE pc_ready;
static volatile UBYTE pc_remove_pending;
static volatile UBYTE pc_insert_pending;
static volatile UBYTE pc_was_online;

static APTR pc_configure_owned(const NetdevCard **card_out, BOOL keep_handle);

static VOID pc_newlist(struct List *list)
{
    list->lh_Head     = (struct Node *)&list->lh_Tail;
    list->lh_Tail     = (struct Node *)0;
    list->lh_TailPred = (struct Node *)&list->lh_Head;
}

/* ReleaseCard() is task-only.  `removed` suppresses the reset which is both
   unnecessary and impossible once the socket has gone away. */
static VOID pc_release_owned(ULONG flags, BOOL removed)
{
    if (pc_linked == 0)
        return;

    if (!removed && pc_present != 0 && pc_used != 0)
        (VOID)pc_reset_card(&pc_handle);

    /* ReleaseCard() can hand a newly inserted card to this queued handle and
       run pc_on_inserted() before it returns.  Clear the old ownership first,
       or that callback's state is lost. */
    pc_owned = 0;
    pc_present = 0;
    pc_used  = 0;
    pc_ready = 0;
    pc_release_card(&pc_handle, flags);

    Disable();
    if ((flags & CARDF_REMOVEHANDLE) != 0)
    {
        pc_remove_pending = 0;
        pc_insert_pending = 0;
        pc_linked = 0;
        pc_handle.cah_CardNode.ln_Name = NULL;
    }
    Enable();
}

static VOID pc_reject_owned(BOOL keep_handle)
{
    BOOL removed = (BOOL)(pc_present == 0);

    pc_release_owned(keep_handle ? 0 : CARDF_REMOVEHANDLE, removed);
}

/* CardHandle callbacks receive is_Data in A1, not the CardHandle.  These use
   only file-static state so the structures can be installed before a unit has
   been built, closing the claim-to-bind removal window. */
static ULONG pc_on_removed(register APTR data __asm("a1"))
{
    NetdevUnit *unit = pc_unit;

    (VOID)data;
    pc_present        = 0;
    pc_ready          = 0;
    pc_remove_pending = 1;
    pc_insert_pending = 0;

    if (unit != NULL)
    {
        pc_was_online       = unit->nu_Online;
        unit->nu_Online     = 0;
        unit->nu_Nic.running = FALSE;
    }

    if (pc_worker != NULL)
        Signal(pc_worker, PC_WORKER_SIGNAL);

    return 0;
}

static ULONG pc_on_inserted(register APTR data __asm("a1"))
{
    (VOID)data;
    pc_present        = 1;
    pc_owned          = 1;
    pc_ready          = 0;
    pc_insert_pending = 1;

    if (pc_worker != NULL)
        Signal(pc_worker, PC_WORKER_SIGNAL);

    return 0;
}

static ULONG pc_on_status(register ULONG changes __asm("d0"),
                          register APTR data __asm("a1"))
{
    NetdevUnit *unit = pc_unit;

    (VOID)data;

    if (CardResource != NULL && CardResource->lib_Version >= 39)
    {
        /* First call: preserve the change mask so card.resource clears Gayle.
           The CARDF_POSTSTATUS call with D0 == 0 is the safe time to touch the
           controller, after the gate-array latch has been cleared. */
        if (changes != 0)
            return changes;
        if (pc_ready != 0 && pc_owned != 0 && pc_present != 0 && unit != NULL)
            {
                unit->nu_InIsr = 1;
                if (netdev_interrupt(unit) != 0)
                {
                    unit->nu_IntSeen++;
                    unit->nu_IntSilent = 0;
                }
                unit->nu_InIsr = 0;
            }
        return 0;
    }

    if (pc_ready != 0 && pc_owned != 0 && pc_present != 0 && unit != NULL)
        {
                unit->nu_InIsr = 1;
                if (netdev_interrupt(unit) != 0)
                {
                    unit->nu_IntSeen++;
                    unit->nu_IntSilent = 0;
                }
                unit->nu_InIsr = 0;
            }

    /* V37 has no post-status callback.  This is cnet.device's Gayle clear:
       acknowledge exactly the latched changes after draining the card. */
    *(volatile UBYTE *)0x00da9000UL =
        (UBYTE)(((UBYTE)changes ^ 0x2cu) | 0xc0u);
    return 0;
}

BOOL netdev_pcmcia_is_unit(const NetdevUnit *unit)
{
    return (BOOL)(unit != NULL && unit->nu_Nic.card != NULL &&
                  unit->nu_Nic.card->bus == NETDEV_BUS_PCMCIA);
}

BOOL netdev_pcmcia_available(const NetdevUnit *unit)
{
    return (BOOL)(!netdev_pcmcia_is_unit(unit) ||
                  (unit == pc_unit && pc_present != 0 && pc_owned != 0 &&
                   pc_ready != 0));
}

VOID netdev_pcmcia_cancel_resume(const NetdevUnit *unit)
{
    if (unit != NULL && unit == pc_unit)
        pc_was_online = 0;
}

static VOID pc_worker_entry(VOID)
{
    for (;;)
    {
        UBYTE removed;
        UBYTE inserted;

        (VOID)Wait(PC_WORKER_SIGNAL);

        for (;;)
        {
            Disable();
            removed  = pc_remove_pending;
            inserted = pc_insert_pending;
            pc_remove_pending = 0;
            pc_insert_pending = 0;
            Enable();

            if (removed == 0 && inserted == 0)
                break;
            if (pc_dev == NULL)
                continue;

            ObtainSemaphore(&pc_dev->nd_PcmciaLock);

            if (removed != 0)
            {
                if (pc_unit != NULL)
                    netdev_pcmcia_detached(
                        pc_unit,
                        S2EVENT_OFFLINE | S2EVENT_ERROR | S2EVENT_HARDWARE);
                if (pc_owned != 0)
                    pc_release_owned(0, TRUE); /* stay queued for reinsertion */
            }

            if (inserted != 0 && pc_owned != 0 && pc_present != 0)
            {
                const NetdevCard *card = NULL;
                APTR              base = pc_configure_owned(&card, TRUE);

                /* pc_configure_owned() releases the card itself on every
                   configuration failure, and may already have taken ownership
                   of a newer insertion.  Do not release again. */
                if (base == NULL)
                {
                    /* The next pending callback, if any, owns the retry. */
                }
                else if (pc_present == 0 || card != pc_card || pc_unit == NULL ||
                         !netdev_pcmcia_reattach(pc_unit, card, base))
                {
                    if (pc_owned != 0)
                        pc_reject_owned(TRUE);
                }
                else if (pc_present != 0)
                {
                    pc_ready = 1;
                    if (pc_was_online != 0 && pc_unit->nu_Openers != 0)
                        (VOID)netdev_online(pc_unit);
                }
            }

            ReleaseSemaphore(&pc_dev->nd_PcmciaLock);
        }
    }
}

static BOOL pc_worker_start(NetdevDevice *dev)
{
    struct MemList *memlist;
    struct Task    *task;

    if (pc_worker != NULL)
    {
        pc_dev = dev;
        return TRUE;
    }

    pc_worker_stack = AllocMem(PC_WORKER_STACK_SIZE, MEMF_PUBLIC | MEMF_CLEAR);
    if (pc_worker_stack == NULL)
        return FALSE;

    memlist = (struct MemList *)AllocMem(sizeof(struct MemList),
                                         MEMF_PUBLIC | MEMF_CLEAR);
    task = (struct Task *)AllocMem(sizeof(struct Task), MEMF_PUBLIC | MEMF_CLEAR);
    if (memlist == NULL || task == NULL)
    {
        if (task != NULL)
            FreeMem(task, sizeof(struct Task));
        if (memlist != NULL)
            FreeMem(memlist, sizeof(struct MemList));
        FreeMem(pc_worker_stack, PC_WORKER_STACK_SIZE);
        pc_worker_stack = NULL;
        return FALSE;
    }

    memlist->ml_NumEntries      = 1;
    memlist->ml_ME[0].me_Addr   = task;
    memlist->ml_ME[0].me_Length = sizeof(struct Task);

    task->tc_Node.ln_Type = NT_TASK;
    /* ReleaseCard() must run before card.resource can notify anybody about the
       next insertion, so this must not be a negative-priority task that a
       CPU-bound application can starve. */
    task->tc_Node.ln_Pri  = 0;
    task->tc_Node.ln_Name = (char *)"anxnet pcmcia";
    task->tc_SPLower      = pc_worker_stack;
    task->tc_SPUpper      = (APTR)((UBYTE *)pc_worker_stack +
                                   PC_WORKER_STACK_SIZE);
    task->tc_SPReg        = task->tc_SPUpper;
    pc_newlist(&task->tc_MemEntry);
    AddTail(&task->tc_MemEntry, (struct Node *)memlist);

    pc_dev = dev;
    pc_worker = task;
    if (AddTask(task, (APTR)pc_worker_entry, (APTR)0) == NULL)
    {
        pc_worker = NULL;
        pc_dev = NULL;
        FreeMem(task, sizeof(struct Task));
        FreeMem(memlist, sizeof(struct MemList));
        FreeMem(pc_worker_stack, PC_WORKER_STACK_SIZE);
        pc_worker_stack = NULL;
        return FALSE;
    }
    return TRUE;
}

static VOID pc_worker_stop(VOID)
{
    struct Task *task;
    APTR         stack;

    Forbid();
    task      = pc_worker;
    stack     = pc_worker_stack;
    pc_worker = NULL;
    pc_worker_stack = NULL;
    if (task != NULL)
        RemTask(task);
    Permit();

    if (stack != NULL)
        FreeMem(stack, PC_WORKER_STACK_SIZE);
}

/* The probe calls this once the unit the slot belongs to exists. */
VOID netdev_pcmcia_bind(NetdevUnit *unit)
{
    pc_unit = unit;
    pc_card = (unit != NULL) ? unit->nu_Nic.card : NULL;
    pc_ready = (UBYTE)(unit != NULL && pc_present != 0 && pc_owned != 0);
    if (pc_remove_pending != 0 && pc_worker != NULL)
        Signal(pc_worker, PC_WORKER_SIGNAL);
}

/*
 * Own the slot, read the CIS, let the CIS choose the row, configure for that
 * row's chip.  One slot per machine: pc_handle and pc_unit are single
 * file-statics, so a second call would overwrite a live handle.
 */
APTR netdev_pcmcia_claim(NetdevDevice *dev, const NetdevCard **card_out)
{
    struct CardHandle *handle = &pc_handle;
    struct CardHandle *owner;
    APTR               base;
    UWORD              ci = ANXDIAG_NOCARD;

    if (CardResource == NULL)
        CardResource = OpenResource((STRPTR)CARDRESNAME);

    pc_trace("pc: resource ", (ULONG)CardResource);
    netdev_diag_note(ANXDIAG_PC_RESOURCE, ci, (ULONG)CardResource);
    if (CardResource == NULL || pc_linked != 0)
        return NULL;

    if (!pc_worker_start(dev))
        return NULL;

    pc_removed.is_Node.ln_Type = NT_INTERRUPT;
    pc_removed.is_Node.ln_Pri  = 0;
    pc_removed.is_Node.ln_Name = (char *)"anxnet.device removed";
    pc_removed.is_Data         = NULL;
    pc_removed.is_Code         = (VOID (*)())pc_on_removed;

    pc_inserted.is_Node.ln_Type = NT_INTERRUPT;
    pc_inserted.is_Node.ln_Pri  = 0;
    pc_inserted.is_Node.ln_Name = (char *)"anxnet.device inserted";
    pc_inserted.is_Data         = NULL;
    pc_inserted.is_Code         = (VOID (*)())pc_on_inserted;

    pc_status.is_Node.ln_Type = NT_INTERRUPT;
    pc_status.is_Node.ln_Pri  = 0;
    pc_status.is_Node.ln_Name = (char *)"anxnet.device status";
    pc_status.is_Data         = NULL;
    pc_status.is_Code         = (VOID (*)())pc_on_status;

    /* IFAVAILABLE prevents an empty-slot probe from becoming a latent owner.
       V39's second callback services the chip only after Gayle's latch is
       clear; V37 is handled explicitly in pc_on_status(). */
    handle->cah_CardNode.ln_Type = 0;
    handle->cah_CardNode.ln_Name = (char *)"anxnet.device";
    handle->cah_CardNode.ln_Pri  = 0;
    handle->cah_CardFlags = (UBYTE)(CARDF_IFAVAILABLE |
        (CardResource->lib_Version >= 39 ? CARDF_POSTSTATUS : 0));
    handle->cah_CardRemoved  = &pc_removed;
    handle->cah_CardInserted = &pc_inserted;
    handle->cah_CardStatus   = &pc_status;

    /* Set the expected inserted state before OwnCard().  A removal callback
       may run after OwnCard has accepted the handle but before it returns;
       writing `present = 1` afterwards would erase that observation. */
    pc_present = 1;
    pc_owned   = 0;
    pc_used    = 0;
    pc_ready   = 0;
    pc_remove_pending = 0;
    pc_insert_pending = 0;

    owner = pc_own_card(handle);
    pc_trace("pc: own ", (ULONG)owner);
    netdev_diag_note(ANXDIAG_PC_OWN, ci, (ULONG)owner);
    if (owner != NULL)
    {
        /* IFAVAILABLE enqueued nothing, so ReleaseCard() would Remove() an
           unlinked node. */
        handle->cah_CardNode.ln_Name = NULL;
        pc_present = 0;
        pc_owned = 0;
        pc_worker_stop();
        pc_dev = NULL;
        return NULL;
    }

    pc_linked = 1;
    pc_owned  = 1;

    /*
     * Gayle never asserts CC_RESET to the slot at power-on or reset (the
     * documented, unfixed A1200 bug), so hold the slot's reset line through
     * Gayle's own latch: PC Cards require a reset time of 100-200 ms.
     */
    {
        volatile UBYTE *gayle_intreq = (volatile UBYTE *)0x00DA9000UL;

        netdev_diag_note(ANXDIAG_CLOCK, ci, netdev_clock_spins_per_line());
        netdev_diag_note(ANXDIAG_CLOCK_LINE, ci, netdev_clock_us_per_line());

        *gayle_intreq = 0xFF;            /* reset start */
        pc_settle(300000);               /* the hold the cards require */
        *gayle_intreq = 0xFC;            /* reset stop */

        pc_trace("pc: reset held ", 300UL);
        netdev_diag_note(ANXDIAG_PC_RESET, ci, 300UL);
        pc_settle(20000);                /* recovery before the CIS walk */
    }

    base = pc_configure_owned(card_out, FALSE);
    if (base == NULL && pc_linked == 0)
    {
        pc_worker_stop();
        pc_dev = NULL;
    }
    return base;
}

/* The insertion callback already owns the handle.  Reuse the same CIS and COR
   path without trying OwnCard() a second time.  keep_handle leaves us queued
   after a foreign or failed replacement so the next insertion is observed. */
static APTR pc_configure_owned(const NetdevCard **card_out, BOOL keep_handle)
{
    struct CardHandle *handle = &pc_handle;
    const NetdevCard  *card;
    UWORD              manf = 0;
    UWORD              prod = 0;
    /* Zeroed for the analyzer: CopyTuple() fills it through inline asm.  The
       returned tuple length, not these zeroes, decides which bytes are real. */
    UBYTE            buf[PC_TUPLE_BUF] = { 0 };
    volatile UBYTE  *attr;
    ULONG            cfg_base;
    UBYTE            index;
    UWORD            tuple_len;
    UWORD            ci = ANXDIAG_NOCARD;

    /* A second claim -- a card taken out and another put in -- must not read
       the first card's CIS. */
    pc_cis_len      = 0;
    pc_have_node_id = FALSE;

    /*
     * CISTPL_FUNCID is optional and real cards ship without it: absent means
     * assume LAN adapter, present must be 6.  CISTPL_CONFIG and
     * CISTPL_CFTABLE_ENTRY carry values that cannot be guessed: required.
     */
    if (!pc_cis_read(handle, CISTPL_FUNCID, buf, &tuple_len))
    {
        pc_trace("pc: no funcid, assume lan ", 0);
        netdev_diag_note(ANXDIAG_PC_FUNCID, ci, ANXDIAG_ABSENT);
    }
    else
    {
        if (tuple_len < 1u)
        {
            netdev_diag_note(ANXDIAG_PC_NOTLAN, ci, ANXDIAG_ABSENT);
            pc_reject_owned(keep_handle);
            return NULL;
        }
        pc_trace("pc: funcid ", (ULONG)buf[2]);
        netdev_diag_note(ANXDIAG_PC_FUNCID, ci, (ULONG)buf[2]);
        if (buf[2] != CIS_FUNC_LAN)
        {
            netdev_diag_note(ANXDIAG_PC_NOTLAN, ci, (ULONG)buf[2]);
            pc_reject_owned(keep_handle);
            return NULL;
        }
    }

    /* A MANFID that is present but truncated is not an absent identity:
       accepting it as one could select the generic NE2000 row for another
       chip. */
    if (pc_cis_read(handle, CISTPL_MANFID, buf, &tuple_len))
    {
        if (tuple_len < 4u)
        {
            netdev_diag_note(ANXDIAG_PC_MANFID, ci, ANXDIAG_ABSENT);
            pc_reject_owned(keep_handle);
            return NULL;
        }
        /* Little-endian words, which is how every tuple carries a number. */
        manf = (UWORD)(((UWORD)buf[3] << 8) | (UWORD)buf[2]);
        prod = (UWORD)(((UWORD)buf[5] << 8) | (UWORD)buf[4]);
        pc_trace("pc: manfid ", (ULONG)manf);
        netdev_diag_note(ANXDIAG_PC_MANFID, ci,
                         ((ULONG)manf << 16) | (ULONG)prod);
    }
    else
    {
        netdev_diag_note(ANXDIAG_PC_MANFID, ci, ANXDIAG_ABSENT);
    }
    (VOID)pc_cis_read(handle, CISTPL_VERS_1, buf, NULL);

    if (pc_cis_read(handle, CISTPL_FUNCE, buf, &tuple_len))
    {
        pc_trace("pc: funce ",
                 (ULONG)(tuple_len >= 1u ? buf[2] : ANXDIAG_ABSENT));
        netdev_diag_note(ANXDIAG_PC_FUNCE, ci,
                         (ULONG)(tuple_len >= 1u ? buf[2] : ANXDIAG_ABSENT));
        if (tuple_len >= 8u && buf[2] == CIS_FUNCE_LAN_NODE_ID &&
            buf[3] == (UBYTE)NETDEV_ADDR_LEN)
        {
            UWORD i;

            for (i = 0; i < NETDEV_ADDR_LEN; i++)
                pc_node_id[i] = buf[4 + i];
            pc_have_node_id = netdev_mac_usable(pc_node_id);
            pc_trace("pc: cis node id ", ((ULONG)pc_node_id[2] << 24) |
                                         ((ULONG)pc_node_id[3] << 16) |
                                         ((ULONG)pc_node_id[4] << 8) |
                                         (ULONG)pc_node_id[5]);
            netdev_diag_note(ANXDIAG_PC_NODEID, ci,
                             (ULONG)(pc_have_node_id ? 1u : 0u));
        }
    }
    else
    {
        netdev_diag_note(ANXDIAG_PC_FUNCE, ci, ANXDIAG_ABSENT);
    }

    /* CISTPL_CONFIG carries the configuration register base, in the card's
       own attribute address space.  TPCC_SZ says how many bytes of base
       follow it, and the low two bits are that count minus one. */
    if (!pc_cis_read(handle, CISTPL_CONFIG, buf, &tuple_len))
    {
        pc_trace("pc: no config tuple ", 0);
        netdev_diag_note(ANXDIAG_PC_NOCONFIG, ci, 0);
        pc_reject_owned(keep_handle);
        return NULL;
    }
    {
        UBYTE  nbytes;
        UBYTE  i;

        if (tuple_len < 1u)
        {
            netdev_diag_note(ANXDIAG_PC_NOCONFIG, ci, 0);
            pc_reject_owned(keep_handle);
            return NULL;
        }

        nbytes = (UBYTE)((buf[2] & 0x03) + 1);
        if (tuple_len < (UWORD)(2u + nbytes))
        {
            netdev_diag_note(ANXDIAG_PC_NOCONFIG, ci, (ULONG)tuple_len);
            pc_reject_owned(keep_handle);
            return NULL;
        }

        cfg_base = 0;
        for (i = 0; i < nbytes && i < 4; i++)
            cfg_base |= ((ULONG)buf[4 + i]) << (8 * i);

        /* Attribute memory is 128 KB and TPCC_SZ can claim four address bytes,
           so clamp: an unusual CIS could otherwise put the COR write past the
           window and into the card's own I/O space at 0xA20000. */
        cfg_base &= 0x0001ffffUL;
    }

    /* CISTPL_CFTABLE_ENTRY's first byte holds the configuration index in its
       low six bits.  The first entry is the one to take: a LAN card's first
       entry is its I/O configuration. */
    pc_trace("pc: cfgbase ", cfg_base);
    netdev_diag_note(ANXDIAG_PC_CFGBASE, ci, cfg_base);
    if (!pc_cis_read(handle, CISTPL_CFTABLE, buf, &tuple_len) ||
        tuple_len < 1u)
    {
        pc_trace("pc: no cftable ", 0);
        netdev_diag_note(ANXDIAG_PC_NOCFTABLE, ci, 0);
        pc_reject_owned(keep_handle);
        return NULL;
    }
    index = (UBYTE)(buf[2] & 0x3f);
    pc_trace("pc: index ", (ULONG)index);
    netdev_diag_note(ANXDIAG_PC_INDEX, ci, (ULONG)index);

    /* The rest of the entry is recorded, not parsed: the I/O descriptor needs
       a walk of the power and timing descriptors before it, and the row's
       register offset is assumed instead. */
    netdev_diag_note(ANXDIAG_PC_CFTABLE, ci,
                     ((ULONG)buf[2] << 24) | ((ULONG)buf[3] << 16) |
                     ((ULONG)buf[4] << 8) | (ULONG)buf[5]);

    card = netdev_card_by_cis(manf, prod);
    if (card == NULL)
    {
        netdev_diag_note(ANXDIAG_PC_NOROW, ci,
                         ((ULONG)manf << 16) | (ULONG)prod);
        pc_reject_owned(keep_handle);
        return NULL;            /* no PCMCIA row at all: nothing to drive it */
    }
    ci = netdev_diag_card(card);
    netdev_diag_note(ANXDIAG_PC_CARD, ci, (ULONG)ci);

    /*
     * The socket comes up as a memory socket, so both bits must be set before
     * anything is written to attribute memory.  Without CARD_DISABLEF_WP the
     * socket stays write-protected and swallows the COR write with no error;
     * CARD_ENABLEF_DIGAUDIO is what configures the socket for the I/O
     * interface.
     */
    {
        UBYTE got = pc_misc_control(handle,
                                    CARD_DISABLEF_WP | CARD_ENABLEF_DIGAUDIO);

        pc_trace("pc: iomode ", (ULONG)got);
        netdev_diag_note(ANXDIAG_PC_IOMODE, ci,
                         (ULONG)(CARD_DISABLEF_WP | CARD_ENABLEF_DIGAUDIO));
        /* The autodoc: a bit cleared in the return is a bit this machine does
           not support.  A socket that answers without CARD_DISABLEF_WP will
           swallow the COR write with no error anywhere. */
        netdev_diag_note(ANXDIAG_PC_MISC, ci, (ULONG)got);
    }

    /*
     * CopyTuple() has already undone attribute memory's byte-per-word doubling
     * for the bytes it handed back, so TPCC_RADR is added to 0xA00000 as-is.
     * The doubled address is a fallback, reached only when the undoubled write
     * failed to bring a chip up.
     */
    {
        UBYTE cor    = index;
        UWORD rounds = 0;

        pc_used = 1;
        attr = (volatile UBYTE *)(ULONG)(0x00a00000UL + cfg_base + PC_COR_OFF);
        *attr = cor;
        pc_trace("pc: cor ", (ULONG)(APTR)attr);
        netdev_diag_note(ANXDIAG_PC_COR, ci, (ULONG)(APTR)attr);
        netdev_diag_note(ANXDIAG_PC_CORVAL, ci, (ULONG)cor);

        if (!pc_chip_settles(card, &rounds))
        {
            netdev_diag_note(ANXDIAG_PC_CR, ci, (ULONG)pc_last_cr);
            netdev_diag_note(ANXDIAG_PC_SETTLE, ci, (ULONG)rounds);

            attr = (volatile UBYTE *)(ULONG)
                       (0x00a00000UL +
                        (((cfg_base + PC_COR_OFF) * PC_ATTR_STRIDE) &
                         0x0001ffffUL));
            *attr = cor;
            pc_trace("pc: cor doubled ", (ULONG)(APTR)attr);
            netdev_diag_note(ANXDIAG_PC_COR2, ci, (ULONG)(APTR)attr);

            if (!pc_chip_settles(card, &rounds))
            {
                pc_trace("pc: chip silent ", 0);
                netdev_diag_note(ANXDIAG_PC_CR2, ci, (ULONG)pc_last_cr);
                netdev_diag_note(ANXDIAG_PC_SETTLE, ci, (ULONG)rounds);
                netdev_diag_note(ANXDIAG_PC_SILENT, ci,
                                 (ULONG)(card->base + card->reg_off));
                pc_reject_owned(keep_handle);
                return NULL;
            }
            netdev_diag_note(ANXDIAG_PC_CR2, ci, (ULONG)pc_last_cr);
            netdev_diag_note(ANXDIAG_PC_SETTLE, ci, (ULONG)rounds);
        }
        else
        {
            netdev_diag_note(ANXDIAG_PC_CR, ci, (ULONG)pc_last_cr);
            netdev_diag_note(ANXDIAG_PC_SETTLE, ci, (ULONG)rounds);
        }
    }

    /*
     * No second CardMiscControl() call: it masks its argument with
     * WR|DIGAUDIO (0x0a) and writes the result out, so an interrupt-bit call
     * would write zero over the I/O-mode call above.  OwnCard()'s
     * ResetGayleRegs() has already enabled BSY/IRQ on V37 and V39 alike.
     */
    pc_trace("pc: status irq v ", (ULONG)CardResource->lib_Version);
    netdev_diag_note(ANXDIAG_PC_IRQMODE, ci,
                     (ULONG)CardResource->lib_Version);

    pc_trace("pc: claimed ", (ULONG)card->base);
    netdev_diag_note(ANXDIAG_PC_CLAIMED, ci,
                     (ULONG)(card->base + card->reg_off));

    *card_out = card;

    return (APTR)(ULONG)card->base;
}

VOID netdev_pcmcia_release(VOID)
{
    NetdevDevice *dev = pc_dev;

    if (dev != NULL)
        ObtainSemaphore(&dev->nd_PcmciaLock);

    /* CardResetCard() is required before handing a configured I/O card to the
       next owner.  ReleaseCard() then restores Gayle and REMOVEHANDLE ensures
       no callback points into an expunged device image. */
    if (CardResource != NULL && pc_linked != 0)
        pc_release_owned(CARDF_REMOVEHANDLE, (BOOL)(pc_present == 0));

    pc_unit   = NULL;
    pc_card   = NULL;
    pc_present = 0;
    pc_owned   = 0;
    pc_used    = 0;
    pc_ready   = 0;
    pc_worker_stop();
    pc_dev = NULL;

    if (dev != NULL)
        ReleaseSemaphore(&dev->nd_PcmciaLock);
}

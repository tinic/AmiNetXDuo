/*
 * anxnet.device: claiming the A600/A1200 PCMCIA slot.
 *
 * Every other card in the table announces itself through autoconfig and the
 * probe finds it by walking the ConfigDev list.  The PCMCIA slot announces
 * nothing.  What is there instead is card.resource, which owns the slot, and
 * a CIS in attribute memory describing what is plugged into it.
 *
 * Three steps a Zorro board does not need:
 *
 *   Own the slot.  OwnCard() keeps two drivers from both driving one card, and
 *   it also powers the socket up.  A driver that pokes Gayle directly works
 *   right up until the machine has a second PCMCIA driver.
 *
 *   Read the CIS.  A PCMCIA slot holds an SRAM card or an IDE adapter as
 *   readily as a network card, and driving the wrong one writes to somebody's
 *   disk.  CopyTuple() walks the tuple chain.  CISTPL_FUNCID says what the
 *   card is, and when a card carries one the answer must be 6, LAN adapter.
 *   It is optional, and a card that omits it is taken at the value
 *   cnet.device takes it at: a network card.
 *
 *   Configure it.  A PCMCIA card decodes nothing until its Configuration
 *   Option Register is written with an index from CISTPL_CFTABLE_ENTRY.  Until
 *   then 0xA20300 reads as bus noise, which this file must not report as "no
 *   card fitted".
 *
 * The station address comes from the chip, not from the CIS.  The core reads
 * the RTL8019's own PROM through remote DMA, the same as every other NE2000
 * row, and a card whose CIS disagreed with its chip would be the CIS that was
 * wrong.  The CIS is read for an address only when the PROM has nothing in it,
 * as pc_cis_read() below shows.  Its bytes are kept either way, as fingerprint
 * material for netdev_macgen.c.
 *
 * SPDX-License-Identifier: MIT
 */

#include "netdev_internal.h"
#include "netdev_cards.h"
#include "netdev_macgen.h"
#include "el3.h"        /* el3_answers(), and no EtherLink III register */

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/nodes.h>
#include <exec/libraries.h>
#include <exec/tasks.h>
#include <resources/card.h>

#include <proto/exec.h>

/* The resource base every stub below calls through. */
struct Library *CardResource;

/*
 * card.resource is called through stubs written here, not through the NDK's,
 * because the two toolchains this builds with disagree about the file names,
 * and each one's proto header is internally inconsistent:
 *
 *   pinned Linux NDK   inline/card.h    proto/card.h -> clib/card_protos.h,
 *                                       which it does not ship
 *   macOS amiga-gcc    inline/cardres.h proto/cardres.h -> clib/card_protos.h
 *                                       and inline/card.h, neither shipped
 *
 * There is no include that works on both.  They agree exactly on the four LVOs
 * and their registers, checked in both inline headers, so those are spelled
 * out below and neither proto header is needed.  resources/card.h is fine
 * everywhere: it is structures and constants, no calls.
 *
 *   OwnCard          -0x06  a1 handle                       -> d0 owner
 *   ReleaseCard      -0x0c  a1 handle, d0 flags             -> void
 *   CardMiscControl  -0x30  a1 handle, d1 bits              -> d0
 *   CopyTuple        -0x48  a1 handle, a0 buf, d1 code,
 *                           d0 size                         -> d0 BOOL
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
/* Local, because netdev_device.c's is static there and this runs before any
   of it.  Same channel: raw SERDAT, which needs nothing to be open. */
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

/* CISTPL_FUNCE subtuples for function 6.  0x04 is the one that carries the
   card's assigned station address. */
#define CIS_FUNCE_LAN_NODE_ID   0x04

/* Configuration Option Register, offset from the configuration base the
   CISTPL_CONFIG tuple gives. */
#define PC_COR_OFF          0

/*
 * Is a DP8390 decoding at the register base?
 *
 * Not by reading the command register.  It comes out of reset reading 0, and
 * an address nothing decodes also reads 0, so the two are indistinguishable.
 * Write it instead.  CR is read/write, STP|RD2 is the state the chip is
 * already in, and a window with nothing behind it does not remember what was
 * written to it.
 */
/*
 * What the command register last read back.  The probe record carries the byte
 * and not only the verdict: 0x00 is "nothing is decoding there at all", 0xff
 * is "the bus is floating", and 0x23 is "a chip answered with a stuck START
 * bit".  Those are three different cards to somebody holding one.
 */
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
     * The START bit is masked out of the comparison.  Some NE2000 clones come
     * out of reset with CR bit 1 stuck set and read back 0x23 where the
     * datasheet says 0x21.  cnet.device masks DSCM_START out of this same
     * comparison (cnetdevice.asm:3611-3615) and names "a buggy chip in the
     * Netgear FA411 (and maybe others)".  A demand for exactly 0x21 rejects
     * those cards as an empty slot.
     *
     * The float case still fails, which is what matters.  An unconfigured or
     * absent card reads 0xff, and 0xff with bit 1 masked off is 0xfd, not
     * 0x21.  An address nothing decodes reads 0x00, also not 0x21.
     */
    return (BOOL)((v & (UBYTE)~0x02u) == 0x21);
}

/*
 * Is the chip this row names decoding at the register base?
 *
 * The test must switch on the chip.  pc_dp8390_answers() writes 0x21 to the
 * byte at the register base and demands it back, which works only for a DP8390
 * command register.  On an EtherLink III that address is the low half of the
 * transmit FIFO, so the write pushes a byte into the transmitter and the read
 * returns whatever the receive FIFO holds.  It said "no card here" for every
 * card that was not an NE2000 clone.
 *
 * The EtherLink III's own test is in el3.c, beside the register definitions it
 * needs.  A third family adds an arm here and a function there.
 */
static BOOL pc_chip_answers(const NetdevCard *card)
{
    if (card->chip == NETDEV_CHIP_EL3)
        return el3_answers(card);

    return pc_dp8390_answers(card);
}

/*
 * A card can be busy for a while after its COR is written, because the write
 * is what configures it.  Linux waits 40 ms before touching the card's I/O
 * space.  This used to read the command register on the next instruction, so a
 * card that took any time to come up read as an empty slot.  Amiberry applies
 * the COR inside the store, so the wait costs nothing there.
 *
 * There is no timer at romtag-init time and a device cannot Delay(), so the
 * wait is a spin on attribute-memory reads.  Each is a real Gayle cycle at the
 * socket's default speed, 250 ns at the fastest, so the count is a lower bound
 * on the microseconds.  Attribute memory is the CIS and is safe to read
 * whatever is in the slot.
 */
static VOID pc_settle(ULONG us)
{
    volatile UBYTE *attr = (volatile UBYTE *)0x00a00000UL;
    ULONG           n    = us * 4u;

    while (n-- != 0)
        (VOID)*attr;
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

/*
 * The CIS is kept for two things, and probing is neither of them.
 *
 * A PCMCIA NE2000 clone whose address PROM is blank often still knows its own
 * address.  The PC Card standard puts it in CISTPL_FUNCE subtuple 4 for a LAN
 * function, and a card built for a PC driver that reads the CIS has no reason
 * to program the PROM as well.  That address is preferred over anything
 * derived, because it is the address the card was assigned.
 *
 * card.resource's CopyTuple() returns the first tuple with a given code, and a
 * LAN card emits several CISTPL_FUNCE tuples: technology, speed, media, node
 * ID and connector, in whatever order its author chose.  If the first one is
 * not the node ID, this finds nothing and records that.  A walk of the whole
 * chain belongs with the CFTABLE walk, and neither is here yet.
 *
 * Everything read is also kept as fingerprint bytes.  MANFID is the card model
 * and separates two models, not two cards.  VERS_1 is free-form strings, and
 * some cards put a lot or serial number in the third one, which is the only
 * per-card input a CIS reliably offers.
 */
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

/*
 * How many body bytes CopyTuple() is asked for, and how big the buffer must
 * be.
 *
 * CopyTuple() writes the two header bytes and then min(size, TPL_LINK) body
 * bytes after them, so the buffer must be size + 2.  The autodoc asks for
 * size + 8, and that margin is used here.  This used to pass the whole buffer
 * as the size, which overruns the frame by two bytes on any tuple whose body
 * reaches the buffer's length.  A real card's CISTPL_VERS_1 carries
 * manufacturer and product strings and often does.  Amiberry's emulated card
 * has a handful of short tuples and never gets near it.
 */
#define PC_TUPLE_BODY   56
#define PC_TUPLE_BUF    (PC_TUPLE_BODY + 8)

/* Read one tuple, keep it for the fingerprint, and hand it back.  buf is
   PC_TUPLE_BUF bytes.  At most PC_TUPLE_BODY + 2 of them are written. */
static BOOL pc_cis_read(struct CardHandle *h, UBYTE code, UBYTE *buf)
{
    UWORD keep;

    if (!pc_copy_tuple(h, buf, (ULONG)code, (ULONG)PC_TUPLE_BODY))
        return FALSE;

    /* buf[1] is TPL_LINK, the body length.  The two header bytes are not
       fingerprint material: the code is a constant and the length follows
       from the body.  Clamped to what was asked for, not to the buffer,
       because the link can say more than CopyTuple() copied. */
    keep = (UWORD)buf[1];
    if (keep > (UWORD)PC_TUPLE_BODY)
        keep = (UWORD)PC_TUPLE_BODY;
    pc_cis_keep(buf + 2, keep);

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

/*
 * Configure the card and hand back the register base, or NULL.  The handle
 * stays owned for as long as the unit does.  A release powers the socket down
 * under a driver that is still using it.
 */
/* One slot per machine, so the handle is here rather than threaded through
   the unit: there is nothing for a second one to point at. */
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
       run pc_on_inserted() before the call returns.  Clear the old ownership
       first so that callback state wins; clearing it afterwards loses the
       insertion and leaves the current card unconfigured until it is removed
       a second time. */
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
            (VOID)netdev_interrupt(unit);
        return 0;
    }

    if (pc_ready != 0 && pc_owned != 0 && pc_present != 0 && unit != NULL)
        (VOID)netdev_interrupt(unit);

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

                /* pc_configure_owned() releases a card itself on every
                   configuration failure.  Do not release again: that call
                   may already have delivered ownership of a newer insertion
                   to this handle. */
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
    /* ReleaseCard() is required before card.resource can notify anybody about
       the next insertion.  A negative-priority task can be starved forever by
       an ordinary CPU-bound application, so run at the normal task priority;
       the worker sleeps except for removal and insertion transactions. */
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
 * Claim once, then identify, then pick the core.
 *
 * This used to take a card row and be called once per matching row, which
 * could only work while there was exactly one PCMCIA row.  pc_handle and
 * pc_unit are single file-statics, because there is one slot in the machine
 * and nothing for a second handle to point at.  A second call overwrote the
 * first card's handle while card.resource still held it.
 *
 * There is one card in the slot and it names itself, so the order is: own the
 * slot, read the CIS, let the CIS choose the row, and configure for that row's
 * chip.  CISTPL_MANFID is the identity, and a 3C589 is 0x0101/0x0589.
 * netdev_card_by_cis() falls back to the row with no MANFID of its own, which
 * is the NE2000 clones, because those are a hundred manufacturer IDs for one
 * chip and cannot be listed.
 *
 * *card_out is set only on success.  Every step before the row is known is
 * recorded against the machine rather than a card, because there is no card
 * yet and a slot step filed under the wrong row misleads.
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
    /* Zeroed because CopyTuple() fills it through an inline asm the analyzer
       cannot see into, so every byte read out of it afterwards reads as
       uninitialized to -fanalyzer.  This is not the pre-zeroing the FUNCID
       check used to rely on: every pc_tuple() below has its return value
       checked, so nothing here mistakes a zero this line wrote for a byte the
       card supplied. */
    UBYTE            buf[PC_TUPLE_BUF] = { 0 };
    volatile UBYTE  *attr;
    ULONG            cfg_base;
    UBYTE            index;
    UWORD            ci = ANXDIAG_NOCARD;

    /* A second claim -- a card taken out and another put in -- must not read
       the first card's CIS. */
    pc_cis_len      = 0;
    pc_have_node_id = FALSE;

    /*
     * What is in the slot.  A card that names any function but LAN adapter is
     * given straight back, because an IDE adapter driven as an NE2000 is a
     * write to somebody's disk.
     *
     * A card that names nothing is not the same as a card that names a
     * function other than LAN.  CISTPL_FUNCID is optional in practice, and
     * real cards ship without it.  This used to pre-zero buf[2], throw
     * CopyTuple()'s result away, and then compare the zero it had written
     * itself against 6.  A card with no FUNCID tuple was therefore rejected as
     * function 0, multi-function, and the slot was given back with a working
     * card in it.  cnet.device assumes a network card when the tuple is absent
     * and enforces the value only when it is present (cnetdevice.asm:4682-4688,
     * "did we get one? if not assume it's a network card... (Neil
     * Cafferkey)").  That workaround exists because cards in the field do this.
     *
     * The two later tuples are not optional in the same way.  CISTPL_CONFIG
     * gives the address of the register that must be written, and
     * CISTPL_CFTABLE_ENTRY gives the value.  Neither can be guessed, so their
     * absence stays a rejection.
     */
    if (!pc_cis_read(handle, CISTPL_FUNCID, buf))
    {
        pc_trace("pc: no funcid, assume lan ", 0);
        netdev_diag_note(ANXDIAG_PC_FUNCID, ci, ANXDIAG_ABSENT);
    }
    else
    {
        pc_trace("pc: funcid ", (ULONG)buf[2]);
        netdev_diag_note(ANXDIAG_PC_FUNCID, ci, (ULONG)buf[2]);
        if (buf[2] != CIS_FUNC_LAN)
        {
            netdev_diag_note(ANXDIAG_PC_NOTLAN, ci, (ULONG)buf[2]);
            pc_reject_owned(keep_handle);
            return NULL;
        }
    }

    /*
     * The identity tuples, read for the fingerprint and for the node ID.
     * Neither is required and neither can fail the claim: a card that does
     * not carry them is a card whose address comes from its PROM, which is
     * every card that works today.
     */
    if (pc_cis_read(handle, CISTPL_MANFID, buf))
    {
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
    (VOID)pc_cis_read(handle, CISTPL_VERS_1, buf);

    if (pc_cis_read(handle, CISTPL_FUNCE, buf))
    {
        pc_trace("pc: funce ", (ULONG)buf[2]);
        netdev_diag_note(ANXDIAG_PC_FUNCE, ci, (ULONG)buf[2]);
        if (buf[2] == CIS_FUNCE_LAN_NODE_ID &&
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
    if (!pc_cis_read(handle, CISTPL_CONFIG, buf))
    {
        pc_trace("pc: no config tuple ", 0);
        netdev_diag_note(ANXDIAG_PC_NOCONFIG, ci, 0);
        pc_reject_owned(keep_handle);
        return NULL;
    }
    {
        UBYTE  nbytes = (UBYTE)((buf[2] & 0x03) + 1);
        UBYTE  i;

        cfg_base = 0;
        for (i = 0; i < nbytes && i < 4; i++)
            cfg_base |= ((ULONG)buf[4 + i]) << (8 * i);

        /* Attribute memory is 128 KB here and TPCC_SZ can claim four address
           bytes, so an unusual or damaged CIS can put the COR write past the
           window and into the card's own I/O space at 0xA20000.  cnet.device
           masks with $0001FFFF (cnetdevice.asm:4694).  This is that mask, and
           the value recorded below is the clamped one, because it is the one
           written. */
        cfg_base &= 0x0001ffffUL;
    }

    /* CISTPL_CFTABLE_ENTRY's first byte holds the configuration index in its
       low six bits.  The first entry is the one to take: a LAN card's first
       entry is its I/O configuration. */
    pc_trace("pc: cfgbase ", cfg_base);
    netdev_diag_note(ANXDIAG_PC_CFGBASE, ci, cfg_base);
    if (!pc_cis_read(handle, CISTPL_CFTABLE, buf))
    {
        pc_trace("pc: no cftable ", 0);
        netdev_diag_note(ANXDIAG_PC_NOCFTABLE, ci, 0);
        pc_reject_owned(keep_handle);
        return NULL;
    }
    index = (UBYTE)(buf[2] & 0x3f);
    pc_trace("pc: index ", (ULONG)index);
    netdev_diag_note(ANXDIAG_PC_INDEX, ci, (ULONG)index);

    /*
     * The rest of the entry, recorded and not parsed.
     *
     * It holds the I/O descriptor, which is the address the card was told to
     * decode at.  A proper read of it means a walk of the power and timing
     * descriptors that precede it, whose lengths are themselves encoded.  This
     * driver assumes the card row's register offset instead, which is the
     * assumption cnet.device makes and has been right about across a hundred
     * cards.  The bytes cost one probe step to record, and one report from a
     * card owner then settles whether the assumption holds for that card.
     */
    netdev_diag_note(ANXDIAG_PC_CFTABLE, ci,
                     ((ULONG)buf[2] << 24) | ((ULONG)buf[3] << 16) |
                     ((ULONG)buf[4] << 8) | (ULONG)buf[5]);

    /*
     * Which row drives this card.  Everything above was about the slot.  From
     * here on there is a card, so the steps carry its row index.
     */
    card = netdev_card_by_cis(manf, prod);
    if (card == NULL)
    {
        /* Recorded, because a silent return here reads in the report exactly
           like a claim that never ran. */
        netdev_diag_note(ANXDIAG_PC_NOROW, ci,
                         ((ULONG)manf << 16) | (ULONG)prod);
        pc_reject_owned(keep_handle);
        return NULL;            /* no PCMCIA row at all: nothing to drive it */
    }
    ci = netdev_diag_card(card);
    netdev_diag_note(ANXDIAG_PC_CARD, ci, (ULONG)ci);

    /*
     * The socket goes into I/O mode before the COR write, not after it.
     *
     * A socket comes up as a memory socket.  Two bits of CardMiscControl()
     * change that, and both must be set before anything is written to
     * attribute memory:
     *
     *   CARD_ENABLEF_DIGAUDIO  the autodoc says this "configures the socket
     *                          for the I/O interface", and that digital audio
     *                          must always be enabled for I/O cards.  The pin
     *                          it names is the one Gayle reuses for I/O.
     *
     *   CARD_DISABLEF_WP       turns off hardware write protection, "for I/O
     *                          cards lacking a write-enable line".  A card
     *                          with no such line leaves the socket reading
     *                          write-protected, and a write-protected socket
     *                          swallows the COR write with no error anywhere.
     *                          The card is never configured, the chip never
     *                          answers, and the driver reports an empty slot.
     *
     * cnet.device makes this same call, with these two bits, in this position
     * (cnetdevice.asm:4713-4715, "enable card I/O functions").  Ours used to
     * make one CardMiscControl() call, with the V39 interrupt bits, after the
     * COR write.  That was too late to help it, and on a V37 or V38
     * card.resource the bits mean nothing.
     */
    {
        UBYTE got = pc_misc_control(handle,
                                    CARD_DISABLEF_WP | CARD_ENABLEF_DIGAUDIO);

        pc_trace("pc: iomode ", (ULONG)got);
        netdev_diag_note(ANXDIAG_PC_IOMODE, ci,
                         (ULONG)(CARD_DISABLEF_WP | CARD_ENABLEF_DIGAUDIO));
        /* The autodoc: a bit cleared in the return is a bit this machine does
           not support.  A socket that answers without CARD_DISABLEF_WP will
           swallow the COR write with no error anywhere, and there is no other
           way to find that out. */
        netdev_diag_note(ANXDIAG_PC_MISC, ci, (ULONG)got);
    }

    /*
     * Write the COR, and check the card answered.
     *
     * The CIS does not settle which address the COR is at.  Attribute memory
     * is byte-per-word, so a card-space offset n is at 0xA00000 + 2n.  But
     * CISTPL_CONFIG's TPCC_RADR is written by whoever authored the CIS, and
     * card.resource's CopyTuple() has already undone the doubling for the
     * bytes it handed back.  What the tuple carries is an address in the
     * Amiga's attribute window, not one to double again.
     *
     * cnet.device has been run against a hundred real cards.  It adds
     * TPCC_RADR to $A00000, writes there, and does nothing else
     * (cnetdevice.asm:4705, 4718-4722).  That form goes first here.  Ours
     * doubled first, which put a stray byte into attribute memory at an
     * address some card is entitled to decode as one of its own registers,
     * before the checked write ever happened.
     *
     * The doubled address stays as a fallback, reached only when the undoubled
     * write has already failed to bring a chip up, so on a card that works it
     * is never written.  An unconfigured or absent card floats the bus and
     * reads 0xff.  A DP8390 in any state has bits clear in CR, which is what
     * pc_chip_answers() decides on.
     */
    {
        /* cnet.device and cnet16.device write the configuration index alone.
           COR bit 6 requests a level-mode PC Card interrupt, but Gayle reports
           the card through its latched status-change mechanism, not a PC-style
           shared IRQ line.  Adding the bit is not vendor parity and has never
           been validated on either of the cards this driver is meant to fix. */
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
     * The card IRQ uses the status-change callback installed in OwnCard().
     * There is deliberately no second CardMiscControl() call.
     *
     * There used to be one on V39 and later, carrying
     * CARD_INTF_SETCLR|CARD_INTF_IRQ to enable the BSY/IRQ status change.
     * CardMiscControl() masks its argument with WR|DIGAUDIO ($0a) and writes
     * the result to the Gayle status register outright.  It is not a set/clear
     * for those two bits, and the autodoc says so: "Finally to
     * reenable write protect, call this function with a mask value of 0."
     * CARD_INTF_SETCLR|CARD_INTF_IRQ is $84, and $84 & $0a is 0, so the call
     * wrote zero over the I/O-mode call above it.  The socket left I/O mode
     * and write protection came back on with the card configured and
     * answering, one step before the chip core read its first register.
     *
     * It also gained nothing.  OwnCard() calls ResetGayleRegs(), which ORs
     * GAYLEF_INT_BVD1|GAYLEF_INT_WR|GAYLEF_INT_BSYIRQ into gayleint, so
     * BSY/IRQ is already on when the slot arrives, on V37 and V39 alike.
     * cnet.device calls CardMiscControl() once, with the I/O-mode bits, and
     * never again (cnetdevice.asm:4713-4715).
     *
     * Amiberry runs the same Kickstart code and its PCMCIA decode does not
     * consult the write-protect or digital-audio bits, so none of this shows
     * there.
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

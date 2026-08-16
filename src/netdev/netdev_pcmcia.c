/*
 * anxnet.device: claiming the A600/A1200 PCMCIA slot.
 *
 * Every other card in the table announces itself through autoconfig and the
 * probe finds it by walking the ConfigDev list.  The PCMCIA slot announces
 * nothing.  What is there instead is card.resource, which owns the slot, and
 * a CIS in attribute memory describing what is plugged into it.
 *
 * WHAT THIS HAS TO DO THAT A ZORRO BOARD DOES NOT
 *
 *   Own the slot.  OwnCard() is how two drivers do not both drive one card,
 *   and it is also what powers the socket up.  A driver that pokes Gayle
 *   directly works right up until the machine has a second PCMCIA driver.
 *
 *   Read the CIS.  A PCMCIA slot holds an SRAM card or an IDE adapter just as
 *   readily as a network card, and driving the wrong one writes to somebody's
 *   disk.  CopyTuple() walks the tuple chain; CISTPL_FUNCID says what the card
 *   is, and when a card carries one the answer has to be 6, LAN adapter.
 *   It is optional, and a card that omits it is taken at the value
 *   cnet.device takes it at: a network card.
 *
 *   Configure it.  A PCMCIA card decodes nothing until its Configuration
 *   Option Register is written with an index from CISTPL_CFTABLE_ENTRY.  Until
 *   then 0xA20300 reads as bus noise, which is the failure mode this file
 *   exists to avoid reporting as "no card fitted".
 *
 * The station address comes from the chip, not from the CIS: the core reads
 * the RTL8019's own PROM through remote DMA, the same as every other NE2000
 * row, and a card whose CIS disagreed with its chip would be the CIS that was
 * wrong.  The CIS is read for one only when the PROM has nothing in it -- see
 * pc_cis_read() below -- and its bytes are kept either way, as fingerprint
 * material for netdev_macgen.c.
 *
 * SPDX-License-Identifier: MIT
 */

#include "netdev_internal.h"
#include "netdev_cards.h"
#include "netdev_macgen.h"
#include "el3.h"        /* el3_answers(), and no EtherLink III register */

#include <exec/types.h>
#include <exec/nodes.h>
#include <exec/libraries.h>
#include <resources/card.h>

#include <proto/exec.h>

/* The resource base every stub below calls through. */
struct Library *CardResource;

/*
 * CARD.RESOURCE IS CALLED THROUGH STUBS WRITTEN HERE, not through the NDK's,
 * because the two toolchains this builds with disagree about the file names
 * and each one's proto header is internally inconsistent:
 *
 *   pinned Linux NDK   inline/card.h    proto/card.h -> clib/card_protos.h,
 *                                       which it does not ship
 *   macOS amiga-gcc    inline/cardres.h proto/cardres.h -> clib/card_protos.h
 *                                       and inline/card.h, neither shipped
 *
 * There is no include that works on both.  What they DO agree on exactly is
 * the four LVOs and their registers, checked in both inline headers, so those
 * are spelled out below and neither proto header is needed.  resources/card.h
 * is fine everywhere: it is structures and constants, no calls.
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

/* Configuration Option Register, offset from the config base the CISTPL_CONFIG
   tuple gives.  Bit 6 is the level-mode interrupt select every LAN card wants. */
#define PC_COR_OFF          0
#define PC_COR_LEVEL_IRQ    0x40

/*
 * Is a DP8390 decoding at the register base?
 *
 * NOT by reading the command register: it comes out of reset reading 0 and an
 * address nothing decodes reads 0 too, so the two are indistinguishable --
 * which cost an evening.  Write it instead.  CR is read/write, STP|RD2 is the
 * state the chip is already in, and a window with nothing behind it does not
 * remember what was written to it.
 */
/*
 * What the command register last read back.  The probe record carries the
 * BYTE and not only the verdict: 0x00 is "nothing is decoding there at all",
 * 0xff is "the bus is floating", 0x23 is "a chip answered with a stuck START
 * bit", and those are three different cards to somebody holding one.
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
     * THE START BIT IS MASKED OUT OF THE COMPARISON.  Some NE2000 clones come
     * out of reset with CR bit 1 stuck set and read back 0x23 where the
     * datasheet says 0x21 -- cnet.device masks DSCM_START out of exactly this
     * comparison (cnetdevice.asm:3611-3615) and names "a buggy chip in the
     * Netgear FA411 (and maybe others)".  Demanding 0x21 exactly rejects
     * those cards as an empty slot.
     *
     * It does not weaken what this is deciding.  The float case is the one
     * that has to keep failing and it does: an unconfigured or absent card
     * reads 0xff, and 0xff with bit 1 masked off is 0xfd, not 0x21.  An
     * address nothing decodes at all reads 0x00, also not 0x21.
     */
    return (BOOL)((v & (UBYTE)~0x02u) == 0x21);
}

/*
 * IS THE CHIP THIS ROW NAMES DECODING AT THE REGISTER BASE?
 *
 * Chip-switched, and it has to be.  The test above writes 0x21 to the byte at
 * the register base and demands it back, which is a DP8390 command register
 * and nothing else: on an EtherLink III that address is the low half of the
 * transmit FIFO, so the write pushes a byte into the transmitter and the read
 * comes back as whatever the receive FIFO holds.  It said "no card here" for
 * every card that was not an NE2000 clone, which is why nothing but an NE2000
 * clone could ever be claimed.
 *
 * The EtherLink III's own test is in el3.c, beside the register definitions
 * it needs.  A third family adds an arm here and a function there.
 */
static BOOL pc_chip_answers(const NetdevCard *card)
{
    if (card->chip == NETDEV_CHIP_EL3)
        return el3_answers(card);

    return pc_dp8390_answers(card);
}

/*
 * A card may be busy for a while after its COR is written: the write is what
 * configures it.  Linux waits 40 ms before touching the card's I/O space; this
 * used to read the command register on the next instruction, so a card that
 * took any time to come up read as an empty slot.  Amiberry applies the COR
 * inside the store, so the wait costs nothing there and is invisible.
 *
 * There is no timer at romtag-init time and a device may not Delay(), so the
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
 * THE CIS, KEPT FOR TWO THINGS NEITHER OF WHICH IS PROBING.
 *
 * A PCMCIA NE2000 clone whose address PROM is blank often still knows its own
 * address: the PC Card standard puts it in CISTPL_FUNCE subtuple 4 for a LAN
 * function, and a card built for a PC driver that reads the CIS has no reason
 * to program the PROM as well.  That address is preferred over anything
 * derived, because it is the address the card was assigned.
 *
 * WHAT CANNOT BE DONE HERE.  card.resource's CopyTuple() returns the FIRST
 * tuple with a given code, and a LAN card emits several CISTPL_FUNCE tuples
 * -- technology, speed, media, node ID, connector -- in whatever order its
 * author chose.  If the first one is not the node ID this finds nothing and
 * says so; walking the whole chain is the CIS walk that belongs with the
 * CFTABLE walk, and neither is here yet.
 *
 * Everything read is also kept as fingerprint bytes.  MANFID is the card
 * model and separates two models, not two cards; VERS_1 is free-form strings
 * and some cards put a lot or serial number in the third one, which is the
 * only genuinely per-card input a CIS reliably offers.
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

/* Read one tuple, keep it for the fingerprint, and hand it back. */
static BOOL pc_cis_read(struct CardHandle *h, UBYTE code, UBYTE *buf, UWORD len)
{
    UWORD keep;

    if (!pc_copy_tuple(h, buf, (ULONG)code, (ULONG)len))
        return FALSE;

    /* buf[1] is TPL_LINK, the body length; the two header bytes are not
       fingerprint material -- the code is a constant and the length follows
       from the body. */
    keep = (UWORD)buf[1];
    if (keep > (UWORD)(len - 2u))
        keep = (UWORD)(len - 2u);
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
 * stays owned for as long as the unit does; releasing it powers the socket
 * down under a driver that is still using it.
 */
/* One slot per machine, so the handle is here rather than threaded through
   the unit: there is nothing for a second one to point at. */
static struct CardHandle pc_handle;

/*
 * CARD REMOVAL.  A PCMCIA card is the one card in the table that can leave
 * while the machine is running, and until this existed the driver went on
 * driving the empty socket: reads return bus noise, the chip never answers,
 * and every request waits for a timeout that means nothing.
 *
 * card.resource calls this at interrupt level with the handle in a1.  It must
 * not touch the card -- there isn't one -- so `running` is cleared FIRST and
 * netdev_offline() then finds ops->stop harmless and answers everything that
 * was queued with S2ERR_OUTOFSERVICE, which is what a caller can act on.
 */
static NetdevUnit *pc_unit;
static struct Interrupt pc_removed;

static ULONG pc_on_removed(register struct CardHandle *h __asm("a1"))
{
    NetdevUnit *unit = pc_unit;

    (VOID)h;

    if (unit != NULL)
    {
        unit->nu_Nic.running = FALSE;
        if (unit->nu_Online)
            netdev_offline(unit, S2EVENT_OFFLINE);
    }

    return 0;
}

/* The probe calls this once the unit the slot belongs to exists. */
VOID netdev_pcmcia_bind(NetdevUnit *unit)
{
    pc_unit = unit;
}

/*
 * GIVE THE SLOT BACK, AND TAKE THE HANDLE OUT OF THE RESOURCE WITH IT.
 *
 * ReleaseCard(handle, 0) drops ownership and leaves the handle enqueued, so
 * card.resource still holds a Node that lives in this driver's BSS: it hands
 * the slot to us again on the next insertion, with nothing of ours listening,
 * and after an expunge the Node is in freed memory.  CARDF_REMOVEHANDLE is
 * what takes it out, and it is what cnet.device passes on every give-up path
 * it has (cnetdevice.asm:963-964 for the OpenDevice error, :1325-1329 for the
 * expunge).  ln_Name is this file's own "the resource still has it" marker,
 * so it is cleared in the same place, once.
 */
static VOID pc_give_up(struct CardHandle *handle)
{
    pc_release_card(handle, CARDF_REMOVEHANDLE);
    handle->cah_CardNode.ln_Name = NULL;
}

/*
 * CLAIM ONCE, THEN IDENTIFY, THEN PICK THE CORE.
 *
 * This used to take a card row and be called once per matching row, which
 * could only ever work while there was exactly one PCMCIA row: pc_handle and
 * pc_unit are single file-statics -- there is one slot in the machine, so
 * there is nothing for a second handle to point at -- and a second call would
 * have overwritten the first card's handle while card.resource still held it.
 * The loop was therefore not "try each row", it was "try the first row and
 * then corrupt it".
 *
 * There is one card in the slot and it says what it is, so the shape that
 * works is the shape of the fact: own the slot, read the CIS, let the CIS
 * choose the row, and configure for that row's chip.  CISTPL_MANFID is the
 * identity -- a 3C589 is 0x0101/0x0589 -- and netdev_card_by_cis() falls back
 * to the row with no MANFID of its own, which is the NE2000 clones, because
 * those are a hundred manufacturer IDs for one chip and cannot be listed.
 *
 * *card_out is set only on success.  Every step before the row is known is
 * recorded against the MACHINE rather than a card: there is no card yet, and
 * a slot step filed under the wrong row is worse than one filed under none.
 */
APTR netdev_pcmcia_claim(const NetdevCard **card_out)
{
    struct CardHandle *handle = &pc_handle;
    const NetdevCard  *card;
    UWORD              manf = 0;
    UWORD              prod = 0;
    /* Zeroed because CopyTuple() fills it through an inline asm the analyzer
       cannot see into, so every byte read out of it afterwards reads as
       uninitialized to -fanalyzer.  This is NOT the pre-zeroing the FUNCID
       check used to rely on: every pc_tuple() below has its return value
       checked, so nothing here mistakes a zero this line wrote for a byte the
       card supplied. */
    UBYTE            buf[64] = { 0 };
    volatile UBYTE  *attr;
    ULONG            cfg_base;
    UBYTE            index;
    UWORD            ci = ANXDIAG_NOCARD;

    if (CardResource == NULL)
        CardResource = OpenResource((STRPTR)CARDRESNAME);

    pc_trace("pc: resource ", (ULONG)CardResource);
    netdev_diag_note(ANXDIAG_PC_RESOURCE, ci, (ULONG)CardResource);
    if (CardResource == NULL)
        return NULL;            /* no slot on this machine */

    /* A second claim -- a card taken out and another put in -- must not read
       the first card's CIS. */
    pc_cis_len      = 0;
    pc_have_node_id = FALSE;

    pc_removed.is_Node.ln_Type = NT_INTERRUPT;
    pc_removed.is_Node.ln_Pri  = 0;
    pc_removed.is_Node.ln_Name = (char *)"anxnet.device";
    pc_removed.is_Data         = NULL;
    pc_removed.is_Code         = (VOID (*)())pc_on_removed;

    /*
     * CARDF_IFAVAILABLE, AND THIS IS THE BIT THAT MATTERS TO SOMEBODY WHO
     * NEVER USES PCMCIA AT ALL.
     *
     * Without it, OwnCard() does not answer "no" -- it enqueues the handle
     * and grants the slot later, when a card appears or the present owner
     * lets go.  anxnet.device is opened once on an A600 or an A1200 with an
     * empty slot, the probe finds nothing and moves on, and the handle stays
     * in card.resource's list for the life of the machine.  From then on the
     * slot is ours the moment anything is plugged into it, with
     * cah_CardInserted NULL so nothing of ours ever notices, and cnet.device
     * -- or the CF IDE driver, or anything else -- is locked out until the
     * next reboot.  With the bit set, OwnCard() returns the current owner and
     * enqueues nothing, which is the only sensible thing for a driver that is
     * probing.
     *
     * cnet.device sets CARDF_IFAVAILABLE (cnetdevice.asm:4655-4665) for the
     * same reason, plus CARDF_POSTSTATUS on V39+, which is part of its
     * interrupt path and not of this one.
     */
    handle->cah_CardNode.ln_Name = (char *)"anxnet.device";
    handle->cah_CardNode.ln_Pri  = 0;
    handle->cah_CardFlags        = CARDF_IFAVAILABLE;
    handle->cah_CardRemoved      = &pc_removed;
    handle->cah_CardInserted     = NULL;
    handle->cah_CardStatus       = NULL;

    {
        struct CardHandle *owner = pc_own_card(handle);

        pc_trace("pc: own ", (ULONG)owner);
        netdev_diag_note(ANXDIAG_PC_OWN, ci, (ULONG)owner);
        if (owner != NULL)
        {
            /*
             * No ReleaseCard() here.  With CARDF_IFAVAILABLE a refused
             * OwnCard() enqueues nothing -- card.resource jumps the Enqueue
             * and returns the owner -- while ReleaseCard(CARDF_REMOVEHANDLE)
             * calls exec Remove() on the handle whether or not it is in a
             * list.  That unlinked a Node still zero from BSS and wrote
             * through its NULL ln_Pred, over address 0 and address 4.  The
             * machine that gets here is one whose slot another driver already
             * owns, which is where a probing driver most has to be harmless.
             * Clearing ln_Name is all that is owed: the resource is holding
             * nothing of ours.
             */
            handle->cah_CardNode.ln_Name = NULL;
            return NULL;
        }
    }

    /*
     * What is in the slot.  A card that SAYS it is anything but a LAN adapter
     * is given straight back: an IDE adapter driven as an NE2000 is a write to
     * somebody's disk.
     *
     * A CARD THAT SAYS NOTHING IS NOT THE SAME AS A CARD THAT SAYS "NOT LAN".
     * CISTPL_FUNCID is optional in practice and real cards ship without it.
     * This used to pre-zero buf[2], throw CopyTuple()'s result away, and then
     * compare the zero it had written itself against 6 -- so a card with no
     * FUNCID tuple was rejected as function 0, multi-function, and the slot
     * given back with the card in it working perfectly.  cnet.device assumes a
     * network card when the tuple is absent and only enforces the value when
     * it is present (cnetdevice.asm:4682-4688, "did we get one? if not assume
     * it's a network card... (Neil Cafferkey)"); that workaround exists
     * because cards in the field do this.
     *
     * The two later tuples are NOT optional in the same way: CISTPL_CONFIG
     * gives the address of the register that has to be written and
     * CISTPL_CFTABLE_ENTRY the value, and neither can be guessed.  Their
     * absence stays a rejection.
     */
    if (!pc_cis_read(handle, CISTPL_FUNCID, buf, sizeof(buf)))
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
            pc_give_up(handle);
            return NULL;
        }
    }

    /*
     * The identity tuples, read for the fingerprint and for the node ID.
     * Neither is required and neither can fail the claim: a card that does
     * not carry them is a card whose address comes from its PROM, which is
     * every card that works today.
     */
    if (pc_cis_read(handle, CISTPL_MANFID, buf, sizeof(buf)))
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
    (VOID)pc_cis_read(handle, CISTPL_VERS_1, buf, sizeof(buf));

    if (pc_cis_read(handle, CISTPL_FUNCE, buf, sizeof(buf)))
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
       follow it; the low two bits are that count minus one. */
    if (!pc_cis_read(handle, CISTPL_CONFIG, buf, sizeof(buf)))
    {
        pc_trace("pc: no config tuple ", 0);
        netdev_diag_note(ANXDIAG_PC_NOCONFIG, ci, 0);
        pc_give_up(handle);
        return NULL;
    }
    {
        UBYTE  nbytes = (UBYTE)((buf[2] & 0x03) + 1);
        UBYTE  i;

        cfg_base = 0;
        for (i = 0; i < nbytes && i < 4; i++)
            cfg_base |= ((ULONG)buf[4 + i]) << (8 * i);

        /* Attribute memory is 128 KB here and TPCC_SZ may claim four address
           bytes, so an unusual or damaged CIS can put the COR write past the
           window and into the card's own I/O space at 0xA20000.  cnet.device
           masks with $0001FFFF (cnetdevice.asm:4694); this is that mask, and
           the value recorded below is the clamped one because it is the one
           written. */
        cfg_base &= 0x0001ffffUL;
    }

    /* CISTPL_CFTABLE_ENTRY's first byte holds the configuration index in its
       low six bits.  The first entry is the one to take: a LAN card's first
       entry is its I/O configuration. */
    pc_trace("pc: cfgbase ", cfg_base);
    netdev_diag_note(ANXDIAG_PC_CFGBASE, ci, cfg_base);
    if (!pc_cis_read(handle, CISTPL_CFTABLE, buf, sizeof(buf)))
    {
        pc_trace("pc: no cftable ", 0);
        netdev_diag_note(ANXDIAG_PC_NOCFTABLE, ci, 0);
        pc_give_up(handle);
        return NULL;
    }
    index = (UBYTE)(buf[2] & 0x3f);
    pc_trace("pc: index ", (ULONG)index);
    netdev_diag_note(ANXDIAG_PC_INDEX, ci, (ULONG)index);

    /*
     * The rest of the entry, recorded and not parsed.
     *
     * What is in there is the I/O descriptor -- which address the card was
     * told to decode at -- and reading it properly means walking the power
     * and timing descriptors that precede it, whose lengths are themselves
     * encoded.  This driver assumes the card row's register offset instead,
     * the same assumption cnet.device makes and has been right about across a
     * hundred cards.  Recording the bytes costs one probe step and means a
     * single report from somebody holding a card settles whether the
     * assumption holds for theirs, rather than the question staying open.
     */
    netdev_diag_note(ANXDIAG_PC_CFTABLE, ci,
                     ((ULONG)buf[2] << 24) | ((ULONG)buf[3] << 16) |
                     ((ULONG)buf[4] << 8) | (ULONG)buf[5]);

    /*
     * WHICH ROW DRIVES THIS CARD.  Everything above was about the slot; from
     * here on there is a card, so the steps carry its row index.
     */
    card = netdev_card_by_cis(manf, prod);
    if (card == NULL)
    {
        pc_give_up(handle);
        return NULL;            /* no PCMCIA row at all: nothing to drive it */
    }
    ci = netdev_diag_card(card);
    netdev_diag_note(ANXDIAG_PC_CARD, ci, (ULONG)ci);

    /*
     * PUT THE SOCKET INTO I/O MODE, BEFORE THE COR WRITE AND NOT AFTER IT.
     *
     * A socket comes up as a MEMORY socket.  Two bits of CardMiscControl()
     * change that and both of them have to be set before anything is written
     * to attribute memory:
     *
     *   CARD_ENABLEF_DIGAUDIO  the autodoc's own words are that this
     *                          "configures the socket for the I/O interface"
     *                          and that you should ALWAYS try to enable
     *                          digital audio for I/O cards.  The pin it names
     *                          is the one Gayle reuses for I/O.
     *
     *   CARD_DISABLEF_WP       turns off hardware write protection, "for I/O
     *                          cards lacking a write-enable line".  A card
     *                          with no such line leaves the socket reading
     *                          write-protected, and a write-protected socket
     *                          swallows the COR write with no error anywhere:
     *                          the card is never configured, the chip never
     *                          answers, and the driver reports an empty slot.
     *
     * cnet.device does exactly this call, with exactly these two bits, in
     * exactly this position (cnetdevice.asm:4713-4715, "enable card I/O
     * functions").  Ours used to make one CardMiscControl() call, with the
     * V39 interrupt bits, AFTER the COR write -- too late to help it, and on
     * a V37 or V38 card.resource the bits mean nothing at all.
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
     * WHICH ADDRESS THE COR IS AT is not something the CIS settles.  Attribute
     * memory is byte-per-word, so a card-space offset n is at 0xA00000 + 2n --
     * but CISTPL_CONFIG's TPCC_RADR is written by whoever authored the CIS,
     * and card.resource's own CopyTuple() has already undone the doubling for
     * the bytes it handed back, so what the tuple carries is an address in the
     * Amiga's attribute window and not one to double again.
     *
     * cnet.device, which is the code that has been run against a hundred real
     * cards, adds TPCC_RADR to $A00000 and writes there, and does nothing else
     * (cnetdevice.asm:4705, 4718-4722).  That form goes first here.  Ours went
     * the other way round -- doubled first -- which put a stray byte into
     * attribute memory at an address some card is entitled to decode as one of
     * its own registers before the validated write ever happened.
     *
     * The doubled address stays as a fallback only, and is reached only when
     * the undoubled write has already failed to bring a chip up, so on a card
     * that works it is never written at all.  An unconfigured or absent card
     * floats the bus and reads 0xff; a DP8390 in any state has bits clear in
     * CR, which is what pc_chip_answers() is deciding.
     */
    {
        UBYTE cor    = (UBYTE)(index | PC_COR_LEVEL_IRQ);
        UWORD rounds = 0;

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
                pc_give_up(handle);
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
     * The card's interrupt is left at card.resource's default, and there is no
     * second CardMiscControl() call.
     *
     * There used to be one on V39 and later, carrying
     * CARD_INTF_SETCLR|CARD_INTF_IRQ to enable the BSY/IRQ status change.
     * CardMiscControl() masks its argument with WR|DIGAUDIO ($0a) and then
     * writes the result to the Gayle status register outright -- it is not a
     * set/clear for those two bits, and the autodoc says so: "Finally to
     * reenable write protect, call this function with a mask value of 0."
     * CARD_INTF_SETCLR|CARD_INTF_IRQ is $84, and $84 & $0a is 0, so the call
     * wrote zero over the I/O-mode call above it.  The socket left I/O mode
     * and write protection came back on with the card configured and
     * answering, one step before the chip core read its first register.
     *
     * It also bought nothing.  OwnCard() calls ResetGayleRegs(), which ORs
     * GAYLEF_INT_BVD1|GAYLEF_INT_WR|GAYLEF_INT_BSYIRQ into gayleint, so
     * BSY/IRQ is already on when the slot arrives, on V37 and V39 alike.
     * cnet.device calls CardMiscControl() once, with the I/O-mode bits, and
     * never again (cnetdevice.asm:4713-4715).
     *
     * Amiberry runs the same Kickstart code and its PCMCIA decode does not
     * consult the write-protect or digital-audio bits, so none of this shows
     * there.
     */
    pc_trace("pc: irq at resource default v ", (ULONG)CardResource->lib_Version);
    netdev_diag_note(ANXDIAG_PC_IRQSKIP, ci,
                     (ULONG)CardResource->lib_Version);

    pc_trace("pc: claimed ", (ULONG)card->base);
    netdev_diag_note(ANXDIAG_PC_CLAIMED, ci,
                     (ULONG)(card->base + card->reg_off));

    *card_out = card;

    return (APTR)(ULONG)card->base;
}

VOID netdev_pcmcia_release(VOID)
{
    struct CardHandle *handle = &pc_handle;

    pc_unit = NULL;

    /* No CardMiscControl() on the way out either: ReleaseCard() calls
       ResetGayleRegs() itself once the slot is free, which puts the status,
       change, control and interrupt registers back to their defaults.  The
       call that was here passed CARD_INTF_IRQ alone, which the resource masks
       to zero and writes to the status register -- the same clobber as the
       one on the claim path, one instruction before the release that would
       have done it properly. */
    if (CardResource != NULL && handle->cah_CardNode.ln_Name != NULL)
        pc_give_up(handle);
}

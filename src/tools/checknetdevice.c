/*
 * CheckNetDevice, say what anxnet.device found and what it refused.
 *
 * The driver publishes its probe record under a public semaphore whatever the
 * outcome; this reads it. Nothing on that path needs a unit, an open device, a
 * running stack or a filesystem.
 *
 * SPDX-License-Identifier: MIT
 */

#include "tools.h"

#include <exec/execbase.h>
#include <exec/memory.h>

#include "aminetxduo/anxdiag.h"
#include "aminetxduo/anxnet.h"

const char *const tool_name = "CheckNetDevice";

static const char version_tag[] __attribute__((used)) =
    TOOL_VERSTAG("CheckNetDevice");

#define TEMPLATE    "DEVICE/K,NOLOAD/S,RAW/S"

enum
{
    ARG_DEVICE = 0,
    ARG_NOLOAD,
    ARG_RAW,
    ARG_COUNT
};

/* ------------------------------------------------------------ the record --
 *
 * Read under Forbid() and copied whole: the driver removes the semaphore under
 * Forbid() before the memory can be freed. The semaphore is never obtained --
 * a diagnostic must not block on the driver that can be the broken thing.
 */
static AnxDiagMark cnd_mark;

#define CND_BAD_VERSION     1
#define CND_ABSENT          2
#define CND_OK              0

static UWORD cnd_read(VOID)
{
    const AnxDiagMark *mark;
    UWORD              status = CND_ABSENT;

    Forbid();

    /* (STRPTR): NDK 3.9 declares FindSemaphore(STRPTR), 3.2 CONST_STRPTR. */
    mark = (const AnxDiagMark *)FindSemaphore((STRPTR)ANXDIAG_NAME);

    if (mark != NULL && mark->ad_Magic == ANXDIAG_MAGIC)
    {
        if (mark->ad_Version == (UWORD)ANXDIAG_VERSION &&
            mark->ad_Size    == (UWORD)sizeof(AnxDiagMark))
        {
            cnd_mark = *mark;
            status   = CND_OK;
        }
        else
        {
            /* Keep the two fields the message needs and nothing else: the
               shapes disagree, so no other field can be believed. */
            cnd_mark.ad_Version = mark->ad_Version;
            cnd_mark.ad_Size    = mark->ad_Size;
            status = CND_BAD_VERSION;
        }
    }

    Permit();

    return status;
}

/* ------------------------------------------------------------- the names -- */

/* The card row name the driver itself carried, so this command never has to
   have been built from the same table.  Never NULL. */
static const char *cnd_card(UWORD card)
{
    if (card == (UWORD)ANXDIAG_NOCARD)
        return "";

    if (card >= cnd_mark.ad_Cards)
        return "?";

    return cnd_mark.ad_Name[card];
}

static const char *cnd_why(ULONG why)
{
    switch (why)
    {
    case ANXDIAG_WHY_CR:
        return "the command register did not read back as a stopped DP8390. "
               "Either nothing is decoding at that address, or what is there "
               "is not a DP8390";
    case ANXDIAG_WHY_ODD:
        return "the odd-numbered registers did not answer a read, as bytes or "
               "as words.  A chip is answering at the even addresses and not "
               "at the odd ones";
    case ANXDIAG_WHY_ODD_BNRY:
        return "the odd-numbered registers answered a read but did not hold "
               "a value written to them, as bytes or as words.  Half the "
               "register file is decoding and half is not";
    case ANXDIAG_WHY_BUFFER:
        return "a 32-byte pattern written through the data port did not read "
               "back, so the data port itself is wrong";
    case ANXDIAG_WHY_MEM:
        return "the data port works: a 32-byte pattern went through it and "
               "came back. A full pass over the 16 KB packet buffer did not, "
               "so the buffer RAM behind it is bad";
    case ANXDIAG_WHY_ADDRESS:
        return "the card offered no usable station address";
    case ANXDIAG_WHY_REGS:
        return "the register file did not answer a write followed by a read";
    case ANXDIAG_WHY_CSR:
        return "CSR0 did not read back as a stopped LANCE";
    case ANXDIAG_WHY_MFGID:
        return "the manufacturer ID read back as neither $6d50 nor $506d, so "
               "no EtherLink III is decoding at that address";
    case ANXDIAG_WHY_EEPROM:
        return "the card's EEPROM never reported itself ready, so no station "
               "address was read out of it";
    default:
        break;
    }

    return "the chip core refused it and did not say why";
}

static const char *cnd_macsource(ULONG src)
{
    switch (src)
    {
    case ANXDIAG_MAC_PROM:
        return "the card's address PROM";
    case ANXDIAG_MAC_PROM_FIXED:
        return "the address PROM (group bit cleared)";
    case ANXDIAG_MAC_CIS:
        return "the CIS (a LAN node ID)";
    case ANXDIAG_MAC_DERIVED:
        return "this machine: the PROM was blank";
    case ANXDIAG_MAC_SERIAL:
        return "the autoconfig serial number";
    default:
        break;
    }

    return "somewhere this command does not know about";
}

static const char *cnd_chip(ULONG chip)
{
    switch (chip)
    {
    case 0:  return "a DP8390 reached through a remote-DMA port (NE2000)";
    case 1:  return "a DP8390 with a memory-mapped packet buffer";
    case 2:  return "an Am7990 LANCE, which masters the bus itself";
    case 3:  return "a 3Com EtherLink III, windowed, with PIO FIFOs";
    default: break;
    }

    return "a chip this command does not know about";
}

static const char *cnd_dmode(ULONG mode)
{
    switch (mode)
    {
    case 0:  return "8-bit, one byte at a time";
    case 1:  return "16-bit through the word data port";
    case 2:  return "32-bit through the mirrored window";
    default: break;
    }

    return "a mode this command does not know about";
}

/* ------------------------------------------------------------ the report -- */

static VOID say(const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    VPrintf((CONST_STRPTR)fmt, (APTR)args);     /* (APTR): see tool_util.c */
    va_end(args);
}

/* A code this command has never heard of is printed as itself, not dropped. */
/* The chip of the card being printed, so ANXDIAG_ATTACH_OK does not claim a
   transfer mode for a part that has no data port.  Set by ANXDIAG_CHIP, which
   the driver records before it calls attach(). */
static UWORD cnd_chip_seen;

static VOID cnd_step(const AnxDiagStep *st)
{
    ULONG v = st->ds_Value;

    switch (st->ds_Code)
    {
    /* ---- the machine ---- */
    case ANXDIAG_START:
        say("  The probe began.  This driver knows %lu card types.\n", v);
        return;
    case ANXDIAG_EXPANSION:
        if (v == 0)
        {
            say("  expansion.library did not open.  Without it no Zorro board\n"
                "  can be found.  Only the PCMCIA slot and the fixed-address\n"
                "  cards were examined.\n");
            return;
        }
        say("  expansion.library opened at $%08lx.\n", v);
        return;
    case ANXDIAG_BOARDS:
        if (v == 0)
        {
            say("  There are no autoconfig boards in this machine at all.\n");
            return;
        }
        say("  %lu autoconfig board(s) on the bus were examined.\n", v);
        return;
    case ANXDIAG_NOMATCH:
        say("  A board with manufacturer %lu product %lu is not a card this\n"
            "  driver supports.  It was left alone.\n",
            (v >> 16) & 0xffffUL, v & 0xffUL);
        return;
    case ANXDIAG_UNITS_FULL:
        say("  This card is supported but there was no unit left for it:\n"
            "  the driver holds %lu at a time.\n", v);
        return;
    case ANXDIAG_DONE:
        say("  The probe finished.  %lu card(s) came up.\n", v);
        return;

    /* ---- one card ---- */
    case ANXDIAG_ZORRO_FOUND:
        say("  Found by autoconfig, board memory at $%08lx.\n", v);
        return;
    case ANXDIAG_FIXED_TRY:
        say("  Looked for at its fixed address $%08lx.  This card has no\n"
            "  autoconfig record, so it is probed rather than found.\n", v);
        return;
    case ANXDIAG_NO_CORE:
        say("  This driver has no chip core for chip type %lu, so the card\n"
            "  cannot be driven even though it was recognised.\n", v);
        return;
    case ANXDIAG_CR_READ:
        say("  Detection read the command register as $%02lx.\n", v);
        return;
    case ANXDIAG_ODD_RETRY:
        say("  Odd-numbered registers did not read as bytes, so the\n"
            "  word-read path some Fast-Ethernet clones need was tried.\n");
        return;
    case ANXDIAG_ODD_PLAIN:
    case ANXDIAG_ODD_WORD:
        say("  Odd registers read as %s: ISR $%02lx, and $5a/$a5 written to\n"
            "  BNRY came back $%02lx/$%02lx.  ISR must have bit 7 set after a\n"
            "  reset, and BNRY must return what was written.\n",
            (LONG)((st->ds_Code == (UWORD)ANXDIAG_ODD_PLAIN)
                       ? "bytes" : "words"),
            (v >> 16) & 0xffUL, (v >> 8) & 0xffUL, v & 0xffUL);
        return;
    case ANXDIAG_BUF_SEEN:
        say("  The first byte that came back wrong was at buffer offset\n"
            "  $%04lx: $%02lx was written there and $%02lx read back.\n",
            (v >> 16) & 0xffffUL, (v >> 8) & 0xffUL, v & 0xffUL);
        return;
    case ANXDIAG_ODDWIN:
        if (v == 0)
        {
            say("  This card's registers are one contiguous block.  It has no\n"
                "  separate window for the odd-numbered ones.\n");
            return;
        }
        say("  Odd-numbered registers are reached through a second window at\n"
            "  $%08lx, which is how Gayle splits PCMCIA I/O.\n", v);
        return;
    case ANXDIAG_CHIP:
        cnd_chip_seen = (UWORD)v;
        say("  The chip is %s.\n", (LONG)cnd_chip(v));
        return;
    case ANXDIAG_ATTACH_OK:
        /*
         * A LANCE has no data port, so it has no transfer mode: reporting
         * bus.dmode would be an invented fact.
         */
        if (cnd_chip_seen == 2 || cnd_chip_seen == 3)
        {
            say("  ATTACHED.\n");
            return;
        }
        say("  ATTACHED.  Packet data moves %s.\n", (LONG)cnd_dmode(v));
        return;
    case ANXDIAG_ATTACH_FAIL:
        say("  REFUSED, and this is why:\n");
        tool_wrap(4, cnd_why(v));
        return;
    case ANXDIAG_MAC_SOURCE:
        say("  The station address came from %s.\n", (LONG)cnd_macsource(v));
        return;
    case ANXDIAG_GETODD:
        if (v != 0)
        {
            say("  Odd-numbered registers are read as words.  This card only\n"
                "  decodes 16-bit I/O cycles, and the driver measured that\n"
                "  by itself.\n");
            return;
        }
        say("  Odd-numbered registers are read as bytes, which is normal.\n");
        return;
    case ANXDIAG_UNIT:
        say("  This card is unit %lu of anxnet.device.\n", v);
        return;

    /* ---- the PCMCIA slot ---- */
    case ANXDIAG_PC_RESOURCE:
        if (v == 0)
        {
            say("  There is no card.resource on this machine, so it has no\n"
                "  PCMCIA slot.\n");
            return;
        }
        say("  card.resource is at $%08lx: this machine has a PCMCIA slot.\n",
            v);
        return;
    case ANXDIAG_PC_OWN:
        if (v == 0)
        {
            say("  OwnCard() gave us the slot.\n");
            return;
        }
        if (v == 0xffffffffUL)
        {
            say("  OwnCard() answered -1: there is nothing in the slot.\n");
            return;
        }
        say("  OwnCard() refused: another driver already owns the slot, and\n"
            "  its CardHandle is at $%08lx.  Nothing further was tried.\n", v);
        return;
    case ANXDIAG_PC_FUNCID:
        if (v == ANXDIAG_ABSENT)
        {
            say("  The card has no CISTPL_FUNCID tuple.  Many real cards do\n"
                "  not have one.  It was assumed to be a network card.\n");
            return;
        }
        if (v == 6)
        {
            say("  CISTPL_FUNCID says function 6, a LAN adapter.\n");
            return;
        }
        say("  CISTPL_FUNCID says function %lu.\n", v);
        return;
    case ANXDIAG_PC_NOTLAN:
        say("  That is not a LAN adapter, so the slot was given straight\n"
            "  back.  Driving an IDE adapter as a network card writes to\n"
            "  the disk behind it.\n");
        return;
    case ANXDIAG_PC_MANFID:
        if (v == ANXDIAG_ABSENT)
        {
            say("  The card has no CISTPL_MANFID tuple, so it does not say\n"
                "  who made it.\n");
            return;
        }
        say("  CISTPL_MANFID: manufacturer $%04lx, product $%04lx.\n",
            (v >> 16) & 0xffffUL, v & 0xffffUL);
        return;
    case ANXDIAG_PC_FUNCE:
        if (v == ANXDIAG_ABSENT)
        {
            say("  The card has no CISTPL_FUNCE tuple, so its CIS carries no\n"
                "  station address.\n");
            return;
        }
        if (v == 4)
        {
            say("  CISTPL_FUNCE subtuple 4: the CIS carries a LAN node ID.\n");
            return;
        }
        say("  CISTPL_FUNCE subtuple %lu, which is not the node ID.  Only\n"
            "  the first FUNCE tuple can be read, so a node ID behind it\n"
            "  cannot be seen.\n", v);
        return;
    case ANXDIAG_PC_NODEID:
        if (v != 0)
        {
            say("  The node ID in the CIS is a usable station address.\n");
            return;
        }
        say("  The node ID in the CIS is not a usable station address and\n"
            "  was ignored.\n");
        return;
    case ANXDIAG_PC_NOCONFIG:
        say("  The card has no CISTPL_CONFIG tuple, so there is no way to\n"
            "  know which register configures it.  The slot was given back.\n");
        return;
    case ANXDIAG_PC_CFGBASE:
        say("  CISTPL_CONFIG puts the configuration registers at $%08lx in\n"
            "  the card's attribute memory.\n", v);
        return;
    case ANXDIAG_PC_NOCFTABLE:
        say("  The card has no CISTPL_CFTABLE_ENTRY tuple, so there is no\n"
            "  configuration index to write.  The slot was given back.\n");
        return;
    case ANXDIAG_PC_INDEX:
        say("  Configuration index %lu.  This is the byte written to the\n"
            "  card's Configuration Option Register, and it is what puts the\n"
            "  card into the configuration chosen above.\n", v);
        return;
    case ANXDIAG_PC_CFCOUNT:
        say("  The card describes %lu configuration table %s.  All of\n"
            "  them are read, not just the first: an entry can describe a\n"
            "  memory configuration, or an access width this driver cannot\n"
            "  use, and the next one then still works.\n",
            v, (ULONG)(APTR)(v == 1 ? "entry" : "entries"));
        return;
    case ANXDIAG_PC_CFPICK:
        if (v == ANXDIAG_ABSENT)
        {
            say("  None of those entries parsed into a configuration this\n"
                "  driver could name, so the first entry's index was written\n"
                "  anyway.  A card that does not answer after that has a CIS\n"
                "  this driver does not understand -- send the CIS bytes\n"
                "  printed above with the report.\n");
            return;
        }
        say("  Entry index %lu was chosen: it decodes %lu address line(s) and\n"
            "  its descriptor flags are $%02lx.\n",
            v & 0x3f, (v >> 8) & 0xff, (v >> 16) & 0xff);
        if (((v >> 24) & 0xffUL) == 1)
        {
            say("  It is the ONLY configuration the card offers, and it asks\n"
                "  for 16-bit accesses while refusing 8-bit ones.  Every\n"
                "  register path here is byte-wide, so a card that answers\n"
                "  nothing below is refusing the width, not the address.\n");
        }
        return;
    case ANXDIAG_PC_IOWIN:
        say("  Its I/O window is %lu byte(s) at $%04lx in the card's own I/O\n"
            "  space.\n", v & 0xffff, (v >> 16) & 0xffff);
        return;
    case ANXDIAG_PC_IOOFF:
        say("  The registers were looked for at offset $%04lx in the slot's\n"
            "  I/O space.  $0300 is the card row's assumption and holds for\n"
            "  any card that leaves its placement to the machine; anything\n"
            "  else came out of the entry above, which named its own base.\n",
            v);
        return;
    case ANXDIAG_PC_MFC:
        say("  The card is a MULTIFUNCTION card: its CIS has a\n"
            "  CISTPL_LONGLINK_MFC tuple naming %lu function chain(s).\n"
            "  card.resource's CopyTuple() follows CISTPL_LONGLINK_A and\n"
            "  CISTPL_LONGLINK_C and no other link, so the network function's\n"
            "  own configuration entries are not reachable through it and\n"
            "  everything read above came from the shared chain.  This card\n"
            "  cannot be configured by this driver.\n", v);
        return;
    case ANXDIAG_PC_IOMODE:
        say("  The socket was put into I/O mode (CardMiscControl $%02lx).\n",
            v);
        return;
    case ANXDIAG_PC_MISC:
        if ((v & 0x0aUL) == 0x0aUL)
        {
            say("  card.resource answered $%02lx: the socket took both bits,\n"
                "  so it is in I/O mode with hardware write protect off.\n", v);
            return;
        }
        say("  card.resource answered $%02lx, and a bit missing from that is\n"
            "  a bit this machine does not support.  Without $08 the socket\n"
            "  is still write-protected, and it accepts the write below\n"
            "  without an error.  Without $02 it is still a memory socket.\n",
            v);
        return;
    case ANXDIAG_PC_COR:
        say("  The configuration option register was written at $%08lx.\n", v);
        return;
    case ANXDIAG_PC_CORVAL:
        /* Bit 6 is the COR's level-mode interrupt request, left clear: Gayle
           reports the PC Card interrupt as an edge whatever the card asked
           for. Named here so the byte does not have to be looked up. */
        say("  The byte written there was $%02lx: configuration index %lu,\n"
            "  and bit 6 clear, so the card was not asked for a level-mode\n"
            "  interrupt.\n",
            v, v & 0x3fUL);
        return;
    case ANXDIAG_CLOCK:
        if (v == 0)
        {
            say("  No raster beam could be read on this machine, so the\n"
                "  driver's waits are counted loops rather than measured\n"
                "  time.  On anything faster than a stock CPU they will be\n"
                "  shorter than they are meant to be.\n");
            return;
        }
        say("  The delay clock measured %lu spin(s) per raster line.  Tens of\n"
            "  them is a stock CPU; tens of thousands is an accelerator, and\n"
            "  that ratio is the whole reason a wait here is timed against the\n"
            "  beam rather than counted in bus reads.  A counted loop measures\n"
            "  the CPU, not the time.\n", v);
        return;
    case ANXDIAG_CLOCK_LINE:
        if (v == 0)
        {
            say("  No beam, so no line to price -- see above.\n");
            return;
        }
        if (v == 63)
        {
            say("  A scan line was measured at 63 us, so this machine is in a\n"
                "  15 kHz PAL or NTSC mode.  Every wait in the driver is\n"
                "  counted in lines at that price.\n");
            return;
        }
        if (v == 31)
        {
            say("  A scan line was measured at 31 us, so this machine is in a\n"
                "  31 kHz multiscan mode.  Every wait in the driver is counted\n"
                "  in lines at that price.\n");
            return;
        }
        say("  A scan line was costed at %lu us because counting the lines in\n"
            "  a field did not give a length any Amiga display mode has.  The\n"
            "  beam still runs, so the waits are still measured, but they come\n"
            "  out about twice as long as they ask for.  Harmless, and worth\n"
            "  reporting: it means the field count is wrong on this machine.\n",
            v);
        return;
    case ANXDIAG_PC_SETTLE:
        if (v == 0)
        {
            say("  The chip answered immediately after that write.\n");
            return;
        }
        say("  The card was given %lu round(s) of 2 ms after that write\n"
            "  before it answered, or before the wait ran out.\n", v);
        return;
    case ANXDIAG_PC_CR:
        if (v == 0xffUL)
        {
            say("  The command register read back $ff: the bus is floating.\n"
                "  Nothing is decoding at that address.\n");
            return;
        }
        if (v == 0)
        {
            say("  The command register read back $00: nothing is decoding\n"
                "  at that address.\n");
            return;
        }
        if ((v & ~0x02UL) == 0x21UL)
        {
            say("  The command register read back $%02lx: a DP8390 answered.\n",
                v);
            return;
        }
        if (v == ANXDIAG_CR_DECOY)
        {
            say("  The command register read back $%02lx, which is the byte the\n"
                "  probe wrote at a DIFFERENT register just before reading it.\n"
                "  The socket is echoing whatever was last driven on the bus\n"
                "  rather than holding a value, so the slot is empty.\n", v);
            return;
        }
        say("  The command register read back $%02lx, which is not what a\n"
            "  stopped DP8390 answers ($21, or $23 on a clone with a stuck\n"
            "  START bit).\n", v);
        return;
    case ANXDIAG_PC_COR2:
        say("  Nothing answered, so the option register was written again at\n"
            "  $%08lx, in case this card's CIS meant a doubled address.\n", v);
        return;
    case ANXDIAG_PC_CR2:
        say("  After the second write the command register read back $%02lx.\n",
            v);
        return;
    case ANXDIAG_PC_SILENT:
        say("  No chip answered at $%08lx after either write, with 40 ms of\n"
            "  settling time allowed for each, so the slot was given back.\n"
            "  Either the card is not an NE2000 clone, or it needs a\n"
            "  configuration entry other than the first, or the socket never\n"
            "  left memory mode.  The card.resource answer above says which\n"
            "  of those it is.\n", v);
        return;
    case ANXDIAG_PC_NOROW:
        say("  The card in the slot says manufacturer $%04lx product $%04lx,\n"
            "  and this driver has no PCMCIA card row for it and no fallback\n"
            "  row either.  The slot was given back.\n",
            (v >> 16) & 0xffffUL, v & 0xffffUL);
        return;
    case ANXDIAG_PC_IRQMODE:
        say("  The card's interrupt was enabled through card.resource V%lu.\n",
            v);
        return;
    case ANXDIAG_PC_IRQSKIP:
        say("  card.resource is V%lu.  The card's interrupt is left at the\n"
            "  resource's own default, which already passes it: OwnCard()\n"
            "  enables BSY/IRQ on every version.  Asking for it again\n"
            "  rewrites the socket's mode register and turns the socket off.\n",
            v);
        return;
    case ANXDIAG_PC_CLAIMED:
        say("  The slot is claimed.  The chip's registers are at $%08lx.\n", v);
        return;
    case ANXDIAG_PC_CARD:
        say("  The card in the slot was identified from its CIS as card row\n"
            "  %lu.  The slot lines printed under THIS MACHINE above happened\n"
            "  before that, so they belong to no card.\n", v);
        return;
    case ANXDIAG_PC_CFTABLE:
        say("  The first configuration table entry begins $%02lx $%02lx $%02lx\n"
            "  $%02lx.  It is printed raw because it is the one tuple whose\n"
            "  meaning depends on every byte before the byte you want.\n",
            (v >> 24) & 0xff, (v >> 16) & 0xff, (v >> 8) & 0xff, v & 0xff);
        return;

    /* ---- the ISA Plug and Play bridge ---- */
    case ANXDIAG_PNP_VENDOR:
        say("  The card behind the ISA Plug and Play bridge identified itself\n"
            "  as vendor and device $%08lx.  $4a8c8019 is a Realtek RTL8019,\n"
            "  which is what an X-Surf carries.  $00000000 means the isolation\n"
            "  reads found nothing driving the bus.\n", v);
        return;
    case ANXDIAG_PNP_SERIAL:
        say("  Its serial number is $%08lx.\n", v);
        return;
    case ANXDIAG_PNP_CSUM:
        if (((v >> 8) & 0xffUL) == (v & 0xffUL))
        {
            say("  The identifier's checksum is $%02lx and that is what it\n"
                "  computes to, so the isolation read a real card.\n",
                v & 0xffUL);
            return;
        }
        say("  The identifier's checksum came back $%02lx and computes to\n"
            "  $%02lx.  The bytes above are not a card's serial identifier,\n"
            "  so the configuration below was written blind -- report this\n"
            "  line with the two above it.\n",
            (v >> 8) & 0xffUL, v & 0xffUL);
        return;
    case ANXDIAG_PNP_IO:
        say("  The chip was told to decode 32 ports at ISA $%04lx, which is\n"
            "  where this card's row says its registers are.\n", v);
        return;
    case ANXDIAG_PNP_SETTLE:
        if (v == 0)
        {
            say("  The chip answered immediately after it was activated.\n");
            return;
        }
        say("  The chip was given %lu round(s) of 2 ms after it was activated\n"
            "  before it answered, or before the wait ran out.  Nothing\n"
            "  documents a settling time for this.  Report a non-zero number\n"
            "  here.\n", v);
        return;
    case ANXDIAG_PNP_CR:
        if ((v & ~0x02UL) == 0x21UL)
        {
            say("  The command register read back $%02lx: the chip is there.\n",
                v);
            return;
        }
        if (v == ANXDIAG_CR_DECOY)
        {
            say("  The command register read back $%02lx, the byte the probe\n"
                "  wrote at a different register just before reading it: the\n"
                "  bus is echoing writes, so no chip is decoding there.\n", v);
            return;
        }
        say("  The command register read back $%02lx, which is not what a\n"
            "  stopped DP8390 answers ($21, or $23 on a clone with a stuck\n"
            "  START bit).  $ff is a floating bus.\n", v);
        return;
    case ANXDIAG_PNP_SILENT:
        say("  Nothing answered at $%08lx after the Plug and Play sequence,\n"
            "  with 250 ms allowed for it, so the board was not made a unit.\n"
            "  The identifier above says whether the bridge answered at all:\n"
            "  a good checksum with no chip means the sequence ran and the\n"
            "  chip was left somewhere else.\n", v);
        return;
    case ANXDIAG_PNP_OK:
        say("  The Plug and Play sequence finished and the chip is decoding\n"
            "  at $%08lx.\n", v);
        return;

    /* ---- the EtherLink III ---- */
    case ANXDIAG_EL3_MFG:
        say("  The manufacturer ID read back as $%04lx.  It is $6d50 on every\n"
            "  EtherLink III.  $506d means the register window exchanges the\n"
            "  halves of a word.  Anything else means no such card answered.\n",
            v);
        return;
    case ANXDIAG_EL3_ORDER:
        if (v == 0)
            say("  Register words arrive as the chip holds them.  There is\n"
                "  no swap.\n");
        else
            say("  The register window exchanges the halves of every word, and\n"
                "  the driver measured that rather than being told it.\n");
        return;
    case ANXDIAG_EL3_MEDIA:
        say("  The card was built with:%s%s%s\n",
            (LONG)((v & 0x0200) != 0 ? " 10BASE-T" : ""),
            (LONG)((v & 0x1000) != 0 ? " 10BASE2" : ""),
            (LONG)((v & 0x2000) != 0 ? " AUI" : ""));
        return;

    default:
        break;
    }

    say("  Step %lu, value $%08lx: this command does not know that step.  It\n"
        "  is older than the driver that recorded it.\n",
        (ULONG)st->ds_Code, v);
}

/*
 * The station address is two steps, because it does not fit in one value.
 * Printed where the high half is.
 */
static VOID cnd_mac(UWORD at)
{
    ULONG hi = cnd_mark.ad_Step[at].ds_Value;
    ULONG lo = 0;
    UWORD i;

    for (i = (UWORD)(at + 1u); i < cnd_mark.ad_Used; i++)
    {
        if (cnd_mark.ad_Step[i].ds_Code == (UWORD)ANXDIAG_MAC_LO &&
            cnd_mark.ad_Step[i].ds_Card == cnd_mark.ad_Step[at].ds_Card)
        {
            lo = cnd_mark.ad_Step[i].ds_Value;
            break;
        }
    }

    say("  Station address %02lx:%02lx:%02lx:%02lx:%02lx:%02lx.\n",
        (hi >> 8) & 0xffUL, hi & 0xffUL,
        (lo >> 24) & 0xffUL, (lo >> 16) & 0xffUL,
        (lo >> 8) & 0xffUL, lo & 0xffUL);
}

/*
 * Grouped by card rather than printed in order: a PCMCIA claim interleaved
 * with a Zorro walk is hard to read one card at a time.
 */
static VOID cnd_report(BOOL raw)
{
    UWORD i;
    UWORD c;
    UWORD seen[16];
    UWORD nseen = 0;

    if (raw)
    {
        say("\nSTEPS AS RECORDED\n");
        for (i = 0; i < cnd_mark.ad_Used; i++)
        {
            say("  %2lu. code %lu card %ld value $%08lx\n", (ULONG)i,
                (ULONG)cnd_mark.ad_Step[i].ds_Code,
                (cnd_mark.ad_Step[i].ds_Card == (UWORD)ANXDIAG_NOCARD)
                    ? -1L : (LONG)cnd_mark.ad_Step[i].ds_Card,
                cnd_mark.ad_Step[i].ds_Value);
        }
        return;
    }

    say("\nTHIS MACHINE\n");
    for (i = 0; i < cnd_mark.ad_Used; i++)
    {
        if (cnd_mark.ad_Step[i].ds_Card == (UWORD)ANXDIAG_NOCARD)
            cnd_step(&cnd_mark.ad_Step[i]);
    }

    for (i = 0; i < cnd_mark.ad_Used; i++)
    {
        BOOL  already = FALSE;
        UWORD j;

        c = cnd_mark.ad_Step[i].ds_Card;
        if (c == (UWORD)ANXDIAG_NOCARD)
            continue;

        /* One block per card, printed at its first step.  The walk below
           gathers the rest of that card's steps, so a card already seen is
           skipped here rather than printed twice. */
        for (j = 0; j < nseen; j++)
        {
            if (seen[j] == c)
                already = TRUE;
        }
        if (already)
            continue;

        if (nseen < (UWORD)(sizeof(seen) / sizeof(seen[0])))
            seen[nseen++] = c;

        say("\nCARD \"%s\"\n", (LONG)cnd_card(c));
        cnd_chip_seen = 0xffffu;

        for (j = 0; j < cnd_mark.ad_Used; j++)
        {
            if (cnd_mark.ad_Step[j].ds_Card != c)
                continue;

            if (cnd_mark.ad_Step[j].ds_Code == (UWORD)ANXDIAG_MAC_LO)
                continue;       /* printed with the high half */

            if (cnd_mark.ad_Step[j].ds_Code == (UWORD)ANXDIAG_MAC_HI)
            {
                cnd_mac(j);
                continue;
            }

            cnd_step(&cnd_mark.ad_Step[j]);
        }
    }
}

/* ---------------------------------------------------------------- main --- */

int main(int argc, char **argv)
{
    LONG           args[ARG_COUNT];
    struct RDArgs *rda;
    const char    *device = ANXNET_DEVICE_NAME;
    UWORD          status;
    LONG           rc;

    (VOID)argv;

    if (tool_from_workbench(argc))
        return RETURN_FAIL;

    tool_break_arm();

    args[ARG_DEVICE] = 0;
    args[ARG_NOLOAD] = 0;
    args[ARG_RAW]    = 0;

    rda = ReadArgs((CONST_STRPTR)TEMPLATE, args, NULL);
    if (rda == NULL)
    {
        tool_fault(IoErr());
        tool_usage("[DEVICE <name>] [NOLOAD] [RAW]",
                   "Say what anxnet.device found when it probed this "
                   "machine, and why any card it did not take was refused.");
        return RETURN_ERROR;
    }

    if (args[ARG_DEVICE] != 0)
        device = (const char *)args[ARG_DEVICE];

    status = cnd_read();

    /*
     * A driver that never ran is not an answer, so without NOLOAD the driver
     * is loaded and the record read again. The open is expected to fail; it is
     * there to make Exec LoadSeg the driver and run its romtag init.
     */
    if (status == CND_ABSENT && args[ARG_NOLOAD] == 0)
    {
        (VOID)tool_device_probe(device, 0, NULL);
        status = cnd_read();
    }

    say("%s: what %s found on this machine\n", (LONG)tool_name, (LONG)device);

    if (status == CND_BAD_VERSION)
    {
        tool_error("the driver publishes a probe record this command cannot "
                   "read");
        say("\n  The record says version %lu, %lu bytes.  This command was\n"
            "  built for version %lu, %lu bytes.  DEVS:Networks/anxnet.device\n"
            "  and C:%s ship together, so one of the two is from a different\n"
            "  release.  Update both.\n",
            (ULONG)cnd_mark.ad_Version, (ULONG)cnd_mark.ad_Size,
            (ULONG)ANXDIAG_VERSION, (ULONG)sizeof(AnxDiagMark),
            (LONG)tool_name);
        FreeArgs(rda);
        return RETURN_ERROR;
    }

    if (status == CND_ABSENT)
    {
        tool_error("no probe record: %s is not loaded", (LONG)device);
        say("\n");
        tool_wrap(2,
            "Nothing was found under the name the driver publishes, so the "
            "driver has not run on this machine since it was booted. That is "
            "not a fault by itself: the driver is loaded when something opens "
            "it.");
        say("\n");
        tool_wrap(2,
            "Check that DEVS:Networks/anxnet.device exists. Then run this "
            "command again without NOLOAD, which loads the driver so it can "
            "probe.");
        FreeArgs(rda);
        return RETURN_ERROR;
    }

    say("\n");
    if (cnd_mark.ad_Lost != 0)
    {
        say("%lu step(s) recorded.  %lu more than the record holds were\n"
            "dropped.\n",
            (ULONG)cnd_mark.ad_Used, (ULONG)cnd_mark.ad_Lost);
    }
    else
    {
        say("%lu step(s) recorded.\n", (ULONG)cnd_mark.ad_Used);
    }

    cnd_report(args[ARG_RAW] != 0);

    say("\nVERDICT\n");
    if (cnd_mark.ad_Units == 0)
    {
        tool_wrap(2,
            "No card came up. Every card this driver looked at is above, "
            "with the step it stopped at. Nothing will open anxnet.device "
            "until one of them attaches.");
        rc = RETURN_WARN;
    }
    else
    {
        say("  %lu card(s) attached and can be opened.\n",
            (ULONG)cnd_mark.ad_Units);
        rc = RETURN_OK;
    }

    if (cnd_mark.ad_Dropped != 0)
    {
        say("\n");
        tool_wrap(2,
            "One or more supported cards were found after the driver had "
            "run out of units. Remove a card, or open the attached cards "
            "by name.");
        rc = RETURN_WARN;
    }

    if (tool_break())
    {
        tool_fault(ERROR_BREAK);
        rc = RETURN_WARN;
    }

    FreeArgs(rda);
    return (int)rc;
}

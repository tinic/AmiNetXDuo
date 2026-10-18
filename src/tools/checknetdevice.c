/*
 * CheckNetDevice, say what anxnet.device found and what it refused.
 *
 * The driver publishes its probe record under a public semaphore whatever the
 * outcome; this reads it. Nothing on that path needs a unit, an open device, a
 * running stack or a filesystem.
 *
 * The PC Card in an A600/A1200 slot is reported whatever the driver did with
 * it, network card or not, read without loading or opening anything.
 *
 * SPDX-License-Identifier: MIT
 */

#include "tool_passive.h"

#include <exec/execbase.h>
#include <exec/memory.h>

#include "aminetxduo/anxdiag.h"
#include "aminetxduo/anxnet.h"
#include "netprobe_pcmcia.h"

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
 * tool_anxdiag_read() copies it whole under Forbid().  The semaphore is never
 * obtained -- a diagnostic must not block on the driver that can be the
 * broken thing.
 */
static AnxDiagMark cnd_mark;

static const char *cnd_device = ANXNET_DEVICE_NAME;   /* what the messages name */

#define CND_OK              TOOL_PASSIVE_OK
#define CND_ABSENT          TOOL_PASSIVE_ABSENT
#define CND_BAD_VERSION     TOOL_PASSIVE_BAD_VERSION

static UWORD cnd_read(const char *device)
{
    return tool_anxdiag_read(device, &cnd_mark);
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
        return "command register is not a stopped DP8390";
    case ANXDIAG_WHY_ODD:
        return "odd registers do not answer (bytes or words)";
    case ANXDIAG_WHY_ODD_BNRY:
        return "odd registers do not hold a written value (bytes or words)";
    case ANXDIAG_WHY_BUFFER:
        return "data port failed a 32-byte pattern";
    case ANXDIAG_WHY_MEM:
        return "packet buffer RAM failed (16 KB pattern)";
    case ANXDIAG_WHY_ED_MEM:
        return "mapped packet buffer failed the pattern test";
    case ANXDIAG_WHY_ADDRESS:
        return "the card offered no usable station address";
    case ANXDIAG_WHY_REGS:
        return "register file failed write/read";
    case ANXDIAG_WHY_CSR:
        return "CSR0 did not read back as a stopped LANCE";
    case ANXDIAG_WHY_MFGID:
        return "manufacturer ID is neither $6d50 nor $506d";
    case ANXDIAG_WHY_EEPROM:
        return "EEPROM never ready; no station address";
    case ANXDIAG_WHY_REV:
        return "GENET version register is not v5";
    case ANXDIAG_WHY_NOMEM:
        return "no fast RAM for the GENET rings";
    case ANXDIAG_WHY_NODMA:
        return "GENET ring RAM outside device-tree physical memory";
    default:
        break;
    }

    return "refused, no reason given";
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
        return "derived (PROM blank)";
    case ANXDIAG_MAC_SERIAL:
        return "the autoconfig serial number";
    case ANXDIAG_MAC_DTREE:
        return "the device tree (local-mac-address)";
    default:
        break;
    }

    return "unknown";
}

static const char *cnd_chip(ULONG chip)
{
    switch (chip)
    {
    case 0:  return "DP8390, remote-DMA port (NE2000)";
    case 1:  return "DP8390, memory-mapped buffer";
    case 2:  return "Am7990 LANCE, bus master";
    case 3:  return "3Com EtherLink III, PIO FIFOs";
    case 4:  return "Broadcom GENET v5 (Pi 4), bus master";
    case 5:  return "MNT ZZ9000 (Zynq MAC via firmware)";
    default: break;
    }

    return "unknown chip";
}

static const char *cnd_dmode(ULONG mode)
{
    switch (mode)
    {
    case 0:  return "8-bit";
    case 1:  return "16-bit word port";
    case 2:  return "32-bit mirrored window";
    default: break;
    }

    return "unknown mode";
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
        say("  Probe started, %lu card types.\n", v);
        return;
    case ANXDIAG_EXPANSION:
        if (v == 0)
        {
            say("  expansion.library did not open; Zorro not probed.\n");
            return;
        }
        say("  expansion.library at $%08lx.\n", v);
        return;
    case ANXDIAG_BOARDS:
        if (v == 0)
        {
            say("  No autoconfig boards.\n");
            return;
        }
        say("  %lu autoconfig board(s) examined.\n", v);
        return;
    case ANXDIAG_NOMATCH:
        say("  Board %lu/%lu: not supported.\n",
            (v >> 16) & 0xffffUL, v & 0xffUL);
        return;
    case ANXDIAG_UNITS_FULL:
        say("  Supported card, no free unit (limit %lu).\n", v);
        return;
    case ANXDIAG_DONE:
        say("  Probe finished, %lu card(s) up.\n", v);
        return;

    /* ---- one card ---- */
    case ANXDIAG_ZORRO_FOUND:
        say("  Autoconfig, board at $%08lx.\n", v);
        return;
    case ANXDIAG_FIXED_TRY:
        say("  Fixed address $%08lx (no autoconfig).\n", v);
        return;
    case ANXDIAG_NO_CORE:
        say("  No chip core for chip type %lu.\n", v);
        return;
    case ANXDIAG_DTREE_FOUND:
        say("  Device tree: registers at $%08lx.\n", v);
        return;
    case ANXDIAG_GENET_REV:
        say("  GENET version register $%08lx (major %lu, minor %lu).\n",
            v, (v >> 24) & 0x0fUL, (v >> 16) & 0x0fUL);
        return;
    case ANXDIAG_GENET_MEM:
        say("  RX buffers at $%08lx (DMA-reachable fast RAM).\n", v);
        return;
    case ANXDIAG_GENET_IRQ:
        if (v == 0)
            say("  No interrupt in device tree; polled.\n");
        else
            say("  GIC interrupt %lu (SPI %lu), through gic400.library.\n",
                v, v - 32UL);
        return;
    case ANXDIAG_GENET_GIC:
        if (v == 0)
            say("  No GIC-400 distributor; dead-line workaround off.\n");
        else
            say("  GIC-400 distributor at $%08lx; dead-line workaround on.\n",
                v);
        return;
    case ANXDIAG_NE_NODEID_PORT:
        say("  AX88796 node ID bytes 0-3 (16-bit port): $%08lx.\n", v);
        return;
    case ANXDIAG_CACHE_GUARD:
        {
            static const char *const how[] =
            {
                "not used (not a 68030, or no coherence probe)",
                "TT0 marks the board's block cache-inhibited",
                "TT1 marks the board's block cache-inhibited",
                "the data cache is OFF while the driver holds the board",
                "NONE, board not coherent"
            };

            say("  Zorro III cache guard: %s.\n",
                (LONG)((v < 5UL) ? how[v] : "?"));
        }
        return;
    case ANXDIAG_CACHE_WHY:
        say("  TT registers skipped:%s%s%s%s%s.\n",
            (LONG)((v & 0x01UL) ? " Exec RAM in block" : ""),
            (LONG)((v & 0x02UL) ? " TT0 in use" : ""),
            (LONG)((v & 0x04UL) ? " TT1 in use" : ""),
            (LONG)((v & 0x08UL) ? " TT0 ineffective" : ""),
            (LONG)((v & 0x10UL) ? " TT1 ineffective" : ""));
        return;
    case ANXDIAG_GENET_DMA:
        say("  RX DMA control $%08lx at attach: %s.\n", v,
            (LONG)((v & 1UL) != 0 ? "running across reboot" : "stopped"));
        return;
    case ANXDIAG_GENET_PHY:
        if (v == 0xffffffffUL)
            say("  PHY: no MDIO answer.\n");
        else
            say("  PHY identifier $%08lx (OUI bits $%06lx, model $%02lx,\n"
                "  revision %lu).\n", v, v >> 10, (v >> 4) & 0x3fUL,
                v & 0x0fUL);
        return;
    case ANXDIAG_CR_READ:
        say("  Command register read $%02lx.\n", v);
        return;
    case ANXDIAG_ODD_RETRY:
        say("  Odd registers failed as bytes; trying words.\n");
        return;
    case ANXDIAG_CR_RETRY:
        say("  No reset; reset port strobed via word read, command register "
            "re-read.\n");
        return;
    case ANXDIAG_ODD_PLAIN:
    case ANXDIAG_ODD_WORD:
        say("  Odd registers as %s: ISR $%02lx, BNRY $5a/$a5 -> $%02lx/$%02lx.\n",
            (LONG)((st->ds_Code == (UWORD)ANXDIAG_ODD_PLAIN)
                       ? "bytes" : "words"),
            (v >> 16) & 0xffUL, (v >> 8) & 0xffUL, v & 0xffUL);
        return;
    case ANXDIAG_BUF_SEEN:
        say("  First bad buffer byte at $%04lx: wrote $%02lx, read $%02lx.\n",
            (v >> 16) & 0xffffUL, (v >> 8) & 0xffUL, v & 0xffUL);
        return;
    case ANXDIAG_ODDWIN:
        if (v == 0)
        {
            say("  Registers: one contiguous block.\n");
            return;
        }
        say("  Odd registers via second window at $%08lx (Gayle split).\n", v);
        return;
    case ANXDIAG_CHIP:
        cnd_chip_seen = (UWORD)v;
        say("  Chip: %s.\n", (LONG)cnd_chip(v));
        return;
    case ANXDIAG_ATTACH_OK:
        /*
         * Only the NE2000's remote-DMA port has a transfer mode: ne2000.c is
         * the one driver that sets bus.dmode.  For the shared-memory DP8390
         * (ed_copy_in/out), a LANCE, an EtherLink III, a GENET that moves
         * frames by its own DMA or a ZZ9000 with its frame windows, reporting
         * it would be an invented fact (F-155).
         */
        if (cnd_chip_seen != 0)
        {
            say("  ATTACHED.\n");
            return;
        }
        say("  ATTACHED.\n  Data path: %s.\n", (LONG)cnd_dmode(v));
        return;
    case ANXDIAG_ATTACH_FAIL:
        say("  REFUSED:\n");
        tool_wrap(4, cnd_why(v));
        return;
    case ANXDIAG_MAC_SOURCE:
        say("  MAC source: %s.\n", (LONG)cnd_macsource(v));
        return;
    case ANXDIAG_GETODD:
        if (v != 0)
        {
            say("  Odd registers read as words (16-bit I/O only, measured).\n");
            return;
        }
        say("  Odd registers read as bytes.\n");
        return;
    case ANXDIAG_UNIT:
        say("  Unit %lu of %s.\n", v, (LONG)cnd_device);
        return;

    /* ---- the PCMCIA slot ---- */
    case ANXDIAG_PC_RESOURCE:
        if (v == 0)
        {
            say("  No card.resource; no PCMCIA slot.\n");
            return;
        }
        say("  card.resource at $%08lx (PCMCIA slot).\n", v);
        return;
    case ANXDIAG_PC_OWN:
        if (v == 0)
        {
            say("  OwnCard(): granted.\n");
            return;
        }
        if (v == 0xffffffffUL)
        {
            say("  OwnCard(): -1, slot empty.\n");
            return;
        }
        say("  OwnCard() refused: slot owned by CardHandle $%08lx.\n", v);
        return;
    case ANXDIAG_PC_FUNCID:
        if (v == ANXDIAG_ABSENT)
        {
            say("  No CISTPL_FUNCID; assumed LAN.\n");
            return;
        }
        if (v == 6)
        {
            say("  CISTPL_FUNCID 6 (LAN).\n");
            return;
        }
        say("  CISTPL_FUNCID says function %lu.\n", v);
        return;
    case ANXDIAG_PC_NOTLAN:
        say("  Not a LAN adapter; slot released.\n");
        return;
    case ANXDIAG_PC_WIRELESS:
        say("  Card $%04lx/$%04lx is a Wi-Fi card (prism2.device's); "
            "slot released, nothing written.\n",
            (v >> 16) & 0xffffUL, v & 0xffffUL);
        return;
    case ANXDIAG_PC_MANFID:
        if (v == ANXDIAG_ABSENT)
        {
            say("  No CISTPL_MANFID.\n");
            return;
        }
        say("  CISTPL_MANFID: manufacturer $%04lx, product $%04lx.\n",
            (v >> 16) & 0xffffUL, v & 0xffffUL);
        return;
    case ANXDIAG_PC_FUNCE:
        if (v == ANXDIAG_ABSENT)
        {
            say("  No CISTPL_FUNCE; no station address in CIS.\n");
            return;
        }
        if (v == 4)
        {
            say("  CISTPL_FUNCE subtuple 4 (LAN node ID).\n");
            return;
        }
        say("  CISTPL_FUNCE subtuple %lu, not a node ID.\n", v);
        return;
    case ANXDIAG_PC_NODEID:
        if (v != 0)
        {
            say("  CIS node ID usable.\n");
            return;
        }
        say("  CIS node ID unusable, ignored.\n");
        return;
    case ANXDIAG_PC_NOCONFIG:
        say("  No CISTPL_CONFIG; slot released.\n");
        return;
    case ANXDIAG_PC_CFGBASE:
        say("  CISTPL_CONFIG: registers at $%08lx (attribute memory).\n", v);
        return;
    case ANXDIAG_PC_NOCFTABLE:
        say("  No CISTPL_CFTABLE_ENTRY; slot released.\n");
        return;
    case ANXDIAG_PC_INDEX:
        say("  Configuration index %lu.\n", v);
        return;
    case ANXDIAG_PC_CFCOUNT:
        say("  %lu configuration %s read (limit 32).\n",
            v, (ULONG)(APTR)(v == 1 ? "entry" : "entries"));
        return;
    case ANXDIAG_PC_CFPICK:
        if (v == ANXDIAG_ABSENT)
        {
            say("  No entry parsed; first entry's index written.\n");
            return;
        }
        say("  Entry index %lu chosen: %lu address line(s), flags $%02lx.\n",
            v & 0x3f, (v >> 8) & 0xff, (v >> 16) & 0xff);
        if (((v >> 24) & 0xffUL) == 1)
            say("  No 8-bit I/O entry; best requires 16-bit.\n");
        return;
    case ANXDIAG_PC_IOWIN:
        say("  I/O window %lu byte(s) at $%04lx.\n",
            v & 0xffff, (v >> 16) & 0xffff);
        return;
    case ANXDIAG_PC_IOOFF:
        say("  Registers at slot I/O offset $%04lx.\n", v);
        return;
    case ANXDIAG_PC_MFC:
        say("  MULTIFUNCTION: CISTPL_LONGLINK_MFC, %lu chain(s); CIS read "
            "directly.\n", (v >> 16) & 0xffffUL);
        if ((v & 0xffffUL) == 0)
        {
            say("  No LAN function.\n");
            return;
        }
        say("  LAN function found.\n");
        return;
    case ANXDIAG_PC_MFCFUNC:
        say("  Chain at CIS offset $%06lx, CISTPL_FUNCID %lu.\n",
            v & 0x00ffffffUL, (v >> 24) & 0xffUL);
        return;
    case ANXDIAG_PC_MFCNOLAN:
        say("  Multifunction card, no configurable LAN function; refused.\n");
        return;
    case ANXDIAG_PC_MFCIOBASE:
        say("  IOBASE_0/IOBASE_1 set to $%04lx.\n", v);
        return;
    case ANXDIAG_PC_IOMODE:
        say("  The socket was put into I/O mode (CardMiscControl $%02lx).\n",
            v);
        return;
    case ANXDIAG_PC_MISC:
        if ((v & 0x0aUL) == 0x0aUL)
        {
            say("  card.resource answered $%02lx: I/O mode, write protect "
                "off.\n", v);
            return;
        }
        say("  card.resource answered $%02lx: $08 or $02 not supported.\n", v);
        return;
    case ANXDIAG_PC_COR:
        say("  Option register written at $%08lx.\n", v);
        return;
    case ANXDIAG_PC_CORVAL:
        /* Bit 6 is the COR's level-mode interrupt request, left clear: Gayle
           reports the PC Card interrupt as an edge whatever the card asked
           for. Named here so the byte does not have to be looked up. */
        if ((v & 0x01UL) != 0)
        {
            /* A multifunction card's COR is not the configuration index:
               bits 2..0 are the three enables and only bits 5..3 of the
               index survive. */
            say("  Wrote $%02lx (multifunction): index bits $%02lx, function "
                "and interrupt enable%s%s.\n",
                v, v & 0x38UL,
                (v & 0x02UL) ? ", address decode" : "",
                (v & 0x40UL) ? ", level-mode interrupt" : "");
            return;
        }
        say("  Wrote $%02lx: configuration index %lu, edge interrupt.\n",
            v, v & 0x3fUL);
        return;
    case ANXDIAG_PC_RESET:
        /* WITHOUT THIS ARM the step fell through to the unknown-step message
           -- about a step defined in the same tree, carrying the one number
           the PCMCIA reset is measured in. */
        say("  Slot reset held %lu ms (requested).\n", v);
        return;
    case ANXDIAG_CLOCK:
        if (v == 0)
        {
            say("  No raster beam; waits are counted loops.\n");
            return;
        }
        say("  Delay clock measured %lu spin(s) per raster line.\n", v);
        return;
    case ANXDIAG_CLOCK_LINE:
        if (v == 0)
            return;             /* no beam: ANXDIAG_CLOCK said so */
        if (v == 63)
        {
            say("  Scan line measured at 63 us (15 kHz).\n");
            return;
        }
        if (v == 31)
        {
            say("  Scan line measured at 31 us (31 kHz).\n");
            return;
        }
        say("  Scan line costed at %lu us (field count fits no mode; waits "
            "about 2x).\n", v);
        return;
    case ANXDIAG_PC_SETTLE:
        if (v == 0)
        {
            say("  Chip answered at once.\n");
            return;
        }
        say("  Waited %lu x 2 ms for the chip.\n", v);
        return;
    case ANXDIAG_PC_CR:
        if (v == 0xffUL)
        {
            say("  Command register read $ff: bus floating.\n");
            return;
        }
        if (v == 0)
        {
            say("  Command register read $00: nothing decoding.\n");
            return;
        }
        if ((v & ~0x02UL) == 0x21UL)
        {
            say("  Command register read $%02lx: DP8390 answered.\n", v);
            return;
        }
        if (v == ANXDIAG_CR_DECOY)
        {
            say("  Command register read $%02lx (echo of last write): slot "
                "empty.\n", v);
            return;
        }
        say("  Command register read $%02lx: not a stopped DP8390 ($21/$23).\n",
            v);
        return;
    case ANXDIAG_PC_COR2:
        say("  No answer; option register rewritten at $%08lx.\n", v);
        return;
    case ANXDIAG_PC_CR2:
        say("  Second write: command register read $%02lx.\n", v);
        return;
    case ANXDIAG_PC_SILENT:
        say("  No chip at $%08lx after two writes (40 ms each); slot "
            "released.\n", v);
        return;
    case ANXDIAG_PC_NOROW:
        say("  Card $%04lx/$%04lx: no card row; slot released.\n",
            (v >> 16) & 0xffffUL, v & 0xffffUL);
        return;
    case ANXDIAG_PC_IRQMODE:
        say("  Card interrupt enabled (card.resource V%lu).\n", v);
        return;
    case ANXDIAG_PC_IRQSKIP:
        say("  card.resource V%lu: interrupt left at default.\n", v);
        return;
    case ANXDIAG_PC_CLAIMED:
        say("  Slot claimed; registers at $%08lx.\n", v);
        return;
    case ANXDIAG_PC_CARD:
        say("  CIS: card row %lu.\n", v);
        return;
    case ANXDIAG_PC_CFTABLE:
        say("  First CFTABLE entry: $%02lx $%02lx $%02lx $%02lx.\n",
            (v >> 24) & 0xff, (v >> 16) & 0xff, (v >> 8) & 0xff, v & 0xff);
        return;

    /* ---- the ISA Plug and Play bridge ---- */
    case ANXDIAG_PNP_VENDOR:
        say("  ISA PnP vendor/device $%08lx.\n", v);
        return;
    case ANXDIAG_PNP_SERIAL:
        say("  Its serial number is $%08lx.\n", v);
        return;
    case ANXDIAG_PNP_CSUM:
        if (((v >> 8) & 0xffUL) == (v & 0xffUL))
        {
            say("  Identifier checksum $%02lx: valid.\n", v & 0xffUL);
            return;
        }
        say("  Identifier checksum $%02lx, computed $%02lx: invalid; "
            "configured blind.\n",
            (v >> 8) & 0xffUL, v & 0xffUL);
        return;
    case ANXDIAG_PNP_IO:
        say("  Chip set to decode 32 ports at ISA $%04lx.\n", v);
        return;
    case ANXDIAG_PNP_SETTLE:
        if (v == 0)
        {
            say("  Chip answered at once after activation.\n");
            return;
        }
        say("  Waited %lu x 2 ms after activation.\n", v);
        return;
    case ANXDIAG_PNP_CR:
        if ((v & ~0x02UL) == 0x21UL)
        {
            say("  Command register read $%02lx: chip present.\n", v);
            return;
        }
        if (v == ANXDIAG_CR_DECOY)
        {
            say("  Command register read $%02lx (echo of last write): no "
                "chip.\n", v);
            return;
        }
        say("  Command register read $%02lx: not a stopped DP8390 ($21/$23).\n",
            v);
        return;
    case ANXDIAG_PNP_SILENT:
        say("  No answer at $%08lx after PnP sequence (250 ms); not a unit.\n",
            v);
        return;
    case ANXDIAG_PNP_OK:
        say("  PnP done; chip at $%08lx.\n", v);
        return;

    /* ---- the EtherLink III ---- */
    case ANXDIAG_EL3_MFG:
        say("  Manufacturer ID $%04lx.\n", v);
        return;
    case ANXDIAG_EL3_ORDER:
        if (v == 0)
            say("  Register words: no swap.\n");
        else
            say("  Register words: halves swapped (measured).\n");
        return;
    case ANXDIAG_EL3_MEDIA:
        say("  Media:%s%s%s\n",
            (LONG)((v & 0x0200) != 0 ? " 10BASE-T" : ""),
            (LONG)((v & 0x1000) != 0 ? " 10BASE2" : ""),
            (LONG)((v & 0x2000) != 0 ? " AUI" : ""));
        return;
    case ANXDIAG_EL3_FIFO:
        say("  TX FIFO %lu bytes free, product ID $%04lx, %lu full frames.\n",
            (ULONG)(v >> 16), (ULONG)(v & 0xffff),
            (ULONG)((v >> 16) / 1520UL));
        return;

    default:
        break;
    }

    say("  Step %lu, value $%08lx: unknown.\n", (ULONG)st->ds_Code, v);
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

/* ------------------------------------------------------------ the slot --- */

static VOID cnd_slot_line(void *ctx, const char *text)
{
    (VOID)ctx;
    say("  %s\n", (LONG)text);
}

static VOID cnd_slot(VOID)
{
    static NetprobeSlot slot;

    netprobe_pcmcia_read(&slot);
    say("\nPC CARD SLOT\n");
    netprobe_slot_report(&slot, cnd_slot_line, NULL);
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
                   "Report what the driver found at probe, and the PC Card.");
        return RETURN_ERROR;
    }

    if (args[ARG_DEVICE] != 0)
        device = (const char *)args[ARG_DEVICE];
    cnd_device = device;

    status = cnd_read(device);

    /*
     * A driver that never ran is not an answer, so without NOLOAD the driver
     * is loaded and the record read again. The open is expected to fail; it is
     * there to make Exec LoadSeg the driver and run its romtag init.
     */
    if (status == CND_ABSENT && args[ARG_NOLOAD] == 0)
    {
        (VOID)tool_device_probe(device, 0, NULL);
        status = cnd_read(device);
    }

    say("%s: %s probe record\n", (LONG)tool_name, (LONG)device);

    if (status != CND_OK)
        cnd_slot();

    if (status == CND_BAD_VERSION)
    {
        tool_error("probe record version %lu (%lu bytes), expected %lu "
                   "(%lu bytes)",
                   (ULONG)cnd_mark.ad_Version, (ULONG)cnd_mark.ad_Size,
                   (ULONG)ANXDIAG_VERSION, (ULONG)sizeof(AnxDiagMark));
        FreeArgs(rda);
        return RETURN_ERROR;
    }

    if (status == CND_ABSENT)
    {
        tool_error("no probe record: %s is not loaded", (LONG)device);
        FreeArgs(rda);
        return RETURN_ERROR;
    }

    say("\n");
    if (cnd_mark.ad_Lost != 0)
    {
        say("%lu step(s) recorded, %lu dropped.\n",
            (ULONG)cnd_mark.ad_Used, (ULONG)cnd_mark.ad_Lost);
    }
    else
    {
        say("%lu step(s) recorded.\n", (ULONG)cnd_mark.ad_Used);
    }

    cnd_report(args[ARG_RAW] != 0);
    cnd_slot();

    say("\nVERDICT\n");
    if (cnd_mark.ad_Units == 0)
    {
        say("  No card attached.\n");
        rc = RETURN_WARN;
    }
    else
    {
        say("  %lu card(s) attached.\n",
            (ULONG)cnd_mark.ad_Units);
        rc = RETURN_OK;
    }

    if (cnd_mark.ad_Dropped != 0)
    {
        say("\n");
        say("  Supported card(s) found with no unit left.\n");
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

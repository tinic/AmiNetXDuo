/*
 * 3Com EtherLink III (3C589 and family): registers, commands and bit fields.
 *
 * Written from the "EtherLink III Parallel Tasking Adapter Drivers Technical
 * Reference", 3Com manual part 09-0398-002B, August 1994.  Offsets, bit
 * positions and command encodings are facts of the device and are what this
 * file records; none of the manual's prose or layout is reproduced here.
 *
 * NetBSD's elink3.c/elink3reg.h and AROS's etherlink3 were read for
 * comprehension and neither was copied: the first is four-clause BSD, whose
 * advertising clause would bind everyone who redistributes this, and the
 * second is GPL-2.0+.  This tree is MIT.  Same rule as lancereg.h, which was
 * written from the Am7990 data sheet for the same reason.
 *
 * The part is sixteen bytes of I/O space.  The last word of it, offset 0x0e,
 * is the Command register when written and the Status register when read, in
 * every window.  The other fourteen bytes are overlaid by one of eight
 * register windows.  There is no window register: the window is chosen by a
 * Command opcode and read back in the top three bits of Status.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_EL3REG_H
#define AMINETXDUO_EL3REG_H

/* The window-independent word: write Command, read Status. */
#define EL3_COMMAND         0x0e
#define EL3_STATUS          0x0e

/* Sixteen bytes, and the card decodes I/O modulo 16 once its Address Config
   I/O base field is zero, which is what epinit() must do on PCMCIA. */
#define EL3_IOSIZE          16

/* ------------------------------------------------------------ commands --- */

/*
 * A command is one 16-bit write: a 5-bit opcode in bits 15..11 and an 11-bit
 * argument under it.  EL3_CMD() builds one.
 *
 * EL3_CMD(EL3_C_RESET, 0) is 0x0000, which is the same word in either byte
 * order.  el3.c leans on that: it is the one command that can be issued
 * before the byte order of the register window is known.
 */
#define EL3_CMD(op, arg)    ((UWORD)(((UWORD)(op) << 11) | ((UWORD)(arg) & 0x07ffu)))

#define EL3_C_RESET             0x00    /* global reset                      */
#define EL3_C_WINDOW            0x01    /* arg bits 2..0 = window 0..7       */
#define EL3_C_COAX_START        0x02    /* DC-DC converter on; the LED, here */
#define EL3_C_RX_DISABLE        0x03
#define EL3_C_RX_ENABLE         0x04
#define EL3_C_RX_RESET          0x05
#define EL3_C_RX_DISCARD        0x08    /* pop the top receive packet        */
#define EL3_C_TX_ENABLE         0x09
#define EL3_C_TX_DISABLE        0x0a
#define EL3_C_TX_RESET          0x0b
#define EL3_C_REQ_INTR          0x0c
#define EL3_C_ACK_INTR          0x0d    /* arg = EL3_S_* bits                */
#define EL3_C_SET_INTR_MASK     0x0e    /* arg = EL3_S_* bits                */
#define EL3_C_SET_ZERO_MASK     0x0f    /* arg = EL3_S_* bits                */
#define EL3_C_SET_RX_FILTER     0x10    /* arg = EL3_FIL_* bits              */
#define EL3_C_SET_RX_EARLY      0x11    /* arg = byte count, dword-truncated */
#define EL3_C_SET_TX_AVAIL      0x12    /* arg = byte count, dword-truncated */
#define EL3_C_SET_TX_START      0x13    /* arg = byte count, dword-truncated */
#define EL3_C_STATS_ENABLE      0x15
#define EL3_C_STATS_DISABLE     0x16
#define EL3_C_COAX_STOP         0x17

/* ------------------------------------------------------------- status ---- */

/*
 * The low byte of Status is the interrupt cause set, and the argument to
 * Acknowledge Interrupt and Set Interrupt Mask has the same bit assignments.
 *
 * What an acknowledge clears is not uniform, and the manual is explicit about
 * it.  TX_COMPLETE clears only when the transmit status stack is popped,
 * RX_COMPLETE only when the packet is read out and discarded, and
 * ADAPTER_FAIL only when the FIFO diagnostic bit behind it is cleared.  An
 * acknowledge of those three does nothing.  INT_LATCH is the interrupt itself
 * and is acknowledged last, after every cause has been dealt with.
 */
#define EL3_S_INT_LATCH         0x0001
#define EL3_S_ADAPTER_FAIL      0x0002
#define EL3_S_TX_COMPLETE       0x0004
#define EL3_S_TX_AVAIL          0x0008
#define EL3_S_RX_COMPLETE       0x0010
#define EL3_S_RX_EARLY          0x0020
#define EL3_S_INT_REQ           0x0040
#define EL3_S_UPD_STATS         0x0080
#define EL3_S_CMD_BUSY          0x1000  /* a multi-cycle command is running  */
#define EL3_S_WINDOW_MASK       0xe000  /* the window, in bits 15..13        */
#define EL3_S_WINDOW_SHIFT      13

/* Everything that can raise an interrupt. */
#define EL3_S_INTS              (EL3_S_ADAPTER_FAIL | EL3_S_TX_COMPLETE | \
                                 EL3_S_TX_AVAIL | EL3_S_RX_COMPLETE | \
                                 EL3_S_RX_EARLY | EL3_S_INT_REQ | \
                                 EL3_S_UPD_STATS)

/* ------------------------------------------------------- receive filter -- */

/*
 * Four bits, and there is no fifth.  The manual states that the part has no
 * multicast hash filter of any kind, so "group" is every multicast address or
 * none.  The DP8390's per-group filter is reproduced by accepting them all
 * here and testing each frame's destination against nic->mar[] in software,
 * which is what el3.c does.
 *
 * Group implies broadcast on this part.  Broadcast is set anyway rather than
 * relying on that.
 */
#define EL3_FIL_INDIVIDUAL      0x01
#define EL3_FIL_GROUP           0x02
#define EL3_FIL_BROADCAST       0x04
#define EL3_FIL_PROMISC         0x08

/* --------------------------------------------------------- window 0 ------ */

#define EL3_W0_MFG_ID           0x00    /* read-only, always EL3_MFG_ID      */
#define EL3_W0_PRODUCT_ID       0x02
#define EL3_W0_CONFIG_CTRL      0x04
#define EL3_W0_ADDR_CFG         0x06
#define EL3_W0_RESOURCE_CFG     0x08
#define EL3_W0_EEPROM_CMD       0x0a
#define EL3_W0_EEPROM_DATA      0x0c

/*
 * The manufacturer ID is 3Com's EISA code and is hard-wired: every part in
 * the family reads this word and no other.  It is what el3_answers() uses to
 * decide both that a card is there and which way round the register window
 * delivers a word -- see el3.c.
 */
#define EL3_MFG_ID              0x6d50
#define EL3_MFG_ID_SWAPPED      0x506d

/* Configuration control.  The three media bits are read-only and say which
   transceivers the card was built with, not which one is selected. */
#define EL3_CC_AUI_PRESENT      0x2000
#define EL3_CC_BNC_PRESENT      0x1000
#define EL3_CC_UTP_PRESENT      0x0200
#define EL3_CC_MEDIA_MASK       (EL3_CC_AUI_PRESENT | EL3_CC_BNC_PRESENT | \
                                 EL3_CC_UTP_PRESENT)
#define EL3_CC_RESET            0x0004
#define EL3_CC_ENABLE           0x0001

/*
 * Address configuration.  Bits 15..14 select the transceiver, 13..12 the boot
 * ROM size, 11..8 its base, and 4..0 the I/O base.
 *
 * On PCMCIA the ROM and I/O base fields must all be zero.  The manual is
 * unambiguous: with a non-zero I/O base the card does not answer I/O.  With it
 * zero the card decodes on the bottom four address lines only, which is why
 * 0x300 works, as would any other sixteen-byte block Gayle maps.
 */
#define EL3_AC_XCVR_MASK        0xc000
#define EL3_AC_XCVR_SHIFT       14
#define EL3_AC_XCVR_UTP         0       /* 10BASE-T                          */
#define EL3_AC_XCVR_AUI         1
#define EL3_AC_XCVR_BNC         3       /* 10BASE2; 2 is not assigned        */
#define EL3_AC_ROM_IO_MASK      0x3f1f  /* ROM size, ROM base and I/O base   */

/*
 * Resource configuration.  Bits 15..12 are the interrupt number and 11..8 are
 * reserved and must read back as all ones.
 *
 * 0x3f00 is not a constant the manual prints.  It follows from the manual's
 * two statements about this register on a PCMCIA card: the interrupt field
 * must be 3, which is 0x3000, and the reserved field must be 0xf, which is
 * 0x0f00.  The card's interrupt reaches the Amiga through Gayle whatever the
 * number, so the 3 is a value the part demands rather than one that means
 * anything here.
 */
#define EL3_RC_IRQ_SHIFT        12
#define EL3_RC_RESERVED         0x0f00
#define EL3_RC_PCMCIA           ((3u << EL3_RC_IRQ_SHIFT) | EL3_RC_RESERVED)

/*
 * The EEPROM, which on this part is not bit-banged: one word write says read
 * word N, and the answer appears in the data register when the busy bit
 * clears.  A read is quoted at 162 us.
 */
#define EL3_EE_BUSY             0x8000
#define EL3_EE_READ             0x0080  /* opcode 10 in bits 7..6            */
#define EL3_EE_ADDR_MASK        0x003f

#define EL3_EE_NODE_ADDR_0      0x00    /* station address, three words      */
#define EL3_EE_PRODUCT_ID       0x03
#define EL3_EE_MFG_ID           0x07
#define EL3_EE_ADDR_CFG         0x08
#define EL3_EE_RESOURCE_CFG     0x09
#define EL3_EE_OEM_ADDR_0       0x0a    /* the OEM address, three words      */

/* --------------------------------------------------------- window 1 ------ */

/*
 * The operating window.  One PIO port carries every frame in both
 * directions: writes push transmit data, reads pull receive data, and the
 * port does not advance an address -- the chip does.
 */
#define EL3_W1_FIFO             0x00
#define EL3_W1_RX_STATUS        0x08
#define EL3_W1_TIMER            0x0a    /* byte, read-only                   */
#define EL3_W1_TX_STATUS        0x0b    /* byte, write any value to pop      */
#define EL3_W1_TX_FREE          0x0c

/*
 * Receive status.  Length is only meaningful once INCOMPLETE is clear.
 * ERROR set means bits 13..11 are the reason; ERROR clear means they are
 * either nothing or the dribble-bits note, which is not a reason to drop a
 * frame.
 */
#define EL3_RXS_INCOMPLETE      0x8000
#define EL3_RXS_ERROR           0x4000
#define EL3_RXS_ERR_MASK        0x3800
#define EL3_RXS_ERR_SHIFT       11
#define EL3_RXS_LEN_MASK        0x07ff

#define EL3_RXE_OVERRUN         0
#define EL3_RXE_RUNT            3
#define EL3_RXE_ALIGN           4
#define EL3_RXE_CRC             5
#define EL3_RXE_OVERSIZE        1

/*
 * Transmit status, a byte at an odd offset, and the one register in this file
 * that is not a word.
 *
 * It is a 31-deep stack: read the byte, write it back to pop, and repeat until
 * the read is zero.  Any error bit disables the transmitter, so TX Enable must
 * follow.  Jabber and underrun need a TX Reset before that.
 */
#define EL3_TXS_COMPLETE        0x80
#define EL3_TXS_INT_REQ         0x40
#define EL3_TXS_JABBER          0x20
#define EL3_TXS_UNDERRUN        0x10
#define EL3_TXS_MAX_COLLISION   0x08
#define EL3_TXS_OVERFLOW        0x04
#define EL3_TXS_FATAL           (EL3_TXS_JABBER | EL3_TXS_UNDERRUN)

#define EL3_TX_FREE_MASK        0x7fff

/* --------------------------------------------------------- window 2 ------ */

/* Six byte registers, written as three words, low octet first. */
#define EL3_W2_ADDR_0           0x00

/* --------------------------------------------------------- window 4 ------ */

#define EL3_W4_NET_DIAG         0x06
#define EL3_W4_MEDIA            0x0a

/* Media type and status.  The three enables are what a driver writes, and the
   rest is what the transceiver reports. */
#define EL3_MEDIA_TP_ENABLED    0x8000
#define EL3_MEDIA_COAX_ENABLED  0x4000
#define EL3_MEDIA_LINK_BEAT     0x0800
#define EL3_MEDIA_JABBER        0x0200
#define EL3_MEDIA_UNSQUELCH     0x0100
#define EL3_MEDIA_LINK_ENABLE   0x0080
#define EL3_MEDIA_JABBER_ENABLE 0x0040
#define EL3_MEDIA_SQE_ENABLE    0x0008

/* Net diagnostics.  Bits 5..1 are the ASIC revision: 1 is a 3C589, 2 a
   3C589B, and the difference matters to a reader and not to this driver. */
#define EL3_ND_REV_MASK         0x003e
#define EL3_ND_REV_SHIFT        1

/* ------------------------------------------------------------- framing --- */

/*
 * A transmit is two preamble words and then the body.
 *
 * Word one is the length in bytes in its low eleven bits.  Word two is a
 * don't-care that the manual asks be written as zero.  The body is then padded
 * to a four-byte boundary, and the pad bytes are in the FIFO but not in the
 * length.  Frames under the Ethernet minimum are padded by the card and must
 * not be padded here.
 */
#define EL3_TX_LEN_MASK         0x07ff
#define EL3_TX_INT_ON_DONE      0x8000

/*
 * Every command that does not finish inside its own I/O cycle, and the global
 * reset, which is worse.  On the 3C589 the busy bit is not visible while a
 * global reset runs, so the only option is to wait out the millisecond the
 * manual asks for.
 */
#define EL3_RESET_US            1000

#endif /* AMINETXDUO_EL3REG_H */

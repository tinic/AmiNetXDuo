/*
 * anxgenet.device: what attach, init and the filter decide, with no register
 * behind it.
 *
 * genet.c reads and writes the MAC in the order NetBSD's driver does; what
 * it decides between the writes -- the revision, whether the multicast table
 * fits the filter, the MDF enable mask, the GIC-400's line bits -- is here,
 * where a host test can hold it to a bit pattern.  Nothing here runs on a
 * frame.  The ring's configuration words are genet_ring.h's, inline, so a
 * constant ring size stays a constant; the six address bytes are shuffled
 * into their registers in genet.c, on the field itself, which is what lets
 * the 68k move them as one longword and one word.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_GENET_WORDS_H
#define AMINETXDUO_GENET_WORDS_H

#include <exec/types.h>

#include "netdev_mcast.h"

/* --------------------------------------------------------------- attach --- */

/* SYS_REV_CTRL's major field as the reference driver reads it: 0 means 1,
   and 5 and 6 read one lower, so the Pi 4's 6 is the GENET v5 this core
   drives. */
ULONG genet_rev_major(ULONG rev);

/* --------------------------------------------------------------- filter --- */

/* The UniMAC filters on GENET_MAX_MDF_FILTER exact addresses.  Slot 0 is
   broadcast, slot 1 our own address, and the unit's referenced multicast
   addresses follow; TRUE when they all fit. */
BOOL  genet_mdf_fits(const NetdevMcast *table, UWORD max);

/* UMAC_MDF_CTRL with the first `slots` slots enabled: slot k is enabled by
   bit (GENET_MAX_MDF_FILTER - 1 - k). */
ULONG genet_mdf_ctrl_word(UWORD slots);

/* ------------------------------------------------------------------ GIC --- */

/* The GIC-400 distributor's registers, for clearing this driver's own line. */
#define GICD_IIDR           0x008
#define GICD_ISENABLER      0x100
#define GICD_ICPENDR        0x280
#define GICD_ISACTIVER      0x300
#define GICD_ICACTIVER      0x380
#define GICD_IIDR_GIC400    0x0200043BUL    /* ProductID 0x020, ARM      */
#define GICD_IIDR_MASK      0xFF000FFFUL

/* The byte offset into a per-line bitmap register bank (32 lines a word)
   and the bit within it for interrupt `irq`. */
ULONG genet_gicd_bank_offset(ULONG irq);
ULONG genet_gicd_bit(ULONG irq);

/* TRUE when a GICD_IIDR word names a GIC-400. */
BOOL  genet_gicd_is_gic400(ULONG iidr);

#endif /* AMINETXDUO_GENET_WORDS_H */

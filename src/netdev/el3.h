/*
 * anxnet.device: the EtherLink III core's own surface.
 *
 * el3_answers() is the only entry netdev_pcmcia.c uses, and it exists so that
 * the slot's chip test can be chip-switched without netdev_pcmcia.c learning
 * a single EtherLink III register.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_EL3_H
#define AMINETXDUO_EL3_H

#include "netdev_nic.h"
#include "netdev_cards.h"

/*
 * Is an EtherLink III decoding at this row's register base?  Addresses the
 * card off the row, because the slot has to be identified before there is a
 * unit to identify it into.  Accepts the manufacturer ID in either byte
 * order; which order it was is measured again, into the unit, by el3_attach().
 */
BOOL  el3_answers(const NetdevCard *card);

LONG  el3_attach(NetdevNic *nic);
LONG  el3_init(NetdevNic *nic);
VOID  el3_halt(NetdevNic *nic);
LONG  el3_tx(NetdevNic *nic, const UBYTE *frame, UWORD len);
VOID  el3_setfilter(NetdevNic *nic);
BOOL  el3_intr(NetdevNic *nic);
VOID  el3_reset(NetdevNic *nic);

/* Pop the transmit status stack until it reads zero, recovering the
   transmitter from whatever each entry says stopped it. */
VOID  el3_drain_tx_status(NetdevNic *nic);

#endif /* AMINETXDUO_EL3_H */

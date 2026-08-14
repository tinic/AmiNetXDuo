/*
 * anxnet.device: the two things a caller can say that a stock SANA-II
 * OpenDevice() cannot.
 *
 * One device file covers the whole NE2000/DP8390 card family, so the card is
 * chosen at open time rather than by which file is opened.  There are two ways
 * to say which, and they answer different questions:
 *
 *   UNIT alone         unit N is the Nth supported board in Zorro autoconfig
 *                      order.  UNIT=0 is right on a one-card machine and needs
 *                      nothing else.
 *
 *   UNIT >= 100        (card index + 1) * 100 + instance, so 100 is the first
 *                      X-Surf 100, 300 the first Ariadne II.  Works through
 *                      any stack, because it is just a unit number.
 *
 *   S2_AnxCardType     ti_Data is a card name, "xsurf100" / "ariadne2" / ...,
 *                      passed on the OpenDevice() buffer-management tag list.
 *                      Readable, and the shape a config keyword maps onto.
 *
 * Whichever is used, a pin that does not match the hardware fails the open.
 * It never falls through to a different board: a configuration that silently
 * binds to the wrong card is the failure this driver exists to remove.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_ANXNET_H
#define AMINETXDUO_ANXNET_H

#define ANXNET_DEVICE_NAME      "anxnet.device"

/* S2_Dummy is (TAG_USER + 0xB0000); Commodore used +1..+3, and +4/+5 are the
   widely-deployed CopyToBuff16 pair, so this sits clear of both. */
#define S2_AnxCardType          (TAG_USER + 0xB0000 + 0x40)

/* unit = (card index + 1) * ANXNET_UNIT_PIN + instance */
#define ANXNET_UNIT_PIN         100

/*
 * Every name S2_AnxCardType accepts, in netdev_cards.c row order, so the Nth
 * entry here is the card ANXNET_UNIT_PIN * (N + 1) names.
 *
 * Here and not only in the table because a config parser has to reject
 * CARD=nonsense without linking the driver: src/config/config_parse.c reads
 * this, src/netdev/test/test_netdev_cards.c fails if the two ever drift.
 */
#define ANXNET_CARD_NAMES \
    { "xsurf100", "xsurf", "ariadne2", "hydra", "lanrover", "pcmcia" }

#endif /* AMINETXDUO_ANXNET_H */

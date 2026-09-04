/*
 * anxnet.device: the card-pinning unit numbers and open tag.
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
 * Every name S2_AnxCardType accepts, and must stay in netdev_cards.c row
 * order: the Nth entry is the card ANXNET_UNIT_PIN * (N + 1) names.
 */
#define ANXNET_CARD_NAMES \
    { "xsurf100", "xsurf", "ariadne2", "hydra", "lanrover", "a2065", \
      "ariadne", "pcmcia", "xsurf500", "3c589", "3ccfem556", "3cxem556" }

#endif /* AMINETXDUO_ANXNET_H */

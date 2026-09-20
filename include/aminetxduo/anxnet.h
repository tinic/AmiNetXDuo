/*
 * anxnet.device: the card-pinning unit numbers and open tag.
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_ANXNET_H
#define AMINETXDUO_ANXNET_H

#define ANXNET_DEVICE_NAME      "anxnet.device"
/* The same driver core with only the Pi 4's GENET in it (netdev_roster.h). */
#define ANXGENET_DEVICE_NAME    "anxgenet.device"

/* Driver-private OpenDevice tag.  It has its own TAG_USER identity rather
   than borrowing the S2_Dummy namespace used by SANA-II buffer hooks. */
#define ANXD_S2_CARD_TYPE       (0x80000000UL | 0x00414e43UL) /* 'ANC' */

/* unit = (card index + 1) * ANXNET_UNIT_PIN + instance */
#define ANXNET_UNIT_PIN         100

/*
 * Every name ANXD_S2_CARD_TYPE accepts, and must stay in netdev_cards.c row
 * order: the Nth entry is the card ANXNET_UNIT_PIN * (N + 1) names.
 */
#define ANXNET_CARD_NAMES \
    { "xsurf100", "xsurf", "ariadne2", "hydra", "lanrover", "a2065", \
      "ariadne", "pcmcia", "xsurf500", "3c589", "3ccfem556", "3cxem556", \
      "genet", "zz9000", "zz9000z2" }

#endif /* AMINETXDUO_ANXNET_H */

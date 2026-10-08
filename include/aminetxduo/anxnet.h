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
#define ANXNET_CARD_NAME_LIST(X) \
    X(xsurf100) X(xsurf) X(ariadne2) X(hydra) X(lanrover) X(a2065) \
    X(ariadne) X(pcmcia) X(xsurf500) X(3c589) X(3ccfem556) X(3cxem556) \
    X(genet) X(zz9000) X(zz9000z2)

#define ANXNET_CARD_NAME_STRING(name) #name,
#define ANXNET_CARD_NAMES \
    { ANXNET_CARD_NAME_LIST(ANXNET_CARD_NAME_STRING) }

#endif /* AMINETXDUO_ANXNET_H */

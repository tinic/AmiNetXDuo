/*
 * anxnet.device: the card-pinning unit numbers and open tag.
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_ANXNET_H
#define AMINETXDUO_ANXNET_H

#include <exec/types.h>

#define ANXNET_DEVICE_NAME      "anxnet.device"
/* The same driver core with only the Pi 4's GENET in it (netdev_roster.h). */
#define ANXGENET_DEVICE_NAME    "anxgenet.device"

/* Driver-private OpenDevice tag.  It has its own TAG_USER identity rather
   than borrowing the S2_Dummy namespace used by SANA-II buffer hooks. */
#define ANXD_S2_CARD_TYPE       (0x80000000UL | 0x00414e43UL) /* 'ANC' */

/* Optional OpenDevice tag pointing to AnxdLinkSpeed. Zero Accepted before
 * OpenDevice and require nonzero afterwards: older drivers ignore the tag.
 * The fixed record is two native ULONGs (8 bytes on m68k), valid through
 * OpenDevice only. Mbps is 10 or 0 (restore normal advertised modes).
 * The selected PHY retains autonegotiation, duplex and PAUSE. A conflicting
 * request on an already-open unit fails; last Close restores the defaults. */
#define ANXD_S2_LINK_SPEED      (0x80000000UL | 0x00414e4cUL) /* 'ANL' */
typedef struct AnxdLinkSpeed
{
    ULONG Mbps;
    ULONG Accepted;
} AnxdLinkSpeed;

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

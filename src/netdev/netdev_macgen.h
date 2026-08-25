/*
 * anxnet.device: a station address for a card that has none -- a
 * locally-administered address derived from a fingerprint of the machine and
 * the card, the same on every boot of one machine and different between
 * machines that differ in anything the fingerprint covers.  It cannot separate
 * two machines identical in every input; HARDWAREADDRESS is the override.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_NETDEV_MACGEN_H
#define AMINETXDUO_NETDEV_MACGEN_H

#include <exec/types.h>

#include "netdev_mcaf.h"        /* NETDEV_ADDR_LEN, and the CRC-32 it uses */

/* Enough for the ExecBase fields, four memory regions, four boards and the
   card's CIS.  netdev_mac_fingerprint() stops at whatever it is given. */
#define NETDEV_MAC_FP_MAX   160

/*
 * Octet 0 of a derived address: bit 1 set is locally administered, bit 0 clear
 * is unicast.  Octet 1 is a fixed marker, so an address this driver invented is
 * recognisable in a capture and is not mistaken for a real vendor's.
 */
#define NETDEV_MAC_LOCAL    0x02u
#define NETDEV_MAC_TAG      0xadu

/* TRUE if the chip can be programmed with this: not all-zero, not all-ones,
   and not a group address. */
BOOL netdev_mac_usable(const UBYTE *mac);

/* Deterministic locally-administered unicast address from `len` fingerprint
   bytes.  Same bytes in, same address out, on any host and every boot. */
VOID netdev_mac_derive(const UBYTE *fp, UWORD len, UBYTE *mac);

/*
 * The fingerprint itself.  In netdev_device.c rather than here because it reads
 * ExecBase and the autoconfig board list.  `salt` is the caller's card-specific
 * part.  Returns the number of bytes written.
 */
UWORD netdev_mac_fingerprint(UBYTE *buf, UWORD max, ULONG salt);

/*
 * The CIS's own LAN node ID, when the card in the PCMCIA slot carries one.  In
 * netdev_pcmcia.c.  FALSE for every other bus, and for a card whose CIS does
 * not offer it.  Preferred over anything derived: it is the assigned address.
 */
BOOL netdev_mac_cis_node_id(UBYTE *mac);

#endif /* AMINETXDUO_NETDEV_MACGEN_H */

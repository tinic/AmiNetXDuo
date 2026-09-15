/*
 * anxnet.device / anxgenet.device: which buses and chip cores this image
 * carries.
 *
 * Two devices are built from this directory.  anxnet.device is every Amiga
 * card: Zorro boards, the A600/A1200 PCMCIA slot, the fixed-address X-Surf
 * 500.  anxgenet.device is the Raspberry Pi 4's GENET behind a PiStorm32,
 * named by Emu68's device tree, and nothing else.  They were one image until
 * 2026-09-15, when the GENET core took it from 39 KB to 48 KB of RAM on every
 * machine, most of which have no Pi in them.  The core -- the AmigaOS device,
 * the SANA-II commands, the queues, the probe record -- is the same source
 * compiled twice; only the card table, the bus probes and the chip cores
 * differ, and this header is the one place that says which.
 *
 * A build that names no roster is everything, which is what the host tests
 * compile against.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_NETDEV_ROSTER_H
#define AMINETXDUO_NETDEV_ROSTER_H

#if defined(NETDEV_ROSTER_CLASSIC)
#define NETDEV_HAS_ZORRO    1
#define NETDEV_HAS_PCMCIA   1
#define NETDEV_HAS_FIXED    1
#define NETDEV_HAS_DTREE    0
#elif defined(NETDEV_ROSTER_GENET)
#define NETDEV_HAS_ZORRO    0
#define NETDEV_HAS_PCMCIA   0
#define NETDEV_HAS_FIXED    0
#define NETDEV_HAS_DTREE    1
#else
#define NETDEV_HAS_ZORRO    1
#define NETDEV_HAS_PCMCIA   1
#define NETDEV_HAS_FIXED    1
#define NETDEV_HAS_DTREE    1
#endif

/* The Amiga-card cores (DP8390 family, LANCE, EtherLink III) ride with any
   of the three Amiga buses; the GENET core with the tree. */
#define NETDEV_HAS_CLASSIC  (NETDEV_HAS_ZORRO || NETDEV_HAS_PCMCIA || NETDEV_HAS_FIXED)

#ifndef NETDEV_DEVICE_NAME
#define NETDEV_DEVICE_NAME  ANXNET_DEVICE_NAME
#endif

#endif /* AMINETXDUO_NETDEV_ROSTER_H */

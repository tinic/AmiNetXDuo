/*
 * anxnet.device: a board named by a device tree.
 *
 * Emu68 publishes the Pi's flattened device tree through devicetree.resource,
 * with every bus range rewritten into the 68k address space.  A
 * NETDEV_BUS_DTREE row is found by its `compatible` string and everything
 * else the shell needs -- where the registers are, which interrupt, what the
 * station address is -- is read from the same node.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_NETDEV_DTREE_H
#define AMINETXDUO_NETDEV_DTREE_H

#include <exec/types.h>
#include <exec/interrupts.h>

typedef struct NetdevDtInfo
{
    ULONG   base;           /* the register window, a 68k address           */
    ULONG   size;
    ULONG   irq;            /* the controller's number: SPI + 32; 0 = none  */
    UBYTE   mac[6];
    UBYTE   mac_ok;         /* local-mac-address was there and non-zero     */
    UBYTE   phy;            /* the PHY's MDIO address, 0xff = not stated    */
} NetdevDtInfo;

/*
 * The first node whose `compatible` names `compat` and whose `status` is not
 * "disabled".  FALSE when there is no devicetree.resource -- which is every
 * machine that is not an Emu68 -- or no such node, or its `reg` could not be
 * translated into a 68k address.
 */
BOOL netdev_dtree_find(const char *compat, NetdevDtInfo *out);

/* Small, read-only discovery primitives used by the Installer's Emu68
 * probe.  They inspect the same tree as the driver without opening a SANA-II
 * device or touching any peripheral registers. */
BOOL netdev_dtree_present(VOID);
BOOL netdev_dtree_root_compatible(const char *compat);
BOOL netdev_dtree_alias_present(const char *alias);

/*
 * A fixed address on the bus the node `bus` (a path, "/soc") gives its
 * children, translated to a 68k address.  For what the tree does not name:
 * Emu68's tree has no node for the SoC's system timer, which it keeps for
 * itself, and the BCM283x/2711 peripheral map puts it at 0x7e003000 on /soc
 * whatever the tree says.  FALSE with no tree, no such node or no range.
 */
BOOL netdev_dtree_bus_addr(const char *bus, ULONG addr, ULONG *out);

/*
 * Whether [addr, addr + len) lies inside what the tree's memory nodes call
 * physical RAM -- the only RAM a bus master's DMA can be pointed at.  FALSE
 * with no tree at all.
 */
BOOL netdev_dtree_ram_covers(ULONG addr, ULONG len);

/*
 * The interrupt half: a server on the GIC through gic400.library, which is
 * what Emu68 delivers the Pi's peripheral interrupts through.  The library is
 * opened on the first add and closed with the last remove.  FALSE when the
 * library is not there, in which case the vertical-blank poll is all the unit
 * gets.
 */
BOOL netdev_dtree_int_add(ULONG irq, struct Interrupt *is);
BOOL netdev_dtree_int_rem(ULONG irq, struct Interrupt *is);

#endif /* AMINETXDUO_NETDEV_DTREE_H */

/*
 * Read-only identity of the PC Card in the A600/A1200 slot, for
 * InstallNetProbe and CheckNetDevice.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef NETPROBE_PCMCIA_H
#define NETPROBE_PCMCIA_H

#include "netprobe_card.h"

/*
 * Fills *slot.  Card detect first: an empty slot is not owned.  A free slot
 * is owned with CARDF_IFAVAILABLE, read with CopyTuple() and released; a slot
 * another driver owns is read from attribute memory (CopyTuple() needs the
 * owner's handle).  No reset, COR write, Gayle write or device open occurs.
 * No card.resource (Kickstart without a PCMCIA slot) gives NETPROBE_SLOT_NONE.
 */
void netprobe_pcmcia_read(NetprobeSlot *slot);

#endif /* NETPROBE_PCMCIA_H */

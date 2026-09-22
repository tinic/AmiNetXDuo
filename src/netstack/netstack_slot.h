/*
 * AmiNetXDuo, which interface slot a newcomer lands in.
 *
 * nx_ip_interface_attach() takes the first free slot, and the SANA-II side
 * has to know that slot before the attach reaches the driver, so the scan is
 * repeated here over a picture of the tables.  When nothing is free, a slot
 * the start-up pass took on its own may be given up for an interface somebody
 * NAMED; this file says which one, and nothing more.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_NETSTACK_SLOT_H
#define AMINETXDUO_NETSTACK_SLOT_H

#include <exec/types.h>

typedef struct AmiNsSlot
{
    BOOL  attached;     /* NetX Duo has an interface in it (nx_interface_valid) */
    BOOL  held;         /* an open SANA-II device sits in it (ns_Iface[])       */
    BOOL  wanted;       /* something NAMED it; the start-up pass leaves it clear */
    UWORD claims;       /* live users pinning it (ns_IfaceClaims[])             */
} AmiNsSlot;

/*
 * The first slot with no interface, no device and no claim, over `count`
 * slots -- the same first-free scan nx_ip_interface_attach() does.  -1 when
 * every slot is taken.
 */
LONG ami_ns_slot_vacant(const AmiNsSlot *slot, UWORD count);

/*
 * A slot that could be made vacant for an interface that was NAMED, and which
 * one.  Only a slot the start-up pass claimed on its own may be offered, last
 * slot first, and never one something holds a claim on.  A candidate, not a
 * casualty: nothing is taken down here.  -1 when nothing may yield.
 */
LONG ami_ns_slot_yield_candidate(const AmiNsSlot *slot, UWORD count);

#endif /* AMINETXDUO_NETSTACK_SLOT_H */

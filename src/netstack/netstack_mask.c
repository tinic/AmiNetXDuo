/*
 * AmiNetXDuo, the mask a static interface carries when its file omits NETMASK.
 *
 * SPDX-License-Identifier: MIT
 */

#include "netstack_mask.h"

ULONG ami_ns_static_netmask(ULONG netmask)
{
    return (netmask != 0UL) ? netmask : 0xFFFFFF00UL;
}

#ifdef AMINETXDUO_PTP
ULONG ami_ns_static_netmask_ptp(ULONG netmask, BOOL ptp)
{
    if (ptp && netmask == 0UL)
        return 0xFFFFFFFFUL;
    return ami_ns_static_netmask(netmask);
}
#endif

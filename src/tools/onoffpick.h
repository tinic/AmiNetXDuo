/*
 * Online/Offline: which configured interface a driver name means, kept apart
 * so the host can test it.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef AMINETXDUO_ONOFFPICK_H
#define AMINETXDUO_ONOFFPICK_H

/*
 * units[0..n-1] are the units of the configured interfaces whose driver is
 * the one named, in directory order.  With UNIT given, the interface on that
 * unit; without it, the driver's only interface, whatever its unit (F-172:
 * it compared unit 0, so a sole driver on unit 1 was "nothing here").
 * Returns how many interfaces qualify and sets *pick to the first; the
 * caller takes 1 as the answer and more than 1 as "give UNIT".
 */
static inline int onoff_pick(int had_unit, unsigned long want,
                             const unsigned long *units, int n, int *pick)
{
    int matches = 0;
    int i;

    *pick = -1;

    for (i = 0; i < n; i++)
    {
        if (had_unit && units[i] != want)
            continue;

        if (matches++ == 0)
            *pick = i;
    }

    return matches;
}

/*
 * Whether the interface is already where Online (`up`) or Offline asked for
 * it.  `linkup` is the stack's link, `device` the SANA-II device's online
 * state.  Offline is done only when the device is offline as well: STATE=DOWN
 * and SM_Down leave the link down and the device online, still receiving.
 */
static inline int onoff_already(int up, int linkup, int device)
{
    return up ? linkup : (!linkup && !device);
}

#endif

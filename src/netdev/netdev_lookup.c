/*
 * anxnet.device: which unit an OpenDevice() names.
 *
 * The two decisions Open() makes before it touches anything: which of the
 * probed units a unit number and a CARD= pin refer to, and whether a request
 * that matched none of them could be answered by the PCMCIA slot.  Both were
 * static in netdev_device.c, which the romtag asm at its head keeps off every
 * host; here they read the card table and the unit roster and nothing else,
 * so src/netdev/test/test_netdev_lookup.c can drive them with a roster it
 * builds.  The Expansion walk, the slot claim and the semaphore stay in
 * netdev_device.c.
 *
 * SPDX-License-Identifier: MIT
 */

#include "netdev_internal.h"

/*
 * A unit below ANXNET_UNIT_PIN is a position in the probe order.  At or above
 * it, the number names a card type: (index + 1) * 100 + instance.
 */
NetdevUnit *netdev_find_unit(NetdevDevice *dev, ULONG unit,
                             const char *pin_name, const char **why)
{
    const NetdevCard *want = NULL;
    UWORD             instance = 0;
    UWORD             seen = 0;
    UWORD             i;

    if (pin_name != NULL)
    {
        want = netdev_card_by_name(pin_name);
        if (want == NULL)
        {
            *why = "no such card type";
            return NULL;
        }
        instance = (UWORD)unit;
        if (unit >= ANXNET_UNIT_PIN)
            instance = (UWORD)(unit % ANXNET_UNIT_PIN);
    }
    else if (unit >= ANXNET_UNIT_PIN)
    {
        ULONG idx = (unit / ANXNET_UNIT_PIN) - 1;

        /* Range-checked as a ULONG: truncating first wraps a huge unit
           number onto a valid card index, which is the one thing a pin
           must never do. */
        if (idx >= (ULONG)netdev_card_count)
        {
            *why = "no such card type";
            return NULL;
        }
        want     = &netdev_cards[idx];
        instance = (UWORD)(unit % ANXNET_UNIT_PIN);
    }
    else
    {
        if (unit >= dev->nd_UnitCount)
        {
            *why = "no such board";
            return NULL;
        }
        return &dev->nd_Units[unit];
    }

    for (i = 0; i < dev->nd_UnitCount; i++)
    {
        if (dev->nd_Units[i].nu_Nic.card == want)
        {
            if (seen == instance)
                return &dev->nd_Units[i];
            seen++;
        }
    }

    *why = "the pinned card is not in this machine";
    return NULL;
}

/*
 * Device initialization cannot leave an IFAVAILABLE handle queued for an empty
 * slot, so a card inserted after the romtag probe needs one task-context retry.
 * PCMCIA is deliberately last in probe order.
 */
#if NETDEV_HAS_PCMCIA
BOOL netdev_request_is_pcmcia(NetdevDevice *dev, ULONG unit,
                              const char *pin_name,
                              const NetdevCard **wanted)
{
    const NetdevCard *card = NULL;

    *wanted = NULL;
    if (pin_name != NULL)
    {
        card = netdev_card_by_name(pin_name);
        if (card == NULL || card->bus != NETDEV_BUS_PCMCIA)
            return FALSE;
        *wanted = card;
        return TRUE;
    }

    if (unit >= ANXNET_UNIT_PIN)
    {
        ULONG idx = (unit / ANXNET_UNIT_PIN) - 1;

        if (idx >= (ULONG)netdev_card_count)
            return FALSE;
        card = &netdev_cards[idx];
        if (card->bus != NETDEV_BUS_PCMCIA)
            return FALSE;
        *wanted = card;
        return TRUE;
    }

    return (BOOL)(unit == (ULONG)dev->nd_UnitCount);
}
#endif /* NETDEV_HAS_PCMCIA */

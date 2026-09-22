/*
 * AmiNetXDuo, what the interface files' IPTYPE lines add up to.
 *
 * SPDX-License-Identifier: MIT
 */

#include "netstack_iptype.h"

BOOL ami_ns_iptype_any(const AmiIfConfig *iface, UWORD count, AmiIpType type)
{
    UWORD i;

    for (i = 0; i < count; i++)
    {
        if (iface[i].iptype == type)
            return TRUE;
    }

    return FALSE;
}

BOOL ami_ns_iptype_any_ipv4(const AmiIfConfig *iface, UWORD count)
{
    UWORD i;

    for (i = 0; i < count; i++)
    {
        if (ami_config_iface_wants_ipv4(&iface[i]))
            return TRUE;
    }

    return FALSE;
}

LONG ami_ns_iptype_autoip_slot(const AmiIfConfig *iface, UWORD count,
                               LONG requested)
{
    UWORD i;

    if (requested >= 0)
    {
        if ((ULONG)requested >= (ULONG)count ||
            !ami_config_iface_wants_ipv4(&iface[requested]))
            return -1;

        return requested;
    }

    for (i = 0; i < count; i++)
    {
        if (iface[i].iptype == AMI_IPTYPE_LINKLOCAL)
            return (LONG)i;
    }

    for (i = 0; i < count; i++)
    {
        if (ami_config_iface_wants_ipv4(&iface[i]))
            return (LONG)i;
    }

    return -1;
}

BOOL ami_ns_iptype_linklocal(ULONG addr)
{
    return (addr >= 0xA9FE0000UL && addr <= 0xA9FEFFFFUL) ? TRUE : FALSE;
}

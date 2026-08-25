/*
 * AmiNetXDuo, DHCP option-12 hostname ownership.
 *
 * SPDX-License-Identifier: MIT
 */

#include "netstack_dhcp_hostname.h"


static VOID ami_ns_hostname_copy(char *dst, const char *src)
{
    UWORD i = 0U;

    if (src != NULL)
        while (i + 1U < (UWORD)AMI_CFG_NAME_LEN && src[i] != '\0')
        {
            dst[i] = src[i];
            i++;
        }
    dst[i] = '\0';
}


static BOOL ami_ns_hostname_same(const char *a, const char *b)
{
    UWORD i = 0U;

    while (a[i] == b[i])
    {
        if (a[i] == '\0')
            return TRUE;
        i++;
    }
    return FALSE;
}


VOID ami_ns_dhcp_hostname_update(AmiNsDhcpHostnameState *state,
                                 UWORD interface_index,
                                 const char *hostname)
{
    if (state == NULL || interface_index >= AMI_CFG_MAX_ATTACHED)
        return;

    ami_ns_hostname_copy(state->lease[interface_index], hostname);
}


BOOL ami_ns_dhcp_hostname_decode(char out[AMI_CFG_NAME_LEN],
                                 const UBYTE *raw, ULONG length)
{
    ULONG i;

    if (out == NULL)
        return FALSE;
    out[0] = '\0';

    if (raw == NULL || length == 0UL ||
        length >= (ULONG)AMI_CFG_NAME_LEN)
        return FALSE;

    for (i = 0UL; i < length; i++)
    {
        if (raw[i] == 0U)
        {
            out[0] = '\0';
            return FALSE;
        }
        out[i] = (char)raw[i];
    }
    out[length] = '\0';

    if (!ami_config_hostname_valid(out))
    {
        out[0] = '\0';
        return FALSE;
    }
    return TRUE;
}


BOOL ami_ns_dhcp_hostname_reconcile(AmiConfig *config,
                                    AmiNsDhcpHostnameState *state)
{
    const char *offered = NULL;
    UWORD       iface;

    if (config == NULL || state == NULL)
        return FALSE;

    for (iface = 0U; iface < (UWORD)AMI_CFG_MAX_ATTACHED; iface++)
        if (state->lease[iface][0] != '\0')
        {
            offered = state->lease[iface];
            break;
        }

    if (state->owner[0] != '\0')
    {
        if (config->hostname_source != (UWORD)AMI_HOSTNAME_DHCP ||
            !ami_ns_hostname_same(config->hostname, state->owner))
        {
            /* A direct writer or a stronger source displaced this lease. */
            ami_ns_dhcp_hostname_displace(state);
            return FALSE;
        }

        if (offered != NULL)
        {
            if (ami_ns_hostname_same(state->owner, offered))
                return FALSE;

            if (!ami_config_hostname_offer(config, (UWORD)AMI_HOSTNAME_DHCP,
                                           offered))
                return FALSE;

            ami_ns_hostname_copy(state->owner, offered);
            return TRUE;
        }

        /* The last owning lease went away. Restore exactly what it replaced,
           including the generated-card name whose source rank is NONE. */
        ami_ns_hostname_copy(config->hostname, state->fallback);
        config->hostname_source = state->fallback_source;
        ami_ns_dhcp_hostname_displace(state);
        return TRUE;
    }

    if (offered == NULL ||
        config->hostname_source >= (UWORD)AMI_HOSTNAME_DHCP)
        return FALSE;

    ami_ns_hostname_copy(state->fallback, config->hostname);
    state->fallback_source = config->hostname_source;

    if (!ami_config_hostname_offer(config, (UWORD)AMI_HOSTNAME_DHCP, offered))
    {
        state->fallback[0] = '\0';
        state->fallback_source = (UWORD)AMI_HOSTNAME_NONE;
        return FALSE;
    }

    ami_ns_hostname_copy(state->owner, offered);
    return TRUE;
}


VOID ami_ns_dhcp_hostname_displace(AmiNsDhcpHostnameState *state)
{
    if (state == NULL)
        return;

    state->owner[0] = '\0';
    state->fallback[0] = '\0';
    state->fallback_source = (UWORD)AMI_HOSTNAME_NONE;
}

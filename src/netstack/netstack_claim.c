/*
 * AmiNetXDuo, stable claims on live interface slots.
 *
 * SPDX-License-Identifier: MIT
 */

#include "netstack_internal.h"

/* Interface names are DEVS:NetInterfaces file names and therefore compare
   case-insensitively on AmigaOS. */
BOOL ami_ns_same_name(const char *a, const char *b)
{
    ULONG i;

    for (i = 0; ; i++)
    {
        char ca = a[i];
        char cb = b[i];

        if (ca >= 'A' && ca <= 'Z')
            ca = (char)(ca + ('a' - 'A'));
        if (cb >= 'A' && cb <= 'Z')
            cb = (char)(cb + ('a' - 'A'));

        if (ca != cb)
            return FALSE;
        if (ca == '\0')
            return TRUE;
    }
}

/* Once the count is raised the slot cannot be detached and reused until the
   matching release. */
LONG netstack_interface_claim(const char *name, UWORD *index_out)
{
    AmiNetStack *ns;
    LONG         rc = AMI_NET_ERR_STATE;
    UWORD        i;

    if (name == NULL || name[0] == '\0' || index_out == NULL)
        return AMI_NET_ERR_CONFIG;

    ami_ns_lock_obtain();

    ns = ami_netstack_raw();
    if (ns != NULL && ns->ns_IpCreated)
    {
        for (i = 0; i < (UWORD)AMI_CFG_MAX_ATTACHED; i++)
        {
            UWORD cfg_index;

            if (ns->ns_Iface[i] == NULL)
                continue;

            cfg_index = ns->ns_IfaceCfg[i];
            if (cfg_index >= (UWORD)AMI_CFG_MAX_ATTACHED ||
                !ns->ns_Config.interfaces[cfg_index].configured ||
                !ami_ns_same_name(ns->ns_Config.interfaces[cfg_index].name,
                                  name))
                continue;

            if (ns->ns_IfaceClaims[i] == (UWORD)-1)
            {
                rc = AMI_NET_ERR_BUSY;
                break;
            }

            ns->ns_IfaceClaims[i]++;
            *index_out = i;
            rc = AMI_NET_OK;
            break;
        }
    }

    ami_ns_lock_release();
    return rc;
}

#ifdef AMINETXDUO_BPF
/* The BPF table treats its SANA-II pointer as an opaque cookie. */
LONG ami_netstack_interface_claim_cookie(APTR cookie, UWORD *index_out)
{
    AmiNetStack *ns;
    LONG         rc = AMI_NET_ERR_STATE;
    UWORD        i;

    if (cookie == NULL || index_out == NULL)
        return AMI_NET_ERR_CONFIG;

    ami_ns_lock_obtain();

    ns = ami_netstack_raw();
    if (ns != NULL && ns->ns_IpCreated)
    {
        for (i = 0; i < (UWORD)AMI_CFG_MAX_ATTACHED; i++)
        {
            if ((APTR)ns->ns_Iface[i] != cookie)
                continue;

            if (ns->ns_IfaceClaims[i] == (UWORD)-1)
                rc = AMI_NET_ERR_BUSY;
            else
            {
                ns->ns_IfaceClaims[i]++;
                *index_out = i;
                rc = AMI_NET_OK;
            }
            break;
        }
    }

    ami_ns_lock_release();
    return rc;
}
#endif

VOID netstack_interface_release(UWORD index)
{
    AmiNetStack *ns;

    ami_ns_lock_obtain();

    ns = ami_netstack_raw();
    if (ns != NULL && index < (UWORD)AMI_CFG_MAX_ATTACHED &&
        ns->ns_IfaceClaims[index] != 0)
        ns->ns_IfaceClaims[index]--;

    ami_ns_lock_release();
}

/*
 * bsdsocket.library, the Tier 3 Roadshow extensions we can implement.
 * SPDX-License-Identifier: MIT
 */

#include "bsdsocket_vectors.h"

#include "aminetxduo/config.h"

#include <proto/exec.h>

BOOL bsd_GetDefaultDomainName(register STRPTR buffer   __asm("a0"),
                              register LONG buffer_size __asm("d0"),
                              register struct AmiSocketBase *SocketBase __asm("a6"))
{
    (VOID)SocketBase;

    if (buffer == NULL || buffer_size <= 0)
        return FALSE;

    buffer[0] = '\0';

    netstack_dns_absorb_pending();

    /* The domain is live configuration. Copy it under the resolver lock so a
       concurrent SetDefaultDomainName() cannot leave a mixed string. */
    return (netstack_domain_name_get((char *)buffer, (ULONG)buffer_size) ==
            AMI_NET_OK) ? TRUE : FALSE;
}

/*
 * One allocation holds the List, the nodes and the address strings, so
 * ReleaseDomainNameServerList() is a single FreeVec of the block the list
 * header sits at the top of.
 */
typedef struct BsdDnsList
{
    struct List                 bdl_List;
    struct DomainNameServerNode bdl_Node[2 * AMI_CFG_MAX_NAMESERVERS];
    char                        bdl_Text[2 * AMI_CFG_MAX_NAMESERVERS]
                                        [AMI_CFG_IP6_STRLEN];
    AmiResolverConfig           bdl_Resolver;
} BsdDnsList;

struct List *bsd_ObtainDomainNameServerList(
    register struct AmiSocketBase *SocketBase __asm("a6"))
{
    BsdDnsList *out;
    UWORD       count = 0;
    UWORD       i;

    netstack_dns_absorb_pending();

    out = (BsdDnsList *)ami_alloc(sizeof(BsdDnsList));
    if (out == NULL)
    {
        (VOID)bsd_fail(SocketBase, AMI_ENOBUFS);
        return NULL;
    }

    if (netstack_resolver_snapshot(&out->bdl_Resolver) != AMI_NET_OK)
    {
        ami_free(out);
        (VOID)bsd_fail(SocketBase, AMI_ENETDOWN);
        return NULL;
    }
    /* NewList(), open-coded: amiga.lib is not available to a shared library. */
    out->bdl_List.lh_Head     = (struct Node *)&out->bdl_List.lh_Tail;
    out->bdl_List.lh_Tail     = NULL;
    out->bdl_List.lh_TailPred = (struct Node *)&out->bdl_List.lh_Head;
    out->bdl_List.lh_Type     = NT_UNKNOWN;

    for (i = 0; i < out->bdl_Resolver.nameserver_count &&
                i < (UWORD)AMI_CFG_MAX_NAMESERVERS; i++)
    {
        struct DomainNameServerNode *node = &out->bdl_Node[count];

        ami_config_format_ip(out->bdl_Resolver.nameserver[i],
                             out->bdl_Text[count],
                             sizeof(out->bdl_Text[count]));

        node->dnsn_Size    = (LONG)sizeof(*node);
        node->dnsn_Address = (STRPTR)out->bdl_Text[count];

        node->dnsn_UseCount = (out->bdl_Resolver.nameserver_use[i] != 0)
                                  ? out->bdl_Resolver.nameserver_use[i]
                                  : -1;

        AddTail((struct List *)&out->bdl_List, (struct Node *)&node->dnsn_MinNode);
        count++;
    }

    for (i = 0; i < out->bdl_Resolver.nameserver6_count &&
                i < (UWORD)AMI_CFG_MAX_NAMESERVERS; i++)
    {
        struct DomainNameServerNode *node = &out->bdl_Node[count];

        ami_config_format_ip6(out->bdl_Resolver.nameserver6[i],
                              out->bdl_Text[count],
                              sizeof(out->bdl_Text[count]));

        node->dnsn_Size    = (LONG)sizeof(*node);
        node->dnsn_Address = (STRPTR)out->bdl_Text[count];

        node->dnsn_UseCount = 1;

        AddTail((struct List *)&out->bdl_List, (struct Node *)&node->dnsn_MinNode);
        count++;
    }

    return &out->bdl_List;
}

/*
 * The address arrives as a dotted quad, not as an in_addr: Roadshow's autodoc
 * spells the parameter "char *address", and its own commands pass the text
 * straight through from their arguments.
 */

LONG bsd_AddDomainNameServer(register STRPTR address __asm("a0"),
                             register struct AmiSocketBase *SocketBase __asm("a6"))
{
    ULONG addr;

    /* The autodoc splits these: a bad parameter is EFAULT, a parameter that is
       not a valid IP address is EINVAL. */
    if (address == NULL)
        return bsd_fail(SocketBase, AMI_EFAULT);

    if (!ami_config_parse_ip((const char *)address, &addr))
        return bsd_fail(SocketBase, AMI_EINVAL);

    switch (netstack_dns_server_add(addr))
    {
        case AMI_NET_OK:          return 0;
        case AMI_NET_ERR_NOMEM:   return bsd_fail(SocketBase, AMI_ENOBUFS);
        case AMI_NET_ERR_STATE:   return bsd_fail(SocketBase, AMI_ENETDOWN);
        default:                  return bsd_fail(SocketBase, AMI_EINVAL);
    }
}

LONG bsd_RemoveDomainNameServer(register STRPTR address __asm("a0"),
                                register struct AmiSocketBase *SocketBase __asm("a6"))
{
    ULONG addr;

    if (address == NULL)
        return bsd_fail(SocketBase, AMI_EFAULT);

    if (!ami_config_parse_ip((const char *)address, &addr))
        return bsd_fail(SocketBase, AMI_EINVAL);

    switch (netstack_dns_server_remove(addr))
    {
        case AMI_NET_OK:          return 0;
        /* "[ENOENT] The IP address to remove was not found", autodoc. */
        case AMI_NET_ERR_NONAME:  return bsd_fail(SocketBase, AMI_ENOENT);
        case AMI_NET_ERR_STATE:   return bsd_fail(SocketBase, AMI_ENETDOWN);
        default:                  return bsd_fail(SocketBase, AMI_EINVAL);
    }
}

/*
 * The VOID return is deliberate. clib/bsdsocket_protos.h:184 says
 *
 *     __stdargs VOID SetDefaultDomainName( STRPTR buffer );
 */
VOID bsd_SetDefaultDomainName(register STRPTR name __asm("a0"),
                              register struct AmiSocketBase *SocketBase __asm("a6"))
{
    switch (netstack_set_domain_name((const char *)name))
    {
        case AMI_NET_OK:          break;
        case AMI_NET_ERR_STATE:   (VOID)bsd_fail(SocketBase, AMI_ENETDOWN); break;
        /* Too long to store: refused rather than truncated. */
        default:                  (VOID)bsd_fail(SocketBase, AMI_EINVAL);   break;
    }
}

VOID bsd_ReleaseDomainNameServerList(register struct List *list __asm("a0"),
                                     register struct AmiSocketBase *SocketBase __asm("a6"))
{
    (VOID)SocketBase;

    /* The List is the first member of the block, so this is the block. */
    if (list != NULL)
        ami_free(list);
}

/*
 * 4.4BSD in_localaddr(): non-zero if the address is on a network this host is
 * directly attached to. Roadshow keeps the same meaning. "Directly attached"
 */
LONG bsd_In_LocalAddr(register in_addr_t address __asm("d0"),
                      register struct AmiSocketBase *SocketBase __asm("a6"))
{
    NX_IP *ip = netstack_ip();
    ULONG  addr = BSD_NTOHL((ULONG)address);
    UINT   i;

    (VOID)SocketBase;

    if (ip == NULL)
        return 0;

    for (i = 0; i < NX_MAX_PHYSICAL_INTERFACES; i++)
    {
        NX_INTERFACE *nxif = &ip->nx_ip_interface[i];
        ULONG         mask;

        if (nxif->nx_interface_valid == 0)
            continue;

        mask = nxif->nx_interface_ip_network_mask;
        if (mask == 0)
            continue;

        if ((addr & mask) == (nxif->nx_interface_ip_address & mask))
            return 1;
    }

    /* The loopback network is always local. */
    if ((addr & 0xFF000000UL) == 0x7F000000UL)
        return 1;

    return 0;
}

/*
 * 4.4BSD in_canforward(): an address can be forwarded unless it is loopback,
 * multicast/class D, class E, or has a zero network part.
 */
LONG bsd_In_CanForward(register in_addr_t address __asm("d0"),
                       register struct AmiSocketBase *SocketBase __asm("a6"))
{
    ULONG addr = BSD_NTOHL((ULONG)address);
    ULONG net  = addr >> 24;

    (VOID)SocketBase;

    if (net == 0 || net == 127)             /* "this network", loopback */
        return 0;

    if ((addr & 0xF0000000UL) == 0xE0000000UL)      /* class D, multicast */
        return 0;

    if ((addr & 0xF0000000UL) == 0xF0000000UL)      /* class E, reserved  */
        return 0;

    return 1;
}

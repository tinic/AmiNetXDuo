/*
 * bsdsocket.library, the Tier 3 Roadshow extensions we can implement.
 *
 *   GetDefaultDomainName()          the resolver's default domain
 *   ObtainDomainNameServerList()    the configured name servers
 *   ReleaseDomainNameServerList()
 *   In_LocalAddr() / In_CanForward()
 *
 * Tier 3 (docs/RESEARCH.md S3.2) is ~35 vectors: interface config and query,
 * routing, GetNetworkStatistics(), the *RoadshowData set. Most now live
 * elsewhere, written against the NDK 3.2 autodoc (docs/RESEARCH.md S47):
 *
 *   interfaces.c   ObtainInterfaceList(), ReleaseInterfaceList(),
 *                  QueryInterfaceTagList(), ConfigureInterfaceTagList(),
 *                  AddInterfaceTagList(), RemoveInterface()
 *   routing.c      AddRouteTagList(), DeleteRouteTagList(), GetRouteInfo(),
 *                  FreeRouteInfo()
 *   netstats.c     GetNetworkStatistics()
 *   addralloc.c    CreateAddrAllocMessageA(), DeleteAddrAllocMessage(),
 *                  BeginInterfaceConfig(), AbortInterfaceConfig(), the
 *                  message and every documented refusal. The allocation
 *                  itself is the one gap left, and it answers AAMR_Ignored
 *                  rather than being a stub, because that vector returns VOID
 *                  and an ENOSYS in it hangs the caller (RESEARCH S47.12).
 *
 * Still stubbed:
 *
 *   ObtainRoadshowData()       struct RoadshowDataNode is defined, but the
 *                              rdn_Name strings are Roadshow-private and
 *                              ChangeRoadshowData() looks items up by name.
 *                              Inventing them gives an API nothing can use
 *                              and that disagrees with Roadshow. The autodoc
 *                              does not list the names either.
 *   the mbuf_* family          no caller we would ship. In the whole NDK a
 *                              `struct mbuf` crosses this ABI in one place
 *                              that is not an mbuf_* prototype:
 *                              IPFilterMsg.ifm_Packet
 *                              (libraries/bsdsocket.h:1147), the packet the
 *                              IP filter hook is handed. So these eleven
 *                              exist for ipf_* clients, and ipf_* is out of
 *                              scope below.
 *
 *                              The monitor hooks do not need them:
 *                              PacketMonitorMessage hands over flat
 *                              pmm_PacketData + pmm_PacketSize, and the TCP,
 *                              UDP and ICMP messages hand over parsed header
 *                              pointers. netmonitor.c is complete without a
 *                              single mbuf.
 *
 *                              Implementing them would also mean a real BSD
 *                              mbuf allocator, since sys/mbuf.h pins the
 *                              layout callers compile against, next/len/
 *                              data chains NetX Duo has no use for, as it
 *                              allocates fixed-size NX_PACKETs from one pool.
 *
 *                              src/mbuf is that allocator, already written and
 *                              tested and linked into nothing. Wiring any one
 *                              of these eleven to it also means linking
 *                              aminetxduo_mbuf and calling ami_mbuf_cleanup()
 *                              from ami_ns_destroy(); see aminetxduo/mbuf.h.
 *                              Without both, every slab is resident until
 *                              reboot.
 *   ChangeRouteTagList()       the NDK assigns it an offset and neither the
 *                              autodoc nor clib/bsdsocket_protos.h says what
 *                              it takes.
 *   the ipf_* set              Roadshow's packet filter, out of scope per
 *                              RESEARCH 9; nothing outside Roadshow's own
 *                              tools calls it, and NetTrace covers the
 *                              capture half through BPF.
 *   vsyslog()                  prototyped; there is nothing on this machine
 *                              for it to log to.
 *
 * The net-monitor hooks are done: netmonitor.c registers all four types, and
 * socket.c dispatches MHT_Connect and MHT_Bind.
 *
 * The autodoc exists: NDK 3.2 ships it at SANA+RoadshowTCP-IP/doc/
 * bsdsocket.doc, beside interfaces/bsdsocket.xml, the same NDK this project
 * builds against. 10,436 lines, 121 functions, including 35 of the vectors
 * that used to answer ENOSYS here. The remaining stubs are unwritten, not
 * unknowable.
 *
 * Not in that autodoc: the seven ipf_* vectors and ChangeRouteTagList.
 *
 * Guessing an ABI cost this project time twice (ndk-include/pwd.h,
 * bpf_set_notify_mask's register order), so anything written here is written
 * against that document and not from the name.
 *
 * SPDX-License-Identifier: MIT
 */

#include "bsdsocket_vectors.h"

#include "aminetxduo/config.h"

#include <proto/exec.h>

/* ------------------------------------------------------ default domain, */

BOOL bsd_GetDefaultDomainName(register STRPTR buffer   __asm("a0"),
                              register LONG buffer_size __asm("d0"),
                              register struct AmiSocketBase *SocketBase __asm("a6"))
{
    const AmiConfig *cfg = netstack_config();

    (VOID)SocketBase;

    if (buffer == NULL || buffer_size <= 0)
        return FALSE;

    buffer[0] = '\0';

    if (cfg == NULL || cfg->resolver.domain[0] == '\0')
        return FALSE;

    /* Truncating would hand back a domain that is not the domain. */
    if (bsd_strlen(cfg->resolver.domain) >= (ULONG)buffer_size)
        return FALSE;

    bsd_strncpy((char *)buffer, cfg->resolver.domain, (ULONG)buffer_size);

    return TRUE;
}

/* ------------------------------------------------------- name servers --- */

/*
 * One allocation holds the List, the nodes and the dotted-quad strings, so
 * ReleaseDomainNameServerList() is a single FreeVec of the block the list
 * header sits at the top of.
 *
 * struct DomainNameServerNode (libraries/bsdsocket.h) embeds a MinNode, not a
 * Node, while the prototype says struct List. The two are layout-compatible
 * for AddTail/traversal, lh_Head and mlh_Head are at the same offset, so
 * the list header is a struct List and the nodes are MinNodes, which is the
 * only reading that satisfies both halves of the published interface.
 */
typedef struct BsdDnsList
{
    struct List                 bdl_List;
    struct DomainNameServerNode bdl_Node[AMI_CFG_MAX_NAMESERVERS];
    char                        bdl_Text[AMI_CFG_MAX_NAMESERVERS][16];
} BsdDnsList;

struct List *bsd_ObtainDomainNameServerList(
    register struct AmiSocketBase *SocketBase __asm("a6"))
{
    const AmiConfig *cfg = netstack_config();
    BsdDnsList      *out;
    UWORD            i;

    if (cfg == NULL)
    {
        (VOID)bsd_fail(SocketBase, AMI_ENETDOWN);
        return NULL;
    }

    out = (BsdDnsList *)ami_alloc(sizeof(BsdDnsList));
    if (out == NULL)
    {
        (VOID)bsd_fail(SocketBase, AMI_ENOBUFS);
        return NULL;
    }

    /* NewList(), open-coded: amiga.lib is not available to a shared library. */
    out->bdl_List.lh_Head     = (struct Node *)&out->bdl_List.lh_Tail;
    out->bdl_List.lh_Tail     = NULL;
    out->bdl_List.lh_TailPred = (struct Node *)&out->bdl_List.lh_Head;
    out->bdl_List.lh_Type     = NT_UNKNOWN;

    for (i = 0; i < cfg->resolver.nameserver_count &&
                i < (UWORD)AMI_CFG_MAX_NAMESERVERS; i++)
    {
        struct DomainNameServerNode *node = &out->bdl_Node[i];

        ami_config_format_ip(cfg->resolver.nameserver[i], out->bdl_Text[i],
                             sizeof(out->bdl_Text[i]));

        node->dnsn_Size    = (LONG)sizeof(*node);
        node->dnsn_Address = (STRPTR)out->bdl_Text[i];

        /*
         * "How many times this server address has been added to the list so
         * far. A negative value indicates that this server was configured
         * statically in the 'DEVS:Internet/resolver' file", autodoc.
         * netstack_dns_server_add() keeps both halves in nameserver_use[]:
         * the sign says where the entry came from, the magnitude is the nest
         * count. A slot in use is never 0, so that stands in for an
         * AmiResolverConfig that predates the field.
         */
        node->dnsn_UseCount = (cfg->resolver.nameserver_use[i] != 0)
                                  ? cfg->resolver.nameserver_use[i]
                                  : -1;

        AddTail((struct List *)&out->bdl_List, (struct Node *)&node->dnsn_MinNode);
    }

    return &out->bdl_List;
}

/*
 * The three calls that change the resolver. Roadshow's own AddNetInterface
 * hands over the name servers from the lease it obtained by calling
 * AddDomainNameServer(). With these as ENOSYS stubs it configured the
 * interface, took a DHCP lease, set the netmask and the default route, then
 * returned rc 20 on the last step, so the command in every Roadshow user's
 * S:Network-Startup reported failure after doing everything right
 * (docs/RESEARCH.md 55).
 *
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
 * VOID is not a slip. clib/bsdsocket_protos.h:184 says
 *
 *     __stdargs VOID SetDefaultDomainName( STRPTR buffer );
 *
 * while its Add/Remove neighbours return LONG. This used to return LONG here,
 * and the mismatch was invisible because the prototype had been hand-added to
 * bsdsocket_vectors.h under the wrong LVO comment, so the generator never saw
 * the disagreement. Regenerating the table surfaced it.
 *
 * The caller gets no result, so failures go to errno only: bsd_fail() sets it
 * and Errno() reports it.
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

/* ------------------------------------------------- address classification */

/*
 * 4.4BSD in_localaddr(): non-zero if the address is on a network this host is
 * directly attached to. Roadshow keeps the same meaning. "Directly attached"
 * is decided entirely by the interface addresses and masks NetX Duo already
 * holds, so nothing is inferred here.
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
 * 4.4BSD in_canforward(): an address may be forwarded unless it is loopback,
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

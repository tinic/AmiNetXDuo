/*
 * bsdsocket.library -- the Tier 3 Roadshow extensions we can implement.
 *
 *   GetDefaultDomainName()          the resolver's default domain
 *   ObtainDomainNameServerList()    the configured name servers
 *   ReleaseDomainNameServerList()
 *   In_LocalAddr() / In_CanForward()
 *
 * WHY THIS FILE IS SHORT
 *
 * Tier 3 (docs/RESEARCH.md S3.2) is ~35 vectors: interface config and query,
 * routing, GetNetworkStatistics(), the *RoadshowData set.
 *
 * Most of them are no longer here, and are not stubbed either. Written
 * against the autodoc named below (docs/RESEARCH.md S47):
 *
 *   interfaces.c   ObtainInterfaceList(), ReleaseInterfaceList(),
 *                  QueryInterfaceTagList(), ConfigureInterfaceTagList(),
 *                  AddInterfaceTagList(), RemoveInterface()
 *   routing.c      AddRouteTagList(), DeleteRouteTagList(), GetRouteInfo(),
 *                  FreeRouteInfo()
 *   netstats.c     GetNetworkStatistics()
 *
 * What is still stubbed, each with its reason written where it belongs:
 *
 *   BeginInterfaceConfig()     the foot of interfaces.c. Not a contract we
 *   AbortInterfaceConfig()     cannot read -- a thing this stack cannot do.
 *   ObtainRoadshowData()       struct RoadshowDataNode is defined, but the
 *                              rdn_Name strings are Roadshow-private and
 *                              ChangeRoadshowData() looks items up BY NAME,
 *                              so inventing them produces an API nothing can
 *                              use and that silently disagrees with Roadshow.
 *                              The autodoc does not list the names either.
 *   the net-monitor hooks      documented, and not yet written.
 *   the mbuf_* family          there is no mbuf allocator to expose.
 *
 * THE PRIMARY SOURCE EXISTS. This comment used to say there was no
 * bsdsocket.doc autodoc anywhere, and that the stubs would stand until one
 * turned up. One has: NDK 3.2 ships it, at
 * SANA+RoadshowTCP-IP/doc/bsdsocket.doc, beside interfaces/bsdsocket.xml --
 * the same NDK this project builds against. It is 10,436 lines and documents
 * 121 functions, including 35 of the 43 vectors that were answering ENOSYS
 * here: the whole interface configuration and query set, the routing set,
 * GetNetworkStatistics, the net-monitor hooks, the domain-name-server calls,
 * *RoadshowData and the mbuf_* family.
 *
 * So the reason these are stubs is no longer "we cannot know the contract".
 * It is that nobody has written them yet. That is a different statement and
 * it should not keep hiding behind the old one -- interfaces.c, routing.c and
 * netstats.c are the parts of it that somebody did write.
 *
 * What is NOT in that autodoc: the seven ipf_* vectors and ChangeRouteTagList.
 * ipf_* remains deliberately out of scope (RESEARCH 9); nothing outside
 * Roadshow's own tools calls it.
 *
 * Guessing an ABI is still how this project lost time twice
 * (ndk-include/pwd.h, bpf_set_notify_mask's register order), so anything
 * written here is written against that document and not from the name.
 *
 * What IS here is everything whose contract the headers pin down completely.
 *
 * SPDX-License-Identifier: MIT
 */

#include "bsdsocket_vectors.h"

#include "aminetxduo/config.h"

#include <proto/exec.h>

/* ------------------------------------------------------ default domain -- */

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
 * for AddTail/traversal -- lh_Head and mlh_Head are the same offset -- so the
 * list header is a struct List and the nodes are MinNodes, which is the only
 * reading that satisfies both halves of the published interface.
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
         * "Negative values indicate statically-configured servers"
         * (libraries/bsdsocket.h). Every server here comes from
         * DEVS:Internet/name_resolution or from the DHCP lease that replaced
         * it; either way nothing in AmiNetXDuo reference-counts them, so they
         * are all reported as static rather than with an invented count.
         */
        node->dnsn_UseCount = -1;

        AddTail((struct List *)&out->bdl_List, (struct Node *)&node->dnsn_MinNode);
    }

    return &out->bdl_List;
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
 * directly attached to. Roadshow keeps the same meaning; nothing here is
 * inferred, because "directly attached" is decided entirely by the interface
 * addresses and masks NetX Duo already holds.
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

/*
 * AmiNetXDuo, the IPv6 half of the stack singleton.
 *
 * Compiled only in an AMINETXDUO_IPV6 build (docs/RESEARCH.md §9, decision 1:
 * IPv6 is a build option, not a default). The floor build never sees it.
 *
 * Three address configuration modes:
 *
 *   LINK-LOCAL  fe80::/64 with a modified-EUI-64 interface identifier built
 *               from the SANA-II device's MAC. It needs no router, no server
 *               and no configuration file, and RFC 4291 requires every IPv6
 *               interface to have one. Both modes below configure it first.
 *               It is also the only mode that can be fully exercised on an
 *               isolated machine.
 *
 *   AUTO        Link-local, plus RFC 4862 stateless autoconfiguration: the
 *               stack sends a router solicitation, and a prefix from any
 *               router advertisement that comes back becomes a global
 *               address. The default when CONFIGURE6 is absent; on a link
 *               with no IPv6 router it costs three ICMPv6 packets and behaves
 *               like LINK-LOCAL.
 *
 *   STATIC      Link-local, plus the ADDRESS6/prefix from the interface file,
 *               plus GATEWAY6 as a default router if given.
 *
 * DHCPv6 is not used. NetX Duo ships a client (addons/dhcp/nxd_dhcpv6_client.c)
 * but it is 40 KB of code before its own IANA/IAID option handling, needs its
 * own thread and UDP socket, and answers what SLAAC already answers on the
 * networks an Amiga is likely to be on; the floor target is a 68020 with 4 MB
 * (docs/RESEARCH.md §9). For a stateful-only network, the addon would be
 * wired up here.
 *
 * SPDX-License-Identifier: MIT
 */

#include "netstack_internal.h"

#include "nx_ipv6.h"
#include "nx_icmpv6.h"

#include <proto/exec.h>

/* How long to wait for duplicate address detection to finish, per address.
   NX_IPV6_DAD_TRANSMITS solicitations at one per second, plus slack. */
#define AMI_DAD_TIMEOUT_TICKS   ((ULONG)(NX_IPV6_DAD_TRANSMITS + 2) * \
                                 (ULONG)NX_IP_PERIODIC_RATE)

/* -------------------------------------------------------------- bring-up, */

LONG ami_netstack_ipv6_enable(AmiNetStack *ns)
{
    UINT status;

    /*
     * Order matters:
     *
     *   nxd_ipv6_enable()  installs _nx_ipv6_packet_receive and initialises
     *                      the default-router table, the reachable/retransmit
     *                      timers and the per-address index fields. It also
     *                      configures ::1 on the internal loopback interface,
     *                      so loopback IPv6 works with no network card.
     *   nxd_icmp_enable()  installs both _nx_icmp_packet_process and
     *                      _nx_icmpv6_packet_process and clears the neighbour
     *                      and destination caches. The IPv4-only
     *                      nx_icmp_enable() that the floor build calls does
     *                      not touch the v6 side, so without this neighbour
     *                      discovery never runs and every IPv6 send fails to
     *                      resolve a MAC.
     *
     * icmp before ipv6_enable works today, but it clears the ND cache before
     * the tables that index it are set up.
     */
    status = nxd_ipv6_enable(&ns->ns_Ip);
    if (status != NX_SUCCESS && status != NX_ALREADY_ENABLED)
    {
        AMI_ERROR("netstack: nxd_ipv6_enable failed (%ld)", (long)status);
        return AMI_NET_ERR_KERNEL;
    }

    status = nxd_icmp_enable(&ns->ns_Ip);
    if (status != NX_SUCCESS && status != NX_ALREADY_ENABLED)
    {
        AMI_ERROR("netstack: nxd_icmp_enable failed (%ld)", (long)status);
        return AMI_NET_ERR_KERNEL;
    }

    /*
     * The resolver's only route in on an IPv6-only link.  Set directly, as
     * nx_ip_packet_id is: there is no setter for it and the field is only read
     * from the IP thread's own advertisement processing.
     */
    ns->ns_Ip.nx_ipv6_rdnss_notify = ami_ns6_rdnss;

    ns->ns_Ipv6Enabled = TRUE;

    AMI_INFO("netstack: IPv6 enabled (ICMPv6, neighbour discovery, ::1)");

    return AMI_NET_OK;
}

/* ------------------------------------------------------------- addressing, */

static VOID ami_ns6_log(const char *what, const ULONG addr[4], ULONG prefix)
{
    char text[AMI_CFG_IP6_STRLEN];

    ami_config_format_ip6(addr, text, sizeof(text));
    AMI_INFO("netstack: %s %s/%lu", what, text, (unsigned long)prefix);
}

/*
 * Wait for duplicate address detection to move an address out of TENTATIVE.
 * A TENTATIVE address cannot be used as a source, so a connect() issued before
 * DAD completes either picks a different source or fails. The wait is bounded,
 * and costs about three seconds each time an interface is configured, once
 * per interface at startup, and once per AddInterfaceTagList(). With
 * NX_DISABLE_IPV6_DAD the address is PREFERRED immediately and this returns on
 * the first look.
 */
static BOOL ami_ns6_wait_ready(AmiNetStack *ns, UINT index)
{
    ULONG waited = 0;

    for (;;)
    {
        UCHAR state = ns->ns_Ip.nx_ipv6_address[index].nxd_ipv6_address_state;

        if (state == NX_IPV6_ADDR_STATE_PREFERRED ||
            state == NX_IPV6_ADDR_STATE_VALID)
            return TRUE;

        if (!ns->ns_Ip.nx_ipv6_address[index].nxd_ipv6_address_valid)
        {
            /* DAD found a duplicate and withdrew the address. */
            return FALSE;
        }

        if (waited >= AMI_DAD_TIMEOUT_TICKS)
            return FALSE;

        tx_thread_sleep(NX_IP_PERIODIC_RATE / 5);
        waited += NX_IP_PERIODIC_RATE / 5;
    }
}

#ifndef NX_DISABLE_ICMPV6_ROUTER_SOLICITATION
/*
 * Start this interface soliciting a router.
 *
 * nxd_ipv6_enable() seeds these four fields, but only for the interfaces that
 * exist when it runs; nx_ip_interface_attach() joins ff02::1 and seeds none of
 * them, so an interface arriving through AddInterfaceTagList() has a solicit
 * count of zero and never asks. And nxd_ipv6_stateless_address_autoconfig_
 * enable() cannot cover it: NetX Duo's autoconfiguration status field means
 * "enabled" when zeroed, so the first call returns NX_ALREADY_ENABLED and
 * returns before it would have reset the counter.
 *
 * Without a solicitation the interface still autoconfigures, off the router's
 * next unsolicited advertisement, which RFC 4861 permits to be half an hour
 * away, and which a router answering solicitations only never sends at all.
 */
static VOID ami_ns6_arm_solicitation(AmiNetStack *ns, UWORD i)
{
    NX_INTERFACE *ifp = &ns->ns_Ip.nx_ip_interface[i];

    tx_mutex_get(&ns->ns_Ip.nx_ip_protection, TX_WAIT_FOREVER);

    ifp->nx_ipv6_rtr_solicitation_max      = NX_ICMPV6_MAX_RTR_SOLICITATIONS;
    ifp->nx_ipv6_rtr_solicitation_count    = NX_ICMPV6_MAX_RTR_SOLICITATIONS;
    ifp->nx_ipv6_rtr_solicitation_interval = NX_ICMPV6_RTR_SOLICITATION_INTERVAL;
    ifp->nx_ipv6_rtr_solicitation_timer    = NX_ICMPV6_RTR_SOLICITATION_DELAY;

    tx_mutex_put(&ns->ns_Ip.nx_ip_protection);
}
#else
#define ami_ns6_arm_solicitation(ns, i)  ((VOID)(ns), (VOID)(i))
#endif /* NX_DISABLE_ICMPV6_ROUTER_SOLICITATION */

static VOID ami_ns6_configure_interface(AmiNetStack *ns, UWORD i)
{
    const AmiIfConfig *cfg = &ns->ns_Config.interfaces[i];
    UINT               index = 0;
    UINT               status;

    if (cfg->ip6type == AMI_IP6TYPE_OFF)
    {
        AMI_INFO("netstack: %s: IPv6 disabled by CONFIGURE6", cfg->name);
        return;
    }

    /*
     * RFC 8200 section 5 puts a floor of 1280 octets under every link IPv6
     * runs over, and there is no fragmentation below it to fall back on. The
     * MTU here comes from S2_DEVICEQUERY and may then have been taken further
     * down by MTU= in DEVS:NetInterfaces, so a driver reporting less, or a
     * configuration asking for less, would otherwise have left IPv6 nominally
     * up on a link that cannot carry a conformant packet. IPv4 has no such
     * floor and stays on the interface.
     */
    if (ns->ns_Ip.nx_ip_interface[i].nx_interface_ip_mtu_size < 1280)
    {
        AMI_WARN("netstack: %s: MTU %lu is below IPv6's 1280, IPv6 not started",
                 cfg->name,
                 (unsigned long)ns->ns_Ip.nx_ip_interface[i].nx_interface_ip_mtu_size);
        return;
    }

    /*
     * Link-local first, always. A NULL address with prefix length 10 tells
     * NetX Duo to derive fe80::/64 from this interface's MAC
     * (nxd_ipv6_address_set.c); the 10 is the fe80::/10 prefix the address is
     * carved out of, not the /64 the identifier occupies.
     *
     * This also creates the solicited-node multicast group membership, which
     * reaches the SANA-II shim as NX_LINK_MULTICAST_JOIN and thence
     * S2_ADDMULTICASTADDRESS. Many devices answer S2ERR_NOT_SUPPORTED and
     * pass multicast anyway, so the shim logs and swallows that failure
     * (src/sana2/sana2_driver.c); neighbour discovery still works on them.
     */
    status = nxd_ipv6_address_set(&ns->ns_Ip, (UINT)i, NX_NULL, 10, &index);
    if (status == NX_DUPLICATED_ENTRY)
    {
        /*
         * The interface already carries its link-local, so it has been through
         * here and everything below it was done then. Not an error: this runs
         * once per boot and again for every interface added at run time, and
         * an interface that was never detached is still holding what the first
         * pass gave it. `index` is not usable after this, the duplicate
         * check happens after the free slot has been picked.
         */
        return;
    }
    if (status != NX_SUCCESS)
    {
        AMI_ERROR("netstack: %s: link-local address failed (%ld)",
                  cfg->name, (long)status);
        return;
    }

    if (!ami_ns6_wait_ready(ns, index))
    {
        AMI_WARN("netstack: %s: link-local address did not pass duplicate "
                 "address detection", cfg->name);
    }
    else
    {
        ami_ns6_log(cfg->name,
                    ns->ns_Ip.nx_ipv6_address[index].nxd_ipv6_address, 64);
    }

    if (cfg->ip6type == AMI_IP6TYPE_STATIC)
    {
        NXD_ADDRESS addr;
        UINT        gindex = 0;

        addr.nxd_ip_version       = NX_IP_VERSION_V6;
        addr.nxd_ip_address.v6[0] = cfg->address6[0];
        addr.nxd_ip_address.v6[1] = cfg->address6[1];
        addr.nxd_ip_address.v6[2] = cfg->address6[2];
        addr.nxd_ip_address.v6[3] = cfg->address6[3];

        status = nxd_ipv6_address_set(&ns->ns_Ip, (UINT)i, &addr,
                                      cfg->prefix6, &gindex);
        if (status != NX_SUCCESS)
        {
            AMI_ERROR("netstack: %s: ADDRESS6 rejected (%ld)",
                      cfg->name, (long)status);
        }
        else if (ami_ns6_wait_ready(ns, gindex))
        {
            ami_ns6_log(cfg->name, cfg->address6, cfg->prefix6);
        }
        else
        {
            AMI_WARN("netstack: %s: ADDRESS6 is a duplicate on this link",
                     cfg->name);
        }
    }
    if (cfg->ip6type == AMI_IP6TYPE_AUTO)
    {
        /*
         * NX_ALREADY_ENABLED is the normal answer: NetX Duo's per-interface
         * status field is zero-initialised and zero means enabled, so
         * autoconfiguration is on before anyone asks for it. Calling enable()
         * anyway keeps the intent visible here.
         */
        status = nxd_ipv6_stateless_address_autoconfig_enable(&ns->ns_Ip,
                                                              (UINT)i);
        if (status != NX_SUCCESS && status != NX_ALREADY_ENABLED)
            AMI_WARN("netstack: %s: stateless autoconfiguration failed (%ld)",
                     cfg->name, (long)status);
        else
            AMI_INFO("netstack: %s: awaiting router advertisements", cfg->name);

        ami_ns6_arm_solicitation(ns, i);
    }
    else
    {
        /*
         * LINKLOCAL and STATIC need an explicit disable, or a router
         * advertisement would add a global address to an interface the
         * operator asked to keep off the global Internet. Requires
         * NX_IPV6_STATELESS_AUTOCONFIG_CONTROL in nx_user.h; see the note
         * there for what happens without it.
         */
        status = nxd_ipv6_stateless_address_autoconfig_disable(&ns->ns_Ip,
                                                               (UINT)i);
        if (status != NX_SUCCESS)
            AMI_WARN("netstack: %s: could not switch stateless "
                     "autoconfiguration off (%ld)", cfg->name, (long)status);
    }

    if (cfg->have_gateway6)
    {
        static const ULONG any[4] = { 0, 0, 0, 0 };

        status = netstack_ipv6_route_add(any, 0, cfg->gateway6, i);
        if (status != NX_SUCCESS)
            AMI_WARN("netstack: %s: GATEWAY6 rejected (%ld)",
                     cfg->name, (long)status);
        else
            ami_ns6_log("default router", cfg->gateway6, 128);
    }
}

VOID ami_netstack_ipv6_configure(AmiNetStack *ns)
{
    UWORD i;

    if (!ns->ns_Ipv6Enabled)
        return;

    for (i = 0; i < ns->ns_IfaceCount; i++)
        ami_ns6_configure_interface(ns, i);
}

VOID ami_netstack_ipv6_configure_one(AmiNetStack *ns, UWORD i)
{
    if (!ns->ns_Ipv6Enabled || i >= (UWORD)NX_MAX_PHYSICAL_INTERFACES)
        return;

    ami_ns6_configure_interface(ns, i);
}

/* ---------------------------------------------------------------- the API, */

BOOL netstack_ipv6_enabled(VOID)
{
    AmiNetStack *ns = ami_netstack_raw();

    return (ns != NULL && ns->ns_IpCreated && ns->ns_Ipv6Enabled) ? TRUE : FALSE;
}

BOOL netstack_ipv6_address_get(UWORD interface_index, UWORD slot,
                               ULONG addr_out[4], ULONG *prefix_out,
                               ULONG *state_out)
{
    AmiNetStack      *ns = ami_netstack_raw();
    NXD_IPV6_ADDRESS *entry;
    UWORD             seen = 0;

    if (ns == NULL || !ns->ns_IpCreated || !ns->ns_Ipv6Enabled)
        return FALSE;

    if (interface_index >= (UWORD)NX_MAX_PHYSICAL_INTERFACES)
        return FALSE;

    /*
     * Walk the interface's own list rather than the flat nx_ipv6_address[]
     * array: that array is shared between interfaces and the loopback ::1
     * entry, so indexing into it would report another interface's address as
     * this one's.
     */
    entry = ns->ns_Ip.nx_ip_interface[interface_index]
                .nxd_interface_ipv6_address_list_head;

    while (entry != NX_NULL)
    {
        if (entry->nxd_ipv6_address_valid)
        {
            if (seen == slot)
            {
                if (addr_out != NULL)
                {
                    addr_out[0] = entry->nxd_ipv6_address[0];
                    addr_out[1] = entry->nxd_ipv6_address[1];
                    addr_out[2] = entry->nxd_ipv6_address[2];
                    addr_out[3] = entry->nxd_ipv6_address[3];
                }
                if (prefix_out != NULL)
                    *prefix_out = (ULONG)entry->nxd_ipv6_address_prefix_length;
                if (state_out != NULL)
                    *state_out = (ULONG)entry->nxd_ipv6_address_state;

                return TRUE;
            }
            seen++;
        }

        entry = entry->nxd_ipv6_address_next;
    }

    return FALSE;
}

/* ---------------------------------------------------------------- routes, */

/*
 * The lifetime the once-a-second tick in nxd_ipv6_prefix_router_timer_tick.c
 * treats as "never expires". Not zero: zero is the value that tick reads as
 * already expired, so a router added with it is invalidated within the second
 * and GATEWAY6 configured nothing that outlived the boot.
 */
#define AMI_NS6_ROUTER_FOREVER  0xFFFFU
#define AMI_NS6_PREFIX_FOREVER  0xFFFFFFFFUL

static BOOL ami_ns6_is_any(const ULONG addr[4])
{
    return (BOOL)((addr[0] | addr[1] | addr[2] | addr[3]) == 0);
}

static VOID ami_ns6_copy(const ULONG src[4], ULONG dst[4])
{
    dst[0] = src[0];
    dst[1] = src[1];
    dst[2] = src[2];
    dst[3] = src[3];
}

/*
 * The destination cache remembers the next hop each destination was last sent
 * to, and _nx_ipv6_packet_send() consults it before either list. Left alone, a
 * route added or removed here would not move a packet already going somewhere
 * until the entry aged out, so the caller would see the route in the table and
 * the traffic still on the old path.
 *
 * Marking the slots invalid is what NetX Duo's own _nx_invalidate_destination_entry()
 * does; the ND cache entries the slots point at are left alone.
 */
static VOID ami_ns6_forget_destinations(AmiNetStack *ns)
{
    UINT i;

    for (i = 0; i < (UINT)NX_IPV6_DESTINATION_TABLE_SIZE; i++)
        ns->ns_Ip.nx_ipv6_destination_table[i].nx_ipv6_destination_entry_valid = 0;

    ns->ns_Ip.nx_ipv6_destination_table_size = 0;
}

/* The prefix list entry for exactly this prefix and length, or NX_NULL. */
static NX_IPV6_PREFIX_ENTRY *ami_ns6_prefix_find(AmiNetStack *ns,
                                                 const ULONG prefix[4],
                                                 ULONG prefix_len)
{
    NX_IPV6_PREFIX_ENTRY *entry = ns->ns_Ip.nx_ipv6_prefix_list_ptr;

    while (entry != NX_NULL)
    {
        if (entry->nx_ipv6_prefix_entry_prefix_length == prefix_len &&
            entry->nx_ipv6_prefix_entry_network_address[0] == prefix[0] &&
            entry->nx_ipv6_prefix_entry_network_address[1] == prefix[1] &&
            entry->nx_ipv6_prefix_entry_network_address[2] == prefix[2] &&
            entry->nx_ipv6_prefix_entry_network_address[3] == prefix[3])
        {
            return entry;
        }

        entry = entry->nx_ipv6_prefix_entry_next;
    }

    return NX_NULL;
}

/* The host bits a prefix of this length does not cover. */
static VOID ami_ns6_mask(ULONG addr[4], ULONG prefix_len)
{
    UINT i;

    for (i = 0; i < 4U; i++)
    {
        ULONG bits = (prefix_len > (ULONG)(i * 32U))
                         ? (prefix_len - (ULONG)(i * 32U)) : 0UL;

        if (bits >= 32UL)
            continue;

        addr[i] &= (bits == 0UL) ? 0UL : (0xFFFFFFFFUL << (32UL - bits));
    }
}

UINT netstack_ipv6_route_add(const ULONG dest[4], ULONG prefix_len,
                             const ULONG next_hop[4], UWORD interface_index)
{
    AmiNetStack *ns = ami_netstack_raw();
    ULONG        prefix[4];
    UINT         status;

    if (ns == NULL || !ns->ns_IpCreated || !ns->ns_Ipv6Enabled)
        return NX_NOT_ENABLED;

    if (dest == NULL || prefix_len > 128UL)
        return NX_IP_ADDRESS_ERROR;

    if (next_hop != NULL && !ami_ns6_is_any(next_hop))
    {
        NXD_ADDRESS router;

        /* Anything but the default route: there is nowhere to store it. */
        if (prefix_len != 0UL || !ami_ns6_is_any(dest))
            return NX_NOT_SUPPORTED;

        if (interface_index >= (UWORD)NX_MAX_PHYSICAL_INTERFACES)
            return NX_INVALID_INTERFACE;

        router.nxd_ip_version = NX_IP_VERSION_V6;
        ami_ns6_copy(next_hop, router.nxd_ip_address.v6);

        status = nxd_ipv6_default_router_add(&ns->ns_Ip, &router,
                                             AMI_NS6_ROUTER_FOREVER,
                                             (UINT)interface_index);
    }
    else
    {
        ami_ns6_copy(dest, prefix);
        ami_ns6_mask(prefix, prefix_len);

        /* _nx_ipv6_prefix_list_add_entry() is called from router advertisement
           processing, which already holds the protection mutex; nothing here
           does. */
        tx_mutex_get(&ns->ns_Ip.nx_ip_protection, TX_WAIT_FOREVER);

        if (ami_ns6_prefix_find(ns, prefix, prefix_len) != NX_NULL)
            status = NX_DUPLICATED_ENTRY;
        else
            status = _nx_ipv6_prefix_list_add_entry(&ns->ns_Ip, prefix,
                                                    prefix_len,
                                                    AMI_NS6_PREFIX_FOREVER);

        if (status == NX_SUCCESS)
            ami_ns6_forget_destinations(ns);

        tx_mutex_put(&ns->ns_Ip.nx_ip_protection);

        return status;
    }

    if (status == NX_SUCCESS)
    {
        tx_mutex_get(&ns->ns_Ip.nx_ip_protection, TX_WAIT_FOREVER);
        ami_ns6_forget_destinations(ns);
        tx_mutex_put(&ns->ns_Ip.nx_ip_protection);
    }

    return status;
}

UINT netstack_ipv6_route_delete(const ULONG dest[4], ULONG prefix_len,
                                const ULONG next_hop[4])
{
    AmiNetStack *ns = ami_netstack_raw();
    ULONG        prefix[4];
    UINT         status;

    if (ns == NULL || !ns->ns_IpCreated || !ns->ns_Ipv6Enabled)
        return NX_NOT_ENABLED;

    if (dest == NULL || prefix_len > 128UL)
        return NX_IP_ADDRESS_ERROR;

    if (next_hop != NULL && !ami_ns6_is_any(next_hop))
    {
        NXD_ADDRESS router;

        router.nxd_ip_version = NX_IP_VERSION_V6;
        ami_ns6_copy(next_hop, router.nxd_ip_address.v6);

        status = nxd_ipv6_default_router_delete(&ns->ns_Ip, &router);

        if (status == NX_SUCCESS)
        {
            tx_mutex_get(&ns->ns_Ip.nx_ip_protection, TX_WAIT_FOREVER);
            ami_ns6_forget_destinations(ns);
            tx_mutex_put(&ns->ns_Ip.nx_ip_protection);
        }

        return status;
    }

    ami_ns6_copy(dest, prefix);
    ami_ns6_mask(prefix, prefix_len);

    /* _nx_ipv6_prefix_list_delete() returns nothing, so the lookup is what
       tells a caller whether there was anything to remove. */
    tx_mutex_get(&ns->ns_Ip.nx_ip_protection, TX_WAIT_FOREVER);

    if (ami_ns6_prefix_find(ns, prefix, prefix_len) == NX_NULL)
    {
        status = NX_ENTRY_NOT_FOUND;
    }
    else
    {
        _nx_ipv6_prefix_list_delete(&ns->ns_Ip, prefix, (INT)prefix_len);
        ami_ns6_forget_destinations(ns);
        status = NX_SUCCESS;
    }

    tx_mutex_put(&ns->ns_Ip.nx_ip_protection);

    return status;
}

BOOL netstack_ipv6_source_for(const ULONG dest[4], ULONG addr_out[4])
{
    AmiNetStack      *ns = ami_netstack_raw();
    NXD_IPV6_ADDRESS *source = NX_NULL;
    ULONG             scratch[4];
    UINT              status;

    if (ns == NULL || !ns->ns_IpCreated || !ns->ns_Ipv6Enabled ||
        dest == NULL || addr_out == NULL)
        return FALSE;

    /*
     * _nxd_ipv6_interface_find() is the same RFC 6724 selection routine the
     * IPv6 send path uses to fill in an outgoing packet's source, so
     * getsockname() reports the address the packets will carry.
     */
    scratch[0] = dest[0];
    scratch[1] = dest[1];
    scratch[2] = dest[2];
    scratch[3] = dest[3];

    status = _nxd_ipv6_interface_find(&ns->ns_Ip, scratch, &source, NX_NULL);
    if (status != NX_SUCCESS || source == NX_NULL)
        return FALSE;

    addr_out[0] = source->nxd_ipv6_address[0];
    addr_out[1] = source->nxd_ipv6_address[1];
    addr_out[2] = source->nxd_ipv6_address[2];
    addr_out[3] = source->nxd_ipv6_address[3];

    return TRUE;
}

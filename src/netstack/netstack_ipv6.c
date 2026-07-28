/*
 * AmiNetXDuo -- the IPv6 half of the stack singleton.
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

#include <proto/exec.h>

/* How long to wait for duplicate address detection to finish, per address.
   NX_IPV6_DAD_TRANSMITS solicitations at one per second, plus slack. */
#define AMI_DAD_TIMEOUT_TICKS   ((ULONG)(NX_IPV6_DAD_TRANSMITS + 2) * \
                                 (ULONG)NX_IP_PERIODIC_RATE)

/* -------------------------------------------------------------- bring-up -- */

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

    ns->ns_Ipv6Enabled = TRUE;

    AMI_INFO("netstack: IPv6 enabled (ICMPv6, neighbour discovery, ::1)");

    return AMI_NET_OK;
}

/* ------------------------------------------------------------- addressing -- */

static VOID ami_ns6_log(const char *what, const ULONG addr[4], ULONG prefix)
{
    char text[AMI_CFG_IP6_STRLEN];

    ami_config_format_ip6(addr, text, sizeof(text));
    AMI_INFO("netstack: %s %s/%lu", what, text, (unsigned long)prefix);
}

/*
 * Wait for duplicate address detection to move an address out of TENTATIVE.
 * A TENTATIVE address cannot be used as a source, so a connect() issued before
 * DAD completes either picks a different source or fails. The wait is bounded
 * and only happens at startup. With NX_DISABLE_IPV6_DAD the address is
 * PREFERRED immediately and this returns on the first look.
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
         * anyway keeps the intent visible here and resets the
         * router-solicitation counter, which matters if the link came up
         * before IPv6 did.
         */
        status = nxd_ipv6_stateless_address_autoconfig_enable(&ns->ns_Ip,
                                                              (UINT)i);
        if (status != NX_SUCCESS && status != NX_ALREADY_ENABLED)
            AMI_WARN("netstack: %s: stateless autoconfiguration failed (%ld)",
                     cfg->name, (long)status);
        else
            AMI_INFO("netstack: %s: awaiting router advertisements", cfg->name);
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
        NXD_ADDRESS router;

        router.nxd_ip_version       = NX_IP_VERSION_V6;
        router.nxd_ip_address.v6[0] = cfg->gateway6[0];
        router.nxd_ip_address.v6[1] = cfg->gateway6[1];
        router.nxd_ip_address.v6[2] = cfg->gateway6[2];
        router.nxd_ip_address.v6[3] = cfg->gateway6[3];

        /*
         * Lifetime 0 means never expires, correct for a statically configured
         * router; one learned from an advertisement carries the lifetime that
         * advertisement gave it.
         */
        status = nxd_ipv6_default_router_add(&ns->ns_Ip, &router, 0, (UINT)i);
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

/* ---------------------------------------------------------------- the API -- */

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

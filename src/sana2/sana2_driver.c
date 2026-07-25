/*
 * AmiNetXDuo -- the NX_IP driver entry.
 *
 * NetX Duo talks to this in NX_LINK_* commands; SANA-II answers in exec
 * IORequests. The interesting asymmetry is framing, and it is handled in
 * sana2_tx.c (never build a link header in cooked mode) and sana2_rx.c
 * (synthesise one from ios2_SrcAddr/DstAddr/PacketType). Everything here is
 * dispatch, state and counters.
 *
 * SPDX-License-Identifier: MIT
 */

#include "sana2_internal.h"

#include "aminetxduo/netstack.h"

#include <proto/exec.h>

/*
 * Interface 0 is attached by nx_ip_create() itself, before anyone can reach
 * its NX_INTERFACE to set nx_interface_additional_link_info. So bindings are
 * recorded here first and folded into additional_link_info the first time the
 * driver is called for that interface.
 */
typedef struct AmiSana2Binding
{
    NX_IP      *ip;
    UINT        index;
    AmiSana2If *iface;
} AmiSana2Binding;

static AmiSana2Binding ami_sana2_bindings[AMI_CFG_MAX_INTERFACES];

LONG ami_sana2_attach(AmiSana2If *iface, NX_IP *ip, UINT index)
{
    UWORD i;

    if (iface == NULL || ip == NULL)
        return AMI_NET_ERR_STATE;

    Forbid();
    for (i = 0; i < AMI_CFG_MAX_INTERFACES; i++)
    {
        if (ami_sana2_bindings[i].iface == iface ||
            ami_sana2_bindings[i].iface == NULL)
        {
            ami_sana2_bindings[i].ip    = ip;
            ami_sana2_bindings[i].index = index;
            ami_sana2_bindings[i].iface = iface;
            Permit();

            iface->ip    = ip;
            iface->index = index;
            return AMI_NET_OK;
        }
    }
    Permit();

    return AMI_NET_ERR_STATE;
}

VOID ami_sana2_unbind(AmiSana2If *iface)
{
    UWORD i;

    Forbid();
    for (i = 0; i < AMI_CFG_MAX_INTERFACES; i++)
    {
        if (ami_sana2_bindings[i].iface == iface)
        {
            ami_sana2_bindings[i].iface = NULL;
            ami_sana2_bindings[i].ip    = NULL;
            ami_sana2_bindings[i].index = 0;
        }
    }
    Permit();
}

static AmiSana2If *ami_sana2_lookup(NX_IP_DRIVER *req)
{
    NX_INTERFACE *interface_ptr = req->nx_ip_driver_interface;
    AmiSana2If   *iface;
    UWORD         i;

    if (interface_ptr == NULL)
        return NULL;

    iface = (AmiSana2If *)interface_ptr->nx_interface_additional_link_info;
    if (iface != NULL)
        return iface;

    for (i = 0; i < AMI_CFG_MAX_INTERFACES; i++)
    {
        if (ami_sana2_bindings[i].iface != NULL &&
            ami_sana2_bindings[i].ip == req->nx_ip_driver_ptr &&
            ami_sana2_bindings[i].index ==
                (UINT)interface_ptr->nx_interface_index)
        {
            iface = ami_sana2_bindings[i].iface;

            interface_ptr->nx_interface_additional_link_info = iface;
            iface->ip            = req->nx_ip_driver_ptr;
            iface->index         = (UINT)interface_ptr->nx_interface_index;
            iface->interface_ptr = interface_ptr;

            return iface;
        }
    }

    return NULL;
}

/* ---------------------------------------------------------------- helpers */

static ULONG ami_sana2_mac_msw(const AmiSana2If *iface)
{
    return (((ULONG)iface->mac[0]) << 8) | ((ULONG)iface->mac[1]);
}

static ULONG ami_sana2_mac_lsw(const AmiSana2If *iface)
{
    return (((ULONG)iface->mac[2]) << 24) | (((ULONG)iface->mac[3]) << 16) |
           (((ULONG)iface->mac[4]) << 8)  | ((ULONG)iface->mac[5]);
}

static UWORD ami_sana2_ether_type(NX_IP_DRIVER *req, NX_PACKET *packet)
{
    switch (req->nx_ip_driver_command)
    {
    case NX_LINK_ARP_SEND:
    case NX_LINK_ARP_RESPONSE_SEND:
        return AMI_ETHERTYPE_ARP;

    case NX_LINK_RARP_SEND:
        return AMI_ETHERTYPE_RARP;

    default:
        break;
    }

    return (packet->nx_packet_ip_version == 4) ? AMI_ETHERTYPE_IPV4
                                               : AMI_ETHERTYPE_IPV6;
}

/* --------------------------------------------------------------- the entry */

VOID ami_sana2_driver_entry(NX_IP_DRIVER *driver_req)
{
    AmiSana2If   *iface;
    NX_INTERFACE *interface_ptr;
    NX_IP        *ip_ptr;

    if (driver_req == NULL)
        return;

    driver_req->nx_ip_driver_status = NX_SUCCESS;

    ip_ptr        = driver_req->nx_ip_driver_ptr;
    interface_ptr = driver_req->nx_ip_driver_interface;

    iface = ami_sana2_lookup(driver_req);
    if (iface == NULL)
    {
        /* An unbound interface can still fail cleanly; a packet it was asked
           to send must not leak. */
        if (driver_req->nx_ip_driver_packet != NULL)
            nx_packet_transmit_release(driver_req->nx_ip_driver_packet);

        driver_req->nx_ip_driver_status = NX_INVALID_INTERFACE;
        return;
    }

    switch (driver_req->nx_ip_driver_command)
    {
    case NX_LINK_INTERFACE_ATTACH:
        iface->interface_ptr = interface_ptr;
        iface->ip            = ip_ptr;
        iface->index         = (UINT)interface_ptr->nx_interface_index;
        break;

    case NX_LINK_INTERFACE_DETACH:
        ami_sana2_rx_stop(iface);
        ami_sana2_tx_drain(iface);
        interface_ptr->nx_interface_additional_link_info = NULL;
        iface->interface_ptr = NULL;
        ami_sana2_unbind(iface);
        break;

    case NX_LINK_INITIALIZE:
        iface->pool = ip_ptr->nx_ip_default_packet_pool;

        /*
         * SANA-II's MTU is already the IP payload size -- unlike a link MTU,
         * it has no room for the header in it -- so it maps straight across.
         */
        nx_ip_interface_mtu_set(ip_ptr, iface->index, iface->mtu);

        nx_ip_interface_physical_address_set(ip_ptr, iface->index,
                                             ami_sana2_mac_msw(iface),
                                             ami_sana2_mac_lsw(iface),
                                             NX_FALSE);

        /*
         * ARP only makes sense where there is a hardware address to resolve
         * to. Point-to-point SANA-II wires (PPP, SLIP) report AddrFieldSize 0
         * and get address mapping turned off instead.
         */
        nx_ip_interface_address_mapping_configure(
            ip_ptr, iface->index,
            (iface->addr_bytes == AMI_ETH_ADDR_SIZE) ? NX_TRUE : NX_FALSE);
        break;

    case NX_LINK_UNINITIALIZE:
        ami_sana2_rx_stop(iface);
        ami_sana2_tx_drain(iface);
        ami_sana2_offline(iface);
        interface_ptr->nx_interface_link_up = NX_FALSE;
        break;

    case NX_LINK_ENABLE:
        if (ami_sana2_online(iface) != 0)
        {
            driver_req->nx_ip_driver_status = NX_NOT_SUCCESSFUL;
            break;
        }

        if (ami_sana2_rx_start(iface) != 0)
        {
            ami_sana2_offline(iface);
            driver_req->nx_ip_driver_status = NX_NOT_SUCCESSFUL;
            break;
        }

        interface_ptr->nx_interface_link_up = NX_TRUE;
        ami_sana2_refresh_stats(iface);
        break;

    case NX_LINK_DISABLE:
        interface_ptr->nx_interface_link_up = NX_FALSE;
        ami_sana2_rx_stop(iface);
        ami_sana2_tx_drain(iface);
        ami_sana2_offline(iface);
        break;

    case NX_LINK_PACKET_SEND:
    case NX_LINK_PACKET_BROADCAST:
    case NX_LINK_ARP_SEND:
    case NX_LINK_ARP_RESPONSE_SEND:
    case NX_LINK_RARP_SEND:
    {
        NX_PACKET *packet = driver_req->nx_ip_driver_packet;

        if (packet == NULL)
        {
            driver_req->nx_ip_driver_status = NX_PTR_ERROR;
            break;
        }

        driver_req->nx_ip_driver_status =
            ami_sana2_tx_send(iface, packet,
                              ami_sana2_ether_type(driver_req, packet),
                              driver_req->nx_ip_driver_physical_address_msw,
                              driver_req->nx_ip_driver_physical_address_lsw);
        break;
    }

    case NX_LINK_MULTICAST_JOIN:
        if (ami_sana2_multicast(iface, S2_ADDMULTICASTADDRESS,
                                driver_req->nx_ip_driver_physical_address_msw,
                                driver_req->nx_ip_driver_physical_address_lsw)
            != 0)
        {
            /*
             * Plenty of SANA-II devices answer S2ERR_NOT_SUPPORTED here and
             * simply pass multicast through (or run wide open). Failing the
             * join would break IGMP and IPv6 ND on hardware that works fine,
             * so this is reported and swallowed.
             */
            AMI_WARN("sana2: multicast join not honoured by %s", iface->device);
        }
        break;

    case NX_LINK_MULTICAST_LEAVE:
        (VOID)ami_sana2_multicast(iface, S2_DELMULTICASTADDRESS,
                                  driver_req->nx_ip_driver_physical_address_msw,
                                  driver_req->nx_ip_driver_physical_address_lsw);
        break;

    case NX_LINK_GET_STATUS:
        *(driver_req->nx_ip_driver_return_ptr) =
            (ULONG)interface_ptr->nx_interface_link_up;
        break;

    case NX_LINK_GET_SPEED:
        *(driver_req->nx_ip_driver_return_ptr) = iface->bps;
        break;

    case NX_LINK_GET_DUPLEX_TYPE:
        /* SANA-II has no notion of duplex. */
        *(driver_req->nx_ip_driver_return_ptr) = 0;
        break;

    case NX_LINK_GET_ERROR_COUNT:
        ami_sana2_refresh_stats(iface);
        *(driver_req->nx_ip_driver_return_ptr) =
            iface->stats.bad_data + iface->stats.overruns +
            iface->stats.tx_errors + iface->stats.rx_errors;
        break;

    case NX_LINK_GET_RX_COUNT:
        ami_sana2_refresh_stats(iface);
        *(driver_req->nx_ip_driver_return_ptr) = iface->stats.packets_received;
        break;

    case NX_LINK_GET_TX_COUNT:
        ami_sana2_tx_reap(iface);
        *(driver_req->nx_ip_driver_return_ptr) = iface->stats.packets_sent;
        break;

    case NX_LINK_GET_ALLOC_ERRORS:
        *(driver_req->nx_ip_driver_return_ptr) = iface->stats.alloc_failures;
        break;

    case NX_LINK_GET_INTERFACE_TYPE:
        *(driver_req->nx_ip_driver_return_ptr) =
            (iface->hw_type == S2WireType_Ethernet) ? NX_INTERFACE_TYPE_ETHERNET
                                                    : NX_INTERFACE_TYPE_OTHER;
        break;

    case NX_LINK_SET_PHYSICAL_ADDRESS:
    {
        struct IOSana2Req req = iface->templ;
        ULONG             msw = driver_req->nx_ip_driver_physical_address_msw;
        ULONG             lsw = driver_req->nx_ip_driver_physical_address_lsw;

        req.ios2_SrcAddr[0] = (UBYTE)(msw >> 8);
        req.ios2_SrcAddr[1] = (UBYTE)(msw);
        req.ios2_SrcAddr[2] = (UBYTE)(lsw >> 24);
        req.ios2_SrcAddr[3] = (UBYTE)(lsw >> 16);
        req.ios2_SrcAddr[4] = (UBYTE)(lsw >> 8);
        req.ios2_SrcAddr[5] = (UBYTE)(lsw);

        /*
         * The spec is explicit that S2_CONFIGINTERFACE may only run once per
         * unit, so this generally fails with S2WERR_IS_CONFIGURED. DHCP does
         * not need it and neither does anything else we ship; it is here for
         * completeness rather than because it is expected to work.
         */
        if (ami_sana2_command(iface, &req, S2_CONFIGINTERFACE) != 0)
        {
            driver_req->nx_ip_driver_status = NX_NOT_SUPPORTED;
        }
        else
        {
            ami_sana2_copy_bytes(iface->mac, req.ios2_SrcAddr,
                                 AMI_ETH_ADDR_SIZE);
        }
        break;
    }

    case NX_LINK_DEFERRED_PROCESSING:
        ami_sana2_tx_reap(iface);
        ami_sana2_refresh_stats(iface);
        break;

    default:
        driver_req->nx_ip_driver_status = NX_UNHANDLED_COMMAND;
        break;
    }
}

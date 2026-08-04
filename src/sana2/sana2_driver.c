/*
 * AmiNetXDuo -- the NX_IP driver entry.
 *
 * NetX Duo talks to this in NX_LINK_* commands; SANA-II answers in exec
 * IORequests. Framing is handled elsewhere: sana2_tx.c never builds a link
 * header in cooked mode, and sana2_rx.c synthesises one from
 * ios2_SrcAddr/DstAddr/PacketType. Everything here is dispatch, state and
 * counters.
 *
 * SPDX-License-Identifier: MIT
 */

#include "sana2_internal.h"

#include "aminetxduo/netstack.h"

#include <proto/exec.h>

/*
 * Interface 0 is attached by nx_ip_create() itself, before anyone can reach
 * its NX_INTERFACE to set nx_interface_additional_link_info. Bindings are
 * therefore recorded here first and folded into additional_link_info the first
 * time the driver is called for that interface.
 */
/*
 * `iface` is the publication flag: attach writes it last, unbind clears it
 * first, and ami_sana2_lookup() below tests it first and reads the other two
 * only after. The reader takes no lock, so that ordering is the whole of what
 * makes it safe -- volatile is here to stop the compiler sinking the `ip` and
 * `index` stores past the `iface` one, which it is otherwise free to do: the
 * three are independent stores to distinct members of a plain static that it
 * can prove nothing about. Three extra stores once per interface attach.
 */
typedef struct AmiSana2Binding
{
    NX_IP      * volatile ip;
    volatile UINT         index;
    AmiSana2If * volatile iface;
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

        /* SANA-II's MTU is already the IP payload size, with no room for the
           link header in it, so it maps straight across. */
        nx_ip_interface_mtu_set(ip_ptr, iface->index, iface->mtu);

        nx_ip_interface_physical_address_set(ip_ptr, iface->index,
                                             ami_sana2_mac_msw(iface),
                                             ami_sana2_mac_lsw(iface),
                                             NX_FALSE);

        /* ARP needs a hardware address to resolve to. Addressless SANA-II
           wires report AddrFieldSize 0 and get address mapping turned off. */
        nx_ip_interface_address_mapping_configure(
            ip_ptr, iface->index,
            (iface->addr_bytes == AMI_ETH_ADDR_SIZE) ? NX_TRUE : NX_FALSE);
        break;

    case NX_LINK_UNINITIALIZE:
        ami_sana2_rx_stop(iface);
        ami_sana2_tx_drain(iface);
        ami_sana2_offline(iface);
        interface_ptr->nx_interface_link_up = NX_FALSE;
        iface->admin_up = FALSE;
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
        iface->admin_up = TRUE;
        ami_sana2_refresh_stats(iface);
        break;

    case NX_LINK_DISABLE:
        interface_ptr->nx_interface_link_up = NX_FALSE;
        iface->admin_up = FALSE;
        ami_sana2_rx_stop(iface);
        ami_sana2_tx_drain(iface);
        ami_sana2_offline(iface);
        break;

    /*
     * SM_Down: "the stack will no longer attempt to transmit messages through
     * this interface", and nothing more. The readers are left alone -- stopping
     * them means S2_OFFLINE, because that is the only thing that returns a
     * queued CMD_READ on a device which ignores AbortIO() (ami_sana2_rx_stop),
     * and taking the wire away is what this command exists not to do. NetX Duo
     * hands a link-down interface no packets to send; the readers keep feeding
     * a stack that will not answer, and NX_LINK_ENABLE picks them back up.
     */
    case AMI_LINK_STACK_DISABLE:
        interface_ptr->nx_interface_link_up = NX_FALSE;
        iface->admin_up = FALSE;
        ami_sana2_tx_drain(iface);
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
             * Many SANA-II devices answer S2ERR_NOT_SUPPORTED here and pass
             * multicast through anyway. Failing the join would break IGMP and
             * IPv6 ND on working hardware, so it is logged and swallowed.
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
         * S2_CONFIGINTERFACE may only run once per unit, so this generally
         * fails with S2WERR_IS_CONFIGURED. Nothing in this tree needs it; it
         * is implemented for completeness.
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
        /*
         * The transmit ring's completion handler. A SANA-II CMD_WRITE finishes
         * long after the driver entry returned, and the packet cannot go back
         * to TCP until then. A reader thread notices the completion and calls
         * _nx_ip_driver_deferred_processing(); the reap happens here, on the
         * IP thread, under nx_ip_protection, in the same context as every
         * send. See sana2_tx.c.
         *
         * Nothing else belongs in this case. Refreshing the SANA-II statistics
         * here would be a synchronous DoIO() to the device on every
         * transmitted frame; the counters are refreshed by the
         * NX_LINK_GET_*_COUNT commands instead.
         */
        ami_sana2_tx_reap(iface);
        break;

    default:
        driver_req->nx_ip_driver_status = NX_UNHANDLED_COMMAND;
        break;
    }
}

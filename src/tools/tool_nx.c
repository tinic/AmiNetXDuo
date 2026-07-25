/*
 * AmiNetXDuo tools -- talking to the live NetX Duo instance.
 *
 * SPDX-License-Identifier: MIT
 */

#include "tools_nx.h"
#include "tx_amiga.h"

/* ThreadX priority the tools adopt at: below the IP thread, above idle. */
#define TOOL_TX_PRIORITY    16

static TX_THREAD    tool_thread;
static BOOL         tool_adopted;

LONG tool_stack_enter(VOID)
{
    UINT status;

    if (tool_adopted)
        return 0;

    if (tx_amiga_kernel_running() != TX_TRUE)
    {
        tool_error("the network kernel is not running");
        return -1;
    }

    status = tx_amiga_adopt_thread(&tool_thread, (CHAR *)tool_name,
                                   TOOL_TX_PRIORITY);
    if (status != TX_SUCCESS)
    {
        tool_error("could not join the network kernel (ThreadX error %ld)",
                   (LONG)status);
        return -1;
    }

    tool_adopted = TRUE;
    return 0;
}

VOID tool_stack_leave(VOID)
{
    if (!tool_adopted)
        return;

    tx_amiga_orphan_thread(&tool_thread);
    tool_adopted = FALSE;
}

/* ------------------------------------------------------------- snapshots */

static VOID tool_mac_from_words(ULONG msw, ULONG lsw, UBYTE *mac)
{
    mac[0] = (UBYTE)((msw >> 8) & 0xff);
    mac[1] = (UBYTE)(msw & 0xff);
    mac[2] = (UBYTE)((lsw >> 24) & 0xff);
    mac[3] = (UBYTE)((lsw >> 16) & 0xff);
    mac[4] = (UBYTE)((lsw >> 8) & 0xff);
    mac[5] = (UBYTE)(lsw & 0xff);
}

static VOID tool_snapshot_interfaces(NX_IP *ip, ToolSnapshot *out)
{
    UINT i;

    for (i = 0; i < (UINT)TOOL_MAX_IF; i++)
    {
        NX_INTERFACE *nxif = &ip->nx_ip_interface[i];
        ToolIfInfo   *info = &out->iface[i];
        AmiSana2If   *sana;

        info->nx_index = (UWORD)i;
        info->attached = (nxif->nx_interface_valid != 0) ? TRUE : FALSE;
        if (!info->attached)
            continue;

        info->link_up = (nxif->nx_interface_link_up != 0) ? TRUE : FALSE;
        info->address = nxif->nx_interface_ip_address;
        info->netmask = nxif->nx_interface_ip_network_mask;
        info->mtu     = nxif->nx_interface_ip_mtu_size;
        info->nx_name = (const char *)nxif->nx_interface_name;

        tool_mac_from_words(nxif->nx_interface_physical_address_msw,
                            nxif->nx_interface_physical_address_lsw,
                            info->mac);

        /*
         * ami_sana2_attach() parks the AmiSana2If here (aminetxduo/sana2.h),
         * which is how the tools reach the driver's own counters without a
         * separate registry.
         */
        sana = (AmiSana2If *)nxif->nx_interface_additional_link_info;
        if (sana != NULL)
        {
            info->have_sana2   = TRUE;
            info->sana2_online = ami_sana2_is_online(sana);
            info->bps          = ami_sana2_get_bps(sana);
            ami_sana2_get_stats(sana, &info->stats);
        }
    }

    out->iface_count = (UWORD)TOOL_MAX_IF;
}

static VOID tool_snapshot_sockets(NX_IP *ip, ToolSnapshot *out)
{
    ULONG n;

#ifndef NX_DISABLE_TCP_INFO
    /* nothing extra needed; the counters live on the socket itself */
#endif

    {
        NX_TCP_SOCKET *sock = ip->nx_ip_tcp_created_sockets_ptr;

        for (n = 0; n < ip->nx_ip_tcp_created_sockets_count && sock != NULL; n++)
        {
            ToolSockInfo *si;

            if (out->sock_count >= (UWORD)TOOL_MAX_SOCK)
            {
                out->sock_truncated = TRUE;
                break;
            }

            si = &out->sock[out->sock_count++];
            si->is_tcp       = TRUE;
            si->local_port   = (UWORD)sock->nx_tcp_socket_port;
            si->peer_port    = (UWORD)sock->nx_tcp_socket_connect_port;
            si->peer_address = sock->nx_tcp_socket_connect_ip.nxd_ip_address.v4;
            si->state        = sock->nx_tcp_socket_state;

            sock = sock->nx_tcp_socket_created_next;
        }
    }

    {
        NX_UDP_SOCKET *sock = ip->nx_ip_udp_created_sockets_ptr;

        for (n = 0; n < ip->nx_ip_udp_created_sockets_count && sock != NULL; n++)
        {
            ToolSockInfo *si;

            if (out->sock_count >= (UWORD)TOOL_MAX_SOCK)
            {
                out->sock_truncated = TRUE;
                break;
            }

            si = &out->sock[out->sock_count++];
            si->is_tcp     = FALSE;
            si->local_port = (UWORD)sock->nx_udp_socket_port;
            si->queued     = sock->nx_udp_socket_receive_count;

            sock = sock->nx_udp_socket_created_next;
        }
    }
}

LONG tool_snapshot(NX_IP *ip, ToolSnapshot *out, BOOL want_sockets)
{
    ULONG gateway = 0;
    UINT  status;
    UWORD i;
    UWORD j;

    if (ip == NULL || out == NULL)
        return -1;

    for (i = 0; i < (UWORD)TOOL_MAX_IF; i++)
    {
        ToolIfInfo *info = &out->iface[i];

        info->nx_index     = i;
        info->attached     = FALSE;
        info->link_up      = FALSE;
        info->address      = 0;
        info->netmask      = 0;
        info->mtu          = 0;
        info->nx_name      = NULL;
        info->have_sana2   = FALSE;
        info->sana2_online = FALSE;
        info->bps          = 0;
        for (j = 0; j < (UWORD)AMI_ETH_ADDR_SIZE; j++)
            info->mac[j] = 0;
    }
    out->iface_count    = 0;
    out->sock_count     = 0;
    out->sock_truncated = FALSE;
    out->gateway        = 0;
    out->have_gateway   = FALSE;

    if (tool_stack_enter() != 0)
        return -1;

    /*
     * Adopted from here to tool_stack_leave(): no dos.library, no Wait(),
     * nothing but memory reads and NetX Duo calls.
     */
    tool_snapshot_interfaces(ip, out);

    if (want_sockets)
        tool_snapshot_sockets(ip, out);

    status = nx_ip_gateway_address_get(ip, &gateway);
    if (status == NX_SUCCESS && gateway != 0)
    {
        out->gateway      = gateway;
        out->have_gateway = TRUE;
    }

    tool_stack_leave();

    return 0;
}

const char *tool_tcp_state_name(UINT state)
{
    switch (state)
    {
        case NX_TCP_CLOSED:         return "CLOSED";
        case NX_TCP_LISTEN_STATE:   return "LISTEN";
        case NX_TCP_SYN_SENT:       return "SYN_SENT";
        case NX_TCP_SYN_RECEIVED:   return "SYN_RCVD";
        case NX_TCP_ESTABLISHED:    return "ESTABLISHED";
        case NX_TCP_CLOSE_WAIT:     return "CLOSE_WAIT";
        case NX_TCP_FIN_WAIT_1:     return "FIN_WAIT_1";
        case NX_TCP_FIN_WAIT_2:     return "FIN_WAIT_2";
        case NX_TCP_CLOSING:        return "CLOSING";
        case NX_TCP_TIMED_WAIT:     return "TIME_WAIT";
        case NX_TCP_LAST_ACK:       return "LAST_ACK";
        default:                    return "UNKNOWN";
    }
}

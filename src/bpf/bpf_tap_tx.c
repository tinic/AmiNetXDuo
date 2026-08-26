/*
 * AmiNetXDuo, the transmit capture tap.
 *
 * Needs NX_PACKET, and therefore tx_api.h / nx_api.h ahead of any exec header
 * (same rule as src/sana2/sana2_internal.h and src/mbuf/mbuf_packet.c).
 *
 * The packet itself is never touched: it is often a queued TCP segment that the
 * stack hands back for retransmission.
 *
 * SPDX-License-Identifier: MIT
 */

#include "tx_api.h"
#include "nx_api.h"

#include "bpf_internal.h"

VOID ami_bpf_tap_tx(APTR cookie, NX_PACKET *packet, BOOL has_link_header,
                    UWORD ether_type, ULONG dst_msw, ULONG dst_lsw,
                    const UBYTE *src_mac)
{
    AmiBpfIf   *ifp;
    AmiBpfView  view;
    NX_PACKET  *p;
    UBYTE       eth[AMI_BPF_ETH_HDR_LEN];
    ULONG       dlt;
    ULONG       wirelen;

    if (ami_bpf_bound_channels == 0 || packet == NX_NULL)
        return;

    ami_bpf_lock();
    ifp = ami_bpf_iface_by_cookie(cookie);
    if (ifp == NULL)
    {
        ami_bpf_unlock();
        return;
    }
    dlt = ifp->dlt;
    ami_bpf_unlock();

    view.wirelen = 0;
    view.caplen  = 0;
    view.nsegs   = 0;

    wirelen = packet->nx_packet_length;

    if (!has_link_header && dlt == DLT_EN10MB)
    {
        /* msw carries the top two address bytes, lsw the bottom four, the
           same split nx_ip_driver_physical_address_msw/lsw uses. */
        eth[0] = (UBYTE)(dst_msw >> 8);
        eth[1] = (UBYTE)(dst_msw);
        eth[2] = (UBYTE)(dst_lsw >> 24);
        eth[3] = (UBYTE)(dst_lsw >> 16);
        eth[4] = (UBYTE)(dst_lsw >> 8);
        eth[5] = (UBYTE)(dst_lsw);

        if (src_mac != NULL)
        {
            UWORD i;

            for (i = 0; i < AMI_BPF_ETH_ADDR_LEN; i++)
                eth[6 + i] = src_mac[i];
        }
        else
        {
            ami_bpf_zero_bytes(&eth[6], (ULONG)AMI_BPF_ETH_ADDR_LEN);
        }

        eth[12] = (UBYTE)(ether_type >> 8);
        eth[13] = (UBYTE)(ether_type);

        (VOID)ami_bpf_view_add(&view, eth, (ULONG)AMI_BPF_ETH_HDR_LEN);
        wirelen += AMI_BPF_ETH_HDR_LEN;
    }

    for (p = packet; p != NX_NULL; p = p->nx_packet_next)
    {
        ULONG len;

        if (p->nx_packet_append_ptr <= p->nx_packet_prepend_ptr)
            continue;

        len = (ULONG)(p->nx_packet_append_ptr - p->nx_packet_prepend_ptr);

        if (ami_bpf_view_add(&view, p->nx_packet_prepend_ptr, len) != 0)
            break;      /* more fragments than AMI_BPF_MAX_SEGS: capture what
                           is present and let bh_caplen < bh_datalen show it */
    }

    /* wirelen is the whole frame even where the view does not cover it, so
       BPF_LEN and bh_datalen stay correct. */
    view.wirelen = wirelen;

    ami_bpf_capture(cookie, &view);
}

/*
 * AmiNetXDuo -- the transmit capture tap.
 *
 * Separate from bpf_tap.c only because it needs NX_PACKET, and therefore
 * tx_api.h / nx_api.h ahead of any exec header (same rule as
 * src/sana2/sana2_internal.h and src/mbuf/mbuf_packet.c).
 *
 * What a transmit looks like at the tap point, in cooked mode -- which is the
 * default and the only mode most SANA-II devices are safe in:
 *
 *     nx_packet_prepend_ptr ---> [ IP header ][ payload ... ]
 *
 * There is no Ethernet header. NetX Duo leaves the link header to the driver
 * and reserves NX_PHYSICAL_HEADER bytes of headroom for it; the SANA-II shim
 * then never builds one either, because CMD_WRITE takes ios2_PacketType and
 * ios2_DstAddr as separate fields and the device does the framing. So what
 * goes on the wire is a complete Ethernet frame that never exists in memory
 * anywhere.
 *
 * A DLT_EN10MB consumer must see that frame. The tap therefore reconstructs
 * the 14 bytes on the stack from exactly the three things the CMD_WRITE is
 * about to carry -- destination from nx_ip_driver_physical_address_msw/lsw,
 * source from the interface's own MAC, type from the driver command -- and
 * hands the filter a two-segment view. The packet is not touched: it is very
 * often a queued TCP segment that will be handed back for retransmission.
 *
 * In raw mode the shim has already prepended the header, so segment 0 is
 * skipped and the view is just the packet chain.
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
    ULONG       wirelen;

    if (ami_bpf_bound_channels == 0 || packet == NX_NULL)
        return;

    ifp = ami_bpf_iface_by_cookie(cookie);
    if (ifp == NULL)
        return;

    view.wirelen = 0;
    view.caplen  = 0;
    view.nsegs   = 0;

    wirelen = packet->nx_packet_length;

    if (!has_link_header && ifp->dlt == DLT_EN10MB)
    {
        /* msw carries the top two address bytes, lsw the bottom four -- the
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
                           we have and let bh_caplen < bh_datalen say so */
    }

    /* wirelen is the whole frame even where the view could not cover it, so
       BPF_LEN and bh_datalen stay truthful. */
    view.wirelen = wirelen;

    ami_bpf_capture(ifp, &view);
}

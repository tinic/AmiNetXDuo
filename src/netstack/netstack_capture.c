/*
 * AmiNetXDuo -- attaching src/bpf/ to the running stack.
 *
 * Until this file existed, `bpf_*` was a subsystem the product shipped and
 * never called: no tap in src/sana2/, no interface registered, all eight LVOs
 * pointing at bsd_enosys(), and `aminetxduo_bpf` linked by nothing but its own
 * unit test.  Two hundred and one checks passed against a workload that was
 * entirely synthetic.
 *
 * There are two capture points, and the reason there are two is structural
 * rather than a matter of taste:
 *
 *   eth0 -- the SANA-II taps in src/sana2/sana2_rx.c and sana2_tx.c.  Every
 *           frame that crosses a wire, in the exact shape the device saw it,
 *           ARP included.
 *
 *   lo0  -- the NetX Duo IP packet filter, installed here.  NetX Duo's
 *           loopback interface has no link driver at all
 *           (nx_ip_create.c:157 sets nx_interface_link_driver_entry to
 *           NX_NULL) and _nx_ip_driver_packet_send() shortcuts a loopback
 *           destination straight into _nx_ip_packet_deferred_receive().  No
 *           driver is called, so no tap on a driver can see it, and loopback
 *           is the path every throughput figure in docs/RESEARCH.md 11 was
 *           measured on.
 *
 * The loopback tap fires on NX_IP_PACKET_OUT only.  A loopback datagram is
 * sent once and received once, so capturing both directions would put two
 * identical records in the file and every analyser downstream would call the
 * second one a retransmission.  OUT is the complete record: it is taken after
 * _nx_ip_header_add(), so the IP header is real and the checksum is final.
 *
 * DLT_EN10MB for both, and lo0's fourteen bytes are synthesised here with
 * zeroed addresses.  A single link type means one pcap writer, one filter
 * program and one set of eyes in Wireshark; the alternative (DLT_NULL for
 * loopback) buys nothing and costs a second code path everywhere.
 *
 * SPDX-License-Identifier: MIT
 */

#include "netstack_internal.h"

#include "aminetxduo/bpf.h"

/* ------------------------------------------------------------ the lo0 tap */

/*
 * A cookie that is not a pointer to anything the taps also use.  The address
 * of this object identifies the loopback pseudo-interface and nothing else.
 */
static const UBYTE ami_ns_lo_cookie;

#define AMI_NS_LO_NAME      "lo0"
#define AMI_NS_LO_MTU       65535UL

static UINT ami_ns_capture_filter(NX_IP *ip_ptr, NX_PACKET *packet_ptr,
                                  UINT direction)
{
    AmiBpfView view;
    UBYTE      eth[AMI_ETH_HEADER_SIZE];
    NX_PACKET *p;
    UWORD      i;

    (VOID)ip_ptr;

    /*
     * OUT only, and only for a packet the loopback interface is carrying.
     * Anything else is on its way to (or in from) a real device, where the
     * SANA-II taps have it with its true link header.
     */
    if (direction != NX_IP_PACKET_OUT ||
        ami_bpf_capturing() == 0 ||
        packet_ptr == NX_NULL ||
        packet_ptr->nx_packet_address.nx_packet_interface_ptr !=
            &ip_ptr->nx_ip_interface[NX_LOOPBACK_INTERFACE])
    {
        return NX_SUCCESS;
    }

    for (i = 0; i < 12; i++)
        eth[i] = 0;

    /*
     * The EtherType from the IP version nibble rather than from a flag: this
     * filter is the only place both families arrive at the same call site, and
     * a dual-stack build loops IPv6 through here as well.
     */
    if ((packet_ptr->nx_packet_prepend_ptr[0] & 0xF0) == 0x60)
    {
        eth[12] = (UBYTE)(AMI_ETHERTYPE_IPV6 >> 8);
        eth[13] = (UBYTE)(AMI_ETHERTYPE_IPV6);
    }
    else
    {
        eth[12] = (UBYTE)(AMI_ETHERTYPE_IPV4 >> 8);
        eth[13] = (UBYTE)(AMI_ETHERTYPE_IPV4);
    }

    view.wirelen = 0;
    view.caplen  = 0;
    view.nsegs   = 0;

    (VOID)ami_bpf_view_add(&view, eth, (ULONG)AMI_ETH_HEADER_SIZE);

    for (p = packet_ptr; p != NX_NULL; p = p->nx_packet_next)
    {
        ULONG len;

        if (p->nx_packet_append_ptr <= p->nx_packet_prepend_ptr)
            continue;

        len = (ULONG)(p->nx_packet_append_ptr - p->nx_packet_prepend_ptr);

        if (ami_bpf_view_add(&view, p->nx_packet_prepend_ptr, len) != 0)
            break;
    }

    view.wirelen = packet_ptr->nx_packet_length + AMI_ETH_HEADER_SIZE;

    ami_bpf_tap_view((APTR)&ami_ns_lo_cookie, &view);

    /* NX_SUCCESS or the packet is dropped -- this is a filter hook that the
       stack asked a yes/no question of, and a capture always says yes. */
    return NX_SUCCESS;
}

/* ------------------------------------------------------------- injection */

static LONG ami_ns_capture_inject(APTR cookie, UWORD ether_type,
                                  const UBYTE *dst, const UBYTE *payload,
                                  ULONG len)
{
    if (cookie == (APTR)&ami_ns_lo_cookie)
        return -1;      /* nothing to inject into: loopback has no device */

    return ami_sana2_inject((AmiSana2If *)cookie, ether_type, dst, payload,
                            len);
}

/* ------------------------------------------------------------- lifecycle */

VOID ami_netstack_capture_start(AmiNetStack *ns)
{
    UWORD i;

    if (ami_bpf_init() != 0)
    {
        AMI_WARN("netstack: bpf init failed; no capture available");
        return;
    }

    for (i = 0; i < ns->ns_IfaceCount; i++)
    {
        const AmiIfConfig *cfg = &ns->ns_Config.interfaces[i];

        if (ami_bpf_attach_interface(cfg->name, ns->ns_Iface[i], DLT_EN10MB,
                                     ami_sana2_get_mtu(ns->ns_Iface[i]),
                                     ami_ns_capture_inject) != 0)
        {
            AMI_WARN("netstack: bpf could not register '%s'", cfg->name);
        }
    }

    if (ami_bpf_attach_interface(AMI_NS_LO_NAME, (APTR)&ami_ns_lo_cookie,
                                 DLT_EN10MB, AMI_NS_LO_MTU,
                                 ami_ns_capture_inject) != 0)
    {
        AMI_WARN("netstack: bpf could not register " AMI_NS_LO_NAME);
    }

    ns->ns_Ip.nx_ip_packet_filter_extended = ami_ns_capture_filter;

    AMI_INFO("netstack: capture attached (%ld interface(s) plus "
             AMI_NS_LO_NAME ")", (long)ns->ns_IfaceCount);
}

VOID ami_netstack_capture_stop(AmiNetStack *ns)
{
    UWORD i;

    /*
     * The filter first.  Detaching an interface unbinds the channels pointing
     * at it, and a filter still installed could reach a slot being zeroed.
     */
    if (ns->ns_IpCreated)
        ns->ns_Ip.nx_ip_packet_filter_extended = NX_NULL;

    ami_bpf_detach_interface((APTR)&ami_ns_lo_cookie);

    for (i = 0; i < ns->ns_IfaceCount; i++)
    {
        if (ns->ns_Iface[i] != NULL)
            ami_bpf_detach_interface(ns->ns_Iface[i]);
    }

    ami_bpf_cleanup();
}

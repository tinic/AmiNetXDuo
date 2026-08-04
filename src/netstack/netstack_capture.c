/*
 * AmiNetXDuo, attaching src/bpf/ to the running stack.
 *
 * There are two capture points:
 *
 *   eth0, the SANA-II taps in src/sana2/sana2_rx.c and sana2_tx.c.  Every
 *           frame that crosses a wire, in the exact shape the device saw it,
 *           ARP included.
 *
 *   lo0 , the NetX Duo IP packet filter, installed here.  NetX Duo's
 *           loopback interface has no link driver (nx_ip_create.c:157 sets
 *           nx_interface_link_driver_entry to NX_NULL) and
 *           _nx_ip_driver_packet_send() shortcuts a loopback destination
 *           straight into _nx_ip_packet_deferred_receive(), so no tap on a
 *           driver can see it.  Loopback is the path every throughput figure
 *           in docs/RESEARCH.md 11 was measured on.
 *
 * The loopback tap fires on NX_IP_PACKET_OUT only.  A loopback datagram is
 * sent once and received once, so capturing both directions would put two
 * identical records in the file and analysers downstream would read the second
 * as a retransmission.  The out direction is taken after _nx_ip_header_add(),
 * so the IP header is real and the checksum final.
 *
 * DLT_EN10MB for both; lo0's fourteen bytes are synthesised here with zeroed
 * addresses.  A single link type means one pcap writer and one filter program;
 * DLT_NULL for loopback would cost a second code path everywhere.
 *
 * SPDX-License-Identifier: MIT
 */

#include "netstack_internal.h"

#include "aminetxduo/bpf.h"

/* ------------------------------------------------------------ the lo0 tap */

/* The address of this object identifies the loopback pseudo-interface; it is
   not a pointer to anything the taps also use. */
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
     * Outbound only, and only for a packet the loopback interface is carrying.
     * Anything else is to or from a real device, where the SANA-II taps have
     * it with its true link header.
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
     * EtherType from the IP version nibble rather than a flag: both families
     * arrive at this one call site, and a dual-stack build loops IPv6 through
     * here as well.
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

    /* Anything other than NX_SUCCESS drops the packet; a capture never
       rejects. */
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

/*
 * The address behind a capture cookie, for AMI_BPF_SIOCGIFADDR. Read from the
 * live NX_INTERFACE every time it is asked for, so a DHCP lease that lands
 * between two calls is reflected in the second.
 */
static ULONG ami_ns_capture_address(APTR cookie)
{
    AmiNetStack *ns = ami_netstack_raw();
    UWORD        i;

    if (cookie == (APTR)&ami_ns_lo_cookie)
        return 0x7F000001UL;

    if (ns == NULL || !ns->ns_IpCreated)
        return 0;

    for (i = 0; i < ns->ns_IfaceCount; i++)
    {
        if ((APTR)ns->ns_Iface[i] != cookie)
            continue;

        if (ns->ns_Ip.nx_ip_interface[i].nx_interface_valid == 0)
            return 0;

        return ns->ns_Ip.nx_ip_interface[i].nx_interface_ip_address;
    }

    return 0;
}

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

    ami_bpf_set_address_hook(ami_ns_capture_address);

    ns->ns_Ip.nx_ip_packet_filter_extended = ami_ns_capture_filter;

    AMI_INFO("netstack: capture attached (%ld interface(s) plus "
             AMI_NS_LO_NAME ")", (long)ns->ns_IfaceCount);
}

/*
 * One interface, registered or unregistered after the stack is already up.
 * An interface added at run time through AddInterfaceTagList() must be
 * capturable like any other; one removed through RemoveInterface() must stop
 * being reachable before its AmiSana2If is freed, since src/bpf/ holds the
 * pointer as an opaque cookie and would hand a freed one to an injector.
 */
VOID ami_netstack_capture_attach_one(AmiNetStack *ns, UWORD index)
{
    const AmiIfConfig *cfg;

    /* Nothing to attach to when capture never started: ami_bpf_init() failed,
       or this is a build without src/bpf/ at all. */
    if (ns->ns_Ip.nx_ip_packet_filter_extended == NX_NULL ||
        ns->ns_Iface[index] == NULL)
        return;

    cfg = &ns->ns_Config.interfaces[index];

    if (ami_bpf_attach_interface(cfg->name, ns->ns_Iface[index], DLT_EN10MB,
                                 ami_sana2_get_mtu(ns->ns_Iface[index]),
                                 ami_ns_capture_inject) != 0)
    {
        AMI_WARN("netstack: bpf could not register '%s'", cfg->name);
    }
}

VOID ami_netstack_capture_detach_one(AmiNetStack *ns, UWORD index)
{
    if (ns->ns_Iface[index] != NULL)
        ami_bpf_detach_interface(ns->ns_Iface[index]);
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

    /* With the filter, and for the same reason: the hook reads ns_Ip. */
    ami_bpf_set_address_hook(NULL);

    ami_bpf_detach_interface((APTR)&ami_ns_lo_cookie);

    for (i = 0; i < ns->ns_IfaceCount; i++)
    {
        if (ns->ns_Iface[i] != NULL)
            ami_bpf_detach_interface(ns->ns_Iface[i]);
    }

    ami_bpf_cleanup();
}

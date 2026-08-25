/*
 * AmiNetXDuo, attaching src/bpf/ to the running stack.  Real interfaces are
 * captured by the SANA-II taps; lo0 has no link driver, so it is captured by
 * the NetX Duo IP packet filter installed here.  DLT_EN10MB for both.
 *
 * SPDX-License-Identifier: MIT
 */

#include "netstack_internal.h"

#include "aminetxduo/bpf.h"

/* ------------------------------------------------------------ the lo0 tap */

/* The address of this object identifies the loopback pseudo-interface.  It is
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
     * Outbound only, and loopback only: a loopback datagram is sent once and
     * received once, and anything on a real device already has its true link
     * header from the SANA-II taps.
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
     * arrive at this one call site in a dual-stack build.
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

    /* Anything other than NX_SUCCESS drops the packet.  A capture never
       rejects. */
    return NX_SUCCESS;
}

/* ------------------------------------------------------------- injection */

static LONG ami_ns_capture_inject(APTR cookie, UWORD ether_type,
                                  const UBYTE *dst, const UBYTE *payload,
                                  ULONG len)
{
    AmiNetStack *ns;
    UWORD index;
    LONG  rc;

    if (cookie == (APTR)&ami_ns_lo_cookie)
        return -1;      /* nothing to inject into: loopback has no device */

    /*
     * A BPF write snapshots this opaque cookie before calling us, and a
     * concurrent RemoveInterface() may already have detached its BPF row, so
     * prove the SANA-II allocation live and pin it for the whole write.
     */
    if (ami_netstack_interface_claim_cookie(cookie, &index) != AMI_NET_OK)
        return -1;

    ns = ami_netstack_raw();
    if (ns == NULL || !ns->ns_IpCreated || index >= ns->ns_IfaceCount ||
        (APTR)ns->ns_Iface[index] != cookie)
    {
        rc = -1;
    }
    else
    {
        /*
         * bpf_write() does not enter through a NetX API, so keep the online
         * test, TX-slot claim and BeginIO() on the same side of the IP
         * protection mutex as the shutdown drain.
         */
        tx_mutex_get(&ns->ns_Ip.nx_ip_protection, TX_WAIT_FOREVER);
        rc = ami_sana2_inject((AmiSana2If *)cookie, ether_type, dst, payload,
                              len);
        tx_mutex_put(&ns->ns_Ip.nx_ip_protection);
    }

    netstack_interface_release(index);

    return rc;
}

/* ------------------------------------------------------------- lifecycle */

/*
 * The address behind a capture cookie, for AMI_BPF_SIOCGIFADDR.  Read from the
 * live NX_INTERFACE every time, so a DHCP lease that lands between two calls
 * is reflected in the second.
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
        AMI_WARN("netstack: bpf init failed. No capture is available");
        return;
    }

    for (i = 0; i < ns->ns_IfaceCount; i++)
    {
        const AmiIfConfig *cfg = &ns->ns_Config.interfaces[i];

        if (ami_bpf_attach_interface(cfg->name, ns->ns_Iface[i], DLT_EN10MB,
                                     ami_sana2_get_mtu(ns->ns_Iface[i]),
                                     ami_ns_capture_inject) != 0)
        {
            AMI_WARN("netstack: bpf cannot register '%s'", cfg->name);
        }
    }

    if (ami_bpf_attach_interface(AMI_NS_LO_NAME, (APTR)&ami_ns_lo_cookie,
                                 DLT_EN10MB, AMI_NS_LO_MTU,
                                 ami_ns_capture_inject) != 0)
    {
        AMI_WARN("netstack: bpf cannot register " AMI_NS_LO_NAME);
    }

    ami_bpf_set_address_hook(ami_ns_capture_address);

    ns->ns_Ip.nx_ip_packet_filter_extended = ami_ns_capture_filter;

    AMI_INFO("netstack: capture attached (%ld interface(s) plus "
             AMI_NS_LO_NAME ")", (long)ns->ns_IfaceCount);
}

/*
 * One interface, registered or unregistered after the stack is already up.  A
 * removed one must stop being reachable before its AmiSana2If is freed,
 * because src/bpf/ holds the pointer as an opaque cookie.
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
        AMI_WARN("netstack: bpf cannot register '%s'", cfg->name);
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
     * at it, and a filter still installed can reach a slot being zeroed.
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

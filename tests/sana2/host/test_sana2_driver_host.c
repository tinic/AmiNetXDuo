/*
 * AmiNetXDuo, the NX_IP driver entry, on the host.
 *
 * src/sana2/sana2_driver.c is the whole of what NetX Duo can ask a SANA-II
 * device to do: twenty-odd NX_LINK_* commands, the binding table that finds
 * the interface each one is about, and the EtherType the send path is framed
 * with.  It had no test.
 *
 * It is a good host target for the reason the transmit test is: it is
 * dispatch, arithmetic and state, and every device-facing call it makes is an
 * extern this file can define.  What runs below is the real
 * ami_sana2_driver_entry(), the real ami_sana2_attach() and the real binding
 * table; only the things on the other side of the device -- the six SANA-II
 * entry points and four nx_ip_interface_* setters -- are answered here.
 *
 * The three that would be silent in the field:
 *
 *   A packet handed to an interface with no binding.  The driver still has to
 *   fail, and it has to release the packet first; a leak here is a packet the
 *   pool never gets back, on a machine with 1 MB and no way to notice.
 *
 *   The EtherType.  ARP and RARP come from the COMMAND, everything else from
 *   the packet's IP version.  Getting it from the packet for an ARP request
 *   puts 0x0800 on an ARP frame, which no peer answers and nothing logs.
 *
 *   The link-up flag.  It is the only thing NetX Duo knows about the wire, and
 *   AMI_LINK_STACK_DISABLE exists precisely to clear it WITHOUT stopping the
 *   readers.  A case that fell through to NX_LINK_DISABLE would take the wire
 *   away and look identical from the stack's side.
 *
 * SPDX-License-Identifier: MIT
 */

#include "sana2_internal.h"

#include "aminetxduo/netstack.h"

#include <stdio.h>
#include <string.h>

/* --------------------------------------------------------------- harness -- */

static unsigned long h_checks;
static unsigned long h_failures;

static void h_check(int ok, const char *what)
{
    h_checks++;
    if (!ok)
    {
        h_failures++;
        printf("  FAIL %s\n", what);
    }
}

/* ------------------------------------------------------------------ exec -- */

/*
 * Counted rather than empty.  ami_sana2_attach() and ami_sana2_unbind() write
 * the binding table under Forbid(), and the table is read with no lock at all
 * by ami_sana2_lookup() on the IP thread.  A Forbid() that is left standing
 * stops multitasking on the whole machine.
 */
static int h_forbid_nest;
static int h_forbid_max;

VOID Forbid(VOID)
{
    h_forbid_nest++;
    if (h_forbid_nest > h_forbid_max)
        h_forbid_max = h_forbid_nest;
}

VOID Permit(VOID)
{
    h_forbid_nest--;
}

/* ------------------------------------------------- what NetX Duo answers -- */

static ULONG h_mtu_set;
static ULONG h_phys_msw;
static ULONG h_phys_lsw;
static UINT  h_mapping;
static int   h_mapping_calls;
static int   h_releases;

UINT _nxe_ip_interface_mtu_set(NX_IP *ip_ptr, UINT interface_index, ULONG mtu_size)
{
    (VOID)ip_ptr;
    (VOID)interface_index;
    h_mtu_set = mtu_size;
    return NX_SUCCESS;
}

UINT _nxe_ip_interface_physical_address_set(NX_IP *ip_ptr, UINT interface_index,
                                            ULONG physical_msw, ULONG physical_lsw,
                                            UINT update_arp_cache)
{
    (VOID)ip_ptr;
    (VOID)interface_index;
    (VOID)update_arp_cache;
    h_phys_msw = physical_msw;
    h_phys_lsw = physical_lsw;
    return NX_SUCCESS;
}

UINT _nxe_ip_interface_address_mapping_configure(NX_IP *ip_ptr, UINT interface_index,
                                                 UINT mapping_needed)
{
    (VOID)ip_ptr;
    (VOID)interface_index;
    h_mapping = mapping_needed;
    h_mapping_calls++;
    return NX_SUCCESS;
}

#ifdef AMINETXDUO_RX_VERIFY
static ULONG h_caps_set;
static int   h_caps_calls;

UINT _nxe_ip_interface_capability_set(NX_IP *ip_ptr, UINT interface_index,
                                      ULONG interface_capability_flag)
{
    (VOID)ip_ptr;
    (VOID)interface_index;
    h_caps_set = interface_capability_flag;
    h_caps_calls++;
    return NX_SUCCESS;
}
#endif

UINT _nxe_packet_transmit_release(NX_PACKET **packet_ptr_ptr)
{
    (VOID)packet_ptr_ptr;
    h_releases++;
    return NX_SUCCESS;
}

/* -------------------------------------------------- what SANA-II answers -- */

/*
 * Every device-facing call the entry makes, recorded in order.  The order is
 * half of what several of these cases are: NX_LINK_DISABLE has to stop the
 * readers before it takes the wire away, and NX_LINK_ENABLE has to put the
 * wire back if the readers will not start.
 */
static char h_log[256];

static void h_note(const char *what)
{
    size_t used = strlen(h_log);

    snprintf(h_log + used, sizeof(h_log) - used, "%s ", what);
}

static LONG h_online_result;
static LONG h_rx_start_result;
static LONG h_tx_send_result;
static LONG h_multicast_result;
static LONG h_command_result;

/* What S2_CONFIGINTERFACE was handed, filled in by the stub below. */
static UCHAR h_cfg_addr[AMI_ETH_ADDR_SIZE];

static ULONG h_mcast_cmd;
static ULONG h_mcast_msw;
static ULONG h_mcast_lsw;

static UWORD h_sent_type;
static ULONG h_sent_msw;
static ULONG h_sent_lsw;
static int   h_sends;

LONG ami_sana2_online(AmiSana2If *iface)
{
    (VOID)iface;
    h_note("online");
    return h_online_result;
}

LONG ami_sana2_offline(AmiSana2If *iface)
{
    (VOID)iface;
    h_note("offline");
    return 0;
}

LONG ami_sana2_rx_start(AmiSana2If *iface)
{
    (VOID)iface;
    h_note("rx_start");
    return h_rx_start_result;
}

VOID ami_sana2_rx_stop(AmiSana2If *iface)
{
    (VOID)iface;
    h_note("rx_stop");
}

VOID ami_sana2_tx_drain(AmiSana2If *iface)
{
    (VOID)iface;
    h_note("tx_drain");
}

VOID ami_sana2_tx_reap(AmiSana2If *iface)
{
    (VOID)iface;
    h_note("tx_reap");
}

VOID ami_sana2_refresh_stats(AmiSana2If *iface)
{
    (VOID)iface;
    h_note("refresh");
}

UINT ami_sana2_tx_send(AmiSana2If *iface, NX_PACKET *packet, UWORD type,
                       ULONG dst_msw, ULONG dst_lsw)
{
    (VOID)iface;
    (VOID)packet;

    h_sent_type = type;
    h_sent_msw  = dst_msw;
    h_sent_lsw  = dst_lsw;
    h_sends++;
    h_note("send");

    return (UINT)h_tx_send_result;
}

LONG ami_sana2_multicast(AmiSana2If *iface, UWORD command, ULONG msw, ULONG lsw)
{
    (VOID)iface;
    h_mcast_cmd = command;
    h_mcast_msw = msw;
    h_mcast_lsw = lsw;
    h_note("mcast");
    return h_multicast_result;
}

LONG ami_sana2_command(AmiSana2If *iface, struct IOSana2Req *req, UWORD command)
{
    (VOID)iface;
    (VOID)command;

    h_note("command");

    /* S2_CONFIGINTERFACE hands the address back in the same request. */
    if (h_command_result == 0)
        memcpy(&h_cfg_addr[0], req->ios2_SrcAddr, AMI_ETH_ADDR_SIZE);

    return h_command_result;
}

VOID ami_log(int level, const char *fmt, ...)
{
    (VOID)level;
    (VOID)fmt;
}

/*
 * src/sana2/sana2_copy.c is linked in for ami_sana2_copy_bytes(), which
 * NX_LINK_SET_PHYSICAL_ADDRESS uses to publish the new address.  Its other two
 * entry points are the interrupt-time hooks and are not reached from here, but
 * they are in the same object, so their two net68k primitives and the vendored
 * checksum have to resolve.  Same three stubs as test_sana2_tx_host.c.
 */
VOID n68k_copy_bytes(UCHAR *to, const UCHAR *from, ULONG len)
{
    if (len != 0)
        memcpy(to, from, (size_t)len);
}

ULONG n68k_copy_sum_longwords(ULONG *to, const ULONG *from, ULONG count)
{
    ULONG acc = 0;

    while (count != 0UL)
    {
        ULONG w = *from++;

        *to++ = w;

        acc += w;
        if (acc < w)
            acc++;

        count--;
    }

    return acc;
}

VOID _nx_ip_packet_checksum_compute(NX_PACKET *packet_ptr)
{
    packet_ptr->nx_packet_interface_capability_flag = 0;
}

/* ------------------------------------------------------------- fixtures -- */

static AmiSana2If   iface;
static NX_IP        ip;
static NX_INTERFACE interface_obj;
static NX_PACKET    packet;
static NX_PACKET_POOL pool;

static void fixture_init(UWORD addr_bytes)
{
    memset(&iface, 0, sizeof(iface));
    memset(&ip, 0, sizeof(ip));
    memset(&interface_obj, 0, sizeof(interface_obj));
    memset(&packet, 0, sizeof(packet));

    iface.mac[0] = 0x02; iface.mac[1] = 0x11; iface.mac[2] = 0x22;
    iface.mac[3] = 0x33; iface.mac[4] = 0x44; iface.mac[5] = 0x55;
    iface.mtu        = 1500;
    iface.bps        = 10000000UL;
    iface.hw_type    = S2WireType_Ethernet;
    iface.addr_bytes = addr_bytes;

    interface_obj.nx_interface_index                = 0;
    interface_obj.nx_interface_additional_link_info = NULL;
    ip.nx_ip_default_packet_pool                    = &pool;

    h_online_result    = 0;
    h_rx_start_result  = 0;
    h_tx_send_result   = NX_SUCCESS;
    h_multicast_result = 0;
    h_command_result   = 0;
    h_log[0]           = '\0';
    h_sends            = 0;
    h_releases         = 0;
    h_mapping_calls    = 0;
#ifdef AMINETXDUO_RX_VERIFY
    h_caps_calls       = 0;
    h_caps_set         = 0;
#endif

    ami_sana2_unbind(&iface);
}

static NX_IP_DRIVER req;

static ULONG drive(UINT command)
{
    ULONG ret = 0xDEADBEEFUL;

    memset(&req, 0, sizeof(req));
    req.nx_ip_driver_command    = command;
    req.nx_ip_driver_ptr        = &ip;
    req.nx_ip_driver_interface  = &interface_obj;
    req.nx_ip_driver_return_ptr = &ret;

    ami_sana2_driver_entry(&req);

    return ret;
}

/* =============================================== the binding table ======= */

/*
 * Interface 0 is attached by nx_ip_create() itself, before anything can reach
 * its NX_INTERFACE, so the binding is recorded first and folded into
 * nx_interface_additional_link_info the first time the driver runs.  The
 * memoisation is what keeps every later command off the table walk.
 */
static void test_lookup_and_memoise(void)
{
    printf("sana2: a binding is found once and then memoised\n");

    fixture_init(AMI_ETH_ADDR_SIZE);

    /* No binding yet: the command fails and nothing is memoised. */
    h_check(drive(NX_LINK_GET_SPEED) == 0xDEADBEEFUL,
            "an unbound interface answers nothing");
    h_check(req.nx_ip_driver_status == NX_INVALID_INTERFACE,
            "and says the interface is invalid");
    h_check(interface_obj.nx_interface_additional_link_info == NULL,
            "and memoises nothing");

    h_check(ami_sana2_attach(&iface, &ip, 0) == AMI_NET_OK,
            "the binding is recorded");
    h_check(h_forbid_nest == 0, "and Forbid is balanced");
    h_check(h_forbid_max > 0, "and the table was written under it");

    h_check(drive(NX_LINK_GET_SPEED) == 10000000UL,
            "now the command is answered");
    h_check(interface_obj.nx_interface_additional_link_info == &iface,
            "and the interface carries the binding from here on");
    h_check(iface.interface_ptr == &interface_obj,
            "and the interface knows its NX_INTERFACE");

    /* Unbind clears the table, but the memo is what the driver reads first,
       so NX_LINK_INTERFACE_DETACH is what clears that. */
    ami_sana2_unbind(&iface);
    h_check(drive(NX_LINK_GET_SPEED) == 10000000UL,
            "the memo outlives the table entry, by design");

    drive(NX_LINK_INTERFACE_DETACH);
    h_check(interface_obj.nx_interface_additional_link_info == NULL,
            "detach clears the memo");
    h_check(drive(NX_LINK_GET_SPEED) == 0xDEADBEEFUL,
            "and the interface is unreachable again");
}

/* A binding for a different NX_IP, or a different index, is not this one. */
static void test_lookup_discriminates(void)
{
    NX_IP        other_ip;
    NX_INTERFACE other_index;

    printf("sana2: a binding names one IP instance and one index\n");

    fixture_init(AMI_ETH_ADDR_SIZE);
    memset(&other_ip, 0, sizeof(other_ip));
    memset(&other_index, 0, sizeof(other_index));

    h_check(ami_sana2_attach(&iface, &ip, 3) == AMI_NET_OK, "bound at index 3");

    interface_obj.nx_interface_index = 3;
    h_check(drive(NX_LINK_GET_SPEED) == 10000000UL, "index 3 is found");

    /* A second NX_INTERFACE at a different index, same IP. */
    other_index.nx_interface_index = 4;
    memset(&req, 0, sizeof(req));
    req.nx_ip_driver_command   = NX_LINK_GET_SPEED;
    req.nx_ip_driver_ptr       = &ip;
    req.nx_ip_driver_interface = &other_index;
    {
        ULONG ret = 0xDEADBEEFUL;
        req.nx_ip_driver_return_ptr = &ret;
        ami_sana2_driver_entry(&req);
        h_check(ret == 0xDEADBEEFUL, "index 4 is not this binding");
        h_check(req.nx_ip_driver_status == NX_INVALID_INTERFACE,
                "and is refused");
    }

    /* The same index on a different NX_IP. */
    other_index.nx_interface_index                = 3;
    other_index.nx_interface_additional_link_info = NULL;
    memset(&req, 0, sizeof(req));
    req.nx_ip_driver_command   = NX_LINK_GET_SPEED;
    req.nx_ip_driver_ptr       = &other_ip;
    req.nx_ip_driver_interface = &other_index;
    {
        ULONG ret = 0xDEADBEEFUL;
        req.nx_ip_driver_return_ptr = &ret;
        ami_sana2_driver_entry(&req);
        h_check(ret == 0xDEADBEEFUL, "another NX_IP is not this binding");
    }

    ami_sana2_unbind(&iface);
}

/* A hole before an existing entry must not turn a reattach into two entries,
   one current and one still reachable under the old IP/index pair. */
static void test_reattach_updates_existing_binding(void)
{
    AmiSana2If  blocker;
    NX_IP       new_ip;
    NX_INTERFACE old_interface;
    NX_INTERFACE new_interface;
    ULONG       ret;

    printf("sana2: reattach updates the existing binding across a table hole\n");

    fixture_init(AMI_ETH_ADDR_SIZE);
    memset(&blocker, 0, sizeof(blocker));
    memset(&new_ip, 0, sizeof(new_ip));
    memset(&old_interface, 0, sizeof(old_interface));
    memset(&new_interface, 0, sizeof(new_interface));

    h_check(ami_sana2_attach(&blocker, &ip, 0) == AMI_NET_OK,
            "a blocker occupies the first slot");
    h_check(ami_sana2_attach(&iface, &ip, 3) == AMI_NET_OK,
            "the interface occupies a later slot");
    ami_sana2_unbind(&blocker);                 /* leave a hole before iface */
    h_check(ami_sana2_attach(&iface, &new_ip, 4) == AMI_NET_OK,
            "reattach succeeds across the hole");

    old_interface.nx_interface_index = 3;
    ret = 0xDEADBEEFUL;
    memset(&req, 0, sizeof(req));
    req.nx_ip_driver_command    = NX_LINK_GET_SPEED;
    req.nx_ip_driver_ptr        = &ip;
    req.nx_ip_driver_interface  = &old_interface;
    req.nx_ip_driver_return_ptr = &ret;
    ami_sana2_driver_entry(&req);
    h_check(req.nx_ip_driver_status == NX_INVALID_INTERFACE,
            "the old binding was replaced, not left stale");
    h_check(ret == 0xDEADBEEFUL, "and the old request received no answer");

    new_interface.nx_interface_index = 4;
    ret = 0xDEADBEEFUL;
    memset(&req, 0, sizeof(req));
    req.nx_ip_driver_command    = NX_LINK_GET_SPEED;
    req.nx_ip_driver_ptr        = &new_ip;
    req.nx_ip_driver_interface  = &new_interface;
    req.nx_ip_driver_return_ptr = &ret;
    ami_sana2_driver_entry(&req);
    h_check(ret == iface.bps, "the replacement binding is reachable");

    ami_sana2_unbind(&iface);
    ami_sana2_unbind(&blocker);
}

/*
 * The leak.  An interface with no binding is a state the stack can reach
 * during teardown, and NX_LINK_PACKET_SEND carries a packet the pool is owed
 * back.  Failing the command without releasing it loses one packet per send,
 * silently, until the pool is empty and the machine stops networking.
 */
static void test_unbound_send_releases_the_packet(void)
{
    printf("sana2: an unbound interface does not keep the packet\n");

    fixture_init(AMI_ETH_ADDR_SIZE);

    memset(&req, 0, sizeof(req));
    req.nx_ip_driver_command   = NX_LINK_PACKET_SEND;
    req.nx_ip_driver_ptr       = &ip;
    req.nx_ip_driver_interface = &interface_obj;
    req.nx_ip_driver_packet    = &packet;

    ami_sana2_driver_entry(&req);

    h_check(req.nx_ip_driver_status == NX_INVALID_INTERFACE,
            "the send is refused");
    h_check(h_releases == 1, "and the packet goes back to the pool");
    h_check(h_sends == 0, "and nothing reached the device");
}

/* And a command with no packet must not release one that is not there. */
static void test_unbound_without_a_packet(void)
{
    printf("sana2: an unbound interface with no packet releases nothing\n");

    fixture_init(AMI_ETH_ADDR_SIZE);

    h_check(drive(NX_LINK_ENABLE) == 0xDEADBEEFUL, "the command is refused");
    h_check(req.nx_ip_driver_status == NX_INVALID_INTERFACE, "as invalid");
    h_check(h_releases == 0, "and nothing was released");
    h_check(h_log[0] == '\0', "and the device was never touched");
}

/* =================================================== the EtherType ======= */

/*
 * ARP and RARP are named by the COMMAND; everything else by the packet's IP
 * version.  An ARP frame framed as 0x0800 is answered by nobody and logged by
 * nothing: the machine simply cannot resolve an address.
 */
static void test_ether_type(void)
{
    static const struct {
        UINT  command;
        UCHAR ip_version;
        UWORD expect;
        const char *what;
    } row[] = {
        { NX_LINK_ARP_SEND,          4, AMI_ETHERTYPE_ARP,
          "an ARP request is framed as ARP" },
        { NX_LINK_ARP_RESPONSE_SEND, 4, AMI_ETHERTYPE_ARP,
          "an ARP reply is framed as ARP" },
        { NX_LINK_RARP_SEND,         4, AMI_ETHERTYPE_RARP,
          "a RARP request is framed as RARP" },
        { NX_LINK_PACKET_SEND,       4, AMI_ETHERTYPE_IPV4,
          "an IPv4 packet is framed as IPv4" },
        { NX_LINK_PACKET_SEND,       6, AMI_ETHERTYPE_IPV6,
          "an IPv6 packet is framed as IPv6" },
        { NX_LINK_PACKET_BROADCAST,  4, AMI_ETHERTYPE_IPV4,
          "a broadcast is framed by its version too" },
    };
    ULONG i;

    printf("sana2: the EtherType comes from the command, then the packet\n");

    for (i = 0; i < sizeof(row) / sizeof(row[0]); i++)
    {
        fixture_init(AMI_ETH_ADDR_SIZE);
        ami_sana2_attach(&iface, &ip, 0);

        packet.nx_packet_ip_version = row[i].ip_version;

        memset(&req, 0, sizeof(req));
        req.nx_ip_driver_command                  = row[i].command;
        req.nx_ip_driver_ptr                      = &ip;
        req.nx_ip_driver_interface                = &interface_obj;
        req.nx_ip_driver_packet                   = &packet;
        req.nx_ip_driver_physical_address_msw     = 0x0000AABBUL;
        req.nx_ip_driver_physical_address_lsw     = 0xCCDDEEFFUL;

        ami_sana2_driver_entry(&req);

        h_check(h_sends == 1 && h_sent_type == row[i].expect, row[i].what);
        h_check(h_sent_msw == 0x0000AABBUL && h_sent_lsw == 0xCCDDEEFFUL,
                "and the destination reaches the transmit path intact");

        ami_sana2_unbind(&iface);
        interface_obj.nx_interface_additional_link_info = NULL;
    }
}

/* A send command with no packet is a caller error, not a null dereference. */
static void test_send_without_a_packet(void)
{
    printf("sana2: a send with no packet is refused, not dereferenced\n");

    fixture_init(AMI_ETH_ADDR_SIZE);
    ami_sana2_attach(&iface, &ip, 0);

    h_check(drive(NX_LINK_PACKET_SEND) == 0xDEADBEEFUL, "nothing is returned");
    h_check(req.nx_ip_driver_status == NX_PTR_ERROR, "and it is a pointer error");
    h_check(h_sends == 0, "and nothing reached the device");

    ami_sana2_unbind(&iface);
}

/* The transmit path's status is the driver's status; a full ring must be
   visible to the stack or it retransmits into a queue that is not draining. */
static void test_send_status_is_passed_up(void)
{
    printf("sana2: the transmit path's answer is the driver's answer\n");

    fixture_init(AMI_ETH_ADDR_SIZE);
    ami_sana2_attach(&iface, &ip, 0);

    h_tx_send_result = NX_NO_PACKET;
    packet.nx_packet_ip_version = 4;

    memset(&req, 0, sizeof(req));
    req.nx_ip_driver_command   = NX_LINK_PACKET_SEND;
    req.nx_ip_driver_ptr       = &ip;
    req.nx_ip_driver_interface = &interface_obj;
    req.nx_ip_driver_packet    = &packet;
    ami_sana2_driver_entry(&req);

    h_check(req.nx_ip_driver_status == NX_NO_PACKET,
            "a refused send is reported as refused");

    ami_sana2_unbind(&iface);
}

/* ================================================= initialise / enable === */

/*
 * NX_LINK_INITIALIZE publishes the MAC to NetX Duo as two words.  The split is
 * not obvious and is easy to get one byte out: the top two bytes are the msw
 * and the bottom four the lsw, both big-endian in the MAC's own order.
 */
static void test_initialize_publishes_the_mac(void)
{
    printf("sana2: initialise publishes the MTU and the MAC\n");

    fixture_init(AMI_ETH_ADDR_SIZE);
    ami_sana2_attach(&iface, &ip, 0);

    drive(NX_LINK_INITIALIZE);

    h_check(iface.pool == &pool, "the packet pool is taken from the IP instance");
    h_check(h_mtu_set == 1500, "the MTU maps straight across");
    h_check(h_phys_msw == 0x00000211UL, "the MAC's top two bytes are the msw");
    h_check(h_phys_lsw == 0x22334455UL, "and its bottom four are the lsw");
    h_check(h_mapping == NX_TRUE, "and an Ethernet wire wants address mapping");

#ifdef AMINETXDUO_RX_VERIFY
    /*
     * The checksum offload NetX Duo is told about.  Receive is five protocols
     * because ami_sana2_rx_deliver() publishes a per-packet flag as well and
     * the stack requires both; transmit is TCP and ONLY TCP, because
     * ami_sana2_copy_from_buff() has a walk to fall back on for that one and
     * for nothing else.  Every extra bit advertised here is another way to put
     * a bad checksum on a wire.
     */
    h_check(h_caps_calls == 1, "the offload is advertised once");
    h_check(h_caps_set == (NX_INTERFACE_CAPABILITY_IPV4_RX_CHECKSUM |
                           NX_INTERFACE_CAPABILITY_TCP_RX_CHECKSUM |
                           NX_INTERFACE_CAPABILITY_UDP_RX_CHECKSUM |
                           NX_INTERFACE_CAPABILITY_ICMPV4_RX_CHECKSUM |
                           NX_INTERFACE_CAPABILITY_IGMP_RX_CHECKSUM |
                           NX_INTERFACE_CAPABILITY_TCP_TX_CHECKSUM),
            "receive for five protocols, transmit for TCP alone");
#endif

    ami_sana2_unbind(&iface);
}

/*
 * An addressless wire, SLIP or PPP, reports AddrFieldSize 0.  ARP has nothing
 * to resolve to there, and leaving mapping on means every send waits for an
 * ARP reply that cannot come.
 */
static void test_initialize_addressless(void)
{
    printf("sana2: an addressless wire gets no address mapping\n");

    fixture_init(0);
    ami_sana2_attach(&iface, &ip, 0);

    drive(NX_LINK_INITIALIZE);

    h_check(h_mapping_calls == 1, "mapping is configured");
    h_check(h_mapping == NX_FALSE, "and it is turned off");

    ami_sana2_unbind(&iface);
}

/*
 * Enable brings the wire up, then the readers.  If the readers will not start
 * the wire has to go back down, or the device is left online with nothing
 * reading it: every frame it receives is dropped by the driver and the stack
 * is told the link is up.
 */
static void test_enable_unwinds_on_a_failed_reader(void)
{
    printf("sana2: enable puts the wire back if the readers will not start\n");

    fixture_init(AMI_ETH_ADDR_SIZE);
    ami_sana2_attach(&iface, &ip, 0);

    drive(NX_LINK_ENABLE);
    h_check(strcmp(h_log, "online rx_start refresh ") == 0,
            "the good path is online, then readers, then counters");
    h_check(interface_obj.nx_interface_link_up == NX_TRUE, "the link is up");
    h_check(iface.admin_up == TRUE, "and the interface is administratively up");
    h_check(req.nx_ip_driver_status == NX_SUCCESS, "and it succeeded");

    /* Now the readers refuse. */
    fixture_init(AMI_ETH_ADDR_SIZE);
    ami_sana2_attach(&iface, &ip, 0);
    h_rx_start_result = -1;

    drive(NX_LINK_ENABLE);
    h_check(strcmp(h_log, "online rx_start offline ") == 0,
            "a failed reader takes the wire back down");
    h_check(interface_obj.nx_interface_link_up != NX_TRUE,
            "and the link is not claimed to be up");
    h_check(iface.admin_up == FALSE, "nor the interface");
    h_check(req.nx_ip_driver_status == NX_NOT_SUCCESSFUL, "and it failed");

    /* And a device that will not come online at all. */
    fixture_init(AMI_ETH_ADDR_SIZE);
    ami_sana2_attach(&iface, &ip, 0);
    h_online_result = -1;

    drive(NX_LINK_ENABLE);
    h_check(strcmp(h_log, "online ") == 0, "no readers are started");
    h_check(req.nx_ip_driver_status == NX_NOT_SUCCESSFUL, "and it failed");

    ami_sana2_unbind(&iface);
}

/*
 * The distinction AMI_LINK_STACK_DISABLE exists for.  SM_Down means "stop
 * transmitting", not "take the wire away": stopping the readers means
 * S2_OFFLINE, which is the only thing that returns a queued CMD_READ on a
 * device that ignores AbortIO.  A case that fell through to NX_LINK_DISABLE
 * would look the same from the stack and would put the device offline.
 */
static void test_stack_disable_leaves_the_readers(void)
{
    printf("sana2: SM_Down stops transmitting and nothing else\n");

    fixture_init(AMI_ETH_ADDR_SIZE);
    ami_sana2_attach(&iface, &ip, 0);
    interface_obj.nx_interface_link_up = NX_TRUE;
    iface.admin_up = TRUE;

    drive(AMI_LINK_STACK_DISABLE);

    h_check(strcmp(h_log, "tx_drain ") == 0,
            "only the transmit ring is drained");
    h_check(interface_obj.nx_interface_link_up == NX_FALSE, "the link is down");
    h_check(iface.admin_up == FALSE, "and the interface is administratively down");

    /* The full disable, for contrast: it does take the wire away. */
    fixture_init(AMI_ETH_ADDR_SIZE);
    ami_sana2_attach(&iface, &ip, 0);
    interface_obj.nx_interface_link_up = NX_TRUE;

    drive(NX_LINK_DISABLE);
    h_check(strcmp(h_log, "rx_stop tx_drain offline ") == 0,
            "a real disable stops the readers and goes offline");

    ami_sana2_unbind(&iface);
}

/* Uninitialise is teardown: readers, transmit ring, wire, link flag. */
static void test_uninitialize(void)
{
    printf("sana2: uninitialise tears the interface down in order\n");

    fixture_init(AMI_ETH_ADDR_SIZE);
    ami_sana2_attach(&iface, &ip, 0);
    interface_obj.nx_interface_link_up = NX_TRUE;
    iface.admin_up = TRUE;

    drive(NX_LINK_UNINITIALIZE);

    h_check(strcmp(h_log, "rx_stop tx_drain offline ") == 0,
            "readers, then the transmit ring, then the wire");
    h_check(interface_obj.nx_interface_link_up == NX_FALSE, "the link is down");
    h_check(iface.admin_up == FALSE, "and so is the interface");

    ami_sana2_unbind(&iface);
}

/* Detach unbinds, so a later command cannot reach a freed interface. */
static void test_detach_unbinds(void)
{
    printf("sana2: detach releases the binding\n");

    fixture_init(AMI_ETH_ADDR_SIZE);
    ami_sana2_attach(&iface, &ip, 0);
    drive(NX_LINK_GET_SPEED);

    drive(NX_LINK_INTERFACE_DETACH);

    h_check(strcmp(h_log, "rx_stop tx_drain ") == 0,
            "the readers and the transmit ring stop first");
    h_check(iface.interface_ptr == NULL, "the interface forgets its NX_INTERFACE");
    h_check(interface_obj.nx_interface_additional_link_info == NULL,
            "and the NX_INTERFACE forgets the interface");
    h_check(drive(NX_LINK_GET_SPEED) == 0xDEADBEEFUL,
            "so nothing can reach it afterwards");
}

/* ==================================================== multicast ========== */

/*
 * A multicast join that the device refuses is logged and swallowed: many
 * SANA-II devices answer S2ERR_NOT_SUPPORTED and pass multicast through
 * anyway, and failing the join would break IGMP and IPv6 ND on hardware that
 * works.
 */
static void test_multicast(void)
{
    printf("sana2: a refused multicast join does not fail the command\n");

    fixture_init(AMI_ETH_ADDR_SIZE);
    ami_sana2_attach(&iface, &ip, 0);

    memset(&req, 0, sizeof(req));
    req.nx_ip_driver_command              = NX_LINK_MULTICAST_JOIN;
    req.nx_ip_driver_ptr                  = &ip;
    req.nx_ip_driver_interface            = &interface_obj;
    req.nx_ip_driver_physical_address_msw = 0x00000100UL;
    req.nx_ip_driver_physical_address_lsw = 0x5E000001UL;
    h_multicast_result = -1;

    ami_sana2_driver_entry(&req);

    h_check(req.nx_ip_driver_status == NX_SUCCESS,
            "a device that will not join does not fail the command");
    h_check(h_mcast_cmd == S2_ADDMULTICASTADDRESS, "it was a join");
    h_check(h_mcast_msw == 0x00000100UL && h_mcast_lsw == 0x5E000001UL,
            "and the group address reached the device intact");

    memset(&req, 0, sizeof(req));
    req.nx_ip_driver_command              = NX_LINK_MULTICAST_LEAVE;
    req.nx_ip_driver_ptr                  = &ip;
    req.nx_ip_driver_interface            = &interface_obj;
    req.nx_ip_driver_physical_address_msw = 0x00000100UL;
    req.nx_ip_driver_physical_address_lsw = 0x5E000001UL;
    ami_sana2_driver_entry(&req);

    h_check(h_mcast_cmd == S2_DELMULTICASTADDRESS, "and a leave is a leave");
    h_check(req.nx_ip_driver_status == NX_SUCCESS, "which also cannot fail");

    ami_sana2_unbind(&iface);
}

/* ==================================================== the counters ======= */

/*
 * Every NX_LINK_GET_* writes through nx_ip_driver_return_ptr.  The three that
 * report device counters refresh them first, and the transmit count reaps the
 * ring first: a count read without that is the count as of the last frame the
 * stack happened to notice.
 */
static void test_counters(void)
{
    printf("sana2: the GET commands answer, and refresh what they must\n");

    fixture_init(AMI_ETH_ADDR_SIZE);
    ami_sana2_attach(&iface, &ip, 0);

    iface.stats.packets_received = 111;
    iface.stats.packets_sent     = 222;
    iface.stats.alloc_failures   = 3;
    iface.stats.bad_data         = 1;
    iface.stats.overruns         = 2;
    iface.stats.tx_errors        = 4;
    iface.stats.rx_errors        = 8;
    interface_obj.nx_interface_link_up = NX_TRUE;

    h_check(drive(NX_LINK_GET_STATUS) == (ULONG)NX_TRUE, "status is the link flag");
    h_check(drive(NX_LINK_GET_SPEED) == 10000000UL, "speed is the bit rate");
    h_check(drive(NX_LINK_GET_DUPLEX_TYPE) == 0, "SANA-II has no duplex");

    h_log[0] = '\0';
    h_check(drive(NX_LINK_GET_RX_COUNT) == 111, "the receive count is answered");
    h_check(strcmp(h_log, "refresh ") == 0, "after a refresh from the device");

    h_log[0] = '\0';
    h_check(drive(NX_LINK_GET_TX_COUNT) == 222, "the transmit count is answered");
    h_check(strcmp(h_log, "tx_reap ") == 0, "after the ring is reaped");

    h_log[0] = '\0';
    h_check(drive(NX_LINK_GET_ERROR_COUNT) == 1 + 2 + 4 + 8,
            "the error count is every kind of error");
    h_check(strcmp(h_log, "refresh ") == 0, "after a refresh");

    h_log[0] = '\0';
    h_check(drive(NX_LINK_GET_ALLOC_ERRORS) == 3,
            "allocation failures are ours, so no refresh");
    h_check(h_log[0] == '\0', "and the device is not touched for them");

    h_check(drive(NX_LINK_GET_INTERFACE_TYPE) == NX_INTERFACE_TYPE_ETHERNET,
            "an Ethernet wire reports Ethernet");

    iface.hw_type = S2WireType_PPP;
    h_check(drive(NX_LINK_GET_INTERFACE_TYPE) == NX_INTERFACE_TYPE_OTHER,
            "and anything else reports other");

    ami_sana2_unbind(&iface);
}

/* Deferred processing is the transmit ring's completion, and nothing else:
   a statistics refresh here would be a synchronous DoIO per frame sent. */
static void test_deferred_processing(void)
{
    printf("sana2: deferred processing reaps the ring and nothing else\n");

    fixture_init(AMI_ETH_ADDR_SIZE);
    ami_sana2_attach(&iface, &ip, 0);

    drive(NX_LINK_DEFERRED_PROCESSING);
    h_check(strcmp(h_log, "tx_reap ") == 0, "one reap, no device traffic");

    ami_sana2_unbind(&iface);
}

/* A command this driver does not implement has to say so, not succeed. */
static void test_unhandled_command(void)
{
    printf("sana2: an unknown command is reported unhandled\n");

    fixture_init(AMI_ETH_ADDR_SIZE);
    ami_sana2_attach(&iface, &ip, 0);

    drive(0x7FFF);
    h_check(req.nx_ip_driver_status == NX_UNHANDLED_COMMAND,
            "and not quietly succeeded");

    ami_sana2_unbind(&iface);
}

/* ============================================= set physical address ====== */

static void test_set_physical_address(void)
{
    printf("sana2: setting the physical address packs the two words\n");

    fixture_init(AMI_ETH_ADDR_SIZE);
    ami_sana2_attach(&iface, &ip, 0);
    memset(h_cfg_addr, 0, sizeof(h_cfg_addr));

    memset(&req, 0, sizeof(req));
    req.nx_ip_driver_command              = NX_LINK_SET_PHYSICAL_ADDRESS;
    req.nx_ip_driver_ptr                  = &ip;
    req.nx_ip_driver_interface            = &interface_obj;
    req.nx_ip_driver_physical_address_msw = 0x0000DEADUL;
    req.nx_ip_driver_physical_address_lsw = 0xBEEF1234UL;

    ami_sana2_driver_entry(&req);

    h_check(req.nx_ip_driver_status == NX_SUCCESS, "the device accepted it");
    h_check(h_cfg_addr[0] == 0xDE && h_cfg_addr[1] == 0xAD &&
            h_cfg_addr[2] == 0xBE && h_cfg_addr[3] == 0xEF &&
            h_cfg_addr[4] == 0x12 && h_cfg_addr[5] == 0x34,
            "and the two words unpack into the six bytes in order");
    h_check(iface.mac[0] == 0xDE && iface.mac[5] == 0x34,
            "and the interface's own copy is updated");

    /* S2_CONFIGINTERFACE usually fails, the unit is already configured. */
    fixture_init(AMI_ETH_ADDR_SIZE);
    ami_sana2_attach(&iface, &ip, 0);
    h_command_result = -1;

    memset(&req, 0, sizeof(req));
    req.nx_ip_driver_command   = NX_LINK_SET_PHYSICAL_ADDRESS;
    req.nx_ip_driver_ptr       = &ip;
    req.nx_ip_driver_interface = &interface_obj;
    ami_sana2_driver_entry(&req);

    h_check(req.nx_ip_driver_status == NX_NOT_SUPPORTED,
            "a device that refuses is reported as not supporting it");
    h_check(iface.mac[0] == 0x02,
            "and the interface's own address is left alone");

    ami_sana2_unbind(&iface);
}

/* ------------------------------------------------------------------ main -- */

int main(void)
{
    test_lookup_and_memoise();
    test_lookup_discriminates();
    test_reattach_updates_existing_binding();
    test_unbound_send_releases_the_packet();
    test_unbound_without_a_packet();

    test_ether_type();
    test_send_without_a_packet();
    test_send_status_is_passed_up();

    test_initialize_publishes_the_mac();
    test_initialize_addressless();
    test_enable_unwinds_on_a_failed_reader();
    test_stack_disable_leaves_the_readers();
    test_uninitialize();
    test_detach_unbinds();

    test_multicast();
    test_counters();
    test_deferred_processing();
    test_unhandled_command();
    test_set_physical_address();

    h_check(h_forbid_nest == 0, "Forbid is balanced at the end");

    printf("%lu checks, %lu failures, %s\n", h_checks, h_failures,
           (h_failures == 0) ? "PASS" : "FAIL");

    return (h_failures == 0) ? 0 : 1;
}

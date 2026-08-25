/*
 * AmiNetXDuo, Multicast Listener Discovery, driven directly rather than over
 * a wire.
 *
 * SPDX-License-Identifier: MIT
 */

#include "nx_api.h"
#include "nx_ip.h"
#include "nx_ipv6.h"
#include "nx_icmpv6.h"
#include "nx_mld.h"

#include <stdio.h>
#include <string.h>


static unsigned long h_checks;
static unsigned long h_failures;

static void h_check(int ok, const char *what)
{
    h_checks++;

    if (!ok)
    {
        h_failures++;
        printf("FAIL %s\n", what);
    }
}


static TX_THREAD h_caller_thread;

TX_THREAD     *_tx_thread_current_ptr = &h_caller_thread;
TX_THREAD      _tx_timer_thread;
UINT           _tx_thread_preempt_disable;
volatile ULONG _tx_thread_system_state;

UINT _tx_thread_interrupt_disable(VOID)
{
    return 0;
}

VOID _tx_thread_interrupt_restore(UINT previous_posture)
{
    NX_PARAMETER_NOT_USED(previous_posture);
}

TX_THREAD *_tx_thread_identify(VOID)
{
    return _tx_thread_current_ptr;
}

UINT _tx_mutex_get(TX_MUTEX *mutex_ptr, ULONG wait_option)
{
    NX_PARAMETER_NOT_USED(mutex_ptr);
    NX_PARAMETER_NOT_USED(wait_option);
    return TX_SUCCESS;
}

UINT _tx_mutex_put(TX_MUTEX *mutex_ptr)
{
    NX_PARAMETER_NOT_USED(mutex_ptr);
    return TX_SUCCESS;
}

/* One packet, handed out and taken back.  An MLD message is 36 octets at
   most and never chains, so a pool would be scaffolding for nothing. */
static NX_PACKET      h_tx_packet;
static UCHAR          h_tx_buffer[256];
static NX_PACKET_POOL h_pool;
static UINT           h_alloc_fails;

UINT _nx_packet_allocate(NX_PACKET_POOL *pool_ptr, NX_PACKET **packet_ptr,
                         ULONG allocate_size, ULONG wait_option)
{
    NX_PARAMETER_NOT_USED(pool_ptr);
    NX_PARAMETER_NOT_USED(wait_option);

    if (h_alloc_fails)
    {
        return NX_NO_PACKET;
    }

    memset(&h_tx_packet, 0, sizeof(h_tx_packet));
    memset(h_tx_buffer, 0, sizeof(h_tx_buffer));

    h_tx_packet.nx_packet_data_start  = h_tx_buffer;
    h_tx_packet.nx_packet_data_end    = h_tx_buffer + sizeof(h_tx_buffer);
    h_tx_packet.nx_packet_prepend_ptr = h_tx_buffer + allocate_size;
    h_tx_packet.nx_packet_append_ptr  = h_tx_buffer + allocate_size;

    *packet_ptr = &h_tx_packet;
    return NX_SUCCESS;
}

static UINT h_packets_released;

UINT _nx_packet_release(NX_PACKET *packet_ptr)
{
    NX_PARAMETER_NOT_USED(packet_ptr);
    h_packets_released++;
    return NX_SUCCESS;
}

UINT _nx_packet_transmit_release(NX_PACKET *packet_ptr)
{
    return _nx_packet_release(packet_ptr);
}

USHORT _nx_ip_checksum_compute(NX_PACKET *packet_ptr, ULONG protocol,
                               UINT data_length, ULONG *src_ip, ULONG *dest_ip)
{
    NX_PARAMETER_NOT_USED(packet_ptr);
    NX_PARAMETER_NOT_USED(protocol);
    NX_PARAMETER_NOT_USED(data_length);
    NX_PARAMETER_NOT_USED(src_ip);
    NX_PARAMETER_NOT_USED(dest_ip);
    return 0;
}

/* What _nx_ipv6_header_add() was asked for.  The real one is not compiled in:
   it drags the whole send path behind it, and what matters here is that MLD
   asks for protocol 0 with a hop limit of 1 and the right source. */
static UINT  h_header_adds;
static ULONG h_header_protocol;
static ULONG h_header_hop_limit;
static ULONG h_header_src[4];
static ULONG h_header_dest[4];

UINT _nx_ipv6_header_add(NX_IP *ip_ptr, NX_PACKET **packet_pptr,
                         ULONG protocol, ULONG payload_size, ULONG hop_limit,
                         ULONG traffic_class, ULONG *src_address,
                         ULONG *dest_address, ULONG *fragment)
{
    NX_PARAMETER_NOT_USED(ip_ptr);
    NX_PARAMETER_NOT_USED(payload_size);
    NX_PARAMETER_NOT_USED(traffic_class);
    NX_PARAMETER_NOT_USED(packet_pptr);
    NX_PARAMETER_NOT_USED(fragment);

    h_header_adds++;
    h_header_protocol = protocol;
    h_header_hop_limit = hop_limit;
    COPY_IPV6_ADDRESS(src_address, h_header_src);
    COPY_IPV6_ADDRESS(dest_address, h_header_dest);

    return NX_SUCCESS;
}

#define H_SENT_MAX      8

static UINT  h_sent_count;
static UCHAR h_sent[H_SENT_MAX][256];
static UINT  h_sent_length[H_SENT_MAX];
static ULONG h_sent_mac_lsw[H_SENT_MAX];
static ULONG h_sent_dest[H_SENT_MAX][4];

static VOID h_driver(NX_IP_DRIVER *request)
{
    request -> nx_ip_driver_status = NX_SUCCESS;

    if (request -> nx_ip_driver_command != NX_LINK_PACKET_SEND)
    {
        return;
    }

    if (h_sent_count < H_SENT_MAX)
    {
    NX_PACKET *packet_ptr = request -> nx_ip_driver_packet;
    UINT       length = (UINT)(packet_ptr -> nx_packet_append_ptr -
                               packet_ptr -> nx_packet_prepend_ptr);

        if (length > sizeof(h_sent[0]))
        {
            length = sizeof(h_sent[0]);
        }

        memcpy(h_sent[h_sent_count], packet_ptr -> nx_packet_prepend_ptr, length);
        h_sent_length[h_sent_count] = length;
        h_sent_mac_lsw[h_sent_count] = request -> nx_ip_driver_physical_address_lsw;
        COPY_IPV6_ADDRESS(h_header_dest, h_sent_dest[h_sent_count]);
    }

    h_sent_count++;
}


static NX_IP           h_ip;
static NXD_IPV6_ADDRESS h_linklocal;

/* The interface's own MAC tail, which is what the solicited-node group and
   the link-local address are both built from. */
#define H_MAC_TAIL      0x496d1dUL

static ULONG h_solicited[4] = {0xFF020000UL, 0, 1, 0xFF000000UL | H_MAC_TAIL};
static ULONG h_all_nodes[4] = {0xFF020000UL, 0, 0, 1};
static ULONG h_ssdp[4]      = {0xFF020000UL, 0, 0, 0xC};
static ULONG h_node_local[4] = {0xFF010000UL, 0, 0, 0x123};

static ULONG h_peer_linklocal[4] = {0xFE800000UL, 0, 0, 0x2};
static ULONG h_unspecified[4] = {0, 0, 0, 0};

static void h_reset(int address_valid)
{
    memset(&h_ip, 0, sizeof(h_ip));
    memset(&h_linklocal, 0, sizeof(h_linklocal));
    memset(&h_pool, 0, sizeof(h_pool));

    h_ip.nx_ip_id = NX_IP_ID;
    h_ip.nx_ip_default_packet_pool = &h_pool;

    h_ip.nx_ip_interface[0].nx_interface_valid = NX_TRUE;
    h_ip.nx_ip_interface[0].nx_interface_link_up = NX_TRUE;
    h_ip.nx_ip_interface[0].nx_interface_link_driver_entry = h_driver;
    h_ip.nx_ip_interface[0].nxd_interface_ipv6_address_list_head = &h_linklocal;

    h_linklocal.nxd_ipv6_address_attached = &h_ip.nx_ip_interface[0];
    h_linklocal.nxd_ipv6_address_state =
        address_valid ? NX_IPV6_ADDR_STATE_VALID : NX_IPV6_ADDR_STATE_TENTATIVE;
    h_linklocal.nxd_ipv6_address[0] = 0xFE800000UL;
    h_linklocal.nxd_ipv6_address[1] = 0;
    h_linklocal.nxd_ipv6_address[2] = 0x028010FFUL;
    h_linklocal.nxd_ipv6_address[3] = 0xFE000000UL | H_MAC_TAIL;
    h_linklocal.nxd_ipv6_address_next = NX_NULL;

    h_sent_count = 0;
    h_packets_released = 0;
    h_header_adds = 0;
    h_alloc_fails = 0;
    memset(h_sent_length, 0, sizeof(h_sent_length));

    (VOID)_nx_mld_enable(&h_ip);
}

/* An MLD message as it would arrive: the Hop-by-Hop header is already gone,
   the way _nx_ip_dispatch_process() leaves it, and the interface reaches the
   receive path through the address entry the way it really does. */
static NX_PACKET     h_rx_packet;
static UCHAR         h_rx_buffer[128];
static NX_IPV6_HEADER h_rx_header;

static void h_deliver(const UCHAR *message, UINT length, ULONG *source,
                      ULONG hop_limit)
{
    memset(&h_rx_packet, 0, sizeof(h_rx_packet));
    memset(&h_rx_header, 0, sizeof(h_rx_header));
    memcpy(h_rx_buffer, message, length);

    h_rx_header.nx_ip_header_word_1 = hop_limit;
    COPY_IPV6_ADDRESS(source, h_rx_header.nx_ip_header_source_ip);

    h_rx_packet.nx_packet_prepend_ptr = h_rx_buffer;
    h_rx_packet.nx_packet_append_ptr = h_rx_buffer + length;
    h_rx_packet.nx_packet_length = length;
    h_rx_packet.nx_packet_ip_header = (UCHAR *)&h_rx_header;
    h_rx_packet.nx_packet_address.nx_packet_ipv6_address_ptr = &h_linklocal;

    _nx_mld_packet_process(&h_ip, &h_rx_packet);
}

static void h_put_group(UCHAR *at, ULONG *group)
{
UINT i;

    for (i = 0; i < 4; i++)
    {
        at[(i * 4)]     = (UCHAR)(group[i] >> 24);
        at[(i * 4) + 1] = (UCHAR)(group[i] >> 16);
        at[(i * 4) + 2] = (UCHAR)(group[i] >> 8);
        at[(i * 4) + 3] = (UCHAR)(group[i]);
    }
}

static void h_query_v1(ULONG *group, UINT max_response_ms, ULONG *source,
                       ULONG hop_limit)
{
UCHAR message[NX_MLD_V1_MESSAGE_SIZE];

    memset(message, 0, sizeof(message));
    message[0] = NX_MLD_QUERY_TYPE;
    message[4] = (UCHAR)(max_response_ms >> 8);
    message[5] = (UCHAR)(max_response_ms);
    h_put_group(&message[8], group);

    h_deliver(message, sizeof(message), source, hop_limit);
}

static void h_query_v2(ULONG *group, UINT max_response_code)
{
UCHAR message[NX_MLD_V2_QUERY_MIN_SIZE];

    memset(message, 0, sizeof(message));
    message[0] = NX_MLD_QUERY_TYPE;
    message[4] = (UCHAR)(max_response_code >> 8);
    message[5] = (UCHAR)(max_response_code);
    h_put_group(&message[8], group);
    message[24] = 2;                    /* QRV  */
    message[25] = 125;                  /* QQIC */

    h_deliver(message, sizeof(message), h_peer_linklocal, 1);
}

static void h_report_v1(ULONG *group)
{
UCHAR message[NX_MLD_V1_MESSAGE_SIZE];

    memset(message, 0, sizeof(message));
    message[0] = NX_MLD_V1_REPORT_TYPE;
    h_put_group(&message[8], group);

    h_deliver(message, sizeof(message), h_peer_linklocal, 1);
}

/* Run the one-second timer until something is sent or the patience runs out.
   The delay is random by design, so a fixed number of ticks is the only
   honest way to ask "did it ever". */
static UINT h_tick(UINT seconds)
{
UINT before = h_sent_count;
UINT i;

    for (i = 0; i < seconds; i++)
    {
        _nx_mld_periodic_processing(&h_ip);
    }

    return h_sent_count - before;
}

static UCHAR *h_message(UINT index)
{
    return h_sent[index];
}

static UINT h_message_type(UINT index)
{
    /* The Hop-by-Hop header is the first eight octets. */
    return h_sent[index][NX_MLD_HOP_BY_HOP_SIZE];
}

static UINT h_record_type(UINT index)
{
    return h_sent[index][NX_MLD_HOP_BY_HOP_SIZE + 8];
}

static int h_group_is(UINT index, ULONG *group)
{
UCHAR expected[16];
UINT  at;

    h_put_group(expected, group);

    at = NX_MLD_HOP_BY_HOP_SIZE +
         ((h_message_type(index) == NX_MLD_V2_REPORT_TYPE) ? 12 : 8);

    return memcmp(&h_sent[index][at], expected, sizeof(expected)) == 0;
}


static void test_join_reports_immediately(void)
{
    h_reset(1);

    h_check(_nx_mld_group_join(&h_ip, h_solicited,
                               &h_ip.nx_ip_interface[0]) == NX_SUCCESS,
            "the join is accepted");
    h_check(h_sent_count == 1, "and one message goes out with it");

    if (h_sent_count == 0)
    {
        return;
    }

    h_check(h_message(0)[0] == NX_PROTOCOL_ICMPV6,
            "the Hop-by-Hop header names ICMPv6 as what follows");
    h_check(h_message(0)[1] == 0,
            "and is one eight-octet block long");
    h_check(h_message(0)[2] == NX_MLD_ROUTER_ALERT_OPTION,
            "the first option is a Router Alert");
    h_check(h_message(0)[3] == 2, "of two octets");
    h_check((h_message(0)[4] == 0) && (h_message(0)[5] == 0),
            "carrying value 0, which is 'an MLD message follows'");
    h_check(h_message(0)[6] == 1, "then a PadN");
    h_check(h_message(0)[7] == 0, "of no further octets");

    h_check(h_message_type(0) == NX_MLD_V2_REPORT_TYPE,
            "a host starts in MLDv2, so the report is type 143");
    h_check(h_record_type(0) == NX_MLD_CHANGE_TO_EXCLUDE_MODE,
            "and the record is a state change to exclude");
    h_check(h_group_is(0, h_solicited), "naming the group joined");
    h_check(h_message(0)[NX_MLD_HOP_BY_HOP_SIZE + 10] == 0 &&
            h_message(0)[NX_MLD_HOP_BY_HOP_SIZE + 11] == 0,
            "with no sources, which is what a host with no filter has");

    h_check(h_header_protocol == NX_PROTOCOL_NEXT_HEADER_HOP_BY_HOP,
            "the IPv6 header is asked for protocol 0, not 58");
    h_check(h_header_hop_limit == 1,
            "hop limit 1: an MLD message is never forwarded");
    h_check(h_sent_dest[0][0] == 0xFF020000UL && h_sent_dest[0][3] == 0x16,
            "and addressed to ff02::16, the version 2 routers");

    /* Robustness Variable is 2, so exactly one more copy is owed and no
       more, however long the timer runs. */
    h_check(h_tick(60) == 1, "the state change is repeated once and once only");
    h_check(h_message_type(1) == NX_MLD_V2_REPORT_TYPE &&
            h_record_type(1) == NX_MLD_CHANGE_TO_EXCLUDE_MODE,
            "the repeat says the same thing");
}


static void test_exemptions(void)
{
    h_reset(1);

    (VOID)_nx_mld_group_join(&h_ip, h_all_nodes, &h_ip.nx_ip_interface[0]);
    h_check(h_sent_count == 0, "ff02::1 is joined and not reported");

    (VOID)_nx_mld_group_join(&h_ip, h_node_local, &h_ip.nx_ip_interface[0]);
    h_check(h_sent_count == 0, "nor is an interface-local group");

    h_check(h_tick(60) == 0, "and no timer fires for either");

    h_query_v1(h_unspecified, 2000, h_peer_linklocal, 1);
    h_check(h_tick(30) == 0, "a general query is answered for neither");

    (VOID)_nx_mld_group_leave(&h_ip, h_all_nodes, &h_ip.nx_ip_interface[0]);
    h_check(h_sent_count == 0, "and leaving ff02::1 says nothing either");
}


static void test_tentative_address_uses_unspecified(void)
{
    h_reset(0);

    (VOID)_nx_mld_group_join(&h_ip, h_solicited, &h_ip.nx_ip_interface[0]);

    h_check(h_sent_count == 1, "a tentative address does not stop the report");
    h_check(CHECK_UNSPECIFIED_ADDRESS(h_header_src),
            "and it is sent from ::");

    h_reset(1);
    (VOID)_nx_mld_group_join(&h_ip, h_solicited, &h_ip.nx_ip_interface[0]);
    h_check(!CHECK_UNSPECIFIED_ADDRESS(h_header_src) &&
            (h_header_src[0] == 0xFE800000UL),
            "a valid link-local address is used when there is one");
}


static void test_query_is_answered(void)
{
    h_reset(1);
    (VOID)_nx_mld_group_join(&h_ip, h_solicited, &h_ip.nx_ip_interface[0]);
    (VOID)h_tick(60);
    h_sent_count = 0;

    h_query_v2(h_unspecified, 4000);
    h_check(h_sent_count == 0, "the answer is delayed, not immediate");
    h_check(h_tick(30) == 1, "and then sent, once");
    h_check(h_message_type(0) == NX_MLD_V2_REPORT_TYPE,
            "a version 2 query is answered in version 2");
    h_check(h_record_type(0) == NX_MLD_MODE_IS_EXCLUDE,
            "with the current state and not a state change");

    h_sent_count = 0;
    h_query_v1(h_solicited, 4000, h_peer_linklocal, 1);
    h_check(h_tick(30) == 1, "an address-specific query is answered");
    h_check(h_message_type(0) == NX_MLD_V1_REPORT_TYPE,
            "a version 1 query is answered in version 1");
    h_check(h_group_is(0, h_solicited), "for the group it named");

    h_sent_count = 0;
    h_query_v1(h_ssdp, 4000, h_peer_linklocal, 1);
    h_check(h_tick(30) == 0, "a query for a group not joined is not answered");
}


static void test_refusals(void)
{
ULONG global[4] = {0x20010DB8UL, 0, 0, 1};

    h_reset(1);
    (VOID)_nx_mld_group_join(&h_ip, h_solicited, &h_ip.nx_ip_interface[0]);
    (VOID)h_tick(60);
    h_sent_count = 0;

    h_query_v1(h_unspecified, 2000, h_peer_linklocal, 64);
    h_check(h_tick(30) == 0, "a query with a hop limit of 64 is not answered");

    h_query_v1(h_unspecified, 2000, global, 1);
    h_check(h_tick(30) == 0, "nor one from off-link");

    h_query_v1(h_unspecified, 2000, h_unspecified, 1);
    h_check(h_tick(30) == 0, "nor one from ::, which no querier has");

    h_query_v1(h_unspecified, 2000, h_peer_linklocal, 1);
    h_check(h_tick(30) == 1, "and the same query from a link-local source is");
}


static void test_suppression(void)
{
    h_reset(1);
    (VOID)_nx_mld_group_join(&h_ip, h_solicited, &h_ip.nx_ip_interface[0]);
    (VOID)h_tick(60);

    h_query_v1(h_unspecified, 8000, h_peer_linklocal, 1);
    h_sent_count = 0;

    h_check(h_tick(30) == 1, "with no other listener, this host answers");

    h_query_v1(h_unspecified, 8000, h_peer_linklocal, 1);
    h_sent_count = 0;
    h_report_v1(h_solicited);
    h_check(h_tick(30) == 0, "another host's report cancels the pending one");

    /* Having been suppressed, this host is not the last reporter, so the
       Done belongs to whoever was.  RFC 2710 section 5. */
    (VOID)_nx_mld_group_leave(&h_ip, h_solicited, &h_ip.nx_ip_interface[0]);
    h_check(h_sent_count == 0,
            "and a host that was suppressed sends no Done");

    /* A version 2 report from another host suppresses nothing: version 2 has
       no suppression, and a router there tracks state per host. */
    h_reset(1);
    (VOID)_nx_mld_group_join(&h_ip, h_solicited, &h_ip.nx_ip_interface[0]);
    (VOID)h_tick(60);
    h_query_v2(h_unspecified, 4000);
    h_sent_count = 0;
    h_report_v1(h_solicited);
    h_check(h_tick(30) == 1,
            "in MLDv2 a report from another host suppresses nothing");
}


static void test_leave(void)
{
    h_reset(1);
    (VOID)_nx_mld_group_join(&h_ip, h_ssdp, &h_ip.nx_ip_interface[0]);
    (VOID)h_tick(60);
    h_sent_count = 0;

    (VOID)_nx_mld_group_leave(&h_ip, h_ssdp, &h_ip.nx_ip_interface[0]);
    h_check(h_sent_count == 1, "leaving says so");
    h_check(h_message_type(0) == NX_MLD_V2_REPORT_TYPE &&
            h_record_type(0) == NX_MLD_CHANGE_TO_INCLUDE_MODE,
            "in version 2 that is a change to include mode");

    h_reset(1);
    (VOID)_nx_mld_group_join(&h_ip, h_ssdp, &h_ip.nx_ip_interface[0]);
    (VOID)h_tick(60);
    h_query_v1(h_unspecified, 1000, h_peer_linklocal, 1);
    (VOID)h_tick(30);
    h_sent_count = 0;

    (VOID)_nx_mld_group_leave(&h_ip, h_ssdp, &h_ip.nx_ip_interface[0]);
    h_check(h_sent_count == 1, "leaving says so in version 1 too");
    h_check(h_message_type(0) == NX_MLD_DONE_TYPE, "and it is a Done");
    h_check(h_group_is(0, h_ssdp), "naming the group given back");
    h_check(h_sent_mac_lsw[0] == 2,
            "addressed to ff02::2, the routers on this link");

    /* Two addresses sharing a solicited-node group are one entry, and the
       first leave announces nothing. */
    h_reset(1);
    (VOID)_nx_mld_group_join(&h_ip, h_solicited, &h_ip.nx_ip_interface[0]);
    (VOID)_nx_mld_group_join(&h_ip, h_solicited, &h_ip.nx_ip_interface[0]);
    (VOID)h_tick(60);
    h_sent_count = 0;

    (VOID)_nx_mld_group_leave(&h_ip, h_solicited, &h_ip.nx_ip_interface[0]);
    h_check(h_sent_count == 0, "the first of two leaves says nothing");
    (VOID)_nx_mld_group_leave(&h_ip, h_solicited, &h_ip.nx_ip_interface[0]);
    h_check(h_sent_count == 1, "the second is the one that does");
}


static void test_enable_and_capacity(void)
{
UINT  i;
ULONG group[4] = {0xFF020000UL, 0, 0, 0x1000};

    h_reset(1);
    h_ip.nx_ip_mld_enabled = NX_FALSE;

    (VOID)_nx_mld_group_join(&h_ip, h_solicited, &h_ip.nx_ip_interface[0]);
    h_check(h_sent_count == 0, "a join before enable announces nothing");
    h_check(h_tick(30) == 0, "and arms nothing");

    h_reset(1);
    for (i = 0; i < NX_MLD_MAX_GROUPS; i++)
    {
        group[3] = 0x1000UL + i;
        (VOID)_nx_mld_group_join(&h_ip, group, &h_ip.nx_ip_interface[0]);
    }

    group[3] = 0x2000UL;
    h_check(_nx_mld_group_join(&h_ip, group, &h_ip.nx_ip_interface[0]) ==
            NX_NO_MORE_ENTRIES,
            "a full table refuses the entry and says which way it failed");
}


int main(void)
{
    test_join_reports_immediately();
    test_exemptions();
    test_tentative_address_uses_unspecified();
    test_query_is_answered();
    test_refusals();
    test_suppression();
    test_leave();
    test_enable_and_capacity();

    printf("%lu checks, %lu failures\n", h_checks, h_failures);

    return h_failures ? 1 : 0;
}

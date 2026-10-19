/*
 * AmiNetXDuo, host regression for a DHCP server that never ACKs (#306).
 *
 * The client took an Offer, sent REQUEST after REQUEST, and stayed in
 * REQUESTING for good.  After NX_DHCP_MAX_REQUEST_RETRANS unanswered
 * retransmissions it now goes back to INIT, takes a new XID and sends a
 * DISCOVER.  This drives the real client: the Offer, ACK and NAK go in as
 * IPv4/UDP frames through _nx_ipv4_packet_receive() to the DHCP socket, and
 * the client's messages are read off the link driver.
 *
 *   a  no ACK at all: INIT, a new XID, a DISCOVER, the restart counted
 *   b  an ACK inside the window: BOUND, no restart
 *   c  broadcast and unicast ACK to a 0.0.0.0 interface: BOUND
 *   d  NAK: INIT
 *   e  an Offer or ACK with the old XID after a restart: last_reject = xid
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NX_THREAD_EXTENSION_PTR_GET(a, b, c)    { (a) = NX_NULL; }
#define NX_TIMER_EXTENSION_PTR_GET(a, b, c)     { (a) = NX_NULL; }

#include "nx_api.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include "nxd_dhcp_client.c"
#pragma GCC diagnostic pop

/* The created-pool list, reset between scenarios (nx_packet.h). */
extern NX_PACKET_POOL *_nx_packet_pool_created_ptr;
extern ULONG           _nx_packet_pool_created_count;

UINT _nx_arp_probe_send(NX_IP *ip_ptr, UINT interface_index, ULONG probe_address)
{
    NX_PARAMETER_NOT_USED(ip_ptr);
    NX_PARAMETER_NOT_USED(interface_index);
    NX_PARAMETER_NOT_USED(probe_address);
    return NX_SUCCESS;
}

static int checks, failures;
#define CHECK(c, ...) do { checks++; if (!(c)) { failures++;                  \
    printf("FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__);            \
    printf("\n"); } } while (0)

#define SERVER      0xC0A80001UL                /* 192.168.0.1   */
#define OFFERED     0xC0A8006FUL                /* 192.168.0.111 */

/* The last DHCP message the client put on the wire. */
static unsigned char sent[2048];
static ULONG         sent_len;
static int           sent_count;

static VOID capture_driver(NX_IP_DRIVER *req)
{
    NX_PACKET *packet_ptr = req -> nx_ip_driver_packet;
    ULONG      n;

    req -> nx_ip_driver_status = NX_SUCCESS;
    if (req -> nx_ip_driver_command != NX_LINK_PACKET_BROADCAST || packet_ptr == NX_NULL)
        return;

    n = (ULONG)(packet_ptr -> nx_packet_append_ptr - packet_ptr -> nx_packet_prepend_ptr);
    n = (n > 28) ? n - 28 : 0;                  /* past IPv4 (20) and UDP (8) */
    if (n > sizeof(sent))
        n = sizeof(sent);
    memcpy(sent, packet_ptr -> nx_packet_prepend_ptr + 28, n);
    sent_len = n;
    sent_count++;
    nx_packet_transmit_release(packet_ptr);
}

static NX_DHCP        g_dhcp;
static NX_IP          g_ip;
static NX_PACKET_POOL g_pool;
static ULONG          g_pool_area[(16 * (1536 + sizeof(NX_PACKET) + 64)) / sizeof(ULONG)];

static NX_DHCP_INTERFACE_RECORD *setup(void)
{
    NX_DHCP_INTERFACE_RECORD *rec;

    memset(&g_dhcp, 0, sizeof(g_dhcp));
    memset(&g_ip, 0, sizeof(g_ip));
    memset(&g_pool, 0, sizeof(g_pool));
    sent_len = 0;
    sent_count = 0;

    /* Each scenario starts afresh; the pool goes back on an empty created list. */
    _nx_packet_pool_created_ptr   = NX_NULL;
    _nx_packet_pool_created_count = 0;

    g_ip.nx_ip_id = NX_IP_ID;
    g_ip.nx_ip_interface[0].nx_interface_valid   = NX_TRUE;
    g_ip.nx_ip_interface[0].nx_interface_link_up = NX_TRUE;
    g_ip.nx_ip_interface[0].nx_interface_physical_address_msw = 0x0009;
    g_ip.nx_ip_interface[0].nx_interface_physical_address_lsw = 0x5b54032e;
    g_ip.nx_ip_interface[0].nx_interface_ip_mtu_size = 1500;
    g_ip.nx_ip_interface[0].nx_interface_ip_network = 0xFFFFFFFFUL;
    g_ip.nx_ip_interface[0].nx_interface_link_driver_entry = capture_driver;

    if (_nx_packet_pool_create(&g_pool, "dhcp", 1536, g_pool_area, sizeof(g_pool_area))
            != NX_SUCCESS)
    {
        printf("FAIL the packet pool\n");
        exit(1);
    }

    g_dhcp.nx_dhcp_ip_ptr          = &g_ip;
    g_dhcp.nx_dhcp_packet_pool_ptr = &g_pool;

    if (_nx_udp_enable(&g_ip) != NX_SUCCESS ||
        _nx_udp_socket_create(&g_ip, &g_dhcp.nx_dhcp_socket, "dhcp", NX_IP_NORMAL,
                              NX_FRAGMENT_OKAY, 64, 5) != NX_SUCCESS ||
        _nx_udp_socket_bind(&g_dhcp.nx_dhcp_socket, 68, NX_NO_WAIT) != NX_SUCCESS)
    {
        printf("FAIL the DHCP socket\n");
        exit(1);
    }

    rec = &g_dhcp.nx_dhcp_interface_record[0];
    rec->nx_dhcp_record_valid    = NX_TRUE;
    rec->nx_dhcp_interface_index = 0;
    rec->nx_dhcp_ip_address      = NX_BOOTP_NO_ADDRESS;
    rec->nx_dhcp_server_ip       = NX_BOOTP_NO_ADDRESS;
    rec->nx_dhcp_xid             = 0x12345678UL;

    /* INIT with the timer about to fire: the next tick sends the DISCOVER. */
    rec->nx_dhcp_state   = NX_DHCP_STATE_INIT;
    rec->nx_dhcp_timeout = NX_DHCP_TIME_INTERVAL;
    return rec;
}

static unsigned sum16(const unsigned char *b, unsigned n)
{
    unsigned sum = 0;

    while (n >= 2) { sum += ((unsigned)b[0] << 8) | b[1]; b += 2; n -= 2; }
    if (n) sum += (unsigned)b[0] << 8;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return sum;
}

static ULONG u32(const unsigned char *b)
{
    return ((ULONG)b[0] << 24) | ((ULONG)b[1] << 16) | ((ULONG)b[2] << 8) | b[3];
}

/* The message type and XID of the last message the client sent. */
static ULONG sent_type(void)
{
    ULONG v = 0;

    if (_nx_dhcp_get_option_value(sent, NX_DHCP_OPTION_DHCP_TYPE, &v, (UINT)sent_len) != NX_SUCCESS)
        return 0;
    return v;
}

/* A server reply, through IPv4 and UDP to the client's socket, then to the
   interface record as the DHCP thread would hand it over. */
static void reply(NX_DHCP_INTERFACE_RECORD *rec, UINT type, ULONG xid, ULONG ip_dest)
{
    NX_PACKET *pkt;
    NX_PACKET *received = NX_NULL;
    UCHAR     *b;
    UCHAR     *ip;
    UCHAR      pseudo[320];
    UINT       idx = NX_BOOTP_OFFSET_OPTIONS;
    unsigned   csum;

    CHECK(nx_packet_allocate(&g_pool, &pkt, NX_IPv4_UDP_PACKET, NX_NO_WAIT) == NX_SUCCESS,
          "reply packet");
    b = pkt->nx_packet_prepend_ptr;
    memset(b, 0, 300);
    b[0] = 2; b[1] = 1; b[2] = 6;
    _nx_dhcp_store_data(b + NX_BOOTP_OFFSET_XID, 4, xid);
    if (type != NX_DHCP_TYPE_DHCPNACK)
        _nx_dhcp_store_data(b + NX_BOOTP_OFFSET_YOUR_IP, 4, OFFERED);
    _nx_dhcp_store_data(b + NX_BOOTP_OFFSET_CLIENT_HW, 2, 0x0009);
    _nx_dhcp_store_data(b + NX_BOOTP_OFFSET_CLIENT_HW + 2, 4, 0x5b54032e);
    _nx_dhcp_store_data(b + NX_BOOTP_OFFSET_VENDOR, 4, NX_BOOTP_MAGIC_COOKIE);
    _nx_dhcp_add_option_value(b, NX_DHCP_OPTION_DHCP_TYPE, 1, type, &idx);
    _nx_dhcp_add_option_value(b, NX_DHCP_OPTION_DHCP_SERVER, 4, SERVER, &idx);
    if (type != NX_DHCP_TYPE_DHCPNACK)
    {
        _nx_dhcp_add_option_value(b, NX_DHCP_OPTION_SUBNET_MASK, 4, 0xFFFFFF00UL, &idx);
        _nx_dhcp_add_option_value(b, NX_DHCP_OPTION_DHCP_LEASE, 4, 3600, &idx);
        _nx_dhcp_add_option_value(b, NX_DHCP_OPTION_GATEWAYS, 4, SERVER, &idx);
    }
    b[idx] = NX_DHCP_OPTION_END;

    /* IPv4 and UDP headers in front, checksums right. */
    ip = b - 28;
    memset(ip, 0, 28);
    ip[0] = 0x45; ip[2] = 1; ip[3] = 72; ip[8] = 64; ip[9] = 17;
    _nx_dhcp_store_data(ip + 12, 4, SERVER);
    _nx_dhcp_store_data(ip + 16, 4, ip_dest);
    ip[21] = 67; ip[23] = 68; ip[24] = 1; ip[25] = 52;
    csum = (~sum16(ip, 20)) & 0xFFFF;
    ip[10] = (UCHAR)(csum >> 8); ip[11] = (UCHAR)csum;
    memcpy(pseudo, ip + 12, 8);
    pseudo[8] = 0; pseudo[9] = 17; pseudo[10] = 1; pseudo[11] = 52;
    memcpy(pseudo + 12, ip + 20, 308);
    csum = (~sum16(pseudo, 320)) & 0xFFFF;
    if (csum == 0)
        csum = 0xFFFF;
    ip[26] = (UCHAR)(csum >> 8); ip[27] = (UCHAR)csum;

    pkt->nx_packet_prepend_ptr   = ip;
    pkt->nx_packet_append_ptr    = ip + 328;
    pkt->nx_packet_length        = 328;
    pkt->nx_packet_ip_version    = NX_IP_VERSION_V4;
    pkt->nx_packet_ip_interface  = &g_ip.nx_ip_interface[0];
    _nx_ipv4_packet_receive(&g_ip, pkt);

    CHECK(_nx_udp_socket_receive(&g_dhcp.nx_dhcp_socket, &received, NX_NO_WAIT) == NX_SUCCESS &&
          received != NX_NULL, "IP and UDP deliver the reply to %08lx", (unsigned long)ip_dest);
    if (received != NX_NULL)
    {
        rec->nx_dhcp_replies_received++;
        _nx_dhcp_packet_process(&g_dhcp, rec, received);
    }
}

/* One second of DHCP time. */
static void tick(void)
{
    _nx_dhcp_timeout_process(&g_dhcp);
}

/* From INIT to REQUESTING: the DISCOVER, then an Offer for it. */
static void to_requesting(NX_DHCP_INTERFACE_RECORD *rec)
{
    tick();
    CHECK(rec->nx_dhcp_state == NX_DHCP_STATE_SELECTING && sent_type() == NX_DHCP_TYPE_DHCPDISCOVER,
          "INIT sends a DISCOVER (state %u)", rec->nx_dhcp_state);
    reply(rec, NX_DHCP_TYPE_DHCPOFFER, rec->nx_dhcp_xid, 0xFFFFFFFFUL);
    CHECK(rec->nx_dhcp_state == NX_DHCP_STATE_REQUESTING && sent_type() == NX_DHCP_TYPE_DHCPREQUEST,
          "the Offer is answered with a REQUEST (state %u)", rec->nx_dhcp_state);
}

static void bound(NX_DHCP_INTERFACE_RECORD *rec, const char *what)
{
    ULONG addr = 0, mask = 0;

    CHECK(rec->nx_dhcp_state == NX_DHCP_STATE_BOUND, "%s: BOUND (state %u, reject %u)",
          what, rec->nx_dhcp_state, rec->nx_dhcp_last_reject);
    _nx_ip_interface_address_get(&g_ip, 0, &addr, &mask);
    CHECK(addr == OFFERED, "%s: interface address %08lx", what, (unsigned long)addr);
}

int main(void)
{
    NX_DHCP_INTERFACE_RECORD *rec;
    ULONG                     old_xid;
    ULONG                     rejected;
    int                       seconds;

    /* a: no ACK ever.  Back to INIT after the window, then a DISCOVER with a new XID. */
    rec = setup();
    to_requesting(rec);
    CHECK(rec->nx_dhcp_offered_address == OFFERED, "offered address %08lx",
          (unsigned long)rec->nx_dhcp_offered_address);
    CHECK(rec->nx_dhcp_request_server == SERVER, "REQUEST server identifier %08lx",
          (unsigned long)rec->nx_dhcp_request_server);
    CHECK(rec->nx_dhcp_last_send_status == NX_SUCCESS, "last send status %u",
          rec->nx_dhcp_last_send_status);
    old_xid = rec->nx_dhcp_xid;
    for (seconds = 0; seconds < 600 && rec->nx_dhcp_state == NX_DHCP_STATE_REQUESTING; seconds++)
        tick();
    printf("NO_ACK seconds=%d requests=%lu restarts=%lu state=%u\n", seconds,
           (unsigned long)rec->nx_dhcp_requests_sent, (unsigned long)rec->nx_dhcp_request_restarts,
           rec->nx_dhcp_state);
    CHECK(rec->nx_dhcp_state == NX_DHCP_STATE_INIT, "no ACK: back to INIT (state %u)", rec->nx_dhcp_state);
    CHECK(rec->nx_dhcp_requests_sent == 1 + NX_DHCP_MAX_REQUEST_RETRANS,
          "no ACK: %lu REQUESTs, want %d", (unsigned long)rec->nx_dhcp_requests_sent,
          1 + NX_DHCP_MAX_REQUEST_RETRANS);
    CHECK(rec->nx_dhcp_request_restarts == 1, "no ACK: one restart counted");
    CHECK(rec->nx_dhcp_xid != old_xid, "no ACK: a new XID");
    CHECK(rec->nx_dhcp_ip_address == NX_BOOTP_NO_ADDRESS && rec->nx_dhcp_server_ip == NX_BOOTP_NO_ADDRESS,
          "no ACK: offered address and server identifier cleared");
    CHECK(rec->nx_dhcp_offered_address == OFFERED, "no ACK: the report keeps the offered address");
    tick();
    CHECK(rec->nx_dhcp_state == NX_DHCP_STATE_SELECTING && sent_type() == NX_DHCP_TYPE_DHCPDISCOVER,
          "no ACK: a fresh DISCOVER (state %u, type %lu)", rec->nx_dhcp_state, (unsigned long)sent_type());
    CHECK(u32(sent + NX_BOOTP_OFFSET_XID) == rec->nx_dhcp_xid && rec->nx_dhcp_xid != old_xid,
          "no ACK: the DISCOVER carries the new XID");
    CHECK(rec->nx_dhcp_discoveries_sent == 2, "no ACK: two DISCOVERs in all");

    /* e: a late Offer and a late ACK for the old XID are discarded. */
    rejected = rec->nx_dhcp_replies_rejected;
    reply(rec, NX_DHCP_TYPE_DHCPOFFER, old_xid, 0xFFFFFFFFUL);
    CHECK(rec->nx_dhcp_state == NX_DHCP_STATE_SELECTING && rec->nx_dhcp_last_reject == NX_DHCP_REJECT_XID &&
          rec->nx_dhcp_replies_rejected == rejected + 1, "late Offer: rejected for its XID");
    reply(rec, NX_DHCP_TYPE_DHCPACK, old_xid, 0xFFFFFFFFUL);
    CHECK(rec->nx_dhcp_state == NX_DHCP_STATE_SELECTING && rec->nx_dhcp_last_reject == NX_DHCP_REJECT_XID &&
          rec->nx_dhcp_replies_rejected == rejected + 2, "late ACK: rejected for its XID");
    CHECK(g_ip.nx_ip_interface[0].nx_interface_ip_address == 0, "late ACK: no address taken");

    /* The new transaction still completes. */
    reply(rec, NX_DHCP_TYPE_DHCPOFFER, rec->nx_dhcp_xid, 0xFFFFFFFFUL);
    CHECK(rec->nx_dhcp_state == NX_DHCP_STATE_REQUESTING, "new XID: Offer taken");
    reply(rec, NX_DHCP_TYPE_DHCPACK, rec->nx_dhcp_xid, 0xFFFFFFFFUL);
    bound(rec, "new XID");

    /* b: the ACK arrives on the last retransmission inside the window. */
    rec = setup();
    to_requesting(rec);
    for (seconds = 0; seconds < 600 && rec->nx_dhcp_request_retrans < NX_DHCP_MAX_REQUEST_RETRANS; seconds++)
        tick();
    CHECK(rec->nx_dhcp_state == NX_DHCP_STATE_REQUESTING, "window: still REQUESTING after %d s", seconds);
    reply(rec, NX_DHCP_TYPE_DHCPACK, rec->nx_dhcp_xid, OFFERED);
    bound(rec, "ACK in the window");
    CHECK(rec->nx_dhcp_request_restarts == 0, "ACK in the window: no restart");

    /* c: broadcast and unicast ACK, interface still 0.0.0.0. */
    rec = setup();
    to_requesting(rec);
    CHECK(g_ip.nx_ip_interface[0].nx_interface_ip_address == 0, "REQUESTING: interface 0.0.0.0");
    reply(rec, NX_DHCP_TYPE_DHCPACK, rec->nx_dhcp_xid, 0xFFFFFFFFUL);
    bound(rec, "broadcast ACK");

    rec = setup();
    to_requesting(rec);
    reply(rec, NX_DHCP_TYPE_DHCPACK, rec->nx_dhcp_xid, OFFERED);
    bound(rec, "unicast ACK");

    /* d: NAK goes to INIT, and INIT sends a DISCOVER. */
    rec = setup();
    to_requesting(rec);
    reply(rec, NX_DHCP_TYPE_DHCPNACK, rec->nx_dhcp_xid, 0xFFFFFFFFUL);
    CHECK(rec->nx_dhcp_state == NX_DHCP_STATE_INIT && rec->nx_dhcp_nacks_received == 1,
          "NAK: INIT (state %u)", rec->nx_dhcp_state);
    tick();
    CHECK(rec->nx_dhcp_state == NX_DHCP_STATE_SELECTING && sent_type() == NX_DHCP_TYPE_DHCPDISCOVER,
          "NAK: then a DISCOVER");

    /* INIT-REBOOT: the address the application asked for stays, as option 50 of the DISCOVER. */
    rec = setup();
    rec->nx_dhcp_ip_address     = OFFERED;
    rec->nx_dhcp_skip_discovery = NX_TRUE;
    tick();
    CHECK(rec->nx_dhcp_state == NX_DHCP_STATE_REQUESTING && sent_type() == NX_DHCP_TYPE_DHCPREQUEST,
          "INIT-REBOOT: REQUEST first");
    for (seconds = 0; seconds < 600 && rec->nx_dhcp_state == NX_DHCP_STATE_REQUESTING; seconds++)
        tick();
    CHECK(rec->nx_dhcp_state == NX_DHCP_STATE_INIT && rec->nx_dhcp_request_restarts == 1,
          "INIT-REBOOT: back to INIT (state %u)", rec->nx_dhcp_state);
    CHECK(rec->nx_dhcp_ip_address == OFFERED && rec->nx_dhcp_skip_discovery == NX_FALSE,
          "INIT-REBOOT: requested address kept, discovery no longer skipped");
    tick();
    {
        ULONG v = 0;

        CHECK(rec->nx_dhcp_state == NX_DHCP_STATE_SELECTING && sent_type() == NX_DHCP_TYPE_DHCPDISCOVER,
              "INIT-REBOOT: then a DISCOVER");
        CHECK(_nx_dhcp_get_option_value(sent, NX_DHCP_OPTION_DHCP_IP_REQ, &v, (UINT)sent_len) == NX_SUCCESS &&
              v == OFFERED, "INIT-REBOOT: the DISCOVER asks for the address");
    }

    printf("RESULT dhcp_request_restart_regression checks=%d failures=%d\n", checks, failures);
    return failures != 0;
}

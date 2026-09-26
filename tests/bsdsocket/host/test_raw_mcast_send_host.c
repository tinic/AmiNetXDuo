/* bsd_raw_send_packet() to an IPv4 group: IP_MULTICAST_TTL and IP_MULTICAST_IF
   reach the NetX send, as they do for UDP (#49).  The shipping raw.c and
   mcast.c are compiled in; NetX and the route lookup are stubbed. */
#include <stdio.h>
#include <string.h>
#include "bsdsocket_vectors.h"

static ULONG h_epoch[4];
static UINT  h_sent;
static UINT  h_index;
static UINT  h_ttl;
static LONG  h_err;

ULONG netstack_interface_epoch(UWORD index)
{
    return (index < 4) ? h_epoch[index] : 0;
}

/* The route always names interface 0. */
BOOL netstack_ipv4_route(ULONG destination, LONG preferred_index,
                         UWORD *index_out, ULONG *next_hop_out,
                         ULONG *source_address_out)
{
    (VOID)destination;
    (VOID)preferred_index;
    (VOID)next_hop_out;
    (VOID)source_address_out;
    *index_out = 0;
    return TRUE;
}

BsdSourceKind bsd_source_select(const AmiSocket *sock, const NXD_ADDRESS *dest,
                                ULONG scope, UINT *index)
{
    (VOID)sock;
    (VOID)dest;
    (VOID)scope;
    (VOID)index;
    return BSD_SOURCE_ROUTE;
}

LONG bsd_cmsg_source_index(NX_IP *ip, const BsdCmsgSource *src, BOOL v6)
{
    (VOID)ip;
    (VOID)v6;
    return (src->cs_Ifindex == 1) ? 0 : -1;
}

LONG bsd_fail(struct AmiSocketBase *base, LONG code)
{
    (VOID)base;
    h_err = code;
    return -1;
}

UINT _txe_mutex_get(TX_MUTEX *mutex_ptr, ULONG wait_option)
{
    (VOID)mutex_ptr;
    (VOID)wait_option;
    return TX_SUCCESS;
}

UINT _txe_mutex_put(TX_MUTEX *mutex_ptr)
{
    (VOID)mutex_ptr;
    return TX_SUCCESS;
}

UINT _nxe_packet_release(NX_PACKET **packet_ptr_ptr)
{
    (VOID)packet_ptr_ptr;
    return NX_SUCCESS;
}

UINT _nxde_ip_raw_packet_source_send(NX_IP *ip_ptr, NX_PACKET *packet_ptr,
                                     NXD_ADDRESS *destination_ip,
                                     UINT address_index, ULONG protocol,
                                     UINT ttl, ULONG tos)
{
    (VOID)ip_ptr;
    (VOID)packet_ptr;
    (VOID)destination_ip;
    (VOID)protocol;
    (VOID)tos;
    h_sent++;
    h_index = address_index;
    h_ttl = ttl;
    return NX_SUCCESS;
}

/* Reached only by paths these checks do not take. */
UINT _nxde_ip_raw_packet_send(NX_IP *ip_ptr, NX_PACKET **packet_ptr_ptr,
                              NXD_ADDRESS *destination_ip, ULONG protocol,
                              UINT ttl, ULONG tos)
{
    (VOID)ip_ptr;
    (VOID)packet_ptr_ptr;
    (VOID)destination_ip;
    (VOID)protocol;
    (VOID)tos;
    h_sent++;
    h_index = 98;
    h_ttl = ttl;
    return NX_SUCCESS;
}

USHORT _nx_ip_checksum_compute(NX_PACKET *packet_ptr, ULONG protocol,
                               UINT data_length, ULONG *_src_ip_addr,
                               ULONG *_dest_ip_addr)
{
    (VOID)packet_ptr;
    (VOID)protocol;
    (VOID)data_length;
    (VOID)_src_ip_addr;
    (VOID)_dest_ip_addr;
    return 0;
}

BOOL netstack_ipv6_source_find(const ULONG dest[4], LONG interface_index,
                               ULONG addr_out[4], UINT *address_index_out)
{
    (VOID)dest;
    (VOID)interface_index;
    (VOID)addr_out;
    (VOID)address_index_out;
    return FALSE;
}

LONG bsd_errno_from_nx(UINT status)
{
    return (LONG)status;
}

/* Unused raw.c and mcast.c entry points disappear with section GC. */
#include "../../../src/bsdsocket/mcast.c"
#include "../../../src/bsdsocket/raw.c"

static unsigned checks;
static unsigned failures;

static void check(int condition, const char *name)
{
    checks++;
    if (!condition)
    {
        failures++;
        printf("FAIL %s\n", name);
    }
}

static struct AmiSocketBase h_base;
static NX_IP                h_ip;
static NX_PACKET            h_packet;
static UCHAR                h_data[64];

static LONG send_to_with_source(AmiSocket *sock, ULONG dest, ULONG len,
                                const BsdCmsgSource *src)
{
    NXD_ADDRESS addr;

    memset(&addr, 0, sizeof addr);
    addr.nxd_ip_version = NX_IP_VERSION_V4;
    addr.nxd_ip_address.v4 = dest;
    memset(&h_packet, 0, sizeof h_packet);
    h_packet.nx_packet_prepend_ptr = h_data;
    h_packet.nx_packet_append_ptr = h_data + len;
    h_packet.nx_packet_length = len;
    h_sent = 0;
    h_index = 99;
    h_ttl = 0;
    return bsd_raw_send_packet(&h_base, sock, &h_packet, &addr, 0UL, src);
}

static LONG send_to(AmiSocket *sock, ULONG dest, ULONG len)
{
    return send_to_with_source(sock, dest, len, NULL);
}

int main(void)
{
    AmiSocket sock;
    const ULONG group = 0xefff2a63UL;   /* 239.255.42.99 */

    memset(&h_base, 0, sizeof h_base);
    memset(&h_ip, 0, sizeof h_ip);
    h_base.sb_StackRefs = 1;
    h_base.sb_StackIp = &h_ip;

    memset(&sock, 0, sizeof sock);
    sock.as_Owner = &h_base;
    sock.as_Protocol = 253;
    sock.as_Ttl = 64;
    sock.as_McastTtl = 1;
    sock.as_McastIf = -1;

    /* Unset options: the multicast default TTL, out of the route. */
    check(send_to(&sock, group, 8) == 0 && h_sent == 1, "default send");
    check(h_ttl == 1, "group send uses multicast default TTL 1");
    check(h_index == 0, "no IP_MULTICAST_IF leaves by the route");

    /* A unicast send keeps IP_TTL. */
    check(send_to(&sock, 0xc0a80105UL, 8) == 0 && h_sent == 1,
          "unicast send");
    check(h_ttl == 64 && h_index == 0, "unicast keeps IP_TTL and the route");

    /* IP_MULTICAST_TTL=7, IP_MULTICAST_IF=interface 1. */
    sock.as_McastTtl = 7;
    sock.as_McastIf = 1;
    sock.as_McastIfEpoch = h_epoch[1];
    check(send_to(&sock, group, 8) == 0 && h_sent == 1, "optioned send");
    check(h_ttl == 7, "group send uses IP_MULTICAST_TTL");
    check(h_index == 1, "group send leaves by IP_MULTICAST_IF");

    /* A packet's IP_PKTINFO interface overrides the socket-wide choice. */
    {
        BsdCmsgSource src;

        memset(&src, 0, sizeof src);
        src.cs_Have = TRUE;
        src.cs_Ifindex = 1; /* one-based index for interface 0 */
        check(send_to_with_source(&sock, group, 8, &src) == 0 && h_sent == 1,
              "packet-selected multicast send");
        check(h_index == 0, "IP_PKTINFO overrides IP_MULTICAST_IF");
        check(h_ttl == 7, "IP_PKTINFO leaves multicast TTL unchanged");
    }

    /* Unicast ignores both multicast options. */
    check(send_to(&sock, 0xc0a80105UL, 8) == 0 && h_ttl == 64 && h_index == 0,
          "unicast ignores multicast options");

    /* The chosen interface was detached: back to the route. */
    h_epoch[1]++;
    check(send_to(&sock, group, 8) == 0 && h_index == 0,
          "stale IP_MULTICAST_IF falls back to the route");

    /* IP_HDRINCL: the header's TTL wins, as it does for unicast. */
    sock.as_HdrIncl = 1;
    memset(h_data, 0, sizeof h_data);
    h_data[0] = 0x45;
    h_data[8] = 3;
    h_data[9] = 253;
    h_data[16] = 239; h_data[17] = 255; h_data[18] = 42; h_data[19] = 99;
    check(send_to(&sock, group, 28) == 0 && h_ttl == 3,
          "IP_HDRINCL header TTL wins");

    printf("raw mcast send: %u checks, %u failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}

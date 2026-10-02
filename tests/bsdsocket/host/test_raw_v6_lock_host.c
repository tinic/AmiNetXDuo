/* Audit N-036/N-037, raw.c's half: bsd_raw_send_v6 holds nx_ip_protection
   from the source choice through the send, so the index chosen still names
   the address sent from (the DHCPv6 client deletes addresses under that
   mutex); and a send the core refuses -- a deleted IPv6 source, an IPv4
   destination with no route -- is an error with the packet released exactly
   once.  The shipping raw.c is compiled in; NetX and source selection are
   stubbed, the mutex counts its holds. */
#include <stdio.h>
#include <string.h>
#include "bsdsocket_vectors.h"

static LONG        h_err;

ULONG netstack_interface_epoch(UWORD index)
{
    (VOID)index;
    return 0;
}

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


LONG bsd_cmsg_source_index(NX_IP *ip, const BsdCmsgSource *src, BOOL v6)
{
    (VOID)ip;
    (VOID)src;
    (VOID)v6;
    return -1;
}

LONG bsd_fail(struct AmiSocketBase *base, LONG code)
{
    (VOID)base;
    h_err = code;
    return -1;
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


/* in6.c's, as it is there. */
BOOL bsd_addr_is_v4mapped(const NXD_ADDRESS *addr, ULONG *v4)
{
    if (addr->nxd_ip_version != NX_IP_VERSION_V6)
        return FALSE;
    if (addr->nxd_ip_address.v6[0] != 0UL ||
        addr->nxd_ip_address.v6[1] != 0UL ||
        addr->nxd_ip_address.v6[2] != 0x0000FFFFUL)
        return FALSE;
    if (v4 != NULL)
        *v4 = addr->nxd_ip_address.v6[3];
    return TRUE;
}

LONG bsd_errno_from_nx(UINT status)
{
    return (LONG)status;
}


static NX_IP          h_ip;
static int            h_held;           /* nx_ip_protection hold depth */
static unsigned       h_hold_gen;       /* bumped on each 0 -> 1 take */
static int            h_select_held, h_send_held;
static unsigned       h_select_gen, h_send_gen;
static BsdSourceKind  h_kind;
static UINT           h_send_status;
static UINT           h_sent;
static UINT           h_released;

UINT _txe_mutex_get(TX_MUTEX *mutex_ptr, ULONG wait_option)
{
    (VOID)wait_option;
    if (mutex_ptr == &h_ip.nx_ip_protection && h_held++ == 0)
        h_hold_gen++;
    return TX_SUCCESS;
}

UINT _txe_mutex_put(TX_MUTEX *mutex_ptr)
{
    if (mutex_ptr == &h_ip.nx_ip_protection)
        h_held--;
    return TX_SUCCESS;
}

UINT _nxe_packet_release(NX_PACKET **packet_ptr_ptr)
{
    (VOID)packet_ptr_ptr;
    h_released++;
    return NX_SUCCESS;
}

BsdSourceKind bsd_source_select(const AmiSocket *sock, const NXD_ADDRESS *dest,
                                ULONG scope, UINT *index)
{
    (VOID)sock;
    (VOID)scope;
    h_select_held = h_held;
    h_select_gen  = h_hold_gen;
    *index = 0;
    return (dest->nxd_ip_version == NX_IP_VERSION_V6) ? h_kind
                                                       : BSD_SOURCE_ROUTE;
}

BOOL netstack_ipv6_source_find(const ULONG dest[4], LONG interface_index,
                               ULONG addr_out[4], UINT *address_index_out)
{
    (VOID)dest;
    (VOID)interface_index;
    (VOID)addr_out;
    /* The real one takes nx_ip_protection itself, recursively.  */
    h_select_held = h_held;
    h_select_gen  = h_hold_gen;
    if (address_index_out != NULL)
        *address_index_out = 1;
    return TRUE;
}

UINT _nxde_ip_raw_packet_source_send(NX_IP *ip_ptr, NX_PACKET *packet_ptr,
                                     NXD_ADDRESS *destination_ip,
                                     UINT address_index, ULONG protocol,
                                     UINT ttl, ULONG tos)
{
    (VOID)ip_ptr; (VOID)packet_ptr; (VOID)destination_ip;
    (VOID)address_index; (VOID)protocol; (VOID)ttl; (VOID)tos;
    h_send_held = h_held;
    h_send_gen  = h_hold_gen;
    h_sent++;
    return h_send_status;
}

UINT _nxde_ip_raw_packet_send(NX_IP *ip_ptr, NX_PACKET **packet_ptr_ptr,
                              NXD_ADDRESS *destination_ip, ULONG protocol,
                              UINT ttl, ULONG tos)
{
    return _nxde_ip_raw_packet_source_send(ip_ptr, *packet_ptr_ptr,
                                           destination_ip, 0, protocol, ttl,
                                           tos);
}

/* Unused raw.c and mcast.c entry points disappear with section GC. */
#include "../../../src/bsdsocket/mcast.c"
#include "../../../src/bsdsocket/raw.c"

static unsigned checks;
static unsigned failures;

static void check(int condition, const char *name)
{
    checks++;
    printf("%s %s\n", condition ? "ok  " : "FAIL", name);
    if (!condition)
        failures++;
}

static struct AmiSocketBase h_base;
static NX_PACKET            h_packet;
static UCHAR                h_data[64];
static AmiSocket            h_sock;

static LONG h_send(const NXD_ADDRESS *to, UINT status)
{
    memset(&h_packet, 0, sizeof h_packet);
    h_packet.nx_packet_prepend_ptr = h_data;
    h_packet.nx_packet_append_ptr  = h_data + 16;
    h_packet.nx_packet_length      = 16;
    h_send_status = status;
    h_sent = h_released = 0;
    h_select_held = h_send_held = -1;
    h_err = 0;
    return bsd_raw_send_packet(&h_base, &h_sock, &h_packet, to, 0UL, NULL);
}

int main(void)
{
    NXD_ADDRESS to6, to4;
    LONG        rc;

    memset(&h_base, 0, sizeof h_base);
    memset(&h_ip, 0, sizeof h_ip);
    h_base.sb_StackRefs = 1;
    h_base.sb_StackIp   = &h_ip;
    h_ip.nx_ipv6_address[0].nxd_ipv6_address_index = 0;
    h_ip.nx_ipv6_address[1].nxd_ipv6_address_index = 1;

    memset(&h_sock, 0, sizeof h_sock);
    h_sock.as_Owner    = &h_base;
    h_sock.as_Flags    = ASF_INET6;
    h_sock.as_Protocol = 17;
    h_sock.as_Ttl      = 64;
    h_sock.as_McastIf  = -1;

    memset(&to6, 0, sizeof to6);
    to6.nxd_ip_version       = NX_IP_VERSION_V6;
    to6.nxd_ip_address.v6[0] = 0x20010db8UL;
    to6.nxd_ip_address.v6[3] = 2;
    memset(&to4, 0, sizeof to4);
    to4.nxd_ip_version    = NX_IP_VERSION_V4;
    to4.nxd_ip_address.v4 = 0xc0000207UL;

    h_kind = BSD_SOURCE_INDEX;
    rc = h_send(&to6, NX_SUCCESS);
    printf("     index source: chosen with depth %d (hold %u), sent with depth %d (hold %u)\n",
           h_select_held, h_select_gen, h_send_held, h_send_gen);
    check(rc == 0 && h_sent == 1, "an IPv6 raw send with an index source is sent");
    check(h_select_held > 0 && h_send_held > 0 && h_select_gen == h_send_gen,
          "the index is chosen and sent under one nx_ip_protection hold");
    check(h_held == 0 && h_released == 0, "the mutex is released; the packet is the stack's");

    h_kind = BSD_SOURCE_ROUTE;
    rc = h_send(&to6, NX_SUCCESS);
    printf("     route source: chosen with depth %d (hold %u), sent with depth %d (hold %u)\n",
           h_select_held, h_select_gen, h_send_held, h_send_gen);
    check(rc == 0 && h_sent == 1, "an IPv6 raw send with a routed source is sent");
    check(h_select_held > 0 && h_send_held > 0 && h_select_gen == h_send_gen,
          "the routed source is found and sent under one nx_ip_protection hold");
    check(h_held == 0, "the mutex is released");

    h_kind = BSD_SOURCE_INDEX;
    rc = h_send(&to6, NX_NO_INTERFACE_ADDRESS);
    check(rc == -1 && h_err == AMI_ENETUNREACH,
          "a deleted IPv6 source refused by the send is ENETUNREACH");
    check(h_released == 1 && h_held == 0, "the packet is released once, the mutex released");

    rc = h_send(&to4, NX_IP_ADDRESS_ERROR);
    check(rc == -1 && h_err == AMI_ENETUNREACH && h_sent == 1,
          "an IPv4 destination with no route is ENETUNREACH, not success");
    check(h_released == 1, "and its packet is released exactly once");

    printf("raw v6 lock: %u checks, %u failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}

/* Audit N-036/N-037, raw.c's half, and N-080's revalidate below: bsd_raw_send_v6 holds nx_ip_protection
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

/* N-080: a release can hand its packet to a pool waiter and yield the baton,
   and the IP thread's filter then runs.  h_on_release stands in for that. */
static VOID      (*h_on_release)(NX_PACKET *packet);

UINT _nxe_packet_release(NX_PACKET **packet_ptr_ptr)
{
    h_released++;
    if (h_on_release != NULL && packet_ptr_ptr != NULL)
        h_on_release(*packet_ptr_ptr);
    return NX_SUCCESS;
}

/* The raw receive semaphore, counted: puts - gets is what a reader sees. */
static unsigned h_sem_puts, h_sem_gets;

UINT _txe_semaphore_get(TX_SEMAPHORE *semaphore_ptr, ULONG wait_option)
{
    (VOID)semaphore_ptr;
    (VOID)wait_option;
    if (h_sem_gets >= h_sem_puts)
        return TX_NO_INSTANCE;
    h_sem_gets++;
    return TX_SUCCESS;
}

BOOL bsd_bind_wants_interface(const AmiSocket *sock, const NX_INTERFACE *nxif)
{
    (VOID)sock;
    (VOID)nxif;
    return TRUE;
}

/* The IPv6 half of the receive predicates links them; the IPv4 packets the
   N-080 case queues never reach them. */
VOID bsd_in6_to_words(const UBYTE bytes[16], ULONG words[4])
{
    (VOID)bytes;
    words[0] = words[1] = words[2] = words[3] = 0UL;
}

ULONG bsd_scope_live(ULONG scope, ULONG epoch)
{
    (VOID)epoch;
    return scope;
}

UINT anx6_scope(const ULONG *addr)
{
    (VOID)addr;
    return 0xEU;
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

/* ------------------------------------------------ N-080: revalidate --- */

#define H_PEER   0x0A000001UL           /* 10.0.0.1, the connected peer */
#define H_OTHER  0x0A000002UL           /* 10.0.0.2, now rejected */

static AmiSocket h_rsock;
static NX_PACKET h_rp[3];               /* A kept, B the old tail, C the copy */
static UCHAR     h_rhdr[3][20];
static NX_PACKET *h_old_tail;

static VOID h_raw_packet(int i, ULONG source)
{
    memset(&h_rp[i], 0, sizeof h_rp[i]);
    memset(h_rhdr[i], 0, sizeof h_rhdr[i]);
    h_rhdr[i][0]  = 0x45;
    h_rhdr[i][12] = (UCHAR)(source >> 24);
    h_rhdr[i][13] = (UCHAR)(source >> 16);
    h_rhdr[i][14] = (UCHAR)(source >> 8);
    h_rhdr[i][15] = (UCHAR)source;
    h_rp[i].nx_packet_ip_version   = NX_IP_VERSION_V4;
    h_rp[i].nx_packet_ip_header    = h_rhdr[i];
    h_rp[i].nx_packet_prepend_ptr  = h_rhdr[i];
    h_rp[i].nx_packet_append_ptr   = h_rhdr[i] + 20;
    h_rp[i].nx_packet_length       = 20;
}

/* What bsd_raw_filter() does for an admitted copy (raw.c), run at the release
   of the rejected old tail, where the real release can yield the baton. */
static VOID h_filter_appends(NX_PACKET *released)
{
    NX_PACKET *copy = &h_rp[2];

    if (released != h_old_tail)
        return;

    copy->nx_packet_queue_next = NX_NULL;
    if (h_rsock.as_RawTail != NX_NULL)
        h_rsock.as_RawTail->nx_packet_queue_next = copy;
    else
        h_rsock.as_RawHead = copy;
    h_rsock.as_RawTail = copy;
    h_rsock.as_RawCount++;
    h_sem_puts++;
}

static VOID t_revalidate_yield(VOID)
{
    NX_PACKET *p;
    NX_PACKET *last = NX_NULL;
    ULONG      walked = 0;
    BOOL       has_copy = FALSE;

    memset(&h_rsock, 0, sizeof h_rsock);
    h_rsock.as_Flags      = ASF_CONNECTED;
    h_rsock.as_RawSemOk   = TRUE;
    h_rsock.as_LocalAddr.nxd_ip_version   = NX_IP_VERSION_V4;
    h_rsock.as_PeerAddr.nxd_ip_version    = NX_IP_VERSION_V4;
    h_rsock.as_PeerAddr.nxd_ip_address.v4 = H_PEER;

    h_raw_packet(0, H_PEER);
    h_raw_packet(1, H_OTHER);
    h_raw_packet(2, H_PEER);

    /* A then B queued by the filter, one put each. */
    h_rp[0].nx_packet_queue_next = &h_rp[1];
    h_rsock.as_RawHead  = &h_rp[0];
    h_rsock.as_RawTail  = &h_rp[1];
    h_rsock.as_RawCount = 2;
    h_old_tail          = &h_rp[1];
    h_sem_puts = 2;
    h_sem_gets = 0;
    h_released = 0;
    h_on_release = h_filter_appends;

    bsd_raw_revalidate_endpoint(&h_rsock);

    h_on_release = NULL;

    for (p = h_rsock.as_RawHead; p != NX_NULL && walked < 8;
         p = p->nx_packet_queue_next)
    {
        if (p == &h_rp[2])
            has_copy = TRUE;
        last = p;
        walked++;
    }

    printf("     revalidate: walked %lu, count %lu, puts %u gets %u, released %u\n",
           (unsigned long)walked, (unsigned long)h_rsock.as_RawCount,
           h_sem_puts, h_sem_gets, h_released);
    check(h_released == 1, "N-080: the rejected old tail is released once");
    check(has_copy, "N-080: the copy appended during that release is kept");
    check(h_rsock.as_RawHead == &h_rp[0] && last == h_rsock.as_RawTail &&
          (last == NX_NULL || last->nx_packet_queue_next == NX_NULL),
          "N-080: head, tail and the queue links agree");
    check(h_rsock.as_RawCount == walked,
          "N-080: as_RawCount is the length of the queue");
    check(h_sem_puts - h_sem_gets == walked,
          "N-080: the semaphore balances the queue (puts - gets == count)");
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

    t_revalidate_yield();

    printf("raw v6 lock: %u checks, %u failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}

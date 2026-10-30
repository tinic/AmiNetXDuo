/*
 * #354: ARP traffic between other hosts must not evict the entry of the host
 * this machine is sending to.
 *
 * The cache holds 19 entries on the target (AMI_ARP_CACHE_SIZE /
 * sizeof(NX_ARP)), and _nx_arp_entry_allocate() recycles the entry allocated
 * longest ago, however busy it is.  With auto entries, every ARP packet heard
 * on the LAN allocates one for its sender, so ordinary LAN chatter evicted
 * the peer of a 550 Mbit/s upload; the next segments found no mapping, two
 * waited for the resolution and ~200 were dropped (nx_ip_driver_packet_send.c,
 * NX_ARP_MAX_QUEUE_DEPTH).
 *
 * chatter: resolve one peer, then hear requests from 3 x POOL other senders
 *          for an address that is not ours.  The peer must still resolve to
 *          its hardware address, and the chatter must leave no entries.
 *
 * Linked for real: _nx_arp_enable, _nx_arp_entry_allocate,
 * _nx_arp_dynamic_entry_delete and _nx_arp_packet_receive, built against
 * port/netxduo-amiga/inc/nx_user.h.  Stubbed: ThreadX, the ARP request and
 * queue sends, the periodic walk and the packet release (counts).
 *
 * SPDX-License-Identifier: MIT
 */

#include "nx_api.h"
#include "nx_arp.h"

#include <stdio.h>
#include <string.h>

#define POOL        19u             /* AMI_ARP_CACHE_SIZE / sizeof(NX_ARP) on the target */
#define OUR_IP      0x0A000002UL    /* 10.0.0.2 */
#define PEER_IP     0x0A000009UL    /* 10.0.0.9 */
#define PEER_MSW    0x0002UL
#define PEER_LSW    0x11223344UL
#define GATEWAY_IP  0x0A000001UL    /* what the chatter asks for */
#define CHATTER     (3u * POOL)

UINT _tx_thread_interrupt_disable(VOID)            { return 0; }
VOID _tx_thread_interrupt_restore(UINT p)          { (void) p; }
UINT _tx_mutex_get(TX_MUTEX *m, ULONG w)           { (void) m; (void) w; return TX_SUCCESS; }
UINT _tx_mutex_put(TX_MUTEX *m)                    { (void) m; return TX_SUCCESS; }
UINT _txe_mutex_get(TX_MUTEX *m, ULONG w)          { (void) m; (void) w; return TX_SUCCESS; }
UINT _txe_mutex_put(TX_MUTEX *m)                   { (void) m; return TX_SUCCESS; }

static UINT released;

UINT _nx_packet_release(NX_PACKET *packet_ptr)
{
    (void) packet_ptr;
    released++;
    return NX_SUCCESS;
}

VOID _nx_packet_transmit_release(NX_PACKET *packet_ptr)
{
    (void) packet_ptr;
}

VOID _nx_arp_packet_send(NX_IP *ip_ptr, ULONG destination_ip, NX_INTERFACE *nx_interface)
{
    (void) ip_ptr; (void) destination_ip; (void) nx_interface;
}

VOID _nx_arp_queue_send(NX_IP *ip_ptr, NX_ARP *arp_ptr)
{
    (void) ip_ptr; (void) arp_ptr;
}

VOID _nx_arp_queue_process(NX_IP *ip_ptr)          { (void) ip_ptr; }
VOID _nx_arp_periodic_update(NX_IP *ip_ptr)        { (void) ip_ptr; }

static NX_IP     ip;
static NX_ARP    cache[POOL];

static UINT hash(ULONG addr)
{
    return (UINT) ((addr + (addr >> 8)) & NX_ARP_TABLE_MASK);
}

static NX_ARP *lookup(ULONG addr)
{
    NX_ARP *head = ip.nx_ip_arp_table[hash(addr)];
    NX_ARP *a    = head;

    while (a != NX_NULL)
    {
        if (a -> nx_arp_ip_address == addr)
            return a;
        a = a -> nx_arp_active_next;
        if (a == head)
            break;
    }
    return NX_NULL;
}

static void put32(UCHAR *p, ULONG v)
{
    p[0] = (UCHAR) (v >> 24); p[1] = (UCHAR) (v >> 16);
    p[2] = (UCHAR) (v >> 8);  p[3] = (UCHAR) v;
}

/* One ARP request off the wire, in network order.  */
static void hear_request(ULONG sender_ip, ULONG sender_lsw, ULONG target_ip)
{
    static ULONG   words[8];
    static NX_PACKET packet;
    UCHAR *m = (UCHAR *) words;

    memset(words, 0, sizeof(words));
    put32(m + 0, 0x00010800UL);                 /* Ethernet, IPv4 */
    put32(m + 4, 0x06040000UL | NX_ARP_OPTION_REQUEST);
    put32(m + 8, (0x0002UL << 16) | (sender_lsw >> 16));
    put32(m + 12, (sender_lsw << 16) | (sender_ip >> 16));
    put32(m + 16, sender_ip << 16);             /* target hardware 0 */
    put32(m + 24, target_ip);

    memset(&packet, 0, sizeof(packet));
    packet.nx_packet_prepend_ptr = m;
    packet.nx_packet_append_ptr  = m + NX_ARP_MESSAGE_SIZE;
    packet.nx_packet_length      = NX_ARP_MESSAGE_SIZE;
    packet.nx_packet_address.nx_packet_interface_ptr = &ip.nx_ip_interface[0];

    _nx_arp_packet_receive(&ip, &packet);
}

static int arm_chatter(void)
{
    NX_ARP *peer;
    UINT    i;
    int     bad = 0;

    memset(&ip, 0, sizeof(ip));
    ip.nx_ip_interface[0].nx_interface_valid      = NX_TRUE;
    ip.nx_ip_interface[0].nx_interface_ip_address = OUR_IP;
    ip.nx_ip_interface[0].nx_interface_physical_address_msw = 0x0002UL;
    ip.nx_ip_interface[0].nx_interface_physical_address_lsw = 0x00000002UL;
    _nx_arp_enable(&ip, cache, (ULONG) sizeof(cache));

    /* The send side's resolution: what nx_ip_driver_packet_send() leaves
       once the reply is in.  */
    if (_nx_arp_entry_allocate(&ip, &ip.nx_ip_arp_table[hash(PEER_IP)], NX_FALSE) != NX_SUCCESS)
    {
        printf("FAIL allocate peer\n");
        return 1;
    }
    peer = ip.nx_ip_arp_table[hash(PEER_IP)] -> nx_arp_active_previous;
    peer -> nx_arp_ip_address           = PEER_IP;
    peer -> nx_arp_physical_address_msw = PEER_MSW;
    peer -> nx_arp_physical_address_lsw = PEER_LSW;
    peer -> nx_arp_ip_interface         = &ip.nx_ip_interface[0];

    for (i = 0; i < CHATTER; i++)
        hear_request(0x0A000100UL + i, 0x55000000UL + i, GATEWAY_IP);

    peer = lookup(PEER_IP);
    printf("%s peer after %u requests between other hosts: %s\n",
           (peer != NX_NULL && peer -> nx_arp_physical_address_lsw == PEER_LSW) ? "ok  " : "FAIL",
           CHATTER, peer == NX_NULL ? "evicted" : "resolved");
    bad |= peer == NX_NULL || peer -> nx_arp_physical_address_lsw != PEER_LSW;

    printf("%s dynamic entries %lu (want 1), packets released %u (want %u)\n",
           (ip.nx_ip_arp_dynamic_active_count == 1 && released == CHATTER) ? "ok  " : "FAIL",
           (unsigned long) ip.nx_ip_arp_dynamic_active_count, released, CHATTER);
    bad |= ip.nx_ip_arp_dynamic_active_count != 1 || released != CHATTER;
    return bad;
}

int main(int argc, char **argv)
{
    int bad;

    if (argc != 2 || strcmp(argv[1], "chatter") != 0)
    {
        fprintf(stderr, "usage: %s chatter\n", argv[0]);
        return 2;
    }
    bad = arm_chatter();
    printf("%s\n", bad ? "FAIL" : "PASS");
    return bad;
}

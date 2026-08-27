/*
 * AmiNetXDuo, the SANA-II transmit path's framing, on the host.
 *
 * SPDX-License-Identifier: MIT
 */

#include "sana2_internal.h"

/* BeginIO(), which the transmit path posts with; the shim declares it and
   this file defines it. */
#include <inline/alib.h>

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
        printf("  FAIL %s\n", what);
    }
}

static struct IORequest *h_sent;
static unsigned long     h_sends;

VOID Disable(VOID) { }
VOID Enable(VOID)  { }
VOID Forbid(VOID)  { }
VOID Permit(VOID)  { }

/* The real one is exec's `io_Flags = 0; BeginIO()`, and the zero is why the
   transmit path does not use it. Nothing under test calls this. */
VOID SendIO(struct IORequest *req)
{
    req->io_Flags = 0;
    BeginIO(req);
}

VOID BeginIO(struct IORequest *req)
{
    h_sent = req;
    h_sends++;
}

LONG AbortIO(struct IORequest *req)
{
    (VOID)req;
    return 0;
}

/* exec's list primitives, which the reply port is built on.  The emptiness
   test in ami_sana2_tx_defer() reads lh_TailPred, so these keep exec's
   representation rather than a simpler one. */
VOID NewList(struct List *list)
{
    list->lh_Head     = (struct Node *)&list->lh_Tail;
    list->lh_Tail     = NULL;
    list->lh_TailPred = (struct Node *)list;
}

VOID AddTail(struct List *list, struct Node *node)
{
    node->ln_Succ           = (struct Node *)&list->lh_Tail;
    node->ln_Pred           = list->lh_TailPred;
    list->lh_TailPred->ln_Succ = node;
    list->lh_TailPred       = node;
}

struct Node *RemHead(struct List *list)
{
    struct Node *node = list->lh_Head;

    if (node->ln_Succ == NULL)
        return NULL;

    list->lh_Head          = node->ln_Succ;
    node->ln_Succ->ln_Pred = (struct Node *)list;

    return node;
}

VOID ReplyMsg(struct Message *msg)
{
    AddTail(&msg->mn_ReplyPort->mp_MsgList, &msg->mn_Node);
}

struct Message *GetMsg(struct MsgPort *port)
{
    return (struct Message *)RemHead(&port->mp_MsgList);
}

/* Hand the write back to its reply port, which is what a device does when the
   frame is on the wire. */
static void h_reply(void)
{
    if (h_sent != NULL)
    {
        ReplyMsg(&h_sent->io_Message);
        h_sent = NULL;
    }
}

static ULONG h_sleeps;
static ULONG h_reply_on_sleep;

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

/* Where the packet's prepend pointer was when the deferred checksum was asked
   for. The real one reads the IP header from there, so an Ethernet header in
   front of it is a checksum computed over the wrong bytes. */
static const UCHAR *h_deferred_at;

VOID _nx_ip_packet_checksum_compute(NX_PACKET *packet_ptr)
{
    h_deferred_at = packet_ptr->nx_packet_prepend_ptr;
    packet_ptr->nx_packet_interface_capability_flag = 0;
}

VOID _nx_ip_driver_deferred_processing(NX_IP *ip_ptr)
{
    (VOID)ip_ptr;
}

/* The reap releases the packet. Counted, and the packet is left alone: every
   assertion after a reap is about the shape it was handed back in. */
static unsigned long h_releases;

/* The error-checking entry points, because nx_api.h maps the nx_ names to
   them when NX_DISABLE_ERROR_CHECKING is not set, which is the host build. */
UINT _nxe_packet_transmit_release(NX_PACKET **packet_ptr_ptr)
{
    (VOID)packet_ptr_ptr;
    h_releases++;
    return NX_SUCCESS;
}

/* ami_sana2_inject() is the bpf_write() end of this file.  It is not exercised
   here, but it is compiled, so its pool calls have to resolve. */
UINT _nxe_packet_allocate(NX_PACKET_POOL *pool_ptr, NX_PACKET **packet_ptr,
                          ULONG packet_type, ULONG wait_option)
{
    (VOID)pool_ptr; (VOID)packet_ptr; (VOID)packet_type; (VOID)wait_option;
    return NX_NO_PACKET;
}

UINT _nxe_packet_data_append(NX_PACKET *packet_ptr, VOID *data_start,
                             ULONG data_size, NX_PACKET_POOL *pool_ptr,
                             ULONG wait_option)
{
    (VOID)packet_ptr; (VOID)data_start; (VOID)data_size;
    (VOID)pool_ptr; (VOID)wait_option;
    return NX_NO_PACKET;
}

UINT _nxe_packet_release(NX_PACKET **packet_ptr_ptr)
{
    (VOID)packet_ptr_ptr;
    return NX_SUCCESS;
}

UINT _tx_thread_sleep(ULONG ticks)
{
    (VOID)ticks;
    h_sleeps++;
    if (h_reply_on_sleep != 0 && h_sleeps == h_reply_on_sleep)
        h_reply();
    return TX_SUCCESS;
}

#ifdef AMINETXDUO_TX_LAZY_COLLECT
/* The lazy-collect tick's ThreadX surface.  The timer is real to the code
   under test -- create succeeds, so tx_lazy_timer_up is set and the parking
   in ami_sana2_tx_send() engages -- and this file drives the expiry itself
   rather than a running ThreadX. */
static VOID (*h_tick_fn)(ULONG);
static ULONG h_tick_arg;
static ULONG h_now;

ULONG _tx_time_get(VOID) { return h_now; }

UINT _txe_timer_create(TX_TIMER *timer_ptr, CHAR *name_ptr,
                       VOID (*expiration_function)(ULONG), ULONG input,
                       ULONG initial_ticks, ULONG reschedule_ticks,
                       UINT auto_activate, UINT size)
{
    (VOID)timer_ptr; (VOID)name_ptr; (VOID)initial_ticks;
    (VOID)reschedule_ticks; (VOID)auto_activate; (VOID)size;
    h_tick_fn  = expiration_function;
    h_tick_arg = input;
    return TX_SUCCESS;
}

UINT _txe_timer_deactivate(TX_TIMER *timer_ptr)
{
    (VOID)timer_ptr;
    return TX_SUCCESS;
}

UINT _txe_timer_delete(TX_TIMER *timer_ptr)
{
    (VOID)timer_ptr;
    h_tick_fn = NULL;
    return TX_SUCCESS;
}
#endif /* AMINETXDUO_TX_LAZY_COLLECT */

VOID ami_sana2_port_init(struct MsgPort *port, struct Task *task, BYTE sigbit,
                         UBYTE flags)
{
    (VOID)task;
    (VOID)sigbit;
    memset(port, 0, sizeof(*port));
    port->mp_Flags = flags;
    NewList(&port->mp_MsgList);
}

VOID ami_log(int level, const char *fmt, ...)
{
    (VOID)level;
    (VOID)fmt;
}

#define POOL_BYTES  256

static AmiSana2If iface;
static NX_PACKET  pkt;
static UCHAR      pool[POOL_BYTES];

static void fixture_init(BOOL raw, ULONG hw_type)
{
    memset(&iface, 0, sizeof(iface));
    iface.online     = TRUE;
    iface.raw_mode   = raw;
    iface.hw_type    = hw_type;
    iface.addr_bytes = AMI_ETH_ADDR_SIZE;
    iface.mtu        = 1500;
    memcpy(iface.mac, "\x02\x41\x4d\x49\x00\x42", AMI_ETH_ADDR_SIZE);

    ami_sana2_tx_init(&iface);

    h_sent     = NULL;
    h_sends    = 0;
    h_releases = 0;
    h_sleeps   = 0;
    h_reply_on_sleep = 0;
}

static void packet_init(const UCHAR *body, ULONG len)
{
    memset(&pkt, 0, sizeof(pkt));
    memset(pool, 0xEE, sizeof(pool));

    pkt.nx_packet_data_start  = pool;
    pkt.nx_packet_data_end    = pool + POOL_BYTES;
    pkt.nx_packet_prepend_ptr = pool + NX_PHYSICAL_HEADER;
    pkt.nx_packet_append_ptr  = pool + NX_PHYSICAL_HEADER + len;
    pkt.nx_packet_length      = len;
    pkt.nx_packet_next        = NX_NULL;

    memcpy(pkt.nx_packet_prepend_ptr, body, (size_t)len);
}

/* The write ami_sana2_tx_send() posted, i.e. the slot the device would see. */
static struct IOSana2Req *sent_req(void)
{
    return (struct IOSana2Req *)h_sent;
}

/* Drive S2_CopyFromBuff exactly as a driver does: one call for the whole
   frame, into a buffer poisoned with 0x5A so a short copy shows. */
static UCHAR devbuf[POOL_BYTES];

static BOOL device_copy(void)
{
    struct IOSana2Req *req = sent_req();

    memset(devbuf, 0x5A, sizeof(devbuf));

    return ami_sana2_copy_from_buff(devbuf, req->ios2_Data,
                                    req->ios2_DataLength);
}

static int tail_is_zero(ULONG from, ULONG to)
{
    ULONG i;

    for (i = from; i < to; i++)
    {
        if (devbuf[i] != 0)
            return 0;
    }

    return 1;
}

/* 28 bytes, the size of an ARP request, and not IPv4, so nothing on the
   transmit path can mistake it for a datagram to fuse a checksum into. */
#define ARP_LEN 28
static UCHAR arp_frame[ARP_LEN];

/* 40 bytes: an IPv4 header and a TCP header with no options, which is what a
   pure acknowledgement is and is six bytes short of the minimum. */
#define ACK_LEN 40
static UCHAR ack_frame[ACK_LEN];

static void frames_init(void)
{
    ULONG i;

    for (i = 0; i < ARP_LEN; i++)
        arp_frame[i] = (UCHAR)((i * 7 + 1) & 0xFF);
    arp_frame[0] = 0x00;
    arp_frame[1] = 0x01;                    /* hardware type 1              */

    memset(ack_frame, 0, sizeof(ack_frame));
    ack_frame[0]  = 0x45;                   /* IPv4, ihl 5                  */
    ack_frame[3]  = ACK_LEN;                /* total length                 */
    ack_frame[9]  = 6;                      /* TCP                          */
    ack_frame[12] = 10; ack_frame[15] = 1;
    ack_frame[16] = 10; ack_frame[19] = 2;
    ack_frame[20] = 0x30; ack_frame[21] = 0x39;
    ack_frame[22] = 0x00; ack_frame[23] = 0x50;
    ack_frame[32] = 0x50;                   /* data offset 5                */
    ack_frame[33] = 0x10;                   /* ACK                          */
}

static void test_pad_cooked_no_fusion(void)
{
    printf("sana2: a short cooked frame is padded to 46 bytes of payload\n");

    fixture_init(FALSE, S2WireType_Ethernet);
    packet_init(arp_frame, ARP_LEN);

    h_check(ami_sana2_tx_send(&iface, &pkt, AMI_ETHERTYPE_ARP, 0xFFFF,
                              0xFFFFFFFF) == NX_SUCCESS,
            "the write is posted");
    h_check(h_sends == 1, "and exactly one write is posted");
    h_check(sent_req()->ios2_DataLength == 46,
            "and the device is told 46 bytes, which is 60 behind a header");
    h_check(pkt.nx_packet_length == 46, "and the packet agrees with it");

    h_check(device_copy() == TRUE, "the copy hook hands over all 46");
    h_check(memcmp(devbuf, arp_frame, ARP_LEN) == 0,
            "and the first 28 are the frame");
    h_check(tail_is_zero(ARP_LEN, 46),
            "and the 18 after it are zero, not the pool's last tenant");
    h_check(devbuf[46] == 0x5A, "and nothing was written past the frame");

    /* The packet goes back to NetX Duo the length it arrived with: a queued
       TCP segment is handed back for retransmission and carries its own. */
    h_reply();
    ami_sana2_tx_reap(&iface);
    h_check(h_releases == 1, "the reap releases the packet");
    h_check(pkt.nx_packet_length == ARP_LEN,
            "and hands it back at its own length");
    h_check(pkt.nx_packet_append_ptr ==
            pool + NX_PHYSICAL_HEADER + ARP_LEN,
            "and with its append pointer back where it was");
    h_check(iface.stats.packets_sent == 1 && iface.stats.tx_errors == 0,
            "and it counts as sent");
}

static void test_pad_cooked_with_fusion(void)
{
    printf("sana2: a short cooked frame is padded through the fusion too\n");

    fixture_init(FALSE, S2WireType_Ethernet);
    packet_init(ack_frame, ACK_LEN);
    pkt.nx_packet_interface_capability_flag =
        NX_INTERFACE_CAPABILITY_TCP_TX_CHECKSUM;

    h_check(ami_sana2_tx_send(&iface, &pkt, AMI_ETHERTYPE_IPV4, 0xBC24,
                              0x11EF103A) == NX_SUCCESS,
            "the write is posted");
    h_check(sent_req()->ios2_DataLength == 46,
            "and the device is told 46 bytes");

    h_check(device_copy() == TRUE, "the copy hook hands over all 46");
    h_check((pkt.nx_packet_interface_capability_flag &
             NX_INTERFACE_CAPABILITY_TCP_TX_CHECKSUM) == 0,
            "and the fusion, not the deferred path, answered for the checksum");

    /* Everything but the checksum field, which the fusion fills in. */
    h_check(memcmp(devbuf, ack_frame, 36) == 0 &&
            memcmp(devbuf + 38, ack_frame + 38, ACK_LEN - 38) == 0,
            "and the first 40 are the datagram");
    h_check(tail_is_zero(ACK_LEN, 46), "and the 6 after it are zero");
    h_check(devbuf[46] == 0x5A, "and nothing was written past the frame");

    h_reply();
    ami_sana2_tx_reap(&iface);
    h_check(pkt.nx_packet_length == ACK_LEN,
            "and the packet goes back at its own length");
}

static void test_pad_raw(void)
{
    printf("sana2: a short raw frame is padded to 60, header included\n");

    fixture_init(TRUE, S2WireType_Ethernet);
    packet_init(arp_frame, ARP_LEN);

    h_check(ami_sana2_tx_send(&iface, &pkt, AMI_ETHERTYPE_ARP, 0xFFFF,
                              0xFFFFFFFF) == NX_SUCCESS,
            "the write is posted");
    h_check(sent_req()->ios2_DataLength == 60,
            "and the device is told 60, because the header is in the packet");

    h_check(device_copy() == TRUE, "the copy hook hands over all 60");
    h_check(devbuf[0] == 0xFF && devbuf[5] == 0xFF,
            "and it starts with the destination address");
    h_check(memcmp(devbuf + AMI_ETH_HEADER_SIZE, arp_frame, ARP_LEN) == 0,
            "and carries the frame behind the header");
    h_check(tail_is_zero(AMI_ETH_HEADER_SIZE + ARP_LEN, 60),
            "and the 18 after it are zero");

    h_reply();
    ami_sana2_tx_reap(&iface);
    h_check(pkt.nx_packet_length == ARP_LEN,
            "and the packet goes back at its own length, header off again");
    h_check(pkt.nx_packet_prepend_ptr == pool + NX_PHYSICAL_HEADER,
            "and with its prepend pointer back where it was");
}

static void test_high_ethertype_goes_raw(void)
{
    printf("sana2: a cooked device gets an EtherType over 0x8000 raw\n");

    fixture_init(FALSE, S2WireType_Ethernet);
    packet_init(arp_frame, ARP_LEN);

    h_check(ami_sana2_tx_send(&iface, &pkt, AMI_ETHERTYPE_IPV6, 0x3333,
                              0x00000002) == NX_SUCCESS,
            "the write is posted");
    h_check((sent_req()->ios2_Req.io_Flags & SANA2IOF_RAW) != 0,
            "and it carries SANA2IOF_RAW");
    h_check(sent_req()->ios2_DataLength == 60,
            "and its length counts the header this shim built");

    h_check(device_copy() == TRUE, "the copy hook hands over all 60");
    h_check(devbuf[0] == 0x33 && devbuf[1] == 0x33 && devbuf[5] == 0x02,
            "and the frame starts with the destination address");
    h_check(memcmp(devbuf + 6, iface.mac, AMI_ETH_ADDR_SIZE) == 0,
            "then the station address");
    h_check(devbuf[12] == 0x86 && devbuf[13] == 0xDD,
            "then the EtherType, which is the byte pair the driver loses");

    h_reply();
    ami_sana2_tx_reap(&iface);
    h_check(pkt.nx_packet_length == ARP_LEN,
            "and the packet goes back at its own length, header off again");
    h_check(pkt.nx_packet_prepend_ptr == pool + NX_PHYSICAL_HEADER,
            "and with its prepend pointer back where it was");
}

static void test_high_ethertype_checksum_first(void)
{
    printf("sana2: the deferred checksum is taken before the header goes on\n");

    fixture_init(FALSE, S2WireType_Ethernet);
    packet_init(ack_frame, ACK_LEN);
    pkt.nx_packet_interface_capability_flag =
        NX_INTERFACE_CAPABILITY_TCP_TX_CHECKSUM;
    h_deferred_at = NULL;

    h_check(ami_sana2_tx_send(&iface, &pkt, AMI_ETHERTYPE_IPV6, 0x3333,
                              0x00000002) == NX_SUCCESS,
            "the write is posted");
    h_check(h_deferred_at == pool + NX_PHYSICAL_HEADER,
            "and the checksum was asked for at the datagram, not the header");
    h_check((pkt.nx_packet_interface_capability_flag &
             NX_INTERFACE_CAPABILITY_TCP_TX_CHECKSUM) == 0,
            "and nothing is left deferred for the copy hook to redo");
    h_check(pkt.nx_packet_prepend_ptr ==
            pool + NX_PHYSICAL_HEADER - AMI_ETH_HEADER_SIZE,
            "and the header is on the front of the packet afterwards");
}

/* IPv4 and ARP are below 0x8000, come out of every driver right, and stay on
   the path they have always taken. */
static void test_low_ethertype_stays_cooked(void)
{
    printf("sana2: an EtherType under 0x8000 is still posted cooked\n");

    fixture_init(FALSE, S2WireType_Ethernet);
    packet_init(arp_frame, ARP_LEN);

    h_check(ami_sana2_tx_send(&iface, &pkt, AMI_ETHERTYPE_ARP, 0xFFFF,
                              0xFFFFFFFF) == NX_SUCCESS,
            "the write is posted");
    h_check((sent_req()->ios2_Req.io_Flags & SANA2IOF_RAW) == 0,
            "and it does not carry SANA2IOF_RAW");
    h_check(sent_req()->ios2_DataLength == 46,
            "and its length is the payload's, no header of ours");
}

static void test_raw_refused_falls_back_to_cooked(void)
{
    printf("sana2: a refused raw write puts the interface back on cooked\n");

    fixture_init(FALSE, S2WireType_Ethernet);
    packet_init(arp_frame, ARP_LEN);

    h_check(ami_sana2_tx_send(&iface, &pkt, AMI_ETHERTYPE_IPV6, 0x3333,
                              0x00000002) == NX_SUCCESS,
            "the first write is posted");
    h_check((sent_req()->ios2_Req.io_Flags & SANA2IOF_RAW) != 0,
            "and it asks for raw framing");

    sent_req()->ios2_Req.io_Error = (BYTE)S2ERR_BAD_ARGUMENT;
    h_reply();
    ami_sana2_tx_reap(&iface);
    h_check(iface.raw_tx_refused == TRUE, "the refusal is latched");
    h_check(iface.stats.tx_errors == 1, "and the frame counts as an error");
    h_check(pkt.nx_packet_length == ARP_LEN,
            "and the packet is handed back at its own length even so");

    packet_init(arp_frame, ARP_LEN);
    h_check(ami_sana2_tx_send(&iface, &pkt, AMI_ETHERTYPE_IPV6, 0x3333,
                              0x00000002) == NX_SUCCESS,
            "the next write of that type is posted");
    h_check((sent_req()->ios2_Req.io_Flags & SANA2IOF_RAW) == 0,
            "and the device is not asked for raw framing twice");
    h_check(sent_req()->ios2_DataLength == 46,
            "and it is framed as the payload again");
}

/* A frame already at or over the minimum is not touched. */
static void test_no_pad_when_long_enough(void)
{
    static UCHAR body[46];
    ULONG        i;

    printf("sana2: a frame at the minimum is left alone\n");

    for (i = 0; i < sizeof(body); i++)
        body[i] = (UCHAR)(i + 1);

    fixture_init(FALSE, S2WireType_Ethernet);
    packet_init(body, (ULONG)sizeof(body));

    h_check(ami_sana2_tx_send(&iface, &pkt, AMI_ETHERTYPE_IPV4, 0, 0) ==
            NX_SUCCESS, "the write is posted");
    h_check(sent_req()->ios2_DataLength == 46, "and its length is unchanged");
    h_check(pkt.nx_packet_append_ptr ==
            pool + NX_PHYSICAL_HEADER + sizeof(body),
            "and the packet was not grown");

    h_reply();
    ami_sana2_tx_reap(&iface);
    h_check(pkt.nx_packet_length == sizeof(body), "and it is handed back as it came");
}

static void test_no_pad_off_ethernet(void)
{
    printf("sana2: a wire that is not Ethernet is not padded\n");

    fixture_init(FALSE, S2WireType_PPP);
    iface.addr_bytes = 0;
    packet_init(arp_frame, ARP_LEN);

    h_check(ami_sana2_tx_send(&iface, &pkt, AMI_ETHERTYPE_IPV4, 0, 0) ==
            NX_SUCCESS, "the write is posted");
    h_check(sent_req()->ios2_DataLength == ARP_LEN,
            "and its length is the frame's, unpadded");
}

/* A reply arriving during the final permitted sleep is still inside the
   drain deadline.  It must be reaped before the interface is declared unsafe
   to free. */
static void test_drain_reaps_final_sleep(void)
{
    printf("sana2: transmit drain reaps the final deadline window\n");

    fixture_init(FALSE, S2WireType_Ethernet);
    packet_init(arp_frame, ARP_LEN);

    h_check(ami_sana2_tx_send(&iface, &pkt, AMI_ETHERTYPE_ARP, 0xFFFF,
                              0xFFFFFFFF) == NX_SUCCESS,
            "the write is posted");

    h_reply_on_sleep = 64;
    ami_sana2_tx_drain(&iface);

    h_check(h_sleeps == 64, "the device replied in the final sleep");
    h_check(iface.tx_orphaned == FALSE,
            "and the returned write is not declared orphaned");
    h_check(iface.tx[0].busy == FALSE, "and its slot is reusable");
    h_check(h_releases == 1, "and its packet was released");
}

#ifdef AMINETXDUO_TX_LAZY_COLLECT
/*
 * The parking, and the safety net that undoes it.  A send over a PA_SIGNAL
 * reply port leaves that port PA_IGNORE, so the completion raises no signal
 * on the reader and the next send's own reap walk collects it.  The one-tick
 * timer hands PA_SIGNAL back once a tick has passed with no send, which is
 * what bounds a lone completion on a quiet link.
 */
static struct Task h_reader;

static void test_lazy_parks_and_unparks(void)
{
    printf("sana2: lazy collect parks the reply port while sends flow\n");

    fixture_init(FALSE, S2WireType_Ethernet);
    packet_init(arp_frame, ARP_LEN);

    h_now = 100;
    ami_sana2_tx_lazy_start(&iface);
    h_check(iface.tx_lazy_timer_up == TRUE, "the tick was created");
    h_check(h_tick_fn != NULL, "and this file can drive its expiry");

    memset(&h_reader, 0, sizeof(h_reader));
    ami_sana2_tx_reap_bind(&iface, &h_reader, 0);
    h_check(iface.tx_port.mp_Flags == PA_SIGNAL, "a bound reader signals");

    h_check(ami_sana2_tx_send(&iface, &pkt, AMI_ETHERTYPE_ARP, 0xFFFF,
                              0xFFFFFFFF) == NX_SUCCESS, "the write is posted");
    h_check(iface.tx_port.mp_Flags == PA_IGNORE,
            "and the reply port is parked, so the completion is silent");
    h_check(iface.tx_lazy_parked == TRUE,
            "parked because of a send, not because no reader is bound");

    /* Driving the tick needs the AmiSana2If * to survive the round trip
       through tx_timer_create()'s ULONG argument.  It does on m68k, where
       both are 32 bits, and it does not on an LP64 host, where the argument
       arrives truncated: the two assertions below therefore run only where
       the round trip is lossless.  Everything either side of them is checked
       on every host. */
    if ((AmiSana2If *)h_tick_arg == &iface)
    {
        /* Inside the same tick a send is still in flight: nothing is handed
           back. */
        h_tick_fn(h_tick_arg);
        h_check(iface.tx_port.mp_Flags == PA_IGNORE,
                "a tick in the same tick as the send keeps it parked");

        /* Two ticks with no send is a quiet link. */
        h_now += 2;
        h_tick_fn(h_tick_arg);
        h_check(iface.tx_port.mp_Flags == PA_SIGNAL,
                "a quiet link gets the signalling port back");
        h_check(iface.tx_lazy_parked == FALSE,
                "and the parking is forgotten");
    }
    else
    {
        printf("  (the tick's ULONG argument truncates on this host, so the"
               " expiry itself was not driven)\n");
    }

    ami_sana2_tx_lazy_stop(&iface);
    h_check(iface.tx_lazy_timer_up == FALSE, "stop takes the tick down");
    h_check(iface.tx_port.mp_Flags == PA_SIGNAL,
            "and leaves the port signalling, which is the arm without a tick");

    ami_sana2_tx_reap_unbind(&iface);
    h_check(iface.tx_port.mp_Flags == PA_IGNORE,
            "unbind means no reader, which is PA_IGNORE again");
    h_check(iface.tx_lazy_parked == FALSE,
            "and that PA_IGNORE is not mistaken for a parking");
}
#endif /* AMINETXDUO_TX_LAZY_COLLECT */

int main(void)
{
    frames_init();

    test_pad_cooked_no_fusion();
    test_pad_cooked_with_fusion();
    test_pad_raw();
    test_high_ethertype_goes_raw();
    test_high_ethertype_checksum_first();
    test_low_ethertype_stays_cooked();
    test_raw_refused_falls_back_to_cooked();
    test_no_pad_when_long_enough();
    test_no_pad_off_ethernet();
    test_drain_reaps_final_sleep();
#ifdef AMINETXDUO_TX_LAZY_COLLECT
    test_lazy_parks_and_unparks();
#endif

    printf("%lu checks, %lu failures, %s\n", h_checks, h_failures,
           (h_failures == 0) ? "PASS" : "FAIL");

    return (h_failures == 0) ? 0 : 1;
}

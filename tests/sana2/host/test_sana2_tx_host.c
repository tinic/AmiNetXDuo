/*
 * AmiNetXDuo, the SANA-II transmit path's framing, on the host.
 *
 * SPDX-License-Identifier: MIT
 */

#include "sana2_internal.h"
#include "aminetxduo/anxs2ext.h"

/* BeginIO(), which the transmit path posts with; the shim declares it and
   this file defines it. */
#include <inline/alib.h>

#include <stdio.h>
#include <string.h>
#include "aminetxduo/asm_abi.h"

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

/* A competing reaper can run after the lock-free queue test, immediately
   before the caller disables interrupts to detach the reply list. */
static VOID (*h_before_disable)(VOID);
VOID Disable(VOID)
{
    VOID (*fn)(VOID) = h_before_disable;
    h_before_disable = NULL;
    if (fn != NULL)
        fn();
}
VOID Enable(VOID)  { }
/* Counted: the GSO path's scheduling regions per block. */
static unsigned long h_forbids;
VOID Forbid(VOID)  { h_forbids++; }
/* ACKPACE stays off here (ack_tpkb 0); the hold is test_sana2_ackpace's. */
BOOL ami_sana2_ack_hold(AmiSana2If *iface, NX_PACKET *packet,
                        ULONG dst_msw, ULONG dst_lsw)
{
    (VOID)iface; (VOID)packet; (VOID)dst_msw; (VOID)dst_lsw;
    return FALSE;
}
VOID Permit(VOID)  { }

/* The real one is exec's `io_Flags = 0; BeginIO()`, and the zero is why the
   transmit path does not use it. Nothing under test calls this. */
VOID SendIO(struct IORequest *req)
{
    req->io_Flags = 0;
    BeginIO(req);
}

/* What the device does with IOF_QUICK: 0 = a driver that queues (clears the
   flag, replies later, the shipped third-party shape), 1 = anxnet's direct
   path (finishes inside BeginIO, keeps the flag, posts nothing). */
static int h_quick_kept;

/* ANXD_CMD_TX_FLUSH as the device answers it: counted apart from the writes,
   quick unless h_flush_queued, refused with h_flush_error. */
static unsigned long     h_flushes;
static int               h_flush_queued;   /* 1 = queued and replied at
                                              once, 2 = queued, reply held
                                              until h_flush_reply() */
static BYTE              h_flush_error;
static struct IORequest *h_flush_held;

VOID ReplyMsg(struct Message *msg);

static void h_flush_reply(void)
{
    if (h_flush_held != NULL)
    {
        ReplyMsg(&h_flush_held->io_Message);
        h_flush_held = NULL;
    }
}

/* For the GSO tests: every write posted, its slot's segment, and the frame a
   device that copies at BeginIO() time would have taken. */
#define H_LOG       64
#define H_FRAME     256
static struct IORequest *h_sent_log[H_LOG];
static UBYTE             h_seg_log[H_LOG];
static ULONG             h_len_log[H_LOG];
static UCHAR             h_frames[H_LOG][H_FRAME];
static BOOL              h_frame_ok[H_LOG];
static int               h_copy_on_begin;

VOID BeginIO(struct IORequest *req)
{
    if (req->io_Command == ANXD_CMD_TX_FLUSH)
    {
        h_flushes++;
        req->io_Error = h_flush_error;
        if (h_flush_queued)
        {
            req->io_Flags &= (UBYTE)~IOF_QUICK;
            if (h_flush_queued == 2)
                h_flush_held = req;
            else
                ReplyMsg(&req->io_Message);
        }
        return;
    }
    h_sent = req;
    h_sends++;
    if (h_sends <= H_LOG)
    {
        struct IOSana2Req *s2   = (struct IOSana2Req *)req;
        AmiTxSlot         *slot = (AmiTxSlot *)s2->ios2_Data;

        h_sent_log[h_sends - 1] = req;
        h_seg_log[h_sends - 1]  = slot->seg_index;
        h_len_log[h_sends - 1]  = s2->ios2_DataLength;
        if (h_copy_on_begin)
        {
            memset(h_frames[h_sends - 1], 0x5A, H_FRAME);
            h_frame_ok[h_sends - 1] =
                ami_sana2_copy_from_buff(h_frames[h_sends - 1], slot,
                                         s2->ios2_DataLength);
        }
    }
    if (!h_quick_kept)
        req->io_Flags &= (UBYTE)~IOF_QUICK;
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
static ULONG h_flush_reply_on_sleep;

AMIGA_ASM_ARGS VOID n68k_copy_bytes(UCHAR *to, const UCHAR *from, ULONG len)
{
    if (len != 0)
        memcpy(to, from, (size_t)len);
}

/* Summed as the 68k loads them, big-endian, as in test_sana2_copy_netx: the
   GSO children's checksums below are compared with one computed from
   scratch, which a little-endian sum would not match. */
AMIGA_ASM_ARGS ULONG n68k_copy_sum_longwords(ULONG *to, const ULONG *from, ULONG count)
{
    ULONG acc = 0;

    while (count != 0UL)
    {
        const UCHAR *b = (const UCHAR *)from;
        ULONG w = ((ULONG)b[0] << 24) | ((ULONG)b[1] << 16) |
                  ((ULONG)b[2] << 8) | (ULONG)b[3];

        *to++ = *from++;

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
static NX_PACKET    *h_released;

/* The error-checking entry points, because nx_api.h maps the nx_ names to
   them when NX_DISABLE_ERROR_CHECKING is not set, which is the host build. */
/* The fill test's parents, counted one by one. */
#define G_FILL_MAX  (1 + (2 + AMI_SANA2_TX_PEND) / 2)
static NX_PACKET g_fill[G_FILL_MAX];
static UBYTE     g_fill_released[G_FILL_MAX];

UINT _nxe_packet_transmit_release(NX_PACKET **packet_ptr_ptr)
{
    h_released = *packet_ptr_ptr;
    if (h_released >= &g_fill[0] && h_released < &g_fill[G_FILL_MAX])
        g_fill_released[h_released - &g_fill[0]]++;
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

/* The current ThreadX thread, as the run bracket sees it: one for the sender
   under test, another for "some other thread". */
static TX_THREAD h_thread_a;
#ifdef AMINETXDUO_TX_RUN
static TX_THREAD h_thread_b;
#endif
static TX_THREAD *h_current = &h_thread_a;

TX_THREAD *_tx_thread_identify(VOID)
{
    return h_current;
}

UINT _tx_thread_sleep(ULONG ticks)
{
    (VOID)ticks;
    h_sleeps++;
    if (h_reply_on_sleep != 0 && h_sleeps == h_reply_on_sleep)
        h_reply();
    if (h_flush_reply_on_sleep != 0 && h_sleeps == h_flush_reply_on_sleep)
        h_flush_reply();
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
/* The GSO context table, as the driver allocates it at link-up. */
static AmiTxGso   h_gso_table[AMI_SANA2_TX_SLOTS + AMI_SANA2_TX_PEND];
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
    memset(h_gso_table, 0, sizeof(h_gso_table));
    iface.gso       = h_gso_table;
    iface.gso_count = (UWORD)(AMI_SANA2_TX_SLOTS + AMI_SANA2_TX_PEND);

    h_sent     = NULL;
    h_sends    = 0;
    h_releases = 0;
    h_sleeps   = 0;
    h_reply_on_sleep = 0;
    h_quick_kept = 0;
    h_flushes      = 0;
    h_flush_queued = 0;
    h_flush_error  = 0;
    h_flush_held   = NULL;
    h_flush_reply_on_sleep = 0;
    h_current      = &h_thread_a;
    h_before_disable = NULL;
    h_copy_on_begin  = 0;
    h_released       = NULL;
    h_forbids        = 0;
    memset(h_frame_ok, 0, sizeof(h_frame_ok));
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

/*
 * A device that negotiated transmit checksums gets the pseudo-header sum in
 * the field, folded and not complemented, per-write metadata through the
 * extension callback, and the plain copy; the fusion is not run.  10.0.0.1 -> 10.0.0.2,
 * TCP, twenty bytes: 0x0a01 + 0x0a02 + 6 + 20 = 0x141d.
 */
static void test_chip_checksum_flags_the_write(void)
{
    printf("sana2: a device that writes the checksum gets the pseudo-header sum and a flagged write\n");

    fixture_init(FALSE, S2WireType_Ethernet);
    iface.tx_csum_ok = ANXD_S2_TXF_TCP;
    packet_init(ack_frame, ACK_LEN);
    pkt.nx_packet_interface_capability_flag =
        NX_INTERFACE_CAPABILITY_TCP_TX_CHECKSUM;

    h_check(ami_sana2_tx_send(&iface, &pkt, AMI_ETHERTYPE_IPV4, 0xBC24,
                              0x11EF103A) == NX_SUCCESS,
            "the write is posted");
    h_check(ami_sana2_tx_flags(sent_req()->ios2_Data) == ANXD_S2_TXF_TCP,
            "and its extension metadata asks for a TCP checksum");
    h_check((sent_req()->ios2_Req.io_Flags &
             ~(IOF_QUICK | SANA2IOF_RAW)) == 0,
            "without borrowing an unassigned io_Flags bit");
    h_check((sent_req()->ios2_Req.io_Flags & SANA2IOF_RAW) == 0,
            "cooked, as the device's header offsets assume");
    h_check((pkt.nx_packet_interface_capability_flag &
             NX_INTERFACE_CAPABILITY_TCP_TX_CHECKSUM) == 0,
            "the packet's deferred checksum is answered for");
    h_check(pkt.nx_packet_prepend_ptr[36] == 0x14 &&
            pkt.nx_packet_prepend_ptr[37] == 0x1d,
            "by the pseudo-header sum in the field");
    h_check(sent_req()->ios2_DataLength == 46, "padded to 46 all the same");

    h_check(device_copy() == TRUE, "the copy hook hands over all 46");
    h_check(devbuf[36] == 0x14 && devbuf[37] == 0x1d,
            "the plain walk copies the pseudo-header sum, the fusion did not run");
    h_check(memcmp(devbuf, ack_frame, 36) == 0 &&
            memcmp(devbuf + 38, ack_frame + 38, ACK_LEN - 38) == 0,
            "and the rest of the datagram as it was");
    h_check(tail_is_zero(ACK_LEN, 46), "and the 6 after it are zero");

    h_reply();
    ami_sana2_tx_reap(&iface);
    h_check(h_releases == 1, "the reap releases the packet");
}

static void test_chip_checksum_needs_the_tag(void)
{
    printf("sana2: without the device's answer the write is not flagged and the fusion answers\n");

    fixture_init(FALSE, S2WireType_Ethernet);
    iface.tx_csum_ok = 0;
    packet_init(ack_frame, ACK_LEN);
    pkt.nx_packet_interface_capability_flag =
        NX_INTERFACE_CAPABILITY_TCP_TX_CHECKSUM;

    h_check(ami_sana2_tx_send(&iface, &pkt, AMI_ETHERTYPE_IPV4, 0xBC24,
                              0x11EF103A) == NX_SUCCESS,
            "the write is posted");
    h_check(ami_sana2_tx_flags(sent_req()->ios2_Data) == 0,
            "and has no checksum metadata");
    h_check((pkt.nx_packet_interface_capability_flag &
             NX_INTERFACE_CAPABILITY_TCP_TX_CHECKSUM) != 0,
            "the checksum is still the copy's to fill");
    h_check(pkt.nx_packet_prepend_ptr[36] == 0 &&
            pkt.nx_packet_prepend_ptr[37] == 0,
            "and the field is the zero NetX Duo left");
    h_check(device_copy() == TRUE, "the copy hook hands over all 46");
    h_check((pkt.nx_packet_interface_capability_flag &
             NX_INTERFACE_CAPABILITY_TCP_TX_CHECKSUM) == 0,
            "and the fusion answered for it");
    h_check(devbuf[36] != 0x14 || devbuf[37] != 0x1d,
            "with the whole checksum, not the pseudo-header sum");

    h_reply();
    ami_sana2_tx_reap(&iface);
}

static void test_chip_checksum_raw_takes_the_stack(void)
{
    printf("sana2: a raw write is never flagged, the stack fills its checksum first\n");

    fixture_init(TRUE, S2WireType_Ethernet);
    iface.tx_csum_ok = ANXD_S2_TXF_TCP;
    packet_init(ack_frame, ACK_LEN);
    pkt.nx_packet_interface_capability_flag =
        NX_INTERFACE_CAPABILITY_TCP_TX_CHECKSUM;

    h_check(ami_sana2_tx_send(&iface, &pkt, AMI_ETHERTYPE_IPV4, 0xBC24,
                              0x11EF103A) == NX_SUCCESS,
            "the write is posted");
    h_check((sent_req()->ios2_Req.io_Flags & SANA2IOF_RAW) != 0, "raw");
    h_check(ami_sana2_tx_flags(sent_req()->ios2_Data) == 0,
            "and has no checksum metadata");
    h_check((pkt.nx_packet_interface_capability_flag &
             NX_INTERFACE_CAPABILITY_TCP_TX_CHECKSUM) == 0,
            "the stack's walk answered for the checksum before the header went on");

    h_reply();
    ami_sana2_tx_reap(&iface);
}

/* What the pseudo-header path declines, so the fusion or the stack keeps it:
   each leaves the field zero and the packet's flag set. */
static void test_chip_checksum_declines(void)
{
    static const struct { int at; UCHAR v; const char *why; } cases[] = {
        { 9,  17,   "UDP" },
        { 0,  0x60, "IPv6" },
        { 6,  0x20, "a fragment (more fragments)" },
        { 7,  0x01, "a fragment (offset)" },
        { 3,  ACK_LEN + 6, "a total length past the packet" },
        { 0,  0x4f, "an IP header longer than the packet" },
    };
    ULONG i;

    printf("sana2: the pseudo-header sum is only for a whole IPv4 TCP segment\n");

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
    {
        packet_init(ack_frame, ACK_LEN);
        pkt.nx_packet_prepend_ptr[cases[i].at] = cases[i].v;
        pkt.nx_packet_interface_capability_flag =
            NX_INTERFACE_CAPABILITY_TCP_TX_CHECKSUM;
        h_check(ami_sana2_tx_pseudo_sum(&pkt) == FALSE, cases[i].why);
        h_check(pkt.nx_packet_prepend_ptr[36] == 0 &&
                pkt.nx_packet_prepend_ptr[37] == 0 &&
                (pkt.nx_packet_interface_capability_flag &
                 NX_INTERFACE_CAPABILITY_TCP_TX_CHECKSUM) != 0,
                "and the packet is left as it was");
    }

    /* The TCP header not whole in the first buffer: the field is elsewhere. */
    packet_init(ack_frame, ACK_LEN);
    pkt.nx_packet_append_ptr = pkt.nx_packet_prepend_ptr + 30;
    pkt.nx_packet_interface_capability_flag =
        NX_INTERFACE_CAPABILITY_TCP_TX_CHECKSUM;
    h_check(ami_sana2_tx_pseudo_sum(&pkt) == FALSE,
            "a TCP header split across buffers");

    /* And the one it takes. */
    packet_init(ack_frame, ACK_LEN);
    pkt.nx_packet_interface_capability_flag =
        NX_INTERFACE_CAPABILITY_TCP_TX_CHECKSUM;
    h_check(ami_sana2_tx_pseudo_sum(&pkt) == TRUE, "a whole IPv4 TCP segment");
    h_check(pkt.nx_packet_prepend_ptr[36] == 0x14 &&
            pkt.nx_packet_prepend_ptr[37] == 0x1d,
            "0x141d: addresses, protocol 6, twenty bytes");
    h_check((pkt.nx_packet_interface_capability_flag &
             NX_INTERFACE_CAPABILITY_TCP_TX_CHECKSUM) == 0,
            "and the flag is taken");
}

static void test_pseudo_header_carries(void)
{
    UCHAR *ip;
    packet_init(ack_frame, ACK_LEN);
    memmove(pkt.nx_packet_prepend_ptr + 1, pkt.nx_packet_prepend_ptr, ACK_LEN);
    pkt.nx_packet_prepend_ptr++;
    pkt.nx_packet_append_ptr++;
    ip = pkt.nx_packet_prepend_ptr;
    memset(ip + 12, 0xff, 8);
    h_check(ami_sana2_tx_pseudo_sum(&pkt) == TRUE &&
            ip[36] == 0 && ip[37] == 26,
            "odd-address pseudo-header loads retain address carries");
    ip[14] = 0xff;
    ip[15] = 0xe6;
    memset(ip + 16, 0, 4);
    h_check(ami_sana2_tx_pseudo_sum(&pkt) == TRUE &&
            ip[36] == 0 && ip[37] == 1,
            "a carry produced by the first fold is folded again");
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

#ifdef AMINETXDUO_TX_RUN
/*
 * A flush the device queued and has not answered when the interface goes
 * down: the request and its reply port live in the interface, so the drain
 * waits for the reply as it waits for a write's, and a reply that never
 * comes leaves the interface orphaned rather than freed under the device.
 */
static void test_drain_waits_for_a_queued_flush(void)
{
    printf("sana2: transmit drain waits for a flush the device still holds\n");

    fixture_init(FALSE, S2WireType_Ethernet);
    iface.tx_quick_ok = TRUE;
    iface.tx_more_ok  = 1;
    h_quick_kept      = 1;
    h_flush_queued    = 2;
    ami_sana2_tx_run_begin(&iface);
    packet_init(arp_frame, ARP_LEN);
    (VOID)ami_sana2_tx_send(&iface, &pkt, AMI_ETHERTYPE_ARP, 0xFFFF, 0xFFFFFFFF);
    ami_sana2_tx_run_end(&iface);
    h_check(iface.tx_flush_busy == 1 && h_flush_held != NULL,
            "the flush is queued at the device with no reply yet");
    h_check(iface.tx[0].busy == FALSE, "and no write is outstanding");

    h_flush_reply_on_sleep = 3;
    ami_sana2_tx_drain(&iface);
    h_check(h_sleeps == 3, "the drain waited for the flush's reply");
    h_check(iface.tx_flush_busy == 0, "which the reap collected");
    h_check(iface.tx_orphaned == FALSE, "so nothing is orphaned");
    h_check(h_releases == 1, "and the one write's packet was released once");

    /* The same, never answered. */
    fixture_init(FALSE, S2WireType_Ethernet);
    iface.tx_quick_ok = TRUE;
    iface.tx_more_ok  = 1;
    h_quick_kept      = 1;
    h_flush_queued    = 2;
    ami_sana2_tx_run_begin(&iface);
    packet_init(arp_frame, ARP_LEN);
    (VOID)ami_sana2_tx_send(&iface, &pkt, AMI_ETHERTYPE_ARP, 0xFFFF, 0xFFFFFFFF);
    ami_sana2_tx_run_end(&iface);
    ami_sana2_tx_drain(&iface);
    h_check(h_sleeps == 64, "a flush never answered is waited for to the deadline");
    h_check(iface.tx_flush_busy == 1 && iface.tx_orphaned == TRUE,
            "and leaves the interface orphaned, not freed under the device");
}
#endif /* AMINETXDUO_TX_RUN */

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

static VOID competing_reaper(VOID)
{
    ami_sana2_tx_reap(&iface);
}

static void test_reap_loses_queue_to_another_reaper(void)
{
    printf("sana2: a second reaper drains the port before the splice lock\n");
    fixture_init(FALSE, S2WireType_Ethernet);
    packet_init(arp_frame, ARP_LEN);
    h_check(ami_sana2_tx_send(&iface, &pkt, AMI_ETHERTYPE_ARP, 0xFFFF,
                              0xFFFFFFFF) == NX_SUCCESS, "the write is posted");
    h_reply();
    h_before_disable = competing_reaper;
    ami_sana2_tx_reap(&iface);
    h_check(h_before_disable == NULL, "the competing reaper ran");
    h_check(h_releases == 1 && iface.tx[0].busy == FALSE,
            "the two reapers complete the packet exactly once");
    h_check(iface.tx_port.mp_MsgList.lh_Head->ln_Succ == NULL,
            "the outer reaper leaves an empty port intact");
}

/* IOF_QUICK on the write: only to a device that answered our tags, and then
   a kept flag is a finished write -- slot back, packet back, nothing to reap;
   a cleared flag is a queued one and the reap still owns it. */
static void test_quick_write_completes_inline(void)
{
    printf("sana2: an ANXD device's write goes out IOF_QUICK and a kept flag "
           "is complete without a reply\n");

    fixture_init(FALSE, S2WireType_Ethernet);
    packet_init(arp_frame, ARP_LEN);
    h_check(ami_sana2_tx_send(&iface, &pkt, AMI_ETHERTYPE_ARP, 0xFFFF,
                              0xFFFFFFFF) == NX_SUCCESS,
            "a third-party device's write is posted");
    h_check((sent_req()->ios2_Req.io_Flags & IOF_QUICK) == 0,
            "without IOF_QUICK: it was never offered");
    h_check(iface.tx[0].busy == TRUE && h_releases == 0,
            "and the slot waits for the reply");
    h_reply();
    ami_sana2_tx_reap(&iface);
    h_check(iface.tx[0].busy == FALSE && h_releases == 1,
            "which the reap completes as before");

    fixture_init(FALSE, S2WireType_Ethernet);
    iface.tx_quick_ok = TRUE;
    h_quick_kept      = 1;
    packet_init(arp_frame, ARP_LEN);
    h_check(ami_sana2_tx_send(&iface, &pkt, AMI_ETHERTYPE_ARP, 0xFFFF,
                              0xFFFFFFFF) == NX_SUCCESS,
            "an ANXD device's write is posted");
    h_check((sent_req()->ios2_Req.io_Flags & IOF_QUICK) != 0,
            "with IOF_QUICK");
    h_check(iface.tx[0].busy == FALSE, "a kept flag: the slot is back");
    h_check(h_releases == 1, "and the packet is released");
    h_check(pkt.nx_packet_length == ARP_LEN,
            "at the length it arrived with");
    h_check(iface.stats.packets_sent == 1, "and it counts as sent");
    h_check(iface.tx_port.mp_MsgList.lh_TailPred ==
            (struct Node *)&iface.tx_port.mp_MsgList,
            "and nothing is on the reply port");
    ami_sana2_tx_reap(&iface);
    h_check(h_releases == 1, "so the reap has nothing to do");

    /* The same device with its ring full queues the write: flag cleared,
       reply later, the slot stays busy until the reap. */
    h_quick_kept = 0;
    packet_init(arp_frame, ARP_LEN);
    h_check(ami_sana2_tx_send(&iface, &pkt, AMI_ETHERTYPE_ARP, 0xFFFF,
                              0xFFFFFFFF) == NX_SUCCESS,
            "a queued write is posted");
    h_check((sent_req()->ios2_Req.io_Flags & IOF_QUICK) == 0,
            "and the device cleared the flag");
    h_check(iface.tx[0].busy == TRUE && h_releases == 1,
            "so the slot is still the device's");
    h_reply();
    ami_sana2_tx_reap(&iface);
    h_check(iface.tx[0].busy == FALSE && h_releases == 2,
            "until the reply is reaped");
}

#ifdef AMINETXDUO_TX_RUN
/*
 * A run (ANXD_S2F_TX_MORE): the holder's writes carry ANXD_S2_TXF_MORE, any
 * other thread's do not, the end of the run and a flush inside it send one
 * ANXD_CMD_TX_FLUSH each and only when something was held, and a device that
 * refuses or queues the command turns runs off.
 */
static void test_run_flags_and_flushes(void)
{
    printf("sana2: a run flags the holder's writes and flushes at its end\n");

    fixture_init(FALSE, S2WireType_Ethernet);
    iface.tx_quick_ok = TRUE;
    iface.tx_more_ok  = 1;
    h_quick_kept      = 1;

    /* Outside a run: a plain write. */
    packet_init(arp_frame, ARP_LEN);
    (VOID)ami_sana2_tx_send(&iface, &pkt, AMI_ETHERTYPE_ARP, 0xFFFF, 0xFFFFFFFF);
    h_check((ami_sana2_tx_flags(sent_req()->ios2_Data) & ANXD_S2_TXF_MORE) == 0,
            "outside a run a write is not one of a run");
    ami_sana2_tx_run_flush(&iface);
    ami_sana2_tx_run_end(&iface);
    h_check(h_flushes == 0, "and flush and end outside a run send nothing");

    ami_sana2_tx_run_begin(&iface);
    h_check(iface.tx_holder == &h_thread_a, "begin makes the caller the holder");
    ami_sana2_tx_run_flush(&iface);
    h_check(h_flushes == 0, "a flush with nothing held sends nothing");

    packet_init(arp_frame, ARP_LEN);
    (VOID)ami_sana2_tx_send(&iface, &pkt, AMI_ETHERTYPE_ARP, 0xFFFF, 0xFFFFFFFF);
    h_check((ami_sana2_tx_flags(sent_req()->ios2_Data) & ANXD_S2_TXF_MORE) != 0,
            "the holder's write is one of a run");
    h_check(iface.tx_held == 1, "and is noted as held");

    /* Another thread sends while the run is open. */
    h_current = &h_thread_b;
    ami_sana2_tx_run_begin(&iface);
    h_check(iface.tx_holder == &h_thread_a,
            "a second thread's begin does not take the run over");
    packet_init(arp_frame, ARP_LEN);
    (VOID)ami_sana2_tx_send(&iface, &pkt, AMI_ETHERTYPE_ARP, 0xFFFF, 0xFFFFFFFF);
    h_check((ami_sana2_tx_flags(sent_req()->ios2_Data) & ANXD_S2_TXF_MORE) == 0,
            "and its write is a plain one");
    ami_sana2_tx_run_end(&iface);
    h_check(iface.tx_holder == &h_thread_a && h_flushes == 0,
            "and its end neither closes the run nor flushes it");
    h_current = &h_thread_a;

    ami_sana2_tx_run_flush(&iface);
    h_check(h_flushes == 1, "the holder's flush sends the command");
    h_check(iface.tx_held == 0, "and clears held");
    h_check(iface.tx_flush_req.ios2_Req.io_Flags == IOF_QUICK,
            "a quick one, kept by the device");
    ami_sana2_tx_run_flush(&iface);
    h_check(h_flushes == 1, "a second flush with nothing new sends nothing");

    packet_init(arp_frame, ARP_LEN);
    (VOID)ami_sana2_tx_send(&iface, &pkt, AMI_ETHERTYPE_ARP, 0xFFFF, 0xFFFFFFFF);
    ami_sana2_tx_run_end(&iface);
    h_check(h_flushes == 2, "the end of the run flushes what came after");
    h_check(iface.tx_holder == NULL, "and hands the run back");
    h_check(iface.tx_more_ok == 1, "runs stay on");

    /* A device that refuses the command: runs go off, nothing else changes. */
    ami_sana2_tx_run_begin(&iface);
    packet_init(arp_frame, ARP_LEN);
    (VOID)ami_sana2_tx_send(&iface, &pkt, AMI_ETHERTYPE_ARP, 0xFFFF, 0xFFFFFFFF);
    h_flush_error = IOERR_NOCMD;
    ami_sana2_tx_run_end(&iface);
    h_check(h_flushes == 3 && iface.tx_more_ok == 0,
            "a refused flush turns runs off");
    h_flush_error = 0;
    ami_sana2_tx_run_begin(&iface);
    h_check(iface.tx_holder == NULL, "and the next begin is a no-op");

    /* A device that queues it: one is outstanding until the reap sees the
       reply, and runs are off for good. */
    fixture_init(FALSE, S2WireType_Ethernet);
    iface.tx_quick_ok = TRUE;
    iface.tx_more_ok  = 1;
    h_quick_kept      = 1;
    h_flush_queued    = 1;
    ami_sana2_tx_run_begin(&iface);
    packet_init(arp_frame, ARP_LEN);
    (VOID)ami_sana2_tx_send(&iface, &pkt, AMI_ETHERTYPE_ARP, 0xFFFF, 0xFFFFFFFF);
    ami_sana2_tx_run_end(&iface);
    h_check(h_flushes == 1 && iface.tx_flush_busy == 1 && iface.tx_more_ok == 0,
            "a queued flush is outstanding and turns runs off");
    ami_sana2_tx_reap(&iface);
    h_check(iface.tx_flush_busy == 0, "its reply is told from a write by the reap");
    h_check(iface.tx[0].busy == FALSE && h_releases == 1,
            "which completed nothing it should not have");
}
#endif /* AMINETXDUO_TX_RUN */

/* A full ring queues the write instead of sleeping a tick: the queue keeps
   the order, the completion that frees a slot launches the head, a full
   queue is the one drop left, and an interface going down releases what
   waits. */
#define Q_N     ((UWORD)AMI_SANA2_TX_SLOTS)
#define Q_MANY  (Q_N + AMI_SANA2_TX_PEND + 2)
static NX_PACKET q_pkt[Q_MANY];
static UCHAR     q_buf[Q_MANY][POOL_BYTES];

static NX_PACKET *q_packet(UWORD i)
{
    NX_PACKET *p = &q_pkt[i];

    memset(p, 0, sizeof(*p));
    p->nx_packet_data_start  = q_buf[i];
    p->nx_packet_data_end    = q_buf[i] + POOL_BYTES;
    p->nx_packet_prepend_ptr = q_buf[i] + NX_PHYSICAL_HEADER;
    p->nx_packet_append_ptr  = q_buf[i] + NX_PHYSICAL_HEADER + ARP_LEN;
    p->nx_packet_length      = ARP_LEN;
    memcpy(p->nx_packet_prepend_ptr, arp_frame, ARP_LEN);
    return p;
}

static UINT q_send(UWORD i)
{
    return ami_sana2_tx_send(&iface, q_packet(i), AMI_ETHERTYPE_ARP, 0xFFFF,
                             0xFFFFFFFF);
}

/* The packet the slot a write went into carries. */
static NX_PACKET *q_sent_packet(void)
{
    return ((AmiTxSlot *)sent_req()->ios2_Data)->packet;
}

static void test_full_ring_queues_in_order(void)
{
    UWORD i;
    ULONG sends;

    printf("sana2: a full write ring queues the write, in order, with no "
           "sleep\n");

    fixture_init(FALSE, S2WireType_Ethernet);
    for (i = 0; i < Q_N; i++)
        h_check(q_send(i) == NX_SUCCESS, "a write into a free slot is posted");
    h_check(h_sends == Q_N && iface.tx_pend_count == 0,
            "every slot took one and nothing waits");

    h_check(q_send(Q_N) == NX_SUCCESS,
            "the write past the ring is accepted");
    h_check(h_sends == Q_N, "and not posted: no slot is free");
    h_check(h_sleeps == 0, "and nothing slept for one");
    h_check(iface.tx_pend_count == 1 && iface.stats.tx_queued == 1,
            "it waits in the queue");
    h_check(h_releases == 0 && iface.stats.tx_errors == 0,
            "and was neither released nor counted as an error");

    h_check(q_send(Q_N + 1) == NX_SUCCESS && iface.tx_pend_count == 2,
            "a second one waits behind it");

    /* A slot the device hands back launches the head of the queue. */
    ReplyMsg(&iface.tx[3].req.ios2_Req.io_Message);
    ami_sana2_tx_reap(&iface);
    h_check(h_releases == 1, "the reap completed the finished write");
    h_check(h_sends == Q_N + 1, "and launched one that waited");
    h_check(q_sent_packet() == &q_pkt[Q_N],
            "the oldest one, into the freed slot");
    h_check(sent_req() == &iface.tx[3].req, "which is the slot handed back");
    h_check(iface.tx_pend_count == 1, "leaving the newer one waiting");

    /* A send that finds a free slot while writes wait goes behind them. */
    iface.tx[5].busy = FALSE;
    h_check(q_send(Q_N + 2) == NX_SUCCESS, "a send while writes wait");
    h_check(h_sends == Q_N + 2 && q_sent_packet() == &q_pkt[Q_N + 1],
            "launched the one that waited, not itself");
    h_check(iface.tx_pend_count == 1 &&
            iface.tx_pend[iface.tx_pend_head].packet == &q_pkt[Q_N + 2],
            "and took its own place at the back");

    ReplyMsg(&iface.tx[3].req.ios2_Req.io_Message);
    ami_sana2_tx_reap(&iface);
    h_check(iface.tx_pend_count == 0 && h_sends == Q_N + 3,
            "the next completion empties the queue");

    /* Nothing waiting: a write into a free slot is direct again. */
    ReplyMsg(&iface.tx[7].req.ios2_Req.io_Message);
    ami_sana2_tx_reap(&iface);
    sends = h_sends;
    h_check(q_send(0) == NX_SUCCESS && h_sends == sends + 1 &&
            iface.tx_pend_count == 0,
            "with the queue empty a free slot is taken at once");

    /* The queue is finite: past it the write is dropped, as a full ring was. */
    fixture_init(FALSE, S2WireType_Ethernet);
    for (i = 0; i < Q_N + AMI_SANA2_TX_PEND; i++)
        (void)q_send(i);
    h_check(iface.tx_pend_count == AMI_SANA2_TX_PEND && h_releases == 0,
            "the queue holds AMI_SANA2_TX_PEND writes behind a full ring");
    h_check(q_send(Q_N + AMI_SANA2_TX_PEND) == NX_TX_QUEUE_DEPTH,
            "one more is refused");
    h_check(h_releases == 1 && iface.stats.tx_queue_full == 1 &&
            iface.stats.tx_errors == 1,
            "released and counted, once");

    /* Down: what waits goes back to the pool. */
    h_reply_on_sleep = 0;
    ami_sana2_tx_drain(&iface);
    h_check(iface.tx_pend_count == 0,
            "the drain empties the queue");
    h_check(h_releases >= 1 + AMI_SANA2_TX_PEND,
            "and released every write that waited");
}

/* ---------------------------------------------------------------- TCP GSO */

/*
 * A GSO parent as the core builds it (aminetxduo/tcpgso.h): the record at
 * data_start, the first segment's IPv4 and TCP headers (twelve bytes of
 * timestamp option, so the TCP header is 32) and the payload of the whole
 * block, chained at the cuts given.  Every expectation below is built from
 * scratch from g_stream and the template, not from what the code under test
 * wrote.
 */
#define G_BUFS      8
#define G_MEM       512
#define G_IHL       20

static NX_PACKET g_buf[G_BUFS];
static UCHAR     g_mem[G_BUFS][G_MEM];
static UCHAR     g_stream[2048];
static UCHAR     g_tmpl[60];
static ULONG     g_thl;
static ULONG     g_payload;
static ULONG     g_mss;

/* RFC 1071 over big-endian words, an odd tail padded with zero; not folded. */
static ULONG ref_sum(const UCHAR *p, ULONG n)
{
    ULONG sum = 0, i;

    for (i = 0; i + 1 < n; i += 2)
        sum += ((ULONG)p[i] << 8) | p[i + 1];
    if (n & 1UL)
        sum += (ULONG)p[n - 1] << 8;
    return sum;
}

static ULONG ref_fold(ULONG sum)
{
    while (sum >> 16)
        sum = (sum & 0xFFFFUL) + (sum >> 16);
    return sum;
}

/* `first` payload bytes share the header buffer; then `cuts`, the rest in a
   last buffer.  Odd buffers start at an odd address. */
static NX_PACKET *gso_parent(ULONG thl, ULONG payload, ULONG mss, UBYTE flags,
                             ULONG seq, UWORD id, ULONG first,
                             const ULONG *cuts, ULONG ncuts)
{
    NX_TCP_GSO_INFO *info;
    ULONG            hlen = G_IHL + thl;
    ULONG            at, i, n;
    UCHAR           *ip  = g_tmpl;
    UCHAR           *tcp = g_tmpl + G_IHL;
    ULONG            sum;

    memset(g_buf, 0, sizeof(g_buf));
    memset(g_mem, 0xEE, sizeof(g_mem));
    for (i = 0; i < sizeof(g_stream); i++)
        g_stream[i] = (UCHAR)((i * 13 + 5) ^ (i >> 8));

    g_thl = thl;
    g_payload = payload;
    g_mss = mss;

    memset(g_tmpl, 0, sizeof(g_tmpl));
    ip[0] = 0x45;
    ip[2] = (UCHAR)((hlen + payload) >> 8);
    ip[3] = (UCHAR)(hlen + payload);
    ip[4] = (UCHAR)(id >> 8);
    ip[5] = (UCHAR)id;
    ip[6] = 0x40;                           /* DF                           */
    ip[8] = 64;
    ip[9] = 6;
    ip[12] = 10; ip[15] = 1;
    ip[16] = 10; ip[19] = 2;
    sum = ~ref_fold(ref_sum(ip, G_IHL)) & 0xFFFFUL;
    ip[10] = (UCHAR)(sum >> 8);
    ip[11] = (UCHAR)sum;

    tcp[0] = 0x30; tcp[1] = 0x39; tcp[2] = 0x00; tcp[3] = 0x50;
    tcp[4] = (UCHAR)(seq >> 24); tcp[5] = (UCHAR)(seq >> 16);
    tcp[6] = (UCHAR)(seq >> 8);  tcp[7] = (UCHAR)seq;
    tcp[8] = 0x01; tcp[9] = 0x02; tcp[10] = 0x03; tcp[11] = 0x04;
    tcp[12] = (UCHAR)((thl / 4) << 4);
    tcp[13] = flags;
    tcp[14] = 0xFF; tcp[15] = 0xF0;
    for (i = 20; i < thl; i++)
        tcp[i] = (UCHAR)(0xA0 + i);         /* options, copied per child    */

    /* The header buffer: record, headroom, headers, the first payload. */
    g_buf[0].nx_packet_data_start  = g_mem[0];
    g_buf[0].nx_packet_data_end    = g_mem[0] + G_MEM;
    g_buf[0].nx_packet_prepend_ptr = g_mem[0] + 4 + NX_PHYSICAL_HEADER;
    memcpy(g_buf[0].nx_packet_prepend_ptr, g_tmpl, hlen);
    n = (first < payload) ? first : payload;
    memcpy(g_buf[0].nx_packet_prepend_ptr + hlen, g_stream, n);
    g_buf[0].nx_packet_append_ptr = g_buf[0].nx_packet_prepend_ptr + hlen + n;
    at = n;

    info = (NX_TCP_GSO_INFO *)(VOID *)g_buf[0].nx_packet_data_start;
    info->version = NX_TCP_GSO_VERSION;
    info->flags   = 0;
    info->mss     = (USHORT)mss;
    info->offset  = 0;
    info->length  = 0;

    for (i = 1; i < G_BUFS && at < payload; i++)
    {
        n = (i - 1 < ncuts) ? cuts[i - 1] : payload - at;
        if (n > payload - at)
            n = payload - at;
        g_buf[i].nx_packet_data_start  = g_mem[i];
        g_buf[i].nx_packet_data_end    = g_mem[i] + G_MEM;
        g_buf[i].nx_packet_prepend_ptr = g_mem[i] + (i & 1);
        memcpy(g_buf[i].nx_packet_prepend_ptr, g_stream + at, n);
        g_buf[i].nx_packet_append_ptr  = g_buf[i].nx_packet_prepend_ptr + n;
        g_buf[i - 1].nx_packet_next    = &g_buf[i];
        at += n;
    }

    g_buf[0].nx_packet_length = hlen + payload;
    g_buf[0].nx_packet_interface_capability_flag =
        NX_INTERFACE_CAPABILITY_TCP_GSO |
        NX_INTERFACE_CAPABILITY_TCP_TX_CHECKSUM;

    return &g_buf[0];
}

/* The frame for payload bytes [off, off + len) with IPv4 ID base + `id_add`
   and TCP flags `flags`, as the wire must carry it.  `offload`: the field
   holds the pseudo-header sum, folded and not complemented. */
static ULONG gso_expect_at(UCHAR *out, ULONG off, ULONG len, ULONG id_add,
                           UBYTE flags, int offload)
{
    ULONG  hlen  = G_IHL + g_thl;
    UCHAR *ip    = out;
    UCHAR *tcp   = out + G_IHL;
    ULONG  id, seq, sum, pseudo;

    memcpy(out, g_tmpl, hlen);
    memcpy(out + hlen, g_stream + off, len);

    ip[2] = (UCHAR)((hlen + len) >> 8);
    ip[3] = (UCHAR)(hlen + len);
    id = (((ULONG)g_tmpl[4] << 8) | g_tmpl[5]) + id_add;
    ip[4] = (UCHAR)(id >> 8);
    ip[5] = (UCHAR)id;
    ip[10] = ip[11] = 0;
    sum = ~ref_fold(ref_sum(ip, G_IHL)) & 0xFFFFUL;
    ip[10] = (UCHAR)(sum >> 8);
    ip[11] = (UCHAR)sum;

    seq = (((ULONG)g_tmpl[G_IHL + 4] << 24) | ((ULONG)g_tmpl[G_IHL + 5] << 16) |
           ((ULONG)g_tmpl[G_IHL + 6] << 8) | g_tmpl[G_IHL + 7]) + off;
    tcp[4] = (UCHAR)(seq >> 24); tcp[5] = (UCHAR)(seq >> 16);
    tcp[6] = (UCHAR)(seq >> 8);  tcp[7] = (UCHAR)seq;
    tcp[13] = flags;
    tcp[16] = tcp[17] = 0;

    pseudo = ref_sum(ip + 12, 8) + 6UL + g_thl + len;
    if (offload)
        sum = ref_fold(pseudo);
    else
    {
        sum = ~ref_fold(pseudo + ref_sum(tcp, g_thl + len)) & 0xFFFFUL;
        if (sum == 0)
            sum = 0xFFFFUL;
    }
    tcp[16] = (UCHAR)(sum >> 8);
    tcp[17] = (UCHAR)sum;

    return hlen + len;
}

/* Child `i` of the whole block. */
static ULONG gso_expect(UCHAR *out, ULONG i, int offload)
{
    ULONG k     = (g_payload + g_mss - 1) / g_mss;
    ULONG off   = i * g_mss;
    ULONG len   = (g_payload - off < g_mss) ? g_payload - off : g_mss;
    UBYTE flags = g_tmpl[G_IHL + 13];

    if (i + 1 < k)
        flags &= (UBYTE)~0x09;              /* FIN, PSH: the last only      */
    if (i != 0)
        flags &= (UBYTE)~0x80;              /* CWR: the first only          */
    return gso_expect_at(out, off, len, i, flags, offload);
}

/* The parent, buffers and descriptors both, as it was handed over. */
static NX_PACKET g_buf_before[G_BUFS];
static UCHAR     g_mem_before[G_BUFS][G_MEM];

static void gso_snapshot(void)
{
    memcpy(g_buf_before, g_buf, sizeof(g_buf));
    memcpy(g_mem_before, g_mem, sizeof(g_mem));
}

static int gso_untouched(void)
{
    return memcmp(g_buf_before, g_buf, sizeof(g_buf)) == 0 &&
           memcmp(g_mem_before, g_mem, sizeof(g_mem)) == 0;
}

static void gso_reply(ULONG n)
{
    ReplyMsg(&h_sent_log[n]->io_Message);
}

/* Each child frame the device took at BeginIO() against the reference.  The
   lengths are the logged ones: a request is its slot, reused once done. */
static int gso_frames_match(ULONG nsent, int offload)
{
    UCHAR  want[H_FRAME];
    ULONG  n;

    for (n = 0; n < nsent; n++)
    {
        ULONG len = gso_expect(want, h_seg_log[n], offload);
        ULONG wire = (len < 46) ? 46 : len;

        if (!h_frame_ok[n] || h_len_log[n] != wire ||
            memcmp(h_frames[n], want, len) != 0)
        {
            printf("  child %lu (segment %u) differs\n", (unsigned long)n,
                   (unsigned)h_seg_log[n]);
            return 0;
        }
        for (; len < wire; len++)
            if (h_frames[n][len] != 0)
                return 0;
        if (h_frames[n][wire] != 0x5A)
            return 0;
    }
    return 1;
}

static const ULONG g_odd_cuts[] = { 61, 3, 150, 1, 77 };

static void test_gso_children_fused(void)
{
    NX_PACKET *p;
    ULONG      n;

    printf("sana2: a GSO block goes out as its segments, checksummed in the copy\n");

    fixture_init(FALSE, S2WireType_Ethernet);
    h_copy_on_begin = 1;
    /* Four segments, the last short; the sequence wraps 2^32 in the third;
       the slice boundaries fall at odd offsets and odd addresses. */
    p = gso_parent(32, 350, 100, 0x99 /* CWR ACK PSH FIN */, 0xFFFFFF60UL,
                   0xFFFE, 37, g_odd_cuts, 5);
    gso_snapshot();

    h_check(ami_sana2_tx_send(&iface, p, AMI_ETHERTYPE_IPV4, 0xBC24,
                              0x11EF103A) == NX_SUCCESS, "the block is taken");
    h_check(h_sends == 4, "as four writes");
    h_check(h_seg_log[0] == 0 && h_seg_log[1] == 1 && h_seg_log[2] == 2 &&
            h_seg_log[3] == 3, "in order");
    h_check(gso_frames_match(4, 0),
            "each one the block's segment: lengths, IDs, sums, seq, flags");
    h_check(h_frames[0][G_IHL + 13] == 0x90 && h_frames[1][G_IHL + 13] == 0x10 &&
            h_frames[3][G_IHL + 13] == 0x19,
            "CWR on the first, FIN and PUSH on the last alone");
    h_check(h_frames[2][G_IHL + 4] == 0x00 && h_frames[2][G_IHL + 7] == 0x28,
            "and the sequence number wrapped past 2^32");
    h_check(ami_sana2_tx_flags(((struct IOSana2Req *)h_sent_log[0])->ios2_Data)
            == 0, "and no device is asked for a checksum");

    for (n = 0; n < 3; n++)
        gso_reply(n);
    ami_sana2_tx_reap(&iface);
    h_check(h_releases == 0, "three done: the parent is still the block's");

    gso_reply(3);
    ami_sana2_tx_reap(&iface);
    h_check(h_releases == 1 && h_released == p,
            "the last one releases the parent, once");
    h_check(iface.stats.packets_sent == 4 && iface.stats.tx_errors == 0,
            "and four frames count as sent");
    h_check(gso_untouched(), "and the parent is byte for byte as it came");
    h_check(iface.gso[0].parent == NULL, "and its context is free");
}

static void test_gso_children_offload(void)
{
    NX_PACKET *p;

    printf("sana2: a GSO block to a device that finishes the TCP checksum\n");

    fixture_init(FALSE, S2WireType_Ethernet);
    iface.tx_csum_ok = ANXD_S2_TXF_TCP;
    h_copy_on_begin = 1;
    p = gso_parent(32, 350, 100, 0x18, 0x10000000UL, 7, 37, g_odd_cuts, 5);
    gso_snapshot();

    (VOID)ami_sana2_tx_send(&iface, p, AMI_ETHERTYPE_IPV4, 0xBC24, 0x11EF103A);
    h_check(h_sends == 4, "four writes");
    h_check(gso_frames_match(4, 1),
            "each carries the pseudo-header sum of its own length");
    h_check(ami_sana2_tx_flags(((struct IOSana2Req *)h_sent_log[2])->ios2_Data)
            == ANXD_S2_TXF_TCP, "and each write is flagged for the card");
    h_check(gso_untouched(), "and the parent is untouched");
}

/* A short last segment on Ethernet: zeroes from the copy, not the parent. */
static void test_gso_pads_the_last(void)
{
    NX_PACKET *p;

    printf("sana2: a GSO child under 60 bytes is padded by the copy\n");

    fixture_init(FALSE, S2WireType_Ethernet);
    h_copy_on_begin = 1;
    p = gso_parent(20, 203, 100, 0x18, 1000, 1, 203, NULL, 0);
    gso_snapshot();

    (VOID)ami_sana2_tx_send(&iface, p, AMI_ETHERTYPE_IPV4, 0xBC24, 0x11EF103A);
    h_check(h_sends == 3, "three writes");
    h_check(gso_frames_match(3, 0), "the last is 43 bytes and 3 of zeroes");

    /* The same frame taken in two pieces, the second carrying the pad. */
    {
        struct IOSana2Req *req = (struct IOSana2Req *)h_sent_log[2];
        UCHAR              want[H_FRAME];
        UCHAR              got[H_FRAME];
        ULONG              len = gso_expect(want, 2, 0);

        memset(got, 0x5A, sizeof(got));
        h_check(ami_sana2_copy_from_buff(got, req->ios2_Data, 30) == TRUE &&
                ami_sana2_copy_from_buff(got + 30, req->ios2_Data, 16) == TRUE,
                "copied again in two pieces");
        h_check(memcmp(got, want, len) == 0 && got[43] == 0 && got[44] == 0 &&
                got[45] == 0 && got[46] == 0x5A,
                "the pieces end in the same three zeroes");
    }
    h_check(gso_untouched(), "and nothing was appended to the parent");
}

/* A device that takes the frame in pieces, and one that starts over. */
static void test_gso_pieces_and_restart(void)
{
    static const ULONG pieces[] = { 7, 13, 1, 50, 2 };
    NX_PACKET         *p;
    struct IOSana2Req *req;
    UCHAR              want[H_FRAME];
    UCHAR              got[H_FRAME];
    ULONG              len, at, i;
    int                arm;

    printf("sana2: a GSO child copied in pieces, and restarted mid-way\n");

    for (arm = 0; arm < 2; arm++)
    {
        fixture_init(FALSE, S2WireType_Ethernet);
        iface.tx_csum_ok = arm ? ANXD_S2_TXF_TCP : 0;
        p = gso_parent(32, 350, 100, 0x99, 0xFFFFFF60UL, 9, 37, g_odd_cuts, 5);

        (VOID)ami_sana2_tx_send(&iface, p, AMI_ETHERTYPE_IPV4, 0xBC24,
                                0x11EF103A);
        req = (struct IOSana2Req *)h_sent_log[2];
        len = gso_expect(want, 2, arm);

        memset(got, 0x5A, sizeof(got));
        at = 0;
        for (i = 0; i < 5; i++)
        {
            h_check(ami_sana2_copy_from_buff(got + at, req->ios2_Data,
                                             pieces[i]) == TRUE,
                    "each piece is handed over");
            at += pieces[i];
        }
        h_check(ami_sana2_copy_from_buff(got + at, req->ios2_Data,
                                         len - at) == TRUE,
                "and the rest");
        h_check(memcmp(got, want, len) == 0 && got[len] == 0x5A,
                arm ? "pieces, offload: the frame exactly"
                    : "pieces, checksum before the bytes: the frame exactly");

        /* 70 bytes in, the device asks for the whole frame again. */
        memset(got, 0x5A, sizeof(got));
        h_check(ami_sana2_copy_from_buff(got, req->ios2_Data, 70) == TRUE,
                "a partial copy");
        memset(got, 0x5A, sizeof(got));
        h_check(ami_sana2_copy_from_buff(got, req->ios2_Data, len) == TRUE,
                "then the whole frame from the start");
        h_check(memcmp(got, want, len) == 0,
                "rewinds to this child's slice, not the parent's start");

        /* And once more after it has all gone out. */
        memset(got, 0x5A, sizeof(got));
        h_check(ami_sana2_copy_from_buff(got, req->ios2_Data, len) == TRUE &&
                memcmp(got, want, len) == 0, "a retry copies it again");
    }
}

/* More children than free slots: they wait in order, and the parent is the
   block's until the last of them is done. */
static void test_gso_queues_in_order(void)
{
    NX_PACKET *p;

    printf("sana2: GSO children over too few slots wait in order\n");

    fixture_init(FALSE, S2WireType_Ethernet);
    iface.tx_slots = 2;
    h_copy_on_begin = 1;
    p = gso_parent(32, 350, 100, 0x99, 0xFFFFFF60UL, 0xFFFE, 37, g_odd_cuts, 5);
    gso_snapshot();

    (VOID)ami_sana2_tx_send(&iface, p, AMI_ETHERTYPE_IPV4, 0xBC24, 0x11EF103A);
    h_check(h_sends == 2 && iface.tx_pend_count == 2,
            "two go out, two wait");
    h_check(iface.tx_pend[iface.tx_pend_head].gso == 1 &&
            iface.tx_pend[(iface.tx_pend_head + 1) % AMI_SANA2_TX_PEND]
                .gso == 1 && iface.tx_pend[iface.tx_pend_head].packet == p,
            "and they wait as the block's own");

    /* A middle child fails at the device. */
    ((struct IOSana2Req *)h_sent_log[1])->ios2_Req.io_Error = S2ERR_TX_FAILURE;
    gso_reply(1);
    ami_sana2_tx_reap(&iface);
    h_check(h_sends == 3 && h_seg_log[2] == 2,
            "a completion launches the next in order");
    h_check(iface.stats.tx_errors == 1, "the failed child is counted");
    h_check(h_releases == 0, "and the parent is still held");

    gso_reply(0);
    gso_reply(2);
    ami_sana2_tx_reap(&iface);
    h_check(h_sends == 4 && h_seg_log[3] == 3, "then the last");
    h_check(h_releases == 0, "still held while the last is out");

    gso_reply(3);
    ami_sana2_tx_reap(&iface);
    h_check(h_releases == 1 && h_released == p, "released once, at the end");
    h_check(iface.stats.packets_sent == 3 && iface.stats.tx_errors == 1,
            "three sent, one lost");
    h_check(gso_frames_match(4, 0), "and the frames are the segments");
    h_check(gso_untouched(), "and the parent is untouched");
}

/* Down with children waiting: the queue gives back its share, the slots
   theirs, and the parent goes once. */
static void test_gso_offline_mid_block(void)
{
    NX_PACKET *p;

    printf("sana2: an interface going down mid-block\n");

    fixture_init(FALSE, S2WireType_Ethernet);
    iface.tx_slots = 2;
    p = gso_parent(32, 350, 100, 0x18, 1, 1, 37, g_odd_cuts, 5);

    (VOID)ami_sana2_tx_send(&iface, p, AMI_ETHERTYPE_IPV4, 0xBC24, 0x11EF103A);
    iface.online = FALSE;
    gso_reply(0);
    ami_sana2_tx_reap(&iface);
    h_check(iface.tx_pend_count == 0 && h_sends == 2,
            "the waiting children are not launched");
    h_check(iface.stats.tx_errors == 2, "and count as lost");
    h_check(h_releases == 0, "while one child still holds the parent");

    gso_reply(1);
    ami_sana2_tx_reap(&iface);
    h_check(h_releases == 1 && h_released == p, "which its completion releases");

    /* The same, torn down by the drain. */
    fixture_init(FALSE, S2WireType_Ethernet);
    iface.tx_slots = 2;
    p = gso_parent(32, 350, 100, 0x18, 1, 1, 37, g_odd_cuts, 5);
    (VOID)ami_sana2_tx_send(&iface, p, AMI_ETHERTYPE_IPV4, 0xBC24, 0x11EF103A);
    gso_reply(0);
    gso_reply(1);
    iface.online = FALSE;
    ami_sana2_tx_drain(&iface);
    h_check(iface.tx_pend_count == 0 && h_releases == 1 && h_released == p,
            "the drain releases the parent once");
    h_check(iface.stats.tx_errors == 2 && iface.stats.packets_sent == 2,
            "two sent, two lost");
}

/* An interface that never advertised GSO has no table, and a block that
   reaches it anyway is dropped, not sent whole. */
static void test_gso_no_table(void)
{
    NX_PACKET *p;

    printf("sana2: a GSO block to an interface that never advertised it\n");

    fixture_init(FALSE, S2WireType_Ethernet);
    iface.gso       = NULL;
    iface.gso_count = 0;
    p = gso_parent(32, 350, 100, 0x18, 1, 1, 37, g_odd_cuts, 5);

    h_check(ami_sana2_tx_send(&iface, p, AMI_ETHERTYPE_IPV4, 0xBC24,
                              0x11EF103A) != NX_SUCCESS &&
            h_sends == 0 && h_releases == 1 && h_released == p,
            "released at once, nothing posted");
    h_check(iface.stats.tx_errors == 1 && iface.stats.tx_queue_full == 0,
            "as a refused packet, not as congestion");
}

/*
 * The table is never what refuses a block.  Two-segment blocks over every
 * write slot and every queue entry: each is accepted, each parent goes back
 * exactly once, and the one after them is refused by the full queue, with
 * contexts still free.
 */
#define G_FILL  ((2 + AMI_SANA2_TX_PEND) / 2)

static void test_gso_fills_slots_and_queue(void)
{
    NX_PACKET *p;
    ULONG      n, rounds;
    UINT       st;
    int        ok = 1;

    printf("sana2: GSO blocks filling every slot and queue entry are all taken\n");

    fixture_init(FALSE, S2WireType_Ethernet);
    iface.tx_slots  = 2;
    iface.gso_count = (UWORD)(iface.tx_slots + AMI_SANA2_TX_PEND);
    p = gso_parent(20, 200, 100, 0x18, 1, 1, 200, NULL, 0);
    memset(g_fill_released, 0, sizeof(g_fill_released));

    for (n = 0; n <= G_FILL; n++)
        g_fill[n] = *p;                     /* the same bytes, its own packet */

    for (n = 0; n < G_FILL; n++)
    {
        st = ami_sana2_tx_send(&iface, &g_fill[n], AMI_ETHERTYPE_IPV4,
                               0xBC24, 0x11EF103A);
        if (st != NX_SUCCESS)
            ok = 0;
    }
    h_check(ok, "every block is taken");
    h_check(h_sends == 2 && iface.tx_pend_count == AMI_SANA2_TX_PEND,
            "the slots and the queue are full");
    h_check(iface.stats.tx_queue_full == 0 && iface.stats.tx_errors == 0,
            "and nothing was dropped");

    st = ami_sana2_tx_send(&iface, &g_fill[G_FILL], AMI_ETHERTYPE_IPV4,
                           0xBC24, 0x11EF103A);
    h_check(st == NX_TX_QUEUE_DEPTH && iface.stats.tx_queue_full == 1 &&
            h_released == &g_fill[G_FILL],
            "one more is refused by the full queue");

    /* Complete whatever is out, again and again, until everything is. */
    for (rounds = 0; rounds < 4 * G_FILL; rounds++)
    {
        ULONG busy = 0;

        for (n = 0; n < iface.tx_slots; n++)
        {
            if (iface.tx[n].busy)
            {
                ReplyMsg(&iface.tx[n].req.ios2_Req.io_Message);
                busy++;
            }
        }
        if (busy == 0)
            break;
        ami_sana2_tx_reap(&iface);
    }

    h_check(iface.tx_pend_count == 0 && h_sends == 2 * G_FILL,
            "every child went out");
    h_check(iface.stats.packets_sent == 2 * G_FILL &&
            iface.stats.tx_errors == 2, "all sent; the refused block's two lost");
    for (n = 0; n <= G_FILL; n++)
        if (g_fill_released[n] != 1)
            ok = 0;
    h_check(ok && h_releases == G_FILL + 1,
            "and every parent was released exactly once");
    for (n = 0; n < iface.gso_count; n++)
        if (iface.gso[n].parent != NULL)
            ok = 0;
    h_check(ok, "and every context is free");
}

/* A version 1 record is its first four bytes: whatever follows them in
   the headroom is not read, and it means the whole block. */
static void test_gso_record_v1(void)
{
    NX_PACKET       *p;
    NX_TCP_GSO_INFO *info;

    printf("sana2: a version 1 GSO record keeps its meaning\n");

    fixture_init(FALSE, S2WireType_Ethernet);
    h_copy_on_begin = 1;
    p = gso_parent(32, 350, 100, 0x99, 0xFFFFFF60UL, 0xFFFE, 37, g_odd_cuts, 5);
    info = (NX_TCP_GSO_INFO *)(VOID *)p->nx_packet_data_start;
    info->version = 1;
    memset(p->nx_packet_data_start + 4, 0xEE, 4);

    h_check(ami_sana2_tx_send(&iface, p, AMI_ETHERTYPE_IPV4, 0xBC24,
                              0x11EF103A) == NX_SUCCESS && h_sends == 4,
            "the whole block, as four writes");
    h_check(gso_frames_match(4, 0), "each one its segment");
}

/*
 * REPAIR: exactly one frame for [offset, offset + length), seq + offset, the
 * header's IPv4 ID, PUSH only where it ends the payload, never FIN or CWR.
 * The template carries all four flags, so each one dropped is visible.
 */
static void test_gso_repair(void)
{
    static const struct
    {
        ULONG       off;
        ULONG       len;
        UBYTE       flags;
        const char *what;
    } c[] = {
        {   0, 100, 0x10, "at the start: no CWR, no PUSH, no FIN" },
        { 137,  61, 0x10, "in the middle, at an odd offset across buffers" },
        { 300,  50, 0x18, "at the end: PUSH, still no FIN" },
        { 349,   1, 0x18, "the last byte alone" },
    };
    NX_PACKET       *p;
    NX_TCP_GSO_INFO *info;
    UCHAR            want[H_FRAME];
    ULONG            i, len;
    int              arm;

    printf("sana2: a GSO repair record is one frame of the payload\n");

    for (arm = 0; arm < 2; arm++)
    {
        for (i = 0; i < sizeof(c) / sizeof(c[0]); i++)
        {
            fixture_init(FALSE, S2WireType_Ethernet);
            iface.tx_csum_ok = arm ? ANXD_S2_TXF_TCP : 0;
            h_copy_on_begin = 1;
            p = gso_parent(32, 350, 100, 0x99, 0xFFFFFF60UL, 0xFFFE, 37,
                           g_odd_cuts, 5);
            info = (NX_TCP_GSO_INFO *)(VOID *)p->nx_packet_data_start;
            info->flags  = NX_TCP_GSO_REPAIR;
            info->offset = (USHORT)c[i].off;
            info->length = (USHORT)c[i].len;
            gso_snapshot();

            h_check(ami_sana2_tx_send(&iface, p, AMI_ETHERTYPE_IPV4, 0xBC24,
                                      0x11EF103A) == NX_SUCCESS &&
                    h_sends == 1 && h_frame_ok[0],
                    "one write, copied");

            len = gso_expect_at(want, c[i].off, c[i].len, 0, c[i].flags, arm);
            h_check(h_len_log[0] == (len < 46 ? 46 : len) &&
                    memcmp(h_frames[0], want, len) == 0, c[i].what);
            h_check(arm == 0 ||
                    ami_sana2_tx_flags(((struct IOSana2Req *)h_sent_log[0])
                                           ->ios2_Data) == ANXD_S2_TXF_TCP,
                    "and on the offload arm the write is flagged");

            gso_reply(0);
            ami_sana2_tx_reap(&iface);
            h_check(h_releases == 1 && h_released == p &&
                    iface.stats.packets_sent == 1,
                    "its completion releases the parent, once");
            h_check(gso_untouched(), "and the parent is untouched");
        }
    }
}

/* What a version 2 record may not say. */
static void test_gso_record_refused(void)
{
    static const struct
    {
        UBYTE       version;
        UBYTE       flags;
        USHORT      off;
        USHORT      len;
        const char *what;
    } c[] = {
        { 2, 0, 1,   0,   "a whole block with an offset" },
        { 2, 0, 0,   1,   "a whole block with a length" },
        { 2, 1, 0,   0,   "a repair of nothing" },
        { 2, 1, 0,   101, "a repair longer than a segment" },
        { 2, 1, 300, 51,  "a repair past the payload" },
        { 2, 1, 350, 1,   "a repair starting at the end" },
        { 2, 2, 0,   0,   "an unknown flag" },
        { 2, 3, 0,   10,  "REPAIR with an unknown flag" },
        { 3, 0, 0,   0,   "version 3" },
        { 0, 0, 0,   0,   "version 0" },
        { 1, 1, 0,   10,  "REPAIR in a version 1 record" },
    };
    NX_PACKET       *p;
    NX_TCP_GSO_INFO *info;
    ULONG            i;

    printf("sana2: GSO records that say something else are dropped\n");

    for (i = 0; i < sizeof(c) / sizeof(c[0]); i++)
    {
        fixture_init(FALSE, S2WireType_Ethernet);
        p = gso_parent(32, 350, 100, 0x99, 1, 1, 37, g_odd_cuts, 5);
        gso_snapshot();
        info = (NX_TCP_GSO_INFO *)(VOID *)p->nx_packet_data_start;
        info->version = c[i].version;
        info->flags   = c[i].flags;
        info->offset  = c[i].off;
        info->length  = c[i].len;

        h_check(ami_sana2_tx_send(&iface, p, AMI_ETHERTYPE_IPV4, 0xBC24,
                                  0x11EF103A) != NX_SUCCESS &&
                h_sends == 0 && h_releases == 1 && h_released == p &&
                iface.stats.tx_errors == 1, c[i].what);
    }

    /* A version 2 record needs all eight bytes in front of the headers.  Six
       here: what would be its length is the first word of the IPv4 header. */
    fixture_init(FALSE, S2WireType_Ethernet);
    p = gso_parent(32, 350, 100, 0x99, 1, 1, 37, g_odd_cuts, 5);
    p->nx_packet_data_start = p->nx_packet_prepend_ptr - 6;
    info = (NX_TCP_GSO_INFO *)(VOID *)p->nx_packet_data_start;
    info->version = NX_TCP_GSO_VERSION;
    info->flags   = NX_TCP_GSO_REPAIR;
    info->mss     = 100;
    info->offset  = 0;
    h_check(ami_sana2_tx_send(&iface, p, AMI_ETHERTYPE_IPV4, 0xBC24,
                              0x11EF103A) != NX_SUCCESS &&
            h_sends == 0 && h_releases == 1,
            "a version 2 record without its eight bytes of headroom");
}

/* What is not a block this interface can cut is never put on the wire. */
static void test_gso_refused(void)
{
    NX_PACKET       *p;
    NX_TCP_GSO_INFO *info;
    int              c;

    printf("sana2: GSO packets that cannot be cut are dropped, not sent whole\n");

    for (c = 0; c < 6; c++)
    {
        fixture_init(c == 1, S2WireType_Ethernet);
        p = gso_parent(32, 350, 100, 0x18, 1, 1, 37, g_odd_cuts, 5);
        info = (NX_TCP_GSO_INFO *)(VOID *)p->nx_packet_data_start;
        if (c == 2)
            info->version = 9;
        if (c == 3)
            info->flags = 1;
        if (c == 4)
            p->nx_packet_prepend_ptr[G_IHL + 13] |= 0x02;      /* SYN */
        if (c == 5)
            info->mss = 1481;               /* 52 + 1481 over a 1500 MTU    */

        h_check(ami_sana2_tx_send(&iface, p, (c == 0) ? AMI_ETHERTYPE_IPV6
                                                       : AMI_ETHERTYPE_IPV4,
                                  0xBC24, 0x11EF103A) != NX_SUCCESS &&
                h_sends == 0 && h_releases == 1 && iface.stats.tx_errors == 1,
                c == 0 ? "IPv6" : c == 1 ? "raw mode" : c == 2 ? "version 9" :
                c == 3 ? "flags set" : c == 4 ? "SYN" : "MSS over the MTU");
    }
}

/* Sixteen children: one Forbid() for the context and all their slots, and
   none per child after that, whether the device queues or finishes inside
   BeginIO(). */
static void test_gso_one_forbid_per_block(void)
{
    static const ULONG cuts[] = { 400, 400, 400, 400 };
    NX_PACKET *p;
    ULONG      n;
    int        quick;

    printf("sana2: a 16-child GSO block takes one Forbid(), not one per child\n");

    for (quick = 0; quick < 2; quick++)
    {
        fixture_init(FALSE, S2WireType_Ethernet);
        h_quick_kept      = quick;
        iface.tx_quick_ok = (BOOL)quick;
        p = gso_parent(20, 1600, 100, 0x18, 1, 1, 37, cuts, 4);

        h_check(ami_sana2_tx_send(&iface, p, AMI_ETHERTYPE_IPV4, 0xBC24,
                                  0x11EF103A) == NX_SUCCESS && h_sends == 16,
                quick ? "quick: sixteen writes" : "queued: sixteen writes");
        if (!quick)
        {
            h_check(h_releases == 0, "queued: the parent is the block's");
            for (n = 0; n < 16; n++)
                gso_reply(n);
            ami_sana2_tx_reap(&iface);
        }
        h_check(h_releases == 1 && h_released == p &&
                iface.gso[0].parent == NULL,
                "the parent goes back once, its context free");
        h_check(h_forbids == 1, quick ? "quick: one Forbid() for the block"
                                      : "queued: one Forbid() for the block");
    }

    /* Four slots and a device that finishes inside BeginIO(): one region
       per round of four, never one per child. */
    fixture_init(FALSE, S2WireType_Ethernet);
    h_quick_kept      = 1;
    iface.tx_quick_ok = TRUE;
    iface.tx_slots    = 4;
    p = gso_parent(20, 1600, 100, 0x18, 1, 1, 37, cuts, 4);
    (VOID)ami_sana2_tx_send(&iface, p, AMI_ETHERTYPE_IPV4, 0xBC24, 0x11EF103A);
    h_check(h_sends == 16 && h_releases == 1 && iface.tx_pend_count == 0,
            "four slots: sixteen writes, the parent back once");
    h_check(h_forbids == 4, "and one Forbid() per round of four slots");
}

int main(void)
{
    frames_init();
    test_quick_write_completes_inline();
    test_reap_loses_queue_to_another_reaper();
#ifdef AMINETXDUO_TX_RUN
    test_run_flags_and_flushes();
#endif
    test_full_ring_queues_in_order();

    test_pad_cooked_no_fusion();
    test_pad_cooked_with_fusion();
    test_chip_checksum_flags_the_write();
    test_chip_checksum_needs_the_tag();
    test_chip_checksum_raw_takes_the_stack();
    test_chip_checksum_declines();
    test_pseudo_header_carries();
    test_pad_raw();
    test_high_ethertype_goes_raw();
    test_high_ethertype_checksum_first();
    test_low_ethertype_stays_cooked();
    test_raw_refused_falls_back_to_cooked();
    test_no_pad_when_long_enough();
    test_no_pad_off_ethernet();
    test_drain_reaps_final_sleep();

    test_gso_children_fused();
    test_gso_children_offload();
    test_gso_pads_the_last();
    test_gso_pieces_and_restart();
    test_gso_queues_in_order();
    test_gso_offline_mid_block();
    test_gso_no_table();
    test_gso_fills_slots_and_queue();
    test_gso_one_forbid_per_block();
    test_gso_refused();
    test_gso_record_v1();
    test_gso_repair();
    test_gso_record_refused();
#ifdef AMINETXDUO_TX_RUN
    test_drain_waits_for_a_queued_flush();
#endif
#ifdef AMINETXDUO_TX_LAZY_COLLECT
    test_lazy_parks_and_unparks();
#endif

    printf("%lu checks, %lu failures, %s\n", h_checks, h_failures,
           (h_failures == 0) ? "PASS" : "FAIL");

    return (h_failures == 0) ? 0 : 1;
}

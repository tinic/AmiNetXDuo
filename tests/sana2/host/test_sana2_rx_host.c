/*
 * AmiNetXDuo, the SANA-II receive path's delivery, on the host.
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

/* The event ring, counted rather than emptied: the S2ERR_OUTOFSERVICE branch
   this harness exercises is one of the paths that records one, and a stub that
   forgets would let the record go missing without a word. */
UWORD host_last_event_code;
ULONG host_event_count;

VOID ami_event(UWORD code, UWORD index, ULONG value)
{
    (VOID)index;
    (VOID)value;
    host_last_event_code = code;
    host_event_count++;
}

VOID Disable(VOID) { }
VOID Enable(VOID)  { }
VOID Forbid(VOID)  { }
VOID Permit(VOID)  { }

VOID SendIO(struct IORequest *req) { (VOID)req; }
VOID BeginIO(struct IORequest *req) { (VOID)req; }
LONG AbortIO(struct IORequest *req) { (VOID)req; return 0; }
BYTE WaitIO(struct IORequest *req) { (VOID)req; return 0; }
struct Message *GetMsg(struct MsgPort *port) { (VOID)port; return NULL; }
VOID ReplyMsg(struct Message *msg) { (VOID)msg; }

VOID NewList(struct List *list)
{
    list->lh_Head              = (struct Node *)&list->lh_Tail;
    list->lh_Tail              = NULL;
    list->lh_TailPred          = (struct Node *)list;
}

VOID AddTail(struct List *list, struct Node *node)
{
    node->ln_Succ              = (struct Node *)&list->lh_Tail;
    node->ln_Pred              = list->lh_TailPred;
    list->lh_TailPred->ln_Succ = node;
    list->lh_TailPred          = node;
}

struct Node *RemHead(struct List *list) { (VOID)list; return NULL; }

struct MsgPort *CreateMsgPort(VOID) { return NULL; }
VOID  DeleteMsgPort(struct MsgPort *port) { (VOID)port; }
LONG  DoIO(struct IORequest *req) { (VOID)req; return 0; }
struct Task *FindTask(STRPTR name) { (VOID)name; return NULL; }
BYTE  AllocSignal(LONG num) { (VOID)num; return -1; }
VOID  FreeSignal(LONG num) { (VOID)num; }
ULONG Wait(ULONG mask) { return mask; }
VOID  Signal(struct Task *task, ULONG mask) { (VOID)task; (VOID)mask; }
VOID  CloseDevice(struct IORequest *req) { (VOID)req; }

APTR ami_alloc_flags(ULONG size, ULONG memf) { (VOID)size; (VOID)memf; return NULL; }
APTR ami_alloc(ULONG size) { (VOID)size; return NULL; }
VOID ami_free(APTR ptr) { (VOID)ptr; }

VOID ami_log(int level, const char *fmt, ...) { (VOID)level; (VOID)fmt; }

VOID ami_random_arrival(VOID) { }

UINT tx_amiga_stack_in_use(APTR base, ULONG size)
{
    (VOID)base;
    (VOID)size;
    return 0;
}

/* No port here, so no Exec Task ever outlives its TX_THREAD: the monotonic
   count the teardown reads to decide whether freeing a reader stack is safe
   is always zero, which is the answer that lets it free. */
ULONG tx_amiga_zombie_tasks(VOID)
{
    return 0;
}

UINT _txe_thread_create(TX_THREAD *p, CHAR *n, VOID (*e)(ULONG), ULONG i,
                        VOID *s, ULONG l, UINT pr, UINT t, ULONG ts, UINT a,
                        UINT size)
{
    (VOID)p; (VOID)n; (VOID)e; (VOID)i; (VOID)s; (VOID)l;
    (VOID)pr; (VOID)t; (VOID)ts; (VOID)a; (VOID)size;
    return TX_SUCCESS;
}

UINT _txe_thread_delete(TX_THREAD *p) { (VOID)p; return TX_SUCCESS; }
UINT _txe_thread_terminate(TX_THREAD *p) { (VOID)p; return TX_SUCCESS; }
UINT _tx_thread_sleep(ULONG t) { (VOID)t; return TX_SUCCESS; }

UINT _txe_semaphore_create(TX_SEMAPHORE *s, CHAR *n, ULONG c, UINT size)
{
    (VOID)s; (VOID)n; (VOID)c; (VOID)size;
    return TX_SUCCESS;
}
UINT _txe_semaphore_delete(TX_SEMAPHORE *s) { (VOID)s; return TX_SUCCESS; }
UINT _txe_semaphore_get(TX_SEMAPHORE *s, ULONG w) { (VOID)s; (VOID)w; return TX_SUCCESS; }
UINT _txe_semaphore_put(TX_SEMAPHORE *s) { (VOID)s; return TX_SUCCESS; }

typedef enum { TO_NOWHERE, TO_IP, TO_ARP, TO_RARP, TO_RELEASED } Destination;

static Destination h_went;
static ULONG       h_seen_length;
static UCHAR       h_seen_first;
static NX_INTERFACE *h_seen_interface;

static void h_record(NX_PACKET *packet, Destination where)
{
    h_went           = where;
    h_seen_length    = packet->nx_packet_length;
    h_seen_first     = packet->nx_packet_prepend_ptr[0];
    h_seen_interface = packet->nx_packet_address.nx_packet_interface_ptr;
}

/*
 * The reader calls the direct entry points now, under nx_ip_protection, and
 * claims the IP thread's seat while it does.  Both halves are checked: a
 * delivery that skipped the mutex would still record the right destination, so
 * h_ip_locked records the lock and h_ip_seated records the seat.
 */
static int h_ip_locked;
static int h_ip_seated;
static int h_lock_depth;

TX_THREAD *_nx_ip_input_thread;
TX_THREAD *_tx_thread_current_ptr;
static TX_THREAD h_reader_thread;

TX_THREAD *_tx_thread_identify(VOID)
{
    return _tx_thread_current_ptr;
}

/* Both spellings: tx_mutex_get is _txe_mutex_get where ThreadX error checking
   is on, which it is in this host build, and _tx_mutex_get where it is off,
   which is the shipping m68k configuration. */
UINT _tx_mutex_get(TX_MUTEX *mutex_ptr, ULONG wait_option)
{
    (VOID)mutex_ptr;
    (VOID)wait_option;
    h_lock_depth++;
    return TX_SUCCESS;
}

UINT _tx_mutex_put(TX_MUTEX *mutex_ptr)
{
    (VOID)mutex_ptr;
    h_lock_depth--;
    return TX_SUCCESS;
}

UINT _txe_mutex_get(TX_MUTEX *mutex_ptr, ULONG wait_option)
{
    return _tx_mutex_get(mutex_ptr, wait_option);
}

UINT _txe_mutex_put(TX_MUTEX *mutex_ptr)
{
    return _tx_mutex_put(mutex_ptr);
}

static VOID h_saw_input(NX_PACKET *packet_ptr, Destination where)
{
    h_ip_locked = (h_lock_depth > 0);
    h_ip_seated = (_nx_ip_input_thread == &h_reader_thread);
    h_record(packet_ptr, where);
}

VOID _nx_ip_packet_receive(NX_IP *ip_ptr, NX_PACKET *packet_ptr)
{
    (VOID)ip_ptr;
    h_saw_input(packet_ptr, TO_IP);
}

VOID _nx_arp_packet_receive(NX_IP *ip_ptr, NX_PACKET *packet_ptr)
{
    (VOID)ip_ptr;
    h_saw_input(packet_ptr, TO_ARP);
}

VOID _nx_rarp_packet_receive(NX_IP *ip_ptr, NX_PACKET *packet_ptr)
{
    (VOID)ip_ptr;
    h_saw_input(packet_ptr, TO_RARP);
}

static int h_releases;

UINT _nxe_packet_release(NX_PACKET **packet_ptr_ptr)
{
    h_releases++;
    h_went = TO_RELEASED;
    (VOID)packet_ptr_ptr;
    return NX_SUCCESS;
}

UINT _nxe_packet_allocate(NX_PACKET_POOL *pool, NX_PACKET **packet,
                          ULONG packet_type, ULONG wait_option)
{
    (VOID)pool; (VOID)packet_type; (VOID)wait_option;
    *packet = NULL;
    return NX_NO_PACKET;
}

VOID ami_sana2_copy_bytes(UCHAR *to, const UCHAR *from, ULONG len)
{
    if (len != 0)
        memcpy(to, from, (size_t)len);
}

static ULONG h_verify_caps;
static UINT  h_verify_drop;
static int   h_verify_walks;
static int   h_verify_sums;

ULONG n68k_rx_verify(NX_PACKET *packet, UINT *drop)
{
    (VOID)packet;
    h_verify_walks++;
    *drop = h_verify_drop;
    return h_verify_caps;
}

ULONG n68k_rx_verify_sum(NX_PACKET *packet, ULONG sum, ULONG length, UINT *drop)
{
    (VOID)packet; (VOID)sum; (VOID)length;
    h_verify_sums++;
    *drop = h_verify_drop;
    return h_verify_caps;
}

VOID ami_sana2_tx_defer(AmiSana2If *iface) { (VOID)iface; }
/* The reader reaps finished writes itself rather than waking the IP
   thread to do it (sana2_rx.c, loop top). */
VOID ami_sana2_tx_reap(AmiSana2If *iface) { (VOID)iface; }
VOID ami_sana2_tx_reap_bind(AmiSana2If *iface, struct Task *task, BYTE sigbit)
{
    (VOID)iface; (VOID)task; (VOID)sigbit;
}
VOID ami_sana2_tx_reap_unbind(AmiSana2If *iface) { (VOID)iface; }
#ifdef AMINETXDUO_TX_LAZY_COLLECT
/* ami_sana2_rx_start()/stop() arm and disarm the lazy-collect tick, and the
   whole file is compiled here.  The tick lives in sana2_tx.c, which this
   target does not link, so it is stubbed like the reap binding above. */
VOID ami_sana2_tx_lazy_start(AmiSana2If *iface) { (VOID)iface; }
VOID ami_sana2_tx_lazy_stop(AmiSana2If *iface) { (VOID)iface; }
#endif
LONG ami_sana2_offline(AmiSana2If *iface) { (VOID)iface; return 0; }

/* The device-derived counters a status query asks the reader for
   (sana2_device.c): no device here, only a link. */
VOID ami_sana2_refresh_stats(AmiSana2If *iface) { (VOID)iface; }
UWORD ami_sana2_bound_count(VOID) { return 1; }
VOID ami_sana2_block_enter(VOID) { }
VOID ami_sana2_block_leave(VOID) { }
LONG ami_sana2_do_io(struct IORequest *req) { (VOID)req; return 0; }

static AmiSana2If   iface;
static NX_IP        ip;
static NX_INTERFACE interface_obj;

static UCHAR      buffer[256];
static NX_PACKET  pkt;
static AmiRxSlot  slot;

/*
 * The reader lifts the copy hook's accumulator out of the slot before it
 * re-arms it, so the delivery takes an AmiRxSum. The tests still set up a slot
 * -- that is where the copy hook writes -- and hand over what the reader would
 * have carried.
 */
static AmiRxSum h_sum_of(const AmiRxSlot *s)
{
    AmiRxSum sum;

    sum.copied = s->copied;
#ifdef AMINETXDUO_RX_VERIFY
    sum.sum    = s->sum;
    sum.summed = s->summed;
    sum.flags  = s->rxflags;
#else
    sum.sum    = 0;
    sum.summed = FALSE;
    sum.flags  = 0;
#endif

    return sum;
}

/*
 * The lock and the seat belong to ami_sana2_rx_drain() now, which is Exec code
 * this host binary does not run, so the tests take them the way the reader
 * would and the receiver stubs still check that they were held.
 */
static void h_deliver(void)
{
    AmiRxSum   sum = h_sum_of(&slot);
    TX_THREAD *outer;

    tx_mutex_get(&ip.nx_ip_protection, TX_WAIT_FOREVER);
    outer = _nx_ip_input_thread;
    _nx_ip_input_thread = tx_thread_identify();

    ami_sana2_rx_deliver(&iface, &pkt, &sum);

    _nx_ip_input_thread = outer;
    tx_mutex_put(&ip.nx_ip_protection);
}

static void fixture_init(void)
{
    memset(&iface, 0, sizeof(iface));
    memset(&ip, 0, sizeof(ip));
    memset(&interface_obj, 0, sizeof(interface_obj));
    memset(&slot, 0, sizeof(slot));

    iface.ip            = &ip;
    iface.interface_ptr = &interface_obj;
    iface.addr_bytes    = AMI_ETH_ADDR_SIZE;
    iface.mtu           = 1500;

    h_went           = TO_NOWHERE;
    h_seen_length    = 0;
    h_seen_first     = 0;
    h_seen_interface = NULL;
    h_releases       = 0;
    h_verify_caps    = 0;
    h_verify_drop    = NX_FALSE;
    h_verify_walks   = 0;
    h_verify_sums    = 0;

    h_ip_locked          = 0;
    h_ip_seated          = 0;
    h_lock_depth         = 0;
    _nx_ip_input_thread  = TX_NULL;
    _tx_thread_current_ptr = &h_reader_thread;
}

static void frame_init(UWORD type, ULONG payload)
{
    UCHAR *base = buffer + AMI_SANA2_RX_PAD;
    ULONG  i;

    memset(buffer, 0, sizeof(buffer));
    memset(&pkt, 0, sizeof(pkt));

    for (i = 0; i < AMI_ETH_ADDR_SIZE; i++)
    {
        base[i]                       = 0xFF;            /* destination */
        base[AMI_ETH_ADDR_SIZE + i]   = (UCHAR)(0x10 + i); /* source    */
    }
    base[12] = (UCHAR)(type >> 8);
    base[13] = (UCHAR)type;

    for (i = 0; i < payload; i++)
        base[AMI_ETH_HEADER_SIZE + i] = (UCHAR)(0x40 + i);

    pkt.nx_packet_data_start  = buffer;
    pkt.nx_packet_data_end    = buffer + sizeof(buffer);
    pkt.nx_packet_prepend_ptr = base;
    pkt.nx_packet_append_ptr  = base + AMI_ETH_HEADER_SIZE + payload;
    pkt.nx_packet_length      = AMI_ETH_HEADER_SIZE + payload;
}

/* A frame shorter than a whole link header. */
static void runt_init(ULONG length)
{
    UCHAR *base = buffer + AMI_SANA2_RX_PAD;

    memset(buffer, 0, sizeof(buffer));
    memset(&pkt, 0, sizeof(pkt));

    pkt.nx_packet_data_start  = buffer;
    pkt.nx_packet_data_end    = buffer + sizeof(buffer);
    pkt.nx_packet_prepend_ptr = base;
    pkt.nx_packet_append_ptr  = base + length;
    pkt.nx_packet_length      = length;
}

static void test_demux(void)
{
    static const struct {
        UWORD       type;
        Destination where;
        const char *what;
    } row[] = {
        { AMI_ETHERTYPE_IPV4, TO_IP,       "IPv4 goes to the IP receiver" },
        { AMI_ETHERTYPE_IPV6, TO_IP,       "IPv6 goes to the IP receiver" },
        { AMI_ETHERTYPE_ARP,  TO_ARP,      "ARP goes to the ARP receiver" },
        { AMI_ETHERTYPE_RARP, TO_RARP,     "RARP goes to the RARP receiver" },
        { 0x8100,             TO_RELEASED, "a VLAN tag is not handled here" },
        { 0x88CC,             TO_RELEASED, "and neither is LLDP" },
    };
    ULONG i;

    printf("sana2: the EtherType picks the receiver\n");

    for (i = 0; i < sizeof(row) / sizeof(row[0]); i++)
    {
        fixture_init();
        frame_init(row[i].type, 40);

        h_deliver();

        h_check(h_went == row[i].where, row[i].what);

        /*
         * Input runs on the reader now, so it has to hold what the IP thread
         * holds and to say so while it does.  A type that reaches no receiver
         * never gets that far.
         */
        if (row[i].where != TO_RELEASED)
        {
            h_check(h_ip_locked, "the receiver ran under nx_ip_protection");
            h_check(h_ip_seated,
                    "and with the reader named as the input thread");
        }

        h_check(h_lock_depth == 0, "and the mutex was given back");
        h_check(_nx_ip_input_thread == TX_NULL, "and the seat was given up");
    }

    /* And an unknown type is counted as one, not as an error: it is a wire
       with other traffic on it, which is normal. */
    fixture_init();
    frame_init(0x8100, 40);
    h_deliver();

    h_check(iface.stats.unknown_types == 1, "an unknown type is counted");
    h_check(iface.stats.rx_errors == 0, "and is not an error");
    h_check(iface.stats.packets_received == 0, "and is not received");
    h_check(h_releases == 1, "and the packet goes back to the pool");
}

static void test_header_strip(void)
{
    printf("sana2: the link header comes off the pointer and the length\n");

    fixture_init();
    frame_init(AMI_ETHERTYPE_IPV4, 40);

    h_deliver();

    h_check(h_went == TO_IP, "the frame reached the IP thread");
    h_check(h_seen_length == 40, "with the payload's length, header removed");
    h_check(h_seen_first == 0x40, "and pointing at the payload's first byte");
    h_check(pkt.nx_packet_prepend_ptr ==
            buffer + AMI_SANA2_RX_PAD + AMI_ETH_HEADER_SIZE,
            "the prepend pointer moved by exactly the header");
    h_check(h_seen_interface == &interface_obj,
            "and the packet names the interface it arrived on");
    h_check(iface.stats.packets_received == 1, "and it is counted as received");
}

static void test_payload_alignment(void)
{
    printf("sana2: the payload lands on a longword boundary\n");

    fixture_init();
    frame_init(AMI_ETHERTYPE_IPV4, 40);

    h_check(((AMI_SANA2_RX_PAD + AMI_ETH_HEADER_SIZE) & 3) == 0,
            "the pad plus the header is a multiple of four");

    h_deliver();

    h_check((((ULONG)(pkt.nx_packet_prepend_ptr - buffer)) & 3) == 0,
            "so the payload starts aligned from the pool block");
}

static void test_runt(void)
{
    ULONG length;

    printf("sana2: a frame shorter than a link header is refused\n");

    for (length = 0; length < AMI_ETH_HEADER_SIZE; length++)
    {
        fixture_init();
        runt_init(length);

        h_deliver();

        h_check(h_went == TO_RELEASED, "a runt reaches no receiver");
        h_check(h_releases == 1, "and goes back to the pool");
        h_check(iface.stats.rx_errors == 1, "and is an error");
        h_check(iface.stats.rx_err_runt == 1, "of the runt kind");
        h_check(iface.stats.packets_received == 0, "and is not received");
    }

    /* Exactly a header and nothing else is NOT a runt: an ARP frame padded to
       the Ethernet minimum arrives with its payload, but a zero-length one is
       a legitimate shape to hand upward and let the stack refuse. */
    fixture_init();
    frame_init(AMI_ETHERTYPE_IPV4, 0);

    h_deliver();

    h_check(h_went == TO_IP, "a header with no payload is not a runt");
    h_check(h_seen_length == 0, "and arrives with nothing in it");
    h_check(iface.stats.rx_err_runt == 0, "and is not counted as one");
}

/* The device supplies a length in the request and independently tells the
   copy hook how many bytes to initialize.  The request may shorten that copy,
   but it may never extend the packet into stale pool storage. */
static void test_completion_length_consistency(void)
{
    ULONG length;

    printf("sana2: receive length never exceeds initialized bytes\n");

    memset(&slot, 0, sizeof(slot));
    slot.capacity = 100;
    slot.copied   = 40;
    length        = 60;
    h_check(ami_sana2_rx_resolve_length(&slot, &length) == FALSE,
            "a device cannot report more bytes than it copied");

    slot.copied = 0;
    length = 40;
    h_check(ami_sana2_rx_resolve_length(&slot, &length) == FALSE,
            "a length without a successful copy is refused");

    slot.copied = 40;
    length = 0;
    h_check(ami_sana2_rx_resolve_length(&slot, &length) == TRUE && length == 40,
            "a missing report falls back to the copy-hook length");

    length = 32;
#ifdef AMINETXDUO_RX_VERIFY
    slot.summed = TRUE;
#endif
    h_check(ami_sana2_rx_resolve_length(&slot, &length) == TRUE && length == 32,
            "a shorter reported frame remains valid");
#ifdef AMINETXDUO_RX_VERIFY
    h_check(slot.summed == FALSE,
            "and a checksum over the longer copy is not reused");
#endif
}

/*
 * THE LINK HEADER IS FOURTEEN BYTES OF PACKET WHOEVER WROTE IT.
 *
 * 1bbb3803 narrowed the synthesis guard to `!iface->raw_mode &&
 * !slot->hdr_written` and took `length += AMI_ETH_HEADER_SIZE` inside it with
 * it, so a device answering ANXD_S2_RX_LINK_HDR had every frame delivered
 * fourteen bytes short.  It survived a day of rig runs because the a2065 never
 * claims (lance.c:296) and only the direct path sets hdr_written.
 *
 * ami_sana2_rx_frame_length() does not take the slot, so the answer CANNOT
 * depend on who filled the header in.  These checks pin that.
 */
static void test_link_header_is_counted_either_way(void)
{
    AmiSana2If lh;

    printf("sana2: the link header counts whoever wrote it\n");

    memset(&lh, 0, sizeof(lh));

    lh.raw_mode = FALSE;
    h_check(ami_sana2_rx_frame_length(&lh, 1460UL) ==
            1460UL + AMI_ETH_HEADER_SIZE,
            "cooked: the payload gains the link header");
    h_check(ami_sana2_rx_frame_length(&lh, 0UL) == AMI_ETH_HEADER_SIZE,
            "cooked: an empty payload is still a header");

    lh.raw_mode = TRUE;
    h_check(ami_sana2_rx_frame_length(&lh, 1460UL) == 1460UL,
            "raw: the frame is what the device copied");

    /* The regression itself: the same payload through both fill paths.  There
       is no slot argument to differ on, which is the fix. */
    lh.raw_mode = FALSE;
    h_check(ami_sana2_rx_frame_length(&lh, 60UL) ==
            ami_sana2_rx_frame_length(&lh, 60UL),
            "the answer does not depend on who wrote the header");
    h_check(ami_sana2_rx_frame_length(&lh, 60UL) == 74UL,
            "and it is the payload plus fourteen");
}

#ifdef AMINETXDUO_RX_VERIFY

static void test_verify_publishes_only_what_it_checked(void)
{
    printf("sana2: what the verifier checked is what is published\n");

    fixture_init();
    frame_init(AMI_ETHERTYPE_IPV4, 40);
    h_verify_caps = NX_INTERFACE_CAPABILITY_IPV4_RX_CHECKSUM |
                    NX_INTERFACE_CAPABILITY_TCP_RX_CHECKSUM;

    h_deliver();

    h_check(h_went == TO_IP, "the frame reached the IP thread");
    h_check(pkt.nx_packet_interface_capability_flag == h_verify_caps,
            "carrying exactly what the verifier answered");

    /* A frame the verifier declines carries no claim at all. */
    fixture_init();
    frame_init(AMI_ETHERTYPE_IPV4, 40);
    h_verify_caps = 0;

    h_deliver();

    h_check(pkt.nx_packet_interface_capability_flag == 0,
            "a declined frame claims nothing and the stack walks it");

    /* IPv6 does not go through the verifier at all. */
    fixture_init();
    frame_init(AMI_ETHERTYPE_IPV6, 40);
    h_verify_caps = 0xFFFFFFFFUL;

    h_deliver();

    h_check(h_verify_walks == 0 && h_verify_sums == 0,
            "IPv6 is not verified here");
    h_check(pkt.nx_packet_interface_capability_flag == 0,
            "and claims nothing");
}

/* A frame the verifier rejects never reaches the stack. */
static void test_verify_drop(void)
{
    printf("sana2: a frame with a bad checksum is dropped here\n");

    fixture_init();
    frame_init(AMI_ETHERTYPE_IPV4, 40);
    h_verify_drop = NX_TRUE;

    h_deliver();

    h_check(h_went == TO_RELEASED, "it reaches no receiver");
    h_check(h_releases == 1, "and goes back to the pool");
    h_check(iface.stats.rx_errors == 1, "and is an error");
    h_check(iface.stats.rx_err_verify == 1, "of the verify kind");
    h_check(iface.stats.packets_received == 0, "and is not received");
}

static void test_verify_uses_the_carried_sum(void)
{
    printf("sana2: a carried sum is used, and only when it is this frame's\n");

    fixture_init();
    frame_init(AMI_ETHERTYPE_IPV4, 40);
    slot.summed = TRUE;
    slot.sum    = 0x1234;
    slot.copied = 40;

    h_deliver();

    h_check(h_verify_sums == 1 && h_verify_walks == 0,
            "a summed slot hands the sum over");

    fixture_init();
    frame_init(AMI_ETHERTYPE_IPV4, 40);
    slot.summed = FALSE;
    slot.sum    = 0x1234;       /* stale, from the previous frame */

    h_deliver();

    h_check(h_verify_walks == 1 && h_verify_sums == 0,
            "an unsummed slot makes the verifier walk instead");

    /* And no slot at all, which is the shape a device that hands the frame
       over some other way leaves behind. */
    fixture_init();
    frame_init(AMI_ETHERTYPE_IPV4, 40);

    ami_sana2_rx_deliver(&iface, &pkt, NULL);

    h_check(h_verify_walks == 1 && h_verify_sums == 0,
            "and no slot at all also walks");
}

#endif /* AMINETXDUO_RX_VERIFY */

#ifdef AMINETXDUO_GRO
/* ---- the held run: VERIFIED frames and the CONTINUES chain ------------- */

/*
 * The reader's take/flush pair on a fake reader: the packets are the two
 * static ones below, the receiver stubs above see what goes up.  A TCP frame
 * here is Ethernet, a 20-byte IPv4 header (0x45, proto 6, total length) and
 * a 20-byte TCP header in front of `payload` bytes -- the shape the device's
 * VERIFIED mark promises, and the fifty-four bytes a chained frame steps over.
 */
static UCHAR      buffer2[256];
static NX_PACKET  pkt2;
static AmiSana2Rx rxs;

static void tcp_frame_init(NX_PACKET *p, UCHAR *buf, UCHAR proto, ULONG payload)
{
    UCHAR *base = buf + AMI_SANA2_RX_PAD;
    UCHAR *ip   = base + AMI_ETH_HEADER_SIZE;
    ULONG  total = 40UL + payload;
    ULONG  i;

    memset(buf, 0, 256);
    memset(p, 0, sizeof(*p));

    base[12] = 0x08;
    base[13] = 0x00;
    ip[0]    = 0x45;
    ip[2]    = (UCHAR)(total >> 8);
    ip[3]    = (UCHAR)total;
    ip[9]    = proto;
    for (i = 0; i < payload; i++)
        ip[40 + i] = (UCHAR)(0x40 + i);

    p->nx_packet_data_start  = buf;
    p->nx_packet_data_end    = buf + 256;
    p->nx_packet_prepend_ptr = base;
    p->nx_packet_append_ptr  = ip + total;
    p->nx_packet_length      = AMI_ETH_HEADER_SIZE + total;
}

static AmiRxSum h_flagged(UBYTE flags)
{
    AmiRxSum sum;

    sum.copied = 0;
    sum.sum    = 0;
    sum.summed = FALSE;
    sum.flags  = flags;
    return sum;
}

static void gro_init(UBYTE answered)
{
    fixture_init();
    memset(&rxs, 0, sizeof(rxs));
    rxs.iface          = &iface;
    iface.rx_flags_ok  = answered;
    tx_mutex_get(&ip.nx_ip_protection, TX_WAIT_FOREVER);
    _nx_ip_input_thread = tx_thread_identify();
}

static void gro_done(void)
{
    _nx_ip_input_thread = TX_NULL;
    tx_mutex_put(&ip.nx_ip_protection);
}

static void test_verified_skips_the_walk(void)
{
    printf("sana2: a VERIFIED frame is not walked and claims by protocol\n");

    fixture_init();
    tcp_frame_init(&pkt, buffer, 6, 40);
    {
        AmiRxSum sum = h_flagged(ANXD_S2_RXF_SUMMED | ANXD_S2_RXF_VERIFIED);

        tx_mutex_get(&ip.nx_ip_protection, TX_WAIT_FOREVER);
        ami_sana2_rx_deliver(&iface, &pkt, &sum);
        tx_mutex_put(&ip.nx_ip_protection);
    }
    h_check(h_went == TO_IP, "a verified TCP frame reaches the IP thread");
    h_check(h_verify_walks == 0 && h_verify_sums == 0,
            "without the verifier running at all");
    h_check(pkt.nx_packet_interface_capability_flag ==
                (NX_INTERFACE_CAPABILITY_IPV4_RX_CHECKSUM |
                 NX_INTERFACE_CAPABILITY_TCP_RX_CHECKSUM),
            "claiming the IPv4 header and the TCP checksum");

    fixture_init();
    tcp_frame_init(&pkt, buffer, 17, 40);
    {
        AmiRxSum sum = h_flagged(ANXD_S2_RXF_SUMMED | ANXD_S2_RXF_VERIFIED);

        tx_mutex_get(&ip.nx_ip_protection, TX_WAIT_FOREVER);
        ami_sana2_rx_deliver(&iface, &pkt, &sum);
        tx_mutex_put(&ip.nx_ip_protection);
    }
    h_check(pkt.nx_packet_interface_capability_flag ==
                (NX_INTERFACE_CAPABILITY_IPV4_RX_CHECKSUM |
                 NX_INTERFACE_CAPABILITY_UDP_RX_CHECKSUM),
            "and a verified UDP frame claims the UDP checksum");

    /* SUMMED alone is the old contract: the verifier runs on the sum. */
    fixture_init();
    tcp_frame_init(&pkt, buffer, 6, 40);
    {
        AmiRxSum sum = h_flagged(ANXD_S2_RXF_SUMMED);

        sum.summed = TRUE;
        sum.copied = 80;
        tx_mutex_get(&ip.nx_ip_protection, TX_WAIT_FOREVER);
        ami_sana2_rx_deliver(&iface, &pkt, &sum);
        tx_mutex_put(&ip.nx_ip_protection);
    }
    h_check(h_verify_sums == 1, "a merely summed frame still goes through the verifier");
}

static void test_held_frame_goes_up_on_flush(void)
{
    AmiRxSum sum = h_flagged(ANXD_S2_RXF_SUMMED | ANXD_S2_RXF_VERIFIED);

    printf("sana2: a verified frame is held and goes up whole on the flush\n");

    gro_init(ANXD_S2_RXF_VERIFIED | ANXD_S2_RXF_CONTINUES);
    tcp_frame_init(&pkt, buffer, 6, 40);

    h_check(ami_sana2_gro_take(&rxs, &pkt, &sum) == TRUE, "the reader takes it");
    h_check(h_went == TO_NOWHERE, "and nothing has gone up yet");
    h_check(rxs.gro_head == &pkt && rxs.gro_count == 1, "it is the held head");

    ami_sana2_gro_flush(&rxs);

    h_check(h_went == TO_IP, "the flush delivers it");
    h_check(h_seen_length == 80, "as the frame it was, header stripped");
    h_check(pkt.nx_packet_next == NX_NULL && pkt.nx_packet_last == NX_NULL,
            "a run of one is not a chain");
    h_check(buffer[AMI_SANA2_RX_PAD + 16] == 0 &&
            buffer[AMI_SANA2_RX_PAD + 17] == 80,
            "and its IP length is untouched");
    h_check(rxs.gro_head == NX_NULL, "and nothing is held after");

    /* A verified UDP datagram is not held: nothing will continue it. */
    gro_init(ANXD_S2_RXF_VERIFIED | ANXD_S2_RXF_CONTINUES);
    tcp_frame_init(&pkt, buffer, 17, 40);
    h_check(ami_sana2_gro_take(&rxs, &pkt, &sum) == FALSE,
            "a verified UDP datagram is left to the caller");
    h_check(rxs.gro_head == NX_NULL, "and not held");

    /* A device that never answered the tag: verified frames are not held. */
    gro_init(0);
    tcp_frame_init(&pkt, buffer, 6, 40);
    h_check(ami_sana2_gro_take(&rxs, &pkt, &sum) == FALSE,
            "without CONTINUES on offer nothing is held");
    h_check(h_went == TO_NOWHERE && rxs.gro_head == NX_NULL,
            "and the caller delivers the frame itself");
    gro_done();
}

static void test_continuing_frame_is_chained(void)
{
    AmiRxSum head = h_flagged(ANXD_S2_RXF_SUMMED | ANXD_S2_RXF_VERIFIED);
    AmiRxSum next = h_flagged(ANXD_S2_RXF_SUMMED | ANXD_S2_RXF_VERIFIED |
                              ANXD_S2_RXF_CONTINUES);

    printf("sana2: a CONTINUES frame is chained behind the head, headers off\n");

    gro_init(ANXD_S2_RXF_VERIFIED | ANXD_S2_RXF_CONTINUES);
    tcp_frame_init(&pkt,  buffer,  6, 40);
    tcp_frame_init(&pkt2, buffer2, 6, 30);

    h_check(ami_sana2_gro_take(&rxs, &pkt, &head) == TRUE, "the head is held");
    h_check(ami_sana2_gro_take(&rxs, &pkt2, &next) == TRUE, "the next is taken");
    h_check(h_went == TO_NOWHERE, "nothing has gone up");
    h_check(rxs.gro_count == 2 && rxs.gro_tail == &pkt2, "the run is two long");
    h_check(pkt.nx_packet_next == &pkt2 && pkt.nx_packet_last == &pkt2,
            "chained behind the head");
    h_check(pkt2.nx_packet_prepend_ptr ==
                buffer2 + AMI_SANA2_RX_PAD + AMI_ETH_HEADER_SIZE + 40,
            "with its fifty-four header bytes stepped over");
    h_check((ULONG)(pkt2.nx_packet_append_ptr - pkt2.nx_packet_prepend_ptr) == 30,
            "leaving its thirty payload bytes");
    h_check(pkt.nx_packet_length == 14 + 80 + 30,
            "and the head's length is the run's");

    ami_sana2_gro_flush(&rxs);

    h_check(h_went == TO_IP, "the flush delivers the run");
    h_check(h_seen_length == 80 + 30, "as one datagram of both payloads");
    h_check(buffer[AMI_SANA2_RX_PAD + 16] == 0 &&
            buffer[AMI_SANA2_RX_PAD + 17] == 110,
            "with the head's IP length rewritten to the run");
    h_check(pkt.nx_packet_interface_capability_flag ==
                (NX_INTERFACE_CAPABILITY_IPV4_RX_CHECKSUM |
                 NX_INTERFACE_CAPABILITY_TCP_RX_CHECKSUM),
            "and both checksum claims, the header's now being stale");
    h_check(h_verify_walks == 0 && h_verify_sums == 0,
            "with the verifier never run");
    gro_done();
}

static void test_run_ends_on_a_frame_that_does_not_continue(void)
{
    AmiRxSum head  = h_flagged(ANXD_S2_RXF_SUMMED | ANXD_S2_RXF_VERIFIED);
    AmiRxSum plain = h_flagged(ANXD_S2_RXF_SUMMED);
    AmiRxSum cont  = h_flagged(ANXD_S2_RXF_SUMMED | ANXD_S2_RXF_VERIFIED |
                               ANXD_S2_RXF_CONTINUES);

    printf("sana2: a frame that does not continue the run flushes it first\n");

    gro_init(ANXD_S2_RXF_VERIFIED | ANXD_S2_RXF_CONTINUES);
    tcp_frame_init(&pkt,  buffer,  6, 40);
    tcp_frame_init(&pkt2, buffer2, 6, 30);

    (VOID)ami_sana2_gro_take(&rxs, &pkt, &head);
    h_check(ami_sana2_gro_take(&rxs, &pkt2, &plain) == FALSE,
            "an unverified frame is not taken");
    h_check(h_went == TO_IP && h_seen_length == 80,
            "but the held head went up ahead of it, alone");
    h_check(rxs.gro_head == NX_NULL, "and nothing is held");

    /* CONTINUES with nothing held: the start of a run, not a chain. */
    h_went = TO_NOWHERE;
    tcp_frame_init(&pkt2, buffer2, 6, 30);
    h_check(ami_sana2_gro_take(&rxs, &pkt2, &cont) == TRUE,
            "a CONTINUES frame with no head is held as one");
    h_check(rxs.gro_head == &pkt2 && rxs.gro_count == 1 &&
            pkt2.nx_packet_prepend_ptr == buffer2 + AMI_SANA2_RX_PAD,
            "whole, headers on");
    ami_sana2_gro_flush(&rxs);
    h_check(h_went == TO_IP && h_seen_length == 70, "and goes up as itself");

    /* A stop drops what is held instead of delivering under the stopper. */
    h_went = TO_NOWHERE;
    tcp_frame_init(&pkt, buffer, 6, 40);
    (VOID)ami_sana2_gro_take(&rxs, &pkt, &head);
    rxs.stop = TRUE;
    ami_sana2_gro_flush(&rxs);
    h_check(h_went == TO_RELEASED && h_releases == 1,
            "a flush under stop releases the run");
    rxs.stop = FALSE;
    gro_done();
}

static void test_run_is_capped(void)
{
    AmiRxSum head = h_flagged(ANXD_S2_RXF_SUMMED | ANXD_S2_RXF_VERIFIED);
    AmiRxSum cont = h_flagged(ANXD_S2_RXF_SUMMED | ANXD_S2_RXF_VERIFIED |
                              ANXD_S2_RXF_CONTINUES);
    static NX_PACKET  many[AMI_SANA2_GRO_MAX];
    static UCHAR      bufs[AMI_SANA2_GRO_MAX][256];
    UWORD i;

    printf("sana2: a run of AMI_SANA2_GRO_MAX frames goes up without a flush\n");

    gro_init(ANXD_S2_RXF_VERIFIED | ANXD_S2_RXF_CONTINUES);
    tcp_frame_init(&pkt, buffer, 6, 10);
    (VOID)ami_sana2_gro_take(&rxs, &pkt, &head);

    for (i = 1; i < AMI_SANA2_GRO_MAX; i++)
    {
        tcp_frame_init(&many[i], bufs[i], 6, 10);
        h_check(ami_sana2_gro_take(&rxs, &many[i], &cont) == TRUE,
                "each continuing frame is taken");
        if (i + 1 < AMI_SANA2_GRO_MAX)
            h_check(h_went == TO_NOWHERE, "and held");
    }

    h_check(h_went == TO_IP, "the frame that fills the run delivers it");
    h_check(h_seen_length == 50UL + 10UL * (AMI_SANA2_GRO_MAX - 1),
            "as the whole run");
    h_check(rxs.gro_head == NX_NULL, "and nothing is held after");
    h_check(pkt.nx_packet_last == &many[AMI_SANA2_GRO_MAX - 1],
            "with the last frame as the chain's last");
    gro_done();
}

#endif /* AMINETXDUO_GRO */

/* Enough packets that the budget never binds: the ladder alone decides. */
#define PLAN_BIG_POOL   512UL

static void plan_for(ULONG bps, ULONG pool, BOOL dual, UWORD ifaces,
                     AmiRxDepths *d)
{
    /* Poisoned first, so a plan that writes nothing fails rather than reading
       as "the floors". */
    d->ipv4 = 0xEEEE;
    d->arp  = 0xEEEE;
    d->ipv6 = 0xEEEE;
    ami_sana2_rx_plan(bps, pool, dual, ifaces, 0, 0, d);
}

/* The same, with the interface file's IPREQUESTS and ARPREQUESTS. */
static void plan_asked(ULONG bps, ULONG pool, UWORD ask_ip, UWORD ask_arp,
                       AmiRxDepths *d)
{
    d->ipv4 = 0xEEEE;
    d->arp  = 0xEEEE;
    d->ipv6 = 0xEEEE;
    ami_sana2_rx_plan(bps, pool, TRUE, 1, ask_ip, ask_arp, d);
}

/* One interface, which is what every case below this line means and what the
   measured table was taken on. */
static void plan_at(ULONG bps, ULONG pool, BOOL dual, AmiRxDepths *d)
{
    plan_for(bps, pool, dual, 1, d);
}

static void test_plan_ladder(void)
{
    static const struct { ULONG bps; UWORD want; const char *what; } cases[] =
    {
        {           1UL,  4, "a device answering 1 bit/s"                  },
        {      115200UL,  4, "a serial line"                               },
        {     4000000UL,  4, "the fastest wire the reader can outrun"      },
        {     4000001UL, 32, "one bit past it"                             },
        {    10000000UL, 32, "ten-megabit Ethernet"                        },
        {   100000000UL, 32, "a hundred-megabit card"                      },
        /* The ring's row, capped here by PLAN_BIG_POOL's share (512 / 8):
           the pool decides on a small machine, the wire on a big one. */
        {  1000000000UL, 64, "a gigabit wire goes past the LAN's 32"        },
        {  0xFFFFFFFFUL, 64, "a rate that did not fit a ULONG"             }
    };
    AmiRxDepths d;
    unsigned    i;

    printf("sana2: a wire slower than the reader caps the read depth\n");

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
    {
        plan_at(cases[i].bps, PLAN_BIG_POOL, TRUE, &d);
        h_check(d.ipv4 == cases[i].want, cases[i].what);
    }

    /* Monotone, which is the property the table is a sample of: no faster wire
       may earn a shallower queue than a slower one. */
    {
        ULONG bps;
        UWORD prev = 0;

        for (bps = 1UL; bps <= 400000000UL; bps += 1000000UL)
        {
            plan_at(bps, PLAN_BIG_POOL, TRUE, &d);
            h_check(d.ipv4 >= prev, "a faster wire is never shallower");
            prev = d.ipv4;
        }
    }
}

static void test_plan_degenerate_bps(void)
{
    AmiRxDepths zero;
    AmiRxDepths ten;
    AmiRxDepths slow;

    printf("sana2: a device that will not say what wire it is lands at ten "
           "megabits\n");

    plan_at(0UL, PLAN_BIG_POOL, TRUE, &zero);
    plan_at(10000000UL, PLAN_BIG_POOL, TRUE, &ten);

    h_check(zero.ipv4 == ten.ipv4, "0 bit/s is planned as ten megabits");
    h_check(zero.arp  == ten.arp,  "for the ARP reader too");
    h_check(zero.ipv6 == ten.ipv6, "and for the IPv6 reader");

    /* And not the other thing 0 could plausibly have meant: the slowest wire
       the table knows about, which would cap every silent device at the
       floor and make a card that merely forgot to fill BPS in unusable. */
    plan_at(1UL, PLAN_BIG_POOL, TRUE, &slow);
    h_check(zero.ipv4 > slow.ipv4,
            "and not the slowest wire, which a silent Ethernet board is not");
}

static void test_plan_budget(void)
{
    AmiRxDepths d;
    ULONG       pool;

    printf("sana2: the readers together pin at most a quarter of the pool\n");

    /* No pool at all -- the shape a caller with nothing allocated would pass.
       The floors, and nothing above them. */
    plan_at(100000000UL, 0UL, TRUE, &d);
    h_check(d.ipv4 == AMI_SANA2_RX_DEPTH_IPV4 &&
            d.arp  == AMI_SANA2_RX_DEPTH_ARP  &&
            d.ipv6 == AMI_SANA2_RX_DEPTH_IPV6,
            "an empty pool buys nothing above the floors");

    plan_at(100000000UL, 17UL, TRUE, &d);
    h_check(d.ipv4 == AMI_SANA2_RX_DEPTH_IPV4,
            "seventeen packets and a fast card still gets the floor");
    h_check((ULONG)d.ipv4 + d.arp + d.ipv6 < 17UL,
            "and the readers do not take the pool");

    plan_at(10000000UL, 47UL, TRUE, &d);
    h_check(d.ipv4 == 5, "47 packets: IPv4 gets the pool's own number, five");
    h_check(d.arp == 2 && d.ipv6 == 4, "and the plan is 5/2/4 exactly");
    h_check((ULONG)d.ipv4 + d.arp + d.ipv6 <= 47UL / AMI_SANA2_RX_BUDGET_SHARE,
            "and the three of them stay inside a quarter of that pool");

    plan_at(10000000UL, 127UL, TRUE, &d);
    h_check(d.ipv4 == 15 && d.arp == 2 && d.ipv6 == 8,
            "127 packets: 15/2/8, which is what the guest printed");
    plan_at(10000000UL, 207UL, TRUE, &d);
    h_check(d.ipv4 == 25 && d.arp == 2 && d.ipv6 == 8,
            "207 packets: 25/2/8, which is what the guest printed");
    plan_at(100000000UL, 513UL, TRUE, &d);
    h_check(d.ipv4 == 32 && d.arp == 2 && d.ipv6 == 8,
            "513 packets: 32/2/8, which is what the A3000 printed");

    plan_at(10000000UL, 368UL, TRUE, &d);
    h_check(d.ipv4 == AMI_SANA2_RX_DEPTH_LAN,
            "368 packets: IPv4 gets the LAN ceiling");
    h_check(d.ipv6 == AMI_SANA2_RX_WANT_IPV6,
            "and IPv6 gets its own cap rather than two");
    plan_at(100000000UL, 368UL, TRUE, &d);
    h_check(d.ipv4 == AMI_SANA2_RX_DEPTH_LAN,
            "and a hundred-megabit card on that machine asks for no more");

    /* THE GIGABIT ROW IS THE RING.  The A1200 + PiStorm32's 4096-packet pool
       on anxgenet.device: both stream readers get the driver's 128, the
       whole plan inside a quarter of the pool; the same wire on the lab's
       368 packets is capped by the pool share and still gives IPv6 what it
       gives IPv4, because a run of segments meets the same ring either way. */
    plan_at(1000000000UL, 4096UL, TRUE, &d);
    h_check(d.ipv4 == AMI_SANA2_RX_MAX_DEPTH,
            "4096 packets on a gigabit wire: IPv4 gets the ring");
    h_check(d.ipv6 == AMI_SANA2_RX_MAX_DEPTH,
            "and so does IPv6");
    h_check((ULONG)d.ipv4 + d.arp + d.ipv6 <= 4096UL / AMI_SANA2_RX_BUDGET_SHARE,
            "inside a quarter of the pool");
    plan_at(1000000000UL, 368UL, TRUE, &d);
    h_check(d.ipv4 < AMI_SANA2_RX_MAX_DEPTH && d.ipv4 > AMI_SANA2_RX_DEPTH_LAN,
            "368 packets on a gigabit wire: the pool share caps it above 32");
    h_check(d.ipv6 > AMI_SANA2_RX_WANT_IPV6,
            "and IPv6 is planned past its small-machine cap");
    h_check((ULONG)d.ipv4 + d.arp + d.ipv6 <= 368UL / AMI_SANA2_RX_BUDGET_SHARE,
            "inside a quarter of that pool");

    plan_at(10000000UL, 4096UL, TRUE, &d);
    h_check(d.ipv6 == AMI_SANA2_RX_WANT_IPV6,
            "and a pool ten times that size does not move the IPv6 cap");

    /* The same machine on a wire slower than it: the cap bites, and it is the
       only configuration in which the reported line rate changes anything. */
    plan_at(2000000UL, 368UL, TRUE, &d);
    h_check(d.ipv4 < AMI_SANA2_RX_MAX_DEPTH,
            "a two-megabit wire on a big pool is capped by the wire");

    /* The budget is never exceeded, at any pool size, on the fastest wire
       there is. */
    for (pool = 0UL; pool <= 600UL; pool++)
    {
        ULONG total;
        ULONG budget = pool / (ULONG)AMI_SANA2_RX_BUDGET_SHARE;
        ULONG floors = (ULONG)AMI_SANA2_RX_DEPTH_IPV4 +
                       (ULONG)AMI_SANA2_RX_DEPTH_ARP +
                       (ULONG)AMI_SANA2_RX_DEPTH_IPV6;

        plan_at(0xFFFFFFFFUL, pool, TRUE, &d);
        total = (ULONG)d.ipv4 + (ULONG)d.arp + (ULONG)d.ipv6;

        h_check(total <= ((budget > floors) ? budget : floors),
                "the plan stays inside the budget, or inside the floors");
        h_check(d.ipv4 <= AMI_SANA2_RX_MAX_DEPTH &&
                d.arp  <= AMI_SANA2_RX_MAX_DEPTH &&
                d.ipv6 <= AMI_SANA2_RX_MAX_DEPTH,
                "and no reader is deeper than there are slots for it");
    }

    /* A bigger pool is never worse, which is the property the samples above
       are points on. */
    {
        UWORD prev = 0;

        for (pool = 0UL; pool <= 600UL; pool++)
        {
            plan_at(100000000UL, pool, TRUE, &d);
            h_check(d.ipv4 >= prev, "a bigger pool is never shallower");
            prev = d.ipv4;
        }
    }
}

static void test_plan_floors(void)
{
    AmiRxDepths d;
    ULONG       pool;
    ULONG       bps;

    printf("sana2: no reader is ever planned below what it had\n");

    for (pool = 0UL; pool <= 600UL; pool += 7UL)
    {
        for (bps = 0UL; bps <= 300000000UL; bps += 7000000UL)
        {
            plan_at(bps, pool, TRUE, &d);
            h_check(d.ipv4 >= AMI_SANA2_RX_DEPTH_IPV4, "IPv4 keeps its floor");
            h_check(d.arp  >= AMI_SANA2_RX_DEPTH_ARP,  "ARP keeps its floor");
            h_check(d.ipv6 >= AMI_SANA2_RX_DEPTH_IPV6, "IPv6 keeps its floor");
        }
    }
}

static void test_plan_single_stack(void)
{
    AmiRxDepths dual;
    AmiRxDepths single;

    printf("sana2: a build with no IPv6 reader does not budget for one\n");

    plan_at(100000000UL, 40UL, FALSE, &single);
    plan_at(100000000UL, 40UL, TRUE,  &dual);

    h_check(single.ipv6 == 0,
            "no IPv6 reader is planned when none will be started");
    h_check(single.arp == dual.arp, "the ARP reader is unmoved either way");

    h_check(single.ipv4 == dual.ipv4,
            "and IPv4 is no deeper for it: its want was already affordable");
    h_check((ULONG)single.ipv4 + single.arp <
            (ULONG)dual.ipv4 + dual.arp + dual.ipv6,
            "what changes is what the machine pins in total");
}

static void test_plan_arp_is_flat(void)
{
    AmiRxDepths d;
    ULONG       bps;

    printf("sana2: the ARP reader does not follow the line rate\n");

    for (bps = 0UL; bps <= 1000000000UL; bps += 50000000UL)
    {
        plan_at(bps, PLAN_BIG_POOL, TRUE, &d);
        h_check(d.arp == AMI_SANA2_RX_DEPTH_ARP,
                "ARP is the same depth on every wire");
    }
}

static void test_plan_asked(void)
{
    AmiRxDepths d;
    AmiRxDepths plain;

    printf("sana2: IPREQUESTS and ARPREQUESTS are met as far as the pool "
           "goes\n");

    /* A serial-speed wire the ladder holds at four: the file says sixteen. */
    plan_asked(115200UL, PLAN_BIG_POOL, 16, 0, &d);
    h_check(d.ipv4 == 16, "IPREQUESTS=16 on a slow wire is sixteen");
    h_check(d.arp == AMI_SANA2_RX_DEPTH_ARP, "and ARP is untouched");

    /* Below the floor is honoured too: a small machine's ask. */
    plan_asked(10000000UL, PLAN_BIG_POOL, 2, 1, &d);
    h_check(d.ipv4 == 2, "IPREQUESTS=2 is two, under the floor");
    h_check(d.arp == 1, "ARPREQUESTS=1 is one, under the floor");

    /* Above the ring is the ring, and an ask goes past the wire's row. */
    plan_asked(10000000UL, 4096UL, 64, 64, &d);
    h_check(d.ipv4 == 64, "IPREQUESTS=64 on a slow wire is sixty-four");
    h_check(d.arp == 64, "ARPREQUESTS=64 is sixty-four");
    plan_asked(10000000UL, 4096UL, 200, 200, &d);
    h_check(d.ipv4 == AMI_SANA2_RX_MAX_DEPTH, "IPREQUESTS=200 is the ceiling");
    h_check(d.arp == AMI_SANA2_RX_MAX_DEPTH, "ARPREQUESTS=200 is the ceiling");

    /* An ARP ask comes out of the same spare, and before the IPv6 default. */
    plan_asked(10000000UL, PLAN_BIG_POOL, 0, 8, &d);
    plan_at(10000000UL, PLAN_BIG_POOL, TRUE, &plain);
    h_check(d.arp == 8, "ARPREQUESTS=8 is eight");
    h_check(d.ipv4 == plain.ipv4, "and IPv4 is planned as without it");
    h_check(d.ipv6 == plain.ipv6, "and so is IPv6, on a pool this size");

    /* The pool budget still holds: seventeen packets buy the floors. */
    plan_asked(100000000UL, 17UL, 32, 32, &d);
    h_check(d.ipv4 == AMI_SANA2_RX_DEPTH_IPV4 &&
            d.arp  == AMI_SANA2_RX_DEPTH_ARP,
            "an ask cannot pin packets a seventeen-packet pool has not got");

    /* 47 packets: the budget is 11, the floors 8, so three to give, and IP
       gets them before ARP. */
    plan_asked(10000000UL, 47UL, 32, 32, &d);
    h_check(d.ipv4 == AMI_SANA2_RX_DEPTH_IPV4 + 3 &&
            d.arp  == AMI_SANA2_RX_DEPTH_ARP,
            "47 packets: the spare goes to IP first");
    plan_asked(10000000UL, 47UL, 0, 32, &d);
    h_check(d.ipv4 == 5 && d.arp == AMI_SANA2_RX_DEPTH_ARP + 2 && d.ipv6 == 2,
            "47 packets, ARP asked alone: IP keeps its plan, ARP takes the "
            "rest before IPv6");

    /* Nothing asked is the plan as it was. */
    plan_asked(10000000UL, 127UL, 0, 0, &d);
    h_check(d.ipv4 == 15 && d.arp == 2 && d.ipv6 == 8,
            "no ask: 127 packets are still 15/2/8");
}

static void test_plan_shares_one_pool(void)
{
    AmiRxDepths one;
    AmiRxDepths two;
    AmiRxDepths four;
    ULONG       pool;
    UWORD       n;

    printf("sana2: the read budget is the machine's, not each interface's\n");

    /* Unchanged for one interface, at every pool size, on the fastest wire and
       on a slow one. */
    for (pool = 0UL; pool <= 600UL; pool += 3UL)
    {
        AmiRxDepths implicit;

        plan_for(10000000UL, pool, TRUE, 1, &one);
        plan_for(10000000UL, pool, TRUE, 0, &implicit);

        h_check(one.ipv4 == implicit.ipv4 && one.arp == implicit.arp &&
                one.ipv6 == implicit.ipv6,
                "a caller that did not count is planned as one interface");
    }

    /* The lab's 8 MB A1200, 368 packets: alone it reaches both ceilings.
       Four interfaces on that pool may not each do so. */
    plan_for(10000000UL, 368UL, TRUE, 1, &one);
    plan_for(10000000UL, 368UL, TRUE, 2, &two);
    plan_for(10000000UL, 368UL, TRUE, 4, &four);

    h_check(one.ipv4 == AMI_SANA2_RX_DEPTH_LAN,
            "368 packets, one interface: the LAN ceiling, as before");
    h_check(two.ipv4 <= one.ipv4 && four.ipv4 <= two.ipv4,
            "and more interfaces never plan a deeper queue than fewer");

    /* The property, stated as the pool arithmetic and not as a number: what
       all the interfaces pin together stays inside the share, once the pool
       is big enough to pay the floors at all. */
    for (pool = 0UL; pool <= 600UL; pool += 1UL)
    {
        for (n = 1; n <= (UWORD)AMI_SANA2_RX_MAX_DEPTH; n *= 2)
        {
            ULONG floors = (ULONG)AMI_SANA2_RX_DEPTH_IPV4 +
                           (ULONG)AMI_SANA2_RX_DEPTH_ARP +
                           (ULONG)AMI_SANA2_RX_DEPTH_IPV6;
            ULONG share  = pool / (ULONG)AMI_SANA2_RX_BUDGET_SHARE;
            ULONG total;

            plan_for(0xFFFFFFFFUL, pool, TRUE, n, &four);
            total = ((ULONG)four.ipv4 + four.arp + four.ipv6) * (ULONG)n;

            h_check(total <= ((share > floors * (ULONG)n)
                                  ? share : floors * (ULONG)n),
                    "all the interfaces together stay inside the share, or "
                    "inside their floors");
        }
    }

    /* And the floors are still every interface's own, on the machine where
       they are the whole answer: 2 MB of chip RAM and no Fast RAM is 47
       packets, which cannot pay two interfaces anything above them. */
    plan_for(10000000UL, 47UL, TRUE, 2, &two);
    h_check(two.ipv4 == AMI_SANA2_RX_DEPTH_IPV4 &&
            two.arp  == AMI_SANA2_RX_DEPTH_ARP &&
            two.ipv6 == AMI_SANA2_RX_DEPTH_IPV6,
            "47 packets and two interfaces: each keeps its floors and no more");
}


/* ------------------------------------------------- the reader's block rule */

/*
 * 0.26.0 and 0.26.1 blocked the reader on `taken >= AMI_SANA2_RX_RUN_MAX ||
 * port empty`.  One Exec signal covers every completion already on the port --
 * it is a bit and not a count -- so blocking on the batch bound slept on
 * frames the reader was holding, and at the end of a response nothing further
 * arrived to wake it.  Read throughput fell from 938 KB/s to 257 on real
 * hardware while transmit was untouched.
 */

static struct MsgPort blk_port;
static struct Message blk_msg[AMI_SANA2_RX_RUN_MAX * 2 + 4];

static void blk_empty(struct List *l)
{
    l->lh_Head     = (struct Node *)&l->lh_Tail;
    l->lh_Tail     = NULL;
    l->lh_TailPred = (struct Node *)&l->lh_Head;
}

static void blk_fill(struct List *l, unsigned n)
{
    unsigned i;

    blk_empty(l);
    for (i = 0; i < n; i++)
    {
        struct Node *node = &blk_msg[i].mn_Node;
        struct Node *pred = l->lh_TailPred;

        node->ln_Succ  = (struct Node *)&l->lh_Tail;
        node->ln_Pred  = pred;
        pred->ln_Succ  = node;
        l->lh_TailPred = node;
    }
}

static void test_block_only_on_an_empty_port(void)
{
    static const UWORD taken[] = {
        0, 1,
        (UWORD)(AMI_SANA2_RX_RUN_MAX - 1),
        (UWORD)AMI_SANA2_RX_RUN_MAX,
        (UWORD)(AMI_SANA2_RX_RUN_MAX + 1),
        (UWORD)(AMI_SANA2_RX_RUN_MAX * 2)
    };
    AmiSana2Rx rx;
    unsigned   t, q;

    memset(&rx, 0, sizeof rx);
    rx.port = &blk_port;

    for (t = 0; t < sizeof taken / sizeof taken[0]; t++)
    {
        blk_empty(&blk_port.mp_MsgList);
        h_check(ami_sana2_rx_should_block(&rx, taken[t]) != FALSE,
                "an empty port is the one thing that blocks");

        for (q = 1; q <= 3; q++)
        {
            blk_fill(&blk_port.mp_MsgList, q);
            h_check(ami_sana2_rx_should_block(&rx, taken[t]) == FALSE,
                    "a queued completion never blocks, whatever the batch count");
        }
    }
}

/*
 * The end of a response: one burst arrives, its signal is consumed by the
 * Wait() that woke the reader, and nothing follows.  Drive the real predicate
 * over that port until it says block, and count what is left behind.
 */
static UWORD blk_left_by_real_rule(void)
{
    AmiSana2Rx rx;
    UWORD      queued = (UWORD)(AMI_SANA2_RX_RUN_MAX + 3);
    UWORD      taken  = 0;

    memset(&rx, 0, sizeof rx);
    rx.port = &blk_port;

    while (queued > 0)
    {
        blk_fill(&blk_port.mp_MsgList, queued);
        if (ami_sana2_rx_should_block(&rx, taken) != FALSE)
            break;
        queued--;
        taken++;
    }

    return queued;
}

/* The rule that shipped, as a model, so the scenario above is known to have
   teeth: if this did not strand a frame the test would prove nothing. */
static UWORD blk_left_by_shipped_rule(void)
{
    UWORD queued = (UWORD)(AMI_SANA2_RX_RUN_MAX + 3);
    UWORD taken  = 0;

    while (queued > 0)
    {
        if (taken >= AMI_SANA2_RX_RUN_MAX)
            break;
        queued--;
        taken++;
    }

    return queued;
}

static void test_a_burst_is_never_left_on_the_port(void)
{
    h_check(blk_left_by_shipped_rule() == 3,
            "the 0.26.0 rule strands the tail of a burst");
    h_check(blk_left_by_real_rule() == 0,
            "the shipped rule takes the whole burst before it blocks");
}

int main(void)
{
    test_demux();
    test_header_strip();
    test_payload_alignment();
    test_runt();
    test_completion_length_consistency();
    test_link_header_is_counted_either_way();
    test_block_only_on_an_empty_port();
    test_a_burst_is_never_left_on_the_port();

    test_plan_ladder();
    test_plan_degenerate_bps();
    test_plan_budget();
    test_plan_floors();
    test_plan_single_stack();
    test_plan_arp_is_flat();
    test_plan_asked();
    test_plan_shares_one_pool();

#ifdef AMINETXDUO_RX_VERIFY
    test_verify_publishes_only_what_it_checked();
    test_verify_drop();
    test_verify_uses_the_carried_sum();
#endif
#ifdef AMINETXDUO_GRO
    test_verified_skips_the_walk();
    test_held_frame_goes_up_on_flush();
    test_continuing_frame_is_chained();
    test_run_ends_on_a_frame_that_does_not_continue();
    test_run_is_capped();
#endif

    printf("%lu checks, %lu failures, %s\n", h_checks, h_failures,
           (h_failures == 0) ? "PASS" : "FAIL");

    return (h_failures == 0) ? 0 : 1;
}

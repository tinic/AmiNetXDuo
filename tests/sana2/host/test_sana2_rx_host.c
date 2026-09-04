/*
 * AmiNetXDuo, the SANA-II receive path's delivery, on the host.
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
#else
    sum.sum    = 0;
    sum.summed = FALSE;
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
    ami_sana2_rx_plan(bps, pool, dual, ifaces, d);
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
        {  1000000000UL, 32, "a gigabit wire"                              },
        {  0xFFFFFFFFUL, 32, "a rate that did not fit a ULONG"             }
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
    h_check(d.ipv4 == AMI_SANA2_RX_MAX_DEPTH,
            "368 packets: IPv4 gets the ceiling");
    h_check(d.ipv6 == AMI_SANA2_RX_WANT_IPV6,
            "and IPv6 gets its own cap rather than two");
    plan_at(100000000UL, 368UL, TRUE, &d);
    h_check(d.ipv4 == AMI_SANA2_RX_MAX_DEPTH,
            "and a hundred-megabit card on that machine asks for no more");

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

    h_check(one.ipv4 == AMI_SANA2_RX_MAX_DEPTH,
            "368 packets, one interface: the ceiling, as before");
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
    test_block_only_on_an_empty_port();
    test_a_burst_is_never_left_on_the_port();

    test_plan_ladder();
    test_plan_degenerate_bps();
    test_plan_budget();
    test_plan_floors();
    test_plan_single_stack();
    test_plan_arp_is_flat();
    test_plan_shares_one_pool();

#ifdef AMINETXDUO_RX_VERIFY
    test_verify_publishes_only_what_it_checked();
    test_verify_drop();
    test_verify_uses_the_carried_sum();
#endif

    printf("%lu checks, %lu failures, %s\n", h_checks, h_failures,
           (h_failures == 0) ? "PASS" : "FAIL");

    return (h_failures == 0) ? 0 : 1;
}

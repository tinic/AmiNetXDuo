/*
 * AmiNetXDuo, the SANA-II receive path's delivery, on the host.
 *
 * ami_sana2_rx_deliver() is where a frame stops being the device's and becomes
 * the stack's.  It reads the EtherType out of the link header, hands the
 * packet to one of three NetX Duo deferred entry points, strips the header and
 * counts what happened -- and it is the last place anything looks at the frame
 * before NetX Duo parses it as an IP datagram.
 *
 * Two of its decisions are load-bearing and neither is visible when wrong:
 *
 *   The strip.  Fourteen bytes come off the front and fourteen off the length,
 *   and the two have to agree.  A length left a header long makes every packet
 *   carry fourteen bytes of somebody else's frame off the end; a prepend
 *   pointer left where it was hands the IP layer an Ethernet header to parse as
 *   a version nibble.  Both look like a peer sending nonsense.
 *
 *   The runt reject.  A frame shorter than a link header has no EtherType to
 *   read, and reading one anyway is a load past the end of what was received.
 *   The check has to come first.
 *
 * WHAT IS DELIBERATELY NOT HERE
 *
 *   ami_sana2_rx_arm(), _post(), _complete() and _drain() are static and are
 *   reached only from ami_sana2_rx_thread(), which is an Exec task that
 *   Wait()s on a signal raised from device interrupt context.  Driving them
 *   from a host binary would mean either making them extern or faking a
 *   scheduler, and neither is worth doing to the shipping code.  The Ethernet
 *   header ami_sana2_rx_complete() synthesises therefore stays covered by
 *   tests/tcpdrill under an emulator, and this file says so rather than
 *   leaving the gap silent.
 *
 * SPDX-License-Identifier: MIT
 */

#include "sana2_internal.h"

#include <stdio.h>
#include <string.h>

/* --------------------------------------------------------------- harness -- */

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

/* ------------------------------------------------------------------ exec -- */

VOID Disable(VOID) { }
VOID Enable(VOID)  { }
VOID Forbid(VOID)  { }
VOID Permit(VOID)  { }

VOID SendIO(struct IORequest *req) { (VOID)req; }
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

/*
 * The rest of sana2_rx.c is the reader task and its teardown.  None of it is
 * called from here; these exist because the translation unit is compiled
 * whole, which is the point -- the function under test is the one that ships,
 * in the object that ships, not a copy.
 */
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

UINT tx_amiga_stack_in_use(APTR base, ULONG size)
{
    (VOID)base;
    (VOID)size;
    return 0;
}

/* No port here, so no Exec Task ever outlives its TX_THREAD: the count the
   teardown reads to decide whether freeing a reader stack is safe is always
   zero, which is the answer that lets it free. */
ULONG tx_amiga_zombie_tasks_live(VOID)
{
    return 0;
}

/* --------------------------------------------------------------- threadx -- */

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

/* --------------------------------------------------------------- netx duo -- */

/*
 * Which deferred entry point a frame reached, and what shape it was in when it
 * got there.  Recorded rather than asserted inside the stub, so each test says
 * what it expects.
 */
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

VOID _nx_ip_packet_deferred_receive(NX_IP *ip_ptr, NX_PACKET *packet_ptr)
{
    (VOID)ip_ptr;
    h_record(packet_ptr, TO_IP);
}

VOID _nx_arp_packet_deferred_receive(NX_IP *ip_ptr, NX_PACKET *packet_ptr)
{
    (VOID)ip_ptr;
    h_record(packet_ptr, TO_ARP);
}

VOID _nx_rarp_packet_deferred_receive(NX_IP *ip_ptr, NX_PACKET *packet_ptr)
{
    (VOID)ip_ptr;
    h_record(packet_ptr, TO_RARP);
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

/* ---------------------------------------------------------------- net68k -- */

VOID ami_sana2_copy_bytes(UCHAR *to, const UCHAR *from, ULONG len)
{
    if (len != 0)
        memcpy(to, from, (size_t)len);
}

/*
 * The receive verifier, when this is an AMINETXDUO_RX_VERIFY build.  What it
 * answers is the test's to choose: the capability flags it publishes are what
 * tell NetX Duo not to walk the payload again, so a frame it declines must
 * carry NO flags and a frame it rejects must not reach the stack at all.
 */
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

/* ---------------------------------------------------- the transmit side -- */

VOID ami_sana2_tx_defer(AmiSana2If *iface) { (VOID)iface; }
VOID ami_sana2_tx_reap_bind(AmiSana2If *iface, struct Task *task, BYTE sigbit)
{
    (VOID)iface; (VOID)task; (VOID)sigbit;
}
VOID ami_sana2_tx_reap_unbind(AmiSana2If *iface) { (VOID)iface; }
LONG ami_sana2_offline(AmiSana2If *iface) { (VOID)iface; return 0; }
VOID ami_sana2_block_enter(VOID) { }
VOID ami_sana2_block_leave(VOID) { }

/* ------------------------------------------------------------- fixtures -- */

static AmiSana2If   iface;
static NX_IP        ip;
static NX_INTERFACE interface_obj;

/*
 * A packet in the shape ami_sana2_rx_complete() hands over: prepend at
 * data_start + AMI_SANA2_RX_PAD, the link header there, the payload after it,
 * length covering both.
 */
static UCHAR      buffer[256];
static NX_PACKET  pkt;
static AmiRxSlot  slot;

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
}

/* A frame of `payload` bytes after a link header carrying `type`. */
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

/* ================================================== the EtherType demux == */

/*
 * Four types go four places and everything else is dropped.  The default arm
 * matters as much as the four: a frame with a type nobody handles that is
 * passed to the IP thread anyway is an NX_PACKET the stack will not recognise
 * and will not release, and on a machine with a fixed pool that is a leak.
 */
static void test_demux(void)
{
    static const struct {
        UWORD       type;
        Destination where;
        const char *what;
    } row[] = {
        { AMI_ETHERTYPE_IPV4, TO_IP,       "IPv4 goes to the IP thread" },
        { AMI_ETHERTYPE_IPV6, TO_IP,       "IPv6 goes to the IP thread" },
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

        ami_sana2_rx_deliver(&iface, &pkt, &slot);

        h_check(h_went == row[i].where, row[i].what);
    }

    /* And an unknown type is counted as one, not as an error: it is a wire
       with other traffic on it, which is normal. */
    fixture_init();
    frame_init(0x8100, 40);
    ami_sana2_rx_deliver(&iface, &pkt, &slot);

    h_check(iface.stats.unknown_types == 1, "an unknown type is counted");
    h_check(iface.stats.rx_errors == 0, "and is not an error");
    h_check(iface.stats.packets_received == 0, "and is not received");
    h_check(h_releases == 1, "and the packet goes back to the pool");
}

/* ==================================================== the header strip === */

/*
 * The prepend pointer and the length have to move together.  A length that
 * kept the header's fourteen bytes puts fourteen bytes of the next thing in
 * the buffer on the end of every datagram, which TCP sees as data and UDP
 * hands to the application.
 */
static void test_header_strip(void)
{
    printf("sana2: the link header comes off the pointer and the length\n");

    fixture_init();
    frame_init(AMI_ETHERTYPE_IPV4, 40);

    ami_sana2_rx_deliver(&iface, &pkt, &slot);

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

/*
 * The IP header's alignment.  Every longword NetX Duo and n68k_checksum.c read
 * out of the payload assumes it starts on a multiple of four, and on a 68000
 * an odd one is an Address Error rather than a slow path.  The pad exists to
 * make that true and there is a _Static_assert on it in sana2_rx.c; this is
 * the run-time half, on the pointer the stack actually receives.
 */
static void test_payload_alignment(void)
{
    printf("sana2: the payload lands on a longword boundary\n");

    fixture_init();
    frame_init(AMI_ETHERTYPE_IPV4, 40);

    h_check(((AMI_SANA2_RX_PAD + AMI_ETH_HEADER_SIZE) & 3) == 0,
            "the pad plus the header is a multiple of four");

    ami_sana2_rx_deliver(&iface, &pkt, &slot);

    h_check((((ULONG)(pkt.nx_packet_prepend_ptr - buffer)) & 3) == 0,
            "so the payload starts aligned from the pool block");
}

/* ===================================================== the runt reject === */

/*
 * A frame shorter than a link header is refused before the type is read.  The
 * order is the whole of it: reading base[12] and base[13] out of a nine-byte
 * frame is a load past what the device wrote, and what it finds there decides
 * where the packet goes.
 */
static void test_runt(void)
{
    ULONG length;

    printf("sana2: a frame shorter than a link header is refused\n");

    for (length = 0; length < AMI_ETH_HEADER_SIZE; length++)
    {
        fixture_init();
        runt_init(length);

        ami_sana2_rx_deliver(&iface, &pkt, &slot);

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

    ami_sana2_rx_deliver(&iface, &pkt, &slot);

    h_check(h_went == TO_IP, "a header with no payload is not a runt");
    h_check(h_seen_length == 0, "and arrives with nothing in it");
    h_check(iface.stats.rx_err_runt == 0, "and is not counted as one");
}

/* ============================================ the receive checksum fork == */

#ifdef AMINETXDUO_RX_VERIFY

/*
 * The offload.  ami_sana2_rx_deliver() tells NetX Duo what it verified through
 * nx_packet_interface_capability_flag, and the stack then does not walk those
 * bytes again.  Publishing a flag for a check that did not happen is a frame
 * accepted with a bad checksum; publishing none for one that did is only slow.
 *
 * IPv4 only.  IPv6, ARP and RARP go through untouched, and a flag left on one
 * of them from a previous frame would be the same lie.
 */
static void test_verify_publishes_only_what_it_checked(void)
{
    printf("sana2: what the verifier checked is what is published\n");

    fixture_init();
    frame_init(AMI_ETHERTYPE_IPV4, 40);
    h_verify_caps = NX_INTERFACE_CAPABILITY_IPV4_RX_CHECKSUM |
                    NX_INTERFACE_CAPABILITY_TCP_RX_CHECKSUM;

    ami_sana2_rx_deliver(&iface, &pkt, &slot);

    h_check(h_went == TO_IP, "the frame reached the IP thread");
    h_check(pkt.nx_packet_interface_capability_flag == h_verify_caps,
            "carrying exactly what the verifier answered");

    /* A frame the verifier declines carries no claim at all. */
    fixture_init();
    frame_init(AMI_ETHERTYPE_IPV4, 40);
    h_verify_caps = 0;

    ami_sana2_rx_deliver(&iface, &pkt, &slot);

    h_check(pkt.nx_packet_interface_capability_flag == 0,
            "a declined frame claims nothing and the stack walks it");

    /* IPv6 does not go through the verifier at all. */
    fixture_init();
    frame_init(AMI_ETHERTYPE_IPV6, 40);
    h_verify_caps = 0xFFFFFFFFUL;

    ami_sana2_rx_deliver(&iface, &pkt, &slot);

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

    ami_sana2_rx_deliver(&iface, &pkt, &slot);

    h_check(h_went == TO_RELEASED, "it reaches no receiver");
    h_check(h_releases == 1, "and goes back to the pool");
    h_check(iface.stats.rx_errors == 1, "and is an error");
    h_check(iface.stats.rx_err_verify == 1, "of the verify kind");
    h_check(iface.stats.packets_received == 0, "and is not received");
}

/*
 * Which of the two verifier entry points is used, and why it matters.
 *
 * The copy hook sums the frame out of loads the copy was already doing, and
 * sets slot->summed when it did.  A slot that did NOT sum -- a misaligned
 * buffer, or a driver that never called the hook -- must send the verifier
 * down the walking path, because the accumulator in the slot is then the
 * PREVIOUS frame's and checking these bytes against it would accept anything.
 */
static void test_verify_uses_the_carried_sum(void)
{
    printf("sana2: a carried sum is used, and only when it is this frame's\n");

    fixture_init();
    frame_init(AMI_ETHERTYPE_IPV4, 40);
    slot.summed = TRUE;
    slot.sum    = 0x1234;
    slot.copied = 40;

    ami_sana2_rx_deliver(&iface, &pkt, &slot);

    h_check(h_verify_sums == 1 && h_verify_walks == 0,
            "a summed slot hands the sum over");

    fixture_init();
    frame_init(AMI_ETHERTYPE_IPV4, 40);
    slot.summed = FALSE;
    slot.sum    = 0x1234;       /* stale, from the previous frame */

    ami_sana2_rx_deliver(&iface, &pkt, &slot);

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

/* ------------------------------------------------------------------ main -- */

int main(void)
{
    test_demux();
    test_header_strip();
    test_payload_alignment();
    test_runt();

#ifdef AMINETXDUO_RX_VERIFY
    test_verify_publishes_only_what_it_checked();
    test_verify_drop();
    test_verify_uses_the_carried_sum();
#endif

    printf("%lu checks, %lu failures, %s\n", h_checks, h_failures,
           (h_failures == 0) ? "PASS" : "FAIL");

    return (h_failures == 0) ? 0 : 1;
}

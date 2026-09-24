/*
 * src/bsdsocket/cork.c on the host, with transfer.c and select.c around it:
 * the small-write cork behind setsockopt(TCP_NODELAY, 0).
 *
 * THE ACTORS, AND HOW EACH IS PLAYED HERE (cork.c's header names them).
 *
 *   A  bsd_send()/bsd_recv()/bsd_WaitSelect(), called for real.
 *   I  bsd_cork_ip_pass(), called by the test where the IP thread would run
 *      its event pass.
 *   T  bsd_cork_tick(), called where the tick would fire; the event it sets
 *      is recorded, and the test decides when I runs.
 *   R  the window notify select.c installs, captured from
 *      nx_tcp_socket_window_update_notify_set() and called directly.
 *   F  the append fast path (the _fast build of this file only), raced
 *      against I by a bsd_bcopy() that runs the pass half way through.
 *   B  another task: FindTask() answers a second Task while it "owns" the
 *      socket, and tx_thread_sleep() is where it finishes.
 *
 * NetX Duo is scripted: nx_tcp_socket_send() takes all of a packet or, for a
 * planned refusal, the planned number of bytes off its front, exactly as
 * _nx_tcp_socket_send_internal() trims a partially sent packet.  Every byte
 * that reaches the "wire" is recorded in order.  Every NetX Duo call and every
 * payload copy asserts it is not inside Forbid().
 *
 * NOT COVERED HERE: socket.c's close paths are not in this binary.  What they
 * call -- bsd_cork_close_graceful(), bsd_cork_close_linger(), bsd_cork_drop(),
 * bsd_cork_shut_write() -- is driven directly, and the park, the sweep and the
 * abort around those calls are exercised on the emulator.
 *
 * 32-bit only, for transfer.c's scatter/gather ABI asserts (test_transfer's
 * reason; tests/bsdsocket/CMakeLists.txt).
 *
 * SPDX-License-Identifier: MIT
 */

#include "bsdsocket_vectors.h"
#include "udp_queue.h"
#include "netmonitor.h"
#include "aminetxduo/sana2.h"
#include "nx_ip.h"

#include <proto/dos.h>

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef AMINETXDUO_TCP_CORK
#error "test_cork is the cork's test: build it with AMINETXDUO_TCP_CORK"
#endif

_Static_assert(sizeof(void *) == 4, "this test needs the target's pointer width");

static unsigned long h_checks;
static unsigned long h_failures;

#define CHECK(cond, what)                                                     \
    do {                                                                      \
        h_checks++;                                                           \
        if (!(cond)) {                                                        \
            h_failures++;                                                     \
            printf("  FAIL %s (line %d)\n", (what), __LINE__);                \
        }                                                                     \
    } while (0)

#define H_FDS           4
#define H_PKTS          16
#define H_BUF           1600
#define H_PLAN          16

#define H_EVENT_SIG     (1UL << 12)
#define H_BREAK_SIG     (1UL << 13)

static struct AmiSocketBase h_base;
static struct Task          h_task;
static struct Task          h_other;
static AmiSocket            h_sock[H_FDS];
static AmiSocket           *h_table[H_FDS];
static NX_PACKET_POOL       h_pool;
static NX_IP                h_ip;
static NX_INTERFACE         h_eth;
static NX_INTERFACE         h_lo;
static int                  h_sana;         /* what link info points at */

typedef struct HPacket
{
    NX_PACKET   nx;
    UBYTE       buf[H_BUF];
    BOOL        in_use;
} HPacket;

static HPacket h_pkt[H_PKTS];

static struct
{
    ULONG        signals;
    struct Task *me;
    int          forbid;
    ULONG        unsafe;            /* NetX Duo or a copy inside Forbid()   */

    LONG         nx_enter_result;
    ULONG        nx_enters, nx_leaves;

    ULONG        mss;

    UINT         alloc_plan[H_PLAN];
    unsigned     alloc_planned, allocs;

    UINT         send_plan[H_PLAN];
    ULONG        send_take[H_PLAN];
    unsigned     send_planned, sends;
    ULONG        send_wait[H_PLAN];
    unsigned     break_at_send;     /* set the break on this send, 1-based  */

    UBYTE        wire[4096];
    ULONG        wire_len;
    ULONG        releases;

    ULONG        events;
    ULONG        event_sets;

    ULONG        timer_creates, activates, deactivates, deletes;
    BOOL         timer_live;

    ULONG        mutex_gets, mutex_puts;
    LONG         enter_result;      /* ami_netstack_enter()'s answer         */
    UINT         deactivate_result, delete_result;
    ULONG        delays;
    void       (*send_hook)(void);  /* runs inside the next NetX send        */
    BOOL         sleep_jumps;       /* the next tx_thread_sleep() longjmps    */
    ULONG        enters, leaves;
    AmiSocket   *mutex_finishes;    /* the pass "ends" when the barrier runs */

    ULONG        sleeps;
    AmiSocket   *sleep_finishes;    /* F or B "finishes" when A sleeps       */

    ULONG        fins;
    ULONG        fin_at;            /* wire_len when the FIN went            */
    ULONG        oobs;
    ULONG        oob_at;

    ULONG        ticks;

    VOID       (*window_notify)(NX_TCP_SOCKET *);

    BOOL         copy_runs_pass;    /* the next bsd_bcopy() runs I mid-copy  */
    AmiSocket   *copy_runs_notify;  /* ...or R's window notify for this one  */
    BOOL         mutex_busy;        /* I holds nx_ip_protection: gets time out */
    BOOL         mutex_breaks;      /* the first busy get raises Ctrl-C      */
    ULONG        mutex_wait_max;
    ULONG        copies;

    ULONG        rx_queue;          /* receive stub: packets it can hand out */
    ULONG        receives;
} h;

static UBYTE h_data[512];

static void h_reset(void)
{
    unsigned i;

    /* The cork's statics outlive a test: take it down, as the stack would,
       with every ThreadX answer back to success. */
    h.enter_result      = AMI_NET_OK;
    h.deactivate_result = TX_SUCCESS;
    h.delete_result     = TX_SUCCESS;
    h.send_hook         = NULL;
    h.sleep_jumps       = FALSE;
    bsd_cork_stop();

    memset(&h, 0, sizeof(h));
    memset(&h_base, 0, sizeof(h_base));
    memset(&h_sock, 0, sizeof(h_sock));
    memset(&h_table, 0, sizeof(h_table));
    memset(&h_pkt, 0, sizeof(h_pkt));
    memset(&h_pool, 0, sizeof(h_pool));
    memset(&h_ip, 0, sizeof(h_ip));
    memset(&h_eth, 0, sizeof(h_eth));
    memset(&h_lo, 0, sizeof(h_lo));

    for (i = 0; i < sizeof(h_data); i++)
        h_data[i] = (UBYTE)(i * 7 + 1);

    h.me  = &h_task;
    h_task.tc_Node.ln_Type = NT_PROCESS;
    h.mss = 100;

    h_base.sb_StackRefs    = 1;
    h_base.sb_StackIp      = &h_ip;
    h_base.sb_StackPool    = &h_pool;
    h_base.sb_Task         = &h_task;
    h_base.sb_Table        = h_table;
    h_base.sb_TableSize    = H_FDS;
    h_base.sb_EventSigMask = H_EVENT_SIG;
    h_base.sb_BreakMask    = H_BREAK_SIG;

    h_eth.nx_interface_valid                = 1;
    h_eth.nx_interface_additional_link_info = &h_sana;
    h_lo.nx_interface_valid                 = 1;
    h_lo.nx_interface_additional_link_info  = NX_NULL;   /* the loopback */

    bsd_cork_start(&h_ip);
}

/* A connected stream socket with its notifies installed, corked. */
static AmiSocket *h_tcp(LONG fd)
{
    AmiSocket *s = &h_sock[fd];

    s->as_Owner = &h_base;
    s->as_Flags = ASF_TCP | ASF_CONNECTED;
    s->as_Nx.tcp.nx_tcp_socket_state                  = NX_TCP_ESTABLISHED;
    s->as_Nx.tcp.nx_tcp_socket_connect_interface      = &h_eth;
    s->as_Nx.tcp.nx_tcp_socket_transmit_queue_maximum = 8;
    h_table[fd] = s;

    bsd_events_attach(s);
    CHECK(bsd_cork_set(&h_base, s, TRUE) == 0, "TCP_NODELAY 0 turns it on");

    return s;
}

static LONG h_send(LONG fd, ULONG off, ULONG len, LONG flags)
{
    return bsd_send(fd, &h_data[off], (LONG)len, flags, &h_base);
}

static ULONG h_pending(const AmiSocket *s)
{
    return (s->as_CorkPkt != NULL) ? s->as_CorkPkt->nx_packet_length : 0UL;
}

static BOOL h_linked(const AmiSocket *s)
{
    return (s->as_CorkFlags & BSD_CORKF_LINKED) != 0;
}

static BOOL h_wire_is(ULONG off, ULONG len)
{
    return h.wire_len == len && memcmp(h.wire, &h_data[off], len) == 0;
}

static void h_pass(void)
{
    h.events &= ~NX_IP_CORK_EVENT;
    bsd_cork_ip_pass(&h_ip);
}

/* ---------------------------------------------------------------- Exec -- */

/* Runs once, at the next Forbid(): another actor slipping in between an
   unlocked look and the lock that should have covered it. */
static void (*h_forbid_hook)(void);

VOID Forbid(VOID)
{
    if (h_forbid_hook != NULL)
    {
        void (*fn)(void) = h_forbid_hook;

        h_forbid_hook = NULL;
        fn();
    }
    h.forbid++;
}
VOID Permit(VOID) { h.forbid--; }

static void h_safe(void)
{
    if (h.forbid != 0)
        h.unsafe++;
}

struct Task *FindTask(const char *name)
{
    (VOID)name;
    return h.me;
}

ULONG SetSignal(ULONG newSignals, ULONG signalSet)
{
    ULONG old = h.signals;

    h.signals = (old & ~signalSet) | (newSignals & signalSet);
    return old;
}

VOID Signal(struct Task *task, ULONG signalSet)
{
    (VOID)task;
    h.signals |= signalSet;
}

ULONG Wait(ULONG signalSet)
{
    printf("  FAIL Wait(0x%lx) reached\n", (unsigned long)signalSet);
    h_failures++;
    abort();
    return 0;
}

BYTE OpenDevice(const UBYTE *devName, ULONG unit, struct IORequest *io,
                ULONG flags)
{
    (VOID)devName; (VOID)unit; (VOID)io; (VOID)flags;
    return 0;
}

VOID SendIO(struct IORequest *io)  { (VOID)io; }
LONG AbortIO(struct IORequest *io) { (VOID)io; return 0; }
LONG WaitIO(struct IORequest *io)  { (VOID)io; return 0; }
struct IORequest *CheckIO(struct IORequest *io) { (VOID)io; return NULL; }

BYTE ami_signal_alloc(VOID)    { return 20; }
VOID ami_signal_free(BYTE sig) { (VOID)sig; }

/* The one copy a corked write makes.  F is raced against I here. */
VOID bsd_bcopy(CONST_APTR src, APTR dst, ULONG size)
{
    h_safe();
    h.copies++;

    if (h.copy_runs_pass)
    {
        h.copy_runs_pass = FALSE;
        h_pass();
    }

    if (h.copy_runs_notify != NULL)
    {
        AmiSocket *s = h.copy_runs_notify;

        h.copy_runs_notify = NULL;
        h.window_notify(&s->as_Nx.tcp);
    }

    memcpy(dst, src, size);
}

VOID bsd_bzero(APTR p, ULONG size) { memset(p, 0, size); }

/* ------------------------------------------------------------ ThreadX -- */

ULONG _tx_time_get(VOID) { return h.ticks; }

static jmp_buf h_sleep_jmp;

/* The stop no longer sleeps; linking this is what would say it does. */
LONG Delay(ULONG ticks)
{
    h.delays++;
    h.ticks += ticks;
    return 0;
}

UINT _tx_thread_sleep(ULONG timer_ticks)
{
    h.sleeps++;
    if (h.sleep_jumps)
    {
        h.sleep_jumps = FALSE;
        longjmp(h_sleep_jmp, 1);
    }
    h.ticks += timer_ticks;

    /* The other actor gets the machine and finishes. */
    if (h.sleep_finishes != NULL)
    {
        h.sleep_finishes->as_CorkState = BSD_CORK_IDLE;
        h.sleep_finishes->as_CorkOwner = NULL;
        h.sleep_finishes = NULL;
    }
    return TX_SUCCESS;
}

UINT _txe_timer_create(TX_TIMER *timer_ptr, CHAR *name_ptr,
                       VOID (*expiration_function)(ULONG input),
                       ULONG expiration_input, ULONG initial_ticks,
                       ULONG reschedule_ticks, UINT auto_activate,
                       UINT timer_control_block_size)
{
    (VOID)timer_ptr; (VOID)name_ptr; (VOID)expiration_input;
    (VOID)timer_control_block_size;

    CHECK(expiration_function == bsd_cork_tick, "the timer's callback is T");
    CHECK(initial_ticks == 1 && reschedule_ticks == 1, "one tick, periodic");
    CHECK(auto_activate == TX_NO_ACTIVATE, "created inactive");
    h.timer_creates++;
    return TX_SUCCESS;
}

UINT _txe_timer_activate(TX_TIMER *timer_ptr)
{
    (VOID)timer_ptr;
    CHECK(!h.timer_live, "never activated twice");
    h.activates++;
    h.timer_live = TRUE;
    return TX_SUCCESS;
}

UINT _txe_timer_deactivate(TX_TIMER *timer_ptr)
{
    (VOID)timer_ptr;
    if (h.deactivate_result != TX_SUCCESS)
        return h.deactivate_result;
    if (h.timer_live)
        h.deactivates++;
    h.timer_live = FALSE;
    return TX_SUCCESS;
}

UINT _txe_timer_delete(TX_TIMER *timer_ptr)
{
    (VOID)timer_ptr;
    CHECK(!h.timer_live, "deleted only once deactivated");
    h.deletes++;
    return h.delete_result;
}

UINT _txe_event_flags_set(TX_EVENT_FLAGS_GROUP *group_ptr, ULONG flags_to_set,
                          UINT set_option)
{
    CHECK(group_ptr == &h_ip.nx_ip_events, "the IP instance's own events");
    CHECK(set_option == TX_OR, "or'ed in");
    h.events |= flags_to_set;
    h.event_sets++;
    return TX_SUCCESS;
}

UINT _txe_mutex_get(TX_MUTEX *mutex_ptr, ULONG wait_option)
{
    (VOID)wait_option;
    h_safe();
    if (mutex_ptr == &h_ip.nx_ip_protection)
    {
        h.mutex_gets++;
        if (wait_option > h.mutex_wait_max)
            h.mutex_wait_max = wait_option;

        /* The pass is still running: a bounded get times out. */
        if (h.mutex_busy)
        {
            h.ticks += wait_option;
            if (h.mutex_breaks)
            {
                h.mutex_breaks = FALSE;
                h.signals |= H_BREAK_SIG;
            }
            return TX_NOT_AVAILABLE;
        }

        /* The barrier's get returns once the pass that held it has ended. */
        if (h.mutex_finishes != NULL)
        {
            h.mutex_finishes->as_CorkState = BSD_CORK_IDLE;
            h.mutex_finishes->as_CorkOwner = NULL;
            h.mutex_finishes = NULL;
        }
    }
    return TX_SUCCESS;
}

UINT _txe_mutex_put(TX_MUTEX *mutex_ptr)
{
    if (mutex_ptr == &h_ip.nx_ip_protection)
        h.mutex_puts++;
    return TX_SUCCESS;
}

/* The bracket start and stop take.  A heap caller is no longer used, so an
   allocation failure is not a path; what is left is the kernel refusing
   (h.enter_result), and whether the timer still goes then. */
LONG ami_netstack_enter(AmiNetCaller *caller)
{
    (VOID)caller;
    h.enters++;
    return h.enter_result;
}

VOID ami_netstack_leave(AmiNetCaller *caller)
{
    (VOID)caller;
    h.leaves++;
}

/* ----------------------------------------------------------- NetX Duo -- */

static HPacket *h_from_nx(NX_PACKET *p)
{
    unsigned i;

    for (i = 0; i < H_PKTS; i++)
    {
        if (&h_pkt[i].nx == p)
            return &h_pkt[i];
    }
    return NULL;
}

static UINT h_plan(const UINT *plan, unsigned planned, unsigned n)
{
    if (planned == 0)
        return NX_SUCCESS;
    return plan[(n < planned) ? n : planned - 1];
}

UINT _nxe_packet_allocate(NX_PACKET_POOL *pool_ptr, NX_PACKET **packet_ptr,
                          ULONG packet_type, ULONG wait_option)
{
    unsigned i;
    UINT     status;

    (VOID)pool_ptr; (VOID)wait_option;
    h_safe();

    status = h_plan(h.alloc_plan, h.alloc_planned, h.allocs);
    h.allocs++;
    if (status != NX_SUCCESS)
        return status;

    for (i = 0; i < H_PKTS; i++)
    {
        if (!h_pkt[i].in_use)
        {
            HPacket *p = &h_pkt[i];

            memset(p, 0, sizeof(*p));
            p->in_use                  = TRUE;
            p->nx.nx_packet_data_start = p->buf;
            p->nx.nx_packet_data_end   = p->buf + H_BUF;
            p->nx.nx_packet_prepend_ptr = p->buf + packet_type;
            p->nx.nx_packet_append_ptr  = p->buf + packet_type;
            *packet_ptr = &p->nx;
            return NX_SUCCESS;
        }
    }
    return NX_NO_PACKET;
}

UINT _nxe_packet_data_append(NX_PACKET *packet_ptr, VOID *data_start,
                             ULONG data_size, NX_PACKET_POOL *pool_ptr,
                             ULONG wait_option)
{
    (VOID)pool_ptr; (VOID)wait_option;
    h_safe();

    memcpy(packet_ptr->nx_packet_append_ptr, data_start, data_size);
    packet_ptr->nx_packet_append_ptr += data_size;
    packet_ptr->nx_packet_length     += data_size;
    return NX_SUCCESS;
}

UINT _nxe_packet_length_get(NX_PACKET *packet_ptr, ULONG *length)
{
    *length = packet_ptr->nx_packet_length;
    return NX_SUCCESS;
}

UINT _nxe_packet_release(NX_PACKET **packet_ptr_ptr)
{
    HPacket *p = h_from_nx(*packet_ptr_ptr);

    h_safe();
    h.releases++;
    if (p != NULL)
        p->in_use = FALSE;
    return NX_SUCCESS;
}

UINT _nxe_tcp_socket_mss_get(NX_TCP_SOCKET *socket_ptr, ULONG *mss)
{
    (VOID)socket_ptr;
    h_safe();
    *mss = h.mss;
    return NX_SUCCESS;
}

static void h_wire(const UBYTE *from, ULONG n)
{
    if (h.wire_len + n <= sizeof(h.wire))
        memcpy(&h.wire[h.wire_len], from, n);
    h.wire_len += n;
}

UINT _nxe_tcp_socket_send(NX_TCP_SOCKET *socket_ptr, NX_PACKET **packet_ptr_ptr,
                          ULONG wait_option)
{
    NX_PACKET *pkt = *packet_ptr_ptr;
    HPacket   *p   = h_from_nx(pkt);
    UINT       status;
    ULONG      take;

    (VOID)socket_ptr;
    h_safe();

    if (h.send_hook != NULL)
    {
        void (*fn)(void) = h.send_hook;

        h.send_hook = NULL;
        fn();
    }

    status = h_plan(h.send_plan, h.send_planned, h.sends);
    take   = (h.sends < H_PLAN) ? h.send_take[h.sends] : 0;
    if (h.sends < H_PLAN)
        h.send_wait[h.sends] = wait_option;
    h.sends++;

    if (h.break_at_send != 0 && h.sends == h.break_at_send)
        h.signals |= H_BREAK_SIG;

    if (status == NX_SUCCESS)
    {
        h_wire(pkt->nx_packet_prepend_ptr, pkt->nx_packet_length);
        if (p != NULL)
            p->in_use = FALSE;          /* NetX Duo owns it now */
        return NX_SUCCESS;
    }

    /* A refusal that still took some: trimmed off the front, as
       _nx_tcp_socket_send_internal() does. */
    if (take > pkt->nx_packet_length)
        take = pkt->nx_packet_length;
    h_wire(pkt->nx_packet_prepend_ptr, take);
    pkt->nx_packet_prepend_ptr += take;
    pkt->nx_packet_length      -= take;

    return status;
}

UINT _nx_tcp_socket_receive(NX_TCP_SOCKET *socket_ptr, NX_PACKET **packet_ptr,
                            ULONG wait_option)
{
    (VOID)socket_ptr; (VOID)packet_ptr; (VOID)wait_option;
    h_safe();
    h.receives++;
    return NX_NO_PACKET;
}

UINT _nxe_tcp_socket_receive(NX_TCP_SOCKET *socket_ptr, NX_PACKET **packet_ptr,
                             ULONG wait_option)
{
    return _nx_tcp_socket_receive(socket_ptr, packet_ptr, wait_option);
}

UINT _nxe_tcp_socket_receive_notify(NX_TCP_SOCKET *socket_ptr,
                                    VOID (*fn)(NX_TCP_SOCKET *socket_ptr))
{
    (VOID)socket_ptr; (VOID)fn;
    return NX_SUCCESS;
}

UINT _nxe_tcp_socket_window_update_notify_set(NX_TCP_SOCKET *socket_ptr,
                                              VOID (*fn)(NX_TCP_SOCKET *socket_ptr))
{
    (VOID)socket_ptr;
    h.window_notify = fn;
    return NX_SUCCESS;
}

UINT _nxe_tcp_socket_establish_notify(NX_TCP_SOCKET *socket_ptr,
                                      VOID (*fn)(NX_TCP_SOCKET *socket_ptr))
{
    (VOID)socket_ptr; (VOID)fn;
    return NX_SUCCESS;
}

UINT _nxe_tcp_socket_disconnect_complete_notify(NX_TCP_SOCKET *socket_ptr,
                                                VOID (*fn)(NX_TCP_SOCKET *socket_ptr))
{
    (VOID)socket_ptr; (VOID)fn;
    return NX_SUCCESS;
}

UINT _nxe_udp_socket_receive_notify(NX_UDP_SOCKET *socket_ptr,
                                    VOID (*fn)(NX_UDP_SOCKET *socket_ptr))
{
    (VOID)socket_ptr; (VOID)fn;
    return NX_SUCCESS;
}

UINT _nxe_udp_socket_icmp_error_notify(NX_UDP_SOCKET *socket_ptr,
                                       UINT (*fn)(NX_UDP_SOCKET *socket_ptr,
                                                  UINT error_code,
                                                  NXD_ADDRESS *peer_address,
                                                  UINT peer_port))
{
    (VOID)socket_ptr; (VOID)fn;
    return NX_SUCCESS;
}

UINT _nxe_tcp_server_socket_accept(NX_TCP_SOCKET *socket_ptr, ULONG wait_option)
{
    (VOID)socket_ptr; (VOID)wait_option;
    return NX_SUCCESS;
}

UINT _nxe_tcp_server_socket_relisten(NX_IP *ip_ptr, UINT port,
                                     NX_TCP_SOCKET *socket_ptr)
{
    (VOID)ip_ptr; (VOID)port; (VOID)socket_ptr;
    return NX_SUCCESS;
}

UINT _nxe_tcp_socket_bytes_available(NX_TCP_SOCKET *socket_ptr,
                                     ULONG *bytes_available)
{
    (VOID)socket_ptr;
    *bytes_available = 0;
    return NX_SUCCESS;
}

/* The UDP half of transfer.c: nothing here sends a datagram. */
UINT _nxe_udp_socket_receive(NX_UDP_SOCKET *socket_ptr, NX_PACKET **packet_ptr,
                             ULONG wait_option)
{
    (VOID)socket_ptr; (VOID)packet_ptr; (VOID)wait_option;
    return NX_NO_PACKET;
}

UINT _nxe_udp_socket_bind(NX_UDP_SOCKET *socket_ptr, UINT port,
                          ULONG wait_option)
{
    (VOID)socket_ptr; (VOID)port; (VOID)wait_option;
    return NX_SUCCESS;
}

UINT _nxe_udp_socket_port_get(NX_UDP_SOCKET *socket_ptr, UINT *port_ptr)
{
    (VOID)socket_ptr;
    *port_ptr = 1024;
    return NX_SUCCESS;
}

UINT _nxde_udp_socket_send(NX_UDP_SOCKET *socket_ptr, NX_PACKET **packet_ptr,
                           NXD_ADDRESS *ip_address, UINT port)
{
    (VOID)socket_ptr; (VOID)packet_ptr; (VOID)ip_address; (VOID)port;
    return NX_SUCCESS;
}

UINT _nxde_udp_socket_source_send(NX_UDP_SOCKET *socket_ptr,
                                  NX_PACKET *packet_ptr,
                                  NXD_ADDRESS *ip_address, UINT port,
                                  UINT address_index)
{
    (VOID)socket_ptr; (VOID)packet_ptr; (VOID)ip_address; (VOID)port;
    (VOID)address_index;
    return NX_SUCCESS;
}

UINT _nxe_udp_socket_source_send(NX_UDP_SOCKET *socket_ptr,
                                 NX_PACKET **packet_ptr, ULONG ip_address,
                                 UINT port, UINT address_index)
{
    (VOID)socket_ptr; (VOID)packet_ptr; (VOID)ip_address; (VOID)port;
    (VOID)address_index;
    return NX_SUCCESS;
}

UINT _nxde_udp_source_extract(NX_PACKET *packet_ptr, NXD_ADDRESS *ip_address,
                              UINT *port)
{
    (VOID)packet_ptr;
    memset(ip_address, 0, sizeof(*ip_address));
    *port = 0;
    return NX_SUCCESS;
}

UINT _nxe_packet_data_extract_offset(NX_PACKET *packet_ptr, ULONG offset,
                                     VOID *buffer_start, ULONG buffer_length,
                                     ULONG *bytes_copied)
{
    (VOID)packet_ptr; (VOID)offset; (VOID)buffer_start; (VOID)buffer_length;
    *bytes_copied = 0;
    return NX_SUCCESS;
}

ULONG _nx_ip_route_find(NX_IP *ip_ptr, ULONG destination_address,
                        NX_INTERFACE **nx_ip_interface, ULONG *next_hop_address)
{
    (VOID)ip_ptr;
    if (nx_ip_interface != NULL)
        *nx_ip_interface = &h_eth;
    if (next_hop_address != NULL)
        *next_hop_address = destination_address;
    return NX_SUCCESS;
}

UINT _nxd_ipv6_interface_find(NX_IP *ip_ptr, ULONG *dest_address,
                              NXD_IPV6_ADDRESS **ipv6_addr, NX_INTERFACE *if_ptr)
{
    (VOID)ip_ptr; (VOID)dest_address; (VOID)ipv6_addr; (VOID)if_ptr;
    return NX_SUCCESS;
}

/* ------------------------------------------------- the socket layer -- */

AmiSocket *bsd_lookup(struct AmiSocketBase *base, LONG fd)
{
    (VOID)base;
    if (fd < 0 || fd >= H_FDS)
        return NULL;
    return h_table[fd];
}

LONG bsd_table_size(struct AmiSocketBase *base) { (VOID)base; return H_FDS; }

LONG bsd_fail(struct AmiSocketBase *base, LONG code)
{
    base->sb_Errno = code;
    return -1;
}

VOID bsd_set_errno(struct AmiSocketBase *base, LONG code)
{
    base->sb_Errno = code;
}

LONG bsd_errno_from_nx(UINT status)
{
    return (status == NX_NO_PACKET) ? AMI_ENOBUFS : AMI_EIO;
}

LONG bsd_wait_errno(ULONG wait, UINT status)
{
    if (status == NX_WINDOW_OVERFLOW || status == NX_TX_QUEUE_DEPTH ||
        status == NX_NO_PACKET)
        return (wait != NX_WAIT_FOREVER) ? AMI_EWOULDBLOCK : AMI_ENOBUFS;
    return bsd_errno_from_nx(status);
}

LONG bsd_nx_enter(struct AmiSocketBase *base)
{
    (VOID)base;
    h.nx_enters++;
    return h.nx_enter_result;
}

VOID bsd_nx_leave(struct AmiSocketBase *base)
{
    (VOID)base;
    h.nx_leaves++;
}

/* socket.c's FIN.  When it goes is the claim: after the last corked byte. */
VOID bsd_tcp_send_fin(AmiSocket *sock)
{
    (VOID)sock;
    h_safe();
    h.fins++;
    h.fin_at = h.wire_len;
}

LONG bsd_oob_send(struct AmiSocketBase *base, AmiSocket *sock, UBYTE byte,
                  LONG flags)
{
    (VOID)base; (VOID)sock; (VOID)flags;
    h.oobs++;
    h.oob_at = h.wire_len;
    h_wire(&byte, 1);
    return 1;
}

BOOL bsd_oob_take(AmiSocket *sock, UBYTE *out)
{
    (VOID)sock; (VOID)out;
    return FALSE;
}

AmiSocket *bsd_incoming_first_ready(const AmiSocket *listener)
{
    (VOID)listener;
    return NULL;
}

VOID  bsd_tcp_window_settle(NX_TCP_SOCKET *tcp, ULONG rtt_ms)
{ (VOID)tcp; (VOID)rtt_ms; }
ULONG ami_millis(VOID) { return 0UL; }

NX_IP *netstack_ip(VOID) { return NX_NULL; }
NX_PACKET_POOL *netstack_pool(VOID) { return &h_pool; }

#ifdef AMINETXDUO_TX_RUN
VOID ami_sana2_tx_run_begin(AmiSana2If *iface) { (VOID)iface; }
VOID ami_sana2_tx_run_flush(AmiSana2If *iface) { (VOID)iface; }
VOID ami_sana2_tx_run_end(AmiSana2If *iface)   { (VOID)iface; }
#endif

BOOL bsd_netmon_have(LONG type) { (VOID)type; return FALSE; }
LONG bsd_netmon_dispatch(LONG type, APTR message)
{ (VOID)type; (VOID)message; return 0; }
STRPTR bsd_netmon_caller(struct AmiSocketBase *base)
{ (VOID)base; return (STRPTR)"host"; }

LONG bsd_cmsg_parse(struct AmiSocketBase *base, AmiSocket *sock,
                    const struct msghdr *msg, BsdCmsgSource *out)
{
    (VOID)base; (VOID)sock; (VOID)msg;
    memset(out, 0, sizeof(*out));
    return 0;
}

VOID bsd_cmsg_build(AmiSocket *sock, NX_PACKET *packet, struct msghdr *msg)
{ (VOID)sock; (VOID)packet; (VOID)msg; }

LONG bsd_cmsg_source_index(NX_IP *ip, const BsdCmsgSource *src, BOOL v6)
{ (VOID)ip; (VOID)src; (VOID)v6; return -1; }

LONG bsd_sockaddr_get(struct AmiSocketBase *base, const struct sockaddr *sa,
                      socklen_t len, NXD_ADDRESS *addr, UINT *port,
                      ULONG *scope_id)
{
    (VOID)base; (VOID)sa; (VOID)len;
    memset(addr, 0, sizeof(*addr));
    addr->nxd_ip_version = NX_IP_VERSION_V4;
    *port = 0;
    *scope_id = 0;
    return 0;
}

VOID bsd_sockaddr_put(const AmiSocket *sock, struct sockaddr *sa,
                      socklen_t *len, const NXD_ADDRESS *addr, UINT port,
                      ULONG scope_id)
{
    (VOID)sock; (VOID)sa; (VOID)len; (VOID)addr; (VOID)port; (VOID)scope_id;
}

VOID bsd_addr_from_v4(NXD_ADDRESS *addr, ULONG v4)
{
    memset(addr, 0, sizeof(*addr));
    addr->nxd_ip_version    = NX_IP_VERSION_V4;
    addr->nxd_ip_address.v4 = v4;
}

BOOL bsd_addr_normalise(const AmiSocket *sock, NXD_ADDRESS *addr)
{ (VOID)sock; (VOID)addr; return TRUE; }

VOID bsd_in6_to_words(const UBYTE bytes[16], ULONG words[4])
{ memcpy(words, bytes, 16); }

BOOL bsd_bind_wants_interface(const AmiSocket *sock, const NX_INTERFACE *nxif)
{ (VOID)sock; (VOID)nxif; return TRUE; }

NX_PACKET *bsd_raw_receive(AmiSocket *sock, ULONG wait, UINT *why)
{
    (VOID)sock; (VOID)wait;
    if (why != NULL)
        *why = NX_NO_PACKET;
    return NX_NULL;
}

LONG bsd_raw_send_packet(struct AmiSocketBase *base, AmiSocket *sock,
                         NX_PACKET *packet, const NXD_ADDRESS *addr,
                         ULONG scope, const BsdCmsgSource *src)
{
    (VOID)base; (VOID)sock; (VOID)packet; (VOID)addr; (VOID)scope; (VOID)src;
    return 0;
}

VOID bsd_raw_source(NX_PACKET *packet, NXD_ADDRESS *addr)
{ (VOID)packet; (VOID)addr; }

BsdSourceKind bsd_source_select(const AmiSocket *sock, const NXD_ADDRESS *dest,
                                ULONG scope, UINT *index)
{
    (VOID)sock; (VOID)dest; (VOID)scope;
    if (index != NULL)
        *index = 0;
    return BSD_SOURCE_ROUTE;
}

UINT bsd_udp_queue_info(const NX_PACKET *packet, UINT *source_port,
                        ULONG *payload_length)
{
    (VOID)packet;
    if (source_port != NULL)
        *source_port = 0;
    if (payload_length != NULL)
        *payload_length = 0;
    return NX_SUCCESS;
}

UINT anx6_scope(const ULONG *addr) { (VOID)addr; return 0; }

LONG bsd_mcast_prepare_send(AmiSocket *sock, const NXD_ADDRESS *addr)
{ (VOID)sock; (VOID)addr; return 0; }

LONG bsd_mcast6_prepare_send(struct AmiSocketBase *base, AmiSocket *sock,
                             const NXD_ADDRESS *addr, ULONG *saved)
{ (VOID)base; (VOID)sock; (VOID)addr; *saved = 0UL; return 0; }

VOID bsd_mcast6_finish_send(struct AmiSocketBase *base, ULONG saved)
{ (VOID)base; (VOID)saved; }

/* ---------------------------------------------------------------- tests -- */

static void t_setget(void)
{
    AmiSocket *s;

    printf("cork: TCP_NODELAY 0 and 1\n");

    h_reset();
    CHECK(h.timer_creates == 1 && h_ip.nx_ip_cork_handler == bsd_cork_ip_pass,
          "the stack's start makes the timer and installs the pass");

    s = h_tcp(0);
    CHECK((s->as_CorkFlags & BSD_CORKF_ON) != 0, "on");
    CHECK(bsd_cork_set(&h_base, s, FALSE) == 0 &&
          (s->as_CorkFlags & BSD_CORKF_ON) == 0, "and off again");

    /* No stack, no timer: 0 is refused rather than corking with no tick. */
    bsd_cork_stop();
    CHECK(h_ip.nx_ip_cork_handler == NX_NULL && h.deletes == 1,
          "the stop deletes the timer and clears the handler");
    CHECK(bsd_cork_set(&h_base, s, TRUE) == AMI_ENOBUFS,
          "with the cork not running, 0 is ENOBUFS");
}

static void t_sub_mss(void)
{
    AmiSocket *s;
    ULONG      enters;

    printf("cork: a write shorter than a segment is held\n");

    h_reset();
    s = h_tcp(0);

    CHECK(h_send(0, 0, 10, 0) == 10, "ten bytes are credited");
    CHECK(h.sends == 0, "and nothing is sent");
    CHECK(h_pending(s) == 10 && h_linked(s), "they are pending and armed");
    CHECK(h.activates == 1 && h.timer_live, "the tick is started");

    enters = h.nx_enters;
    CHECK(h_send(0, 10, 20, 0) == 20, "a second write is credited");
    CHECK(h.sends == 0 && h_pending(s) == 30, "and joins the first");
    CHECK(h.activates == 1, "the tick is started once, not per write");
#ifdef AMINETXDUO_TCP_CORK_FASTPATH
    CHECK(h.nx_enters == enters,
          "the second write took the fast path: no bracket at all");
#else
    CHECK(h.nx_enters == enters + 1, "the second write took the bracket");
#endif
    CHECK(h.unsafe == 0, "no NetX Duo call and no copy inside Forbid()");
}

static void t_tick(void)
{
    AmiSocket *s;

    printf("cork: the tick sets the event and the pass sends\n");

    h_reset();
    s = h_tcp(0);
    (VOID)h_send(0, 0, 30, 0);

    bsd_cork_tick((ULONG)&h_ip);
    CHECK((h.events & NX_IP_CORK_EVENT) != 0, "T sets NX_IP_CORK_EVENT");
    CHECK(h.sends == 0, "and does nothing else");

    h_pass();
    CHECK(h.sends == 1 && h.send_wait[0] == NX_NO_WAIT,
          "the pass sends once, without waiting");
    CHECK(h_wire_is(0, 30), "the thirty bytes, in order");
    CHECK(s->as_CorkPkt == NULL && !h_linked(s), "nothing is left pending");
    CHECK(!h.timer_live && h.deactivates == 1,
          "and the tick stops with nothing left to look at");
    CHECK(s->as_CorkState == BSD_CORK_IDLE, "the socket is IDLE again");
}

static void t_mss_fill(void)
{
    AmiSocket *s;

    printf("cork: a full segment goes at once\n");

    h_reset();
    s = h_tcp(0);

    CHECK(h_send(0, 0, 60, 0) == 60, "sixty");
    CHECK(h_send(0, 60, 40, 0) == 40, "forty more fill the segment");
    CHECK(h.sends == 1 && h_wire_is(0, 100),
          "and the hundred go as one segment, now");
    CHECK(s->as_CorkPkt == NULL, "nothing pending");
}

static void t_larger_than_room(void)
{
    printf("cork: a write that does not fit flushes first\n");

    h_reset();
    (VOID)h_tcp(0);

    (VOID)h_send(0, 0, 60, 0);
    CHECK(h_send(0, 60, 70, 0) == 70, "seventy are credited");
    CHECK(h.sends == 2, "two segments: the pending one, then the write");
    CHECK(h_wire_is(0, 130), "the pending sixty first: the order is kept");

    /* A segment's worth with nothing pending is never held. */
    h_reset();
    (VOID)h_tcp(0);
    CHECK(h_send(0, 0, 150, 0) == 150 && h.sends == 2 && h_wire_is(0, 150),
          "a write of a segment or more takes the uncorked path");
}

static void t_window_stall(void)
{
    AmiSocket *s;

    printf("cork: a window that takes part of it\n");

    h_reset();
    s = h_tcp(0);
    (VOID)h_send(0, 0, 30, 0);

    h.send_plan[0] = NX_WINDOW_OVERFLOW;
    h.send_take[0] = 10;
    h.send_plan[1] = NX_SUCCESS;
    h.send_planned = 2;

    h_pass();
    CHECK(h_wire_is(0, 10), "ten went");
    CHECK(h_pending(s) == 20, "twenty are held, trimmed by the send");
    CHECK((s->as_CorkFlags & BSD_CORKF_STALLED) != 0 && h_linked(s),
          "STALLED and still armed");
    CHECK(!h.timer_live, "the tick stops: the window notify wakes it");

    h.event_sets = 0;
    h.window_notify(&s->as_Nx.tcp);
    CHECK(h.sends == 1, "the notify never sends");
    CHECK((s->as_CorkFlags & BSD_CORKF_STALLED) == 0 &&
          (h.events & NX_IP_CORK_EVENT) != 0,
          "it clears STALLED and sets the event");

    h_pass();
    CHECK(h.sends == 2 && h_wire_is(0, 30),
          "the pass sends the rest, and every byte went once");
    CHECK(s->as_CorkPkt == NULL && !h_linked(s), "nothing pending");
}

static void t_no_packet(void)
{
    AmiSocket *s;

    printf("cork: no packet for the send\n");

    h_reset();
    s = h_tcp(0);
    (VOID)h_send(0, 0, 30, 0);

    h.send_plan[0] = NX_NO_PACKET;
    h.send_planned = 1;

    h_pass();
    CHECK(h_pending(s) == 30 && h_linked(s) &&
          (s->as_CorkFlags & BSD_CORKF_TICK) != 0,
          "NX_NO_PACKET keeps it, on the tick");
    CHECK(h.timer_live && h.deactivates == 0, "the tick keeps running");
}

static void t_drop_statuses(void)
{
    AmiSocket *s;

    printf("cork: a connection gone, and a send refused\n");

    h_reset();
    s = h_tcp(0);
    (VOID)h_send(0, 0, 30, 0);
    h.send_plan[0] = NX_NOT_CONNECTED;
    h.send_planned = 1;

    h_pass();
    CHECK(s->as_CorkPkt == NULL && h.releases == 1, "NOT_CONNECTED releases it");
    CHECK(s->as_SoError == 0 && (s->as_Events & FD_ERROR) == 0,
          "silently: the close is reported elsewhere");

    h_reset();
    s = h_tcp(0);
    (VOID)h_send(0, 0, 30, 0);
    h.send_plan[0] = NX_INVALID_PACKET;
    h.send_planned = 1;

    h_pass();
    CHECK(s->as_CorkPkt == NULL && h.releases == 1, "another refusal releases");
    CHECK(s->as_SoError == AMI_EIO && (s->as_Events & FD_ERROR) != 0,
          "and becomes SO_ERROR with FD_ERROR");
    CHECK(h_send(0, 0, 5, 0) == -1 && h_base.sb_Errno == AMI_EIO &&
          s->as_SoError == 0,
          "the next send reports it, once");

    /* A reader is told the same way. */
    h_reset();
    s = h_tcp(0);
    (VOID)h_send(0, 0, 30, 0);
    h.send_plan[0] = NX_INVALID_PACKET;
    h.send_planned = 1;
    h_pass();
    {
        UBYTE buf[4];

        CHECK(bsd_recv(0, buf, sizeof(buf), MSG_DONTWAIT, &h_base) == -1 &&
              h_base.sb_Errno == AMI_EIO && s->as_SoError == 0,
              "and so does the next recv, once");
        CHECK(h.receives == 0, "before asking NetX Duo for data");
    }
}

static void t_pass_during_append(void)
{
    AmiSocket *s;

    printf("cork: the pass meets a segment being appended to\n");

    h_reset();
    s = h_tcp(0);
    (VOID)h_send(0, 0, 10, 0);

#ifdef AMINETXDUO_TCP_CORK_FASTPATH
    /* F copies; I runs in the middle of the copy. */
    h.copy_runs_pass = TRUE;
    CHECK(h_send(0, 10, 20, 0) == 20, "the fast path takes the write");
    CHECK(h.sends == 0, "the pass that ran mid-copy skipped the segment");
#else
    s->as_CorkState = BSD_CORK_APPEND;
    h_pass();
    CHECK(h.sends == 0, "the pass skips a segment in APPEND");
    s->as_CorkState = BSD_CORK_IDLE;
    (VOID)h_send(0, 10, 20, 0);
#endif
    CHECK(h_linked(s) && (s->as_CorkFlags & BSD_CORKF_TICK) != 0 &&
          h.timer_live, "and left it armed for the next tick");

    h_pass();
    CHECK(h.sends == 1 && h_wire_is(0, 30),
          "the next pass sends all of it, in order");
}

static void t_handoff_and_fast_decline(void)
{
    AmiSocket *s;
    ULONG      enters;

    printf("cork: a segment another task holds\n");

    h_reset();
    s = h_tcp(0);
    (VOID)h_send(0, 0, 10, 0);

    /* B has it detached for a send of its own. */
    s->as_CorkState = BSD_CORK_FLUSH;
    s->as_CorkOwner = &h_other;

    enters = h.nx_enters;
    CHECK(h_send(0, 10, 5, MSG_DONTWAIT) == -1 &&
          h_base.sb_Errno == AMI_EAGAIN && s->as_TxWait == 1,
          "a non-blocking write is EAGAIN and asks for FD_WRITE");
    CHECK(h.nx_enters == enters + 1,
          "the fast path declined a FLUSH segment and took the bracket");

    h_pass();
    CHECK(h.sends == 0, "the pass leaves B's segment alone");

    /* A blocking write waits for B, a tick at a time. */
    h.sleep_finishes = s;
    CHECK(h_send(0, 10, 5, 0) == 5 && h.sleeps == 1,
          "a blocking one waits B out, then appends");
    CHECK(h_pending(s) == 15, "behind what B left");
}

static void t_blocking_break_and_timeout(void)
{
    AmiSocket *s;

    printf("cork: a blocking write that has to wait\n");

    /* The break arrives while the pending segment waits on the window. */
    h_reset();
    s = h_tcp(0);
    (VOID)h_send(0, 0, 60, 0);

    h.send_plan[0]   = NX_WINDOW_OVERFLOW;
    h.send_take[0]   = 20;
    h.send_planned   = 1;
    h.break_at_send  = 1;

    CHECK(h_send(0, 60, 70, 0) == -1 && h_base.sb_Errno == AMI_EINTR,
          "EINTR: none of this call's bytes were taken");
    CHECK(h_pending(s) == 40 && (s->as_CorkFlags & BSD_CORKF_STALLED) != 0,
          "the remainder is reattached, STALLED");
    CHECK(h_wire_is(0, 20), "the twenty that went are not sent again");

    h.signals = 0;
    h.send_planned = 0;
    s->as_CorkFlags &= (UBYTE)~BSD_CORKF_STALLED;
    h_pass();
    CHECK(h_wire_is(0, 60), "the rest goes later: each byte exactly once");

    /* SO_SNDTIMEO runs out. */
    h_reset();
    s = h_tcp(0);
    (VOID)h_send(0, 0, 60, 0);
    s->as_SndTimeout = 20;
    h.send_plan[0]   = NX_WINDOW_OVERFLOW;
    h.send_planned   = 1;

    CHECK(h_send(0, 60, 70, 0) == -1 && h_base.sb_Errno == AMI_EWOULDBLOCK,
          "SO_SNDTIMEO: EAGAIN with nothing taken");
    CHECK(h_pending(s) == 60 && h.wire_len == 0, "and nothing lost");
    CHECK(h.sends >= 2, "the wait was sliced, as an uncorked send's is");
}

static void t_nonblocking(void)
{
    AmiSocket *s;

    printf("cork: a non-blocking write against a closed window\n");

    h_reset();
    s = h_tcp(0);
    s->as_Flags |= ASF_NONBLOCK;
    (VOID)h_send(0, 0, 60, 0);

    h.send_plan[0] = NX_WINDOW_OVERFLOW;
    h.send_planned = 1;

    CHECK(h_send(0, 60, 70, 0) == 40,
          "what fits in the segment's room is taken");
    CHECK(s->as_TxWait == 1 && (s->as_CorkFlags & BSD_CORKF_STALLED) != 0,
          "as_TxWait and STALLED are set");
    CHECK(h_pending(s) == 100 && h.send_wait[0] == NX_NO_WAIT,
          "the segment is full, and the flush did not wait");

    CHECK(h_send(0, 100, 10, 0) == -1 && h_base.sb_Errno == AMI_EWOULDBLOCK,
          "with no room left it is EAGAIN");
    CHECK(h.wire_len == 0, "and nothing went");

    /* bsd_writable(): a segment with room takes the next write; a full one
       needs the queue to take it first. */
    s->as_Nx.tcp.nx_tcp_socket_transmit_sent_count = 8;
    CHECK(bsd_writable(s) == FALSE && s->as_TxWait == 1,
          "a full segment and a full queue: not writable");
    s->as_Nx.tcp.nx_tcp_socket_transmit_sent_count = 0;
    s->as_Nx.tcp.nx_tcp_socket_transmit_queue_maximum = 8;
    CHECK(bsd_writable(s) == TRUE, "room in the queue for it: writable");
    s->as_Nx.tcp.nx_tcp_socket_transmit_sent_count = 7;
    CHECK(bsd_writable(s) == FALSE, "sent_count + 1 == max: not writable");
}

static void t_oob(void)
{
    printf("cork: MSG_OOB\n");

    h_reset();
    (VOID)h_tcp(0);
    (VOID)h_send(0, 0, 30, 0);

    CHECK(h_send(0, 30, 5, MSG_OOB) == 5, "five, the last one urgent");
    CHECK(h.oobs == 1 && h.oob_at == 34,
          "the corked thirty, then four, then the urgent byte");
    CHECK(h_wire_is(0, 35), "every byte in order");

    /* A single urgent byte flushes as well. */
    h_reset();
    (VOID)h_tcp(0);
    (VOID)h_send(0, 0, 30, 0);
    CHECK(h_send(0, 30, 1, MSG_OOB) == 1 && h.oob_at == 30,
          "a lone urgent byte waits for the corked thirty");
}

static void t_shut_write(void)
{
    AmiSocket *s;

    printf("cork: shutdown(SHUT_WR) against a closed window\n");

    h_reset();
    s = h_tcp(0);
    (VOID)h_send(0, 0, 30, 0);

    h.send_plan[0] = NX_WINDOW_OVERFLOW;
    h.send_planned = 1;
    CHECK(bsd_cork_shut_write(s) == TRUE, "the FIN is deferred");
    CHECK(h.fins == 0 && (s->as_CorkFlags & BSD_CORKF_FIN) != 0,
          "not sent, flagged behind the segment");
    CHECK((s->as_CorkFlags & BSD_CORKF_TICK) != 0 && h.timer_live,
          "and on the tick, not only the window notify");

    h.send_planned = 0;
    h.window_notify(&s->as_Nx.tcp);
    h_pass();
    CHECK(h_wire_is(0, 30) && h.fins == 1 && h.fin_at == 30,
          "the data first, then the FIN");

    /* Nothing pending: the caller sends it itself. */
    h_reset();
    s = h_tcp(0);
    CHECK(bsd_cork_shut_write(s) == FALSE, "nothing held, nothing deferred");
}

static void t_close_graceful(void)
{
    AmiSocket *s;

    printf("cork: a graceful close with data held\n");

    h_reset();
    s = h_tcp(0);
    (VOID)h_send(0, 0, 30, 0);

    h.send_plan[0] = NX_WINDOW_OVERFLOW;
    h.send_planned = 1;
    CHECK(bsd_cork_close_graceful(s) == TRUE, "the FIN is deferred");
    CHECK((s->as_CorkFlags & BSD_CORKF_TICK) != 0 && h.timer_live,
          "a socket about to be parked keeps the tick");

    /* socket.c's bsd_closing_park(). */
    s->as_Flags |= ASF_CLOSING;
    s->as_Nx.tcp.nx_tcp_socket_reserved_ptr = NX_NULL;
    s->as_Owner = NULL;

    h.window_notify(&s->as_Nx.tcp);
    CHECK((h.events & NX_IP_CORK_EVENT) == 0,
          "a parked socket's notify cannot find it");

    h.send_plan[0] = NX_WINDOW_OVERFLOW;
    h.send_plan[1] = NX_SUCCESS;
    h.send_planned = 2;
    h.sends        = 0;
    h_pass();
    CHECK(h_linked(s) && (s->as_CorkFlags & BSD_CORKF_TICK) != 0,
          "still refused: a parked socket stays on the tick, not STALLED");
    h_pass();
    CHECK(h_wire_is(0, 30) && h.fins == 1 && h.fin_at == 30,
          "the tick sends the data, then the FIN");

    /* The sweep's deadline: dropped and unlinked. */
    h_reset();
    s = h_tcp(0);
    (VOID)h_send(0, 0, 30, 0);
    h.send_plan[0] = NX_WINDOW_OVERFLOW;
    h.send_planned = 1;
    (VOID)bsd_cork_close_graceful(s);
    s->as_Flags |= ASF_CLOSING;
    bsd_cork_drop(s);
    CHECK(s->as_CorkPkt == NULL && !h_linked(s) && h.releases == 1,
          "the deadline drops the segment and unlinks the socket");
    h_pass();
    CHECK(h.fins == 0 && h.sends == 1, "no pass reaches it afterwards");
    CHECK(!h.timer_live, "and the tick stops");
}

static void t_linger_and_abort(void)
{
    AmiSocket *s;
    ULONG      left = 0;

    printf("cork: SO_LINGER and the abortive close\n");

    /* linger 0 / unread data: the drop. */
    h_reset();
    s = h_tcp(0);
    (VOID)h_send(0, 0, 30, 0);
    bsd_cork_drop(s);
    CHECK(s->as_CorkPkt == NULL && h.releases == 1 && h.wire_len == 0,
          "an abortive close releases what is held unsent");

    /* A timed linger that runs out. */
    h_reset();
    s = h_tcp(0);
    (VOID)h_send(0, 0, 30, 0);
    h.send_plan[0] = NX_WINDOW_OVERFLOW;
    h.send_planned = 1;
    CHECK(bsd_cork_close_linger(s, 100, &left) == FALSE && left == 0,
          "the linger expires with data held: the caller aborts");
    CHECK(h.send_wait[0] == 100, "the send was given the linger time");
    CHECK(s->as_CorkPkt == NULL && h.releases == 1, "and it is released");

    /* One that does not. */
    h_reset();
    s = h_tcp(0);
    (VOID)h_send(0, 0, 30, 0);
    CHECK(bsd_cork_close_linger(s, 100, &left) == TRUE && left == 100,
          "sent in time: the disconnect gets the rest of the linger");
    CHECK(h_wire_is(0, 30), "and the data went");
}

static void t_recv(void)
{
    AmiSocket *s;
    UBYTE      buf[8];

    printf("cork: recv() flushes only when it is about to wait\n");

    h_reset();
    s = h_tcp(0);
    (VOID)h_send(0, 0, 30, 0);

    (VOID)bsd_recv(0, buf, sizeof(buf), MSG_DONTWAIT, &h_base);
    CHECK(h.sends == 0, "a non-blocking recv does not flush");

    s->as_Nx.tcp.nx_tcp_socket_receive_queue_count = 1;
    s->as_RcvTimeout = 20;
    (VOID)bsd_recv(0, buf, sizeof(buf), 0, &h_base);
    CHECK(h.sends == 0, "nor does one with data queued to read");

    s->as_Nx.tcp.nx_tcp_socket_receive_queue_count = 0;
    (VOID)bsd_recv(0, buf, sizeof(buf), 0, &h_base);
    CHECK(h.sends == 1 && h_wire_is(0, 30),
          "one about to wait for the peer sends what is held first");
}

static void t_select(void)
{
    AmiSocket     *s;
    ULONG          rd[BSD_FD_WORDS];
    struct timeval poll = { 0, 0 };

    printf("cork: WaitSelect()\n");

    h_reset();
    s = h_tcp(0);
    (VOID)h_send(0, 0, 30, 0);

    s->as_Nx.tcp.nx_tcp_socket_receive_queue_count = 1;
    memset(rd, 0, sizeof(rd));
    rd[0] = 1;
    CHECK(bsd_WaitSelect(1, rd, NULL, NULL, &poll, NULL, &h_base) == 1,
          "readable");
    CHECK(h.sends == 0, "a readable socket is not flushed");

    s->as_Nx.tcp.nx_tcp_socket_receive_queue_count = 0;
    memset(rd, 0, sizeof(rd));
    rd[0] = 1;
    CHECK(bsd_WaitSelect(1, rd, NULL, NULL, &poll, NULL, &h_base) == 0,
          "not readable");
    CHECK(h.sends == 1 && h_wire_is(0, 30),
          "waiting to read with a write held sends it");

    /* Writable with a segment that has room. */
    (VOID)h_send(0, 30, 10, 0);
    CHECK(bsd_writable(s) == TRUE && s->as_TxWait == 0,
          "a pending segment with room is writable");
}

static void t_disconnect(void)
{
    AmiSocket *s;

    printf("cork: the connection goes away under it\n");

    h_reset();
    s = h_tcp(0);
    (VOID)h_send(0, 0, 30, 0);
    h.send_plan[0] = NX_WINDOW_OVERFLOW;
    h.send_planned = 1;
    h_pass();
    CHECK((s->as_CorkFlags & BSD_CORKF_STALLED) != 0, "stalled");

    s->as_Nx.tcp.nx_tcp_socket_state = NX_TCP_CLOSED;
    h.events = 0;
    bsd_tcp_disconnect_callback(&s->as_Nx.tcp);
    CHECK((h.events & NX_IP_CORK_EVENT) != 0 &&
          (s->as_CorkFlags & BSD_CORKF_STALLED) == 0,
          "the disconnect wakes the pass");

    h_pass();
    CHECK(s->as_CorkPkt == NULL && h.releases == 1 && s->as_SoError == 0,
          "which drops the segment, silently");
    CHECK(h.sends == 1, "without asking NetX Duo to send it");

    /* The interface going away takes it too. */
    h_reset();
    s = h_tcp(0);
    (VOID)h_send(0, 0, 30, 0);
    h_eth.nx_interface_additional_link_info = NX_NULL;
    h_pass();
    CHECK(s->as_CorkPkt == NULL && h.sends == 0 && h.releases == 1,
          "an interface gone: dropped, not sent");
}

static void t_teardown(void)
{
    AmiSocket *s, *t;

    printf("cork: destroy, CloseLibrary and the stack going down\n");

    h_reset();
    s = h_tcp(0);
    t = h_tcp(1);
    (VOID)h_send(0, 0, 30, 0);
    (VOID)h_send(1, 30, 30, 0);

    bsd_cork_drop(s);
    CHECK(!h_linked(s) && h_linked(t), "destroy unlinks its own socket only");
    bsd_cork_drop(t);
    h_pass();
    CHECK(!h.timer_live && h.sends == 0,
          "with the list empty the next pass stops the tick");

    /* The stack going down with a segment still armed. */
    h_reset();
    s = h_tcp(0);
    (VOID)h_send(0, 0, 30, 0);
    bsd_cork_stop();
    CHECK(h_ip.nx_ip_cork_handler == NX_NULL && h.deletes == 1 &&
          !h.timer_live, "the timer goes and the handler is cleared");
    CHECK(s->as_CorkPkt == NULL && !h_linked(s) && h.releases == 1,
          "and what was armed is released while the pool still exists");
    CHECK(h.mutex_gets == 1 && h.mutex_puts == 1,
          "under nx_ip_protection, so no pass is running meanwhile");

    /* A later destroy of that socket finds nothing. */
    bsd_cork_drop(s);
    CHECK(h.releases == 1, "and a drop afterwards is a no-op");
}

static void t_stop_refused(void)
{
    AmiSocket *s;

    printf("cork: the stack going down when the bracket is refused\n");

    /* The kernel still running but the bracket refused (adoption failed):
       the timer goes anyway, before the NX_IP can be reclaimed. */
    h_reset();
    s = h_tcp(0);
    (VOID)h_send(0, 0, 30, 0);
    CHECK(h.timer_live, "a live tick");
    h.enter_result = AMI_NET_ERR_KERNEL;
    CHECK(bsd_cork_stop() == FALSE, "and says the stack must be kept");
    CHECK(!h.timer_live && h.deactivates == 1 && h.deletes == 1,
          "refused bracket, kernel live: the timer is deactivated and deleted");
    CHECK(h_ip.nx_ip_cork_handler == NX_NULL, "and the handler cleared");
    CHECK(h.mutex_gets == 0 && h.releases == 0,
          "with no NetX Duo call made outside a bracket");
    CHECK(s->as_CorkPkt == NULL && !h_linked(s),
          "the segment is forgotten with its pool, and the socket unlinked");
    bsd_cork_drop(s);
    CHECK(h.releases == 0, "so a later drop releases nothing into a dead pool");
    CHECK(bsd_cork_set(&h_base, s, TRUE) == AMI_ENOBUFS, "and the cork is off");

    /* The kernel already stopped: nothing can fire, and there is no timer
       list to take it off. */
    h_reset();
    s = h_tcp(0);
    (VOID)h_send(0, 0, 30, 0);
    h.enter_result = AMI_NET_ERR_STATE;
    bsd_cork_stop();
    CHECK(h.deletes == 0 && h.mutex_gets == 0,
          "kernel stopped: no ThreadX call at all");
    CHECK(h_ip.nx_ip_cork_handler == NX_NULL && !h_linked(s),
          "the handler cleared and the list emptied all the same");

    /* A start the bracket refuses leaves no timer and no handler. */
    h_reset();
    bsd_cork_stop();
    h.timer_creates = 0;
    h.enter_result  = AMI_NET_ERR_KERNEL;
    bsd_cork_start(&h_ip);
    CHECK(h.timer_creates == 0 && h_ip.nx_ip_cork_handler == NX_NULL &&
          !bsd_cork_running(), "a refused start: no timer, no handler");
    h.enter_result = AMI_NET_OK;
    CHECK(h.enters == h.leaves + 1, "and a refused bracket is not left");
}

static void t_stop_timer_refusals(void)
{
    printf("cork: a timer that will not go\n");

    /* tx_timer_delete() refuses (TX_CALLER_ERROR): the timer stays ours,
       deactivated, and T fires into nothing. */
    h_reset();
    (VOID)h_tcp(0);
    (VOID)h_send(0, 0, 30, 0);
    h.delete_result = TX_CALLER_ERROR;
    bsd_cork_stop();
    CHECK(h.deletes == 1 && !h.timer_live, "deactivated, the delete refused");
    CHECK(h_ip.nx_ip_cork_handler == NX_NULL, "the handler cleared");
    h.events = 0;
    bsd_cork_tick((ULONG)&h_ip);
    CHECK(h.events == 0, "a tick after the stop reaches no NX_IP");

    h.delete_result = TX_SUCCESS;
    h.timer_creates = 0;
    bsd_cork_start(&h_ip);
    CHECK(h.timer_creates == 0 && bsd_cork_running(),
          "the next start reuses the timer it still owns");
    bsd_cork_tick((ULONG)&h_ip);
    CHECK((h.events & NX_IP_CORK_EVENT) != 0, "which ticks again");
    bsd_cork_stop();
    CHECK(h.deletes == 2, "and the next stop deletes it");

    /* tx_timer_deactivate() refuses: not deleted while it may run, and cut
       off all the same. */
    h_reset();
    (VOID)h_tcp(0);
    (VOID)h_send(0, 0, 30, 0);
    h.deactivate_result = TX_CALLER_ERROR;
    bsd_cork_stop();
    CHECK(h.deletes == 0, "a timer that would not stop is not deleted");
    h.events = 0;
    bsd_cork_tick((ULONG)&h_ip);
    CHECK(h.events == 0, "and its ticks reach nothing");

    /* Still ours and still running: the next start ticks on it, and the next
       stop that can retires it. */
    h.deactivate_result = TX_SUCCESS;
    h.timer_creates     = 0;
    bsd_cork_start(&h_ip);
    CHECK(h.timer_creates == 0, "the next start takes it back");
    bsd_cork_stop();
    CHECK(h.deletes == 1 && !h.timer_live, "and the next stop deletes it");
}

/* Inside the pass's own send: the stack goes down with the bracket refused,
   then a close drops the socket the pass is sending. */
static AmiSocket *h_mid;
static BOOL       h_mid_drop_waited;

static void h_stop_mid_pass(void)
{
    ULONG releases = h.releases;

    h.enter_result = AMI_NET_ERR_KERNEL;
    CHECK(bsd_cork_stop() == FALSE,
          "refused while the kernel runs: the stack must be kept");
    CHECK(h.delays == 0, "and the stop does not wait for anything");
    CHECK(h_ip.nx_ip_cork_handler == NX_NULL && h.deletes == 1,
          "handler cleared and timer deleted before anything is reclaimed");
    CHECK(h_mid->as_CorkState == BSD_CORK_FLUSH &&
          h_mid->as_CorkOwner == BSD_CORK_OWNER_IP,
          "the socket the pass is sending stays the pass's");
    CHECK(h.releases == releases, "and no packet was released under it");

    if (setjmp(h_sleep_jmp) == 0)
    {
        h.sleep_jumps = TRUE;
        bsd_cork_drop(h_mid);
        CHECK(FALSE, "a drop took a segment the pass still holds");
    }
    h_mid_drop_waited = TRUE;
    CHECK(h_mid->as_CorkState == BSD_CORK_FLUSH && h.releases == releases,
          "a drop waits for the pass instead of stealing its FLUSH");
}

static void t_stop_during_pass(void)
{
    AmiSocket *s;
    VOID     (*latched)(NX_IP *);
    ULONG      gets;

    printf("cork: the stack going down while a pass is in its send\n");

    h_reset();
    h_mid = h_tcp(0);
    (VOID)h_send(0, 0, 30, 0);
    h_mid_drop_waited = FALSE;
    h.send_hook = h_stop_mid_pass;
    h_pass();
    CHECK(h_mid_drop_waited, "the stop and the drop ran inside the send");
    CHECK(h_mid->as_CorkState == BSD_CORK_IDLE && h_mid->as_CorkPkt == NULL &&
          h_wire_is(0, 30), "the pass finished its send and handed it back");
    CHECK(!h.timer_live && h.activates == 1,
          "and did not start a stopped cork's timer again");
    bsd_cork_drop(h_mid);
    CHECK(h.releases == 0, "the drop afterwards has nothing left to release");

    printf("cork: the IP thread has read the handler but not yet run it\n");

    /* No pass running, none counted: the IP thread has only latched the
       handler (nx_ip_thread_entry.c reads it, then calls it).  A refused stop
       cannot see that, so it refuses the teardown anyway. */
    h_reset();
    s = h_tcp(0);
    (VOID)h_send(0, 0, 30, 0);
    latched = h_ip.nx_ip_cork_handler;
    h.enter_result = AMI_NET_ERR_KERNEL;
    CHECK(bsd_cork_stop() == FALSE,
          "refused with nothing visibly running: still FALSE, stack kept");
    CHECK(s->as_CorkPkt == NULL && h.releases == 0,
          "the IDLE segment is forgotten, never released without a bracket");

    /* The latched call lands after the stop: it touches nothing. */
    latched(&h_ip);
    CHECK(h.sends == 0 && s->as_CorkState == BSD_CORK_IDLE,
          "the late call finds the cork stopped and returns");

    /* The kept stack's next shutdown: still refused -- still FALSE. */
    CHECK(bsd_cork_stop() == FALSE,
          "a later stop without the bracket cannot prove it either");

    /* With the bracket, it takes nx_ip_protection on that NX_IP first. */
    h.enter_result = AMI_NET_OK;
    gets = h.mutex_gets;
    CHECK(bsd_cork_stop() == TRUE && h.mutex_gets == gets + 1,
          "a stop with the bracket proves it on the mutex, then says TRUE");
    CHECK(bsd_cork_stop() == TRUE && h.mutex_gets == gets + 1,
          "and owes nothing after that");

    /* A reopen on the kept stack hands the proof to its own stop. */
    h_reset();
    (VOID)h_tcp(0);
    h.enter_result = AMI_NET_ERR_KERNEL;
    CHECK(bsd_cork_stop() == FALSE, "refused");
    h.enter_result = AMI_NET_OK;
    bsd_cork_start(&h_ip);
    gets = h.mutex_gets;
    CHECK(bsd_cork_stop() == TRUE && h.mutex_gets == gets + 1,
          "the reopened cork's stop takes the mutex, and that is the proof");

    /* The kernel stopped: no pass can run, TRUE without a bracket. */
    h_reset();
    (VOID)h_tcp(0);
    h.enter_result = AMI_NET_ERR_STATE;
    CHECK(bsd_cork_stop() == TRUE, "no kernel, no pass: TRUE");
    h.enter_result = AMI_NET_OK;
    h_reset();
    CHECK(bsd_cork_stop() == TRUE, "and with the bracket held, TRUE");
}

static void t_abort_while_owned(void)
{
    AmiSocket *s;

    printf("cork: an abort while somebody holds the segment\n");

    /* F is copying: the drop waits a tick for it. */
    h_reset();
    s = h_tcp(0);
    (VOID)h_send(0, 0, 30, 0);
    s->as_CorkState  = BSD_CORK_APPEND;
    h.sleep_finishes = s;
    bsd_cork_drop(s);
    CHECK(h.sleeps == 1 && s->as_CorkPkt == NULL && h.releases == 1,
          "APPEND: waited out, then released");

    /* I has it in FLUSH: the barrier. */
    h_reset();
    s = h_tcp(0);
    (VOID)h_send(0, 0, 30, 0);
    s->as_CorkState  = BSD_CORK_FLUSH;
    s->as_CorkOwner  = BSD_CORK_OWNER_IP;
    h.mutex_finishes = s;
    bsd_cork_drop(s);
    CHECK(h.mutex_gets == 1 && h.mutex_puts == 1 && h.sleeps == 0,
          "FLUSH(IP): the mutex barrier, not a sleep");
    CHECK(s->as_CorkPkt == NULL && h.releases == 1, "then released");
}

static void t_claim_vs_pass(void)
{
    AmiSocket *s;

    printf("cork: a write that finds the IP thread's pass on the segment\n");

    /* Non-blocking: EAGAIN at once, no wait on the pass at all. */
    h_reset();
    s = h_tcp(0);
    (VOID)h_send(0, 0, 30, 0);
    s->as_CorkState = BSD_CORK_FLUSH;
    s->as_CorkOwner = BSD_CORK_OWNER_IP;
    h.mutex_busy    = TRUE;
    h.mutex_gets    = 0;

    CHECK(h_send(0, 30, 5, MSG_DONTWAIT) == -1 &&
          h_base.sb_Errno == AMI_EAGAIN,
          "a non-blocking write is EAGAIN");
    CHECK(h.mutex_gets == 0, "without taking nx_ip_protection even once");
    CHECK(s->as_TxWait == 1, "and asks for FD_WRITE");

    /* SO_SNDTIMEO: the barrier is sliced and runs out. */
    s->as_SndTimeout = 20;
    CHECK(h_send(0, 30, 5, 0) == -1 && h_base.sb_Errno == AMI_EAGAIN,
          "SO_SNDTIMEO: EAGAIN when the pass outlasts it");
    CHECK(h.mutex_gets >= 2 && h.mutex_wait_max <= 10,
          "in bounded slices, never TX_WAIT_FOREVER");

    /* Ctrl-C while it waits. */
    s->as_SndTimeout = 0;
    h.mutex_breaks   = TRUE;
    CHECK(h_send(0, 30, 5, 0) == -1 && h_base.sb_Errno == AMI_EINTR,
          "the break ends the wait with EINTR");
    CHECK(h.mutex_wait_max <= 10, "still a slice at a time");
    CHECK(h.sends == 0 && h_pending(s) == 30, "and nothing moved");

    /* The pass ends: the write goes behind what it left. */
    h.signals        = 0;
    h.mutex_busy     = FALSE;
    h.mutex_finishes = s;
    CHECK(h_send(0, 30, 5, 0) == 5 && h_pending(s) == 35,
          "once the pass is over the write is appended");
}

#ifdef AMINETXDUO_TCP_CORK_FASTPATH
static void t_fast_kick(void)
{
    AmiSocket *s;
    ULONG      enters;

    printf("cork: the window notify during a fast-path copy\n");

    /* R's notify lands while F is copying: it finds APPEND, not STALLED, and
       leaves KICK.  F's completion must wake the pass. */
    h_reset();
    s = h_tcp(0);
    (VOID)h_send(0, 0, 10, 0);
    h.events = 0;
    h.event_sets = 0;
    h.copy_runs_notify = s;
    CHECK(h_send(0, 10, 20, 0) == 20, "the fast path takes the write");
    CHECK((s->as_CorkFlags & BSD_CORKF_KICK) == 0 &&
          (s->as_CorkFlags & BSD_CORKF_TICK) != 0 && h_linked(s) &&
          h.timer_live && h.event_sets == 0,
          "and turns the KICK the notify left into the tick, setting no "
          "event from outside the bracket");
    h_pass();
    CHECK(h_wire_is(0, 30), "which sends it all");

    /* A STALLED segment is not appended to outside the bracket. */
    h_reset();
    s = h_tcp(0);
    (VOID)h_send(0, 0, 10, 0);
    h.send_plan[0] = NX_WINDOW_OVERFLOW;
    h.send_planned = 1;
    h_pass();
    CHECK((s->as_CorkFlags & BSD_CORKF_STALLED) != 0 &&
          (s->as_CorkFlags & BSD_CORKF_TICK) == 0 && !h.timer_live,
          "stalled, off the tick");
    enters = h.nx_enters;
    CHECK(h_send(0, 10, 5, 0) == 5 && h.nx_enters == enters + 1,
          "the fast path declines a STALLED segment");
    h.window_notify(&s->as_Nx.tcp);
    h.send_planned = 0;
    h_pass();
    CHECK(h_wire_is(0, 15), "and the notify still gets it all out");
}
#endif

/* B, sharing the socket, sets TCP_NODELAY 1; its push gets NX_NO_PACKET,
   so a segment stays pending on the tick with the cork off. */
static void h_b_nodelay_off(void)
{
    struct Task *was = h.me;

    h.me = &h_other;
    (VOID)bsd_cork_set(&h_base, &h_sock[0], FALSE);
    h.me = was;
}

static void t_nodelay_race(void)
{
    AmiSocket *s;
    ULONG      enters;

    printf("cork: TCP_NODELAY 1 from a task sharing the socket\n");

    h_reset();
    s = h_tcp(0);
    (VOID)h_send(0, 0, 10, 0);

    h.send_plan[0] = NX_NO_PACKET;          /* B's push                    */
    h.send_plan[1] = NX_SUCCESS;
    h.send_planned = 2;

    enters        = h.nx_enters;
    h_forbid_hook = h_b_nodelay_off;
    CHECK(h_send(0, 10, 5, 0) == 5, "A's write is taken");
    CHECK((s->as_CorkFlags & BSD_CORKF_ON) == 0, "B turned the cork off");
#ifdef AMINETXDUO_TCP_CORK_FASTPATH
    CHECK(h.nx_enters == enters + 1,
          "the fast path saw it under Forbid() and took the bracket");
#else
    (VOID)enters;
#endif
    CHECK(s->as_CorkPkt == NULL, "nothing is appended to the leftover segment");
    CHECK(h.sends == 3 && h_wire_is(0, 15),
          "it drains first, then the write goes: in order, each byte once");

    /* The slow path the same way: B's NODELAY 1 lands while A's claim waits
       for the IP pass. */
    h_reset();
    s = h_tcp(0);
    (VOID)h_send(0, 0, 10, 0);
    h.send_plan[0] = NX_NO_PACKET;
    h.send_plan[1] = NX_SUCCESS;
    h.send_planned = 2;
    (VOID)bsd_cork_set(&h_base, s, FALSE);  /* B: a segment stays pending */
    CHECK(h_pending(s) == 10, "the push left the segment on the tick");
    CHECK(h_send(0, 10, 5, 0) == 5 && s->as_CorkPkt == NULL &&
          h_wire_is(0, 15),
          "after TCP_NODELAY 1 a write only drains what is left, then goes");
}

static void t_loopback(void)
{
    AmiSocket *s;

    printf("cork: the loopback is never corked\n");

    h_reset();
    s = h_tcp(0);
    s->as_Nx.tcp.nx_tcp_socket_connect_interface = &h_lo;

    CHECK(h_send(0, 0, 10, 0) == 10 && h.sends == 1 && h_wire_is(0, 10),
          "a small write on the loopback goes at once");
    CHECK(s->as_CorkPkt == NULL && !h.timer_live, "nothing held, no tick");
}

static void t_nodelay_off(void)
{
    AmiSocket *s;

    printf("cork: TCP_NODELAY 1 with data held\n");

    h_reset();
    s = h_tcp(0);
    (VOID)h_send(0, 0, 30, 0);

    h.send_plan[0] = NX_WINDOW_OVERFLOW;
    h.send_plan[1] = NX_SUCCESS;
    h.send_planned = 2;
    CHECK(bsd_cork_set(&h_base, s, FALSE) == 0 && h.sends == 1,
          "1 pushes what is held, without waiting");
    CHECK(h_pending(s) == 30, "the window refused it: still held");

    CHECK(h_send(0, 30, 10, 0) == 10, "the next write");
    CHECK(h_wire_is(0, 40) && s->as_CorkPkt == NULL,
          "goes after what was held, never around it");
}

int main(void)
{
#ifdef AMINETXDUO_TCP_CORK_FASTPATH
    printf("cork.c host checks, with the append fast path\n\n");
#else
    printf("cork.c host checks\n\n");
#endif

    t_setget();
    t_sub_mss();
    t_tick();
    t_mss_fill();
    t_larger_than_room();
    t_window_stall();
    t_no_packet();
    t_drop_statuses();
    t_pass_during_append();
    t_handoff_and_fast_decline();
    t_blocking_break_and_timeout();
    t_nonblocking();
    t_oob();
    t_shut_write();
    t_close_graceful();
    t_linger_and_abort();
    t_recv();
    t_select();
    t_disconnect();
    t_teardown();
    t_stop_refused();
    t_stop_timer_refusals();
    t_stop_during_pass();
    t_abort_while_owned();
    t_claim_vs_pass();
#ifdef AMINETXDUO_TCP_CORK_FASTPATH
    t_fast_kick();
#endif
    t_nodelay_race();
    t_loopback();
    t_nodelay_off();

    printf("\n%lu checks, %lu failures\n", h_checks, h_failures);

    return (h_failures == 0) ? 0 : 1;
}

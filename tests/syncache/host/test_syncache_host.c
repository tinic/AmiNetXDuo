/*
 * AmiNetXDuo, the SYN defence: the cookie's arithmetic and the cache's
 * bookkeeping, on the host.
 *
 * SPDX-License-Identifier: MIT
 */

/* Half the cache, so two cohorts fill it exactly at any configured size. */
#define COHORT  (NX_TCP_SYNCACHE_SIZE / 2)

#include "nx_api.h"
#include "nx_ip.h"
#include "nx_tcp.h"
#include "nx_packet.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>


static ULONG host_now;
static ULONG host_ms;       /* NX_TCP_SYNCACHE_CLOCK, the port's millisecond clock */

ULONG _nx_amiga_handshake_millis(VOID)
{
    return host_ms;
}

ULONG _tx_time_get(VOID)
{
    return host_now;
}

static int stub_synacks;
static int stub_rsts;
static int stub_established;
static NX_TCP_SOCKET *stub_last_socket;
static ULONG stub_last_seq;
static ULONG stub_synack_mss;
static ULONG stub_synack_scale;
static ULONG stub_synack_window;

VOID _nx_tcp_packet_send_syn(NX_TCP_SOCKET *socket_ptr, ULONG tx_sequence)
{
    ULONG mss;

    stub_last_seq = tx_sequence;
    stub_synacks++;

    mss = (ULONG) (socket_ptr -> nx_tcp_socket_connect_interface
                       -> nx_interface_ip_mtu_size
                   - sizeof(NX_IPV4_HEADER) - sizeof(NX_TCP_HEADER));
    mss &= 0x0000FFFFUL;

    if (mss < socket_ptr -> nx_tcp_socket_peer_mss)
    {
        socket_ptr -> nx_tcp_socket_connect_mss = mss;
    }
    else
    {
        socket_ptr -> nx_tcp_socket_connect_mss = socket_ptr -> nx_tcp_socket_peer_mss;
    }

#ifdef NX_ENABLE_TCP_WINDOW_SCALING
    if (socket_ptr -> nx_tcp_snd_win_scale_value != 0xFF)
    {
        UINT  scale;
        ULONG scale_window = socket_ptr -> nx_tcp_socket_rx_window_current;

        /* The shipped sender's arithmetic: from the largest window the socket
           may grow to, not the one it advertises now.  */
        if (socket_ptr -> nx_tcp_socket_rx_window_maximum > scale_window)
        {
            scale_window = socket_ptr -> nx_tcp_socket_rx_window_maximum;
        }

        for (scale = 0; scale < 15; scale++)
        {
            if ((scale_window >> scale) < 65536)
            {
                break;
            }
        }
        if (scale == 15)
        {
            scale = 14;
        }
        socket_ptr -> nx_tcp_rcv_win_scale_value = scale;
    }
    else
    {
        socket_ptr -> nx_tcp_rcv_win_scale_value = 0;
    }
#endif

    stub_synack_mss = socket_ptr -> nx_tcp_socket_connect_mss;
    stub_synack_window = socket_ptr -> nx_tcp_socket_rx_window_current;
#ifdef NX_ENABLE_TCP_WINDOW_SCALING
    stub_synack_scale = socket_ptr -> nx_tcp_rcv_win_scale_value;
#endif
}

VOID _nx_tcp_packet_send_rst(NX_TCP_SOCKET *socket_ptr, NX_TCP_HEADER *header_ptr)
{
    (void) socket_ptr;
    (void) header_ptr;
    stub_rsts++;
}

VOID _nx_tcp_socket_state_syn_received(NX_TCP_SOCKET *socket_ptr,
                                       NX_TCP_HEADER *tcp_header_ptr)
{
    (void) tcp_header_ptr;
    socket_ptr -> nx_tcp_socket_state = NX_TCP_ESTABLISHED;
    stub_last_socket = socket_ptr;
    stub_established++;
}

/* What the cache asks of the rest of the stack once a connection is handed
   over (2d44d563).  Each is a model of what the real one does to what the
   cache gives it, checked, so a call the cache should not make, or makes with
   the wrong thing, fails the arm instead of being absorbed.  */
static int stub_violations;

static void stub_violation(const char *what)
{
    printf("FAIL model: %s\n", what);
    stub_violations++;
}

/* _nx_tcp_packet_send_ack: an ACK from a connection that has a peer, at the
   sequence number it is handed.  Recorded with the window it advertises.  */
static int            stub_acks;
static NX_TCP_SOCKET *stub_ack_socket;
static ULONG          stub_ack_seq;
static ULONG          stub_ack_ack;
static ULONG          stub_ack_window;

VOID _nx_tcp_packet_send_ack(NX_TCP_SOCKET *socket_ptr, ULONG tx_sequence)
{
    if ((socket_ptr == NX_NULL) || (socket_ptr -> nx_tcp_socket_connect_port == 0))
    {
        stub_violation("an ACK from a socket with no peer");
        return;
    }
    stub_acks++;
    stub_ack_socket = socket_ptr;
    stub_ack_seq = tx_sequence;
    stub_ack_ack = socket_ptr -> nx_tcp_socket_rx_sequence;
    stub_ack_window = socket_ptr -> nx_tcp_socket_rx_window_current;
}

/* The rig's packets.  A packet is the cache's to hold or release only while
   it is allocated: releasing one twice, or one the rig never gave out, is a
   violation.  */
#define RIG_PACKETS 4
static NX_PACKET rig_rx[RIG_PACKETS];
static UCHAR     rig_rx_data[RIG_PACKETS][64];
static int       rig_rx_live[RIG_PACKETS];
static int       stub_released;

static int rig_rx_index(NX_PACKET *packet_ptr)
{
    int i;

    for (i = 0; i < RIG_PACKETS; i++)
    {
        if (packet_ptr == &rig_rx[i])
        {
            return(i);
        }
    }
    return(-1);
}

UINT _nx_packet_release(NX_PACKET *packet_ptr)
{
    int i = rig_rx_index(packet_ptr);

    if ((i < 0) || (rig_rx_live[i] == 0))
    {
        stub_violation("a packet released that the cache did not own");
        return(NX_PTR_ERROR);
    }
    rig_rx_live[i] = 0;
    stub_released++;
    return(NX_SUCCESS);
}

/* _nx_tcp_socket_packet_process, as an ESTABLISHED socket takes in-order
   data: the packet is the socket's, no longer on any queue, and starts at
   the next byte the socket expects; the socket takes its data and the
   packet.  */
static int    stub_processed;
static ULONG  stub_processed_bytes;

VOID _nx_tcp_socket_packet_process(NX_TCP_SOCKET *socket_ptr, NX_PACKET *packet_ptr)
{
    NX_TCP_HEADER *h;
    ULONG          header_length;
    int            i = rig_rx_index(packet_ptr);

    if ((i < 0) || (rig_rx_live[i] == 0))
    {
        stub_violation("a packet processed that the cache did not own");
        return;
    }
    if (socket_ptr -> nx_tcp_socket_state != NX_TCP_ESTABLISHED)
    {
        stub_violation("a held packet processed before the socket is established");
    }
    if (packet_ptr -> nx_packet_union_next.nx_packet_tcp_queue_next != (NX_PACKET *) NX_PACKET_ALLOCATED)
    {
        stub_violation("a held packet processed while still marked queued");
    }
    h = (NX_TCP_HEADER *) packet_ptr -> nx_packet_prepend_ptr;
    header_length = (h -> nx_tcp_header_word_3 >> NX_TCP_HEADER_SHIFT) << 2;
    if (h -> nx_tcp_sequence_number != socket_ptr -> nx_tcp_socket_rx_sequence)
    {
        stub_violation("a held packet processed out of order");
    }
    socket_ptr -> nx_tcp_socket_rx_sequence += packet_ptr -> nx_packet_length - header_length;
    stub_processed_bytes += packet_ptr -> nx_packet_length - header_length;
    stub_processed++;
    rig_rx_live[i] = 0;
}

ULONG _nx_ip_route_find(NX_IP *ip_ptr, ULONG destination_address,
                        NX_INTERFACE **nx_ip_interface, ULONG *next_hop_address)
{
    (void) ip_ptr;
    (void) nx_ip_interface;
    *next_hop_address = destination_address;
    return NX_SUCCESS;
}


static int failures;

static void ok(const char *what, int cond)
{
    if (cond)
    {
        printf("ok   %s\n", what);
    }
    else
    {
        printf("FAIL %s\n", what);
        failures++;
    }
}

static void eq(const char *what, unsigned long got, unsigned long want)
{
    if (got == want)
    {
        printf("ok   %s = %lu\n", what, got);
    }
    else
    {
        printf("FAIL %s: got %lu, want %lu\n", what, got, want);
        failures++;
    }
}

/* A deliberately awkward key: nothing here may depend on it, and a run that
   passes only for the key NX_RAND happened to draw is not a test.  */
static ULONG test_key[4] = { 0x0badc0deUL, 0x1234abcdUL, 0xfeedfaceUL, 0x00000001UL };


static UINT cookie_case(void)
{
    ULONG tuple[3];
    ULONG cookie;
    ULONG back = 0;
    ULONG count = 12345;
    ULONG irs = 0x11223344UL;
    ULONG data;
    UINT  i;
    int   round_trips = 0;
    int   accepted;

    tuple[0] = 0xc0a80105UL;    /* 192.168.1.5, the peer          */
    tuple[1] = 0xc0a80158UL;    /* 192.168.1.88, this machine     */
    tuple[2] = (50UL << 16) | 40000UL;

    for (data = 0; data < 1024; data++)
    {
        cookie = _nx_tcp_syncache_cookie_build(test_key, tuple, 3, irs, count, data);

        if (_nx_tcp_syncache_cookie_check(test_key, tuple, 3, irs, count, cookie,
                                          &back) == NX_TRUE)
        {
            if (back == data)
            {
                round_trips++;
            }
        }
    }
    eq("every option encoding round trips", (unsigned long) round_trips, 1024);

    /* The counter window.  A cookie is accepted in the step it was minted and
       the one after, and in nothing else -- which is what bounds how long a
       captured one is worth replaying.  */
    cookie = _nx_tcp_syncache_cookie_build(test_key, tuple, 3, irs, count, 0x155);

    ok("accepted in its own counter step",
       _nx_tcp_syncache_cookie_check(test_key, tuple, 3, irs, count, cookie, &back) == NX_TRUE);
    eq("and carries its options out", back, 0x155);

    ok("accepted one counter step later",
       _nx_tcp_syncache_cookie_check(test_key, tuple, 3, irs, count + 1, cookie,
                                     &back) == NX_TRUE);

    ok("refused two counter steps later",
       _nx_tcp_syncache_cookie_check(test_key, tuple, 3, irs, count + 2, cookie,
                                     &back) == NX_FALSE);

    ok("refused from the step before it was minted",
       _nx_tcp_syncache_cookie_check(test_key, tuple, 3, irs, count - 1, cookie,
                                     &back) == NX_FALSE);

    /* The counter at the top of its range.  It is read off the clock as a
       ULONG and only its low eight bits go into the cookie, so the step after
       0xFFFFFFFF is 0 on a 32-bit ULONG and 0x100000000 on a 64-bit one; both
       are the next step.  */
    {
        ULONG top = 0xFFFFFFFFUL;
        ULONG wrap = _nx_tcp_syncache_cookie_build(test_key, tuple, 3, irs, top, 0x2AB);

        ok("minted at counter 0xFFFFFFFF, accepted in its own step",
           _nx_tcp_syncache_cookie_check(test_key, tuple, 3, irs, top, wrap, &back) == NX_TRUE &&
           back == 0x2AB);
        ok("accepted one step on, at counter 0",
           _nx_tcp_syncache_cookie_check(test_key, tuple, 3, irs, 0UL, wrap, &back) == NX_TRUE &&
           back == 0x2AB);
        ok("accepted one step on, as top + 1 in ULONG arithmetic",
           _nx_tcp_syncache_cookie_check(test_key, tuple, 3, irs, top + 1UL, wrap, &back) == NX_TRUE);
        ok("refused two steps on, at counter 1",
           _nx_tcp_syncache_cookie_check(test_key, tuple, 3, irs, 1UL, wrap, &back) == NX_FALSE);
        ok("refused two steps on, as top + 2",
           _nx_tcp_syncache_cookie_check(test_key, tuple, 3, irs, top + 2UL, wrap, &back) == NX_FALSE);
        ok("refused the step before, 0xFFFFFFFE",
           _nx_tcp_syncache_cookie_check(test_key, tuple, 3, irs, 0xFFFFFFFEUL, wrap, &back) == NX_FALSE);

        wrap = _nx_tcp_syncache_cookie_build(test_key, tuple, 3, irs, 0xFFUL, 0x011);
        ok("minted at 0xFF, the eight-bit field's top, accepted at 0x100",
           _nx_tcp_syncache_cookie_check(test_key, tuple, 3, irs, 0x100UL, wrap, &back) == NX_TRUE &&
           back == 0x011);
        ok("and refused at 0x101",
           _nx_tcp_syncache_cookie_check(test_key, tuple, 3, irs, 0x101UL, wrap, &back) == NX_FALSE);
    }

    /* The additive carry.  The cookie is h1 + irs + (count << 24) +
       ((h2 + data) mod 2^24): pick a peer sequence number for which h1 + irs
       overflows 32 bits and h2 + data crosses 2^24, so building it carries
       out of the option field and checking it, (rest - h2), borrows.  Once
       at an ordinary counter and once at 0xFFFFFFFF, where the step after
       also wraps.  Searched, not assumed: h2 depends on irs and the counter.  */
    {
        static const ULONG counts[2] = { 12345UL, 0xFFFFFFFFUL };
        UINT  k;

        for (k = 0; k < 2; k++)
        {
            ULONG cnt = counts[k];
            ULONG start;
            ULONG c_irs;
            ULONG h1;
            ULONG h2 = 0;
            ULONG counted[5];
            ULONG carry_cookie;
            ULONG tries;
            int   found = 0;

            h1 = _nx_tcp_syncache_hash(&test_key[0], tuple, 3) & 0xFFFFFFFFUL;
            start = (0xFFFFFFFFUL - h1 + 1UL) & 0xFFFFFFFFUL;
            for (tries = 0, c_irs = start; tries < 0x400000UL; tries++, c_irs = (c_irs + 1UL) & 0xFFFFFFFFUL)
            {
                counted[0] = tuple[0];
                counted[1] = tuple[1];
                counted[2] = tuple[2];
                counted[3] = cnt & 0xFFFFFFFFUL;
                counted[4] = c_irs;
                h2 = _nx_tcp_syncache_hash(&test_key[2], counted, 5) & 0xFFFFFFFFUL;
                if (((h2 & 0x00FFFFFFUL) > (0x00FFFFFFUL - 0x3FFUL)) &&
                    (((h1 + c_irs) & 0xFFFFFFFFUL) < h1))
                {
                    found = 1;
                    break;
                }
            }
            printf("     carry vector at counter %08lx: found %d, h1 %08lx irs %08lx h2 %08lx\n",
                   (unsigned long) cnt, found, (unsigned long) h1, (unsigned long) c_irs,
                   (unsigned long) h2);
            eq("a carry vector was found within the bound", (unsigned long) found, 1);
            if (found == 0)
            {
                continue;
            }

            carry_cookie = _nx_tcp_syncache_cookie_build(test_key, tuple, 3, c_irs, cnt, 0x3FF);
            ok("h2 + data crosses 2^24 and h1 + irs 2^32: the options round trip",
               _nx_tcp_syncache_cookie_check(test_key, tuple, 3, c_irs, cnt, carry_cookie,
                                             &back) == NX_TRUE && back == 0x3FF);
            ok("the carry does not move the counter step: accepted one on",
               _nx_tcp_syncache_cookie_check(test_key, tuple, 3, c_irs,
                                             (cnt + 1UL) & 0xFFFFFFFFUL, carry_cookie,
                                             &back) == NX_TRUE && back == 0x3FF);
            ok("refused two on",
               _nx_tcp_syncache_cookie_check(test_key, tuple, 3, c_irs,
                                             (cnt + 2UL) & 0xFFFFFFFFUL, carry_cookie,
                                             &back) == NX_FALSE);
            ok("refused one before",
               _nx_tcp_syncache_cookie_check(test_key, tuple, 3, c_irs,
                                             (cnt - 1UL) & 0xFFFFFFFFUL, carry_cookie,
                                             &back) == NX_FALSE);
            carry_cookie = _nx_tcp_syncache_cookie_build(test_key, tuple, 3, c_irs, cnt, 0x000);
            ok("the same irs with data 0, no carry out of the field, round trips",
               _nx_tcp_syncache_cookie_check(test_key, tuple, 3, c_irs, cnt, carry_cookie,
                                             &back) == NX_TRUE && back == 0x000);
        }
    }

    /* The peer's own sequence number is bound in, so a cookie cannot be
       lifted onto a different handshake from the same address.  */
    ok("refused against a different peer sequence number",
       _nx_tcp_syncache_cookie_check(test_key, tuple, 3, irs + 1, count, cookie,
                                     &back) == NX_FALSE);

    /* And the four addresses and two ports are bound in, one at a time.  */
    accepted = 0;
    for (i = 0; i < 3; i++)
    {
        ULONG saved = tuple[i];

        tuple[i] = saved ^ 1UL;
        if (_nx_tcp_syncache_cookie_check(test_key, tuple, 3, irs, count, cookie,
                                          &back) == NX_TRUE)
        {
            accepted++;
        }
        tuple[i] = saved;
    }
    eq("refused against a changed address or port", (unsigned long) accepted, 0);

    /* A different key is a different machine.  Without this the construction
       would be a checksum anybody could reproduce.  */
    {
        ULONG other[4];

        for (i = 0; i < 4; i++)
        {
            other[i] = test_key[i];
        }
        other[2] ^= 0x00000040UL;

        ok("refused under a different second key",
           _nx_tcp_syncache_cookie_check(other, tuple, 3, irs, count, cookie,
                                         &back) == NX_FALSE);

        other[2] = test_key[2];
        other[0] ^= 0x00000040UL;

        ok("refused under a different first key",
           _nx_tcp_syncache_cookie_check(other, tuple, 3, irs, count, cookie,
                                         &back) == NX_FALSE);
    }

    {
        int hits = 0;

        srand(1);
        for (i = 0; i < 250000u; i++)
        {
            ULONG guess = ((ULONG) rand() << 17) ^ ((ULONG) rand() << 3) ^ (ULONG) rand();

            if (_nx_tcp_syncache_cookie_check(test_key, tuple, 3, irs, count,
                                              guess & 0xFFFFFFFFUL, &back) == NX_TRUE)
            {
                hits++;
            }
        }
        printf("     250000 blind guesses accepted %d\n", hits);
        ok("a blind acknowledgment number is not a connection", hits <= 3);
    }

    /* The MSS table is what the four bits of MSS mean.  It must never hand
       back a segment size larger than the peer asked for -- that is a
       connection announcing an MTU the peer never agreed to.  */
    {
        int inflated = 0;
        int exact = 0;
        ULONG mss;

        for (mss = 88; mss <= 1600; mss++)
        {
            ULONG got = _nx_tcp_syncache_mss_decode(_nx_tcp_syncache_mss_encode(mss));

            if (got > mss)
            {
                inflated++;
            }
            if (got == mss)
            {
                exact++;
            }
        }
        eq("the cookie never inflates an MSS", (unsigned long) inflated, 0);
        ok("and reproduces the common ones exactly", exact >= 10);

        eq("1460 survives exactly",
           _nx_tcp_syncache_mss_decode(_nx_tcp_syncache_mss_encode(1460)), 1460);
        eq("1220 survives exactly",
           _nx_tcp_syncache_mss_decode(_nx_tcp_syncache_mss_encode(1220)), 1220);
        eq("536 survives exactly",
           _nx_tcp_syncache_mss_decode(_nx_tcp_syncache_mss_encode(536)), 536);
        eq("the floor is below anything a legal link produces",
           _nx_tcp_syncache_mss_decode(_nx_tcp_syncache_mss_encode(100)), 88);
    }

    /* The hash under all of it.  One input bit has to move about half the
       output bits, or the arithmetic above is reversible by inspection.  */
    {
        ULONG base_in[3];
        ULONG base;
        UINT  bit;
        UINT  total = 0;
        UINT  samples = 0;

        base_in[0] = 0x01020304UL;
        base_in[1] = 0x05060708UL;
        base_in[2] = 0x090a0b0cUL;
        base = _nx_tcp_syncache_hash(test_key, base_in, 3);

        for (bit = 0; bit < 96; bit++)
        {
            ULONG mutated[3];
            ULONG out;
            ULONG diff;
            UINT  b;
            UINT  set = 0;

            mutated[0] = base_in[0];
            mutated[1] = base_in[1];
            mutated[2] = base_in[2];
            mutated[bit / 32] ^= (1UL << (bit % 32));

            out = _nx_tcp_syncache_hash(test_key, mutated, 3);
            diff = (base ^ out) & 0xFFFFFFFFUL;

            for (b = 0; b < 32; b++)
            {
                if (diff & (1UL << b))
                {
                    set++;
                }
            }
            total += set;
            samples++;
        }

        printf("     avalanche: %u bits over %u single-bit changes\n", total, samples);
        ok("one input bit moves about half the output",
           (total >= (samples * 12u)) && (total <= (samples * 20u)));

        ok("a different key is a different hash",
           _nx_tcp_syncache_hash(test_key, base_in, 3) !=
           _nx_tcp_syncache_hash(&test_key[2], base_in, 3));

        ok("length is part of the message",
           _nx_tcp_syncache_hash(test_key, base_in, 2) !=
           _nx_tcp_syncache_hash(test_key, base_in, 3));
    }

    return failures == 0 ? NX_TRUE : NX_FALSE;
}


static NX_IP          rig_ip;
static NX_TCP_LISTEN  rig_listen;
static NX_TCP_SOCKET  rig_socket;
static NX_TCP_SOCKET  rig_socket2;     /* what a relisten parks next */
static NX_INTERFACE   rig_interface;
static NX_PACKET      rig_packet;

static int rig_callbacks;

static VOID rig_listen_callback(NX_TCP_SOCKET *socket_ptr, UINT port)
{
    (void) socket_ptr;
    (void) port;
    rig_callbacks++;
}

static void rig_reset(void)
{
    memset(&rig_ip, 0, sizeof(rig_ip));
    memset(&rig_listen, 0, sizeof(rig_listen));
    memset(&rig_socket, 0, sizeof(rig_socket));
    memset(&rig_socket2, 0, sizeof(rig_socket2));
    memset(&rig_interface, 0, sizeof(rig_interface));
    memset(rig_rx, 0, sizeof(rig_rx));
    memset(rig_rx_live, 0, sizeof(rig_rx_live));
    memset(&rig_packet, 0, sizeof(rig_packet));

    rig_interface.nx_interface_ip_mtu_size = 1500;
    rig_interface.nx_interface_valid = NX_TRUE;
    rig_interface.nx_interface_ip_address = 0xc0a80158UL;    /* rig_syn's dest_ip */

    rig_packet.nx_packet_ip_version = NX_IP_VERSION_V4;
    rig_packet.nx_packet_address.nx_packet_interface_ptr = &rig_interface;

    rig_listen.nx_tcp_listen_port = 80;
    rig_listen.nx_tcp_listen_queue_maximum = 8;
    rig_listen.nx_tcp_listen_rx_window = 8192;
    rig_listen.nx_tcp_listen_callback = rig_listen_callback;
    rig_listen.nx_tcp_listen_socket_ptr = NX_NULL;

    rig_socket.nx_tcp_socket_ip_ptr = &rig_ip;
    rig_socket.nx_tcp_socket_state = NX_TCP_LISTEN_STATE;
    rig_socket.nx_tcp_socket_rx_window_default = 8192;

    /* What nx_tcp_server_socket_relisten() hands deliver(): unbound, CLOSED. */
    rig_socket2.nx_tcp_socket_ip_ptr = &rig_ip;
    rig_socket2.nx_tcp_socket_state = NX_TCP_CLOSED;
    rig_socket2.nx_tcp_socket_rx_window_default = 8192;

    host_now = 100000;
    host_ms  = 0;
    stub_synacks = 0;
    stub_rsts = 0;
    stub_established = 0;
    stub_last_socket = NX_NULL;
    rig_callbacks = 0;
    stub_acks = 0;
    stub_ack_socket = NX_NULL;
    stub_released = 0;
    stub_processed = 0;
    stub_processed_bytes = 0;

    _nx_tcp_syncache_initialize(&rig_ip);
    memcpy(rig_ip.nx_ip_tcp_syncache.nx_tcp_syncache_key, test_key, sizeof(test_key));
}

/* One SYN from a made-up address.  `n` picks the peer, so a loop of these is
   a flood from a different forged source every time -- which is the shape the
   cache has to survive.  */
static ULONG rig_syn(ULONG n, ULONG irs)
{
    ULONG source_ip = 0x0a000000UL + n;
    ULONG dest_ip = 0xc0a80158UL;
    NX_TCP_HEADER header;

    memset(&header, 0, sizeof(header));
    header.nx_tcp_sequence_number = irs;
    header.nx_tcp_header_word_3 = NX_TCP_SYN_BIT | 8192UL;

    _nx_tcp_syncache_syn_received(&rig_ip, &rig_listen, &rig_packet, &header,
                                  &source_ip, &dest_ip, (UINT) (30000u + (n & 0xFFFu)),
                                  &rig_interface, 1460, 2, NX_TRUE, NX_TRUE, 777);

    return stub_last_seq;
}

static UINT rig_ack(ULONG n, ULONG irs, ULONG iss)
{
    ULONG source_ip = 0x0a000000UL + n;
    ULONG dest_ip = 0xc0a80158UL;
    NX_TCP_HEADER header;

    memset(&header, 0, sizeof(header));
    header.nx_tcp_sequence_number = irs + 1;
    header.nx_tcp_acknowledgment_number = iss + 1;
    header.nx_tcp_header_word_3 = NX_TCP_ACK_BIT | 8192UL;

    return _nx_tcp_syncache_ack_received(&rig_ip, &rig_listen, &rig_packet, &header,
                                          &source_ip, &dest_ip,
                                          (UINT) (30000u + (n & 0xFFFu)),
                                          &rig_interface, NX_TRUE, 888);
}

/* A segment from the peer of a connection waiting for accept, as
   _nx_tcp_packet_process would hand it to _nx_tcp_syncache_hold: header in
   host order, `len` bytes of data after it.  */
static NX_PACKET *rig_segment(int slot, ULONG word_3, ULONG seq, ULONG ack, ULONG len)
{
    NX_PACKET     *p = &rig_rx[slot];
    NX_TCP_HEADER *h = (NX_TCP_HEADER *) rig_rx_data[slot];

    memset(p, 0, sizeof(*p));
    memset(rig_rx_data[slot], 0, sizeof(rig_rx_data[slot]));
    h -> nx_tcp_header_word_3 = (5UL << NX_TCP_HEADER_SHIFT) | word_3 | 8192UL;
    h -> nx_tcp_sequence_number = seq;
    h -> nx_tcp_acknowledgment_number = ack;
    p -> nx_packet_prepend_ptr = rig_rx_data[slot];
    /* Twenty bytes of header on the wire, the data offset above: not
       sizeof(NX_TCP_HEADER), which is wider with a 64-bit ULONG.  */
    p -> nx_packet_append_ptr = rig_rx_data[slot] + 20 + len;
    p -> nx_packet_length = 20UL + len;
    rig_rx_live[slot] = 1;
    return(p);
}

/* A connection the cache handed over and accept has not been called on is
   left as upstream left one: bound, in LISTEN, with the peer's port
   (2d44d563).  */
static int rig_awaiting_accept(NX_TCP_SOCKET *socket_ptr, ULONG n)
{
    return((socket_ptr -> nx_tcp_socket_state == NX_TCP_LISTEN_STATE) &&
           (socket_ptr -> nx_tcp_socket_bound_next != NX_NULL) &&
           (socket_ptr -> nx_tcp_socket_connect_port == (UINT) (30000u + (n & 0xFFFu))));
}

/* Fill the cache with answered handshakes while a socket is parked, then take
   the socket away: the cache is full and a SYN can only be answered with a
   cookie, with no socket on the port (96502647: with no socket a SYN that
   finds room is deferred, so the cache is filled while one is parked).  */
static void rig_fill_then_unpark(ULONG base, ULONG irs)
{
    ULONG i;

    rig_listen.nx_tcp_listen_socket_ptr = &rig_socket;
    for (i = 0; i < NX_TCP_SYNCACHE_SIZE; i++)
    {
        (void) rig_syn(base + i, irs + i);
    }
    rig_listen.nx_tcp_listen_socket_ptr = NX_NULL;
}

static UINT cache_case(void)
{
    NX_TCP_SYNCACHE *cache = &rig_ip.nx_ip_tcp_syncache;
    ULONG i;
    ULONG iss_first;
    ULONG iss_last;

    rig_reset();
    rig_listen.nx_tcp_listen_socket_ptr = &rig_socket;

    iss_first = rig_syn(1, 0x1000);

    eq("one SYN, one entry", cache -> nx_tcp_syncache_count, 1);
    eq("one SYN, one answer", (unsigned long) stub_synacks, 1);
    eq("no cookie was needed", cache -> nx_tcp_syncache_cookies_sent, 0);
    ok("the socket is still the listen request's",
       rig_listen.nx_tcp_listen_socket_ptr == &rig_socket);
    eq("and nothing was handed to the application", (unsigned long) rig_callbacks, 0);

    ok("a repeated SYN is answered again", rig_syn(1, 0x1000) == iss_first);
    eq("and does not take a second entry", cache -> nx_tcp_syncache_count, 1);
    eq("but is answered", (unsigned long) stub_synacks, 2);

    ok("the ACK is consumed", rig_ack(1, 0x1000, iss_first) == NX_TRUE);
    eq("the entry is given back", cache -> nx_tcp_syncache_count, 0);
    eq("the application is told once", (unsigned long) rig_callbacks, 1);
    ok("the listen request's socket has been taken",
       rig_listen.nx_tcp_listen_socket_ptr == NX_NULL);
    /* 2d44d563: accept moves it on, not the ACK.  */
    eq("the connection waits for accept, not established by the ACK",
       (unsigned long) stub_established, 0);
    ok("on the parked socket, bound in LISTEN with the peer's port",
       rig_awaiting_accept(&rig_socket, 1));
    eq("and nothing is sent while it waits", (unsigned long) stub_acks, 0);
    ok("accept connects it", _nx_tcp_syncache_accept(&rig_socket) == NX_TRUE);
    eq("the connection is established", (unsigned long) stub_established, 1);
    ok("and it is the socket that was parked", stub_last_socket == &rig_socket);
    eq("accept sends one ACK, which opens the window", (unsigned long) stub_acks, 1);
    ok("from that socket, at iss + 1, acknowledging irs + 1",
       stub_ack_socket == &rig_socket && stub_ack_seq == iss_first + 1 && stub_ack_ack == 0x1001);
    ok("a second accept is not a second connection",
       _nx_tcp_syncache_accept(&rig_socket) == NX_FALSE && stub_established == 1);
    eq("with the sequence numbers the handshake agreed",
       rig_socket.nx_tcp_socket_tx_sequence, (unsigned long) (iss_first + 1));
    eq("and the peer's", rig_socket.nx_tcp_socket_rx_sequence, 0x1001);

    /* The options the SYN carried are on the socket, not defaults. */
    eq("the peer's MSS is exact, not quantised",
       rig_socket.nx_tcp_socket_peer_mss, 1460);
#ifdef NX_ENABLE_TCP_WINDOW_SCALING
    eq("the peer's window scale survived", rig_socket.nx_tcp_snd_win_scale_value, 2);
#endif
#ifdef NX_ENABLE_TCP_SACK
    eq("SACK-Permitted survived", rig_socket.nx_tcp_socket_sack_permitted, NX_TRUE);
#endif
#ifdef NX_ENABLE_TCP_TIMESTAMP
    eq("timestamps survived", rig_socket.nx_tcp_socket_timestamp_enabled, NX_TRUE);
    eq("and TS.Recent came off the ACK, which is fresher than the SYN",
       rig_socket.nx_tcp_socket_ts_recent, 888);
#endif

    /* Data the peer sends before accept is held on the socket unacknowledged
       and processed by accept, in order (2d44d563); a segment that does not
       continue it is released.  */
    rig_reset();
    rig_listen.nx_tcp_listen_socket_ptr = &rig_socket;
    {
        ULONG iss = rig_syn(2, 0x1100);

        ok("a handshake to hold data for", rig_ack(2, 0x1100, iss) == NX_TRUE &&
                                           rig_awaiting_accept(&rig_socket, 2));
        ok("in-order data is held",
           _nx_tcp_syncache_hold(&rig_socket,
                                 rig_segment(0, NX_TCP_ACK_BIT | NX_TCP_PSH_BIT, 0x1101, iss + 1, 10)) == NX_TRUE);
        ok("a segment that does not follow it is taken",
           _nx_tcp_syncache_hold(&rig_socket,
                                 rig_segment(1, NX_TCP_ACK_BIT, 0x1101 + 500, iss + 1, 10)) == NX_TRUE);
        eq("and released, not held", (unsigned long) stub_released, 1);
        eq("one segment is held", rig_socket.nx_tcp_socket_receive_queue_count, 1);
        eq("nothing was acknowledged while held", (unsigned long) stub_acks, 0);
        eq("nor processed", (unsigned long) stub_processed, 0);
        ok("accept connects it", _nx_tcp_syncache_accept(&rig_socket) == NX_TRUE);
        eq("and processes what was held", (unsigned long) stub_processed, 1);
        eq("all ten bytes of it", (unsigned long) stub_processed_bytes, 10);
        eq("leaving nothing on the hold queue", rig_socket.nx_tcp_socket_receive_queue_count, 0);
        eq("after the one window-opening ACK", (unsigned long) stub_acks, 1);
        eq("and no packet is left owned by the cache",
           (unsigned long) (rig_rx_live[0] + rig_rx_live[1]), 0);
    }

    rig_reset();
    rig_listen.nx_tcp_listen_socket_ptr = &rig_socket;

    ok("an acknowledgment with no entry and no cookie is refused",
       rig_ack(9, 0x5000, 0x77777777UL) == NX_FALSE);
    eq("and made nothing", (unsigned long) stub_established, 0);
    eq("and sent nothing back", (unsigned long) (stub_rsts + stub_synacks), 0);
    ok("and was counted as a forgery",
       cache -> nx_tcp_syncache_cookies_invalid == 1);

    rig_reset();
    rig_listen.nx_tcp_listen_socket_ptr = &rig_socket;

    for (i = 0; i < NX_TCP_SYNCACHE_SIZE; i++)
    {
        (void) rig_syn(1000 + i, 0x2000 + i);
    }

    eq("the cache fills", cache -> nx_tcp_syncache_count, NX_TCP_SYNCACHE_SIZE);
    eq("every SYN was answered", (unsigned long) stub_synacks, NX_TCP_SYNCACHE_SIZE);
    eq("and none needed a cookie", cache -> nx_tcp_syncache_cookies_sent, 0);

    iss_last = rig_syn(9999, 0x3000);

    eq("past full, still answered", (unsigned long) stub_synacks, NX_TCP_SYNCACHE_SIZE + 1);
    eq("statelessly", cache -> nx_tcp_syncache_cookies_sent, 1);
    eq("so the cache did not grow", cache -> nx_tcp_syncache_count, NX_TCP_SYNCACHE_SIZE);
    eq("and no entry was thrown out to make room", cache -> nx_tcp_syncache_evicted, 0);

    /* The connection made with no entry behind it still completes, and the
       socket it lands on still carries the options the SYN offered.  */
    ok("and the cookie completes a real connection",
       rig_ack(9999, 0x3000, iss_last) == NX_TRUE);
    eq("with no entry ever stored for it", cache -> nx_tcp_syncache_count,
       NX_TCP_SYNCACHE_SIZE);
    eq("the cookie was recognised", cache -> nx_tcp_syncache_cookies_valid, 1);
    ok("the connection waits for accept on the parked socket (2d44d563)",
       stub_established == 0 && rig_awaiting_accept(&rig_socket, 9999));
    ok("and accept connects it", _nx_tcp_syncache_accept(&rig_socket) == NX_TRUE);
    eq("the connection is established", (unsigned long) stub_established, 1);
#ifdef NX_ENABLE_TCP_WINDOW_SCALING
    eq("the window scale survived the cookie",
       rig_socket.nx_tcp_snd_win_scale_value, 2);
#endif
#ifdef NX_ENABLE_TCP_SACK
    eq("SACK-Permitted survived the cookie",
       rig_socket.nx_tcp_socket_sack_permitted, NX_TRUE);
#endif
#ifdef NX_ENABLE_TCP_TIMESTAMP
    eq("timestamps survived the cookie",
       rig_socket.nx_tcp_socket_timestamp_enabled, NX_TRUE);
#endif
    eq("and the MSS came back at the table value below what was asked",
       rig_socket.nx_tcp_socket_peer_mss, 1460);

    eq("the segment size the SYN-ACK announced is reproduced",
       rig_socket.nx_tcp_socket_connect_mss, stub_synack_mss);
#ifdef NX_ENABLE_TCP_WINDOW_SCALING
    eq("and so is the window scale it announced",
       rig_socket.nx_tcp_rcv_win_scale_value, stub_synack_scale);
#endif

    rig_reset();
    rig_listen.nx_tcp_listen_socket_ptr = &rig_socket;

    /* Two cohorts that together fill the cache, so the assertion is about
       age and not about a number: NX_TCP_SYNCACHE_SIZE is a build option now
       (32 in the minimal drawer, 512 by default) and a hardcoded 200 tested
       nothing on a build smaller than that. */
    for (i = 0; i < COHORT; i++)
    {
        (void) rig_syn(2000 + i, 0x4000 + i);
    }
    host_now += 5 * NX_IP_PERIODIC_RATE;
    for (i = 0; i < COHORT; i++)
    {
        (void) rig_syn(3000 + i, 0x4000 + i);
    }
    eq("both cohorts are held", cache -> nx_tcp_syncache_count, 2 * COHORT);

    host_now += NX_TCP_SYNCACHE_TIMEOUT - (4 * NX_IP_PERIODIC_RATE);
    stub_synacks = 0;
    _nx_tcp_syncache_periodic(&rig_ip);

    eq("the older cohort is given up", cache -> nx_tcp_syncache_count, COHORT);
    eq("and counted as expired", cache -> nx_tcp_syncache_expired, COHORT);

    ok("and the survivors are the younger ones",
       _nx_tcp_syncache_deliver(&rig_ip, &rig_listen, &rig_socket) == NX_FALSE);

    host_now += NX_TCP_SYNCACHE_TIMEOUT;
    _nx_tcp_syncache_periodic(&rig_ip);
    eq("and eventually all of them", cache -> nx_tcp_syncache_count, 0);
    eq("the free list is whole again", cache -> nx_tcp_syncache_expired, 2 * COHORT);

    rig_reset();
    rig_listen.nx_tcp_listen_socket_ptr = &rig_socket;

    iss_first = rig_syn(77, 0x6000);
    host_now += NX_TCP_SYNCACHE_TIMEOUT + NX_IP_PERIODIC_RATE;
    _nx_tcp_syncache_periodic(&rig_ip);
    eq("the entry is gone", cache -> nx_tcp_syncache_count, 0);

    /* The clock has moved less than one cookie counter step, so a cookie
       minted with the SYN would still be inside the window.  A cached entry's
       sequence number is not a cookie (e5e89f1b): the handshake is lost with
       the entry.  What this rig proves is the cache's side only: the ACK is
       not consumed (NX_FALSE) and the cache sends nothing for it.  It does
       not link _nx_tcp_packet_process and is no evidence of the RST that
       goes on the wire; the fork's netx_3_06, 8_02 and 10_23_01 and
       ctl_syncookie check that.  */
    stub_rsts = 0;
    stub_synacks = 0;
    ok("the acknowledgment of an expired cached entry is not consumed (NX_FALSE)",
       rig_ack(77, 0x6000, iss_first) == NX_FALSE);
    eq("it does not decode as a cookie", cache -> nx_tcp_syncache_cookies_valid, 0);
    eq("and is counted as not one", cache -> nx_tcp_syncache_cookies_invalid, 1);
    eq("no connection is made", (unsigned long) stub_established, 0);
    ok("or rebuilt onto the parked socket",
       rig_listen.nx_tcp_listen_socket_ptr == &rig_socket &&
       rig_socket.nx_tcp_socket_state == NX_TCP_LISTEN_STATE &&
       rig_socket.nx_tcp_socket_connect_port == 0);
    eq("the cache sends nothing for it itself", (unsigned long) (stub_rsts + stub_synacks), 0);
    eq("and holds nothing for it", cache -> nx_tcp_syncache_count, 0);

    /* The peer starts again, and that works.  */
    {
        ULONG iss = rig_syn(77, 0x6100);

        eq("a fresh SYN from the same peer is answered", (unsigned long) stub_synacks, 1);
        ok("with a new sequence number", iss != iss_first);
        ok("and its ACK completes the handshake", rig_ack(77, 0x6100, iss) == NX_TRUE &&
                                                  rig_awaiting_accept(&rig_socket, 77));
        ok("which accept connects", _nx_tcp_syncache_accept(&rig_socket) == NX_TRUE &&
                                    stub_established == 1);
    }

    /* A SYN with no socket parked is deferred and not answered (96502647),
       so a finished handshake with no socket comes from SYNs answered while
       one was parked: ten arrive, the first ACK takes the socket, the next
       eight queue for accept, and the tenth is past the backlog.  */
    rig_reset();
    rig_listen.nx_tcp_listen_socket_ptr = &rig_socket;
    {
        ULONG iss[10];

        for (i = 0; i < 10; i++)
        {
            iss[i] = rig_syn(4000 + i, 0x7000 + i);
        }
        eq("ten SYNs answered while a socket is parked", (unsigned long) stub_synacks, 10);
        ok("the first ACK takes the parked socket",
           rig_ack(4000, 0x7000, iss[0]) == NX_TRUE && rig_awaiting_accept(&rig_socket, 4000));
        for (i = 1; i < 9; i++)
        {
            (void) rig_ack(4000 + i, 0x7000 + i, iss[i]);
        }
        eq("a full backlog waits", cache -> nx_tcp_syncache_accept_count, 8);
        eq("and none of them reset the peer", (unsigned long) stub_rsts, 0);
        eq("and no socket was committed", (unsigned long) stub_established, 0);

        (void) rig_ack(4009, 0x7009, iss[9]);
    }
    eq("past the backlog the queue does not grow",
       cache -> nx_tcp_syncache_accept_count, 8);
    eq("and the peer is told, rather than left hanging",
       (unsigned long) stub_rsts, 1);

    ok("relisten takes one", _nx_tcp_syncache_deliver(&rig_ip, &rig_listen,
                                                      &rig_socket2) == NX_TRUE);
    eq("the queue shortens", cache -> nx_tcp_syncache_accept_count, 7);
    ok("onto the relistened socket, waiting for accept", rig_awaiting_accept(&rig_socket2, 4001));
    ok("which accept connects", _nx_tcp_syncache_accept(&rig_socket2) == NX_TRUE);
    eq("and the connection reaches the application",
       (unsigned long) stub_established, 1);

    /* Unlisten gives up the rest, and resets the peers that think they are
       connected.  */
    stub_rsts = 0;
    _nx_tcp_syncache_flush(&rig_ip, 80);
    eq("unlisten empties the queue", cache -> nx_tcp_syncache_accept_count, 0);
    eq("and resets every peer waiting on it", (unsigned long) stub_rsts, 7);

#ifdef NX_TCP_SYNCACHE_DEFERRED_MAXIMUM
    /* More clients than the backlog and no socket parked: the SYNs are held
       up to NX_TCP_SYNCACHE_DEFERRED_MAXIMUM rather than pushed out at the
       backlog, and a socket parked later goes to the oldest whose client is
       still repeating its SYN, not to one that has gone quiet.  */
    rig_reset();
    for (i = 0; i < 20; i++)
    {
        (void) rig_syn(5000 + i, 0x8000 + i);
    }
    eq("twenty SYNs held past a backlog of eight", cache -> nx_tcp_syncache_count, 20);
    eq("and none answered", (unsigned long) stub_synacks, 0);

    host_now += 2 * NX_IP_PERIODIC_RATE;
    (void) rig_syn(5007, 0x8007);
    eq("a repeated SYN takes no entry of its own", cache -> nx_tcp_syncache_count, 20);

    rig_listen.nx_tcp_listen_socket_ptr = &rig_socket;
    _nx_tcp_syncache_answer_deferred(&rig_ip, &rig_listen);
    eq("a parked socket answers one", (unsigned long) stub_synacks, 1);
    ok("the client still trying, not the oldest",
       rig_ack(5007, 0x8007, stub_last_seq) == NX_TRUE && rig_awaiting_accept(&rig_socket, 5007));
#endif /* NX_TCP_SYNCACHE_DEFERRED_MAXIMUM */

    rig_reset();
    rig_listen.nx_tcp_listen_rx_window = 65536 * 4;      /* needs a scale */

    rig_fill_then_unpark(6000, 0xA000);
    iss_last = rig_syn(6999, 0xB000);                    /* past full: a cookie */
    eq("the cookie SYN-ACK was sent with no socket on the port",
       cache -> nx_tcp_syncache_cookies_sent, 1);
#ifdef NX_ENABLE_TCP_WINDOW_SCALING
    ok("and it announced a window scale", stub_synack_scale > 0);
#endif

    rig_listen.nx_tcp_listen_socket_ptr = &rig_socket;
    ok("the cookie completes", rig_ack(6999, 0xB000, iss_last) == NX_TRUE &&
                               cache -> nx_tcp_syncache_cookies_valid == 1 &&
                               rig_awaiting_accept(&rig_socket, 6999));
#ifdef NX_ENABLE_TCP_WINDOW_SCALING
    eq("with the scale the SYN-ACK announced, not the parked socket's",
       rig_socket.nx_tcp_rcv_win_scale_value, stub_synack_scale);
#endif

#ifdef NX_ENABLE_TCP_WINDOW_SCALING
    /* A port whose sockets open small and may grow (bsdsocket.library opens
       at 100,352 and lets a socket settle at 262,144 once the link is
       known): the SYN-ACK's scale is the grown size's, from the cache and
       from a cookie alike, or the grown window is not expressible and the
       socket stays small for the whole connection.  */
    rig_reset();
    rig_listen.nx_tcp_listen_rx_window = 100352;
    rig_listen.nx_tcp_listen_rx_window_maximum = 262144;
    rig_socket.nx_tcp_socket_rx_window_default = 100352;
    rig_listen.nx_tcp_listen_socket_ptr = &rig_socket;
    {
        ULONG iss = rig_syn(90, 0xC100);

        eq("a cached SYN-ACK advertises the window the socket opened with",
           stub_synack_window, 100352);
        eq("with the scale of the window it may grow to", stub_synack_scale, 3);
        ok("the handshake completes", rig_ack(90, 0xC100, iss) == NX_TRUE);
        eq("and the socket carries that scale",
           rig_socket.nx_tcp_rcv_win_scale_value, 3);
    }

    rig_reset();
    rig_listen.nx_tcp_listen_rx_window = 100352;
    rig_listen.nx_tcp_listen_rx_window_maximum = 262144;
    rig_socket.nx_tcp_socket_rx_window_default = 100352;
    rig_fill_then_unpark(7000, 0xA100);
    stub_synack_scale = 0;
    iss_last = rig_syn(7999, 0xB100);                    /* past full: a cookie */
    eq("past full with no socket parked, a cookie", cache -> nx_tcp_syncache_cookies_sent, 1);
    eq("a cookie SYN-ACK announces the grown window's scale too",
       stub_synack_scale, 3);
    rig_listen.nx_tcp_listen_socket_ptr = &rig_socket;
    ok("the cookie completes", rig_ack(7999, 0xB100, iss_last) == NX_TRUE &&
                               cache -> nx_tcp_syncache_cookies_valid == 1);
    eq("and the ACK reconstructs that scale, not the advertised window's",
       rig_socket.nx_tcp_rcv_win_scale_value, 3);

    /* A port that never set a maximum scales from the window, as before.  */
    rig_reset();
    rig_listen.nx_tcp_listen_rx_window = 100352;
    rig_socket.nx_tcp_socket_rx_window_default = 100352;
    rig_listen.nx_tcp_listen_socket_ptr = &rig_socket;
    (void) rig_syn(91, 0xC200);
    eq("no maximum: the scale is the advertised window's", stub_synack_scale, 1);
#endif

    /* Two SYNs answered while one socket is parked: the first ACK takes the
       socket, the second finishes with none (96502647).  */
    rig_reset();
    rig_listen.nx_tcp_listen_socket_ptr = &rig_socket;
    {
        ULONG iss87 = rig_syn(87, 0xBF00);
        ULONG iss = rig_syn(88, 0xC000);

        (void) rig_ack(87, 0xBF00, iss87);
        (void) rig_ack(88, 0xC000, iss);
    }
    eq("one finished handshake is waiting", cache -> nx_tcp_syncache_accept_count, 1);
    (void) rig_syn(88, 0xD000);
    eq("a forged SYN on the same four-tuple does not throw it away",
       cache -> nx_tcp_syncache_accept_count, 1);

    rig_reset();
    rig_listen.nx_tcp_listen_socket_ptr = &rig_socket;

    (void) rig_syn(55, 0x8000);
    eq("one half-open connection", cache -> nx_tcp_syncache_count, 1);
    {
        ULONG         source_ip = 0x0a000000UL + 55;
        NX_TCP_HEADER header;

        /* Wrong sequence number first.  An off-path remote that can guess a
           four-tuple but has not seen the SYN must not be able to end
           somebody else's handshake -- RFC 5961 section 3.  */
        memset(&header, 0, sizeof(header));
        header.nx_tcp_header_word_3 = NX_TCP_RST_BIT;
        header.nx_tcp_sequence_number = 0x8000 + 5000;
        _nx_tcp_syncache_reset_received(&rig_ip, &header, &source_ip,
                                        NX_IP_VERSION_V4, 80,
                                        (UINT) (30000u + 55u));
        eq("a RST on the wrong sequence number is refused",
           cache -> nx_tcp_syncache_count, 1);
        eq("and counted", cache -> nx_tcp_syncache_resets_refused, 1);

        header.nx_tcp_sequence_number = 0x8001;
        _nx_tcp_syncache_reset_received(&rig_ip, &header, &source_ip,
                                        NX_IP_VERSION_V4, 80,
                                        (UINT) (30000u + 55u));
    }
    eq("a RST on the right one gives it back", cache -> nx_tcp_syncache_count, 0);

    rig_reset();
    rig_listen.nx_tcp_listen_socket_ptr = &rig_socket;

    (void) rig_syn(66, 0x9000);
    stub_synacks = 0;

    for (i = 0; i < NX_TCP_SYNCACHE_TIMEOUT / NX_IP_PERIODIC_RATE; i++)
    {
        host_now += NX_IP_PERIODIC_RATE;
        _nx_tcp_syncache_periodic(&rig_ip);
    }
    eq("a lost SYN-ACK is sent again, a bounded number of times",
       (unsigned long) stub_synacks, NX_TCP_SYNCACHE_RETRIES);
    eq("and the entry is given up in the end", cache -> nx_tcp_syncache_count, 0);

    /* The handshake's round trip, on the port's clock: SYN-ACK out to ACK
       in, handed to the socket; not measured across a retransmitted SYN-ACK,
       from a cookie, or without a clock; not inflated by a wait in the
       accept queue. */
    rig_reset();
    rig_listen.nx_tcp_listen_socket_ptr = &rig_socket;
    host_ms = 1000;
    {
        ULONG iss = rig_syn(93, 0xE000);

        host_ms = 1026;
        ok("a handshake completes", rig_ack(93, 0xE000, iss) == NX_TRUE);
        eq("and the socket carries its round trip",
           rig_socket.nx_tcp_socket_handshake_rtt, 26);
    }

    rig_reset();
    rig_listen.nx_tcp_listen_socket_ptr = &rig_socket;
    host_ms = 1000;
    {
        ULONG iss = rig_syn(94, 0xE100);

        ok("the ACK in the same millisecond", rig_ack(94, 0xE100, iss) == NX_TRUE);
        eq("is a round trip of one, not of nothing measured",
           rig_socket.nx_tcp_socket_handshake_rtt, 1);
    }

    rig_reset();
    rig_listen.nx_tcp_listen_socket_ptr = &rig_socket;
    {
        ULONG iss = rig_syn(95, 0xE200);

        host_ms = 500;
        ok("without a clock the handshake completes", rig_ack(95, 0xE200, iss) == NX_TRUE);
        eq("and measured nothing", rig_socket.nx_tcp_socket_handshake_rtt, 0);
    }

    rig_reset();
    rig_listen.nx_tcp_listen_socket_ptr = &rig_socket;
    host_ms = 2000;
    {
        ULONG iss = rig_syn(96, 0xE300);

        /* Past the first rung of the retry ladder (the file's own
           NX_TCP_SYNCACHE_RETRY_LADDER starts at one second). */
        stub_synacks = 0;
        for (i = 0; i < 3 && stub_synacks == 0; i++)
        {
            host_now += NX_IP_PERIODIC_RATE;
            _nx_tcp_syncache_periodic(&rig_ip);
        }
        eq("the SYN-ACK was sent again", (unsigned long) stub_synacks, 1);
        host_ms = 2030;
        ok("and the ACK completes it", rig_ack(96, 0xE300, iss) == NX_TRUE);
        eq("with no round trip: it could answer either copy",
           rig_socket.nx_tcp_socket_handshake_rtt, 0);
    }

    rig_reset();
    rig_listen.nx_tcp_listen_socket_ptr = &rig_socket;
    host_ms = 3000;
    {
        ULONG iss96 = rig_syn(98, 0xE380);
        ULONG iss = rig_syn(97, 0xE400);

        host_ms = 3040;
        (void) rig_ack(98, 0xE380, iss96);                 /* takes the socket */
        ok("with no socket parked the handshake is queued",
           rig_ack(97, 0xE400, iss) == NX_TRUE);
        eq("in the accept queue", cache -> nx_tcp_syncache_accept_count, 1);
        host_ms = 9000;
        ok("a relisten takes it", _nx_tcp_syncache_deliver(&rig_ip, &rig_listen,
                                                           &rig_socket2) == NX_TRUE);
        eq("with the round trip of the handshake, not of the wait",
           rig_socket2.nx_tcp_socket_handshake_rtt, 40);
    }

    rig_reset();
    host_ms = 4000;
    rig_fill_then_unpark(8000, 0xF000);
    iss_last = rig_syn(8999, 0xF999);                    /* past full: a cookie */
    host_ms = 4030;
    rig_listen.nx_tcp_listen_socket_ptr = &rig_socket;
    ok("a cookie completes", rig_ack(8999, 0xF999, iss_last) == NX_TRUE &&
                             cache -> nx_tcp_syncache_cookies_valid == 1);
    eq("and carries no round trip: nothing was stored to time it",
       rig_socket.nx_tcp_socket_handshake_rtt, 0);

    return failures == 0 ? NX_TRUE : NX_FALSE;
}


int main(int argc, char **argv)
{
    const char *which = (argc > 1) ? argv[1] : "cookie";

    if (strcmp(which, "cookie") == 0)
    {
        (void) cookie_case();
    }
    else if (strcmp(which, "cache") == 0)
    {
        (void) cache_case();
    }
    else
    {
        printf("usage: test_syncache cookie|cache\n");
        return 2;
    }

    failures += stub_violations;

    if (failures != 0)
    {
        printf("syncache %s: %d failures\n", which, failures);
        return 1;
    }

    printf("syncache %s: all ok\n", which);
    return 0;
}

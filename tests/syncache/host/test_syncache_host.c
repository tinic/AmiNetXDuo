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

#include <stdio.h>
#include <string.h>
#include <stdlib.h>


static ULONG host_now;

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
        UINT scale;

        for (scale = 0; scale < 15; scale++)
        {
            if ((socket_ptr -> nx_tcp_socket_rx_window_current >> scale) < 65536)
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
    memset(&rig_interface, 0, sizeof(rig_interface));
    memset(&rig_packet, 0, sizeof(rig_packet));

    rig_interface.nx_interface_ip_mtu_size = 1500;
    rig_interface.nx_interface_valid = NX_TRUE;

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


    host_now = 100000;
    stub_synacks = 0;
    stub_rsts = 0;
    stub_established = 0;
    stub_last_socket = NX_NULL;
    rig_callbacks = 0;

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
    eq("the connection is established", (unsigned long) stub_established, 1);
    eq("the application is told once", (unsigned long) rig_callbacks, 1);
    ok("the listen request's socket has been taken",
       rig_listen.nx_tcp_listen_socket_ptr == NX_NULL);
    ok("and it is the socket that was parked", stub_last_socket == &rig_socket);
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

    /* The clock has moved less than one cookie counter step, so the ACK is
       still inside the window.  This is why a cached entry's sequence number
       is a cookie too: the client's handshake is not lost with the entry.  */
    ok("but the acknowledgment still completes it",
       rig_ack(77, 0x6000, iss_first) == NX_TRUE);
    eq("from the cookie alone", cache -> nx_tcp_syncache_cookies_valid, 1);
    eq("and the connection is made", (unsigned long) stub_established, 1);

    rig_reset();
    rig_listen.nx_tcp_listen_socket_ptr = NX_NULL;

    for (i = 0; i < 8; i++)
    {
        ULONG iss = rig_syn(4000 + i, 0x7000 + i);

        (void) rig_ack(4000 + i, 0x7000 + i, iss);
    }
    eq("a full backlog waits", cache -> nx_tcp_syncache_accept_count, 8);
    eq("and none of them reset the peer", (unsigned long) stub_rsts, 0);
    eq("and no socket was committed", (unsigned long) stub_established, 0);

    {
        ULONG iss = rig_syn(4008, 0x7008);

        (void) rig_ack(4008, 0x7008, iss);
    }
    eq("past the backlog the queue does not grow",
       cache -> nx_tcp_syncache_accept_count, 8);
    eq("and the peer is told, rather than left hanging",
       (unsigned long) stub_rsts, 1);

    rig_socket.nx_tcp_socket_state = NX_TCP_CLOSED;
    ok("relisten takes one", _nx_tcp_syncache_deliver(&rig_ip, &rig_listen,
                                                      &rig_socket) == NX_TRUE);
    eq("the queue shortens", cache -> nx_tcp_syncache_accept_count, 7);
    eq("and the connection reaches the application",
       (unsigned long) stub_established, 1);

    /* Unlisten gives up the rest, and resets the peers that think they are
       connected.  */
    stub_rsts = 0;
    _nx_tcp_syncache_flush(&rig_ip, 80);
    eq("unlisten empties the queue", cache -> nx_tcp_syncache_accept_count, 0);
    eq("and resets every peer waiting on it", (unsigned long) stub_rsts, 7);

    rig_reset();
    rig_listen.nx_tcp_listen_socket_ptr = NX_NULL;
    rig_listen.nx_tcp_listen_rx_window = 65536 * 4;      /* needs a scale */

    for (i = 0; i < NX_TCP_SYNCACHE_SIZE; i++)
    {
        (void) rig_syn(6000 + i, 0xA000 + i);
    }
    iss_last = rig_syn(6999, 0xB000);                    /* past full: a cookie */
    eq("the cookie SYN-ACK was sent with no socket on the port",
       cache -> nx_tcp_syncache_cookies_sent, 1);
#ifdef NX_ENABLE_TCP_WINDOW_SCALING
    ok("and it announced a window scale", stub_synack_scale > 0);
#endif

    rig_socket.nx_tcp_socket_state = NX_TCP_LISTEN_STATE;
    rig_listen.nx_tcp_listen_socket_ptr = &rig_socket;
    ok("the cookie completes", rig_ack(6999, 0xB000, iss_last) == NX_TRUE);
#ifdef NX_ENABLE_TCP_WINDOW_SCALING
    eq("with the scale the SYN-ACK announced, not the parked socket's",
       rig_socket.nx_tcp_rcv_win_scale_value, stub_synack_scale);
#endif

    rig_reset();
    rig_listen.nx_tcp_listen_socket_ptr = NX_NULL;
    {
        ULONG iss = rig_syn(88, 0xC000);

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

    if (failures != 0)
    {
        printf("syncache %s: %d failures\n", which, failures);
        return 1;
    }

    printf("syncache %s: all ok\n", which);
    return 0;
}

/*
 * AmiNetXDuo -- RFC 7323 timestamps, as arithmetic.
 *
 * Two modular comparisons decide what the option does: PAWS (is the arriving
 * TSval older than TS.Recent) and section 4.3 (does the segment reach the
 * sequence number the last acknowledgment named).  Compared as unsigned they
 * work until the counter wraps and then reject everything for the rest of the
 * connection.  The wraps are 994 days of ticks at 50 Hz and 4 GB of sequence
 * space, so only a host test that sets the counters directly reaches them.
 *
 * Real: nx_tcp_timestamp_option_get.c, nx_tcp_timestamp_option_build.c,
 * nx_tcp_timestamp_check.c.  Stubbed: the clock.
 *
 * SPDX-License-Identifier: MIT
 */

#include "nx_api.h"
#include "nx_tcp.h"
#include "nx_packet.h"

#include <stdio.h>
#include <string.h>

#ifndef NX_ENABLE_TCP_TIMESTAMPS
#error "this test is about NX_ENABLE_TCP_TIMESTAMPS and needs it defined"
#endif


/* ---------------------------------------------------------- the clock ----- */

static ULONG h_now;

ULONG _tx_time_get(VOID)
{
    return h_now;
}


/* ------------------------------------------------------------- harness ---- */

static unsigned long h_checks;
static unsigned long h_failures;

static void h_check(int ok, const char *what)
{
    h_checks++;

    if (!ok)
    {
        h_failures++;
        printf("FAIL %s\n", what);
    }
}

static void h_check_eq(ULONG got, ULONG want, const char *what)
{
    h_checks++;

    if (got != want)
    {
        h_failures++;
        printf("FAIL %s: wanted %lu, got %lu\n", what,
               (unsigned long)want, (unsigned long)got);
    }
}


/* ------------------------------------------------------------- fixture ---- */

static NX_TCP_SOCKET h_sock;
static NX_PACKET     h_pkt;
static UCHAR         h_frame[64];

/* Build a TCP header with an option area, and point the packet at it.  The
   header words are in host order, as _nx_tcp_packet_process leaves them by the
   time _nx_tcp_socket_packet_process runs; the options are raw bytes, which is
   also how the stack sees them. */
static ULONG h_segment(ULONG control_bits, const UCHAR *options, ULONG option_bytes)
{
NX_TCP_HEADER *header_ptr;
ULONG          header_length = (ULONG)sizeof(NX_TCP_HEADER) + option_bytes;
ULONG          i;

    memset(h_frame, 0, sizeof(h_frame));

    header_ptr = (NX_TCP_HEADER *)h_frame;
    header_ptr -> nx_tcp_header_word_3 = ((header_length >> 2) << NX_TCP_HEADER_SHIFT) | control_bits;

    for (i = 0; i < option_bytes; i++)
    {
        h_frame[sizeof(NX_TCP_HEADER) + i] = options[i];
    }

    h_pkt.nx_packet_prepend_ptr = h_frame;

    return header_length;
}

/* The option a peer would send, laid out as _nx_tcp_timestamp_option_build
   lays it out: two NOPs, kind, length, TSval, TSecr. */
static void h_timestamp_option(UCHAR *out, ULONG tsval, ULONG tsecr)
{
    out[0]  = NX_TCP_NOP_KIND;
    out[1]  = NX_TCP_NOP_KIND;
    out[2]  = NX_TCP_TIMESTAMP_KIND;
    out[3]  = NX_TCP_TIMESTAMP_LENGTH;
    out[4]  = (UCHAR)(tsval >> 24);
    out[5]  = (UCHAR)((tsval >> 16) & 0xFF);
    out[6]  = (UCHAR)((tsval >> 8) & 0xFF);
    out[7]  = (UCHAR)(tsval & 0xFF);
    out[8]  = (UCHAR)(tsecr >> 24);
    out[9]  = (UCHAR)((tsecr >> 16) & 0xFF);
    out[10] = (UCHAR)((tsecr >> 8) & 0xFF);
    out[11] = (UCHAR)(tsecr & 0xFF);
}

static void h_fixture(ULONG recent, ULONG last_ack_sent)
{
    memset(&h_sock, 0, sizeof(h_sock));
    memset(&h_pkt, 0, sizeof(h_pkt));

    h_now = 1000;

    h_sock.nx_tcp_socket_timestamp_enabled    = NX_TRUE;
    h_sock.nx_tcp_socket_timestamp_recent     = recent;
    h_sock.nx_tcp_socket_timestamp_recent_age = h_now;
    h_sock.nx_tcp_socket_rx_sequence_acked    = last_ack_sent;
}

/* One arriving segment: PAWS, then the record step the caller only reaches
   when the window test accepted it. */
static UINT h_arrive(ULONG control_bits, ULONG tsval, ULONG sequence)
{
UCHAR option[NX_TCP_TIMESTAMP_OPTION_SIZE];
ULONG header_length;
UINT  accepted;

    h_timestamp_option(option, tsval, 0);
    header_length = h_segment(control_bits, option, sizeof(option));

    accepted = _nx_tcp_timestamp_check(&h_sock, &h_pkt, header_length);

    if (accepted == NX_TRUE)
    {
        _nx_tcp_timestamp_record(&h_sock, sequence);
    }

    return accepted;
}


/* --------------------------------------------------------- the parser ----- */

static void t_option_get(void)
{
UCHAR option[24];
ULONG tsval, tsecr, present;

    h_timestamp_option(option, 0x11223344UL, 0x55667788UL);

    h_check_eq(_nx_tcp_timestamp_option_get(option, 12, &tsval, &tsecr, &present),
               NX_TRUE, "a well formed option is valid");
    h_check_eq(present, NX_TRUE, "a well formed option is found");
    h_check_eq(tsval, 0x11223344UL, "TSval, most significant byte first");
    h_check_eq(tsecr, 0x55667788UL, "TSecr, most significant byte first");

    /* An MSS option in front of it, which is what a SYN looks like. */
    option[0] = NX_TCP_MSS_KIND;
    option[1] = 4;
    option[2] = 0x05;
    option[3] = 0xB4;
    h_timestamp_option(&option[4], 0x0A0B0C0DUL, 0);

    h_check_eq(_nx_tcp_timestamp_option_get(option, 16, &tsval, &tsecr, &present),
               NX_TRUE, "an option behind the MSS is valid");
    h_check_eq(present, NX_TRUE, "an option behind the MSS is found");
    h_check_eq(tsval, 0x0A0B0C0DUL, "TSval behind the MSS");

    /* No option at all is not an error, and is what a peer that never offered
       it sends for the life of the connection. */
    option[0] = NX_TCP_MSS_KIND;
    option[1] = 4;
    option[2] = 0x05;
    option[3] = 0xB4;

    h_check_eq(_nx_tcp_timestamp_option_get(option, 4, &tsval, &tsecr, &present),
               NX_TRUE, "an area without the option is valid");
    h_check_eq(present, NX_FALSE, "an area without the option reports nothing");

    /* A length that is not ten is a malformed option, not a short one. */
    h_timestamp_option(option, 1, 2);
    option[3] = 8;

    h_check_eq(_nx_tcp_timestamp_option_get(option, 12, &tsval, &tsecr, &present),
               NX_FALSE, "a wrong option length is rejected");

    /* And one that runs off the end of the area is rejected rather than read
       out of whatever follows the packet. */
    h_timestamp_option(option, 1, 2);

    h_check_eq(_nx_tcp_timestamp_option_get(option, 8, &tsval, &tsecr, &present),
               NX_FALSE, "an option past the end of the area is rejected");

    /* A zero length would be an infinite loop in a walk that trusted it. */
    option[0] = 0x0F;
    option[1] = 0;

    h_check_eq(_nx_tcp_timestamp_option_get(option, 12, &tsval, &tsecr, &present),
               NX_FALSE, "a zero option length is rejected");

    /* End of list stops the walk: anything after it is padding, not options. */
    option[0] = NX_TCP_EOL_KIND;
    h_timestamp_option(&option[1], 0x99999999UL, 0);

    h_check_eq(_nx_tcp_timestamp_option_get(option, 13, &tsval, &tsecr, &present),
               NX_TRUE, "end of list is valid");
    h_check_eq(present, NX_FALSE, "nothing is read past end of list");
}


/* --------------------------------------------------------- the builder ---- */

static void t_option_build(void)
{
UCHAR option[NX_TCP_TIMESTAMP_OPTION_SIZE];

    memset(&h_sock, 0, sizeof(h_sock));
    memset(option, 0xEE, sizeof(option));

    h_now = 0x01020304UL;

    /* A connection that did not negotiate the option writes nothing. */
    h_sock.nx_tcp_socket_timestamp_enabled = NX_FALSE;
    h_check_eq(_nx_tcp_timestamp_option_build(&h_sock, option), 0,
               "no option for a connection that did not negotiate it");
    h_check_eq(option[0], 0xEE, "and nothing written");

    h_sock.nx_tcp_socket_timestamp_enabled = NX_TRUE;
    h_sock.nx_tcp_socket_timestamp_recent  = 0xDEADBEEFUL;

    h_check_eq(_nx_tcp_timestamp_option_build(&h_sock, option),
               NX_TCP_TIMESTAMP_OPTION_SIZE, "twelve bytes");
    h_check_eq(option[0], NX_TCP_NOP_KIND, "NOP");
    h_check_eq(option[1], NX_TCP_NOP_KIND, "NOP");
    h_check_eq(option[2], NX_TCP_TIMESTAMP_KIND, "kind 8");
    h_check_eq(option[3], NX_TCP_TIMESTAMP_LENGTH, "length 10");
    h_check_eq(((ULONG)option[4] << 24) | ((ULONG)option[5] << 16) |
               ((ULONG)option[6] << 8) | (ULONG)option[7],
               0x01020304UL, "TSval is the clock now");
    h_check_eq(((ULONG)option[8] << 24) | ((ULONG)option[9] << 16) |
               ((ULONG)option[10] << 8) | (ULONG)option[11],
               0xDEADBEEFUL, "TSecr is TS.Recent");
}


/* ---------------------------------------------------------------- PAWS ---- */

static void t_paws(void)
{
    /* Newer than TS.Recent: accepted, and it moves TS.Recent. */
    h_fixture(1000, 500);
    h_check_eq(h_arrive(NX_TCP_ACK_BIT, 1001, 500), NX_TRUE, "a newer timestamp is accepted");
    h_check_eq(h_sock.nx_tcp_socket_timestamp_recent, 1001, "and becomes TS.Recent");

    /* Equal: accepted.  Section 5.3 R1 is strictly older, and a peer whose
       clock has not ticked between two segments sends both with the same
       value. */
    h_fixture(1000, 500);
    h_check_eq(h_arrive(NX_TCP_ACK_BIT, 1000, 500), NX_TRUE, "an equal timestamp is accepted");

    /* Older: rejected. */
    h_fixture(1000, 500);
    h_check_eq(h_arrive(NX_TCP_ACK_BIT, 999, 500), NX_FALSE, "an older timestamp is rejected");
    h_check_eq(h_sock.nx_tcp_socket_timestamp_recent, 1000, "and TS.Recent does not move");

    /* Across the wrap, both ways.  As unsigned the arriving value is four
       billion less than TS.Recent; as a signed difference it is 356 more. */
    h_fixture(0xFFFFFF00UL, 500);
    h_check_eq(h_arrive(NX_TCP_ACK_BIT, 0x00000100UL, 500), NX_TRUE,
               "a timestamp past the wrap is accepted");
    h_check_eq(h_sock.nx_tcp_socket_timestamp_recent, 0x00000100UL,
               "and becomes TS.Recent");

    h_fixture(0x00000100UL, 500);
    h_check_eq(h_arrive(NX_TCP_ACK_BIT, 0xFFFFFF00UL, 500), NX_FALSE,
               "a timestamp from before the wrap is rejected");

    /* Half the counter away is where the signed difference turns over. */
    h_fixture(0, 500);
    h_check_eq(h_arrive(NX_TCP_ACK_BIT, 0x7FFFFFFFUL, 500), NX_TRUE,
               "just under half the counter ahead is newer");

    h_fixture(0, 500);
    h_check_eq(h_arrive(NX_TCP_ACK_BIT, 0x80000001UL, 500), NX_FALSE,
               "just over half the counter ahead reads as older");

    /* Section 5.2: a reset is exempt. */
    h_fixture(1000, 500);
    h_check_eq(h_arrive(NX_TCP_RST_BIT, 1, 500), NX_TRUE, "a reset is exempt from PAWS");

    /* Section 5.5: after 24 days quiet TS.Recent rejects nothing. */
    h_fixture(1000, 500);
    h_now += NX_TCP_TIMESTAMP_IDLE_LIMIT + 1;
    h_check_eq(h_arrive(NX_TCP_ACK_BIT, 999, 500), NX_TRUE,
               "an outdated TS.Recent rejects nothing");

    /* One tick under the limit it still does. */
    h_fixture(1000, 500);
    h_now += NX_TCP_TIMESTAMP_IDLE_LIMIT;
    h_check_eq(h_arrive(NX_TCP_ACK_BIT, 999, 500), NX_FALSE,
               "and one tick sooner it still rejects");

    /* A connection that never negotiated the option is not subject to any of
       this, whatever a segment happens to carry. */
    h_fixture(1000, 500);
    h_sock.nx_tcp_socket_timestamp_enabled = NX_FALSE;
    h_check_eq(h_arrive(NX_TCP_ACK_BIT, 1, 500), NX_TRUE,
               "a connection without the option is not subject to PAWS");
    h_check_eq(h_sock.nx_tcp_socket_timestamp_recent, 1000,
               "and TS.Recent does not move");

    /* A segment carrying no option is taken.  Section 3.2 SHOULDs a silent
       drop; that stalls wherever something in the path strips options. */
    {
    UCHAR option[4];
    ULONG header_length;

        h_fixture(1000, 500);

        option[0] = NX_TCP_NOP_KIND;
        option[1] = NX_TCP_NOP_KIND;
        option[2] = NX_TCP_NOP_KIND;
        option[3] = NX_TCP_NOP_KIND;
        header_length = h_segment(NX_TCP_ACK_BIT, option, sizeof(option));

        h_check_eq(_nx_tcp_timestamp_check(&h_sock, &h_pkt, header_length), NX_TRUE,
                   "a segment without the option is accepted");
        h_check_eq(h_sock.nx_tcp_socket_timestamp_seen, NX_FALSE,
                   "and nothing is carried forward from it");

        /* And so is one with no option area at all. */
        header_length = h_segment(NX_TCP_ACK_BIT, option, 0);
        h_check_eq(_nx_tcp_timestamp_check(&h_sock, &h_pkt, header_length), NX_TRUE,
                   "a segment with no options at all is accepted");
    }
}


/* ------------------------------------------------ which one moves it ------ */

static void t_record(void)
{
    /* Section 4.3 R1, first condition.  PAWS rejects such a segment before it
       gets here, so the fields are set directly: what is asserted is that the
       guard exists. */
    h_fixture(1000, 500);
    h_sock.nx_tcp_socket_timestamp_seen  = NX_TRUE;
    h_sock.nx_tcp_socket_timestamp_value = 999;
    _nx_tcp_timestamp_record(&h_sock, 500);
    h_check_eq(h_sock.nx_tcp_socket_timestamp_recent, 1000,
               "an older TSval does not move TS.Recent");

    /* Except after section 5.5's 24 days: a value the peer's clock has wrapped
       past is newer than everything it will send again, so keeping the ordering
       test here would leave TS.Recent stuck for the rest of the connection. */
    h_fixture(1000, 500);
    h_now += NX_TCP_TIMESTAMP_IDLE_LIMIT + 1;
    h_check_eq(h_arrive(NX_TCP_ACK_BIT, 999, 500), NX_TRUE,
               "an older TSval is accepted once TS.Recent has aged out");
    h_check_eq(h_sock.nx_tcp_socket_timestamp_recent, 999,
               "and replaces it");

    /* Second condition: SEG.SEQ at or below the sequence number the last
       acknowledgment named. */
    h_fixture(1000, 500);
    h_check_eq(h_arrive(NX_TCP_ACK_BIT, 2000, 500), NX_TRUE, "in sequence is accepted");
    h_check_eq(h_sock.nx_tcp_socket_timestamp_recent, 2000, "and moves TS.Recent");

    /* Below it: data already acknowledged, a retransmission the peer sent
       because our acknowledgment was lost.  Its timestamp is current. */
    h_fixture(1000, 500);
    h_check_eq(h_arrive(NX_TCP_ACK_BIT, 2000, 400), NX_TRUE, "behind the edge is accepted");
    h_check_eq(h_sock.nx_tcp_socket_timestamp_recent, 2000, "and moves TS.Recent");

    /* One byte above it is a segment queued behind a hole, and it must not
       move TS.Recent: the duplicate acknowledgments keep echoing the segment
       that opened the hole. */
    h_fixture(1000, 500);
    h_check_eq(h_arrive(NX_TCP_ACK_BIT, 2000, 501), NX_TRUE, "out of order is accepted");
    h_check_eq(h_sock.nx_tcp_socket_timestamp_recent, 1000,
               "and does NOT move TS.Recent");

    /* Modular too.  Last.ACK.sent just past the sequence wrap and a segment
       just below it: unsigned, four billion ahead; in fact 100 bytes behind. */
    h_fixture(1000, 0x00000064UL);
    h_check_eq(h_arrive(NX_TCP_ACK_BIT, 2000, 0xFFFFFFFFUL), NX_TRUE,
               "a segment from before the sequence wrap is accepted");
    h_check_eq(h_sock.nx_tcp_socket_timestamp_recent, 2000,
               "and moves TS.Recent, being behind the edge");

    /* And the other way round. */
    h_fixture(1000, 0xFFFFFFFFUL);
    h_check_eq(h_arrive(NX_TCP_ACK_BIT, 2000, 0x00000064UL), NX_TRUE,
               "a segment past the sequence wrap is accepted");
    h_check_eq(h_sock.nx_tcp_socket_timestamp_recent, 1000,
               "and does not move TS.Recent, being ahead of the edge");

    /* Section 5.5's 24 days count from the last time TS.Recent moved, not from
       the last segment. */
    h_fixture(1000, 500);
    h_now = 777777;
    h_check_eq(h_arrive(NX_TCP_ACK_BIT, 2000, 500), NX_TRUE, "accepted");
    h_check_eq(h_sock.nx_tcp_socket_timestamp_recent_age, 777777,
               "TS.Recent is stamped when it moves");

    h_fixture(1000, 500);
    h_now = 777777;
    h_check_eq(h_arrive(NX_TCP_ACK_BIT, 2000, 501), NX_TRUE, "accepted out of order");
    h_check(h_sock.nx_tcp_socket_timestamp_recent_age != 777777,
            "and is not stamped when it does not");
}


/* ------------------------------------------------------------- the clock -- */

static void t_clock(void)
{
    /* Section 4.2.1 wants 1 ms to 1 s per tick.  The nx_tcp.h #error covers the
       fast end; this covers the slow end. */
    h_check(NX_IP_PERIODIC_RATE >= 1, "the timestamp clock ticks at least once a second");
    h_check(NX_IP_PERIODIC_RATE <= 1000, "and no more than a thousand times");

    /* Section 5.5's 24 days must fit in a ULONG of ticks. */
    h_check(NX_TCP_TIMESTAMP_IDLE_LIMIT / 86400UL == 24UL * (ULONG)NX_IP_PERIODIC_RATE,
            "24 days of ticks is 24 days of ticks");
    h_check(NX_TCP_TIMESTAMP_IDLE_LIMIT > 0, "and has not overflowed a ULONG");
}


int main(void)
{
    t_option_get();
    t_option_build();
    t_paws();
    t_record();
    t_clock();

    printf("%lu checks, %lu failures\n", h_checks, h_failures);

    return (h_failures == 0) ? 0 : 1;
}

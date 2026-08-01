/*
 * AmiNetXDuo -- the internet checksum, in the form the 68k actually has.
 *
 * third_party/netxduo/common/src/nx_ip_checksum_compute.c walks the payload a
 * longword at a time and splits each one into two 16-bit halves by hand:
 *
 *     checksum += (*long_ptr & NX_LOWER_16_MASK);
 *     checksum += (*long_ptr >> NX_SHIFT_BY_16);
 *
 * which GCC 15.2 -O2 -m68020 turns into seven instructions per longword
 * (move.l (An)+,Dn / move.l / andi.l #65535 / clr.w / swap / add.l / add.l)
 * plus the dbf.  That is the price of expressing a carry in a language that
 * has no carry flag.
 *
 * The machine has one.  A one's-complement sum is add.l followed by addx.l of
 * a zero register -- two instructions per longword, no immediate, no swap, and
 * the operand comes straight out of (An)+ with no separate load.  The 32-bit
 * sum that produces is congruent to the 16-bit one modulo 0xFFFF, so folding
 * it at the end gives the same checksum; see n68k_sum_longwords().
 *
 * Everything else -- the pseudo-header arithmetic, the chain walk, the
 * end-pointer rounding, the two-byte carry across a packet boundary whose
 * append pointer is 2 mod 4, and the trailing 1/2/3-byte case including its
 * zero-write into the pad byte -- is structurally identical to the vendored
 * code.  The requirement is to return exactly what NetX Duo would have
 * returned, not merely a correct internet checksum.  tests/perf/host/ checks
 * that differentially against the vendored function compiled under a different
 * name, over random buffers, every length from 0 to 64, every start alignment
 * and multi-packet chains.
 *
 * SPDX-License-Identifier: MIT
 */

#include "net68k.h"

#include "nx_ip.h"


/* ------------------------------------------------------------ the loop --- */

#ifndef AMINETXDUO_NET68K_ASM

/*
 * The portable fallback: the same carry chain the assembly does, which GCC
 * compiles to
 *
 *     movea.l (a1)+,a0 / add.l a0,d0 / cmp.l a0,d0 / bcc.s / addq.l #1,d0
 *
 * -- four instructions and a branch where the machine needs two, still better
 * than the vendored seven.  Selecting the assembly is a build option
 * (AMINETXDUO_NET68K_ASM) and not a #if on the target, because a host build
 * has to compile this file to run the host tier of the differential test.
 */
ULONG n68k_sum_longwords(const ULONG *p, ULONG count)
{

ULONG   acc = 0;


    while (count != 0UL)
    {
    ULONG   w = *p++;

        acc += w;
        if (acc < w)
        {
            acc++;                      /* end-around carry */
        }

        count--;
    }

    return(acc);
}

/*
 * The melded copy, in portable form.  The assembly is what earns anything
 * here -- it feeds the adds from the movem.l the copy already does -- but the
 * contract is the same either way: copy, and return what n68k_sum_longwords()
 * would have returned over the source.
 */
ULONG n68k_copy_sum_longwords(ULONG *to, const ULONG *from, ULONG count)
{

ULONG   acc = 0;


    while (count != 0UL)
    {
    ULONG   w = *from++;

        *to++ = w;

        acc += w;
        if (acc < w)
        {
            acc++;                      /* end-around carry */
        }

        count--;
    }

    return(acc);
}

#endif /* AMINETXDUO_NET68K_ASM */


/* Fold a 32-bit one's-complement sum down to 16 bits. */
static ULONG n68k_fold(ULONG sum)
{

    sum =  (sum >> 16) + (sum & 0xFFFFUL);
    sum =  (sum >> 16) + (sum & 0xFFFFUL);

    return(sum);
}


/* ----------------------------------------------------------- the stash --- */

#ifdef AMINETXDUO_RX_COPY_SUM

N68kRxStats  n68k_rx_stats;

VOID n68k_rx_stash_invalidate(NX_PACKET *packet)
{

    N68K_RX_SENTINEL(packet) =  0UL;
}

VOID n68k_rx_copy_stash(NX_PACKET *packet, UCHAR *to, const UCHAR *from,
                        ULONG len)
{

ULONG   words =  len >> 2;
ULONG   tail  =  len & 3UL;
ULONG   sum;


    sum =  n68k_copy_sum_longwords((ULONG *)to, (const ULONG *)from, words);

    if (tail != 0UL)
    {

    const UCHAR    *s =  from + (words << 2);
    UCHAR          *d =  to + (words << 2);
    ULONG           i;

    union
    {
        ULONG   l;
        UCHAR   b[4];
    } w;


        /*
         * What lies past `len` in the device's buffer is the previous frame,
         * not zeroes, so the last longword is built out of the bytes that
         * actually arrived and padded with zeroes -- which is what the walk
         * below does with a partial trailing longword, for all three of its
         * lengths.
         *
         * Assembled through the byte array rather than by shifting: the walk
         * sums whole longwords in host order and byte swaps once at the end,
         * so what matters is where the bytes sit in memory, not what they are
         * worth.  The two agree on the 68k either way and disagree on a
         * little-endian host, which is where the differential runs.
         */
        w.l =  0UL;

        for (i = 0UL; i < tail; i++)
        {
            d[i]    =  s[i];
            w.b[i]  =  s[i];
        }

        sum +=  w.l;
        if (sum < w.l)
        {
            sum++;                      /* end-around carry */
        }
    }

    N68K_RX_SUM(packet)      =  sum;
    N68K_RX_START(packet)    =  N68K_RX_ADDR(to);
    N68K_RX_SENTINEL(packet) =  N68K_RX_STASH_MAGIC ^ len;

    n68k_rx_stats.stamped++;
}

#endif /* AMINETXDUO_RX_COPY_SUM */


/* --------------------------------------------------------- the compute --- */

USHORT n68k_ip_checksum_compute(NX_PACKET *packet_ptr, ULONG protocol,
                                UINT data_length, ULONG *src_ip_addr,
                                ULONG *dest_ip_addr)
{

ULONG       checksum = 0;
USHORT      tmp;
USHORT     *short_ptr;
ULONG      *long_ptr;
#ifndef NX_DISABLE_PACKET_CHAIN
ULONG       packet_size;
#endif
NX_PACKET  *current_packet;
ALIGN_TYPE  end_ptr;
#ifdef FEATURE_NX_IPV6
UINT        i;
#endif


    /* TCP, UDP and ICMPv6 cover a pseudo header; ICMPv4 does not. */
    if ((protocol == NX_PROTOCOL_UDP) ||
#ifdef FEATURE_NX_IPV6
        (protocol == NX_PROTOCOL_ICMPV6) ||
#endif
        (protocol == NX_PROTOCOL_TCP))
    {

    USHORT *src_ip_short, *dest_ip_short;


        checksum =  protocol;

        NX_ASSERT((src_ip_addr != NX_NULL) && (dest_ip_addr != NX_NULL));

        src_ip_short  =  (USHORT *)src_ip_addr;
        dest_ip_short =  (USHORT *)dest_ip_addr;

        checksum +=  src_ip_short[0];
        checksum +=  src_ip_short[1];
        checksum +=  dest_ip_short[0];
        checksum +=  dest_ip_short[1];

#ifdef FEATURE_NX_IPV6
        /* An IPv6 address is 128 bits, not 32. */
        if (packet_ptr -> nx_packet_ip_version == NX_IP_VERSION_V6)
        {
            for (i = 2; i < 8; i++)
            {
                checksum +=  dest_ip_short[i];
                checksum +=  src_ip_short[i];
            }
        }
#endif

        checksum +=  data_length;

        checksum =  n68k_fold(checksum);

        /* The payload below is summed in network order; this is not. */
        tmp =  (USHORT)checksum;
        NX_CHANGE_USHORT_ENDIAN(tmp);
        checksum =  tmp;
    }

#ifdef AMINETXDUO_RX_COPY_SUM

    /*
     * The receive short-circuit.  ami_sana2_copy_to_buff() summed this frame
     * while copying it out of the device's buffer, so the walk below would be
     * a second pass over the same bytes.  The pseudo header above is still
     * this function's own work -- only the payload pass is skipped.
     *
     * TCP and UDP only.  The IPv4 header's own checksum comes through here
     * with no addresses at all and falls through on that; ICMP and ICMPv6 are
     * declined by the protocol, which also means the running checksum is
     * never zero at this point -- the pseudo header carries at least a
     * protocol number and a length -- and that is what makes one's-complement
     * -0 and +0 the same answer here rather than an accept against a drop.
     *
     * A chain is rejected outright: reassembly links fragments through
     * nx_packet_next (nx_ip_fragment_assembly.c), and their sums would have
     * to be combined rather than read.
     *
     * Consumed on the way out.  A packet is checksummed once on receive, and
     * clearing it here is what stops the same sentinel answering a transmit
     * call after the buffer has been round the pool.
     */
    if (N68K_RX_SENTINEL(packet_ptr) == 0UL)
    {
        n68k_rx_stats.miss_none++;
    }
#ifndef NX_DISABLE_PACKET_CHAIN
    else if (packet_ptr -> nx_packet_next != NX_NULL)
    {
        n68k_rx_stats.miss_chained++;
    }
#endif
    else if (((protocol != NX_PROTOCOL_TCP) && (protocol != NX_PROTOCOL_UDP)) ||
             (src_ip_addr == NX_NULL) || (dest_ip_addr == NX_NULL))
    {
        n68k_rx_stats.miss_protocol++;
    }
    else
    {

    /*
     * The stash covers the frame from the IP header on and this call covers a
     * suffix of it, so the answer is the stash's sum less the sum of the
     * prefix in front of the prepend pointer.  `start` is longword aligned
     * and a header length is a multiple of 4, so the two runs share one
     * longword grid and ceil(bytes/4) - prefix/4 is exactly
     * ceil(data_length/4): one subtraction, no parity case, no byte swap.
     *
     * `bytes` is not stored twice.  It is what the stash would have to have
     * covered for this call to be the suffix, and the sentinel says whether
     * it was -- which is also the check that a frame carrying Ethernet
     * padding fails, its copied length being longer than its datagram.
     */
    ULONG   prefix =  N68K_RX_ADDR(packet_ptr -> nx_packet_prepend_ptr) -
                      N68K_RX_START(packet_ptr);
    ULONG   ahead  =  (ULONG)(packet_ptr -> nx_packet_prepend_ptr -
                              packet_ptr -> nx_packet_data_start);


        if (((prefix & 3UL) != 0UL) || (prefix > ahead))
        {
            n68k_rx_stats.miss_prefix++;
        }
        else if (N68K_RX_SENTINEL(packet_ptr) !=
                 (N68K_RX_STASH_MAGIC ^ (prefix + (ULONG)data_length)))
        {
            n68k_rx_stats.miss_length++;
        }
        else
        {

        ULONG   head =  n68k_sum_longwords(
                            (const ULONG *)(packet_ptr -> nx_packet_prepend_ptr -
                                            prefix), prefix >> 2);


            N68K_RX_SENTINEL(packet_ptr) =  0UL;
            n68k_rx_stats.used++;

            checksum +=  n68k_fold(N68K_RX_SUM(packet_ptr)) +
                         (0xFFFFUL - n68k_fold(head));

            checksum =   n68k_fold(checksum);

            tmp =  (USHORT)checksum;
            NX_CHANGE_USHORT_ENDIAN(tmp);

            return(tmp);
        }
    }

#endif /* AMINETXDUO_RX_COPY_SUM */

    long_ptr       =  (ULONG *)packet_ptr -> nx_packet_prepend_ptr;
    current_packet =  packet_ptr;

#ifndef NX_DISABLE_PACKET_CHAIN
    while (current_packet)
    {

        packet_size =  (ULONG)(current_packet -> nx_packet_append_ptr -
                               current_packet -> nx_packet_prepend_ptr);

        if (data_length > (UINT)packet_size)
        {
            end_ptr =  ((ALIGN_TYPE)current_packet -> nx_packet_append_ptr) &
                       (ALIGN_TYPE)(~3);
        }
        else
        {
#endif /* NX_DISABLE_PACKET_CHAIN */
            end_ptr =  (ALIGN_TYPE)current_packet -> nx_packet_prepend_ptr +
                       data_length - 3;
#ifndef NX_DISABLE_PACKET_CHAIN
        }
#endif

        long_ptr =  (ULONG *)current_packet -> nx_packet_prepend_ptr;

        if ((ALIGN_TYPE)long_ptr < end_ptr)
        {

        ULONG   consumed;
        ULONG   words;


            consumed =  (ULONG)(((end_ptr + 3) & (ALIGN_TYPE)(~3)) -
                                (ALIGN_TYPE)long_ptr);

            data_length -=  (UINT)consumed;

            /*
             * The vendored loop is `while (long_ptr < end_ptr) long_ptr++`,
             * which is ceil((end_ptr - long_ptr) / 4) iterations.  That equals
             * consumed/4 whenever the prepend pointer is longword aligned --
             * always, here -- but it is computed rather than assumed, so a
             * pool that handed out an odd prepend pointer would still sum the
             * same bytes the vendored code sums.
             */
            words =  ((ULONG)(end_ptr - (ALIGN_TYPE)long_ptr) + 3UL) >> 2;

            checksum +=  n68k_fold(n68k_sum_longwords(long_ptr, words));

            long_ptr +=  words;
        }

#ifndef NX_DISABLE_PACKET_CHAIN
        if ((data_length > 0) && (current_packet -> nx_packet_next))
        {

            /* Two bytes left over at a chain boundary belong to this packet. */
            if ((((ALIGN_TYPE)current_packet -> nx_packet_append_ptr) & 3) == 2)
            {
                short_ptr    =  (USHORT *)long_ptr;
                checksum    +=  *short_ptr;
                data_length -=  2;
            }

            current_packet =  current_packet -> nx_packet_next;
        }
        else
        {
            current_packet =  NX_NULL;
        }
    }
#endif /* NX_DISABLE_PACKET_CHAIN */

    /* One, two or three bytes may be left; the odd byte is zero padded. */
    if (data_length)
    {

        short_ptr =  (USHORT *)(long_ptr);

        if (data_length == 1)
        {
            *((UCHAR *)short_ptr + 1) =  0;
        }
        else if (data_length == 3)
        {
            checksum +=  *short_ptr;
            short_ptr++;

            *((UCHAR *)short_ptr + 1) =  0;
        }

        checksum +=  *short_ptr;
    }

    checksum =  n68k_fold(checksum);

    tmp =  (USHORT)checksum;
    NX_CHANGE_USHORT_ENDIAN(tmp);

    return(tmp);
}

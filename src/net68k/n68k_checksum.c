/*
 * AmiNetXDuo, the internet checksum, in the form the 68k actually has.
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
 * a zero register, two instructions per longword, no immediate, no swap, and
 * the operand comes straight out of (An)+ with no separate load.  The 32-bit
 * sum that produces is congruent to the 16-bit one modulo 0xFFFF, so folding
 * it at the end gives the same checksum; see n68k_sum_longwords().
 *
 * Everything else, the pseudo-header arithmetic, the chain walk, the
 * end-pointer rounding, the two-byte carry across a packet boundary whose
 * append pointer is 2 mod 4, and the trailing 1/2/3-byte case including its
 * zero-write into the pad byte, is structurally identical to the vendored
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
 * four instructions and a branch where the machine needs two, still better
 * than the vendored seven.  Selecting the assembly is a build option
 * (AMINETXDUO_NET68K_ASM) and not a #if on the target, because a host build
 * has to compile this file to run the host tier of the differential test.
 */
N68K_NO_SANITIZE_ALIGNMENT
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

#endif /* AMINETXDUO_NET68K_ASM */


/* Fold a 32-bit one's-complement sum down to 16 bits. */
static ULONG n68k_fold(ULONG sum)
{

    sum =  (sum >> 16) + (sum & 0xFFFFUL);
    sum =  (sum >> 16) + (sum & 0xFFFFUL);

    return(sum);
}


/* --------------------------------------------------------- the compute --- */

/*
 * The melded copy, in portable form.  The assembly is what earns anything
 * here -- it feeds the adds from the movem.l the copy already does -- but the
 * contract is the same either way: copy, and return what n68k_sum_longwords()
 * would have returned over the source.
 */
#ifndef AMINETXDUO_NET68K_ASM
/* The assembly counterpart is in n68k_checksum.S; same guard as
   n68k_sum_longwords above, so a host build still has one. */
N68K_NO_SANITIZE_ALIGNMENT
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


N68K_NO_SANITIZE_ALIGNMENT
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
             * consumed/4 whenever the prepend pointer is longword aligned,
             * always, here, but it is computed rather than assumed, so a
             * pool that handed out an odd prepend pointer would still sum the
             * same bytes the vendored code sums.
             */
            words =  ((ULONG)(end_ptr - (ALIGN_TYPE)long_ptr) + 3UL) >> 2;

            checksum +=  n68k_fold(N68K_SUM_LONGWORDS(long_ptr, words));

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

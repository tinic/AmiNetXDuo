/*
 * AmiNetXDuo, receive checksum checks in the driver glue.
 *
 * The fork requires the per-packet nx_packet_interface_capability_flag as well
 * as the interface capability, so a frame this file does not clear is checked
 * by the stack exactly as before.
 *
 * The core owns the lifetime of the per-packet flag: _nx_packet_allocate() and
 * _nx_packet_release() both clear it.  This file sets bits only on a frame it
 * has just checked.
 *
 * SPDX-License-Identifier: MIT
 */

#include "net68k.h"

#include "nx_api.h"
#include "nx_ip.h"

#ifdef AMINETXDUO_RX_VERIFY

/* Big-endian reads: the header has not been byte swapped yet.  This runs
   before nx_ipv4_packet_receive(), so a frame is cleared or declined before
   the stack looks at it.

   Byte at a time, so they are legal at ANY address and on any endianness --
   the host tier compiles this file.  Where the offset is provably even, use
   N68K_RDW16/N68K_RDW32 below instead. */
#define N68K_RD16(p)    ((ULONG)(((ULONG)(p)[0] << 8) | (ULONG)(p)[1]))
#define N68K_RD32(p)    ((ULONG)(((ULONG)(p)[0] << 24) | ((ULONG)(p)[1] << 16) | \
                                 ((ULONG)(p)[2] << 8)  |  (ULONG)(p)[3]))

/*
 * THE SAME READS, ONE INSTRUCTION EACH, WHERE THE ADDRESS ALLOWS IT.
 *
 * A big-endian machine that can load a word from an even address needs no
 * assembly at all: the byte macros above cost four loads, three shifts and
 * three ORs for a longword where a 68000 does it in one or two moves, and
 * _n68k_rx_verify_sum carries 2.5% of the real-path receive profile for what
 * is otherwise twenty bytes of arithmetic.
 *
 * THE EVENNESS IS NOT AN ASSUMPTION THIS FILE INTRODUCES.  The IPv4 fast path
 * below already hands `ip` to N68K_SUM_LONGWORDS() as a `const ULONG *`, which
 * a 68000 answers with an address error unless it is a multiple of four -- so
 * the path is longword-aligned or it was already broken.  It is: the receive
 * packet's prepend_ptr is the pool payload plus AMI_SANA2_RX_PAD (2) plus the
 * 14-byte Ethernet header, which is 16 (lance.c:70).  n68k_rxv_even() states
 * it anyway and sends anything else to the ordinary byte-at-a-time path,
 * because a loopback or raw frame does not come from that arithmetic.
 */
#if defined(__mc68000__) || defined(__m68k__)
#define N68K_RDW16(p)   ((ULONG)*(const USHORT *)(const void *)(p))
#define N68K_RDW32(p)   ((ULONG)*(const ULONG *)(const void *)(p))
#else
#define N68K_RDW16(p)   N68K_RD16(p)
#define N68K_RDW32(p)   N68K_RD32(p)
#endif

/*
 * THE COUNTERS COST MORE THAN THEY EARN ON THE TARGET.
 *
 * n68k_rx_verify_stats is read in exactly one place --
 * ami_sana2_rxprobe_report() (sana2_rx.c:386) -- whose only call site is
 * inside #ifdef AMINETXDUO_RXPROBE (sana2_rx.c:2021).  A shipping build ran
 * all forty increments and never read one.  Four are on the fused fast path,
 * once a frame: ip_ok, transport_ok, from_copy, v4_fused.  Each is a
 * read-modify-write of a global, and the removal prize prices this block at
 * +3.40% for ~118 instructions, so an instruction here is ~0.029% of receive.
 *
 * THE HOST TIER KEEPS THEM, BECAUSE IT ASSERTS ON THEM:
 * test_rxverify_host.c:1037 and :1046 check from_copy and v4_fused to prove
 * the FUSED path was the one that ran, which is a property no other test can
 * see.  So the gate is the TARGET, not the feature -- the host build always
 * counts, and an m68k RXPROBE build still does.
 */
#if defined(AMINETXDUO_RXPROBE) || \
    !(defined(__mc68000__) || defined(__m68k__))
#define N68K_RXV_COUNT(f)   (n68k_rx_verify_stats.f++)
#else
#define N68K_RXV_COUNT(f)   ((VOID)0)
#endif

/* Longword-aligned, which is what both of the above need on a 68000: the word
   form is only ever used at an even offset from this same pointer. */
static UINT n68k_rxv_even(const UCHAR *p)
{
    return ((((ALIGN_TYPE)p) & 3UL) == 0UL) ? NX_TRUE : NX_FALSE;
}

N68kRxVerifyStats  n68k_rx_verify_stats;

/*
 * One checksum, through the same function the stack uses, with the packet
 * presented the way that layer expects to see it.  The caller restores the
 * packet fields.  Nothing here leaves the packet modified after it returns.
 */
static UINT n68k_rx_sum_ok(NX_PACKET *packet, ULONG protocol, UINT length,
                           ULONG *src, ULONG *dst)
{
ULONG   checksum;

    checksum =  _nx_ip_checksum_compute(packet, protocol, length, src, dst);

    return ((~checksum & NX_LOWER_16_MASK) == 0UL) ? NX_TRUE : NX_FALSE;
}

#ifdef FEATURE_NX_IPV6

/* The extension headers, by next-header value. */
#define N68K_V6_HOPOPT      0U      /* hop-by-hop options               */
#define N68K_V6_ROUTING     43U     /* routing                          */
#define N68K_V6_FRAGMENT    44U     /* fragment                         */
#define N68K_V6_ESP         50U     /* encapsulating security payload   */
#define N68K_V6_AH          51U     /* authentication                   */
#define N68K_V6_NONEXT      59U     /* no next header                   */
#define N68K_V6_DSTOPT      60U     /* destination options              */

/* A chain longer than this is refused rather than walked.  RFC 8200 puts at
   most one of each header in a datagram, bar destination options, which may
   appear twice; eight is slack and still a bound on interrupt-time work. */
#define N68K_V6_MAX_EXT     8U

/*
 * The destination options header, looked at option by option.
 *
 * ONE option is the reason this exists.  RFC 6275 6.3 puts a mobile node's
 * home address in a destination option and says the upper-layer checksum
 * carries THAT address in the pseudo header, not the care-of address in the
 * source field of the frame.  A verifier that did not look would compute a
 * different sum and drop a packet that is correct, which is worse than
 * declining it.  So a destination options header carrying one is declined,
 * and the walk over the options also catches a length that overruns.
 *
 * `opt` is the header past its next-header and length bytes, `bytes` what
 * those two bytes say is left.
 */
static UINT n68k_rxv6_dstopt_plain(const UCHAR *opt, ULONG bytes)
{
ULONG   i =  0UL;

    while (i < bytes)
    {
        if (opt[i] == 0U)               /* Pad1: one byte, no length */
        {
            i++;
            continue;
        }

        if ((i + 2UL) > bytes)
        {
            return (NX_FALSE);
        }

        if (opt[i] == 201U)             /* Home Address */
        {
            return (NX_FALSE);
        }

        i +=  2UL + (ULONG)opt[i + 1UL];
    }

    /* An option whose length runs past the header ends the walk beyond
       `bytes`, which is a header that lies about itself. */
    return ((i == bytes) ? NX_TRUE : NX_FALSE);
}

/*
 * IPv6 has no header checksum, so there is no header verdict and no IPv4 bit
 * to publish -- only the transport.  What this decides is where the transport
 * header starts and what upper-layer length the pseudo header carries, which
 * for IPv6 means walking the extension header chain: the payload length in the
 * fixed header counts the extension headers, and the pseudo header does not.
 *
 * *offset is where the transport header starts, *payload the upper-layer
 * length, *protocol the upper-layer protocol.
 *
 * A DECLINE HERE IS NOT A VERDICT ON THE FRAME.  The stack checks whatever
 * this refuses, exactly as before, so the only wrong answer this function can
 * give is NX_TRUE with the wrong offset, length or protocol.  Every header
 * below is therefore walked only where the walk is exact:
 *
 *   hop-by-hop, destination options   TLV, walked; the jumbogram option is
 *                                     caught by the zero payload length, and
 *                                     the home address option by the scan
 *                                     above
 *   routing                           only with segments left zero.  With
 *                                     segments left, the frame's destination
 *                                     is not the one the sender put in the
 *                                     pseudo header (RFC 8200 4.4)
 *   fragment                          only an atomic fragment, offset zero
 *                                     and no more fragments.  Any other and
 *                                     the checksum covers the reassembled
 *                                     datagram, which one frame cannot see.
 *                                     _nx_ipv6_process_fragment_option()
 *                                     returns NX_CONTINUE for exactly this
 *                                     case, so the packet that reaches the
 *                                     transport is the one checked here
 *   authentication                    walked; its length is in 4-byte units,
 *                                     unlike every other header here, and it
 *                                     does not change the transport checksum
 *   ESP                               declined: the transport header is
 *                                     inside the encrypted payload
 *   no next header                    declined: there is no transport
 *   anything else                     declined, including the mobility
 *                                     header, HIP and shim6, which are upper
 *                                     layers this file does not check
 */
static UINT n68k_rxv6_shape(const UCHAR *ip, ULONG length, UINT *protocol,
                            UINT *offset, UINT *payload)
{
ULONG   plen;
ULONG   end;
ULONG   at =  40UL;
ULONG   hlen;
UINT    walked =  0U;         /* only to bound the loop */
UINT    next;

    if (length < 40UL)
    {
        N68K_RXV_COUNT(skip_short);
        return (NX_FALSE);
    }

    plen =  N68K_RD16(&ip[4]);

    /* Zero is a jumbogram, whose real length is in a hop-by-hop option this
       does not read. */
    if ((plen == 0UL) || ((plen + 40UL) > length))
    {
        N68K_RXV_COUNT(skip_length);
        return (NX_FALSE);
    }

    end  =  40UL + plen;
    next =  (UINT)ip[6];

    /* ---- the chain ------------------------------------------------------ */
    while ((next != NX_PROTOCOL_TCP) && (next != NX_PROTOCOL_UDP) &&
           (next != NX_PROTOCOL_ICMPV6))
    {
        if (walked >= N68K_V6_MAX_EXT)
        {
            N68K_RXV_COUNT(skip_ext);
            return (NX_FALSE);
        }

        /* Every header below is at least eight bytes and starts with its own
           next-header byte and a length byte. */
        if ((at + 8UL) > end)
        {
            N68K_RXV_COUNT(skip_length);
            return (NX_FALSE);
        }

        switch (next)
        {
        case N68K_V6_HOPOPT:
        case N68K_V6_DSTOPT:
        case N68K_V6_ROUTING:
            hlen =  ((ULONG)ip[at + 1UL] + 1UL) * 8UL;
            break;

        case N68K_V6_FRAGMENT:
            hlen =  8UL;
            break;

        case N68K_V6_AH:
            /* RFC 4302 2.2: the length is in 32-bit words, less two. */
            hlen =  ((ULONG)ip[at + 1UL] + 2UL) * 4UL;
            break;

        case N68K_V6_ESP:
        case N68K_V6_NONEXT:
            N68K_RXV_COUNT(skip_ext);
            return (NX_FALSE);

        default:
            N68K_RXV_COUNT(skip_protocol);
            return (NX_FALSE);
        }

        if ((hlen < 8UL) || ((at + hlen) > end))
        {
            N68K_RXV_COUNT(skip_length);
            return (NX_FALSE);
        }

        if (next == N68K_V6_ROUTING)
        {
            /* Segments left: the destination in the frame is a waypoint, and
               the sender summed the final one. */
            if (ip[at + 3UL] != 0U)
            {
                N68K_RXV_COUNT(skip_ext);
                return (NX_FALSE);
            }
        }
        else if (next == N68K_V6_FRAGMENT)
        {
            /* Offset in the top thirteen bits, more-fragments in the bottom
               one; the two reserved bits between them are ignored. */
            if ((N68K_RD16(&ip[at + 2UL]) & 0xFFF9UL) != 0UL)
            {
                N68K_RXV_COUNT(skip_fragment);
                return (NX_FALSE);
            }
        }
        else if (next == N68K_V6_DSTOPT)
        {
            if (n68k_rxv6_dstopt_plain(&ip[at + 2UL], hlen - 2UL) != NX_TRUE)
            {
                N68K_RXV_COUNT(skip_ext);
                return (NX_FALSE);
            }
        }
        else
        {
            /* Hop-by-hop and AH: nothing in either moves the transport header
               anywhere but past its own length. */
        }

        next =  (UINT)ip[at];
        at   +=  hlen;
        walked++;
    }

    /* ---- the transport -------------------------------------------------- */

    /* TCP's header is twenty bytes, UDP's and ICMPv6's eight.  A frame with
       less than that left is truncated, whatever its length field says. */
    if ((at + ((next == NX_PROTOCOL_TCP) ? 20UL : 8UL)) > end)
    {
        N68K_RXV_COUNT(skip_length);
        return (NX_FALSE);
    }

    /* A UDP datagram may carry a zero checksum, which means the sender
       computed none.  There is then nothing to check and no bit to claim.
       Over IPv6 that is illegal, but the stack is the one that says so. */
    if ((next == NX_PROTOCOL_UDP) && (N68K_RD16(&ip[at + 6UL]) == 0UL))
    {
        N68K_RXV_COUNT(skip_udp_nosum);
        return (NX_FALSE);
    }

    *protocol =  next;
    *offset   =  (UINT)at;
    *payload  =  (UINT)(end - at);

    return (NX_TRUE);
}

/* Which bit one verified IPv6 transport publishes. */
static ULONG n68k_rxv6_bit(UINT protocol)
{
    if (protocol == NX_PROTOCOL_TCP)
        return (NX_INTERFACE_CAPABILITY_TCP_RX_CHECKSUM);
    if (protocol == NX_PROTOCOL_UDP)
        return (NX_INTERFACE_CAPABILITY_UDP_RX_CHECKSUM);

    return (NX_INTERFACE_CAPABILITY_ICMPV6_RX_CHECKSUM);
}

static ULONG n68k_rxv6_verify(NX_PACKET *packet, UINT *drop)
{
UCHAR      *ip =  packet -> nx_packet_prepend_ptr;
UCHAR      *saved_prepend;
ULONG       saved_length;
UCHAR       saved_version;
ULONG       src[4];
ULONG       dst[4];
UINT        protocol;
UINT        offset;
UINT        payload;
UINT        ok;

    if (n68k_rxv6_shape(ip, packet -> nx_packet_length, &protocol, &offset,
                        &payload) != NX_TRUE)
    {
        return (0UL);
    }

    /* Into host-order longwords: the address in the frame need not be aligned
       the way the checksum function casts it, and a ones-complement sum does
       not care in what order the halves arrive.  Keep these assignments here:
       GCC 16's analyzer invents an uninitialized return value for a void
       helper which fills either four-word array through a pointer. */
    src[0] =  N68K_RD32(&ip[8]);
    src[1] =  N68K_RD32(&ip[12]);
    src[2] =  N68K_RD32(&ip[16]);
    src[3] =  N68K_RD32(&ip[20]);
    dst[0] =  N68K_RD32(&ip[24]);
    dst[1] =  N68K_RD32(&ip[28]);
    dst[2] =  N68K_RD32(&ip[32]);
    dst[3] =  N68K_RD32(&ip[36]);

    saved_prepend =  packet -> nx_packet_prepend_ptr;
    saved_length  =  packet -> nx_packet_length;
    saved_version =  packet -> nx_packet_ip_version;

    /* The checksum function reads the version off the packet to decide the
       pseudo header is 128-bit; the caller's value is put back below. */
    packet -> nx_packet_prepend_ptr =  ip + offset;
    packet -> nx_packet_length      =  (ULONG)payload;
    packet -> nx_packet_ip_version  =  NX_IP_VERSION_V6;

    ok =  n68k_rx_sum_ok(packet, (ULONG)protocol, payload, src, dst);

    packet -> nx_packet_prepend_ptr =  saved_prepend;
    packet -> nx_packet_length      =  saved_length;
    packet -> nx_packet_ip_version  =  saved_version;

    if (ok != NX_TRUE)
    {
        N68K_RXV_COUNT(bad_transport);
        *drop =  NX_TRUE;
        return (0UL);
    }

    N68K_RXV_COUNT(transport_ok);
    N68K_RXV_COUNT(v6_ok);
    if (offset > 40U)
    {
        N68K_RXV_COUNT(v6_ext);
    }

    return (n68k_rxv6_bit(protocol));
}

#endif /* FEATURE_NX_IPV6 */

/*
 * Check what can be checked, and report what was checked.
 *
 * Returns the capability bits to publish on the packet, or sets *drop when the
 * frame is corrupt and must not reach the stack at all.  A frame this function
 * does not understand -- a fragment, an IPv6 extension header, a truncated or
 * padded header -- returns fewer bits and is checked by the stack in the
 * ordinary way.  The conservative answer is always no bits.
 */
ULONG n68k_rx_verify(NX_PACKET *packet, UINT *drop)
{
UCHAR      *ip;
UCHAR      *saved_prepend;
ULONG       saved_length;
ULONG       flags =  0UL;
ULONG       total;
ULONG       frag;
ULONG       src;
ULONG       dst;
UINT        ihl;
UINT        protocol;
UINT        payload;
UINT        ok;

    *drop =  NX_FALSE;

    ip =  packet -> nx_packet_prepend_ptr;

    /* Shorter than an IPv4 header: nothing to check, the stack rejects it. */
    if (packet -> nx_packet_length < 20UL)
    {
        N68K_RXV_COUNT(skip_short);
        return (0UL);
    }

#ifdef FEATURE_NX_IPV6
    if ((ip[0] >> 4) == 6U)
    {
        return (n68k_rxv6_verify(packet, drop));
    }
#endif

    if ((ip[0] >> 4) != 4U)
    {
        N68K_RXV_COUNT(skip_version);
        return (0UL);
    }

    ihl =  (UINT)((ip[0] & 0x0FU) << 2);

    if ((ihl < 20U) || ((ULONG)ihl > packet -> nx_packet_length))
    {
        N68K_RXV_COUNT(skip_short);
        return (0UL);
    }

    /* ---- the IPv4 header ------------------------------------------------ */

    saved_length =  packet -> nx_packet_length;

    if (n68k_rx_sum_ok(packet, NX_IP_VERSION_V4, ihl, NX_NULL, NX_NULL)
        != NX_TRUE)
    {
        /* A header that fails here is what the stack drops anyway, and
           dropping it now saves carrying it further. */
        N68K_RXV_COUNT(bad_ip);
        *drop =  NX_TRUE;
        return (0UL);
    }

    flags =  NX_INTERFACE_CAPABILITY_IPV4_RX_CHECKSUM;
    N68K_RXV_COUNT(ip_ok);

    /* ---- the transport ---------------------------------------------------
     *
     * The total length in the IP header is the authority, rather than
     * nx_packet_length.  An Ethernet frame below the minimum is padded, and
     * the padding is not part of the datagram or of its checksum.  A frame
     * whose header claims more than arrived is truncated and gets no
     * transport bit.
     */
    total =  N68K_RD16(&ip[2]);
    frag  =  N68K_RD16(&ip[6]);

    if ((total < (ULONG)ihl) || (total > saved_length))
    {
        N68K_RXV_COUNT(skip_length);
        return (flags);
    }

    /* MF or a non-zero offset: the transport checksum covers the reassembled
       datagram, which a frame-level check cannot see.  The stack checks it
       after reassembly, which is what declining the bit asks for. */
    if ((frag & 0x3FFFUL) != 0UL)
    {
        N68K_RXV_COUNT(skip_fragment);
        return (flags);
    }

    protocol =  (UINT)ip[9];
    payload  =  (UINT)(total - (ULONG)ihl);
    src      =  N68K_RD32(&ip[12]);
    dst      =  N68K_RD32(&ip[16]);

    switch (protocol)
    {
    case NX_PROTOCOL_TCP:
    case NX_PROTOCOL_UDP:
    case NX_PROTOCOL_ICMP:
    case NX_PROTOCOL_IGMP:
        break;

    default:
        N68K_RXV_COUNT(skip_protocol);
        return (flags);
    }

    /*
     * A UDP datagram can legitimately carry a zero checksum, which means the
     * sender did not compute one.  There is then nothing to check, and the
     * bit must not be claimed.
     */
    if ((protocol == NX_PROTOCOL_UDP) && (payload >= 8U) &&
        (N68K_RD16(&ip[ihl + 6]) == 0UL))
    {
        N68K_RXV_COUNT(skip_udp_nosum);
        return (flags);
    }

    saved_prepend =  packet -> nx_packet_prepend_ptr;

    packet -> nx_packet_prepend_ptr =  ip + ihl;
    packet -> nx_packet_length      =  (ULONG)payload;

    ok =  n68k_rx_sum_ok(packet, (ULONG)protocol, payload,
                         (protocol == NX_PROTOCOL_ICMP) ||
                         (protocol == NX_PROTOCOL_IGMP) ? NX_NULL : &src,
                         (protocol == NX_PROTOCOL_ICMP) ||
                         (protocol == NX_PROTOCOL_IGMP) ? NX_NULL : &dst);

    packet -> nx_packet_prepend_ptr =  saved_prepend;
    packet -> nx_packet_length      =  saved_length;

    if (ok != NX_TRUE)
    {
        N68K_RXV_COUNT(bad_transport);
        *drop =  NX_TRUE;
        return (0UL);
    }

    switch (protocol)
    {
    case NX_PROTOCOL_TCP:
        flags |=  NX_INTERFACE_CAPABILITY_TCP_RX_CHECKSUM;
        break;
    case NX_PROTOCOL_UDP:
        flags |=  NX_INTERFACE_CAPABILITY_UDP_RX_CHECKSUM;
        break;
    case NX_PROTOCOL_ICMP:
        flags |=  NX_INTERFACE_CAPABILITY_ICMPV4_RX_CHECKSUM;
        break;
    default:
        flags |=  NX_INTERFACE_CAPABILITY_IGMP_RX_CHECKSUM;
        break;
    }

    N68K_RXV_COUNT(transport_ok);

    return (flags);
}


#ifdef FEATURE_NX_IPV6

/*
 * The big-endian longword sum of a byte range, end-around carry.
 *
 * NOT N68K_SUM_LONGWORDS.  That one reads the machine's own word order, which
 * is right for a valid IPv4 header -- it sums to the one's-complement zero in
 * either order, so taking it back out of the carried sum is a no-op whichever
 * way it was read.  Nothing else in a frame has that property, so a region
 * subtracted from the carried sum has to be read the way the carried sum was
 * taken.
 */
static ULONG n68k_rxv_sum_be(const UCHAR *p, ULONG longwords)
{
ULONG   acc =  0UL;

    while (longwords != 0UL)
    {
    ULONG   w =  N68K_RD32(p);

        acc +=  w;
        if (acc < w)
            acc++;                          /* end-around carry */

        p +=  4;
        longwords--;
    }

    return (acc);
}

#endif /* FEATURE_NX_IPV6 */

/*
 * Fold a 32-bit accumulator to 16 bits, carries wrapped around.
 *
 * TWO STEPS ARE EXACTLY ENOUGH, so this is branch-free and small enough to
 * inline -- it was a loop, it did not inline, and it is called FOUR times per
 * frame on the fast path.  The bound: both halves are at most 0xFFFF, so the
 * first step is at most 0x1FFFE; the second therefore adds a carry of at most
 * 1 to a low half of at most 0xFFFE, giving at most 0xFFFF.  A third step
 * could never change anything, which is why the loop always ran twice.
 */
static inline ULONG n68k_rxv_fold(ULONG sum)
{
    sum =  (sum & 0xFFFFUL) + (sum >> 16);
    sum =  (sum & 0xFFFFUL) + (sum >> 16);

    return (sum);
}

/*
 * The same check, from a sum the copy already produced.
 *
 * `carried` is the ones-complement sum of `copied` bytes, starting where the
 * copy hook wrote.  For a cooked frame that is the IPv4 header onward, which
 * is what this function looks at.  The transport sum is then the carried sum
 * less the header sum, one subtraction instead of a second walk of the
 * payload.  The header is still summed here, twenty bytes against up to
 * fifteen hundred.
 *
 * A frame carrying Ethernet padding is declined rather than corrected.  The
 * padding sits inside `copied` and outside the datagram, and it is only zero
 * by convention.  Such frames are short, so the ordinary walk costs little.
 * The fast path is for the full-sized frames.
 */
ULONG n68k_rx_verify_sum(NX_PACKET *packet, ULONG carried, ULONG copied,
                         UINT *drop)
{
UCHAR  *ip;
ULONG   flags;
ULONG   total;
ULONG   frag;
ULONG   src;
ULONG   dst;
ULONG   head;
ULONG   sum;
UINT    ihl;
UINT    protocol;
UINT    payload;
UINT    ver;
#ifdef FEATURE_NX_IPV6
UINT    offset;
#endif

    *drop =  NX_FALSE;

    ip =  packet -> nx_packet_prepend_ptr;

    if (packet -> nx_packet_length < 20UL)
    {
        N68K_RXV_COUNT(skip_short);
        return (0UL);
    }

    /* ONE LOAD AND ONE SHIFT.  The version nibble was read twice -- against 6
       here and against 4 below -- and an IPv4 frame, which is every frame of
       a bulk receive, paid for both. */
    ver =  (UINT)(ip[0] >> 4);

#ifdef FEATURE_NX_IPV6
    if (ver == 6U)
    {
        if (n68k_rxv6_shape(ip, packet -> nx_packet_length, &protocol,
                            &offset, &payload) != NX_TRUE)
        {
            return (0UL);
        }

        /* Ethernet padding, or a chain whose length is not a whole number of
           longwords: neither can be taken out of the carried sum exactly, so
           the ordinary path re-reads the frame instead. */
        if ((copied != ((ULONG)offset + (ULONG)payload)) ||
            ((((ULONG)offset - 40UL) & 3UL) != 0UL))
        {
            return (n68k_rxv6_verify(packet, drop));
        }

        /*
         * The IPv6 pseudo header is the source and destination addresses, the
         * upper-layer length and the next header.  The addresses are already
         * inside the carried sum; only the first two longwords of the fixed
         * header are not part of it, so those are what comes back out.
         */
        sum =  n68k_rxv_sum_be(&ip[0], 2UL);

        /* The extension headers are inside the carried sum and outside the
           pseudo header, so they come out with those two longwords. */
        if (offset > 40U)
        {
        ULONG   ext =  n68k_rxv_sum_be(&ip[40], ((ULONG)offset - 40UL) >> 2);

            head =  sum;
            sum  =  head + ext;
            if (sum < ext)
                sum++;
        }
        head =  sum;

        sum =  carried + (~head);
        if (sum < carried)
            sum++;                          /* end-around carry */

        sum =  n68k_rxv_fold(sum);
        sum +=  (ULONG)protocol;
        sum +=  (ULONG)payload;

        if (n68k_rxv_fold(sum) != 0xFFFFUL)
        {
            N68K_RXV_COUNT(bad_transport);
            *drop =  NX_TRUE;
            return (0UL);
        }

        N68K_RXV_COUNT(transport_ok);
        N68K_RXV_COUNT(v6_ok);
        N68K_RXV_COUNT(from_copy);
        if (offset > 40U)
        {
            N68K_RXV_COUNT(v6_ext);
        }

        return (n68k_rxv6_bit(protocol));
    }
#endif

    if (ver != 4U)
    {
        N68K_RXV_COUNT(skip_version);
        return (0UL);
    }

    /* Everything below reads words and longwords out of this header, and the
       sum below hands it over as a `const ULONG *`.  Anything else takes the
       ordinary path, which reads a byte at a time. */
    if (n68k_rxv_even(ip) != NX_TRUE)
    {
        return (n68k_rx_verify(packet, drop));
    }

    ihl   =  (UINT)((ip[0] & 0x0FU) << 2);
    total =  N68K_RDW16(&ip[2]);

    /* Anything the carried sum cannot describe exactly goes to the ordinary
       path, which re-derives everything from the frame. */
    /* `ihl > nx_packet_length` WAS HERE AND IS IMPLIED: the next two tests
       establish ihl <= total <= nx_packet_length, so a header longer than the
       frame cannot reach past them. */
    if ((ihl < 20U) || (total < (ULONG)ihl) ||
        (total > packet -> nx_packet_length) || (copied != total))
    {
        return (n68k_rx_verify(packet, drop));
    }

    protocol =  (UINT)ip[9];
    payload  =  (UINT)(total - (ULONG)ihl);
    frag     =  N68K_RDW16(&ip[6]);

    /* Only TCP and UDP: the others carry no pseudo header, and they are rare
       and short enough that the ordinary path is the right answer. */
    if (((protocol != NX_PROTOCOL_TCP) && (protocol != NX_PROTOCOL_UDP)) ||
        ((frag & 0x3FFFUL) != 0UL))
    {
        return (n68k_rx_verify(packet, drop));
    }

    if ((protocol == NX_PROTOCOL_UDP) && (payload >= 8U) &&
        (N68K_RDW16(&ip[ihl + 6]) == 0UL))
    {
        return (n68k_rx_verify(packet, drop));
    }

    /* ---- the IPv4 header ------------------------------------------------ */
    if (ihl == 20U)
    {
        /*
         * FIVE LONGWORDS, IN LINE.  N68K_SUM_LONGWORDS is an INDIRECT call
         * through the CPU-dispatch vector (net68k.h:93) into a routine that
         * saves d2-d7/a2 before it adds anything -- for the twenty bytes that
         * is every IPv4 header without options, the prologue costs more than
         * the sum.  Options are rare and keep the call.
         *
         * Byte for byte the same arithmetic as n68k_sum_longwords()
         * (n68k_checksum.c:80): accumulate, and add the carry back around.
         */
        /*
         * UNROLLED, BECAUSE -Os LEFT THE LOOP COSTING MORE THAN THE SUM.
         *
         * The `for (k = 0; k < 5; k++)` above it compiled to TEN INSTRUCTIONS
         * A LONGWORD on m68k:
         *
         *     move.l (a2,a0.l*4),d1     indexed load, scaled
         *     add.l  d1,d0
         *     cmp.l  d1,d0              the carry test...
         *     scs    d1                 ...as a byte...
         *     extb.l d1                 ...sign extended...
         *     sub.l  d1,d0              ...and subtracted
         *     addq.l #1,a0
         *     moveq  #5,d1              the BOUND, reloaded every iteration
         *     cmp.l  a0,d1
         *     jne    L32
         *
         * Four of those ten are the loop and one is a constant GCC would not
         * hoist.  The count is FIVE and it is fixed by the ihl == 20 test that
         * selects this branch, so writing it out removes twenty instructions a
         * frame and leaves the arithmetic identical -- same adds, same
         * end-around carry, same order.
         *
         * STILL C AND STILL BRANCHLESS.  An `addx.l` chain in inline asm is
         * two instructions a longword rather than six and was the first
         * instinct; it would also be invisible to the host tier, which
         * compiles this file and is what caught the last change here.  A
         * five-fold saving that no test can see is not better than a two-fold
         * saving that every test does.
         */
        const ULONG    *w =  (const ULONG *)(const void *)ip;
        ULONG           acc;

        acc  =  w[0];
        acc +=  w[1];  acc += (acc < w[1]) ? 1UL : 0UL;
        acc +=  w[2];  acc += (acc < w[2]) ? 1UL : 0UL;
        acc +=  w[3];  acc += (acc < w[3]) ? 1UL : 0UL;
        acc +=  w[4];  acc += (acc < w[4]) ? 1UL : 0UL;

        head =  acc;
    }
    else
    {
        head =  N68K_SUM_LONGWORDS((const ULONG *)ip, (ULONG)ihl >> 2);
    }

    if (n68k_rxv_fold(head) != 0xFFFFUL)
    {
        N68K_RXV_COUNT(bad_ip);
        *drop =  NX_TRUE;
        return (0UL);
    }

    flags =  NX_INTERFACE_CAPABILITY_IPV4_RX_CHECKSUM;
    N68K_RXV_COUNT(ip_ok);

    /* ---- transport = carried, plus the pseudo header -------------------- */
    /*
     * THE HEADER IS NOT SUBTRACTED, BECAUSE IT CONTRIBUTES NOTHING.
     *
     * `sum = carried + ~head` with an end-around carry stood here.  Four lines
     * above, fold(head) was proved to be 0xFFFF -- that IS the IPv4 header
     * check -- and 0xFFFF is the one's-complement zero.  Formally: end-around
     * carry is arithmetic modulo 65535, 0xFFFF = 0 there, and 0xFFFFFFFF is
     * 65535 * 65537, so ~head = 0xFFFFFFFF - head = 0 - 0 = 0 as well.  The
     * subtraction is the identity.
     *
     * The one place that could bite is the 0x0000-against-0xFFFF spelling of
     * that zero, because the verdict below tests for 0xFFFF exactly.  It
     * cannot: `acc += w; acc += (acc < w)` returns 0 only from all-zero input,
     * and w[0] of a valid IPv4 header is 0x45xx xxxx.  The old line could not
     * reach 0 either -- a + b == 2^32 leaves 0, and its own carry correction
     * then makes it 1.  Both spellings are 0xFFFF, both before and after.
     *
     * AND THE PSEUDO-HEADER ADDRESSES GO IN WHOLE.  Adding a 32-bit word with
     * an end-around carry equals adding its two halves, since 2^16 = 1 modulo
     * 65535 -- the same identity the header sum above already relies on.  Four
     * shifts and four masks leave with it.
     */
    src =  N68K_RDW32(&ip[12]);
    dst =  N68K_RDW32(&ip[16]);

    sum =  carried;

    sum +=  src;   sum += (sum < src) ? 1UL : 0UL;
    sum +=  dst;   sum += (sum < dst) ? 1UL : 0UL;
    sum +=  (ULONG)protocol;
    sum +=  (ULONG)payload;

    if (n68k_rxv_fold(sum) != 0xFFFFUL)
    {
        N68K_RXV_COUNT(bad_transport);
        *drop =  NX_TRUE;
        return (0UL);
    }

    flags |=  (protocol == NX_PROTOCOL_TCP)
              ? NX_INTERFACE_CAPABILITY_TCP_RX_CHECKSUM
              : NX_INTERFACE_CAPABILITY_UDP_RX_CHECKSUM;

    N68K_RXV_COUNT(transport_ok);
    N68K_RXV_COUNT(from_copy);

    /* ONLY here.  Nothing else moves this, which is what makes it usable as
       proof that the IPv4 fast path ran at all: from_copy counts both
       families and ip_ok is bumped by the ordinary walk as well, so an
       assertion on either passes with this path switched off. */
    N68K_RXV_COUNT(v4_fused);

    return (flags);
}

#endif /* AMINETXDUO_RX_VERIFY */

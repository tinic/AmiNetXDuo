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
   the stack looks at it. */
#define N68K_RD16(p)    ((ULONG)(((ULONG)(p)[0] << 8) | (ULONG)(p)[1]))
#define N68K_RD32(p)    ((ULONG)(((ULONG)(p)[0] << 24) | ((ULONG)(p)[1] << 16) | \
                                 ((ULONG)(p)[2] << 8)  |  (ULONG)(p)[3]))

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

/*
 * IPv6 has no header checksum, so there is no header verdict and no IPv4 bit
 * to publish -- only the transport, and only when the transport header sits
 * directly at offset 40.  This file does not walk the extension header chain;
 * a frame that carries one is refused here and the stack handles it as before.
 */
static UINT n68k_rxv6_shape(const UCHAR *ip, ULONG length, UINT *protocol,
                            UINT *payload)
{
ULONG   plen;
UINT    next;

    if (length < 40UL)
    {
        n68k_rx_verify_stats.skip_short++;
        return (NX_FALSE);
    }

    next =  (UINT)ip[6];

    if ((next != NX_PROTOCOL_TCP) && (next != NX_PROTOCOL_UDP))
    {
        switch (next)
        {
        case 0U:    case 43U:   case 44U:   case 50U:
        case 51U:   case 59U:   case 60U:   case 135U:
            /* Hop-by-hop, routing, fragment, ESP, AH, none, dest opts,
               mobility: the transport header is not at offset 40. */
            n68k_rx_verify_stats.skip_ext++;
            break;
        default:
            n68k_rx_verify_stats.skip_protocol++;
            break;
        }
        return (NX_FALSE);
    }

    plen =  N68K_RD16(&ip[4]);

    /* Zero means a jumbogram, which needs the hop-by-hop header above. */
    if ((plen == 0UL) || ((plen + 40UL) > length))
    {
        n68k_rx_verify_stats.skip_length++;
        return (NX_FALSE);
    }

    if ((next == NX_PROTOCOL_UDP) && (plen >= 8UL) &&
        (N68K_RD16(&ip[46]) == 0UL))
    {
        n68k_rx_verify_stats.skip_udp_nosum++;
        return (NX_FALSE);
    }

    *protocol =  next;
    *payload  =  (UINT)plen;

    return (NX_TRUE);
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
UINT        payload;
UINT        ok;

    if (n68k_rxv6_shape(ip, packet -> nx_packet_length, &protocol, &payload)
        != NX_TRUE)
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
    packet -> nx_packet_prepend_ptr =  ip + 40;
    packet -> nx_packet_length      =  (ULONG)payload;
    packet -> nx_packet_ip_version  =  NX_IP_VERSION_V6;

    ok =  n68k_rx_sum_ok(packet, (ULONG)protocol, payload, src, dst);

    packet -> nx_packet_prepend_ptr =  saved_prepend;
    packet -> nx_packet_length      =  saved_length;
    packet -> nx_packet_ip_version  =  saved_version;

    if (ok != NX_TRUE)
    {
        n68k_rx_verify_stats.bad_transport++;
        *drop =  NX_TRUE;
        return (0UL);
    }

    n68k_rx_verify_stats.transport_ok++;
    n68k_rx_verify_stats.v6_ok++;

    return ((protocol == NX_PROTOCOL_TCP)
            ? NX_INTERFACE_CAPABILITY_TCP_RX_CHECKSUM
            : NX_INTERFACE_CAPABILITY_UDP_RX_CHECKSUM);
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
        n68k_rx_verify_stats.skip_short++;
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
        n68k_rx_verify_stats.skip_version++;
        return (0UL);
    }

    ihl =  (UINT)((ip[0] & 0x0FU) << 2);

    if ((ihl < 20U) || ((ULONG)ihl > packet -> nx_packet_length))
    {
        n68k_rx_verify_stats.skip_short++;
        return (0UL);
    }

    /* ---- the IPv4 header ------------------------------------------------ */

    saved_length =  packet -> nx_packet_length;

    if (n68k_rx_sum_ok(packet, NX_IP_VERSION_V4, ihl, NX_NULL, NX_NULL)
        != NX_TRUE)
    {
        /* A header that fails here is what the stack drops anyway, and
           dropping it now saves carrying it further. */
        n68k_rx_verify_stats.bad_ip++;
        *drop =  NX_TRUE;
        return (0UL);
    }

    flags =  NX_INTERFACE_CAPABILITY_IPV4_RX_CHECKSUM;
    n68k_rx_verify_stats.ip_ok++;

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
        n68k_rx_verify_stats.skip_length++;
        return (flags);
    }

    /* MF or a non-zero offset: the transport checksum covers the reassembled
       datagram, which a frame-level check cannot see.  The stack checks it
       after reassembly, which is what declining the bit asks for. */
    if ((frag & 0x3FFFUL) != 0UL)
    {
        n68k_rx_verify_stats.skip_fragment++;
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
        n68k_rx_verify_stats.skip_protocol++;
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
        n68k_rx_verify_stats.skip_udp_nosum++;
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
        n68k_rx_verify_stats.bad_transport++;
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

    n68k_rx_verify_stats.transport_ok++;

    return (flags);
}


/* Fold a 32-bit accumulator to 16 bits, carries wrapped around. */
static ULONG n68k_rxv_fold(ULONG sum)
{
    while ((sum >> 16) != 0UL)
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

    *drop =  NX_FALSE;

    ip =  packet -> nx_packet_prepend_ptr;

    if (packet -> nx_packet_length < 20UL)
    {
        n68k_rx_verify_stats.skip_short++;
        return (0UL);
    }

#ifdef FEATURE_NX_IPV6
    if ((ip[0] >> 4) == 6U)
    {
        if (n68k_rxv6_shape(ip, packet -> nx_packet_length, &protocol,
                            &payload) != NX_TRUE)
        {
            return (0UL);
        }

        if (copied != ((ULONG)payload + 40UL))
        {
            return (n68k_rxv6_verify(packet, drop));
        }

        /*
         * The IPv6 pseudo header is the source and destination addresses, the
         * upper-layer length and the next header.  The addresses are already
         * inside the carried sum; only the first two longwords of the fixed
         * header are not part of it, so those are what comes back out.
         */
        /* Read big-endian, not through N68K_SUM_LONGWORDS: unlike a valid IPv4
           header these eight bytes do not sum to zero, so the subtraction is
           only a no-op on the byte order the carried sum was taken in. */
        head =  N68K_RD32(&ip[4]);
        sum  =  N68K_RD32(&ip[0]) + head;
        if (sum < head)
            sum++;
        head =  sum;

        sum =  carried + (~head);
        if (sum < carried)
            sum++;                          /* end-around carry */

        sum =  n68k_rxv_fold(sum);
        sum +=  (ULONG)protocol;
        sum +=  (ULONG)payload;

        if (n68k_rxv_fold(sum) != 0xFFFFUL)
        {
            n68k_rx_verify_stats.bad_transport++;
            *drop =  NX_TRUE;
            return (0UL);
        }

        n68k_rx_verify_stats.transport_ok++;
        n68k_rx_verify_stats.v6_ok++;
        n68k_rx_verify_stats.from_copy++;

        return ((protocol == NX_PROTOCOL_TCP)
                ? NX_INTERFACE_CAPABILITY_TCP_RX_CHECKSUM
                : NX_INTERFACE_CAPABILITY_UDP_RX_CHECKSUM);
    }
#endif

    if ((ip[0] >> 4) != 4U)
    {
        n68k_rx_verify_stats.skip_version++;
        return (0UL);
    }

    ihl   =  (UINT)((ip[0] & 0x0FU) << 2);
    total =  N68K_RD16(&ip[2]);

    /* Anything the carried sum cannot describe exactly goes to the ordinary
       path, which re-derives everything from the frame. */
    if ((ihl < 20U) || ((ULONG)ihl > packet -> nx_packet_length) ||
        (total < (ULONG)ihl) || (total > packet -> nx_packet_length) ||
        (copied != total))
    {
        return (n68k_rx_verify(packet, drop));
    }

    protocol =  (UINT)ip[9];
    payload  =  (UINT)(total - (ULONG)ihl);
    frag     =  N68K_RD16(&ip[6]);

    /* Only TCP and UDP: the others carry no pseudo header, and they are rare
       and short enough that the ordinary path is the right answer. */
    if (((protocol != NX_PROTOCOL_TCP) && (protocol != NX_PROTOCOL_UDP)) ||
        ((frag & 0x3FFFUL) != 0UL))
    {
        return (n68k_rx_verify(packet, drop));
    }

    if ((protocol == NX_PROTOCOL_UDP) && (payload >= 8U) &&
        (N68K_RD16(&ip[ihl + 6]) == 0UL))
    {
        return (n68k_rx_verify(packet, drop));
    }

    /* ---- the IPv4 header ------------------------------------------------ */
    head =  N68K_SUM_LONGWORDS((const ULONG *)ip, (ULONG)ihl >> 2);

    if (n68k_rxv_fold(head) != 0xFFFFUL)
    {
        n68k_rx_verify_stats.bad_ip++;
        *drop =  NX_TRUE;
        return (0UL);
    }

    flags =  NX_INTERFACE_CAPABILITY_IPV4_RX_CHECKSUM;
    n68k_rx_verify_stats.ip_ok++;

    /* ---- transport = carried - header, plus the pseudo header ----------- */
    sum =  carried + (~head);
    if (sum < carried)
        sum++;                              /* end-around carry */

    src =  N68K_RD32(&ip[12]);
    dst =  N68K_RD32(&ip[16]);

    sum =  n68k_rxv_fold(sum);
    sum +=  (src >> 16) & 0xFFFFUL;
    sum +=  src & 0xFFFFUL;
    sum +=  (dst >> 16) & 0xFFFFUL;
    sum +=  dst & 0xFFFFUL;
    sum +=  (ULONG)protocol;
    sum +=  (ULONG)payload;

    if (n68k_rxv_fold(sum) != 0xFFFFUL)
    {
        n68k_rx_verify_stats.bad_transport++;
        *drop =  NX_TRUE;
        return (0UL);
    }

    flags |=  (protocol == NX_PROTOCOL_TCP)
              ? NX_INTERFACE_CAPABILITY_TCP_RX_CHECKSUM
              : NX_INTERFACE_CAPABILITY_UDP_RX_CHECKSUM;

    n68k_rx_verify_stats.transport_ok++;
    n68k_rx_verify_stats.from_copy++;

    return (flags);
}

#endif /* AMINETXDUO_RX_VERIFY */

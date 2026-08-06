/*
 * AmiNetXDuo, receive checksum validation in the driver glue.
 *
 * NX_ENABLE_INTERFACE_CAPABILITY lets an interface tell NetX Duo that receive
 * checksums are already verified, so the stack skips its own walk.  Upstream
 * checks the capability PER INTERFACE, which would make this glue answerable
 * for every frame on the interface with no way to decline one; the fork
 * requires the per-packet nx_packet_interface_capability_flag as well, so a
 * frame this file does not vouch for is checked by the stack exactly as
 * before.  That is what makes declining possible, and declining is what makes
 * this safe.
 *
 * WHY NOT FUSE IT INTO THE COPY.  Summing inside ami_sana2_copy_to_buff()
 * saves the second pass over the payload and is measurably faster, and it was
 * tried: it needs somewhere to leave the answer, the only sanctioned place is
 * NX_PACKET_HEADER_PAD, and growing NX_PACKET wedged the stack on 6 of 6
 * tcpdrill runs against 0 of 6 without it.  This costs a second parse of the
 * IP header and a second walk of the payload -- the walk the stack would have
 * done anyway, just here instead -- and it changes no structure, allocates
 * nothing, and keeps no state between packets.  There is nothing to go stale.
 *
 * The per-packet flag's lifetime is the core's: _nx_packet_allocate() and
 * _nx_packet_release() both clear it.  This file only ever sets bits on a
 * frame it has just checked.
 *
 * SPDX-License-Identifier: MIT
 */

#include "net68k.h"

#include "nx_api.h"
#include "nx_ip.h"

#ifdef AMINETXDUO_RX_VERIFY

/* Big-endian reads: the header has not been byte swapped yet.  This runs
   before nx_ipv4_packet_receive(), which is the whole point -- a frame is
   vouched for or it is not, before the stack looks at it. */
#define N68K_RD16(p)    ((ULONG)(((ULONG)(p)[0] << 8) | (ULONG)(p)[1]))
#define N68K_RD32(p)    ((ULONG)(((ULONG)(p)[0] << 24) | ((ULONG)(p)[1] << 16) | \
                                 ((ULONG)(p)[2] << 8)  |  (ULONG)(p)[3]))

N68kRxVerifyStats  n68k_rx_verify_stats;

/*
 * One checksum, through the same function the stack would have used, with the
 * packet temporarily presented the way that layer expects to see it.  The
 * caller restores; nothing here keeps the packet modified past its return.
 */
static UINT n68k_rx_sum_ok(NX_PACKET *packet, ULONG protocol, UINT length,
                           ULONG *src, ULONG *dst)
{
ULONG   checksum;

    checksum =  _nx_ip_checksum_compute(packet, protocol, length, src, dst);

    return ((~checksum & NX_LOWER_16_MASK) == 0UL) ? NX_TRUE : NX_FALSE;
}

/*
 * Verify what can be verified and say what was verified.
 *
 * Returns the capability bits to publish on the packet, or sets *drop when the
 * frame is corrupt and must not reach the stack at all.  A frame this does not
 * understand -- IPv6, a fragment, a truncated or padded header, anything odd
 * -- returns fewer bits and is checked by the stack in the ordinary way.  The
 * conservative answer is always "no bits".
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

    /* Below an IPv4 header there is nothing to check; the stack rejects it. */
    if (packet -> nx_packet_length < 20UL)
    {
        n68k_rx_verify_stats.skip_short++;
        return (0UL);
    }

    if ((ip[0] >> 4) != 4U)
    {
        /* IPv6 is not handled here yet: its header carries no checksum and
           the transport pseudo header is 128-bit. */
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
        /* A header that fails here is exactly what the stack would drop, and
           dropping it now saves carrying it further. */
        n68k_rx_verify_stats.bad_ip++;
        *drop =  NX_TRUE;
        return (0UL);
    }

    flags =  NX_INTERFACE_CAPABILITY_IPV4_RX_CHECKSUM;
    n68k_rx_verify_stats.ip_ok++;

    /* ---- the transport ---------------------------------------------------
     *
     * The IP header's total length is the authority, not nx_packet_length: an
     * Ethernet frame below the minimum is padded, and the padding is not part
     * of the datagram or of its checksum.  A frame whose header claims more
     * than arrived is truncated and gets no transport bit.
     */
    total =  N68K_RD16(&ip[2]);
    frag  =  N68K_RD16(&ip[6]);

    if ((total < (ULONG)ihl) || (total > saved_length))
    {
        n68k_rx_verify_stats.skip_length++;
        return (flags);
    }

    /* MF or a non-zero offset: the transport checksum covers the reassembled
       datagram, which a frame-level check cannot see.  The stack verifies it
       after reassembly, which is what declining the bit asks it to do. */
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
     * A UDP datagram may legitimately carry a zero checksum, which means the
     * sender did not compute one.  There is then nothing to verify and the
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

#endif /* AMINETXDUO_RX_VERIFY */

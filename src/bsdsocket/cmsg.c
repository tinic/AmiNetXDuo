/*
 * bsdsocket.library -- RFC 3542 ancillary data.
 *
 * The subset worth having, and no more: which interface and local address a
 * datagram arrived on, what its hop limit was, and which source to answer
 * from.  The routing-header, path-MTU and options families of RFC 3542 are
 * not here and are not planned -- docs/BACKLOG.md says which and why.
 *
 * Nothing in this file needs a new LVO.  sendmsg()/recvmsg() already carry
 * msg_control, and `struct msghdr` in the Roadshow NDK is already the 28-byte
 * 4.4BSD shape with msg_control at offset 16 (pinned in transfer.c).  The ABI
 * that had to be settled is in include/aminetxduo/cmsg.h: CMSG_ALIGN is 4.
 *
 * WHERE THE ANSWERS COME FROM
 *
 * NX_PACKET carries both halves already.  nx_packet_ip_header points at the
 * arriving IP header for the life of the packet -- the receive path advances
 * nx_packet_prepend_ptr past it rather than dropping it -- so the destination
 * address, the TTL and the hop limit are read straight out of the wire bytes
 * rather than reconstructed.  nx_packet_address holds the arrival interface,
 * but NOT the same way in both families: nx_ipv4_packet_receive leaves an
 * NX_INTERFACE * there, while nx_ipv6_packet_receive overwrites it with the
 * NXD_IPV6_ADDRESS * the datagram matched (nx_ipv6_packet_receive.c:300), and
 * the interface is one dereference further on.  bsd_cmsg_ifindex() below is
 * the only place that difference is handled.
 *
 * ALIGNMENT
 *
 * cmsg_len is a socklen_t, which is a 32-bit load, and a 68000 takes an
 * address error on an odd one.  A caller's msg_control is whatever it handed
 * us -- a `char buf[CMSG_SPACE(n)]` is only byte-aligned as far as the
 * language is concerned -- so an odd buffer is reported as MSG_CTRUNC with
 * nothing written rather than faulted on.  cmsg.h's CMSG_BUFFER() is the
 * declaration that cannot be wrong.
 *
 * Odd, and not "not a multiple of 4".  The test used to be `& 3`, which is the
 * alignment struct cmsghdr would have on a machine that aligns a long to its
 * width.  m68k does not: __alignof__(long) is 2 there, so every m68k object
 * this could describe -- a stack local, an AllocVec block, a CMSG_BUFFER --
 * is even, and half of them are 2 mod 4.  `& 3` refused those, which meant it
 * refused CMSG_BUFFER() itself about half the time it was used.  Two bytes
 * short of a longword boundary is a perfectly good place to read a longword
 * from on this machine; one byte is not.  The header now also asks for 4 so
 * the loads are single-cycle on an 020, but the gate is the hardware's.
 *
 * SPDX-License-Identifier: MIT
 */

#include "bsdsocket_vectors.h"

#include "nx_ip.h"
#ifdef AMINETXDUO_IPV6
#include "nx_ipv6.h"
#endif

/* Fixed header sizes, as much as is read of each. */
#define BSD_CMSG_IPV4_HDR       20
#define BSD_CMSG_IPV6_HDR       40

/* ---------------------------------------------------------------- the ABI --
 *
 * Every one of these is a number an application will have compiled into it.
 * A toolchain whose <sys/socket.h> disagrees is a build failure here, the
 * same rule in6.c holds sockaddr_in6 to.
 */
_Static_assert(sizeof(struct cmsghdr) == 12, "cmsghdr is not 12 bytes");
_Static_assert(offsetof(struct cmsghdr, cmsg_len)   == 0, "cmsg_len moved");
_Static_assert(offsetof(struct cmsghdr, cmsg_level) == 4, "cmsg_level moved");
_Static_assert(offsetof(struct cmsghdr, cmsg_type)  == 8, "cmsg_type moved");
_Static_assert(sizeof(((struct cmsghdr *)0)->cmsg_len) == 4,
               "socklen_t is not four bytes");

/* CMSG_ALIGN is 4 and that is ABI -- see aminetxduo/cmsg.h. */
_Static_assert(CMSG_ALIGN(0) == 0 && CMSG_ALIGN(1) == 4 && CMSG_ALIGN(4) == 4 &&
               CMSG_ALIGN(5) == 8, "CMSG_ALIGN is not 4-byte");
_Static_assert(CMSG_LEN(0) == 12 && CMSG_LEN(4) == 16,
               "CMSG_LEN is not the header plus the payload");
_Static_assert(CMSG_SPACE(0) == 12 && CMSG_SPACE(1) == 16 &&
               CMSG_SPACE(4) == 16 && CMSG_SPACE(5) == 20,
               "CMSG_SPACE does not round the payload up");

/* No padding in front of the data: CMSG_DATA is the twelfth byte. */
_Static_assert(CMSG_ALIGN(sizeof(struct cmsghdr)) == sizeof(struct cmsghdr),
               "struct cmsghdr needs padding before its data");

/* CMSG_BUFFER() has to deliver what its name promises; m68k will not do it
   from the union alone.  See the attribute in cmsg.h. */
typedef CMSG_BUFFER(BsdCmsgProbe, 16);
_Static_assert(__alignof__(BsdCmsgProbe) >= 4,
               "CMSG_BUFFER() is not longword aligned");

_Static_assert(sizeof(struct in_pktinfo) == 12, "in_pktinfo is not 12 bytes");
_Static_assert(offsetof(struct in_pktinfo, ipi_ifindex)  == 0, "ipi_ifindex moved");
_Static_assert(offsetof(struct in_pktinfo, ipi_spec_dst) == 4, "ipi_spec_dst moved");
_Static_assert(offsetof(struct in_pktinfo, ipi_addr)     == 8, "ipi_addr moved");

#ifdef AMINETXDUO_IPV6
_Static_assert(sizeof(struct in6_pktinfo) == 20, "in6_pktinfo is not 20 bytes");
_Static_assert(offsetof(struct in6_pktinfo, ipi6_addr)    ==  0, "ipi6_addr moved");
_Static_assert(offsetof(struct in6_pktinfo, ipi6_ifindex) == 16, "ipi6_ifindex moved");

_Static_assert(sizeof(struct icmp6_filter) == 32, "icmp6_filter is not 256 bits");
#endif

/* ------------------------------------------------------------- the packet -- */

/*
 * The arriving IP header, or NULL when `need` bytes of it are not readable.
 *
 * Bounded against nx_packet_append_ptr, not nx_packet_prepend_ptr: on a raw
 * IPv4 socket the two are the same address, because raw.c hands the datagram
 * over starting at its own header, and bounding on the prepend pointer would
 * report "no header" for exactly the case where it is most obviously there.
 * The header is always in the first fragment, so the first fragment's append
 * pointer is the right end of the run.
 */
static const UBYTE *bsd_cmsg_iphdr(NX_PACKET *packet, ULONG need)
{
    const UBYTE *hdr;

    if (packet == NX_NULL || packet->nx_packet_ip_header == NX_NULL)
        return NULL;

    hdr = (const UBYTE *)packet->nx_packet_ip_header;

    if (packet->nx_packet_append_ptr < hdr)
        return NULL;

    if ((ULONG)(packet->nx_packet_append_ptr - hdr) < need)
        return NULL;

    return hdr;
}

/*
 * 1-based, as if_nametoindex() and rtm_index count.  0 when unknown, which
 * RFC 3542 6.6 defines as "unspecified".
 *
 * Loopback is numbered here too, and is the whole of NX_MAX_IP_INTERFACES
 * rather than NX_MAX_PHYSICAL_INTERFACES below.  NetX Duo parks it at
 * nx_ip_interface[NX_LOOPBACK_INTERFACE], one past the physical slots, and the
 * convention this library settled on is slot + 1 -- so loopback is
 * NX_LOOPBACK_INTERFACE + 1 and nothing else has to move.  if_indextoname()
 * names it and if_nametoindex() takes the name back, so the number a datagram
 * over ::1 reports is one a caller can resolve and one bsd_cmsg_source_index()
 * accepts on the way out.
 */
static ULONG bsd_cmsg_ifindex(NX_IP *ip, NX_PACKET *packet)
{
    const NX_INTERFACE *nxif = NX_NULL;
    UINT                i;

    if (ip == NULL || packet == NX_NULL)
        return 0UL;

#ifdef AMINETXDUO_IPV6
    if (packet->nx_packet_ip_version == NX_IP_VERSION_V6)
    {
        const NXD_IPV6_ADDRESS *addr =
            packet->nx_packet_address.nx_packet_ipv6_address_ptr;

        if (addr != NX_NULL)
            nxif = addr->nxd_ipv6_address_attached;
    }
    else
#endif
    {
        nxif = packet->nx_packet_ip_interface;
    }

    if (nxif == NX_NULL)
        return 0UL;

    for (i = 0; i < (UINT)NX_MAX_IP_INTERFACES; i++)
    {
        if (nxif == &ip->nx_ip_interface[i])
            return (ULONG)i + 1UL;
    }

    return 0UL;
}

/* ------------------------------------------------------------- the buffer -- */

typedef struct
{
    UBYTE     *co_Base;
    socklen_t  co_Size;
    socklen_t  co_Used;
    BOOL       co_Trunc;
} BsdCmsgOut;

static VOID bsd_cmsg_put(BsdCmsgOut *out, LONG level, LONG type,
                         const VOID *data, socklen_t len)
{
    struct cmsghdr *hdr;
    socklen_t       space = CMSG_SPACE(len);

    if (out->co_Base == NULL || space > out->co_Size - out->co_Used)
    {
        out->co_Trunc = TRUE;
        return;
    }

    hdr = (struct cmsghdr *)(VOID *)(out->co_Base + out->co_Used);

    hdr->cmsg_len   = CMSG_LEN(len);
    hdr->cmsg_level = level;
    hdr->cmsg_type  = type;

    bsd_bcopy((const APTR)data, CMSG_DATA(hdr), (ULONG)len);

    out->co_Used += space;
}

/* --------------------------------------------------------------- defaults -- */

#ifdef AMINETXDUO_IPV6
/* "When an ICMPv6 raw socket is created, it will by default pass all ICMPv6
   message types" -- RFC 3542 3.2, and what a zero-length setsockopt restores. */
static VOID bsd_cmsg_filter_passall(AmiSocket *sock)
{
    UINT i;

    for (i = 0; i < 8; i++)
        sock->as_Icmp6Filter[i] = 0xFFFFFFFFUL;
}
#endif

VOID bsd_cmsg_reset(AmiSocket *sock)
{
#ifdef AMINETXDUO_IPV6
    bsd_cmsg_filter_passall(sock);
#endif

    sock->as_CmsgWant = 0UL;
    bsd_bzero(&sock->as_CmsgSticky, sizeof(sock->as_CmsgSticky));
}

/* -------------------------------------------------------------- receiving -- */

#ifdef AMINETXDUO_IPV6
static VOID bsd_cmsg_build_v6(AmiSocket *sock, NX_IP *ip, NX_PACKET *packet,
                              BsdCmsgOut *out)
{
    const UBYTE *hdr = bsd_cmsg_iphdr(packet, BSD_CMSG_IPV6_HDR);

    if (hdr == NULL)
        return;

    if ((sock->as_CmsgWant & ACW_RECVPKTINFO6) != 0)
    {
        struct in6_pktinfo info;
        LONG               type = ((sock->as_CmsgWant & ACW_PKTINFO6_LINUX) != 0)
                                      ? IPV6_PKTINFO_LINUX : IPV6_PKTINFO;
        UINT               i;

        /* The header's destination, which for a multicast datagram is the
           group and not one of this machine's addresses. */
        for (i = 0; i < 16; i++)
            info.ipi6_addr.s6_addr[i] = hdr[24 + i];

        info.ipi6_ifindex = bsd_cmsg_ifindex(ip, packet);

        bsd_cmsg_put(out, IPPROTO_IPV6, type, &info, (socklen_t)sizeof(info));
    }

    if ((sock->as_CmsgWant & ACW_RECVHOPLIMIT) != 0)
    {
        LONG hops = (LONG)hdr[7];
        LONG type = ((sock->as_CmsgWant & ACW_HOPLIMIT_LINUX) != 0)
                        ? IPV6_HOPLIMIT_LINUX : IPV6_HOPLIMIT;

        bsd_cmsg_put(out, IPPROTO_IPV6, type, &hops, (socklen_t)sizeof(hops));
    }
}
#endif

static VOID bsd_cmsg_build_v4(AmiSocket *sock, NX_IP *ip, NX_PACKET *packet,
                              BsdCmsgOut *out)
{
    const UBYTE *hdr = bsd_cmsg_iphdr(packet, BSD_CMSG_IPV4_HDR);
    ULONG        dst;

    if (hdr == NULL)
        return;

    dst = ((ULONG)hdr[16] << 24) | ((ULONG)hdr[17] << 16) |
          ((ULONG)hdr[18] <<  8) |  (ULONG)hdr[19];

    if ((sock->as_CmsgWant & ACW_PKTINFO4) != 0)
    {
        struct in_pktinfo info;
        ULONG             index = bsd_cmsg_ifindex(ip, packet);

        info.ipi_ifindex = index;
        info.ipi_addr.s_addr = dst;

        /*
         * ipi_spec_dst is the local address the datagram was addressed to,
         * which for a broadcast or multicast one is not the header's
         * destination.  The arrival interface's own address is that answer.
         */
        if (index > 0UL && index <= (ULONG)NX_MAX_IP_INTERFACES)
        {
            info.ipi_spec_dst.s_addr =
                ip->nx_ip_interface[index - 1UL].nx_interface_ip_address;
        }
        else
        {
            info.ipi_spec_dst.s_addr = dst;
        }

        bsd_cmsg_put(out, IPPROTO_IP, IP_PKTINFO, &info,
                     (socklen_t)sizeof(info));
    }

    if ((sock->as_CmsgWant & ACW_RECVDSTADDR4) != 0)
    {
        struct in_addr addr;

        addr.s_addr = dst;

        bsd_cmsg_put(out, IPPROTO_IP, IP_RECVDSTADDR, &addr,
                     (socklen_t)sizeof(addr));
    }
}

VOID bsd_cmsg_build(AmiSocket *sock, NX_PACKET *packet, struct msghdr *msg)
{
    NX_IP     *ip = netstack_ip();
    BsdCmsgOut out;

    if (msg == NULL)
        return;

    out.co_Base  = (UBYTE *)msg->msg_control;
    out.co_Size  = (socklen_t)msg->msg_controllen;
    out.co_Used  = 0;
    out.co_Trunc = FALSE;

    msg->msg_controllen = 0;

    if (out.co_Base == NULL || out.co_Size < (socklen_t)sizeof(struct cmsghdr))
        return;

    if ((sock->as_CmsgWant & ACW_RECV_ANY) == 0)
        return;

    /* See the header note: a 32-bit store through an odd pointer is an
       address error on a 68000, and this pointer is the caller's. */
    if ((((ULONG)out.co_Base) & 1UL) != 0UL)
    {
        msg->msg_flags |= MSG_CTRUNC;
        return;
    }

#ifdef AMINETXDUO_IPV6
    if (packet != NX_NULL && packet->nx_packet_ip_version == NX_IP_VERSION_V6)
        bsd_cmsg_build_v6(sock, ip, packet, &out);
    else
#endif
        bsd_cmsg_build_v4(sock, ip, packet, &out);

    msg->msg_controllen = out.co_Used;

    if (out.co_Trunc)
        msg->msg_flags |= MSG_CTRUNC;
}

/* ---------------------------------------------------------------- sending -- */

/*
 * RFC 3542 6.6 lets a PKTINFO name an address, an interface, or both, and
 * leave the other half to the stack.  An all-zero one means "undo the sticky
 * option", which is why cs_Have is separate from the two fields.
 */
#ifdef AMINETXDUO_IPV6
static LONG bsd_cmsg_pktinfo6(struct AmiSocketBase *base, const UBYTE *data,
                              socklen_t len, BsdCmsgSource *out)
{
    const struct in6_pktinfo *info = (const struct in6_pktinfo *)(const VOID *)data;
    ULONG                     words[4];

    if (len < (socklen_t)sizeof(*info))
        return bsd_fail(base, AMI_EINVAL);

    bsd_in6_to_words(info->ipi6_addr.s6_addr, words);

    out->cs_Have    = TRUE;
    out->cs_Ifindex = info->ipi6_ifindex;

    if ((words[0] | words[1] | words[2] | words[3]) != 0UL)
    {
        out->cs_Source.nxd_ip_version = NX_IP_VERSION_V6;
        out->cs_Source.nxd_ip_address.v6[0] = words[0];
        out->cs_Source.nxd_ip_address.v6[1] = words[1];
        out->cs_Source.nxd_ip_address.v6[2] = words[2];
        out->cs_Source.nxd_ip_address.v6[3] = words[3];
    }

    return 0;
}
#endif /* AMINETXDUO_IPV6 */

#ifdef AMINETXDUO_IPV6
/*
 * RFC 3542 6.3.  "The interpretation of the received hop limit ... -1: use
 * kernel default; 0 to 255: use this value; other: return EINVAL."  -1 puts
 * the socket's own IPV6_UNICAST_HOPS back for this datagram, which is what
 * cs_HaveHops being FALSE means.
 */
static LONG bsd_cmsg_hoplimit(struct AmiSocketBase *base, const UBYTE *data,
                              socklen_t len, BsdCmsgSource *out)
{
    LONG hops;

    if (len < (socklen_t)sizeof(LONG))
        return bsd_fail(base, AMI_EINVAL);

    hops = *(const LONG *)(const VOID *)data;

    if (hops == -1)
    {
        out->cs_HaveHops = FALSE;
        return 0;
    }

    if (hops < 0 || hops > 255)
        return bsd_fail(base, AMI_EINVAL);

    out->cs_Hops     = hops;
    out->cs_HaveHops = TRUE;

    return 0;
}
#endif /* AMINETXDUO_IPV6 */

static LONG bsd_cmsg_pktinfo4(struct AmiSocketBase *base, const UBYTE *data,
                              socklen_t len, BsdCmsgSource *out)
{
    const struct in_pktinfo *info = (const struct in_pktinfo *)(const VOID *)data;

    if (len < (socklen_t)sizeof(*info))
        return bsd_fail(base, AMI_EINVAL);

    out->cs_Have    = TRUE;
    out->cs_Ifindex = info->ipi_ifindex;

    if (info->ipi_spec_dst.s_addr != 0UL)
        bsd_addr_from_v4(&out->cs_Source, info->ipi_spec_dst.s_addr);

    return 0;
}

LONG bsd_cmsg_parse(struct AmiSocketBase *base, AmiSocket *sock,
                    const struct msghdr *msg, BsdCmsgSource *out)
{
    const UBYTE *base_ptr;
    socklen_t    total;
    socklen_t    at;

    /*
     * The sticky IPV6_PKTINFO is the default a per-datagram one overrides.
     * Copied whether or not ACW_STICKY6 is set, because clearing the option
     * zeroes the record: cs_Have is the authority, and send()/sendto() -- which
     * never come through here -- read the same field directly.
     *
     * cs_HaveHops is not part of it: IPV6_HOPLIMIT is ancillary-only, so the
     * sticky record can never carry one and send()/sendto() get FALSE from the
     * same zeroed field.
     */
    *out = sock->as_CmsgSticky;
    out->cs_Hops     = 0;
    out->cs_HaveHops = FALSE;

    if (msg->msg_control == NULL ||
        msg->msg_controllen < (socklen_t)sizeof(struct cmsghdr))
        return 0;

    base_ptr = (const UBYTE *)msg->msg_control;
    total    = (socklen_t)msg->msg_controllen;

    if ((((ULONG)base_ptr) & 1UL) != 0UL)
        return bsd_fail(base, AMI_EINVAL);

    for (at = 0; total - at >= (socklen_t)sizeof(struct cmsghdr); )
    {
        const struct cmsghdr *hdr =
            (const struct cmsghdr *)(const VOID *)(base_ptr + at);
        const UBYTE          *data = base_ptr + at +
                                     CMSG_ALIGN(sizeof(struct cmsghdr));
        socklen_t             len;
        socklen_t             step;

        if (hdr->cmsg_len < CMSG_LEN(0) || hdr->cmsg_len > total - at)
            return bsd_fail(base, AMI_EINVAL);

        len  = (socklen_t)(hdr->cmsg_len - CMSG_LEN(0));
        step = CMSG_ALIGN(hdr->cmsg_len);

        /*
         * TCP is the one socket that cannot carry any of this, and not for want
         * of a call: a stream's source is fixed when the SYN goes out and every
         * segment after it carries nx_tcp_socket_connect_interface, so there is
         * nothing per-write to name.  Naming it at connect() is a different
         * question and a different file (socket.c, over the fork's
         * nxd_tcp_client_socket_source_connect()).  Per-write is refused here
         * whatever happens there.
         *
         * Raw is allowed, and the checksum argument that used to refuse it does
         * not survive: bsd_raw_send_v6() picks the source first and computes the
         * ICMPv6 checksum over it afterwards, so a named source is the one the
         * checksum covers.
         */
        if ((sock->as_Flags & ASF_TCP) != 0)
            return bsd_fail(base, AMI_EINVAL);

        if (hdr->cmsg_level == IPPROTO_IP && hdr->cmsg_type == IP_PKTINFO)
        {
            if ((sock->as_Flags & ASF_INET6) != 0)
                return bsd_fail(base, AMI_EINVAL);
            if (bsd_cmsg_pktinfo4(base, data, len, out) != 0)
                return -1;
        }
#ifdef AMINETXDUO_IPV6
        else if (hdr->cmsg_level == IPPROTO_IPV6 &&
                 (hdr->cmsg_type == IPV6_PKTINFO ||
                  hdr->cmsg_type == IPV6_PKTINFO_LINUX))
        {
            if ((sock->as_Flags & ASF_INET6) == 0)
                return bsd_fail(base, AMI_EINVAL);
            if (bsd_cmsg_pktinfo6(base, data, len, out) != 0)
                return -1;
        }
        else if (hdr->cmsg_level == IPPROTO_IPV6 &&
                 (hdr->cmsg_type == IPV6_HOPLIMIT ||
                  hdr->cmsg_type == IPV6_HOPLIMIT_LINUX))
        {
            if ((sock->as_Flags & ASF_INET6) == 0)
                return bsd_fail(base, AMI_EINVAL);
            if (bsd_cmsg_hoplimit(base, data, len, out) != 0)
                return -1;
        }
#endif
        else
        {
            /* Refused rather than ignored, the rule the whole library is held
               to: a caller asking for something it will not get is told so. */
            return bsd_fail(base, AMI_EINVAL);
        }

        if (step == 0 || step > total - at)
            break;

        at += step;
    }

    return 0;
}

/*
 * The address index nxd_udp_socket_source_send() and
 * nxd_ip_raw_packet_source_send() want.  It is not the same number in the two
 * families: for IPv4 it indexes nx_ip_interface[], for IPv6 nx_ipv6_address[].
 * -1 when the source cannot be resolved, which the caller turns into
 * EADDRNOTAVAIL rather than letting NetX pick -- picking is what the option
 * was given to prevent.
 *
 * Not bsd_source_select() (socket.c), which produces the same kind of index
 * and is deliberately left alone: it answers "where does this SOCKET send
 * from", from the bound address and the RFC 4007 zone, both standing state.
 * This answers "where does this DATAGRAM send from", and takes a bare
 * interface index -- which a bind cannot express -- as well as an address.
 * The one thing they must agree on is the +NX_LOOPBACK_IPV6_ENABLED bound
 * below; both have it, and a comment at each.
 */
LONG bsd_cmsg_source_index(NX_IP *ip, const BsdCmsgSource *src, BOOL v6)
{
    if (ip == NULL || !src->cs_Have)
        return -1;

#ifdef AMINETXDUO_IPV6
    if (v6)
    {
        const NX_INTERFACE *want = NX_NULL;
        UINT                i;

        /* NX_MAX_IP_INTERFACES, not NX_MAX_PHYSICAL_INTERFACES: loopback is
           the last slot and bsd_cmsg_ifindex() hands its number out. */
        if (src->cs_Ifindex > 0UL)
        {
            if (src->cs_Ifindex > (ULONG)NX_MAX_IP_INTERFACES)
                return -1;
            want = &ip->nx_ip_interface[src->cs_Ifindex - 1UL];
        }

        /*
         * The +NX_LOOPBACK_IPV6_ENABLED matters: nxd_ipv6_enable() parks ::1
         * in the slot past the configurable ones, the same place the loopback
         * interface sits past the physical ones.  Without it, naming ::1 as a
         * source is refused for an address the machine plainly has.
         * nxde_udp_socket_source_send() bounds the index the same way.
         */
        for (i = 0;
             i < (UINT)(NX_MAX_IPV6_ADDRESSES + NX_LOOPBACK_IPV6_ENABLED);
             i++)
        {
            const NXD_IPV6_ADDRESS *a = &ip->nx_ipv6_address[i];

            if (a->nxd_ipv6_address_valid == 0)
                continue;
            if (a->nxd_ipv6_address_state != NX_IPV6_ADDR_STATE_VALID)
                continue;
            if (want != NX_NULL && a->nxd_ipv6_address_attached != want)
                continue;

            if (src->cs_Source.nxd_ip_version == NX_IP_VERSION_V6 &&
                (a->nxd_ipv6_address[0] != src->cs_Source.nxd_ip_address.v6[0] ||
                 a->nxd_ipv6_address[1] != src->cs_Source.nxd_ip_address.v6[1] ||
                 a->nxd_ipv6_address[2] != src->cs_Source.nxd_ip_address.v6[2] ||
                 a->nxd_ipv6_address[3] != src->cs_Source.nxd_ip_address.v6[3]))
                continue;

            return (LONG)a->nxd_ipv6_address_index;
        }

        return -1;
    }
#else
    (VOID)v6;
#endif

    if (src->cs_Ifindex > 0UL)
    {
        if (src->cs_Ifindex > (ULONG)NX_MAX_IP_INTERFACES)
            return -1;

        return (LONG)(src->cs_Ifindex - 1UL);
    }

    if (src->cs_Source.nxd_ip_version == NX_IP_VERSION_V4 &&
        src->cs_Source.nxd_ip_address.v4 != 0UL)
    {
        UINT i;

        /* 127/8 by identity, as bsd_source_select() does it: NetX gives the
           loopback interface one address out of the block, so 127.0.0.2 would
           miss on the address compare below. */
        if ((src->cs_Source.nxd_ip_address.v4 >> 24) == 127UL)
            return (LONG)NX_LOOPBACK_INTERFACE;

        for (i = 0; i < (UINT)NX_MAX_PHYSICAL_INTERFACES; i++)
        {
            if (ip->nx_ip_interface[i].nx_interface_valid == 0)
                continue;
            if (ip->nx_ip_interface[i].nx_interface_ip_address ==
                src->cs_Source.nxd_ip_address.v4)
                return (LONG)i;
        }
    }

    return -1;
}

/* -------------------------------------------------------------- the options */

/*
 * Every option here is either "attach this to recvmsg()" or, for
 * IPV6_PKTINFO, "send from here".  Returns 1 for an optname this file does
 * not own so the caller can go on looking, 0 for done, -1 for done with errno
 * set.
 */

static LONG bsd_cmsg_flag(struct AmiSocketBase *base, AmiSocket *sock,
                          APTR optval, socklen_t *optlen, BOOL set,
                          ULONG bit, ULONG lineage, BOOL linux_numbering)
{
    LONG value;

    if (optval == NULL || optlen == NULL)
        return bsd_fail(base, AMI_EFAULT);

    if (set)
    {
        if (*optlen >= (socklen_t)sizeof(LONG))
            value = *(LONG *)optval;
        else if (*optlen >= (socklen_t)sizeof(WORD))
            value = *(WORD *)optval;
        else
            return bsd_fail(base, AMI_EINVAL);

        if (value != 0)
        {
            sock->as_CmsgWant |= bit;

            if (lineage != 0UL)
            {
                if (linux_numbering)
                    sock->as_CmsgWant |= lineage;
                else
                    sock->as_CmsgWant &= ~lineage;
            }
        }
        else
        {
            sock->as_CmsgWant &= ~bit;
        }

        return 0;
    }

    value = ((sock->as_CmsgWant & bit) != 0) ? 1 : 0;

    if (*optlen >= (socklen_t)sizeof(LONG))
    {
        *(LONG *)optval = value;
        *optlen = (socklen_t)sizeof(LONG);
    }
    else if (*optlen >= (socklen_t)sizeof(WORD))
    {
        *(WORD *)optval = (WORD)value;
        *optlen = (socklen_t)sizeof(WORD);
    }
    else
    {
        return bsd_fail(base, AMI_EINVAL);
    }

    return 0;
}

#ifdef AMINETXDUO_IPV6

/* setsockopt(IPPROTO_IPV6, IPV6_PKTINFO) is the sticky source, RFC 3542 6.6. */
static LONG bsd_cmsg_sticky(struct AmiSocketBase *base, AmiSocket *sock,
                            APTR optval, socklen_t *optlen, BOOL set)
{
    if (optval == NULL || optlen == NULL)
        return bsd_fail(base, AMI_EFAULT);

    if (set)
    {
        BsdCmsgSource src;

        if (*optlen < (socklen_t)sizeof(struct in6_pktinfo))
            return bsd_fail(base, AMI_EINVAL);

        bsd_bzero(&src, sizeof(src));

        if (bsd_cmsg_pktinfo6(base, (const UBYTE *)optval,
                              (socklen_t)sizeof(struct in6_pktinfo),
                              &src) != 0)
            return -1;

        /* An all-zero in6_pktinfo clears it, which is how the RFC undoes a
           sticky option. */
        if (src.cs_Ifindex == 0UL &&
            src.cs_Source.nxd_ip_version != NX_IP_VERSION_V6)
        {
            sock->as_CmsgWant &= ~ACW_STICKY6;
            bsd_bzero(&sock->as_CmsgSticky, sizeof(sock->as_CmsgSticky));
        }
        else
        {
            sock->as_CmsgSticky = src;
            sock->as_CmsgWant  |= ACW_STICKY6;
        }

        return 0;
    }

    if (*optlen < (socklen_t)sizeof(struct in6_pktinfo))
        return bsd_fail(base, AMI_EINVAL);

    {
        struct in6_pktinfo info;

        bsd_bzero(&info, sizeof(info));

        if ((sock->as_CmsgWant & ACW_STICKY6) != 0)
        {
            info.ipi6_ifindex = sock->as_CmsgSticky.cs_Ifindex;

            if (sock->as_CmsgSticky.cs_Source.nxd_ip_version == NX_IP_VERSION_V6)
                bsd_words_to_in6(sock->as_CmsgSticky.cs_Source.nxd_ip_address.v6,
                                 info.ipi6_addr.s6_addr);
        }

        bsd_bcopy(&info, optval, (ULONG)sizeof(info));
        *optlen = (socklen_t)sizeof(info);
    }

    return 0;
}

/*
 * ICMP6_FILTER.  Only on a raw ICMPv6 socket -- it filters by ICMPv6 type,
 * and nothing else this library carries has one.
 */
static LONG bsd_cmsg_icmp6_filter(struct AmiSocketBase *base, AmiSocket *sock,
                                  APTR optval, socklen_t *optlen, BOOL set)
{
    if ((sock->as_Flags & ASF_RAW) == 0 ||
        (sock->as_Protocol & 0xFF) != IPPROTO_ICMPV6)
        return bsd_fail(base, AMI_ENOPROTOOPT);

    if (optlen == NULL)
        return bsd_fail(base, AMI_EFAULT);

    if (set)
    {
        /* "In order to clear an installed filter the application can issue a
           setsockopt for ICMP6_FILTER with a zero length." -- RFC 3542 3.2. */
        if (*optlen == 0)
        {
            bsd_cmsg_filter_passall(sock);
            return 0;
        }

        if (optval == NULL)
            return bsd_fail(base, AMI_EFAULT);

        if (*optlen < (socklen_t)sizeof(struct icmp6_filter))
            return bsd_fail(base, AMI_EINVAL);

        bsd_bcopy(optval, sock->as_Icmp6Filter,
                  (ULONG)sizeof(sock->as_Icmp6Filter));

        return 0;
    }

    if (optval == NULL)
        return bsd_fail(base, AMI_EFAULT);

    if (*optlen < (socklen_t)sizeof(struct icmp6_filter))
        return bsd_fail(base, AMI_EINVAL);

    bsd_bcopy(sock->as_Icmp6Filter, optval,
              (ULONG)sizeof(sock->as_Icmp6Filter));
    *optlen = (socklen_t)sizeof(struct icmp6_filter);

    return 0;
}

#endif /* AMINETXDUO_IPV6 */

LONG bsd_cmsg_option(struct AmiSocketBase *base, AmiSocket *sock, LONG level,
                     LONG optname, APTR optval, socklen_t *optlen, BOOL set)
{
    if (level == IPPROTO_IP)
    {
        if (optname == IP_PKTINFO)
            return bsd_cmsg_flag(base, sock, optval, optlen, set,
                                 ACW_PKTINFO4, 0UL, FALSE);

        if (optname == IP_RECVDSTADDR)
            return bsd_cmsg_flag(base, sock, optval, optlen, set,
                                 ACW_RECVDSTADDR4, 0UL, FALSE);

        return 1;
    }

#ifdef AMINETXDUO_IPV6
    if (level == IPPROTO_IPV6)
    {
        if (optname == IPV6_RECVPKTINFO || optname == IPV6_RECVPKTINFO_LINUX)
            return bsd_cmsg_flag(base, sock, optval, optlen, set,
                                 ACW_RECVPKTINFO6, ACW_PKTINFO6_LINUX,
                                 (BOOL)(optname == IPV6_RECVPKTINFO_LINUX));

        if (optname == IPV6_RECVHOPLIMIT || optname == IPV6_RECVHOPLIMIT_LINUX)
            return bsd_cmsg_flag(base, sock, optval, optlen, set,
                                 ACW_RECVHOPLIMIT, ACW_HOPLIMIT_LINUX,
                                 (BOOL)(optname == IPV6_RECVHOPLIMIT_LINUX));

        if (optname == IPV6_PKTINFO || optname == IPV6_PKTINFO_LINUX)
            return bsd_cmsg_sticky(base, sock, optval, optlen, set);

        /*
         * RFC 3542 6.3: the hop limit of one datagram is ancillary data on
         * sendmsg(), never a sticky option -- IPV6_UNICAST_HOPS is the sticky
         * one, and in6.c answers it.
         */
        if (optname == IPV6_HOPLIMIT || optname == IPV6_HOPLIMIT_LINUX)
            return bsd_fail(base, AMI_EINVAL);

        return 1;
    }

    if (level == IPPROTO_ICMPV6)
    {
        if (optname == ICMP6_FILTER)
            return bsd_cmsg_icmp6_filter(base, sock, optval, optlen, set);

        return bsd_fail(base, AMI_ENOPROTOOPT);
    }
#endif

    return 1;
}

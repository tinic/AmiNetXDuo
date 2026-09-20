/*
 * Stateless validation from a checksum accumulated while a frame was copied.
 *
 * SPDX-License-Identifier: MIT
 */

#include "netdev_verify.h"
#include "aminetxduo/anxs2ext.h"

/* Production is big-endian m68k and gets one aligned word load.  Keeping the
   byte form for other hosts makes the same source independently testable. */
static UWORD nd_be16(const UBYTE *p)
{
#if defined(__BYTE_ORDER__) && (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
    return *(const UWORD *)(CONST_APTR)p;
#else
    return (UWORD)(((UWORD)p[0] << 8) | p[1]);
#endif
}

static ULONG nd_be32(const UBYTE *p)
{
    return ((ULONG)nd_be16(p) << 16) | nd_be16(p + 2);
}

static UWORD nd_fold16(ULONG acc)
{
    acc = (acc & 0xffffUL) + (acc >> 16);
    acc = (acc & 0xffffUL) + (acc >> 16);
    return (UWORD)acc;
}

static VOID nd_tcp_key(const UBYTE *tcp, UWORD tlen, NetdevRxSegment *seg)
{
    if (seg == NULL)
        return;

    seg->ports = nd_be32(tcp);
    seg->seq   = nd_be32(tcp + 4);
    seg->ack   = nd_be32(tcp + 8);
    seg->flags = tcp[13];
    seg->win   = nd_be16(tcp + 14);
    seg->data  = (UWORD)(tlen - 20);
    seg->tcp   = (UBYTE)(((tcp[12] >> 4) == 5) ? 1 : 0);
}

UBYTE netdev_rx_verify4(const UBYTE *ip, UWORD plen, ULONG sum)
{
    UWORD total;
    UWORD tlen;
    UBYTE proto;
    ULONG acc;
    UWORD i;

    if (ip == NULL || ip[0] != 0x45)
        return 0;                       /* not IPv4, or options */
    total = nd_be16(ip + 2);
    if (total != plen || total < 20)
        return 0;                       /* padded, truncated, or short */
    if ((ip[6] & 0x3f) != 0 || ip[7] != 0)
        return 0;                       /* MF, or a fragment offset */
    proto = ip[9];
    if (proto != 6 && proto != 17)
        return 0;

    acc = 0;
    for (i = 0; i < 20; i += 2)
        acc += nd_be16(ip + i);
    if (nd_fold16(acc) != 0xffffu)
        return 0;

    tlen = (UWORD)(total - 20);
    if (proto == 17)
    {
        if (tlen < 8 || nd_be16(ip + 24) != tlen)
            return 0;
        if (nd_be16(ip + 26) == 0)      /* IPv4 UDP checksum absent */
            return 0;
    }
    else if (tlen < 20)
        return 0;

    acc  = nd_fold16(sum);
    acc += nd_be16(ip + 12);
    acc += nd_be16(ip + 14);
    acc += nd_be16(ip + 16);
    acc += nd_be16(ip + 18);
    acc += proto;
    acc += tlen;
    if (nd_fold16(acc) != 0xffffu)
        return 0;

    return ANXD_S2_RXF_VERIFIED;
}

UBYTE netdev_rx_trust4(const UBYTE *ip, UWORD plen, UBYTE verdict)
{
    UWORD total;
    UWORD tlen;
    UBYTE proto;

    if (ip == NULL || ip[0] != 0x45)
        return 0;                       /* not IPv4, or IP options */
    total = nd_be16(ip + 2);
    if (total != plen || total < 20)
        return 0;                       /* padded, truncated, or short */
    if ((ip[6] & 0x3f) != 0 || ip[7] != 0)
        return 0;                       /* MF, or a fragment offset */

    proto = ip[9];
    if ((verdict == 2 && proto != 6) ||
        (verdict == 3 && proto != 17) ||
        (verdict != 2 && verdict != 3))
        return 0;

    tlen = (UWORD)(total - 20);
    if (proto == 17)
    {
        if (tlen < 8 || nd_be16(ip + 24) != tlen)
            return 0;
        if (nd_be16(ip + 26) == 0)      /* IPv4 UDP checksum absent */
            return 0;
    }
    else if (tlen < 20)
        return 0;

    return ANXD_S2_RXF_VERIFIED;
}

UBYTE netdev_tx_csum4(const UBYTE *frame, UWORD len, UBYTE supported,
                      UWORD *checksum_offset)
{
    const UBYTE *ip;
    UWORD plen;
    UWORD ihl;
    UWORD total;
    UWORD tlen;
    UWORD offset;
    UBYTE flag;

    if (frame == NULL || checksum_offset == NULL || len < 14 + 20 ||
        frame[12] != 0x08 || frame[13] != 0x00)
        return 0;

    ip   = frame + 14;
    plen = (UWORD)(len - 14);
    if ((ip[0] & 0xf0u) != 0x40u)
        return 0;
    ihl = (UWORD)((ip[0] & 0x0fu) << 2);
    if (ihl < 20 || ihl > plen)
        return 0;
    total = nd_be16(ip + 2);
    if (total < ihl || total > plen)
        return 0;
    if ((ip[6] & 0x3fu) != 0 || ip[7] != 0)
        return 0;

    tlen = (UWORD)(total - ihl);
    if (ip[9] == 6)
    {
        if ((supported & ANXD_S2_TXF_TCP) == 0 || tlen < 20 ||
            (ip[ihl + 12] >> 4) < 5)
            return 0;
        offset = (UWORD)(14 + ihl + 16);
        flag = ANXD_S2_TXF_TCP;
    }
    else if (ip[9] == 17)
    {
        if ((supported & ANXD_S2_TXF_UDP) == 0 || tlen < 8 ||
            nd_be16(ip + ihl + 4) != tlen)
            return 0;
        offset = (UWORD)(14 + ihl + 6);
        flag = ANXD_S2_TXF_UDP;
    }
    else
        return 0;

    if ((UWORD)(offset + 2) > (UWORD)(14 + total))
        return 0;
    *checksum_offset = offset;
    return flag;
}

UBYTE netdev_rx_verify6(const UBYTE *ip, UWORD plen, ULONG sum)
{
    UWORD tlen;
    UBYTE nh;
    ULONG acc;
    if (ip == NULL || (ip[0] >> 4) != 6)
        return 0;
    tlen = nd_be16(ip + 4);
    if ((UWORD)(tlen + 40) != plen)
        return 0;                       /* padded or truncated */
    nh = ip[6];
    if (nh != 6 && nh != 17)
        return 0;                       /* extension header, or other */

    if (nh == 17)
    {
        if (tlen < 8 || nd_be16(ip + 44) != tlen)
            return 0;
        if (nd_be16(ip + 46) == 0)      /* invalid for IPv6 */
            return 0;
    }
    else if (tlen < 20)
        return 0;

    acc  = nd_fold16(sum);
    acc += (UWORD)~nd_fold16((ULONG)nd_be16(ip) + nd_be16(ip + 2) +
                              nd_be16(ip + 4) + nd_be16(ip + 6));
    acc += tlen;
    acc += nh;
    if (nd_fold16(acc) != 0xffffu)
        return 0;

    return ANXD_S2_RXF_VERIFIED;
}

UBYTE netdev_rx_verify(const UBYTE *ip, UWORD plen, ULONG sum)
{
    if (ip == NULL || plen == 0)
        return 0;
    if ((ip[0] >> 4) == 4)
        return netdev_rx_verify4(ip, plen, sum);
    if ((ip[0] >> 4) == 6)
        return netdev_rx_verify6(ip, plen, sum);
    return 0;
}

VOID netdev_rx_segment4(const UBYTE *ip, NetdevRxSegment *seg)
{
    seg->tcp = 0;
    if (ip[9] == 6)
    {
        seg->addr[0] = nd_be32(ip + 12);
        seg->addr[1] = nd_be32(ip + 16);
        seg->words   = 2;
        nd_tcp_key(ip + 20, (UWORD)(nd_be16(ip + 2) - 20), seg);
    }
}

VOID netdev_rx_segment6(const UBYTE *ip, NetdevRxSegment *seg)
{
    UWORD i;

    seg->tcp = 0;
    if (ip[6] == 6)
    {
        for (i = 0; i < 8; i++)
            seg->addr[i] = nd_be32(ip + 8 + 4 * i);
        seg->words = 8;
        nd_tcp_key(ip + 40, nd_be16(ip + 4), seg);
    }
}

UBYTE netdev_rx_continues(NetdevRxGro *g, const NetdevRxSegment *seg,
                          UBYTE verified, UBYTE max)
{
    BOOL candidate = (BOOL)(verified != 0 && seg->tcp != 0 &&
                            seg->data != 0 &&
                            (seg->flags == 0x10 || seg->flags == 0x18));
    UBYTE i;

    if (!candidate)
    {
        g->live = 0;
        return 0;
    }

    if (g->live && g->run < max &&
        g->words == seg->words && g->ports == seg->ports &&
        g->seq == seg->seq && g->ack == seg->ack &&
        g->win == seg->win)
    {
        for (i = 0; i < seg->words; i++)
            if (g->addr[i] != seg->addr[i])
                break;
        if (i == seg->words)
        {
            g->seq = seg->seq + seg->data;
            g->run++;
            return ANXD_S2_RXF_CONTINUES;
        }
    }

    for (i = 0; i < seg->words; i++)
        g->addr[i] = seg->addr[i];
    g->words = seg->words;
    g->ports = seg->ports;
    g->seq   = seg->seq + seg->data;
    g->ack   = seg->ack;
    g->win   = seg->win;
    g->flags = seg->flags;
    g->live  = 1;
    g->run   = 1;
    return 0;
}

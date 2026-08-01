/*
 * AmiNetXDuo -- net68k: 68k data-path primitives for NetX Duo.
 *
 * The same arrangement src/crypto68k/ uses, for the same reason: NetX Duo's
 * portable C is correct everywhere and fast nowhere in particular, and a
 * handful of its inner loops are on every byte of every packet.  Nothing here
 * modifies third_party/ -- the vendored object is dropped from the build in
 * the top-level CMakeLists and this module supplies the symbol instead.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_NET68K_H
#define AMINETXDUO_NET68K_H

#include "nx_api.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The one's-complement 32-bit sum of `count` longwords at `p`, i.e. the
 * add.l/addx.l carry chain.  The result is congruent to the plain sum modulo
 * 0xFFFFFFFF and is zero only when every longword was zero, so it is
 * interchangeable with the 16-bit accumulation NetX Duo does.
 *
 * `p` is read as longwords, so on a strict-alignment host it must be 4-byte
 * aligned.  The 68020 does not require that, and neither does the caller
 * below: NetX Duo only ever enters the loop on a packet's prepend pointer,
 * which the pool keeps longword aligned.
 */
ULONG n68k_sum_longwords(const ULONG *p, ULONG count);

/* Copy and sum in one pass: the same result as n68k_sum_longwords() over
   `from`, with the copy paid for out of the loads it already does.  Both
   pointers must be longword aligned. */
ULONG n68k_copy_sum_longwords(ULONG *to, const ULONG *from, ULONG count);

/*
 * The replacement for _nx_ip_checksum_compute().  Same signature, semantics
 * and side effects (it zero-pads a trailing odd byte in the packet buffer
 * where the vendored code does), differing only in the inner loop.
 * n68k_checksum_hook.c gives it the vendored name; the perf harness links the
 * algorithm without the hook so it can A/B against the vendored implementation
 * in one process.
 */
USHORT n68k_ip_checksum_compute(NX_PACKET *packet_ptr, ULONG protocol,
                                UINT data_length, ULONG *src_ip_addr,
                                ULONG *dest_ip_addr);

/*
 * Bulk copy.  On the assembly path this is movem.l based -- see n68k_copy.S
 * for what it is measured against and why C cannot reach it.  Off that path it
 * is a plain loop, present so a host build links.
 */
VOID n68k_copy_bytes(UCHAR *to, const UCHAR *from, ULONG len);


#ifdef AMINETXDUO_RX_COPY_SUM

/*
 * The receive stash.  ami_sana2_copy_to_buff() has to move the frame out of
 * the device's buffer anyway, and n68k_copy_sum_longwords() sums it out of the
 * loads that copy already does; it leaves the answer here and
 * n68k_ip_checksum_compute() returns it instead of walking the payload again.
 *
 * The three longwords are NX_PACKET_HEADER_PAD's, turned on in
 * port/netxduo-amiga/inc/nx_user.h.  It is NetX Duo's own extension point and
 * nothing in the vendored tree touches the field.
 *
 *   sum      the one's-complement sum over [start, start+bytes), zero padded
 *            at the end, in the 32-bit end-around-carry form
 *            n68k_sum_longwords() produces.
 *   sentinel MAGIC ^ bytes.  The reader derives what `bytes` would have to be
 *            from the call in front of it and compares, so one load answers
 *            both "is this ours" and "is it the same run of bytes", and a
 *            length that does not agree cannot look like a magic that does.
 *   start    the first byte the copy wrote.  The reader is asked about a
 *            suffix -- the frame less its IP header -- and subtracts the
 *            prefix it names.
 *
 * A frame the copy hook could not take gets a zero sentinel written anyway --
 * MAGIC ^ length can never be zero for any length a frame can have -- so
 * nothing can inherit one.  The sentinel is also cleared as it is consumed,
 * and n68k_packet_allocate_hook.c clears it on every packet leaving the pool,
 * because a transmit packet is a receive packet's buffer reissued and TX calls
 * this same function to INSERT a checksum rather than verify one.
 */
#define N68K_RX_STASH_MAGIC     0x6E36384BUL    /* 'n68K' */

#define N68K_RX_SUM(pkt)        ((pkt)->nx_packet_packet_pad[0])
#define N68K_RX_SENTINEL(pkt)   ((pkt)->nx_packet_packet_pad[1])
#define N68K_RX_START(pkt)      ((pkt)->nx_packet_packet_pad[2])

/*
 * A pointer as the pad field can hold it.  The two are the same width on the
 * 68k; on the host, where the differential test runs, the truncation is the
 * same for both operands of the one subtraction that uses it, and the
 * subtraction is what the reader wants.  It never reconstructs a pointer from
 * the field -- it walks back from the prepend pointer by the difference.
 */
#define N68K_RX_ADDR(p)         ((ULONG)(ALIGN_TYPE)(p))

/*
 * Copy `len` bytes of freshly received frame and leave the stash behind.
 * `to` and `from` must be longword aligned; ami_sana2_copy_to_buff() decides
 * that, because it is a fact about the frame in front of it.  Nothing here
 * looks at what the frame contains -- the reader has the IP header's length
 * in its own arguments and needs no second opinion about it.
 *
 * Here rather than in src/sana2/ because the trailing 1-3 bytes have to be
 * folded in exactly the way the walk in n68k_checksum.c folds them, and the
 * two have to be read side by side to see that they are.  tests/perf/host/
 * calls this one, not a copy of it.
 */
VOID n68k_rx_copy_stash(NX_PACKET *packet, UCHAR *to, const UCHAR *from,
                        ULONG len);

/* Nothing to stash: leave the packet unable to answer for one. */
VOID n68k_rx_stash_invalidate(NX_PACKET *packet);

/*
 * Where the fast path went.  A stash that is never taken and a stash that is
 * taken and saves nothing look exactly alike in a throughput figure, and the
 * conditions are strict enough that the first is the likelier of the two, so
 * they are counted rather than reasoned about.  Not per interface and not
 * locked: they are read once at teardown and an increment lost to a race
 * costs nothing but the last digit.
 *
 * `used + miss_*` counts every call to n68k_ip_checksum_compute(), transmit
 * and IP header included, so `miss_none` and `miss_protocol` are large by
 * construction and mean nothing on their own.  What matters is `stamped`
 * against `used`.
 */
typedef struct N68kRxStats
{
    ULONG   stamped;            /* frames summed inside the copy            */
    ULONG   skip_dst;           /* raw mode: dst is data_start + 2          */
    ULONG   skip_from;          /* the device's buffer was not aligned      */

    ULONG   used;               /* checksum calls answered from a stash     */
    ULONG   miss_none;          /* no stash on the packet                   */
    ULONG   miss_chained;       /* reassembled: sums would have to combine  */
    ULONG   miss_protocol;      /* not TCP/UDP, or no pseudo-header address */
    ULONG   miss_prefix;        /* prefix negative or not a multiple of 4   */
    ULONG   miss_length;        /* the stash covered a different run        */
} N68kRxStats;

extern N68kRxStats n68k_rx_stats;

#endif /* AMINETXDUO_RX_COPY_SUM */

#ifdef __cplusplus
}
#endif

#endif /* AMINETXDUO_NET68K_H */

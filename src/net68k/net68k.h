/*
 * AmiNetXDuo, net68k: 68k data-path primitives for NetX Duo.
 *
 * The same arrangement src/crypto68k/ uses, for the same reason: NetX Duo's
 * portable C is correct everywhere and fast nowhere in particular, and a
 * handful of its inner loops are on every byte of every packet.  Nothing here
 * modifies third_party/, the vendored object is dropped from the build in
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

/* The host differential test drives these loops at every start alignment,
   odd ones included, because the 68020+ takes a misaligned word load on
   purpose and real pools longword-align for the 68000.  This turns off only
   UBSan's alignment check, only on the functions that carry it. */
#if defined(__GNUC__) || defined(__clang__)
#define N68K_NO_SANITIZE_ALIGNMENT __attribute__((no_sanitize("alignment")))
#else
#define N68K_NO_SANITIZE_ALIGNMENT
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
 * Bulk copy.  On the assembly path this is movem.l based, see n68k_copy.S
 * for what it is measured against and why C cannot reach it.  Off that path it
 * is a plain loop, present so a host build links.
 */
VOID n68k_copy_bytes(UCHAR *to, const UCHAR *from, ULONG len);

/*
 * Point the three routines above at the forms this machine wants, from an
 * AttnFlags the caller supplies -- SysBase->AttnFlags, and it is a parameter
 * so that nothing here has to have an ExecBase of its own.  Call it once,
 * before the first packet; bsd_lib_init() does.
 *
 * Only the `any` build has anything to choose (n68k_variant.h).  Everywhere
 * else this is an empty function, so a caller need not know which build it is
 * in, and skipping it is a slow stack rather than a broken one.
 */
VOID n68k_cpu_select(ULONG attnflags);

/*
 * What the stack's own callers use.  In an `any` build the three names above
 * are trampolines -- a load and a jump, ~20 cycles, which is 2.5% of a 20-byte
 * memcpy on a 68000 and nothing at all on a 288-byte one -- and they exist for
 * callers this tree does not own: the vendored code that reaches memcpy(), the
 * benches, the tests.  Anything in here that is on the data path goes through
 * the vector instead and pays only the indirection the call already needed.
 *
 * Everywhere else these are the plain names and the call is direct, so the
 * per-CPU builds are exactly what they always were.
 */
#ifdef N68K_MV_MULTI

extern ULONG (*n68k_vec_sum)(const ULONG *, ULONG);
extern ULONG (*n68k_vec_copy_sum)(ULONG *, const ULONG *, ULONG);
extern VOID  (*n68k_vec_copy)(UCHAR *, const UCHAR *, ULONG);

#define N68K_SUM_LONGWORDS      (*n68k_vec_sum)
#define N68K_COPY_SUM_LONGWORDS (*n68k_vec_copy_sum)
#define N68K_COPY_BYTES         (*n68k_vec_copy)

#else

#define N68K_SUM_LONGWORDS      n68k_sum_longwords
#define N68K_COPY_SUM_LONGWORDS n68k_copy_sum_longwords
#define N68K_COPY_BYTES         n68k_copy_bytes

#endif

#ifdef __cplusplus
}
#endif


/*
 * Copy `count` longwords and return the ones-complement sum of what was
 * copied, out of the loads the copy is already doing.  Both pointers must be
 * longword aligned; the caller decides that, because it is a fact about the
 * frame in front of it.
 */
ULONG n68k_copy_sum_longwords(ULONG *to, const ULONG *from, ULONG count);

#ifdef AMINETXDUO_RX_VERIFY

/*
 * Where the receive verification went.  A frame that is vouched for and one
 * that is declined cost different things and neither shows up in a throughput
 * figure, so the split is counted rather than assumed.
 */
typedef struct N68kRxVerifyStats
{
    ULONG   ip_ok;              /* IPv4 header verified                     */
    ULONG   transport_ok;       /* transport verified too                   */
    ULONG   bad_ip;             /* dropped here: bad IPv4 header checksum   */
    ULONG   bad_transport;      /* dropped here: bad transport checksum     */
    ULONG   skip_short;         /* shorter than the header it claims        */
    ULONG   skip_version;       /* not IPv4                                 */
    ULONG   skip_length;        /* truncated, or the header overclaims      */
    ULONG   skip_fragment;      /* MF or offset: the stack reassembles      */
    ULONG   skip_protocol;      /* nothing this file verifies               */
    ULONG   skip_udp_nosum;     /* UDP declining to carry one               */
    ULONG   from_copy;          /* verified from the copy's own sum         */
} N68kRxVerifyStats;

extern N68kRxVerifyStats n68k_rx_verify_stats;

/*
 * Verify a freshly received frame, prepend pointing at the IPv4 header.
 * Returns the nx_packet_interface_capability_flag bits to publish, and sets
 * *drop when the frame is corrupt and must not reach the stack.
 */
ULONG n68k_rx_verify(NX_PACKET *packet, UINT *drop);

/*
 * The same, from the sum ami_sana2_copy_to_buff() produced while copying.
 * `carried` covers `copied` bytes from the frame's start.  Anything the sum
 * cannot be trusted to describe exactly falls through to n68k_rx_verify().
 */
ULONG n68k_rx_verify_sum(NX_PACKET *packet, ULONG carried, ULONG copied,
                         UINT *drop);

#endif /* AMINETXDUO_RX_VERIFY */

#endif /* AMINETXDUO_NET68K_H */

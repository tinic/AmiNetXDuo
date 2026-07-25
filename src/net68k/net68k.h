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
 * 0xFFFFFFFF and is zero only when every longword was zero, which is what
 * makes it interchangeable with the 16-bit accumulation NetX Duo does.
 *
 * `p` is read as longwords, so on a strict-alignment host it must be 4-byte
 * aligned.  The 68020 does not require that, and neither does the caller
 * below: NetX Duo only ever enters the loop on a packet's prepend pointer,
 * which the pool keeps longword aligned.
 */
ULONG n68k_sum_longwords(const ULONG *p, ULONG count);

/*
 * The replacement for _nx_ip_checksum_compute().  Same signature, same
 * semantics, same side effects (it zero-pads a trailing odd byte in the
 * packet buffer exactly where the vendored code does), differing only in the
 * inner loop.  n68k_checksum_hook.c gives it the vendored name; the perf
 * harness links the algorithm without the hook so it can A/B against the
 * vendored implementation in one process.
 */
USHORT n68k_ip_checksum_compute(NX_PACKET *packet_ptr, ULONG protocol,
                                UINT data_length, ULONG *src_ip_addr,
                                ULONG *dest_ip_addr);

#ifdef __cplusplus
}
#endif

#endif /* AMINETXDUO_NET68K_H */

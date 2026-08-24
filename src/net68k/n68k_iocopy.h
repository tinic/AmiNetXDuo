/*
 * AmiNetXDuo, bulk moves for card memory and for a mirrored data port.
 *
 * Separate from net68k.h deliberately: that header declares the checksum over
 * NX_PACKET, and anxnet.device links neither NetX Duo nor ThreadX.  These
 * routines need nothing but exec types, so the driver can take them without
 * taking the stack.
 *
 * The far side of every one of them accepts word and longword accesses, and
 * never a byte access.  That is why n68k_copy_bytes() cannot be used there.
 * The header of n68k_iocopy.S has the detail.  Both pointers must be even,
 * and `longs` and `blocks` are counts of longwords and of 32-byte blocks, not
 * byte lengths.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_N68K_IOCOPY_H
#define AMINETXDUO_N68K_IOCOPY_H

#include <exec/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Memory to memory, or memory to a mapped card buffer.  Both sides advance. */
VOID n68k_copy_longs(volatile void *to, const volatile void *from,
                     ULONG longs);

/* A data port that does not advance a host address: `port` is one address. */
VOID n68k_port_in(void *to, const volatile void *port, ULONG blocks);
VOID n68k_port_out(volatile void *port, const void *from, ULONG blocks);

/* The same, for a 16-bit port that is one address and not mirrored: the reads
   stay word-wide, the host side moves a block at a time. */
VOID n68k_port_in_w(void *to, const volatile void *port, ULONG blocks);

/* Drain + longword ones-complement sum in one pass; the sum has exactly
   n68k_copy_sum_longwords() semantics so the verifier cannot tell who
   produced it.  Destination must be even. */
ULONG n68k_port_in_w_sum(void *to, const volatile void *port, ULONG bytes);
VOID n68k_port_out_w(volatile void *port, const void *from, ULONG blocks);

#ifdef __cplusplus
}
#endif

#endif /* AMINETXDUO_N68K_IOCOPY_H */

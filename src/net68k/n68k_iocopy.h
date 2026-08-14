/*
 * AmiNetXDuo, bulk moves for card memory and for a mirrored data port.
 *
 * Separate from net68k.h deliberately: that header declares the checksum over
 * NX_PACKET, and anxnet.device links neither NetX Duo nor ThreadX.  These
 * three routines need nothing but exec types, so the driver can take them
 * without taking the stack.
 *
 * The far side of all three tolerates word and longword accesses and NOT byte
 * accesses, which is why n68k_copy_bytes() cannot be used there; see the
 * header of n68k_iocopy.S.  Both pointers must be even, and `longs` and
 * `blocks` are counts of longwords and of 32-byte blocks, not byte lengths.
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

#ifdef __cplusplus
}
#endif

#endif /* AMINETXDUO_N68K_IOCOPY_H */

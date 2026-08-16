/*
 * AmiNetXDuo, give net68k's bulk copy the C library's name.
 *
 * The three bulk copies in NetX Duo -- nx_packet_data_append(),
 * nx_packet_data_extract_offset() and the nx_packet_copy() the loopback and
 * driver paths run -- all reach memcpy(), which is library code this tree does
 * not own.  A definition here makes the linker resolve memcpy() from this
 * archive and never pull the libm020 member.  It is the mechanism
 * src/net68k/n68k_checksum_hook.c uses for the checksum.  Nothing is patched
 * and no call site changes.
 *
 * A 23% gain on a primitive is not a 23% gain on a stack, so the effect is
 * measured.  docs/RESEARCH.md holds the table: n68k_copy_bytes() runs at
 * 176.6 ns/B against 216-224 ns/B for the libm020 memcpy, the loopback data
 * path spends three copies per byte, and the end-to-end effect on a megabyte
 * of TCP is there too.  The option exists so that the two can be built from
 * one tree and compared.
 *
 * This does not make memcpy() safe for overlapping regions.  The C library
 * version is not safe for them either.  memmove() covers that case and is
 * left alone.
 *
 * SPDX-License-Identifier: MIT
 */

#include "net68k.h"

#include <stddef.h>

void *memcpy(void *dst, const void *src, size_t n)
{

    N68K_COPY_BYTES((UCHAR *)dst, (const UCHAR *)src, (ULONG)n);

    return(dst);
}

/*
 * AmiNetXDuo, give net68k's bulk copy the C library's name.
 *
 * NetX Duo's three bulk copies, nx_packet_data_append(),
 * nx_packet_data_extract_offset() and the nx_packet_copy() the loopback and
 * driver paths run, all reach memcpy(), and memcpy() is vendored code we do
 * not own.  Defining the symbol here means the linker resolves it from this
 * archive and never pulls libm020's member, which is the same mechanism
 * src/net68k/n68k_checksum_hook.c uses for the checksum.  Nothing is patched
 * and no call site changes.
 *
 * Measured, since 23% on a primitive is not 23% on a stack.  See
 * docs/RESEARCH.md: n68k_copy_bytes() is 176.6 ns/B against libm020 memcpy's
 * 216-224 ns/B, the loopback data path spends three copies per byte, and the
 * end-to-end effect on a megabyte of TCP is in that table.  The option exists
 * so the two can be built from one tree and compared.
 *
 * This does not make memcpy() safe for overlapping regions.  Neither does the
 * C library's, that is what memmove() is for, and memmove() is left alone.
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

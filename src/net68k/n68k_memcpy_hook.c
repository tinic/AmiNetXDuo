/*
 * AmiNetXDuo, give net68k's bulk copy the C library's name.
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

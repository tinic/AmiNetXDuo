/*
 * AmiNetXDuo, give net68k's bulk copy the C library's name.
 *
 * This does not make memcpy() safe for overlapping regions.  The C library
 * version is not safe for them either.  memmove() covers that case and is
 * left alone.
 *
 * <string.h> IS LOAD-BEARING HERE, not a tidiness.  Under -mregparm=3 the
 * compiler calls memcpy by NAME with every argument on the stack, whatever
 * the caller looks like: `*p = g' leaves `pea 64.w / pea _g / move.l a0,-(sp)
 * / jsr _memcpy', and an explicit call does the same.  A definition of memcpy
 * reads those arguments off the stack for the same reason -- the name is
 * recognized -- and not because anything here says so.  Measured on
 * m68k-amigaos-gcc 16.2.0b: the identical body with -fno-builtin instead
 * reads d0/a0/a1, so the recognition is the only thing holding the two sides
 * together.
 *
 * The include makes it stated rather than recognized.  This toolchain's own
 * string.h declares `__stdargs void *memcpy(...)' -- the same
 * __attribute__((__stkparm__)) this tree spells AMIGA_ASM_ARGS -- so the
 * definition inherits the stack convention from the declaration, and stays
 * right if -fno-builtin is ever added.  It costs nothing: the hook is already
 * stack-convention, so the emitted code is unchanged.
 *
 * SPDX-License-Identifier: MIT
 */

#include "net68k.h"

#include <stddef.h>
#include <string.h>

void *memcpy(void *dst, const void *src, size_t n)
{

    N68K_COPY_BYTES((UCHAR *)dst, (const UCHAR *)src, (ULONG)n);

    return(dst);
}

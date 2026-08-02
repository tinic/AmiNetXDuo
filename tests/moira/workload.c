/*
 * AmiNetXDuo -- a workload for the cycle-attribution prototype.
 *
 * Deliberately C, compiled by the same m68k-amigaos-gcc the Amiga build uses,
 * and deliberately calling out to the two shipped assembly primitives: the
 * point of the exercise is a call tree with both languages in it, reached
 * through the linker's RELOC32 fixups, which is the shape a NetX Duo profile
 * would have.  A flat objcopy image would not have proved that -- hand-written
 * position-independent assembly needs no relocation.
 *
 * Freestanding: no libc, no OS, nothing that would drag in a stub.
 *
 * SPDX-License-Identifier: MIT
 */

typedef unsigned long  ULONG;
typedef unsigned char  UCHAR;

extern void  n68k_copy_bytes(UCHAR *to, const UCHAR *from, ULONG len);
extern ULONG n68k_sum_longwords(const ULONG *p, ULONG count);

/* A pure-C loop in the same profile, so the report has something to compare
   the assembly against.  Marked noinline so it keeps its own frame -- the
   shadow stack can only attribute what the compiler left as a call. */
__attribute__((noinline))
static ULONG wl_fold(const ULONG *p, ULONG count)
{
ULONG   s = 0;

    while (count--) s += *p++;

    return(s);
}

__attribute__((noinline))
ULONG wl_stage(UCHAR *dst, const UCHAR *src, ULONG len)
{
    n68k_copy_bytes(dst, src, len);

    return(n68k_sum_longwords((const ULONG *)dst, len / 4) + wl_fold((const ULONG *)dst, 8));
}

ULONG wl_run(ULONG reps, UCHAR *src, UCHAR *dst)
{
ULONG   acc = 0;
ULONG   i;

    for (i = 0; i < reps; i++)
    {
        acc += wl_stage(dst, src, 1460);
        acc += wl_stage(dst, src, 20);
    }

    return(acc);
}

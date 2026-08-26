/*
 * AmiNetXDuo, pick this machine's inner loops.
 *
 * The vectors start on the 68000 forms rather than on NULL, so a caller that
 * never calls n68k_cpu_select() gets the slow answer and not an address error:
 * the 68000 copy carries a parity guard the faster forms omit.
 *
 * AttnFlags is cumulative -- a 68060 sets the 010, 020, 030 and 040 bits as
 * well -- so this reads highest first.
 *
 * SPDX-License-Identifier: MIT
 */

#include "net68k.h"

#ifdef N68K_MV_MULTI

#include <exec/execbase.h>

extern ULONG n68k_sum_longwords_mv0(const ULONG *p, ULONG count);
extern ULONG n68k_sum_longwords_mv20(const ULONG *p, ULONG count);
extern ULONG n68k_sum_longwords_mv40(const ULONG *p, ULONG count);
extern ULONG n68k_sum_longwords_mv60(const ULONG *p, ULONG count);

extern ULONG n68k_copy_sum_longwords_mv0(ULONG *to, const ULONG *from,
                                         ULONG count);
extern ULONG n68k_copy_sum_longwords_mv20(ULONG *to, const ULONG *from,
                                          ULONG count);
extern ULONG n68k_copy_sum_longwords_mv40(ULONG *to, const ULONG *from,
                                          ULONG count);
extern ULONG n68k_copy_sum_longwords_mv60(ULONG *to, const ULONG *from,
                                          ULONG count);

extern VOID n68k_copy_bytes_mv0(UCHAR *to, const UCHAR *from, ULONG len);
extern VOID n68k_copy_bytes_mv20(UCHAR *to, const UCHAR *from, ULONG len);
extern VOID n68k_copy_bytes_mv40(UCHAR *to, const UCHAR *from, ULONG len);
extern VOID n68k_copy_bytes_mv60(UCHAR *to, const UCHAR *from, ULONG len);

/* n68k_dispatch.S jumps through these three.  They are written once, before
   the first packet, and read on every one after that. */
ULONG (*n68k_vec_sum)(const ULONG *, ULONG) = n68k_sum_longwords_mv0;
ULONG (*n68k_vec_copy_sum)(ULONG *, const ULONG *, ULONG) =
    n68k_copy_sum_longwords_mv0;
VOID (*n68k_vec_copy)(UCHAR *, const UCHAR *, ULONG) = n68k_copy_bytes_mv0;

VOID n68k_cpu_select(ULONG attnflags)
{

    if ((attnflags & AFF_68060) != 0UL)
    {
        n68k_vec_sum      = n68k_sum_longwords_mv60;
        n68k_vec_copy_sum = n68k_copy_sum_longwords_mv60;
        n68k_vec_copy     = n68k_copy_bytes_mv60;
    }
    else if ((attnflags & AFF_68040) != 0UL)
    {
        n68k_vec_sum      = n68k_sum_longwords_mv40;
        n68k_vec_copy_sum = n68k_copy_sum_longwords_mv40;
        n68k_vec_copy     = n68k_copy_bytes_mv40;
    }
    else if ((attnflags & AFF_68020) != 0UL)
    {
        n68k_vec_sum      = n68k_sum_longwords_mv20;
        n68k_vec_copy_sum = n68k_copy_sum_longwords_mv20;
        n68k_vec_copy     = n68k_copy_bytes_mv20;
    }
    else
    {
        n68k_vec_sum      = n68k_sum_longwords_mv0;
        n68k_vec_copy_sum = n68k_copy_sum_longwords_mv0;
        n68k_vec_copy     = n68k_copy_bytes_mv0;
    }
}

#else

/* One variant, chosen when it was assembled: the call is direct and there is
   nothing to select.  The entry point still exists so that a caller does not
   have to know which build it is in. */
VOID n68k_cpu_select(ULONG attnflags)
{

    (VOID)attnflags;
}

#endif /* N68K_MV_MULTI */

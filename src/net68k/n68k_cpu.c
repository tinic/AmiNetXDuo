/*
 * AmiNetXDuo, pick this machine's inner loops.
 *
 * The three routines in this directory each exist in one form per CPU class
 * (n68k_variant.h), and this is where one of them is chosen.  Until the `any`
 * build the choice was the preprocessor's and the archive carried a library
 * per CPU; here it is AttnFlags at library init and there is one binary.
 *
 * The vectors start on the 68000 forms rather than on NULL, so a caller that
 * links net68k and never calls n68k_cpu_select() -- a Shell command, a bench,
 * a test -- gets the slow answer and not an address error.  Every variant is
 * assembled from original 68000 instructions, so a wrong answer here is only
 * ever a wrong speed; the one exception is the parity guard in the 68000 copy,
 * which the faster forms omit because they run on parts that fetch a longword
 * from an odd address, and that is what makes the 68000 vector the safe start.
 *
 * AttnFlags is what Exec worked out at boot and is cumulative: a 68060 sets
 * the 010, 020, 030 and 040 bits as well, so this reads highest first.  A
 * 68010 lands on 0 and a 68030 on 20, which is the mapping the per-CPU builds
 * already had (cmake/toolchain-m68k-amigaos.cmake).
 *
 * AFF_68060 IS NOT SET BY THE ROM.  Kickstart 3.1 predates the part; the bit
 * is set by 68060.library, which every real 68060 installs because the machine
 * needs it for the instructions the 68060 dropped.  A machine without it reads
 * 0x0F and lands on the 68040 forms -- measured, `-c 68060` under Amiberry
 * with a 3.1 ROM does exactly that (tests/perf/n68kmv).  That is a slower
 * answer and never a wrong one: the two differ in whether the copy uses
 * movem.l, and nothing here is an instruction the 68060 lacks.
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

/* The C data path, `-mcpu=68060`, for anything with AFF_68020 set. */
extern USHORT n68k_ip_checksum_compute_fast(NX_PACKET *packet_ptr,
                                            ULONG protocol, UINT data_length,
                                            ULONG *src_ip_addr,
                                            ULONG *dest_ip_addr);
#ifdef AMINETXDUO_RX_VERIFY
extern ULONG n68k_rx_verify_fast(NX_PACKET *packet, UINT *drop);
extern ULONG n68k_rx_verify_sum_fast(NX_PACKET *packet, ULONG carried,
                                     ULONG copied, UINT *drop);
#endif

/* n68k_dispatch.S jumps through these three.  They are written once, before
   the first packet, and read on every one after that. */
ULONG (*n68k_vec_sum)(const ULONG *, ULONG) = n68k_sum_longwords_mv0;
ULONG (*n68k_vec_copy_sum)(ULONG *, const ULONG *, ULONG) =
    n68k_copy_sum_longwords_mv0;
VOID (*n68k_vec_copy)(UCHAR *, const UCHAR *, ULONG) = n68k_copy_bytes_mv0;

/* The C ones start on the copy that is legal everywhere, for the same reason
   the assembly does: this file is linked by programs that never select. */
USHORT (*n68k_vec_ip_checksum)(NX_PACKET *, ULONG, UINT, ULONG *, ULONG *) =
    n68k_ip_checksum_compute;
#ifdef AMINETXDUO_RX_VERIFY
ULONG (*n68k_vec_rx_verify)(NX_PACKET *, UINT *) = n68k_rx_verify;
ULONG (*n68k_vec_rx_verify_sum)(NX_PACKET *, ULONG, ULONG, UINT *) =
    n68k_rx_verify_sum;
#endif

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

    /* The C has one boundary rather than four: 32-bit multiply, scaled index
       and bitfield exist above a 68010 and nowhere below it. */
    if ((attnflags & AFF_68020) != 0UL)
    {
        n68k_vec_ip_checksum   = n68k_ip_checksum_compute_fast;
#ifdef AMINETXDUO_RX_VERIFY
        n68k_vec_rx_verify     = n68k_rx_verify_fast;
        n68k_vec_rx_verify_sum = n68k_rx_verify_sum_fast;
#endif
    }
    else
    {
        n68k_vec_ip_checksum   = n68k_ip_checksum_compute;
#ifdef AMINETXDUO_RX_VERIFY
        n68k_vec_rx_verify     = n68k_rx_verify;
        n68k_vec_rx_verify_sum = n68k_rx_verify_sum;
#endif
    }
}

#else

/* One variant, chosen when it was assembled: the call is direct and there is
   nothing to select.  The entry point still exists so a caller need not know
   which build it is in. */
VOID n68k_cpu_select(ULONG attnflags)
{

    (VOID)attnflags;
}

#endif /* N68K_MV_MULTI */

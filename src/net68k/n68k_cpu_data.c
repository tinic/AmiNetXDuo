/*
 * AmiNetXDuo, pick this machine's data-path C.
 *
 * The other half of n68k_cpu.c.  The checksum walk and the receive
 * verification are compiled twice, `-m68000` and `-mcpu=68060`, and this is
 * where one pair is chosen -- and it is also where n68k_cpu_select() lives,
 * because a caller wants one call and not two.
 *
 * Two copies rather than four: the assembly variants differ over which 68000
 * instruction is fastest on which part, which is four answers, and the C ones
 * differ over which instructions exist at all, which is one boundary.
 * n68k_variant.h has the rest, including why the fast copy is built for the
 * 68060 rather than the 68020.
 *
 * Splitting this off from n68k_cpu.c is not tidiness: naming these functions
 * is what pulls them, and their file, and nx_ip_checksum's whole neighbourhood
 * into the link.  A program that only wants the copy loops calls
 * n68k_cpu_select_prims() and links none of it.
 *
 * SPDX-License-Identifier: MIT
 */

#include "net68k.h"

#ifdef N68K_MV_MULTI

#include <exec/execbase.h>

extern USHORT n68k_ip_checksum_compute_fast(NX_PACKET *packet_ptr,
                                            ULONG protocol, UINT data_length,
                                            ULONG *src_ip_addr,
                                            ULONG *dest_ip_addr);
#ifdef AMINETXDUO_RX_VERIFY
extern ULONG n68k_rx_verify_fast(NX_PACKET *packet, UINT *drop);
extern ULONG n68k_rx_verify_sum_fast(NX_PACKET *packet, ULONG carried,
                                     ULONG copied, UINT *drop);
#endif

/* Start on the copy that is legal everywhere, as the primitives do. */
USHORT (*n68k_vec_ip_checksum)(NX_PACKET *, ULONG, UINT, ULONG *, ULONG *) =
    n68k_ip_checksum_compute;
#ifdef AMINETXDUO_RX_VERIFY
ULONG (*n68k_vec_rx_verify)(NX_PACKET *, UINT *) = n68k_rx_verify;
ULONG (*n68k_vec_rx_verify_sum)(NX_PACKET *, ULONG, ULONG, UINT *) =
    n68k_rx_verify_sum;
#endif

VOID n68k_cpu_select(ULONG attnflags)
{

    n68k_cpu_select_prims(attnflags);

    /* One boundary: the 32-bit multiply, the scaled index and the bitfield
       exist above a 68010 and nowhere below it. */
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

#endif /* N68K_MV_MULTI */

/*
 * AmiNetXDuo -- crypto68k: SHA-256, for the TLS record path and everything
 * else in the handshake that hashes.
 *
 *   docs/RESEARCH.md 15.7 ends with the plainest sentence in that section:
 *   "the largest single lever available to https:// on a classic Amiga is
 *   still an unwritten 68020 SHA-256."  This is it.  We were already 1.28x
 *   AmiSSL on HMAC-SHA256 and both sides were portable C, so the 1.28x was a
 *   statement about two C implementations and not about the machine.
 *
 *   The C below is 1.29x the vendored implementation on the same buffer --
 *   85,952 us to 66,687 for 16 KiB -- and none of that came from assembly.
 *   Two changes did it:
 *
 *     1. The sixteen message words are LOADED.  This is a big-endian machine,
 *        so W[t] for t < 16 is the longword at data + 4t, and on a 68020 that
 *        is one MOVE.L at any alignment.  nx_crypto_sha2.c builds each one
 *        from four byte loads, three shifts and three ORs.
 *     2. The message schedule is computed up front rather than interleaved
 *        with the rounds, which stops the round loop's two scratch registers
 *        from having to serve both.
 *
 *   A hand-written 68020 compression function WAS written, checked and timed
 *   against this, on the argument that SHA-256's rotations map onto ROR.L,
 *   ROL.L and SWAP and that a compiler cannot use a count above eight without
 *   burning a data register.  It did not win: 67,656 us against 66,687.  The
 *   measured instruction costs say why -- on this part ROR.L #n is 5.94
 *   cycles and ROR.L Dm,Dn is 7.91, so SWAP-then-rotate (9.89) and
 *   MOVEQ-then-rotate (9.91) are the same price.  The SWAP idiom is a 68000
 *   habit, where a rotate cost 8+2n and the trick was worth a great deal; a
 *   68020's shifter is flat and it is worth nothing.  docs/RESEARCH.md 18 has
 *   the table.
 *
 * THE INTERFACE IS nx_crypto's, DELIBERATELY
 *
 *   c68k_sha256_initialize / _update / _digest_calculate have the same
 *   signatures as _nx_crypto_sha256_initialize and friends, because
 *   _nx_crypto_hmac_metadata_set() takes those three as function pointers.
 *   So HMAC-SHA256 is nx_crypto's own framing with the hash swapped
 *   underneath it, and none of the padding, key-shortening or ipad/opad logic
 *   is reimplemented here.  See src/tls/ami_tls_crypto.c.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_C68K_SHA256_H
#define AMINETXDUO_C68K_SHA256_H

#include "nx_crypto.h"

#ifdef __cplusplus
extern "C" {
#endif

#define C68K_SHA256_BLOCK_SIZE      64u
#define C68K_SHA256_DIGEST_SIZE     32u

typedef struct C68K_SHA256_STRUCT
{
    ULONG   c68k_sha256_state[8];
    ULONG   c68k_sha256_bits_hi;
    ULONG   c68k_sha256_bits_lo;
    ULONG   c68k_sha256_used;
    UCHAR   c68k_sha256_buffer[C68K_SHA256_BLOCK_SIZE];
} C68K_SHA256;

/*
 * `algorithm` is accepted and ignored beyond a check, so that the three
 * functions are drop-in for nx_crypto's.  SHA-224 is not implemented; it is
 * not on any path this client takes.
 */
UINT c68k_sha256_initialize(C68K_SHA256 *ctx, UINT algorithm);
UINT c68k_sha256_update(C68K_SHA256 *ctx, UCHAR *input, UINT input_length);
UINT c68k_sha256_digest_calculate(C68K_SHA256 *ctx, UCHAR *digest,
                                  UINT algorithm);

/*
 * The compression function itself: `blocks` 64-byte blocks folded into the
 * eight-word state.  `data` needs no alignment -- the 68020 does misaligned
 * longword reads in hardware, and a TLS record's payload starts 21 bytes into
 * the packet buffer.
 *
 * Public because it is the thing being measured, and because measuring it
 * through the update/padding layer measures the padding layer too.
 */
VOID c68k_sha256_blocks(ULONG *state, const UCHAR *data, ULONG blocks);


/* ---------------------------------------------------------- the variants -- */

/*
 * There is only one, and that is the result.  A 68020 assembly compression
 * function was written, checked against the vectors and measured against this
 * C in the same process: 67,656 us against 66,687 for 16 KiB on an aligned
 * buffer, 67,653 against 70,241 on a misaligned one.  Once the misaligned
 * MOVE.L -- the assembly's only real advantage -- moved into the C as three
 * lines of inline assembly, the C was ahead on both and 230 lines of hand
 * written rounds had nothing left to earn.  See docs/RESEARCH.md 18 for the
 * instruction costs that explain why, and for the 68000-era SWAP idiom that
 * a 68020's flat shifter makes pointless.
 */
#define C68K_SHA256_V_C     0u  /* portable C, schedule computed up front  */
#define C68K_SHA256_V_COUNT 1u

#define C68K_SHA256_V_BEST  C68K_SHA256_V_C

extern UINT c68k_sha256_variant;

const char *c68k_sha256_variant_name(UINT variant);
UINT c68k_sha256_variant_is_asm(UINT variant);

#ifdef __cplusplus
}
#endif

#endif /* AMINETXDUO_C68K_SHA256_H */

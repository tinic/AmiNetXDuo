/*
 * AmiNetXDuo -- crypto68k: AES-128/192/256 in CBC, for the TLS record path.
 *
 * WHY THIS EXISTS
 *
 *   docs/RESEARCH.md 15 measured the bulk path against AmiSSL and found a dead
 *   heat -- 85.3 ms against 84.2 ms for 16 KiB of AES-128-CBC -- for the same
 *   reason on both sides: neither tree has a single byte of m68k assembly for
 *   it.  That row is the one that decides `https://` throughput, because it is
 *   paid on every byte of every transfer rather than once per connection.
 *
 *   The ciphersuites this client actually negotiates are 0xC027 and 0xC023,
 *   both AES-128-CBC with HMAC-SHA256, so this and c68k_sha256.c are the whole
 *   of the record path.  GCM is compiled in and never reached.
 *
 * WHAT THE MACHINE SAYS, AND WHAT IT CHANGED
 *
 *   tests/perf/cpucal on the A1200 profile, -k 56:
 *
 *     32 KB / 64 B read ratio  0.88x        -- there is NO data cache
 *     Fast RAM longword read   117.7 ns     -- 6.6 cycles, against ADD.L's 2
 *     Fast RAM byte read        29.4 ns/B
 *     instruction cache        256 bytes
 *
 *   No data cache is the interesting constraint, because the classic AES
 *   four-table layout is an argument about cache footprint, and on this
 *   machine footprint costs nothing at all: a lookup into a 4 KB table and a
 *   lookup into a 1 KB table are the same instruction with the same bus cycle.
 *   So the usual reason to prefer one table and three rotates over four tables
 *   -- the 3 KB it saves in a cache that would otherwise hold the state --
 *   does not exist here, and the rotates are pure loss.  That is the single
 *   biggest decision in this file and it was measured, not assumed; see
 *   c68k_aes_variant below and the numbers in docs/RESEARCH.md 16.
 *
 *   The other half of the answer is that AES on this machine is NOT memory
 *   bound.  160 table reads per block at ~140 ns is 22 us against a measured
 *   83 us per block, so three quarters of the cost is instruction issue, and
 *   the lever is the instruction count of the byte extraction rather than the
 *   number of bytes read.  A byte-oriented S-box implementation -- which
 *   trades 4-byte reads for 1-byte reads and pays for MixColumns in the ALU --
 *   moves the wrong one of those two, and measured accordingly.
 *
 * CONSTANT TIME: NO.  Table-driven AES is not, on any machine without a data
 * cache to hide the access pattern in, and this one has neither the cache nor
 * a defence.  docs/RESEARCH.md 9's threat model -- a vintage machine on a LAN,
 * no remote timing attacker -- is what makes that acceptable here.  It would
 * not be acceptable for a server.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_C68K_AES_H
#define AMINETXDUO_C68K_AES_H

#include "nx_crypto.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Key schedule.  `ek` is the encryption schedule in the order the round loop
 * reads it, `dk` the equivalent-inverse-cipher schedule for decryption; both
 * are big-endian words, which on this machine means a plain longword load.
 *
 * 60 longwords is AES-256's 14 rounds plus one.  Sized for the largest key
 * because a TLS client picks the suite at run time.
 */
typedef struct C68K_AES_STRUCT
{
    ULONG   c68k_aes_ek[60];
    ULONG   c68k_aes_dk[60];
    UINT    c68k_aes_rounds;
} C68K_AES;

/*
 * key_bits is 128, 192 or 256.  Returns NX_CRYPTO_SUCCESS or
 * NX_CRYPTO_INVALID_PARAMETER; builds both schedules, because a TLS session
 * encrypts in one direction and decrypts in the other with different keys and
 * the cost is a few microseconds once per connection.
 */
UINT c68k_aes_key_set(C68K_AES *aes, const UCHAR *key, UINT key_bits);

/*
 * One block, ECB.  Present because the vectors are specified that way and
 * because CCM/CTR would want it; the record path uses the CBC entry points
 * below, which do not call through here.
 */
VOID c68k_aes_encrypt_block(const C68K_AES *aes, const UCHAR *in, UCHAR *out);
VOID c68k_aes_decrypt_block(const C68K_AES *aes, const UCHAR *in, UCHAR *out);

/*
 * CBC over `blocks` 16-byte blocks.  `iv` is read and updated in place, so a
 * record can be encrypted in several calls; `in` and `out` may be the same
 * pointer but must not otherwise overlap.  Neither needs any alignment: the
 * 68020 does misaligned longword accesses in hardware and a TLS record's
 * payload starts 21 bytes into the buffer.
 *
 * Fused rather than "call a block cipher through a function pointer per 16
 * bytes, then XOR in a second pass", which is what nx_crypto_cbc.c does.  The
 * chaining value never leaves registers.
 */
VOID c68k_aes_cbc_encrypt(const C68K_AES *aes, UCHAR *iv,
                          const UCHAR *in, UCHAR *out, ULONG blocks);
VOID c68k_aes_cbc_decrypt(const C68K_AES *aes, UCHAR *iv,
                          const UCHAR *in, UCHAR *out, ULONG blocks);


/* ---------------------------------------------------------- the variants -- */

/*
 * Which implementation the entry points above dispatch to.  A variable and
 * not a build option for the same reason c68k_karatsuba_limbs is one: the
 * question "which of these is right for a machine with no data cache" is
 * answered by timing them against each other in ONE process on the same
 * buffer, and by checking each against the others' output before believing
 * any of it.  tests/crypto68k/crypto68k_bulk does exactly that.
 *
 * The default is C68K_AES_V_BEST, which is whichever won.
 */
#define C68K_AES_V_T4_C     0u  /* four 1 KB tables, portable C              */
#define C68K_AES_V_T1_C     1u  /* one 1 KB table + three rotates, portable C */
#define C68K_AES_V_SBOX_C   2u  /* 256-byte S-box, MixColumns in the ALU     */
#define C68K_AES_V_T4_ASM   3u  /* four 1 KB tables, 68020 assembly          */
#define C68K_AES_V_T1_ASM   4u  /* one 1 KB table + rotates, 68020 assembly  */
#define C68K_AES_V_COUNT    5u

#define C68K_AES_V_BEST     C68K_AES_V_T4_ASM

extern UINT c68k_aes_variant;

/* Name of a variant, for a report that cannot mislabel a row. */
const char *c68k_aes_variant_name(UINT variant);

/* NX_CRYPTO_TRUE when `variant` is assembly that this build actually has. */
UINT c68k_aes_variant_is_asm(UINT variant);

#ifdef __cplusplus
}
#endif

#endif /* AMINETXDUO_C68K_AES_H */

/*
 * AmiNetXDuo, crypto68k: AES-128/192/256 in CBC, for the TLS record path.
 *
 *   docs/RESEARCH.md 15 measured the bulk path against AmiSSL and found the
 *   two equal, 85.3 ms against 84.2 ms for 16 KiB of AES-128-CBC, for the same
 *   reason on both sides: neither tree had any m68k assembly for it then.
 *   c68k_aes.S was written afterwards and is the default variant on a 68020.
 *   That row decides `https://` throughput, because it is paid on every byte
 *   of every transfer rather than once per connection.
 *
 *   This and c68k_sha256.c are the record path for 0xC027 and 0xC023, both
 *   AES-128-CBC with HMAC-SHA256, which is what a server negotiates when it
 *   will not take the AEAD this client now prefers.  Many servers still do
 *   not, GitHub among them.  c68k_chacha20.c is the other path and the faster
 *   one.
 *
 *   tests/perf/cpucal on the A1200 profile, -k 56:
 *
 *     32 KB / 64 B read ratio  0.88x, no data cache
 *     Fast RAM longword read   117.7 ns, 6.6 cycles, against ADD.L's 2
 *     Fast RAM byte read        29.4 ns/B
 *     instruction cache        256 bytes
 *
 *   No data cache is the constraint that matters.  The classic AES four-table
 *   layout is an argument about cache footprint, and here footprint costs
 *   nothing: a lookup into a 4 KB table and a lookup into a 1 KB table are the
 *   same instruction with the same bus cycle.  The usual reason to prefer one
 *   table and three rotates over four tables, the 3 KB it saves in a cache
 *   that otherwise holds the state, does not apply, and the rotates are pure
 *   loss.  Measured.  See c68k_aes_variant below and docs/RESEARCH.md 18.2.
 *
 *   AES on this machine is also not memory bound.  160 table reads per block
 *   at ~140 ns is 22 us against a measured 83 us per block, so three quarters
 *   of the cost is instruction issue.  The instruction count of the byte
 *   extraction is what matters, not the number of bytes read.  A byte-oriented
 *   S-box implementation trades 4-byte reads for 1-byte reads and pays for
 *   MixColumns in the ALU, which moves the wrong one of those two, and it
 *   measured accordingly.
 *
 * Not constant time.  Table-driven AES cannot be, on a machine with no data
 * cache to hide the access pattern in, and this one has neither the cache nor
 * a defence.  The threat model in docs/RESEARCH.md 9, a vintage machine on a
 * LAN with no remote timing attacker, makes that acceptable here.  It is not
 * acceptable for a server.
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
 * reads it, `dk` the equivalent-inverse-cipher schedule for decryption.  Both
 * are big-endian words, which on this machine means a plain longword load.
 *
 * 60 longwords is the 14 rounds of AES-256 plus one.  Sized for the largest
 * key because a TLS client picks the suite at run time.
 */
typedef struct C68K_AES_STRUCT
{
    ULONG   c68k_aes_ek[60];
    ULONG   c68k_aes_dk[60];
    UINT    c68k_aes_rounds;
} C68K_AES;

/*
 * key_bits is 128, 192 or 256.  Returns NX_CRYPTO_SUCCESS or
 * NX_CRYPTO_INVALID_PARAMETER.  It builds both schedules, because a TLS
 * session encrypts in one direction and decrypts in the other with different
 * keys, and the cost is a few microseconds once per connection.
 */
UINT c68k_aes_key_set(C68K_AES *aes, const UCHAR *key, UINT key_bits);

/*
 * One block, ECB.  The vectors are specified that way and CCM/CTR needs it.
 * The record path uses the CBC entry points below, which do not call through
 * here.
 */
VOID c68k_aes_encrypt_block(const C68K_AES *aes, const UCHAR *in, UCHAR *out);
VOID c68k_aes_decrypt_block(const C68K_AES *aes, const UCHAR *in, UCHAR *out);

/*
 * CBC over `blocks` 16-byte blocks.  `iv` is read and updated in place, so a
 * record can be encrypted in several calls.  `in` and `out` can be the same
 * pointer but must not otherwise overlap.  Neither needs any alignment: the
 * 68020 does misaligned longword accesses in hardware, and the payload of a
 * TLS record starts 21 bytes into the buffer.
 *
 * Fused, rather than a call through a function pointer per 16 bytes with the
 * XOR in a second pass, which is what nx_crypto_cbc.c does.  The chaining
 * value never leaves registers.
 */
VOID c68k_aes_cbc_encrypt(const C68K_AES *aes, UCHAR *iv,
                          const UCHAR *in, UCHAR *out, ULONG blocks);
VOID c68k_aes_cbc_decrypt(const C68K_AES *aes, UCHAR *iv,
                          const UCHAR *in, UCHAR *out, ULONG blocks);


/* ---------------------------------------------------------- the variants, */

/*
 * Which implementation the entry points above dispatch to.  A variable and
 * not a build option for the same reason c68k_karatsuba_limbs is one: which
 * form suits a machine with no data cache is settled by timing them against
 * each other in one process on the same buffer, and by checking each against
 * the output of the others.  tests/crypto68k/crypto68k_bulk does that.
 *
 * The default is C68K_AES_V_BEST, whichever won.
 */
#define C68K_AES_V_T4_C     0u  /* four 1 KB tables, portable C              */
#define C68K_AES_V_T1_C     1u  /* one 1 KB table + three rotates, portable C */
#define C68K_AES_V_SBOX_C   2u  /* 256-byte S-box, MixColumns in the ALU     */
#define C68K_AES_V_T4_ASM   3u  /* four 1 KB tables, 68020 assembly          */
#define C68K_AES_V_T1_ASM   4u  /* one 1 KB table + rotates, 68020 assembly  */
#define C68K_AES_V_COUNT    5u

/*
 * Resolves to the C form in a build without the assembly,
 * AMINETXDUO_CRYPTO68K_ASM=OFF, one of CI's four configurations, so the
 * reported variant never names code that is not there.
 */
#if defined(C68K_MV)
/*
 * One binary for every CPU starts on the C and is moved to the assembly by
 * c68k_cpu_select() when AttnFlags says 68020 or better.  The kernels need the
 * scaled index and nothing else -- an instruction the 68060 kept -- so this
 * follows "has the 68020 addressing modes", not "has the 64-bit MULU.L".
 */
#define C68K_AES_V_BEST     C68K_AES_V_T4_C
#elif defined(C68K_ASM_AES)
#define C68K_AES_V_BEST     C68K_AES_V_T4_ASM
#else
#define C68K_AES_V_BEST     C68K_AES_V_T4_C
#endif

extern UINT c68k_aes_variant;

/* Name of a variant, so a report cannot mislabel a row. */
const char *c68k_aes_variant_name(UINT variant);

/* NX_CRYPTO_TRUE when `variant` is assembly that this build actually has. */
UINT c68k_aes_variant_is_asm(UINT variant);

#ifdef __cplusplus
}
#endif

#endif /* AMINETXDUO_C68K_AES_H */

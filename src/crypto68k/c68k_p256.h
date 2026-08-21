/*
 * AmiNetXDuo, crypto68k: P-256 elliptic curve arithmetic for the 68020.
 *
 *   docs/RESEARCH.md 9 records that after src/crypto68k made RSA 8x faster,
 *   elliptic curve arithmetic became what dominates a TLS handshake: ECDHE
 *   P-256 shared secret 5.18 s, ECDSA P-256 verify 6.97 s on the emulated
 *   68020.  This file is the follow-on.
 *
 *   Already present in the vendored code, and not redone here:
 *
 *     - NAF point multiplication (_nx_crypto_ec_naf_compute).
 *     - Solinas fast reduction for P-256, not Barrett or Montgomery.
 *     - a fixed-base comb table for G, checked as reached on the paths that
 *       matter.  _nx_crypto_ec_fp_projective_multiple dispatches to
 *       _nx_crypto_ec_fp_fixed_multiple when the point argument is
 *       pointer-identical to curve -> nx_crypto_ec_g, and both
 *       _nx_crypto_ecdsa_verify (the u1*G half) and
 *       _nx_crypto_ec_key_pair_generation_extra pass exactly that pointer.
 *       The measured 1.52 s keygen against 5.18 s ECDH shared secret is the
 *       comb working: a fixed-base multiply is ~3.4x a generic one.
 *
 *     - _nx_crypto_huge_number_square is a real Yang squaring, not a multiply.
 *     - Jacobian projective coordinates with one inversion at the end.
 *
 *   So no textbook algorithmic gain was missing.  The slow layer is underneath
 *   them.
 *
 *   Every field operation in nx_crypto_ec.c runs through NX_CRYPTO_HUGE_NUMBER:
 *   a variable-length limb array with a size field, a sign field, and an
 *   _nx_crypto_huge_number_adjust_size() after every operation.  On top of that
 *   _nx_crypto_ec_secp256r1_reduce(), called ~17 times per point doubling,
 *   does not work on limbs at all.  It calls _nx_crypto_huge_number_extract()
 *   to serialise the value into a 64-byte big-endian byte stream one byte at a
 *   time, memmoves it, calls _nx_crypto_huge_number_setup() to parse 32 of
 *   those bytes back into limbs one byte at a time, then builds nine 8-limb
 *   terms with 64 more per-word byte swaps, and adds and subtracts them as
 *   sign-carrying huge numbers, for a reduction whose whole mathematical
 *   content is "add five limb permutations, subtract four".
 *
 *   So this module drops the wrapper.  A field element is eight 32-bit limbs,
 *   fixed size, always in [0, p), least significant limb first, the same
 *   layout HN_UBASE already uses, so conversion at the boundary is a copy.
 *   The reduction is one pass over eight limb positions with a signed
 *   accumulator and no byte ever touched.  Multiplication reuses
 *   c68k_addmul_1() from crypto68k.h, so the 68020 assembly limb loop that
 *   RSA got carries over.
 *
 *   The point layer changes only in the small ways that follow from having a
 *   cheap squaring: dbl-2001-b (3M + 5S) instead of the vendored 4M + 4S
 *   doubling, madd-2007-bl (7M + 4S) instead of 8M + 3S, and a width-5 wNAF
 *   instead of a width-2 NAF for generic points, 43 additions per 256
 *   doublings instead of 85.
 *
 *   One routine is hand-written assembly, and only after the C version was
 *   measured.  c68k_p256.S carries the carry chains, the eight-limb add and
 *   subtract, and the Solinas reduction's sixty-three-term pass, because C
 *   has no carry flag and GCC therefore cannot emit ADD.L / ADDX.L.  It spends
 *   five instructions and a branch where the machine needs two.  The multiply
 *   is left to the compiler.  It is already very close to the MULU.L floor of
 *   the 68020, as the sibling RSA work found, and no instruction selection
 *   moves it.
 *
 *   Shamir's trick for ECDSA verify, rejected.  The usual argument is that
 *   verify computes u1*G + u2*Q as two separate scalar multiplications, and
 *   that an interleave halves the doublings.  It does not apply, because u1*G
 *   is not a generic scalar multiplication.  It is a comb, and a comb needs 26
 *   doublings, not 256.  An interleave forces the G half up to 256 shared
 *   doublings to save the 26 it already needs.  Field operations:
 *
 *       separate  comb(G) + wNAF(Q) : 284 doublings + 100 additions
 *       Shamir, both as wNAF        : 258 doublings +  93 additions
 *
 *   Priced at the point-operation costs this module measures on the emulated
 *   68020, 3.88 ms a doubling, 5.19 ms a mixed addition, that is 1.62 s
 *   against 1.48 s, about 8% of a 2.0 s verify, and it needs a second scalar
 *   multiplication routine and a second static table.  Not taken.  The next
 *   8% comes from here, and it is the last cheap one.
 *
 * Measured, emulated 68020, tests/crypto68k/crypto68k_ec_bench.  Every pair is
 * the unmodified vendored routine and this module run back to back in one
 * process, with nothing changed but nx_crypto_ec_multiple in a copy of the
 * curve structure, so the headline three are timed through the real
 * _nx_crypto_ecdsa_verify and _nx_crypto_ecdh_compute_secret:
 *
 *     P-256 Solinas reduction alone     1.019 ms  ->  0.084 ms    12.1x
 *     field multiply + reduce           1.466 ms  ->  0.449 ms     3.3x
 *     field square + reduce             1.254 ms  ->  0.395 ms     3.2x
 *     Jacobian doubling                 14.97 ms  ->   3.88 ms     3.9x
 *     Jacobian + affine addition        16.62 ms  ->   5.19 ms     3.2x
 *     k*G, fixed base                    1.475 s  ->  0.380 s      3.9x
 *     k*Q, generic                       5.184 s  ->  1.334 s      3.9x
 *     ECDHE P-256 keygen                 1.475 s  ->  0.381 s      3.9x
 *     ECDHE P-256 shared secret          5.245 s  ->  1.368 s      3.8x
 *     ECDSA P-256 verify                 7.028 s  ->  1.961 s      3.6x
 *
 * The three vendored absolutes reproduce the 1.52 / 5.18 / 6.97 s measured
 * independently in docs/RESEARCH.md 9 to within 1%, so this benchmark times
 * the same computation the gate did.
 *
 * The ratios are meaningful, the absolute times are not, and only the 68020
 * rows count.  The FS-UAE 68020 is not a 14 MHz A1200.  Its 68030 model
 * reports 4.5x to 5.0x for the same binary, and an ECDSA verify of 196 ms
 * against 7028 ms on the 68020 model, 36x, which is not a clock ratio.  The
 * 68030 model does not charge for MULU.L, so it favours exactly the work this
 * module moves out of the multiply and into carry chains.
 *
 * Not constant time.  wNAF digits and comb digits select table entries by
 * value, and every conditional field correction is a branch.  The vendored
 * code is not constant time either.  For the threat model in docs/RESEARCH.md
 * (a vintage machine on a LAN) that is acceptable.  It is not a defence
 * against a remote timing attacker.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_C68K_P256_H
#define AMINETXDUO_C68K_P256_H

#include "crypto68k.h"
#include "nx_crypto_ec.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * A field element: eight limbs, least significant first, value in [0, p).
 * Fixed size, the variable size field of NX_CRYPTO_HUGE_NUMBER is a large
 * part of what this module avoids.
 */
#define C68K_P256_LIMBS     8u

typedef c68k_limb c68k_p256_fe[C68K_P256_LIMBS];

/* A point in Jacobian coordinates: (X : Y : Z) is affine (X/Z^2, Y/Z^3).
   Z == 0 is the point at infinity, and is the only representation of it. */
typedef struct
{
    c68k_p256_fe    x;
    c68k_p256_fe    y;
    c68k_p256_fe    z;
} c68k_p256_jac;

/* An affine point.  infinity is (0, 0), matching what the vendored
   _nx_crypto_ec_point_is_infinite() recognises. */
typedef struct
{
    c68k_p256_fe    x;
    c68k_p256_fe    y;
} c68k_p256_aff;


/*
 * Working memory for a generic scalar multiplication.  A parameter and not a
 * local because this runs under a 4 KB AmigaOS process stack, and not a
 * static because nx_crypto is otherwise reentrant.
 *
 *   naf   width-5 wNAF digits, at most 257 of them
 *   tab   q, 3q, 5q, ... 15q, Jacobian during the build, then converted in
 *         place to affine, after which the z field is dead
 *   acc   the running products a batch inversion needs
 */
#define C68K_P256_WNAF_W        5u
#define C68K_P256_WNAF_POINTS   (1u << (C68K_P256_WNAF_W - 2u))     /* 8 */

typedef struct
{
    signed char     naf[260];
    c68k_p256_jac   tab[C68K_P256_WNAF_POINTS];
    c68k_p256_fe    acc[C68K_P256_WNAF_POINTS];
} c68k_p256_work;

/* Scratch a caller must have spare, in HN_UBASE limbs, for the drop-in below. */
#define C68K_P256_WORK_LIMBS    ((sizeof(c68k_p256_work) + 3u) >> 2)


/* --------------------------------------------------------- comb table ---- */

/* Lim-Lee comb parameters.  Kept here rather than in the generated file so
   that a table built with different parameters fails to compile. */
#define C68K_P256_COMB_W        5u
#define C68K_P256_COMB_D        52u     /* ceil(256 / w) */
#define C68K_P256_COMB_E        26u     /* ceil(d / 2), the loop count */
#define C68K_P256_COMB_HALF     ((1u << C68K_P256_COMB_W) - 1u)     /* 31 */
#define C68K_P256_COMB_POINTS   (2u * C68K_P256_COMB_HALF)          /* 62 */

/* [0 .. 30] are T1[digit-1].  [31 .. 61] are T2[digit-1] = 2^e * T1[digit-1].
   Each row is x limbs 0..7 then y limbs 0..7.  Generated by
   src/crypto68k/gen_p256_table.py. */
extern const c68k_limb c68k_p256_comb[C68K_P256_COMB_POINTS][16];


/* -------------------------------------------------- carry primitives ----- */

/*
 * The three routines c68k_p256.S hand-writes, with portable C fallbacks in
 * c68k_p256.c under the same build flag as c68k_prim.S.  Exposed rather than
 * static because the assembly has to define them, and because the test drives
 * the reduction directly against the vendored one.
 *
 * GCC cannot generate the ADD.L / ADDX.L pair these need, C has no carry
 * flag, and the disassembly of both plausible C spellings is in the header
 * comment of c68k_p256.S.  The only place in the module where hand-written
 * assembly wins.  The multiply is already at the MULU.L floor and is left to
 * the compiler.
 */

/* r = a + b over eight limbs, no modular reduction.  Returns the carry out. */
c68k_limb c68k_p256_add_raw(c68k_limb *r, const c68k_limb *a, const c68k_limb *b);

/* r = a - b over eight limbs, no modular reduction.  Returns the borrow out. */
c68k_limb c68k_p256_sub_raw(c68k_limb *r, const c68k_limb *a, const c68k_limb *b);

/*
 * One pass of the Solinas reduction: writes r[0..7] and returns the leftover
 * signed coefficient of 2^256, which c68k_p256_fe_reduce() folds in.
 */
INT c68k_p256_reduce_core(c68k_limb *r, const c68k_limb *t);


/* ------------------------------------------------------ field, exposed --- */

/*
 * Exposed so the test can drive them directly against the vendored
 * equivalents.  A field bug otherwise surfaces only as "the wrong point"
 * three thousand operations later.
 */

/* r = a * b mod p.  r can alias a or b. */
VOID c68k_p256_fe_mul(c68k_p256_fe r, const c68k_p256_fe a, const c68k_p256_fe b);

/* r = a * a mod p.  r can alias a. */
VOID c68k_p256_fe_sqr(c68k_p256_fe r, const c68k_p256_fe a);

/* r = a + b mod p, r = a - b mod p.  r can alias either operand. */

/*
 * r = t mod p, where t is a 16-limb product.  The Solinas reduction, and the
 * hottest routine in the module.
 */
VOID c68k_p256_fe_reduce(c68k_p256_fe r, const c68k_limb t[16]);

/* r = a^-1 mod p, by binary extended Euclid.  a == 0 yields 0. */
VOID c68k_p256_fe_inv(c68k_p256_fe r, const c68k_p256_fe a);


/* ------------------------------------------------------------- points ---- */

/* r = 2 * p1.  r can alias p1.  Infinity in, infinity out. */
VOID c68k_p256_jac_double(c68k_p256_jac *r, const c68k_p256_jac *p1);

/*
 * r = p1 + q, q affine.  r can alias p1.  Handles every degenerate case:
 * p1 infinite, q infinite, p1 == q (doubles), and p1 == -q (infinity).
 */
VOID c68k_p256_jac_add_affine(c68k_p256_jac *r, const c68k_p256_jac *p1,
                              const c68k_p256_aff *q);

/* Jacobian to affine.  Infinity becomes (0, 0). */


/* -------------------------------------------- scalar multiplication ------ */

/*
 * r = k * G, using the fixed-base comb.  k is eight limbs, least significant
 * first, and need not be reduced, k == 0 gives infinity.
 */
VOID c68k_p256_mul_g(c68k_p256_aff *r, const c68k_limb k[C68K_P256_LIMBS]);

/*
 * r = k * q for an arbitrary affine q, width-5 wNAF with a batch-inverted
 * affine table.  q must be on the curve.  q at infinity gives infinity.
 * work is scratch, see c68k_p256_work above.  It must not alias anything.
 */
VOID c68k_p256_mul_point(c68k_p256_aff *r, const c68k_p256_aff *q,
                         const c68k_limb k[C68K_P256_LIMBS],
                         c68k_p256_work *work);


/* --------------------------------------------------------- integration --- */

/*
 * Signature-compatible drop-in for the nx_crypto_ec_multiple of NX_CRYPTO_EC.
 * One line wires it in:
 *
 *     curve -> nx_crypto_ec_multiple = c68k_p256_ec_multiple;
 *
 * on the secp256r1 curve only, which makes ECDH key generation, ECDH shared
 * secret and both halves of ECDSA verify take this path with no other change
 * anywhere.  Anything it cannot handle falls through to
 * _nx_crypto_ec_fp_projective_multiple(), so the substitution is safe.  That
 * is a curve that is not secp256r1, or a scalar wider than 256 bits, which
 * _nx_crypto_ec_precomputation() does use.
 */
VOID c68k_p256_ec_multiple(NX_CRYPTO_EC *curve,
                           NX_CRYPTO_EC_POINT *g,
                           NX_CRYPTO_HUGE_NUMBER *d,
                           NX_CRYPTO_EC_POINT *r,
                           HN_UBASE *scratch);

/*
 * Recompute two comb table entries from G with the generic routine and compare.
 * Returns NX_CRYPTO_SUCCESS, or NX_CRYPTO_NOT_SUCCESSFUL if the generated table
 * does not belong to this curve.  Two generic scalar multiplications.  Call it
 * once at startup for the check.
 */
UINT c68k_p256_self_check(VOID);

/* Diagnostic-only return values used by run-lto-probe.sh.  Normal builds
   preserve the public success/not-successful contract above. */
#define C68K_P256_CHECK_GENERIC_X    0x201u
#define C68K_P256_CHECK_GENERIC_Y    0x202u
#define C68K_P256_CHECK_COMB_X       0x203u
#define C68K_P256_CHECK_COMB_Y       0x204u
#define C68K_P256_CHECK_FIELD_MUL    0x211u
#define C68K_P256_CHECK_FIELD_SQR    0x212u
#define C68K_P256_CHECK_FIELD_INV    0x213u

#ifdef __cplusplus
}
#endif

#endif /* AMINETXDUO_C68K_P256_H */

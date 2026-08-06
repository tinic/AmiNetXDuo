/*
 * AmiNetXDuo, crypto68k: X25519 and Ed25519 over eight 32-bit limbs.
 *
 * Read src/crypto68k/c68k_25519.h first; it says why this file exists.  This
 * comment covers how, and the two decisions that are not obvious.
 *
 *   A field element is eight uint32 limbs, little-endian, holding any value
 *   below 2^256 that is congruent to the intended one modulo p = 2^255-19.
 *   Nothing is kept fully reduced until it is serialised.  That works because
 *
 *       2^256 = 2 * 2^255 = 2 * (p + 19) = 2p + 38 = 38  (mod p)
 *
 *   so a 512-bit product folds into 256 bits with eight multiplies by 38, and
 *   an addition that carries out of the top folds with a single add of 38.
 *
 *   For comparison, what Dropbear runs today: TweetNaCl stores sixteen 16-bit
 *   limbs in an `i64[16]` and multiplies with `t[i+j] += a[i]*b[j]` on those
 *   i64s, 256 iterations, each a software 64x64 multiply, on a machine whose
 *   MULU.L does 32x32->64 in one instruction.  Here it is 64 iterations of
 *   that instruction.
 *
 *   Not radix 2^25.5 (ref10's layout): this part has a real 32x32->64 multiply
 *   and a real ADDX, and ref10 splits limbs to 25/26 bits only to keep
 *   products inside a 64-bit accumulator on machines where the carry is
 *   expensive to propagate.  ref10 pays 100 multiplies where this pays 64 (72
 *   with the reduction), and its saving is carry handling this machine does in
 *   the ALU for free.
 *
 *   The Montgomery ladder for X25519 and the double-and-add for Ed25519 are
 *   the textbook algorithms, and the Ed25519 base point has no precomputed
 *   table.  A signed-window base table would cut Ed25519 signing by roughly
 *   another five times; it is a separate, testable change.  This file changes
 *   the field and nothing else, so a measurement of it measures one thing.
 *
 * SPDX-License-Identifier: MIT
 */

#include "c68k_25519.h"

/*
 * <stdint.h> rather than exec/types.h: this file has to compile on the build
 * host for the vector tests, and it never touches an AmigaOS type.
 */
#include <stdint.h>
#include <string.h>

typedef uint32_t fe[8];


/* ------------------------------------------------------------- constants */

/* d = -121665/121666, the Edwards curve constant; d2 = 2d; sqrtm1 = sqrt(-1).
   Generated rather than transcribed, see the derivation in the commit that
   added this file. */
static const fe fe_d      = { 0x135978a3u, 0x75eb4dcau, 0x4141d8abu, 0x00700a4du,
                              0x7779e898u, 0x8cc74079u, 0x2b6ffe73u, 0x52036ceeu };
static const fe fe_d2     = { 0x26b2f159u, 0xebd69b94u, 0x8283b156u, 0x00e0149au,
                              0xeef3d130u, 0x198e80f2u, 0x56dffce7u, 0x2406d9dcu };
static const fe fe_sqrtm1 = { 0x4a0ea0b0u, 0xc4ee1b27u, 0xad2fe478u, 0x2f431806u,
                              0x3dfbd7a7u, 0x2b4d0099u, 0x4fc1df0bu, 0x2b832480u };

static const fe ge_base_x = { 0x8f25d51au, 0xc9562d60u, 0x9525a7b2u, 0x692cc760u,
                              0xfdd6dc5cu, 0xc0a4e231u, 0xcd6e53feu, 0x216936d3u };
static const fe ge_base_y = { 0x66666658u, 0x66666666u, 0x66666666u, 0x66666666u,
                              0x66666666u, 0x66666666u, 0x66666666u, 0x66666666u };
static const fe ge_base_t = { 0xa5b7dda3u, 0x6dde8ab3u, 0x775152f5u, 0x20f09f80u,
                              0x64abe37du, 0x66ea4e8eu, 0xd78b7665u, 0x67875f0fu };

static const fe fe_zero = { 0, 0, 0, 0, 0, 0, 0, 0 };
static const fe fe_one  = { 1, 0, 0, 0, 0, 0, 0, 0 };


/* ------------------------------------------------------ field arithmetic */

static void fe_copy(fe r, const fe a)
{
    int i;
    for (i = 0; i < 8; i++)
        r[i] = a[i];
}

/*
 * Fold a carry out of the top back in.  2^256 = 38 (mod p), so a carry of c
 * out of limb 7 is worth 38c at the bottom.
 *
 * The loop is required.  The first version ran one propagation pass and
 * dropped whatever came out of limb 7, assuming a value near 2^256-1 could not
 * arise.  It arises constantly: this representation is lazy, so 0 is routinely
 * carried as 2^256-38 and 1 as 2^256-37, and adding 38 to either carries
 * straight through all eight limbs.  Dropping that carry loses exactly 38,
 * the symptom was an Ed25519 doubling returning 37 where it owed -1.  A second
 * pass always terminates: a carry out means every limb is now small.
 */
static void fe_fold(fe r, uint32_t c)
{
    uint64_t t = (uint64_t)c * 38u;
    int      i;

    while (t != 0) {
        for (i = 0; i < 8 && t != 0; i++) {
            t += r[i];
            r[i] = (uint32_t)t;
            t >>= 32;
        }
        t *= 38u;
    }
}

static void fe_add_c(fe r, const fe a, const fe b)
{
    uint64_t t = 0;
    int      i;

    for (i = 0; i < 8; i++) {
        t += (uint64_t)a[i] + b[i];
        r[i] = (uint32_t)t;
        t >>= 32;
    }
    fe_fold(r, (uint32_t)t);
}

static void fe_sub_c(fe r, const fe a, const fe b)
{
    uint64_t t = 0;
    int      i;

    /* a - b, borrowing.  A borrow out means the true value is a-b+2^256, and
       2^256 = 38 (mod p), so 38 has to come back off. */
    for (i = 0; i < 8; i++) {
        t = (uint64_t)a[i] - b[i] - (uint32_t)(t >> 63 ? 1u : 0u);
        r[i] = (uint32_t)t;
        t = (t >> 32) & 1u ? (uint64_t)1 << 63 : 0;   /* borrow flag */
    }

    /* Same form as fe_fold, and the same trap: a borrow out of limb 7 is worth
       another 38 off, because dropping it silently adds 2^256 = 38 back on.
       Terminates for the mirror-image reason. */
    while (t) {
        uint64_t s = 38u;

        for (i = 0; i < 8; i++) {
            uint64_t v = (uint64_t)r[i] - (uint32_t)s;
            r[i] = (uint32_t)v;
            s = (v >> 32) & 1u;                        /* borrow */
            if (s == 0)
                break;
        }
        t = s;
    }
}

#if defined(C68K_ASM) || defined(C68K_ASM_MULW)
extern void c68k_fe_add_asm(fe r, const fe a, const fe b);
extern void c68k_fe_sub_asm(fe r, const fe a, const fe b);
#define fe_add  c68k_fe_add_asm
#define fe_sub  c68k_fe_sub_asm
#else
#define fe_add  fe_add_c
#define fe_sub  fe_sub_c
#endif

void c68k_25519_fe_add(uint32_t r[8], const uint32_t a[8], const uint32_t b[8])
{
    fe_add(r, a, b);
}

void c68k_25519_fe_add_ref(uint32_t r[8], const uint32_t a[8],
                           const uint32_t b[8])
{
    fe_add_c(r, a, b);
}

void c68k_25519_fe_sub(uint32_t r[8], const uint32_t a[8], const uint32_t b[8])
{
    fe_sub(r, a, b);
}

void c68k_25519_fe_sub_ref(uint32_t r[8], const uint32_t a[8],
                           const uint32_t b[8])
{
    fe_sub_c(r, a, b);
}

/*
 * The multiply.  Operand scanning, 64 MULU.L, then the 2^256 = 38 fold.
 *
 * The accumulator never overflows: (2^32-1)^2 + (2^32-1) + (2^32-1) is exactly
 * 2^64-1, so the product, the running word and the carry can all be added
 * before the store without a second accumulator word.
 */
static void fe_mul_c(fe r, const fe a, const fe b)
{
    uint32_t t[16];
    uint64_t v;
    int      i, j;

    for (i = 0; i < 16; i++)
        t[i] = 0;

    for (i = 0; i < 8; i++) {
        uint32_t ai = a[i];
        uint32_t c  = 0;

        for (j = 0; j < 8; j++) {
            v = (uint64_t)ai * b[j] + t[i + j] + c;
            t[i + j] = (uint32_t)v;
            c = (uint32_t)(v >> 32);
        }
        t[i + 8] = c;
    }

    /* r = low + 38 * high.  The carry out is below 39, so one fold finishes
       it. */
    v = 0;
    for (i = 0; i < 8; i++) {
        v += (uint64_t)t[i + 8] * 38u + t[i];
        r[i] = (uint32_t)v;
        v >>= 32;
    }
    fe_fold(r, (uint32_t)v);
}

/*
 * Which one the rest of this file calls, the same shape c68k_poly1305.c uses:
 * a macro rather than a wrapper, so a build without the assembly gets no extra
 * call and there is nothing to choose between at run time.
 *
 * BOTH assembly options select it, and they are different code.  C68K_ASM is
 * the 68020/68040 build and c68k_25519.S uses MULU.L there; C68K_ASM_MULW is
 * the 68000 and 68060 build, where that form of MULU.L either does not exist
 * or traps, and the same file builds the product from four MULU.W.  The two
 * are mutually exclusive and src/crypto68k/CMakeLists.txt refuses both at
 * once, because they define the same symbol.
 */
#if defined(C68K_ASM) || defined(C68K_ASM_MULW)
extern void c68k_fe_mul_asm(fe r, const fe a, const fe b);
#define fe_mul  c68k_fe_mul_asm
#else
#define fe_mul  fe_mul_c
#endif

/*
 * The dispatch and the reference, as real functions, so an on-Amiga test can
 * run the shipped kernel and the portable one over the same input and compare.
 * Neither is on any hot path: everything below calls the macro.  They also
 * keep fe_mul_c referenced in an assembly build, where nothing else calls it.
 */
void c68k_25519_fe_mul(uint32_t r[8], const uint32_t a[8], const uint32_t b[8])
{
    fe_mul(r, a, b);
}

void c68k_25519_fe_mul_ref(uint32_t r[8], const uint32_t a[8],
                           const uint32_t b[8])
{
    fe_mul_c(r, a, b);
}

int c68k_25519_fe_mul_is_asm(void)
{
#if defined(C68K_ASM) || defined(C68K_ASM_MULW)
    return 1;
#else
    return 0;
#endif
}

/*
 * The squaring.  Thirty-six multiplies instead of sixty-four, because every
 * off-diagonal product appears twice: sum them once, double the lot, then add
 * the eight diagonal squares.
 *
 * A transcription error here would still produce plausible-looking output, so
 * tests/crypto68k/host checks it against fe_mul(r,a,a) on random inputs rather
 * than only against published vectors.
 */
static void fe_sqr_c(fe r, const fe a)
{
    uint32_t t[16];
    uint64_t v;
    uint32_t c;
    int      i, j;

    for (i = 0; i < 16; i++)
        t[i] = 0;

    for (i = 0; i < 8; i++) {
        uint32_t ai = a[i];

        c = 0;
        for (j = i + 1; j < 8; j++) {
            v = (uint64_t)ai * a[j] + t[i + j] + c;
            t[i + j] = (uint32_t)v;
            c = (uint32_t)(v >> 32);
        }
        t[i + 8] = c;
    }

    /* Double.  The top word cannot carry out: the off-diagonal sum is below
       2^(2*256-1). */
    c = 0;
    for (i = 0; i < 16; i++) {
        uint32_t hi = t[i] >> 31;
        t[i] = (t[i] << 1) | c;
        c = hi;
    }

    /* Add the diagonal. */
    v = 0;
    for (i = 0; i < 8; i++) {
        v += (uint64_t)a[i] * a[i] + t[2 * i];
        t[2 * i] = (uint32_t)v;
        v >>= 32;
        v += t[2 * i + 1];
        t[2 * i + 1] = (uint32_t)v;
        v >>= 32;
    }

    v = 0;
    for (i = 0; i < 8; i++) {
        v += (uint64_t)t[i + 8] * 38u + t[i];
        r[i] = (uint32_t)v;
        v >>= 32;
    }
    fe_fold(r, (uint32_t)v);
}

#if defined(C68K_ASM) || defined(C68K_ASM_MULW)
extern void c68k_fe_sqr_asm(fe r, const fe a);
#define fe_sqr  c68k_fe_sqr_asm
#else
#define fe_sqr  fe_sqr_c
#endif

void c68k_25519_fe_sqr(uint32_t r[8], const uint32_t a[8])
{
    fe_sqr(r, a);
}

void c68k_25519_fe_sqr_ref(uint32_t r[8], const uint32_t a[8])
{
    fe_sqr_c(r, a);
}

/* r = a * k for a small k; used for a24 = 121666 in the ladder. */
static void fe_mul_small(fe r, const fe a, uint32_t k)
{
    uint64_t v = 0;
    int      i;

    for (i = 0; i < 8; i++) {
        v += (uint64_t)a[i] * k;
        r[i] = (uint32_t)v;
        v >>= 32;
    }
    fe_fold(r, (uint32_t)v);
}

/* r = -a.  2p - a is congruent to -a and stays below 2^256 for any a < 2^256
   only after the fold, which is what fe_sub already does with a = 0. */
static void fe_neg(fe r, const fe a)
{
    fe_sub(r, fe_zero, a);
}

/* Swap a and b if b_flag is 1, leave them if it is 0, with no branch on the
   flag and no data-dependent address. */
static void fe_cswap(fe a, fe b, uint32_t bit)
{
    uint32_t mask = (uint32_t)0 - bit;
    int      i;

    for (i = 0; i < 8; i++) {
        uint32_t t = mask & (a[i] ^ b[i]);
        a[i] ^= t;
        b[i] ^= t;
    }
}

static void fe_frombytes(fe r, const unsigned char b[32])
{
    int i;

    for (i = 0; i < 8; i++)
        r[i] = (uint32_t)b[4 * i]
             | ((uint32_t)b[4 * i + 1] << 8)
             | ((uint32_t)b[4 * i + 2] << 16)
             | ((uint32_t)b[4 * i + 3] << 24);
    r[7] &= 0x7fffffffu;                 /* RFC 7748: bit 255 is ignored */
}

/*
 * Full reduction, then little-endian bytes.
 *
 * Two steps.  First fold bit 255 back in, 2^255 = 19 (mod p), which leaves
 * a value below 2^255.  That is still not canonical, because anything from p
 * to 2^255-1 is a second representation of 0..18.  So compute t+19 and look at
 * its bit 255: it is set exactly when t >= p, and in that case t+19-2^255 is
 * t-p.
 */
static void fe_tobytes(unsigned char out[32], const fe a)
{
    fe       t;
    uint32_t s[8];
    uint64_t v;
    uint32_t mask;
    int      i;

    fe_copy(t, a);

    /* Fold twice: the first fold can push the value back over 2^255 only if
       every limb was saturated, and the second cannot. */
    for (i = 0; i < 2; i++) {
        uint32_t c = t[7] >> 31;
        int      j;

        t[7] &= 0x7fffffffu;
        v = (uint64_t)c * 19u;
        for (j = 0; j < 8 && v != 0; j++) {
            v += t[j];
            t[j] = (uint32_t)v;
            v >>= 32;
        }
    }

    v = 19;
    for (i = 0; i < 8; i++) {
        v += t[i];
        s[i] = (uint32_t)v;
        v >>= 32;
    }

    /* mask = all ones when t >= p */
    mask = (uint32_t)0 - (s[7] >> 31);
    s[7] &= 0x7fffffffu;
    for (i = 0; i < 8; i++)
        t[i] ^= mask & (t[i] ^ s[i]);

    for (i = 0; i < 8; i++) {
        out[4 * i]     = (unsigned char)(t[i]);
        out[4 * i + 1] = (unsigned char)(t[i] >> 8);
        out[4 * i + 2] = (unsigned char)(t[i] >> 16);
        out[4 * i + 3] = (unsigned char)(t[i] >> 24);
    }
}

static int fe_isnonzero(const fe a)
{
    unsigned char b[32];
    unsigned char d = 0;
    int           i;

    fe_tobytes(b, a);
    for (i = 0; i < 32; i++)
        d |= b[i];
    return d != 0;
}

static unsigned char fe_isodd(const fe a)
{
    unsigned char b[32];

    fe_tobytes(b, a);
    return (unsigned char)(b[0] & 1);
}

/*
 * r = a^(p-2), the inverse.  The addition chain is ref10's: 254 squarings and
 * 11 multiplies, against the 254 squarings and 251 multiplies that TweetNaCl's
 * square-and-multiply-every-bit loop costs.  That is 240 field multiplies
 * saved per inversion, and there are three inversions in a handshake.
 */
static void fe_pow_2n_mul(fe r, const fe a, int n, const fe m)
{
    fe  t;
    int i;

    fe_copy(t, a);
    for (i = 0; i < n; i++)
        fe_sqr(t, t);
    fe_mul(r, t, m);
}

static void fe_invert(fe out, const fe z)
{
    fe  t0, t1, t2, t3;
    int i;

    fe_sqr(t0, z);                       /* z^2      */
    fe_sqr(t1, t0);
    fe_sqr(t1, t1);                      /* z^8      */
    fe_mul(t1, z, t1);                   /* z^9      */
    fe_mul(t0, t0, t1);                  /* z^11     */
    fe_sqr(t2, t0);                      /* z^22     */
    fe_mul(t1, t1, t2);                  /* 2^5 - 1  */

    fe_pow_2n_mul(t1, t1, 5, t1);        /* 2^10 - 1 */
    fe_pow_2n_mul(t2, t1, 10, t1);       /* 2^20 - 1 */
    fe_pow_2n_mul(t3, t2, 20, t2);       /* 2^40 - 1 */
    fe_pow_2n_mul(t1, t3, 10, t1);       /* 2^50 - 1 */
    fe_pow_2n_mul(t2, t1, 50, t1);       /* 2^100 - 1 */
    fe_pow_2n_mul(t3, t2, 100, t2);      /* 2^200 - 1 */
    fe_pow_2n_mul(t1, t3, 50, t1);       /* 2^250 - 1 */

    fe_copy(t2, t1);
    for (i = 0; i < 5; i++)
        fe_sqr(t2, t2);
    fe_mul(out, t2, t0);                 /* 2^255 - 21 = p - 2 */
}

/* r = a^((p-5)/8), the exponent Ed25519 decompression needs.  Same chain shape
   as the inverse, stopped three squarings earlier. */
static void fe_pow22523(fe out, const fe z)
{
    fe  t0, t1, t2, t3;
    int i;

    fe_sqr(t0, z);
    fe_sqr(t1, t0);
    fe_sqr(t1, t1);
    fe_mul(t1, z, t1);
    fe_mul(t0, t0, t1);
    fe_sqr(t0, t0);
    fe_mul(t0, t1, t0);                  /* 2^5 - 1 */

    fe_pow_2n_mul(t0, t0, 5, t0);        /* 2^10 - 1 */
    fe_pow_2n_mul(t1, t0, 10, t0);       /* 2^20 - 1 */
    fe_pow_2n_mul(t2, t1, 20, t1);       /* 2^40 - 1 */
    fe_pow_2n_mul(t0, t2, 10, t0);       /* 2^50 - 1 */
    fe_pow_2n_mul(t1, t0, 50, t0);       /* 2^100 - 1 */
    fe_pow_2n_mul(t2, t1, 100, t1);      /* 2^200 - 1 */
    fe_pow_2n_mul(t0, t2, 50, t0);       /* 2^250 - 1 */

    fe_copy(t3, t0);
    for (i = 0; i < 2; i++)
        fe_sqr(t3, t3);
    fe_mul(out, t3, z);                  /* (p-5)/8 */
}


/* ------------------------------------------------------------- X25519 --- */

/*
 * RFC 7748 section 5's ladder, unchanged in structure.  Nine full multiplies
 * (four of them squarings) and one multiply by a24 per bit, and one inversion
 * at the end.
 */
static void x25519_core(fe out, const unsigned char e[32], const fe x1)
{
    fe       x2, z2, x3, z3;
    fe       a, aa, b, bb, ee, c, d, da, cb, t;
    uint32_t swap = 0;
    int      pos;

    fe_copy(x2, fe_one);
    fe_copy(z2, fe_zero);
    fe_copy(x3, x1);
    fe_copy(z3, fe_one);

    for (pos = 254; pos >= 0; pos--) {
        uint32_t bit = (uint32_t)((e[pos >> 3] >> (pos & 7)) & 1);

        swap ^= bit;
        fe_cswap(x2, x3, swap);
        fe_cswap(z2, z3, swap);
        swap = bit;

        fe_add(a, x2, z2);
        fe_sqr(aa, a);
        fe_sub(b, x2, z2);
        fe_sqr(bb, b);
        fe_sub(ee, aa, bb);
        fe_add(c, x3, z3);
        fe_sub(d, x3, z3);
        fe_mul(da, d, a);
        fe_mul(cb, c, b);

        fe_add(t, da, cb);
        fe_sqr(x3, t);
        fe_sub(t, da, cb);
        fe_sqr(t, t);
        fe_mul(z3, x1, t);

        fe_mul(x2, aa, bb);
        fe_mul_small(t, ee, 121665u);   /* a24, RFC 7748 section 5 */
        fe_add(t, aa, t);
        fe_mul(z2, ee, t);
    }

    fe_cswap(x2, x3, swap);
    fe_cswap(z2, z3, swap);

    fe_invert(z2, z2);
    fe_mul(out, x2, z2);
}

static void x25519_clamp(unsigned char e[32], const unsigned char n[32])
{
    int i;

    for (i = 0; i < 32; i++)
        e[i] = n[i];
    e[0]  &= 248;
    e[31] &= 127;
    e[31] |= 64;
}

int c68k_x25519(unsigned char q[32], const unsigned char n[32],
                const unsigned char p[32])
{
    unsigned char e[32];
    fe            x1, r;
    int           i, nonzero = 0;

    x25519_clamp(e, n);
    fe_frombytes(x1, p);
    x25519_core(r, e, x1);
    fe_tobytes(q, r);

    for (i = 0; i < 32; i++)
        nonzero |= q[i];

    memset(e, 0, sizeof(e));
    return nonzero ? 0 : -1;
}

int c68k_x25519_base(unsigned char q[32], const unsigned char n[32])
{
    static const unsigned char basepoint[32] = { 9 };

    return c68k_x25519(q, n, basepoint);
}


/* ------------------------------------------------- Edwards group ops --- */

/* Extended coordinates (X : Y : Z : T) with T = XY/Z, for a = -1. */
typedef struct {
    fe X, Y, Z, T;
} ge;

static void ge_zero(ge *p)
{
    fe_copy(p->X, fe_zero);
    fe_copy(p->Y, fe_one);
    fe_copy(p->Z, fe_one);
    fe_copy(p->T, fe_zero);
}

static void ge_base(ge *p)
{
    fe_copy(p->X, ge_base_x);
    fe_copy(p->Y, ge_base_y);
    fe_copy(p->Z, fe_one);
    fe_copy(p->T, ge_base_t);
}

static void ge_copy(ge *r, const ge *p)
{
    fe_copy(r->X, p->X);
    fe_copy(r->Y, p->Y);
    fe_copy(r->Z, p->Z);
    fe_copy(r->T, p->T);
}

static void ge_cswap(ge *p, ge *q, uint32_t bit)
{
    fe_cswap(p->X, q->X, bit);
    fe_cswap(p->Y, q->Y, bit);
    fe_cswap(p->Z, q->Z, bit);
    fe_cswap(p->T, q->T, bit);
}

/* add-2008-hwcd-3 for a = -1: eight multiplies, one of them by 2d. */
static void ge_add(ge *r, const ge *p, const ge *q)
{
    fe a, b, c, d, e, f, g, h, t;

    fe_sub(a, p->Y, p->X);
    fe_sub(t, q->Y, q->X);
    fe_mul(a, a, t);

    fe_add(b, p->Y, p->X);
    fe_add(t, q->Y, q->X);
    fe_mul(b, b, t);

    fe_mul(c, p->T, q->T);
    fe_mul(c, c, fe_d2);

    fe_mul(d, p->Z, q->Z);
    fe_add(d, d, d);

    fe_sub(e, b, a);
    fe_sub(f, d, c);
    fe_add(g, d, c);
    fe_add(h, b, a);

    fe_mul(r->X, e, f);
    fe_mul(r->Y, g, h);
    fe_mul(r->T, e, h);
    fe_mul(r->Z, f, g);
}

/*
 * dbl-2008-hwcd for a = -1: four multiplies and four squarings, against the
 * eight-multiply generic addition TweetNaCl reuses for doubling.  On this part
 * a squaring is a little over half a multiply, so this is worth about a third
 * of the doubling.
 */
static void ge_dbl(ge *r, const ge *p)
{
    fe a, b, c, e, f, g, h, t;

    fe_sqr(a, p->X);
    fe_sqr(b, p->Y);
    fe_sqr(c, p->Z);
    fe_add(c, c, c);

    fe_add(t, p->X, p->Y);
    fe_sqr(e, t);
    fe_sub(e, e, a);
    fe_sub(e, e, b);                     /* E = 2*X*Y */

    fe_sub(g, b, a);                     /* G = D + B, D = -A */
    fe_sub(f, g, c);                     /* F = G - C */
    fe_add(h, a, b);
    fe_neg(h, h);                        /* H = D - B = -(A+B) */

    fe_mul(r->X, e, f);
    fe_mul(r->Y, g, h);
    fe_mul(r->T, e, h);
    fe_mul(r->Z, f, g);
}

static void ge_pack(unsigned char out[32], const ge *p)
{
    fe zi, x, y;

    fe_invert(zi, p->Z);
    fe_mul(x, p->X, zi);
    fe_mul(y, p->Y, zi);
    fe_tobytes(out, y);
    out[31] ^= (unsigned char)(fe_isodd(x) << 7);
}

/*
 * Constant-time double-and-add.  The scalar steers a conditional swap and
 * never a branch, as in TweetNaCl.
 */
static void ge_scalarmult(ge *r, const ge *q, const unsigned char s[32])
{
    ge  p, t;
    int i;

    ge_zero(&p);
    ge_copy(&t, q);

    for (i = 255; i >= 0; i--) {
        uint32_t bit = (uint32_t)((s[i >> 3] >> (i & 7)) & 1);

        ge_cswap(&p, &t, bit);
        ge_add(&t, &t, &p);
        ge_dbl(&p, &p);
        ge_cswap(&p, &t, bit);
    }
    ge_copy(r, &p);
}

static void ge_scalarmult_base(ge *r, const unsigned char s[32])
{
    ge b;

    ge_base(&b);
    ge_scalarmult(r, &b, s);
}

/*
 * Decompress a public key: recover x from y and the sign bit, on the curve
 * -x^2 + y^2 = 1 + d x^2 y^2.  Returns 0 on success.  The point is negated on
 * the way out, because verification computes h*(-A) + s*B and comparing that
 * with R is one group addition cheaper than the other arrangement.
 */
static int ge_unpack_neg(ge *r, const unsigned char p[32])
{
    fe u, v, v3, vxx, check, x;
    int i;

    fe_frombytes(r->Y, p);
    fe_copy(r->Z, fe_one);

    fe_sqr(u, r->Y);
    fe_mul(v, u, fe_d);
    fe_sub(u, u, r->Z);                  /* u = y^2 - 1 */
    fe_add(v, v, r->Z);                  /* v = d y^2 + 1 */

    fe_sqr(v3, v);
    fe_mul(v3, v3, v);                   /* v^3 */
    fe_sqr(x, v3);
    fe_mul(x, x, v);
    fe_mul(x, x, u);                     /* u v^7 */

    fe_pow22523(x, x);                   /* (u v^7)^((p-5)/8) */
    fe_mul(x, x, v3);
    fe_mul(x, x, u);                     /* u v^3 (u v^7)^((p-5)/8) */

    fe_sqr(vxx, x);
    fe_mul(vxx, vxx, v);
    fe_sub(check, vxx, u);
    if (fe_isnonzero(check)) {
        fe_add(check, vxx, u);
        if (fe_isnonzero(check))
            return -1;
        fe_mul(x, x, fe_sqrtm1);
    }

    if (fe_isodd(x) != (unsigned char)(p[31] >> 7))
        fe_neg(x, x);

    /* -A: negate the x coordinate. */
    fe_neg(r->X, x);
    fe_mul(r->T, r->X, r->Y);

    (void)i;
    return 0;
}


/* --------------------------------------------------- scalars mod L ----- */

/*
 * L = 2^252 + 27742317777372353535851937790883648493, the group order, as 32
 * little-endian bytes.  The reduction below is TweetNaCl's, and is nowhere
 * near the hot path: it runs three times in a handshake against twenty
 * thousand field multiplies.
 */
static const int64_t sc_L[32] = {
    0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58,
    0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9, 0xde, 0x14,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0x10
};

static void sc_modL(unsigned char r[32], int64_t x[64])
{
    int64_t carry;
    int     i, j;

    for (i = 63; i >= 32; i--) {
        carry = 0;
        for (j = i - 32; j < i - 12; j++) {
            x[j] += carry - 16 * x[i] * sc_L[j - (i - 32)];
            carry = (x[j] + 128) >> 8;
            x[j] -= carry << 8;
        }
        x[j] += carry;
        x[i] = 0;
    }
    carry = 0;
    for (j = 0; j < 32; j++) {
        x[j] += carry - (x[31] >> 4) * sc_L[j];
        carry = x[j] >> 8;
        x[j] &= 255;
    }
    for (j = 0; j < 32; j++)
        x[j] -= carry * sc_L[j];
    for (i = 0; i < 32; i++) {
        x[i + 1] += x[i] >> 8;
        r[i] = (unsigned char)(x[i] & 255);
    }
}

static void sc_reduce(unsigned char r[64])
{
    int64_t x[64];
    int     i;

    for (i = 0; i < 64; i++)
        x[i] = (int64_t)(unsigned char)r[i];
    for (i = 0; i < 64; i++)
        r[i] = 0;
    sc_modL(r, x);
}

/* s = (a*b + c) mod L, every argument 32 bytes little-endian. */
static void sc_muladd(unsigned char s[32], const unsigned char a[32],
                      const unsigned char b[32], const unsigned char c[32])
{
    int64_t x[64];
    int     i, j;

    for (i = 0; i < 64; i++)
        x[i] = 0;
    for (i = 0; i < 32; i++)
        x[i] = (int64_t)(unsigned char)c[i];
    for (i = 0; i < 32; i++)
        for (j = 0; j < 32; j++)
            x[i + j] += (int64_t)(unsigned char)a[i] *
                        (int64_t)(unsigned char)b[j];
    sc_modL(s, x);
}


/* -------------------------------------------------------- Ed25519 ------ */

static void ed25519_expand(c68k_sha512_fn sha512, unsigned char d[64],
                           const unsigned char sk[32])
{
    sha512(d, sk, 32, (const unsigned char *)0, 0, (const unsigned char *)0, 0);
    d[0]  &= 248;
    d[31] &= 127;
    d[31] |= 64;
}

void c68k_ed25519_pubkey(c68k_sha512_fn sha512, unsigned char pk[32],
                         const unsigned char sk[32])
{
    unsigned char d[64];
    ge            a;

    ed25519_expand(sha512, d, sk);
    ge_scalarmult_base(&a, d);
    ge_pack(pk, &a);
    memset(d, 0, sizeof(d));
}

void c68k_ed25519_sign(c68k_sha512_fn sha512, unsigned char sig[64],
                       const unsigned char *m, unsigned long mlen,
                       const unsigned char sk[32], const unsigned char pk[32])
{
    unsigned char d[64], r[64], h[64];
    ge            p;

    ed25519_expand(sha512, d, sk);

    sha512(r, d + 32, 32, m, mlen, (const unsigned char *)0, 0);
    sc_reduce(r);

    ge_scalarmult_base(&p, r);
    ge_pack(sig, &p);

    sha512(h, sig, 32, pk, 32, m, mlen);
    sc_reduce(h);

    sc_muladd(sig + 32, h, d, r);

    memset(d, 0, sizeof(d));
    memset(r, 0, sizeof(r));
}

/*
 * Verification.  Two variable-base scalar multiplications and one addition:
 *   [s]B + [h](-A) must equal R.
 * The two multiplications are done separately rather than with a joint
 * (Straus) loop; a joint one saves about a quarter, and is a change to make
 * against a measurement of this.
 */
int c68k_ed25519_verify(c68k_sha512_fn sha512,
                        const unsigned char *m, unsigned long mlen,
                        const unsigned char sig[64],
                        const unsigned char pk[32])
{
    unsigned char h[64], check[32];
    ge            a, p, q;
    int           i;

    /* RFC 8032 section 5.1.7: S must be canonical, below L.  Compared from
       the top down, written out because the little-endian encoding makes that
       awkward. */
    for (i = 31; i >= 0; i--) {
        if ((int64_t)(unsigned char)sig[32 + i] < sc_L[i])
            break;
        if ((int64_t)(unsigned char)sig[32 + i] > sc_L[i])
            return -1;
        if (i == 0)
            return -1;                   /* S == L exactly */
    }

    if (ge_unpack_neg(&a, pk) != 0)
        return -1;

    sha512(h, sig, 32, pk, 32, m, mlen);
    sc_reduce(h);

    ge_scalarmult(&p, &a, h);            /* [h](-A) */
    ge_scalarmult_base(&q, sig + 32);    /* [s]B    */
    ge_add(&p, &p, &q);
    ge_pack(check, &p);

    for (i = 0; i < 32; i++)
        if (check[i] != sig[i])
            return -1;
    return 0;
}


/* ---------------------------------------------------------- self-check - */

/*
 * Exposed only for tests/crypto68k: fe_sqr(r,a) must equal fe_mul(r,a,a) for
 * every input, and published vectors cannot prove that because they exercise
 * the same handful of values.  Kept out of the header so nothing in the stack
 * can reach it.
 */
int c68k_25519_selfcheck_sqr(const unsigned char in[32]);
int c68k_25519_selfcheck_sqr(const unsigned char in[32])
{
    fe            a, r1, r2;
    unsigned char b1[32], b2[32];

    fe_frombytes(a, in);
    fe_sqr(r1, a);
    fe_mul(r2, a, a);
    fe_tobytes(b1, r1);
    fe_tobytes(b2, r2);
    return memcmp(b1, b2, 32) == 0 ? 0 : -1;
}

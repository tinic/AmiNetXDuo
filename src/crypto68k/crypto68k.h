/*
 * AmiNetXDuo, crypto68k: 68020 multi-precision arithmetic for nx_crypto.
 *
 * A drop-in replacement for the exponentiation half of the vendored
 * nx_crypto_huge_number.c, which is left untouched as the test oracle.
 * NOT constant time -- see the note above c68k_mont_power_modulus().
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_CRYPTO68K_H
#define AMINETXDUO_CRYPTO68K_H

#include "nx_crypto_huge_number.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Limb type.  The assembly is written for EXACTLY this: 32-bit limbs,
 * big-endian host, little-endian limb order (limb 0 least significant).
 */
typedef HN_UBASE    c68k_limb;

#if (NX_CRYPTO_HUGE_NUMBER_BITS != 32)
#error "crypto68k requires NX_CRYPTO_HUGE_NUMBER_BITS == 32"
#endif


/* ------------------------------------------------------------ primitives, */

/*
 * r[0..n-1] += a * b[0..n-1], carry propagating.  Returns the carry out of
 * the top limb (a FULL LIMB, not a single bit).  The 64-bit intermediate
 * cannot overflow: (2^32-1)^2 + 2*(2^32-1) = 2^64-1 exactly.
 */
c68k_limb c68k_addmul_1(c68k_limb *r, const c68k_limb *b, UINT n, c68k_limb a);

/* The portable C version, always present under its own name whichever build
   option is in force, so the benchmark can time both in one run. */
c68k_limb c68k_addmul_1_c(c68k_limb *r, const c68k_limb *b, UINT n, c68k_limb a);

/*
 * dst[j] = src[j] + carry, for j in 0..n-1.  Returns the final carry (0 or 1
 * after the first limb).  dst can alias src.
 */
c68k_limb c68k_add_carry(c68k_limb *dst, const c68k_limb *src, UINT n,
                         c68k_limb carry);

/* r[0..n-1] += b[0..n-1].  Returns the carry out (0 or 1). */
c68k_limb c68k_add(c68k_limb *r, const c68k_limb *b, UINT n);

/*
 * r[0..n-1] -= b[0..n-1].  Returns the borrow out (0 or 1).
 */
c68k_limb c68k_sub(c68k_limb *r, const c68k_limb *b, UINT n);

/* r[0..n-1] -= a * b[0..n-1].  Returns the borrow out (a full limb). */
c68k_limb c68k_submul_1(c68k_limb *r, const c68k_limb *b, UINT n, c68k_limb a);

/*
 * (hi:lo) / d, returning the quotient and storing the remainder.
 * THE CALLER MUST CHECK hi < d: the DIVU.L form traps otherwise.
 * DIVU.L 64/32 is unimplemented on a 68060, so AMINETXDUO_CRYPTO68K_ASM must
 * never be enabled for a 68060 build.
 */
c68k_limb c68k_div_2by1(c68k_limb hi, c68k_limb lo, c68k_limb d,
                        c68k_limb *rem);

/*
 * rem = u mod m by Knuth's algorithm D over 32-bit limbs.  m[m_len-1] must be
 * non-zero and u_len >= m_len.  scratch needs
 * C68K_MOD_SCRATCH_LIMBS(u_len, m_len) and must not alias.
 */
#define C68K_MOD_SCRATCH_LIMBS(u_len, m_len) \
    ((UINT)(u_len) + (UINT)(m_len) + 2u)

VOID c68k_mod(c68k_limb *rem, const c68k_limb *u, UINT u_len,
              const c68k_limb *m, UINT m_len, c68k_limb *scratch);

/* Whether c68k_setup_rr() reduces with c68k_mod() (default) or the vendored
   16-bit divider.  A variable so both can be timed in one process. */
extern UINT c68k_fast_modulus;

/*
 * rr = R^2 mod m, where R = 2^(32*m_len).  `setup` needs 4*m_len + 3 limbs,
 * which also covers the vendored-divider path's 3*m_len + 3.
 */
VOID c68k_mont_setup_rr(c68k_limb *rr, const c68k_limb *m, UINT m_len,
                        c68k_limb *setup);

/* Unsigned compare of two n-limb values.  -1, 0 or 1. */
INT c68k_cmp(const c68k_limb *a, const c68k_limb *b, UINT n);

/*
 * Which limb primitives were compiled in: 0 portable C, 1 c68k_prim.S (68020),
 * 2 c68k_prim_mulw.S (68060 multiply-accumulate, rest still C).
 */
#define C68K_ASM_NONE   0u
#define C68K_ASM_68020  1u
#define C68K_ASM_68060  2u

UINT c68k_using_assembly(VOID);


/* ------------------------------------------------------ the CPU, at run time */
/*
 * Call before the first handshake.  Without the call the portable C stays in
 * place: slower, never wrong, and never an illegal instruction.
 */
VOID c68k_cpu_select(ULONG attnflags);

/* What was selected, as the C68K_ASM_* values above. */
UINT c68k_cpu_class(VOID);

/* The two primitives that cannot be one routine across CPUs; the rest of
   c68k_prim.S is 68000 code called by name everywhere. */
#ifdef C68K_MV

extern c68k_limb (*c68k_vec_addmul_1)(c68k_limb *, const c68k_limb *, UINT,
                                      c68k_limb);
extern c68k_limb (*c68k_vec_div_2by1)(c68k_limb, c68k_limb, c68k_limb,
                                      c68k_limb *);

#define C68K_ADDMUL_1   (*c68k_vec_addmul_1)
#define C68K_DIV_2BY1   (*c68k_vec_div_2by1)

#else

#define C68K_ADDMUL_1   c68k_addmul_1
#define C68K_DIV_2BY1   c68k_div_2by1

#endif


/* ------------------------------------------------------------ Montgomery, */

/* -m[0]^-1 mod 2^32, by Newton iteration.  m MUST be odd. */
c68k_limb c68k_mont_n0inv(c68k_limb m0);

/*
 * Operand width, in limbs, at or above which the Montgomery product is split
 * with Karatsuba.  Default C68K_KARATSUBA_DEFAULT; 0 or 1 disables the split.
 */
extern UINT c68k_karatsuba_limbs;

#define C68K_KARATSUBA_DEFAULT  64u

/*
 * r = x * y * R^-1 mod m, where R = 2^(32*m_len).  y MUST be < m.  r may alias
 * x or y (both are consumed before r is written).  `work` is
 * C68K_MONT_WORK_LIMBS(m_len) limbs and must not alias anything.
 */

/*
 * Scratch, in limbs, for c68k_mont_mul()/c68k_mont_sqr().  Every caller must
 * use this macro: a value too small makes the Karatsuba recombination write
 * past the end.
 */
#define C68K_MONT_WORK_LIMBS(m_len)     ((8u * (UINT)(m_len)) + 2u)

VOID c68k_mont_mul(c68k_limb *r,
                   const c68k_limb *x, const c68k_limb *y,
                   const c68k_limb *m, UINT m_len, c68k_limb n0inv,
                   c68k_limb *work);

/* r = x * x * R^-1 mod m.  Same interface as c68k_mont_mul. */
VOID c68k_mont_sqr(c68k_limb *r,
                   const c68k_limb *x,
                   const c68k_limb *m, UINT m_len, c68k_limb n0inv,
                   c68k_limb *work);


/* -------------------------------------------------------- exponentiation, */

/*
 * Scratch, in limbs, that c68k_mont_power_modulus() needs for a modulus of
 * m_len limbs and a window of w bits.  A window of w keeps 2^(w-1) odd powers.
 *
 *   x_mont, acc, one, rr, xpad          =  5 * m_len
 *   SOS work area + Karatsuba scratch   =  8 * m_len + 2
 *   radix^2 mod m setup + c68k_mod       =  7 * m_len + 8
 *   window table, 2^(w-1) odd powers    = (1 << (w-1)) * m_len
 */
#define C68K_POWM_SCRATCH_LIMBS(m_len, w) \
    ((((20u + (1u << ((w) - 1u))) * (UINT)(m_len))) + 16u)

/* Cap only: c68k_mont_power_modulus() picks the largest window that fits the
   scratch it is given. */
#define C68K_POWM_MAX_WINDOW    6u

/*
 * result = x^e mod m, with m ODD and x < m.  Sizes in limbs; the window is
 * chosen to fit scratch_limbs, down to w = 1.  Returns NX_CRYPTO_SUCCESS or
 * NX_CRYPTO_SIZE_ERROR.
 *
 * NOT CONSTANT TIME: table addresses, the conditional subtraction and the
 * exponent bit length all leak.  Unacceptable for a server exposed to remote
 * timing.
 */
UINT c68k_mont_power_modulus(c68k_limb *result,
                             const c68k_limb *x, UINT x_len,
                             const c68k_limb *e, UINT e_len,
                             const c68k_limb *m, UINT m_len,
                             c68k_limb *scratch, UINT scratch_limbs);

/*
 * Signature-compatible drop-in for _nx_crypto_huge_number_mont_power_modulus.
 * Its scratch requirement is LARGER than the vendored one; a caller buffer
 * that is too small falls back to a smaller window rather than failing.
 */
VOID c68k_huge_number_mont_power_modulus(NX_CRYPTO_HUGE_NUMBER *x,
                                         NX_CRYPTO_HUGE_NUMBER *e,
                                         NX_CRYPTO_HUGE_NUMBER *m,
                                         NX_CRYPTO_HUGE_NUMBER *result,
                                         HN_UBASE *scratch,
                                         UINT scratch_limbs);

/*
 * RSA private operation with the Chinese Remainder Theorem: the vendored
 * _nx_crypto_huge_number_crt_power_modulus() with the two exponentiations
 * replaced.  `scratch` is the huge-number bump area the vendored routine
 * wants.  powm_scratch belongs to this module, kept separate so the two
 * allocators cannot overlap.
 *
 * CRT is worth ~3.6x on its own, measured, and multiplies with everything
 * else here.  See the header comment in c68k_crt.c for where nx_secure does
 * not currently take it.
 */
VOID c68k_crt_power_modulus(NX_CRYPTO_HUGE_NUMBER *x,
                            NX_CRYPTO_HUGE_NUMBER *e,
                            NX_CRYPTO_HUGE_NUMBER *p,
                            NX_CRYPTO_HUGE_NUMBER *q,
                            NX_CRYPTO_HUGE_NUMBER *m,
                            NX_CRYPTO_HUGE_NUMBER *result,
                            HN_UBASE *scratch,
                            HN_UBASE *powm_scratch, UINT powm_scratch_limbs);

/*
 * The AmigaOS ThreadX port does not preempt a thread that makes no ThreadX
 * call, so a multi-second public-key operation stops the whole stack.  Set
 * this to a routine that gives the machine back.  NULL is the default.
 */
extern VOID (*c68k_yield_hook)(VOID);

#define C68K_YIELD()                                            \
    do {                                                        \
        if (c68k_yield_hook != (VOID (*)(VOID))0)               \
        {                                                       \
            c68k_yield_hook();                                  \
        }                                                       \
    } while (0)

/*
 * The yield stride is sized in LIMB PRODUCTS, not loop iterations, so the
 * interval is the same whatever the modulus width or loop body.  At this
 * setting an RSA-2048 public operation yields 469 times, ~50 ms apart.
 */
#define C68K_YIELD_PRODUCTS     256u

/*
 * Iterations of a loop whose body is `products` limb products that fit in one
 * yield interval.  Never zero; with nothing hooked it returns a count no loop
 * here can reach.
 */
UINT c68k_yield_stride(UINT products);

#ifdef __cplusplus
}
#endif

#endif /* AMINETXDUO_CRYPTO68K_H */

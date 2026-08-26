/*
 * AmiNetXDuo, crypto68k RSA CRT exponentiation.
 *
 * SPDX-License-Identifier: MIT
 */

#include "crypto68k.h"


VOID c68k_crt_power_modulus(NX_CRYPTO_HUGE_NUMBER *x,
                            NX_CRYPTO_HUGE_NUMBER *e,
                            NX_CRYPTO_HUGE_NUMBER *p,
                            NX_CRYPTO_HUGE_NUMBER *q,
                            NX_CRYPTO_HUGE_NUMBER *m,
                            NX_CRYPTO_HUGE_NUMBER *result,
                            HN_UBASE *scratch,
                            HN_UBASE *powm_scratch, UINT powm_scratch_limbs)
{

NX_CRYPTO_HUGE_NUMBER  *ep;
NX_CRYPTO_HUGE_NUMBER  *eq;
NX_CRYPTO_HUGE_NUMBER  *xp;
NX_CRYPTO_HUGE_NUMBER  *xq;
NX_CRYPTO_HUGE_NUMBER  *m1;
NX_CRYPTO_HUGE_NUMBER  *m2;
NX_CRYPTO_HUGE_NUMBER   pi, qi;
NX_CRYPTO_HUGE_NUMBER   temp1, temp2, temp3;
NX_CRYPTO_HUGE_NUMBER   digit;
HN_UBASE                digit_value;


    /*
     *   ep = e mod (p-1)   xp = x mod p   m1 = xp^ep mod p
     *   eq = e mod (q-1)   xq = x mod q   m2 = xq^eq mod q
     *   pi = p^-1 mod q    qi = q^-1 mod p
     *   r  = (m1*qi*q + m2*pi*p) mod m
     *
     * The buffer carving below is the vendored one, unchanged, and load
     * bearing.  NX_CRYPTO_HUGE_NUMBER_INITIALIZE is a bump allocator that
     * advances `scratch`, and several of these buffers are reused under a
     * second name once their first value is dead.
     */
    NX_CRYPTO_HUGE_NUMBER_INITIALIZE(&pi, scratch, p -> nx_crypto_huge_buffer_size);
    NX_CRYPTO_HUGE_NUMBER_INITIALIZE(&qi, scratch, q -> nx_crypto_huge_buffer_size);

    _nx_crypto_huge_number_inverse_modulus_prime(q, p, &qi, scratch);
    _nx_crypto_huge_number_inverse_modulus_prime(p, q, &pi, scratch);

    NX_CRYPTO_HUGE_NUMBER_INITIALIZE(&temp1, scratch, m -> nx_crypto_huge_buffer_size);
    NX_CRYPTO_HUGE_NUMBER_INITIALIZE(&temp2, scratch, p -> nx_crypto_huge_buffer_size);
    NX_CRYPTO_HUGE_NUMBER_INITIALIZE(&temp3, scratch, p -> nx_crypto_huge_buffer_size);
    NX_CRYPTO_HUGE_NUMBER_INITIALIZE_DIGIT(&digit, &digit_value, 1);

    /* ep = e mod (p - 1) */
    ep = &temp2;
    NX_CRYPTO_HUGE_NUMBER_COPY(ep, e);
    NX_CRYPTO_HUGE_NUMBER_COPY(&temp1, p);
    _nx_crypto_huge_number_subtract(&temp1, &digit);
    _nx_crypto_huge_number_modulus(ep, &temp1);

    /* xp = x mod p */
    xp = &temp3;
    NX_CRYPTO_HUGE_NUMBER_COPY(xp, x);
    _nx_crypto_huge_number_modulus(xp, p);

    /* m1 = xp ^ ep mod p, the only line that differs from the vendored code. */
    m1 = &temp1;
    c68k_huge_number_mont_power_modulus(xp, ep, p, m1,
                                        powm_scratch, powm_scratch_limbs);

    /* m1 * qi * q */
    _nx_crypto_huge_number_multiply(&qi, m1, &temp3);
    _nx_crypto_huge_number_multiply(&temp3, q, result);

    /* eq = e mod (q - 1) */
    eq = &temp2;
    NX_CRYPTO_HUGE_NUMBER_COPY(eq, e);
    NX_CRYPTO_HUGE_NUMBER_COPY(&temp1, q);
    _nx_crypto_huge_number_subtract(&temp1, &digit);
    _nx_crypto_huge_number_modulus(eq, &temp1);

    /* xq = x mod q */
    xq = &temp3;
    NX_CRYPTO_HUGE_NUMBER_COPY(xq, x);
    _nx_crypto_huge_number_modulus(xq, q);

    /* m2 = xq ^ eq mod q, the other differing line. */
    m2 = &temp1;
    c68k_huge_number_mont_power_modulus(xq, eq, q, m2,
                                        powm_scratch, powm_scratch_limbs);

    /* pi * p * m2 */
    _nx_crypto_huge_number_multiply(&pi, m2, &temp3);
    _nx_crypto_huge_number_multiply(p, &temp3, &temp1);

    /* r = (m1*qi*q + m2*pi*p) mod m */
    _nx_crypto_huge_number_add(result, &temp1);
    _nx_crypto_huge_number_modulus(result, m);
}

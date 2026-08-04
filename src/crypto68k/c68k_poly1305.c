/*
 * AmiNetXDuo, crypto68k: Poly1305 (RFC 8439 section 2.5).
 *
 * See c68k_poly1305.h for why the limbs are 2^26 wide.
 *
 * Written here from RFC 8439's specification.  The five-limb radix-2^26
 * decomposition is the standard one for a 32-bit machine, what Bernstein's
 * original description does and what poly1305-donna (Andrew Moon, public
 * domain) made the common spelling, and the clamp constants below are the
 * RFC's, transposed into those limbs.  No code was copied from either.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>

#include "c68k_poly1305.h"


/* A little-endian longword at an arbitrary address.
 *
 * The 68020 does misaligned data accesses in hardware, so this is one MOVE.L
 * and a three-instruction byte reversal; the portable form GCC compiles the
 * shifts-and-ORs into is four byte reads and six ALU operations.  Same
 * reasoning and the same __mc68020__ guard as c68k_sha256_load_be(), with a
 * reversal because Poly1305 reads its input little-endian and this is a
 * big-endian machine. */
static ULONG c68k_poly1305_load_le(const UCHAR *p)
{

#ifdef __mc68020__
ULONG   v;

    __asm__ ("move.l %1,%0" : "=d" (v) : "m" (*p));
    __asm__ ("rol.w #8,%0\n\tswap %0\n\trol.w #8,%0" : "+d" (v));

    return(v);
#else
    return(((ULONG)p[0]) | (((ULONG)p[1]) << 8) |
           (((ULONG)p[2]) << 16) | (((ULONG)p[3]) << 24));
#endif
}

static VOID c68k_poly1305_store_le(UCHAR *p, ULONG v)
{

    p[0] = (UCHAR)(v);
    p[1] = (UCHAR)(v >> 8);
    p[2] = (UCHAR)(v >> 16);
    p[3] = (UCHAR)(v >> 24);
}


VOID c68k_poly1305_initialize(C68K_POLY1305 *ctx, const UCHAR *key)
{

ULONG   t0, t1, t2, t3;


    t0 = c68k_poly1305_load_le(&key[0]);
    t1 = c68k_poly1305_load_le(&key[4]);
    t2 = c68k_poly1305_load_le(&key[8]);
    t3 = c68k_poly1305_load_le(&key[12]);

    /*
     * r &= 0x0ffffffc0ffffffc0ffffffc0fffffff, RFC 8439's clamp, folded
     * into the limb masks so that the AND and the split are one operation.
     */
    ctx -> c68k_poly1305_r[0] = (t0) & 0x03FFFFFFuL;
    ctx -> c68k_poly1305_r[1] = ((t0 >> 26) | (t1 << 6)) & 0x03FFFF03uL;
    ctx -> c68k_poly1305_r[2] = ((t1 >> 20) | (t2 << 12)) & 0x03FFC0FFuL;
    ctx -> c68k_poly1305_r[3] = ((t2 >> 14) | (t3 << 18)) & 0x03F03FFFuL;
    ctx -> c68k_poly1305_r[4] = ((t3 >> 8)) & 0x000FFFFFuL;

    ctx -> c68k_poly1305_h[0] = 0uL;
    ctx -> c68k_poly1305_h[1] = 0uL;
    ctx -> c68k_poly1305_h[2] = 0uL;
    ctx -> c68k_poly1305_h[3] = 0uL;
    ctx -> c68k_poly1305_h[4] = 0uL;

    ctx -> c68k_poly1305_pad[0] = c68k_poly1305_load_le(&key[16]);
    ctx -> c68k_poly1305_pad[1] = c68k_poly1305_load_le(&key[20]);
    ctx -> c68k_poly1305_pad[2] = c68k_poly1305_load_le(&key[24]);
    ctx -> c68k_poly1305_pad[3] = c68k_poly1305_load_le(&key[28]);

    ctx -> c68k_poly1305_leftover = 0u;
}


/*
 * h = (h + m) * r mod 2^130 - 5, over `blocks` whole 16-byte blocks.
 *
 * `hibit` is the 2^128 term the padding rule adds to every full block and
 * withholds from a short final one, passed in rather than tested for, so the
 * loop has no branch in it.
 *
 * The twenty-five products are the whole cost.  Each is a MULU.L with a 64-bit
 * destination, real on a 68020, and the reason src/crypto68k exists at all.
 * The s1..s4 values are r1..r4 pre-multiplied by five: the reduction of the
 * terms that fall off the top of the 130-bit accumulator, folded into the
 * multiplier instead of into a separate pass.
 */
VOID c68k_poly1305_blocks_c(C68K_POLY1305 *ctx, const UCHAR *m,
                            ULONG blocks, ULONG hibit)
{

ULONG   r0, r1, r2, r3, r4;
ULONG   s1, s2, s3, s4;
ULONG   h0, h1, h2, h3, h4;
ULONG   t0, t1, t2, t3;
ULONG   c;
ULONG64 d0, d1, d2, d3, d4;


    r0 = ctx -> c68k_poly1305_r[0];
    r1 = ctx -> c68k_poly1305_r[1];
    r2 = ctx -> c68k_poly1305_r[2];
    r3 = ctx -> c68k_poly1305_r[3];
    r4 = ctx -> c68k_poly1305_r[4];

    s1 = r1 * 5uL;
    s2 = r2 * 5uL;
    s3 = r3 * 5uL;
    s4 = r4 * 5uL;

    h0 = ctx -> c68k_poly1305_h[0];
    h1 = ctx -> c68k_poly1305_h[1];
    h2 = ctx -> c68k_poly1305_h[2];
    h3 = ctx -> c68k_poly1305_h[3];
    h4 = ctx -> c68k_poly1305_h[4];

    while (blocks != 0uL)
    {

        /* h += m */
        t0 = c68k_poly1305_load_le(&m[0]);
        t1 = c68k_poly1305_load_le(&m[4]);
        t2 = c68k_poly1305_load_le(&m[8]);
        t3 = c68k_poly1305_load_le(&m[12]);

        h0 += (t0) & 0x03FFFFFFuL;
        h1 += ((t0 >> 26) | (t1 << 6)) & 0x03FFFFFFuL;
        h2 += ((t1 >> 20) | (t2 << 12)) & 0x03FFFFFFuL;
        h3 += ((t2 >> 14) | (t3 << 18)) & 0x03FFFFFFuL;
        h4 += (t3 >> 8) | hibit;

        /* h *= r */
        d0 = ((ULONG64)h0 * r0) + ((ULONG64)h1 * s4) + ((ULONG64)h2 * s3) +
             ((ULONG64)h3 * s2) + ((ULONG64)h4 * s1);
        d1 = ((ULONG64)h0 * r1) + ((ULONG64)h1 * r0) + ((ULONG64)h2 * s4) +
             ((ULONG64)h3 * s3) + ((ULONG64)h4 * s2);
        d2 = ((ULONG64)h0 * r2) + ((ULONG64)h1 * r1) + ((ULONG64)h2 * r0) +
             ((ULONG64)h3 * s4) + ((ULONG64)h4 * s3);
        d3 = ((ULONG64)h0 * r3) + ((ULONG64)h1 * r2) + ((ULONG64)h2 * r1) +
             ((ULONG64)h3 * r0) + ((ULONG64)h4 * s4);
        d4 = ((ULONG64)h0 * r4) + ((ULONG64)h1 * r3) + ((ULONG64)h2 * r2) +
             ((ULONG64)h3 * r1) + ((ULONG64)h4 * r0);

        /* h %= 2^130 - 5, partially, enough that the next block cannot
           overflow, with the last carry folded round by the factor of five. */
        c  = (ULONG)(d0 >> 26); h0 = (ULONG)d0 & 0x03FFFFFFuL;
        d1 += c; c = (ULONG)(d1 >> 26); h1 = (ULONG)d1 & 0x03FFFFFFuL;
        d2 += c; c = (ULONG)(d2 >> 26); h2 = (ULONG)d2 & 0x03FFFFFFuL;
        d3 += c; c = (ULONG)(d3 >> 26); h3 = (ULONG)d3 & 0x03FFFFFFuL;
        d4 += c; c = (ULONG)(d4 >> 26); h4 = (ULONG)d4 & 0x03FFFFFFuL;
        h0 += c * 5uL; c = (h0 >> 26); h0 &= 0x03FFFFFFuL;
        h1 += c;

        m += C68K_POLY1305_BLOCK_SIZE;
        blocks--;
    }

    ctx -> c68k_poly1305_h[0] = h0;
    ctx -> c68k_poly1305_h[1] = h1;
    ctx -> c68k_poly1305_h[2] = h2;
    ctx -> c68k_poly1305_h[3] = h3;
    ctx -> c68k_poly1305_h[4] = h4;
}


/*
 * Which one the rest of this file calls.  A macro rather than a wrapper, as in
 * c68k_chacha20.c: a build without the assembly gets no extra call, and there
 * is nothing to choose between, the assembly is the same function, faster,
 * on every part that can run it.  AMINETXDUO_CRYPTO68K_ASM=OFF, the 68000 and
 * the 68060 take the C.
 */
#ifdef C68K_ASM
extern VOID c68k_poly1305_blocks_asm(C68K_POLY1305 *ctx, const UCHAR *m,
                                     ULONG blocks, ULONG hibit);
#define C68K_POLY1305_BLOCKS    c68k_poly1305_blocks_asm

/*
 * c68k_poly1305.S reaches into C68K_POLY1305 with two hardcoded offsets, and
 * a struct that moved under it would give a wrong tag rather than a crash, so
 * make it a build failure.  (An array of negative size; this tree is built as
 * C89 and _Static_assert is not available.)
 */
typedef char c68k_poly1305_layout_check[
    (offsetof(C68K_POLY1305, c68k_poly1305_r) == 0u &&
     offsetof(C68K_POLY1305, c68k_poly1305_h) == 20u) ? 1 : -1];
#else
#define C68K_POLY1305_BLOCKS    c68k_poly1305_blocks_c
#endif

/*
 * The dispatch, as a real function, so tests/crypto68k/crypto68k_bulk can call
 * the shipped kernel and the portable one over the same input and compare; it
 * is not compiled with C68K_ASM, so it cannot make the choice itself.  Nothing
 * on the hot path goes through here: update() and finish() below call the
 * macro.
 */
VOID c68k_poly1305_blocks(C68K_POLY1305 *ctx, const UCHAR *m, ULONG blocks,
                          ULONG hibit)
{

    C68K_POLY1305_BLOCKS(ctx, m, blocks, hibit);
}

UINT c68k_poly1305_blocks_is_asm(VOID)
{

#ifdef C68K_ASM
    return((UINT)NX_CRYPTO_TRUE);
#else
    return((UINT)NX_CRYPTO_FALSE);
#endif
}


VOID c68k_poly1305_update(C68K_POLY1305 *ctx, const UCHAR *input,
                          ULONG input_length)
{

ULONG   want;
ULONG   blocks;
UINT    i;


    if (ctx -> c68k_poly1305_leftover != 0u)
    {
        want = (ULONG)(C68K_POLY1305_BLOCK_SIZE -
                       ctx -> c68k_poly1305_leftover);
        if (want > input_length)
        {
            want = input_length;
        }

        for (i = 0u; i < (UINT)want; i++)
        {
            ctx -> c68k_poly1305_buffer[ctx -> c68k_poly1305_leftover + i] =
                input[i];
        }

        ctx -> c68k_poly1305_leftover += (UINT)want;
        input += want;
        input_length -= want;

        if (ctx -> c68k_poly1305_leftover < C68K_POLY1305_BLOCK_SIZE)
        {
            return;
        }

        C68K_POLY1305_BLOCKS(ctx, ctx -> c68k_poly1305_buffer, 1uL,
                             (ULONG)1uL << 24);
        ctx -> c68k_poly1305_leftover = 0u;
    }

    blocks = input_length / C68K_POLY1305_BLOCK_SIZE;
    if (blocks != 0uL)
    {
        C68K_POLY1305_BLOCKS(ctx, input, blocks, (ULONG)1uL << 24);
        input += blocks * C68K_POLY1305_BLOCK_SIZE;
        input_length -= blocks * C68K_POLY1305_BLOCK_SIZE;
    }

    for (i = 0u; i < (UINT)input_length; i++)
    {
        ctx -> c68k_poly1305_buffer[i] = input[i];
    }
    ctx -> c68k_poly1305_leftover = (UINT)input_length;
}


VOID c68k_poly1305_finish(C68K_POLY1305 *ctx, UCHAR *tag)
{

ULONG   h0, h1, h2, h3, h4;
ULONG   g0, g1, g2, g3, g4;
ULONG   c, mask;
ULONG64 f;
UINT    i;


    if (ctx -> c68k_poly1305_leftover != 0u)
    {

        /* The short final block carries its own 1 byte rather than the
           implicit 2^128, so it goes through with hibit zero. */
        i = ctx -> c68k_poly1305_leftover;
        ctx -> c68k_poly1305_buffer[i] = 1u;
        for (i = i + 1u; i < C68K_POLY1305_BLOCK_SIZE; i++)
        {
            ctx -> c68k_poly1305_buffer[i] = 0u;
        }

        C68K_POLY1305_BLOCKS(ctx, ctx -> c68k_poly1305_buffer, 1uL, 0uL);
        ctx -> c68k_poly1305_leftover = 0u;
    }

    h0 = ctx -> c68k_poly1305_h[0];
    h1 = ctx -> c68k_poly1305_h[1];
    h2 = ctx -> c68k_poly1305_h[2];
    h3 = ctx -> c68k_poly1305_h[3];
    h4 = ctx -> c68k_poly1305_h[4];

    /* Carry fully, which the block loop does not. */
    c = h1 >> 26; h1 &= 0x03FFFFFFuL;
    h2 += c; c = h2 >> 26; h2 &= 0x03FFFFFFuL;
    h3 += c; c = h3 >> 26; h3 &= 0x03FFFFFFuL;
    h4 += c; c = h4 >> 26; h4 &= 0x03FFFFFFuL;
    h0 += c * 5uL; c = h0 >> 26; h0 &= 0x03FFFFFFuL;
    h1 += c;

    /* g = h + 5.  If that does not borrow at the top, h was >= 2^130 - 5 and
       g is the reduced value; if it does, h already was. */
    g0 = h0 + 5uL; c = g0 >> 26; g0 &= 0x03FFFFFFuL;
    g1 = h1 + c; c = g1 >> 26; g1 &= 0x03FFFFFFuL;
    g2 = h2 + c; c = g2 >> 26; g2 &= 0x03FFFFFFuL;
    g3 = h3 + c; c = g3 >> 26; g3 &= 0x03FFFFFFuL;
    g4 = h4 + c - ((ULONG)1uL << 26);

    /* Branch-free select: the borrow is g4's sign bit, and ULONG is 32 bits
       here, as the header requires. */
    mask = (g4 >> 31) - 1uL;
    g0 &= mask;
    g1 &= mask;
    g2 &= mask;
    g3 &= mask;
    g4 &= mask;
    mask = ~mask;
    h0 = (h0 & mask) | g0;
    h1 = (h1 & mask) | g1;
    h2 = (h2 & mask) | g2;
    h3 = (h3 & mask) | g3;
    h4 = (h4 & mask) | g4;

    /* Back to four 32-bit words: h mod 2^128. */
    h0 = (h0) | (h1 << 26);
    h1 = (h1 >> 6) | (h2 << 20);
    h2 = (h2 >> 12) | (h3 << 14);
    h3 = (h3 >> 18) | (h4 << 8);

    /* tag = (h + s) mod 2^128 */
    f = (ULONG64)h0 + ctx -> c68k_poly1305_pad[0];
    h0 = (ULONG)f;
    f = (ULONG64)h1 + ctx -> c68k_poly1305_pad[1] + (f >> 32);
    h1 = (ULONG)f;
    f = (ULONG64)h2 + ctx -> c68k_poly1305_pad[2] + (f >> 32);
    h2 = (ULONG)f;
    f = (ULONG64)h3 + ctx -> c68k_poly1305_pad[3] + (f >> 32);
    h3 = (ULONG)f;

    c68k_poly1305_store_le(&tag[0], h0);
    c68k_poly1305_store_le(&tag[4], h1);
    c68k_poly1305_store_le(&tag[8], h2);
    c68k_poly1305_store_le(&tag[12], h3);

    /* r and the accumulator are the secret here, not the tag. */
    for (i = 0u; i < 5u; i++)
    {
        ctx -> c68k_poly1305_r[i] = 0uL;
        ctx -> c68k_poly1305_h[i] = 0uL;
    }
    for (i = 0u; i < 4u; i++)
    {
        ctx -> c68k_poly1305_pad[i] = 0uL;
    }
    for (i = 0u; i < C68K_POLY1305_BLOCK_SIZE; i++)
    {
        ctx -> c68k_poly1305_buffer[i] = 0u;
    }
}

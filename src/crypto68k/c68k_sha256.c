/*
 * AmiNetXDuo -- crypto68k: SHA-256.  The portable C compression function, the
 * buffering and padding layer both variants share, and the dispatch.
 *
 * WHAT THE C Variant does that the vendored one does not
 *
 *   Two things, and both are worth having independently of the assembly,
 *   because they are what makes "is the assembly earning its place" a fair
 *   question rather than a comparison against a straw man:
 *
 *     1. The first sixteen message words are LOADED, not assembled.  This is
 *        a big-endian machine, so W[t] for t < 16 is the longword at
 *        data + 4t.  nx_crypto_sha2.c's W0(t) macro builds each one from four
 *        byte loads, three shifts and three ORs -- 128 instructions a block
 *        that do not need to exist.  When the pointer is not longword
 *        aligned the byte path is still there, because a portable C fallback
 *        that faults on a misaligned load is not a fallback.
 *
 *     2. The message schedule is computed up front rather than interleaved
 *        with the rounds.  On a machine with eight data registers and eight
 *        live state variables, the interleaved form spills both.
 *
 * SPDX-License-Identifier: MIT
 */

#include "c68k_sha256.h"


/* ------------------------------------------------------------- variant --- */

UINT    c68k_sha256_variant = C68K_SHA256_V_BEST;

static const char *const c68k_sha256_names[C68K_SHA256_V_COUNT] =
{
    "portable C"
};

const char *c68k_sha256_variant_name(UINT variant)
{

    if (variant >= C68K_SHA256_V_COUNT)
    {
        return("(no such variant)");
    }

    return(c68k_sha256_names[variant]);
}

UINT c68k_sha256_variant_is_asm(UINT variant)
{

    /* None of it is.  See docs/RESEARCH.md 18: the 68020 assembly was
       written, measured and removed. */
    (VOID)variant;

    return(NX_CRYPTO_FALSE);
}


/* ------------------------------------------------------------ constants --- */

/*
 * Shared with the assembly, which walks it with (An)+ -- so the order is the
 * round order and nothing may be inserted.
 */
const ULONG c68k_sha256_k[64] =
{
    0x428a2f98uL, 0x71374491uL, 0xb5c0fbcfuL, 0xe9b5dba5uL,
    0x3956c25buL, 0x59f111f1uL, 0x923f82a4uL, 0xab1c5ed5uL,
    0xd807aa98uL, 0x12835b01uL, 0x243185beuL, 0x550c7dc3uL,
    0x72be5d74uL, 0x80deb1feuL, 0x9bdc06a7uL, 0xc19bf174uL,
    0xe49b69c1uL, 0xefbe4786uL, 0x0fc19dc6uL, 0x240ca1ccuL,
    0x2de92c6fuL, 0x4a7484aauL, 0x5cb0a9dcuL, 0x76f988dauL,
    0x983e5152uL, 0xa831c66duL, 0xb00327c8uL, 0xbf597fc7uL,
    0xc6e00bf3uL, 0xd5a79147uL, 0x06ca6351uL, 0x14292967uL,
    0x27b70a85uL, 0x2e1b2138uL, 0x4d2c6dfcuL, 0x53380d13uL,
    0x650a7354uL, 0x766a0abbuL, 0x81c2c92euL, 0x92722c85uL,
    0xa2bfe8a1uL, 0xa81a664buL, 0xc24b8b70uL, 0xc76c51a3uL,
    0xd192e819uL, 0xd6990624uL, 0xf40e3585uL, 0x106aa070uL,
    0x19a4c116uL, 0x1e376c08uL, 0x2748774cuL, 0x34b0bcb5uL,
    0x391c0cb3uL, 0x4ed8aa4auL, 0x5b9cca4fuL, 0x682e6ff3uL,
    0x748f82eeuL, 0x78a5636fuL, 0x84c87814uL, 0x8cc70208uL,
    0x90befffauL, 0xa4506cebuL, 0xbef9a3f7uL, 0xc67178f2uL
};


/* --------------------------------------------------- the C compression --- */

#define ROTR(x, n)  (((x) >> (n)) | ((x) << (32u - (n))))

#define BIG_S0(x)   (ROTR(x, 2u) ^ ROTR(x, 13u) ^ ROTR(x, 22u))
#define BIG_S1(x)   (ROTR(x, 6u) ^ ROTR(x, 11u) ^ ROTR(x, 25u))
#define SMALL_S0(x) (ROTR(x, 7u) ^ ROTR(x, 18u) ^ ((x) >> 3))
#define SMALL_S1(x) (ROTR(x, 17u) ^ ROTR(x, 19u) ^ ((x) >> 10))

#define CH(x, y, z)     ((z) ^ ((x) & ((y) ^ (z))))
#define MAJ(x, y, z)    (((x) & (y)) | ((z) & ((x) | (y))))

#define ROUND(a, b, c, d, e, f, g, h, t)                                \
    t1 = (h) + BIG_S1(e) + CH(e, f, g) + c68k_sha256_k[t] + w[t];       \
    t2 = BIG_S0(a) + MAJ(a, b, c);                                      \
    (d) = (d) + t1;                                                     \
    (h) = t1 + t2;

/*
 * A big-endian longword at an arbitrary address.
 *
 * On the 68020 this is ONE MOVE.L, written as inline assembly because C
 * cannot say "this pointer is not aligned and I want the load anyway": the
 * part does misaligned data accesses in hardware, and a TLS record's payload
 * starts 21 bytes into the packet buffer, so misaligned is the normal case
 * here and not the exceptional one.
 *
 * nx_crypto_sha2.c's W0() macro builds each of the sixteen message words from
 * four byte loads, three shifts and three ORs.  That is 128 instructions a
 * block that need not exist on a big-endian machine, and it is most of why
 * the C below is 1.29x the vendored implementation before any assembly is
 * written at all.
 *
 * Everything else takes the portable form, which is also the only CORRECT
 * form anywhere else: reading the longword directly is right on a big-endian
 * machine and wrong on the build host, and the host tier of the vectors is
 * what catches getting that backwards.  It did.
 */
static ULONG c68k_sha256_load_be(const UCHAR *p)
{

#ifdef __mc68020__
ULONG   v;

    __asm__ ("move.l %1,%0" : "=d" (v) : "m" (*p));

    return(v);
#else
    return((((ULONG)p[0]) << 24) | (((ULONG)p[1]) << 16) |
           (((ULONG)p[2]) << 8) | ((ULONG)p[3]));
#endif
}


static VOID c68k_sha256_blocks_c(ULONG *state, const UCHAR *data, ULONG blocks)
{

ULONG   w[64];
ULONG   a, b, c, d, e, f, g, h;
ULONG   t1, t2;
UINT    t;

    a = state[0];
    b = state[1];
    c = state[2];
    d = state[3];
    e = state[4];
    f = state[5];
    g = state[6];
    h = state[7];

    while (blocks != 0uL)
    {
        for (t = 0; t < 16u; t++)
        {
            w[t] = c68k_sha256_load_be(&data[t << 2]);
        }

        for (t = 16u; t < 64u; t++)
        {
            w[t] = SMALL_S1(w[t - 2u]) + w[t - 7u] +
                   SMALL_S0(w[t - 15u]) + w[t - 16u];
        }

        for (t = 0; t < 64u; t += 8u)
        {
            ROUND(a, b, c, d, e, f, g, h, t)
            ROUND(h, a, b, c, d, e, f, g, t + 1u)
            ROUND(g, h, a, b, c, d, e, f, t + 2u)
            ROUND(f, g, h, a, b, c, d, e, t + 3u)
            ROUND(e, f, g, h, a, b, c, d, t + 4u)
            ROUND(d, e, f, g, h, a, b, c, t + 5u)
            ROUND(c, d, e, f, g, h, a, b, t + 6u)
            ROUND(b, c, d, e, f, g, h, a, t + 7u)
        }

        a = a + state[0];
        b = b + state[1];
        c = c + state[2];
        d = d + state[3];
        e = e + state[4];
        f = f + state[5];
        g = g + state[6];
        h = h + state[7];

        state[0] = a;
        state[1] = b;
        state[2] = c;
        state[3] = d;
        state[4] = e;
        state[5] = f;
        state[6] = g;
        state[7] = h;

        data = &data[C68K_SHA256_BLOCK_SIZE];
        blocks--;
    }
}

VOID c68k_sha256_blocks(ULONG *state, const UCHAR *data, ULONG blocks)
{

    c68k_sha256_blocks_c(state, data, blocks);
}


/* ------------------------------------------------ buffering and padding --- */

UINT c68k_sha256_initialize(C68K_SHA256 *ctx, UINT algorithm)
{

    (VOID)algorithm;

    if (ctx == NX_CRYPTO_NULL)
    {
        return(NX_CRYPTO_PTR_ERROR);
    }

    ctx -> c68k_sha256_state[0] = 0x6a09e667uL;
    ctx -> c68k_sha256_state[1] = 0xbb67ae85uL;
    ctx -> c68k_sha256_state[2] = 0x3c6ef372uL;
    ctx -> c68k_sha256_state[3] = 0xa54ff53auL;
    ctx -> c68k_sha256_state[4] = 0x510e527fuL;
    ctx -> c68k_sha256_state[5] = 0x9b05688cuL;
    ctx -> c68k_sha256_state[6] = 0x1f83d9abuL;
    ctx -> c68k_sha256_state[7] = 0x5be0cd19uL;

    ctx -> c68k_sha256_bits_hi = 0uL;
    ctx -> c68k_sha256_bits_lo = 0uL;
    ctx -> c68k_sha256_used    = 0uL;

    return(NX_CRYPTO_SUCCESS);
}

UINT c68k_sha256_update(C68K_SHA256 *ctx, UCHAR *input, UINT input_length)
{

ULONG   used;
ULONG   room;
ULONG   whole;
ULONG   i;
ULONG   before;


    if ((ctx == NX_CRYPTO_NULL) ||
        ((input == NX_CRYPTO_NULL) && (input_length != 0u)))
    {
        return(NX_CRYPTO_PTR_ERROR);
    }

    if (input_length == 0u)
    {
        return(NX_CRYPTO_SUCCESS);
    }

    /* 64-bit bit count, kept as two longwords because this toolchain's
       64-bit arithmetic reaches helpers in src/common/ami_udivdi3.c. */
    before = ctx -> c68k_sha256_bits_lo;
    ctx -> c68k_sha256_bits_lo = before + (((ULONG)input_length) << 3);
    if (ctx -> c68k_sha256_bits_lo < before)
    {
        ctx -> c68k_sha256_bits_hi++;
    }
    ctx -> c68k_sha256_bits_hi += (((ULONG)input_length) >> 29);

    used = ctx -> c68k_sha256_used;

    if (used != 0uL)
    {
        room = C68K_SHA256_BLOCK_SIZE - used;

        if ((ULONG)input_length < room)
        {
            for (i = 0; i < (ULONG)input_length; i++)
            {
                ctx -> c68k_sha256_buffer[used + i] = input[i];
            }
            ctx -> c68k_sha256_used = used + (ULONG)input_length;
            return(NX_CRYPTO_SUCCESS);
        }

        for (i = 0; i < room; i++)
        {
            ctx -> c68k_sha256_buffer[used + i] = input[i];
        }

        c68k_sha256_blocks(ctx -> c68k_sha256_state,
                           ctx -> c68k_sha256_buffer, 1uL);

        input        = &input[room];
        input_length = (UINT)((ULONG)input_length - room);
        ctx -> c68k_sha256_used = 0uL;
    }

    whole = ((ULONG)input_length) >> 6;
    if (whole != 0uL)
    {
        c68k_sha256_blocks(ctx -> c68k_sha256_state, input, whole);
        input        = &input[whole << 6];
        input_length = (UINT)(((ULONG)input_length) & 63uL);
    }

    for (i = 0; i < (ULONG)input_length; i++)
    {
        ctx -> c68k_sha256_buffer[i] = input[i];
    }
    ctx -> c68k_sha256_used = (ULONG)input_length;

    return(NX_CRYPTO_SUCCESS);
}

UINT c68k_sha256_digest_calculate(C68K_SHA256 *ctx, UCHAR *digest,
                                  UINT algorithm)
{

ULONG   used;
ULONG   i;
ULONG   hi;
ULONG   lo;
ULONG   v;


    (VOID)algorithm;

    if ((ctx == NX_CRYPTO_NULL) || (digest == NX_CRYPTO_NULL))
    {
        return(NX_CRYPTO_PTR_ERROR);
    }

    hi = ctx -> c68k_sha256_bits_hi;
    lo = ctx -> c68k_sha256_bits_lo;

    used = ctx -> c68k_sha256_used;
    ctx -> c68k_sha256_buffer[used] = 0x80u;
    used++;

    if (used > (C68K_SHA256_BLOCK_SIZE - 8u))
    {
        while (used < C68K_SHA256_BLOCK_SIZE)
        {
            ctx -> c68k_sha256_buffer[used] = 0u;
            used++;
        }
        c68k_sha256_blocks(ctx -> c68k_sha256_state,
                           ctx -> c68k_sha256_buffer, 1uL);
        used = 0uL;
    }

    while (used < (C68K_SHA256_BLOCK_SIZE - 8u))
    {
        ctx -> c68k_sha256_buffer[used] = 0u;
        used++;
    }

    ctx -> c68k_sha256_buffer[56] = (UCHAR)(hi >> 24);
    ctx -> c68k_sha256_buffer[57] = (UCHAR)(hi >> 16);
    ctx -> c68k_sha256_buffer[58] = (UCHAR)(hi >> 8);
    ctx -> c68k_sha256_buffer[59] = (UCHAR)(hi);
    ctx -> c68k_sha256_buffer[60] = (UCHAR)(lo >> 24);
    ctx -> c68k_sha256_buffer[61] = (UCHAR)(lo >> 16);
    ctx -> c68k_sha256_buffer[62] = (UCHAR)(lo >> 8);
    ctx -> c68k_sha256_buffer[63] = (UCHAR)(lo);

    c68k_sha256_blocks(ctx -> c68k_sha256_state,
                       ctx -> c68k_sha256_buffer, 1uL);

    for (i = 0; i < 8uL; i++)
    {
        v = ctx -> c68k_sha256_state[i];
        digest[i << 2]          = (UCHAR)(v >> 24);
        digest[(i << 2) + 1uL]  = (UCHAR)(v >> 16);
        digest[(i << 2) + 2uL]  = (UCHAR)(v >> 8);
        digest[(i << 2) + 3uL]  = (UCHAR)(v);
    }

    return(NX_CRYPTO_SUCCESS);
}

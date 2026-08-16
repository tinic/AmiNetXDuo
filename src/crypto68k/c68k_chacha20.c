/*
 * AmiNetXDuo, crypto68k: ChaCha20 and the RFC 8439 AEAD.
 *
 * See c68k_chacha20.h for why this cipher and not AES-GCM.
 *
 * Written here from RFC 8439.  The quarter-round, the state layout and the
 * padding rule of the AEAD are the ones from the RFC.  No code was copied from
 * any implementation of it.  Checked against the RFC 8439 vectors, the 2.3.2
 * block, the 2.4.2 keystream and the 2.8.2 AEAD, in tests/crypto68k, on the
 * host and on the Amiga.
 *
 * SPDX-License-Identifier: MIT
 */

#include "c68k_chacha20.h"
#include "c68k_variant.h"


/* "expand 32-byte k", the four constant words, little-endian. */
#define C68K_CHACHA20_C0    0x61707865uL
#define C68K_CHACHA20_C1    0x3320646EuL
#define C68K_CHACHA20_C2    0x79622D32uL
#define C68K_CHACHA20_C3    0x6B206574uL

#define C68K_ROTL(v, n)     (((v) << (n)) | ((v) >> (32u - (n))))

#define C68K_QR(a, b, c, d)                                             \
    (a) += (b); (d) ^= (a); (d) = C68K_ROTL((d), 16u);                  \
    (c) += (d); (b) ^= (c); (b) = C68K_ROTL((b), 12u);                  \
    (a) += (b); (d) ^= (a); (d) = C68K_ROTL((d), 8u);                   \
    (c) += (d); (b) ^= (c); (b) = C68K_ROTL((b), 7u)


/* A little-endian longword at an arbitrary address.  Same form and the same
   __mc68020__ guard as c68k_sha256_load_be(), with a byte reversal because
   ChaCha20 reads its key and nonce little-endian on a big-endian machine. */
static ULONG c68k_chacha20_load_le(const UCHAR *p)
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


/*
 * The core: twenty rounds over a copy of the state, then the original added
 * back in.  The state is an array rather than sixteen locals.  The part has
 * eight data registers and a quarter-round needs four of them, so a compiler
 * with sixteen live values spills them anyway, and an array lets it spill the
 * ones it chooses.  docs/RESEARCH.md 18.1 is the same finding from the AES
 * side: on this machine the state lives in memory.
 *
 * The reference, not the fast path.  c68k_chacha20.S does the same sixteen
 * words in registers, eight in d0-d7, seven in a0-a6 and one on the stack,
 * exchanged with EXG, and crypto68k_bulk checks the two against each other
 * block for block before it times either.  This stays compiled and reachable
 * in the assembly build so that check is possible.
 */
VOID c68k_chacha20_core_c(const ULONG *in, ULONG *out)
{

ULONG   x[16];
UINT    i;


    for (i = 0u; i < 16u; i++)
    {
        x[i] = in[i];
    }

    for (i = 0u; i < 10u; i++)
    {

        /* Columns. */
        C68K_QR(x[0], x[4], x[8],  x[12]);
        C68K_QR(x[1], x[5], x[9],  x[13]);
        C68K_QR(x[2], x[6], x[10], x[14]);
        C68K_QR(x[3], x[7], x[11], x[15]);

        /* Diagonals. */
        C68K_QR(x[0], x[5], x[10], x[15]);
        C68K_QR(x[1], x[6], x[11], x[12]);
        C68K_QR(x[2], x[7], x[8],  x[13]);
        C68K_QR(x[3], x[4], x[9],  x[14]);
    }

    for (i = 0u; i < 16u; i++)
    {
        out[i] = x[i] + in[i];
    }
}


/*
 * Which one the rest of this file calls.  A macro rather than a wrapper so a
 * build without the assembly has no extra call, and not a variant switch like
 * the one in c68k_aes.c, because there is nothing to choose between.  The
 * assembly is the same algorithm, faster, on every part that can run it.
 * AMINETXDUO_CRYPTO68K_ASM=OFF takes the C, and so do the 68000 and the 68060.
 */
#ifdef C68K_ASM_CHACHA20
extern VOID c68k_chacha20_core_asm(const ULONG *in, ULONG *out);
#define C68K_CHACHA20_CORE  c68k_chacha20_core_asm
#else
#define C68K_CHACHA20_CORE  c68k_chacha20_core_c
#endif

UINT c68k_chacha20_core_is_asm(VOID)
{

#ifdef C68K_ASM_CHACHA20
    return((UINT)NX_CRYPTO_TRUE);
#else
    return((UINT)NX_CRYPTO_FALSE);
#endif
}


/*
 * out = in XOR the little-endian serialisation of one keystream block.
 *
 * On a 68020 that is one MOVE.L in, a three-instruction byte reversal of the
 * keystream word, an EOR.L and one MOVE.L out, about 34 cycles for four
 * bytes.  A serialisation of the block to a byte array with a byte-by-byte
 * exclusive-or, which is what the portable form below has to do, is four times
 * that by the instruction table in docs/RESEARCH.md 18.1.
 *
 * The 68020 build takes the assembly instead: this loop is a sixth of the
 * cipher and -Os does not compile it to those seven instructions.
 */
#ifndef C68K_ASM_CHACHA20
static VOID c68k_chacha20_xor_block(const ULONG *ks, const UCHAR *in,
                                    UCHAR *out)
{

UINT    i;
#ifdef __mc68020__
ULONG   k, p;


    for (i = 0u; i < 16u; i++)
    {
        k = ks[i];
        __asm__ ("rol.w #8,%0\n\tswap %0\n\trol.w #8,%0" : "+d" (k));
        __asm__ ("move.l %1,%0" : "=d" (p) : "m" (*in));
        p ^= k;
        __asm__ ("move.l %1,%0" : "=m" (*out) : "d" (p));
        in += 4;
        out += 4;
    }
#else
ULONG   k;


    for (i = 0u; i < 16u; i++)
    {
        k = ks[i];
        out[0] = (UCHAR)(in[0] ^ (UCHAR)(k));
        out[1] = (UCHAR)(in[1] ^ (UCHAR)(k >> 8));
        out[2] = (UCHAR)(in[2] ^ (UCHAR)(k >> 16));
        out[3] = (UCHAR)(in[3] ^ (UCHAR)(k >> 24));
        in += 4;
        out += 4;
    }
#endif
}
#else
/* The same seven instructions a word, hand-written: see c68k_chacha20.S for
   what -Os made of the loop above.  Same #ifndef/#ifdef structure as the limb
   primitives in c68k_prim.c. */
extern VOID c68k_chacha20_xor_block_asm(const ULONG *ks, const UCHAR *in,
                                        UCHAR *out);
#define c68k_chacha20_xor_block  c68k_chacha20_xor_block_asm
#endif /* C68K_ASM */

/* The same block, serialised rather than applied, for the tail of a
   transfer, which has to be held until the next call asks for it. */
static VOID c68k_chacha20_store_block(const ULONG *ks, UCHAR *out)
{

UINT    i;
ULONG   k;


    for (i = 0u; i < 16u; i++)
    {
        k = ks[i];
        out[0] = (UCHAR)(k);
        out[1] = (UCHAR)(k >> 8);
        out[2] = (UCHAR)(k >> 16);
        out[3] = (UCHAR)(k >> 24);
        out += 4;
    }
}


VOID c68k_chacha20_initialize(C68K_CHACHA20 *ctx, const UCHAR *key,
                              const UCHAR *nonce, ULONG counter)
{

UINT    i;


    ctx -> c68k_chacha20_state[0] = C68K_CHACHA20_C0;
    ctx -> c68k_chacha20_state[1] = C68K_CHACHA20_C1;
    ctx -> c68k_chacha20_state[2] = C68K_CHACHA20_C2;
    ctx -> c68k_chacha20_state[3] = C68K_CHACHA20_C3;

    for (i = 0u; i < 8u; i++)
    {
        ctx -> c68k_chacha20_state[4u + i] =
            c68k_chacha20_load_le(&key[i << 2]);
    }

    ctx -> c68k_chacha20_state[12] = counter;

    for (i = 0u; i < 3u; i++)
    {
        ctx -> c68k_chacha20_state[13u + i] =
            c68k_chacha20_load_le(&nonce[i << 2]);
    }

    ctx -> c68k_chacha20_used = C68K_CHACHA20_BLOCK_SIZE;
}


VOID c68k_chacha20_xor(C68K_CHACHA20 *ctx, const UCHAR *in, UCHAR *out,
                       ULONG length)
{

ULONG   ks[16];
ULONG   want;
UINT    i;


    /* The bytes the last call left behind, first. */
    if (ctx -> c68k_chacha20_used < C68K_CHACHA20_BLOCK_SIZE)
    {
        want = (ULONG)(C68K_CHACHA20_BLOCK_SIZE - ctx -> c68k_chacha20_used);
        if (want > length)
        {
            want = length;
        }

        for (i = 0u; i < (UINT)want; i++)
        {
            out[i] = (UCHAR)(in[i] ^
                ctx -> c68k_chacha20_stream[ctx -> c68k_chacha20_used + i]);
        }

        ctx -> c68k_chacha20_used += (UINT)want;
        in += want;
        out += want;
        length -= want;
    }

    while (length >= C68K_CHACHA20_BLOCK_SIZE)
    {
        C68K_CHACHA20_CORE(ctx -> c68k_chacha20_state, ks);
        ctx -> c68k_chacha20_state[12]++;

        c68k_chacha20_xor_block(ks, in, out);

        in += C68K_CHACHA20_BLOCK_SIZE;
        out += C68K_CHACHA20_BLOCK_SIZE;
        length -= C68K_CHACHA20_BLOCK_SIZE;
    }

    if (length != 0uL)
    {
        C68K_CHACHA20_CORE(ctx -> c68k_chacha20_state, ks);
        ctx -> c68k_chacha20_state[12]++;

        c68k_chacha20_store_block(ks, ctx -> c68k_chacha20_stream);

        for (i = 0u; i < (UINT)length; i++)
        {
            out[i] = (UCHAR)(in[i] ^ ctx -> c68k_chacha20_stream[i]);
        }

        ctx -> c68k_chacha20_used = (UINT)length;
    }
}


VOID c68k_chacha20_keystream(C68K_CHACHA20 *ctx, UCHAR *out, ULONG length)
{

UINT    i;


    for (i = 0u; i < (UINT)length; i++)
    {
        out[i] = 0u;
    }

    c68k_chacha20_xor(ctx, out, out, length);
}


/* ------------------------------------------------------------- the AEAD, */

/* The zero padding that brings a half to a multiple of sixteen.  RFC 8439
   pads the associated data and the ciphertext independently, so one cannot be
   read as part of the other. */
static VOID c68k_chacha20_poly1305_pad16(C68K_CHACHA20_POLY1305 *ctx,
                                         ULONG so_far)
{

UCHAR   zeros[16];
UINT    n;
UINT    i;


    n = (UINT)(so_far & 15uL);
    if (n == 0u)
    {
        return;
    }

    n = 16u - n;
    for (i = 0u; i < n; i++)
    {
        zeros[i] = 0u;
    }

    c68k_poly1305_update(&ctx -> c68k_aead_mac, zeros, (ULONG)n);
}

/* Called at the associated-data-to-payload transition, and again from _tag()
   in case there was no payload. */
static VOID c68k_chacha20_poly1305_start_data(C68K_CHACHA20_POLY1305 *ctx)
{

    if (ctx -> c68k_aead_data_started == 0u)
    {
        c68k_chacha20_poly1305_pad16(ctx, ctx -> c68k_aead_aad_length);
        ctx -> c68k_aead_data_started = 1u;
    }
}


VOID c68k_chacha20_poly1305_initialize(C68K_CHACHA20_POLY1305 *ctx,
                                       const UCHAR *key, const UCHAR *nonce)
{

UCHAR   mac_key[C68K_POLY1305_KEY_SIZE];
UINT    i;


    /*
     * The one-time Poly1305 key is the first 32 bytes of the cipher's own
     * block 0, and the payload then starts at block 1.  The key is one-time
     * without a second key exchange because it is a function of this record's
     * nonce.
     */
    c68k_chacha20_initialize(&ctx -> c68k_aead_cipher, key, nonce, 0uL);
    c68k_chacha20_keystream(&ctx -> c68k_aead_cipher, mac_key,
                            (ULONG)C68K_POLY1305_KEY_SIZE);

    c68k_poly1305_initialize(&ctx -> c68k_aead_mac, mac_key);

    for (i = 0u; i < C68K_POLY1305_KEY_SIZE; i++)
    {
        mac_key[i] = 0u;
    }

    c68k_chacha20_initialize(&ctx -> c68k_aead_cipher, key, nonce, 1uL);

    ctx -> c68k_aead_aad_length = 0uL;
    ctx -> c68k_aead_data_length = 0uL;
    ctx -> c68k_aead_data_started = 0u;
}


VOID c68k_chacha20_poly1305_associate(C68K_CHACHA20_POLY1305 *ctx,
                                      const UCHAR *aad, ULONG aad_length)
{

    if (ctx -> c68k_aead_data_started != 0u)
    {
        return;
    }

    c68k_poly1305_update(&ctx -> c68k_aead_mac, aad, aad_length);
    ctx -> c68k_aead_aad_length += aad_length;
}


VOID c68k_chacha20_poly1305_encrypt(C68K_CHACHA20_POLY1305 *ctx,
                                    const UCHAR *in, UCHAR *out,
                                    ULONG length)
{

    c68k_chacha20_poly1305_start_data(ctx);

    c68k_chacha20_xor(&ctx -> c68k_aead_cipher, in, out, length);

    /* The tag covers the ciphertext, encrypt-then-MAC, so a forged record
       costs nothing to reject. */
    c68k_poly1305_update(&ctx -> c68k_aead_mac, out, length);
    ctx -> c68k_aead_data_length += length;
}


VOID c68k_chacha20_poly1305_decrypt(C68K_CHACHA20_POLY1305 *ctx,
                                    const UCHAR *in, UCHAR *out,
                                    ULONG length)
{

    c68k_chacha20_poly1305_start_data(ctx);

    /* Read the ciphertext into the tag before it is overwritten, because `in`
       and `out` are the same buffer on the record path. */
    c68k_poly1305_update(&ctx -> c68k_aead_mac, in, length);
    ctx -> c68k_aead_data_length += length;

    c68k_chacha20_xor(&ctx -> c68k_aead_cipher, in, out, length);
}


VOID c68k_chacha20_poly1305_tag(C68K_CHACHA20_POLY1305 *ctx, UCHAR *tag)
{

UCHAR   lengths[16];
ULONG   n;
UINT    i;


    c68k_chacha20_poly1305_start_data(ctx);
    c68k_chacha20_poly1305_pad16(ctx, ctx -> c68k_aead_data_length);

    /* The two lengths, each a 64-bit little-endian count.  A TLS record
       cannot reach 2^32 bytes, so the top halves are zero.  They are written
       out because the field is 64 bits wide. */
    for (i = 0u; i < 16u; i++)
    {
        lengths[i] = 0u;
    }

    n = ctx -> c68k_aead_aad_length;
    lengths[0] = (UCHAR)(n);
    lengths[1] = (UCHAR)(n >> 8);
    lengths[2] = (UCHAR)(n >> 16);
    lengths[3] = (UCHAR)(n >> 24);

    n = ctx -> c68k_aead_data_length;
    lengths[8]  = (UCHAR)(n);
    lengths[9]  = (UCHAR)(n >> 8);
    lengths[10] = (UCHAR)(n >> 16);
    lengths[11] = (UCHAR)(n >> 24);

    c68k_poly1305_update(&ctx -> c68k_aead_mac, lengths, 16uL);
    c68k_poly1305_finish(&ctx -> c68k_aead_mac, tag);
}


UINT c68k_chacha20_poly1305_verify(const UCHAR *a, const UCHAR *b)
{

UCHAR   diff;
UINT    i;


    diff = 0u;
    for (i = 0u; i < C68K_POLY1305_TAG_SIZE; i++)
    {
        diff = (UCHAR)(diff | (UCHAR)(a[i] ^ b[i]));
    }

    return((diff == 0u) ? (UINT)NX_CRYPTO_TRUE : (UINT)NX_CRYPTO_FALSE);
}

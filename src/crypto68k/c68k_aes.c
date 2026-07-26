/*
 * AmiNetXDuo -- crypto68k: AES-128/192/256 CBC.  Tables, key schedule, and
 * the portable C forms of every variant the 68020 assembly is measured
 * against.  See c68k_aes.h for why this file exists and what the machine's
 * missing data cache changed about it.
 *
 * WHAT IS IN HERE, AND WHY THERE IS MORE THAN ONE OF IT
 *
 *   Three portable implementations of the same cipher.  That is not
 *   indecision: "what is the right AES for a machine with no data cache" has
 *   three plausible answers in the literature and they disagree about which
 *   resource is scarce.  All three are built, all three are checked against
 *   the NIST vectors and against each other, and all three are timed in one
 *   process on the same buffer by tests/crypto68k/crypto68k_bulk.  The two
 *   that lose are kept, because a measurement nobody can reproduce is an
 *   assertion.
 *
 *     T4    four 1 KB tables.  One indexed longword read per byte and no
 *           post-processing.  What OpenSSL's aes_core.c does, and therefore
 *           what AmiSSL runs on this target.
 *     T1    one 1 KB table and three rotates per column.  What
 *           nx_crypto_aes.c does.  Costs 12 rotates a round to save 3 KB of
 *           table -- a trade that only pays if the 3 KB would have displaced
 *           something from a data cache.
 *     SBOX  a 256-byte S-box and MixColumns done in the ALU, word-parallel.
 *           Trades 4-byte reads for 1-byte reads, which is the right trade
 *           only if the bus is the binding constraint.
 *
 *   The tables are BUILT, not spelled out: 2,048 hexadecimal constants in a
 *   source file cannot be reviewed, and generating them from the field
 *   arithmetic is 60 lines that can.  The cost is a few hundred microseconds
 *   once, inside the first c68k_aes_key_set().
 *
 * SPDX-License-Identifier: MIT
 */

#include "c68k_aes.h"


/* ------------------------------------------------------------- variant --- */

UINT    c68k_aes_variant = C68K_AES_V_BEST;

static const char *const c68k_aes_names[C68K_AES_V_COUNT] =
{
    "T4   four 1 KB tables, C",
    "T1   one 1 KB table + rotates, C",
    "SBOX 256 B S-box, MixColumns in the ALU, C",
    "T4   four 1 KB tables, 68020 assembly",
    "T1   one 1 KB table + rotates, 68020 assembly"
};

const char *c68k_aes_variant_name(UINT variant)
{

    if (variant >= C68K_AES_V_COUNT)
    {
        return("(no such variant)");
    }

    return(c68k_aes_names[variant]);
}

UINT c68k_aes_variant_is_asm(UINT variant)
{

#ifdef C68K_ASM
    return((variant >= C68K_AES_V_T4_ASM) ? NX_CRYPTO_TRUE : NX_CRYPTO_FALSE);
#else
    (VOID)variant;
    return(NX_CRYPTO_FALSE);
#endif
}


/* -------------------------------------------------------------- tables --- */

/*
 * Shared with the assembly, which indexes them with the 68020's scaled
 * (An,Dn.W*4) mode and therefore needs them to be exactly what it thinks they
 * are.  Not static, and named without the usual underscore prefix confusion:
 * the assembler sees them as _c68k_aes_te0 and so on.
 */
ULONG   c68k_aes_te[4][256];        /* Te0..Te3, encryption               */
ULONG   c68k_aes_td[4][256];        /* Td0..Td3, equivalent inverse       */
UCHAR   c68k_aes_sbox[256];
UCHAR   c68k_aes_isbox[256];

/*
 * Built once, on the first c68k_aes_key_set().  Not guarded by anything, and
 * that is deliberate rather than overlooked: tls.library is an AmigaOS shared
 * library with ONE data segment behind every opener, so two processes can
 * reach this at the same time.
 *
 * It is safe because both of them write the SAME BYTES.  The flag is set last,
 * so a thread that sees it set is looking at a finished table; a thread that
 * does not see it set rebuilds, and every longword it stores is the value that
 * was already there.  Reads only ever begin after the reader's own
 * c68k_aes_key_set() returned, which means after its own build completed.  A
 * Forbid() pair would buy nothing and would have to be explained anyway.
 */
static UINT c68k_aes_tables_ready;

/* a * b in GF(2^8) with the AES polynomial, by the schoolbook double-and-add.
   Used only while building the tables, so its cost is irrelevant. */
static UCHAR c68k_gf_mul(UCHAR a, UCHAR b)
{

UCHAR   r;
UINT    i;


    r = 0;
    for (i = 0; i < 8u; i++)
    {
        if ((b & 1u) != 0u)
        {
            r = (UCHAR)(r ^ a);
        }
        b = (UCHAR)(b >> 1);
        if ((a & 0x80u) != 0u)
        {
            a = (UCHAR)(((UCHAR)(a << 1)) ^ 0x1Bu);
        }
        else
        {
            a = (UCHAR)(a << 1);
        }
    }

    return(r);
}

static ULONG c68k_ror32(ULONG v, UINT n)
{

    return(((v >> n) | (v << (32u - n))) & 0xFFFFFFFFuL);
}

static VOID c68k_aes_tables_init(VOID)
{

UCHAR   pow_tab[256];
UCHAR   log_tab[256];
UINT    i;
UCHAR   x;
UCHAR   s;
UCHAR   inv;
ULONG   w;


    if (c68k_aes_tables_ready != 0u)
    {
        return;
    }

    /* Powers of the generator 3, and the log that inverts them.  x * 3 is
       x ^ xtime(x), which is what the loop below writes out. */
    x = 1u;
    for (i = 0; i < 256u; i++)
    {
        pow_tab[i]      = x;
        log_tab[x]      = (UCHAR)i;
        x               = c68k_gf_mul(x, 3u);
    }

    for (i = 0; i < 256u; i++)
    {
        if (i == 0u)
        {
            inv = 0u;
        }
        else
        {
            inv = pow_tab[255u - log_tab[i]];
        }

        /* The AES affine transform: b ^ rotl(b,1) ^ rotl(b,2) ^ rotl(b,3) ^
           rotl(b,4) ^ 0x63, over 8-bit rotations. */
        s = (UCHAR)(inv ^ 0x63u);
        s = (UCHAR)(s ^ (UCHAR)((inv << 1) | (inv >> 7)));
        s = (UCHAR)(s ^ (UCHAR)((inv << 2) | (inv >> 6)));
        s = (UCHAR)(s ^ (UCHAR)((inv << 3) | (inv >> 5)));
        s = (UCHAR)(s ^ (UCHAR)((inv << 4) | (inv >> 4)));

        c68k_aes_sbox[i] = s;
        c68k_aes_isbox[s] = (UCHAR)i;
    }

    for (i = 0; i < 256u; i++)
    {
        s = c68k_aes_sbox[i];

        /* Te0 = [2s, s, s, 3s], most significant byte first -- which on this
           machine is byte order, so the assembly can load it with MOVE.L. */
        w = (((ULONG)c68k_gf_mul(s, 2u)) << 24) |
            (((ULONG)s) << 16) |
            (((ULONG)s) << 8) |
            ((ULONG)c68k_gf_mul(s, 3u));

        c68k_aes_te[0][i] = w;
        c68k_aes_te[1][i] = c68k_ror32(w, 8u);
        c68k_aes_te[2][i] = c68k_ror32(w, 16u);
        c68k_aes_te[3][i] = c68k_ror32(w, 24u);

        s = c68k_aes_isbox[i];

        /* Td0 = [14s, 9s, 13s, 11s] -- the first column of InvMixColumns. */
        w = (((ULONG)c68k_gf_mul(s, 14u)) << 24) |
            (((ULONG)c68k_gf_mul(s, 9u)) << 16) |
            (((ULONG)c68k_gf_mul(s, 13u)) << 8) |
            ((ULONG)c68k_gf_mul(s, 11u));

        c68k_aes_td[0][i] = w;
        c68k_aes_td[1][i] = c68k_ror32(w, 8u);
        c68k_aes_td[2][i] = c68k_ror32(w, 16u);
        c68k_aes_td[3][i] = c68k_ror32(w, 24u);
    }

    c68k_aes_tables_ready = 1u;
}


/* -------------------------------------------------------- key schedule --- */

static const UCHAR c68k_aes_rcon[11] =
{
    0x00u, 0x01u, 0x02u, 0x04u, 0x08u, 0x10u,
    0x20u, 0x40u, 0x80u, 0x1Bu, 0x36u
};

/*
 * A big-endian longword at an arbitrary address.
 *
 * On the 68020 that is one MOVE.L: the part does misaligned data accesses in
 * hardware, at the cost of an extra bus cycle, and this is the normal case
 * rather than the exceptional one -- a TLS record's payload starts 21 bytes
 * into the packet buffer, so every CBC block in the record path is on an odd
 * address.  Written as inline assembly because C cannot express "I know this
 * pointer is not aligned and I want the load anyway": GCC compiles the
 * portable form below into four byte reads, three shifts and three ORs, which
 * is 300 cycles a block of load and store around a 3,000 cycle cipher.
 */
static ULONG c68k_aes_load_be(const UCHAR *p)
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

static VOID c68k_aes_store_be(UCHAR *p, ULONG v)
{

#ifdef __mc68020__
    __asm__ ("move.l %1,%0" : "=m" (*p) : "d" (v));
#else
    p[0] = (UCHAR)(v >> 24);
    p[1] = (UCHAR)(v >> 16);
    p[2] = (UCHAR)(v >> 8);
    p[3] = (UCHAR)(v);
#endif
}

static ULONG c68k_aes_subword(ULONG w)
{

    return((((ULONG)c68k_aes_sbox[(w >> 24) & 0xFFu]) << 24) |
           (((ULONG)c68k_aes_sbox[(w >> 16) & 0xFFu]) << 16) |
           (((ULONG)c68k_aes_sbox[(w >> 8) & 0xFFu]) << 8) |
           ((ULONG)c68k_aes_sbox[w & 0xFFu]));
}

/* One column through InvMixColumns, which is what turns an encryption round
   key into an equivalent-inverse-cipher one. */
static ULONG c68k_aes_invmix(ULONG w)
{

UCHAR   a0, a1, a2, a3;


    a0 = (UCHAR)(w >> 24);
    a1 = (UCHAR)(w >> 16);
    a2 = (UCHAR)(w >> 8);
    a3 = (UCHAR)(w);

    return((((ULONG)(UCHAR)(c68k_gf_mul(a0, 14u) ^ c68k_gf_mul(a1, 11u) ^
                            c68k_gf_mul(a2, 13u) ^ c68k_gf_mul(a3, 9u))) << 24) |
           (((ULONG)(UCHAR)(c68k_gf_mul(a0, 9u) ^ c68k_gf_mul(a1, 14u) ^
                            c68k_gf_mul(a2, 11u) ^ c68k_gf_mul(a3, 13u))) << 16) |
           (((ULONG)(UCHAR)(c68k_gf_mul(a0, 13u) ^ c68k_gf_mul(a1, 9u) ^
                            c68k_gf_mul(a2, 14u) ^ c68k_gf_mul(a3, 11u))) << 8) |
           ((ULONG)(UCHAR)(c68k_gf_mul(a0, 11u) ^ c68k_gf_mul(a1, 13u) ^
                           c68k_gf_mul(a2, 9u) ^ c68k_gf_mul(a3, 14u))));
}

UINT c68k_aes_key_set(C68K_AES *aes, const UCHAR *key, UINT key_bits)
{

UINT    nk;
UINT    nr;
UINT    i;
UINT    r;
UINT    j;
ULONG   temp;


    if ((aes == NX_CRYPTO_NULL) || (key == NX_CRYPTO_NULL))
    {
        return(NX_CRYPTO_PTR_ERROR);
    }

    if ((key_bits != 128u) && (key_bits != 192u) && (key_bits != 256u))
    {
        return(NX_CRYPTO_INVALID_PARAMETER);
    }

    c68k_aes_tables_init();

    nk = key_bits >> 5;
    nr = nk + 6u;

    aes -> c68k_aes_rounds = nr;

    for (i = 0; i < nk; i++)
    {
        aes -> c68k_aes_ek[i] = c68k_aes_load_be(&key[i << 2]);
    }

    for (i = nk; i < ((nr + 1u) << 2); i++)
    {
        temp = aes -> c68k_aes_ek[i - 1u];

        if ((i % nk) == 0u)
        {
            temp = c68k_aes_subword((temp << 8) | (temp >> 24));
            temp = temp ^ (((ULONG)c68k_aes_rcon[i / nk]) << 24);
        }
        else if ((nk > 6u) && ((i % nk) == 4u))
        {
            temp = c68k_aes_subword(temp);
        }
        else
        {
            /* nothing */
        }

        aes -> c68k_aes_ek[i] = aes -> c68k_aes_ek[i - nk] ^ temp;
    }

    /* The equivalent inverse cipher: the same words, round groups reversed,
       with InvMixColumns applied to everything but the first and last. */
    for (r = 0; r <= nr; r++)
    {
        for (j = 0; j < 4u; j++)
        {
            aes -> c68k_aes_dk[(r << 2) + j] =
                aes -> c68k_aes_ek[((nr - r) << 2) + j];
        }
    }

    for (r = 1u; r < nr; r++)
    {
        for (j = 0; j < 4u; j++)
        {
            aes -> c68k_aes_dk[(r << 2) + j] =
                c68k_aes_invmix(aes -> c68k_aes_dk[(r << 2) + j]);
        }
    }

    return(NX_CRYPTO_SUCCESS);
}


/* =============================================================== T4, C ==== */

#define B0(x)   ((UINT)((x) >> 24))
#define B1(x)   ((UINT)(((x) >> 16) & 0xFFu))
#define B2(x)   ((UINT)(((x) >> 8) & 0xFFu))
#define B3(x)   ((UINT)((x) & 0xFFu))

static VOID c68k_aes_enc_t4(const ULONG *rk, UINT nr, ULONG *st)
{

ULONG   s0, s1, s2, s3;
ULONG   t0, t1, t2, t3;
UINT    r;


    s0 = st[0] ^ rk[0];
    s1 = st[1] ^ rk[1];
    s2 = st[2] ^ rk[2];
    s3 = st[3] ^ rk[3];
    rk = &rk[4];

    for (r = 1u; r < nr; r++)
    {
        t0 = c68k_aes_te[0][B0(s0)] ^ c68k_aes_te[1][B1(s1)] ^
             c68k_aes_te[2][B2(s2)] ^ c68k_aes_te[3][B3(s3)] ^ rk[0];
        t1 = c68k_aes_te[0][B0(s1)] ^ c68k_aes_te[1][B1(s2)] ^
             c68k_aes_te[2][B2(s3)] ^ c68k_aes_te[3][B3(s0)] ^ rk[1];
        t2 = c68k_aes_te[0][B0(s2)] ^ c68k_aes_te[1][B1(s3)] ^
             c68k_aes_te[2][B2(s0)] ^ c68k_aes_te[3][B3(s1)] ^ rk[2];
        t3 = c68k_aes_te[0][B0(s3)] ^ c68k_aes_te[1][B1(s0)] ^
             c68k_aes_te[2][B2(s1)] ^ c68k_aes_te[3][B3(s2)] ^ rk[3];

        s0 = t0;
        s1 = t1;
        s2 = t2;
        s3 = t3;
        rk = &rk[4];
    }

    st[0] = ((((ULONG)c68k_aes_sbox[B0(s0)]) << 24) |
            (((ULONG)c68k_aes_sbox[B1(s1)]) << 16) |
            (((ULONG)c68k_aes_sbox[B2(s2)]) << 8) |
            ((ULONG)c68k_aes_sbox[B3(s3)])) ^ rk[0];
    st[1] = ((((ULONG)c68k_aes_sbox[B0(s1)]) << 24) |
            (((ULONG)c68k_aes_sbox[B1(s2)]) << 16) |
            (((ULONG)c68k_aes_sbox[B2(s3)]) << 8) |
            ((ULONG)c68k_aes_sbox[B3(s0)])) ^ rk[1];
    st[2] = ((((ULONG)c68k_aes_sbox[B0(s2)]) << 24) |
            (((ULONG)c68k_aes_sbox[B1(s3)]) << 16) |
            (((ULONG)c68k_aes_sbox[B2(s0)]) << 8) |
            ((ULONG)c68k_aes_sbox[B3(s1)])) ^ rk[2];
    st[3] = ((((ULONG)c68k_aes_sbox[B0(s3)]) << 24) |
            (((ULONG)c68k_aes_sbox[B1(s0)]) << 16) |
            (((ULONG)c68k_aes_sbox[B2(s1)]) << 8) |
            ((ULONG)c68k_aes_sbox[B3(s2)])) ^ rk[3];
}

static VOID c68k_aes_dec_t4(const ULONG *rk, UINT nr, ULONG *st)
{

ULONG   s0, s1, s2, s3;
ULONG   t0, t1, t2, t3;
UINT    r;


    s0 = st[0] ^ rk[0];
    s1 = st[1] ^ rk[1];
    s2 = st[2] ^ rk[2];
    s3 = st[3] ^ rk[3];
    rk = &rk[4];

    for (r = 1u; r < nr; r++)
    {
        t0 = c68k_aes_td[0][B0(s0)] ^ c68k_aes_td[1][B1(s3)] ^
             c68k_aes_td[2][B2(s2)] ^ c68k_aes_td[3][B3(s1)] ^ rk[0];
        t1 = c68k_aes_td[0][B0(s1)] ^ c68k_aes_td[1][B1(s0)] ^
             c68k_aes_td[2][B2(s3)] ^ c68k_aes_td[3][B3(s2)] ^ rk[1];
        t2 = c68k_aes_td[0][B0(s2)] ^ c68k_aes_td[1][B1(s1)] ^
             c68k_aes_td[2][B2(s0)] ^ c68k_aes_td[3][B3(s3)] ^ rk[2];
        t3 = c68k_aes_td[0][B0(s3)] ^ c68k_aes_td[1][B1(s2)] ^
             c68k_aes_td[2][B2(s1)] ^ c68k_aes_td[3][B3(s0)] ^ rk[3];

        s0 = t0;
        s1 = t1;
        s2 = t2;
        s3 = t3;
        rk = &rk[4];
    }

    st[0] = ((((ULONG)c68k_aes_isbox[B0(s0)]) << 24) |
            (((ULONG)c68k_aes_isbox[B1(s3)]) << 16) |
            (((ULONG)c68k_aes_isbox[B2(s2)]) << 8) |
            ((ULONG)c68k_aes_isbox[B3(s1)])) ^ rk[0];
    st[1] = ((((ULONG)c68k_aes_isbox[B0(s1)]) << 24) |
            (((ULONG)c68k_aes_isbox[B1(s0)]) << 16) |
            (((ULONG)c68k_aes_isbox[B2(s3)]) << 8) |
            ((ULONG)c68k_aes_isbox[B3(s2)])) ^ rk[1];
    st[2] = ((((ULONG)c68k_aes_isbox[B0(s2)]) << 24) |
            (((ULONG)c68k_aes_isbox[B1(s1)]) << 16) |
            (((ULONG)c68k_aes_isbox[B2(s0)]) << 8) |
            ((ULONG)c68k_aes_isbox[B3(s3)])) ^ rk[2];
    st[3] = ((((ULONG)c68k_aes_isbox[B0(s3)]) << 24) |
            (((ULONG)c68k_aes_isbox[B1(s2)]) << 16) |
            (((ULONG)c68k_aes_isbox[B2(s1)]) << 8) |
            ((ULONG)c68k_aes_isbox[B3(s0)])) ^ rk[3];
}


/* =============================================================== T1, C ==== */
/*
 * The same rounds with Te1..Te3 replaced by rotations of Te0.  This is what
 * nx_crypto_aes.c does, and it is the shape that a machine with a data cache
 * prefers.  Kept so the comparison is against a real implementation of the
 * idea rather than against a description of one.
 */

#define ROR8(v)     (((v) >> 8) | ((v) << 24))
#define ROR16(v)    (((v) >> 16) | ((v) << 16))
#define ROR24(v)    (((v) >> 24) | ((v) << 8))

static VOID c68k_aes_enc_t1(const ULONG *rk, UINT nr, ULONG *st)
{

ULONG   s0, s1, s2, s3;
ULONG   t0, t1, t2, t3;
UINT    r;


    s0 = st[0] ^ rk[0];
    s1 = st[1] ^ rk[1];
    s2 = st[2] ^ rk[2];
    s3 = st[3] ^ rk[3];
    rk = &rk[4];

    for (r = 1u; r < nr; r++)
    {
        t0 = c68k_aes_te[0][B0(s0)] ^ ROR8(c68k_aes_te[0][B1(s1)]) ^
             ROR16(c68k_aes_te[0][B2(s2)]) ^ ROR24(c68k_aes_te[0][B3(s3)]) ^ rk[0];
        t1 = c68k_aes_te[0][B0(s1)] ^ ROR8(c68k_aes_te[0][B1(s2)]) ^
             ROR16(c68k_aes_te[0][B2(s3)]) ^ ROR24(c68k_aes_te[0][B3(s0)]) ^ rk[1];
        t2 = c68k_aes_te[0][B0(s2)] ^ ROR8(c68k_aes_te[0][B1(s3)]) ^
             ROR16(c68k_aes_te[0][B2(s0)]) ^ ROR24(c68k_aes_te[0][B3(s1)]) ^ rk[2];
        t3 = c68k_aes_te[0][B0(s3)] ^ ROR8(c68k_aes_te[0][B1(s0)]) ^
             ROR16(c68k_aes_te[0][B2(s1)]) ^ ROR24(c68k_aes_te[0][B3(s2)]) ^ rk[3];

        s0 = t0;
        s1 = t1;
        s2 = t2;
        s3 = t3;
        rk = &rk[4];
    }

    st[0] = ((((ULONG)c68k_aes_sbox[B0(s0)]) << 24) |
            (((ULONG)c68k_aes_sbox[B1(s1)]) << 16) |
            (((ULONG)c68k_aes_sbox[B2(s2)]) << 8) |
            ((ULONG)c68k_aes_sbox[B3(s3)])) ^ rk[0];
    st[1] = ((((ULONG)c68k_aes_sbox[B0(s1)]) << 24) |
            (((ULONG)c68k_aes_sbox[B1(s2)]) << 16) |
            (((ULONG)c68k_aes_sbox[B2(s3)]) << 8) |
            ((ULONG)c68k_aes_sbox[B3(s0)])) ^ rk[1];
    st[2] = ((((ULONG)c68k_aes_sbox[B0(s2)]) << 24) |
            (((ULONG)c68k_aes_sbox[B1(s3)]) << 16) |
            (((ULONG)c68k_aes_sbox[B2(s0)]) << 8) |
            ((ULONG)c68k_aes_sbox[B3(s1)])) ^ rk[2];
    st[3] = ((((ULONG)c68k_aes_sbox[B0(s3)]) << 24) |
            (((ULONG)c68k_aes_sbox[B1(s0)]) << 16) |
            (((ULONG)c68k_aes_sbox[B2(s1)]) << 8) |
            ((ULONG)c68k_aes_sbox[B3(s2)])) ^ rk[3];
}

static VOID c68k_aes_dec_t1(const ULONG *rk, UINT nr, ULONG *st)
{

ULONG   s0, s1, s2, s3;
ULONG   t0, t1, t2, t3;
UINT    r;


    s0 = st[0] ^ rk[0];
    s1 = st[1] ^ rk[1];
    s2 = st[2] ^ rk[2];
    s3 = st[3] ^ rk[3];
    rk = &rk[4];

    for (r = 1u; r < nr; r++)
    {
        t0 = c68k_aes_td[0][B0(s0)] ^ ROR8(c68k_aes_td[0][B1(s3)]) ^
             ROR16(c68k_aes_td[0][B2(s2)]) ^ ROR24(c68k_aes_td[0][B3(s1)]) ^ rk[0];
        t1 = c68k_aes_td[0][B0(s1)] ^ ROR8(c68k_aes_td[0][B1(s0)]) ^
             ROR16(c68k_aes_td[0][B2(s3)]) ^ ROR24(c68k_aes_td[0][B3(s2)]) ^ rk[1];
        t2 = c68k_aes_td[0][B0(s2)] ^ ROR8(c68k_aes_td[0][B1(s1)]) ^
             ROR16(c68k_aes_td[0][B2(s0)]) ^ ROR24(c68k_aes_td[0][B3(s3)]) ^ rk[2];
        t3 = c68k_aes_td[0][B0(s3)] ^ ROR8(c68k_aes_td[0][B1(s2)]) ^
             ROR16(c68k_aes_td[0][B2(s1)]) ^ ROR24(c68k_aes_td[0][B3(s0)]) ^ rk[3];

        s0 = t0;
        s1 = t1;
        s2 = t2;
        s3 = t3;
        rk = &rk[4];
    }

    st[0] = ((((ULONG)c68k_aes_isbox[B0(s0)]) << 24) |
            (((ULONG)c68k_aes_isbox[B1(s3)]) << 16) |
            (((ULONG)c68k_aes_isbox[B2(s2)]) << 8) |
            ((ULONG)c68k_aes_isbox[B3(s1)])) ^ rk[0];
    st[1] = ((((ULONG)c68k_aes_isbox[B0(s1)]) << 24) |
            (((ULONG)c68k_aes_isbox[B1(s0)]) << 16) |
            (((ULONG)c68k_aes_isbox[B2(s3)]) << 8) |
            ((ULONG)c68k_aes_isbox[B3(s2)])) ^ rk[1];
    st[2] = ((((ULONG)c68k_aes_isbox[B0(s2)]) << 24) |
            (((ULONG)c68k_aes_isbox[B1(s1)]) << 16) |
            (((ULONG)c68k_aes_isbox[B2(s0)]) << 8) |
            ((ULONG)c68k_aes_isbox[B3(s3)])) ^ rk[2];
    st[3] = ((((ULONG)c68k_aes_isbox[B0(s3)]) << 24) |
            (((ULONG)c68k_aes_isbox[B1(s2)]) << 16) |
            (((ULONG)c68k_aes_isbox[B2(s1)]) << 8) |
            ((ULONG)c68k_aes_isbox[B3(s0)])) ^ rk[3];
}


/* ============================================================= SBOX, C ==== */
/*
 * A 256-byte S-box and MixColumns computed in the ALU, word-parallel:
 *
 *   u = w ^ rotl8(w)                 per byte: a^b, b^c, c^d, d^a
 *   t = u ^ rotl16(u)                per byte: a^b^c^d
 *   out = w ^ t ^ xtime(u)           the AES MixColumns identity
 *
 * The reason to try it on this machine is the bus: four byte reads per column
 * instead of four longword reads is half the traffic on a 16-bit path.  The
 * reason it does not win is that the traffic was never the binding constraint
 * -- see c68k_aes.h.
 */

#define ROL8(v)     (((v) << 8) | ((v) >> 24))
#define ROL16(v)    (((v) << 16) | ((v) >> 16))

static ULONG c68k_aes_xtime32(ULONG w)
{

ULONG   m;


    m = (w & 0x80808080uL) >> 7;

    /* m is 0 or 1 in each byte and 0x1B is under 0x20, so the four shifted
       copies never carry between bytes and no multiply is needed. */
    return((((w & 0x7F7F7F7FuL) << 1) ^
            (m ^ (m << 1) ^ (m << 3) ^ (m << 4))) & 0xFFFFFFFFuL);
}

static ULONG c68k_aes_mixcolumn(ULONG w)
{

ULONG   u;
ULONG   t;


    u = w ^ ROL8(w);
    t = u ^ ROL16(u);

    return(w ^ t ^ c68k_aes_xtime32(u));
}

static VOID c68k_aes_enc_sbox(const ULONG *rk, UINT nr, ULONG *st)
{

ULONG   s0, s1, s2, s3;
ULONG   t0, t1, t2, t3;
UINT    r;


    s0 = st[0] ^ rk[0];
    s1 = st[1] ^ rk[1];
    s2 = st[2] ^ rk[2];
    s3 = st[3] ^ rk[3];
    rk = &rk[4];

    for (r = 1u; r < nr; r++)
    {
        t0 = (((ULONG)c68k_aes_sbox[B0(s0)]) << 24) |
             (((ULONG)c68k_aes_sbox[B1(s1)]) << 16) |
             (((ULONG)c68k_aes_sbox[B2(s2)]) << 8) |
             ((ULONG)c68k_aes_sbox[B3(s3)]);
        t1 = (((ULONG)c68k_aes_sbox[B0(s1)]) << 24) |
             (((ULONG)c68k_aes_sbox[B1(s2)]) << 16) |
             (((ULONG)c68k_aes_sbox[B2(s3)]) << 8) |
             ((ULONG)c68k_aes_sbox[B3(s0)]);
        t2 = (((ULONG)c68k_aes_sbox[B0(s2)]) << 24) |
             (((ULONG)c68k_aes_sbox[B1(s3)]) << 16) |
             (((ULONG)c68k_aes_sbox[B2(s0)]) << 8) |
             ((ULONG)c68k_aes_sbox[B3(s1)]);
        t3 = (((ULONG)c68k_aes_sbox[B0(s3)]) << 24) |
             (((ULONG)c68k_aes_sbox[B1(s0)]) << 16) |
             (((ULONG)c68k_aes_sbox[B2(s1)]) << 8) |
             ((ULONG)c68k_aes_sbox[B3(s2)]);

        s0 = c68k_aes_mixcolumn(t0) ^ rk[0];
        s1 = c68k_aes_mixcolumn(t1) ^ rk[1];
        s2 = c68k_aes_mixcolumn(t2) ^ rk[2];
        s3 = c68k_aes_mixcolumn(t3) ^ rk[3];
        rk = &rk[4];
    }

    st[0] = ((((ULONG)c68k_aes_sbox[B0(s0)]) << 24) |
            (((ULONG)c68k_aes_sbox[B1(s1)]) << 16) |
            (((ULONG)c68k_aes_sbox[B2(s2)]) << 8) |
            ((ULONG)c68k_aes_sbox[B3(s3)])) ^ rk[0];
    st[1] = ((((ULONG)c68k_aes_sbox[B0(s1)]) << 24) |
            (((ULONG)c68k_aes_sbox[B1(s2)]) << 16) |
            (((ULONG)c68k_aes_sbox[B2(s3)]) << 8) |
            ((ULONG)c68k_aes_sbox[B3(s0)])) ^ rk[1];
    st[2] = ((((ULONG)c68k_aes_sbox[B0(s2)]) << 24) |
            (((ULONG)c68k_aes_sbox[B1(s3)]) << 16) |
            (((ULONG)c68k_aes_sbox[B2(s0)]) << 8) |
            ((ULONG)c68k_aes_sbox[B3(s1)])) ^ rk[2];
    st[3] = ((((ULONG)c68k_aes_sbox[B0(s3)]) << 24) |
            (((ULONG)c68k_aes_sbox[B1(s0)]) << 16) |
            (((ULONG)c68k_aes_sbox[B2(s1)]) << 8) |
            ((ULONG)c68k_aes_sbox[B3(s2)])) ^ rk[3];
}

/*
 * Decryption the same way.  The equivalent inverse cipher's InvMixColumns is
 * three MixColumns-shaped applications of xtime, which is why this one is
 * visibly worse than its encryption twin and why nobody writes it: it is here
 * so the SBOX row of the comparison covers both directions rather than half.
 */
static ULONG c68k_aes_invmixcolumn(ULONG w)
{

ULONG   u;
ULONG   v;


    /* InvMixColumns = MixColumns composed with the multiplication by
       (04 00 04 00), which word-parallel is one doubling twice over. */
    u = c68k_aes_xtime32(c68k_aes_xtime32(w ^ ROL16(w)));
    v = w ^ u;

    return(c68k_aes_mixcolumn(v));
}

static VOID c68k_aes_dec_sbox(const ULONG *rk, UINT nr, ULONG *st)
{

ULONG   s0, s1, s2, s3;
ULONG   t0, t1, t2, t3;
UINT    r;


    s0 = st[0] ^ rk[0];
    s1 = st[1] ^ rk[1];
    s2 = st[2] ^ rk[2];
    s3 = st[3] ^ rk[3];
    rk = &rk[4];

    for (r = 1u; r < nr; r++)
    {
        t0 = (((ULONG)c68k_aes_isbox[B0(s0)]) << 24) |
             (((ULONG)c68k_aes_isbox[B1(s3)]) << 16) |
             (((ULONG)c68k_aes_isbox[B2(s2)]) << 8) |
             ((ULONG)c68k_aes_isbox[B3(s1)]);
        t1 = (((ULONG)c68k_aes_isbox[B0(s1)]) << 24) |
             (((ULONG)c68k_aes_isbox[B1(s0)]) << 16) |
             (((ULONG)c68k_aes_isbox[B2(s3)]) << 8) |
             ((ULONG)c68k_aes_isbox[B3(s2)]);
        t2 = (((ULONG)c68k_aes_isbox[B0(s2)]) << 24) |
             (((ULONG)c68k_aes_isbox[B1(s1)]) << 16) |
             (((ULONG)c68k_aes_isbox[B2(s0)]) << 8) |
             ((ULONG)c68k_aes_isbox[B3(s3)]);
        t3 = (((ULONG)c68k_aes_isbox[B0(s3)]) << 24) |
             (((ULONG)c68k_aes_isbox[B1(s2)]) << 16) |
             (((ULONG)c68k_aes_isbox[B2(s1)]) << 8) |
             ((ULONG)c68k_aes_isbox[B3(s0)]);

        s0 = c68k_aes_invmixcolumn(t0) ^ rk[0];
        s1 = c68k_aes_invmixcolumn(t1) ^ rk[1];
        s2 = c68k_aes_invmixcolumn(t2) ^ rk[2];
        s3 = c68k_aes_invmixcolumn(t3) ^ rk[3];
        rk = &rk[4];
    }

    st[0] = ((((ULONG)c68k_aes_isbox[B0(s0)]) << 24) |
            (((ULONG)c68k_aes_isbox[B1(s3)]) << 16) |
            (((ULONG)c68k_aes_isbox[B2(s2)]) << 8) |
            ((ULONG)c68k_aes_isbox[B3(s1)])) ^ rk[0];
    st[1] = ((((ULONG)c68k_aes_isbox[B0(s1)]) << 24) |
            (((ULONG)c68k_aes_isbox[B1(s0)]) << 16) |
            (((ULONG)c68k_aes_isbox[B2(s3)]) << 8) |
            ((ULONG)c68k_aes_isbox[B3(s2)])) ^ rk[1];
    st[2] = ((((ULONG)c68k_aes_isbox[B0(s2)]) << 24) |
            (((ULONG)c68k_aes_isbox[B1(s1)]) << 16) |
            (((ULONG)c68k_aes_isbox[B2(s0)]) << 8) |
            ((ULONG)c68k_aes_isbox[B3(s3)])) ^ rk[2];
    st[3] = ((((ULONG)c68k_aes_isbox[B0(s3)]) << 24) |
            (((ULONG)c68k_aes_isbox[B1(s2)]) << 16) |
            (((ULONG)c68k_aes_isbox[B2(s1)]) << 8) |
            ((ULONG)c68k_aes_isbox[B3(s0)])) ^ rk[3];
}



/* ====================================================== the entry points == */

/*
 * The assembly, when this build has it.  Same shape as the C round cores --
 * (round keys, rounds, four-longword state in memory) -- so the dispatch is a
 * switch and not two code paths.
 *
 * THE STATE LIVES IN MEMORY ON PURPOSE, and it is the one design decision in
 * the assembly that is not obvious.  The 68020 has eight data registers; a
 * round needs four state words, four accumulators, an index and a temporary,
 * which is ten.  Something has to give, and the choice is between extracting
 * the sixteen index bytes from registers -- MOVE.B plus a ROL.L each, because
 * only the low byte of a register can be moved out -- and reading them
 * straight out of a sixteen-byte buffer with MOVE.B d16(An),Dn, one
 * instruction and no rotate.  The second is fewer instructions AND leaves
 * four registers free to hold the four accumulators, so the round ends with a
 * single MOVEM.L of all four.  On a part with a data cache the sixteen byte
 * reads would be free; here they are bus cycles, and they are still cheaper
 * than the rotates they replace.
 */
#ifdef C68K_ASM
extern VOID c68k_aes_core_enc_t4_asm(const ULONG *rk, UINT nr, ULONG *st);
extern VOID c68k_aes_core_dec_t4_asm(const ULONG *rk, UINT nr, ULONG *st);
extern VOID c68k_aes_core_enc_t1_asm(const ULONG *rk, UINT nr, ULONG *st);
extern VOID c68k_aes_core_dec_t1_asm(const ULONG *rk, UINT nr, ULONG *st);
#endif

static VOID c68k_aes_enc_dispatch(const ULONG *rk, UINT nr, ULONG *st)
{

    switch (c68k_aes_variant)
    {
#ifdef C68K_ASM
    case C68K_AES_V_T4_ASM:
        c68k_aes_core_enc_t4_asm(rk, nr, st);
        break;

    case C68K_AES_V_T1_ASM:
        c68k_aes_core_enc_t1_asm(rk, nr, st);
        break;
#endif

    case C68K_AES_V_T1_C:
        c68k_aes_enc_t1(rk, nr, st);
        break;

    case C68K_AES_V_SBOX_C:
        c68k_aes_enc_sbox(rk, nr, st);
        break;

    default:
        c68k_aes_enc_t4(rk, nr, st);
        break;
    }
}

static VOID c68k_aes_dec_dispatch(const ULONG *rk, UINT nr, ULONG *st)
{

    switch (c68k_aes_variant)
    {
#ifdef C68K_ASM
    case C68K_AES_V_T4_ASM:
        c68k_aes_core_dec_t4_asm(rk, nr, st);
        break;

    case C68K_AES_V_T1_ASM:
        c68k_aes_core_dec_t1_asm(rk, nr, st);
        break;
#endif

    case C68K_AES_V_T1_C:
        c68k_aes_dec_t1(rk, nr, st);
        break;

    case C68K_AES_V_SBOX_C:
        c68k_aes_dec_sbox(rk, nr, st);
        break;

    default:
        c68k_aes_dec_t4(rk, nr, st);
        break;
    }
}

VOID c68k_aes_encrypt_block(const C68K_AES *aes, const UCHAR *in, UCHAR *out)
{

ULONG   st[4];


    st[0] = c68k_aes_load_be(&in[0]);
    st[1] = c68k_aes_load_be(&in[4]);
    st[2] = c68k_aes_load_be(&in[8]);
    st[3] = c68k_aes_load_be(&in[12]);

    c68k_aes_enc_dispatch(aes -> c68k_aes_ek, aes -> c68k_aes_rounds, st);

    c68k_aes_store_be(&out[0], st[0]);
    c68k_aes_store_be(&out[4], st[1]);
    c68k_aes_store_be(&out[8], st[2]);
    c68k_aes_store_be(&out[12], st[3]);
}

VOID c68k_aes_decrypt_block(const C68K_AES *aes, const UCHAR *in, UCHAR *out)
{

ULONG   st[4];


    st[0] = c68k_aes_load_be(&in[0]);
    st[1] = c68k_aes_load_be(&in[4]);
    st[2] = c68k_aes_load_be(&in[8]);
    st[3] = c68k_aes_load_be(&in[12]);

    c68k_aes_dec_dispatch(aes -> c68k_aes_dk, aes -> c68k_aes_rounds, st);

    c68k_aes_store_be(&out[0], st[0]);
    c68k_aes_store_be(&out[4], st[1]);
    c68k_aes_store_be(&out[8], st[2]);
    c68k_aes_store_be(&out[12], st[3]);
}

/*
 * CBC.  The chaining value stays in st[] across blocks rather than going back
 * to the IV buffer, which is the whole reason these are here and not a loop
 * around c68k_aes_encrypt_block(); nx_crypto_cbc.c does the latter, through a
 * function pointer, with a separate XOR pass over the block.
 *
 * The byte-at-a-time loads are deliberate and are what a portable fallback
 * costs: `in` may be on any address, because a TLS record's payload starts 21
 * bytes into the packet buffer.  The fused assembly path below uses MOVE.L
 * regardless, since the 68020 does misaligned longword accesses in hardware.
 */
VOID c68k_aes_cbc_encrypt(const C68K_AES *aes, UCHAR *iv,
                          const UCHAR *in, UCHAR *out, ULONG blocks)
{

ULONG   st[4];
ULONG   i;


    st[0] = c68k_aes_load_be(&iv[0]);
    st[1] = c68k_aes_load_be(&iv[4]);
    st[2] = c68k_aes_load_be(&iv[8]);
    st[3] = c68k_aes_load_be(&iv[12]);

    for (i = 0; i < blocks; i++)
    {
        st[0] = st[0] ^ c68k_aes_load_be(&in[0]);
        st[1] = st[1] ^ c68k_aes_load_be(&in[4]);
        st[2] = st[2] ^ c68k_aes_load_be(&in[8]);
        st[3] = st[3] ^ c68k_aes_load_be(&in[12]);

        c68k_aes_enc_dispatch(aes -> c68k_aes_ek, aes -> c68k_aes_rounds, st);

        c68k_aes_store_be(&out[0], st[0]);
        c68k_aes_store_be(&out[4], st[1]);
        c68k_aes_store_be(&out[8], st[2]);
        c68k_aes_store_be(&out[12], st[3]);

        in  = &in[16];
        out = &out[16];
    }

    if (blocks != 0uL)
    {
        c68k_aes_store_be(&iv[0], st[0]);
        c68k_aes_store_be(&iv[4], st[1]);
        c68k_aes_store_be(&iv[8], st[2]);
        c68k_aes_store_be(&iv[12], st[3]);
    }
}

VOID c68k_aes_cbc_decrypt(const C68K_AES *aes, UCHAR *iv,
                          const UCHAR *in, UCHAR *out, ULONG blocks)
{

ULONG   st[4];
ULONG   p0, p1, p2, p3;
ULONG   c0, c1, c2, c3;
ULONG   i;


    p0 = c68k_aes_load_be(&iv[0]);
    p1 = c68k_aes_load_be(&iv[4]);
    p2 = c68k_aes_load_be(&iv[8]);
    p3 = c68k_aes_load_be(&iv[12]);

    for (i = 0; i < blocks; i++)
    {
        c0 = c68k_aes_load_be(&in[0]);
        c1 = c68k_aes_load_be(&in[4]);
        c2 = c68k_aes_load_be(&in[8]);
        c3 = c68k_aes_load_be(&in[12]);

        st[0] = c0;
        st[1] = c1;
        st[2] = c2;
        st[3] = c3;

        c68k_aes_dec_dispatch(aes -> c68k_aes_dk, aes -> c68k_aes_rounds, st);

        c68k_aes_store_be(&out[0], st[0] ^ p0);
        c68k_aes_store_be(&out[4], st[1] ^ p1);
        c68k_aes_store_be(&out[8], st[2] ^ p2);
        c68k_aes_store_be(&out[12], st[3] ^ p3);

        p0 = c0;
        p1 = c1;
        p2 = c2;
        p3 = c3;

        in  = &in[16];
        out = &out[16];
    }

    if (blocks != 0uL)
    {
        c68k_aes_store_be(&iv[0], p0);
        c68k_aes_store_be(&iv[4], p1);
        c68k_aes_store_be(&iv[8], p2);
        c68k_aes_store_be(&iv[12], p3);
    }
}

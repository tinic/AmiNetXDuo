/*
 * AmiNetXDuo, the TLS record path, measured: AES-128-CBC and SHA-256.
 *
 *   docs/RESEARCH.md 15 left the bulk path as the one row where neither this
 *   tree nor AmiSSL had any m68k assembly, and named it the largest remaining
 *   lever on `https://` throughput.  Before writing any, two things had to be
 *   settled on the machine rather than from the literature:
 *
 *     1. Which AES suits a part with no data cache.  The classic four-table
 *        layout, the one-table-and-rotates layout that nx_crypto_aes.c uses,
 *        and a byte-oriented S-box are three different answers to "which
 *        resource is scarce", and the usual reasoning assumes a cache this
 *        machine does not have.
 *     2. Where the instruction cache knee falls.  It is 256 bytes here, so
 *        unrolling can and does make things slower, and the turning point has
 *        to be measured before an assembly round is written.
 *
 *   So this program is three things in one run: instruction-cost kernels,
 *   every AES and SHA-256 variant timed on the same buffer, and every one of
 *   them checked against the published vectors and against each other before
 *   a single time is believed.
 *
 *   As in c68k_amissl_bench.c, ratios measured back to back inside one run are
 *   the only figures an emulator does not distort, and the only difference
 *   between two rows here is the code that ran.
 *
 * SPDX-License-Identifier: MIT
 */

#include "c68k_support.h"
#include "c68k_timer.h"
#include "c68k_aes.h"
#include "c68k_sha256.h"
#include "c68k_chacha20.h"

#include "aminetxduo/crashguard.h"

#include "nx_crypto_aes.h"
#include "nx_crypto_cbc.h"
#include "nx_crypto_sha2.h"
#include "nx_crypto_hmac.h"

#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>


/* 16 KiB is one TLS record's worth of payload and then some, RFC 5246 caps
   a plaintext fragment at 2^14, so it is the unit the bulk path moves. */
#define B_BULK_BYTES        16384UL
#define B_BULK_BLOCKS       (B_BULK_BYTES / 16UL)

static ULONG    b_failures;

static UCHAR    b_plain[B_BULK_BYTES];
static UCHAR    b_cipher[B_BULK_BYTES];
static UCHAR    b_back[B_BULK_BYTES];
static UCHAR    b_ref[B_BULK_BYTES];

static UCHAR    b_key[32];
static UCHAR    b_iv[16];
static UCHAR    b_iv_work[16];

static C68K_AES b_aes;

static UCHAR    b_digest[32];
static UCHAR    b_digest_ref[32];

static NX_CRYPTO_AES        b_nx_aes;
static NX_CRYPTO_CBC        b_nx_cbc;
static NX_CRYPTO_SHA256     b_nx_sha;
static NX_CRYPTO_HMAC       b_nx_hmac;
static NX_CRYPTO_SHA256     b_nx_hmac_ctx;
static NX_CRYPTO_HMAC       b_our_hmac;
static C68K_SHA256          b_our_hmac_ctx;


/* --------------------------------------------------------------- report -- */

static VOID b_fail(const char *what)
{

    c68k_log("  FAIL: %s", (LONG)what);
    b_failures++;
}

static VOID b_check(const char *what, const UCHAR *a, const UCHAR *b, ULONG n)
{

ULONG   i;


    for (i = 0; i < n; i++)
    {
        if (a[i] != b[i])
        {
            c68k_log("  MISMATCH %s at byte %lu: %02lx vs %02lx",
                     (LONG)what, i, (ULONG)a[i], (ULONG)b[i]);
            b_failures++;
            return;
        }
    }
}

/* Bytes per second, in KiB/s, from a microsecond figure for B_BULK_BYTES. */
static ULONG b_kbs(ULONG micros)
{

    if (micros == 0uL)
    {
        return(0uL);
    }

    return(((B_BULK_BYTES * 1000uL) / micros) * 1000uL / 1024uL);
}


/* ================================================= instruction kernels ==== */

extern VOID bk_empty(ULONG reps);
extern VOID bk_add(ULONG reps);
extern VOID bk_eor(ULONG reps);
extern VOID bk_and(ULONG reps);
extern VOID bk_moveb_dd(ULONG reps);
extern VOID bk_swapd(ULONG reps);
extern VOID bk_ror1(ULONG reps);
extern VOID bk_ror8(ULONG reps);
extern VOID bk_rorreg(ULONG reps);
extern VOID bk_lsr3(ULONG reps);
extern VOID bk_addself(ULONG reps);
extern VOID bk_exg_da(ULONG reps);
extern VOID bk_movel_ad(ULONG reps);
extern VOID bk_movel_da(ULONG reps);
extern VOID bk_addx(ULONG reps);
extern VOID bk_andi(ULONG reps);
extern VOID bk_rol6(ULONG reps);
extern VOID bk_mulu_dd(APTR table, ULONG reps);
extern VOID bk_mulu_an(APTR table, ULONG reps);
extern VOID bk_mulu_d16(APTR table, ULONG reps);
extern VOID bk_idx1k(APTR table, ULONG reps);
extern VOID bk_idx4k(APTR table, ULONG reps);
extern VOID bk_idxb(APTR table, ULONG reps);
extern VOID bk_movel_d16(APTR table, ULONG reps);
extern VOID bk_moveb_d16(APTR table, ULONG reps);
extern VOID bk_movel_store(APTR table, ULONG reps);
extern VOID bk_ic32(ULONG reps);
extern VOID bk_ic128(ULONG reps);
extern VOID bk_ic256(ULONG reps);
extern VOID bk_ic512(ULONG reps);
extern VOID bk_ic1024(ULONG reps);
extern VOID bk_ic2048(ULONG reps);

#define B_KERNEL_REPS   40000UL
#define B_KERNEL_SLOTS  16UL

static ULONG    b_empty_ps;             /* loop overhead, ps per body slot */
static ULONG    b_add_ps;               /* one ADD.L, the yardstick        */
static APTR     b_table;                /* 4 KB of Fast RAM                */

typedef VOID (*B_REG_KERNEL)(ULONG);
typedef VOID (*B_MEM_KERNEL)(APTR, ULONG);

/*
 * Picoseconds per body slot.
 *
 * The first version of this was wrong in a way that has caught this project
 * twice (see the note above c_measure_ps() in tests/perf/cpucal.c): ULONG is
 * 32 bits, a 23 ms kernel is 23,000,000 nanoseconds, and multiplying that by
 * the further thousand picoseconds want overflows and prints a rotate as
 * costing nothing.  So the scaling divides the slot count by a thousand
 * instead, and the rep count is a round 40,000 to keep that exact.
 */
static ULONG b_scale(ULONG micros, ULONG slots)
{

    if (slots < 1000uL)
    {
        return(0uL);
    }

    return((micros * 1000uL) / (slots / 1000uL));
}

static ULONG b_time_reg(B_REG_KERNEL fn, ULONG reps)
{

ULONG   start;
ULONG   us;


    start = c68k_eclock();
    fn(reps);
    us = c68k_eclock_micros(c68k_eclock() - start);

    return(b_scale(us, reps * B_KERNEL_SLOTS));
}

static ULONG b_time_mem(B_MEM_KERNEL fn, ULONG reps)
{

ULONG   start;
ULONG   us;


    start = c68k_eclock();
    fn(b_table, reps);
    us = c68k_eclock_micros(c68k_eclock() - start);

    return(b_scale(us, reps * B_KERNEL_SLOTS));
}

static VOID b_report_ps(const char *what, ULONG raw)
{

ULONG   ps;
ULONG   cycles_x100;


    ps = (raw > b_empty_ps) ? (raw - b_empty_ps) : 0uL;

    if (b_add_ps == 0uL)
    {
        b_add_ps = ps;
    }

    cycles_x100 = (b_add_ps != 0uL) ? ((ps * 200uL) / b_add_ps) : 0uL;

    c68k_log("  %s %lu.%03lu ns   implied %lu.%02lu cycles",
             (LONG)what, ps / 1000uL, ps % 1000uL,
             cycles_x100 / 100uL, cycles_x100 % 100uL);
}

/*
 * The instruction-cache sweep reports per ADD.L for a straight-line body of
 * the named size.  The loop overhead is spread over a different number of
 * slots in each row, so it is divided out here rather than subtracted.
 */
static VOID b_report_ic(const char *what, B_REG_KERNEL fn, ULONG body_bytes)
{

ULONG   start;
ULONG   us;
ULONG   reps;
ULONG   slots;
ULONG   ps;


    slots = body_bytes / 2uL;
    reps  = (2048uL * 1024uL) / body_bytes;     /* ~1 M instructions a row */

    start = c68k_eclock();
    fn(reps);
    us = c68k_eclock_micros(c68k_eclock() - start);

    ps = b_scale(us, reps * slots);

    c68k_log("  %s %lu.%03lu ns per ADD.L", (LONG)what, ps / 1000uL,
             ps % 1000uL);
}

static VOID b_bench_kernels(VOID)
{

    c68k_log("");
    c68k_log("1. What the instructions cost here");

    b_table = AllocMem(4096uL, MEMF_FAST | MEMF_CLEAR);
    if (b_table == NULL)
    {
        b_table = AllocMem(4096uL, MEMF_ANY | MEMF_CLEAR);
        c68k_log("  (no Fast RAM for the table -- the memory rows below are "
                 "whatever AllocMem gave us)");
    }

    b_empty_ps = b_time_reg(bk_empty, B_KERNEL_REPS);
    b_add_ps   = 0uL;

    b_report_ps("ADD.L  Dn,Dm            ", b_time_reg(bk_add, B_KERNEL_REPS));
    b_report_ps("EOR.L  Dn,Dm            ", b_time_reg(bk_eor, B_KERNEL_REPS));
    b_report_ps("AND.L  Dn,Dm            ", b_time_reg(bk_and, B_KERNEL_REPS));
    b_report_ps("MOVE.B Dn,Dm            ", b_time_reg(bk_moveb_dd, B_KERNEL_REPS));
    b_report_ps("ADD.L  Dn,Dn  (shift 1) ", b_time_reg(bk_addself, B_KERNEL_REPS));
    b_report_ps("SWAP   Dn               ", b_time_reg(bk_swapd, B_KERNEL_REPS));
    b_report_ps("ROR.L  #1,Dn            ", b_time_reg(bk_ror1, B_KERNEL_REPS));
    b_report_ps("ROR.L  #8,Dn            ", b_time_reg(bk_ror8, B_KERNEL_REPS));
    b_report_ps("ROR.L  Dm,Dn  (count 13)", b_time_reg(bk_rorreg, B_KERNEL_REPS));
    b_report_ps("LSR.L  #3,Dn            ", b_time_reg(bk_lsr3, B_KERNEL_REPS));
    b_report_ps("EXG    Dn,An            ", b_time_reg(bk_exg_da, B_KERNEL_REPS));
    b_report_ps("MOVE.L An,Dn            ", b_time_reg(bk_movel_ad, B_KERNEL_REPS));
    b_report_ps("MOVEA.L Dn,An           ", b_time_reg(bk_movel_da, B_KERNEL_REPS));
    b_report_ps("ADDX.L Dn,Dm            ", b_time_reg(bk_addx, B_KERNEL_REPS));
    b_report_ps("AND.L  #imm32,Dn        ", b_time_reg(bk_andi, B_KERNEL_REPS));
    b_report_ps("ROL.L  #6,Dn            ", b_time_reg(bk_rol6, B_KERNEL_REPS));

    if (b_table != NULL)
    {
        b_report_ps("MOVE.L (An,Dn.W*4),Dm   1 KB table",
                    b_time_mem(bk_idx1k, B_KERNEL_REPS));
        b_report_ps("MOVE.L (An,Dn.W*4),Dm   4 KB table",
                    b_time_mem(bk_idx4k, B_KERNEL_REPS));
        b_report_ps("MOVE.B (An,Dn.W),Dm     256 B table",
                    b_time_mem(bk_idxb, B_KERNEL_REPS));
        b_report_ps("MOVE.L d16(An),Dm       ",
                    b_time_mem(bk_movel_d16, B_KERNEL_REPS));
        b_report_ps("MOVE.B d16(An),Dm       ",
                    b_time_mem(bk_moveb_d16, B_KERNEL_REPS));
        b_report_ps("MOVE.L Dm,d16(An)       ",
                    b_time_mem(bk_movel_store, B_KERNEL_REPS));
        /* Poly1305's whole register allocation turns on these three: fourteen
           live values against fifteen registers only works if MULU.L takes
           its multiplier straight out of memory. */
        b_report_ps("MULU.L Dn,Dh:Dl         ",
                    b_time_mem(bk_mulu_dd, B_KERNEL_REPS / 4uL));
        b_report_ps("MULU.L (An),Dh:Dl       ",
                    b_time_mem(bk_mulu_an, B_KERNEL_REPS / 4uL));
        b_report_ps("MULU.L d16(An),Dh:Dl    ",
                    b_time_mem(bk_mulu_d16, B_KERNEL_REPS / 4uL));
    }

    c68k_log("");
    c68k_log("  the 256-byte instruction cache, as a straight-line body grows:");
    b_report_ic("     32 B body ", bk_ic32,   32uL);
    b_report_ic("    128 B body ", bk_ic128,  128uL);
    b_report_ic("    256 B body ", bk_ic256,  256uL);
    b_report_ic("    512 B body ", bk_ic512,  512uL);
    b_report_ic("   1024 B body ", bk_ic1024, 1024uL);
    b_report_ic("   2048 B body ", bk_ic2048, 2048uL);

    /* AmigaOS does not reclaim AllocMem() memory when a process exits, so a
       benchmark that leaves it costs 4 KB until reboot on every run.  Freed
       here rather than at exit: nothing outside this function reads it. */
    if (b_table != NULL)
    {
        FreeMem(b_table, 4096uL);
        b_table = NULL;
    }
}


/* ============================================================== vectors ==== */

/* FIPS-197 C.1, AES-128 with key 000102..0f. */
static const UCHAR b_fips_key128[16] =
{
    0x00u, 0x01u, 0x02u, 0x03u, 0x04u, 0x05u, 0x06u, 0x07u,
    0x08u, 0x09u, 0x0Au, 0x0Bu, 0x0Cu, 0x0Du, 0x0Eu, 0x0Fu
};
static const UCHAR b_fips_key256[32] =
{
    0x00u, 0x01u, 0x02u, 0x03u, 0x04u, 0x05u, 0x06u, 0x07u,
    0x08u, 0x09u, 0x0Au, 0x0Bu, 0x0Cu, 0x0Du, 0x0Eu, 0x0Fu,
    0x10u, 0x11u, 0x12u, 0x13u, 0x14u, 0x15u, 0x16u, 0x17u,
    0x18u, 0x19u, 0x1Au, 0x1Bu, 0x1Cu, 0x1Du, 0x1Eu, 0x1Fu
};
static const UCHAR b_fips_plain[16] =
{
    0x00u, 0x11u, 0x22u, 0x33u, 0x44u, 0x55u, 0x66u, 0x77u,
    0x88u, 0x99u, 0xAAu, 0xBBu, 0xCCu, 0xDDu, 0xEEu, 0xFFu
};
static const UCHAR b_fips_c128[16] =
{
    0x69u, 0xC4u, 0xE0u, 0xD8u, 0x6Au, 0x7Bu, 0x04u, 0x30u,
    0xD8u, 0xCDu, 0xB7u, 0x80u, 0x70u, 0xB4u, 0xC5u, 0x5Au
};
static const UCHAR b_fips_c256[16] =
{
    0x8Eu, 0xA2u, 0xB7u, 0xCAu, 0x51u, 0x67u, 0x45u, 0xBFu,
    0xEAu, 0xFCu, 0x49u, 0x90u, 0x4Bu, 0x49u, 0x60u, 0x89u
};

/* FIPS 180-4, SHA-256("abc") and the 56-byte message. */
static const UCHAR b_sha_abc[32] =
{
    0xBAu, 0x78u, 0x16u, 0xBFu, 0x8Fu, 0x01u, 0xCFu, 0xEAu,
    0x41u, 0x41u, 0x40u, 0xDEu, 0x5Du, 0xAEu, 0x22u, 0x23u,
    0xB0u, 0x03u, 0x61u, 0xA3u, 0x96u, 0x17u, 0x7Au, 0x9Cu,
    0xB4u, 0x10u, 0xFFu, 0x61u, 0xF2u, 0x00u, 0x15u, 0xADu
};
static const UCHAR b_sha_empty[32] =
{
    0xE3u, 0xB0u, 0xC4u, 0x42u, 0x98u, 0xFCu, 0x1Cu, 0x14u,
    0x9Au, 0xFBu, 0xF4u, 0xC8u, 0x99u, 0x6Fu, 0xB9u, 0x24u,
    0x27u, 0xAEu, 0x41u, 0xE4u, 0x64u, 0x9Bu, 0x93u, 0x4Cu,
    0xA4u, 0x95u, 0x99u, 0x1Bu, 0x78u, 0x52u, 0xB8u, 0x55u
};
static const UCHAR b_sha_448[32] =
{
    0x24u, 0x8Du, 0x6Au, 0x61u, 0xD2u, 0x06u, 0x38u, 0xB8u,
    0xE5u, 0xC0u, 0x26u, 0x93u, 0x0Cu, 0x3Eu, 0x60u, 0x39u,
    0xA3u, 0x3Cu, 0xE4u, 0x59u, 0x64u, 0xFFu, 0x21u, 0x67u,
    0xF6u, 0xECu, 0xEDu, 0xD4u, 0x19u, 0xDBu, 0x06u, 0xC1u
};
/* FIPS 180-4, one million 'a', the case that exercises the length counter
   and every buffering path there is. */
static const UCHAR b_sha_million[32] =
{
    0xCDu, 0xC7u, 0x6Eu, 0x5Cu, 0x99u, 0x14u, 0xFBu, 0x92u,
    0x81u, 0xA1u, 0xC7u, 0xE2u, 0x84u, 0xD7u, 0x3Eu, 0x67u,
    0xF1u, 0x80u, 0x9Au, 0x48u, 0xA4u, 0x97u, 0x20u, 0x0Eu,
    0x04u, 0x6Du, 0x39u, 0xCCu, 0xC7u, 0x11u, 0x2Cu, 0xD0u
};

/* RFC 4231 test case 2: key "Jefe", data "what do ya want for nothing?". */
static const UCHAR b_hmac_rfc4231_2[32] =
{
    0x5Bu, 0xDCu, 0xC1u, 0x46u, 0xBFu, 0x60u, 0x75u, 0x4Eu,
    0x6Au, 0x04u, 0x24u, 0x26u, 0x08u, 0x95u, 0x75u, 0xC7u,
    0x5Au, 0x00u, 0x3Fu, 0x08u, 0x9Du, 0x27u, 0x39u, 0x83u,
    0x9Du, 0xECu, 0x58u, 0xB9u, 0x64u, 0xECu, 0x38u, 0x43u
};


/* ================================================================== AES ==== */

static UINT b_aes_check_vectors(VOID)
{

UCHAR   out[16];
UCHAR   back[16];
UINT    bad;


    bad = 0u;

    if (c68k_aes_key_set(&b_aes, b_fips_key128, 128u) != NX_CRYPTO_SUCCESS)
    {
        b_fail("AES-128 key schedule");
        return(1u);
    }

    c68k_aes_encrypt_block(&b_aes, b_fips_plain, out);
    b_check("FIPS-197 AES-128 ciphertext", out, b_fips_c128, 16uL);

    c68k_aes_decrypt_block(&b_aes, b_fips_c128, back);
    b_check("FIPS-197 AES-128 plaintext", back, b_fips_plain, 16uL);

    if (c68k_aes_key_set(&b_aes, b_fips_key256, 256u) != NX_CRYPTO_SUCCESS)
    {
        b_fail("AES-256 key schedule");
        return(1u);
    }

    c68k_aes_encrypt_block(&b_aes, b_fips_plain, out);
    b_check("FIPS-197 AES-256 ciphertext", out, b_fips_c256, 16uL);

    c68k_aes_decrypt_block(&b_aes, b_fips_c256, back);
    b_check("FIPS-197 AES-256 plaintext", back, b_fips_plain, 16uL);

    return(bad);
}

/*
 * The awkward shapes.  Every one of these has been a bug in somebody's CBC:
 * a zero-length call that walks off the end, a single block that never enters
 * the loop, a chaining value that is not carried between calls, and an
 * in-place transform that reads a byte it has already overwritten.
 */
static VOID b_aes_check_shapes(VOID)
{

UCHAR   iv_a[16];
UCHAR   iv_b[16];
UCHAR   one[16];
UCHAR   two[32];
UCHAR   ref[64];
UCHAR   got[64];
UINT    i;


    (VOID)c68k_aes_key_set(&b_aes, b_key, 128u);

    for (i = 0; i < 16u; i++)
    {
        iv_a[i] = b_iv[i];
        iv_b[i] = b_iv[i];
    }

    /* Zero blocks must touch nothing and leave the IV alone. */
    one[0] = 0x5Au;
    c68k_aes_cbc_encrypt(&b_aes, iv_a, b_plain, one, 0uL);
    if (one[0] != 0x5Au)
    {
        b_fail("CBC encrypt with 0 blocks wrote to the output");
    }
    b_check("CBC encrypt with 0 blocks moved the IV", iv_a, b_iv, 16uL);

    /* Four blocks in one call against four blocks in four calls, which is what
       an IV read and written in place has to survive. */
    c68k_aes_cbc_encrypt(&b_aes, iv_a, b_plain, ref, 4uL);

    for (i = 0; i < 4u; i++)
    {
        c68k_aes_cbc_encrypt(&b_aes, iv_b, &b_plain[i << 4], &got[i << 4], 1uL);
    }
    b_check("CBC chaining across calls", got, ref, 64uL);
    b_check("CBC chaining leaves the same IV", iv_a, iv_b, 16uL);

    /* Decrypt in place, which is what a record layer wants to do. */
    for (i = 0; i < 64u; i++)
    {
        got[i] = ref[i];
    }
    for (i = 0; i < 16u; i++)
    {
        iv_a[i] = b_iv[i];
    }
    c68k_aes_cbc_decrypt(&b_aes, iv_a, got, got, 4uL);
    b_check("CBC decrypt in place", got, b_plain, 64uL);

    /* One block, both directions. */
    for (i = 0; i < 16u; i++)
    {
        iv_a[i] = b_iv[i];
        iv_b[i] = b_iv[i];
    }
    c68k_aes_cbc_encrypt(&b_aes, iv_a, b_plain, two, 1uL);
    c68k_aes_cbc_decrypt(&b_aes, iv_b, two, &two[16], 1uL);
    b_check("CBC single block round trip", &two[16], b_plain, 16uL);

    /* Misaligned input and output.  The 68020 does misaligned longword
       accesses in hardware and a TLS record's payload starts 21 bytes into
       the packet, so this is the normal case rather than an edge one. */
    for (i = 0; i < 16u; i++)
    {
        iv_a[i] = b_iv[i];
    }
    for (i = 0; i < 33u; i++)
    {
        got[i] = b_plain[i];
    }
    c68k_aes_cbc_encrypt(&b_aes, iv_a, &got[1], &got[1], 2uL);
    for (i = 0; i < 16u; i++)
    {
        iv_a[i] = b_iv[i];
    }
    c68k_aes_cbc_decrypt(&b_aes, iv_a, &got[1], &got[1], 2uL);
    b_check("CBC on an odd address", &got[1], &b_plain[1], 32uL);
}

static VOID b_bench_aes(VOID)
{

UINT    v;
UINT    i;
ULONG   start;
ULONG   enc_us;
ULONG   dec_us;
UINT    saved;


    c68k_log("");
    c68k_log("2. AES-128-CBC, %lu bytes a call, both directions",
             B_BULK_BYTES);
    c68k_log("   every row checked against the FIPS-197 vectors, against "
             "nx_crypto,");
    c68k_log("   and against every other row, before it is timed");

    saved = c68k_aes_variant;

    /* The reference: the vendored implementation, on the same buffer. */
    if (_nx_crypto_aes_key_set(&b_nx_aes, b_key,
                               NX_CRYPTO_AES_KEY_SIZE_128_BITS) !=
        NX_CRYPTO_SUCCESS)
    {
        b_fail("nx_crypto AES key schedule");
        return;
    }

    for (i = 0; i < 16u; i++)
    {
        b_iv_work[i] = b_iv[i];
    }

    start = c68k_eclock();
    (VOID)_nx_crypto_cbc_encrypt_init(&b_nx_cbc, b_iv_work, 16u);
    (VOID)_nx_crypto_cbc_encrypt(&b_nx_aes, &b_nx_cbc,
                                 (UINT (*)(VOID *, UCHAR *, UCHAR *, UINT))
                                     _nx_crypto_aes_encrypt,
                                 b_plain, b_ref, B_BULK_BYTES, 16u);
    enc_us = c68k_eclock_micros(c68k_eclock() - start);

    for (i = 0; i < 16u; i++)
    {
        b_iv_work[i] = b_iv[i];
    }

    start = c68k_eclock();
    (VOID)_nx_crypto_cbc_decrypt_init(&b_nx_cbc, b_iv_work, 16u);
    (VOID)_nx_crypto_cbc_decrypt(&b_nx_aes, &b_nx_cbc,
                                 (UINT (*)(VOID *, UCHAR *, UCHAR *, UINT))
                                     _nx_crypto_aes_decrypt,
                                 b_ref, b_back, B_BULK_BYTES, 16u);
    dec_us = c68k_eclock_micros(c68k_eclock() - start);

    b_check("nx_crypto CBC round trip", b_back, b_plain, B_BULK_BYTES);

    c68k_log("  %-44s enc %lu us (%lu KB/s)  dec %lu us (%lu KB/s)",
             (LONG)"nx_crypto, the implementation we ship today",
             enc_us, b_kbs(enc_us), dec_us, b_kbs(dec_us));

    for (v = 0; v < C68K_AES_V_COUNT; v++)
    {
        c68k_aes_variant = v;

        if ((v >= C68K_AES_V_T4_ASM) && (c68k_aes_variant_is_asm(v) == 0u))
        {
            continue;                   /* not in this build */
        }

        if (b_aes_check_vectors() != 0u)
        {
            continue;
        }
        b_aes_check_shapes();

        (VOID)c68k_aes_key_set(&b_aes, b_key, 128u);

        for (i = 0; i < 16u; i++)
        {
            b_iv_work[i] = b_iv[i];
        }
        start = c68k_eclock();
        c68k_aes_cbc_encrypt(&b_aes, b_iv_work, b_plain, b_cipher,
                             B_BULK_BLOCKS);
        enc_us = c68k_eclock_micros(c68k_eclock() - start);

        b_check("ciphertext against nx_crypto", b_cipher, b_ref,
                B_BULK_BYTES);

        for (i = 0; i < 16u; i++)
        {
            b_iv_work[i] = b_iv[i];
        }
        start = c68k_eclock();
        c68k_aes_cbc_decrypt(&b_aes, b_iv_work, b_cipher, b_back,
                             B_BULK_BLOCKS);
        dec_us = c68k_eclock_micros(c68k_eclock() - start);

        b_check("plaintext after our own round trip", b_back, b_plain,
                B_BULK_BYTES);

        c68k_log("  %-44s enc %lu us (%lu KB/s)  dec %lu us (%lu KB/s)",
                 (LONG)c68k_aes_variant_name(v),
                 enc_us, b_kbs(enc_us), dec_us, b_kbs(dec_us));
    }

    c68k_aes_variant = saved;
}


/* ============================================================= SHA-256 ==== */

static VOID b_sha_one(const UCHAR *msg, ULONG len, const UCHAR *want,
                      const char *what)
{

C68K_SHA256 ctx;


    (VOID)c68k_sha256_initialize(&ctx, NX_CRYPTO_HASH_SHA256);
    (VOID)c68k_sha256_update(&ctx, (UCHAR *)(VOID *)msg, (UINT)len);
    (VOID)c68k_sha256_digest_calculate(&ctx, b_digest, NX_CRYPTO_HASH_SHA256);
    b_check(what, b_digest, want, 32uL);
}

static VOID b_sha_check_vectors(VOID)
{

C68K_SHA256     ctx;
static UCHAR    msg448[56];
UINT            i;
ULONG           left;


    b_sha_one((const UCHAR *)"abc", 3uL, b_sha_abc, "SHA-256(\"abc\")");
    b_sha_one((const UCHAR *)"", 0uL, b_sha_empty, "SHA-256(\"\")");

    for (i = 0; i < 56u; i++)
    {
        msg448[i] = (UCHAR)"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"[i];
    }
    b_sha_one(msg448, 56uL, b_sha_448, "SHA-256(56-byte message)");

    /* Split across calls at every awkward boundary: 1 byte, then 62 (which
       completes the first block mid-buffer), then the rest. */
    (VOID)c68k_sha256_initialize(&ctx, NX_CRYPTO_HASH_SHA256);
    (VOID)c68k_sha256_update(&ctx, &msg448[0], 1u);
    (VOID)c68k_sha256_update(&ctx, &msg448[1], 54u);
    (VOID)c68k_sha256_update(&ctx, &msg448[55], 1u);
    (VOID)c68k_sha256_digest_calculate(&ctx, b_digest, NX_CRYPTO_HASH_SHA256);
    b_check("SHA-256 split across three updates", b_digest, b_sha_448, 32uL);

    /* One million 'a', the vector that catches a 64-bit length counter that
       is really a 32-bit one, and every buffering path in update(). */
    for (i = 0; i < 56u; i++)
    {
        msg448[i] = (UCHAR)'a';
    }
    (VOID)c68k_sha256_initialize(&ctx, NX_CRYPTO_HASH_SHA256);
    left = 1000000uL;
    while (left != 0uL)
    {
        UINT chunk = (left > 56uL) ? 56u : (UINT)left;

        (VOID)c68k_sha256_update(&ctx, msg448, chunk);
        left -= (ULONG)chunk;
    }
    (VOID)c68k_sha256_digest_calculate(&ctx, b_digest, NX_CRYPTO_HASH_SHA256);
    b_check("SHA-256 of one million 'a'", b_digest, b_sha_million, 32uL);
}

static VOID b_bench_sha(VOID)
{

UINT        v;
UINT        saved;
ULONG       start;
ULONG       us;
C68K_SHA256 ctx;


    c68k_log("");
    c68k_log("3. SHA-256, %lu bytes a call", B_BULK_BYTES);

    saved = c68k_sha256_variant;

    /* The reference. */
    start = c68k_eclock();
    (VOID)_nx_crypto_sha256_initialize(&b_nx_sha, NX_CRYPTO_HASH_SHA256);
    (VOID)_nx_crypto_sha256_update(&b_nx_sha, b_plain, (UINT)B_BULK_BYTES);
    (VOID)_nx_crypto_sha256_digest_calculate(&b_nx_sha, b_digest_ref,
                                             NX_CRYPTO_HASH_SHA256);
    us = c68k_eclock_micros(c68k_eclock() - start);

    c68k_log("  %-44s %lu us (%lu KB/s)",
             (LONG)"nx_crypto, the implementation we ship today",
             us, b_kbs(us));

    for (v = 0; v < C68K_SHA256_V_COUNT; v++)
    {
        c68k_sha256_variant = v;

        b_sha_check_vectors();

        start = c68k_eclock();
        (VOID)c68k_sha256_initialize(&ctx, NX_CRYPTO_HASH_SHA256);
        (VOID)c68k_sha256_update(&ctx, b_plain, (UINT)B_BULK_BYTES);
        (VOID)c68k_sha256_digest_calculate(&ctx, b_digest,
                                           NX_CRYPTO_HASH_SHA256);
        us = c68k_eclock_micros(c68k_eclock() - start);

        b_check("digest against nx_crypto", b_digest, b_digest_ref, 32uL);

        c68k_log("  %-44s %lu us (%lu KB/s)",
                 (LONG)c68k_sha256_variant_name(v), us, b_kbs(us));

        /* And once more on an odd address, which is where a TLS record's
           payload actually lives. */
        start = c68k_eclock();
        (VOID)c68k_sha256_initialize(&ctx, NX_CRYPTO_HASH_SHA256);
        (VOID)c68k_sha256_update(&ctx, &b_plain[1], (UINT)(B_BULK_BYTES - 1uL));
        (VOID)c68k_sha256_digest_calculate(&ctx, b_digest,
                                           NX_CRYPTO_HASH_SHA256);
        us = c68k_eclock_micros(c68k_eclock() - start);
        c68k_log("      the same starting one byte in: %lu us", us);
    }

    c68k_sha256_variant = saved;
}


/* ================================================= HMAC, through nx_crypto == */
/*
 * HMAC is nx_crypto's framing with the hash swapped underneath it, which is
 * exactly how src/tls/ami_tls_crypto.c wires it, so measuring it any other
 * way would measure something the TLS stack does not run.
 */

static VOID b_bench_hmac(VOID)
{

ULONG   start;
ULONG   ours;
ULONG   theirs;
UINT    v;
UINT    saved;


    c68k_log("");
    c68k_log("4. HMAC-SHA256 over %lu bytes -- nx_crypto's framing, both "
             "hashes", B_BULK_BYTES);

    _nx_crypto_hmac_metadata_set(&b_nx_hmac, &b_nx_hmac_ctx,
                                 NX_CRYPTO_AUTHENTICATION_HMAC_SHA2_256,
                                 64u, 32u,
                                 (UINT (*)(VOID *, UINT))
                                     _nx_crypto_sha256_initialize,
                                 (UINT (*)(VOID *, UCHAR *, UINT))
                                     _nx_crypto_sha256_update,
                                 (UINT (*)(VOID *, UCHAR *, UINT))
                                     _nx_crypto_sha256_digest_calculate);

    _nx_crypto_hmac_metadata_set(&b_our_hmac, &b_our_hmac_ctx,
                                 NX_CRYPTO_AUTHENTICATION_HMAC_SHA2_256,
                                 64u, 32u,
                                 (UINT (*)(VOID *, UINT))
                                     c68k_sha256_initialize,
                                 (UINT (*)(VOID *, UCHAR *, UINT))
                                     c68k_sha256_update,
                                 (UINT (*)(VOID *, UCHAR *, UINT))
                                     c68k_sha256_digest_calculate);

    /* RFC 4231 case 2 first, on both, so a timing row is never printed for a
       hash that does not agree with the standard. */
    (VOID)_nx_crypto_hmac(&b_our_hmac,
                          (UCHAR *)(VOID *)"what do ya want for nothing?", 28u,
                          (UCHAR *)(VOID *)"Jefe", 4u, b_digest, 32u);
    b_check("RFC 4231 case 2", b_digest, b_hmac_rfc4231_2, 32uL);

    start = c68k_eclock();
    (VOID)_nx_crypto_hmac(&b_nx_hmac, b_plain, (UINT)B_BULK_BYTES,
                          b_key, 32u, b_digest_ref, 32u);
    theirs = c68k_eclock_micros(c68k_eclock() - start);

    c68k_log("  %-44s %lu us (%lu KB/s)",
             (LONG)"nx_crypto SHA-256", theirs, b_kbs(theirs));

    saved = c68k_sha256_variant;
    for (v = 0; v < C68K_SHA256_V_COUNT; v++)
    {
        c68k_sha256_variant = v;

        start = c68k_eclock();
        (VOID)_nx_crypto_hmac(&b_our_hmac, b_plain, (UINT)B_BULK_BYTES,
                              b_key, 32u, b_digest, 32u);
        ours = c68k_eclock_micros(c68k_eclock() - start);

        b_check("HMAC tag against nx_crypto", b_digest, b_digest_ref, 32uL);

        c68k_log("  %-44s %lu us (%lu KB/s)",
                 (LONG)c68k_sha256_variant_name(v), ours, b_kbs(ours));
    }
    c68k_sha256_variant = saved;
}


/* ================================================ ChaCha20-Poly1305 ======= */
/*
 * The other record path.  The case for adding it rests on one claim: that on a
 * 68020 an AEAD nobody has hand-optimised beats the AES-CBC-plus-HMAC pair two
 * rounds of work have already gone into.  Checked against RFC 8439's own
 * vectors first, 2.4.2 for the cipher, 2.5.2 for the authenticator, 2.8.2
 * for the AEAD.
 */

static const UCHAR b_cc_key[32] =
{
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
    0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F
};

static const UCHAR b_cc_nonce[12] =
{
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x4A, 0x00, 0x00, 0x00, 0x00
};

/* RFC 8439 2.4.2: the first 64 bytes of the keystream at block counter 1. */
static const UCHAR b_cc_stream[64] =
{
    0x22, 0x4F, 0x51, 0xF3, 0x40, 0x1B, 0xD9, 0xE1,
    0x2F, 0xDE, 0x27, 0x6F, 0xB8, 0x63, 0x1D, 0xED,
    0x8C, 0x13, 0x1F, 0x82, 0x3D, 0x2C, 0x06, 0xE2,
    0x7E, 0x4F, 0xCA, 0xEC, 0x9E, 0xF3, 0xCF, 0x78,
    0x8A, 0x3B, 0x0A, 0xA3, 0x72, 0x60, 0x0A, 0x92,
    0xB5, 0x79, 0x74, 0xCD, 0xED, 0x2B, 0x93, 0x34,
    0x79, 0x4C, 0xBA, 0x40, 0xC6, 0x3E, 0x34, 0xCD,
    0xEA, 0x21, 0x2C, 0x4C, 0xF0, 0x7D, 0x41, 0xB7
};

/* RFC 8439 2.5.2. */
static const UCHAR b_poly_key[32] =
{
    0x85, 0xD6, 0xBE, 0x78, 0x57, 0x55, 0x6D, 0x33,
    0x7F, 0x44, 0x52, 0xFE, 0x42, 0xD5, 0x06, 0xA8,
    0x01, 0x03, 0x80, 0x8A, 0xFB, 0x0D, 0xB2, 0xFD,
    0x4A, 0xBF, 0xF6, 0xAF, 0x41, 0x49, 0xF5, 0x1B
};

static const UCHAR b_poly_tag[16] =
{
    0xA8, 0x06, 0x1D, 0xC1, 0x30, 0x51, 0x36, 0xC6,
    0xC2, 0x2B, 0x8B, 0xAF, 0x0C, 0x01, 0x27, 0xA9
};

/* RFC 8439 2.8.2, the AEAD, tag only; the ciphertext is checked by the
   round trip below. */
static const UCHAR b_aead_key[32] =
{
    0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
    0x88, 0x89, 0x8A, 0x8B, 0x8C, 0x8D, 0x8E, 0x8F,
    0x90, 0x91, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97,
    0x98, 0x99, 0x9A, 0x9B, 0x9C, 0x9D, 0x9E, 0x9F
};

static const UCHAR b_aead_nonce[12] =
{
    0x07, 0x00, 0x00, 0x00, 0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47
};

static const UCHAR b_aead_aad[12] =
{
    0x50, 0x51, 0x52, 0x53, 0xC0, 0xC1, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xC7
};

static const UCHAR b_aead_tag[16] =
{
    0x1A, 0xE1, 0x0B, 0x59, 0x4F, 0x09, 0xE2, 0x6A,
    0x7E, 0x90, 0x2E, 0xCB, 0xD0, 0x60, 0x06, 0x91
};

/* 114 bytes and a NUL, which is why the size is left to the compiler: only
   the 114 are fed to the AEAD. */
static const UCHAR b_aead_plain[] =
    "Ladies and Gentlemen of the class of '99: If I could offer you only "
    "one tip for the future, sunscreen would be it.";

static C68K_CHACHA20            b_cc;
static C68K_POLY1305            b_poly;
static C68K_CHACHA20_POLY1305   b_aead;
static UCHAR                    b_tag[16];

static VOID b_chacha_check_vectors(VOID)
{

static UCHAR    got[128];
UINT            i;


    for (i = 0; i < 64u; i++)
    {
        got[i] = 0u;
    }
    c68k_chacha20_initialize(&b_cc, b_cc_key, b_cc_nonce, 1uL);
    c68k_chacha20_keystream(&b_cc, got, 64uL);
    b_check("RFC 8439 2.4.2 keystream", got, b_cc_stream, 64uL);

    /* Split at boundaries no block aligns with, because that is what a packet
       chain hands the record path. */
    for (i = 0; i < 64u; i++)
    {
        got[i] = 0u;
    }
    c68k_chacha20_initialize(&b_cc, b_cc_key, b_cc_nonce, 1uL);
    c68k_chacha20_keystream(&b_cc, &got[0], 7uL);
    c68k_chacha20_keystream(&b_cc, &got[7], 50uL);
    c68k_chacha20_keystream(&b_cc, &got[57], 7uL);
    b_check("RFC 8439 2.4.2 across three calls", got, b_cc_stream, 64uL);

    c68k_poly1305_initialize(&b_poly, b_poly_key);
    c68k_poly1305_update(&b_poly,
                         (const UCHAR *)"Cryptographic Forum Research Group",
                         34uL);
    c68k_poly1305_finish(&b_poly, b_tag);
    b_check("RFC 8439 2.5.2 tag", b_tag, b_poly_tag, 16uL);

    c68k_poly1305_initialize(&b_poly, b_poly_key);
    c68k_poly1305_update(&b_poly,
                         (const UCHAR *)"Cryptographic Forum Research Group",
                         1uL);
    c68k_poly1305_update(&b_poly,
                         (const UCHAR *)"ryptographic Forum Research Group",
                         20uL);
    c68k_poly1305_update(&b_poly, (const UCHAR *)"Forum Research Group" + 7u,
                         13uL);
    c68k_poly1305_finish(&b_poly, b_tag);
    b_check("RFC 8439 2.5.2 across three updates", b_tag, b_poly_tag, 16uL);

    c68k_chacha20_poly1305_initialize(&b_aead, b_aead_key, b_aead_nonce);
    c68k_chacha20_poly1305_associate(&b_aead, b_aead_aad, 12uL);
    c68k_chacha20_poly1305_encrypt(&b_aead, b_aead_plain, got, 114uL);
    c68k_chacha20_poly1305_tag(&b_aead, b_tag);
    b_check("RFC 8439 2.8.2 AEAD tag", b_tag, b_aead_tag, 16uL);

    c68k_chacha20_poly1305_initialize(&b_aead, b_aead_key, b_aead_nonce);
    c68k_chacha20_poly1305_associate(&b_aead, b_aead_aad, 12uL);
    c68k_chacha20_poly1305_decrypt(&b_aead, got, got, 114uL);
    c68k_chacha20_poly1305_tag(&b_aead, b_tag);
    b_check("RFC 8439 2.8.2 plaintext recovered", got, b_aead_plain, 114uL);
    b_check("RFC 8439 2.8.2 tag on decrypt", b_tag, b_aead_tag, 16uL);
}

/*
 * The block function this build ships against the portable C one, eight
 * blocks deep.  RFC 8439's vectors check one block and a keystream; this
 * checks that the register schedule in c68k_chacha20.S is still the same
 * function after the counter has advanced, which is what an assembly bug
 * that only appeared on the second block would look like.  It costs nothing
 * on a build with no assembly, where it compares the C against itself.
 */
#define B_CC_BLOCKS     8u

static VOID b_chacha_check_core(VOID)
{

static UCHAR    got[B_CC_BLOCKS * 64u];
static UCHAR    want[B_CC_BLOCKS * 64u];
ULONG           st[16];
ULONG           ks[16];
UINT            b;
UINT            i;
UINT            o;


    c68k_chacha20_initialize(&b_cc, b_aead_key, b_aead_nonce, 1uL);
    c68k_chacha20_keystream(&b_cc, got, (ULONG)(B_CC_BLOCKS * 64u));

    c68k_chacha20_initialize(&b_cc, b_aead_key, b_aead_nonce, 1uL);
    for (b = 0u; b < B_CC_BLOCKS; b++)
    {
        for (i = 0u; i < 16u; i++)
        {
            st[i] = b_cc.c68k_chacha20_state[i];
        }
        st[12] += (ULONG)b;

        c68k_chacha20_core_c(st, ks);

        for (i = 0u; i < 16u; i++)
        {
            o = (b << 6) + (i << 2);
            want[o]      = (UCHAR)(ks[i]);
            want[o + 1u] = (UCHAR)(ks[i] >> 8);
            want[o + 2u] = (UCHAR)(ks[i] >> 16);
            want[o + 3u] = (UCHAR)(ks[i] >> 24);
        }
    }

    b_check("block function against the portable C, 8 blocks",
            got, want, (ULONG)(B_CC_BLOCKS * 64u));
}

/*
 * Poly1305's block function, the shipped one against the portable C one, over
 * the same blocks and from the same starting accumulator.
 *
 * The equivalent of the ChaCha20 check above, for the same reason: RFC 8439
 * 2.5.2 authenticates 34 bytes, two blocks and a partial, and a kernel bug in
 * the carry between limbs or in the fold round the top of 2^130 can hide until
 * the accumulator is large and the limbs have been near their bounds a few
 * hundred times.  So it runs both over 1, 2, 3, 5, 17 and 200 blocks, twice
 * per count, once with the 2^128 bit a full block carries and once without,
 * which is the short-final-block case c68k_poly1305_finish() takes and which
 * nothing else here reaches with a grown accumulator.
 *
 * It compares the accumulator rather than a tag: h is what the kernel writes,
 * a tag is h after a reduction that is common to both paths, and a difference
 * in the fifth limb that the final carry happens to absorb should still be a
 * failure.
 */
static const ULONG b_poly_block_counts[] =
{
    1uL, 2uL, 3uL, 5uL, 17uL, 200uL
};

static VOID b_poly_check_blocks(VOID)
{

C68K_POLY1305   ours;
C68K_POLY1305   ref;
ULONG           hibit;
ULONG           n;
UINT            k;
UINT            pass;


    for (pass = 0u; pass < 2u; pass++)
    {
        hibit = (pass == 0u) ? ((ULONG)1uL << 24) : 0uL;

        for (k = 0u; k < (UINT)(sizeof(b_poly_block_counts) /
                                sizeof(b_poly_block_counts[0])); k++)
        {
            n = b_poly_block_counts[k];

            c68k_poly1305_initialize(&ours, b_poly_key);
            c68k_poly1305_initialize(&ref, b_poly_key);

            /* Not from zero: run 64 blocks in first, through both paths, so
               that the counts below compare a kernel picking up an accumulator
               it did not itself produce. */
            c68k_poly1305_blocks(&ours, b_plain, 64uL, (ULONG)1uL << 24);
            c68k_poly1305_blocks_c(&ref, b_plain, 64uL, (ULONG)1uL << 24);

            c68k_poly1305_blocks(&ours, &b_plain[1024], n, hibit);
            c68k_poly1305_blocks_c(&ref, &b_plain[1024], n, hibit);

            b_check("Poly1305 block function against the portable C",
                    (const UCHAR *)ours.c68k_poly1305_h,
                    (const UCHAR *)ref.c68k_poly1305_h, 20uL);
        }
    }
}

/*
 * Independent known answers: the tag over the first N bytes of b_plain under
 * RFC 8439 2.5.2's key, for seventeen lengths that straddle every boundary the
 * buffering has, empty, one byte, one short of a block, exactly a block, one
 * past it, and the same around 32, 64, 128, 1024 and the whole 16 KiB.
 *
 * They were computed off-target from the definition in RFC 8439 2.5, r
 * clamped, the message read little-endian sixteen bytes at a time with the
 * 2^128 bit appended, the accumulator multiplied by r modulo 2^130 - 5, s
 * added at the end, by a generator that reproduces 2.5.2's own tag before it
 * emits any of these.  They are not this implementation's own output recorded,
 * which would only prove it is deterministic.
 *
 * Every length is run twice: once as a single update, and once split at 1, 13
 * and the rest, which lands the split inside a block for every length past 14
 * and exercises the leftover buffer in the state a packet chain leaves it.
 */
static const ULONG b_poly_lens[] =
{
    0uL, 1uL, 15uL, 16uL, 17uL, 31uL, 32uL, 33uL, 63uL, 64uL, 127uL, 128uL,
    129uL, 1023uL, 1024uL, 4096uL, 16384uL
};

static const UCHAR b_poly_tags[][16] =
{
    { 0x01, 0x03, 0x80, 0x8A, 0xFB, 0x0D, 0xB2, 0xFD,
      0x4A, 0xBF, 0xF6, 0xAF, 0x41, 0x49, 0xF5, 0x1B },     /* 0     */
    { 0x0B, 0x88, 0x56, 0x49, 0x04, 0x62, 0x07, 0x6B,
      0x4E, 0x3B, 0x3B, 0x02, 0x50, 0x89, 0xCA, 0x22 },     /* 1     */
    { 0xD1, 0xF2, 0x85, 0x98, 0xBC, 0x85, 0x0C, 0xAA,
      0xE6, 0xC4, 0x57, 0x9B, 0x04, 0xBB, 0x4F, 0xA0 },     /* 15    */
    { 0xA1, 0x8A, 0x0D, 0xE2, 0xBA, 0x29, 0x91, 0x28,
      0x30, 0x3A, 0x39, 0x8E, 0x28, 0xBD, 0xE4, 0xF0 },     /* 16    */
    { 0x37, 0x47, 0x7D, 0x65, 0x16, 0x0C, 0x3C, 0xA0,
      0x46, 0x6A, 0xAC, 0x57, 0x80, 0x78, 0x5E, 0xF5 },     /* 17    */
    { 0x0E, 0x25, 0xD9, 0x3D, 0x39, 0x50, 0x72, 0x35,
      0x12, 0x7A, 0x78, 0xC3, 0x43, 0x31, 0x1E, 0xCA },     /* 31    */
    { 0xA0, 0xA5, 0x0F, 0x18, 0xE2, 0x7E, 0x3B, 0x64,
      0xB5, 0x5C, 0x78, 0xB7, 0x10, 0xBC, 0x53, 0x6B },     /* 32    */
    { 0x4B, 0x43, 0xC8, 0x89, 0xC0, 0x67, 0xC2, 0xFA,
      0x13, 0xA0, 0xF4, 0x16, 0xF5, 0xB9, 0x1D, 0x74 },     /* 33    */
    { 0x14, 0x29, 0x57, 0xB2, 0xB0, 0x3E, 0x4D, 0xCA,
      0x04, 0x00, 0xA6, 0xD5, 0x4D, 0x2E, 0xFC, 0x66 },     /* 63    */
    { 0x2A, 0x7B, 0xEB, 0xAD, 0xAE, 0x82, 0x9F, 0x59,
      0x5B, 0xBD, 0xE2, 0xCB, 0x6C, 0xCA, 0x72, 0xA9 },     /* 64    */
    { 0x5E, 0x16, 0x8C, 0x4D, 0xA1, 0x9B, 0xF3, 0x8E,
      0x76, 0xD5, 0xD4, 0xA7, 0xE8, 0xF3, 0xB3, 0x54 },     /* 127   */
    { 0x81, 0x0B, 0xDC, 0x8B, 0x49, 0x0A, 0x58, 0xDF,
      0x33, 0x48, 0x8B, 0xA2, 0xAB, 0xB2, 0xAC, 0xD9 },     /* 128   */
    { 0x19, 0x37, 0x4E, 0x4A, 0xD5, 0xC2, 0x41, 0x13,
      0x61, 0x45, 0x20, 0xB7, 0xD6, 0x80, 0xDA, 0x2B },     /* 129   */
    { 0x96, 0xCC, 0x60, 0x62, 0x56, 0xEE, 0x56, 0xD0,
      0x32, 0x52, 0x0A, 0xAA, 0x98, 0xAF, 0x29, 0xF8 },     /* 1023  */
    { 0x29, 0x3C, 0x07, 0x2B, 0x53, 0xD8, 0xD2, 0xD1,
      0x3C, 0x7B, 0x7E, 0xFD, 0x03, 0x9A, 0x08, 0x73 },     /* 1024  */
    { 0x6E, 0x9A, 0x3A, 0xB0, 0xDD, 0x5B, 0xC4, 0xC5,
      0xBD, 0xEC, 0x52, 0xF5, 0x17, 0xBB, 0xEC, 0x42 },     /* 4096  */
    { 0x66, 0xBF, 0x39, 0xC5, 0xE1, 0x6D, 0x1E, 0x38,
      0x76, 0xFD, 0x59, 0x87, 0x40, 0x0E, 0x1F, 0xDC }      /* 16384 */
};

static VOID b_poly_check_lengths(VOID)
{

ULONG   n;
ULONG   first;
ULONG   second;
UINT    k;


    for (k = 0u; k < (UINT)(sizeof(b_poly_lens) / sizeof(b_poly_lens[0])); k++)
    {
        n = b_poly_lens[k];

        c68k_poly1305_initialize(&b_poly, b_poly_key);
        c68k_poly1305_update(&b_poly, b_plain, n);
        c68k_poly1305_finish(&b_poly, b_tag);
        b_check("Poly1305 known answer", b_tag, b_poly_tags[k], 16uL);

        first  = (n < 1uL) ? n : 1uL;
        second = ((n - first) < 13uL) ? (n - first) : 13uL;

        c68k_poly1305_initialize(&b_poly, b_poly_key);
        c68k_poly1305_update(&b_poly, b_plain, first);
        c68k_poly1305_update(&b_poly, &b_plain[first], second);
        c68k_poly1305_update(&b_poly, &b_plain[first + second],
                             n - first - second);
        c68k_poly1305_finish(&b_poly, b_tag);
        b_check("Poly1305 known answer, split 1/13/rest", b_tag,
                b_poly_tags[k], 16uL);
    }
}

/*
 * Neither the RFC vectors nor the comparisons above would catch a verify()
 * that always agreed.  So: the real tag, then every one of the sixteen bytes
 * flipped in turn, then a record whose ciphertext was altered under an
 * untouched tag, which is the forgery an attacker can construct.
 */
static VOID b_poly_check_forged(VOID)
{

static UCHAR    cipher[114];
UCHAR           forged[16];
UCHAR           good[16];
UINT            i;


    c68k_chacha20_poly1305_initialize(&b_aead, b_aead_key, b_aead_nonce);
    c68k_chacha20_poly1305_associate(&b_aead, b_aead_aad, 12uL);
    c68k_chacha20_poly1305_encrypt(&b_aead, b_aead_plain, cipher, 114uL);
    c68k_chacha20_poly1305_tag(&b_aead, good);

    if (c68k_chacha20_poly1305_verify(good, b_aead_tag) !=
        (UINT)NX_CRYPTO_TRUE)
    {
        b_fail("the genuine tag did not verify");
    }

    for (i = 0u; i < 16u; i++)
    {
        UINT j;

        for (j = 0u; j < 16u; j++)
        {
            forged[j] = good[j];
        }
        forged[i] ^= 0x40u;

        if (c68k_chacha20_poly1305_verify(good, forged) !=
            (UINT)NX_CRYPTO_FALSE)
        {
            b_fail("a tag with one byte altered VERIFIED");
        }
    }

    cipher[57] ^= 0x01u;
    c68k_chacha20_poly1305_initialize(&b_aead, b_aead_key, b_aead_nonce);
    c68k_chacha20_poly1305_associate(&b_aead, b_aead_aad, 12uL);
    c68k_chacha20_poly1305_decrypt(&b_aead, cipher, cipher, 114uL);
    c68k_chacha20_poly1305_tag(&b_aead, forged);

    if (c68k_chacha20_poly1305_verify(good, forged) != (UINT)NX_CRYPTO_FALSE)
    {
        b_fail("an altered ciphertext kept its tag");
    }
}

static VOID b_bench_chacha(VOID)
{

ULONG   start;
ULONG   cipher_us;
ULONG   mac_us;
ULONG   mac_c_us;
ULONG   aead_us;
ULONG   odd_us;


    c68k_log("");
    c68k_log("5. ChaCha20-Poly1305 over %lu bytes -- the AEAD record path",
             B_BULK_BYTES);
    c68k_log("   ChaCha20 block function: %s",
             (LONG)(c68k_chacha20_core_is_asm() == (UINT)NX_CRYPTO_TRUE ?
                    "68020 assembly" : "portable C"));
    c68k_log("   Poly1305 block function: %s",
             (LONG)(c68k_poly1305_blocks_is_asm() == (UINT)NX_CRYPTO_TRUE ?
                    "68020 assembly" : "portable C"));

    b_chacha_check_vectors();
    b_chacha_check_core();
    b_poly_check_blocks();
    b_poly_check_lengths();
    b_poly_check_forged();

    c68k_chacha20_initialize(&b_cc, b_cc_key, b_cc_nonce, 1uL);
    start = c68k_eclock();
    c68k_chacha20_xor(&b_cc, b_plain, b_cipher, B_BULK_BYTES);
    cipher_us = c68k_eclock_micros(c68k_eclock() - start);

    c68k_poly1305_initialize(&b_poly, b_poly_key);
    start = c68k_eclock();
    c68k_poly1305_update(&b_poly, b_cipher, B_BULK_BYTES);
    c68k_poly1305_finish(&b_poly, b_tag);
    mac_us = c68k_eclock_micros(c68k_eclock() - start);

    /* And the portable C block function over the same bytes, in the same run
      , the only comparison an emulator does not distort, and the one that
       says what the assembly is worth on this machine. */
    c68k_poly1305_initialize(&b_poly, b_poly_key);
    start = c68k_eclock();
    c68k_poly1305_blocks_c(&b_poly, b_cipher, B_BULK_BYTES / 16uL,
                           (ULONG)1uL << 24);
    mac_c_us = c68k_eclock_micros(c68k_eclock() - start);

    c68k_log("  %-44s %lu us (%lu KB/s)", (LONG)"ChaCha20 alone",
             cipher_us, b_kbs(cipher_us));
    c68k_log("  %-44s %lu us (%lu KB/s)", (LONG)"Poly1305 alone",
             mac_us, b_kbs(mac_us));
    c68k_log("  %-44s %lu us (%lu KB/s)",
             (LONG)"      its block function, portable C",
             mac_c_us, b_kbs(mac_c_us));

    /* And through the AEAD, which is what a record costs: the two above plus
       one extra ChaCha20 block for the one-time Poly1305 key. */
    start = c68k_eclock();
    c68k_chacha20_poly1305_initialize(&b_aead, b_aead_key, b_aead_nonce);
    c68k_chacha20_poly1305_associate(&b_aead, b_aead_aad, 13uL);
    c68k_chacha20_poly1305_encrypt(&b_aead, b_plain, b_cipher, B_BULK_BYTES);
    c68k_chacha20_poly1305_tag(&b_aead, b_tag);
    aead_us = c68k_eclock_micros(c68k_eclock() - start);

    c68k_log("  %-44s %lu us (%lu KB/s)", (LONG)"the AEAD, one whole record",
             aead_us, b_kbs(aead_us));

    /* The same buffer starting one byte in, which is where a TLS record's
       payload actually lives. */
    c68k_chacha20_initialize(&b_cc, b_cc_key, b_cc_nonce, 1uL);
    start = c68k_eclock();
    c68k_chacha20_xor(&b_cc, &b_plain[1], &b_cipher[1], B_BULK_BYTES - 1uL);
    odd_us = c68k_eclock_micros(c68k_eclock() - start);
    c68k_log("      the cipher starting one byte in: %lu us", odd_us);
}


/* ============================================================== summary ==== */

static VOID b_summary(VOID)
{

ULONG   start;
ULONG   enc;
ULONG   mac;
ULONG   dec;
ULONG   aead_enc;
ULONG   aead_dec;
UINT    i;
C68K_SHA256 ctx;


    c68k_log("");
    c68k_log("6. One 16 KiB TLS record, end to end, at the defaults this "
             "build ships");

    (VOID)c68k_aes_key_set(&b_aes, b_key, 128u);

    for (i = 0; i < 16u; i++)
    {
        b_iv_work[i] = b_iv[i];
    }

    start = c68k_eclock();
    c68k_aes_cbc_encrypt(&b_aes, b_iv_work, b_plain, b_cipher, B_BULK_BLOCKS);
    enc = c68k_eclock_micros(c68k_eclock() - start);

    start = c68k_eclock();
    (VOID)_nx_crypto_hmac(&b_our_hmac, b_plain, (UINT)B_BULK_BYTES,
                          b_key, 32u, b_digest, 32u);
    mac = c68k_eclock_micros(c68k_eclock() - start);

    for (i = 0; i < 16u; i++)
    {
        b_iv_work[i] = b_iv[i];
    }
    start = c68k_eclock();
    c68k_aes_cbc_decrypt(&b_aes, b_iv_work, b_cipher, b_back, B_BULK_BLOCKS);
    dec = c68k_eclock_micros(c68k_eclock() - start);

    (VOID)c68k_sha256_initialize(&ctx, NX_CRYPTO_HASH_SHA256);

    c68k_log("  0xC027/0xC023, AES-128-CBC + HMAC-SHA256");
    c68k_log("    send: AES encrypt %lu us + HMAC %lu us = %lu us, %lu KB/s",
             enc, mac, enc + mac, b_kbs(enc + mac));
    c68k_log("    recv: HMAC %lu us + AES decrypt %lu us = %lu us, %lu KB/s",
             mac, dec, mac + dec, b_kbs(mac + dec));

    /*
     * The same record through the AEAD, in one call each way, because that is
     * how it is paid: the cipher and the authenticator are one operation, not
     * two passes to choose between.
     */
    start = c68k_eclock();
    c68k_chacha20_poly1305_initialize(&b_aead, b_key, b_iv);
    c68k_chacha20_poly1305_associate(&b_aead, b_iv, 13uL);
    c68k_chacha20_poly1305_encrypt(&b_aead, b_plain, b_cipher, B_BULK_BYTES);
    c68k_chacha20_poly1305_tag(&b_aead, b_tag);
    aead_enc = c68k_eclock_micros(c68k_eclock() - start);

    start = c68k_eclock();
    c68k_chacha20_poly1305_initialize(&b_aead, b_key, b_iv);
    c68k_chacha20_poly1305_associate(&b_aead, b_iv, 13uL);
    c68k_chacha20_poly1305_decrypt(&b_aead, b_cipher, b_back, B_BULK_BYTES);
    c68k_chacha20_poly1305_tag(&b_aead, b_digest);
    aead_dec = c68k_eclock_micros(c68k_eclock() - start);

    b_check("AEAD round trip", b_back, b_plain, B_BULK_BYTES);
    b_check("AEAD tag agrees both ways", b_digest, b_tag, 16uL);

    c68k_log("  0xCCA8/0xCCA9, ChaCha20-Poly1305");
    c68k_log("    send: %lu us, %lu KB/s", aead_enc, b_kbs(aead_enc));
    c68k_log("    recv: %lu us, %lu KB/s", aead_dec, b_kbs(aead_dec));

    /* Which record path is cheaper on this machine, and by how much.  Scaled
       by 100 because there is no floating point here. */
    c68k_log("  AEAD is %lu.%02lux the CBC pair on send, %lu.%02lux on recv",
             ((enc + mac) * 100uL / aead_enc) / 100uL,
             ((enc + mac) * 100uL / aead_enc) % 100uL,
             ((mac + dec) * 100uL / aead_dec) / 100uL,
             ((mac + dec) * 100uL / aead_dec) % 100uL);
}


/* ================================================================== body == */

static int b_body(VOID)
{

UINT    i;


    c68k_log("AmiNetXDuo -- the TLS record path on a 68020");
    c68k_log("  AES default variant: %s",
             (LONG)c68k_aes_variant_name(c68k_aes_variant));
    c68k_log("  SHA-256 default:     %s",
             (LONG)c68k_sha256_variant_name(c68k_sha256_variant));

    if (!c68k_timer_open())
    {
        c68k_log("FATAL: timer.device UNIT_ECLOCK would not open");
        return(20);
    }
    c68k_log("  E-Clock %lu Hz", c68k_eclock_hz());

    for (i = 0; i < (UINT)B_BULK_BYTES; i++)
    {
        b_plain[i] = (UCHAR)(i ^ (i >> 8));
    }
    for (i = 0; i < 32u; i++)
    {
        b_key[i] = (UCHAR)(0x10u + i);
    }
    for (i = 0; i < 16u; i++)
    {
        b_iv[i] = (UCHAR)(0xA0u + i);
    }

    b_bench_kernels();
    b_bench_aes();
    b_bench_sha();
    b_bench_hmac();
    b_bench_chacha();
    b_summary();

    c68k_log("");
    if (b_failures == 0uL)
    {
        c68k_log("PASS -- every variant agreed with the published vectors and "
                 "with nx_crypto");
    }
    else
    {
        c68k_log("FAIL -- %lu check(s) failed", b_failures);
    }

    c68k_timer_close();

    return((b_failures == 0uL) ? 0 : 20);
}

int main(VOID)
{

int rc;


    ami_crash_set_reference((APTR)main, "crypto68k_bulk");
    if (!ami_crash_install())
    {
        c68k_log("CRASHED -- see the serial log and DH0:crash.txt");
        c68k_flush();
        ami_crash_remove();
        return(20);
    }

    rc = b_body();

    c68k_flush();
    ami_crash_remove();

    return(rc);
}

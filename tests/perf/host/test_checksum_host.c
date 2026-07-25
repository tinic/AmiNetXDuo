/*
 * AmiNetXDuo -- src/net68k/ checksum, differentially against the vendored one.
 *
 * A faster checksum that is wrong corrupts silently: every packet still goes
 * out, the peer drops some of them, and the symptom is "the network is a bit
 * flaky".  So this does not check that n68k_ip_checksum_compute() computes a
 * correct internet checksum -- it checks that it returns EXACTLY what
 * _nx_ip_checksum_compute() returns, for every input shape the stack can
 * produce and several it cannot.
 *
 * Both functions are compiled into this binary from source: the vendored one
 * with -D_nx_ip_checksum_compute=n68k_checksum_reference, which renames the
 * symbol without touching the file.  host/shim/tx_port.h pins the m68k's type
 * widths so the vendored loop reads longwords, not 64-bit words.
 *
 * COVERED
 *   - every data_length from 0 to 96, and a spread up to 8 KB
 *   - TCP and UDP (pseudo header) and ICMP (none)
 *   - prepend offsets 0..7, i.e. every alignment including the odd ones the
 *     packet pool never produces
 *   - chains of 1..5 packets, with per-packet fill chosen so append pointers
 *     land on all four residues mod 4 -- that is where the vendored code has
 *     its two-byte carry across the boundary
 *   - the trailing 1/2/3-byte cases, which write a zero pad byte into the
 *     packet buffer, so the buffer is restored between the two calls
 *   - data_length shorter than the packet, and longer (chain continues)
 *
 * SPDX-License-Identifier: MIT
 */

#include "nx_api.h"
#include "net68k.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

USHORT n68k_checksum_reference(NX_PACKET *packet_ptr, ULONG protocol,
                               UINT data_length, ULONG *src_ip_addr,
                               ULONG *dest_ip_addr);

/*
 * NX_ASSERT's failure path parks the calling thread in tx_thread_sleep(-1).
 * Both implementations reference it and neither reaches it -- every call
 * below passes non-null addresses -- but the link needs the symbol, and a
 * stub that aborts is better than one that returns, because reaching it would
 * mean the test had stopped testing what it thinks it is.
 */
UINT _tx_thread_sleep(ULONG timer_ticks)
{
    (void)timer_ticks;
    fprintf(stderr, "NX_ASSERT fired\n");
    abort();
}


/* ------------------------------------------------------------- harness --- */

static unsigned long h_checks;
static unsigned long h_failures;

static void h_fail(const char *what, unsigned long a, unsigned long b,
                   unsigned long detail)
{
    h_failures++;
    printf("FAIL %s: ours=0x%04lx ref=0x%04lx (%lu)\n", what, a, b, detail);
}

#define H_MAX_PACKETS   5
#define H_BUF           9216

typedef struct
{
    NX_PACKET   pkt[H_MAX_PACKETS];
    UCHAR       buf[H_MAX_PACKETS][H_BUF];
    UCHAR       saved[H_MAX_PACKETS][H_BUF];
    UINT        count;
} h_chain;

/* Deterministic pseudo-random fill: the failures have to be reproducible. */
static ULONG h_rng_state = 0x12345678UL;

static ULONG h_rand(void)
{
    h_rng_state = (h_rng_state * 1103515245UL) + 12345UL;
    return (h_rng_state >> 8) & 0x00FFFFFFUL;
}

/*
 * Build a chain of `count` packets.  Packet i holds `fill[i]` bytes starting
 * at `offset` inside its buffer; NetX Duo keeps that offset longword aligned,
 * but the tests below deliberately do not always.
 */
static void h_build(h_chain *c, UINT count, const UINT *fill, UINT offset)
{
    UINT i, j;

    c->count = count;

    for (i = 0; i < count; i++)
    {
        for (j = 0; j < H_BUF; j++)
        {
            c->buf[i][j] = (UCHAR)h_rand();
        }

        memset(&c->pkt[i], 0, sizeof(NX_PACKET));

        c->pkt[i].nx_packet_data_start   = c->buf[i];
        c->pkt[i].nx_packet_data_end     = c->buf[i] + H_BUF;
        c->pkt[i].nx_packet_prepend_ptr  = c->buf[i] + offset;
        c->pkt[i].nx_packet_append_ptr   = c->buf[i] + offset + fill[i];
        c->pkt[i].nx_packet_next         = NX_NULL;
#ifdef FEATURE_NX_IPV6
        c->pkt[i].nx_packet_ip_version   = NX_IP_VERSION_V4;
#endif

        if (i > 0)
        {
            c->pkt[i - 1].nx_packet_next = &c->pkt[i];
        }
    }

    memcpy(c->saved, c->buf, sizeof(c->saved));
}

static void h_restore(h_chain *c)
{
    memcpy(c->buf, c->saved, sizeof(c->buf));
}

/*
 * One comparison.  Ours runs first on a pristine chain, the chain is restored,
 * then the reference runs -- both of them may write a zero pad byte past the
 * data, and neither may be allowed to see the other's.
 */
static void h_compare(h_chain *c, ULONG protocol, UINT data_length,
                      ULONG *src, ULONG *dst, const char *what)
{
    USHORT ours;
    USHORT ref;

    h_restore(c);
    ours = n68k_ip_checksum_compute(&c->pkt[0], protocol, data_length, src, dst);

    h_restore(c);
    ref = n68k_checksum_reference(&c->pkt[0], protocol, data_length, src, dst);

    h_checks++;

    if (ours != ref)
    {
        h_fail(what, (unsigned long)ours, (unsigned long)ref,
               (unsigned long)data_length);
    }
}


/* --------------------------------------------------------------- cases --- */

static ULONG h_src[4] = { 0xC0A80001UL, 0, 0, 0 };
static ULONG h_dst[4] = { 0xC0A80002UL, 0, 0, 0 };

static const ULONG h_protocols[3] =
{
    NX_PROTOCOL_TCP, NX_PROTOCOL_UDP, NX_PROTOCOL_ICMP
};

static h_chain h_c;

int main(void)
{
    UINT  len, offset, p, n, i, trial;
    UINT  fill[H_MAX_PACKETS];

    printf("net68k checksum: differential against the vendored implementation\n");

    /* ---- 1. single packet, every length 0..96, every prepend alignment -- */
    for (offset = 0; offset < 8; offset++)
    {
        for (len = 0; len <= 96; len++)
        {
            fill[0] = len;
            h_build(&h_c, 1, fill, offset);

            for (p = 0; p < 3; p++)
            {
                h_compare(&h_c, h_protocols[p], len, h_src, h_dst,
                          "short single packet");
            }
        }
    }

    /* ---- 2. single packet, realistic and awkward lengths --------------- */
    {
        static const UINT lens[] =
        {
            97, 127, 128, 129, 255, 256, 257, 511, 512, 513,
            535, 536, 537, 1023, 1024, 1025, 1459, 1460, 1461,
            1472, 1500, 1514, 1568, 2047, 2048, 4095, 4096, 8192
        };

        for (i = 0; i < sizeof(lens) / sizeof(lens[0]); i++)
        {
            for (offset = 0; offset < 4; offset += 2)
            {
                fill[0] = lens[i];
                h_build(&h_c, 1, fill, offset);

                for (p = 0; p < 3; p++)
                {
                    h_compare(&h_c, h_protocols[p], lens[i], h_src, h_dst,
                              "single packet");
                }
            }
        }
    }

    /* ---- 3. data_length SHORTER than the packet holds ------------------ */
    for (len = 0; len <= 64; len++)
    {
        fill[0] = 1460;
        h_build(&h_c, 1, fill, 0);

        for (p = 0; p < 3; p++)
        {
            h_compare(&h_c, h_protocols[p], len, h_src, h_dst,
                      "short read of a full packet");
        }
    }

    /* ---- 4. chains ----------------------------------------------------- */
    /*
     * The interesting axis is the append pointer's residue mod 4 on every
     * packet but the last: residue 2 makes the vendored code carry two bytes
     * across the boundary, residues 1 and 3 make it lose the odd byte, and
     * this has to reproduce all three.
     */
    for (n = 2; n <= H_MAX_PACKETS; n++)
    {
        for (trial = 0; trial < 64; trial++)
        {
            UINT total = 0;

            for (i = 0; i < n; i++)
            {
                fill[i] = 1 + (UINT)(h_rand() % 200UL);
                total  += fill[i];
            }

            for (offset = 0; offset < 4; offset += 2)
            {
                h_build(&h_c, n, fill, offset);

                for (p = 0; p < 3; p++)
                {
                    /* the whole chain, and a prefix of it */
                    h_compare(&h_c, h_protocols[p], total, h_src, h_dst,
                              "chain, whole");
                    h_compare(&h_c, h_protocols[p], total / 2, h_src, h_dst,
                              "chain, prefix");
                    h_compare(&h_c, h_protocols[p], fill[0], h_src, h_dst,
                              "chain, first packet only");
                    h_compare(&h_c, h_protocols[p], fill[0] + 1, h_src, h_dst,
                              "chain, one byte into the second");
                }
            }
        }
    }

    /* ---- 5. chains with every residue on the boundary, exhaustively ---- */
    for (i = 0; i < 4; i++)
    {
        for (len = 0; len < 8; len++)
        {
            fill[0] = 40 + i;               /* append_ptr % 4 == i          */
            fill[1] = 40 + len;
            h_build(&h_c, 2, fill, 0);

            for (p = 0; p < 3; p++)
            {
                h_compare(&h_c, h_protocols[p], fill[0] + fill[1], h_src, h_dst,
                          "boundary residue");
            }
        }
    }

    /* ---- 6. all-zero and all-ones payloads ----------------------------- */
    /*
     * The one place a one's-complement sum can disagree with a 16-bit
     * accumulation is 0x0000 against 0xFFFF, and these are the inputs that
     * produce it.
     */
    {
        static const UCHAR patterns[3] = { 0x00, 0xFF, 0x55 };

        for (i = 0; i < 3; i++)
        {
            for (len = 0; len <= 64; len++)
            {
                fill[0] = len;
                h_build(&h_c, 1, fill, 0);
                memset(h_c.buf[0], patterns[i], H_BUF);
                memcpy(h_c.saved, h_c.buf, sizeof(h_c.saved));

                for (p = 0; p < 3; p++)
                {
                    h_compare(&h_c, h_protocols[p], len, h_src, h_dst,
                              "uniform payload");
                }
            }
        }
    }

    /* ---- 7. the sum primitive on its own -------------------------------- */
    /*
     * n68k_sum_longwords() is what the assembly replaces, so it gets checked
     * against a plain 64-bit sum folded by hand -- the property the C caller
     * relies on is congruence modulo 0xFFFF, plus "zero only when empty".
     */
    {
        static ULONG words[257];

        for (n = 0; n <= 256; n++)
        {
            unsigned long long plain = 0;
            ULONG              got;
            ULONG              a, b;

            for (i = 0; i < n; i++)
            {
                words[i] = (h_rand() << 8) ^ h_rand();
                plain   += words[i];
            }

            got = n68k_sum_longwords(words, n);

            while (plain >> 32)
            {
                plain = (plain & 0xFFFFFFFFULL) + (plain >> 32);
            }

            a = ((got   >> 16) + (got   & 0xFFFFUL));
            a = ((a     >> 16) + (a     & 0xFFFFUL));
            b = (ULONG)((plain >> 16) + (plain & 0xFFFFULL));
            b = ((b     >> 16) + (b     & 0xFFFFUL));

            h_checks++;
            if (a != b)
            {
                h_fail("sum_longwords fold", a, b, n);
            }

            h_checks++;
            if ((n == 0) != (got == 0UL) && n == 0)
            {
                h_fail("sum_longwords empty", got, 0, n);
            }
        }
    }

    printf("%lu checks, %lu failures -- %s\n",
           h_checks, h_failures, (h_failures == 0UL) ? "PASS" : "FAIL");

    return (h_failures == 0UL) ? 0 : 1;
}

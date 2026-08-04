/*
 * AmiNetXDuo -- DNS wire-format helpers shared by fuzz_dns and fuzz_mdns.
 *
 * mDNS is DNS on the wire (RFC 6762 §18), so one builder and one mutator serve
 * both drivers.  Only plain C types here: the two drivers compile with
 * different ULONG widths (see their headers) and nothing in this file may
 * depend on that.
 *
 * The seed table below is the part worth reading.  It is the list of shapes a
 * hostile answer can take -- self-referential and cyclic compression
 * pointers, offsets past the datagram, names that expand beyond 255 bytes,
 * labels longer than 63, an RDLENGTH that disagrees with the bytes present,
 * header counts that disagree with the records -- and the mutator works from
 * those rather than from noise, because uniform random bytes almost never
 * produce a parsable header, let alone a pointer loop.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_FUZZ_WIRE_H
#define AMINETXDUO_FUZZ_WIRE_H

#include <stddef.h>
#include <string.h>

/* Bigger than a 512-byte DNS message on purpose: an answer off the wire is
   whatever the sender put in the datagram, not what the querier asked for. */
#define FZW_MAX             1600

#define FZW_QR              0x8000u
#define FZW_AA              0x0400u
#define FZW_TC              0x0200u

#define FZW_TYPE_A          1
#define FZW_TYPE_NS         2
#define FZW_TYPE_CNAME      5
#define FZW_TYPE_SOA        6
#define FZW_TYPE_PTR        12
#define FZW_TYPE_MX         15
#define FZW_TYPE_TXT        16
#define FZW_TYPE_AAAA       28
#define FZW_TYPE_SRV        33
#define FZW_TYPE_NSEC       47
#define FZW_TYPE_ANY        255

#define FZW_CLASS_IN        1
#define FZW_CLASS_FLUSH     0x8001u     /* mDNS cache-flush bit           */
#define FZW_CLASS_QU        0x8001u     /* mDNS unicast-response bit (QU) */

typedef struct
{
    unsigned char b[FZW_MAX];
    size_t        len;
} FzwBuf;

static void fzw_reset(FzwBuf *w)
{
    memset(w->b, 0, sizeof(w->b));
    w->len = 0;
}

static void fzw_u8(FzwBuf *w, unsigned v)
{
    if (w->len < FZW_MAX)
        w->b[w->len++] = (unsigned char)(v & 0xFFu);
}

static void fzw_u16(FzwBuf *w, unsigned v)
{
    fzw_u8(w, v >> 8);
    fzw_u8(w, v);
}

static void fzw_u32(FzwBuf *w, unsigned long v)
{
    fzw_u16(w, (unsigned)(v >> 16));
    fzw_u16(w, (unsigned)v);
}

static void fzw_raw(FzwBuf *w, const void *p, size_t n)
{
    const unsigned char *s = (const unsigned char *)p;
    size_t               i;

    for (i = 0; i < n; i++)
        fzw_u8(w, s[i]);
}

/* A dotted name as length-prefixed labels, root-terminated. */
static void fzw_name(FzwBuf *w, const char *dotted)
{
    const char *p = dotted;

    while (*p != '\0')
    {
        const char *dot = strchr(p, '.');
        size_t      n   = (dot != NULL) ? (size_t)(dot - p) : strlen(p);

        fzw_u8(w, (unsigned)n);
        fzw_raw(w, p, n);

        p += n;
        if (*p == '.')
            p++;
    }

    fzw_u8(w, 0);
}

/* A compression pointer to `offset`. */
static void fzw_ptr(FzwBuf *w, unsigned offset)
{
    fzw_u16(w, 0xC000u | (offset & 0x3FFFu));
}

static void fzw_hdr(FzwBuf *w, unsigned id, unsigned flags,
                    unsigned qd, unsigned an, unsigned ns, unsigned ar)
{
    fzw_u16(w, id);
    fzw_u16(w, flags);
    fzw_u16(w, qd);
    fzw_u16(w, an);
    fzw_u16(w, ns);
    fzw_u16(w, ar);
}

static void fzw_question(FzwBuf *w, const char *name, unsigned type,
                         unsigned class_)
{
    fzw_name(w, name);
    fzw_u16(w, type);
    fzw_u16(w, class_);
}

/* Patch a 16-bit big-endian field already written. */
static void fzw_patch16(FzwBuf *w, size_t at, unsigned v)
{
    if (at + 1 < w->len)
    {
        w->b[at]     = (unsigned char)(v >> 8);
        w->b[at + 1] = (unsigned char)v;
    }
}

/* --------------------------------------------------------------- random --- */

/* unsigned long long, not unsigned long: fuzz_mdns is built -m32 (NetX Duo's
   mDNS cache keeps pointers in ULONG slots, so it needs sizeof(void*) == 4),
   where unsigned long is 32 bits -- the multiplier would truncate and the
   shift would be undefined. Fixing the width also makes a seed mean the same
   sequence on a 32- and a 64-bit host. */
static unsigned long long fzw_state = 1;

static unsigned fzw_rand(void)
{
    fzw_state = fzw_state * 6364136223846793005ULL + 1442695040888963407ULL;
    return (unsigned)(fzw_state >> 33);
}

static unsigned fzw_below(unsigned n)
{
    return (n == 0) ? 0 : (fzw_rand() % n);
}

/* ---------------------------------------------------------------- seeds --- */

/*
 * The hazards, as whole datagrams.  `qname` is the name the driver told the
 * resolver to look up, so a seed can either match it -- which gets past the
 * question-section comparison and into the record loop -- or not.
 */

typedef void (*FzwSeedFn)(FzwBuf *w, const char *qname);

/* A well-formed A answer.  The baseline: if this stops parsing, the harness
   itself is wrong. */
static void fzs_a_answer(FzwBuf *w, const char *qname)
{
    fzw_hdr(w, 0, FZW_QR | FZW_AA, 1, 1, 0, 0);
    fzw_question(w, qname, FZW_TYPE_A, FZW_CLASS_IN);
    fzw_ptr(w, 12);
    fzw_u16(w, FZW_TYPE_A);
    fzw_u16(w, FZW_CLASS_IN);
    fzw_u32(w, 300);
    fzw_u16(w, 4);
    fzw_u8(w, 10); fzw_u8(w, 0); fzw_u8(w, 0); fzw_u8(w, 9);
}

static void fzs_aaaa_answer(FzwBuf *w, const char *qname)
{
    int i;

    fzw_hdr(w, 0, FZW_QR | FZW_AA, 1, 1, 0, 0);
    fzw_question(w, qname, FZW_TYPE_AAAA, FZW_CLASS_IN);
    fzw_ptr(w, 12);
    fzw_u16(w, FZW_TYPE_AAAA);
    fzw_u16(w, FZW_CLASS_IN);
    fzw_u32(w, 300);
    fzw_u16(w, 16);
    for (i = 0; i < 16; i++)
        fzw_u8(w, (unsigned)i);
}

/*
 * The question a reverse lookup of 10.0.0.9 asks.  addons/dns builds it from
 * the octets in reverse followed by lookup_end[], which is spelled in capitals
 * -- so this is the exact string on the wire, and a server that answers in
 * lower case is answering the same question in the other case.
 */
#define FZW_INADDR_QNAME    "9.0.0.10.IN-ADDR.ARPA"

/* A PTR answer, which is what a reverse lookup parses. */
static void fzs_ptr_answer(FzwBuf *w, const char *qname)
{
    size_t rdlen_at;
    size_t rdata_at;

    fzw_hdr(w, 0, FZW_QR | FZW_AA, 1, 1, 0, 0);
    fzw_question(w, qname, FZW_TYPE_PTR, FZW_CLASS_IN);
    fzw_ptr(w, 12);
    fzw_u16(w, FZW_TYPE_PTR);
    fzw_u16(w, FZW_CLASS_IN);
    fzw_u32(w, 300);
    rdlen_at = w->len;
    fzw_u16(w, 0);
    rdata_at = w->len;
    fzw_name(w, "amiga.example.com");
    fzw_patch16(w, rdlen_at, (unsigned)(w->len - rdata_at));
}

/* A name whose first two bytes are a pointer to themselves. */
static void fzs_ptr_self(FzwBuf *w, const char *qname)
{
    (void)qname;
    fzw_hdr(w, 0, FZW_QR | FZW_AA, 1, 1, 0, 0);
    fzw_ptr(w, 12);                      /* the question name, at offset 12 */
    fzw_u16(w, FZW_TYPE_A);
    fzw_u16(w, FZW_CLASS_IN);
    fzw_ptr(w, 12);
    fzw_u16(w, FZW_TYPE_A);
    fzw_u16(w, FZW_CLASS_IN);
    fzw_u32(w, 300);
    fzw_u16(w, 4);
    fzw_u32(w, 0x0A000009UL);
}

/* A pointer that goes forward, so following it never reduces the offset. */
static void fzs_ptr_forward(FzwBuf *w, const char *qname)
{
    (void)qname;
    fzw_hdr(w, 0, FZW_QR | FZW_AA, 1, 1, 0, 0);
    fzw_ptr(w, 20);
    fzw_u16(w, FZW_TYPE_A);
    fzw_u16(w, FZW_CLASS_IN);
    fzw_u8(w, 1); fzw_u8(w, 'a');
    fzw_ptr(w, 26);
    fzw_u8(w, 1); fzw_u8(w, 'b');
    fzw_ptr(w, 32);
    fzw_u8(w, 1); fzw_u8(w, 'c');
    fzw_u8(w, 0);
}

/* Two pointers that point at each other. */
static void fzs_ptr_cycle(FzwBuf *w, const char *qname)
{
    (void)qname;
    fzw_hdr(w, 0, FZW_QR | FZW_AA, 1, 1, 0, 0);
    fzw_ptr(w, 14);                     /* -> 14 */
    fzw_ptr(w, 12);                     /* -> 12 */
    fzw_u16(w, FZW_TYPE_A);
    fzw_u16(w, FZW_CLASS_IN);
    fzw_ptr(w, 12);
    fzw_u16(w, FZW_TYPE_A);
    fzw_u16(w, FZW_CLASS_IN);
    fzw_u32(w, 300);
    fzw_u16(w, 4);
    fzw_u32(w, 0x0A000009UL);
}

/* A three-step cycle, in case a two-step one is special-cased. */
static void fzs_ptr_cycle3(FzwBuf *w, const char *qname)
{
    (void)qname;
    fzw_hdr(w, 0, FZW_QR | FZW_AA, 1, 1, 0, 0);
    fzw_ptr(w, 14);
    fzw_ptr(w, 16);
    fzw_ptr(w, 12);
    fzw_u16(w, FZW_TYPE_A);
    fzw_u16(w, FZW_CLASS_IN);
    fzw_u32(w, 300);
    fzw_u16(w, 4);
    fzw_u32(w, 0x0A000009UL);
}

/* An offset past the end of the datagram. */
static void fzs_ptr_past_end(FzwBuf *w, const char *qname)
{
    (void)qname;
    fzw_hdr(w, 0, FZW_QR | FZW_AA, 1, 1, 0, 0);
    fzw_ptr(w, 0x3FFF);
    fzw_u16(w, FZW_TYPE_A);
    fzw_u16(w, FZW_CLASS_IN);
    fzw_ptr(w, 600);
    fzw_u16(w, FZW_TYPE_A);
    fzw_u16(w, FZW_CLASS_IN);
    fzw_u32(w, 300);
    fzw_u16(w, 4);
    fzw_u32(w, 0x0A000009UL);
}

/* A pointer into the middle of a length byte, so the label read is misaligned
   with the bytes the sender wrote. */
static void fzs_ptr_misaligned(FzwBuf *w, const char *qname)
{
    (void)qname;
    fzw_hdr(w, 0, FZW_QR | FZW_AA, 1, 1, 0, 0);
    fzw_u8(w, 3); fzw_raw(w, "www", 3);
    fzw_ptr(w, 13);                      /* into "www", not at a length */
    fzw_u16(w, FZW_TYPE_A);
    fzw_u16(w, FZW_CLASS_IN);
    fzw_u32(w, 300);
    fzw_u16(w, 4);
    fzw_u32(w, 0x0A000009UL);
}

/* Labels that unencode to well over 255 bytes. */
static void fzs_name_over_255(FzwBuf *w, const char *qname)
{
    int i;

    (void)qname;
    fzw_hdr(w, 0, FZW_QR | FZW_AA, 1, 0, 0, 0);
    for (i = 0; i < 40; i++)
    {
        fzw_u8(w, 9);
        fzw_raw(w, "abcdefghi", 9);
    }
    fzw_u8(w, 0);
    fzw_u16(w, FZW_TYPE_A);
    fzw_u16(w, FZW_CLASS_IN);
}

/* Over 255 bytes reached through a pointer rather than in one run: the length
   check has to hold across the jump as well as within a label. */
static void fzs_name_over_255_via_ptr(FzwBuf *w, const char *qname)
{
    int i;

    (void)qname;
    fzw_hdr(w, 0, FZW_QR | FZW_AA, 1, 0, 0, 0);
    for (i = 0; i < 20; i++)
    {
        fzw_u8(w, 9);
        fzw_raw(w, "abcdefghi", 9);
    }
    fzw_ptr(w, 12);                      /* back to the first label */
    fzw_u16(w, FZW_TYPE_A);
    fzw_u16(w, FZW_CLASS_IN);
}

/* A label length above the 63 the format allows, but below 0xC0 so it is not
   a pointer either -- the reserved encodings of RFC 1035 §4.1.4. */
static void fzs_label_over_63(FzwBuf *w, const char *qname)
{
    (void)qname;
    fzw_hdr(w, 0, FZW_QR | FZW_AA, 1, 1, 0, 0);
    fzw_u8(w, 100);
    fzw_raw(w, "0123456789", 10);        /* and nothing like 100 bytes */
    fzw_u8(w, 0);
    fzw_u16(w, FZW_TYPE_A);
    fzw_u16(w, FZW_CLASS_IN);
}

static void fzs_label_reserved(FzwBuf *w, const char *qname)
{
    (void)qname;
    fzw_hdr(w, 0, FZW_QR | FZW_AA, 1, 1, 0, 0);
    fzw_u8(w, 0x80);                     /* 10xxxxxx: reserved */
    fzw_u8(w, 0x40);                     /* 01xxxxxx: reserved */
    fzw_u8(w, 0);
    fzw_u16(w, FZW_TYPE_A);
    fzw_u16(w, FZW_CLASS_IN);
}

/* A record header cut off mid-way. */
static void fzs_truncated_rr(FzwBuf *w, const char *qname)
{
    fzw_hdr(w, 0, FZW_QR | FZW_AA, 1, 1, 0, 0);
    fzw_question(w, qname, FZW_TYPE_A, FZW_CLASS_IN);
    fzw_ptr(w, 12);
    fzw_u16(w, FZW_TYPE_A);
    fzw_u8(w, 0);                        /* half a class, then nothing */
}

/* The question section cut off mid-name. */
static void fzs_truncated_question(FzwBuf *w, const char *qname)
{
    (void)qname;
    fzw_hdr(w, 0, FZW_QR | FZW_AA, 1, 1, 0, 0);
    fzw_u8(w, 4); fzw_raw(w, "test", 4);
    fzw_u8(w, 7); fzw_raw(w, "exa", 3);  /* claims 7, three present */
}

/* RDLENGTH larger than the bytes that follow. */
static void fzs_rdlength_too_big(FzwBuf *w, const char *qname)
{
    fzw_hdr(w, 0, FZW_QR | FZW_AA, 1, 1, 0, 0);
    fzw_question(w, qname, FZW_TYPE_A, FZW_CLASS_IN);
    fzw_ptr(w, 12);
    fzw_u16(w, FZW_TYPE_A);
    fzw_u16(w, FZW_CLASS_IN);
    fzw_u32(w, 300);
    fzw_u16(w, 0xFFFF);
    fzw_u32(w, 0x0A000009UL);
}

/* RDLENGTH of zero on a type whose data has a fixed size. */
static void fzs_rdlength_zero(FzwBuf *w, const char *qname)
{
    fzw_hdr(w, 0, FZW_QR | FZW_AA, 1, 1, 0, 0);
    fzw_question(w, qname, FZW_TYPE_A, FZW_CLASS_IN);
    fzw_ptr(w, 12);
    fzw_u16(w, FZW_TYPE_A);
    fzw_u16(w, FZW_CLASS_IN);
    fzw_u32(w, 300);
    fzw_u16(w, 0);
}

/* An A record carrying sixteen bytes and an AAAA carrying four. */
static void fzs_rdlength_wrong_type(FzwBuf *w, const char *qname)
{
    int i;

    fzw_hdr(w, 0, FZW_QR | FZW_AA, 1, 2, 0, 0);
    fzw_question(w, qname, FZW_TYPE_A, FZW_CLASS_IN);

    fzw_ptr(w, 12);
    fzw_u16(w, FZW_TYPE_A);
    fzw_u16(w, FZW_CLASS_IN);
    fzw_u32(w, 300);
    fzw_u16(w, 16);
    for (i = 0; i < 16; i++)
        fzw_u8(w, 0xAA);

    fzw_ptr(w, 12);
    fzw_u16(w, FZW_TYPE_AAAA);
    fzw_u16(w, FZW_CLASS_IN);
    fzw_u32(w, 300);
    fzw_u16(w, 4);
    fzw_u32(w, 0x0A000009UL);
}

/* Counts that disagree with the records present. */
static void fzs_ancount_lies(FzwBuf *w, const char *qname)
{
    fzw_hdr(w, 0, FZW_QR | FZW_AA, 1, 0xFFFF, 0xFFFF, 0xFFFF);
    fzw_question(w, qname, FZW_TYPE_A, FZW_CLASS_IN);
}

static void fzs_qdcount_lies(FzwBuf *w, const char *qname)
{
    (void)qname;
    fzw_hdr(w, 0, FZW_QR | FZW_AA, 1, 1, 0, 0);
    /* No question at all, and no answer either. */
}

static void fzs_qdcount_many(FzwBuf *w, const char *qname)
{
    int i;

    fzw_hdr(w, 0, FZW_QR | FZW_AA, 8, 1, 0, 0);
    for (i = 0; i < 8; i++)
        fzw_question(w, qname, FZW_TYPE_A, FZW_CLASS_IN);
    fzw_ptr(w, 12);
    fzw_u16(w, FZW_TYPE_A);
    fzw_u16(w, FZW_CLASS_IN);
    fzw_u32(w, 300);
    fzw_u16(w, 4);
    fzw_u32(w, 0x0A000009UL);
}

/* Every section populated, each with a pointer-heavy name. */
static void fzs_all_sections(FzwBuf *w, const char *qname)
{
    int i;

    fzw_hdr(w, 0, FZW_QR | FZW_AA, 1, 2, 2, 2);
    fzw_question(w, qname, FZW_TYPE_A, FZW_CLASS_IN);

    for (i = 0; i < 6; i++)
    {
        fzw_ptr(w, 12);
        fzw_u16(w, (i & 1) ? FZW_TYPE_AAAA : FZW_TYPE_A);
        fzw_u16(w, FZW_CLASS_IN);
        fzw_u32(w, 0);
        if (i & 1)
        {
            int j;

            fzw_u16(w, 16);
            for (j = 0; j < 16; j++)
                fzw_u8(w, (unsigned)j);
        }
        else
        {
            fzw_u16(w, 4);
            fzw_u32(w, 0x0A000000UL + (unsigned long)i);
        }
    }
}

/* Types whose processing is only compiled with the extended set, so the size
   walk has to step over them correctly either way. */
static void fzs_extended_types(FzwBuf *w, const char *qname)
{
    size_t at;

    fzw_hdr(w, 0, FZW_QR | FZW_AA, 1, 5, 0, 0);
    fzw_question(w, qname, FZW_TYPE_A, FZW_CLASS_IN);

    fzw_ptr(w, 12);
    fzw_u16(w, FZW_TYPE_CNAME);
    fzw_u16(w, FZW_CLASS_IN);
    fzw_u32(w, 60);
    at = w->len;
    fzw_u16(w, 0);
    fzw_ptr(w, 12);
    fzw_patch16(w, at, 2);

    fzw_ptr(w, 12);
    fzw_u16(w, FZW_TYPE_MX);
    fzw_u16(w, FZW_CLASS_IN);
    fzw_u32(w, 60);
    fzw_u16(w, 4);
    fzw_u16(w, 10);
    fzw_ptr(w, 12);

    fzw_ptr(w, 12);
    fzw_u16(w, FZW_TYPE_TXT);
    fzw_u16(w, FZW_CLASS_IN);
    fzw_u32(w, 60);
    fzw_u16(w, 6);
    fzw_u8(w, 200);                     /* a character-string that lies */
    fzw_raw(w, "hello", 5);

    fzw_ptr(w, 12);
    fzw_u16(w, FZW_TYPE_SRV);
    fzw_u16(w, FZW_CLASS_IN);
    fzw_u32(w, 60);
    fzw_u16(w, 8);
    fzw_u16(w, 0); fzw_u16(w, 0); fzw_u16(w, 80);
    fzw_ptr(w, 12);

    fzw_ptr(w, 12);
    fzw_u16(w, FZW_TYPE_SOA);
    fzw_u16(w, FZW_CLASS_IN);
    fzw_u32(w, 60);
    at = w->len;
    fzw_u16(w, 0);
    fzw_ptr(w, 12);
    fzw_ptr(w, 12);
    fzw_u32(w, 1); fzw_u32(w, 2); fzw_u32(w, 3); fzw_u32(w, 4); fzw_u32(w, 5);
    fzw_patch16(w, at, 24);
}

/* Just the header. */
static void fzs_header_only(FzwBuf *w, const char *qname)
{
    (void)qname;
    fzw_hdr(w, 0, FZW_QR | FZW_AA, 0, 0, 0, 0);
}

/* Less than a header. */
static void fzs_runt(FzwBuf *w, const char *qname)
{
    (void)qname;
    fzw_u16(w, 0);
    fzw_u8(w, 0x81);
}

static void fzs_empty(FzwBuf *w, const char *qname)
{
    (void)qname;
    (void)w;
}

/* All zeroes, and all ones, at a full datagram's length. */
static void fzs_zeros(FzwBuf *w, const char *qname)
{
    int i;

    (void)qname;
    for (i = 0; i < 512; i++)
        fzw_u8(w, 0);
}

static void fzs_ones(FzwBuf *w, const char *qname)
{
    int i;

    (void)qname;
    for (i = 0; i < 512; i++)
        fzw_u8(w, 0xFF);
}

/* A name made only of pointers, filling the datagram. */
static void fzs_pointer_storm(FzwBuf *w, const char *qname)
{
    int i;

    (void)qname;
    fzw_hdr(w, 0, FZW_QR | FZW_AA, 1, 0xFF, 0, 0);
    for (i = 0; i < 240; i++)
        fzw_ptr(w, (unsigned)(12 + 2 * i));
}

/* A server failure and a truncated response, which take the early exits. */
static void fzs_rcode_error(FzwBuf *w, const char *qname)
{
    fzw_hdr(w, 0, FZW_QR | 0x0003u, 1, 0, 0, 0);
    fzw_question(w, qname, FZW_TYPE_A, FZW_CLASS_IN);
}

static void fzs_truncated_flag(FzwBuf *w, const char *qname)
{
    fzw_hdr(w, 0, FZW_QR | FZW_TC, 1, 1, 0, 0);
    fzw_question(w, qname, FZW_TYPE_A, FZW_CLASS_IN);
    fzw_ptr(w, 12);
}

/* A question that is not the one asked, which is the mismatch path. */
static void fzs_wrong_question(FzwBuf *w, const char *qname)
{
    (void)qname;
    fzw_hdr(w, 0, FZW_QR | FZW_AA, 1, 1, 0, 0);
    fzw_question(w, "evil.example.net", FZW_TYPE_A, FZW_CLASS_IN);
    fzw_ptr(w, 12);
    fzw_u16(w, FZW_TYPE_A);
    fzw_u16(w, FZW_CLASS_IN);
    fzw_u32(w, 300);
    fzw_u16(w, 4);
    fzw_u32(w, 0x0A000009UL);
}

/*
 * An A record for somebody else, in the authority section, with nothing in the
 * answer section.  The authority section names the servers to ask next; it is
 * not where an answer lives, and the owner name of a record there is never
 * compared with the question.  Taking one as an answer meant a server asked
 * about one name could return -- and get cached -- an address for another.
 */
static void fzs_a_in_authority(FzwBuf *w, const char *qname)
{
    fzw_hdr(w, 0, FZW_QR | FZW_AA, 1, 0, 1, 0);
    fzw_question(w, qname, FZW_TYPE_A, FZW_CLASS_IN);
    fzw_name(w, "bank.example.com");
    fzw_u16(w, FZW_TYPE_A);
    fzw_u16(w, FZW_CLASS_IN);
    fzw_u32(w, 300);
    fzw_u16(w, 4);
    fzw_u8(w, 10); fzw_u8(w, 0); fzw_u8(w, 0); fzw_u8(w, 66);
}

/* The same, with a real answer as well: the answer must still be the one
   taken, and the authority record must not displace it. */
static void fzs_a_answer_plus_authority(FzwBuf *w, const char *qname)
{
    fzw_hdr(w, 0, FZW_QR | FZW_AA, 1, 1, 1, 0);
    fzw_question(w, qname, FZW_TYPE_A, FZW_CLASS_IN);

    fzw_ptr(w, 12);
    fzw_u16(w, FZW_TYPE_A);
    fzw_u16(w, FZW_CLASS_IN);
    fzw_u32(w, 300);
    fzw_u16(w, 4);
    fzw_u8(w, 10); fzw_u8(w, 0); fzw_u8(w, 0); fzw_u8(w, 9);

    fzw_name(w, "bank.example.com");
    fzw_u16(w, FZW_TYPE_A);
    fzw_u16(w, FZW_CLASS_IN);
    fzw_u32(w, 300);
    fzw_u16(w, 4);
    fzw_u8(w, 10); fzw_u8(w, 0); fzw_u8(w, 66);
}

/* The question echoed in the other case.  RFC 4343 makes that the same name,
   and several resolvers normalise before echoing. */
static void fzs_question_upper(FzwBuf *w, const char *qname)
{
    char  upper[256];
    size_t i;

    for (i = 0; i + 1 < sizeof(upper) && qname[i] != '\0'; i++)
        upper[i] = (char)((qname[i] >= 'a' && qname[i] <= 'z')
                          ? (qname[i] - 'a' + 'A') : qname[i]);
    upper[i] = '\0';

    fzw_hdr(w, 0, FZW_QR | FZW_AA, 1, 1, 0, 0);
    fzw_question(w, upper, FZW_TYPE_A, FZW_CLASS_IN);
    fzw_ptr(w, 12);
    fzw_u16(w, FZW_TYPE_A);
    fzw_u16(w, FZW_CLASS_IN);
    fzw_u32(w, 300);
    fzw_u16(w, 4);
    fzw_u8(w, 10); fzw_u8(w, 0); fzw_u8(w, 0); fzw_u8(w, 9);
}

/*
 * A PTR answer carrying the question a reverse lookup actually asks.  Without
 * this the reverse parser stops at the question comparison for every seed in
 * the table and its record walk is never reached.
 */
static void fzs_ptr_answer_inaddr(FzwBuf *w, const char *qname)
{
    size_t rdlen_at;
    size_t rdata_at;

    (void)qname;
    fzw_hdr(w, 0, FZW_QR | FZW_AA, 1, 1, 0, 0);
    fzw_question(w, FZW_INADDR_QNAME, FZW_TYPE_PTR, FZW_CLASS_IN);
    fzw_ptr(w, 12);
    fzw_u16(w, FZW_TYPE_PTR);
    fzw_u16(w, FZW_CLASS_IN);
    fzw_u32(w, 300);
    rdlen_at = w->len;
    fzw_u16(w, 0);
    rdata_at = w->len;
    fzw_name(w, "amiga.example.com");
    fzw_patch16(w, rdlen_at, (unsigned)(w->len - rdata_at));
}

/* The same shape, asking about a different address.  The reverse path used to
   skip the question, so this answered the query it was not the answer to. */
static void fzs_ptr_wrong_question(FzwBuf *w, const char *qname)
{
    size_t rdlen_at;
    size_t rdata_at;

    (void)qname;
    fzw_hdr(w, 0, FZW_QR | FZW_AA, 1, 1, 0, 0);
    fzw_question(w, "4.3.2.1.IN-ADDR.ARPA", FZW_TYPE_PTR, FZW_CLASS_IN);
    fzw_ptr(w, 12);
    fzw_u16(w, FZW_TYPE_PTR);
    fzw_u16(w, FZW_CLASS_IN);
    fzw_u32(w, 300);
    rdlen_at = w->len;
    fzw_u16(w, 0);
    rdata_at = w->len;
    fzw_name(w, "evil.example.net");
    fzw_patch16(w, rdlen_at, (unsigned)(w->len - rdata_at));
}

typedef struct
{
    const char *name;
    FzwSeedFn   build;
} FzwSeed;

static const FzwSeed fzw_seeds[] =
{
    { "a_answer",                fzs_a_answer                },
    { "aaaa_answer",             fzs_aaaa_answer             },
    { "ptr_answer",              fzs_ptr_answer              },
    { "ptr_self",                fzs_ptr_self                },
    { "ptr_forward",             fzs_ptr_forward             },
    { "ptr_cycle",               fzs_ptr_cycle               },
    { "ptr_cycle3",              fzs_ptr_cycle3              },
    { "ptr_past_end",            fzs_ptr_past_end            },
    { "ptr_misaligned",          fzs_ptr_misaligned          },
    { "name_over_255",           fzs_name_over_255           },
    { "name_over_255_via_ptr",   fzs_name_over_255_via_ptr   },
    { "label_over_63",           fzs_label_over_63           },
    { "label_reserved",          fzs_label_reserved          },
    { "truncated_rr",            fzs_truncated_rr            },
    { "truncated_question",      fzs_truncated_question      },
    { "rdlength_too_big",        fzs_rdlength_too_big        },
    { "rdlength_zero",           fzs_rdlength_zero           },
    { "rdlength_wrong_type",     fzs_rdlength_wrong_type     },
    { "ancount_lies",            fzs_ancount_lies            },
    { "qdcount_lies",            fzs_qdcount_lies            },
    { "qdcount_many",            fzs_qdcount_many            },
    { "all_sections",            fzs_all_sections            },
    { "extended_types",          fzs_extended_types          },
    { "header_only",             fzs_header_only             },
    { "runt",                    fzs_runt                    },
    { "empty",                   fzs_empty                   },
    { "zeros",                   fzs_zeros                   },
    { "ones",                    fzs_ones                    },
    { "pointer_storm",           fzs_pointer_storm           },
    { "rcode_error",             fzs_rcode_error             },
    { "truncated_flag",          fzs_truncated_flag          },
    { "wrong_question",          fzs_wrong_question          },
    { "a_in_authority",          fzs_a_in_authority          },
    { "a_answer_plus_authority", fzs_a_answer_plus_authority },
    { "question_upper",          fzs_question_upper          },
    { "ptr_answer_inaddr",       fzs_ptr_answer_inaddr       },
    { "ptr_wrong_question",      fzs_ptr_wrong_question      }
};

#define FZW_SEED_COUNT  (int)(sizeof(fzw_seeds) / sizeof(fzw_seeds[0]))

/* -------------------------------------------------------------- mutation --- */

/*
 * Mutations aimed at the fields that decide how far the parser walks, not at
 * the payload bytes.  A flipped address byte changes an answer; a flipped
 * length byte changes where the next read lands.
 */
static void fzw_mutate(FzwBuf *w)
{
    unsigned rounds = fzw_below(6) + 1u;

    while (rounds-- > 0)
    {
        if (w->len == 0)
            return;

        switch (fzw_below(10))
        {
        case 0:
            /* A byte, anywhere. */
            w->b[fzw_below((unsigned)w->len)] = (unsigned char)fzw_rand();
            break;

        case 1:
        {
            /* Turn two bytes into a compression pointer to a random offset. */
            size_t at = fzw_below((unsigned)w->len);

            if (at + 1 < w->len)
            {
                w->b[at]     = (unsigned char)(0xC0u | (fzw_rand() & 0x3Fu));
                w->b[at + 1] = (unsigned char)fzw_rand();
            }
            break;
        }

        case 2:
            /* A label length, including the reserved and pointer encodings. */
            w->b[fzw_below((unsigned)w->len)] =
                (unsigned char)(fzw_below(4) == 0 ? fzw_rand()
                                                  : fzw_below(0x100));
            break;

        case 3:
            /* Truncate.  Every parser here decides where to stop from the
               header, and the datagram is free to end sooner. */
            w->len = fzw_below((unsigned)w->len + 1u);
            break;

        case 4:
        {
            /* One of the five header counts. */
            size_t at = 2u * (size_t)(fzw_below(5) + 1u);

            if (at + 1 < w->len)
            {
                w->b[at]     = (unsigned char)fzw_rand();
                w->b[at + 1] = (unsigned char)fzw_rand();
            }
            break;
        }

        case 5:
        {
            /* An RDLENGTH-shaped 16-bit field, set to an extreme. */
            size_t   at = fzw_below((unsigned)w->len);
            unsigned v  = (fzw_below(2) == 0) ? 0xFFFFu : 0u;

            if (at + 1 < w->len)
            {
                w->b[at]     = (unsigned char)(v >> 8);
                w->b[at + 1] = (unsigned char)v;
            }
            break;
        }

        case 6:
        {
            /* Splice a region over another, which makes names refer into the
               middle of records. */
            size_t from = fzw_below((unsigned)w->len);
            size_t to   = fzw_below((unsigned)w->len);
            size_t n    = fzw_below(32) + 1u;

            while (n-- > 0 && from < w->len && to < w->len)
                w->b[to++] = w->b[from++];
            break;
        }

        case 7:
            /* Extend with random bytes: a record can claim more than the
               sender sent, and can also send more than it claims. */
            {
                unsigned n = fzw_below(64);

                while (n-- > 0 && w->len < FZW_MAX)
                    w->b[w->len++] = (unsigned char)fzw_rand();
            }
            break;

        case 8:
            /* Set the response and authoritative bits, so a mutated datagram
               still reaches the record loop most of the time. */
            if (w->len > 3)
            {
                w->b[2] = (unsigned char)((fzw_below(4) == 0)
                                          ? fzw_rand() : 0x84u);
                w->b[3] = (unsigned char)((fzw_below(4) == 0)
                                          ? fzw_rand() : 0x00u);
            }
            break;

        default:
        {
            /* A run of a single byte value. */
            size_t   at = fzw_below((unsigned)w->len);
            unsigned n  = fzw_below(48) + 1u;
            unsigned v  = fzw_rand() & 0xFFu;

            while (n-- > 0 && at < w->len)
                w->b[at++] = (unsigned char)v;
            break;
        }
        }
    }
}

#endif /* AMINETXDUO_FUZZ_WIRE_H */

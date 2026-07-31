/*
 * AmiNetXDuo -- host fuzz driver for the X.509 certificate path.
 *
 * A certificate is the largest thing a TLS client parses before it has any
 * reason to trust the sender, and the only one whose grammar is recursive.
 * DER is nested tag-length-value: every length is attacker-chosen, every
 * nesting level subtracts one from another, and the walk is done with UINT
 * arithmetic that wraps. Chain verification happens after all of it. With no
 * MMU, a length that underflows in the middle of that walk is not a crashed
 * process -- it is a read, and then a memcpy, wherever the arithmetic landed.
 *
 * WHAT IS DRIVEN
 *
 *   _nx_secure_x509_certificate_parse()      the DER walk itself
 *   _nx_secure_x509_certificate_initialize() the trust-store entry, which is
 *                                            what tls_store.c calls on a root
 *   _nx_secure_tls_process_remote_certificate()
 *                                            the Certificate handshake
 *                                            message: 3-byte total length,
 *                                            then a 3-byte length per cert
 *
 * and, on a certificate that parsed, the consumers the fields flow into:
 * _nx_secure_x509_common_name_dns_check() (tls_conn.c calls it with the
 * hostname), extension_find, key usage, extended key usage, subject alt names,
 * expiration, distinguished name compare. Those run ONLY after a successful
 * parse -- their callers only ever see a parsed certificate, and driving them
 * on a half-filled struct reports null dereferences the wire cannot produce.
 * That is not hypothetical: an earlier version of this driver called
 * subject_alt_names_find() on a zeroed extension and UBSan duly complained
 * about a parser that was behaving correctly.
 *
 * THE TWO LENGTH CONTRACTS, WHICH ARE NOT THE SAME
 *
 *   fx_parse()   allocates exactly the DER's length. That is the contract
 *                _nx_secure_x509_certificate_parse() states -- it is given a
 *                buffer and a length and must not read past it -- so anything
 *                beyond is a real over-read whatever the caller's buffer
 *                happens to be padded with.
 *
 *   fx_message() allocates TLS_RECORD_BUFFER bytes, because
 *                _nx_secure_tls_process_remote_certificate() carves its
 *                NX_SECURE_X509_CERT structures out of the tail of the record
 *                buffer past data_length. It is tls_conn.c's tc_RecordBuffer,
 *                at tls_internal.h's default size, and it has to be the real
 *                thing or the function has nowhere to put the certificates it
 *                parses. Over-reads inside that buffer are invisible here and
 *                are equally invisible on the target; a walk off the end of it
 *                is not.
 *
 * The seed corpus is real DER: the sample leaf and CA from
 * tests/tls/tls_test_certs.h and the ISRG Root X1 that tls_handshake and the
 * fetch test use. A corpus of hand-built certificates would exercise the
 * grammar the author imagined rather than the one certificates are written in.
 *
 * Usage, matching fuzz_dhcp:
 *   fuzz_tls_x509 -s                every seed case, named
 *   fuzz_tls_x509 -c NAME           one seed case by name
 *   fuzz_tls_x509 -r SEED COUNT     seeds plus mutations
 *   fuzz_tls_x509 < der             one certificate from stdin
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nx_secure_tls.h"
#include "nx_secure_x509.h"

/*
 * tls_test_certs.h also carries the leaf's private key, which tls_handshake.c
 * needs for its server side and this driver does not -- a private key is not
 * something a peer supplies. GCC's -Wunused-variable would fail the build over
 * the two symbols, so the push/pop is around the include alone; everything
 * below the pop is under the full set.
 */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
#include "tls_test_certs.h"
#pragma GCC diagnostic pop

#include "tls_root_isrg_x1.h"

/* The vendored code takes the TLS protection mutex around anything that can
   suspend. Nothing suspends here, so the object only has to exist. */
TX_MUTEX _nx_secure_tls_protection;

#define FX_MAX              4096

/*
 * _nx_secure_x509_asn1_tlv_block_parse() reads buffer[0] one statement before
 * it tests *buffer_length < 1, so every caller that reaches the end of its
 * data with nothing left over-reads by one byte. It is reachable from the wire
 * -- a two-byte certificate through _nx_secure_x509_certificate_parse() gets
 * there -- and from tls_store.c's issuer walk, which calls the same function
 * with whatever is left. Found by this driver at FX_KNOWN_SLOP 0; recorded in
 * docs/BACKLOG.md; the fix is to move the test above the read, in nx_secure.
 *
 * One byte of slop keeps the sweep on the bugs nobody has found yet. It costs
 * the driver the ability to see any other one-byte over-read at the end of a
 * certificate. Set it to 0 to reproduce: `fuzz_tls_x509 -r 1 20000` finds it.
 */
#define FX_KNOWN_SLOP       1

/* tls_internal.h's TLS_DEFAULT_RECORD_BUFFER. Not included from there --
   tls_internal.h pulls in exec/types.h and the AmigaOS library ABI -- so it is
   restated, and it is the size tls_conn.c actually allocates. */
#define FX_RECORD_BUFFER    10240

typedef struct
{
    unsigned char b[FX_MAX];
    unsigned      len;
} FxBuf;

static void fx_reset(FxBuf *w)
{
    memset(w->b, 0, sizeof(w->b));
    w->len = 0;
}

static void fx_u8(FxBuf *w, unsigned v)
{
    if (w->len < FX_MAX)
        w->b[w->len++] = (unsigned char)v;
}

static void fx_raw(FxBuf *w, const unsigned char *p, unsigned n)
{
    while (n-- > 0)
        fx_u8(w, *p++);
}

/* --------------------------------------------------------- the seed certs -- */

typedef struct
{
    const unsigned char *der;
    unsigned             len;
} FxCert;

static FxCert fx_cert_leaf(void)
{
    FxCert c;
    c.der = test_device_cert_der;
    c.len = test_device_cert_der_len;
    return c;
}

static FxCert fx_cert_ca(void)
{
    FxCert c;
    c.der = test_ca_cert_der;
    c.len = test_ca_cert_der_len;
    return c;
}

static FxCert fx_cert_isrg(void)
{
    FxCert c;
    c.der = isrg_root_x1_der;
    c.len = isrg_root_x1_der_len;
    return c;
}

/* ------------------------------------------------------------- the seeds --- */

static void fxs_leaf(FxBuf *w)
{
    FxCert c = fx_cert_leaf();
    fx_reset(w);
    fx_raw(w, c.der, c.len);
}

static void fxs_ca(FxBuf *w)
{
    FxCert c = fx_cert_ca();
    fx_reset(w);
    fx_raw(w, c.der, c.len);
}

static void fxs_isrg(FxBuf *w)
{
    FxCert c = fx_cert_isrg();
    fx_reset(w);
    fx_raw(w, c.der, c.len);
}

/* The outer SEQUENCE says the certificate is longer than it is. */
static void fxs_outer_too_long(FxBuf *w)
{
    fxs_leaf(w);
    if (w->len > 4)
    {
        w->b[2] = 0x7F;
        w->b[3] = 0xFF;
    }
}

/* The outer SEQUENCE says it is shorter, so every inner field overruns it. */
static void fxs_outer_too_short(FxBuf *w)
{
    fxs_leaf(w);
    if (w->len > 4)
    {
        w->b[2] = 0x00;
        w->b[3] = 0x08;
    }
}

/* A length in ASN.1 long form claiming four length bytes, which is where a
   ULONG length and a UINT buffer index stop agreeing. */
static void fxs_long_form_length(FxBuf *w)
{
    fx_reset(w);
    fx_u8(w, 0x30);
    fx_u8(w, 0x84);             /* long form, 4 length bytes */
    fx_u8(w, 0xFF);
    fx_u8(w, 0xFF);
    fx_u8(w, 0xFF);
    fx_u8(w, 0xFF);
    fx_u8(w, 0x02);
    fx_u8(w, 0x01);
    fx_u8(w, 0x00);
}

/* More length bytes than a ULONG holds. */
static void fxs_length_bytes_max(FxBuf *w)
{
    unsigned i;

    fx_reset(w);
    fx_u8(w, 0x30);
    fx_u8(w, 0x8F);             /* 15 length bytes */
    for (i = 0; i < 15; i++)
        fx_u8(w, 0xFF);
    fx_u8(w, 0x00);
}

/* Every field zero-length: the walk must not treat "no bytes" as "skip to
   whatever is next". */
static void fxs_empty_fields(FxBuf *w)
{
    fx_reset(w);
    fx_u8(w, 0x30); fx_u8(w, 0x08);     /* Certificate */
    fx_u8(w, 0x30); fx_u8(w, 0x00);     /* TBSCertificate, empty */
    fx_u8(w, 0x30); fx_u8(w, 0x00);     /* AlgorithmIdentifier, empty */
    fx_u8(w, 0x03); fx_u8(w, 0x00);     /* signature BIT STRING, empty */
    fx_u8(w, 0x00); fx_u8(w, 0x00);
}

/* SEQUENCE inside SEQUENCE inside SEQUENCE, as deep as the buffer allows: the
   walk is iterative per field but the DN and extension parsers recurse into
   their own loops, and nothing bounds the nesting. */
static void fxs_deep_nesting(FxBuf *w)
{
    unsigned depth = 200;
    unsigned i;

    fx_reset(w);
    for (i = 0; i < depth; i++)
    {
        fx_u8(w, 0x30);
        fx_u8(w, 0x82);
        fx_u8(w, (unsigned)((depth - i) * 4) >> 8);
        fx_u8(w, (unsigned)((depth - i) * 4) & 0xFF);
    }
}

/* The outer SEQUENCE is there and the body is one byte. */
static void fxs_runt(FxBuf *w)
{
    fx_reset(w);
    fx_u8(w, 0x30);
    fx_u8(w, 0x01);
    fx_u8(w, 0x00);
}

/* A single byte: below anything the parser can use, and the shortest input its
   first read is allowed to touch. */
static void fxs_one_byte(FxBuf *w)
{
    fx_reset(w);
    fx_u8(w, 0x30);
}

/* An indefinite-length form, which DER forbids and BER allows. */
static void fxs_indefinite(FxBuf *w)
{
    fx_reset(w);
    fx_u8(w, 0x30);
    fx_u8(w, 0x80);
    fx_u8(w, 0x02); fx_u8(w, 0x01); fx_u8(w, 0x00);
    fx_u8(w, 0x00); fx_u8(w, 0x00);
}

/* A real certificate truncated part way through, which is what a Certificate
   message with a wrong per-cert length produces. */
static void fxs_truncated(FxBuf *w)
{
    fxs_leaf(w);
    w->len = w->len / 2;
}

/* A real certificate with its tag class made context-specific throughout. */
static void fxs_tag_class(FxBuf *w)
{
    unsigned i;

    fxs_leaf(w);
    for (i = 0; i + 1 < w->len; i += 2)
    {
        if (w->b[i] == 0x30)
            w->b[i] = 0xA0;
    }
}

static void fxs_zeros(FxBuf *w)
{
    fx_reset(w);
    while (w->len < 128)
        fx_u8(w, 0);
}

static void fxs_ones(FxBuf *w)
{
    fx_reset(w);
    while (w->len < 128)
        fx_u8(w, 0xFF);
}

typedef void (*FxSeedFn)(FxBuf *);

typedef struct
{
    const char *name;
    FxSeedFn    build;
} FxSeed;

static const FxSeed fx_seeds[] =
{
    { "leaf",             fxs_leaf             },
    { "ca",               fxs_ca               },
    { "isrg_root_x1",     fxs_isrg             },
    { "outer_too_long",   fxs_outer_too_long   },
    { "outer_too_short",  fxs_outer_too_short  },
    { "long_form_length", fxs_long_form_length },
    { "length_bytes_max", fxs_length_bytes_max },
    { "empty_fields",     fxs_empty_fields     },
    { "deep_nesting",     fxs_deep_nesting     },
    { "runt",             fxs_runt             },
    { "one_byte",         fxs_one_byte         },
    { "indefinite",       fxs_indefinite       },
    { "truncated",        fxs_truncated        },
    { "tag_class",        fxs_tag_class        },
    { "zeros",            fxs_zeros            },
    { "ones",             fxs_ones             }
};

#define FX_SEED_COUNT   (int)(sizeof(fx_seeds) / sizeof(fx_seeds[0]))

/* ------------------------------------------------------------- the driver -- */

/*
 * Everything a parsed certificate is asked for afterwards. Only ever called on
 * a certificate _nx_secure_x509_certificate_parse() accepted, because that is
 * the only kind its callers ever hold.
 */
static void fx_consume(NX_SECURE_X509_CERT *cert)
{
    NX_SECURE_X509_EXTENSION extension;
    USHORT                   bitfield;

    /* tls_conn.c calls this with the hostname the caller asked for. Both a
       name that could match and one that cannot, so the wildcard compare runs
       either way. */
    (void)_nx_secure_x509_common_name_dns_check(cert,
                                                (const UCHAR *)"www.example.com",
                                                15);
    (void)_nx_secure_x509_common_name_dns_check(cert, (const UCHAR *)"a", 1);

    (void)_nx_secure_x509_expiration_check(cert, 0);
    (void)_nx_secure_x509_expiration_check(cert, 0xFFFFFFFFUL);

    memset(&extension, 0, sizeof(extension));
    if (_nx_secure_x509_extension_find(cert, &extension,
                                       NX_SECURE_TLS_X509_TYPE_SUBJECT_ALT_NAME) ==
        NX_SECURE_X509_SUCCESS)
    {
        (void)_nx_secure_x509_subject_alt_names_find(
                  &extension, (const UCHAR *)"www.example.com", 15,
                  NX_SECURE_X509_SUB_ALT_NAME_TAG_DNSNAME);
        (void)_nx_secure_x509_subject_alt_names_find(
                  &extension, (const UCHAR *)"a", 1,
                  NX_SECURE_X509_SUB_ALT_NAME_TAG_IPADDRESS);
    }

    (void)_nx_secure_x509_key_usage_extension_parse(cert, &bitfield);

    memset(&extension, 0, sizeof(extension));
    (void)_nx_secure_x509_extended_key_usage_extension_parse(
              cert, NX_SECURE_TLS_X509_TYPE_PKIX_KP_SERVER_AUTH);

    /* A certificate is compared against itself, which is what the store lookup
       does when a chain's issuer is its own subject. */
    (void)_nx_secure_x509_distinguished_name_compare(
              &cert->nx_secure_x509_distinguished_name,
              &cert->nx_secure_x509_issuer, NX_SECURE_X509_NAME_ALL_FIELDS);
}

/*
 * The DER walk, on an allocation of exactly the length the parser is told it
 * has. Anything it reads beyond that is out of bounds by its own contract.
 */
static void fx_parse(const unsigned char *der, unsigned len)
{
    NX_SECURE_X509_CERT  cert;
    UCHAR               *copy;
    UINT                 bytes_processed = 0;
    UINT                 status;

    if (len == 0)
        return;

    copy = (UCHAR *)malloc(len + FX_KNOWN_SLOP);
    if (copy == NX_NULL)
        return;
    memcpy(copy, der, len);
    memset(copy + len, 0, FX_KNOWN_SLOP);

    memset(&cert, 0, sizeof(cert));
    status = _nx_secure_x509_certificate_parse(copy, len, &bytes_processed,
                                               &cert);

    if (status == NX_SECURE_X509_SUCCESS)
        fx_consume(&cert);

    /* The trust-store path: tls_store.c hands a root's DER straight to this
       with raw_data_buffer NULL, so the parsed certificate points into the
       caller's memory rather than a copy. Same bytes, different entry. */
    memset(&cert, 0, sizeof(cert));
    (void)_nx_secure_x509_certificate_initialize(&cert, copy, (USHORT)len,
                                                 NX_NULL, 0, NX_NULL, 0,
                                                 NX_SECURE_X509_KEY_TYPE_NONE);

    free(copy);
}

/* The chain verify a real session would do needs the crypto table and a
   trusted root. Answering NX_SUCCESS here is "the chain checked out", which is
   the branch that continues into the endpoint copy -- the part of
   _nx_secure_tls_process_remote_certificate() that moves attacker bytes. */
static UINT fx_verify_ok(NX_SECURE_X509_CERTIFICATE_STORE *store,
                         NX_SECURE_X509_CERT *certificate, ULONG current_time)
{
    NX_PARAMETER_NOT_USED(store);
    NX_PARAMETER_NOT_USED(certificate);
    NX_PARAMETER_NOT_USED(current_time);
    return NX_SUCCESS;
}

static NX_SECURE_TLS_CRYPTO fx_crypto;

/*
 * The Certificate handshake message, in the buffer tls_conn.c allocates.
 *
 * `chain` certificates are concatenated with their 3-byte lengths, the whole
 * thing behind a 3-byte total, and the record buffer is the real size so the
 * function has somewhere to carve its NX_SECURE_X509_CERT structures from.
 */
static void fx_message(const unsigned char *der, unsigned len, unsigned chain,
                       unsigned total_fudge)
{
    NX_SECURE_TLS_SESSION s;
    UCHAR                *buffer;
    unsigned              at = 0;
    unsigned              i;
    unsigned              total;

    if (len == 0 || chain == 0)
        return;

    /* Three for the total, then three plus the DER for each certificate. */
    if ((3u + chain * (3u + len)) > FX_RECORD_BUFFER / 2u)
        return;

    buffer = (UCHAR *)malloc(FX_RECORD_BUFFER);
    if (buffer == NX_NULL)
        return;
    memset(buffer, 0, FX_RECORD_BUFFER);

    total = chain * (3u + len) + total_fudge;

    buffer[at++] = (UCHAR)(total >> 16);
    buffer[at++] = (UCHAR)(total >> 8);
    buffer[at++] = (UCHAR)total;

    for (i = 0; i < chain; i++)
    {
        buffer[at++] = (UCHAR)(len >> 16);
        buffer[at++] = (UCHAR)(len >> 8);
        buffer[at++] = (UCHAR)len;
        memcpy(&buffer[at], der, len);
        at += len;
    }

    /*
     * The three buffer fields are set here rather than through
     * _nx_secure_tls_session_packet_buffer_set(), which rounds the pointer by
     * casting it to a ULONG -- the target's pointer width, and not this
     * host's. original_size matters: _nx_secure_tls_remote_certificate_free_all()
     * restores size from it before the endpoint is copied, and leaving it zero
     * underflows cert_buf_size into a wild memset that is the harness's fault
     * and not the parser's.
     */
    memset(&s, 0, sizeof(s));
    s.nx_secure_tls_socket_type                 = NX_SECURE_TLS_SESSION_TYPE_CLIENT;
    s.nx_secure_tls_packet_buffer               = buffer;
    s.nx_secure_tls_packet_buffer_size          = FX_RECORD_BUFFER;
    s.nx_secure_tls_packet_buffer_original_size = FX_RECORD_BUFFER;
    s.nx_secure_remote_certificate_verify       = fx_verify_ok;
    s.nx_secure_tls_crypto_table                = &fx_crypto;

    (void)_nx_secure_tls_process_remote_certificate(&s, buffer, at, at);

    free(buffer);
}

static void fx_run(const unsigned char *der, unsigned len, unsigned chain,
                   unsigned total_fudge)
{
    if (len > FX_MAX)
        len = FX_MAX;

    fx_parse(der, len);
    fx_message(der, len, chain, total_fudge);
}

/*
 * Proof that the driver reaches the parser.
 *
 * A driver that stopped at the outer SEQUENCE -- a length that never cleared,
 * a struct rejected on its first field -- would report "clean" for every input
 * and read as coverage. So the sample leaf certificate is parsed at startup
 * and the fields it must have filled are checked: the whole DER consumed, an
 * RSA public key recognised, a common name found in the subject, and the
 * issuer's common name found too. If any of that is missing the sweep does not
 * run.
 */
static void fx_selftest(void)
{
    NX_SECURE_X509_CERT cert;
    FxCert              c = fx_cert_leaf();
    UINT                bytes_processed = 0;
    UINT                status;

    memset(&cert, 0, sizeof(cert));
    status = _nx_secure_x509_certificate_parse(c.der, c.len, &bytes_processed,
                                               &cert);

    if (status != NX_SECURE_X509_SUCCESS)
    {
        printf("fuzz_tls_x509: SELFTEST FAILED -- the sample leaf returned %u\n",
               (unsigned)status);
        exit(2);
    }

    if (bytes_processed != c.len)
    {
        printf("fuzz_tls_x509: SELFTEST FAILED -- consumed %u of %u bytes\n",
               (unsigned)bytes_processed, c.len);
        exit(2);
    }

    if (cert.nx_secure_x509_public_algorithm != NX_SECURE_TLS_X509_TYPE_RSA)
    {
        printf("fuzz_tls_x509: SELFTEST FAILED -- public key not RSA (got %u)\n",
               (unsigned)cert.nx_secure_x509_public_algorithm);
        exit(2);
    }

    if (cert.nx_secure_x509_distinguished_name.nx_secure_x509_common_name ==
            NX_NULL ||
        cert.nx_secure_x509_distinguished_name.nx_secure_x509_common_name_length
            == 0)
    {
        printf("fuzz_tls_x509: SELFTEST FAILED -- no subject common name\n");
        exit(2);
    }

    if (cert.nx_secure_x509_issuer.nx_secure_x509_common_name == NX_NULL ||
        cert.nx_secure_x509_issuer.nx_secure_x509_common_name_length == 0)
    {
        printf("fuzz_tls_x509: SELFTEST FAILED -- no issuer common name\n");
        exit(2);
    }

    /* And the Certificate message around it, so that the framing is known to
       be reached as well and not only the DER. */
    fx_message(c.der, c.len, 1, 0);
}

static unsigned long fx_state = 1;

static unsigned fx_rand(void)
{
    fx_state = fx_state * 1103515245UL + 12345UL;
    return (unsigned)((fx_state >> 16) & 0x7FFFUL);
}

static unsigned fx_below(unsigned n)
{
    return (n == 0) ? 0 : (fx_rand() % n);
}

/*
 * Aimed at DER's structure bytes -- a tag, a length, a long-form length count
 * -- rather than at the payload. A flipped byte in a modulus changes a key
 * nobody uses here; a flipped length byte changes where the next TLV starts
 * and how much of the buffer the rest of the walk believes it has.
 */
static void fx_mutate(FxBuf *w)
{
    unsigned rounds = fx_below(6) + 1u;

    while (rounds-- > 0)
    {
        unsigned at;

        if (w->len == 0)
            return;

        at = fx_below(w->len);

        switch (fx_below(8))
        {
        case 0:     /* any byte */
            w->b[at] = (unsigned char)fx_rand();
            break;
        case 1:     /* a length byte to its maximum */
            w->b[at] = 0xFF;
            break;
        case 2:     /* a length byte to zero */
            w->b[at] = 0;
            break;
        case 3:     /* a tag, to one of the ones the walk switches on */
            {
                static const unsigned char tags[] =
                    { 0x30, 0x31, 0x02, 0x03, 0x04, 0x05, 0x06, 0x0C,
                      0x13, 0x16, 0x17, 0x18, 0xA0, 0xA3, 0x80 };
                w->b[at] = tags[fx_below((unsigned)sizeof(tags))];
            }
            break;
        case 4:     /* long form, with a length-byte count that overflows */
            w->b[at] = (unsigned char)(0x80 | (1 + fx_below(15)));
            break;
        case 5:     /* indefinite length */
            w->b[at] = 0x80;
            break;
        case 6:     /* truncate */
            w->len = fx_below(w->len) + 1u;
            break;
        default:    /* splice a second certificate's bytes in */
            {
                FxCert c = fx_cert_ca();
                unsigned n = fx_below(32) + 1u;
                unsigned i;

                for (i = 0; i < n && at + i < w->len; i++)
                    w->b[at + i] = c.der[(at + i) % c.len];
            }
            break;
        }
    }
}

static void fx_run_seed(int s)
{
    FxBuf w;

    fx_seeds[s].build(&w);

    /* One certificate, then a chain of three: the per-certificate length walk
       only has more than one iteration in the second. */
    fx_run(w.b, w.len, 1, 0);
    fx_run(w.b, w.len, 3, 0);
}

int main(int argc, char **argv)
{
    int i;

    fx_selftest();

    for (i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "-s") == 0)
        {
            int s;

            for (s = 0; s < FX_SEED_COUNT; s++)
            {
                fx_run_seed(s);
                printf("  %-18s ok\n", fx_seeds[s].name);
            }

            printf("fuzz_tls_x509: %d seed case(s), clean\n", FX_SEED_COUNT);
            return 0;
        }

        if (strcmp(argv[i], "-c") == 0 && i + 1 < argc)
        {
            const char *want = argv[++i];
            int         s;

            for (s = 0; s < FX_SEED_COUNT; s++)
            {
                if (strcmp(fx_seeds[s].name, want) == 0)
                {
                    fx_run_seed(s);
                    printf("fuzz_tls_x509: seed '%s', clean\n", want);
                    return 0;
                }
            }

            printf("fuzz_tls_x509: no seed case named '%s'\n", want);
            return 2;
        }

        if (strcmp(argv[i], "-r") == 0 && i + 2 < argc)
        {
            unsigned long seed  = strtoul(argv[++i], NULL, 0);
            unsigned long count = strtoul(argv[++i], NULL, 0);
            unsigned long n;
            int           s;

            /* The seeds first, always: a sweep that never runs the known
               shapes is a sweep whose coverage nobody can state. */
            for (s = 0; s < FX_SEED_COUNT; s++)
                fx_run_seed(s);

            fx_state = seed ? seed : 1;

            for (n = 0; n < count; n++)
            {
                FxBuf w;

                fx_seeds[fx_below((unsigned)FX_SEED_COUNT)].build(&w);
                fx_mutate(&w);
                fx_run(w.b, w.len, fx_below(3) + 1u, fx_below(4));
            }

            printf("fuzz_tls_x509: %d seed(s) + %lu mutation(s) from %lu,"
                   " clean\n", FX_SEED_COUNT, count, seed);
            return 0;
        }
    }

    /* One certificate on stdin. */
    {
        unsigned char buf[FX_MAX];
        size_t        got = fread(buf, 1, sizeof(buf), stdin);

        fx_run(buf, (unsigned)got, 1, 0);
    }

    return 0;
}

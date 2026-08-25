/*
 * AmiNetXDuo, host fuzz driver for the TLS record and handshake layers.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nx_secure_tls.h"

/* The vendored parsers take the TLS protection mutex around anything that can
   suspend. Nothing suspends here, no thread runs, so the object only has
   to exist. */
TX_MUTEX _nx_secure_tls_protection;

#define FR_MAX          4096
#define FR_HDR          NX_SECURE_TLS_RECORD_HEADER_SIZE

#define FR_KNOWN_SLOP   0

typedef struct
{
    unsigned char b[FR_MAX];
    unsigned      len;
} FrBuf;

static void fr_reset(FrBuf *w)
{
    memset(w->b, 0, sizeof(w->b));
    w->len = 0;
}

static void fr_u8(FrBuf *w, unsigned v)
{
    if (w->len < FR_MAX)
        w->b[w->len++] = (unsigned char)v;
}

static void fr_u16(FrBuf *w, unsigned v)
{
    fr_u8(w, v >> 8);
    fr_u8(w, v);
}

static void fr_u24(FrBuf *w, unsigned long v)
{
    fr_u8(w, (unsigned)(v >> 16));
    fr_u8(w, (unsigned)(v >> 8));
    fr_u8(w, (unsigned)v);
}

static void fr_bytes(FrBuf *w, unsigned n, unsigned char fill)
{
    while (n-- > 0)
        fr_u8(w, fill);
}

static void fr_patch16(FrBuf *w, unsigned at, unsigned v)
{
    if (at + 1 < FR_MAX)
    {
        w->b[at]     = (unsigned char)(v >> 8);
        w->b[at + 1] = (unsigned char)v;
    }
}

static void fr_patch24(FrBuf *w, unsigned at, unsigned long v)
{
    if (at + 2 < FR_MAX)
    {
        w->b[at]     = (unsigned char)(v >> 16);
        w->b[at + 1] = (unsigned char)(v >> 8);
        w->b[at + 2] = (unsigned char)v;
    }
}

static void fr_record(FrBuf *w, unsigned type)
{
    fr_reset(w);
    fr_u8(w, type);
    fr_u8(w, 0x03);
    fr_u8(w, 0x03);             /* TLS 1.2 */
    fr_u16(w, 0);               /* patched by fr_seal */
}

static void fr_seal(FrBuf *w)
{
    fr_patch16(w, 3, w->len - FR_HDR);
}

static unsigned fr_msg(FrBuf *w, unsigned type)
{
    unsigned at;

    fr_u8(w, type);
    at = w->len;
    fr_u24(w, 0);
    return at;
}

static void fr_msg_end(FrBuf *w, unsigned at)
{
    fr_patch24(w, at, w->len - (at + 3));
}

/* Numeric rather than included from src/tls/ami_tls_crypto.h, which pulls in
   exec/types.h. RFC 7905. */
#define FR_ECDHE_RSA_CHACHA20_POLY1305      0xCCA8
#define FR_ECDHE_ECDSA_CHACHA20_POLY1305    0xCCA9

static NX_SECURE_TLS_CIPHERSUITE_INFO fr_suites[] =
{
    { FR_ECDHE_ECDSA_CHACHA20_POLY1305,
      NX_NULL, NX_NULL, NX_NULL, 12, 32, NX_NULL,  0, NX_NULL },
    { FR_ECDHE_RSA_CHACHA20_POLY1305,
      NX_NULL, NX_NULL, NX_NULL, 12, 32, NX_NULL,  0, NX_NULL },
    { TLS_ECDHE_ECDSA_WITH_AES_128_CBC_SHA256,
      NX_NULL, NX_NULL, NX_NULL, 16, 16, NX_NULL, 32, NX_NULL },
    { TLS_ECDHE_RSA_WITH_AES_128_CBC_SHA256,
      NX_NULL, NX_NULL, NX_NULL, 16, 16, NX_NULL, 32, NX_NULL }
};

#define FR_SUITE_DEFAULT    TLS_ECDHE_RSA_WITH_AES_128_CBC_SHA256

static NX_SECURE_TLS_CRYPTO fr_crypto;

static void fr_session_init(NX_SECURE_TLS_SESSION *s)
{
    memset(s, 0, sizeof(*s));

    fr_crypto.nx_secure_tls_ciphersuite_lookup_table      = fr_suites;
    fr_crypto.nx_secure_tls_ciphersuite_lookup_table_size =
        sizeof(fr_suites) / sizeof(fr_suites[0]);

    s->nx_secure_tls_crypto_table = &fr_crypto;
    s->nx_secure_tls_socket_type  = NX_SECURE_TLS_SESSION_TYPE_CLIENT;

    /* Where a client is when the server's first flight arrives: the
       ClientHello has gone out and nothing has come back. */
    s->nx_secure_tls_client_state = NX_SECURE_TLS_CLIENT_STATE_HELLO_REQUEST;
}

static void frs_hello_body(FrBuf *w, unsigned sid_len, unsigned suite)
{
    unsigned i;

    fr_u16(w, 0x0303);
    for (i = 0; i < NX_SECURE_TLS_RANDOM_SIZE; i++)
        fr_u8(w, i);
    fr_u8(w, sid_len);
    for (i = 0; i < sid_len; i++)
        fr_u8(w, 0xA0 + i);
    fr_u16(w, suite);
    fr_u8(w, 0);                /* compression: null */
}

static void frs_serverhello(FrBuf *w)
{
    unsigned at;

    fr_record(w, NX_SECURE_TLS_HANDSHAKE);
    at = fr_msg(w, NX_SECURE_TLS_SERVER_HELLO);
    frs_hello_body(w, 32, FR_SUITE_DEFAULT);
    fr_u16(w, 0);               /* no extensions */
    fr_msg_end(w, at);
    fr_seal(w);
}

static void frs_extensions(FrBuf *w)
{
    unsigned at;
    unsigned ext_at;

    fr_record(w, NX_SECURE_TLS_HANDSHAKE);
    at = fr_msg(w, NX_SECURE_TLS_SERVER_HELLO);
    frs_hello_body(w, 0, FR_SUITE_DEFAULT);

    ext_at = w->len;
    fr_u16(w, 0);

    fr_u16(w, NX_SECURE_TLS_EXTENSION_SECURE_RENEGOTIATION);
    fr_u16(w, 1);
    fr_u8(w, 0);                /* renegotiated_connection: empty */

    fr_u16(w, NX_SECURE_TLS_EXTENSION_EC_POINT_FORMATS);
    fr_u16(w, 2);
    fr_u8(w, 1);
    fr_u8(w, 0);

    fr_u16(w, NX_SECURE_TLS_EXTENSION_SERVER_NAME_INDICATION);
    fr_u16(w, 0);

    fr_patch16(w, ext_at, w->len - (ext_at + 2));
    fr_msg_end(w, at);
    fr_seal(w);
}

/* An extension whose length runs past the extension block, which must be
   bounded by the block and not by what the extension claims. */
static void frs_ext_len_past_end(FrBuf *w)
{
    unsigned at;
    unsigned ext_at;

    fr_record(w, NX_SECURE_TLS_HANDSHAKE);
    at = fr_msg(w, NX_SECURE_TLS_SERVER_HELLO);
    frs_hello_body(w, 0, FR_SUITE_DEFAULT);

    ext_at = w->len;
    fr_u16(w, 0);
    fr_u16(w, NX_SECURE_TLS_EXTENSION_SECURE_RENEGOTIATION);
    fr_u16(w, 0xFFF0);          /* 65520 promised, 4 present */
    fr_bytes(w, 4, 0x5A);
    fr_patch16(w, ext_at, w->len - (ext_at + 2));

    fr_msg_end(w, at);
    fr_seal(w);
}

static void frs_ext_block_overrun(FrBuf *w)
{
    unsigned at;
    unsigned ext_at;

    fr_record(w, NX_SECURE_TLS_HANDSHAKE);
    at = fr_msg(w, NX_SECURE_TLS_SERVER_HELLO);
    frs_hello_body(w, 0, FR_SUITE_DEFAULT);
    ext_at = w->len;
    fr_u16(w, 0x0FFF);
    fr_bytes(w, 8, 0x33);
    (void)ext_at;
    fr_msg_end(w, at);
    fr_seal(w);
}

static void frs_sid_max(FrBuf *w)
{
    unsigned at;

    fr_record(w, NX_SECURE_TLS_HANDSHAKE);
    at = fr_msg(w, NX_SECURE_TLS_SERVER_HELLO);
    frs_hello_body(w, 255, FR_SUITE_DEFAULT);
    fr_u16(w, 0);
    fr_msg_end(w, at);
    fr_seal(w);
}

static void frs_sh_min(FrBuf *w)
{
    unsigned at;

    fr_record(w, NX_SECURE_TLS_HANDSHAKE);
    at = fr_msg(w, NX_SECURE_TLS_SERVER_HELLO);
    frs_hello_body(w, 0, FR_SUITE_DEFAULT);
    fr_msg_end(w, at);
    fr_seal(w);
}

static void frs_sh_unknown_suite(FrBuf *w)
{
    unsigned at;

    fr_record(w, NX_SECURE_TLS_HANDSHAKE);
    at = fr_msg(w, NX_SECURE_TLS_SERVER_HELLO);
    frs_hello_body(w, 4, 0xDEAD);
    fr_u16(w, 0);
    fr_msg_end(w, at);
    fr_seal(w);
}

/* Four messages in one record: the walk has to find each header at the right
   offset and stop at the record, not at the first message that looks whole. */
static void frs_multi_message(FrBuf *w)
{
    unsigned at;

    fr_record(w, NX_SECURE_TLS_HANDSHAKE);

    at = fr_msg(w, NX_SECURE_TLS_SERVER_HELLO);
    frs_hello_body(w, 8, FR_SUITE_DEFAULT);
    fr_u16(w, 0);
    fr_msg_end(w, at);

    at = fr_msg(w, NX_SECURE_TLS_CERTIFICATE_REQUEST);
    fr_u8(w, 1);                /* one certificate type */
    fr_u8(w, 1);                /* rsa_sign */
    fr_u16(w, 2);               /* signature algorithms length */
    fr_u16(w, 0x0401);          /* rsa_pkcs1_sha256 */
    fr_u16(w, 0);               /* no CA names */
    fr_msg_end(w, at);

    at = fr_msg(w, NX_SECURE_TLS_NEW_SESSION_TICKET);
    fr_u8(w, 0); fr_u8(w, 0); fr_u8(w, 0x0E); fr_u8(w, 0x10);  /* lifetime */
    fr_u16(w, 16);
    fr_bytes(w, 16, 0x77);
    fr_msg_end(w, at);

    at = fr_msg(w, NX_SECURE_TLS_SERVER_HELLO_DONE);
    fr_msg_end(w, at);

    fr_seal(w);
}

/* A message header that claims more than the record holds, the fragment
   path, which must not read the tail it was promised. */
static void frs_msg_past_record(FrBuf *w)
{
    unsigned at;

    fr_record(w, NX_SECURE_TLS_HANDSHAKE);
    at = fr_msg(w, NX_SECURE_TLS_SERVER_HELLO);
    frs_hello_body(w, 0, FR_SUITE_DEFAULT);
    fr_msg_end(w, at);
    fr_patch24(w, at, 0xFFFFFF);
    fr_seal(w);
}

static void frs_ticket_overrun(FrBuf *w)
{
    unsigned at;

    fr_record(w, NX_SECURE_TLS_HANDSHAKE);
    at = fr_msg(w, NX_SECURE_TLS_NEW_SESSION_TICKET);
    fr_bytes(w, 4, 0);
    fr_u16(w, 0xFFFF);
    fr_bytes(w, 4, 0x11);
    fr_msg_end(w, at);
    fr_seal(w);
}

static void frs_certreq_max(FrBuf *w)
{
    unsigned at;

    fr_record(w, NX_SECURE_TLS_HANDSHAKE);
    at = fr_msg(w, NX_SECURE_TLS_CERTIFICATE_REQUEST);
    fr_u8(w, 255);
    fr_bytes(w, 8, 1);
    fr_u16(w, 0xFFFF);
    fr_bytes(w, 8, 4);
    fr_u16(w, 0xFFFF);
    fr_msg_end(w, at);
    fr_seal(w);
}

static void frs_header_only(FrBuf *w)
{
    fr_record(w, NX_SECURE_TLS_HANDSHAKE);
    fr_seal(w);
}

/* Fewer bytes than a record header: the parser must ask for more, not read
   what is not there. */
static void frs_short_header(FrBuf *w)
{
    fr_reset(w);
    fr_u8(w, NX_SECURE_TLS_HANDSHAKE);
    fr_u8(w, 0x03);
    fr_u8(w, 0x03);
}

static void frs_len_past_end(FrBuf *w)
{
    unsigned at;

    fr_record(w, NX_SECURE_TLS_HANDSHAKE);
    at = fr_msg(w, NX_SECURE_TLS_SERVER_HELLO);
    frs_hello_body(w, 0, FR_SUITE_DEFAULT);
    fr_msg_end(w, at);
    fr_patch16(w, 3, 0x4000);
}

static void frs_bad_version(FrBuf *w)
{
    frs_serverhello(w);
    w->b[1] = 0x09;
    w->b[2] = 0x99;
}

static void frs_alert(FrBuf *w)
{
    fr_record(w, NX_SECURE_TLS_ALERT);
    fr_u8(w, 2);                /* fatal */
    fr_u8(w, 40);               /* handshake_failure */
    fr_seal(w);
}

static void frs_changecipherspec(FrBuf *w)
{
    fr_record(w, NX_SECURE_TLS_CHANGE_CIPHER_SPEC);
    fr_u8(w, 1);
    fr_seal(w);
}

static void frs_bad_type(FrBuf *w)
{
    frs_serverhello(w);
    w->b[0] = 0xFF;
}

static void frs_zeros(FrBuf *w)
{
    fr_reset(w);
    fr_bytes(w, 64, 0);
}

static void frs_ones(FrBuf *w)
{
    fr_reset(w);
    fr_bytes(w, 64, 0xFF);
}

typedef void (*FrSeedFn)(FrBuf *);

typedef struct
{
    const char *name;
    FrSeedFn    build;
} FrSeed;

static const FrSeed fr_seeds[] =
{
    { "serverhello",       frs_serverhello       },
    { "extensions",        frs_extensions        },
    { "ext_len_past_end",  frs_ext_len_past_end  },
    { "ext_block_overrun", frs_ext_block_overrun },
    { "sid_max",           frs_sid_max           },
    { "sh_min",            frs_sh_min            },
    { "sh_unknown_suite",  frs_sh_unknown_suite  },
    { "multi_message",     frs_multi_message     },
    { "msg_past_record",   frs_msg_past_record   },
    { "ticket_overrun",    frs_ticket_overrun    },
    { "certreq_max",       frs_certreq_max       },
    { "header_only",       frs_header_only       },
    { "short_header",      frs_short_header      },
    { "len_past_end",      frs_len_past_end      },
    { "bad_version",       frs_bad_version       },
    { "alert",             frs_alert             },
    { "changecipherspec",  frs_changecipherspec  },
    { "bad_type",          frs_bad_type          },
    { "zeros",             frs_zeros             },
    { "ones",              frs_ones              }
};

#define FR_SEED_COUNT   (int)(sizeof(fr_seeds) / sizeof(fr_seeds[0]))

static void fr_dispatch(NX_SECURE_TLS_SESSION *s, UCHAR *data, unsigned len)
{
    UCHAR   *msg    = data;
    unsigned left   = len;

    while (left > 0)
    {
        USHORT   type;
        UINT     header_bytes = left;
        UINT     message_length;

        if (_nx_secure_tls_process_handshake_header(msg, &type, &header_bytes,
                                                    &message_length) != NX_SUCCESS)
        {
            return;
        }

        /* Fragmented across records; the reassembly state machine owns it and
           this driver does not reproduce that. */
        if ((message_length + header_bytes) > left)
            return;

        switch (type)
        {
        case NX_SECURE_TLS_SERVER_HELLO:
            (void)_nx_secure_tls_process_serverhello(s, msg + header_bytes,
                                                     message_length);
            break;

        case NX_SECURE_TLS_CERTIFICATE_REQUEST:
            (void)_nx_secure_tls_process_certificate_request(s,
                                                             msg + header_bytes,
                                                             message_length);
            break;

        default:
            break;
        }

        msg  += header_bytes + message_length;
        left -= header_bytes + message_length;
    }
}

static UINT fr_record_header(NX_SECURE_TLS_SESSION *s, const unsigned char *msg,
                             unsigned len, unsigned split, USHORT *type,
                             UINT *length)
{
    NX_PACKET     pkt[2];
    unsigned char first[FR_MAX];
    unsigned char second[FR_MAX];
    UCHAR         header[FR_HDR];
    USHORT        header_length;
    unsigned      n;

    if (len == 0)
        return NX_SECURE_TLS_INVALID_PACKET;

    n = (split == 0 || split >= len) ? len : split;

    memset(pkt, 0, sizeof(pkt));
    memcpy(first, msg, n);

    pkt[0].nx_packet_prepend_ptr = first;
    pkt[0].nx_packet_append_ptr  = first + n;
    pkt[0].nx_packet_length      = len;
    pkt[0].nx_packet_last        = &pkt[0];

    if (n < len)
    {
        memcpy(second, msg + n, len - n);
        pkt[1].nx_packet_prepend_ptr = second;
        pkt[1].nx_packet_append_ptr  = second + (len - n);
        pkt[1].nx_packet_length      = len - n;
        pkt[0].nx_packet_next        = &pkt[1];
        pkt[0].nx_packet_last        = &pkt[1];
    }

    memset(header, 0, sizeof(header));

    return _nx_secure_tls_process_header(s, &pkt[0], 0, type, length,
                                         header, &header_length);
}

static void fr_run(const unsigned char *msg, unsigned len, unsigned split)
{
    NX_SECURE_TLS_SESSION s;
    USHORT                type   = 0;
    UINT                  length = 0;
    UCHAR                *payload;
    unsigned              have;

    if (len > FR_MAX)
        len = FR_MAX;

    fr_session_init(&s);

    if (fr_record_header(&s, msg, len, split, &type, &length) != NX_SUCCESS)
        return;

    if (type != NX_SECURE_TLS_HANDSHAKE || length == 0)
        return;

    /* A record header can promise more than arrived; the payload is what is
       here, which is what the record buffer would hold. */
    have = len - FR_HDR;
    if (length < have)
        have = length;
    if (have == 0)
        return;

    payload = (UCHAR *)malloc(have + FR_KNOWN_SLOP);
    if (payload == NX_NULL)
        return;

    memcpy(payload, msg + FR_HDR, have);
    memset(payload + have, 0, FR_KNOWN_SLOP);

    fr_dispatch(&s, payload, have);

    free(payload);
}

static void fr_selftest(void)
{
    NX_SECURE_TLS_SESSION s;
    FrBuf                 w;
    UCHAR                *payload;
    USHORT                type   = 0;
    UINT                  length = 0;
    UINT                  status;

    frs_serverhello(&w);

    fr_session_init(&s);

    status = fr_record_header(&s, w.b, w.len, 0, &type, &length);
    if (status != NX_SUCCESS)
    {
        printf("fuzz_tls_record: SELFTEST FAILED, record header returned %u\n",
               (unsigned)status);
        exit(2);
    }

    if (type != NX_SECURE_TLS_HANDSHAKE || length != w.len - FR_HDR)
    {
        printf("fuzz_tls_record: SELFTEST FAILED, header gave type %u len %u,"
               " expected %u/%u\n",
               (unsigned)type, (unsigned)length,
               (unsigned)NX_SECURE_TLS_HANDSHAKE, w.len - FR_HDR);
        exit(2);
    }

    payload = (UCHAR *)malloc(length + FR_KNOWN_SLOP);
    if (payload == NX_NULL)
    {
        printf("fuzz_tls_record: SELFTEST FAILED, out of memory\n");
        exit(2);
    }
    memcpy(payload, w.b + FR_HDR, length);
    memset(payload + length, 0, FR_KNOWN_SLOP);

    fr_dispatch(&s, payload, length);
    free(payload);

    if (s.nx_secure_tls_protocol_version != NX_SECURE_TLS_VERSION_TLS_1_2)
    {
        printf("fuzz_tls_record: SELFTEST FAILED, version not negotiated"
               " (got %04x)\n", (unsigned)s.nx_secure_tls_protocol_version);
        exit(2);
    }

    if (s.nx_secure_tls_session_id_length != 32)
    {
        printf("fuzz_tls_record: SELFTEST FAILED, session id not stored"
               " (len %u)\n", (unsigned)s.nx_secure_tls_session_id_length);
        exit(2);
    }

    if (s.nx_secure_tls_session_ciphersuite == NX_NULL ||
        s.nx_secure_tls_session_ciphersuite->nx_secure_tls_ciphersuite !=
            FR_SUITE_DEFAULT)
    {
        printf("fuzz_tls_record: SELFTEST FAILED, ciphersuite not chosen\n");
        exit(2);
    }
}

static unsigned long fr_state = 1;

static unsigned fr_rand(void)
{
    fr_state = fr_state * 1103515245UL + 12345UL;
    return (unsigned)((fr_state >> 16) & 0x7FFFUL);
}

static unsigned fr_below(unsigned n)
{
    return (n == 0) ? 0 : (fr_rand() % n);
}

static void fr_mutate(FrBuf *w)
{
    unsigned rounds = fr_below(6) + 1u;

    while (rounds-- > 0)
    {
        unsigned at;

        if (w->len == 0)
            return;

        switch (fr_below(8))
        {
        case 0:     /* any byte */
            w->b[fr_below(w->len)] = (unsigned char)fr_rand();
            break;
        case 1:     /* a byte to its maximum */
            w->b[fr_below(w->len)] = 0xFF;
            break;
        case 2:     /* a byte to zero */
            w->b[fr_below(w->len)] = 0;
            break;
        case 3:     /* the record length */
            fr_patch16(w, 3, fr_rand() & 0x3FFF);
            break;
        case 4:     /* a 16-bit field anywhere in the payload */
            if (w->len > FR_HDR + 2)
            {
                at = FR_HDR + fr_below(w->len - FR_HDR - 2);
                fr_patch16(w, at, fr_rand() & 0xFFFF);
            }
            break;
        case 5:     /* a 24-bit field anywhere in the payload */
            if (w->len > FR_HDR + 3)
            {
                at = FR_HDR + fr_below(w->len - FR_HDR - 3);
                fr_patch24(w, at, (unsigned long)fr_rand() << 9);
            }
            break;
        case 6:     /* truncate */
            w->len = fr_below(w->len) + 1u;
            break;
        default:    /* the record type */
            w->b[0] = (unsigned char)(20 + fr_below(6));
            break;
        }
    }
}

static void fr_run_seed(int s)
{
    FrBuf w;

    fr_seeds[s].build(&w);

    fr_run(w.b, w.len, 0);
    fr_run(w.b, w.len, 3);
}

int main(int argc, char **argv)
{
    int i;

    fr_selftest();

    for (i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "-s") == 0)
        {
            int s;

            for (s = 0; s < FR_SEED_COUNT; s++)
            {
                fr_run_seed(s);
                printf("  %-18s ok\n", fr_seeds[s].name);
            }

            printf("fuzz_tls_record: %d seed case(s), clean\n", FR_SEED_COUNT);
            return 0;
        }

        if (strcmp(argv[i], "-c") == 0 && i + 1 < argc)
        {
            const char *want = argv[++i];
            int         s;

            for (s = 0; s < FR_SEED_COUNT; s++)
            {
                if (strcmp(fr_seeds[s].name, want) == 0)
                {
                    fr_run_seed(s);
                    printf("fuzz_tls_record: seed '%s', clean\n", want);
                    return 0;
                }
            }

            printf("fuzz_tls_record: no seed case named '%s'\n", want);
            return 2;
        }

        if (strcmp(argv[i], "-r") == 0 && i + 2 < argc)
        {
            unsigned long seed  = strtoul(argv[++i], NULL, 0);
            unsigned long count = strtoul(argv[++i], NULL, 0);
            unsigned long n;
            int           s;

            for (s = 0; s < FR_SEED_COUNT; s++)
                fr_run_seed(s);

            fr_state = seed ? seed : 1;

            for (n = 0; n < count; n++)
            {
                FrBuf w;

                fr_seeds[fr_below((unsigned)FR_SEED_COUNT)].build(&w);
                fr_mutate(&w);
                fr_run(w.b, w.len, fr_below(FR_HDR + 2));
            }

            printf("fuzz_tls_record: %d seed(s) + %lu mutation(s) from %lu,"
                   " clean\n", FR_SEED_COUNT, count, seed);
            return 0;
        }
    }

    {
        unsigned char buf[FR_MAX];
        size_t        got = fread(buf, 1, sizeof(buf), stdin);

        fr_run(buf, (unsigned)got, 0);
    }

    return 0;
}

/*
 * N-128, N-129, N-130, E-234: _nx_secure_x509_certificate_parse() must take
 * no length it has not checked, and must hand the outer parser the exact size
 * of the certificate data so the signature algorithm is read where it is.
 *
 * The cases are real certificates re-encoded with one field changed: unique
 * IDs inserted (valid, and large), an empty EC public key, an empty signature,
 * an empty version.  Each runs in its own child with the DER flush against
 * 128 KB of PROT_NONE, so a read past it faults.  Only parsing is under test
 * here: whether a crypto method would refuse the result later is a separate
 * question and not asked.
 *
 * SPDX-License-Identifier: MIT
 */

#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include "nx_secure_tls.h"
#include "nx_secure_x509.h"

#include "tls_test_certs.h"

/* The header's key and CA are not needed here; they are static, so name them. */
static const void *const unused_certs[] = { test_device_cert_key_der, &test_device_cert_key_der_len, &test_device_cert_der_len,
                                            test_ca_cert_der, &test_ca_cert_der_len };

/* A self-signed P-256 certificate (openssl, CN=n129.test), for the EC key. */
static const unsigned char test_ec_cert_der[] = {
    0x30, 0x82, 0x01, 0x7e, 0x30, 0x82, 0x01, 0x23, 0xa0, 0x03, 0x02, 0x01,
    0x02, 0x02, 0x14, 0x66, 0x49, 0xa5, 0x1a, 0x11, 0x29, 0xe6, 0x31, 0xf6,
    0x32, 0x20, 0x02, 0xf6, 0x31, 0x2e, 0xa4, 0x30, 0xd2, 0x22, 0xc8, 0x30,
    0x0a, 0x06, 0x08, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x04, 0x03, 0x02, 0x30,
    0x14, 0x31, 0x12, 0x30, 0x10, 0x06, 0x03, 0x55, 0x04, 0x03, 0x0c, 0x09,
    0x6e, 0x31, 0x32, 0x39, 0x2e, 0x74, 0x65, 0x73, 0x74, 0x30, 0x1e, 0x17,
    0x0d, 0x32, 0x36, 0x31, 0x30, 0x30, 0x32, 0x32, 0x31, 0x35, 0x34, 0x30,
    0x35, 0x5a, 0x17, 0x0d, 0x33, 0x36, 0x30, 0x39, 0x32, 0x39, 0x32, 0x31,
    0x35, 0x34, 0x30, 0x35, 0x5a, 0x30, 0x14, 0x31, 0x12, 0x30, 0x10, 0x06,
    0x03, 0x55, 0x04, 0x03, 0x0c, 0x09, 0x6e, 0x31, 0x32, 0x39, 0x2e, 0x74,
    0x65, 0x73, 0x74, 0x30, 0x59, 0x30, 0x13, 0x06, 0x07, 0x2a, 0x86, 0x48,
    0xce, 0x3d, 0x02, 0x01, 0x06, 0x08, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x03,
    0x01, 0x07, 0x03, 0x42, 0x00, 0x04, 0x25, 0xff, 0x1e, 0x30, 0x68, 0x16,
    0x10, 0x92, 0x59, 0xa0, 0x28, 0xfe, 0x3f, 0xd5, 0xaf, 0xc0, 0xb3, 0xa5,
    0x4a, 0xe8, 0x9a, 0x13, 0x25, 0x6e, 0xba, 0x5e, 0x63, 0xad, 0x8a, 0x31,
    0x4a, 0x28, 0x83, 0x78, 0xfa, 0x0f, 0x64, 0xd9, 0x5c, 0xbe, 0xa2, 0x52,
    0x08, 0x1b, 0xa3, 0x87, 0x5c, 0x4e, 0xfb, 0x33, 0xbb, 0x89, 0x06, 0x48,
    0x65, 0xe9, 0x37, 0x7b, 0xfe, 0x68, 0x2a, 0x06, 0x54, 0x56, 0xa3, 0x53,
    0x30, 0x51, 0x30, 0x1d, 0x06, 0x03, 0x55, 0x1d, 0x0e, 0x04, 0x16, 0x04,
    0x14, 0x77, 0xf1, 0x9f, 0x88, 0x7f, 0x77, 0xe9, 0x50, 0x5d, 0x70, 0x0e,
    0x9d, 0xef, 0xe9, 0x07, 0x93, 0x7e, 0x15, 0x6c, 0xfe, 0x30, 0x1f, 0x06,
    0x03, 0x55, 0x1d, 0x23, 0x04, 0x18, 0x30, 0x16, 0x80, 0x14, 0x77, 0xf1,
    0x9f, 0x88, 0x7f, 0x77, 0xe9, 0x50, 0x5d, 0x70, 0x0e, 0x9d, 0xef, 0xe9,
    0x07, 0x93, 0x7e, 0x15, 0x6c, 0xfe, 0x30, 0x0f, 0x06, 0x03, 0x55, 0x1d,
    0x13, 0x01, 0x01, 0xff, 0x04, 0x05, 0x30, 0x03, 0x01, 0x01, 0xff, 0x30,
    0x0a, 0x06, 0x08, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x04, 0x03, 0x02, 0x03,
    0x49, 0x00, 0x30, 0x46, 0x02, 0x21, 0x00, 0xae, 0x1e, 0xf1, 0x50, 0x0e,
    0x89, 0x29, 0x58, 0x4e, 0x4e, 0x88, 0x51, 0x0f, 0xa7, 0xe1, 0x51, 0xc3,
    0xfb, 0xdf, 0x8e, 0xc5, 0x06, 0x04, 0xa5, 0x52, 0xce, 0x80, 0xa0, 0x87,
    0x40, 0x86, 0x5d, 0x02, 0x21, 0x00, 0x8e, 0x60, 0x5e, 0x3b, 0xbd, 0x4e,
    0x62, 0x11, 0x41, 0xef, 0x89, 0x79, 0x18, 0x29, 0x2c, 0x1b, 0x8d, 0xb9,
    0xea, 0xa7, 0x99, 0x2e, 0xe3, 0x8c, 0xa0, 0x73, 0xae, 0x57, 0x1b, 0x36,
    0x86, 0x47,
};

static int failures;
static int checks;

static void expect(int ok, const char *what)
{
    checks++;
    if (!ok)
    {
        printf("FAIL %s\n", what);
        failures++;
    }
}

/* ------------------------------------------------------------- DER ------ */

typedef struct
{
    const UCHAR *p;         /* the whole TLV */
    UINT         len;
} Tlv;

/* One TLV at p: its total size, and where its content starts. */
static UINT tlv(const UCHAR *p, UINT *content_off, UINT *content_len)
{
    UINT n = p[1], hdr = 2, len;

    if (n < 0x80)
        len = n;
    else
    {
        UINT k = n & 0x7F, i;

        len = 0;
        for (i = 0; i < k; i++)
            len = (len << 8) | p[2 + i];
        hdr += k;
    }
    if (content_off) *content_off = hdr;
    if (content_len) *content_len = len;
    return hdr + len;
}

static UINT put_header(UCHAR *out, UCHAR tag, UINT len)
{
    out[0] = tag;
    if (len < 0x80)  { out[1] = (UCHAR)len; return 2; }
    if (len < 0x100) { out[1] = 0x81; out[2] = (UCHAR)len; return 3; }
    out[1] = 0x82; out[2] = (UCHAR)(len >> 8); out[3] = (UCHAR)len;
    return 4;
}

/* The pieces of a certificate: tbs items, signature algorithm, signature. */
typedef struct
{
    Tlv  item[12];
    UINT items;
    Tlv  sig_alg;
    Tlv  sig;
} Cert;

static void split(const UCHAR *der, Cert *c)
{
    UINT off, len, toff, tlen, at;

    tlv(der, &off, &len);                       /* Certificate SEQUENCE */
    tlv(der + off, &toff, &tlen);               /* tbsCertificate */

    c->items = 0;
    for (at = 0; at < tlen; )
    {
        const UCHAR *p = der + off + toff + at;

        c->item[c->items].p   = p;
        c->item[c->items].len = tlv(p, NULL, NULL);
        at += c->item[c->items++].len;
    }

    at = off + toff + tlen;
    c->sig_alg.p = der + at; c->sig_alg.len = tlv(der + at, NULL, NULL); at += c->sig_alg.len;
    c->sig.p     = der + at; c->sig.len     = tlv(der + at, NULL, NULL);
}

static UCHAR built[8192];
static UINT  built_len;

/* Re-encode: tbs from its items, then the outer SEQUENCE around it all. */
static void join(const Cert *c)
{
    static UCHAR tbs[8192];
    UINT tbs_body = 0, i, n = 0, body;

    for (i = 0; i < c->items; i++)
    {
        memcpy(&tbs[8 + tbs_body], c->item[i].p, c->item[i].len);
        tbs_body += c->item[i].len;
    }

    body = 0;
    {
        UCHAR h[4];
        UINT  hl = put_header(h, 0x30, tbs_body);

        body = hl + tbs_body + c->sig_alg.len + c->sig.len;
        n  = put_header(built, 0x30, body);
        memcpy(&built[n], h, hl);                          n += hl;
        memcpy(&built[n], &tbs[8], tbs_body);              n += tbs_body;
        memcpy(&built[n], c->sig_alg.p, c->sig_alg.len);   n += c->sig_alg.len;
        memcpy(&built[n], c->sig.p, c->sig.len);           n += c->sig.len;
    }
    built_len = n;
}

/* A [1] IMPLICIT BIT STRING issuerUniqueID of `bytes` ID bytes. */
static UCHAR uid[2048];

static Tlv make_uid(UINT bytes)
{
    Tlv  t;
    UINT h = put_header(uid, 0x81, bytes + 1);

    uid[h] = 0x00;
    memset(&uid[h + 1], 0x5A, bytes);
    t.p = uid; t.len = h + 1 + bytes;
    return t;
}

/* Insert before the extensions, the last tbs item. */
static void insert_before_last(Cert *c, Tlv t)
{
    c->item[c->items] = c->item[c->items - 1];
    c->item[c->items - 1] = t;
    c->items++;
}

/* --------------------------------------------------------------- cases -- */

typedef enum
{
    V_RSA, V_EC, V_UID_SMALL, V_UID_LARGE, V_EC_EMPTY_KEY, V_EMPTY_SIG, V_EMPTY_VERSION
} Variant;

static void build(Variant v)
{
    Cert c;

    split(v == V_EC || v == V_EC_EMPTY_KEY ? test_ec_cert_der : test_device_cert_der, &c);

    switch (v)
    {
    case V_RSA:
    case V_EC:
        break;
    case V_UID_SMALL:
        insert_before_last(&c, make_uid(4));
        break;
    case V_UID_LARGE:
        insert_before_last(&c, make_uid(600));      /* larger than sigAlg + signature */
        break;
    case V_EC_EMPTY_KEY:
    {
        /* subjectPublicKeyInfo = SEQ { its algorithm, BIT STRING of 0 bytes }. */
        static UCHAR spki[256];
        const UCHAR *old = c.item[6].p;              /* version, serial, alg, issuer, validity, subject, spki */
        UINT off, len, alen, n;

        tlv(old, &off, &len);
        alen = tlv(old + off, NULL, NULL);
        n = put_header(spki, 0x30, alen + 2);
        memcpy(&spki[n], old + off, alen);  n += alen;
        spki[n++] = 0x03; spki[n++] = 0x00;
        c.item[6].p = spki; c.item[6].len = n;
        break;
    }
    case V_EMPTY_SIG:
    {
        static const UCHAR empty_bits[] = { 0x03, 0x00 };

        c.sig.p = empty_bits; c.sig.len = sizeof(empty_bits);
        break;
    }
    case V_EMPTY_VERSION:
    {
        static const UCHAR empty_version[] = { 0xA0, 0x00 };

        c.item[0].p = empty_version; c.item[0].len = sizeof(empty_version);
        break;
    }
    }

    join(&c);
}

#define H_GUARD_BYTES   (128u * 1024u)

static const UCHAR *flush(void)
{
    long   page  = sysconf(_SC_PAGESIZE);
    size_t lead  = ((built_len / (size_t)page) + 1) * (size_t)page;
    size_t gsize = ((H_GUARD_BYTES + (size_t)page - 1) / (size_t)page) * (size_t)page;
    UCHAR *base  = mmap(NULL, lead + gsize, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    if (base == MAP_FAILED || mprotect(base + lead, gsize, PROT_NONE) != 0)
    {
        printf("FAIL guard region setup\n");
        _exit(2);
    }

    memcpy(base + lead - built_len, built, built_len);
    return base + lead - built_len;
}

typedef struct
{
    const char *name;
    Variant     v;
    UINT        want;
} Case;

static const Case cases[] =
{
    { "RSA certificate parses whole",                 V_RSA,           NX_SECURE_X509_SUCCESS },
    { "EC certificate parses whole",                  V_EC,            NX_SECURE_X509_SUCCESS },
    { "v3 with a 4-byte issuerUniqueID parses whole", V_UID_SMALL,     NX_SECURE_X509_SUCCESS },
    { "v3 with a 600-byte issuerUniqueID parses whole", V_UID_LARGE,   NX_SECURE_X509_SUCCESS },
    { "empty EC public key",                          V_EC_EMPTY_KEY,  NX_SECURE_X509_ASN1_LENGTH_TOO_LONG },
    { "empty signature bit string",                   V_EMPTY_SIG,     NX_SECURE_X509_ASN1_LENGTH_TOO_LONG },
    { "empty version",                                V_EMPTY_VERSION, NX_SECURE_X509_INVALID_VERSION },
};

static int run_case(const Case *c)
{
    NX_SECURE_X509_CERT cert;
    UINT                bytes = 0, status;
    const UCHAR        *der;
    char                what[200];
    Cert                ref;

    build(c->v);
    der = flush();
    memset(&cert, 0, sizeof(cert));

    alarm(10);
    status = _nx_secure_x509_certificate_parse(der, built_len, &bytes, &cert);

    snprintf(what, sizeof(what), "%s: status 0x%x, want 0x%x", c->name, status, c->want);
    expect(status == c->want, what);

    if (status == NX_SECURE_X509_SUCCESS)
    {
        /* The whole certificate was consumed, and the signature is the
           certificate's own, not bytes found at a shifted offset. */
        split(der, &ref);
        snprintf(what, sizeof(what), "%s: consumed %u of %u", c->name, bytes, built_len);
        expect(bytes == built_len, what);
        expect(cert.nx_secure_x509_signature_data == ref.sig.p + (ref.sig.len - (UINT)(cert.nx_secure_x509_signature_data_length)),
               "the signature pointer is the signature field's");
    }

    return failures == 0 ? 0 : 1;
}

int main(void)
{
    unsigned i;

    (void)unused_certs;

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
    {
        int   status = 0;
        pid_t pid;
        char  what[200];

        fflush(stdout);
        pid = fork();
        if (pid == 0)
        {
            int rc;

            /* The child's verdict is this case's alone. */
            failures = 0;
            checks   = 0;
            rc = run_case(&cases[i]);
            fflush(stdout);
            _exit(rc);
        }

        if (pid > 0 && waitpid(pid, &status, 0) != pid)
            pid = -1;

        if (pid > 0 && WIFSIGNALED(status))
            snprintf(what, sizeof(what), "%s: died on signal %d (read past the certificate)",
                     cases[i].name, WTERMSIG(status));
        else
            snprintf(what, sizeof(what), "%s", cases[i].name);

        expect(pid > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0, what);
    }

    printf("x509_field_bounds: %d checks, %d failures\n", checks, failures);

    return failures == 0 ? 0 : 1;
}

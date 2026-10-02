/*
 * N-113: _nx_secure_tls_process_certificate_request() must not read past the
 * CertificateRequest it is handed.
 *
 * The server writes every byte of this message before it is authenticated, so
 * each case puts the message flush against a PROT_NONE page: a read one byte
 * past it is a SIGSEGV here instead of a quiet look at whatever follows in the
 * record buffer.  Each case runs in its own child so a fault fails that case
 * and the rest still run.  Valid TLS 1.2 and TLS 1.3 requests must still be
 * accepted with the same answer as before.
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

static NX_SECURE_TLS_SESSION session;
static NX_SECURE_X509_CERT   local_cert;

/* An ECDSA P-256 client certificate: 1.3 asks for ecdsa_secp256r1_sha256,
   1.2 for the ecdsa_sign type and SHA-256/ECDSA. */
static void session_init(int tls_1_3)
{
    memset(&session, 0, sizeof(session));
    memset(&local_cert, 0, sizeof(local_cert));

    local_cert.nx_secure_x509_public_algorithm = NX_SECURE_TLS_X509_TYPE_EC;
    local_cert.nx_secure_x509_private_key.ec_private_key.nx_secure_ec_named_curve =
        NX_CRYPTO_EC_SECP256R1;

    session.nx_secure_tls_credentials.nx_secure_tls_active_certificate = &local_cert;
    /* A TLS 1.3 session carries the legacy 1.2 version (process_serverhello.c
       stores the ServerHello's), which is how it reaches the shared
       signature-algorithms block. */
    session.nx_secure_tls_protocol_version = NX_SECURE_TLS_VERSION_TLS_1_2;
    session.nx_secure_tls_1_3 = (UCHAR)tls_1_3;
}

/* The message at the very end of a readable page, the next page PROT_NONE. */
static UINT run_flush(const UCHAR *msg, UINT len)
{
    long   page = sysconf(_SC_PAGESIZE);
    UCHAR *base = mmap(NULL, (size_t)page * 2, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    UCHAR *at;

    if (base == MAP_FAILED || mprotect(base + page, (size_t)page, PROT_NONE) != 0)
    {
        printf("FAIL guard page setup\n");
        _exit(2);
    }

    at = base + page - len;
    memcpy(at, msg, len);

    return _nx_secure_tls_process_certificate_request(&session, at, len);
}

typedef struct
{
    const char  *name;
    int          tls_1_3;
    const UCHAR *msg;
    UINT         len;
    UINT         want;
    UINT         want_sig;       /* checked when want == NX_SUCCESS and 1.3 */
} Case;

#define SIG_ALGS    0x00, 0x0D
#define ECDSA_256   0x04, 0x03

/* TLS 1.3: context, then the extensions block to the end of the message. */
static const UCHAR v13_ok[]          = { 0x00, 0x00, 0x08, SIG_ALGS, 0x00, 0x04, 0x00, 0x02, ECDSA_256 };
static const UCHAR v13_no_sig[]      = { 0x00, 0x00, 0x04, 0x00, 0x2F, 0x00, 0x00 };
/* An empty extension, then three bytes the block covers: a header cut short. */
static const UCHAR v13_tail3[]       = { 0x00, 0x00, 0x07, 0x00, 0x2F, 0x00, 0x00, 0x00, 0x0D, 0x00 };
static const UCHAR v13_tail1[]       = { 0x00, 0x00, 0x05, 0x00, 0x2F, 0x00, 0x00, 0x00 };
/* A payload length that runs past the block. */
static const UCHAR v13_cross[]       = { 0x00, 0x00, 0x06, 0x00, 0x2F, 0x00, 0x10, 0xAA, 0xBB };
/* An odd list that ends the message. */
static const UCHAR v13_odd[]         = { 0x00, 0x00, 0x07, SIG_ALGS, 0x00, 0x03, 0x00, 0x01, 0x04 };
/* A list that claims more than its extension holds. */
static const UCHAR v13_list_cross[]  = { 0x00, 0x00, 0x0C, SIG_ALGS, 0x00, 0x04, 0x00, 0x06, ECDSA_256,
                                          0x00, 0x2F, 0x00, 0x00 };
/* Bytes after the block, inside the message. */
static const UCHAR v13_after_block[] = { 0x00, 0x00, 0x08, SIG_ALGS, 0x00, 0x04, 0x00, 0x02, ECDSA_256,
                                          0xEE };

/* TLS 1.2: types, signature algorithms, then the CA list. */
static const UCHAR v12_ok[]          = { 0x01, 0x40, 0x00, 0x02, ECDSA_256, 0x00, 0x00 };
static const UCHAR v12_odd[]         = { 0x01, 0x40, 0x00, 0x03, 0x01, 0x02, 0x03 };

static const Case cases[] =
{
    { "1.3 valid request",                         1, v13_ok,          sizeof(v13_ok),
      NX_SUCCESS, NX_SECURE_TLS_SIGNATURE_ECDSA_SHA256 },
    { "1.3 no signature_algorithms",               1, v13_no_sig,      sizeof(v13_no_sig),
      NX_SECURE_TLS_UNSUPPORTED_CERT_SIGN_ALG, 0 },
    { "1.3 three-byte header stub at the end",     1, v13_tail3,       sizeof(v13_tail3),
      NX_SECURE_TLS_INCORRECT_MESSAGE_LENGTH, 0 },
    { "1.3 one-byte header stub at the end",       1, v13_tail1,       sizeof(v13_tail1),
      NX_SECURE_TLS_INCORRECT_MESSAGE_LENGTH, 0 },
    { "1.3 payload crossing the block",            1, v13_cross,       sizeof(v13_cross),
      NX_SECURE_TLS_INCORRECT_MESSAGE_LENGTH, 0 },
    { "1.3 odd signature list at the end",         1, v13_odd,         sizeof(v13_odd),
      NX_SECURE_TLS_INCORRECT_MESSAGE_LENGTH, 0 },
    { "1.3 signature list crossing its extension", 1, v13_list_cross,  sizeof(v13_list_cross),
      NX_SECURE_TLS_INCORRECT_MESSAGE_LENGTH, 0 },
    { "1.3 bytes after the extensions block",      1, v13_after_block, sizeof(v13_after_block),
      NX_SECURE_TLS_INCORRECT_MESSAGE_LENGTH, 0 },
    { "1.2 valid request",                         0, v12_ok,          sizeof(v12_ok),
      NX_SUCCESS, 0 },
    { "1.2 odd signature list at the end",         0, v12_odd,         sizeof(v12_odd),
      NX_SECURE_TLS_INCORRECT_MESSAGE_LENGTH, 0 },
};

static int run_case(const Case *c)
{
    UINT status;
    char what[160];

    session_init(c->tls_1_3);
    alarm(10);
    status = run_flush(c->msg, c->len);

    snprintf(what, sizeof(what), "N-113 %s: status 0x%x, want 0x%x",
             c->name, status, c->want);
    expect(status == c->want, what);

    if (c->want == NX_SUCCESS && c->tls_1_3)
        expect(session.nx_secure_tls_signature_algorithm == c->want_sig,
               "N-113 1.3 valid request: the server's ECDSA-SHA256 is chosen");

    return failures == 0 ? 0 : 1;
}

int main(void)
{
    unsigned i;

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
    {
        int   status = 0;
        pid_t pid;
        char  what[160];

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
            snprintf(what, sizeof(what), "N-113 %s: died on signal %d (read past the message)",
                     cases[i].name, WTERMSIG(status));
        else
            snprintf(what, sizeof(what), "N-113 %s", cases[i].name);

        expect(pid > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0, what);
    }

    printf("cert_request_bounds: %d checks, %d failures\n", checks, failures);

    return failures == 0 ? 0 : 1;
}

/*
 * N-115: the server's ClientHello list walkers - supported_groups, and
 * signature_algorithms for TLS 1.2 and 1.3 - must check a list's length is
 * whole and even before reading it two bytes at a time.
 *
 * _nx_secure_tls_proc_clienthello_sec_sa_extension() is driven directly with
 * one extension whose data ends flush against a PROT_NONE page, one forked
 * child per case.  A read past the extension faults; a well-formed list must
 * still negotiate the same curve and signature algorithm.
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

/* ------------------------------------------------------------ the world -- */

static NX_SECURE_TLS_SESSION session;
static NX_SECURE_TLS_CRYPTO  crypto;
static NX_SECURE_X509_CERT   cert;

/* P-256 first, as ours is; P-384 second.  The methods are only compared
   against NULL by the code under test. */
static NX_CRYPTO_METHOD        curve_p256;
static NX_CRYPTO_METHOD        curve_p384;
static const USHORT            groups[]  = { (USHORT)NX_CRYPTO_EC_SECP256R1, (USHORT)NX_CRYPTO_EC_SECP384R1 };
static const NX_CRYPTO_METHOD *curves[]  = { &curve_p256, &curve_p384 };
static NX_SECURE_X509_CRYPTO   x509_table[1];

static void session_init(int tls_1_3)
{
    memset(&session, 0, sizeof(session));
    memset(&crypto, 0, sizeof(crypto));
    memset(&cert, 0, sizeof(cert));
    memset(x509_table, 0, sizeof(x509_table));

    session.nx_secure_tls_ecc.nx_secure_tls_ecc_supported_groups       = groups;
    session.nx_secure_tls_ecc.nx_secure_tls_ecc_supported_groups_count = 2;
    session.nx_secure_tls_ecc.nx_secure_tls_ecc_curves                 = curves;

    x509_table[0].nx_secure_x509_crypto_identifier = NX_SECURE_TLS_X509_TYPE_ECDSA_SHA_256;
    crypto.nx_secure_tls_x509_cipher_table          = x509_table;
    crypto.nx_secure_tls_x509_cipher_table_size     = 1;
    session.nx_secure_tls_crypto_table              = &crypto;

    /* An ECDSA P-256 server certificate. */
    cert.nx_secure_x509_public_algorithm = NX_SECURE_TLS_X509_TYPE_EC;
    cert.nx_secure_x509_private_key.ec_private_key.nx_secure_ec_named_curve = NX_CRYPTO_EC_SECP256R1;
    session.nx_secure_tls_credentials.nx_secure_tls_active_certificate = &cert;

    /* 1.3 keeps the legacy 1.2 version, and that is how the 1.2 arm below
       is skipped for it: the dispatcher tests the 1.3 flag first. */
    session.nx_secure_tls_protocol_version = NX_SECURE_TLS_VERSION_TLS_1_2;
    session.nx_secure_tls_1_3 = (UCHAR)tls_1_3;
}

/* --------------------------------------------------------------- cases -- */

#define ECDSA_SHA256    0x04, 0x03
#define RSA_SHA256      0x04, 0x01

static const UCHAR g_ok[]      = { 0x00, 0x04, 0x00, 0x18, 0x00, 0x17 };      /* P-384, P-256 */
static const UCHAR g_odd[]     = { 0x00, 0x03, 0x00, 0x1D, 0x00 };            /* odd, at the end */
static const UCHAR g_short[]   = { 0x00, 0x08, 0x00, 0x17, 0x00, 0x18 };      /* claims 8, holds 4 */
/* Twelve groups we do not have: the list's own length is 0x0018, P-384. */
static const UCHAR g_phantom[] = { 0x00, 0x18,
                                   0x00, 0x1D, 0x00, 0x1E, 0x01, 0x00, 0x01, 0x01, 0x01, 0x02, 0x01, 0x03,
                                   0x01, 0x04, 0x00, 0x1D, 0x00, 0x1E, 0x01, 0x00, 0x01, 0x01, 0x01, 0x02 };

static const UCHAR s_ok[]      = { 0x00, 0x04, RSA_SHA256, ECDSA_SHA256 };
static const UCHAR s_odd[]     = { 0x00, 0x03, RSA_SHA256, 0x05 };           /* odd, at the end */
static const UCHAR s_short[]   = { 0x00, 0x06, ECDSA_SHA256 };              /* claims 6, holds 2 */

typedef struct
{
    const char  *name;
    int          tls_1_3;
    USHORT       ext_id;
    const UCHAR *data;
    USHORT       len;
    UINT         want_status;
    UINT         want_curve;        /* checked for EC_GROUPS on success */
    UINT         want_sig;          /* checked for SIGNATURE_ALGORITHMS on success */
} Case;

#define GROUPS  NX_SECURE_TLS_EXTENSION_EC_GROUPS
#define SIGALGS NX_SECURE_TLS_EXTENSION_SIGNATURE_ALGORITHMS
#define BADLEN  NX_SECURE_TLS_INCORRECT_MESSAGE_LENGTH

static const Case cases[] =
{
    { "groups: P-384 and P-256 offered, P-256 chosen", 0, GROUPS,  g_ok,      sizeof(g_ok),      NX_SUCCESS, 0x0017, 0 },
    { "groups: odd length at the end",                 0, GROUPS,  g_odd,     sizeof(g_odd),     BADLEN,     0,      0 },
    { "groups: list longer than the extension",        0, GROUPS,  g_short,   sizeof(g_short),   BADLEN,     0,      0 },
    { "groups: the length word is not a group",        0, GROUPS,  g_phantom, sizeof(g_phantom), NX_SUCCESS, 0,      0 },
    { "1.2 sigalgs: ECDSA-SHA256 chosen",              0, SIGALGS, s_ok,      sizeof(s_ok),      NX_SUCCESS, 0,      0x0403 },
    { "1.2 sigalgs: odd length at the end",            0, SIGALGS, s_odd,     sizeof(s_odd),     BADLEN,     0,      0 },
    { "1.3 sigalgs: ECDSA-SHA256 chosen",              1, SIGALGS, s_ok,      sizeof(s_ok),      NX_SUCCESS, 0,      0x0403 },
    { "1.3 sigalgs: odd length at the end",            1, SIGALGS, s_odd,     sizeof(s_odd),     BADLEN,     0,      0 },
    { "1.3 sigalgs: list longer than the extension",   1, SIGALGS, s_short,   sizeof(s_short),   BADLEN,     0,      0 },
};

/* The extension data at the very end of a readable page, the next PROT_NONE. */
static const UCHAR *flush(const UCHAR *data, USHORT len)
{
    long   page = sysconf(_SC_PAGESIZE);
    UCHAR *base = mmap(NULL, (size_t)page * 2, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    if (base == MAP_FAILED || mprotect(base + page, (size_t)page, PROT_NONE) != 0)
    {
        printf("FAIL guard page setup\n");
        _exit(2);
    }

    memcpy(base + page - len, data, len);
    return base + page - len;
}

static int run_case(const Case *c)
{
    NX_SECURE_TLS_HELLO_EXTENSION ext;
    UINT                          selected_curve = 0;
    UINT                          cert_curve_supported = 0;
    USHORT                        ecdhe_sig = 0;
    UINT                          status;
    char                          what[200];

    session_init(c->tls_1_3);
    memset(&ext, 0, sizeof(ext));
    ext.nx_secure_tls_extension_id          = c->ext_id;
    ext.nx_secure_tls_extension_data        = flush(c->data, c->len);
    ext.nx_secure_tls_extension_data_length = c->len;

    alarm(10);
    status = _nx_secure_tls_proc_clienthello_sec_sa_extension(&session, &ext, 1,
                                                              &selected_curve,
                                                              (USHORT)NX_CRYPTO_EC_SECP256R1,
                                                              &cert_curve_supported,
                                                              &ecdhe_sig, &cert);

    snprintf(what, sizeof(what), "N-115 %s: status 0x%x, want 0x%x",
             c->name, status, c->want_status);
    expect(status == c->want_status, what);

    if (status == NX_SUCCESS && c->ext_id == GROUPS)
    {
        snprintf(what, sizeof(what), "N-115 %s: curve 0x%x, want 0x%x",
                 c->name, selected_curve, c->want_curve);
        expect(selected_curve == c->want_curve, what);
    }

    if (status == NX_SUCCESS && c->ext_id == SIGALGS)
    {
        UINT got = c->tls_1_3 ? session.nx_secure_tls_signature_algorithm : ecdhe_sig;

        snprintf(what, sizeof(what), "N-115 %s: signature 0x%x, want 0x%x",
                 c->name, got, c->want_sig);
        expect(got == c->want_sig, what);
    }

    return failures == 0 ? 0 : 1;
}

int main(void)
{
    unsigned i;

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
            snprintf(what, sizeof(what), "N-115 %s: died on signal %d (read past the extension)",
                     cases[i].name, WTERMSIG(status));
        else
            snprintf(what, sizeof(what), "N-115 %s", cases[i].name);

        expect(pid > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0, what);
    }

    printf("clienthello_lists: %d checks, %d failures\n", checks, failures);

    return failures == 0 ? 0 : 1;
}

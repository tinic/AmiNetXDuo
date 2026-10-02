/*
 * N-116: _nx_secure_tls_process_remote_certificate() must account for the
 * whole Certificate message before it reads, copies or parses any of it.
 *
 * The server writes every byte of this message, and the parse runs before the
 * chain is verified.  Each case puts the message flush against a PROT_NONE
 * page, one forked child per case, so a read past it faults and fails that
 * case.  Chain verification is replaced below by a function returning
 * H_REACHED_VERIFY: a well-formed message must get that far, a malformed one
 * must be refused before it.
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

/* The header's key is not needed here; it is static, so name it once. */
static const void *const unused_key[] = { test_device_cert_key_der, &test_device_cert_key_der_len };

#define H_REACHED_VERIFY    0x7777u

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

/* Linked in place of nx_secure's: nothing here has a trusted store, and the
   question is only whether the parse reached this point. */
UINT _nx_secure_tls_remote_certificate_verify(NX_SECURE_TLS_SESSION *tls_session)
{
    (void)tls_session;
    return H_REACHED_VERIFY;
}

static NX_SECURE_TLS_SESSION session;
static NX_SECURE_TLS_CRYPTO  crypto;
static UCHAR                 record_buffer[16384];

static void session_init(int tls_1_3)
{
    memset(&session, 0, sizeof(session));
    memset(&crypto, 0, sizeof(crypto));

    session.nx_secure_tls_packet_buffer      = record_buffer;
    session.nx_secure_tls_packet_buffer_size = sizeof(record_buffer);
    session.nx_secure_tls_crypto_table       = &crypto;
    session.nx_secure_tls_1_3                = (UCHAR)tls_1_3;
}

/* ----------------------------------------------------------- messages -- */

static UCHAR msg[4096];
static UINT  msg_len;

static void put8(UINT v)  { msg[msg_len++] = (UCHAR)v; }
static void put16(UINT v) { put8(v >> 8); put8(v); }
static void put24(UINT v) { put8(v >> 16); put8(v >> 8); put8(v); }
static void put(const UCHAR *p, UINT n) { memcpy(&msg[msg_len], p, n); msg_len += n; }

/* One certificate entry: length, DER, and in 1.3 an extensions block. */
static UINT entry_size(UINT cert_len, int tls_1_3, UINT ext_len)
{
    return 3 + cert_len + (tls_1_3 ? 2 + ext_len : 0);
}

typedef enum
{
    M_V13_ONE, M_V13_TWO, M_V12_ONE,
    M_V13_EMPTY_BODY, M_V13_CONTEXT_PAST_END,
    M_V12_LIST_TWO_BYTES, M_V12_LIST_PAST_MESSAGE,
    M_V13_NO_EXT_LENGTH, M_V13_EXT_OVERRUN, M_V13_TRAILING,
    M_V12_CERT_PAST_LIST
} Shape;

static void build(Shape shape)
{
    const UINT dev = test_device_cert_der_len;
    const UINT ca  = test_ca_cert_der_len;

    msg_len = 0;

    switch (shape)
    {
    case M_V13_ONE:
        put8(0);
        put24(entry_size(dev, 1, 0));
        put24(dev); put(test_device_cert_der, dev); put16(0);
        break;
    case M_V13_TWO:
        put8(0);
        put24(entry_size(dev, 1, 0) + entry_size(ca, 1, 0));
        put24(dev); put(test_device_cert_der, dev); put16(0);
        put24(ca);  put(test_ca_cert_der, ca);      put16(0);
        break;
    case M_V12_ONE:
        put24(entry_size(dev, 0, 0));
        put24(dev); put(test_device_cert_der, dev);
        break;
    case M_V13_EMPTY_BODY:
        break;                                  /* a zero-length body */
    case M_V13_CONTEXT_PAST_END:
        put8(5);                                /* a context that is not there */
        break;
    case M_V12_LIST_TWO_BYTES:
        put24(2); put8(0xAA); put8(0xBB);       /* too short for a length field */
        break;
    case M_V12_LIST_PAST_MESSAGE:
        put24(6); put24(0);                     /* claims three bytes it does not have */
        break;
    case M_V13_NO_EXT_LENGTH:
        put8(0);
        put24(3 + dev);                         /* the certificate fills the list */
        put24(dev); put(test_device_cert_der, dev);
        break;
    case M_V13_EXT_OVERRUN:
        put8(0);
        put24(3 + dev + 2);
        put24(dev); put(test_device_cert_der, dev); put16(0xFFFF);
        break;
    case M_V13_TRAILING:
        put8(0);
        put24(entry_size(dev, 1, 0));
        put24(dev); put(test_device_cert_der, dev); put16(0);
        put8(0xEE);                             /* past the list, inside the message */
        break;
    case M_V12_CERT_PAST_LIST:
        put24(3 + 10);
        put24(dev); put(test_device_cert_der, 10);
        break;
    }
}

/*
 * The message at the very end of readable memory, then 128 KB of PROT_NONE:
 * more than the wrapped walk's reach (a 24-bit certificate length after a
 * 16-bit extensions length), so a read that skips past the first guard page
 * still lands in the guard rather than in some other mapping.
 */
#define H_GUARD_BYTES   (128u * 1024u)

static UINT run_flush(void)
{
    long   page  = sysconf(_SC_PAGESIZE);
    size_t lead  = ((msg_len / (size_t)page) + 1) * (size_t)page;
    size_t gsize = ((H_GUARD_BYTES + (size_t)page - 1) / (size_t)page) * (size_t)page;
    UCHAR *base  = mmap(NULL, lead + gsize, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    UCHAR *guard;
    UCHAR *at;

    if (base == MAP_FAILED)
    {
        printf("FAIL guard page setup\n");
        _exit(2);
    }

    guard = base + lead;
    if (mprotect(guard, gsize, PROT_NONE) != 0)
    {
        printf("FAIL guard page setup\n");
        _exit(2);
    }

    at = guard - msg_len;
    memcpy(at, msg, msg_len);

    return _nx_secure_tls_process_remote_certificate(&session, at, msg_len, 0);
}

typedef struct
{
    const char *name;
    Shape       shape;
    int         tls_1_3;
    UINT        want;
} Case;

static const Case cases[] =
{
    { "1.3 one certificate",                   M_V13_ONE,               1, H_REACHED_VERIFY },
    { "1.3 two certificates",                  M_V13_TWO,               1, H_REACHED_VERIFY },
    { "1.2 one certificate",                   M_V12_ONE,               0, H_REACHED_VERIFY },
    { "1.3 zero-length body",                  M_V13_EMPTY_BODY,        1, NX_SECURE_TLS_INCORRECT_MESSAGE_LENGTH },
    { "1.3 context past the end",              M_V13_CONTEXT_PAST_END,  1, NX_SECURE_TLS_INCORRECT_MESSAGE_LENGTH },
    { "1.2 list of two bytes",                 M_V12_LIST_TWO_BYTES,    0, NX_SECURE_TLS_INCORRECT_MESSAGE_LENGTH },
    { "1.2 list past the message",             M_V12_LIST_PAST_MESSAGE, 0, NX_SECURE_TLS_INCORRECT_MESSAGE_LENGTH },
    { "1.3 certificate with no extensions length", M_V13_NO_EXT_LENGTH, 1, NX_SECURE_TLS_INCORRECT_MESSAGE_LENGTH },
    { "1.3 extensions overrunning the list",   M_V13_EXT_OVERRUN,       1, NX_SECURE_TLS_INCORRECT_MESSAGE_LENGTH },
    { "1.3 bytes after the list",              M_V13_TRAILING,          1, NX_SECURE_TLS_INCORRECT_MESSAGE_LENGTH },
    { "1.2 certificate past the list",         M_V12_CERT_PAST_LIST,    0, NX_SECURE_TLS_INCORRECT_MESSAGE_LENGTH },
};

static int run_case(const Case *c)
{
    UINT status;
    char what[160];

    session_init(c->tls_1_3);
    build(c->shape);
    alarm(10);
    status = run_flush();

    snprintf(what, sizeof(what), "N-116 %s: status 0x%x, want 0x%x",
             c->name, status, c->want);
    expect(status == c->want, what);

    return failures == 0 ? 0 : 1;
}

int main(void)
{
    unsigned i;

    (void)unused_key;

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
            snprintf(what, sizeof(what), "N-116 %s: died on signal %d (read past the message)",
                     cases[i].name, WTERMSIG(status));
        else
            snprintf(what, sizeof(what), "N-116 %s", cases[i].name);

        expect(pid > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0, what);
    }

    printf("remote_cert_bounds: %d checks, %d failures\n", checks, failures);

    return failures == 0 ? 0 : 1;
}

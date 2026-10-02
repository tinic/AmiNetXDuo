/*
 * N-107 / N-109 bounded fault-injection fixture: driver.
 *
 * Exercises the two handshake drivers in the states that reach the three
 * client builders (send_certificate, send_certificate_verify, send_finished)
 * and the four server builders (send_certificate_request, send_certificate,
 * send_certificate_verify, send_finished). For each builder it asserts the
 * bounded benign-fault contract:
 *
 *   1. the handshake driver returns the ORIGINAL builder status unchanged
 *      (INJECTED_STATUS), and
 *   2. the packet allocated for the failed builder is released EXACTLY ONCE
 *      by the caller (no leak, no double-free), and
 *   3. no record for the failed builder is sent.
 *
 * It separately injects a record-send failure and asserts the driver does NOT
 * release the packet itself (the record layer owns that release), i.e. zero
 * caller releases => no double-free.
 */

#include <stdio.h>
#include <string.h>
#include "nx_secure_tls_1_3_handshake_fault_stubs.h"

static int g_failures = 0;

#define CHECK(cond, name)                                                      \
    do                                                                         \
    {                                                                          \
        if (!(cond))                                                           \
        {                                                                      \
            printf("  FAIL: %s\n", name);                                       \
            g_failures++;                                                       \
        }                                                                      \
    } while (0)

static void make_header(UCHAR *buf, UCHAR message_type, UINT message_length)
{
    buf[0] = message_type;
    buf[1] = (UCHAR)((message_length >> 16) & 0xFF);
    buf[2] = (UCHAR)((message_length >> 8) & 0xFF);
    buf[3] = (UCHAR)(message_length & 0xFF);
}

/* Drive the client handshake with a Finished message, optional certificate
 * request, and the configured injection. Returns the driver's status. */
static UINT run_client(UINT certificate_requested)
{
    NX_SECURE_TLS_SESSION session;
    UCHAR buf[4];
    UINT  ret;

    memset(&session, 0, sizeof(session));
    session.nx_secure_tls_1_3                    = NX_TRUE;
    session.nx_secure_tls_client_certificate_requested = certificate_requested;

    make_header(buf, NX_SECURE_TLS_FINISHED, 0);
    ret = _nx_secure_tls_1_3_client_handshake(&session, buf, sizeof(buf), 0);
    return ret;
}

/* Drive the server handshake with a ClientHello, optional client-certificate
 * verification, and the configured injection. Returns the driver's status. */
static UINT run_server(UINT verify_client_certificate)
{
    NX_SECURE_TLS_SESSION session;
    /* The SEND_HELLO state NX_ASSERTs that a ciphersuite was chosen by
     * ClientHello processing. A real process_clienthello selects one; our
     * stub does not, so supply a dummy non-NULL pointer to satisfy the
     * (active) assert and reach the builders under test. */
    static const NX_SECURE_TLS_CIPHERSUITE_INFO dummy_ciphersuite;
    UCHAR buf[4];
    UINT  ret;

    memset(&session, 0, sizeof(session));
    session.nx_secure_tls_1_3                 = NX_TRUE;
    session.nx_secure_tls_verify_client_certificate = verify_client_certificate;
    session.nx_secure_tls_session_ciphersuite = &dummy_ciphersuite;

    make_header(buf, NX_SECURE_TLS_CLIENT_HELLO, 0);
    ret = _nx_secure_tls_1_3_server_handshake(&session, buf, sizeof(buf), 0);
    return ret;
}

/* Assert the common builder-failure contract and report a named case. */
static void expect_builder_failure(const char *name, UINT ret,
                                   UINT expected_record_sends)
{
    CHECK(ret == INJECTED_STATUS, name);
    CHECK(g_caller_release_count == 1, name);
    CHECK(g_record_send_count == expected_record_sends, name);
}

/* Assert the record-send-failure contract (record layer owns release). */
static void expect_record_send_failure(const char *name, UINT ret,
                                       UINT expected_record_sends)
{
    CHECK(ret == INJECTED_STATUS, name);
    CHECK(g_caller_release_count == 0, name);   /* no caller release => no double-free */
    CHECK(g_record_send_count == expected_record_sends, name);
}

int main(void)
{
    UINT ret;


    /* --- Client builder failures (N-109) --- */

    test_reset();
    g_inject = INJECT_CLIENT_SEND_CERTIFICATE;
    ret = run_client(1);                        /* cert requested -> send_certificate runs */
    expect_builder_failure("client send_certificate fails", ret, 0);

    test_reset();
    g_inject = INJECT_CLIENT_SEND_CERTIFICATE_VERIFY;
    ret = run_client(1);                        /* cert succeeds, cert_verify fails */
    expect_builder_failure("client send_certificate_verify fails", ret, 1);

    test_reset();
    g_inject = INJECT_CLIENT_SEND_FINISHED;
    ret = run_client(0);                        /* no cert requested -> straight to finished */
    expect_builder_failure("client send_finished fails", ret, 0);

    /* --- Client record-send failure (record layer owns release) --- */

    test_reset();
    g_record_send_fail_type = NX_SECURE_TLS_FINISHED;
    ret = run_client(0);
    expect_record_send_failure("client record-send(finished) fails", ret, 1);

    /* --- Server builder failures (N-107) --- */

    test_reset();
    g_inject = INJECT_SERVER_SEND_CERTIFICATE_REQUEST;
    ret = run_server(1);                        /* verify_client_certificate -> cert_request runs */
    expect_builder_failure("server send_certificate_request fails", ret, 2);

    test_reset();
    g_inject = INJECT_SERVER_SEND_CERTIFICATE;
    ret = run_server(0);                        /* no verify -> cert runs */
    expect_builder_failure("server send_certificate fails", ret, 2);

    test_reset();
    g_inject = INJECT_SERVER_SEND_CERTIFICATE_VERIFY;
    ret = run_server(0);
    expect_builder_failure("server send_certificate_verify fails", ret, 3);

    test_reset();
    g_inject = INJECT_SERVER_SEND_FINISHED;
    ret = run_server(0);
    expect_builder_failure("server send_finished fails", ret, 4);

    /* --- Server record-send failure (record layer owns release) --- */

    test_reset();
    g_record_send_fail_type = NX_SECURE_TLS_CERTIFICATE_MSG;
    ret = run_server(0);
    expect_record_send_failure("server record-send(certificate) fails", ret, 3);

    if (g_failures == 0)
    {
        printf("PASS: all N-107/N-109 fault-injection checks\n");
        return 0;
    }
    printf("FAILED: %d check(s)\n", g_failures);
    return 1;
}

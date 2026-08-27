/*
 * RFC 7301 ALPN, src/tls/alpn/nx_secure_tls_alpn.c and src/tlslib/tls_alpn.c.
 *
 * nx_secure had no ALPN at all, so nothing built on it could negotiate
 * HTTP/2: h2 over TLS is defined only over a negotiated "h2" (RFC 7540 3.2).
 * This is the whole of the new code that is byte manipulation, and it is
 * exercised here in both directions -- a client offer parsed by the server
 * half, and the server's answer parsed by the client half.
 *
 * The check that matters most is RFC 7301 3.2: a server that selects a
 * protocol the client never offered must be refused.  Accepting it means
 * speaking a protocol the caller may not implement over a channel it has
 * already authenticated.
 *
 * SPDX-License-Identifier: MIT
 */

#include "tls_vectors.h"

#include <stdio.h>
#include <string.h>

static int checks;
static int failures;

#define CHECK(expr)                                                        \
    do {                                                                   \
        checks++;                                                          \
        if (!(expr))                                                       \
        {                                                                  \
            failures++;                                                    \
            printf("  FAIL line %d: %s\n", __LINE__, #expr);               \
        }                                                                  \
    } while (0)

VOID tls_memcpy(APTR dst, const void *src, ULONG size)
{
    memcpy(dst, src, (size_t)size);
}

/* ------------------------------------------------------- the tag string --- */

static void test_encode(void)
{
    TLSConnection conn;

    printf("tls_alpn: TLSA_ALPN's comma list becomes the RFC 7301 encoding\n");

    memset(&conn, 0, sizeof(conn));
    CHECK(tls_alpn_encode(&conn, (CONST_STRPTR)"h2,http/1.1") == TLS_OK);
    CHECK(conn.tc_AlpnLength == 12);
    CHECK(memcmp(conn.tc_Alpn, "\x02" "h2" "\x08" "http/1.1", 12) == 0);

    /* One name, no comma. */
    memset(&conn, 0, sizeof(conn));
    CHECK(tls_alpn_encode(&conn, (CONST_STRPTR)"h2") == TLS_OK);
    CHECK(conn.tc_AlpnLength == 3);
    CHECK(memcmp(conn.tc_Alpn, "\x02h2", 3) == 0);

    /* No tag and an empty tag are both "do not offer ALPN", not an error. */
    memset(&conn, 0, sizeof(conn));
    CHECK(tls_alpn_encode(&conn, NULL) == TLS_OK);
    CHECK(conn.tc_AlpnLength == 0);
    CHECK(tls_alpn_encode(&conn, (CONST_STRPTR)"") == TLS_OK);
    CHECK(conn.tc_AlpnLength == 0);
}

static void test_encode_rejects(void)
{
    TLSConnection conn;
    char          long_name[TLS_ALPN_NAME_MAX + 4];
    char          long_list[256];
    ULONG         i;

    printf("tls_alpn: a malformed list is refused, never shortened\n");

    /* An empty name has no spelling in RFC 7301 3.1: ProtocolName is
       opaque<1..255>.  A leading, trailing or doubled comma is one. */
    memset(&conn, 0, sizeof(conn));
    CHECK(tls_alpn_encode(&conn, (CONST_STRPTR)",h2") == TLS_ERR_BADALPN);
    CHECK(tls_alpn_encode(&conn, (CONST_STRPTR)"h2,") == TLS_ERR_BADALPN);
    CHECK(tls_alpn_encode(&conn, (CONST_STRPTR)"h2,,http/1.1") == TLS_ERR_BADALPN);
    CHECK(conn.tc_AlpnLength == 0);

    /* One name over the ceiling. */
    for (i = 0; i < (ULONG)TLS_ALPN_NAME_MAX + 1UL; i++)
        long_name[i] = 'x';
    long_name[TLS_ALPN_NAME_MAX + 1] = '\0';
    CHECK(tls_alpn_encode(&conn, (CONST_STRPTR)long_name) == TLS_ERR_BADALPN);

    /* Exactly at the ceiling is accepted, so the refusal above is the length
       and not the loop. */
    long_name[TLS_ALPN_NAME_MAX] = '\0';
    CHECK(tls_alpn_encode(&conn, (CONST_STRPTR)long_name) == TLS_OK);
    CHECK(conn.tc_AlpnLength == TLS_ALPN_NAME_MAX + 1);

    /* A list of legal names that does not fit.  Refused whole: a truncated
       offer is a protocol the caller did not ask for. */
    long_list[0] = '\0';
    for (i = 0; i < 30UL; i++)
        strcat(long_list, (i == 0) ? "ab" : ",ab");
    CHECK(tls_alpn_encode(&conn, (CONST_STRPTR)long_list) == TLS_ERR_BADALPN);
    CHECK(conn.tc_AlpnLength == 0);
}

/* ------------------------------------------------------------- the wire --- */

static const UCHAR offer_h2_http11[] = "\x02" "h2" "\x08" "http/1.1";
#define OFFER_LENGTH    12

static void test_set_and_send(void)
{
    NX_SECURE_TLS_SESSION session;
    UCHAR                 packet[64];
    ULONG                 offset = 0;
    USHORT                written = 0;

    printf("tls_alpn: the ClientHello extension is the offer, byte for byte\n");

    memset(&session, 0, sizeof(session));

    CHECK(_nx_secure_tls_alpn_protocol_set(&session, offer_h2_http11,
                                           OFFER_LENGTH) == NX_SUCCESS);

    /* A list whose name lengths do not add up is refused at the API, not on
       the wire: a malformed ClientHello comes back as an unexplained
       handshake failure. */
    CHECK(_nx_secure_tls_alpn_protocol_set(&session, (const UCHAR *)"\x05" "h2",
                                           3) != NX_SUCCESS);
    CHECK(_nx_secure_tls_alpn_protocol_set(&session, (const UCHAR *)"\x00", 1) !=
          NX_SUCCESS);

    /* And the good one is still installed after those refusals. */
    memset(packet, 0xAA, sizeof(packet));
    CHECK(_nx_secure_tls_alpn_send_extension(&session, packet, &offset, &written,
                                             sizeof(packet), NX_FALSE) ==
          NX_SUCCESS);
    CHECK(written == 6 + OFFER_LENGTH);
    CHECK(offset == written);
    CHECK(packet[0] == 0x00 && packet[1] == 0x10);              /* ext type   */
    CHECK(packet[2] == 0x00 && packet[3] == OFFER_LENGTH + 2);  /* ext length */
    CHECK(packet[4] == 0x00 && packet[5] == OFFER_LENGTH);      /* list       */
    CHECK(memcmp(&packet[6], offer_h2_http11, OFFER_LENGTH) == 0);

    /* No offer set means no extension, not an empty one. */
    memset(&session, 0, sizeof(session));
    offset = 0;
    written = 0xFFFF;
    CHECK(_nx_secure_tls_alpn_send_extension(&session, packet, &offset, &written,
                                             sizeof(packet), NX_FALSE) ==
          NX_SUCCESS);
    CHECK(written == 0);
    CHECK(offset == 0);

    /* A buffer that cannot hold it is refused, not part-written. */
    memset(&session, 0, sizeof(session));
    CHECK(_nx_secure_tls_alpn_protocol_set(&session, offer_h2_http11,
                                           OFFER_LENGTH) == NX_SUCCESS);
    offset = 0;
    CHECK(_nx_secure_tls_alpn_send_extension(&session, packet, &offset, &written,
                                             8, NX_FALSE) ==
          NX_SECURE_TLS_PACKET_BUFFER_TOO_SMALL);
    CHECK(offset == 0);
}

static void test_process_response(void)
{
    NX_SECURE_TLS_SESSION session;
    const UCHAR          *selected = NX_NULL;
    UCHAR                 length = 0;

    printf("tls_alpn: the server's answer must be one of the offered names\n");

    memset(&session, 0, sizeof(session));
    CHECK(_nx_secure_tls_alpn_protocol_set(&session, offer_h2_http11,
                                           OFFER_LENGTH) == NX_SUCCESS);

    /* ext_length, list_length, name.  "h2" was offered. */
    CHECK(_nx_secure_tls_alpn_process_response(
              &session, (const UCHAR *)"\x00\x05\x00\x03\x02" "h2", 7) ==
          NX_SUCCESS);
    CHECK(_nx_secure_tls_alpn_protocol_get(&session, &selected, &length) ==
          NX_SUCCESS);
    CHECK(length == 2);
    CHECK(memcmp(selected, "h2", 2) == 0);

    /* RFC 7301 3.2.  "spdy/3" was never offered. */
    memset(&session, 0, sizeof(session));
    CHECK(_nx_secure_tls_alpn_protocol_set(&session, offer_h2_http11,
                                           OFFER_LENGTH) == NX_SUCCESS);
    CHECK(_nx_secure_tls_alpn_process_response(
              &session, (const UCHAR *)"\x00\x09\x00\x07\x06" "spdy/3", 11) ==
          NX_SECURE_TLS_ALPN_PROTOCOL_MISMATCH);
    CHECK(session.nx_secure_tls_alpn_selected_length == 0);

    /* The server's list is exactly one name (RFC 7301 3.1).  Two is not a
       longer answer, it is a message this code cannot act on. */
    CHECK(_nx_secure_tls_alpn_process_response(
              &session, (const UCHAR *)"\x00\x08\x00\x06\x02" "h2\x02" "h3", 10) ==
          NX_SECURE_TLS_INCORRECT_MESSAGE_LENGTH);

    /* A length field that overruns the message, and one that disagrees with
       the extension length.  Both are how a parser walks off the end. */
    CHECK(_nx_secure_tls_alpn_process_response(
              &session, (const UCHAR *)"\x00\x40\x00\x03\x02" "h2", 7) ==
          NX_SECURE_TLS_INCORRECT_MESSAGE_LENGTH);
    CHECK(_nx_secure_tls_alpn_process_response(
              &session, (const UCHAR *)"\x00\x05\x00\x09\x02" "h2", 7) ==
          NX_SECURE_TLS_INCORRECT_MESSAGE_LENGTH);
    CHECK(_nx_secure_tls_alpn_process_response(&session,
                                               (const UCHAR *)"\x00", 1) ==
          NX_SECURE_TLS_INCORRECT_MESSAGE_LENGTH);

    /* An empty name. */
    CHECK(_nx_secure_tls_alpn_process_response(
              &session, (const UCHAR *)"\x00\x03\x00\x01\x00", 5) ==
          NX_SECURE_TLS_INCORRECT_MESSAGE_LENGTH);

    /* Nothing negotiated reads back as nothing, not as stale bytes. */
    memset(&session, 0, sizeof(session));
    selected = (const UCHAR *)"stale";
    length = 9;
    CHECK(_nx_secure_tls_alpn_protocol_get(&session, &selected, &length) ==
          NX_SECURE_TLS_EXTENSION_NOT_FOUND);
    CHECK(selected == NX_NULL);
    CHECK(length == 0);
}

static void test_server_selection(void)
{
    NX_SECURE_TLS_SESSION server;
    NX_SECURE_TLS_SESSION client;
    UCHAR                 packet[64];
    ULONG                 offset;
    USHORT                written = 0;

    printf("tls_alpn: the server picks by ITS order, and answers with one\n");

    /* The server prefers http/1.1; the client offers h2 first.  RFC 7301 3.2
       leaves the choice to the server, so http/1.1 wins. */
    memset(&server, 0, sizeof(server));
    CHECK(_nx_secure_tls_alpn_protocol_set(&server,
                                           (const UCHAR *)"\x08" "http/1.1\x02" "h2",
                                           12) == NX_SUCCESS);
    CHECK(_nx_secure_tls_alpn_process_offer(
              &server, (const UCHAR *)"\x00\x0e\x00\x0c\x02" "h2\x08" "http/1.1",
              16) == NX_SUCCESS);
    CHECK(server.nx_secure_tls_alpn_selected_length == 8);
    CHECK(memcmp(server.nx_secure_tls_alpn_selected, "http/1.1", 8) == 0);

    /* And what it writes is a list of exactly one, which the client half then
       accepts because it was offered.  This is the round trip. */
    offset = 0;
    CHECK(_nx_secure_tls_alpn_send_extension(&server, packet, &offset, &written,
                                             sizeof(packet), NX_TRUE) ==
          NX_SUCCESS);
    CHECK(written == 6 + 1 + 8);

    memset(&client, 0, sizeof(client));
    CHECK(_nx_secure_tls_alpn_protocol_set(&client, offer_h2_http11,
                                           OFFER_LENGTH) == NX_SUCCESS);
    CHECK(_nx_secure_tls_alpn_process_response(&client, &packet[2],
                                               (UINT)written - 2) == NX_SUCCESS);
    CHECK(client.nx_secure_tls_alpn_selected_length == 8);
    CHECK(memcmp(client.nx_secure_tls_alpn_selected, "http/1.1", 8) == 0);

    /* No overlap selects nothing, and a server with nothing selected writes
       no extension at all rather than an empty one. */
    memset(&server, 0, sizeof(server));
    CHECK(_nx_secure_tls_alpn_protocol_set(&server, (const UCHAR *)"\x02" "h2",
                                           3) == NX_SUCCESS);
    CHECK(_nx_secure_tls_alpn_process_offer(
              &server, (const UCHAR *)"\x00\x0b\x00\x09\x08" "http/1.1", 13) ==
          NX_SUCCESS);
    CHECK(server.nx_secure_tls_alpn_selected_length == 0);

    offset = 0;
    written = 0xFFFF;
    CHECK(_nx_secure_tls_alpn_send_extension(&server, packet, &offset, &written,
                                             sizeof(packet), NX_TRUE) ==
          NX_SUCCESS);
    CHECK(written == 0);
    CHECK(offset == 0);

    /* A server with no list of its own selects nothing and does not fail: a
       client offer is not a demand. */
    memset(&server, 0, sizeof(server));
    CHECK(_nx_secure_tls_alpn_process_offer(
              &server, (const UCHAR *)"\x00\x05\x00\x03\x02" "h2", 7) ==
          NX_SUCCESS);
    CHECK(server.nx_secure_tls_alpn_selected_length == 0);

    /* A client list whose names do not add up is a bad message. */
    CHECK(_nx_secure_tls_alpn_process_offer(
              &server, (const UCHAR *)"\x00\x05\x00\x03\x09" "h2", 7) ==
          NX_SECURE_TLS_INCORRECT_MESSAGE_LENGTH);
}

/* --------------------------------------------------------- the vector --- */

static void test_get_alpn_vector(void)
{
    TLSConnection conn;
    char          buffer[TLS_ALPN_NAME_MAX + 1];

    printf("tls_alpn: TLSGetALPN answers a NUL-terminated name, or nothing\n");

    memset(&conn, 0, sizeof(conn));

    /* Nothing negotiated is 0 and an empty string, not -1: a server may
       decline ALPN and HTTP then falls back to HTTP/1.1. */
    memset(buffer, 0x5A, sizeof(buffer));
    CHECK(tls_TLSGetALPN(&conn, buffer, (LONG)sizeof(buffer), NULL) == 0);
    CHECK(buffer[0] == '\0');

    memcpy(conn.tc_Session.nx_secure_tls_alpn_selected, "h2", 2);
    conn.tc_Session.nx_secure_tls_alpn_selected_length = 2;

    memset(buffer, 0x5A, sizeof(buffer));
    CHECK(tls_TLSGetALPN(&conn, buffer, (LONG)sizeof(buffer), NULL) == 2);
    CHECK(strcmp(buffer, "h2") == 0);

    /* Exactly big enough, and one byte short.  Short is refused, because half
       a protocol name is a different protocol. */
    memset(buffer, 0x5A, sizeof(buffer));
    CHECK(tls_TLSGetALPN(&conn, buffer, 3, NULL) == 2);
    CHECK(tls_TLSGetALPN(&conn, buffer, 2, NULL) == -1);

    CHECK(tls_TLSGetALPN(NULL, buffer, (LONG)sizeof(buffer), NULL) == -1);
    CHECK(tls_TLSGetALPN(&conn, NULL, (LONG)sizeof(buffer), NULL) == -1);
    CHECK(tls_TLSGetALPN(&conn, buffer, 0, NULL) == -1);
}

int main(void)
{
    /* Unbuffered: this test's failures are memory faults in a byte parser,
       and a buffered line is a line that never reaches the log. */
    setvbuf(stdout, NULL, _IONBF, 0);

    test_encode();
    test_encode_rejects();
    test_set_and_send();
    test_process_response();
    test_server_selection();
    test_get_alpn_vector();

    printf("%d checks, %d failure(s)\n", checks, failures);

    return (failures == 0) ? 0 : 1;
}

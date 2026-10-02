/*
 * N-107 / N-109 bounded fault-injection fixture: shared stub controls.
 *
 * This test links ONLY the two handshake drivers under test
 * (_nx_secure_tls_1_3_client_handshake, _nx_secure_tls_1_3_server_handshake)
 * against minimal stubs for every other symbol they reference. It is NOT a
 * broad netstack/guest harness: no ThreadX scheduler, no NX_PACKET pools, no
 * record layer. Every send/builder/allocate/hash function is a controllable
 * stub so each TLS 1.3 builder-failure and record-send-failure path can be
 * driven to completion and its release contract asserted directly.
 */
#ifndef N107_HANDSHAKE_FAULT_STUBS_H
#define N107_HANDSHAKE_FAULT_STUBS_H

#include "nx_api.h"
#include "nx_secure_tls.h"

/* Distinct status a stubbed builder / record-send returns when told to fail.
 * The handshake driver must return this value unchanged. */
#define INJECTED_STATUS (0x1234U)

/* Which builder to fail, or INJECT_NONE. */
typedef enum
{
    INJECT_NONE = 0,
    INJECT_CLIENT_SEND_CERTIFICATE,
    INJECT_CLIENT_SEND_CERTIFICATE_VERIFY,
    INJECT_CLIENT_SEND_FINISHED,
    INJECT_SERVER_SEND_CERTIFICATE_REQUEST,
    INJECT_SERVER_SEND_CERTIFICATE,
    INJECT_SERVER_SEND_CERTIFICATE_VERIFY,
    INJECT_SERVER_SEND_FINISHED
} inject_target_t;

/* Shared, resettable injection/count state (defined in stubs.c). */
extern inject_target_t g_inject;
extern UCHAR           g_record_send_fail_type;  /* handshake_type that fails; 0 = never fail */
extern UINT            g_caller_release_count;   /* invocations of _nx_secure_tls_packet_release */
extern UINT            g_record_send_count;      /* invocations of _nx_secure_tls_send_handshake_record */

void test_reset(void);

#endif /* N107_HANDSHAKE_FAULT_STUBS_H */

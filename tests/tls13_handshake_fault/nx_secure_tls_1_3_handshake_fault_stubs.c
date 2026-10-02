/*
 * N-107 / N-109 bounded fault-injection fixture: stub surface.
 *
 * Defines every symbol the two handshake drivers reference but that lives
 * elsewhere in nx_secure/ThreadX. Builders (send_*), the handshake-record
 * sender, packet allocation, transcript/key hashing, and the message
 * processors are all controllable so each failure path can be injected.
 *
 * Release semantics modelled here match the real code:
 *   - The three client builders + four server builders own NO packet_release
 *     internally; the CALLER (handshake driver) owns the packet until it is
 *     handed to _nx_secure_tls_send_handshake_record.
 *   - _nx_secure_tls_send_handshake_record owns the release on record-send
 *     failure (real impl: nx_secure_tls_send_handshake_record.c). The stub
 *     therefore does NOT touch g_caller_release_count on failure.
 *   - nx_secure_tls_packet_release is _nx_secure_tls_packet_release under
 *     NX_SECURE_KEY_CLEAR (the shipping config), so the caller release is the
 *     single call the driver makes, counted here.
 */

#include "nx_secure_tls_1_3_handshake_fault_stubs.h"

/* TLS 1.2 downgrade sentinels (defined in nx_secure_tls_send_serverhello.c). */
const UCHAR _nx_secure_tls_1_1_random[8] = {0};
const UCHAR _nx_secure_tls_1_2_random[8] = {0};

inject_target_t g_inject                = INJECT_NONE;
UCHAR           g_record_send_fail_type  = 0;
UINT            g_caller_release_count   = 0;
UINT            g_record_send_count      = 0;

/* A single dummy packet handed out by the allocate stub; never dereferenced. */
static NX_PACKET g_dummy_packet;

void test_reset(void)
{
    g_inject               = INJECT_NONE;
    g_record_send_fail_type = 0;
    g_caller_release_count  = 0;
    g_record_send_count     = 0;
}

/* --- packet allocation / release --- */

UINT _nx_secure_tls_allocate_handshake_packet(NX_SECURE_TLS_SESSION *tls_session,
                                              NX_PACKET_POOL *packet_pool,
                                              NX_PACKET **send_packet, ULONG wait_option)
{
    (void)tls_session;
    (void)packet_pool;
    (void)wait_option;
    *send_packet = &g_dummy_packet;
    return NX_SUCCESS;
}

UINT _nx_secure_tls_packet_release(NX_PACKET *packet_ptr)
{
    (void)packet_ptr;
    g_caller_release_count++;
    return NX_SUCCESS;
}

/* --- record send (owns release on failure, does NOT count a caller release) --- */

UINT _nx_secure_tls_send_handshake_record(NX_SECURE_TLS_SESSION *tls_session,
                                          NX_PACKET *send_packet, UCHAR handshake_type,
                                          ULONG wait_option)
{
    (void)tls_session;
    (void)send_packet;
    (void)wait_option;
    g_record_send_count++;
    if (g_record_send_fail_type != 0 && handshake_type == g_record_send_fail_type)
    {
        return INJECTED_STATUS;
    }
    return NX_SUCCESS;
}

/* --- key / transcript / hash machinery (succeed unless injected) --- */

UINT _nx_secure_tls_1_3_generate_handshake_keys(NX_SECURE_TLS_SESSION *tls_session)
{
    (void)tls_session;
    return NX_SUCCESS;
}

UINT _nx_secure_tls_1_3_generate_session_keys(NX_SECURE_TLS_SESSION *tls_session)
{
    (void)tls_session;
    return NX_SUCCESS;
}

UINT _nx_secure_tls_1_3_session_keys_set(NX_SECURE_TLS_SESSION *tls_session, USHORT key_set)
{
    (void)tls_session;
    (void)key_set;
    return NX_SUCCESS;
}

UINT _nx_secure_tls_1_3_transcript_hash_save(NX_SECURE_TLS_SESSION *tls_session,
                                             UINT hash_index, UINT need_copy)
{
    (void)tls_session;
    (void)hash_index;
    (void)need_copy;
    return NX_SUCCESS;
}

UINT _nx_secure_tls_handshake_hash_init(NX_SECURE_TLS_SESSION *tls_session)
{
    (void)tls_session;
    return NX_SUCCESS;
}

UINT _nx_secure_tls_handshake_hash_update(NX_SECURE_TLS_SESSION *tls_session,
                                          UCHAR *data, UINT length)
{
    (void)tls_session;
    (void)data;
    (void)length;
    return NX_SUCCESS;
}

/* --- message processors (set the state the driver then acts on) --- */

UINT _nx_secure_tls_process_handshake_header(UCHAR *packet_buffer, USHORT *message_type,
                                             UINT *header_size, UINT *message_length)
{
    /* Real TLS handshake header: 1-byte type, 3-byte big-endian length. */
    *message_type  = (USHORT)packet_buffer[0];
    *message_length = ((UINT)packet_buffer[1] << 16) | ((UINT)packet_buffer[2] << 8) |
                      (UINT)packet_buffer[3];
    *header_size   = 4;
    return NX_SUCCESS;
}

UINT _nx_secure_tls_process_finished(NX_SECURE_TLS_SESSION *tls_session,
                                     UCHAR *packet_buffer, UINT message_length)
{
    (void)packet_buffer;
    (void)message_length;
    /* Client: move to the state that sends Cert/CertVerify/Finished. */
    tls_session -> nx_secure_tls_client_state = NX_SECURE_TLS_CLIENT_STATE_HANDSHAKE_FINISHED;
    return NX_SUCCESS;
}

UINT _nx_secure_tls_process_clienthello(NX_SECURE_TLS_SESSION *tls_session,
                                        UCHAR *packet_buffer, UINT message_length)
{
    (void)packet_buffer;
    (void)message_length;
    /* Negotiate TLS 1.3 so the server proceeds past the version gate. */
    tls_session -> nx_secure_tls_1_3 = NX_TRUE;
    return NX_SUCCESS;
}

UINT _nx_secure_tls_process_certificate_request(NX_SECURE_TLS_SESSION *tls_session,
                                                UCHAR *packet_buffer, UINT message_length)
{
    (void)tls_session;
    (void)packet_buffer;
    (void)message_length;
    return NX_SUCCESS;
}

UINT _nx_secure_tls_process_certificate_verify(NX_SECURE_TLS_SESSION *tls_session,
                                               UCHAR *packet_buffer, UINT message_length)
{
    (void)tls_session;
    (void)packet_buffer;
    (void)message_length;
    return NX_SUCCESS;
}

UINT _nx_secure_tls_process_encrypted_extensions(NX_SECURE_TLS_SESSION *tls_session,
                                                 UCHAR *packet_buffer, UINT message_length)
{
    (void)tls_session;
    (void)packet_buffer;
    (void)message_length;
    return NX_SUCCESS;
}

UINT _nx_secure_tls_process_remote_certificate(NX_SECURE_TLS_SESSION *tls_session,
                                               UCHAR *packet_buffer, UINT message_length,
                                               UINT data_length)
{
    (void)tls_session;
    (void)packet_buffer;
    (void)message_length;
    (void)data_length;
    return NX_SUCCESS;
}

UINT _nx_secure_tls_process_serverhello(NX_SECURE_TLS_SESSION *tls_session,
                                        UCHAR *packet_buffer, UINT message_length)
{
    (void)tls_session;
    (void)packet_buffer;
    (void)message_length;
    return NX_SUCCESS;
}

/* --- builders (client + server). Each returns INJECTED_STATUS when selected. --- */

UINT _nx_secure_tls_send_certificate(NX_SECURE_TLS_SESSION *tls_session, NX_PACKET *send_packet,
                                     ULONG wait_option)
{
    (void)tls_session;
    (void)send_packet;
    (void)wait_option;
    if (g_inject == INJECT_CLIENT_SEND_CERTIFICATE ||
        g_inject == INJECT_SERVER_SEND_CERTIFICATE)
    {
        return INJECTED_STATUS;
    }
    return NX_SUCCESS;
}

UINT _nx_secure_tls_send_certificate_verify(NX_SECURE_TLS_SESSION *tls_session, NX_PACKET *send_packet)
{
    (void)tls_session;
    (void)send_packet;
    if (g_inject == INJECT_CLIENT_SEND_CERTIFICATE_VERIFY ||
        g_inject == INJECT_SERVER_SEND_CERTIFICATE_VERIFY)
    {
        return INJECTED_STATUS;
    }
    return NX_SUCCESS;
}

UINT _nx_secure_tls_send_finished(NX_SECURE_TLS_SESSION *tls_session, NX_PACKET *send_packet)
{
    (void)tls_session;
    (void)send_packet;
    if (g_inject == INJECT_CLIENT_SEND_FINISHED ||
        g_inject == INJECT_SERVER_SEND_FINISHED)
    {
        return INJECTED_STATUS;
    }
    return NX_SUCCESS;
}

UINT _nx_secure_tls_send_certificate_request(NX_SECURE_TLS_SESSION *tls_session, NX_PACKET *send_packet)
{
    (void)tls_session;
    (void)send_packet;
    if (g_inject == INJECT_SERVER_SEND_CERTIFICATE_REQUEST)
    {
        return INJECTED_STATUS;
    }
    return NX_SUCCESS;
}

UINT _nx_secure_tls_send_serverhello(NX_SECURE_TLS_SESSION *tls_session, NX_PACKET *send_packet)
{
    (void)tls_session;
    (void)send_packet;
    return NX_SUCCESS;
}

UINT _nx_secure_tls_send_encrypted_extensions(NX_SECURE_TLS_SESSION *tls_session, NX_PACKET *send_packet)
{
    (void)tls_session;
    (void)send_packet;
    return NX_SUCCESS;
}

UINT _nx_secure_tls_send_clienthello(NX_SECURE_TLS_SESSION *tls_session, NX_PACKET *send_packet)
{
    (void)tls_session;
    (void)send_packet;
    return NX_SUCCESS;
}

/* --- ThreadX (only _tx_thread_sleep is referenced by the drivers) --- */

UINT _tx_thread_sleep(ULONG timer_ticks)
{
    (void)timer_ticks;
    return NX_SUCCESS;
}

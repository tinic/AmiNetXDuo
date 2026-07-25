/*
 * AmiNetXDuo -- a real TLS 1.2 handshake, on a real 68k, end to end.
 *
 * tls_bench.c times the primitives.  This times the thing itself: a complete
 * TLS 1.2 handshake between an nx_secure client and an nx_secure server, over
 * real TCP, on one Amiga, including everything the primitive benchmark leaves
 * out -- ClientHello/ServerHello negotiation, X.509 DER parsing, certificate
 * chain verification against a trust store, key derivation, Finished hashing
 * and the record layer.
 *
 * WHY LOOPBACK AND NOT A PUBLIC HOST
 *
 *   Reaching a real HTTPS server from the emulator is possible in principle
 *   (SLIRP gives outbound internet, and tests/netstack proves DNS and routing
 *   work), but it makes the measurement depend on a certificate chain we do
 *   not control, a trust store we would have to ship, and a server's patience:
 *   the client-side arithmetic alone is over ten seconds on the floor target
 *   (see the tls_bench figures), which is inside most servers' handshake
 *   timeout but not comfortably so.  Running both halves locally measures the
 *   same arithmetic with none of that variance, and it is the arithmetic that
 *   decides the question.  The cost is that ONE Amiga does BOTH sides' work --
 *   so the wall time here is a client handshake plus a server handshake, and
 *   the two are reported separately where possible.
 *
 * SHAPE
 *
 *   Same fabric as tests/ram_driver: ThreadX on Exec, two NX_IP instances
 *   talking over the in-tree simulated RAM driver, the server on a
 *   ThreadX-created thread and the client on this process's own adopted Exec
 *   Task.  On top of that, one NX_SECURE_TLS_SESSION each.
 *
 * SPDX-License-Identifier: MIT
 */

#include "tx_api.h"
#include "tx_amiga.h"
#include "nx_api.h"
#include "nx_secure_tls_api.h"

#include "tls.h"
#include "aminetxduo/crashguard.h"

#include <exec/types.h>
#include <exec/execbase.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include <stdarg.h>

#include "tls_test_certs.h"


/* ------------------------------------------------------------- logging --- */

#ifndef RawPutChar
#  define RawPutChar(c) \
      LP1NR(0x204, RawPutChar, UBYTE, (c), d0, , EXEC_BASE_NAME)
#endif

#define H_LOG_SIZE      6144

static char     h_log_buffer[H_LOG_SIZE];
static ULONG    h_log_used;

static VOID h_put(UBYTE c)
{

    RawPutChar(c);

    if (h_log_used < (ULONG)(H_LOG_SIZE - 1))
    {
        h_log_buffer[h_log_used++] = (char)c;
    }
}

static VOID h_put_char(register UBYTE c      __asm("d0"),
                       register APTR  unused __asm("a3"))
{

    (VOID) unused;
    if (c != '\0')
    {
        h_put(c);
    }
}

static VOID h_log(const char *fmt, ...)
{

va_list args;


    va_start(args, fmt);
    RawDoFmt((STRPTR)fmt, args, (void (*)()) h_put_char, NULL);
    va_end(args);

    h_put('\n');
}

static VOID h_flush(VOID)
{

BPTR    out;


    out =  Output();
    if (out != (BPTR)0)
    {
        (VOID) Write(out, (APTR)h_log_buffer, (LONG)h_log_used);
    }
}

static volatile ULONG   h_checks;
static volatile ULONG   h_failures;

static UINT h_check(UINT ok, const char *what, ULONG detail)
{

    Forbid();
    h_checks++;
    if (!ok)
    {
        h_failures++;
    }
    Permit();

    if (ok)
    {
        h_log("  ok   %s", (LONG)what);
    }
    else
    {
        h_log("  FAIL %s (0x%lx)", (LONG)what, detail);
    }

    return(ok);
}

#define H_OK(status, what)      h_check((UINT)((status) == NX_SUCCESS), (what), (ULONG)(status))
#define H_TX_OK(status, what)   h_check((UINT)((status) == TX_SUCCESS), (what), (ULONG)(status))


/* ---------------------------------------------------------- test fabric -- */

#define H_PACKET_PAYLOAD        1568        /* == AMI_POOL_PAYLOAD          */
#define H_PACKET_COUNT          32          /* a cert chain is several MTU  */
#define H_PACKET_OVERHEAD       96

#define H_IP0_ADDRESS           IP_ADDRESS(192, 168, 100, 1)
#define H_IP1_ADDRESS           IP_ADDRESS(192, 168, 100, 2)
#define H_NETMASK               0xFFFFFF00UL
#define H_PORT                  4433

#define H_IP_STACK_SIZE         3072
#define H_SERVER_STACK_SIZE     8192        /* TLS state machine is deep    */

/*
 * TLS working memory, per session.
 *
 * The crypto metadata block is where nx_secure keeps every algorithm's context
 * for the session; nx_secure_tls_metadata_size_calculate() computes the exact
 * requirement from the ciphersuite table, and the value is printed at run time
 * so the memory estimate in the report is measured rather than assumed.  16 KB
 * is the upstream sample's figure rounded up.
 *
 * The packet reassembly buffer must hold the largest single handshake message,
 * which here is the Certificate message carrying the chain.  The upstream
 * sample uses 40 KB; that is sized for 16 KB application records, not for a
 * handshake, and on a 4 MB machine it is not a defensible default.  8 KB holds
 * a two-certificate RSA-2048 chain with room to spare.
 */
#define H_METADATA_SIZE         16384
#define H_PACKET_BUFFER_SIZE    8192

extern VOID _nx_ram_network_driver(NX_IP_DRIVER *driver_req_ptr);

/*
 * The ECC-capable ciphersuite table.  It carries the ECDHE_RSA and
 * ECDHE_ECDSA suites as well as the plain RSA ones, so what actually gets
 * negotiated is decided by the two ends and reported below rather than
 * assumed here.
 */
extern const NX_SECURE_TLS_CRYPTO       nx_crypto_tls_ciphers_ecc;
extern const USHORT                     nx_crypto_ecc_supported_groups[];
extern const NX_CRYPTO_METHOD          *nx_crypto_ecc_curves[];
extern const UINT                       nx_crypto_ecc_supported_groups_size;

static NX_PACKET_POOL           h_pool;
static NX_IP                    h_ip0;
static NX_IP                    h_ip1;
static NX_TCP_SOCKET            h_client_socket;
static NX_TCP_SOCKET            h_server_socket;

static NX_SECURE_TLS_SESSION    h_client_session;
static NX_SECURE_TLS_SESSION    h_server_session;

static NX_SECURE_X509_CERT      h_server_certificate;
static NX_SECURE_X509_CERT      h_trusted_certificate;
static NX_SECURE_X509_CERT      h_remote_certificate;
static NX_SECURE_X509_CERT      h_remote_issuer;

static UCHAR                    h_remote_cert_buffer[2048];
static UCHAR                    h_remote_issuer_buffer[2048];

static TX_THREAD                h_server_thread;
static TX_THREAD                h_main_thread;
static TX_SEMAPHORE             h_server_done;

static ULONG    h_pool_memory[(H_PACKET_COUNT * (H_PACKET_PAYLOAD + H_PACKET_OVERHEAD)) / sizeof(ULONG)];
static ULONG    h_ip0_stack[H_IP_STACK_SIZE / sizeof(ULONG)];
static ULONG    h_ip1_stack[H_IP_STACK_SIZE / sizeof(ULONG)];
static ULONG    h_server_stack[H_SERVER_STACK_SIZE / sizeof(ULONG)];
static ULONG    h_arp0_cache[1024 / sizeof(ULONG)];
static ULONG    h_arp1_cache[1024 / sizeof(ULONG)];

/* Longword aligned: nx_secure carves aligned crypto contexts out of these. */
static ULONG    h_client_metadata[H_METADATA_SIZE / sizeof(ULONG)];
static ULONG    h_server_metadata[H_METADATA_SIZE / sizeof(ULONG)];
static ULONG    h_client_packet_buffer[H_PACKET_BUFFER_SIZE / sizeof(ULONG)];
static ULONG    h_server_packet_buffer[H_PACKET_BUFFER_SIZE / sizeof(ULONG)];

/* Timings, filled in by whichever half measured them. */
static ULONG    h_server_handshake_us;
static ULONG    h_client_handshake_us;

static const char h_message[] = "AmiNetXDuo TLS 1.2 loopback";


/* --------------------------------------------------------- server half --- */

static VOID h_server_entry(ULONG id)
{

UINT        status;
NX_PACKET  *packet_ptr;
ULONG       actual;
ULONG       start;


    (VOID) id;

    status =  nx_ip_status_check(&h_ip1, NX_IP_INITIALIZE_DONE, &actual,
                                 5UL * NX_IP_PERIODIC_RATE);
    (VOID) H_OK(status, "server: ip1 initialised");

    status =  nx_tcp_socket_create(&h_ip1, &h_server_socket, "tls server",
                                   NX_IP_NORMAL, NX_FRAGMENT_OKAY,
                                   NX_IP_TIME_TO_LIVE, 4096, NX_NULL, NX_NULL);
    (VOID) H_OK(status, "server: tcp socket create");

    status =  nx_secure_tls_session_create(&h_server_session,
                                           &nx_crypto_tls_ciphers_ecc,
                                           (VOID *)h_server_metadata,
                                           (ULONG)sizeof(h_server_metadata));
    if (!H_OK(status, "server: tls session create"))
    {
        (VOID) tx_semaphore_put(&h_server_done);
        return;
    }

    status =  nx_secure_tls_ecc_initialize(&h_server_session,
                                           nx_crypto_ecc_supported_groups,
                                           nx_crypto_ecc_supported_groups_size,
                                           nx_crypto_ecc_curves);
    (VOID) H_OK(status, "server: ecc curves registered");

    status =  nx_secure_tls_session_packet_buffer_set(&h_server_session,
                                                      (UCHAR *)h_server_packet_buffer,
                                                      (ULONG)sizeof(h_server_packet_buffer));
    (VOID) H_OK(status, "server: packet buffer set");

    status =  nx_secure_x509_certificate_initialize(&h_server_certificate,
                                                    test_device_cert_der,
                                                    test_device_cert_der_len,
                                                    NX_NULL, 0,
                                                    test_device_cert_key_der,
                                                    test_device_cert_key_der_len,
                                                    NX_SECURE_X509_KEY_TYPE_RSA_PKCS1_DER);
    (VOID) H_OK(status, "server: leaf certificate + RSA key parsed");

    status =  nx_secure_tls_local_certificate_add(&h_server_session,
                                                  &h_server_certificate);
    (VOID) H_OK(status, "server: local certificate added");

    status =  nx_tcp_server_socket_listen(&h_ip1, H_PORT, &h_server_socket, 5,
                                          NX_NULL);
    (VOID) H_OK(status, "server: listen");

    status =  nx_tcp_server_socket_accept(&h_server_socket,
                                          20UL * NX_IP_PERIODIC_RATE);
    if (!H_OK(status, "server: tcp accept"))
    {
        (VOID) tx_semaphore_put(&h_server_done);
        return;
    }

    /*
     * The handshake.  On this side it costs one RSA-2048 private operation
     * (the ServerKeyExchange signature under an ECDHE_RSA suite, or the
     * premaster decryption under a plain RSA suite) plus an ECDHE key pair --
     * the expensive half, per tls_bench.
     */
    start =  ami_tls_eclock();
    status = nx_secure_tls_session_start(&h_server_session, &h_server_socket,
                                         120UL * NX_IP_PERIODIC_RATE);
    h_server_handshake_us = ami_tls_eclock_micros(ami_tls_eclock() - start);

    if (H_OK(status, "server: TLS handshake completed"))
    {
        packet_ptr = NX_NULL;
        status = nx_secure_tls_session_receive(&h_server_session, &packet_ptr,
                                               20UL * NX_IP_PERIODIC_RATE);
        if (H_OK(status, "server: encrypted record received"))
        {
            (VOID) nx_packet_release(packet_ptr);
        }
    }

    (VOID) nx_secure_tls_session_end(&h_server_session,
                                     5UL * NX_IP_PERIODIC_RATE);
    (VOID) nx_secure_tls_session_delete(&h_server_session);

    (VOID) nx_tcp_socket_disconnect(&h_server_socket,
                                    5UL * NX_IP_PERIODIC_RATE);
    (VOID) nx_tcp_server_socket_unaccept(&h_server_socket);
    (VOID) nx_tcp_server_socket_unlisten(&h_ip1, H_PORT);
    (VOID) nx_tcp_socket_delete(&h_server_socket);

    (VOID) tx_semaphore_put(&h_server_done);
}


/* --------------------------------------------------------- client half --- */

static UINT h_client_run(VOID)
{

UINT        status;
NX_PACKET  *packet_ptr;
ULONG       actual;
ULONG       start;
ULONG       metadata_needed = 0;


    /*
     * Ask nx_secure how much crypto metadata this ciphersuite table really
     * needs, rather than trusting the upstream sample's round number -- the
     * memory figure in the report should be measured.
     *
     * This has to happen HERE and not in tx_application_define(): the nxe_
     * error-checking wrappers apply NX_THREADS_ONLY_CALLER_CHECKING, so a call
     * from initialization context comes back NX_CALLER_ERROR (0x11) with the
     * output untouched.
     */
    status =  nx_secure_tls_metadata_size_calculate(&nx_crypto_tls_ciphers_ecc,
                                                    &metadata_needed);
    h_log("  crypto metadata: %lu bytes required, %lu allocated",
          metadata_needed, (ULONG)H_METADATA_SIZE);
    (VOID) h_check((UINT)((status == NX_SUCCESS) &&
                          (metadata_needed <= (ULONG)H_METADATA_SIZE)),
                   "metadata allocation is large enough", (ULONG)status);

    status =  nx_ip_status_check(&h_ip0, NX_IP_INITIALIZE_DONE, &actual,
                                 5UL * NX_IP_PERIODIC_RATE);
    (VOID) H_OK(status, "client: ip0 initialised");

    status =  nx_tcp_socket_create(&h_ip0, &h_client_socket, "tls client",
                                   NX_IP_NORMAL, NX_FRAGMENT_OKAY,
                                   NX_IP_TIME_TO_LIVE, 4096, NX_NULL, NX_NULL);
    (VOID) H_OK(status, "client: tcp socket create");

    status =  nx_secure_tls_session_create(&h_client_session,
                                           &nx_crypto_tls_ciphers_ecc,
                                           (VOID *)h_client_metadata,
                                           (ULONG)sizeof(h_client_metadata));
    if (!H_OK(status, "client: tls session create"))
    {
        return(TX_FALSE);
    }

    status =  nx_secure_tls_ecc_initialize(&h_client_session,
                                           nx_crypto_ecc_supported_groups,
                                           nx_crypto_ecc_supported_groups_size,
                                           nx_crypto_ecc_curves);
    (VOID) H_OK(status, "client: ecc curves registered");

    status =  nx_secure_tls_session_packet_buffer_set(&h_client_session,
                                                      (UCHAR *)h_client_packet_buffer,
                                                      (ULONG)sizeof(h_client_packet_buffer));
    (VOID) H_OK(status, "client: packet buffer set");

    /*
     * The trust store, and the buffers the incoming chain is parsed into.
     * Without the remote-certificate allocation nx_secure has nowhere to put
     * what the server sends and the handshake fails with a buffer error
     * rather than a verification error -- a distinction worth knowing when
     * this is being debugged.
     */
    status =  nx_secure_x509_certificate_initialize(&h_trusted_certificate,
                                                    test_ca_cert_der,
                                                    test_ca_cert_der_len,
                                                    NX_NULL, 0, NX_NULL, 0,
                                                    NX_SECURE_X509_KEY_TYPE_NONE);
    (VOID) H_OK(status, "client: CA certificate parsed");

    status =  nx_secure_tls_trusted_certificate_add(&h_client_session,
                                                    &h_trusted_certificate);
    (VOID) H_OK(status, "client: CA added to trust store");

    status =  nx_secure_tls_remote_certificate_allocate(&h_client_session,
                                                        &h_remote_certificate,
                                                        h_remote_cert_buffer,
                                                        sizeof(h_remote_cert_buffer));
    (VOID) H_OK(status, "client: remote cert buffer");

    status =  nx_secure_tls_remote_certificate_allocate(&h_client_session,
                                                        &h_remote_issuer,
                                                        h_remote_issuer_buffer,
                                                        sizeof(h_remote_issuer_buffer));
    (VOID) H_OK(status, "client: remote issuer buffer");

    status =  nx_tcp_client_socket_bind(&h_client_socket, NX_ANY_PORT,
                                        5UL * NX_IP_PERIODIC_RATE);
    (VOID) H_OK(status, "client: bind");

    status =  nx_tcp_client_socket_connect(&h_client_socket, H_IP1_ADDRESS,
                                           H_PORT, 20UL * NX_IP_PERIODIC_RATE);
    if (!H_OK(status, "client: tcp connect"))
    {
        return(TX_FALSE);
    }

    /*
     * THE MEASUREMENT.  Everything from ClientHello to Finished: negotiation,
     * the server's certificate chain parsed and verified against the trust
     * store, the key exchange, key derivation and the handshake hash.
     *
     * Note that this wall time INCLUDES the server's own arithmetic, because
     * the server is another thread on the same 68k and the client is blocked
     * waiting for it.  It is a whole-machine figure, and the server's own
     * measurement is reported alongside so the two can be separated.
     */
    start =  ami_tls_eclock();
    status = nx_secure_tls_session_start(&h_client_session, &h_client_socket,
                                         120UL * NX_IP_PERIODIC_RATE);
    h_client_handshake_us = ami_tls_eclock_micros(ami_tls_eclock() - start);

    if (!H_OK(status, "client: TLS handshake completed"))
    {
        (VOID) nx_secure_tls_session_delete(&h_client_session);
        (VOID) nx_tcp_socket_disconnect(&h_client_socket,
                                        5UL * NX_IP_PERIODIC_RATE);
        (VOID) nx_tcp_client_socket_unbind(&h_client_socket);
        (VOID) nx_tcp_socket_delete(&h_client_socket);
        return(TX_FALSE);
    }

    h_log("  negotiated ciphersuite 0x%lx, TLS version 0x%lx",
          (ULONG)h_client_session.nx_secure_tls_session_ciphersuite ->
              nx_secure_tls_ciphersuite,
          (ULONG)h_client_session.nx_secure_tls_protocol_version);

    /* One application record, to prove the session actually carries data. */
    status =  nx_secure_tls_packet_allocate(&h_client_session, &h_pool,
                                            &packet_ptr,
                                            5UL * NX_IP_PERIODIC_RATE);
    if (H_OK(status, "client: tls packet allocate"))
    {
        status = nx_packet_data_append(packet_ptr, (VOID *)h_message,
                                       (ULONG)sizeof(h_message), &h_pool,
                                       5UL * NX_IP_PERIODIC_RATE);
        (VOID) H_OK(status, "client: data append");

        status = nx_secure_tls_session_send(&h_client_session, packet_ptr,
                                            10UL * NX_IP_PERIODIC_RATE);
        if (!H_OK(status, "client: encrypted record sent"))
        {
            (VOID) nx_packet_release(packet_ptr);
        }
    }

    (VOID) nx_secure_tls_session_end(&h_client_session,
                                     5UL * NX_IP_PERIODIC_RATE);
    (VOID) nx_secure_tls_session_delete(&h_client_session);

    (VOID) nx_tcp_socket_disconnect(&h_client_socket,
                                    5UL * NX_IP_PERIODIC_RATE);
    (VOID) nx_tcp_client_socket_unbind(&h_client_socket);
    (VOID) nx_tcp_socket_delete(&h_client_socket);

    status =  tx_semaphore_get(&h_server_done, 200UL * NX_IP_PERIODIC_RATE);
    (VOID) H_TX_OK(status, "client: server half completed");

    return(TX_TRUE);
}


/* ------------------------------------------------------ ThreadX startup --- */

VOID tx_application_define(VOID *first_unused_memory)
{

UINT    status;


    (VOID) first_unused_memory;

    nx_system_initialize();

    status =  nx_packet_pool_create(&h_pool, "AmiNetXDuo TLS pool",
                                    H_PACKET_PAYLOAD, (VOID *)h_pool_memory,
                                    (ULONG)sizeof(h_pool_memory));
    (VOID) H_OK(status, "define: packet pool");

    status =  nx_ip_create(&h_ip0, "ip0", H_IP0_ADDRESS, H_NETMASK, &h_pool,
                           _nx_ram_network_driver, (VOID *)h_ip0_stack,
                           (ULONG)sizeof(h_ip0_stack), 1);
    (VOID) H_OK(status, "define: ip0 create");

    status =  nx_ip_create(&h_ip1, "ip1", H_IP1_ADDRESS, H_NETMASK, &h_pool,
                           _nx_ram_network_driver, (VOID *)h_ip1_stack,
                           (ULONG)sizeof(h_ip1_stack), 1);
    (VOID) H_OK(status, "define: ip1 create");

    status =  nx_arp_enable(&h_ip0, (VOID *)h_arp0_cache, (ULONG)sizeof(h_arp0_cache));
    (VOID) H_OK(status, "define: ip0 arp");

    status =  nx_arp_enable(&h_ip1, (VOID *)h_arp1_cache, (ULONG)sizeof(h_arp1_cache));
    (VOID) H_OK(status, "define: ip1 arp");

    status =  nx_tcp_enable(&h_ip0);
    (VOID) H_OK(status, "define: ip0 tcp");

    status =  nx_tcp_enable(&h_ip1);
    (VOID) H_OK(status, "define: ip1 tcp");

    /* Returns void: it only builds nx_secure's internal session list. */
    nx_secure_tls_initialize();

    status =  tx_semaphore_create(&h_server_done, "server done", 0);
    (VOID) H_TX_OK(status, "define: semaphore");

    status =  tx_thread_create(&h_server_thread, "tls server", h_server_entry,
                               0UL, (VOID *)h_server_stack,
                               (ULONG)sizeof(h_server_stack), 16, 16,
                               TX_NO_TIME_SLICE, TX_AUTO_START);
    (VOID) H_TX_OK(status, "define: server thread");
}


/* ------------------------------------------------------------------ main -- */

int main(VOID)
{

UINT    status;
ULONG   whole_start;
ULONG   whole_us;


    ami_crash_set_reference((APTR)main, "tls_handshake");
    if (!ami_crash_install())
    {
        h_log("CRASHED -- see the serial log and DH0:crash.txt");
        h_flush();
        ami_crash_remove();
        return(20);
    }

    h_log("AmiNetXDuo -- TLS 1.2 loopback handshake (docs/RESEARCH.md 9 gate)");

    if (!ami_tls_timer_open())
    {
        h_log("FATAL: timer.device UNIT_ECLOCK would not open");
        h_flush();
        ami_crash_remove();
        return(20);
    }

    status =  tx_amiga_kernel_start();
    if (status != TX_SUCCESS)
    {
        h_log("FATAL: tx_amiga_kernel_start() = %lu", (ULONG)status);
        h_flush();
        ami_crash_remove();
        return(20);
    }

    status =  tx_amiga_adopt_thread(&h_main_thread, "tls client", 16);
    if (!H_TX_OK(status, "main: adopted this Exec Task"))
    {
        h_flush();
        ami_crash_remove();
        return(20);
    }

    whole_start = ami_tls_eclock();
    (VOID) h_client_run();
    whole_us = ami_tls_eclock_micros(ami_tls_eclock() - whole_start);

    (VOID) tx_amiga_orphan_thread(&h_main_thread);

    h_log("");
    h_log("Handshake cost (measured, not composed):");
    h_log("  client nx_secure_tls_session_start : %lu.%lu s",
          h_client_handshake_us / 1000000UL,
          (h_client_handshake_us % 1000000UL) / 100000UL);
    h_log("  server nx_secure_tls_session_start : %lu.%lu s",
          h_server_handshake_us / 1000000UL,
          (h_server_handshake_us % 1000000UL) / 100000UL);
    h_log("  connect through first record       : %lu.%lu s",
          whole_us / 1000000UL, (whole_us % 1000000UL) / 100000UL);
    h_log("  (both halves ran on this one CPU, so the client figure contains");
    h_log("   the server's arithmetic and vice versa -- see the file header)");

    h_log("");
    h_log("%lu checks, %lu failures -- %s", h_checks, h_failures,
          (LONG)((h_failures == 0UL) ? "PASS" : "FAIL"));

    ami_tls_timer_close();
    h_flush();
    ami_crash_remove();

    return((h_failures == 0UL) ? 0 : 20);
}

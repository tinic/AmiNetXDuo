/*
 * AmiNetXDuo, a real TLS 1.2 handshake, on a real 68k, end to end.
 *
 * tls_bench.c times the primitives.  This times the thing itself: a complete
 * TLS 1.2 handshake between an nx_secure client and an nx_secure server, over
 * real TCP, on one Amiga, including everything the primitive benchmark leaves
 * out, ClientHello/ServerHello negotiation, X.509 DER parsing, certificate
 * chain verification against a trust store, key derivation, Finished hashing
 * and the record layer.
 *
 *   tls_handshake   one round in the shipping configuration: crypto68k
 *                   arithmetic, CRT on every private-key path.  This is the
 *                   correctness gate and the headline number.
 *
 *   tls_decompose   four rounds, {reference, crypto68k} x {no CRT, CRT},
 *                   so the before number, the after number and the two levers
 *                   in between are measured through identical instrumentation
 *                   in one process.  Composing a decomposition out of separate
 *                   runs on an inexact emulator gives a total that does not
 *                   add up.
 *
 *   tls_interop     the same handshake with our fast crypto on one side and
 *                   the unmodified vendored ciphersuite table on the other,
 *                   both ways round, plus vendored-on-both.  If both ends
 *                   change together a mutual arithmetic error is invisible;
 *                   this round makes it visible.
 *
 *   Reaching a real HTTPS server from the emulator is possible in principle
 *   (SLIRP gives outbound internet, and tests/netstack proves DNS and routing
 *   work), but it makes the measurement depend on a certificate chain we do
 *   not control, a trust store we would have to ship, and a server's patience.
 *   Running both halves locally measures the same arithmetic with none of that
 *   variance.  The cost is that one Amiga does both sides' work, so the wall
 *   time here is a client handshake plus a server handshake; the two are
 *   reported separately.
 *
 *   A client fetching a page never performs the private-key operation, and
 *   after this work the server's private-key operation is most of the loopback
 *   total, so the per-role arithmetic breakdown below is the figure to read
 *   for what a client costs.
 *
 *   Same fabric as tests/ram_driver: ThreadX on Exec, two NX_IP instances
 *   talking over the in-tree simulated RAM driver, the server on a
 *   ThreadX-created thread and the client on this process's own adopted Exec
 *   Task.  On top of that, one NX_SECURE_TLS_SESSION each, per round.
 *
 * SPDX-License-Identifier: MIT
 */

#include "tx_api.h"
#include "tx_amiga.h"
#include "nx_api.h"
#include "nx_secure_tls_api.h"

#include "tls.h"
#include "ami_tls_crypto.h"
#include "c68k_p256.h"
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

#define H_LOG_SIZE      12288

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

/* Seconds and one decimal, from microseconds, without floating point. */
#define H_SEC(us)       ((ULONG)(us) / 1000000UL)
#define H_TENTH(us)     (((ULONG)(us) % 1000000UL) / 100000UL)


/* ------------------------------------------------------------- the rounds -- */

/*
 * The vendored tables, still exported by nx_crypto even though we no longer
 * use them by default.  They are what "unmodified nx_secure" means in the
 * interop rounds.
 */
extern const NX_SECURE_TLS_CRYPTO       nx_crypto_tls_ciphers_ecc;
extern const USHORT                     nx_crypto_ecc_supported_groups[];
extern const NX_CRYPTO_METHOD          *nx_crypto_ecc_curves[];
extern const UINT                       nx_crypto_ecc_supported_groups_size;

typedef struct H_ROUND_STRUCT
{
    const char *h_label;
    UINT        h_client_fast;      /* TX_TRUE: our tables; else vendored   */
    UINT        h_server_fast;
    UINT        h_arithmetic;       /* AMI_TLS_ARITH_*                      */
    UINT        h_crt;              /* recover primes on the paths that skip */
    UINT        h_expect_crt;       /* assert the server really used CRT     */
} H_ROUND;

#if defined(H_ROUNDS_DECOMPOSE)

static const H_ROUND h_rounds[] =
{
    {"reference arithmetic, no CRT  (the M9 gate baseline)",
     TX_TRUE, TX_TRUE, AMI_TLS_ARITH_REFERENCE, TX_FALSE, TX_FALSE},
    {"reference arithmetic, CRT     (the CRT lever alone)",
     TX_TRUE, TX_TRUE, AMI_TLS_ARITH_REFERENCE, TX_TRUE,  TX_TRUE},
    {"crypto68k arithmetic, no CRT  (the module lever alone)",
     TX_TRUE, TX_TRUE, AMI_TLS_ARITH_C68K,      TX_FALSE, TX_FALSE},
    {"crypto68k arithmetic, CRT     (the shipping configuration)",
     TX_TRUE, TX_TRUE, AMI_TLS_ARITH_C68K,      TX_TRUE,  TX_TRUE},
};

#elif defined(H_ROUNDS_INTEROP)

static const H_ROUND h_rounds[] =
{
    {"crypto68k CLIENT against a stock nx_secure SERVER",
     TX_TRUE,  TX_FALSE, AMI_TLS_ARITH_C68K, TX_TRUE, TX_FALSE},
    {"stock nx_secure CLIENT against a crypto68k SERVER",
     TX_FALSE, TX_TRUE,  AMI_TLS_ARITH_C68K, TX_TRUE, TX_TRUE},
    {"stock nx_secure on BOTH sides (the control)",
     TX_FALSE, TX_FALSE, AMI_TLS_ARITH_C68K, TX_TRUE, TX_FALSE},
};

#else

static const H_ROUND h_rounds[] =
{
    {"crypto68k arithmetic, CRT     (the shipping configuration)",
     TX_TRUE, TX_TRUE, AMI_TLS_ARITH_C68K, TX_TRUE, TX_TRUE},
};

#endif

#define H_ROUND_COUNT   (sizeof(h_rounds) / sizeof(H_ROUND))

static volatile UINT    h_round;


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
 * nx_secure_tls_metadata_size_calculate() computes the exact requirement from
 * the ciphersuite table, and the value is printed at run time so the memory
 * figure in the report is measured rather than assumed.
 *
 * It grew when src/tls/ami_tls_crypto.c was wired in: our RSA method carries
 * 6 KB of sliding-window scratch on top of the vendored NX_CRYPTO_RSA, and a
 * session allocates a public-cipher slot and a public-auth slot at the largest
 * size in the table, so that is ~12 KB.
 *
 * The packet reassembly buffer must hold the largest single handshake message,
 * which here is the Certificate message carrying the chain.  8 KB holds a
 * two-certificate RSA-2048 chain with room to spare.
 */
#define H_METADATA_SIZE         32768
#define H_PACKET_BUFFER_SIZE    8192

extern VOID _nx_ram_network_driver(NX_IP_DRIVER *driver_req_ptr);

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

/*
 * Signals that the server is listening and the client may connect.  Without it
 * the two halves race, and nx_tcp_client_socket_connect() against a port with
 * no listener does not wait, it comes straight back NX_NOT_CONNECTED.  The
 * client only won before the rounds became a loop, when the server's setup
 * happened once at thread start.
 */
static TX_SEMAPHORE             h_server_ready;

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

/* Per-round results, filled in by whichever half measured them. */
static ULONG    h_server_handshake_us[H_ROUND_COUNT];
static ULONG    h_client_handshake_us[H_ROUND_COUNT];
static ULONG    h_whole_us[H_ROUND_COUNT];
static ULONG    h_client_arith_us[H_ROUND_COUNT];
static ULONG    h_server_arith_us[H_ROUND_COUNT];
static AMI_TLS_CRYPTO_COUNTERS  h_client_ops[H_ROUND_COUNT];
static AMI_TLS_CRYPTO_COUNTERS  h_server_ops[H_ROUND_COUNT];
static USHORT   h_negotiated[H_ROUND_COUNT];

static const char h_message[] = "AmiNetXDuo TLS 1.2 loopback";

static ULONG h_arith_total(const AMI_TLS_CRYPTO_COUNTERS *c)
{

    return(c -> ami_rsa_public_us + c -> ami_rsa_private_crt_us +
           c -> ami_rsa_private_plain_us + c -> ami_ec_multiple_us);
}


/* --------------------------------------------------------- server half --- */

static VOID h_server_round(const H_ROUND *round)
{

UINT        status;
NX_PACKET  *packet_ptr;
ULONG       start;


    status =  nx_tcp_socket_create(&h_ip1, &h_server_socket, "tls server",
                                   NX_IP_NORMAL, NX_FRAGMENT_OKAY,
                                   NX_IP_TIME_TO_LIVE, 4096, NX_NULL, NX_NULL);
    (VOID) H_OK(status, "server: tcp socket create");

    status =  nx_secure_tls_session_create(&h_server_session,
                                           round -> h_server_fast ?
                                               &ami_crypto_tls_ciphers_ecc :
                                               &nx_crypto_tls_ciphers_ecc,
                                           (VOID *)h_server_metadata,
                                           (ULONG)sizeof(h_server_metadata));
    if (!H_OK(status, "server: tls session create"))
    {
        (VOID) tx_semaphore_put(&h_server_ready);
        (VOID) tx_semaphore_put(&h_server_done);
        return;
    }

    if (round -> h_server_fast)
    {
        status = nx_secure_tls_ecc_initialize(&h_server_session,
                                              ami_crypto_ecc_supported_groups,
                                              ami_crypto_ecc_supported_groups_size,
                                              ami_crypto_ecc_curves);
    }
    else
    {
        status = nx_secure_tls_ecc_initialize(&h_server_session,
                                              nx_crypto_ecc_supported_groups,
                                              nx_crypto_ecc_supported_groups_size,
                                              nx_crypto_ecc_curves);
    }
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

    /*
     * ami_tls_local_certificate_add() is nx_secure_tls_local_certificate_add()
     * plus ami_tls_rsa_key_register().  The registration is what lets the
     * ECDHE_RSA ServerKeyExchange signature use CRT: nx_secure only ever calls
     * NX_CRYPTO_SET_PRIME_P from nx_secure_process_client_key_exchange.c, so
     * on this path the primes have to arrive from the certificate instead.
     */
    status =  ami_tls_local_certificate_add(&h_server_session,
                                            &h_server_certificate);
    (VOID) H_OK(status, "server: local certificate added");

    status =  nx_tcp_server_socket_listen(&h_ip1, H_PORT, &h_server_socket, 5,
                                          NX_NULL);
    (VOID) H_OK(status, "server: listen");

    (VOID) tx_semaphore_put(&h_server_ready);

    status =  nx_tcp_server_socket_accept(&h_server_socket,
                                          30UL * NX_IP_PERIODIC_RATE);
    if (!H_OK(status, "server: tcp accept"))
    {
        (VOID) nx_secure_tls_session_delete(&h_server_session);
        (VOID) nx_tcp_server_socket_unlisten(&h_ip1, H_PORT);
        (VOID) nx_tcp_socket_delete(&h_server_socket);
        (VOID) tx_semaphore_put(&h_server_done);
        return;
    }

    /*
     * The handshake.  On this side it costs one RSA-2048 private operation
     * (the ServerKeyExchange signature under an ECDHE_RSA suite) plus an ECDHE
     * key pair, the expensive half.
     */
    start =  ami_tls_eclock();
    status = nx_secure_tls_session_start(&h_server_session, &h_server_socket,
                                         600UL * NX_IP_PERIODIC_RATE);
    h_server_handshake_us[h_round] = ami_tls_eclock_micros(ami_tls_eclock() - start);

    if (H_OK(status, "server: TLS handshake completed"))
    {
        packet_ptr = NX_NULL;
        status = nx_secure_tls_session_receive(&h_server_session, &packet_ptr,
                                               30UL * NX_IP_PERIODIC_RATE);
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

static VOID h_server_entry(ULONG id)
{

UINT    status;
ULONG   actual;
UINT    i;


    (VOID) id;

    status =  nx_ip_status_check(&h_ip1, NX_IP_INITIALIZE_DONE, &actual,
                                 5UL * NX_IP_PERIODIC_RATE);
    (VOID) H_OK(status, "server: ip1 initialised");

    for (i = 0; i < (UINT)H_ROUND_COUNT; i++)
    {
        while (h_round != i)
        {
            (VOID) tx_thread_sleep(1);
        }

        h_server_round(&h_rounds[i]);
    }
}


/* --------------------------------------------------------- client half --- */

static UINT h_client_round(const H_ROUND *round)
{

UINT        status;
NX_PACKET  *packet_ptr;
ULONG       start;


    status =  nx_tcp_socket_create(&h_ip0, &h_client_socket, "tls client",
                                   NX_IP_NORMAL, NX_FRAGMENT_OKAY,
                                   NX_IP_TIME_TO_LIVE, 4096, NX_NULL, NX_NULL);
    (VOID) H_OK(status, "client: tcp socket create");

    status =  nx_secure_tls_session_create(&h_client_session,
                                           round -> h_client_fast ?
                                               &ami_crypto_tls_ciphers_ecc :
                                               &nx_crypto_tls_ciphers_ecc,
                                           (VOID *)h_client_metadata,
                                           (ULONG)sizeof(h_client_metadata));
    if (!H_OK(status, "client: tls session create"))
    {
        return(TX_FALSE);
    }

    if (round -> h_client_fast)
    {
        status = nx_secure_tls_ecc_initialize(&h_client_session,
                                              ami_crypto_ecc_supported_groups,
                                              ami_crypto_ecc_supported_groups_size,
                                              ami_crypto_ecc_curves);
    }
    else
    {
        status = nx_secure_tls_ecc_initialize(&h_client_session,
                                              nx_crypto_ecc_supported_groups,
                                              nx_crypto_ecc_supported_groups_size,
                                              nx_crypto_ecc_curves);
    }
    (VOID) H_OK(status, "client: ecc curves registered");

    status =  nx_secure_tls_session_packet_buffer_set(&h_client_session,
                                                      (UCHAR *)h_client_packet_buffer,
                                                      (ULONG)sizeof(h_client_packet_buffer));
    (VOID) H_OK(status, "client: packet buffer set");

    /*
     * The trust store, and the buffers the incoming chain is parsed into.
     * Without the remote-certificate allocation nx_secure has nowhere to put
     * what the server sends and the handshake fails with a buffer error
     * rather than a verification error, a distinction that matters when
     * debugging this.
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

    status =  tx_semaphore_get(&h_server_ready, 60UL * NX_IP_PERIODIC_RATE);
    (VOID) H_TX_OK(status, "client: server is listening");

    status =  nx_tcp_client_socket_connect(&h_client_socket, H_IP1_ADDRESS,
                                           H_PORT, 30UL * NX_IP_PERIODIC_RATE);
    if (!H_OK(status, "client: tcp connect"))
    {
        return(TX_FALSE);
    }

    /*
     * The measurement: everything from ClientHello to Finished, negotiation,
     * the server's certificate chain parsed and verified against the trust
     * store, the key exchange, key derivation and the handshake hash.
     *
     * This wall time includes the server's own arithmetic, because the server
     * is another thread on the same 68k and the client is blocked waiting for
     * it.  It is a whole-machine figure; the per-role arithmetic totals below
     * separate the two.
     */
    start =  ami_tls_eclock();
    status = nx_secure_tls_session_start(&h_client_session, &h_client_socket,
                                         600UL * NX_IP_PERIODIC_RATE);
    h_client_handshake_us[h_round] = ami_tls_eclock_micros(ami_tls_eclock() - start);

    if (!H_OK(status, "client: TLS handshake completed"))
    {
        (VOID) nx_secure_tls_session_delete(&h_client_session);
        (VOID) nx_tcp_socket_disconnect(&h_client_socket,
                                        5UL * NX_IP_PERIODIC_RATE);
        (VOID) nx_tcp_client_socket_unbind(&h_client_socket);
        (VOID) nx_tcp_socket_delete(&h_client_socket);
        return(TX_FALSE);
    }

    h_negotiated[h_round] = h_client_session.nx_secure_tls_session_ciphersuite ->
                                nx_secure_tls_ciphersuite;

    h_log("  negotiated ciphersuite 0x%lx, TLS version 0x%lx",
          (ULONG)h_negotiated[h_round],
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
                                            30UL * NX_IP_PERIODIC_RATE);
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

    status =  tx_semaphore_get(&h_server_done, 600UL * NX_IP_PERIODIC_RATE);
    (VOID) H_TX_OK(status, "client: server half completed");

    return(TX_TRUE);
}

static UINT h_client_run(VOID)
{

UINT    status;
UINT    ok = TX_TRUE;
UINT    i;
ULONG   actual;
ULONG   metadata_needed = 0;
ULONG   start;


    /*
     * Ask nx_secure how much crypto metadata this ciphersuite table really
     * needs, rather than trusting a round number, the memory figure in the
     * report should be measured.
     *
     * This has to happen here and not in tx_application_define(): the nxe_
     * error-checking wrappers apply NX_THREADS_ONLY_CALLER_CHECKING, so a call
     * from initialization context comes back NX_CALLER_ERROR (0x11) with the
     * output untouched.
     */
    status =  nx_secure_tls_metadata_size_calculate(&ami_crypto_tls_ciphers_ecc,
                                                    &metadata_needed);
    h_log("  crypto metadata, crypto68k table: %lu bytes required, %lu allocated",
          metadata_needed, (ULONG)H_METADATA_SIZE);
    (VOID) h_check((UINT)((status == NX_SUCCESS) &&
                          (metadata_needed <= (ULONG)H_METADATA_SIZE)),
                   "metadata allocation is large enough", (ULONG)status);

    status =  nx_secure_tls_metadata_size_calculate(&nx_crypto_tls_ciphers_ecc,
                                                    &metadata_needed);
    h_log("  crypto metadata, vendored table:  %lu bytes required", metadata_needed);

    status =  nx_ip_status_check(&h_ip0, NX_IP_INITIALIZE_DONE, &actual,
                                 5UL * NX_IP_PERIODIC_RATE);
    (VOID) H_OK(status, "client: ip0 initialised");

    for (i = 0; i < (UINT)H_ROUND_COUNT; i++)
    {
        h_log("");
        h_log("---- round %lu: %s", (ULONG)(i + 1), (LONG)h_rounds[i].h_label);

        ami_tls_crypto_set_arithmetic(h_rounds[i].h_arithmetic);
        ami_tls_crypto_set_crt(h_rounds[i].h_crt);
        ami_tls_crypto_counters_reset();

        h_round = i;

        start = ami_tls_eclock();
        if (!h_client_round(&h_rounds[i]))
        {
            ok = TX_FALSE;
        }
        h_whole_us[i] = ami_tls_eclock_micros(ami_tls_eclock() - start);

        ami_tls_crypto_counters_get(&h_client_ops[i], &h_server_ops[i]);
        h_client_arith_us[i] = h_arith_total(&h_client_ops[i]);
        h_server_arith_us[i] = h_arith_total(&h_server_ops[i]);

        if (h_rounds[i].h_expect_crt)
        {
            (VOID) h_check((UINT)((h_server_ops[i].ami_rsa_private_crt_count > 0UL) &&
                                  (h_server_ops[i].ami_rsa_private_plain_count == 0UL)),
                           "server: RSA private key operation used CRT",
                           h_server_ops[i].ami_rsa_private_plain_count);
        }

        if (h_rounds[i].h_client_fast && (h_rounds[i].h_arithmetic == AMI_TLS_ARITH_C68K))
        {
            (VOID) h_check((UINT)(h_client_ops[i].ami_ec_multiple_count > 0UL),
                           "client: P-256 went through crypto68k",
                           h_client_ops[i].ami_ec_multiple_count);
        }
    }

    return(ok);
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

    status =  tx_semaphore_create(&h_server_ready, "server ready", 0);
    (VOID) H_TX_OK(status, "define: ready semaphore");

    h_round = (UINT)~0u;    /* the server thread waits for round 0 */

    status =  tx_thread_create(&h_server_thread, "tls server", h_server_entry,
                               0UL, (VOID *)h_server_stack,
                               (ULONG)sizeof(h_server_stack), 16, 16,
                               TX_NO_TIME_SLICE, TX_AUTO_START);
    (VOID) H_TX_OK(status, "define: server thread");
}


/* --------------------------------------------------------- the report ----- */

static VOID h_report_ops(const char *who, const AMI_TLS_CRYPTO_COUNTERS *c)
{

    h_log("    %s RSA public   x%lu  %lu.%lu s", (LONG)who,
          c -> ami_rsa_public_count,
          H_SEC(c -> ami_rsa_public_us), H_TENTH(c -> ami_rsa_public_us));
    h_log("    %s RSA priv CRT x%lu  %lu.%lu s", (LONG)who,
          c -> ami_rsa_private_crt_count,
          H_SEC(c -> ami_rsa_private_crt_us), H_TENTH(c -> ami_rsa_private_crt_us));
    h_log("    %s RSA priv full x%lu %lu.%lu s", (LONG)who,
          c -> ami_rsa_private_plain_count,
          H_SEC(c -> ami_rsa_private_plain_us), H_TENTH(c -> ami_rsa_private_plain_us));
    h_log("    %s P-256 k*P    x%lu  %lu.%lu s", (LONG)who,
          c -> ami_ec_multiple_count,
          H_SEC(c -> ami_ec_multiple_us), H_TENTH(c -> ami_ec_multiple_us));
}

static VOID h_report(VOID)
{

UINT    i;


    h_log("");
    h_log("Handshake cost, measured, emulated 68020:");

    for (i = 0; i < (UINT)H_ROUND_COUNT; i++)
    {
        h_log("");
        h_log("  round %lu: %s", (ULONG)(i + 1), (LONG)h_rounds[i].h_label);
        h_log("    negotiated ciphersuite             : 0x%lx",
              (ULONG)h_negotiated[i]);
        h_log("    connect through first record       : %lu.%lu s",
              H_SEC(h_whole_us[i]), H_TENTH(h_whole_us[i]));
        h_log("    client nx_secure_tls_session_start : %lu.%lu s",
              H_SEC(h_client_handshake_us[i]), H_TENTH(h_client_handshake_us[i]));
        h_log("    server nx_secure_tls_session_start : %lu.%lu s",
              H_SEC(h_server_handshake_us[i]), H_TENTH(h_server_handshake_us[i]));
        h_log("    CLIENT-ONLY public-key arithmetic  : %lu.%lu s",
              H_SEC(h_client_arith_us[i]), H_TENTH(h_client_arith_us[i]));
        h_log("    server public-key arithmetic       : %lu.%lu s",
              H_SEC(h_server_arith_us[i]), H_TENTH(h_server_arith_us[i]));

        h_report_ops("client", &h_client_ops[i]);
        h_report_ops("server", &h_server_ops[i]);
    }

    h_log("");
    h_log("  Both halves ran on this one CPU, so the wall figures contain each");
    h_log("  other's arithmetic.  The CLIENT-ONLY line is what a machine");
    h_log("  fetching a page actually pays, a client never performs the");
    h_log("  private-key operation.  Rounds where either side used the stock");
    h_log("  nx_secure tables report zero for that side: the counters live in");
    h_log("  our crypto methods, and the stock ones are not instrumented.");
}


/* ------------------------------------------------------------------ main -- */

int main(VOID)
{

UINT    status;


    ami_crash_set_reference((APTR)main, "tls_handshake");
    if (!ami_crash_install())
    {
        h_log("CRASHED, see the serial log and DH0:crash.txt");
        h_flush();
        ami_crash_remove();
        return(20);
    }

    h_log("AmiNetXDuo, TLS 1.2 loopback handshake (docs/RESEARCH.md 9 gate)");

    if (!ami_tls_timer_open())
    {
        h_log("FATAL: timer.device UNIT_ECLOCK would not open");
        h_flush();
        ami_crash_remove();
        return(20);
    }

    status =  ami_tls_crypto_initialize();
    (VOID) H_OK(status, "main: crypto68k tables built");

    /*
     * The comb table in c68k_p256_table.c is generated, so a table built for
     * the wrong curve would produce points that are self-consistent and wrong.
     * Two generic scalar multiplications settle it.  Nothing else here would
     * catch it: such a table still completes a handshake against a peer that
     * has the same one.
     */
    status =  c68k_p256_self_check();
    (VOID) h_check((UINT)(status == NX_CRYPTO_SUCCESS),
                   "main: P-256 comb table belongs to this curve",
                   (ULONG)status);

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

    /* Everything this thread does goes in the client counter bank. */
    ami_tls_crypto_set_client_thread(&h_main_thread);

    (VOID) h_client_run();

    (VOID) tx_amiga_orphan_thread(&h_main_thread);

    h_report();

    h_log("");
    h_log("%lu checks, %lu failures, %s", h_checks, h_failures,
          (LONG)((h_failures == 0UL) ? "PASS" : "FAIL"));

    ami_tls_timer_close();
    h_flush();
    ami_crash_remove();

    return((h_failures == 0UL) ? 0 : 20);
}

/*
 * AmiNetXDuo, a TLS 1.2 handshake against a real public HTTPS server.
 *
 * tls_handshake.c runs both ends of the conversation, which measures the
 * arithmetic exactly and says nothing about interoperating with anybody else.
 * This talks to a host on the internet that has never heard of us, through
 * FS-UAE's SLIRP NAT, over the real SANA-II path:
 *
 *     DHCP -> DNS -> TCP -> ClientHello -> a chain we did not issue,
 *     verified against a root CA compiled in here -> HTTP GET -> a response.
 *
 * tls-v1-2.badssl.com exists to be connected to, speaks TLS 1.2, its chain is
 * two deep (leaf + Let's Encrypt R13) rather than the four Cloudflare and
 * SSL.com hand out, and, checked, not assumed, it will negotiate
 * TLS_ECDHE_RSA_WITH_AES_128_CBC_SHA256, the same suite the loopback test
 * uses.  That last point matters on this machine: nx_crypto's AES-GCM is a
 * bit-serial GHASH at 345 ms per KB against AES-CBC's 22 ms, so a GCM suite
 * would make the transfer the slow part.
 *
 * Measured cost of that chain, 68020: three signature verifications totalling
 * 4.1 s, not 3 x 0.68 s, because ISRG Root X1 is a 4096-bit key and verifying
 * the intermediate against it is a 4096-bit modular exponentiation, roughly
 * 4x a 2048-bit one.  Root key size is a real term in a client's handshake
 * cost here, and the client does not get to choose it.
 *
 * Not a baseline: it depends on the internet, on DHCP from SLIRP, on a third
 * party's server and on a certificate that rotates every ninety days.  It is
 * built but never part of the pass/fail set.  Only one root CA is compiled in,
 * so it is no evidence that this stack can reach arbitrary sites; a real trust
 * store is a separate and larger problem.
 *
 * SPDX-License-Identifier: MIT
 */

#include "tx_api.h"
#include "tx_amiga.h"
#include "nx_api.h"
#include "nx_secure_tls_api.h"

#include "aminetxduo/netstack.h"
#include "aminetxduo/crashguard.h"

#include "tls.h"
#include "ami_tls_crypto.h"

#include <exec/types.h>
#include <exec/execbase.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <inline/macros.h>
#include <proto/dos.h>

#include <stdarg.h>

#ifdef W_ROOT_LOCAL
#include "tls_root_local.h"
#define W_ROOT_DER      w_root_local_der
#define W_ROOT_DER_LEN  w_root_local_der_len
#else
#include "tls_root_isrg_x1.h"
#define W_ROOT_DER      isrg_root_x1_der
#define W_ROOT_DER_LEN  isrg_root_x1_der_len
#endif


/* ------------------------------------------------------------- logging --- */

#ifndef RawPutChar
#  define RawPutChar(c) \
      LP1NR(0x204, RawPutChar, UBYTE, (c), d0, , EXEC_BASE_NAME)
#endif

#define W_LOG_SIZE      8192

static char     w_log_buffer[W_LOG_SIZE];
static ULONG    w_log_used;

static VOID w_put(UBYTE c)
{

    RawPutChar(c);

    if (w_log_used < (ULONG)(W_LOG_SIZE - 1))
    {
        w_log_buffer[w_log_used++] = (char)c;
    }
}

static VOID w_put_char(register UBYTE c      __asm("d0"),
                       register APTR  unused __asm("a3"))
{

    (VOID) unused;
    if (c != '\0')
    {
        w_put(c);
    }
}

static VOID w_log(const char *fmt, ...)
{

va_list args;


    va_start(args, fmt);
    RawDoFmt((STRPTR)fmt, args, (void (*)()) w_put_char, NULL);
    va_end(args);

    w_put('\n');
}

static VOID w_flush(VOID)
{

BPTR    out;


    out =  Output();
    if (out != (BPTR)0)
    {
        (VOID) Write(out, (APTR)w_log_buffer, (LONG)w_log_used);
    }
}

static ULONG    w_checks;
static ULONG    w_failures;

static UINT w_check(UINT ok, const char *what, ULONG detail)
{

    w_checks++;
    if (!ok)
    {
        w_failures++;
        w_log("  FAIL %s (0x%lx)", (LONG)what, detail);
        return(0u);
    }

    w_log("  ok   %s", (LONG)what);
    return(1u);
}

#define W_OK(status, what)  w_check((UINT)((status) == NX_SUCCESS), (what), (ULONG)(status))

#define W_SEC(us)       ((ULONG)(us) / 1000000UL)
#define W_TENTH(us)     (((ULONG)(us) % 1000000UL) / 100000UL)


/* -------------------------------------------------------------- the run --- */

/* Overridable so this can be aimed at tests/peer/httppeer.py's local PKI,
   where the chain is short enough to verify at 14 MHz and the step-by-step
   output below is the only working view into a handshake: tls.library links
   no logger and RawPutChar loses characters to the emulated serial. */
#ifndef W_HOST
#define W_HOST          "tls-v1-2.badssl.com"
#endif
#ifndef W_PORT
#define W_PORT          443
#endif
/* The check names carry the port, so -DW_PORT=1012 has to reach them too:
   a transcript that says 443 while the connection went somewhere else is a
   record of the wrong run. */
#define W_STRINGIFY2(x) #x
#define W_STRINGIFY(x)  W_STRINGIFY2(x)
#define W_PORT_TEXT     W_STRINGIFY(W_PORT)
#define W_DNS_TIMEOUT   (20UL * NX_IP_PERIODIC_RATE)

#define W_METADATA_SIZE         32768
#define W_PACKET_BUFFER_SIZE    12288       /* a public chain is bigger      */
#define W_CERT_BUFFER_SIZE      3072

static NX_TCP_SOCKET            w_socket;
static NX_SECURE_TLS_SESSION    w_session;
static NX_SECURE_X509_CERT      w_root;
static NX_SECURE_X509_CERT      w_remote[3];
static NX_SECURE_X509_DNS_NAME  w_sni;

static ULONG    w_metadata[W_METADATA_SIZE / sizeof(ULONG)];
static ULONG    w_packet_buffer[W_PACKET_BUFFER_SIZE / sizeof(ULONG)];
static UCHAR    w_cert_buffer[3][W_CERT_BUFFER_SIZE];

static const char w_request[] =
    "GET / HTTP/1.0\r\n"
    "Host: " W_HOST "\r\n"
    "Connection: close\r\n"
    "User-Agent: AmiNetXDuo/0 (m68k)\r\n"
    "\r\n";

static UINT w_run(VOID)
{

NX_IP                      *ip;
NX_PACKET                  *packet_ptr;
NX_PACKET_POOL             *pool;
UINT                        status;
LONG                        net_status;
ULONG                       address = 0;
ULONG                       start;
ULONG                       handshake_us;
ULONG                       bytes;
ULONG                       i;
UCHAR                      *data;
ULONG                       length;
AMI_TLS_CRYPTO_COUNTERS     ops;
AMI_TLS_CRYPTO_COUNTERS     other;
ULONG                       arith_us;
char                        line[96];


    ip   =  netstack_ip();
    pool =  netstack_pool();

    if (!w_check((UINT)((ip != NX_NULL) && (pool != NX_NULL)),
                 "netstack is up", 0UL))
    {
        return(0u);
    }

    net_status =  netstack_resolve(W_HOST, &address, W_DNS_TIMEOUT);
    if (!w_check((UINT)((net_status == 0) && (address != 0UL)),
                 "DNS: " W_HOST " resolved", (ULONG)net_status))
    {
        return(0u);
    }

    w_log("  %s is %lu.%lu.%lu.%lu", (LONG)W_HOST,
          (address >> 24) & 0xFFUL, (address >> 16) & 0xFFUL,
          (address >> 8) & 0xFFUL, address & 0xFFUL);

    status =  nx_tcp_socket_create(ip, &w_socket, "https", NX_IP_NORMAL,
                                   NX_FRAGMENT_OKAY, NX_IP_TIME_TO_LIVE,
                                   8192, NX_NULL, NX_NULL);
    (VOID) W_OK(status, "tcp socket create");

    status =  nx_tcp_client_socket_bind(&w_socket, NX_ANY_PORT,
                                        5UL * NX_IP_PERIODIC_RATE);
    (VOID) W_OK(status, "tcp bind");

    status =  nx_tcp_client_socket_connect(&w_socket, address, W_PORT,
                                           30UL * NX_IP_PERIODIC_RATE);
    if (!W_OK(status, "tcp connect to port " W_PORT_TEXT))
    {
        return(0u);
    }

    status =  nx_secure_tls_session_create(&w_session,
                                           &ami_crypto_tls_ciphers_ecc,
                                           (VOID *)w_metadata,
                                           (ULONG)sizeof(w_metadata));
    if (!W_OK(status, "tls session create"))
    {
        return(0u);
    }

    status =  nx_secure_tls_ecc_initialize(&w_session,
                                           ami_crypto_ecc_supported_groups,
                                           ami_crypto_ecc_supported_groups_size,
                                           ami_crypto_ecc_curves);
    (VOID) W_OK(status, "ecc curves registered");

    /*
     * WHAT THE CLIENTHELLO OFFERS IS NOT THE SAME LIST, and this offered all
     * three.
     *
     * nx_secure_tls_ecc_initialize() writes the session's TLS list and the
     * process-wide X.509 list from one call, so it is given the complete set
     * for the sake of certificate chains -- a chain carries P-384 keys that
     * still have to be checked -- and the session's own list is then narrowed
     * to what is worth offering.  Without the narrowing,
     * _nx_secure_tls_1_3_crypto_init() generates a key pair for every group
     * the session lists, inside nx_secure_tls_session_start() and before the
     * ClientHello leaves.  Measured here, A1200 68020 at 13.08 MHz:
     *
     *     P-256   1.1 s      P-384   4.5 s      P-521   8.9 s
     *
     * Fourteen and a half seconds of arithmetic between the TCP connect and
     * the first byte of the handshake, for two groups no server in reach
     * picks.  tls-v1-2.badssl.com closed the connection inside that window and
     * nx_secure_tls_session_start() returned NX_NOT_CONNECTED at 14.8 s.  The
     * peer was not at fault: this test was holding a connection open through
     * fourteen seconds of work it had no reason to do.  Narrowed, the same
     * step is 1.1 s and the ServerHello arrives 0.2 s later.
     *
     * tls.library does the same narrowing at src/tlslib/tls_conn.c:624, which
     * is the difference between the shipping path and this one.
     */
    w_session.nx_secure_tls_ecc.nx_secure_tls_ecc_supported_groups =
        ami_crypto_ecc_offered_groups;
    w_session.nx_secure_tls_ecc.nx_secure_tls_ecc_supported_groups_count =
        (USHORT)ami_crypto_ecc_offered_groups_size;
    w_session.nx_secure_tls_ecc.nx_secure_tls_ecc_curves =
        ami_crypto_ecc_offered_curves;

    status =  nx_secure_tls_session_packet_buffer_set(&w_session,
                                                      (UCHAR *)w_packet_buffer,
                                                      (ULONG)sizeof(w_packet_buffer));
    (VOID) W_OK(status, "packet buffer set");

    status =  nx_secure_x509_certificate_initialize(&w_root,
                                                    (UCHAR *)W_ROOT_DER,
                                                    W_ROOT_DER_LEN,
                                                    NX_NULL, 0, NX_NULL, 0,
                                                    NX_SECURE_X509_KEY_TYPE_NONE);
    (VOID) W_OK(status, "ISRG Root X1 parsed");

    status =  nx_secure_tls_trusted_certificate_add(&w_session, &w_root);
    (VOID) W_OK(status, "root added to trust store");

    for (i = 0; i < 3UL; i++)
    {
        status = nx_secure_tls_remote_certificate_allocate(&w_session,
                                                           &w_remote[i],
                                                           w_cert_buffer[i],
                                                           W_CERT_BUFFER_SIZE);
        (VOID) W_OK(status, "remote certificate buffer");
    }

    /*
     * SNI is not optional against anything modern, a shared-IP host answers
     * with the wrong certificate, or a handshake_failure, without it.  The DNS
     * name struct must outlive the handshake, so it is static.
     */
    for (i = 0; (i < (ULONG)(sizeof(W_HOST) - 1UL)) &&
                (i < (ULONG)NX_SECURE_X509_DNS_NAME_MAX); i++)
    {
        w_sni.nx_secure_x509_dns_name[i] = (UCHAR)W_HOST[i];
    }
    w_sni.nx_secure_x509_dns_name_length = (USHORT)i;
    w_sni.nx_secure_x509_dns_name_next   = NX_NULL;

    status =  nx_secure_tls_session_sni_extension_set(&w_session, &w_sni);
    (VOID) W_OK(status, "SNI extension set");

    ami_tls_crypto_counters_reset();

    start = ami_tls_eclock();
    status = nx_secure_tls_session_start(&w_session, &w_socket,
                                         120UL * NX_IP_PERIODIC_RATE);
    handshake_us = ami_tls_eclock_micros(ami_tls_eclock() - start);

    /*
     * One thread does everything here and is not registered as the client,
     * there is no server half to tell it apart from, so fold both counter
     * banks together rather than reading the one that happens to be empty.
     */
    ami_tls_crypto_counters_get(&ops, &other);

    ops.ami_rsa_public_count        += other.ami_rsa_public_count;
    ops.ami_rsa_public_us           += other.ami_rsa_public_us;
    ops.ami_rsa_private_crt_count   += other.ami_rsa_private_crt_count;
    ops.ami_rsa_private_crt_us      += other.ami_rsa_private_crt_us;
    ops.ami_rsa_private_plain_count += other.ami_rsa_private_plain_count;
    ops.ami_rsa_private_plain_us    += other.ami_rsa_private_plain_us;
    ops.ami_ec_multiple_count       += other.ami_ec_multiple_count;
    ops.ami_ec_multiple_us          += other.ami_ec_multiple_us;

    arith_us =  ops.ami_rsa_public_us + ops.ami_ec_multiple_us;

    if (!W_OK(status, "TLS handshake with a server that never heard of us"))
    {
        w_log("  handshake failed after %lu.%lu s", W_SEC(handshake_us),
              W_TENTH(handshake_us));
        (VOID) nx_secure_tls_session_delete(&w_session);
        return(0u);
    }

    /* nx_secure_tls_protocol_version is the legacy record version and stays
       0x0303 on a TLS 1.3 connection (RFC 8446 5.1), so it is not the answer
       to "which version did this negotiate".  Same translation TLSInfo()
       does. */
    w_log("  negotiated ciphersuite 0x%lx, TLS version 0x%lx",
          (ULONG)w_session.nx_secure_tls_session_ciphersuite ->
              nx_secure_tls_ciphersuite,
          (w_session.nx_secure_tls_1_3 != 0) ? 0x304UL :
              (ULONG)w_session.nx_secure_tls_protocol_version);
    w_log("  handshake, connect to Finished  : %lu.%lu s",
          W_SEC(handshake_us), W_TENTH(handshake_us));
    w_log("  of which public-key arithmetic  : %lu.%lu s",
          W_SEC(arith_us), W_TENTH(arith_us));
    w_log("    RSA-2048 verify x%lu %lu.%lu s / P-256 k*P x%lu %lu.%lu s",
          ops.ami_rsa_public_count,
          W_SEC(ops.ami_rsa_public_us), W_TENTH(ops.ami_rsa_public_us),
          ops.ami_ec_multiple_count,
          W_SEC(ops.ami_ec_multiple_us), W_TENTH(ops.ami_ec_multiple_us));

    (VOID) w_check((UINT)(ops.ami_rsa_private_crt_count == 0UL &&
                          ops.ami_rsa_private_plain_count == 0UL),
                   "client performed no private-key operation",
                   ops.ami_rsa_private_plain_count);

    /* ------------------------------------------------------- the request -- */

    status =  nx_secure_tls_packet_allocate(&w_session, pool, &packet_ptr,
                                            5UL * NX_IP_PERIODIC_RATE);
    (VOID) W_OK(status, "tls packet allocate");

    if (status == NX_SUCCESS)
    {
        status = nx_packet_data_append(packet_ptr, (VOID *)w_request,
                                       (ULONG)(sizeof(w_request) - 1UL), pool,
                                       5UL * NX_IP_PERIODIC_RATE);
        (VOID) W_OK(status, "request appended");

        status = nx_secure_tls_session_send(&w_session, packet_ptr,
                                            30UL * NX_IP_PERIODIC_RATE);
        if (!W_OK(status, "GET / sent over TLS"))
        {
            (VOID) nx_packet_release(packet_ptr);
        }
    }

    packet_ptr = NX_NULL;
    status =  nx_secure_tls_session_receive(&w_session, &packet_ptr,
                                            60UL * NX_IP_PERIODIC_RATE);
    if (W_OK(status, "encrypted response received"))
    {
        bytes  =  packet_ptr -> nx_packet_length;
        data   =  packet_ptr -> nx_packet_prepend_ptr;
        length =  (ULONG)(packet_ptr -> nx_packet_append_ptr - data);

        for (i = 0; (i < (ULONG)(sizeof(line) - 1UL)) && (i < length); i++)
        {
            if ((data[i] == (UCHAR)'\r') || (data[i] == (UCHAR)'\n'))
            {
                break;
            }
            line[i] = (char)data[i];
        }
        line[i] = '\0';

        w_log("  %lu bytes, first line: %s", bytes, (LONG)line);

        (VOID) w_check((UINT)((i > 8UL) &&
                              (line[0] == 'H') && (line[1] == 'T') &&
                              (line[2] == 'T') && (line[3] == 'P')),
                       "response is HTTP", (ULONG)i);

        (VOID) nx_packet_release(packet_ptr);
    }

    (VOID) nx_secure_tls_session_end(&w_session, 5UL * NX_IP_PERIODIC_RATE);
    (VOID) nx_secure_tls_session_delete(&w_session);

    (VOID) nx_tcp_socket_disconnect(&w_socket, 5UL * NX_IP_PERIODIC_RATE);
    (VOID) nx_tcp_client_socket_unbind(&w_socket);
    (VOID) nx_tcp_socket_delete(&w_socket);

    return(1u);
}


/* ------------------------------------------------------------------ main -- */

int main(VOID)
{

AmiNetCaller    caller;
UINT            status;
LONG            net_status;


    ami_crash_set_reference((APTR)main, "tls_https");
    if (!ami_crash_install())
    {
        w_log("CRASHED, see the serial log and DH0:crash.txt");
        w_flush();
        ami_crash_remove();
        return(20);
    }

    w_log("AmiNetXDuo, TLS 1.2 against a real public HTTPS server");

    (VOID) ami_tls_timer_open();

    status =  ami_tls_crypto_initialize();
    (VOID) W_OK(status, "crypto68k tables built");

    net_status =  netstack_startup();
    if (!w_check((UINT)(net_status == 0), "netstack_startup()",
                 (ULONG)net_status))
    {
        w_log("");
        w_log("%lu checks, %lu failures, SKIPPED (no network)",
              w_checks, w_failures);
        ami_tls_timer_close();
        w_flush();
        ami_crash_remove();
        return(0);
    }

    if (ami_netstack_enter(&caller) == 0)
    {
        (VOID) w_run();
        ami_netstack_leave(&caller);
    }
    else
    {
        (VOID) w_check(0u, "ami_netstack_enter()", 0UL);
    }

    netstack_shutdown();

    w_log("");
    w_log("%lu checks, %lu failures, %s", w_checks, w_failures,
          (LONG)((w_failures == 0UL) ? "PASS" : "FAIL"));

    ami_tls_timer_close();
    w_flush();
    ami_crash_remove();

    return((w_failures == 0UL) ? 0 : 20);
}

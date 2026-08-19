/*
 * tls.library, borrowing bsdsocket.library's NetX Duo instead of linking one.
 *
 *   NetX Duo and ThreadX are singletons with file-scope state: the IP thread,
 *   the created-object lists, _tx_thread_current_ptr.  bsdsocket.library
 *   contains the only copy on the machine.  A netxduo linked into this library
 *   too gives it a second, private set of those globals, a stack with no
 *   interfaces and a scheduler with no threads.  Every call then fails in a
 *   way that looks like a NetX Duo bug.
 *
 *   nx_secure and nx_crypto have no such state that is not per-session, so a
 *   link here is correct and keeps 227 KB out of the resident library of every
 *   machine that never makes a TLS connection.
 *
 *   nx_secure calls twelve NetX Duo/ThreadX entry points and nothing else,
 *   measured with `nm` over the archives, not assumed.  This file defines those
 *   twelve names, so the linker resolves nx_secure's references here, and each
 *   definition is a one-line call through the table bsdsocket.library supplies
 *   over its private LVO.  No vendored source is edited and no NetX Duo object
 *   is duplicated.
 *
 *   The same mechanism carries the entropy pool.  NX_RAND is ami_random_rand()
 *   (port/netxduo-amiga/inc/nx_port.h), and the ami_random_* names are defined
 *   here rather than linked from src/common/ami_random.c, so a TLS handshake
 *   draws from the pool bsdsocket.library already seeded, one pool per
 *   machine, not two.
 *
 *   Every forwarder checks for a context.  A forwarder reached without a
 *   context is a bug in this library (TLSOpenA binds before it does anything
 *   else), but the failure must be a status code and not a jump through NULL
 *   on a machine with no memory protection.
 *
 * SPDX-License-Identifier: MIT
 */

#include "tls_internal.h"

#include "crypto68k.h"

#include <proto/exec.h>

static const AmiNetXDuoContext *tls_ctx;

/* --------------------------------------------------------- acquisition --- */

/*
 * The private LVO, called by hand.  The register assignment is stated in
 * include/aminetxduo/nxcontext.h and implemented in
 * src/bsdsocket/nxcontext.c.  This is the third and last place it appears.
 */
#ifdef TLSLIB_HOST_TEST
extern LONG tls_test_obtain_context(APTR socket_base,
                                    const AmiNetXDuoContext **out);
#define tls_obtain_context tls_test_obtain_context
#else
static LONG tls_obtain_context(APTR socket_base, const AmiNetXDuoContext **out)
{
    register APTR                      a6 __asm("a6") = socket_base;
    register const AmiNetXDuoContext **a0 __asm("a0") = out;
    register ULONG                     d0 __asm("d0") = AMI_NXD_CONTEXT_MAGIC;
    register ULONG                     d1 __asm("d1") = AMI_NXD_CONTEXT_VERSION;
    register LONG                      res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");

    __asm __volatile ("jsr a6@(-864:W)"     /* -0x360, AMI_NXD_CONTEXT_LVO */
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0)
                      : "r" (a6), "r" (a0), "r" (d0), "r" (d1)
                      : "a1", "cc", "memory");
    return res;
}
#endif

/*
 * The handshake's public-key arithmetic is the longest stretch of code in
 * this library that makes no ThreadX call, and the port only reschedules at
 * ThreadX API boundaries, so without this the IP thread does not run for the
 * length of a key exchange or a chain check.  crypto68k calls this between
 * iterations.  The pair is a no-op for a caller that does not hold the baton,
 * and nesting-safe for one that does.
 */
static VOID tls_crypto_yield(VOID)
{
    const AmiNetXDuoContext *ctx = tls_ctx;

    if (ctx != NULL)
    {
        ctx->nxc_BatonRelease();
        ctx->nxc_BatonAcquire();
    }
}

LONG tls_netx_bind(APTR socket_base)
{
    const AmiNetXDuoContext *ctx = NULL;

    if (socket_base == NULL)
        return -1;

    /*
     * A bsdsocket.library built without AMINETXDUO_TLS has no slot at that
     * offset: the table's (APTR)-1 terminator stops MakeLibrary() before it,
     * so exec never wrote a JMP there and the jump table's negative region is
     * not that long.  Hence the lib_NegSize check.
     */
    if (((struct Library *)socket_base)->lib_NegSize < (UWORD)(-AMI_NXD_CONTEXT_LVO))
        return -1;

    if (tls_obtain_context(socket_base, &ctx) != 0)
        return -1;
    if (ctx == NULL)
        return -1;

    /* nxcontext.c checks these too.  A second check costs four compares and
       makes a mismatched pair of libraries fail here rather than in the middle
       of a handshake. */
    if (ctx->nxc_Magic != AMI_NXD_CONTEXT_MAGIC)
        return -1;
    if (ctx->nxc_Version != AMI_NXD_CONTEXT_VERSION)
        return -1;
    if (ctx->nxc_Size != (ULONG)sizeof(AmiNetXDuoContext))
        return -1;

    /* Refresh this on every TLSOpen().  tls.library can stay resident while
       bsdsocket.library is expunged and reloaded; its context table lives in
       the latter's segment, so keeping the first pointer forever turns the
       next handshake into a call through unloaded memory.  A failed refresh
       leaves the previous context intact for connections already using it. */
    tls_ctx         = ctx;
    c68k_yield_hook = tls_crypto_yield;

    return 0;
}

const AmiNetXDuoContext *tls_netx_ctx(VOID)
{
    return tls_ctx;
}

/* ------------------------------------------- what nx_secure links against, */

UINT _nx_packet_allocate(NX_PACKET_POOL *pool_ptr, NX_PACKET **packet_ptr,
                         ULONG packet_type, ULONG wait_option)
{
    if (tls_ctx == NULL)
        return NX_NOT_ENABLED;

    return tls_ctx->nxc_packet_allocate(pool_ptr, packet_ptr, packet_type,
                                        wait_option);
}

UINT _nx_packet_data_append(NX_PACKET *packet_ptr, VOID *data_start,
                            ULONG data_size, NX_PACKET_POOL *pool_ptr,
                            ULONG wait_option)
{
    if (tls_ctx == NULL)
        return NX_NOT_ENABLED;

    return tls_ctx->nxc_packet_data_append(packet_ptr, data_start, data_size,
                                           pool_ptr, wait_option);
}

UINT _nx_packet_data_extract_offset(NX_PACKET *packet_ptr, ULONG offset,
                                    VOID *buffer_start, ULONG buffer_length,
                                    ULONG *bytes_copied)
{
    if (tls_ctx == NULL)
        return NX_NOT_ENABLED;

    return tls_ctx->nxc_packet_data_extract_offset(packet_ptr, offset,
                                                   buffer_start, buffer_length,
                                                   bytes_copied);
}

UINT _nx_packet_release(NX_PACKET *packet_ptr)
{
    if (tls_ctx == NULL)
        return NX_NOT_ENABLED;

    return tls_ctx->nxc_packet_release(packet_ptr);
}

UINT _nx_tcp_socket_receive(NX_TCP_SOCKET *socket_ptr, NX_PACKET **packet_ptr,
                            ULONG wait_option)
{
    if (tls_ctx == NULL)
        return NX_NOT_ENABLED;

    return tls_ctx->nxc_tcp_socket_receive(socket_ptr, packet_ptr, wait_option);
}

UINT _nx_tcp_socket_send(NX_TCP_SOCKET *socket_ptr, NX_PACKET *packet_ptr,
                         ULONG wait_option)
{
    if (tls_ctx == NULL)
        return NX_NOT_ENABLED;

    return tls_ctx->nxc_tcp_socket_send(socket_ptr, packet_ptr, wait_option);
}

UINT _tx_mutex_create(TX_MUTEX *mutex_ptr, CHAR *name_ptr, UINT inherit)
{
    if (tls_ctx == NULL)
        return TX_NOT_AVAILABLE;

    return tls_ctx->nxc_mutex_create(mutex_ptr, name_ptr, inherit);
}

UINT _tx_mutex_delete(TX_MUTEX *mutex_ptr)
{
    if (tls_ctx == NULL)
        return TX_NOT_AVAILABLE;

    return tls_ctx->nxc_mutex_delete(mutex_ptr);
}

UINT _tx_mutex_get(TX_MUTEX *mutex_ptr, ULONG wait_option)
{
    if (tls_ctx == NULL)
        return TX_NOT_AVAILABLE;

    return tls_ctx->nxc_mutex_get(mutex_ptr, wait_option);
}

UINT _tx_mutex_put(TX_MUTEX *mutex_ptr)
{
    if (tls_ctx == NULL)
        return TX_NOT_AVAILABLE;

    return tls_ctx->nxc_mutex_put(mutex_ptr);
}

TX_THREAD *_tx_thread_identify(VOID)
{
    if (tls_ctx == NULL)
        return TX_NULL;

    return tls_ctx->nxc_thread_identify();
}

UINT _tx_thread_sleep(ULONG timer_ticks)
{
    if (tls_ctx == NULL)
        return TX_NOT_AVAILABLE;

    return tls_ctx->nxc_thread_sleep(timer_ticks);
}

/*
 * The one argument-checking wrapper supplied here rather than borrowed.
 *
 * nx_secure's `nxe_*` objects reference _tx_thread_current_ptr,
 * _tx_thread_system_state and _tx_timer_thread directly, out of
 * NX_THREADS_ONLY_CALLER_CHECKING.  Those are ThreadX *data*, not functions,
 * so they cannot be forwarded through a table.  A definition here is a copy,
 * not an alias, and reads as "no thread is running" forever.
 *
 * That does not normally matter, because this library calls the `_nx_secure_*`
 * entry points directly and does its own argument checking at the LVO, so no
 * nxe_ object is pulled in.  One exception: src/tls/ami_tls_crypto.c offers
 * ami_tls_local_certificate_add() for a server or a client certificate, and
 * that spells the call `nx_secure_tls_local_certificate_add`, which the
 * vendored header maps to the wrapper.  Nothing in tls.library uses it, and it
 * arrives only because it shares a translation unit with the crypto tables.
 * The linker does not know that, so the wrapper defined here keeps the archive
 * member out and the three data symbols with it.
 *
 * Checked after every link: `nm tls.library | grep _tx_thread_current_ptr` is
 * empty.
 */
UINT _nxe_secure_tls_local_certificate_add(NX_SECURE_TLS_SESSION *tls_session,
                                           NX_SECURE_X509_CERT *certificate)
{
    if (tls_session == NX_NULL || certificate == NX_NULL)
        return NX_PTR_ERROR;

    return _nx_secure_tls_local_certificate_add(tls_session, certificate);
}

/* --------------------------------------------- the machine's entropy pool, */

int ami_random_rand(void)
{
    if (tls_ctx == NULL)
        return 0;

    return tls_ctx->nxc_random_rand();
}

/*
 * NX_CRYPTO_RBG, which is where the ECDHE private key comes from.  Not
 * ami_random_rand() in a loop: that one clears bit 31 to keep rand()'s
 * contract, and nx_crypto's own huge-number RBG then puts the gap in every
 * 32-bit word of the key.  Zero is NX_CRYPTO_SUCCESS.  A missing context
 * leaves the buffer alone, and the handshake fails on the key exchange rather
 * than on a silently weak one.
 */
unsigned int ami_crypto_rbg(unsigned int bits, unsigned char *result)
{
    if (tls_ctx == NULL)
        return NX_PTR_ERROR;

    tls_ctx->nxc_random_bytes(result, (ULONG)((bits + 7u) >> 3));
    return 0u;
}

VOID ami_random_add_entropy(const void *data, ULONG length, ULONG credit_bits)
{
    if (tls_ctx == NULL)
        return;

    tls_ctx->nxc_random_add_entropy(data, length, credit_bits);
}

ULONG ami_random_entropy_bits(VOID)
{
    if (tls_ctx == NULL)
        return 0;

    return tls_ctx->nxc_random_entropy_bits();
}

/*
 * bsdsocket.library seeded the pool at its own OpenLibrary(), and this library
 * only runs behind an open bsdsocket.library, so a second run of the 22 ms
 * collection here costs time and credits nothing.  The symbol exists because
 * src/tls/tls_amiga.c calls it.
 */
VOID ami_random_init(VOID)
{
}

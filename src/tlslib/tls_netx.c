/*
 * tls.library borrows bsdsocket.library's NetX Duo rather than linking one:
 * ThreadX and NetX Duo are singletons with file-scope state.  The twelve entry
 * points nx_secure needs, and the entropy pool, come through the private LVO.
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
 * include/aminetxduo/nxcontext.h and implemented in src/bsdsocket/nxcontext.c.
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
 * The port only reschedules at ThreadX API boundaries, and the handshake's
 * public-key arithmetic makes no such call, so crypto68k calls this between
 * iterations.  A no-op without the baton, nesting-safe with it.
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
     * so the jump table's negative region is not that long.
     */
    if (((struct Library *)socket_base)->lib_NegSize < (UWORD)(-AMI_NXD_CONTEXT_LVO))
        return -1;

    if (tls_obtain_context(socket_base, &ctx) != 0)
        return -1;
    if (ctx == NULL)
        return -1;

    /* Checked again here, so a mismatched pair of libraries fails at the bind
       rather than in the middle of a handshake. */
    if (ctx->nxc_Magic != AMI_NXD_CONTEXT_MAGIC)
        return -1;
    if (ctx->nxc_Version != AMI_NXD_CONTEXT_VERSION)
        return -1;
    if (ctx->nxc_Size != (ULONG)sizeof(AmiNetXDuoContext))
        return -1;

    /* Refreshed on every TLSOpen(): tls.library can stay resident while
       bsdsocket.library is expunged and reloaded, and its context table lives
       in the latter's segment.  A failed refresh leaves the previous intact. */
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

/*
 * aminetxduo_tls supplies a relinquish fallback for programs that link the
 * ThreadX core directly.  tls.library does not link that core, and its normal
 * crypto hook is the stronger baton bracket above; provide the symbol only so
 * the shared crypto archive remains linkable.  The fallback is not selected
 * after tls_netx_bind() has installed tls_crypto_yield.
 */
VOID tx_thread_relinquish(VOID)
{
    tls_crypto_yield();
}

UINT _tx_thread_sleep(ULONG timer_ticks)
{
    if (tls_ctx == NULL)
        return TX_NOT_AVAILABLE;

    return tls_ctx->nxc_thread_sleep(timer_ticks);
}

/*
 * nx_secure's `nxe_*` objects reference _tx_thread_current_ptr and two other
 * ThreadX *data* symbols, which cannot be forwarded through a table.  Defining
 * this wrapper keeps that archive member, and those symbols, out of the link.
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
 * NX_CRYPTO_RBG, where the ECDHE private key comes from.  Not
 * ami_random_rand() in a loop: that one clears bit 31 to keep rand()'s
 * contract, and the gap then lands in every 32-bit word of the key.
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
 * bsdsocket.library seeded the pool at its own OpenLibrary() and this library
 * only runs behind an open one, so there is nothing to do.  The symbol exists
 * because src/tls/tls_amiga.c calls it.
 */
VOID ami_random_init(VOID)
{
}

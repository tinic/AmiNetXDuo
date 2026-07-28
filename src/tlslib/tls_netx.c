/*
 * tls.library -- borrowing bsdsocket.library's NetX Duo instead of linking one.
 *
 *   NetX Duo and ThreadX are singletons with file-scope state: the IP thread,
 *   the created-object lists, _tx_thread_current_ptr.  bsdsocket.library
 *   contains the only copy on the machine.  If this library linked netxduo
 *   too it would get a second, private set of those globals -- a stack with no
 *   interfaces, a scheduler with no threads -- and every call would fail in a
 *   way that looked like a NetX Duo bug.
 *
 *   nx_secure and nx_crypto have no such state that is not per-session, so
 *   linking them here is correct and keeps 227 KB out of the resident library
 *   of every machine that never makes a TLS connection.
 *
 *   nx_secure calls twelve NetX Duo/ThreadX entry points and nothing else --
 *   measured with `nm` over the archives, not assumed.  This file defines those
 *   twelve names, so the linker resolves nx_secure's references here, and each
 *   definition is a one-line call through the table bsdsocket.library supplies
 *   over its private LVO.  No vendored source is edited and no NetX Duo object
 *   is duplicated.
 *
 *   The same mechanism carries the entropy pool: NX_RAND is ami_random_rand()
 *   (port/netxduo-amiga/inc/nx_port.h), and defining the ami_random_* names
 *   here rather than linking src/common/ami_random.c means a TLS handshake
 *   draws from the pool bsdsocket.library already seeded -- one pool per
 *   machine, not two.
 *
 *   Every forwarder checks for a context.  Reaching one without a context is a
 *   bug in this library (TLSOpenA binds before it does anything else), but the
 *   failure has to be a status code and not a jump through NULL on a machine
 *   with no memory protection.
 *
 * SPDX-License-Identifier: MIT
 */

#include "tls_internal.h"

#include <proto/exec.h>

static const AmiNetXDuoContext *tls_ctx;

/* --------------------------------------------------------- acquisition --- */

/*
 * The private LVO, called by hand.  The register assignment is stated in
 * include/aminetxduo/nxcontext.h and implemented in
 * src/bsdsocket/nxcontext.c; this is the third and last place it appears.
 */
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

LONG tls_netx_bind(APTR socket_base)
{
    const AmiNetXDuoContext *ctx = NULL;

    if (tls_ctx != NULL)
        return 0;

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

    /* nxcontext.c checks these too; re-checking costs four compares and makes
       a mismatched pair of libraries fail here rather than in the middle of a
       handshake. */
    if (ctx->nxc_Magic != AMI_NXD_CONTEXT_MAGIC)
        return -1;
    if (ctx->nxc_Version != AMI_NXD_CONTEXT_VERSION)
        return -1;
    if (ctx->nxc_Size != (ULONG)sizeof(AmiNetXDuoContext))
        return -1;

    tls_ctx = ctx;

    return 0;
}

const AmiNetXDuoContext *tls_netx_ctx(VOID)
{
    return tls_ctx;
}

/* ------------------------------------------- what nx_secure links against -- */

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
 * so they cannot be forwarded through a table -- a definition here would be a
 * copy, not an alias, and would read as "no thread is running" forever.
 *
 * That is normally moot, because this library calls the `_nx_secure_*` entry
 * points directly and does its own argument checking at the LVO, so no nxe_
 * object is pulled in.  One exception: src/tls/ami_tls_crypto.c offers
 * ami_tls_local_certificate_add() for a server or a client certificate, and
 * that spells the call `nx_secure_tls_local_certificate_add`, which the
 * vendored header maps to the wrapper.  Nothing in tls.library uses it -- it
 * arrives only because it shares a translation unit with the crypto tables --
 * but the linker does not know that, so defining the wrapper here keeps the
 * archive member out and the three data symbols with it.
 *
 * Verified after every link: `nm tls.library | grep _tx_thread_current_ptr` is
 * empty.
 */
UINT _nxe_secure_tls_local_certificate_add(NX_SECURE_TLS_SESSION *tls_session,
                                           NX_SECURE_X509_CERT *certificate)
{
    if (tls_session == NX_NULL || certificate == NX_NULL)
        return NX_PTR_ERROR;

    return _nx_secure_tls_local_certificate_add(tls_session, certificate);
}

/* --------------------------------------------- the machine's entropy pool -- */

int ami_random_rand(void)
{
    if (tls_ctx == NULL)
        return 0;

    return tls_ctx->nxc_random_rand();
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
 * only runs behind an open bsdsocket.library, so re-running the 22 ms
 * collection here would cost time and credit nothing.  The symbol exists
 * because src/tls/tls_amiga.c calls it.
 */
VOID ami_random_init(VOID)
{
}

/*
 * tls.library, what nx_secure links against when there is no NetX Duo.
 *
 * nx_secure is written against NetX Duo and ThreadX.  tls.library links
 * neither: the stack underneath it is whatever bsdsocket.library the machine
 * has -- ours, Roadshow, AmiTCP or Miami -- and it is reached through the
 * published vectors in tls_sock.c and nothing else.  So the handful of NetX
 * Duo and ThreadX entry points nx_secure calls are defined here, over AmigaOS:
 *
 *   nx_tcp_socket_send/receive   send() and recv() on the caller's descriptor
 *   nx_packet_*                  tls_packet.c, this library's own pool
 *   tx_mutex_*                   Exec signal semaphores
 *   tx_thread_identify           TX_NULL: no thread here is a ThreadX thread
 *   the entropy pool             src/common/ami_random.c, linked in
 *
 * WHY THERE IS NO FAST PATH FOR OUR OWN STACK.  There used to be one, and it
 * was the only path: a private LVO at -0x360 handed tls.library our
 * bsdsocket.library's NX_TCP_SOCKET and packet pool, and every other stack got
 * TLS_ERR_NOSTACK.  What it bought was one copy avoided per record.  What it
 * cost was a second implementation of every function in this file, the two
 * libraries having to ship from one build, and the ThreadX baton being held by
 * the caller's task across a handshake -- which is why crypto68k needed a
 * yield hook at all.  Going through send() and recv() is one memcpy per record
 * against arithmetic measured in seconds, and it deletes all three costs.
 *
 * SPDX-License-Identifier: MIT
 */

#include "tls_internal.h"

#include <exec/semaphores.h>
#include <proto/exec.h>

/* ---------------------------------------------------------- the socket --- */

/*
 * NX_TCP_SOCKET is nx_secure's handle for the transport and it reads three of
 * its fields directly, so the real structure is embedded rather than faked:
 *
 *   nx_tcp_socket_client_type      client or server, chosen at session_start
 *   nx_tcp_socket_ip_ptr           NX_NULL here, and every read of it in
 *                                  nx_secure is guarded on that
 *   nx_tcp_socket_connect_ip       only nxd_ip_version is read, to size the
 *                                  header reserve a record packet asks for
 *
 * The descriptor rides behind it.  tls_transport_open() is what makes the two
 * one object, so the cast in the two entry points below is sound.
 */

VOID tls_transport_open(TLSTransport *transport, APTR socket_base, LONG fd,
                        NX_PACKET_POOL *pool, BOOL server, BOOL ipv6)
{
    tls_bzero(transport, sizeof(*transport));

    transport->tt_SocketBase = socket_base;
    transport->tt_Fd         = fd;
    transport->tt_Pool       = pool;

    transport->tt_Socket.nx_tcp_socket_client_type = server ? NX_FALSE : NX_TRUE;
    transport->tt_Socket.nx_tcp_socket_ip_ptr      = NX_NULL;
    transport->tt_Socket.nx_tcp_socket_connect_ip.nxd_ip_version =
        ipv6 ? NX_IP_VERSION_V6 : NX_IP_VERSION_V4;
}

NX_TCP_SOCKET *tls_transport_socket(TLSTransport *transport)
{
    return &transport->tt_Socket;
}

/*
 * NetX Duo wait options are timer ticks at NX_IP_PERIODIC_RATE.  WaitSelect()
 * wants seconds and microseconds, and NX_WAIT_FOREVER wants no timeval at all.
 * Returns NULL for "block", `store` otherwise.
 */
static const TLSSockTimeval *tls_transport_timeout(ULONG wait_option,
                                                   TLSSockTimeval *store)
{
    ULONG ticks = wait_option;

    if (ticks == NX_WAIT_FOREVER)
        return NULL;

    store->tv_secs  = (LONG)(ticks / NX_IP_PERIODIC_RATE);
    store->tv_micro = (LONG)(((ticks % NX_IP_PERIODIC_RATE) * 1000000UL) /
                             NX_IP_PERIODIC_RATE);

    return store;
}

UINT _nx_tcp_socket_receive(NX_TCP_SOCKET *socket_ptr, NX_PACKET **packet_ptr,
                            ULONG wait_option)
{
    TLSTransport         *transport = (TLSTransport *)(VOID *)socket_ptr;
    TLSSockTimeval        timeout;
    const TLSSockTimeval *timeout_ptr;
    NX_PACKET            *packet = NX_NULL;
    UINT                  status;
    LONG                  ready;
    LONG                  got;

    if (socket_ptr == NX_NULL || packet_ptr == NX_NULL)
        return NX_PTR_ERROR;
    if (transport->tt_Broken)
        return NX_NOT_CONNECTED;

    timeout_ptr = tls_transport_timeout(wait_option, &timeout);

    /*
     * WaitSelect() first and recv() second, never a bare blocking recv():
     * TLSA_Timeout has to be a ceiling on a peer that goes quiet, and no
     * bsdsocket.library is required to honour SO_RCVTIMEO.
     */
    ready = tls_sock_wait(transport->tt_SocketBase, transport->tt_Fd, FALSE,
                          timeout_ptr);
    if (ready == 0)
        return NX_NO_PACKET;
    if (ready < 0)
    {
        return (tls_sock_errno(transport->tt_SocketBase) == TLS_SOCK_EINTR)
               ? NX_WAIT_ABORTED : NX_NOT_CONNECTED;
    }

    status = _nx_packet_allocate(transport->tt_Pool, &packet, NX_RECEIVE_PACKET,
                                 wait_option);
    if (status != NX_SUCCESS)
        return status;

    got = tls_sock_recv(transport->tt_SocketBase, transport->tt_Fd,
                        packet->nx_packet_prepend_ptr,
                        (LONG)(packet->nx_packet_data_end -
                               packet->nx_packet_prepend_ptr));

    if (got <= 0)
    {
        LONG err = (got < 0) ? tls_sock_errno(transport->tt_SocketBase) : 0;

        (VOID)_nx_packet_release(packet);

        if (got == 0)
        {
            /* A clean FIN.  nx_secure reads NX_NOT_CONNECTED as end of
               stream, and tls_conn.c turns that into TLS_ERR_CLOSED. */
            transport->tt_Broken = NX_TRUE;
            return NX_NOT_CONNECTED;
        }
        if (err == TLS_SOCK_EINTR)
            return NX_WAIT_ABORTED;
        if (err == TLS_SOCK_EWOULDBLOCK)
            return NX_NO_PACKET;

        transport->tt_Broken = NX_TRUE;
        return NX_NOT_CONNECTED;
    }

    packet->nx_packet_append_ptr = packet->nx_packet_prepend_ptr + got;
    packet->nx_packet_length     = (ULONG)got;

    /*
     * Arrival timing is the one entropy source this library can reach on a
     * stack that is not ours.  The private LVO used to borrow
     * bsdsocket.library's pool, which had the SANA-II receive path feeding it;
     * a record arriving off the wire is the same class of measurement, and it
     * is what keeps the pool above AMI_RANDOM_INTERNAL_MAX_BITS here.
     */
    ami_random_arrival();

    *packet_ptr = packet;

    return NX_SUCCESS;
}

UINT _nx_tcp_socket_send(NX_TCP_SOCKET *socket_ptr, NX_PACKET *packet_ptr,
                         ULONG wait_option)
{
    TLSTransport         *transport = (TLSTransport *)(VOID *)socket_ptr;
    TLSSockTimeval        timeout;
    const TLSSockTimeval *timeout_ptr;
    NX_PACKET            *current;

    if (socket_ptr == NX_NULL || packet_ptr == NX_NULL)
        return NX_PTR_ERROR;
    if (transport->tt_Broken)
        return NX_NOT_CONNECTED;

    timeout_ptr = tls_transport_timeout(wait_option, &timeout);

    for (current = packet_ptr; current != NX_NULL;
         current = current->nx_packet_next)
    {
        const UCHAR *data = current->nx_packet_prepend_ptr;
        LONG         left = (LONG)(current->nx_packet_append_ptr -
                                   current->nx_packet_prepend_ptr);

        while (left > 0)
        {
            LONG ready;
            LONG sent;

            ready = tls_sock_wait(transport->tt_SocketBase, transport->tt_Fd,
                                  TRUE, timeout_ptr);
            if (ready <= 0)
            {
                /*
                 * A record half on the wire cannot be retried: the peer's
                 * sequence number has moved.  Say so once and refuse every
                 * later call rather than corrupt the stream.
                 */
                if (data != packet_ptr->nx_packet_prepend_ptr)
                    transport->tt_Broken = NX_TRUE;
                return (ready == 0) ? NX_NO_PACKET : NX_NOT_CONNECTED;
            }

            sent = tls_sock_send(transport->tt_SocketBase, transport->tt_Fd,
                                 data, left);
            if (sent > 0)
            {
                data += sent;
                left -= sent;
                continue;
            }

            if (sent < 0)
            {
                LONG err = tls_sock_errno(transport->tt_SocketBase);

                if (err == TLS_SOCK_EINTR || err == TLS_SOCK_EWOULDBLOCK)
                    continue;
            }

            transport->tt_Broken = NX_TRUE;
            return NX_NOT_CONNECTED;
        }
    }

    /* NetX Duo's contract: the stack owns the packet once the send has
       succeeded, and the caller releases it only on failure. */
    (VOID)_nx_packet_release(packet_ptr);

    return NX_SUCCESS;
}

/* ------------------------------------------------------------- mutexes --- */

/*
 * nx_secure creates exactly one, _nx_secure_tls_protection, and takes it
 * around every change to the process-wide session list.  Four slots because a
 * table with a spare is cheaper to reason about than a table that is exactly
 * full, and because the DTLS half would add a second if it were ever built.
 *
 * A table and not an overlay on TX_MUTEX's own storage: nothing here should
 * depend on ThreadX's structure layout when ThreadX is not linked.
 *
 * ObtainSemaphore() is recursive for the owning task and so is a ThreadX mutex
 * without priority inheritance, so the nesting nx_secure relies on holds.
 */
#define TLS_MUTEX_SLOTS 4

typedef struct TLSMutexSlot
{
    TX_MUTEX               *tm_Mutex;
    struct SignalSemaphore  tm_Semaphore;
} TLSMutexSlot;

static TLSMutexSlot tls_mutex_slots[TLS_MUTEX_SLOTS];

static TLSMutexSlot *tls_mutex_find(TX_MUTEX *mutex_ptr)
{
    UINT i;

    for (i = 0; i < TLS_MUTEX_SLOTS; i++)
    {
        if (tls_mutex_slots[i].tm_Mutex == mutex_ptr)
            return &tls_mutex_slots[i];
    }

    return NULL;
}

UINT _tx_mutex_create(TX_MUTEX *mutex_ptr, CHAR *name_ptr, UINT inherit)
{
    TLSMutexSlot *slot = NULL;
    UINT          i;

    (VOID)name_ptr;
    (VOID)inherit;

    if (mutex_ptr == TX_NULL)
        return TX_MUTEX_ERROR;

    Forbid();

    if (tls_mutex_find(mutex_ptr) == NULL)
    {
        for (i = 0; i < TLS_MUTEX_SLOTS; i++)
        {
            if (tls_mutex_slots[i].tm_Mutex == TX_NULL)
            {
                slot = &tls_mutex_slots[i];
                InitSemaphore(&slot->tm_Semaphore);
                slot->tm_Mutex = mutex_ptr;
                break;
            }
        }
    }
    else
    {
        slot = tls_mutex_find(mutex_ptr);
    }

    Permit();

    return (slot != NULL) ? TX_SUCCESS : TX_NO_MEMORY;
}

UINT _tx_mutex_delete(TX_MUTEX *mutex_ptr)
{
    TLSMutexSlot *slot;

    Forbid();
    slot = tls_mutex_find(mutex_ptr);
    if (slot != NULL)
        slot->tm_Mutex = TX_NULL;
    Permit();

    return (slot != NULL) ? TX_SUCCESS : TX_MUTEX_ERROR;
}

UINT _tx_mutex_get(TX_MUTEX *mutex_ptr, ULONG wait_option)
{
    TLSMutexSlot *slot = tls_mutex_find(mutex_ptr);

    if (slot == NULL)
        return TX_MUTEX_ERROR;

    if (wait_option == TX_NO_WAIT)
        return AttemptSemaphore(&slot->tm_Semaphore) ? TX_SUCCESS : TX_NOT_AVAILABLE;

    ObtainSemaphore(&slot->tm_Semaphore);

    return TX_SUCCESS;
}

UINT _tx_mutex_put(TX_MUTEX *mutex_ptr)
{
    TLSMutexSlot *slot = tls_mutex_find(mutex_ptr);

    if (slot == NULL)
        return TX_MUTEX_ERROR;

    ReleaseSemaphore(&slot->tm_Semaphore);

    return TX_SUCCESS;
}

/* ------------------------------------------------------------- threads --- */

/*
 * The one caller in the TLS half is nx_secure_tls_send_record.c, comparing the
 * answer against the IP thread of the socket's NX_IP.  There is no NX_IP here,
 * that comparison is guarded on a NX_NULL nx_tcp_socket_ip_ptr, and no task in
 * this library is a ThreadX thread.  TX_NULL is the truthful answer.
 */
TX_THREAD *_tx_thread_identify(VOID)
{
    return TX_NULL;
}

/*
 * Referenced only by the DTLS half, which is not built.  Defined so that
 * turning AMINETXDUO_DTLS on is a build that links rather than one that does
 * not; a millisecond is the smallest thing Delay() can be asked for.
 */
UINT _tx_thread_sleep(ULONG timer_ticks)
{
    tls_delay_ticks(timer_ticks);

    return TX_SUCCESS;
}

/*
 * crypto68k calls this between iterations of the public-key inner loops.  It
 * mattered when the caller held the ThreadX baton across a handshake and the
 * IP thread could not answer the network until the arithmetic finished.  This
 * library holds no baton: the arithmetic runs on the caller's own task, Exec
 * preempts it like any other, and the stack is a separate task throughout.
 */
/*
 * GCC 16.2's m68k analyser invents an uninitialised return value for this
 * empty VOID wrapper.  There is no body to analyse, so confine the workaround
 * to the diagnostic's exact source rather than carrying a file-wide false
 * positive.  The same suppression stood around this function when it had a
 * body (`ded4fb54`) and went with the body that replaced it.
 */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wanalyzer-use-of-uninitialized-value"
#endif
VOID tx_thread_relinquish(VOID)
{
}
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

/*
 * nx_secure's `nxe_*` objects reference _tx_thread_current_ptr and two other
 * ThreadX *data* symbols, which no definition here can supply.  Defining this
 * wrapper keeps that archive member, and those symbols, out of the link.
 */
UINT _nxe_secure_tls_local_certificate_add(NX_SECURE_TLS_SESSION *tls_session,
                                           NX_SECURE_X509_CERT *certificate)
{
    if (tls_session == NX_NULL || certificate == NX_NULL)
        return NX_PTR_ERROR;

    return _nx_secure_tls_local_certificate_add(tls_session, certificate);
}

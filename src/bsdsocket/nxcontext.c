/*
 * bsdsocket.library, the private vector that hands tls.library the stack.
 *
 * Compiled only when AMINETXDUO_TLS_CONTEXT is defined, which happens only in
 * an AMINETXDUO_TLS build.  A default build has neither this object nor the
 * vector-table slot that points at it, so the default bsdsocket.library stays
 * byte-identical to a build of the tree without TLS.
 *
 * See include/aminetxduo/nxcontext.h for why a separate library has to borrow
 * the stack rather than link one.
 *
 * SPDX-License-Identifier: MIT
 */

#include "bsdsocket_vectors.h"

#include "aminetxduo/nxcontext.h"
#include "aminetxduo/random.h"

#include "nx_packet.h"
#include "nx_tcp.h"

/* The baton hooks are src/netstack internals, not public API, so the
   declarations come from there rather than include/aminetxduo/netstack.h. */
VOID ami_netstack_baton_release(VOID);
VOID ami_netstack_baton_acquire(VOID);

/*
 * The table is const and static; every entry is a function this library
 * already contains.  Nothing in it is per opener, so one copy serves every
 * caller, the only per-caller argument is the SocketBase passed to
 * nxc_TcpSocket().
 */

static NX_TCP_SOCKET *bsd_nxc_tcp_socket(APTR socket_base, LONG fd)
{
    struct AmiSocketBase *base = (struct AmiSocketBase *)socket_base;
    AmiSocket            *sock;

    if (base == NULL)
        return NX_NULL;

    sock = bsd_lookup(base, fd);
    if (sock == NULL)
        return NX_NULL;

    /* TLS runs only over a connected byte stream.  A UDP socket, a listening
       socket or one whose connect() has not completed would instead fail late
       and obscurely: nx_secure would sit in nx_tcp_socket_receive() until the
       caller's timeout. */
    if ((sock->as_Flags & ASF_TCP) == 0)
        return NX_NULL;
    if ((sock->as_Flags & ASF_CONNECTED) == 0)
        return NX_NULL;
    if ((sock->as_Flags & (ASF_DELETED | ASF_LISTENING)) != 0)
        return NX_NULL;

    return &sock->as_Nx.tcp;
}

static const AmiNetXDuoContext bsd_nxd_context =
{
    AMI_NXD_CONTEXT_MAGIC,
    AMI_NXD_CONTEXT_VERSION,
    (ULONG)sizeof(AmiNetXDuoContext),

    netstack_ip,
    netstack_pool,
    bsd_nxc_tcp_socket,
    ami_netstack_enter,
    ami_netstack_leave,
    ami_netstack_baton_release,
    ami_netstack_baton_acquire,

    _nx_packet_allocate,
    _nx_packet_data_append,
    _nx_packet_data_extract_offset,
    _nx_packet_release,
    _nx_tcp_socket_receive,
    _nx_tcp_socket_send,

    _tx_mutex_create,
    _tx_mutex_delete,
    _tx_mutex_get,
    _tx_mutex_put,
    _tx_thread_identify,
    _tx_thread_sleep,

    ami_random_rand,
    ami_random_bytes,
    ami_random_add_entropy,
    ami_random_entropy_bits
};

LONG ami_nxd_context_obtain(ULONG magic, ULONG version,
                            const AmiNetXDuoContext **ctx, APTR socket_base)
{
    (VOID)socket_base;

    /* Write nothing unless every one of these holds, see the header. */
    if (magic != AMI_NXD_CONTEXT_MAGIC)
        return -1;
    if (version != AMI_NXD_CONTEXT_VERSION)
        return -1;
    if (ctx == NULL)
        return -1;

    /* The stack is up because the caller holds an open bsdsocket.library;
       checked anyway, since a context with a NULL NX_IP behind it would fail
       in nx_secure with a caller error instead of here. */
    if (netstack_ip() == NX_NULL)
        return -1;

    *ctx = &bsd_nxd_context;

    return 0;
}

/* The LVO wrapper.  Register assignment must match
   include/aminetxduo/nxcontext.h and the inline in src/tlslib/tls_netx.c. */
LONG bsd_ObtainNetXDuoContext(
    register ULONG                   magic       __asm("d0"),
    register ULONG                   version     __asm("d1"),
    register const AmiNetXDuoContext **ctx       __asm("a0"),
    register struct AmiSocketBase    *SocketBase __asm("a6"))
{
    return ami_nxd_context_obtain(magic, version, ctx, (APTR)SocketBase);
}

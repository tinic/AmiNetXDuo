/*
 * bsdsocket.library -- the private vector that hands tls.library the stack.
 *
 * Compiled only when AMINETXDUO_TLS_CONTEXT is defined, which happens only in
 * an AMINETXDUO_TLS build.  A default build has neither this object nor the
 * vector-table slot that points at it, which is what keeps the default
 * bsdsocket.library byte-identical to a build of the tree without TLS.
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

/*
 * The table is `const` and static, and every entry is a function this library
 * already contains.  Nothing here is per opener, so one copy serves every
 * caller; the only per-caller argument is the SocketBase passed to
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

    /*
     * TLS runs over a connected byte stream and nothing else.  A UDP socket, a
     * listening socket or a socket whose connect() has not completed would all
     * fail later and much less legibly -- nx_secure would sit in
     * nx_tcp_socket_receive() until the caller's timeout.
     */
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
    ami_random_add_entropy,
    ami_random_entropy_bits
};

LONG ami_nxd_context_obtain(ULONG magic, ULONG version,
                            const AmiNetXDuoContext **ctx, APTR socket_base)
{
    (VOID)socket_base;

    /* Write nothing unless every one of these holds -- see the header. */
    if (magic != AMI_NXD_CONTEXT_MAGIC)
        return -1;
    if (version != AMI_NXD_CONTEXT_VERSION)
        return -1;
    if (ctx == NULL)
        return -1;

    /* The stack is up because the caller holds an open bsdsocket.library, but
       say so anyway: a caller that gets a context with a NULL NX_IP behind it
       would fail in nx_secure with a caller error instead of here. */
    if (netstack_ip() == NX_NULL)
        return -1;

    *ctx = &bsd_nxd_context;

    return 0;
}

/* The LVO wrapper.  Register assignment is fixed by
   include/aminetxduo/nxcontext.h and by the inline in src/tlslib/tls_netx.c;
   they must agree, and this is the one place it is spelled out in C. */
LONG bsd_ObtainNetXDuoContext(
    register ULONG                   magic       __asm("d0"),
    register ULONG                   version     __asm("d1"),
    register const AmiNetXDuoContext **ctx       __asm("a0"),
    register struct AmiSocketBase    *SocketBase __asm("a6"))
{
    return ami_nxd_context_obtain(magic, version, ctx, (APTR)SocketBase);
}

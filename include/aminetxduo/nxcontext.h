/*
 * AmiNetXDuo, the private handle bsdsocket.library gives tls.library.  The
 * NetX Duo stack is a singleton in bsdsocket's segment: tls.library must not
 * link NetX Duo, and the two must be built from one tree and shipped together.
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_NXCONTEXT_H
#define AMINETXDUO_NXCONTEXT_H

#include <exec/types.h>

#include "tx_api.h"
#include "nx_api.h"

#include "aminetxduo/netstack.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The private LVO, the first slot past everything the published bsdsocket ABI
   names.  Emitted only when AMINETXDUO_TLS_CONTEXT is defined. */
#define AMI_NXD_CONTEXT_LVO         (-0x360)

#define AMI_NXD_CONTEXT_MAGIC       0x414E5844UL    /* 'ANXD' */

/*
 * The low half carries AMINETXDUO_IPV6, which changes the layout of every
 * struct crossing this interface.  Bump the high half whenever this header
 * changes.
 */
#ifdef AMINETXDUO_IPV6
#define AMI_NXD_CONTEXT_VERSION     0x00020001UL
#else
#define AMI_NXD_CONTEXT_VERSION     0x00020000UL
#endif

/* Everything tls.library needs from the stack, in one table. */
typedef struct AmiNetXDuoContext
{
    ULONG               nxc_Magic;          /* AMI_NXD_CONTEXT_MAGIC        */
    ULONG               nxc_Version;        /* AMI_NXD_CONTEXT_VERSION      */
    ULONG               nxc_Size;           /* sizeof(AmiNetXDuoContext)    */

    /* ---- the singleton ------------------------------------------------- */

    NX_IP              *(*nxc_IP)(VOID);
    NX_PACKET_POOL     *(*nxc_Pool)(VOID);

    /*
     * `socket_base` is the caller's SocketBase; the descriptor table is per
     * opener, so the fd is meaningless without it.  NX_NULL unless the fd is a
     * connected TCP socket.  Unenforced: no recv()/send() on it until close.
     */
    NX_TCP_SOCKET      *(*nxc_TcpSocket)(APTR socket_base, LONG fd);

    /*
     * The ThreadX bracket, ami_netstack_enter()/leave() semantics: nesting is
     * safe and an already-adopted task is left alone.  Every LVO that reaches
     * NetX Duo must be bracketed.
     */
    LONG                (*nxc_Enter)(AmiNetCaller *caller);
    VOID                (*nxc_Leave)(AmiNetCaller *caller);

    /*
     * A ThreadX thread must bracket any blocking Exec call with these, or it
     * stalls the IP thread and both SANA-II readers.  Nesting is handled
     * inside; a caller that does not hold the baton gets two no-ops.
     */
    VOID                (*nxc_BatonRelease)(VOID);
    VOID                (*nxc_BatonAcquire)(VOID);

    /* ---- what nx_secure links against ---------------------------------- */

    UINT                (*nxc_packet_allocate)(NX_PACKET_POOL *pool_ptr,
                                               NX_PACKET **packet_ptr,
                                               ULONG packet_type,
                                               ULONG wait_option);
    UINT                (*nxc_packet_data_append)(NX_PACKET *packet_ptr,
                                                  VOID *data_start,
                                                  ULONG data_size,
                                                  NX_PACKET_POOL *pool_ptr,
                                                  ULONG wait_option);
    UINT                (*nxc_packet_data_extract_offset)(NX_PACKET *packet_ptr,
                                                          ULONG offset,
                                                          VOID *buffer_start,
                                                          ULONG buffer_length,
                                                          ULONG *bytes_copied);
    UINT                (*nxc_packet_release)(NX_PACKET *packet_ptr);

    UINT                (*nxc_tcp_socket_receive)(NX_TCP_SOCKET *socket_ptr,
                                                  NX_PACKET **packet_ptr,
                                                  ULONG wait_option);
    UINT                (*nxc_tcp_socket_send)(NX_TCP_SOCKET *socket_ptr,
                                               NX_PACKET *packet_ptr,
                                               ULONG wait_option);

    UINT                (*nxc_mutex_create)(TX_MUTEX *mutex_ptr,
                                            CHAR *name_ptr, UINT inherit);
    UINT                (*nxc_mutex_delete)(TX_MUTEX *mutex_ptr);
    UINT                (*nxc_mutex_get)(TX_MUTEX *mutex_ptr,
                                         ULONG wait_option);
    UINT                (*nxc_mutex_put)(TX_MUTEX *mutex_ptr);

    TX_THREAD          *(*nxc_thread_identify)(VOID);
    UINT                (*nxc_thread_sleep)(ULONG timer_ticks);

    /* ---- the machine's entropy pool ------------------------------------ */

    int                 (*nxc_random_rand)(VOID);

    /*
     * Key material must come from here, never nxc_random_rand() in a loop:
     * that one clears bit 31 to stay inside rand()'s range.
     */
    VOID                (*nxc_random_bytes)(APTR buffer, ULONG length);

    VOID                (*nxc_random_add_entropy)(const VOID *data, ULONG len,
                                                  ULONG credit_bits);
    ULONG               (*nxc_random_entropy_bits)(VOID);
} AmiNetXDuoContext;

/* Returns 0 and writes *ctx on success; -1 and writes nothing otherwise, which
   is what a wrong magic or version gets. */
LONG ami_nxd_context_obtain(ULONG magic, ULONG version,
                            const AmiNetXDuoContext **ctx, APTR socket_base);

#ifdef __cplusplus
}
#endif

#endif /* AMINETXDUO_NXCONTEXT_H */

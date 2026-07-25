/*
 * AmiNetXDuo -- the private handle bsdsocket.library gives tls.library.
 *
 * WHY THIS EXISTS AT ALL
 *
 *   The whole NetX Duo/ThreadX stack is a singleton that lives inside
 *   bsdsocket.library's segment: one NX_IP, one packet pool, one ThreadX
 *   kernel, one set of NetX Duo globals.  A second library that linked its own
 *   copy of netxduo would get a second set of those globals and drive a stack
 *   that owns no interfaces.  So tls.library must not link NetX Duo -- it must
 *   CALL bsdsocket.library's copy.
 *
 *   The coupling surface is small, and that is a measurement rather than a
 *   hope.  `nm` on libnx_secure.a + libnx_crypto.a + libcrypto68k.a +
 *   libaminetxduo_tls.a, minus everything they define themselves, leaves
 *   exactly twelve NetX Duo/ThreadX entry points (plus memcpy, ami_random and
 *   __udivdi3, which are ordinary link-time dependencies).  Those twelve are
 *   the middle block below; src/tlslib/tls_netx.c defines the vendored symbol
 *   names as one-line forwarders through this table, so no nx_secure source is
 *   touched and no NetX Duo object is duplicated.
 *
 *   The three ThreadX *data* symbols nx_secure also wants
 *   (_tx_thread_current_ptr, _tx_thread_system_state, _tx_timer_thread) are
 *   referenced only by the `nxe_*` argument-checking wrappers.  tls.library
 *   calls the `_nx_secure_*` entry points directly and does its own argument
 *   checking at the LVO, so those archive members are never pulled in and the
 *   data symbols never become undefined.  Verified with `nm` after the link.
 *
 * HOW IT IS OBTAINED
 *
 *   One private LVO on bsdsocket.library at AMI_NXD_CONTEXT_LVO, present only
 *   in an AMINETXDUO_TLS build.  The public 121-vector Roadshow/AmiTCP ABI is
 *   untouched, and a default build of bsdsocket.library does not have this
 *   vector at all (which is what keeps that build byte-identical).
 *
 *   The magic and version arguments are not ceremony.  The slot sits past the
 *   last vector any published bsdsocket ABI defines, so the only way to reach
 *   it is deliberately -- but if some future Roadshow ever allocates the same
 *   offset for something else, a caller of THAT function lands here with
 *   whatever it had in d0/d1, and this call must do nothing rather than
 *   something.  Wrong magic or wrong version returns failure and writes
 *   nothing.
 *
 * VERSION LOCKING
 *
 *   tls.library and bsdsocket.library share struct layouts (NX_TCP_SOCKET,
 *   NX_PACKET, NX_PACKET_POOL) and must be built from the same tree with the
 *   same options -- AMINETXDUO_IPV6 above all, since it changes the layout of
 *   every one of them.  nxc_Version is bumped whenever anything here or in
 *   those layouts changes, and tls.library refuses to run against a version it
 *   does not recognise.  Ship the two files together.
 *
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

/*
 * The private LVO.  -0x360 is the first slot past the six reserved ones that
 * clib/bsdsocket_protos.h documents after getnameinfo(), i.e. past everything
 * the published ABI names.  src/bsdsocket/bsdsocket_vectors.c only emits it
 * when AMINETXDUO_TLS_CONTEXT is defined.
 */
#define AMI_NXD_CONTEXT_LVO         (-0x360)

#define AMI_NXD_CONTEXT_MAGIC       0x414E5844UL    /* 'ANXD' */
#define AMI_NXD_CONTEXT_VERSION     1UL

/*
 * Everything tls.library needs from the stack, in one table.
 *
 * The first block is the singleton and the two brackets an application-level
 * caller has to take; the second is the NetX Duo/ThreadX surface nx_secure
 * links against; the third is the machine's one entropy pool, shared rather
 * than duplicated so that a TLS client draws from the pool bsdsocket.library
 * already seeded at OpenLibrary() time (docs/RESEARCH.md 9, "the NX_RAND
 * problem") instead of starting a second, colder one.
 */
typedef struct AmiNetXDuoContext
{
    ULONG               nxc_Magic;          /* AMI_NXD_CONTEXT_MAGIC        */
    ULONG               nxc_Version;        /* AMI_NXD_CONTEXT_VERSION      */
    ULONG               nxc_Size;           /* sizeof(AmiNetXDuoContext)    */

    /* ---- the singleton ------------------------------------------------- */

    NX_IP              *(*nxc_IP)(VOID);
    NX_PACKET_POOL     *(*nxc_Pool)(VOID);

    /*
     * The transport behind a descriptor.  `socket_base` is the CALLER's
     * SocketBase, because the descriptor table is per opener (docs/RESEARCH.md
     * 3.1: SocketBase is never shared), so the fd is meaningless without it.
     *
     * Returns NX_NULL unless the descriptor is a connected TCP socket.  Once a
     * descriptor has been handed over this way, the application must not
     * recv()/send() on it: those would take bytes out of the middle of a TLS
     * record.  Nothing enforces that -- enforcing it would mean a flag test on
     * the hot path of every recv() in the system -- so it is a contract, and
     * TLSClose() is what ends it.
     */
    NX_TCP_SOCKET      *(*nxc_TcpSocket)(APTR socket_base, LONG fd);

    /*
     * The ThreadX bracket.  Identical semantics to ami_netstack_enter()/
     * ami_netstack_leave(): nesting is safe, and an already-adopted task is
     * recognised and left alone.  tls.library brackets every LVO that reaches
     * NetX Duo, so an application never has to know this exists.
     */
    LONG                (*nxc_Enter)(AmiNetCaller *caller);
    VOID                (*nxc_Leave)(AmiNetCaller *caller);

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
    VOID                (*nxc_random_add_entropy)(const VOID *data, ULONG len,
                                                  ULONG credit_bits);
    ULONG               (*nxc_random_entropy_bits)(VOID);
} AmiNetXDuoContext;

/*
 * The vector itself, for anyone linking against src/bsdsocket directly.  A
 * shared-library caller reaches it through the LVO -- see
 * src/tlslib/tls_netx.c, which is the only caller in the tree.
 *
 * Returns 0 and writes *ctx on success; -1 and writes nothing otherwise.
 */
LONG ami_nxd_context_obtain(ULONG magic, ULONG version,
                            const AmiNetXDuoContext **ctx, APTR socket_base);

#ifdef __cplusplus
}
#endif

#endif /* AMINETXDUO_NXCONTEXT_H */

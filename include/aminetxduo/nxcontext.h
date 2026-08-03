/*
 * AmiNetXDuo -- the private handle bsdsocket.library gives tls.library.
 *
 *   The whole NetX Duo/ThreadX stack is a singleton that lives inside
 *   bsdsocket.library's segment: one NX_IP, one packet pool, one ThreadX
 *   kernel, one set of NetX Duo globals.  A second library that linked its own
 *   copy of netxduo would get a second set of those globals and drive a stack
 *   with no interfaces.  So tls.library must not link NetX Duo; it calls
 *   bsdsocket.library's copy.
 *
 *   The coupling surface was measured.  `nm` on libnx_secure.a + libnx_crypto.a
 *   + libcrypto68k.a + libaminetxduo_tls.a, minus everything they define
 *   themselves, leaves exactly twelve NetX Duo/ThreadX entry points (plus
 *   memcpy, ami_random and __udivdi3, which are ordinary link-time
 *   dependencies).  Those twelve are the middle block below;
 *   src/tlslib/tls_netx.c defines the vendored symbol names as one-line
 *   forwarders through this table, so no nx_secure source is touched and no
 *   NetX Duo object is duplicated.
 *
 *   The three ThreadX *data* symbols nx_secure also wants
 *   (_tx_thread_current_ptr, _tx_thread_system_state, _tx_timer_thread) are
 *   referenced only by the `nxe_*` argument-checking wrappers.  tls.library
 *   calls the `_nx_secure_*` entry points directly and does its own argument
 *   checking at the LVO, so those archive members are never pulled in and the
 *   data symbols never become undefined.  Verified with `nm` after the link.
 *
 *   One private LVO on bsdsocket.library at AMI_NXD_CONTEXT_LVO, present only
 *   in an AMINETXDUO_TLS build.  The public 121-vector Roadshow/AmiTCP ABI is
 *   untouched, and a default build of bsdsocket.library does not have this
 *   vector at all, which keeps that build byte-identical.
 *
 *   The magic and version arguments are load-bearing.  The slot sits past the
 *   last vector any published bsdsocket ABI defines, so the only way to reach
 *   it is on purpose; but if some future Roadshow allocates the same offset for
 *   something else, a caller of that function lands here with whatever it had
 *   in d0/d1, and this call must then do nothing.  Wrong magic or wrong version
 *   returns failure and writes nothing.
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

/*
 * The version carries the build option that changes struct layout.
 *
 * AMINETXDUO_IPV6 decides FEATURE_NX_IPV6, which changes the layout of NX_IP,
 * NX_INTERFACE, NX_PACKET and NX_TCP_SOCKET, all of which cross this interface.
 * Two libraries built from the same source with different answers would pass a
 * plain version check and then read each other's structs at the wrong offsets,
 * on a machine with no memory protection.  Folding the flag into the version
 * number turns that into a refusal at TLSOpen().
 *
 * Bump the high half whenever this header changes.
 */
#ifdef AMINETXDUO_IPV6
#define AMI_NXD_CONTEXT_VERSION     0x00020001UL
#else
#define AMI_NXD_CONTEXT_VERSION     0x00020000UL
#endif

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
     * The transport behind a descriptor.  `socket_base` is the caller's
     * SocketBase, because the descriptor table is per opener (docs/RESEARCH.md
     * 3.1: SocketBase is never shared), so the fd is meaningless without it.
     *
     * Returns NX_NULL unless the descriptor is a connected TCP socket.  Once a
     * descriptor has been passed over this way, the application must not
     * recv()/send() on it: those would take bytes out of the middle of a TLS
     * record.  Nothing enforces that -- enforcing it would mean a flag test on
     * the hot path of every recv() in the system -- so the caller must observe
     * it until TLSClose().
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

    /*
     * Release the ThreadX baton around a blocking Exec call and acquire it
     * again afterwards.  These are the hooks the SANA-II readers use to make it
     * legal for a ThreadX thread to sit in Wait() for an IORequest.
     *
     * tls.library needs them for one thing: reading a CA root out of
     * DEVS:Internet/certificates happens inside the handshake -- the issuer is
     * not known until the server sends its chain -- and dos.library's Read()
     * blocks in Exec.  Doing that while holding the baton would stop the IP
     * thread and both SANA-II readers for the duration of a disk access, which
     * on a floppy is long enough to lose packets.  Bracketing the file I/O with
     * these keeps the rest of the stack running while the head moves.
     *
     * Nesting is handled inside; a caller that does not hold the baton gets a
     * pair of no-ops.
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
     * Raw bytes, not nxc_random_rand() in a loop.  That one owes rand()'s
     * caller 0..0x7FFFFFFF and clears bit 31 to provide it, so a key built out
     * of it is short a bit in every 32.  nx_crypto's huge-number RBG draws the
     * ECDHE private key exactly that way; ami_crypto_rbg() uses this instead.
     */
    VOID                (*nxc_random_bytes)(APTR buffer, ULONG length);

    VOID                (*nxc_random_add_entropy)(const VOID *data, ULONG len,
                                                  ULONG credit_bits);
    ULONG               (*nxc_random_entropy_bits)(VOID);
} AmiNetXDuoContext;

/*
 * The vector itself, for anyone linking against src/bsdsocket directly.  A
 * shared-library caller reaches it through the LVO; see src/tlslib/tls_netx.c,
 * the only caller in the tree.
 *
 * Returns 0 and writes *ctx on success; -1 and writes nothing otherwise.
 */
LONG ami_nxd_context_obtain(ULONG magic, ULONG version,
                            const AmiNetXDuoContext **ctx, APTR socket_base);

#ifdef __cplusplus
}
#endif

#endif /* AMINETXDUO_NXCONTEXT_H */

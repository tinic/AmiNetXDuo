/*
 * tls.library -- LVO declarations.
 *
 * Hand-written, unlike src/bsdsocket/bsdsocket_vectors.h, because there is no
 * vendor .fd to generate them from -- this ABI is local.  The register
 * assignment here is the ABI, and has to match the inline stubs in
 * include/aminetxduo/tlslib.h exactly; those are the only other place it
 * appears.
 *
 * Only d0, d1, a0 and a1 carry arguments.  a2 upward are ones GCC wants for
 * itself on m68k, and every call here fits in four registers because anything
 * with more parameters takes a struct (TLSSelect) instead.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_TLS_VECTORS_H
#define AMINETXDUO_TLS_VECTORS_H

#include "tls_internal.h"

/* -6 / -12 / -18 / -24 */
struct TLSLibBase *tls_lib_open(register ULONG version __asm("d0"),
                                register struct TLSLibBase *TLSBase __asm("a6"));
APTR tls_lib_close(register struct TLSLibBase *TLSBase __asm("a6"));
APTR tls_lib_expunge(register struct TLSLibBase *TLSBase __asm("a6"));
APTR tls_lib_reserved(VOID);

/* -30 */
struct TLSConnection *tls_TLSOpenA(
        register APTR                  socket_base __asm("a0"),
        register const struct TagItem *tags        __asm("a1"),
        register LONG                  sock        __asm("d0"),
        register struct TLSLibBase    *TLSBase     __asm("a6"));

/* -36 */
VOID tls_TLSClose(register struct TLSConnection *conn    __asm("a0"),
                  register struct TLSLibBase    *TLSBase __asm("a6"));

/* -42 */
LONG tls_TLSRead(register struct TLSConnection *conn    __asm("a0"),
                 register APTR                  buffer  __asm("a1"),
                 register LONG                  length  __asm("d0"),
                 register struct TLSLibBase    *TLSBase __asm("a6"));

/* -48 */
LONG tls_TLSWrite(register struct TLSConnection *conn    __asm("a0"),
                  register CONST_APTR            buffer  __asm("a1"),
                  register LONG                  length  __asm("d0"),
                  register struct TLSLibBase    *TLSBase __asm("a6"));

/* -54 */
LONG tls_TLSPending(register struct TLSConnection *conn    __asm("a0"),
                    register struct TLSLibBase    *TLSBase __asm("a6"));

/* -60 */
LONG tls_TLSInfo(register struct TLSConnection *conn    __asm("a0"),
                 register struct TLSInfo       *info    __asm("a1"),
                 register struct TLSLibBase    *TLSBase __asm("a6"));

/* -66 */
CONST_STRPTR tls_TLSErrorString(register LONG               code    __asm("d0"),
                                register struct TLSLibBase *TLSBase __asm("a6"));

/* -72 */
LONG tls_TLSWaitSelect(register struct TLSSelect   *sel     __asm("a0"),
                       register struct TLSLibBase *TLSBase  __asm("a6"));

/* -78 */
LONG tls_TLSRandom(register APTR               buffer  __asm("a0"),
                   register LONG               length  __asm("d0"),
                   register struct TLSLibBase *TLSBase __asm("a6"));

/* -84 */
LONG tls_TLSBuffered(register struct TLSConnection *conn    __asm("a0"),
                     register struct TLSLibBase    *TLSBase __asm("a6"));

extern const APTR TlsVectorTable[];

#endif /* AMINETXDUO_TLS_VECTORS_H */

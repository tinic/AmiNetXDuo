/*
 * tls.library, LVO declarations.
 *
 * Hand-written, unlike src/bsdsocket/bsdsocket_vectors.h, because this ABI is
 * local and there is no vendor .fd to generate them from.  The register
 * assignment here is the ABI, and must match the inline stubs in
 * include/aminetxduo/tlslib.h exactly.  Those stubs are the only other place
 * it appears.
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

/* Register arguments are part of the m68k library ABI. A host regression
   compiles the shipping TLSClose() body, where those register names do not
   exist, so only that explicitly marked build erases the annotations. */
#ifdef TLSLIB_HOST_TEST
#  define TLSLIB_REG(name)
#else
#  define TLSLIB_REG(name) __asm(name)
#endif

/* -6 / -12 / -18 / -24 */
struct TLSLibBase *tls_lib_open(register ULONG version TLSLIB_REG("d0"),
                                register struct TLSLibBase *TLSBase TLSLIB_REG("a6"));
APTR tls_lib_close(register struct TLSLibBase *TLSBase TLSLIB_REG("a6"));
APTR tls_lib_expunge(register struct TLSLibBase *TLSBase TLSLIB_REG("a6"));
APTR tls_lib_reserved(VOID);

/* -30 */
struct TLSConnection *tls_TLSOpenA(
        register APTR                  socket_base TLSLIB_REG("a0"),
        register const struct TagItem *tags        TLSLIB_REG("a1"),
        register LONG                  sock        TLSLIB_REG("d0"),
        register struct TLSLibBase    *TLSBase     TLSLIB_REG("a6"));

/* -36 */
VOID tls_TLSClose(register struct TLSConnection *conn    TLSLIB_REG("a0"),
                  register struct TLSLibBase    *TLSBase TLSLIB_REG("a6"));

/* -42 */
LONG tls_TLSRead(register struct TLSConnection *conn    TLSLIB_REG("a0"),
                 register APTR                  buffer  TLSLIB_REG("a1"),
                 register LONG                  length  TLSLIB_REG("d0"),
                 register struct TLSLibBase    *TLSBase TLSLIB_REG("a6"));

/* -48 */
LONG tls_TLSWrite(register struct TLSConnection *conn    TLSLIB_REG("a0"),
                  register CONST_APTR            buffer  TLSLIB_REG("a1"),
                  register LONG                  length  TLSLIB_REG("d0"),
                  register struct TLSLibBase    *TLSBase TLSLIB_REG("a6"));

/* -54 */
LONG tls_TLSPending(register struct TLSConnection *conn    TLSLIB_REG("a0"),
                    register struct TLSLibBase    *TLSBase TLSLIB_REG("a6"));

/* -60 */
LONG tls_TLSInfo(register struct TLSConnection *conn    TLSLIB_REG("a0"),
                 register struct TLSInfo       *info    TLSLIB_REG("a1"),
                 register struct TLSLibBase    *TLSBase TLSLIB_REG("a6"));

/* -66 */
CONST_STRPTR tls_TLSErrorString(register LONG               code    TLSLIB_REG("d0"),
                                register struct TLSLibBase *TLSBase TLSLIB_REG("a6"));

/* -72 */
LONG tls_TLSWaitSelect(register struct TLSSelect   *sel     TLSLIB_REG("a0"),
                       register struct TLSLibBase *TLSBase TLSLIB_REG("a6"));

/* -78 */
LONG tls_TLSRandom(register APTR               buffer  TLSLIB_REG("a0"),
                   register LONG               length  TLSLIB_REG("d0"),
                   register struct TLSLibBase *TLSBase TLSLIB_REG("a6"));

/* -84 */
LONG tls_TLSBuffered(register struct TLSConnection *conn    TLSLIB_REG("a0"),
                     register struct TLSLibBase    *TLSBase TLSLIB_REG("a6"));

/* -90 */
LONG tls_TLSGetALPN(register struct TLSConnection *conn    TLSLIB_REG("a0"),
                    register APTR                  buffer  TLSLIB_REG("a1"),
                    register LONG                  size    TLSLIB_REG("d0"),
                    register struct TLSLibBase    *TLSBase TLSLIB_REG("a6"));

extern const APTR TlsVectorTable[];

#endif /* AMINETXDUO_TLS_VECTORS_H */

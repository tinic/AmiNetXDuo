/*
 * tls.library, the LVO vector table.
 *
 * Ten user vectors and no reserved slots after them.  A caller reaching past
 * the end of the table lands on the (APTR)-1 terminator, which MakeLibrary()
 * does not turn into a jump.
 *
 * SPDX-License-Identifier: MIT
 */

#include "tls_vectors.h"

const APTR TlsVectorTable[] =
{
    /* The four Exec vectors, then TLS_LIB_VECTORS user vectors, then the
       terminator, asserted below, which stops a new vector from shipping
       without TLS_LIB_VERSION moving. */
    /* -6 Open, -12 Close, -18 Expunge, -24 Reserved */
    (APTR)tls_lib_open,
    (APTR)tls_lib_close,
    (APTR)tls_lib_expunge,
    (APTR)tls_lib_reserved,

    (APTR)tls_TLSOpenA,             /* -0x01e  TLSOpenA        */
    (APTR)tls_TLSClose,             /* -0x024  TLSClose        */
    (APTR)tls_TLSRead,              /* -0x02a  TLSRead         */
    (APTR)tls_TLSWrite,             /* -0x030  TLSWrite        */
    (APTR)tls_TLSPending,           /* -0x036  TLSPending      */
    (APTR)tls_TLSInfo,              /* -0x03c  TLSInfo         */
    (APTR)tls_TLSErrorString,       /* -0x042  TLSErrorString  */
    (APTR)tls_TLSWaitSelect,        /* -0x048  TLSWaitSelect   */
    (APTR)tls_TLSRandom,            /* -0x04e  TLSRandom       */
    (APTR)tls_TLSBuffered,          /* -0x054  TLSBuffered     */

    (APTR)-1
};

/*
 * The drift guard, and why TLS_LIB_VECTORS is derived from TLS_LIB_VERSION
 * rather than written down beside it.
 *
 * A vector added to the table above makes this assertion fail.  The only way
 * to make it pass is to declare a TLS_LIB_VECTORS_V<n> in the public header
 * and point TLS_LIB_VERSION at it.  So a new vector cannot reach a shipped
 * library without the version moving with it, and OpenLibrary() stays
 * accurate.
 *
 * Version 2 nearly went out wrong.  TLSRandom and TLSBuffered went into the
 * table while the version stayed at 1, and OpenLibrary("tls.library", 1) then
 * hands an older library to a caller that jumps past its jump table.
 */
_Static_assert(sizeof(TlsVectorTable) / sizeof(TlsVectorTable[0]) ==
                   (4u + TLS_LIB_VECTORS + 1u),
               "TLS_LIB_VERSION must be bumped when a vector is added, see "
               "the version rule in include/aminetxduo/tlslib.h");

/* And the last LVO the header publishes must be the last one here, or a
   caller that checks lib_NegSize against TLS_LVO_LAST checks the wrong
   thing. */
_Static_assert((4u + TLS_LIB_VECTORS) * 6u == (unsigned)(-TLS_LVO_LAST),
               "TLS_LVO_LAST must name the last vector in TlsVectorTable");

/*
 * tls.library -- the LVO vector table.
 *
 * Ten user vectors, and no reserved slots after them: this ABI is ours, so
 * "reserved for future expansion" would only mean "we have not decided yet".
 * A caller reaching past the end of the table lands on the (APTR)-1
 * terminator, which MakeLibrary() does not turn into a jump.
 *
 * SPDX-License-Identifier: MIT
 */

#include "tls_vectors.h"

const APTR TlsVectorTable[] =
{
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

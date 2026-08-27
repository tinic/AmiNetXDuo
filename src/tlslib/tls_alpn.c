/*
 * tls.library, RFC 7301 application protocols.
 *
 * TLSA_ALPN takes one comma-separated string because that is what a caller
 * has: "h2,http/1.1".  The wire wants a run of length-prefixed names with no
 * separator and no terminator, and the conversion is here.
 *
 * The encoded list lives IN THE CONNECTION and not on the caller's stack:
 * nx_secure keeps the pointer and reads it when it builds the ClientHello,
 * which is after TLSOpen()'s caller has moved on from its tag list.
 *
 * SPDX-License-Identifier: MIT
 */

#include "tls_vectors.h"

/*
 * Encode `list` into conn->tc_Alpn.  TLS_ERR_BADALPN for an empty name, a name
 * over TLS_ALPN_NAME_MAX, or a list that does not fit -- never a shortened
 * offer, because an offer the caller did not make is one it cannot honour.
 */
LONG tls_alpn_encode(TLSConnection *conn, CONST_STRPTR list)
{
    const char *p;
    ULONG       out = 0;

    if (conn == NULL)
        return TLS_ERR_INTERNAL;

    conn->tc_AlpnLength = 0;

    if (list == NULL || *(const char *)list == '\0')
        return TLS_OK;

    p = (const char *)list;

    while (*p != '\0')
    {
        const char *start = p;
        ULONG       n;

        while (*p != '\0' && *p != ',')
            p++;

        n = (ULONG)(p - start);

        if (n == 0 || n > (ULONG)TLS_ALPN_NAME_MAX)
            return TLS_ERR_BADALPN;
        if ((out + 1UL + n) > (ULONG)TLS_ALPN_LIST_MAX)
            return TLS_ERR_BADALPN;

        conn->tc_Alpn[out++] = (UBYTE)n;
        tls_memcpy(&conn->tc_Alpn[out], start, n);
        out += n;

        if (*p == ',')
        {
            p++;

            /* A trailing or doubled comma is an empty name, which RFC 7301
               3.1 does not have a spelling for. */
            if (*p == '\0')
                return TLS_ERR_BADALPN;
        }
    }

    conn->tc_AlpnLength = (UWORD)out;

    return TLS_OK;
}

/* ------------------------------------------------------- TLSGetALPN --- */

LONG tls_TLSGetALPN(register struct TLSConnection *conn    TLSLIB_REG("a0"),
                    register APTR                  buffer  TLSLIB_REG("a1"),
                    register LONG                  size    TLSLIB_REG("d0"),
                    register struct TLSLibBase    *TLSBase TLSLIB_REG("a6"))
{
    const UCHAR *protocol = NX_NULL;
    UCHAR        length   = 0;

    (VOID)TLSBase;

    if (conn == NULL || buffer == NULL || size <= 0)
        return -1;

    if (_nx_secure_tls_alpn_protocol_get(&conn->tc_Session, &protocol,
                                         &length) != NX_SUCCESS)
    {
        /* Nothing negotiated.  Not an error: a server may decline ALPN, and
           for HTTP the answer is then HTTP/1.1 by the pre-ALPN default. */
        *(char *)buffer = '\0';
        return 0;
    }

    /* NUL terminated for the caller's convenience, and refused rather than
       truncated: a half protocol name is a different protocol. */
    if ((LONG)length + 1 > size)
        return -1;

    tls_memcpy(buffer, protocol, (ULONG)length);
    ((char *)buffer)[length] = '\0';

    return (LONG)length;
}

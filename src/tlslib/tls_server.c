/*
 * tls.library as the server: the identity a TLSA_Server connection presents.
 *
 * nx_secure has had a complete TLS server state machine all along -- both
 * handshakes, the ClientHello parse, the ServerHello and the key exchange --
 * and tls.library had no way to enter it.  It was 2.8% of the library that
 * nothing could reach.  This is the door: two DER files off disk, an
 * NX_SECURE_X509_CERT built from them, and that certificate added to the
 * session as the local one.  tls_conn.c does the rest by handing the transport
 * `server` and letting _nx_secure_tls_session_start() take the other branch.
 *
 * DER AND NOT PEM, and one certificate and not a chain.  A PEM reader is
 * base64 plus a line parser plus a multi-certificate walk, in a library on a
 * machine with no memory protection, for a conversion the machine that made
 * the key can do once with any tool.  tools/mkcertstore.py already takes that
 * position for the trust store.
 *
 * SPDX-License-Identifier: MIT
 */

#include "tls_internal.h"

#include "ami_tls_crypto.h"

#include <dos/dos.h>
#include <exec/semaphores.h>
#include <proto/dos.h>
#include <proto/exec.h>

/*
 * Read a whole file, or refuse.  Not Seek()-then-Read(): the length has to be
 * checked against the buffer anyway, and a file that grew between the two
 * calls would be read short with no error.  A file that does not fit is
 * TLS_ERR_BADCERT and not a truncated certificate.
 */
static LONG tls_server_read_der(const char *path, UCHAR *buffer, ULONG max,
                                ULONG *length)
{
    BPTR  fh;
    ULONG total = 0;

    *length = 0;

    fh = Open((STRPTR)path, MODE_OLDFILE);
    if (fh == (BPTR)0)
        return TLS_ERR_BADCERT;

    while (total < max)
    {
        LONG n = Read(fh, &buffer[total], (LONG)(max - total));

        if (n < 0)
        {
            Close(fh);
            return TLS_ERR_BADCERT;
        }
        if (n == 0)
            break;

        total += (ULONG)n;
    }

    if (total == max)
    {
        UBYTE probe;

        /* One byte past the cap: a file that has one is a file this cannot
           hold, and half a DER structure parses as a different one. */
        if (Read(fh, &probe, 1) != 0)
        {
            Close(fh);
            return TLS_ERR_BADCERT;
        }
    }

    Close(fh);

    /* 0x30 is SEQUENCE, the first byte of every DER certificate and every DER
       private key.  A PEM file starts "-----BEGIN", and telling the caller
       that here is better than an X.509 parse error later. */
    if (total < 4 || buffer[0] != 0x30)
        return TLS_ERR_BADCERT;

    *length = total;

    return TLS_OK;
}

LONG tls_server_identity(TLSConnection *conn, CONST_STRPTR cert_path,
                         CONST_STRPTR key_path, ULONG key_type)
{
    UINT  nx_key_type;
    UINT  status;
    LONG  error;

    if (conn == NULL)
        return TLS_ERR_INTERNAL;

    if (cert_path == NULL || key_path == NULL ||
        *(const char *)cert_path == '\0' || *(const char *)key_path == '\0')
        return TLS_ERR_NOCERT;

    switch (key_type)
    {
    case TLS_KEY_EC:
        nx_key_type = NX_SECURE_X509_KEY_TYPE_EC_DER;
        break;
    case TLS_KEY_RSA:
        nx_key_type = NX_SECURE_X509_KEY_TYPE_RSA_PKCS1_DER;
        break;
    default:
        return TLS_ERR_BADCERT;
    }

    conn->tc_LocalDer = (UCHAR *)tls_alloc(TLS_SERVER_DER_MAX);
    conn->tc_LocalKey = (UCHAR *)tls_alloc(TLS_SERVER_KEY_MAX);
    if (conn->tc_LocalDer == NULL || conn->tc_LocalKey == NULL)
        return TLS_ERR_NOMEM;

    error = tls_server_read_der((const char *)cert_path, conn->tc_LocalDer,
                                TLS_SERVER_DER_MAX, &conn->tc_LocalDerLength);
    if (error != TLS_OK)
        return error;

    error = tls_server_read_der((const char *)key_path, conn->tc_LocalKey,
                                TLS_SERVER_KEY_MAX, &conn->tc_LocalKeyLength);
    if (error != TLS_OK)
        return error;

    /*
     * raw_data_buffer NULL: the certificate is parsed in place out of
     * tc_LocalDer, which lives as long as the connection does.  That is the
     * same arrangement tls_store.c uses for a root.
     */
    status = _nx_secure_x509_certificate_initialize(&conn->tc_LocalCert,
                                                     conn->tc_LocalDer,
                                                     (USHORT)conn->tc_LocalDerLength,
                                                     NX_NULL, 0,
                                                     conn->tc_LocalKey,
                                                     (USHORT)conn->tc_LocalKeyLength,
                                                     nx_key_type);
    if (status != NX_SUCCESS)
        return TLS_ERR_BADCERT;

    /*
     * ami_tls_local_certificate_add() and not the bare nx_secure call: it also
     * records the RSA primes, and CRT is the difference between about seven
     * and about twenty-five seconds for the private-key operation this server
     * performs on every unresumed handshake.
     */
    status = ami_tls_local_certificate_add(&conn->tc_Session,
                                            &conn->tc_LocalCert);
    if (status != NX_SUCCESS)
        return TLS_ERR_BADCERT;

    ObtainSemaphore(&conn->tc_Base->tb_Lock);
    conn->tc_Base->tb_ServerKeys++;
    ReleaseSemaphore(&conn->tc_Base->tb_Lock);

    return TLS_OK;
}

VOID tls_server_forget(TLSConnection *conn)
{
    if (conn == NULL || conn->tc_LocalDer == NULL)
        return;

    /*
     * The prime table in ami_tls_crypto.c points INTO tc_LocalKey's parse and
     * is process-wide, so it can only be cleared when the last server
     * connection has gone.  Clearing it here unconditionally would strip
     * every other live server of CRT; not clearing it would leave it pointing
     * at memory that is about to be handed back by AllocVec().
     */
    if (conn->tc_Base != NULL)
    {
        ObtainSemaphore(&conn->tc_Base->tb_Lock);
        if (conn->tc_Base->tb_ServerKeys > 0)
            conn->tc_Base->tb_ServerKeys--;
        if (conn->tc_Base->tb_ServerKeys == 0)
            ami_tls_rsa_key_reset();
        ReleaseSemaphore(&conn->tc_Base->tb_Lock);
    }

    /* The private key goes back to AllocVec()'s free list otherwise, and
       there is no MMU here to fault a read of it. */
    if (conn->tc_LocalKey != NULL)
        tls_bzero(conn->tc_LocalKey, TLS_SERVER_KEY_MAX);

    tls_free(conn->tc_LocalKey);
    tls_free(conn->tc_LocalDer);

    conn->tc_LocalKey = NULL;
    conn->tc_LocalDer = NULL;
    conn->tc_LocalKeyLength = 0;
    conn->tc_LocalDerLength = 0;
}

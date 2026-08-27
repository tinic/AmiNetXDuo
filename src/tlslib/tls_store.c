/*
 * tls.library trust store: DEVS:Internet/certificates, an index read once at
 * TLSOpen() and the one root a chain needs read and parsed lazily inside the
 * handshake.  Big-endian on disk; written by tools/mkcertstore.py.
 *
 * SPDX-License-Identifier: MIT
 */

#include "tls_internal.h"

#include <dos/dos.h>
#include <proto/dos.h>
#include <proto/exec.h>

#define TLS_STORE_MAGIC     0x41435331UL        /* 'ACS1' */
#define TLS_STORE_HEADER    16UL
#define TLS_STORE_ENTRY     12UL
#define TLS_STORE_MAX_ROOTS 1024UL              /* sanity, not policy */

/* The index is decoded in place over the bytes read from the file, which is
   only sound while the struct is exactly the record size.  If a field is ever
   added, allocate a second array instead of relaxing this. */
_Static_assert(sizeof(TLSStoreEntry) == TLS_STORE_ENTRY,
               "TLSStoreEntry must match the on-disk record exactly");

/* ------------------------------------------------------------- FNV-1a --- */

#define TLS_FNV_OFFSET      2166136261UL
#define TLS_FNV_PRIME       16777619UL

static ULONG tls_fnv1a(const UCHAR *data, ULONG length)
{
    ULONG hash = TLS_FNV_OFFSET;

    while (length-- > 0)
    {
        hash ^= (ULONG)*data++;
        hash *= TLS_FNV_PRIME;
    }

    /* Zero means "no key" everywhere in this file, so it must not be a real
       one.  tools/mkcertstore.py applies the same fold. */
    if (hash == 0)
        hash = TLS_FNV_PRIME;

    return hash;
}

/* --------------------------------------------------- the ASN.1 walk ----- */

/*
 * The issuer Name, as bytes.  nx_secure parses the issuer into fields and
 * keeps pointers to the values, which cannot recover the enclosing Name, so
 * this walks the TBSCertificate to its fourth field with the vendored parser.
 */
static const UCHAR *tls_issuer_name_der(const NX_SECURE_X509_CERT *cert,
                                        ULONG *out_length)
{
    const UCHAR *p;
    const UCHAR *data;
    ULONG        avail;
    ULONG        scratch;
    ULONG        tlv_length;
    ULONG        header_length;
    USHORT       tlv_type;
    USHORT       tlv_class;
    UINT         status;
    UINT         field;

    if (cert == NX_NULL)
        return NX_NULL;

    p     = cert->nx_secure_x509_certificate_data;
    avail = cert->nx_secure_x509_certificate_data_length;

    if (p == NX_NULL || avail == 0)
        return NX_NULL;

    scratch = avail;
    status  = _nx_secure_x509_asn1_tlv_block_parse(p, &scratch, &tlv_type,
                                                   &tlv_class, &tlv_length,
                                                   &data, &header_length);
    if (status != NX_SECURE_X509_SUCCESS ||
        tlv_type != NX_SECURE_ASN_TAG_SEQUENCE ||
        tlv_class != NX_SECURE_ASN_TAG_CLASS_UNIVERSAL)
    {
        return NX_NULL;
    }

    p     = data;
    avail = tlv_length;

    for (field = 0; field < 3; field++)
    {
        scratch = avail;
        status  = _nx_secure_x509_asn1_tlv_block_parse(p, &scratch, &tlv_type,
                                                       &tlv_class, &tlv_length,
                                                       &data, &header_length);
        if (status != NX_SECURE_X509_SUCCESS)
            return NX_NULL;

        if (field == 0 && tlv_class != NX_SECURE_ASN_TAG_CLASS_CONTEXT)
        {
            /* No explicit version: this block is already the serial number,
               so it counts as field 1 and not as the version. */
            field++;
        }

        if ((header_length + tlv_length) > avail)
            return NX_NULL;

        p     += header_length + tlv_length;
        avail -= header_length + tlv_length;
    }

    scratch = avail;
    status  = _nx_secure_x509_asn1_tlv_block_parse(p, &scratch, &tlv_type,
                                                   &tlv_class, &tlv_length,
                                                   &data, &header_length);
    if (status != NX_SECURE_X509_SUCCESS ||
        tlv_type != NX_SECURE_ASN_TAG_SEQUENCE ||
        tlv_class != NX_SECURE_ASN_TAG_CLASS_UNIVERSAL)
    {
        return NX_NULL;
    }

    if ((header_length + tlv_length) > avail)
        return NX_NULL;

    *out_length = header_length + tlv_length;

    return p;
}

static ULONG tls_cert_issuer_key(const NX_SECURE_X509_CERT *cert)
{
    const UCHAR *name;
    ULONG        length = 0;

    name = tls_issuer_name_der(cert, &length);
    if (name == NX_NULL || length == 0)
        return 0;

    return tls_fnv1a(name, length);
}

/* ------------------------------------------------------- the index ------ */

static ULONG tls_be32(const UBYTE *p)
{
    return ((ULONG)p[0] << 24) | ((ULONG)p[1] << 16) |
           ((ULONG)p[2] <<  8) |  (ULONG)p[3];
}

VOID tls_store_close(TLSStore *store)
{
    if (store == NULL)
        return;

    tls_free(store->ts_Index);
    store->ts_Index       = NULL;
    store->ts_Count       = 0;
    store->ts_Fingerprint = 0;
    store->ts_Path[0]     = '\0';
}

/*
 * Read the index.  Runs outside the ThreadX bracket, from TLSOpen(), which is
 * what lets it block in dos.library without returning the baton the way
 * tls_store_fetch() must.
 */
LONG tls_store_open(TLSStore *store, const char *path)
{
    BPTR   fh;
    UBYTE  header[TLS_STORE_HEADER];
    UBYTE *raw;
    ULONG  count;
    ULONG  index_offset;
    ULONG  i;

    if (store == NULL || path == NULL || DOSBase == NULL)
        return TLS_ERR_INTERNAL;

    tls_store_close(store);

    fh = Open((STRPTR)path, MODE_OLDFILE);
    if (fh == (BPTR)0)
        return TLS_ERR_TRUSTSTORE;

    if (Read(fh, header, (LONG)TLS_STORE_HEADER) != (LONG)TLS_STORE_HEADER ||
        tls_be32(header) != TLS_STORE_MAGIC)
    {
        Close(fh);
        return TLS_ERR_TRUSTSTORE;
    }

    count        = tls_be32(&header[4]);
    index_offset = tls_be32(&header[8]);

    if (count == 0 || count > TLS_STORE_MAX_ROOTS ||
        index_offset != TLS_STORE_HEADER)
    {
        Close(fh);
        return TLS_ERR_TRUSTSTORE;
    }

    raw = (UBYTE *)tls_alloc(count * TLS_STORE_ENTRY);
    if (raw == NULL)
    {
        Close(fh);
        return TLS_ERR_NOMEM;
    }

    if (Read(fh, raw, (LONG)(count * TLS_STORE_ENTRY)) !=
        (LONG)(count * TLS_STORE_ENTRY))
    {
        tls_free(raw);
        Close(fh);
        return TLS_ERR_TRUSTSTORE;
    }

    Close(fh);

    {
        TLSStoreEntry *index = (TLSStoreEntry *)raw;

        for (i = count; i-- > 0; )
        {
            ULONG key = tls_be32(&raw[i * TLS_STORE_ENTRY]);
            ULONG off = tls_be32(&raw[i * TLS_STORE_ENTRY + 4]);
            ULONG len = tls_be32(&raw[i * TLS_STORE_ENTRY + 8]);

            index[i].se_Key    = key;
            index[i].se_Offset = off;
            index[i].se_Length = len;
        }

        store->ts_Index = index;
    }

    store->ts_Count = count;
    tls_strncpy(store->ts_Path, path, sizeof(store->ts_Path));

    /*
     * The fingerprint of this root set.  src/tlslib/tls_resume.c keys a cached
     * session on it, so a session checked against one trust store cannot be
     * resumed by a caller that presents a different one.
     */
    {
        ULONG hash = TLS_FNV_OFFSET;

        hash ^= count;
        hash *= TLS_FNV_PRIME;

        for (i = 0; i < count; i++)
        {
            hash ^= store->ts_Index[i].se_Key;    hash *= TLS_FNV_PRIME;
            hash ^= store->ts_Index[i].se_Offset; hash *= TLS_FNV_PRIME;
            hash ^= store->ts_Index[i].se_Length; hash *= TLS_FNV_PRIME;
        }

        /* Zero is "no store" everywhere in the resumption key, so it must not
           be a real fingerprint. */
        store->ts_Fingerprint = (hash == 0) ? TLS_FNV_PRIME : hash;
    }

    return TLS_OK;
}

ULONG tls_store_count(const TLSStore *store)
{
    return (store != NULL) ? store->ts_Count : 0;
}

static ULONG tls_store_fetch(TLSStore *store, ULONG key, UCHAR *buffer, ULONG size)
{
    ULONG lo, hi, mid;
    ULONG offset = 0;
    ULONG length = 0;
    BPTR  fh;

    if (store == NULL || store->ts_Index == NULL || key == 0 || buffer == NULL)
        return 0;

    lo = 0;
    hi = store->ts_Count;

    while (lo < hi)
    {
        mid = lo + (hi - lo) / 2;

        if (store->ts_Index[mid].se_Key < key)
        {
            lo = mid + 1;
        }
        else if (store->ts_Index[mid].se_Key > key)
        {
            hi = mid;
        }
        else
        {
            offset = store->ts_Index[mid].se_Offset;
            length = store->ts_Index[mid].se_Length;
            break;
        }
    }

    if (length == 0 || length > size)
        return 0;

    /*
     * The one disk access inside the handshake.  It used to need the ThreadX
     * baton returned around it; tls.library holds no baton now, so a blocking
     * dos.library call here stalls this task and nothing else.
     */
    fh = Open((STRPTR)store->ts_Path, MODE_OLDFILE);
    if (fh == (BPTR)0)
        return 0;

    if (Seek(fh, (LONG)offset, OFFSET_BEGINNING) < 0 ||
        Read(fh, buffer, (LONG)length) != (LONG)length)
    {
        length = 0;
    }

    Close(fh);

    return length;
}

/* ------------------------------------------ the lazy certificate check -- */

/*
 * nx_secure's certificate-check hook takes the certificate store, not the
 * session, so the connection is recovered from it through this registry.
 */
#define TLS_REGISTRY_SLOTS      8

typedef struct TLSRegistrySlot
{
    NX_SECURE_X509_CERTIFICATE_STORE *rs_Store;
    TLSConnection                    *rs_Conn;
} TLSRegistrySlot;

static TLSRegistrySlot tls_registry[TLS_REGISTRY_SLOTS];

/*
 * The scans take the same Forbid() as the claim and the release: a TLSClose()
 * on another task can clear a slot and free the TLSConnection between the load
 * of rs_Conn and the dereference below.
 */
static TLSConnection *tls_conn_for_store(NX_SECURE_X509_CERTIFICATE_STORE *store)
{
    TLSConnection *conn = NULL;
    UWORD          i;

    Forbid();
    for (i = 0; i < TLS_REGISTRY_SLOTS; i++)
    {
        if (tls_registry[i].rs_Store == store)
        {
            conn = tls_registry[i].rs_Conn;
            break;
        }
    }
    Permit();

    return conn;
}

TLSConnection *tls_conn_for_session(const NX_SECURE_TLS_SESSION *session)
{
    TLSConnection *conn = NULL;
    UWORD          i;

    Forbid();
    for (i = 0; i < TLS_REGISTRY_SLOTS; i++)
    {
        if (tls_registry[i].rs_Conn != NULL &&
            &tls_registry[i].rs_Conn->tc_Session == session)
        {
            conn = tls_registry[i].rs_Conn;
            break;
        }
    }
    Permit();

    return conn;
}

static VOID tls_registry_remove(TLSConnection *conn)
{
    UWORD i;

    Forbid();
    for (i = 0; i < TLS_REGISTRY_SLOTS; i++)
    {
        if (tls_registry[i].rs_Conn == conn)
        {
            tls_registry[i].rs_Conn  = NULL;
            tls_registry[i].rs_Store = NULL;
        }
    }
    Permit();
}

/* Give the store whatever roots this chain needs, and no others. */
static VOID tls_store_supply(TLSConnection *conn,
                             NX_SECURE_X509_CERTIFICATE_STORE *store)
{
    NX_SECURE_X509_CERT *cert;
    ULONG                key;
    ULONG                length;
    ULONG                i;
    UINT                 status;

    if (conn == NULL || conn->tc_Store == NULL || conn->tc_RootDer == NULL)
        return;

    conn->tc_ChainDepth = 0;

    for (cert = store->nx_secure_x509_remote_certificates;
         cert != NX_NULL;
         cert = cert->nx_secure_x509_next_certificate)
    {
        conn->tc_ChainDepth++;

        if (conn->tc_RootsLoaded >= TLS_MAX_ROOTS)
            break;

        key = tls_cert_issuer_key(cert);
        if (key == 0)
            continue;

        for (i = 0; i < conn->tc_RootsLoaded; i++)
        {
            if (conn->tc_RootKey[i] == key)
                break;
        }
        if (i < conn->tc_RootsLoaded)
            continue;

        i      = conn->tc_RootsLoaded;
        length = tls_store_fetch(conn->tc_Store, key,
                                 &conn->tc_RootDer[i * TLS_ROOT_DER_MAX],
                                 TLS_ROOT_DER_MAX);
        if (length == 0)
            continue;

        /*
         * raw_data_buffer is NULL: the DER stays where it was read, memory
         * this connection owns until TLSClose(), which is the lifetime
         * nx_secure needs -- the parsed certificate points into it.
         */
        status = _nx_secure_x509_certificate_initialize(
                     &conn->tc_Root[i],
                     &conn->tc_RootDer[i * TLS_ROOT_DER_MAX],
                     (USHORT)length, NX_NULL, 0, NX_NULL, 0,
                     NX_SECURE_X509_KEY_TYPE_NONE);
        if (status != NX_SUCCESS)
            continue;

        status = _nx_secure_x509_store_certificate_add(
                     &conn->tc_Root[i], store,
                     NX_SECURE_X509_CERT_LOCATION_TRUSTED);
        if (status != NX_SUCCESS)
            continue;

        conn->tc_RootKey[i] = key;
        conn->tc_RootsLoaded++;
    }
}

static UINT tls_store_verify(NX_SECURE_X509_CERTIFICATE_STORE *store,
                             NX_SECURE_X509_CERT *certificate,
                             ULONG current_time)
{
    TLSConnection *conn = tls_conn_for_store(store);

    if (conn != NULL)
        tls_store_supply(conn, store);

    return _nx_secure_remote_certificate_verify(store, certificate,
                                                current_time);
}

/*
 * Registration is its own call, made for every connection: src/tlslib/
 * tls_resume.c reaches a connection from an NX_SECURE_TLS_SESSION through this
 * registry too, and resumption runs whether or not the chain is checked.
 */
VOID tls_registry_add(TLSConnection *conn)
{
    NX_SECURE_X509_CERTIFICATE_STORE *store;
    UWORD                             i;

    if (conn == NULL)
        return;

    store = &conn->tc_Session.nx_secure_tls_credentials
                 .nx_secure_tls_certificate_store;

    tls_registry_remove(conn);

    Forbid();
    for (i = 0; i < TLS_REGISTRY_SLOTS; i++)
    {
        if (tls_registry[i].rs_Conn == NULL)
        {
            tls_registry[i].rs_Conn  = conn;
            tls_registry[i].rs_Store = store;
            break;
        }
    }
    Permit();
}

VOID tls_store_attach(TLSConnection *conn)
{
    if (conn == NULL)
        return;

    if (tls_conn_for_session(&conn->tc_Session) != conn)
    {
        /* The registry is full.  A silent check against an empty trusted store
           is a security hole, so the vendored check stays in place: it fails
           closed with ISSUER_CERTIFICATE_NOT_FOUND. */
        return;
    }

    conn->tc_Session.nx_secure_remote_certificate_verify = tls_store_verify;
}

VOID tls_store_detach(TLSConnection *conn)
{
    tls_registry_remove(conn);
}

/*
 * tls.library, the trust store, and the lazy loader that reads it.
 *
 *   The Mozilla root set is about 120 certificates and 125 KB of DER.  This
 *   machine has four megabytes.  Parsing all of them at startup is not viable
 *   and neither is holding them: an NX_SECURE_X509_CERT is 252 bytes, so the
 *   parsed set alone is 30 KB before the DER it points into, and the parse
 *   itself is 120 ASN.1 walks on a 14 MHz 68020 in front of a user waiting for
 *   a web page.
 *
 *   A chain needs exactly one of them.  So the file is indexed, the index is
 *   the only part read up front, and the one root a chain needs is read and
 *   parsed while the handshake is already in progress.
 *
 *   DEVS:Internet/certificates, written by tools/mkcertstore.py.  Big-endian
 *   throughout, which is the machine's own order, so nothing is byte-swapped.
 *
 *       0   'A' 'C' 'S' '1'      magic and format version
 *       4   ULONG count          roots in the file
 *       8   ULONG index_offset   always 16
 *      12   ULONG data_offset    first byte of the first DER
 *      16   count x 12 bytes     the index, sorted ascending by key:
 *                ULONG key       FNV-1a 32 over the subject Name DER
 *                ULONG offset    absolute file offset of this root's DER
 *                ULONG length    its length in bytes
 *      ...  the DER blobs, concatenated
 *
 *   For the Mozilla set that is 16 + 1,428 + 127,484 = about 126 KB on disk,
 *   of which 1,428 bytes are ever resident.
 *
 *   RFC 5280 says a certificate's issuer field matches a CA's subject field
 *   when the two encoded Names are equal.  The key is a hash of exactly those
 *   bytes, the Name SEQUENCE including its tag and length, taken from the
 *   subject side by the generator and from the issuer side here.  Nothing is
 *   parsed into fields on either side, so the two implementations cannot
 *   disagree about what a Name means.
 *
 *   nx_secure's own store lookup compares distinguished names by common name
 *   only unless NX_SECURE_X509_STRICT_NAME_COMPARE is defined, and in the
 *   current Mozilla set four roots share the common name "GlobalSign" and four
 *   more have no common name at all.  Handing nx_secure a store with all four
 *   GlobalSign roots in it would let it pick the first and fail the signature
 *   check.  Matching on the full Name adds exactly the one root the chain
 *   asked for, so the name nx_secure then looks up is unambiguous by
 *   construction.
 *
 *   Roots expire, get distrusted, and get added.  There is no package manager
 *   on this machine and there is not going to be one, so the update story is:
 *   replace the file.  Either copy a fresh `certificates` from a release, or
 *   run tools/mkcertstore.py over a current cacert.pem.  The index is read
 *   fresh at every TLSOpen() and belongs to that connection, so a replacement
 *   takes effect on the next connection, no reboot, no `avail flush`, no
 *   cache to invalidate.  See the note on TLSStore in tls_internal.h for the
 *   cost.
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

    /*
     * Zero means "no key" everywhere in this file, so it must not be a real
     * one.  tools/mkcertstore.py applies the same fold, and would refuse to
     * emit an entry that produced one.
     */
    if (hash == 0)
        hash = TLS_FNV_PRIME;

    return hash;
}

/* --------------------------------------------------- the ASN.1 walk ----- */

/*
 * The issuer Name, as bytes.  nx_secure parses the issuer into fields and
 * keeps pointers to the field values, which is not enough to recover the
 * enclosing Name, so this walks the TBSCertificate again, four fields deep,
 * using the vendored TLV parser so there is no second ASN.1 implementation in
 * the tree.
 *
 *   TBSCertificate ::= SEQUENCE {
 *       version         [0] EXPLICIT Version DEFAULT v1,
 *       serialNumber        CertificateSerialNumber,
 *       signature           AlgorithmIdentifier,
 *       issuer              Name,                    <-- this one
 *       ... }
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

    /* The TBSCertificate SEQUENCE itself. */
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

    /*
     * Walk to the issuer.  `field` counts what has been consumed:
     *   0, nothing yet; the next block is either [0] version or the serial
     *   1, serial consumed
     *   2, signature AlgorithmIdentifier consumed; the next block is the
     *        issuer
     */
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

    /* Whatever is here now is the issuer Name. */
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
 * Read the index.  Called once per connection, from TLSOpen() and therefore
 * outside the ThreadX bracket, which is what lets it do blocking dos.library
 * I/O without handing the baton back the way tls_store_fetch() has to.
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

    /*
     * Decode in place, so the 12-byte on-disk records become TLSStoreEntry
     * without a second allocation.  Only sound while the struct is exactly the
     * record size, hence the assertion at the top of this file.
     */
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
     * The fingerprint of this root set, computed here because this is the one
     * moment the whole index is in memory.  src/tlslib/tls_resume.c keys a
     * cached session on it, so a session verified against one trust store
     * cannot be resumed by a caller presenting a different one, a resumed
     * handshake verifies nothing, so the trust decision has to travel with the
     * session.
     *
     * Over the decoded values rather than the raw bytes, so the fingerprint
     * describes the root set and not the file encoding.
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
    const AmiNetXDuoContext *ctx;
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
     * The one disk access that happens inside the handshake, the issuer is
     * not known until the server has sent its chain, and therefore inside
     * the ThreadX bracket, holding the baton.  dos.library blocks in Exec, so
     * without this the IP thread and both SANA-II readers would stop for the
     * length of a disk access; on a floppy that is long enough to drop
     * packets.  Handing the baton back is what the SANA-II readers do around
     * their own Wait().  Both calls are no-ops for a caller that does not hold
     * it, so this stays correct if a future path reaches here outside a
     * bracket.
     */
    ctx = tls_netx_ctx();
    if (ctx != NULL)
        ctx->nxc_BatonRelease();

    fh = Open((STRPTR)store->ts_Path, MODE_OLDFILE);
    if (fh == (BPTR)0)
    {
        if (ctx != NULL)
            ctx->nxc_BatonAcquire();
        return 0;
    }

    if (Seek(fh, (LONG)offset, OFFSET_BEGINNING) < 0 ||
        Read(fh, buffer, (LONG)length) != (LONG)length)
    {
        length = 0;
    }

    Close(fh);

    if (ctx != NULL)
        ctx->nxc_BatonAcquire();

    return length;
}

/* ------------------------------------------------- the lazy verifier ---- */

/*
 * nx_secure's verify hook takes the certificate store, not the session, so the
 * connection has to be recovered from it.  A small registry rather than
 * pointer arithmetic through two vendored structs: eight entries, an O(8) scan
 * once per handshake, and no way to be wrong that only shows up as a wild
 * pointer on a machine with no memory protection.
 */
#define TLS_REGISTRY_SLOTS      8

typedef struct TLSRegistrySlot
{
    NX_SECURE_X509_CERTIFICATE_STORE *rs_Store;
    TLSConnection                    *rs_Conn;
} TLSRegistrySlot;

static TLSRegistrySlot tls_registry[TLS_REGISTRY_SLOTS];

/*
 * The scans take the same Forbid() as the claim and the release. One registry
 * for every opener, so a TLSClose() on another task can clear a slot and free
 * the TLSConnection between the load of rs_Conn and the dereference below,
 * which tls_conn_for_session() does, reading tc_Session through it.
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

/*
 * Give the store whatever roots this chain needs, and no others.
 *
 * Every certificate the server sent is asked "who issued you?"; the answer is
 * looked up in the file; a hit is parsed and put in the trusted store.  For an
 * ordinary two-deep chain that is one index hit and one disk read: the leaf's
 * issuer is the intermediate, which is not a root and misses in RAM, and the
 * intermediate's issuer is the root, which hits.
 */
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

        /* Already loaded for this connection? */
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
         * nx_secure needs (the parsed certificate is all pointers into it).
         * Copying it again would double the cost for nothing.
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
 * The registry is not only the trust store's: src/tlslib/tls_resume.c reaches
 * a connection from an NX_SECURE_TLS_SESSION through it too, and resumption
 * runs whether or not the chain is being verified.  So registration is its own
 * call, made for every connection, and tls_store_attach() only swaps the
 * verifier.
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
        /* The registry is full.  More than eight simultaneous handshakes on a
           68020 does not warrant its own code path, but silently verifying
           against an empty trusted store would be a security hole, so leave
           the vendored verifier in place: it fails closed with
           ISSUER_CERTIFICATE_NOT_FOUND. */
        return;
    }

    conn->tc_Session.nx_secure_remote_certificate_verify = tls_store_verify;
}

VOID tls_store_detach(TLSConnection *conn)
{
    tls_registry_remove(conn);
}

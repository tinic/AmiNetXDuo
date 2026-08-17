/*
 * tls.library, the peer's certificates and the caller's verdict on them.
 *
 *   Two things live here because they are the same moment.  nx_secure has
 *   finished checking the chain and has not yet returned; the certificates are
 *   parsed and in memory; and whether to go on with this peer is about to be
 *   decided.  A TLSA_VerifyHook is asked exactly there, and what it is shown is
 *   built exactly there.
 *
 *   WHAT IS CAPTURED, AND WHY IT IS COPIED
 *
 *   An NX_SECURE_X509_CERT is pointers into the DER buffer this connection
 *   allocated, so the parsed certificate is alive from the moment it is parsed
 *   until _nx_secure_tls_session_delete().  TLSClose() does that delete and
 *   then frees the DER, in that order, so a caller holding a subject line
 *   after TLSClose() would be reading freed memory either way.  The public
 *   contract is that a struct TLSCertificate lasts until TLSClose(), which is
 *   the lifetime of the connection and not of the session, so the strings are
 *   copied out rather than pointed at.
 *
 *   Two allocations for a whole chain, not two per string.  Every string is
 *   measured in a first pass, one block is allocated, and a second pass fills
 *   it.  Three certificates with subject, common name, issuer and a list of
 *   alternative names is a dozen strings, and a dozen AllocVec()s on a machine
 *   with a 1 MB heap is a dozen chances to fragment it for the sake of a
 *   structure that is read once and thrown away.
 *
 *   A field that will not parse is NULL, or 0 for the times, and never a
 *   failed handshake.  A malformed corner of a certificate is not a reason to
 *   refuse a connection that verified; it is a reason not to show that corner.
 *
 *   THE ORDER OF THE CHAIN
 *
 *   nx_secure's remote list is in receipt order, which is the server's order,
 *   which RFC 5246 says is leaf-first and which real servers get wrong.  The
 *   public contract says index 0 is the leaf and counting up goes towards the
 *   root, so the chain is walked here from the endpoint certificate through
 *   issuer names rather than trusted as it arrived.
 *
 * SPDX-License-Identifier: MIT
 */

#include "tls_internal.h"
#include "tls_certfmt.h"

#include "c68k_sha256.h"

/* ------------------------------------------------------------ the chain --- */

static BOOL tls_cert_self_signed(NX_SECURE_X509_CERT *cert)
{
    return (BOOL)((_nx_secure_x509_distinguished_name_compare(
                       &cert->nx_secure_x509_distinguished_name,
                       &cert->nx_secure_x509_issuer,
                       NX_SECURE_X509_NAME_ALL_FIELDS) == 0) ? TRUE : FALSE);
}

/*
 * The chain the peer sent, leaf first.
 *
 * The endpoint is the certificate nobody else in the list issued, which is what
 * _nx_secure_x509_remote_endpoint_certificate_get() answers, and each step up
 * is the certificate whose subject is this one's issuer.  A chain that does not
 * link, or that links back to something already seen, stops where it stops:
 * this is display, and half a chain shown is better than a loop walked.
 */
static ULONG tls_cert_chain(NX_SECURE_X509_CERTIFICATE_STORE *store,
                            NX_SECURE_X509_CERT **out, ULONG max)
{
    NX_SECURE_X509_CERT *cert = NX_NULL;
    ULONG                count = 0;
    ULONG                i;

    if (_nx_secure_x509_remote_endpoint_certificate_get(store, &cert)
            != NX_SUCCESS || cert == NX_NULL)
    {
        /* No endpoint: report the list as it arrived rather than nothing, so
           that a caller still learns who it was talking to. */
        for (cert = store->nx_secure_x509_remote_certificates;
             cert != NX_NULL && count < max;
             cert = cert->nx_secure_x509_next_certificate)
        {
            out[count++] = cert;
        }

        return count;
    }

    while (cert != NX_NULL && count < max)
    {
        NX_SECURE_X509_CERT *issuer;

        for (i = 0; i < count; i++)
        {
            if (out[i] == cert)
                return count;
        }

        out[count++] = cert;

        if (tls_cert_self_signed(cert))
            break;

        for (issuer = store->nx_secure_x509_remote_certificates;
             issuer != NX_NULL;
             issuer = issuer->nx_secure_x509_next_certificate)
        {
            if (issuer != cert &&
                _nx_secure_x509_distinguished_name_compare(
                    &issuer->nx_secure_x509_distinguished_name,
                    &cert->nx_secure_x509_issuer,
                    NX_SECURE_X509_NAME_ALL_FIELDS) == 0)
            {
                break;
            }
        }

        cert = issuer;
    }

    return count;
}

/* ------------------------------------------------------------ the parts --- */

/*
 * The subject alternative names, comma separated, or the length that would
 * take when dst is NULL.
 *
 * nx_secure keeps no list of them: the parser retains a pointer at the
 * extensions block and walks it on demand, and its one walker is a match
 * function, not an enumerator.  So this walks the GeneralNames itself, with
 * the same vendored TLV parser src/tlslib/tls_store.c uses for the issuer
 * Name, and there is still only one ASN.1 implementation in the tree.
 *
 *     SubjectAltName ::= GeneralNames
 *     GeneralNames   ::= SEQUENCE SIZE (1..MAX) OF GeneralName
 *     GeneralName    ::= CHOICE { ... dNSName [2] IA5String ... }
 *
 * Only dNSNames are taken.  An rfc822Name or an iPAddress in the list is a
 * name for something other than the host this connection is checked against,
 * and the field documents itself as what the host name is checked against.
 */
static ULONG tls_cert_alt_names(NX_SECURE_X509_CERT *cert, char *dst, ULONG size)
{
    NX_SECURE_X509_EXTENSION ext;
    const UCHAR             *p;
    const UCHAR             *data;
    ULONG                    avail;
    ULONG                    scratch;
    ULONG                    tlv_length;
    ULONG                    header_length;
    ULONG                    used = 0;
    USHORT                   tlv_type;
    USHORT                   tlv_class;

    if (_nx_secure_x509_extension_find(cert, &ext,
                                        NX_SECURE_TLS_X509_TYPE_SUBJECT_ALT_NAME)
            != NX_SECURE_X509_SUCCESS)
    {
        return 0;
    }

    p     = ext.nx_secure_x509_extension_data;
    avail = ext.nx_secure_x509_extension_data_length;

    if (p == NX_NULL || avail == 0)
        return 0;

    scratch = avail;
    if (_nx_secure_x509_asn1_tlv_block_parse(p, &scratch, &tlv_type, &tlv_class,
                                             &tlv_length, &data, &header_length)
            != NX_SECURE_X509_SUCCESS ||
        tlv_class != NX_SECURE_ASN_TAG_CLASS_UNIVERSAL ||
        tlv_type != NX_SECURE_ASN_TAG_SEQUENCE)
    {
        return 0;
    }

    p     = data;
    avail = tlv_length;

    while (avail > 0)
    {
        scratch = avail;
        if (_nx_secure_x509_asn1_tlv_block_parse(p, &scratch, &tlv_type,
                                                 &tlv_class, &tlv_length, &data,
                                                 &header_length)
                != NX_SECURE_X509_SUCCESS)
        {
            break;
        }

        if ((header_length + tlv_length) > avail)
            break;

        if (tlv_class == NX_SECURE_ASN_TAG_CLASS_CONTEXT &&
            tlv_type == NX_SECURE_X509_SUB_ALT_NAME_TAG_DNSNAME)
        {
            used = tls_cert_list_append(dst, size, used, data, tlv_length);
        }

        p     += header_length + tlv_length;
        avail -= header_length + tlv_length;
    }

    return used;
}

/*
 * The named curves, and their sizes in bits.
 *
 * A table rather than arithmetic on the encoded point: an uncompressed point is
 * one byte plus two field elements, which gives 256 and 384 correctly and 528
 * for P-521, whose field is 521 bits inside 66 bytes.  A caller comparing
 * against a policy number wants 521.
 */
static const struct
{
    ULONG   cb_Curve;
    ULONG   cb_Bits;
} tls_cert_curve_bits[] =
{
    { NX_SECURE_TLS_X509_EC_SECP160K1, 160 },
    { NX_SECURE_TLS_X509_EC_SECP160R1, 160 },
    { NX_SECURE_TLS_X509_EC_SECP160R2, 160 },
    { NX_SECURE_TLS_X509_EC_SECP192K1, 192 },
    { NX_SECURE_TLS_X509_EC_SECP192R1, 192 },
    { NX_SECURE_TLS_X509_EC_SECP224K1, 224 },
    { NX_SECURE_TLS_X509_EC_SECP224R1, 224 },
    { NX_SECURE_TLS_X509_EC_SECP256K1, 256 },
    { NX_SECURE_TLS_X509_EC_SECP256R1, 256 },
    { NX_SECURE_TLS_X509_EC_SECP384R1, 384 },
    { NX_SECURE_TLS_X509_EC_SECP521R1, 521 }
};

static VOID tls_cert_key(NX_SECURE_X509_CERT *cert, ULONG *type, ULONG *bits)
{
    *type = TLS_KEY_UNKNOWN;
    *bits = 0;

    if (cert->nx_secure_x509_public_algorithm == NX_SECURE_TLS_X509_TYPE_RSA)
    {
        *type = TLS_KEY_RSA;
        *bits = (ULONG)cert->nx_secure_x509_public_key.rsa_public_key
                    .nx_secure_rsa_public_modulus_length * 8UL;
        return;
    }

#ifdef NX_SECURE_ENABLE_ECC_CIPHERSUITE
    if (cert->nx_secure_x509_public_algorithm == NX_SECURE_TLS_X509_TYPE_EC)
    {
        ULONG curve = (ULONG)cert->nx_secure_x509_public_key.ec_public_key
                          .nx_secure_ec_named_curve;
        ULONG i;

        *type = TLS_KEY_EC;

        for (i = 0; i < (ULONG)(sizeof(tls_cert_curve_bits) /
                                sizeof(tls_cert_curve_bits[0])); i++)
        {
            if (tls_cert_curve_bits[i].cb_Curve == curve)
            {
                *bits = tls_cert_curve_bits[i].cb_Bits;
                return;
            }
        }

        /* An unlisted curve: the encoded point is still a size, and an
           approximate answer beats none. */
        {
            ULONG length = (ULONG)cert->nx_secure_x509_public_key.ec_public_key
                               .nx_secure_ec_public_key_length;

            if (length > 1)
                *bits = ((length - 1UL) / 2UL) * 8UL;
        }
    }
#endif /* NX_SECURE_ENABLE_ECC_CIPHERSUITE */
}

/*
 * SHA-256 over the certificate's DER, the whole Certificate and not the
 * TBSCertificate, which is what every tool that prints a fingerprint hashes.
 *
 * c68k_sha256 rather than nx_crypto's: it is the implementation this library
 * already links and already routes the handshake hash through (src/crypto68k),
 * its context is 108 bytes against nx_crypto's 360, and adding a second
 * SHA-256 to a library that has one is not a trade worth making for a call
 * that runs three times per handshake.
 */
static VOID tls_cert_fingerprint(NX_SECURE_X509_CERT *cert, UBYTE *out)
{
    C68K_SHA256 ctx;

    if (cert->nx_secure_x509_certificate_raw_data == NX_NULL ||
        cert->nx_secure_x509_certificate_raw_data_length == 0)
    {
        return;
    }

    if (c68k_sha256_initialize(&ctx, NX_CRYPTO_HASH_SHA256) != NX_CRYPTO_SUCCESS)
        return;

    (VOID)c68k_sha256_update(&ctx, cert->nx_secure_x509_certificate_raw_data,
                             (UINT)cert->nx_secure_x509_certificate_raw_data_length);
    (VOID)c68k_sha256_digest_calculate(&ctx, out, NX_CRYPTO_HASH_SHA256);
}

/* ------------------------------------------------------------ the arena --- */

/*
 * One block for every string in the chain.  tt_Base NULL means the measuring
 * pass: nothing is written, tt_Used still counts, and the number it ends on is
 * what to allocate.
 */
typedef struct TLSCertText
{
    char   *tt_Base;
    ULONG   tt_Size;
    ULONG   tt_Used;
} TLSCertText;

static char *tls_text_reserve(TLSCertText *t, ULONG length)
{
    char *p = NULL;

    if (t->tt_Base != NULL && (t->tt_Used + length + 1UL) <= t->tt_Size)
        p = &t->tt_Base[t->tt_Used];

    t->tt_Used += length + 1UL;

    return p;
}

static STRPTR tls_text_name(TLSCertText *t, const TLSCertAttr *attrs,
                            ULONG count)
{
    unsigned long length = tls_cert_name_format(NULL, 0, attrs, count);
    char         *p;

    if (length == 0)
        return NULL;

    p = tls_text_reserve(t, (ULONG)length);
    if (p != NULL)
        (VOID)tls_cert_name_format(p, (unsigned long)length + 1UL, attrs, count);

    return (STRPTR)p;
}

static STRPTR tls_text_alt_names(TLSCertText *t, NX_SECURE_X509_CERT *cert)
{
    ULONG length = tls_cert_alt_names(cert, NULL, 0);
    char *p;

    if (length == 0)
        return NULL;

    p = tls_text_reserve(t, length);
    if (p != NULL)
        (VOID)tls_cert_alt_names(cert, p, length + 1UL);

    return (STRPTR)p;
}

/*
 * One certificate.  Run twice: once against a measuring arena, to size the
 * block, and once against the real one.  Both passes execute the same code, so
 * the two lengths cannot drift; the first pass's structure is a scratch the
 * caller throws away.
 */
static VOID tls_cert_fill(struct TLSCertificate *out, NX_SECURE_X509_CERT *cert,
                          ULONG depth, TLSCertText *t)
{
    NX_SECURE_X509_DISTINGUISHED_NAME *subject = &cert->nx_secure_x509_distinguished_name;
    NX_SECURE_X509_DISTINGUISHED_NAME *issuer  = &cert->nx_secure_x509_issuer;
    TLSCertAttr                        attrs[3];

    tls_bzero(out, sizeof(*out));

    out->tc_Size  = sizeof(*out);
    out->tc_Depth = depth;

    attrs[0].ca_Label  = "CN";
    attrs[0].ca_Value  = subject->nx_secure_x509_common_name;
    attrs[0].ca_Length = subject->nx_secure_x509_common_name_length;
    attrs[1].ca_Label  = "O";
    attrs[1].ca_Value  = subject->nx_secure_x509_organization;
    attrs[1].ca_Length = subject->nx_secure_x509_organization_length;
    attrs[2].ca_Label  = "C";
    attrs[2].ca_Value  = subject->nx_secure_x509_country;
    attrs[2].ca_Length = subject->nx_secure_x509_country_length;

    out->tc_Subject = tls_text_name(t, attrs, 3);

    /* The common name on its own, which is what fits in a requester title. */
    out->tc_CommonName = tls_text_name(t, attrs, 1);

    attrs[0].ca_Value  = issuer->nx_secure_x509_common_name;
    attrs[0].ca_Length = issuer->nx_secure_x509_common_name_length;
    attrs[1].ca_Value  = issuer->nx_secure_x509_organization;
    attrs[1].ca_Length = issuer->nx_secure_x509_organization_length;
    attrs[2].ca_Value  = issuer->nx_secure_x509_country;
    attrs[2].ca_Length = issuer->nx_secure_x509_country_length;

    out->tc_Issuer   = tls_text_name(t, attrs, 3);
    out->tc_AltNames = tls_text_alt_names(t, cert);

    out->tc_NotBefore = tls_cert_asn1_time(
        cert->nx_secure_x509_not_before,
        cert->nx_secure_x509_not_before_length,
        cert->nx_secure_x509_not_before_validity_format ==
            NX_SECURE_ASN_TAG_GENERALIZED_TIME);

    out->tc_NotAfter = tls_cert_asn1_time(
        cert->nx_secure_x509_not_after,
        cert->nx_secure_x509_not_after_length,
        cert->nx_secure_x509_not_after_validity_format ==
            NX_SECURE_ASN_TAG_GENERALIZED_TIME);

    tls_cert_fingerprint(cert, out->tc_Fingerprint);
    tls_cert_key(cert, &out->tc_KeyType, &out->tc_KeyBits);

    out->tc_SelfSigned = tls_cert_self_signed(cert);
}

/* ----------------------------------------------------------- the capture --- */

VOID tls_cert_release(TLSConnection *conn)
{
    if (conn == NULL)
        return;

    tls_free(conn->tc_Certs);
    tls_free(conn->tc_CertText);

    conn->tc_Certs     = NULL;
    conn->tc_CertText  = NULL;
    conn->tc_CertCount = 0;
}

VOID tls_cert_capture(TLSConnection *conn,
                      NX_SECURE_X509_CERTIFICATE_STORE *store)
{
    NX_SECURE_X509_CERT   *chain[TLS_MAX_CHAIN];
    struct TLSCertificate  scratch;
    TLSCertText            text;
    ULONG                  count;
    ULONG                  bytes;
    ULONG                  i;

    /* Once per connection.  nx_secure calls the verification function once per
       handshake, and a renegotiation would otherwise leak the first set. */
    if (conn == NULL || store == NULL || conn->tc_Certs != NULL)
        return;

    count = tls_cert_chain(store, chain, (ULONG)TLS_MAX_CHAIN);
    if (count == 0)
        return;

    text.tt_Base = NULL;
    text.tt_Size = 0;
    text.tt_Used = 0;

    for (i = 0; i < count; i++)
        tls_cert_fill(&scratch, chain[i], i, &text);

    bytes = text.tt_Used;

    conn->tc_Certs = (struct TLSCertificate *)
                     tls_alloc(count * (ULONG)sizeof(struct TLSCertificate));
    if (conn->tc_Certs == NULL)
        return;

    conn->tc_CertText = (bytes > 0) ? (char *)tls_alloc(bytes) : NULL;

    /*
     * A text block that could not be allocated is not a failed handshake: the
     * second pass then runs in measuring mode again and every string comes out
     * NULL, which is what the public header says an unparseable field looks
     * like.  The dates, the key and the fingerprint still fill.
     */
    text.tt_Base = conn->tc_CertText;
    text.tt_Size = (conn->tc_CertText != NULL) ? bytes : 0;
    text.tt_Used = 0;

    for (i = 0; i < count; i++)
        tls_cert_fill(&conn->tc_Certs[i], chain[i], i, &text);

    conn->tc_CertCount  = count;
    conn->tc_ChainDepth = count;
}

/* ------------------------------------------------------------- the hook --- */

/*
 * The hook's entry point, called the way AmigaOS calls one: the hook in a0,
 * the object in a2, the message in a1, the answer in d0.
 *
 * Directly rather than through utility.library's CallHookPkt().  That is the
 * only call this library would want from utility.library, opening a library
 * for one function that is four instructions is not a trade, and the four
 * instructions are the documented ABI rather than an implementation detail
 * that could move.
 *
 * h_Entry is declared ULONG (*)() with no parameters, so it is cast through a
 * union: a plain cast between function types trips -Wcast-function-type, the
 * same treatment src/bsdsocket/netmonitor.c gives its hooks.
 */
static ULONG tls_hook_enter(struct Hook *hook, struct TLSConnection *object,
                            struct TLSVerifyMsg *message)
{
    union
    {
        ULONG (*he_Fn)(void);
        APTR    he_Raw;
    } entry;

    entry.he_Fn = hook->h_Entry;

    {
        /* Declared last and assigned here, with nothing between these and the
           asm: a local register variable is only guaranteed to hold its value
           across the statement that sets it and the asm that reads it. */
        register APTR  a0     __asm("a0") = (APTR)hook;
        register APTR  a1     __asm("a1") = (APTR)message;
        register APTR  a2     __asm("a2") = (APTR)object;
        register APTR  target __asm("a3") = entry.he_Raw;
        register ULONG res    __asm("d0");

        __asm __volatile ("jsr a3@"
                          : "=r" (res), "+r" (a0), "+r" (a1), "+r" (a2),
                            "+r" (target)
                          :
                          : "d1", "cc", "memory");

        return res;
    }
}

/*
 * Ask the caller.  TRUE goes on with the connection, FALSE fails TLSOpen()
 * with TLS_ERR_REFUSED.
 *
 * The ThreadX baton goes back for the duration.  A hook that puts up a
 * requester holds this call for as long as somebody takes to read it, and the
 * baton is what the IP thread and both SANA-II readers need to run: without
 * this, thirty seconds of a person reading is thirty seconds with the machine's
 * networking stopped, including the other connections of whatever is asking.
 * The trust store's disk read does the same thing for the same reason, see
 * tls_store_fetch() in src/tlslib/tls_store.c.
 *
 * The hook's own time is therefore not inside a NetX wait and is not charged
 * against TLSA_Timeout, which measures the socket waits either side of it.
 */
static BOOL tls_hook_ask(TLSConnection *conn, LONG reason)
{
    const AmiNetXDuoContext *ctx = tls_netx_ctx();
    struct TLSVerifyMsg      msg;
    ULONG                    answer;

    tls_bzero(&msg, sizeof(msg));

    msg.tv_Size       = sizeof(msg);
    msg.tv_HostName   = (conn->tc_HostNameLength > 0)
                        ? (STRPTR)conn->tc_HostName : NULL;
    msg.tv_Reason     = reason;
    msg.tv_ChainDepth = conn->tc_CertCount;
    msg.tv_Chain      = conn->tc_Certs;
    msg.tv_Leaf       = (conn->tc_CertCount > 0) ? &conn->tc_Certs[0] : NULL;

    if (ctx != NULL)
        ctx->nxc_BatonRelease();

    answer = tls_hook_enter(conn->tc_Hook, conn, &msg);

    if (ctx != NULL)
        ctx->nxc_BatonAcquire();

    return (BOOL)((answer != 0) ? TRUE : FALSE);
}

/* --------------------------------------------------------- the verdict --- */

/*
 * What the chain check concluded, in the vocabulary struct TLSVerifyMsg uses.
 *
 * Deliberately narrower than tls_error_from_nx(): tv_Reason is documented as
 * one of four values, and a hook written against that documentation switches
 * on four cases.  Anything the verification can return that is not an expiry
 * is a chain that did not reach a trusted root, which is what UNTRUSTED means.
 */
static LONG tls_cert_reason(UINT status)
{
    switch (status)
    {
    case NX_SUCCESS:
        return TLS_OK;

    case NX_SECURE_X509_CERTIFICATE_EXPIRED:
    case NX_SECURE_X509_CERTIFICATE_NOT_YET_VALID:
        return TLS_ERR_EXPIRED;

    case NX_SECURE_X509_CERTIFICATE_DNS_MISMATCH:
        return TLS_ERR_HOSTNAME;

    default:
        return TLS_ERR_UNTRUSTED;
    }
}

UINT tls_cert_verdict(TLSConnection *conn,
                      NX_SECURE_X509_CERTIFICATE_STORE *store,
                      NX_SECURE_X509_CERT *leaf, UINT status)
{
    LONG reason;

    if (conn == NULL)
        return status;

    /*
     * Once per handshake, which is the contract the public header states.
     * nx_secure has one call site for the verification function, so this only
     * fires on a renegotiation, and a second requester for a peer the caller
     * has already accepted is not something to leave to the vendored code's
     * structure staying as it is.
     */
    if ((conn->tc_Flags & TLSF_HOOKED) != 0)
        return NX_SUCCESS;

    /* Whatever the verdict.  TLSInfo() answers "who was I talking to" on a
       connection that failed as readily as on one that did not, and a hook
       cannot be shown a certificate that was never formatted. */
    tls_cert_capture(conn, store);

    reason = tls_cert_reason(status);

    /*
     * The host-name check, here rather than only in tls_certificate_callback().
     *
     * nx_secure runs the two in a fixed order: this function first, the
     * application's certificate callback second (see
     * _nx_secure_tls_remote_certificate_verify()).  A hook asked here would
     * therefore be told TLS_OK about a certificate issued to somebody else, and
     * TLS_ERR_HOSTNAME is one of the four answers it exists to be given.  So
     * the check is made now, and tls_certificate_callback() stands down when
     * the caller has already answered.
     *
     * Only when there is a hook.  Without one this is the same check twice for
     * a result nothing reads, and the no-hook path is meant to cost what it
     * cost before this file existed.
     */
    if (conn->tc_Hook != NULL && reason == TLS_OK && leaf != NX_NULL &&
        (conn->tc_Flags & TLSF_VERIFY) != 0)
    {
        if (conn->tc_HostNameLength == 0 ||
            _nx_secure_x509_common_name_dns_check(leaf, conn->tc_HostName,
                                                  conn->tc_HostNameLength)
                != NX_SUCCESS)
        {
            reason = TLS_ERR_HOSTNAME;
        }
    }

    conn->tc_VerifyReason = reason;

    if (conn->tc_Hook == NULL)
        return status;

    if (!tls_hook_ask(conn, reason))
    {
        conn->tc_Flags |= TLSF_REFUSED;

        /*
         * Any failing status stops the handshake; this one is chosen because
         * it is a chain-verification failure, which is what this function is.
         * TLSOpen() reports TLS_ERR_REFUSED off TLSF_REFUSED rather than off
         * the status, so the caller can tell "the user said no" from "it did
         * not work".
         */
        return NX_SECURE_X509_CHAIN_VERIFY_FAILURE;
    }

    /* The caller has spoken.  The name check below this must not overrule it,
       and neither must a chain that did not verify. */
    conn->tc_Flags |= TLSF_HOOKED;

    return NX_SUCCESS;
}

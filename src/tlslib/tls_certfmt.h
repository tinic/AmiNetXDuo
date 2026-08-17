/*
 * Turning the parts of a certificate into something a person can be shown.
 *
 * Split out of tls_cert.c for the reason tls_expiry.c is split out of
 * tls_resume.c: the three things here are decisions about bytes and nothing
 * else, so they can be asked without an Amiga, without nx_secure and without a
 * handshake.  See src/tlslib/test/test_tls_certfmt.c.
 *
 * Nothing here allocates.  Every function writes into a buffer the caller
 * supplies and returns the length it wanted, and a NULL buffer measures rather
 * than writes, which is what lets tls_cert.c size one allocation for a whole
 * chain in a first pass and fill it in a second.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_TLS_CERTFMT_H
#define AMINETXDUO_TLS_CERTFMT_H

/*
 * One attribute of a distinguished name, as the certificate carries it: bytes
 * and a length, not a C string.  nx_secure keeps pointers into the DER, so
 * there is no NUL anywhere in a parsed name.
 */
typedef struct TLSCertAttr
{
    const char          *ca_Label;      /* "CN", "O", "C"                     */
    const unsigned char *ca_Value;
    unsigned long        ca_Length;
} TLSCertAttr;

/*
 * "CN=example.com, O=Example Ltd, C=GB", in the order given, skipping the
 * attributes the certificate does not carry.  Returns the length it wanted,
 * not counting the terminator, so a measuring pass with dst == NULL says how
 * much to allocate.  Zero means the name had nothing in it worth showing.
 *
 * A value is copied one byte at a time rather than memcpy()d because it comes
 * off the wire: a control character in a subject ends up in a requester
 * title, and an embedded NUL would truncate the string at a place the
 * certificate chose.  Both become '?'.
 */
unsigned long tls_cert_name_format(char *dst, unsigned long size,
                                   const TLSCertAttr *attrs,
                                   unsigned long count);

/*
 * Append one name to a comma-separated list, `used` bytes long so far, and
 * return the new length.  The separator goes in only when something is already
 * there, so the caller does not have to know whether it is on the first one.
 * Same measuring rule: dst == NULL counts.
 */
unsigned long tls_cert_list_append(char *dst, unsigned long size,
                                   unsigned long used,
                                   const unsigned char *value,
                                   unsigned long length);

/*
 * An ASN.1 UTCTime or GeneralizedTime as X.509 carries it, without its tag and
 * length, converted to seconds since 1970.  Zero when it will not parse, which
 * is what struct TLSCertificate documents for a date it could not read.
 *
 *   UTCTime          YYMMDDhhmm[ss]Z,   YY < 50 is 20YY (RFC 5280 4.1.2.5.1)
 *   GeneralizedTime  YYYYMMDDhhmm[ss]Z
 *
 * Only Zulu is accepted.  RFC 5280 requires it, so a certificate with a local
 * offset is malformed, and reporting a time that is out by hours is worse than
 * reporting that the field would not parse.  nx_secure's own converter reads
 * the offset and then ignores it; this does not follow it there.
 */
unsigned long tls_cert_asn1_time(const unsigned char *asn1,
                                 unsigned long length, int generalized);

#endif /* AMINETXDUO_TLS_CERTFMT_H */

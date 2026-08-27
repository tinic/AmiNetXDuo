/*
 * RFC 7301, Application-Layer Protocol Negotiation, for nx_secure.
 *
 * nx_secure has no ALPN of any kind, which is why nothing built on it can
 * speak HTTP/2: h2 over TLS is defined only over a negotiated "h2", never by
 * assumption (RFC 7540 3.2).  This file is the whole of the mechanism.  The
 * seven call sites in third_party/netxduo/nx_secure are one line each, on
 * purpose: it keeps a submodule bump a merge of seven single lines rather than
 * of a feature.
 *
 * It is built into the nx_secure archive alongside the vendored sources, the
 * same way src/tls/rfc7905/ supplies the two ChaCha20-Poly1305 record files.
 *
 * THE WIRE SHAPE, both directions:
 *
 *   ext_type (2) | ext_length (2) | list_length (2) | ProtocolName...
 *   ProtocolName = length (1) | bytes
 *
 * The client sends its whole preference list.  The server answers with a list
 * of exactly one, which is the selection (RFC 7301 3.1); in TLS 1.2 that rides
 * in the ServerHello and in TLS 1.3 in EncryptedExtensions (RFC 8446 4.2).
 *
 * A server that does not answer is a server that did not negotiate, and that
 * is NOT an error: the connection continues with no application protocol
 * agreed, which for HTTP means HTTP/1.1 by the pre-ALPN default.  A server
 * that answers with something that was never offered IS an error, and RFC 7301
 * 3.2 says it is a fatal no_application_protocol.
 *
 * SPDX-License-Identifier: MIT
 */

#define NX_SECURE_SOURCE_CODE

#include "nx_secure_tls.h"

/*
 * Walk a ProtocolNameList and answer whether it is well formed: a run of
 * length-prefixed names that ends exactly on `length`, with no empty name
 * (RFC 7301 3.1 makes ProtocolName opaque<1..2^8-1>).
 */
static UINT nx_secure_tls_alpn_list_valid(const UCHAR *list, UINT length)
{
UINT offset = 0;

    if (list == NX_NULL)
    {
        return(NX_FALSE);
    }

    while (offset < length)
    {
    UINT name_length = list[offset];

        if (name_length == 0)
        {
            return(NX_FALSE);
        }

        offset += 1u + name_length;
    }

    return((offset == length) ? NX_TRUE : NX_FALSE);
}

/* TRUE when `name` appears in the wire-encoded `list`. */
static UINT nx_secure_tls_alpn_list_has(const UCHAR *list, UINT list_length,
                                        const UCHAR *name, UINT name_length)
{
UINT offset = 0;

    while ((offset + 1u + name_length) <= list_length)
    {
    UINT entry_length = list[offset];

        if ((entry_length == name_length) &&
            (NX_SECURE_MEMCMP(&list[offset + 1], name, name_length) == 0))
        {
            return(NX_TRUE);
        }

        offset += 1u + entry_length;
    }

    return(NX_FALSE);
}

/**************************************************************************/
/*  _nx_secure_tls_alpn_protocol_set                                      */
/*                                                                        */
/*  Offer `protocol_list`, the wire encoding of ProtocolNameList without  */
/*  its outer length: "\x02h2\x08http/1.1".  The caller keeps ownership   */
/*  and the buffer must outlive the handshake.  A NULL list clears the    */
/*  offer, and the extension is then not sent at all.                     */
/**************************************************************************/
UINT _nx_secure_tls_alpn_protocol_set(NX_SECURE_TLS_SESSION *tls_session,
                                      const UCHAR *protocol_list,
                                      USHORT protocol_list_length)
{
    if (tls_session == NX_NULL)
    {
        return(NX_PTR_ERROR);
    }

    if ((protocol_list == NX_NULL) || (protocol_list_length == 0))
    {
        tls_session -> nx_secure_tls_alpn_protocol_list = NX_NULL;
        tls_session -> nx_secure_tls_alpn_protocol_list_length = 0;
        return(NX_SUCCESS);
    }

    /* Checked here rather than on the wire: a malformed list written into a
       ClientHello is a malformed ClientHello, and the server's answer to that
       is a handshake failure with no explanation. */
    if (!nx_secure_tls_alpn_list_valid(protocol_list, protocol_list_length))
    {
        return(NX_SECURE_TLS_INVALID_PACKET);
    }

    tls_session -> nx_secure_tls_alpn_protocol_list = protocol_list;
    tls_session -> nx_secure_tls_alpn_protocol_list_length = protocol_list_length;

    return(NX_SUCCESS);
}

/**************************************************************************/
/*  _nx_secure_tls_alpn_protocol_get                                      */
/*                                                                        */
/*  What was negotiated, or NX_SECURE_TLS_EXTENSION_NOT_FOUND when the    */
/*  peer did not answer.  Not NUL terminated: ProtocolName is opaque.     */
/**************************************************************************/
UINT _nx_secure_tls_alpn_protocol_get(NX_SECURE_TLS_SESSION *tls_session,
                                      const UCHAR **protocol,
                                      UCHAR *protocol_length)
{
    if ((tls_session == NX_NULL) || (protocol == NX_NULL) ||
        (protocol_length == NX_NULL))
    {
        return(NX_PTR_ERROR);
    }

    if (tls_session -> nx_secure_tls_alpn_selected_length == 0)
    {
        *protocol = NX_NULL;
        *protocol_length = 0;
        return(NX_SECURE_TLS_EXTENSION_NOT_FOUND);
    }

    *protocol = tls_session -> nx_secure_tls_alpn_selected;
    *protocol_length = tls_session -> nx_secure_tls_alpn_selected_length;

    return(NX_SUCCESS);
}

/**************************************************************************/
/*  _nx_secure_tls_alpn_send_extension                                    */
/*                                                                        */
/*  Write the extension at *packet_offset.  On a client that is the whole */
/*  offer; on a server it is the one selected name, and nothing at all if */
/*  no name was selected.  *extension_length is zero when nothing was     */
/*  written, which is what the callers add to their running total.        */
/**************************************************************************/
UINT _nx_secure_tls_alpn_send_extension(NX_SECURE_TLS_SESSION *tls_session,
                                        UCHAR *packet_buffer, ULONG *packet_offset,
                                        USHORT *extension_length,
                                        ULONG available_size, UINT server)
{
ULONG        offset;
const UCHAR *list;
UINT         list_length;
UCHAR        one[NX_SECURE_TLS_ALPN_PROTOCOL_MAX + 1];

    if ((tls_session == NX_NULL) || (packet_buffer == NX_NULL) ||
        (packet_offset == NX_NULL) || (extension_length == NX_NULL))
    {
        return(NX_PTR_ERROR);
    }

    *extension_length = 0;

    if (server)
    {
        if (tls_session -> nx_secure_tls_alpn_selected_length == 0)
        {

            /* Nothing was selected, so nothing is answered.  RFC 7301 3.2
               allows the server to stay silent, and the connection then has no
               agreed application protocol. */
            return(NX_SUCCESS);
        }

        one[0] = tls_session -> nx_secure_tls_alpn_selected_length;
        NX_SECURE_MEMCPY(&one[1], tls_session -> nx_secure_tls_alpn_selected,
                         tls_session -> nx_secure_tls_alpn_selected_length); /* Use case of memcpy is verified. */

        list = one;
        list_length = 1u + tls_session -> nx_secure_tls_alpn_selected_length;
    }
    else
    {
        if (tls_session -> nx_secure_tls_alpn_protocol_list_length == 0)
        {
            return(NX_SUCCESS);
        }

        list = tls_session -> nx_secure_tls_alpn_protocol_list;
        list_length = tls_session -> nx_secure_tls_alpn_protocol_list_length;
    }

    offset = *packet_offset;

    /* ext_type, ext_length, list_length, then the names. */
    if (available_size < (offset + 6u + list_length))
    {
        return(NX_SECURE_TLS_PACKET_BUFFER_TOO_SMALL);
    }

    packet_buffer[offset]     = (UCHAR)((NX_SECURE_TLS_EXTENSION_ALPN & 0xFF00) >> 8);
    packet_buffer[offset + 1] = (UCHAR)(NX_SECURE_TLS_EXTENSION_ALPN & 0x00FF);
    offset += 2;

    packet_buffer[offset]     = (UCHAR)(((list_length + 2u) & 0xFF00) >> 8);
    packet_buffer[offset + 1] = (UCHAR)((list_length + 2u) & 0x00FF);
    offset += 2;

    packet_buffer[offset]     = (UCHAR)((list_length & 0xFF00) >> 8);
    packet_buffer[offset + 1] = (UCHAR)(list_length & 0x00FF);
    offset += 2;

    NX_SECURE_MEMCPY(&packet_buffer[offset], list, list_length); /* Use case of memcpy is verified. */
    offset += list_length;

    *extension_length = (USHORT)(offset - *packet_offset);
    *packet_offset = offset;

    return(NX_SUCCESS);
}

/**************************************************************************/
/*  _nx_secure_tls_alpn_process_response                                  */
/*                                                                        */
/*  The client half.  `packet_buffer` starts at the extension's own       */
/*  two-byte length field and `message_length` is what remains of the     */
/*  message, so this validates its own bounds.                            */
/**************************************************************************/
UINT _nx_secure_tls_alpn_process_response(NX_SECURE_TLS_SESSION *tls_session,
                                          const UCHAR *packet_buffer,
                                          UINT message_length)
{
UINT ext_length;
UINT list_length;
UINT name_length;

    if ((tls_session == NX_NULL) || (packet_buffer == NX_NULL))
    {
        return(NX_PTR_ERROR);
    }

    if (message_length < 2u)
    {
        return(NX_SECURE_TLS_INCORRECT_MESSAGE_LENGTH);
    }

    ext_length = (UINT)((packet_buffer[0] << 8) + packet_buffer[1]);
    if ((ext_length + 2u) > message_length)
    {
        return(NX_SECURE_TLS_INCORRECT_MESSAGE_LENGTH);
    }

    /* list_length (2) + name_length (1) + at least one byte of name. */
    if (ext_length < 4u)
    {
        return(NX_SECURE_TLS_INCORRECT_MESSAGE_LENGTH);
    }

    list_length = (UINT)((packet_buffer[2] << 8) + packet_buffer[3]);
    if (list_length != (ext_length - 2u))
    {
        return(NX_SECURE_TLS_INCORRECT_MESSAGE_LENGTH);
    }

    name_length = packet_buffer[4];
    if ((name_length == 0) || ((name_length + 1u) != list_length))
    {

        /* RFC 7301 3.1: the server's list is exactly one name.  Two names is
           not a longer answer, it is a message this code cannot act on. */
        return(NX_SECURE_TLS_INCORRECT_MESSAGE_LENGTH);
    }

    if (name_length > NX_SECURE_TLS_ALPN_PROTOCOL_MAX)
    {
        return(NX_SECURE_TLS_ALPN_PROTOCOL_MISMATCH);
    }

    /*
     * RFC 7301 3.2, and this is the check that matters.  A server that selects
     * a protocol the client never offered has answered a question that was not
     * asked, and a client that accepts it then speaks a protocol it may not
     * implement over an authenticated channel.
     */
    if (!nx_secure_tls_alpn_list_has(tls_session -> nx_secure_tls_alpn_protocol_list,
                                     tls_session -> nx_secure_tls_alpn_protocol_list_length,
                                     &packet_buffer[5], name_length))
    {
        return(NX_SECURE_TLS_ALPN_PROTOCOL_MISMATCH);
    }

    NX_SECURE_MEMCPY(tls_session -> nx_secure_tls_alpn_selected,
                     &packet_buffer[5], name_length); /* Use case of memcpy is verified. */
    tls_session -> nx_secure_tls_alpn_selected_length = (UCHAR)name_length;

    return(NX_SUCCESS);
}

/**************************************************************************/
/*  _nx_secure_tls_alpn_process_offer                                     */
/*                                                                        */
/*  The server half.  Selects the FIRST of our own protocols that the     */
/*  client also offered, so the preference is the server's (RFC 7301 3.2  */
/*  leaves the choice to the server and warns against following the       */
/*  client's order).  No overlap selects nothing and is not an error       */
/*  here: the caller decides between a silent answer and an alert.        */
/**************************************************************************/
UINT _nx_secure_tls_alpn_process_offer(NX_SECURE_TLS_SESSION *tls_session,
                                       const UCHAR *packet_buffer,
                                       UINT message_length)
{
UINT         ext_length;
UINT         list_length;
UINT         offset;
const UCHAR *ours;
UINT         ours_length;

    if ((tls_session == NX_NULL) || (packet_buffer == NX_NULL))
    {
        return(NX_PTR_ERROR);
    }

    tls_session -> nx_secure_tls_alpn_selected_length = 0;

    ours = tls_session -> nx_secure_tls_alpn_protocol_list;
    ours_length = tls_session -> nx_secure_tls_alpn_protocol_list_length;

    if (message_length < 2u)
    {
        return(NX_SECURE_TLS_INCORRECT_MESSAGE_LENGTH);
    }

    ext_length = (UINT)((packet_buffer[0] << 8) + packet_buffer[1]);
    if (((ext_length + 2u) > message_length) || (ext_length < 4u))
    {
        return(NX_SECURE_TLS_INCORRECT_MESSAGE_LENGTH);
    }

    list_length = (UINT)((packet_buffer[2] << 8) + packet_buffer[3]);
    if (list_length != (ext_length - 2u))
    {
        return(NX_SECURE_TLS_INCORRECT_MESSAGE_LENGTH);
    }

    if (!nx_secure_tls_alpn_list_valid(&packet_buffer[4], list_length))
    {
        return(NX_SECURE_TLS_INCORRECT_MESSAGE_LENGTH);
    }

    if ((ours == NX_NULL) || (ours_length == 0))
    {
        return(NX_SUCCESS);
    }

    for (offset = 0; offset < ours_length; offset += 1u + ours[offset])
    {
    UINT name_length = ours[offset];

        if (name_length > NX_SECURE_TLS_ALPN_PROTOCOL_MAX)
        {
            continue;
        }

        if (nx_secure_tls_alpn_list_has(&packet_buffer[4], list_length,
                                        &ours[offset + 1], name_length))
        {
            NX_SECURE_MEMCPY(tls_session -> nx_secure_tls_alpn_selected,
                             &ours[offset + 1], name_length); /* Use case of memcpy is verified. */
            tls_session -> nx_secure_tls_alpn_selected_length = (UCHAR)name_length;
            break;
        }
    }

    return(NX_SUCCESS);
}

/*
 * httphead, the request head.  See httphead.h for why this is its own file.
 *
 * Includes only its portable siblings, deliberately: this translation unit is
 * compiled once for m68k as part of the server and once natively by
 * src/tools/test/test_httphead.c.
 *
 * SPDX-License-Identifier: MIT
 */

#include "httphead.h"
#include "httprequest.h"

/* ------------------------------------------------------------- the small --- */

static unsigned long hh_len(const char *s)
{
    unsigned long n = 0;

    while (s[n] != '\0')
        n++;

    return n;
}

/* Case-insensitive compare of `n` characters, so a header name matches
   however the client capitalised it. */
static int hh_nicmp(const char *a, const char *b, unsigned long n)
{
    unsigned long i;

    for (i = 0; i < n; i++)
    {
        int ca = (unsigned char)a[i];
        int cb = (unsigned char)b[i];

        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;

        if (ca != cb || ca == 0)
            return ca - cb;
    }

    return 0;
}

static int hh_equal(const char *a, const char *b)
{
    unsigned long n = hh_len(b);

    return (hh_len(a) == n && hh_nicmp(a, b, n) == 0) ? 1 : 0;
}

static void hh_copy(char *dst, unsigned long dstlen, const char *src)
{
    unsigned long n = 0;

    if (dstlen == 0UL)
        return;

    while (src[n] != '\0' && n + 1UL < dstlen)
    {
        dst[n] = src[n];
        n++;
    }

    dst[n] = '\0';
}

/* ------------------------------------------------------------- the reset --- */

void http_head_reset(HttpHead *h)
{
    h->method[0] = '\0';
    h->has_range = 0;
    /* RFC 4918 makes an absent Depth mean infinity, which is refused here, so
       an absent header gets 1: the collection itself. */
    h->depth     = 1;
    h->body_left = 0;

    h->expect      = 0;
    h->overwrite   = 1;             /* Overwrite defaults to T, RFC 4918 10.6 */
    h->gzip_ok     = 0;
    http_chunk_off(&h->chunk);
    h->lock_secs   = 0;
    h->host[0]     = '\0';
    h->ifmatch[0]  = '\0';
    h->ifnone[0]   = '\0';
    h->dest_url[0] = '\0';
    h->iftoken[0][0] = '\0';
    h->iftoken[1][0] = '\0';
    h->ifhdr[0]    = '\0';
    h->unlock_token[0] = '\0';

    h->ws_upgrade    = 0;
    h->ws_connection = 0;
    h->ws_version    = 0;
    h->ws_key[0]     = '\0';

    h->reason = 0;
    h->note   = 0;
}

/* ------------------------------------------------------------ the parsing --- */

static unsigned long hh_refuse(HttpHead *h, unsigned long status,
                               const char *reason)
{
    h->reason = reason;
    return status;
}

/* "bytes=0-1023", "bytes=1024-", "bytes=-512".  One range only: a multipart
   answer is a different framing and no client needs it to open a file. */
static int hh_parse_range(HttpHead *h, const char *value)
{
    unsigned long from;
    unsigned long to;

    /* Suffix and multipart ranges are deliberately ignored: sending the whole
       representation is legal.  Reinterpreting malformed text as a different
       single range is not. */
    if (!http_frame_range(value, &from, &to))
        return 0;

    h->has_range  = 1;
    h->range_from = from;
    h->range_to   = to;

    return 1;
}

/* Everything a header can say that this server acts on is picked out here,
   in one pass over the buffer. */
unsigned long http_head_parse(HttpHead *h, const unsigned char *in,
                              unsigned long headlen,
                              char *target, unsigned long target_max,
                              char *value, unsigned long value_max)
{
    unsigned long i = 0;
    unsigned long n = 0;
    unsigned long headers = 0;
    int seen_len = 0;
    int seen_te  = 0;
    int seen_close = 0;
    int seen_overwrite = 0;

    h->reason = 0;
    h->note   = 0;

    /* ---- the request line ------------------------------------------- */

    while (i < headlen && in[i] != ' ' && in[i] != '\r' && in[i] != '\n')
    {
        if (n + 1UL >= sizeof(h->method))
            return hh_refuse(h, 501, "that is not a method this server has");
        h->method[n++] = (char)in[i++];
    }
    h->method[n] = '\0';

    while (i < headlen && in[i] == ' ')
        i++;

    n = 0;
    while (i < headlen && in[i] != ' ' && in[i] != '\r' && in[i] != '\n')
    {
        if (n + 1UL >= target_max)
            return hh_refuse(h, 414, "that address is longer than this server "
                                     "will read");
        target[n++] = (char)in[i++];
    }
    target[n] = '\0';

    while (i < headlen && in[i] == ' ')
        i++;

    /* The version is part of the framing, not a prefix hint: a missing one or
       `HTTP/1.1anything` must not be accepted and kept alive. */
    {
        unsigned long    start = i;
        HttpFrameVersion version;

        while (i < headlen && in[i] != '\r' && in[i] != '\n')
            i++;

        version = http_frame_version((const char *)&in[start], i - start);
        if (version == HTTP_VERSION_BAD)
            return hh_refuse(h, 400, "that is not an HTTP version this server "
                                     "reads");

        h->http11 = (version == HTTP_VERSION_11) ? 1 : 0;
    }

    h->keepalive = h->http11;

    while (i < headlen && in[i] != '\n')
        i++;
    i++;

    if (h->method[0] == '\0' || target[0] == '\0')
        return hh_refuse(h, 400, "that is not a request line");

    /* ---- the headers ------------------------------------------------- */

    while (i < headlen)
    {
        char          name[40];
        unsigned long start = i;
        int           cut   = 0;    /* the value did not fit in value[]    */

        while (i < headlen && in[i] != '\n')
            i++;

        /* An empty line is the end of the head. */
        if (i == start || (i == start + 1UL && in[start] == '\r'))
        {
            i++;
            break;
        }

        if (++headers > (unsigned long)HTTPD_HEADERS_MAX)
            return hh_refuse(h, 431, "too many headers");

        {
            unsigned long j;
            unsigned long colon;

            if (!http_frame_field_name((const char *)&in[start], i - start,
                                       &colon))
                return hh_refuse(h, 400, "that is not an HTTP header");

            j = start;

            n = 0;
            while (j < start + colon)
            {
                if (n + 1UL < sizeof(name))
                    name[n++] = (char)in[j];
                j++;
            }
            name[n] = '\0';

            j++;                            /* the colon                  */
            while (j < i && (in[j] == ' ' || in[j] == '\t'))
                j++;

            n = 0;
            while (j < i && in[j] != '\r')
            {
                if (n + 1UL < value_max)
                    value[n++] = (char)in[j];
                else
                    cut = 1;
                j++;
            }
            value[n] = '\0';
        }

        i++;

        if (hh_equal(name, "Content-Length"))
        {
            unsigned long   len;
            HttpFrameResult bad = http_frame_length(value, &len);

            /* A length this server cannot read is not a length to guess at:
               whatever it gets wrong stays in the socket and is parsed as the
               next request. */
            if (cut || bad != HTTP_FRAME_OK)
            {
                h->note = cut ? "longer than this server reads"
                              : http_frame_error(bad);
                return hh_refuse(h, 400, "that is not a Content-Length");
            }

            /* RFC 7230 3.3.3: two of them that disagree is the same hazard as
               one that overflowed, and for the same reason. */
            if (seen_len && len != h->body_left)
                return hh_refuse(h, 400, "two Content-Lengths that disagree");

            h->body_left = len;
            seen_len     = 1;
        }
        else if (hh_equal(name, "Depth"))
        {
            /* RFC 4918 10.2 has three values and no others.  Anything else
               is refused rather than read as the nearest one. */
            if (hh_nicmp(value, "infinity", 8) == 0 && value[8] == '\0')
                h->depth = -1;
            else if (value[0] == '1' && value[1] == '\0')
                h->depth = 1;
            else if (value[0] == '0' && value[1] == '\0')
                h->depth = 0;
            else
                return hh_refuse(h, 400, "that is not a Depth this server has");
        }
        else if (hh_equal(name, "Connection"))
        {
            /* Connection is a comma-separated token list, and several field
               lines are one combined list: `close` wins wherever it appears
               and cannot be undone by a later `keep-alive` line. */
            if (http_frame_has_token(value, "close"))
            {
                seen_close = 1;
                h->keepalive = 0;
            }
            else if (!seen_close && http_frame_has_token(value, "keep-alive"))
                h->keepalive = 1;

            if (http_frame_has_token(value, "upgrade"))
                h->ws_connection = 1;
        }
        else if (hh_equal(name, "Upgrade"))
        {
            /* Upgrade is a protocol list.  `websocketX` is not websocket, and
               a second field line that offers something else does not erase a
               valid offer in the first. */
            if (!cut && http_frame_has_token(value, "websocket"))
                h->ws_upgrade = 1;
        }
        else if (hh_equal(name, "Sec-WebSocket-Key"))
        {
            /* A key that did not fit is not a shorter key.  Copied whole or
               left empty, and httpws.c refuses an empty one. */
            if (!cut && hh_len(value) + 1UL < sizeof(h->ws_key))
                hh_copy(h->ws_key, sizeof(h->ws_key), value);
        }
        else if (hh_equal(name, "Sec-WebSocket-Version"))
        {
            if (!cut && http_frame_has_token(value, "13"))
                h->ws_version = HTTPD_WS_VERSION;
        }
        else if (hh_equal(name, "Range"))
        {
            (void)hh_parse_range(h, value);
        }
        else if (hh_equal(name, "Transfer-Encoding"))
        {
            /* Finder does not know how long a file it is uploading is until
               it has sent it, so it chunks. */
            HttpFrameCoding te = http_frame_coding(value);

            /* Two of these is the same list written on two lines, and this
               server can apply one coding or none. */
            if (seen_te)
                return hh_refuse(h, 400, "two Transfer-Encodings");

            if (cut || te == HTTP_TE_UNSUPPORTED)
                return hh_refuse(h, 501, "that is not a transfer encoding "
                                         "this server can undo");

            if (te == HTTP_TE_CHUNKED)
                http_chunk_start(&h->chunk);
            else
                http_chunk_off(&h->chunk);

            seen_te = 1;
        }
        else if (hh_equal(name, "Expect"))
        {
            /* curl sends this on every PUT over a certain size and waits a
               second for the answer.  The Windows redirector waits too. */
            if (cut || !http_frame_token_is(value, "100-continue"))
                return hh_refuse(h, 417, "that is not an expectation this "
                                         "server can meet");

            h->expect = 1;
        }
        else if (hh_equal(name, "Accept-Encoding"))
        {
            /* Read for the terminal's and the console's pages and nothing
               else.  A cut list can have lost the coding that was refused,
               so it is read as no offer at all. */
            h->gzip_ok = (!cut && http_request_accepts_gzip(value)) ? 1 : 0;
        }
        else if (hh_equal(name, "If-None-Match"))
        {
            /* Repeated field lines are one comma-separated list.  Replacing
               the first with the second, or keeping only the prefix that fit,
               can erase the validator that says a write must not happen. */
            if (cut || !http_frame_list_add(h->ifnone, sizeof(h->ifnone),
                                            value))
                return hh_refuse(h, 431, "that If-None-Match list is longer "
                                         "than this server reads");
        }
        else if (hh_equal(name, "If-Match"))
        {
            if (cut || !http_frame_list_add(h->ifmatch, sizeof(h->ifmatch),
                                            value))
                return hh_refuse(h, 431, "that If-Match list is longer than "
                                         "this server reads");
        }
        else if (hh_equal(name, "Host"))
        {
            /* Read for one thing only: telling a Destination that names this
               server from one that names another.  Nothing here is
               virtual-hosted, there is one document root. */
            hh_copy(h->host, sizeof(h->host), value);
        }
        else if (hh_equal(name, "Destination"))
        {
            /* A truncated Destination still resolves, to a shorter path,
               and Overwrite defaults to T.  Nothing is guessed here. */
            if (cut || hh_len(value) + 1UL >= sizeof(h->dest_url))
                return hh_refuse(h, 414, "that destination is longer than "
                                         "this server will read");

            hh_copy(h->dest_url, sizeof(h->dest_url), value);
        }
        else if (hh_equal(name, "Overwrite"))
        {
            if (seen_overwrite)
                return hh_refuse(h, 400, "two Overwrite directives");

            if (http_frame_token_is(value, "t"))
                h->overwrite = 1;
            else if (http_frame_token_is(value, "f"))
                h->overwrite = 0;
            else
            {
                /* The default is T only when the field is absent.  Guessing T
                   for an invalid value turns a typo into deletion of the
                   destination the client meant to preserve. */
                return hh_refuse(h, 400, "Overwrite must be T or F");
            }

            seen_overwrite = 1;
        }
        else if (hh_equal(name, "If"))
        {
            /* Half an If: is not a weaker condition, it is a different one,
               and the half that survives the cut can be the one that says
               yes. */
            if (cut || hh_len(value) + 1UL >= sizeof(h->ifhdr))
                return hh_refuse(h, 431, "that If: is longer than this server "
                                         "will read");

            hh_copy(h->ifhdr, sizeof(h->ifhdr), value);
            (void)http_request_lock_tokens(
                value, &h->iftoken[0][0],
                (unsigned long)sizeof(h->iftoken[0]),
                (unsigned long)(sizeof(h->iftoken) / sizeof(h->iftoken[0])));
        }
        else if (hh_equal(name, "Lock-Token"))
        {
            const char *p = value;

            if (cut)
                return hh_refuse(h, 400, "that is not a lock token");

            while (*p != '\0' && *p != '<')
                p++;
            if (*p == '<')
                p++;

            hh_copy(h->unlock_token, sizeof(h->unlock_token), p);

            {
                unsigned long n2 = hh_len(h->unlock_token);

                while (n2 > 0UL && h->unlock_token[n2 - 1] != '>')
                    n2--;
                if (n2 > 0UL)
                    h->unlock_token[n2 - 1] = '\0';
            }
        }
        else if (hh_equal(name, "Timeout"))
        {
            h->lock_secs = http_request_timeout(value, HTTPD_LOCK_CAP);
        }
    }

    /* RFC 7230 3.3.3: both together is a request whose length two ends can
       read differently, which is the whole of request smuggling.  Refused
       rather than resolved: a proxy in front can disagree about precedence. */
    if (seen_te && seen_len)
        return hh_refuse(h, 400, "a body cannot have both a length and an "
                                 "encoding");

    return 0;
}

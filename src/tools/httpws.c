/*
 * httpws, RFC 6455 on the wire.  See httpws.h for what this is and what it
 * deliberately is not.
 *
 * SPDX-License-Identifier: MIT
 */

#include "httpws.h"

/* ---------------------------------------------------------------- base64 --- */

static const char ws_b64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

unsigned long http_ws_b64_encode(const unsigned char *src, unsigned long len,
                                 char *out, unsigned long outlen)
{
    unsigned long need = ((len + 2UL) / 3UL) * 4UL;
    unsigned long i    = 0;
    unsigned long n    = 0;

    if (out == 0 || outlen <= need)
        return 0;

    while (i < len)
    {
        unsigned long v    = (unsigned long)src[i] << 16;
        unsigned long have = 1;

        if (i + 1UL < len) { v |= (unsigned long)src[i + 1] << 8; have = 2; }
        if (i + 2UL < len) { v |= (unsigned long)src[i + 2];      have = 3; }

        out[n++] = ws_b64[(v >> 18) & 0x3f];
        out[n++] = ws_b64[(v >> 12) & 0x3f];
        out[n++] = (have > 1) ? ws_b64[(v >> 6) & 0x3f] : '=';
        out[n++] = (have > 2) ? ws_b64[v & 0x3f]        : '=';

        i += 3UL;
    }

    out[n] = '\0';
    return n;
}

static int ws_b64_value(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return (c - 'a') + 26;
    if (c >= '0' && c <= '9') return (c - '0') + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

long http_ws_b64_decode(const char *text, unsigned char *out,
                        unsigned long outlen)
{
    unsigned long acc  = 0;
    unsigned      bits = 0;
    unsigned long n    = 0;
    unsigned long pad  = 0;
    unsigned long got  = 0;      /* characters, padding included            */

    if (text == 0 || out == 0)
        return -1;

    while (*text != '\0')
    {
        char c = *text++;
        int  v;

        if (c == '=')
        {
            /* Padding is the end.  Anything after it that is not more
               padding is not base64, and a decoder that skipped it would
               accept two different texts for one value. */
            pad++;
            got++;
            if (pad > 2UL)
                return -1;
            continue;
        }

        if (pad > 0UL)
            return -1;

        v = ws_b64_value(c);
        if (v < 0)
            return -1;

        got++;
        acc = (acc << 6) | (unsigned long)v;
        bits += 6;

        if (bits >= 8)
        {
            bits -= 8;
            if (n >= outlen)
                return -1;
            out[n++] = (unsigned char)((acc >> bits) & 0xff);
        }
    }

    /* A base64 text is a whole number of quartets, and the bits left over
       from the last one must be zero.  "AAAAA" decodes to three bytes and a
       stray six bits under a lenient decoder, and 24 characters of it would
       then pass for a key. */
    if ((got % 4UL) != 0UL)
        return -1;
    if (bits > 0 && ((acc << (8 - bits)) & 0xff) != 0)
        return -1;

    return (long)n;
}

/* ------------------------------------------------------------ the accept --- */

/* RFC 6455 1.3.  A public constant, present so that a cache or a proxy cannot
   produce the accept by echoing the key. */
static const char ws_guid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

int http_ws_accept(const char *key, char *out, unsigned long outlen)
{
    unsigned char  concat[24 + 36];
    unsigned char  digest[20];
    unsigned char  raw[16];
    unsigned long  n = 0;
    unsigned long  i;

    if (key == 0 || out == 0 || outlen < 29UL)
        return 0;

    while (*key == ' ' || *key == '\t')
        key++;

    while (key[n] != '\0' && key[n] != ' ' && key[n] != '\t')
    {
        if (n >= 24UL)
            return 0;               /* longer than a key can be            */
        n++;
    }

    /* Trailing space is all that can follow. */
    for (i = n; key[i] != '\0'; i++)
    {
        if (key[i] != ' ' && key[i] != '\t')
            return 0;
    }

    if (n != 24UL)
        return 0;

    {
        char text[25];

        for (i = 0; i < 24UL; i++)
            text[i] = key[i];
        text[24] = '\0';

        if (http_ws_b64_decode(text, raw, sizeof(raw)) != 16L)
            return 0;

        for (i = 0; i < 24UL; i++)
            concat[i] = (unsigned char)text[i];
    }

    for (i = 0; i < 36UL; i++)
        concat[24 + i] = (unsigned char)ws_guid[i];

    http_ws_sha1(concat, sizeof(concat), digest);

    return (http_ws_b64_encode(digest, sizeof(digest), out, outlen) == 28UL)
               ? 1 : 0;
}

/* ------------------------------------------------------------ the decoder --- */

enum
{
    WS_HEAD = 0,        /* collecting the frame header                     */
    WS_PAYLOAD,         /* the payload of a data frame, streamed out       */
    WS_CONTROL,         /* the payload of a control frame, held whole      */
    WS_DEAD             /* the framing failed, nothing more is read        */
};

void http_ws_reset(HttpWsIn *in)
{
    unsigned i;

    in->state    = WS_HEAD;
    in->hdr_n    = 0;
    in->hdr_need = 2;
    in->opcode   = 0;
    in->fin      = 0;
    in->left     = 0;
    in->maskpos  = 0;
    in->msg      = HTTP_WS_EV_NONE;
    in->ctl_n    = 0;
    in->failed   = 0;

    for (i = 0; i < 4; i++)
        in->mask[i] = 0;
}

static void ws_fail(HttpWsIn *in, unsigned short code)
{
    in->failed = code;
    in->state  = WS_DEAD;
}

static HttpWsEvent ws_event_of(unsigned char opcode)
{
    switch (opcode)
    {
        case 0x1: return HTTP_WS_EV_TEXT;
        case 0x2: return HTTP_WS_EV_BINARY;
        case 0x8: return HTTP_WS_EV_CLOSE;
        case 0x9: return HTTP_WS_EV_PING;
        case 0xa: return HTTP_WS_EV_PONG;
        default:  return HTTP_WS_EV_NONE;
    }
}

/*
 * The header is complete.  Everything RFC 6455 says a server must refuse is
 * decided here, once, rather than being spread over the payload path where
 * half of it would be reachable only with the right split across reads.
 */
static void ws_header_done(HttpWsIn *in)
{
    unsigned char  b0     = in->hdr[0];
    unsigned char  b1     = in->hdr[1];
    unsigned char  len7   = (unsigned char)(b1 & 0x7f);
    unsigned long  extra  = 0;
    unsigned       i;

    in->fin    = (unsigned char)((b0 & 0x80) ? 1 : 0);
    in->opcode = (unsigned char)(b0 & 0x0f);

    /* No extension was negotiated, so a reserved bit that is set describes a
       payload transformation this server cannot undo. */
    if ((b0 & 0x70) != 0)
    {
        ws_fail(in, HTTP_WS_CLOSE_PROTOCOL);
        return;
    }

    if (ws_event_of(in->opcode) == HTTP_WS_EV_NONE && in->opcode != 0x0)
    {
        ws_fail(in, HTTP_WS_CLOSE_PROTOCOL);
        return;
    }

    /* RFC 6455 5.1.  A client frame that is not masked ends the connection.
       See the header for why this one is not negotiable. */
    if ((b1 & 0x80) == 0)
    {
        ws_fail(in, HTTP_WS_CLOSE_PROTOCOL);
        return;
    }

    if (len7 == 126)
    {
        extra = 2;
        in->left = ((unsigned long)in->hdr[2] << 8) | (unsigned long)in->hdr[3];
    }
    else if (len7 == 127)
    {
        extra = 8;

        /* The top bit of a 64-bit length must be zero, and the top four bytes
           are a length this machine cannot address at all.  Both are 1009
           rather than 1002, because the frame is well formed and too big. */
        if (in->hdr[2] != 0 || in->hdr[3] != 0 ||
            in->hdr[4] != 0 || in->hdr[5] != 0)
        {
            ws_fail(in, HTTP_WS_CLOSE_TOOBIG);
            return;
        }

        in->left = ((unsigned long)in->hdr[6] << 24) |
                   ((unsigned long)in->hdr[7] << 16) |
                   ((unsigned long)in->hdr[8] <<  8) |
                   ((unsigned long)in->hdr[9]);
    }
    else
    {
        in->left = len7;
    }

    for (i = 0; i < 4; i++)
        in->mask[i] = in->hdr[2 + extra + i];
    in->maskpos = 0;

    if ((in->opcode & 0x8) != 0)
    {
        /* RFC 6455 5.5: a control frame is never fragmented and never longer
           than 125 bytes.  Both are 1002. */
        if (in->fin == 0 || in->left > (unsigned long)HTTP_WS_CTL_MAX)
        {
            ws_fail(in, HTTP_WS_CLOSE_PROTOCOL);
            return;
        }

        in->ctl_n = 0;
        in->state = WS_CONTROL;
        return;
    }

    if (in->opcode == 0x0)
    {
        /* A continuation of nothing. */
        if (in->msg == HTTP_WS_EV_NONE)
        {
            ws_fail(in, HTTP_WS_CLOSE_PROTOCOL);
            return;
        }
    }
    else
    {
        /* A new data message while one is still open.  Fragmentation must not
           interleave data frames. */
        if (in->msg != HTTP_WS_EV_NONE)
        {
            ws_fail(in, HTTP_WS_CLOSE_PROTOCOL);
            return;
        }

        in->msg = (unsigned char)ws_event_of(in->opcode);
    }

    if (in->left > HTTP_WS_MSG_MAX)
    {
        ws_fail(in, HTTP_WS_CLOSE_TOOBIG);
        return;
    }

    in->state = WS_PAYLOAD;
}

/*
 * A frame whose payload is empty is complete the moment its header is, and
 * nothing after it is needed to say so.  Delivering it from inside the payload
 * loop held an empty close or ping at the end of a read until the client sent
 * something else, and after a close the client sends nothing.
 */
static void ws_finish_empty(HttpWsIn *in, HttpWsSink sink, void *ctx)
{
    if (in->state == WS_CONTROL && in->left == 0UL)
    {
        if (sink != 0)
            sink(ctx, ws_event_of(in->opcode), in->ctl, 0, 1);
        in->state = WS_HEAD;
        return;
    }

    if (in->state == WS_PAYLOAD && in->left == 0UL)
    {
        HttpWsEvent ev   = (HttpWsEvent)in->msg;
        int         last = (in->fin != 0) ? 1 : 0;

        if (sink != 0)
            sink(ctx, ev, in->ctl, 0, last);
        if (last)
            in->msg = HTTP_WS_EV_NONE;
        in->state = WS_HEAD;
    }
}

long http_ws_feed(HttpWsIn *in, const unsigned char *data, long len,
                  HttpWsSink sink, void *ctx)
{
    long used = 0;

    while (used < len && in->state != WS_DEAD)
    {
        if (in->state == WS_HEAD)
        {
            in->hdr[in->hdr_n++] = data[used++];

            if (in->hdr_n == 2)
            {
                unsigned char len7 = (unsigned char)(in->hdr[1] & 0x7f);

                /* Two bytes say how many more there are: the extended length
                   and then the mask, which is always present on a client
                   frame and whose absence is caught in ws_header_done(). */
                in->hdr_need = (unsigned char)(2 + ((len7 == 126) ? 2 :
                                                    (len7 == 127) ? 8 : 0) +
                                               (((in->hdr[1] & 0x80) != 0) ? 4
                                                                           : 0));
            }

            if (in->hdr_n >= in->hdr_need)
            {
                ws_header_done(in);
                in->hdr_n    = 0;
                in->hdr_need = 2;

                if (in->state != WS_DEAD)
                    ws_finish_empty(in, sink, ctx);
            }
            continue;
        }

        {
            long avail = len - used;
            long take  = ((unsigned long)avail > in->left) ? (long)in->left
                                                           : avail;
            long i;

            if (in->state == WS_CONTROL)
            {
                for (i = 0; i < take; i++)
                {
                    in->ctl[in->ctl_n + (unsigned char)i] =
                        (unsigned char)(data[used + i] ^
                                        in->mask[(in->maskpos + (unsigned char)i) & 3]);
                }
                in->ctl_n   = (unsigned char)(in->ctl_n + take);
                in->maskpos = (unsigned char)((in->maskpos + take) & 3);
                in->left   -= (unsigned long)take;
                used       += take;

                if (in->left == 0UL)
                {
                    HttpWsEvent ev = ws_event_of(in->opcode);

                    /* RFC 6455 5.5.1: a close payload is empty or is at least
                       a two-byte code.  One byte is not a code. */
                    if (ev == HTTP_WS_EV_CLOSE && in->ctl_n == 1)
                    {
                        ws_fail(in, HTTP_WS_CLOSE_PROTOCOL);
                        break;
                    }

                    if (sink != 0)
                        sink(ctx, ev, in->ctl, (long)in->ctl_n, 1);

                    in->state = WS_HEAD;
                }
                continue;
            }

            /* WS_PAYLOAD.  Unmasked in place into a small scratch and handed
               on as it arrives: a paste of a kilobyte must not have to fit
               anywhere before the terminal sees the first byte of it. */
            {
                unsigned char piece[256];

                if (take > (long)sizeof(piece))
                    take = (long)sizeof(piece);

                for (i = 0; i < take; i++)
                {
                    piece[i] = (unsigned char)(data[used + i] ^
                                    in->mask[(in->maskpos + (unsigned char)i) & 3]);
                }

                in->maskpos = (unsigned char)((in->maskpos + take) & 3);
                in->left   -= (unsigned long)take;
                used       += take;

                {
                    int         last = (in->left == 0UL && in->fin != 0) ? 1 : 0;
                    HttpWsEvent ev   = (HttpWsEvent)in->msg;

                    if (sink != 0 && (take > 0 || last))
                        sink(ctx, ev, piece, take, last);

                    if (in->left == 0UL)
                    {
                        if (last)
                            in->msg = HTTP_WS_EV_NONE;
                        in->state = WS_HEAD;
                    }
                }
            }
        }
    }

    return used;
}

const char *http_ws_close_reason(unsigned short code)
{
    switch (code)
    {
        case HTTP_WS_CLOSE_NORMAL:   return "the session ended";
        case HTTP_WS_CLOSE_GOING:    return "the server is going away";
        case HTTP_WS_CLOSE_PROTOCOL: return "that is not a frame this server "
                                            "can read";
        case HTTP_WS_CLOSE_TOOBIG:   return "that message is larger than this "
                                            "server will read";
        default:                     return "the connection ended";
    }
}

/* ------------------------------------------------------------ the encoder --- */

static unsigned char ws_opcode_of(HttpWsEvent ev)
{
    switch (ev)
    {
        case HTTP_WS_EV_TEXT:   return 0x1;
        case HTTP_WS_EV_BINARY: return 0x2;
        case HTTP_WS_EV_CLOSE:  return 0x8;
        case HTTP_WS_EV_PING:   return 0x9;
        case HTTP_WS_EV_PONG:   return 0xa;
        default:                return 0x2;
    }
}

unsigned long http_ws_head(unsigned char *out, unsigned long outlen,
                           HttpWsEvent ev, unsigned long len, int final)
{
    unsigned long n = 0;

    if (out == 0)
        return 0;

    if (outlen < 2UL)
        return 0;

    out[n++] = (unsigned char)((final ? 0x80 : 0x00) | ws_opcode_of(ev));

    if (len < 126UL)
    {
        out[n++] = (unsigned char)len;
    }
    else if (len < 65536UL)
    {
        if (outlen < 4UL)
            return 0;
        out[n++] = 126;
        out[n++] = (unsigned char)((len >> 8) & 0xff);
        out[n++] = (unsigned char)(len & 0xff);
    }
    else
    {
        if (outlen < 10UL)
            return 0;
        out[n++] = 127;
        out[n++] = 0;
        out[n++] = 0;
        out[n++] = 0;
        out[n++] = 0;
        out[n++] = (unsigned char)((len >> 24) & 0xff);
        out[n++] = (unsigned char)((len >> 16) & 0xff);
        out[n++] = (unsigned char)((len >>  8) & 0xff);
        out[n++] = (unsigned char)(len & 0xff);
    }

    return n;
}

unsigned long http_ws_close_frame(unsigned char *out, unsigned long outlen,
                                  unsigned short code, const char *reason)
{
    unsigned long body = 2;
    unsigned long head;
    unsigned long i;

    if (reason != 0)
    {
        for (i = 0; reason[i] != '\0'; i++)
            body++;
    }

    if (body > (unsigned long)HTTP_WS_CTL_MAX)
        body = (unsigned long)HTTP_WS_CTL_MAX;

    head = http_ws_head(out, outlen, HTTP_WS_EV_CLOSE, body, 1);
    if (head == 0UL || outlen < head + body)
        return 0;

    out[head]     = (unsigned char)((code >> 8) & 0xff);
    out[head + 1] = (unsigned char)(code & 0xff);

    for (i = 2; i < body; i++)
        out[head + i] = (unsigned char)reason[i - 2];

    return head + body;
}

/* --------------------------------------------------------- peer liveness --- */

/* Half the budget, rounded up, so a timeout of 1 still leaves a whole second
   to answer in rather than none. */
static unsigned long ws_half(unsigned long timeout)
{
    return (timeout + 1UL) / 2UL;
}

int http_ws_live_stale(unsigned long progress, int pinged,
                       unsigned long now, unsigned long timeout)
{
    if (timeout == 0UL || now < progress)
        return 0;

    return (pinged && now - progress >= ws_half(timeout)) ? 1 : 0;
}

int http_ws_live_ping_due(unsigned long progress, int pinged,
                          unsigned long now, unsigned long timeout)
{
    if (timeout == 0UL || pinged || now < progress)
        return 0;

    return (now - progress >= ws_half(timeout)) ? 1 : 0;
}

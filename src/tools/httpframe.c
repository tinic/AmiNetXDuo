/*
 * httpframe, how long a request body is, and where it ends.
 *
 * See httpframe.h for what this is separate for.  Nothing here calls anything:
 * no library, no allocation, no I/O.
 *
 * SPDX-License-Identifier: MIT
 */

#include "httpframe.h"

/* ------------------------------------------------------------ the length --- */

/*
 * 4294967295 is the largest a ULONG holds on the target.  The check is made
 * before the multiply rather than after it, because after it there is nothing
 * left to see: 4294967306 had already become 10.
 */
#define HF_ULONG_MAX    4294967295UL
#define HF_LIMIT        (HF_ULONG_MAX / 10UL)
#define HF_LAST         (HF_ULONG_MAX % 10UL)

HttpFrameResult http_frame_length(const char *value, unsigned long *out)
{
    unsigned long len = 0;
    int           digits = 0;

    if (out != 0)
        *out = 0;

    if (value == 0)
        return HTTP_FRAME_EMPTY;

    /* Leading space is what a header value has already had trimmed, so
       anything here is the value itself. */
    while (*value >= '0' && *value <= '9')
    {
        unsigned long d = (unsigned long)(*value - '0');

        if (len > HF_LIMIT || (len == HF_LIMIT && d > HF_LAST))
            return HTTP_FRAME_OVERFLOW;

        len = (len * 10UL) + d;
        digits++;
        value++;
    }

    if (digits == 0)
        return HTTP_FRAME_EMPTY;

    /* Trailing whitespace is allowed and anything else is not.  A value of
       "5abc" is a header this server does not understand, and a body it
       therefore cannot place. */
    while (*value == ' ' || *value == '\t')
        value++;

    if (*value != '\0')
        return HTTP_FRAME_JUNK;

    if (out != 0)
        *out = len;

    return HTTP_FRAME_OK;
}

const char *http_frame_error(HttpFrameResult why)
{
    switch (why)
    {
        case HTTP_FRAME_OK:       return "no fault";
        case HTTP_FRAME_EMPTY:    return "a length with no digits";
        case HTTP_FRAME_JUNK:     return "a length with something after it";
        case HTTP_FRAME_OVERFLOW: return "a length larger than this server counts";
    }

    return "refused";
}

/* ---------------------------------------------------------- the encoding --- */

static int hf_token_is(const char *start, const char *end, const char *want)
{
    while (start < end && *want != '\0')
    {
        int a = *start;
        int b = *want;

        if (a >= 'A' && a <= 'Z')
            a += 'a' - 'A';

        if (a != b)
            return 0;

        start++;
        want++;
    }

    return (start == end && *want == '\0') ? 1 : 0;
}

int http_frame_has_token(const char *value, const char *want)
{
    if (value == 0 || want == 0 || *want == '\0')
        return 0;

    for (;;)
    {
        const char *start;
        const char *end;

        while (*value == ' ' || *value == '\t' || *value == ',')
            value++;

        if (*value == '\0')
            return 0;

        start = value;
        while (*value != '\0' && *value != ',')
            value++;

        end = value;
        while (end > start && (end[-1] == ' ' || end[-1] == '\t'))
            end--;

        if (hf_token_is(start, end, want))
            return 1;
    }
}

static int hf_field_char(int ch)
{
    if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
        (ch >= '0' && ch <= '9'))
        return 1;

    switch (ch)
    {
        case '!': case '#': case '$': case '%': case '&': case '\'':
        case '*': case '+': case '-': case '.': case '^': case '_':
        case '`': case '|': case '~':
            return 1;
    }

    return 0;
}

int http_frame_field_name(const char *line, unsigned long len,
                          unsigned long *colon)
{
    unsigned long i;

    if (colon != 0)
        *colon = 0;

    if (line == 0 || len == 0)
        return 0;

    for (i = 0; i < len && line[i] != ':'; i++)
        if (!hf_field_char((unsigned char)line[i]))
            return 0;

    if (i == 0 || i >= len)
        return 0;

    if (colon != 0)
        *colon = i;

    return 1;
}

static int hf_is_weak(const char *start, const char *end)
{
    return (end - start >= 2 && (start[0] == 'W' || start[0] == 'w') &&
            start[1] == '/') ? 1 : 0;
}

int http_frame_etag_listed(const char *list, const char *etag, int weak)
{
    const char *etag_end;

    if (list == 0 || etag == 0 || *etag == '\0')
        return 0;

    etag_end = etag;
    while (*etag_end != '\0')
        etag_end++;

    if (weak && hf_is_weak(etag, etag_end))
        etag += 2;

    for (;;)
    {
        const char *start;
        const char *end;
        const char *a;
        const char *b;

        while (*list == ' ' || *list == '\t' || *list == ',')
            list++;

        if (*list == '\0')
            return 0;

        start = list;
        while (*list != '\0' && *list != ',')
            list++;

        end = list;
        while (end > start && (end[-1] == ' ' || end[-1] == '\t'))
            end--;

        if (hf_is_weak(start, end))
        {
            if (!weak)
                continue;
            start += 2;
        }

        a = start;
        b = etag;
        while (a < end && b < etag_end && *a == *b)
        {
            a++;
            b++;
        }

        if (a == end && b == etag_end)
            return 1;
    }
}

HttpFrameVersion http_frame_version(const char *value, unsigned long len)
{
    static const char prefix[] = "HTTP/1.";
    unsigned long     i;

    if (value == 0 || len != 8UL)
        return HTTP_VERSION_BAD;

    for (i = 0; i < 7UL; i++)
        if (value[i] != prefix[i])
            return HTTP_VERSION_BAD;

    if (value[7] == '0')
        return HTTP_VERSION_10;
    if (value[7] == '1')
        return HTTP_VERSION_11;

    return HTTP_VERSION_BAD;
}

HttpFrameCoding http_frame_coding(const char *value)
{
    HttpFrameCoding last = HTTP_TE_IDENTITY;
    int             seen = 0;

    if (value == 0)
        return HTTP_TE_IDENTITY;

    for (;;)
    {
        const char *start;
        const char *end;

        while (*value == ' ' || *value == '\t' || *value == ',')
            value++;

        if (*value == '\0')
            break;

        start = value;
        while (*value != '\0' && *value != ',')
            value++;

        end = value;
        while (end > start && (end[-1] == ' ' || end[-1] == '\t'))
            end--;

        /* A coding can carry parameters after a ';'.  None of the ones this
           server takes has any, so a token with one is not one of them. */
        if (hf_token_is(start, end, "chunked"))
            last = HTTP_TE_CHUNKED;
        else if (hf_token_is(start, end, "identity"))
            last = HTTP_TE_IDENTITY;
        else
            return HTTP_TE_UNSUPPORTED;

        seen++;
    }

    /* "chunked, chunked" is a list this server cannot apply twice, and
       "chunked, identity" puts the framing somewhere it is not.  Exactly one
       coding, or none. */
    if (seen > 1)
        return HTTP_TE_UNSUPPORTED;

    return last;
}

/* ------------------------------------------------------------- the chunks --- */

void http_chunk_start(HttpChunk *ch)
{
    ch->state   = (unsigned char)HTTP_CHUNK_SIZE;
    ch->n       = 0;
    ch->left    = 0;
    ch->total   = 0;
    ch->line[0] = '\0';
}

void http_chunk_off(HttpChunk *ch)
{
    ch->state   = (unsigned char)HTTP_CHUNK_OFF;
    ch->n       = 0;
    ch->left    = 0;
    ch->total   = 0;
    ch->line[0] = '\0';
}

/*
 * The size line that has just been collected, as a count.
 *
 * Hex, at most eight digits of it, then either the end of the line or the ';'
 * that starts a chunk extension.  Eight digits are the whole of a 32-bit
 * count, and a ninth used to shift the first one out, which is how "100000000"
 * became 0, was read as the terminator, and left the body to be parsed as the
 * next request.
 */
static int hf_chunk_size(const char *p, unsigned long *out)
{
    unsigned long size = 0;
    int           digits = 0;
    int           significant = 0;

    for (;;)
    {
        int d = *p;

        if      (d >= '0' && d <= '9') d -= '0';
        else if (d >= 'a' && d <= 'f') d -= 'a' - 10;
        else if (d >= 'A' && d <= 'F') d -= 'A' - 10;
        else                           break;

        /* Leading zeros are legal and cost nothing, so only the digits that
           carry a value are counted against the eight. */
        if (significant > 0 || d != 0)
            significant++;

        if (significant > 8)
            return 0;

        size = (size << 4) | (unsigned long)d;
        digits++;
        p++;
    }

    if (digits == 0)
        return 0;                   /* a line with no size in it            */

    /* Either the end of the line, or the ';' that starts a chunk extension.
       "5X" is a size this server cannot read, and a body it therefore cannot
       place. */
    if (*p != '\0' && *p != ';')
        return 0;

    *out = size;

    return 1;
}

long http_chunk_feed(HttpChunk *ch, const unsigned char *data, long len,
                     HttpChunkSink sink, void *ctx)
{
    long i = 0;

    while (i < len && ch->state != (unsigned char)HTTP_CHUNK_DONE &&
           ch->state != (unsigned char)HTTP_CHUNK_ERROR)
    {
        switch (ch->state)
        {
            case HTTP_CHUNK_SIZE:
            {
                int c = data[i++];

                if (c != '\n')
                {
                    if (c == '\r')
                        break;

                    /* A size line that does not fit is not truncated into one
                       that parses.  Nothing legitimate is near the buffer. */
                    if ((unsigned long)ch->n + 1UL >= sizeof(ch->line))
                    {
                        ch->state = (unsigned char)HTTP_CHUNK_ERROR;
                        break;
                    }

                    ch->line[ch->n++] = (char)c;
                    break;
                }

                ch->line[ch->n] = '\0';
                ch->n           = 0;

                if (ch->line[0] == '\0' ||
                    !hf_chunk_size(ch->line, &ch->left))
                {
                    ch->state = (unsigned char)HTTP_CHUNK_ERROR;
                    break;
                }

                ch->state = (unsigned char)((ch->left > 0UL)
                                ? HTTP_CHUNK_DATA : HTTP_CHUNK_TRAILER);
                break;
            }

            case HTTP_CHUNK_DATA:
            {
                unsigned long take = (unsigned long)(len - i);

                if (take > ch->left)
                    take = ch->left;

                if (sink != 0 && take > 0UL)
                    sink(ctx, &data[i], (long)take);

                i         += (long)take;
                ch->left  -= take;
                ch->total += take;

                if (ch->left == 0UL)
                    ch->state = (unsigned char)HTTP_CHUNK_CRLF;
                break;
            }

            case HTTP_CHUNK_CRLF:
            {
                int c = data[i++];

                /* A chunk's data is followed by CRLF and by nothing else.  This
                   used to skip whatever was there until it found a '\n', so a
                   chunk whose length disagreed with its data resynchronised
                   onto the body instead of being refused. */
                if (c == '\n')
                    ch->state = (unsigned char)HTTP_CHUNK_SIZE;
                else if (c != '\r')
                    ch->state = (unsigned char)HTTP_CHUNK_ERROR;
                break;
            }

            default:                    /* HTTP_CHUNK_TRAILER               */
            {
                int c = data[i++];

                /* Trailer lines, then a blank one.  Nothing here reads a
                   trailer.  They are counted so the blank line is found. */
                if (c == '\n')
                {
                    if (ch->n == 0U)
                        ch->state = (unsigned char)HTTP_CHUNK_DONE;
                    ch->n = 0;
                }
                else if (c != '\r')
                {
                    ch->n = 1;
                }
                break;
            }
        }
    }

    return i;
}

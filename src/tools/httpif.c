/*
 * The If: header.  See httpif.h.
 *
 * SPDX-License-Identifier: MIT
 */

#include "httpif.h"

static int hi_lower(int c)
{
    return (c >= 'A' && c <= 'Z') ? (c + 32) : c;
}

static int hi_nicmp(const char *a, const char *b, unsigned long n)
{
    unsigned long i;

    for (i = 0; i < n; i++)
    {
        int ca = hi_lower((unsigned char)a[i]);
        int cb = hi_lower((unsigned char)b[i]);

        if (ca != cb || ca == 0)
            return ca - cb;
    }

    return 0;
}

static int hi_same(const char *a, const char *b)
{
    unsigned long i = 0;

    while (a[i] != '\0' && a[i] == b[i])
        i++;

    return (a[i] == '\0' && b[i] == '\0') ? 1 : 0;
}

static const char *hi_skip_ws(const char *p)
{
    while (*p == ' ' || *p == '\t')
        p++;

    return p;
}

/*
 * The text up to `close`, bounded, and the position after it.  *closed says
 * whether the delimiter was there: a header that stops in the middle of a
 * coded-URL is malformed, and a truncated token that happened to match would
 * be a condition holding on half of itself.
 */
static const char *hi_take(const char *p, int close, char *out,
                           unsigned long outlen, int *closed)
{
    unsigned long n = 0;

    while (*p != '\0' && *p != close)
    {
        if (n + 1UL < outlen)
            out[n++] = *p;
        p++;
    }

    out[n]  = '\0';
    *closed = (*p == close) ? 1 : 0;

    return *closed ? (p + 1) : p;
}

/*
 * An entity tag as the header wrote it against one the server made.  A weak
 * tag is compared as the strong one it wraps, because this server never emits
 * a weak tag and a client that asks about one means the resource.
 */
static int hi_etag_same(const char *a, const char *b)
{
    char        stripped[2][HTTP_IF_ETAG_MAX];
    const char *in[2];
    int         k;

    in[0] = a;
    in[1] = b;

    for (k = 0; k < 2; k++)
    {
        const char   *p = in[k];
        unsigned long n = 0;

        if (hi_nicmp(p, "w/", 2) == 0)
            p += 2;

        if (*p == '"')
            p++;

        while (*p != '\0' && *p != '"' &&
               n + 1UL < (unsigned long)HTTP_IF_ETAG_MAX)
            stripped[k][n++] = *p++;

        stripped[k][n] = '\0';
    }

    if (stripped[0][0] == '\0' || stripped[1][0] == '\0')
        return 0;

    return hi_same(stripped[0], stripped[1]);
}

int http_if_eval(const char *header, HttpIfLookup lookup, void *ctx)
{
    char        tag[HTTP_IF_TAG_MAX];
    HttpIfState state;
    int         have_state = 0;
    const char *p;

    /* No <stddef.h>: this file includes nothing, the same way httppath.c
       does, so that the host test compiles it and nothing else. */
    if (header == 0 || lookup == 0)
        return 0;

    tag[0] = '\0';

    for (p = header; ; )
    {
        int list;
        int seen;
        int closed;

        p = hi_skip_ws(p);

        if (*p == '\0')
            break;

        /* A Resource-Tag: every list after it is about that resource, until
           the next one. */
        if (*p == '<')
        {
            p = hi_take(p + 1, '>', tag, sizeof(tag), &closed);
            have_state = 0;
            continue;
        }

        if (*p != '(')
        {
            p++;                    /* nothing else belongs between lists  */
            continue;
        }

        if (!have_state)
        {
            state.token[0] = '\0';
            state.etag[0]  = '\0';
            lookup(ctx, tag, &state);
            have_state = 1;
        }

        list = 1;
        seen = 0;
        p++;                        /* the '('                             */

        for (;;)
        {
            int negate = 0;
            int holds;

            p = hi_skip_ws(p);

            if (*p == ')' || *p == '\0')
                break;

            if (hi_nicmp(p, "not", 3) == 0 &&
                (p[3] == ' ' || p[3] == '\t' || p[3] == '<' || p[3] == '['))
            {
                negate = 1;
                p = hi_skip_ws(p + 3);
            }

            if (*p == '<')
            {
                char token[HTTP_IF_TOKEN_MAX];

                p = hi_take(p + 1, '>', token, sizeof(token), &closed);

                if (!closed)
                {
                    list = 0;
                    break;
                }

                holds = (token[0] != '\0' && hi_same(token, state.token))
                            ? 1 : 0;
            }
            else if (*p == '[')
            {
                char etag[HTTP_IF_ETAG_MAX];

                p = hi_take(p + 1, ']', etag, sizeof(etag), &closed);

                if (!closed)
                {
                    list = 0;
                    break;
                }

                holds = hi_etag_same(etag, state.etag);
            }
            else
            {
                /* Not a condition this grammar has.  A list nobody can read
                   is not the one that makes the header true. */
                list = 0;
                p++;
                continue;
            }

            if (negate)
                holds = !holds;

            seen = 1;

            if (!holds)
                list = 0;
        }

        /* A list that was never closed is a header cut off in the middle of
           one, not a list that holds. */
        if (*p == ')')
            p++;
        else
            list = 0;

        if (seen && list)
            return 1;               /* one list holding is the whole header */
    }

    return 0;
}

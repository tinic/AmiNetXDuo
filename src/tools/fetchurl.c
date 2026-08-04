/*
 * fetchurl, see fetchurl.h for what is here and why it is its own file.
 *
 * Includes nothing, deliberately.  This translation unit is compiled twice:
 * once for m68k as part of `fetch`, and once natively by
 * src/tools/test/test_fetchurl.c, which is the only reason the RFC 3986
 * section 5.4 examples can be asserted on rather than reasoned about.
 *
 * SPDX-License-Identifier: MIT
 */

#include "fetchurl.h"

/* ------------------------------------------------------------- the small --- */

static unsigned long fu_len(const char *s)
{
    unsigned long n = 0;

    while (s[n] != '\0')
        n++;

    return n;
}

static int fu_lower(int c)
{
    return (c >= 'A' && c <= 'Z') ? (c + 32) : c;
}

static int fu_alpha(int c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

static int fu_digit(int c)
{
    return c >= '0' && c <= '9';
}

/* Case-insensitive compare of `n` bytes of `a` against the whole of `want`. */
static int fu_match(const char *a, unsigned long n, const char *want)
{
    unsigned long i;

    if (n != fu_len(want))
        return 0;

    for (i = 0; i < n; i++)
    {
        if (fu_lower((unsigned char)a[i]) != fu_lower((unsigned char)want[i]))
            return 0;
    }

    return 1;
}


/* -------------------------------------------------------- URI components --- */

/*
 * RFC 3986 section 3, as far as this command needs it: the five components
 * with the fragment already gone.  Nothing is copied; the pointers are into
 * the caller's string.
 */
typedef struct FuRef
{
    const char    *scheme;
    unsigned long  scheme_len;
    int            has_scheme;
    const char    *auth;
    unsigned long  auth_len;
    int            has_auth;
    const char    *path;
    unsigned long  path_len;
    const char    *query;
    unsigned long  query_len;
    int            has_query;
} FuRef;

/*
 * The "defined" of section 5.2.2 is not the same as "empty": "?" and no "?"
 * at all give different answers there, so has_query is carried separately
 * from query_len.
 */
static void fu_split(const char *s, FuRef *r)
{
    const char *end = s;
    const char *p;
    const char *q;

    while (end[0] != '\0' && end[0] != '#')     /* RFC 9110 section 7.1: the
                                                   target URI has no fragment */
        end++;

    r->scheme     = s;
    r->scheme_len = 0;
    r->has_scheme = 0;
    r->auth       = s;
    r->auth_len   = 0;
    r->has_auth   = 0;
    r->query      = s;
    r->query_len  = 0;
    r->has_query  = 0;

    p = s;

    /* scheme = ALPHA *( ALPHA / DIGIT / "+" / "-" / "." ) ":" */
    if (p < end && fu_alpha((unsigned char)p[0]))
    {
        q = p + 1;
        while (q < end && (fu_alpha((unsigned char)q[0]) ||
                           fu_digit((unsigned char)q[0]) ||
                           q[0] == '+' || q[0] == '-' || q[0] == '.'))
            q++;

        if (q < end && q[0] == ':')
        {
            r->scheme     = p;
            r->scheme_len = (unsigned long)(q - p);
            r->has_scheme = 1;
            p = q + 1;
        }
    }

    if (end - p >= 2 && p[0] == '/' && p[1] == '/')
    {
        p += 2;
        q = p;
        while (q < end && q[0] != '/' && q[0] != '?')
            q++;

        r->auth     = p;
        r->auth_len = (unsigned long)(q - p);
        r->has_auth = 1;
        p = q;
    }

    q = p;
    while (q < end && q[0] != '?')
        q++;

    r->path     = p;
    r->path_len = (unsigned long)(q - p);

    if (q < end && q[0] == '?')
    {
        r->query     = q + 1;
        r->query_len = (unsigned long)(end - (q + 1));
        r->has_query = 1;
    }
}


/* ------------------------------------------------------------- the pieces --- */

static void fu_default_port(FetchUrl *u)
{
    u->port = u->secure ? 443 : 80;
}

static FetchUrlResult fu_scheme(const char *s, unsigned long n, FetchUrl *out)
{
    if (fu_match(s, n, "https"))
        out->secure = 1;
    else if (fu_match(s, n, "http"))
        out->secure = 0;
    else
        return FETCH_URL_SCHEME;

    fu_default_port(out);

    return FETCH_URL_OK;
}

/*
 * host[:port].  An IPv6 literal is written in brackets, RFC 3986 style,
 * because its own colons would otherwise be read as the port separator.  The
 * brackets are kept: tool_sock_resolve() strips them and the Host: field
 * carries them.
 *
 * The port always starts from the scheme's default, so an authority taken
 * from a redirect does not inherit the port of the one it replaces.
 */
static FetchUrlResult fu_authority(const char *s, unsigned long n,
                                   FetchUrl *out)
{
    unsigned long i = 0;
    unsigned long j = 0;

    fu_default_port(out);

    /*
     * RFC 9110 section 4.2.4: a sender MUST NOT generate userinfo in an
     * http(s) target URI, and a recipient SHOULD treat its presence in a
     * reference from an untrusted source as an error, "it is likely being
     * used to obscure the authority for the sake of phishing attacks".  This
     * command has no authentication for it to carry either way.
     */
    for (i = 0; i < n; i++)
    {
        if (s[i] == '@')
            return FETCH_URL_USERINFO;
    }

    i = 0;
    if (n > 0 && s[0] == '[')
    {
        while (i < n && s[i] != ']')
        {
            if (j + 2 >= (unsigned long)FETCH_HOST_MAX)
                return FETCH_URL_HOST_LONG;
            out->host[j++] = s[i++];
        }

        if (i >= n)
            return FETCH_URL_BRACKET;

        out->host[j++] = s[i++];        /* the ']' */
    }
    else
    {
        while (i < n && s[i] != ':')
        {
            if (j + 1 >= (unsigned long)FETCH_HOST_MAX)
                return FETCH_URL_HOST_LONG;
            out->host[j++] = s[i++];
        }
    }
    out->host[j] = '\0';

    if (j == 0)
        return FETCH_URL_NO_HOST;

    if (i < n)
    {
        unsigned long port = 0;

        if (s[i] != ':')
            return FETCH_URL_PORT;      /* junk between ']' and the port */
        i++;

        if (i >= n || !fu_digit((unsigned char)s[i]))
            return FETCH_URL_PORT;

        while (i < n && fu_digit((unsigned char)s[i]))
        {
            port = (port * 10UL) + (unsigned long)(s[i] - '0');
            if (port > 65535UL)
                return FETCH_URL_PORT_RANGE;
            i++;
        }

        if (i < n)
            return FETCH_URL_PORT;

        out->port = (unsigned short)port;
    }

    return FETCH_URL_OK;
}

/* Remove the last segment of `out` and the "/" in front of it. */
static void fu_pop_segment(const char *out, unsigned long *o)
{
    while (*o > 0 && out[*o - 1] != '/')
        (*o)--;

    if (*o > 0)
        (*o)--;
}

/*
 * RFC 3986 section 5.2.4, the two-buffer method, letter for letter.  Returns
 * the length written, or 0 with nothing written when it would not fit, a
 * path this long has already been refused by the caller's buffer, so 0 is not
 * ambiguous with the empty result the "." reference produces.
 */
static unsigned long fu_remove_dot_segments(const char *in, unsigned long inlen,
                                            char *out, unsigned long outlen)
{
    unsigned long i = 0;
    unsigned long o = 0;

    while (i < inlen)
    {
        const char   *p   = in + i;
        unsigned long rem = inlen - i;

        /* A. a leading "../" or "./" is dropped */
        if (rem >= 3 && p[0] == '.' && p[1] == '.' && p[2] == '/')
        {
            i += 3;
            continue;
        }
        if (rem >= 2 && p[0] == '.' && p[1] == '/')
        {
            i += 2;
            continue;
        }

        /* B. "/./" becomes "/", and a trailing "/." leaves "/" behind */
        if (rem >= 3 && p[0] == '/' && p[1] == '.' && p[2] == '/')
        {
            i += 2;
            continue;
        }
        if (rem == 2 && p[0] == '/' && p[1] == '.')
        {
            if (o + 1 >= outlen)
                return 0;
            out[o++] = '/';
            i = inlen;
            continue;
        }

        /* C. the same for "/../", and the output loses a segment */
        if (rem >= 4 && p[0] == '/' && p[1] == '.' && p[2] == '.' &&
            p[3] == '/')
        {
            i += 3;
            fu_pop_segment(out, &o);
            continue;
        }
        if (rem == 3 && p[0] == '/' && p[1] == '.' && p[2] == '.')
        {
            fu_pop_segment(out, &o);
            if (o + 1 >= outlen)
                return 0;
            out[o++] = '/';
            i = inlen;
            continue;
        }

        /* D. an input of nothing but "." or ".." */
        if ((rem == 1 && p[0] == '.') ||
            (rem == 2 && p[0] == '.' && p[1] == '.'))
        {
            i = inlen;
            continue;
        }

        /* E. move one segment across, leading "/" and all */
        {
            unsigned long k = (p[0] == '/') ? 1UL : 0UL;
            unsigned long m;

            while (k < rem && p[k] != '/')
                k++;

            if (o + k >= outlen)
                return 0;

            for (m = 0; m < k; m++)
                out[o++] = p[m];

            i += k;
        }
    }

    out[o] = '\0';

    return o;
}

/*
 * RFC 3986 section 5.2.3.  `base` always has an authority here, so the
 * empty-path arm is the one that cannot happen, it is written anyway,
 * because the arm that cannot happen is the one that does.
 */
static unsigned long fu_merge(const char *base, unsigned long baselen,
                              const char *ref, unsigned long reflen,
                              char *out, unsigned long outlen)
{
    unsigned long keep = 0;
    unsigned long i;
    unsigned long o = 0;

    if (baselen == 0)
    {
        if (reflen + 2 > outlen)
            return 0;
        out[o++] = '/';
    }
    else
    {
        for (i = 0; i < baselen; i++)
        {
            if (base[i] == '/')
                keep = i + 1;       /* through the right-most "/" */
        }

        if (keep + reflen + 1 > outlen)
            return 0;

        for (i = 0; i < keep; i++)
            out[o++] = base[i];
    }

    for (i = 0; i < reflen; i++)
        out[o++] = ref[i];

    out[o] = '\0';

    return o;
}

/*
 * The origin-form request target: the normalised path, then the query.  An
 * authority with an empty path is written "/", which RFC 9112 section 3.2.1
 * requires of the request line.
 */
static FetchUrlResult fu_target(FetchUrl *out,
                                const char *path, unsigned long pathlen,
                                const char *query, unsigned long querylen,
                                int has_query)
{
    unsigned long o = 0;
    unsigned long i;

    if (pathlen == 0)
    {
        out->path[o++] = '/';
    }
    else
    {
        if (pathlen + 1 > (unsigned long)FETCH_PATH_MAX)
            return FETCH_URL_PATH_LONG;

        for (i = 0; i < pathlen; i++)
            out->path[o++] = path[i];
    }

    if (has_query)
    {
        if (o + querylen + 2 > (unsigned long)FETCH_PATH_MAX)
            return FETCH_URL_PATH_LONG;

        out->path[o++] = '?';
        for (i = 0; i < querylen; i++)
            out->path[o++] = query[i];
    }

    out->path[o] = '\0';

    return FETCH_URL_OK;
}


/* ------------------------------------------------------------------ parse --- */

FetchUrlResult fetch_url_parse(const char *url, FetchUrl *out)
{
    const char    *end = url;
    const char    *p   = url;
    const char    *q;
    const char    *auth;
    unsigned long  auth_len;
    char           clean[FETCH_PATH_MAX];
    unsigned long  cleanlen;
    FetchUrlResult why;

    out->secure  = 0;
    out->port    = 80;
    out->host[0] = '\0';
    out->path[0] = '\0';

    while (end[0] != '\0' && end[0] != '#')
        end++;

    /*
     * "scheme://" is the only spelling that names a scheme here, and not the
     * strict scheme rule fu_split() uses.  A user typing "host:8080/x" at the
     * Shell means a port, not a scheme called "host", and a bare "host/path"
     * means http, neither is a URI reference arriving from a server, which
     * is what fetch_url_resolve() is for.
     */
    for (q = url; q < end; q++)
    {
        if (q[0] == '/' || q[0] == '?')
            break;

        if (q[0] == ':' && end - q >= 3 && q[1] == '/' && q[2] == '/')
        {
            why = fu_scheme(url, (unsigned long)(q - url), out);
            if (why != FETCH_URL_OK)
                return why;

            p = q + 3;
            break;
        }
    }

    auth = p;
    q    = p;
    while (q < end && q[0] != '/' && q[0] != '?')
        q++;
    auth_len = (unsigned long)(q - p);

    why = fu_authority(auth, auth_len, out);
    if (why != FETCH_URL_OK)
        return why;

    p = q;
    while (q < end && q[0] != '?')
        q++;

    cleanlen = fu_remove_dot_segments(p, (unsigned long)(q - p),
                                      clean, sizeof(clean));
    if (cleanlen == 0 && q != p)
        return FETCH_URL_PATH_LONG;

    if (q < end)
        return fu_target(out, clean, cleanlen, q + 1,
                         (unsigned long)(end - (q + 1)), 1);

    return fu_target(out, clean, cleanlen, url, 0, 0);
}


/* ---------------------------------------------------------------- resolve --- */

FetchUrlResult fetch_url_resolve(const FetchUrl *base, const char *ref,
                                 FetchUrl *out)
{
    FuRef          r;
    char           merged[FETCH_PATH_MAX * 2];
    char           clean[FETCH_PATH_MAX];
    const char    *path;
    unsigned long  pathlen;
    const char    *query    = ref;
    unsigned long  querylen = 0;
    int            has_query;
    unsigned long  cleanlen;
    FetchUrlResult why;

    fu_split(ref, &r);

    /* T.scheme and T.authority default to the base's; every arm below that
       does not overwrite them wants exactly that. */
    *out = *base;

    path      = r.path;
    pathlen   = r.path_len;
    has_query = r.has_query;
    query     = r.query;
    querylen  = r.query_len;

    if (r.has_scheme)
    {
        why = fu_scheme(r.scheme, r.scheme_len, out);
        if (why != FETCH_URL_OK)
            return why;

        if (!r.has_auth)
            return FETCH_URL_NO_HOST;

        why = fu_authority(r.auth, r.auth_len, out);
        if (why != FETCH_URL_OK)
            return why;
    }
    else if (r.has_auth)
    {
        /* A network-path reference: the base's scheme, a new authority. */
        why = fu_authority(r.auth, r.auth_len, out);
        if (why != FETCH_URL_OK)
            return why;
    }
    else
    {
        const char   *bpath = base->path;
        unsigned long blen  = 0;
        const char   *bq    = base->path;
        unsigned long bqlen = 0;
        int           bhas  = 0;

        while (bpath[blen] != '\0' && bpath[blen] != '?')
            blen++;
        if (bpath[blen] == '?')
        {
            bq    = &bpath[blen + 1];
            bqlen = fu_len(bq);
            bhas  = 1;
        }

        if (r.path_len == 0)
        {
            path    = bpath;
            pathlen = blen;

            if (!r.has_query)
            {
                query     = bq;
                querylen  = bqlen;
                has_query = bhas;
            }
        }
        else if (r.path[0] != '/')
        {
            unsigned long n = fu_merge(bpath, blen, r.path, r.path_len,
                                       merged, sizeof(merged));

            if (n == 0)
                return FETCH_URL_PATH_LONG;

            path    = merged;
            pathlen = n;
        }
    }

    cleanlen = fu_remove_dot_segments(path, pathlen, clean, sizeof(clean));
    if (cleanlen == 0 && pathlen != 0 && path[0] == '/')
        return FETCH_URL_PATH_LONG;

    return fu_target(out, clean, cleanlen, query, querylen, has_query);
}

int fetch_url_authority(const FetchUrl *u, char *dst, unsigned long dstlen)
{
    unsigned long o = 0;
    unsigned long i;
    unsigned short dflt = u->secure ? 443 : 80;

    for (i = 0; u->host[i] != '\0'; i++)
    {
        if (o + 1 >= dstlen)
            return 0;
        dst[o++] = u->host[i];
    }

    if (u->port != dflt)
    {
        char          digits[8];
        unsigned long n     = 0;
        unsigned long value = u->port;

        do
        {
            digits[n++] = (char)('0' + (value % 10UL));
            value /= 10UL;
        }
        while (value != 0UL && n < sizeof(digits));

        if (o + n + 2 > dstlen)
            return 0;

        dst[o++] = ':';
        for (i = 0; i < n; i++)
            dst[o++] = digits[n - 1 - i];
    }

    dst[o] = '\0';

    return 1;
}

const char *fetch_url_error(FetchUrlResult why)
{
    switch (why)
    {
        case FETCH_URL_OK:
            return "that URL is fine";
        case FETCH_URL_SCHEME:
            return "that is not an http: or https: URL";
        case FETCH_URL_NO_HOST:
            return "that URL has no host name in it";
        case FETCH_URL_HOST_LONG:
            return "the host name in that URL is too long";
        case FETCH_URL_PATH_LONG:
            return "the path in that URL is too long";
        case FETCH_URL_BRACKET:
            return "the address in that URL has no closing bracket";
        case FETCH_URL_PORT:
            return "the port number in that URL is not a number";
        case FETCH_URL_PORT_RANGE:
            return "the port number in that URL is out of range";
        case FETCH_URL_USERINFO:
            return "that URL carries a user name, which this command does not "
                   "send and which is how a phishing link hides where it goes";
        default:
            break;
    }

    return "that URL cannot be used";
}


/* ------------------------------------------------------- the header block --- */

void fetch_head_start(FetchHead *h, char *buf, unsigned long size)
{
    h->buf       = buf;
    h->size      = size;
    h->len       = 0;
    h->truncated = 0;
    h->complete  = 0;
    h->tail[0]   = 0;
    h->tail[1]   = 0;
    h->tail[2]   = 0;
    h->tail[3]   = 0;

    if (size > 0)
        buf[0] = '\0';
}

unsigned long fetch_head_feed(FetchHead *h, const unsigned char *data,
                              unsigned long len)
{
    unsigned long at = 0;

    while (!h->complete && at < len)
    {
        char c = (char)data[at++];

        if (h->len + 1 < h->size)
            h->buf[h->len++] = c;
        else
            h->truncated = 1;

        h->tail[0] = h->tail[1];
        h->tail[1] = h->tail[2];
        h->tail[2] = h->tail[3];
        h->tail[3] = c;

        /* CRLFCRLF, and the bare LFLF a lenient server sends. */
        if ((h->tail[0] == '\r' && h->tail[1] == '\n' &&
             h->tail[2] == '\r' && h->tail[3] == '\n') ||
            (h->tail[2] == '\n' && h->tail[3] == '\n'))
        {
            h->complete = 1;
        }
    }

    if (h->len < h->size)
        h->buf[h->len] = '\0';

    return at;
}

unsigned long fetch_head_status(const FetchHead *h)
{
    const char   *p    = h->buf;
    unsigned long code = 0;
    unsigned long i;

    if (h->len == 0)
        return 0;

    while (p[0] != '\0' && p[0] != ' ')
        p++;
    while (p[0] == ' ')
        p++;

    for (i = 0; i < 3UL; i++)
    {
        if (p[i] < '0' || p[i] > '9')
            return 0;
        code = (code * 10UL) + (unsigned long)(p[i] - '0');
    }

    return code;
}

int fetch_head_interim(unsigned long status)
{
    return status >= 100UL && status < 200UL;
}

const char *fetch_head_field(const FetchHead *h, const char *name)
{
    unsigned long namelen = fu_len(name);
    const char   *head    = h->buf;
    unsigned long i;

    /* Skip the status line, a header never appears on it. */
    for (i = 0; head[i] != '\0'; i++)
    {
        unsigned long j;
        int           hit = 1;

        if (head[i] != '\n')
            continue;

        i++;

        /* `name` has no NUL inside namelen, so the end of the block stops
           this comparison rather than being read past. */
        for (j = 0; j < namelen; j++)
        {
            if (fu_lower((unsigned char)head[i + j]) !=
                fu_lower((unsigned char)name[j]))
            {
                hit = 0;
                break;
            }
        }

        if (!hit)
            continue;

        i += namelen;
        while (head[i] == ' ' || head[i] == '\t')
            i++;

        return &head[i];
    }

    return 0;
}

int fetch_head_value(const char *value, char *dst, unsigned long dstlen)
{
    unsigned long i = 0;

    while (value[i] != '\0' && value[i] != '\r' && value[i] != '\n')
    {
        if (i + 1 >= dstlen)
            return 0;
        dst[i] = value[i];
        i++;
    }

    dst[i] = '\0';

    return i > 0;
}

void fetch_head_first_line(const FetchHead *h, char *dst, unsigned long dstlen)
{
    const char   *head = h->buf;
    unsigned long i    = 0;

    while (head[i] != '\0' && head[i] != '\r' && head[i] != '\n' &&
           i + 1 < dstlen)
    {
        dst[i] = head[i];
        i++;
    }

    dst[i] = '\0';
}

/*
 * httppath, request target to AmigaOS path.  See httppath.h for what this
 * is defending against and why it is its own file.
 *
 * Includes nothing, deliberately.  This translation unit is compiled twice:
 * once for m68k as part of the server, and once natively by
 * src/tools/test/test_httppath.c, which is the only reason the checks below
 * can be asserted on rather than reasoned about.  A single #include of
 * tools.h would end that, because tools.h reaches tx_api.h and proto/dos.h.
 *
 * SPDX-License-Identifier: MIT
 */

#include "httppath.h"

/* ------------------------------------------------------------- the small --- */

static unsigned long hp_len(const char *s)
{
    unsigned long n = 0;

    while (s[n] != '\0')
        n++;

    return n;
}

static int hp_lower(int c)
{
    return (c >= 'A' && c <= 'Z') ? (c + 32) : c;
}

/* -1 when `c` is not a hex digit. */
static int hp_hex(int c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;

    return -1;
}

/*
 * Unreserved, RFC 3986 2.3.  Everything else in a path segment is escaped on
 * the way out, which on this machine is mostly the space: AmigaOS filenames
 * are full of them and an unescaped one ends the request line.
 */
static int hp_unreserved(int c)
{
    return ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~');
}

/* ------------------------------------------------------------- resolving --- */

/*
 * Step past the scheme and authority of an absolute-form target.  RFC 7230
 * says a server must accept it, and a client behind a proxy configuration
 * sends it.  Both Windows' redirector and gvfs have been seen to.  Returns a
 * pointer to the path, which is "/" when the authority was the whole target.
 */
static const char *hp_skip_authority(const char *target)
{
    const char *p = target;
    unsigned long i;

    for (i = 0; i < 8UL && p[i] != '\0'; i++)
    {
        if (p[i] == ':')
            break;
        if (!((p[i] >= 'a' && p[i] <= 'z') || (p[i] >= 'A' && p[i] <= 'Z')))
            return target;
    }

    if (i == 0UL || p[i] != ':' || p[i + 1] != '/' || p[i + 2] != '/')
        return target;

    p += i + 3;

    while (*p != '\0' && *p != '/')
        p++;

    return (*p == '/') ? p : "/";
}

/*
 * The decode.  Everything a client can say arrives here as text and leaves as
 * bytes, and the checks that follow read the bytes.  So %2E%2E is ".." by the
 * time anything asks whether it is.
 */
static HttpPathResult hp_decode(const char *target, char *out,
                                unsigned long outlen, unsigned long *outused)
{
    unsigned long n = 0;

    while (*target != '\0' && *target != '?' && *target != '#')
    {
        int c = (unsigned char)*target++;

        if (c == '%')
        {
            int hi = hp_hex((unsigned char)target[0]);
            int lo = (hi < 0) ? -1 : hp_hex((unsigned char)target[1]);

            if (lo < 0)
                return HTTP_PATH_BAD_ESCAPE;

            c = (hi << 4) | lo;
            target += 2;
        }

        /*
         * A NUL truncates every AmigaDOS call downstream, and a control
         * character in a name that arrived over a socket names no file the
         * server serves.  Both are refused here rather than sanitised.
         */
        if (c < 0x20 || c == 0x7f)
            return HTTP_PATH_CONTROL;

        if (n + 1UL >= outlen)
            return HTTP_PATH_TOO_LONG;

        out[n++] = (char)c;
    }

    out[n] = '\0';
    *outused = n;

    return HTTP_PATH_OK;
}

HttpPathResult http_path_resolve(const char *root, const char *target,
                                 HttpPath *out)
{
    char           decoded[HTTP_URL_MAX];
    unsigned long  declen = 0;
    unsigned long  rootlen;
    unsigned long  pathlen;
    unsigned long  urllen = 0;
    unsigned long  i;
    int            segments = 0;
    int            namestart = -1;
    HttpPathResult why;

    if (root == 0 || target == 0 || out == 0)
        return HTTP_PATH_NOT_ABSOLUTE;

    target = hp_skip_authority(target);

    if (target[0] != '/')
        return HTTP_PATH_NOT_ABSOLUTE;

    why = hp_decode(target, decoded, sizeof(decoded), &declen);
    if (why != HTTP_PATH_OK)
        return why;

    rootlen = hp_len(root);
    if (rootlen == 0UL || rootlen + 1UL >= HTTP_PATH_MAX)
        return HTTP_PATH_TOO_LONG;

    for (i = 0; i < rootlen; i++)
        out->path[i] = root[i];

    pathlen = rootlen;

    /*
     * The join is done here rather than by the caller so that no path this
     * function returns can contain the doubled slash that means the parent
     * directory on AmigaOS.  A root ending in ':' or '/' already carries its
     * separator.  "DH0:" + "foo" is "DH0:foo" and not "DH0:/foo", which is a
     * different place, the root directory of the volume rather than the
     * drawer.
     */
    i = 0;
    while (i < declen)
    {
        unsigned long start;
        unsigned long seglen;
        unsigned long k;

        while (i < declen && decoded[i] == '/')
            i++;                            /* empty segments collapse       */

        if (i >= declen)
            break;

        start = i;
        while (i < declen && decoded[i] != '/')
            i++;
        seglen = i - start;

        /* ".." is refused rather than walked.  A browser drops the segment
           before it sends, so a request that still carries one is not one to
           be resolved. */
        if (seglen == 2UL && decoded[start] == '.' && decoded[start + 1] == '.')
            return HTTP_PATH_PARENT;

        if (seglen == 1UL && decoded[start] == '.')
            continue;

        for (k = start; k < i; k++)
        {
            /* The AmigaOS one.  Everything left of a colon is a device or an
               assign, so a single one anywhere in the path leaves the document
               root entirely, "RAM:", "DH0:", "S:startup-sequence". */
            if (decoded[k] == ':')
                return HTTP_PATH_DEVICE;

            /* Not a separator on this machine, and a legal file name
               character, but it is a separator to whoever sent it, so a
               request containing one does not mean what it says. */
            if (decoded[k] == '\\')
                return HTTP_PATH_BACKSLASH;
        }

        if (segments >= HTTP_PATH_SEGMENTS)
            return HTTP_PATH_TOO_DEEP;

        if (pathlen > 0UL && out->path[pathlen - 1] != ':' &&
            out->path[pathlen - 1] != '/')
        {
            if (pathlen + 1UL >= HTTP_PATH_MAX)
                return HTTP_PATH_TOO_LONG;
            out->path[pathlen++] = '/';
        }

        if (pathlen + seglen >= HTTP_PATH_MAX)
            return HTTP_PATH_TOO_LONG;

        namestart = (int)pathlen;
        for (k = 0; k < seglen; k++)
            out->path[pathlen++] = decoded[start + k];

        if (urllen + seglen + 1UL >= HTTP_URL_MAX)
            return HTTP_PATH_TOO_LONG;

        out->url[urllen++] = '/';
        for (k = 0; k < seglen; k++)
            out->url[urllen++] = decoded[start + k];

        segments++;
    }

    out->path[pathlen] = '\0';

    if (urllen == 0UL)
        out->url[urllen++] = '/';
    out->url[urllen] = '\0';

    if (namestart < 0)
    {
        out->name[0] = '\0';
    }
    else
    {
        unsigned long n = 0;

        for (i = (unsigned long)namestart; i < pathlen; i++)
        {
            if (n + 1UL >= (unsigned long)HTTP_NAME_MAX)
                return HTTP_PATH_TOO_LONG;
            out->name[n++] = out->path[i];
        }
        out->name[n] = '\0';
    }

    out->segments       = segments;
    out->trailing_slash = (declen > 0UL && decoded[declen - 1] == '/') ? 1 : 0;

    return HTTP_PATH_OK;
}

int http_path_root(const char *given, char *out, unsigned long outlen)
{
    unsigned long n = 0;

    if (out == 0 || outlen == 0UL)
        return 0;

    out[0] = '\0';

    if (given == 0)
        return 0;

    while (given[n] != '\0' && n + 1UL < outlen)
    {
        out[n] = given[n];
        n++;
    }

    if (given[n] != '\0')
    {
        out[0] = '\0';
        return 0;
    }

    out[n] = '\0';

    /* One character is kept whatever it is: "/" has nothing above it to trim
       to, and neither does "". */
    while (n > 1UL && out[n - 1] == '/')
        out[--n] = '\0';

    return 1;
}

void http_utf8_trim(char *text)
{
    unsigned long n;
    unsigned long back;

    if (text == 0)
        return;

    n = hp_len(text);

    /* At most three continuation bytes can precede the lead of the longest
       sequence there is, so the walk back is bounded whatever the bytes. */
    for (back = 0; back < 4UL && back < n; back++)
    {
        unsigned char c = (unsigned char)text[n - 1UL - back];
        unsigned long need;

        if ((c & 0xC0) == 0x80)
            continue;                       /* a continuation byte          */

        if ((c & 0x80) == 0)
        {
            /* Plain ASCII, so anything walked over to reach it was a
               continuation byte with no lead in front of it. */
            if (back > 0UL)
                text[n - back] = '\0';

            return;
        }

        if      ((c & 0xE0) == 0xC0) need = 2UL;
        else if ((c & 0xF0) == 0xE0) need = 3UL;
        else if ((c & 0xF8) == 0xF0) need = 4UL;
        else
        {
            /* An invalid lead byte where a lead byte belongs.  Nothing here
               completes a sequence, so it is cut. */
            text[n - 1UL - back] = '\0';
            return;
        }

        if (back + 1UL < need)
            text[n - 1UL - back] = '\0';

        return;
    }

    /* Four continuation bytes and no lead: nothing here is a character. */
    if (n >= 4UL)
        text[n - 4UL] = '\0';
}

const char *http_path_error(HttpPathResult why)
{
    switch (why)
    {
        case HTTP_PATH_OK:           return "no fault";
        case HTTP_PATH_BAD_ESCAPE:   return "a % that is not an escape";
        case HTTP_PATH_CONTROL:      return "a control character";
        case HTTP_PATH_NOT_ABSOLUTE: return "not an absolute path";
        case HTTP_PATH_PARENT:       return "a .. that leaves the root";
        case HTTP_PATH_DEVICE:       return "a colon, an AmigaOS device";
        case HTTP_PATH_BACKSLASH:    return "a backslash";
        case HTTP_PATH_TOO_LONG:     return "too long";
        case HTTP_PATH_TOO_DEEP:     return "too many directories";
    }

    return "refused";
}

/* ---------------------------------------------------------------- walking --- */

int http_path_join(char *path, unsigned long pathlen, const char *name)
{
    unsigned long n = hp_len(path);
    unsigned long namelen;
    unsigned long need;
    unsigned long i;
    int           separate;

    if (name == 0 || name[0] == '\0')
        return 0;

    for (namelen = 0; name[namelen] != '\0'; namelen++)
    {
        if (name[namelen] == '/' || name[namelen] == ':' ||
            name[namelen] == '\\')
            return 0;
    }

    separate = (n > 0UL && path[n - 1] != ':' && path[n - 1] != '/') ? 1 : 0;
    need     = namelen + (unsigned long)separate;

    /* Everything is measured before anything is written.  A join that ran out
       of room half way would leave the walk holding a path with a trailing
       separator, which is the parent directory on this machine. */
    if (n + need + 1UL > pathlen)
        return 0;

    if (separate)
        path[n++] = '/';

    for (i = 0; i < namelen; i++)
        path[n++] = name[i];

    path[n] = '\0';

    return 1;
}

void http_path_up(char *path)
{
    unsigned long n = hp_len(path);

    while (n > 0UL && path[n - 1] != '/' && path[n - 1] != ':')
        n--;

    /* A trailing ':' is the device and stays.  A trailing '/' was the
       separator this level was joined with and goes. */
    if (n > 0UL && path[n - 1] == '/')
        n--;

    path[n] = '\0';
}

int http_path_within(const char *prefix, const char *path)
{
    unsigned long n = hp_len(prefix);
    unsigned long i;

    if (n == 0UL)
        return 0;

    for (i = 0; i < n; i++)
    {
        if (path[i] == '\0' ||
            hp_lower((unsigned char)path[i]) !=
            hp_lower((unsigned char)prefix[i]))
            return 0;
    }

    if (path[n] == '\0')
        return 1;

    /* "Work:Public" must not match "Work:PublicSecrets", so what follows the
       prefix has to be a separator, unless the prefix ended in one. */
    return (path[n] == '/' || prefix[n - 1] == ':' || prefix[n - 1] == '/')
               ? 1 : 0;
}

/* -------------------------------------------------------------- escaping --- */

unsigned long http_url_escape(const char *path, char *out, unsigned long outlen)
{
    static const char hex[] = "0123456789ABCDEF";
    unsigned long n = 0;

    if (outlen == 0UL)
        return 0;

    while (*path != '\0')
    {
        int c = (unsigned char)*path++;

        if (hp_unreserved(c) || c == '/')
        {
            if (n + 1UL >= outlen)
                goto full;
            out[n++] = (char)c;
        }
        else
        {
            if (n + 3UL >= outlen)
                goto full;
            out[n++] = '%';
            out[n++] = hex[(c >> 4) & 15];
            out[n++] = hex[c & 15];
        }
    }

    out[n] = '\0';
    return n;

full:
    out[0] = '\0';
    return 0;
}

unsigned long http_xml_escape(const char *text, char *out, unsigned long outlen)
{
    unsigned long n = 0;

    if (outlen == 0UL)
        return 0;

    while (*text != '\0')
    {
        int         c = (unsigned char)*text++;
        const char *rep = 0;
        unsigned long replen = 0;

        switch (c)
        {
            case '&':  rep = "&amp;";  replen = 5; break;
            case '<':  rep = "&lt;";   replen = 4; break;
            case '>':  rep = "&gt;";   replen = 4; break;
            case '"':  rep = "&quot;"; replen = 6; break;
            default:   break;
        }

        if (rep != 0)
        {
            unsigned long k;

            if (n + replen >= outlen)
                goto full;
            for (k = 0; k < replen; k++)
                out[n++] = rep[k];
        }
        else
        {
            if (n + 1UL >= outlen)
                goto full;
            out[n++] = (char)c;
        }
    }

    out[n] = '\0';
    return n;

full:
    out[0] = '\0';
    return 0;
}

/* --------------------------------------------------------------- content --- */

/*
 * Only what an Amiga is likely to be serving, and text/plain rather than
 * application/octet-stream for the ones a browser can show.  A table rather
 * than a chain of strcmp calls.
 */
typedef struct HttpMimeEntry
{
    const char *suffix;
    const char *type;
} HttpMimeEntry;

static const HttpMimeEntry http_mime_table[] =
{
    { "html", "text/html"                },
    { "htm",  "text/html"                },
    { "txt",  "text/plain"               },
    { "doc",  "text/plain"               },
    { "readme", "text/plain"             },
    { "guide", "text/plain"              },
    { "css",  "text/css"                 },
    { "js",   "application/javascript"   },
    { "json", "application/json"         },
    { "xml",  "text/xml"                 },
    { "gif",  "image/gif"                },
    { "jpg",  "image/jpeg"               },
    { "jpeg", "image/jpeg"               },
    { "png",  "image/png"                },
    { "iff",  "image/x-ilbm"             },
    { "ilbm", "image/x-ilbm"             },
    { "lbm",  "image/x-ilbm"             },
    { "pdf",  "application/pdf"          },
    { "zip",  "application/zip"          },
    { "lha",  "application/x-lha"        },
    { "lzh",  "application/x-lha"        },
    { "lzx",  "application/x-lzx"        },
    { "adf",  "application/x-amiga-disk" },
    { "dms",  "application/x-amiga-disk" },
    { "mod",  "audio/x-mod"              },
    { "wav",  "audio/x-wav"              },
    { "8svx", "audio/x-8svx"             },
    { "mp3",  "audio/mpeg"               },
    { "info", "application/x-amiga-icon" },
    { 0,      0                          }
};

const char *http_content_type(const char *name)
{
    const char *dot;
    unsigned long at = 0;               /* 0 means no '.' seen */
    unsigned long n = 0;
    unsigned long i;

    /* By index rather than by pointer: walking with one and remembering the
       other makes the two alias after the increment, and -fanalyzer then reads
       the later null check as a check after a dereference. */
    while (name[n] != '\0')
    {
        if (name[n] == '.')
            at = n + 1;
        n++;
    }

    if (at == 0 || name[at] == '\0')
        return "application/octet-stream";

    dot = name + at;

    for (i = 0; http_mime_table[i].suffix != 0; i++)
    {
        const char *a = http_mime_table[i].suffix;
        const char *b = dot;

        while (*a != '\0' && *b != '\0' && hp_lower((unsigned char)*a) ==
                                           hp_lower((unsigned char)*b))
        {
            a++;
            b++;
        }

        if (*a == '\0' && *b == '\0')
            return http_mime_table[i].type;
    }

    return "application/octet-stream";
}

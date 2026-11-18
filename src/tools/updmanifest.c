/*
 * updmanifest: see updmanifest.h for the format.
 *
 * SPDX-License-Identifier: MIT
 */

#include "updmanifest.h"

static const char *const um_flavours[UPD_FLAVOUR_COUNT] = {
    "full", "minimal", "micro", "fat"
};

static const char *const um_classes[UPD_CLASS_COUNT] = {
    "LIBS", "DEVS/Networks", "C", "certificates"
};

static const char *const um_channels[UPD_CHANNEL_COUNT] = {
    "release", "prerelease"
};

static const char *const um_results[UPD_RESULT_COUNT] = {
    "ok",
    "truncated",
    "line too long",
    "syntax error",
    "unknown format",
    "duplicate record",
    "required record missing",
    "record count wrong",
    "unknown destination",
    "bad hex string",
    "bad number",
    "bad path",
    "bad version",
    "too many files",
    "wrong product",
    "wrong channel",
    "not newer than installed",
    "installed flavour not offered",
    "no known signing key",
    "bad signature",
    "bad argument"
};

/* Records that may appear once. */
#define UM_FORMAT   0x01u
#define UM_PRODUCT  0x02u
#define UM_VERSION  0x04u
#define UM_CHANNEL  0x08u
#define UM_CERTS    0x10u

#define UM_CHANNEL_UNKNOWN 0xff

typedef struct UmField
{
    const char    *p;
    unsigned long  n;
} UmField;


/* ----------------------------------------------------------- the words --- */

static int um_is(const char *s, unsigned long n, const char *want)
{
    unsigned long i;

    for (i = 0; i < n; i++)
    {
        if (want[i] == '\0' || want[i] != s[i])
            return 0;
    }

    return want[n] == '\0';
}

static int um_lookup(const char *s, unsigned long n,
                     const char *const *names, unsigned int count)
{
    unsigned int i;

    for (i = 0; i < count; i++)
    {
        if (um_is(s, n, names[i]))
            return (int)i;
    }

    return -1;
}

static int um_nibble(int c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

/* Exactly 2 * outlen hex digits. */
static int um_hex(const UmField *f, unsigned char *out, unsigned long outlen)
{
    unsigned long i;

    if (f->n != 2UL * outlen)
        return 0;

    for (i = 0; i < outlen; i++)
    {
        int hi = um_nibble((unsigned char)f->p[2 * i]);
        int lo = um_nibble((unsigned char)f->p[2 * i + 1]);

        if (hi < 0 || lo < 0)
            return 0;
        out[i] = (unsigned char)((hi << 4) | lo);
    }

    return 1;
}

/* Decimal, no leading zero, at most 4294967295. */
static int um_number(const UmField *f, unsigned long *out)
{
    unsigned long i;
    unsigned long v = 0;

    if (f->n == 0 || f->n > 10 || (f->p[0] == '0' && f->n > 1))
        return 0;

    for (i = 0; i < f->n; i++)
    {
        unsigned long d;

        if (f->p[i] < '0' || f->p[i] > '9')
            return 0;
        d = (unsigned long)(f->p[i] - '0');
        if (v > (0xffffffffUL - d) / 10UL)
            return 0;
        v = v * 10UL + d;
    }

    *out = v;
    return 1;
}

/*
 * A path relative to its destination, or with `plain` a bare file name.  The
 * refusals are the AmigaDOS ones: ":" names a volume, a leading "/" or an
 * empty part between two is the parent.
 */
static int um_path(const UmField *f, int plain)
{
    unsigned long i;
    unsigned long part = 0;

    if (f->n == 0 || f->n > (plain ? UPD_NAME_MAX : UPD_PATH_MAX))
        return 0;

    if (f->p[0] == ' ' || f->p[f->n - 1] == ' ' || f->p[f->n - 1] == '/')
        return 0;

    for (i = 0; i <= f->n; i++)
    {
        if (i == f->n || f->p[i] == '/')
        {
            unsigned long len = i - part;

            if (plain && i != f->n)
                return 0;
            if (len == 0)
                return 0;
            if (f->p[part] == '.' && (len == 1 ||
                                      (len == 2 && f->p[part + 1] == '.')))
                return 0;
            part = i + 1;
        }
        else if (f->p[i] == ':' || (plain && f->p[i] == ' '))
        {
            return 0;
        }
    }

    return 1;
}

static int um_fold(int c)
{
    return (c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c;
}

static int um_same_name(const char *a, unsigned long an,
                        const char *b, unsigned long bn)
{
    unsigned long i;

    if (an != bn)
        return 0;

    for (i = 0; i < an; i++)
    {
        if (um_fold((unsigned char)a[i]) != um_fold((unsigned char)b[i]))
            return 0;
    }

    return 1;
}

/* `k` fields split at single spaces; the last takes the rest of the value. */
static int um_fields(const char *v, unsigned long n, UmField *f, unsigned int k)
{
    unsigned long i = 0;
    unsigned int  j;

    for (j = 0; j < k; j++)
    {
        f[j].p = v + i;

        if (j == k - 1)
        {
            f[j].n = n - i;
        }
        else
        {
            while (i < n && v[i] != ' ')
                i++;
            f[j].n = (unsigned long)(v + i - f[j].p);
            if (i >= n)
                return 0;
            i++;                        /* the space                       */
        }

        if (f[j].n == 0)
            return 0;
    }

    return 1;
}

/*
 * The line starting at text[pos]: its length without the LF in *n, after the
 * byte checks.  UPD_OK or the reason it is not a line.
 */
static UpdResult um_line(const char *text, unsigned long len,
                         unsigned long pos, unsigned long *n)
{
    unsigned long i;

    for (i = 0; pos + i < len && i <= UPD_LINE_MAX; i++)
    {
        int c = (unsigned char)text[pos + i];

        if (c == '\n')
        {
            *n = i;
            return UPD_OK;
        }
        if (c < 0x20 || c > 0x7e)
            return UPD_E_SYNTAX;
    }

    return (i > UPD_LINE_MAX) ? UPD_E_LINE_LONG : UPD_E_TRUNCATED;
}


/* ------------------------------------------------------- the manifest --- */

typedef struct UmState
{
    const UpdWant *want;
    UpdManifest   *m;
    UpdFile       *files;
    unsigned int   max_files;
    unsigned int   seen;                /* UM_* bits                       */
    unsigned int   containers;          /* flavour bits                    */
    unsigned int   flavours_with_files;
    int            any_certs;
    int            have_product;
} UmState;

/*
 * The flavour a container= or file= line starts with, or -1 for one this
 * updater does not know.  Such a line is skipped whole, whatever follows, so
 * a later release can add a flavour.
 */
static int um_flavour(const char *v, unsigned long n)
{
    unsigned long i = 0;

    while (i < n && v[i] != ' ')
        i++;

    return um_lookup(v, i, um_flavours, UPD_FLAVOUR_COUNT);
}

static UpdResult um_container(UmState *st, const char *v, unsigned long n)
{
    UmField       f[4];
    int           fl;
    unsigned long size;
    unsigned char sha[32];

    if ((fl = um_flavour(v, n)) < 0)
        return UPD_OK;
    if (!um_fields(v, n, f, 4))
        return UPD_E_SYNTAX;
    if (!um_number(&f[1], &size))
        return UPD_E_NUMBER;
    if (!um_hex(&f[2], sha, 32))
        return UPD_E_HEX;
    if (!um_path(&f[3], 1))
        return UPD_E_PATH;
    if (st->containers & (1u << fl))
        return UPD_E_DUPLICATE;

    st->containers |= 1u << fl;

    if (fl == st->want->flavour)
    {
        unsigned int i;

        st->m->container      = f[3].p;
        st->m->container_len  = (unsigned short)f[3].n;
        st->m->container_size = size;
        for (i = 0; i < 32; i++)
            st->m->container_sha256[i] = sha[i];
    }

    return UPD_OK;
}

static UpdResult um_file(UmState *st, const char *v, unsigned long n)
{
    UmField       f[5];
    int           fl, cls;
    unsigned long size;
    unsigned char sha[32];
    UpdFile      *e;
    unsigned int  i;

    if ((fl = um_flavour(v, n)) < 0)
        return UPD_OK;
    if (!um_fields(v, n, f, 5))
        return UPD_E_SYNTAX;

    /* A file this updater cannot place: fatal only in the installed one. */
    if ((cls = um_lookup(f[1].p, f[1].n, um_classes, UPD_CLASS_COUNT)) < 0)
        return (fl == st->want->flavour) ? UPD_E_CLASS : UPD_OK;
    if (!um_number(&f[2], &size))
        return UPD_E_NUMBER;
    if (!um_hex(&f[3], sha, 32))
        return UPD_E_HEX;
    if (!um_path(&f[4], 0))
        return UPD_E_PATH;

    st->flavours_with_files |= 1u << fl;
    if (cls == UPD_CLASS_CERTIFICATES)
        st->any_certs = 1;

    if (fl != st->want->flavour)
        return UPD_OK;

    for (i = 0; i < st->m->nfiles; i++)
    {
        if (st->files[i].cls == cls &&
            um_same_name(st->files[i].path, st->files[i].path_len,
                         f[4].p, f[4].n))
            return UPD_E_DUPLICATE;
    }

    if (st->m->nfiles >= st->max_files)
        return UPD_E_TOO_MANY;

    e = &st->files[st->m->nfiles++];
    e->path     = f[4].p;
    e->path_len = (unsigned short)f[4].n;
    e->cls      = (unsigned char)cls;
    e->size     = size;
    for (i = 0; i < 32; i++)
        e->sha256[i] = sha[i];

    return UPD_OK;
}

/* One record, `key` and `v` split at the first "=". */
static UpdResult um_record(UmState *st, const char *key, unsigned long kn,
                           const char *v, unsigned long vn)
{
    UmField f;
    int     c;

    f.p = v;
    f.n = vn;

    if (um_is(key, kn, "format"))
    {
        if (st->seen & UM_FORMAT)
            return UPD_E_DUPLICATE;
        if (!um_is(v, vn, "1"))
            return UPD_E_FORMAT;
        st->seen |= UM_FORMAT;
        return UPD_OK;
    }

    if (!(st->seen & UM_FORMAT))
        return UPD_E_FORMAT;

    if (um_is(key, kn, "file"))
        return um_file(st, v, vn);

    if (um_is(key, kn, "container"))
        return um_container(st, v, vn);

    if (um_is(key, kn, "product"))
    {
        if (st->seen & UM_PRODUCT)
            return UPD_E_DUPLICATE;
        st->seen |= UM_PRODUCT;
        st->have_product = um_is(v, vn, UPD_PRODUCT);
        return UPD_OK;
    }

    if (um_is(key, kn, "version"))
    {
        if (st->seen & UM_VERSION)
            return UPD_E_DUPLICATE;
        if (!upd_ver_parse(v, vn, &st->m->version))
            return UPD_E_VERSION;
        st->seen |= UM_VERSION;
        return UPD_OK;
    }

    if (um_is(key, kn, "channel"))
    {
        if (st->seen & UM_CHANNEL)
            return UPD_E_DUPLICATE;
        c = um_lookup(v, vn, um_channels, UPD_CHANNEL_COUNT);
        st->m->channel = (unsigned char)(c < 0 ? UM_CHANNEL_UNKNOWN : c);
        st->seen |= UM_CHANNEL;
        return UPD_OK;
    }

    if (um_is(key, kn, "certificates.prev"))
    {
        if (st->seen & UM_CERTS)
            return UPD_E_DUPLICATE;
        if (!um_hex(&f, st->m->certs_prev, 32))
            return UPD_E_HEX;
        st->seen |= UM_CERTS;
        st->m->has_certs_prev = 1;
        return UPD_OK;
    }

    return UPD_OK;                      /* a key from a later format       */
}

static int um_key_byte(int c)
{
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.';
}

UpdResult upd_manifest_parse(const char *text, unsigned long len,
                             const UpdWant *want, UpdManifest *m,
                             UpdFile *files, unsigned int max_files)
{
    UmState       st;
    unsigned long pos = 0;
    unsigned long lines = 0;
    int           ended = 0;
    UpdResult     r;

    if (text == 0 || want == 0 || m == 0 || (files == 0 && max_files != 0) ||
        want->flavour >= UPD_FLAVOUR_COUNT)
        return UPD_E_ARG;

    {
        unsigned char *b = (unsigned char *)m;
        unsigned long  i;

        for (i = 0; i < sizeof(*m); i++)
            b[i] = 0;
    }

    st.want                = want;
    st.m                   = m;
    st.files               = files;
    st.max_files           = max_files;
    st.seen                = 0;
    st.containers          = 0;
    st.flavours_with_files = 0;
    st.any_certs           = 0;
    st.have_product        = 0;

    while (pos < len)
    {
        const char   *line = text + pos;
        unsigned long n, k;

        if ((r = um_line(text, len, pos, &n)) != UPD_OK)
            return r;

        pos += n + 1;

        if (ended)
            return UPD_E_SYNTAX;        /* something after end=            */

        if (n > 0 && line[0] == '#')
        {
            lines++;
            continue;
        }

        for (k = 0; k < n && um_key_byte((unsigned char)line[k]); k++)
            ;
        if (k == 0 || k == n || line[k] != '=')
            return UPD_E_SYNTAX;

        if (um_is(line, k, "end"))
        {
            UmField       f;
            unsigned long count;

            if (!(st.seen & UM_FORMAT))
                return UPD_E_FORMAT;
            f.p = line + k + 1;
            f.n = n - k - 1;
            if (!um_number(&f, &count))
                return UPD_E_NUMBER;
            if (count != lines)
                return UPD_E_COUNT;
            ended = 1;
            continue;
        }

        if ((r = um_record(&st, line, k, line + k + 1, n - k - 1)) != UPD_OK)
            return r;

        lines++;
    }

    if (!ended)
        return UPD_E_TRUNCATED;

    if ((st.seen & (UM_PRODUCT | UM_VERSION | UM_CHANNEL)) !=
            (UM_PRODUCT | UM_VERSION | UM_CHANNEL) ||
        (st.any_certs && !(st.seen & UM_CERTS)) ||
        (st.flavours_with_files & ~st.containers) != 0)
        return UPD_E_MISSING;

    /* ---- the refusals ---- */

    if (!st.have_product)
        return UPD_E_PRODUCT;

    if (m->channel != want->channel ||
        (m->channel == UPD_CHANNEL_RELEASE && m->version.pre[0] != '\0'))
        return UPD_E_CHANNEL;

    if (upd_ver_cmp(&m->version, &want->installed) <= 0)
        return UPD_E_NOT_NEWER;

    if (!(st.containers & (1u << want->flavour)))
        return UPD_E_NO_FLAVOUR;

    return UPD_OK;
}


/* ------------------------------------------------- the signature file --- */

UpdResult upd_sig_parse(const char *text, unsigned long len,
                        UpdSigLine *lines, unsigned int *nlines)
{
    unsigned long pos = 0;
    unsigned int  count = 0;
    UpdResult     r;

    if (text == 0 || lines == 0 || nlines == 0)
        return UPD_E_ARG;

    *nlines = 0;

    while (pos < len)
    {
        const char   *line = text + pos;
        unsigned long n, k, i;
        UmField       f;
        unsigned int  j;

        if ((r = um_line(text, len, pos, &n)) != UPD_OK)
            return r;

        pos += n + 1;

        if (n < 4 || !um_is(line, 4, "key="))
            return UPD_E_SYNTAX;

        for (k = 4; k < n; k++)
        {
            int c = (unsigned char)line[k];

            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                  c == '-'))
                break;
        }

        if (k == 4 || k - 4 > UPD_KEYID_MAX || n - k < 5 ||
            !um_is(line + k, 5, " sig="))
            return UPD_E_SYNTAX;

        if (count >= UPD_SIG_LINES)
            return UPD_E_TOO_MANY;

        for (j = 0; j < count; j++)
        {
            if (um_is(line + 4, k - 4, lines[j].id))
                return UPD_E_DUPLICATE;
        }

        f.p = line + k + 5;
        f.n = n - k - 5;
        if (!um_hex(&f, lines[count].sig, 64))
            return UPD_E_HEX;

        for (i = 4; i < k; i++)
            lines[count].id[i - 4] = line[i];
        lines[count].id[k - 4] = '\0';

        count++;
    }

    if (count == 0)
        return UPD_E_TRUNCATED;

    *nlines = count;
    return UPD_OK;
}


/* ------------------------------------------------------------- words --- */

const char *upd_flavour_name(unsigned int f)
{
    return (f < UPD_FLAVOUR_COUNT) ? um_flavours[f] : 0;
}

const char *upd_class_name(unsigned int c)
{
    return (c < UPD_CLASS_COUNT) ? um_classes[c] : 0;
}

const char *upd_channel_name(unsigned int c)
{
    return (c < UPD_CHANNEL_COUNT) ? um_channels[c] : 0;
}

const char *upd_result_text(UpdResult r)
{
    return ((unsigned int)r < UPD_RESULT_COUNT) ? um_results[r] : 0;
}

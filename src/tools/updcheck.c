/*
 * updcheck: see updcheck.h.
 *
 * SPDX-License-Identifier: MIT
 */

#include "updcheck.h"

static int uc_lower(int c)
{
    return (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c;
}

/* `n` bytes of `a` equal the NUL-terminated `word`, case aside. */
static int uc_is(const char *a, unsigned long n, const char *word)
{
    unsigned long i;

    for (i = 0; i < n; i++)
    {
        if (word[i] == '\0' || uc_lower((unsigned char)a[i]) != uc_lower((unsigned char)word[i]))
            return 0;
    }

    return word[n] == '\0';
}

static void uc_setting(UpdSettings *s, const char *k, unsigned long klen,
                       const char *v, unsigned long vlen)
{
    unsigned long i;

    if (uc_is(k, klen, "auto"))
    {
        if (uc_is(v, vlen, "on"))
            s->auto_check = 1;
        else if (uc_is(v, vlen, "off"))
            s->auto_check = 0;
        else
            s->bad++;
    }
    else if (uc_is(k, klen, "channel"))
    {
        if (uc_is(v, vlen, "release"))
            s->channel = UPD_CHANNEL_RELEASE;
        else if (uc_is(v, vlen, "prerelease"))
            s->channel = UPD_CHANNEL_PRERELEASE;
        else
            s->bad++;
    }
    else if (uc_is(k, klen, "mirror"))
    {
        /* Room for the "/" it may need and the NUL. */
        if (vlen == 0 || vlen + 2 > sizeof(s->mirror))
        {
            s->bad++;
            return;
        }

        for (i = 0; i < vlen; i++)
        {
            if ((unsigned char)v[i] <= ' ' || (unsigned char)v[i] > '~')
            {
                s->mirror[0] = '\0';
                s->bad++;
                return;
            }
            s->mirror[i] = v[i];
        }

        if (s->mirror[vlen - 1] != '/')
            s->mirror[vlen++] = '/';
        s->mirror[vlen] = '\0';
    }
}

void upd_settings_parse(const char *text, unsigned long len, UpdSettings *s)
{
    unsigned long i = 0;

    s->auto_check = 1;
    s->channel    = -1;
    s->mirror[0]  = '\0';
    s->bad        = 0;

    if (text == 0)
        return;

    while (i < len)
    {
        unsigned long start = i, end, eq, k0, k1, v0, v1;

        while (i < len && text[i] != '\n')
            i++;
        end = i;
        if (i < len)
            i++;
        if (end > start && text[end - 1] == '\r')
            end--;

        k0 = start;
        while (k0 < end && (text[k0] == ' ' || text[k0] == '\t'))
            k0++;
        if (k0 == end || text[k0] == '#' || text[k0] == ';')
            continue;

        for (eq = k0; eq < end && text[eq] != '='; eq++)
            ;
        if (eq == end)
        {
            s->bad++;
            continue;
        }

        k1 = eq;
        while (k1 > k0 && (text[k1 - 1] == ' ' || text[k1 - 1] == '\t'))
            k1--;
        v0 = eq + 1;
        while (v0 < end && (text[v0] == ' ' || text[v0] == '\t'))
            v0++;
        v1 = end;
        while (v1 > v0 && (text[v1 - 1] == ' ' || text[v1 - 1] == '\t'))
            v1--;

        uc_setting(s, text + k0, k1 - k0, text + v0, v1 - v0);
    }
}

int upd_flavour_from_id(const char *id)
{
    static const char field[] = "profile ";
    const char       *p;
    unsigned long     n;

    if (id == 0)
        return -1;

    for (p = id; *p != '\0'; p++)
    {
        unsigned long i;

        for (i = 0; field[i] != '\0' && p[i] == field[i]; i++)
            ;
        if (field[i] != '\0')
            continue;
        /* A field begins the string or follows ", " or "(". */
        if (p != id && p[-1] != ' ' && p[-1] != '(')
            continue;

        p += sizeof(field) - 1;
        for (n = 0; p[n] != '\0' && p[n] != ')' && p[n] != ',' &&
                    p[n] != ' ' && p[n] != '\r' && p[n] != '\n'; n++)
            ;

        if (uc_is(p, n, "default"))
            return UPD_FLAVOUR_FULL;
        if (uc_is(p, n, "minimal"))
            return UPD_FLAVOUR_MINIMAL;
        if (uc_is(p, n, "micro"))
            return UPD_FLAVOUR_MICRO;
        if (uc_is(p, n, "fat"))
            return UPD_FLAVOUR_FAT;
        return -1;
    }

    return -1;
}

static int uc_append(char *out, unsigned long outlen, unsigned long *at,
                     const char *s)
{
    while (*s != '\0')
    {
        if (*at + 1 >= outlen)
            return 0;
        out[(*at)++] = *s++;
    }
    out[*at] = '\0';

    return 1;
}

int upd_url(char *out, unsigned long outlen, const char *mirror,
            unsigned int channel, const char *name)
{
    const char   *ch = upd_channel_name(channel);
    unsigned long at = 0;

    if (outlen == 0)
        return 0;
    out[0] = '\0';
    if (mirror == 0 || mirror[0] == '\0' || ch == 0 || name == 0)
        return 0;

    return uc_append(out, outlen, &at, mirror) &&
           uc_append(out, outlen, &at, ch) &&
           uc_append(out, outlen, &at, "/") &&
           uc_append(out, outlen, &at, name);
}

static int uc_leap(unsigned long y)
{
    return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
}

void upd_date(unsigned long days, char *out)
{
    static const unsigned char mdays[12] =
        { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    unsigned long y = 1978, m = 0, d;
    int           i;

    for (;;)
    {
        unsigned long n = uc_leap(y) ? 366 : 365;

        if (days < n)
            break;
        days -= n;
        y++;
    }

    for (;;)
    {
        unsigned long n = mdays[m] + (m == 1 && uc_leap(y) ? 1 : 0);

        if (days < n)
            break;
        days -= n;
        m++;
    }
    d = days + 1;
    m++;

    for (i = 3; i >= 0; i--)
    {
        out[i] = (char)('0' + y % 10);
        y /= 10;
    }
    out[4] = '-';
    out[5] = (char)('0' + m / 10);
    out[6] = (char)('0' + m % 10);
    out[7] = '-';
    out[8] = (char)('0' + d / 10);
    out[9] = (char)('0' + d % 10);
    out[10] = '\0';
}

UpdVerdict upd_check_verdict(const unsigned char *m, unsigned long mlen,
                             const char *sig, unsigned long siglen,
                             const UpdKey *keys, unsigned int nkeys,
                             const UpdWant *want, UpdManifest *out,
                             UpdFile *files, unsigned int max_files,
                             UpdResult *why)
{
    UpdResult r;

    r = upd_verify(m, mlen, sig, siglen, keys, nkeys);
    if (why != 0)
        *why = r;
    if (r == UPD_E_SIG_UNKNOWN_KEY)
        return UPD_V_UNKNOWN_KEY;
    if (r != UPD_OK)
        return UPD_V_SIGNATURE;

    r = upd_manifest_parse((const char *)m, mlen, want, out, files,
                           max_files);
    if (why != 0)
        *why = r;
    if (r == UPD_OK)
        return UPD_V_AVAILABLE;
    if (r == UPD_E_NOT_NEWER)
        return UPD_V_CURRENT;

    return UPD_V_MANIFEST;
}

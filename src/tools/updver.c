/*
 * updver: see updver.h.
 *
 * SPDX-License-Identifier: MIT
 */

#include "updver.h"

static int uv_digit(int c)
{
    return c >= '0' && c <= '9';
}

static int uv_alpha(int c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

/* One of MAJOR, MINOR, PATCH from s[*p] up to `stop` or the end.  */
static int uv_number(const char *s, unsigned long len, unsigned long *p,
                     char stop, unsigned long *out)
{
    unsigned long i = *p;
    unsigned long n = 0;

    if (i >= len || !uv_digit((unsigned char)s[i]))
        return 0;

    if (s[i] == '0' && i + 1 < len && uv_digit((unsigned char)s[i + 1]))
        return 0;                       /* leading zero                    */

    while (i < len && uv_digit((unsigned char)s[i]))
    {
        n = n * 10UL + (unsigned long)(s[i] - '0');
        if (n > UPD_VER_NUM_MAX)
            return 0;
        i++;
    }

    if (stop != '\0' && (i >= len || s[i] != stop))
        return 0;

    *out = n;
    *p   = i;
    return 1;
}

int upd_ver_parse(const char *s, unsigned long len, UpdVer *v)
{
    unsigned long p = 0;
    unsigned long n = 0;
    int           last = '.';           /* class of the last byte: . a 0  */
    int           zero = 0;             /* the digit run so far is "0"     */

    if (len == 0 || len >= UPD_VER_MAX)
        return 0;

    if (!uv_number(s, len, &p, '.', &v->major))
        return 0;
    p++;
    if (!uv_number(s, len, &p, '.', &v->minor))
        return 0;
    p++;
    if (!uv_number(s, len, &p, '\0', &v->patch))
        return 0;

    v->pre[0] = '\0';

    if (p == len)
        return 1;

    if (s[p] != '-')
        return 0;
    p++;

    if (p == len || len - p >= UPD_PRE_MAX)
        return 0;

    for (; p < len; p++)
    {
        int c = (unsigned char)s[p];

        if (c == '.')
        {
            if (last == '.')
                return 0;               /* empty part                      */
            last = '.';
        }
        else if (uv_digit(c))
        {
            if (last == '0' && zero)
                return 0;               /* leading zero                    */
            zero = (last != '0' && c == '0');
            last = '0';
        }
        else if (uv_alpha(c))
        {
            last = 'a';
        }
        else
        {
            return 0;
        }

        v->pre[n++] = (char)c;
    }

    if (last == '.')
        return 0;

    v->pre[n] = '\0';
    return 1;
}

static int uv_kind(int c)
{
    if (c == '.')
        return 0;
    if (uv_digit(c))
        return 1;
    return 2;
}

/* Prerelease parts: digit runs by value, letter runs by ASCII, a part that
   runs out first is the older. */
static int uv_pre_cmp(const char *a, const char *b)
{
    for (;;)
    {
        int ka, kb;
        unsigned long la = 0, lb = 0, i;

        if (a[0] == '\0' || b[0] == '\0')
            return (a[0] != '\0') - (b[0] != '\0');

        ka = uv_kind((unsigned char)a[0]);
        kb = uv_kind((unsigned char)b[0]);
        if (ka != kb)
            return ka - kb;

        if (ka == 0)
        {
            a++;
            b++;
            continue;
        }

        while (a[la] != '\0' && uv_kind((unsigned char)a[la]) == ka)
            la++;
        while (b[lb] != '\0' && uv_kind((unsigned char)b[lb]) == kb)
            lb++;

        /* No leading zeros, so a longer digit run is the larger number. */
        if (ka == 1 && la != lb)
            return (la > lb) ? 1 : -1;

        for (i = 0; i < la && i < lb; i++)
        {
            if (a[i] != b[i])
                return ((unsigned char)a[i] > (unsigned char)b[i]) ? 1 : -1;
        }

        if (la != lb)
            return (la > lb) ? 1 : -1;

        a += la;
        b += lb;
    }
}

int upd_ver_cmp(const UpdVer *a, const UpdVer *b)
{
    if (a->major != b->major)
        return (a->major > b->major) ? 1 : -1;
    if (a->minor != b->minor)
        return (a->minor > b->minor) ? 1 : -1;
    if (a->patch != b->patch)
        return (a->patch > b->patch) ? 1 : -1;

    /* A release is newer than any of its prereleases. */
    if (a->pre[0] == '\0' || b->pre[0] == '\0')
        return (a->pre[0] == '\0') - (b->pre[0] == '\0');

    return uv_pre_cmp(a->pre, b->pre);
}

static char *uv_put(char *o, unsigned long n)
{
    char t[8];
    int  k = 0;

    do
    {
        t[k++] = (char)('0' + n % 10UL);
        n /= 10UL;
    } while (n != 0);

    while (k > 0)
        *o++ = t[--k];

    return o;
}

void upd_ver_format(const UpdVer *v, char *out)
{
    const char *p;

    out = uv_put(out, v->major);
    *out++ = '.';
    out = uv_put(out, v->minor);
    *out++ = '.';
    out = uv_put(out, v->patch);

    if (v->pre[0] != '\0')
    {
        *out++ = '-';
        for (p = v->pre; *p != '\0'; p++)
            *out++ = *p;
    }

    *out = '\0';
}

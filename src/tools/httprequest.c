/*
 * Small, portable HTTP request-value policies used by httpd.
 *
 * Includes nothing beyond its own contract so the host tier can exercise the
 * exact code the m68k server runs.
 *
 * SPDX-License-Identifier: MIT
 */

#include "httprequest.h"

static int hr_lower(int c)
{
    return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

static int hr_nicmp(const char *a, const char *b, unsigned long n)
{
    while (n-- != 0UL)
    {
        int ca = hr_lower((unsigned char)*a++);
        int cb = hr_lower((unsigned char)*b++);

        if (ca != cb)
            return ca - cb;
        if (ca == '\0')
            return 0;
    }

    return 0;
}

int http_request_query_take(const char *target)
{
    unsigned long i = 0;

    if (target == 0)
        return 0;

    while (target[i] != '\0' && target[i] != '?')
        i++;

    if (target[i] != '?')
        return 0;

    i++;
    while (target[i] != '\0')
    {
        unsigned long start = i;

        while (target[i] != '\0' && target[i] != '&')
            i++;

        if (i - start == 6UL &&
            hr_nicmp(&target[start], "take=1", 6UL) == 0)
            return 1;

        if (target[i] == '&')
            i++;
    }

    return 0;
}

int http_request_query_session(const char *target)
{
    unsigned long i = 0;
    int seen = 0;
    int slot = 0;

    if (target == 0)
        return 0;
    while (target[i] != '\0' && target[i] != '?')
        i++;
    if (target[i] != '?')
        return 0;

    i++;
    while (target[i] != '\0')
    {
        unsigned long start = i;
        unsigned long n;

        while (target[i] != '\0' && target[i] != '&')
            i++;
        n = i - start;
        if (n >= 8UL && hr_nicmp(&target[start], "session=", 8UL) == 0)
        {
            if (seen || n != 9UL ||
                (target[start + 8UL] != '0' && target[start + 8UL] != '1'))
                return -1;
            seen = 1;
            slot = target[start + 8UL] - '0';
        }
        else if (n == 7UL && hr_nicmp(&target[start], "session", 7UL) == 0)
            return -1;

        if (target[i] == '&')
            i++;
    }
    return slot;
}

unsigned long http_request_timeout(const char *value, unsigned long cap)
{
    unsigned long secs = 0;

    if (value == 0)
        return 0;

    while (*value == ' ' || *value == '\t')
        value++;

    if (hr_nicmp(value, "infinite", 8UL) == 0)
        return cap;

    if (hr_nicmp(value, "second-", 7UL) != 0)
        return 0;

    value += 7;
    while (*value >= '0' && *value <= '9')
    {
        unsigned long digit = (unsigned long)(*value++ - '0');

        /* The caller caps the grant anyway.  Saturating here means a long
           hostile field cannot wrap into a short, apparently valid lease. */
        if (secs > cap / 10UL ||
            (secs == cap / 10UL && digit > cap % 10UL))
            return cap;

        secs = secs * 10UL + digit;
    }

    return secs;
}

int http_request_accepts_gzip(const char *v)
{
    if (v == 0)
        return 0;

    while (*v != '\0')
    {
        int is_gzip;
        int refused = 0;

        while (*v == ' ' || *v == '\t' || *v == ',')
            v++;

        is_gzip = (hr_nicmp(v, "gzip", 4UL) == 0 &&
                   (v[4] == '\0' || v[4] == ',' || v[4] == ';' ||
                    v[4] == ' '  || v[4] == '\t'));

        /* To the end of this coding, reading any q= on the way past.  A
           quality of zero is a refusal and 0.000 is still zero. */
        while (*v != '\0' && *v != ',')
        {
            if (*v == ';')
            {
                const char *q = v + 1;

                while (*q == ' ' || *q == '\t')
                    q++;

                if ((*q == 'q' || *q == 'Q') && q[1] == '=')
                {
                    int zero = 1;

                    for (q += 2; *q != '\0' && *q != ',' && *q != ';'; q++)
                        if (*q >= '1' && *q <= '9')
                            zero = 0;

                    if (zero)
                        refused = 1;
                }
            }
            v++;
        }

        if (is_gzip)
            return refused ? 0 : 1;
    }

    return 0;
}

unsigned long http_request_lock_tokens(const char *value, char *out,
                                       unsigned long stride,
                                       unsigned long count)
{
    unsigned long n = 0;
    unsigned long i;

    if (value == 0 || out == 0 || stride == 0UL || count == 0UL)
        return 0;

    for (i = 0; i < count; i++)
        out[i * stride] = '\0';

    while (*value != '\0' && n < count)
    {
        if (*value == '<')
        {
            const char   *start = ++value;
            unsigned long len = 0;

            while (*value != '\0' && *value != '>')
            {
                value++;
                len++;
            }

            /* A tagged list names a URL the same way, so only values with
               the opaque-token scheme are credentials. */
            if (hr_nicmp(start, "opaquelocktoken:", 16UL) == 0 &&
                len < stride)
            {
                char *slot = out + n * stride;

                for (i = 0; i < len; i++)
                    slot[i] = start[i];
                slot[len] = '\0';
                n++;
            }

            if (*value == '>')
                value++;
        }
        else
        {
            value++;
        }
    }

    return n;
}

/* One decimal path segment, or -1.  The iperf endpoint's seconds. */
long http_request_decimal(const char *s, unsigned long len)
{
    unsigned long i;
    unsigned long v = 0;

    if (len == 0 || len > 9)
        return -1;

    for (i = 0; i < len; i++)
    {
        if (s[i] < '0' || s[i] > '9')
            return -1;
        v = v * 10UL + (unsigned long)(s[i] - '0');
    }

    return (long)v;
}

/* A dotted quad, and nothing else.  The iperf endpoint's peer. */
int http_request_dotted(const char *s, unsigned long *out)
{
    unsigned long addr = 0;
    unsigned long i;

    for (i = 0; i < 4; i++)
    {
        unsigned long v = 0;
        unsigned long d = 0;

        while (*s >= '0' && *s <= '9' && d < 3)
        {
            v = v * 10UL + (unsigned long)(*s++ - '0');
            d++;
        }

        if (d == 0 || v > 255UL)
            return 0;

        addr = (addr << 8) | v;

        if (i < 3)
        {
            if (*s != '.')
                return 0;
            s++;
        }
    }

    if (*s != '\0')
        return 0;

    *out = addr;
    return 1;
}

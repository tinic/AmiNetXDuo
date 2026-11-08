/*
 * The time zone the Locale preferences show.  See tooltz.h.
 *
 * SPDX-License-Identifier: MIT
 */

#include "tooltz.h"

long tooltz_minutes_west(long loc_gmtoffset, int prefs_loaded)
{
    return prefs_loaded ? loc_gmtoffset : TOOLTZ_DEFAULT_WEST;
}

unsigned long tooltz_local_secs(unsigned long utc_secs, long minutes_west)
{
    /* 32-bit arithmetic: no 64-bit helpers in a 68000 command. */
    unsigned long shift = (unsigned long)(minutes_west < 0 ? -minutes_west
                                                           : minutes_west)
                          * 60UL;

    if (minutes_west >= 0)
        return (utc_secs >= shift) ? utc_secs - shift : utc_secs;
    return (utc_secs <= 0xffffffffUL - shift) ? utc_secs + shift : utc_secs;
}

/* ------------------------------------------------------------ POSIX TZ -- */

#define TZ_DAY  86400UL

/* A changeover rule: Jn (1..365, no Feb 29), n (0..365) or Mm.w.d. */
typedef struct
{
    char kind;                  /* 'J', 'D' or 'M'                          */
    int  a, b, c;               /* n, or month, week, weekday               */
    long time;                  /* seconds after local midnight             */
} TzRule;

static int tz_digit(char c)
{
    return c >= '0' && c <= '9';
}

static int tz_leap(unsigned long y)
{
    return (y % 4UL == 0UL && y % 100UL != 0UL) || y % 400UL == 0UL;
}

/* A number of at most `max`; 0 when there is none or it is too big. */
static int tz_num(const char **pp, long max, long *out)
{
    const char *p = *pp;
    long        n = 0;

    if (!tz_digit(*p))
        return 0;
    while (tz_digit(*p))
    {
        n = n * 10L + (long)(*p++ - '0');
        if (n > max)
            return 0;
    }
    *out = n;
    *pp  = p;
    return 1;
}

/* [+-]hh[:mm[:ss]], hours up to `maxh`, as seconds. */
static int tz_hms(const char **pp, long maxh, long *out)
{
    const char *p    = *pp;
    long        sign = 1L;
    long        h, m = 0, s = 0;

    if (*p == '+' || *p == '-')
        sign = (*p++ == '-') ? -1L : 1L;
    if (!tz_num(&p, maxh, &h))
        return 0;
    if (*p == ':')
    {
        p++;
        if (!tz_num(&p, 59L, &m))
            return 0;
        if (*p == ':')
        {
            p++;
            if (!tz_num(&p, 59L, &s))
                return 0;
        }
    }
    *out = sign * (h * 3600L + m * 60L + s);
    *pp  = p;
    return 1;
}

/* Three or more letters, or <anything but '>'>. */
static int tz_name(const char **pp)
{
    const char *p = *pp;
    int         n = 0;

    if (*p == '<')
    {
        p++;
        while (*p != '\0' && *p != '>')
        {
            p++;
            n++;
        }
        if (*p++ != '>')
            return 0;
    }
    else
    {
        while ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z'))
        {
            p++;
            n++;
        }
    }
    *pp = p;
    return n >= 3;
}

static int tz_rule(const char **pp, TzRule *r)
{
    const char *p = *pp;
    long        a, b = 0, c = 0;

    if (*p == 'M')
    {
        p++;
        if (!tz_num(&p, 12L, &a) || a < 1 || *p++ != '.' ||
            !tz_num(&p, 5L, &b) || b < 1 || *p++ != '.' ||
            !tz_num(&p, 6L, &c))
            return 0;
        r->kind = 'M';
    }
    else if (*p == 'J')
    {
        p++;
        if (!tz_num(&p, 365L, &a) || a < 1)
            return 0;
        r->kind = 'J';
    }
    else
    {
        if (!tz_num(&p, 365L, &a))
            return 0;
        r->kind = 'D';
    }
    r->a    = (int)a;
    r->b    = (int)b;
    r->c    = (int)c;
    r->time = 2L * 3600L;
    if (*p == '/')
    {
        p++;
        if (!tz_hms(&p, 167L, &r->time))
            return 0;
    }
    *pp = p;
    return 1;
}

/* Days from 1978-01-01 to January 1st of `year`. */
static unsigned long tz_year_days(unsigned long year)
{
    unsigned long d = 0UL;
    unsigned long y;

    for (y = 1978UL; y < year; y++)
        d += tz_leap(y) ? 366UL : 365UL;
    return d;
}

/* Day of the year, from 0, that `r` falls on in `year`. */
static unsigned long tz_rule_day(const TzRule *r, unsigned long year)
{
    static const unsigned char mdays[12] =
        { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    unsigned long leap = (unsigned long)tz_leap(year);
    unsigned long first = 0UL;
    unsigned long len;
    unsigned long wday;
    unsigned long day;
    int           m;

    if (r->kind == 'J')
        return (unsigned long)(r->a - 1) + ((leap && r->a > 59) ? 1UL : 0UL);
    if (r->kind == 'D')
        return (unsigned long)r->a;

    for (m = 0; m < r->a - 1; m++)
        first += mdays[m] + ((m == 1) ? leap : 0UL);
    len = mdays[r->a - 1] + ((r->a == 2) ? leap : 0UL);

    /* 1978-01-01 was a Sunday. */
    wday = (tz_year_days(year) + first) % 7UL;
    day  = ((unsigned long)r->c + 7UL - wday) % 7UL
           + (unsigned long)(r->b - 1) * 7UL;
    while (day >= len)
        day -= 7UL;
    return first + day;
}

/* The UTC instant of `r` in `year`, for a clock `west` seconds behind UTC. */
static unsigned long tz_rule_utc(const TzRule *r, unsigned long year,
                                 long west)
{
    return (tz_year_days(year) + tz_rule_day(r, year)) * TZ_DAY
           + (unsigned long)r->time + (unsigned long)west;
}

int tooltz_posix_west(const char *tz, unsigned long utc_secs,
                      long *seconds_west)
{
    const char   *p = tz;
    long          std, dst;
    TzRule        start, end;
    unsigned long local, days, year, t0, t1;
    int           in_dst;

    if (p == 0)
        return 0;
    if (*p == ':')
        return 0;                       /* a zoneinfo file: not here     */
    if (!tz_name(&p) || !tz_hms(&p, 24L, &std))
        return 0;

    if (*p == '\0')
    {
        *seconds_west = std;
        return 1;
    }

    if (!tz_name(&p))
        return 0;
    dst = std - 3600L;
    if (*p != ',' && *p != '\0' && !tz_hms(&p, 24L, &dst))
        return 0;

    if (*p == '\0')
    {
        /* No rules: SetDST names the DST zone only while it is in effect
           and rewrites TZONE at each changeover. */
        *seconds_west = dst;
        return 1;
    }
    if (*p++ != ',' || !tz_rule(&p, &start) || *p++ != ',' ||
        !tz_rule(&p, &end) || *p != '\0')
        return 0;

    /* The year by standard local time; a few hours either side of New
       Year changes nothing, the rules are months away from it. */
    local = utc_secs - (unsigned long)std;
    days  = local / TZ_DAY;
    for (year = 1978UL; ; year++)
    {
        unsigned long n = tz_leap(year) ? 366UL : 365UL;

        if (days < n)
            break;
        days -= n;
    }

    t0 = tz_rule_utc(&start, year, std);
    t1 = tz_rule_utc(&end, year, dst);
    if (t0 < t1)
        in_dst = utc_secs >= t0 && utc_secs < t1;
    else
        in_dst = utc_secs >= t0 || utc_secs < t1;   /* southern summer */

    *seconds_west = in_dst ? dst : std;
    return 1;
}

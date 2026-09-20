/* HTTP and WebDAV dates, kept independent of the server state machine.
 * SPDX-License-Identifier: MIT
 */

#include "httpdate.h"

#include <libraries/locale.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <proto/locale.h>

#define HTTPD_AMIGA_EPOCH  252460800UL

/* proto/locale.h's inlines require this symbol. */
struct LocaleBase *LocaleBase;

static LONG httpd_gmt_west;       /* minutes west of GMT, from locale */

/* AmigaOS keeps local time and HTTP dates are GMT.  locale.library is V38, so
   a 2.04 machine has none and the dates are then local time labelled GMT. */
VOID httpd_read_gmt_offset(VOID)
{
    struct Locale *locale;

    httpd_gmt_west = 0;

    LocaleBase = (struct LocaleBase *)
                 OpenLibrary((CONST_STRPTR)"locale.library", 38UL);
    if (LocaleBase == NULL)
        return;

    locale = OpenLocale(NULL);          /* NULL: the current preferences */
    if (locale != NULL)
    {
        httpd_gmt_west = (LONG)locale->loc_GMTOffset;
        CloseLocale(locale);
    }

    CloseLibrary((struct Library *)LocaleBase);
    LocaleBase = NULL;
}

/* A DateStamp as seconds since 1970, in GMT. */
ULONG httpd_stamp_secs(const struct DateStamp *ds)
{
    ULONG secs;

    if (ds->ds_Days < 0 || ds->ds_Minute < 0 || ds->ds_Tick < 0)
        return HTTPD_AMIGA_EPOCH;

    secs  = (ULONG)ds->ds_Days * 86400UL;
    secs += (ULONG)ds->ds_Minute * 60UL;
    secs += (ULONG)ds->ds_Tick / (ULONG)TICKS_PER_SECOND;
    secs += HTTPD_AMIGA_EPOCH;

    /* loc_GMTOffset is minutes west, so GMT is later than local time here. */
    if (httpd_gmt_west > 0)
        secs += (ULONG)httpd_gmt_west * 60UL;
    else if (httpd_gmt_west < 0)
    {
        ULONG east = (ULONG)(-httpd_gmt_west) * 60UL;

        secs = (secs > east) ? (secs - east) : 0UL;
    }

    return secs;
}

/* Seconds of local time since the Amiga epoch.  It can step backwards if the
   clock is set, which the caller handles by treating any backwards step as the
   present moment rather than as an expiry. */
ULONG httpd_now(VOID)
{
    struct DateStamp ds;

    ds.ds_Days   = 0;
    ds.ds_Minute = 0;
    ds.ds_Tick   = 0;
    (VOID)DateStamp(&ds);

    return (ULONG)ds.ds_Days * 86400UL + (ULONG)ds.ds_Minute * 60UL +
           (ULONG)ds.ds_Tick / (ULONG)TICKS_PER_SECOND;
}

/* Seconds since 1970 to a calendar date. */
static VOID httpd_civil(ULONG secs, LONG *y, LONG *mo, LONG *d,
                        LONG *h, LONG *mi, LONG *s, LONG *dow)
{
    static const LONG mdays[12] =
        { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    ULONG days = secs / 86400UL;
    ULONG rem  = secs % 86400UL;
    LONG  year = 1970;
    LONG  month;

    *h  = (LONG)(rem / 3600UL);
    *mi = (LONG)((rem % 3600UL) / 60UL);
    *s  = (LONG)(rem % 60UL);
    *dow = (LONG)((days + 4UL) % 7UL);  /* 1970-01-01 was Thursday. */

    for (;;)
    {
        ULONG len = ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)
                        ? 366UL : 365UL;

        if (days < len)
            break;
        days -= len;
        year++;
    }

    for (month = 0; month < 12; month++)
    {
        ULONG len = (ULONG)mdays[month];

        if (month == 1 &&
            ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0))
            len = 29UL;
        if (days < len)
            break;
        days -= len;
    }

    *y  = year;
    *mo = month + 1;
    *d  = (LONG)days + 1;
}

static VOID httpd_two(char *out, LONG value)
{
    out[0] = (char)('0' + ((value / 10) % 10));
    out[1] = (char)('0' + (value % 10));
}

VOID httpd_rfc1123(ULONG secs, char *out)
{
    static const char *const dows[7] =
        { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    static const char *const months[12] =
        { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
          "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    LONG y, mo, d, h, mi, s, dow;
    ULONG n = 0;

    httpd_civil(secs, &y, &mo, &d, &h, &mi, &s, &dow);

    out[n++] = dows[dow][0];
    out[n++] = dows[dow][1];
    out[n++] = dows[dow][2];
    out[n++] = ',';
    out[n++] = ' ';
    httpd_two(&out[n], d);          n += 2;
    out[n++] = ' ';
    out[n++] = months[mo - 1][0];
    out[n++] = months[mo - 1][1];
    out[n++] = months[mo - 1][2];
    out[n++] = ' ';
    httpd_two(&out[n], y / 100);    n += 2;
    httpd_two(&out[n], y % 100);    n += 2;
    out[n++] = ' ';
    httpd_two(&out[n], h);          n += 2;
    out[n++] = ':';
    httpd_two(&out[n], mi);         n += 2;
    out[n++] = ':';
    httpd_two(&out[n], s);          n += 2;
    out[n++] = ' ';
    out[n++] = 'G';
    out[n++] = 'M';
    out[n++] = 'T';
    out[n]   = '\0';
}

VOID httpd_iso8601(ULONG secs, char *out)
{
    LONG y, mo, d, h, mi, s, dow;
    ULONG n = 0;

    httpd_civil(secs, &y, &mo, &d, &h, &mi, &s, &dow);
    httpd_two(&out[n], y / 100);    n += 2;
    httpd_two(&out[n], y % 100);    n += 2;
    out[n++] = '-';
    httpd_two(&out[n], mo);         n += 2;
    out[n++] = '-';
    httpd_two(&out[n], d);          n += 2;
    out[n++] = 'T';
    httpd_two(&out[n], h);          n += 2;
    out[n++] = ':';
    httpd_two(&out[n], mi);         n += 2;
    out[n++] = ':';
    httpd_two(&out[n], s);          n += 2;
    out[n++] = 'Z';
    out[n]   = '\0';
}

static BOOL httpd_leap(LONG year)
{
    return ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)
               ? TRUE : FALSE;
}

static ULONG httpd_days_from_civil(LONG y, LONG mo, LONG d)
{
    static const ULONG cum[12] =
        { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
    ULONG days = 0;
    LONG  year;

    for (year = 1970; year < y; year++)
        days += httpd_leap(year) ? 366UL : 365UL;
    days += cum[mo - 1];
    if (mo > 2 && httpd_leap(y))
        days++;
    return days + (ULONG)(d - 1);
}

static UBYTE httpd_fold(UBYTE c)
{
    return (c >= 'A' && c <= 'Z') ? (UBYTE)(c + ('a' - 'A')) : c;
}

static BOOL httpd_month_equal(const char *a, const char *b)
{
    ULONG i;

    for (i = 0UL; i < 3UL; i++)
    {
        if (a[i] == '\0' || httpd_fold((UBYTE)a[i]) !=
                             httpd_fold((UBYTE)b[i]))
            return FALSE;
    }

    return TRUE;
}

static LONG httpd_month(const char *name)
{
    static const char *const months[12] =
        { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
          "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    LONG i;

    for (i = 0; i < 12; i++)
    {
        if (httpd_month_equal(name, months[i]))
            return i + 1;
    }
    return 0;
}

static ULONG httpd_digits(const char **p, ULONG count)
{
    const char *s = *p;
    ULONG value = 0;
    ULONG i;

    for (i = 0; i < count && s[i] >= '0' && s[i] <= '9'; i++)
        value = (value * 10UL) + (ULONG)(s[i] - '0');
    *p = s + i;
    return value;
}

BOOL httpd_parse_rfc1123(const char *text, struct DateStamp *ds)
{
    LONG day, month, year;
    ULONG h, mi, s, secs;

    while (*text == ' ')
        text++;
    if (text[0] != '\0' && text[1] != '\0' && text[2] != '\0' &&
        text[3] == ',')
        text += 4;
    while (*text == ' ')
        text++;

    day = (LONG)httpd_digits(&text, 2);
    while (*text == ' ' || *text == '-')
        text++;
    month = httpd_month(text);
    if (month == 0 || day < 1 || day > 31)
        return FALSE;
    text += 3;
    while (*text == ' ' || *text == '-')
        text++;
    year = (LONG)httpd_digits(&text, 4);
    if (year < 1978 || year > 2100)
        return FALSE;
    while (*text == ' ')
        text++;

    h = httpd_digits(&text, 2);
    if (*text == ':')
        text++;
    mi = httpd_digits(&text, 2);
    if (*text == ':')
        text++;
    s = httpd_digits(&text, 2);
    if (h > 23UL || mi > 59UL || s > 60UL)
        return FALSE;

    secs = httpd_days_from_civil(year, month, day) * 86400UL;
    secs += (h * 3600UL) + (mi * 60UL) + s;
    if (httpd_gmt_west > 0)
    {
        ULONG west = (ULONG)httpd_gmt_west * 60UL;
        if (secs < west)
            return FALSE;
        secs -= west;
    }
    else if (httpd_gmt_west < 0)
        secs += (ULONG)(-httpd_gmt_west) * 60UL;

    if (secs < HTTPD_AMIGA_EPOCH)
        return FALSE;
    secs -= HTTPD_AMIGA_EPOCH;

    ds->ds_Days   = (LONG)(secs / 86400UL);
    ds->ds_Minute = (LONG)((secs % 86400UL) / 60UL);
    ds->ds_Tick   = (LONG)((secs % 60UL) * (ULONG)TICKS_PER_SECOND);
    return TRUE;
}

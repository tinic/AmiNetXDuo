/* HTTP and WebDAV dates, kept independent of the server state machine.
 * SPDX-License-Identifier: MIT
 */

#include "httpdate.h"

#include <libraries/locale.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <proto/locale.h>

#include "tooltz.h"

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
        BPTR prefs = Lock((CONST_STRPTR)"ENV:Sys/locale.prefs", ACCESS_READ);

        httpd_gmt_west = (LONG)tooltz_minutes_west(locale->loc_GMTOffset,
                                                   prefs != (BPTR)0);
        if (prefs != (BPTR)0) UnLock(prefs);
        CloseLocale(locale);
    }

    CloseLibrary((struct Library *)LocaleBase);
    LocaleBase = NULL;
}

/* The last second 32 bits hold: Sun, 07 Feb 2106 06:28:15 GMT.  A later
   date is given as this one rather than wrapped into the 1970s (F-217). */
#define HTTPD_SECS_MAX     0xffffffffULL

/* A DateStamp as seconds since 1970, in GMT.  Summed in 64 bits and clamped
   once, so a zone east of GMT still brings a date back under the limit. */
ULONG httpd_stamp_secs(const struct DateStamp *ds)
{
    unsigned long long secs;

    if (ds->ds_Days < 0 || ds->ds_Minute < 0 || ds->ds_Tick < 0)
        return HTTPD_AMIGA_EPOCH;

    secs  = (unsigned long long)ds->ds_Days * 86400ULL;
    secs += (unsigned long long)ds->ds_Minute * 60ULL;
    secs += (unsigned long long)ds->ds_Tick / TICKS_PER_SECOND;
    secs += HTTPD_AMIGA_EPOCH;

    /* loc_GMTOffset is minutes west, so GMT is later than local time here. */
    if (httpd_gmt_west > 0)
        secs += (unsigned long long)httpd_gmt_west * 60ULL;
    else if (httpd_gmt_west < 0)
    {
        unsigned long long east = (unsigned long long)(-httpd_gmt_west) * 60ULL;

        secs = (secs > east) ? (secs - east) : 0ULL;
    }

    return (secs > HTTPD_SECS_MAX) ? (ULONG)HTTPD_SECS_MAX : (ULONG)secs;
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

/* The length of a month, with February adjusted for a leap year.  Parsed days
   are checked against this so a date the civil roll would silently move into
   the next month is refused instead. */
static LONG httpd_days_in_month(LONG month, LONG year)
{
    static const LONG mdays[12] =
        { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };

    if (month == 2 && httpd_leap(year))
        return 29;
    return mdays[month - 1];
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
    /* The day must exist in that month: days_from_civil would otherwise roll
       an invalid day (a 29th in a non-leap February, any 30th or 31st
       February, and 31 April) silently into the next month, and a PROPPATCH
       would write that wrong date. */
    if (day > httpd_days_in_month(month, year))
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

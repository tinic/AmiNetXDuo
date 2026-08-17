/*
 * Turning the parts of a certificate into something a person can be shown.
 *
 * The interface and the reasoning are in tls_certfmt.h.  Nothing here reaches
 * an Amiga header, an nx_secure header or a libc function; <stddef.h> is for
 * NULL and is the compiler's own.  That is what makes it testable on the host,
 * and it is the whole reason it is not in tls_cert.c.
 *
 * SPDX-License-Identifier: MIT
 */

#include "tls_certfmt.h"

#include <stddef.h>

/* -------------------------------------------------------------- output --- */

/*
 * One byte into the buffer, if there is a buffer and it fits, and counted
 * either way.  The count is the return value of every function here, so a
 * measuring pass (dst == NULL) and a writing pass cannot disagree about the
 * length: they run the same code and only this differs.
 *
 * `size` always reserves the last byte for the terminator, so a caller that
 * allocates the measured length plus one gets the whole string and a caller
 * that allocates less gets a truncated one rather than a walked-over heap.
 */
static void tls_fmt_put(char *dst, unsigned long size, unsigned long *used,
                        char c)
{
    if (dst != NULL && size > 0 && (*used + 1) < size)
        dst[*used] = c;

    (*used)++;
}

/*
 * A byte of a name.  Control characters and DEL become '?': the value comes
 * off the wire, it ends up in a requester title, and an embedded NUL would
 * end the string wherever the certificate decided to put one.
 */
static void tls_fmt_put_value(char *dst, unsigned long size, unsigned long *used,
                              const unsigned char *value, unsigned long length)
{
    unsigned long i;

    for (i = 0; i < length; i++)
    {
        unsigned char b = value[i];

        tls_fmt_put(dst, size, used,
                    (b < 0x20u || b == 0x7Fu) ? '?' : (char)b);
    }
}

static void tls_fmt_terminate(char *dst, unsigned long size, unsigned long used)
{
    if (dst == NULL || size == 0)
        return;

    dst[(used < size) ? used : (size - 1)] = '\0';
}

/* ---------------------------------------------------------------- names --- */

unsigned long tls_cert_name_format(char *dst, unsigned long size,
                                   const TLSCertAttr *attrs,
                                   unsigned long count)
{
    unsigned long used = 0;
    unsigned long i;

    if (attrs != NULL)
    {
        for (i = 0; i < count; i++)
        {
            const char *label = attrs[i].ca_Label;

            if (attrs[i].ca_Value == NULL || attrs[i].ca_Length == 0 ||
                label == NULL)
            {
                continue;
            }

            if (used > 0)
            {
                tls_fmt_put(dst, size, &used, ',');
                tls_fmt_put(dst, size, &used, ' ');
            }

            while (*label != '\0')
                tls_fmt_put(dst, size, &used, *label++);
            tls_fmt_put(dst, size, &used, '=');

            tls_fmt_put_value(dst, size, &used, attrs[i].ca_Value,
                              attrs[i].ca_Length);
        }
    }

    tls_fmt_terminate(dst, size, used);

    return used;
}

unsigned long tls_cert_list_append(char *dst, unsigned long size,
                                   unsigned long used,
                                   const unsigned char *value,
                                   unsigned long length)
{
    if (value == NULL || length == 0)
        return used;

    if (used > 0)
    {
        tls_fmt_put(dst, size, &used, ',');
        tls_fmt_put(dst, size, &used, ' ');
    }

    tls_fmt_put_value(dst, size, &used, value, length);
    tls_fmt_terminate(dst, size, used);

    return used;
}

/* ---------------------------------------------------------------- dates --- */

#define TLS_FMT_DIGIT(c)    ((c) >= '0' && (c) <= '9')

static unsigned long tls_fmt_digits(const unsigned char *p, unsigned long n)
{
    unsigned long value = 0;
    unsigned long i;

    for (i = 0; i < n; i++)
        value = (value * 10UL) + (unsigned long)(p[i] - '0');

    return value;
}

static int tls_fmt_all_digits(const unsigned char *p, unsigned long n)
{
    unsigned long i;

    for (i = 0; i < n; i++)
    {
        if (!TLS_FMT_DIGIT(p[i]))
            return 0;
    }

    return 1;
}

static int tls_fmt_leap(unsigned long year)
{
    return ((year % 4UL) == 0 && (year % 100UL) != 0) || ((year % 400UL) == 0);
}

/* Days in all the months before this one, ignoring February's extra day, which
   the caller adds.  Indexed by month - 1. */
static const unsigned short tls_fmt_days_before[12] =
    { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };

static const unsigned char tls_fmt_days_in[12] =
    { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };

/* Leap years strictly before `year`, counting from year 1. */
static unsigned long tls_fmt_leaps_before(unsigned long year)
{
    unsigned long y = year - 1UL;

    return (y / 4UL) - (y / 100UL) + (y / 400UL);
}

unsigned long tls_cert_asn1_time(const unsigned char *asn1,
                                 unsigned long length, int generalized)
{
    unsigned long year, month, day, hour, minute, second;
    unsigned long index;
    unsigned long days;

    if (asn1 == NULL)
        return 0;

    if (generalized)
    {
        /* YYYYMMDDhhmm[ss]Z */
        if (length < 13 || !tls_fmt_all_digits(asn1, 12))
            return 0;

        year  = tls_fmt_digits(&asn1[0], 4);
        month = tls_fmt_digits(&asn1[4], 2);
        day   = tls_fmt_digits(&asn1[6], 2);
        hour  = tls_fmt_digits(&asn1[8], 2);
        minute = tls_fmt_digits(&asn1[10], 2);
        index = 12;
    }
    else
    {
        /* YYMMDDhhmm[ss]Z */
        if (length < 11 || !tls_fmt_all_digits(asn1, 10))
            return 0;

        year  = tls_fmt_digits(&asn1[0], 2);
        year += (year < 50UL) ? 2000UL : 1900UL;
        month = tls_fmt_digits(&asn1[2], 2);
        day   = tls_fmt_digits(&asn1[4], 2);
        hour  = tls_fmt_digits(&asn1[6], 2);
        minute = tls_fmt_digits(&asn1[8], 2);
        index = 10;
    }

    second = 0;
    if (asn1[index] != 'Z')
    {
        /* Seconds are optional, and what follows them is not. */
        if ((index + 3) > length || !tls_fmt_all_digits(&asn1[index], 2))
            return 0;

        second = tls_fmt_digits(&asn1[index], 2);
        index += 2;

        if (index >= length || asn1[index] != 'Z')
            return 0;
    }

    /* A certificate dated before the epoch has no UNIX time to report, and one
       dated after 2199 is far enough out that a parse error is the likelier
       explanation than a certificate. */
    if (year < 1970UL || year > 2199UL)
        return 0;
    if (month < 1UL || month > 12UL || day < 1UL)
        return 0;
    if (hour > 23UL || minute > 59UL || second > 60UL)
        return 0;

    {
        unsigned long in_month = tls_fmt_days_in[month - 1UL];

        if (month == 2UL && tls_fmt_leap(year))
            in_month++;
        if (day > in_month)
            return 0;
    }

    days = (year - 1970UL) * 365UL
         + tls_fmt_leaps_before(year) - tls_fmt_leaps_before(1970UL)
         + tls_fmt_days_before[month - 1UL]
         + (day - 1UL);

    if (month > 2UL && tls_fmt_leap(year))
        days++;

    /*
     * A leap second is reported as the second before it.  Nothing downstream
     * can represent 23:59:60, and a certificate that carries one is not asking
     * anything of the answer beyond an ordering.
     */
    if (second > 59UL)
        second = 59UL;

    return (((days * 24UL + hour) * 60UL + minute) * 60UL) + second;
}

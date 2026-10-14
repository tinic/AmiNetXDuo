/*
 * NetMeter's arithmetic; see netmeter_fmt.h.
 *
 * SPDX-License-Identifier: MIT
 */

#include "netmeter_fmt.h"

#include <stddef.h>

typedef struct
{
    char  *out;
    uint32_t  len;
    uint32_t  used;
} NmText;

static void nm_put(NmText *t, const char *s)
{
    while (*s != '\0' && t->used + 1 < t->len)
        t->out[t->used++] = *s++;
    t->out[t->used] = '\0';
}

static void nm_put_ulong(NmText *t, uint32_t v)
{
    char  rev[11];
    char  one[2];
    uint32_t n = 0;

    do
    {
        rev[n++] = (char)('0' + v % 10u);
        v /= 10u;
    } while (v != 0 && n < sizeof(rev));

    one[1] = '\0';
    while (n != 0)
    {
        one[0] = rev[--n];
        nm_put(t, one);
    }
}

/*
 * `whole` units and `rem`/1024 of the next unit down, to three significant
 * figures: 1.50, 12.3, 256.
 */
static void nm_put_scaled(NmText *t, uint32_t whole, uint32_t rem, const char *unit)
{
    uint32_t frac;

    nm_put_ulong(t, whole);
    if (whole < 10u)
    {
        frac = (rem * 100u) >> 10;
        nm_put(t, ".");
        if (frac < 10u)
            nm_put(t, "0");
        nm_put_ulong(t, frac);
    }
    else if (whole < 100u)
    {
        frac = (rem * 10u) >> 10;
        nm_put(t, ".");
        nm_put_ulong(t, frac);
    }
    nm_put(t, " ");
    nm_put(t, unit);
}

void nm_format_rate(uint32_t v, char *out, uint32_t outlen)
{
    static const char *const units[] = { "B/s", "KB/s", "MB/s", "GB/s" };
    NmText t;
    uint32_t  u = 0;
    uint32_t  rem = 0;

    if (out == NULL || outlen == 0)
        return;
    t.out = out; t.len = outlen; t.used = 0; out[0] = '\0';

    while (v >= 1024u && u < 3u)
    {
        rem = v & 1023u;
        v >>= 10;
        u++;
    }

    if (u == 0)
    {
        nm_put_ulong(&t, v);
        nm_put(&t, " ");
        nm_put(&t, units[0]);
    }
    else
        nm_put_scaled(&t, v, rem, units[u]);
}

void nm_format_total(uint32_t hi, uint32_t lo, char *out, uint32_t outlen)
{
    static const char *const units[] = { "KB", "MB", "GB", "TB", "PB" };
    NmText t;
    uint32_t  v;
    uint32_t  rem;
    uint32_t  u = 0;

    if (out == NULL || outlen == 0)
        return;
    t.out = out; t.len = outlen; t.used = 0; out[0] = '\0';

    if (hi == 0 && lo < 1024u)
    {
        nm_put_ulong(&t, lo);
        nm_put(&t, lo == 1u ? " byte" : " bytes");
        return;
    }

    /* In KB below 4 TB, in MB above it: either fits 32 bits, up to 4 PB. */
    if ((hi >> 10) == 0)
    {
        v   = (hi << 22) | (lo >> 10);
        rem = lo & 1023u;
    }
    else
    {
        v   = (hi << 12) | (lo >> 20);
        rem = (lo >> 10) & 1023u;
        u   = 1;
    }

    while (v >= 1024u && u < 4u)
    {
        rem = v & 1023u;
        v >>= 10;
        u++;
    }
    nm_put_scaled(&t, v, rem, units[u]);
}

uint32_t nm_delta(uint32_t hi, uint32_t lo, uint32_t prev_hi, uint32_t prev_lo)
{
    if (hi < prev_hi || (hi == prev_hi && lo < prev_lo))
        return 0;
    if (hi == prev_hi)
        return lo - prev_lo;
    if (hi == prev_hi + 1u && lo < prev_lo)
        return lo - prev_lo;          /* across the carry, modulo 2^32 */
    return 0xFFFFFFFFu;
}

uint32_t nm_interval_ms(int32_t days0, int32_t min0, int32_t tick0,
                        int32_t days1, int32_t min1, int32_t tick1)
{
    int32_t dd = days1 - days0, mins, ms;

    if (dd < 0 || dd > 1)
        return 0;
    mins = dd * 1440 + (min1 - min0);
    if (mins < 0 || mins > 1 + (int32_t)(NM_INTERVAL_MAX / 60000u))
        return 0;
    ms = mins * 60000 + (tick1 - tick0) * 20;
    if (ms < 0 || (uint32_t)ms > NM_INTERVAL_MAX)
        return 0;
    return (uint32_t)ms;
}

uint32_t nm_rate(uint32_t bytes, uint32_t ms)
{
    if (ms == 0)
        return 0;
    if (bytes <= 0xFFFFFFFFu / 1000u)
        return (bytes * 1000u) / ms;
    return (bytes / ms) * 1000u;
}

uint32_t nm_average(uint32_t hi, uint32_t lo, uint32_t ms)
{
    uint32_t p0, p1, mlo, mhi, rem, q = 0, top;
    int      i;

    if (ms == 0)
        return 0;
    if (hi > 0xFFFFFFFFu / 1000u)
        return 0xFFFFFFFFu;       /* over 2^64 / 1000 bytes: past 32 bits */

    /* (hi:lo) * 1000, from the 16-bit halves of lo. */
    p0  = (lo & 0xFFFFu) * 1000u;
    p1  = (lo >> 16) * 1000u;
    mlo = p0 + (p1 << 16);
    mhi = hi * 1000u;                   /* fits: hi <= 0xFFFFFFFF / 1000 */
    p1  = (p1 >> 16) + (mlo < p0 ? 1u : 0u);
    if (mhi > 0xFFFFFFFFu - p1)
        return 0xFFFFFFFFu;       /* the low word's carry overflows it   */
    mhi += p1;
    if (mhi >= ms)
        return 0xFFFFFFFFu;       /* the quotient needs more than 32 bits */

    /* (mhi:mlo) / ms a bit at a time; mhi < ms keeps the quotient in 32. */
    rem = mhi;
    for (i = 0; i < 32; i++)
    {
        top = rem >> 31;
        rem = (rem << 1) | (mlo >> 31);
        mlo <<= 1;
        q <<= 1;
        if (top != 0 || rem >= ms)
        {
            rem -= ms;
            q |= 1u;
        }
    }
    return q;
}

static void nm_add64(uint32_t *hi, uint32_t *lo, uint32_t v)
{
    uint32_t was = *lo;

    *lo += v;
    if (*lo < was)
        (*hi)++;
}

void nm_span_add(NmSpan *s, uint32_t rx, uint32_t tx, uint32_t ms)
{
    if (s->frozen || s->ms > 0xFFFFFFFFu - ms)
    {
        s->frozen = 1;
        return;
    }
    nm_add64(&s->rx_hi, &s->rx_lo, rx);
    nm_add64(&s->tx_hi, &s->tx_lo, tx);
    s->ms += ms;
}

uint32_t nm_scale_next(uint32_t scale, uint32_t rx_rate, uint32_t tx_rate)
{
    uint32_t peak = (rx_rate > tx_rate) ? rx_rate : tx_rate;

    scale -= scale >> 4;
    if (peak > scale)
        scale = (peak > 0xFFFFFFFFu - (peak >> 2)) ? 0xFFFFFFFFu
                                                    : peak + (peak >> 2);
    if (scale < NM_SCALE_FLOOR)
        scale = NM_SCALE_FLOOR;
    return scale;
}

uint32_t nm_bar_fill(uint32_t rate, uint32_t scale, uint32_t width)
{
    if (scale == 0 || rate == 0 || width == 0)
        return 0;
    if (rate >= scale)
        return width;
    if (width > 0xFFFFu)
        width = 0xFFFFu;
    while (rate > 0xFFFFu)
    {
        rate  >>= 1;
        scale >>= 1;
        if (scale == 0)
            return width;
    }
    return (rate * width) / scale;
}

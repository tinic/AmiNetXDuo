/* SPDX-License-Identifier: MIT */
#include "devicehome.h"

#include <string.h>

/* Every driver an install writes into its Devs/Networks. */
static const char *const dh_drivers[] =
{
    "anxnet.device",
    "anxgenet.device",
    "anxwifipi.device",
    "anxzz9000.device",
};

#define DH_DRIVERS (sizeof(dh_drivers) / sizeof(dh_drivers[0]))

static size_t dh_len(const char *s)
{
    size_t n = 0;
    while (s[n] != '\0') n++;
    return n;
}

static char dh_lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

/* AmigaDOS names compare without case. */
static int dh_equal(const char *a, size_t alen, const char *b)
{
    size_t i;
    for (i = 0; i < alen; i++)
        if (b[i] == '\0' || dh_lower(a[i]) != dh_lower(b[i])) return 0;
    return b[alen] == '\0';
}

int dh_absolute_driver(const char *value, size_t len, char *dir,
                       size_t dir_cap, const char **driver)
{
    size_t colon = 0, base = 0, dir_len, i, d;

    while (colon < len && value[colon] != ':') colon++;
    /* No volume (":Devs/..." is the current one), or the assign itself. */
    if (colon == 0 || colon >= len || dh_equal(value, colon, "AmiNetXDuo"))
        return 0;
    for (i = colon; i < len; i++)
        if (value[i] == ':' || value[i] == '/') base = i + 1;

    for (d = 0; d < DH_DRIVERS; d++)
        if (dh_equal(value + base, len - base, dh_drivers[d])) break;
    if (d == DH_DRIVERS)
        return 0;

    dir_len = (value[base - 1] == '/') ? base - 1 : base;
    if (dir_len + 1 > dir_cap)
        return 0;
    for (i = 0; i < dir_len; i++) dir[i] = value[i];
    dir[dir_len] = '\0';
    *driver = dh_drivers[d];
    return 1;
}

static int dh_put(char *out, size_t cap, size_t *used, const char *s, size_t n)
{
    size_t i;
    if (n > cap - *used) return 0;
    for (i = 0; i < n; i++) out[(*used)++] = s[i];
    return 1;
}

/* The value of a DEVICE line, as src/config/config_text.c reads it: "DEVICE",
   optional '=', then one token or a quoted string.  A quoted value with an
   AmigaDOS '*' escape is not offered: its bytes are not the name. */
static int dh_device_value(const char *line, size_t len, size_t *vbeg,
                           size_t *vend)
{
    size_t p = 0, k;

    while (p < len && (line[p] == ' ' || line[p] == '\t')) p++;
    k = p;
    while (p < len && line[p] != '=' && line[p] != ' ' && line[p] != '\t' &&
           line[p] != '\r')
        p++;
    if (!dh_equal(line + k, p - k, "device")) return 0;
    while (p < len && (line[p] == ' ' || line[p] == '\t')) p++;
    if (p < len && line[p] == '=') p++;
    while (p < len && (line[p] == ' ' || line[p] == '\t')) p++;

    if (p < len && line[p] == '"')
    {
        *vbeg = ++p;
        while (p < len && line[p] != '"')
            if (line[p++] == '*') return 0;
        if (p >= len) return 0;
    }
    else
    {
        *vbeg = p;
        while (p < len && line[p] != ' ' && line[p] != '\t' && line[p] != '\r' &&
               line[p] != '#' && line[p] != ';')
            p++;
    }
    *vend = p;
    return p > *vbeg;
}

long dh_rehome(const char *old, size_t oldlen, char *out, size_t cap,
               size_t *newlen, DhOursFn ours, void *ctx)
{
    size_t at = 0, used = 0;
    long   rewritten = 0;

    while (at < oldlen)
    {
        size_t end = at, vbeg, vend;
        char   dir[128];
        const char *driver;

        while (end < oldlen && old[end] != '\n') end++;
        if (dh_device_value(old + at, end - at, &vbeg, &vend) &&
            dh_absolute_driver(old + at + vbeg, vend - vbeg, dir, sizeof(dir),
                               &driver) &&
            ours(dir, ctx))
        {
            if (!dh_put(out, cap, &used, old + at, vbeg) ||
                !dh_put(out, cap, &used, DH_ASSIGN_NETWORKS "/",
                        sizeof(DH_ASSIGN_NETWORKS "/") - 1) ||
                !dh_put(out, cap, &used, driver, dh_len(driver)))
                return -1;
            at += vend;
            rewritten++;
        }
        if (end < oldlen) end++;
        if (!dh_put(out, cap, &used, old + at, end - at))
            return -1;
        at = end;
    }
    *newlen = used;
    return rewritten;
}

size_t dh_hostline(const char *buf, size_t len, char *out, size_t cap)
{
    size_t at = 0;

    while (at < len)
    {
        size_t start = at, end, i, n;

        while (at < len && buf[at] != '\n')
            at++;
        end = at;
        if (at < len)
            at++;
        if (end > start && buf[end - 1] == '\r')
            end--;

        i = start;
        while (i < end && (buf[i] == ' ' || buf[i] == '\t'))
            i++;
        if (end - i < 9 || memcmp(buf + i, "hostname", 8) != 0 ||
            (buf[i + 8] != ' ' && buf[i + 8] != '\t'))
            continue;
        n = i + 8;
        while (n < end && (buf[n] == ' ' || buf[n] == '\t'))
            n++;
        if (n == end)
            continue;
        if (end - start + 1 > cap)
            return 0;
        memcpy(out, buf + start, end - start);
        out[end - start] = '\0';
        return end - start;
    }
    return 0;
}

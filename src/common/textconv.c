/*
 * Local codeset <-> UTF-8; see aminetxduo/textconv.h.
 *
 * Includes nothing of the AmigaOS: compiled for m68k into ssh and httpd, and
 * natively by the host tests.
 *
 * SPDX-License-Identifier: MIT
 */

#include "aminetxduo/textconv.h"

/* The byte for a code point >= 0xA0, exactly; 0 when the codeset has none. */
static uint8_t tc_byte(const txt_codeset *cs, uint32_t cp)
{
    size_t i;

    if (cs == NULL || cs->upper == NULL)
        return (cp >= 0xA0u && cp <= 0xFFu) ? (uint8_t)cp : 0;

    for (i = 0x20; i < 0x80; i++)            /* bytes 0xA0-0xFF */
        if (cs->upper[i] == cp)
            return (uint8_t)(0x80u + i);
    return 0;
}

/* One code point >= 0x80 for display: the byte, the look-alike, or '?'. */
static size_t tc_map(const txt_dec *d, const txt_codeset *cs, uint32_t cp,
                     uint8_t *out)
{
    uint8_t b;
    size_t  i, n;

    if (cp < 0xA0u)
    {
        out[0] = d->c1 ? (uint8_t)cp : (uint8_t)'?';
        return 1;
    }

    b = tc_byte(cs, cp);
    if (b != 0)
    {
        out[0] = b;
        return 1;
    }

    if (cs != NULL && cs->foreign != NULL)
    {
        n = cs->foreign(cs->ctx, cp, out, TXT_CHAR_MAX);
        if (n > TXT_CHAR_MAX)
            n = TXT_CHAR_MAX;
        for (i = 0; i < n; i++)         /* text only: no controls, no C1 */
            if (out[i] < 0x20u || out[i] == 0x7Fu ||
                (out[i] >= 0x80u && out[i] <= 0x9Fu))
                out[i] = '?';
        if (n > 0)
            return n;
    }

    out[0] = '?';
    return 1;
}

/* The sequence just completed is a character: overlong, surrogates and
   beyond U+10FFFF are not. */
static int tc_valid(const txt_dec *d)
{
    static const uint32_t least[5] = { 0, 0, 0x80u, 0x800u, 0x10000u };

    return d->cp >= least[d->len] && d->cp <= 0x10FFFFu &&
           !(d->cp >= 0xD800u && d->cp <= 0xDFFFu);
}

/* Start a sequence on lead byte b.  0 when b cannot start one. */
static int tc_lead(txt_dec *d, uint8_t b)
{
    if (b >= 0xC2u && b <= 0xDFu)
        d->cp = b & 0x1Fu, d->need = 1, d->len = 2;
    else if (b >= 0xE0u && b <= 0xEFu)
        d->cp = b & 0x0Fu, d->need = 2, d->len = 3;
    else if (b >= 0xF0u && b <= 0xF4u)
        d->cp = b & 0x07u, d->need = 3, d->len = 4;
    else
        return 0;
    return 1;
}

size_t txt_dec_byte(txt_dec *d, const txt_codeset *cs, uint8_t b, uint8_t *out)
{
    size_t o = 0;

    if (d->need > 0)
    {
        if ((b & 0xC0u) == 0x80u)
        {
            d->cp = (d->cp << 6) | (b & 0x3Fu);
            if (--d->need > 0)
                return 0;
            if (!tc_valid(d))
            {
                out[0] = '?';
                return 1;
            }
            return tc_map(d, cs, d->cp, out);
        }
        d->need = 0;                       /* cut short */
        out[o++] = '?';
    }

    if (b < 0x80u)
        out[o++] = b;
    else if (!tc_lead(d, b))
        out[o++] = '?';
    return o;
}

size_t txt_dec_flush(txt_dec *d, uint8_t *out)
{
    if (d->need == 0)
        return 0;
    d->need = 0;
    out[0] = '?';
    return 1;
}

size_t txt_utf8_to_local(const txt_codeset *cs, const uint8_t *in, size_t n,
                         uint8_t *out, size_t cap)
{
    txt_dec d = { 0, 0, 0, 0 };
    uint8_t tmp[TXT_CHAR_MAX];
    size_t  i, k, m, o = 0;

    for (i = 0; i <= n; i++)
    {
        m = (i < n) ? txt_dec_byte(&d, cs, in[i], tmp) : txt_dec_flush(&d, tmp);
        if (m > cap - o)
            break;
        for (k = 0; k < m; k++)
            out[o++] = tmp[k];
    }
    return o;
}

size_t txt_local_to_utf8(const txt_codeset *cs, uint8_t b, uint8_t out[3])
{
    uint32_t cp = b;

    if (b < 0xA0u)                          /* ASCII and C1 as they are */
    {
        if (b < 0x80u)
        {
            out[0] = b;
            return 1;
        }
    }
    else if (cs != NULL && cs->upper != NULL)
        cp = cs->upper[b - 0x80u];

    if (cp == 0)
    {
        out[0] = '?';
        return 1;
    }
    if (cp < 0x800u)
    {
        out[0] = (uint8_t)(0xC0u | (cp >> 6));
        out[1] = (uint8_t)(0x80u | (cp & 0x3Fu));
        return 2;
    }
    out[0] = (uint8_t)(0xE0u | (cp >> 12));
    out[1] = (uint8_t)(0x80u | ((cp >> 6) & 0x3Fu));
    out[2] = (uint8_t)(0x80u | (cp & 0x3Fu));
    return 3;
}

size_t txt_local_to_utf8_str(const txt_codeset *cs, const uint8_t *in, size_t n,
                             uint8_t *out, size_t cap, size_t *used)
{
    uint8_t u[3];
    size_t  i, k, m, o = 0;

    for (i = 0; i < n; i++)
    {
        m = txt_local_to_utf8(cs, in[i], u);
        if (m > cap - o)
            break;
        for (k = 0; k < m; k++)
            out[o++] = u[k];
    }
    if (used != NULL)
        *used = i;
    return o;
}

int txt_utf8_to_local_exact(const txt_codeset *cs, const uint8_t *in, size_t n,
                            uint8_t *out, size_t cap, size_t *outlen)
{
    txt_dec d = { 0, 0, 0, 1 };
    size_t  i, o = 0;

    for (i = 0; i < n; i++)
    {
        uint8_t b = in[i];

        if (d.need > 0)
        {
            if ((b & 0xC0u) != 0x80u)
                return TXT_BAD_UTF8;
            d.cp = (d.cp << 6) | (b & 0x3Fu);
            if (--d.need > 0)
                continue;
            if (!tc_valid(&d))
                return TXT_BAD_UTF8;
            if (d.cp < 0xA0u)
                b = (uint8_t)d.cp;          /* C1 */
            else if ((b = tc_byte(cs, d.cp)) == 0)
                return TXT_UNMAPPABLE;
        }
        else if (b >= 0x80u)
        {
            if (!tc_lead(&d, b))
                return TXT_BAD_UTF8;
            continue;
        }

        /* Written behind what has been read: out may be in. */
        if (o >= cap)
            return TXT_TOO_LONG;
        out[o++] = b;
    }
    if (d.need > 0)
        return TXT_BAD_UTF8;
    *outlen = o;
    return TXT_OK;
}

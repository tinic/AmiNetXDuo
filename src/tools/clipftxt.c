/*
 * Clipboard text conversion and FORM FTXT; see clipftxt.h.
 *
 * SPDX-License-Identifier: MIT
 */

#include "clipftxt.h"
#include "aminetxduo/textconv.h"

uint32_t clip_utf8_to_local(const struct txt_codeset *cs, const uint8_t *in,
                            uint32_t n, uint8_t *out, uint32_t cap)
{
    /* No look-alike text: out may be in, so one character, one byte. */
    txt_codeset plain = { cs != NULL ? cs->upper : NULL, NULL, NULL };
    txt_dec     d = { 0, 0, 0, 1 };
    uint32_t    i = 0, o = 0;

    while (i < n && o + 1u < cap + 1u)
    {
        uint8_t c = in[i];
        uint8_t tmp[TXT_CHAR_MAX];
        size_t  k, m;

        if (c == '\r')
        {
            m = txt_dec_flush(&d, tmp);
            if (o + m + 1u > cap)
                break;
            for (k = 0; k < m; k++)
                out[o++] = tmp[k];
            out[o++] = '\n';
            i += (i + 1 < n && in[i + 1] == '\n') ? 2u : 1u;
            continue;
        }

        m = txt_dec_byte(&d, &plain, c, tmp);
        if (o + m > cap)
            break;
        for (k = 0; k < m; k++)
            out[o++] = tmp[k];
        i++;
    }
    if (i == n && o < cap)
        o += (uint32_t)txt_dec_flush(&d, &out[o]);
    return o;
}

uint32_t clip_local_to_utf8(const struct txt_codeset *cs, const uint8_t *in,
                            uint32_t n, uint8_t *out, uint32_t cap)
{
    return (uint32_t)txt_local_to_utf8_str(cs, in, n, out, cap, NULL);
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

static uint32_t get32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static int is_id(const uint8_t *p, const char *id)
{
    return p[0] == (uint8_t)id[0] && p[1] == (uint8_t)id[1] &&
           p[2] == (uint8_t)id[2] && p[3] == (uint8_t)id[3];
}

void clip_ftxt_header(uint32_t n, uint8_t out[CLIP_FTXT_HEADER])
{
    out[0] = 'F'; out[1] = 'O'; out[2] = 'R'; out[3] = 'M';
    put32(out + 4, 4u + 8u + n + (n & 1u));
    out[8] = 'F'; out[9] = 'T'; out[10] = 'X'; out[11] = 'T';
    out[12] = 'C'; out[13] = 'H'; out[14] = 'R'; out[15] = 'S';
    put32(out + 16, n);
}

uint32_t clip_ftxt_text(const uint8_t *iff, uint32_t n,
                        uint8_t *out, uint32_t cap)
{
    uint32_t at, end, o = 0;

    if (n < 12u || !is_id(iff, "FORM") || !is_id(iff + 8, "FTXT"))
        return 0;

    end = get32(iff + 4);
    end = (end > n - 8u) ? n : end + 8u;

    for (at = 12u; at + 8u <= end; )
    {
        uint32_t size = get32(iff + at + 4);
        uint32_t have = end - (at + 8u);
        uint32_t take = (size < have) ? size : have;

        if (is_id(iff + at, "CHRS"))
        {
            uint32_t k;

            for (k = 0; k < take && o < cap; k++)
                out[o++] = iff[at + 8u + k];
        }
        if (size >= have)
            break;
        at += 8u + size + (size & 1u);
    }
    return o;
}

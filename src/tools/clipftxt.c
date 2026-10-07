/*
 * Clipboard text conversion and FORM FTXT; see clipftxt.h.
 *
 * SPDX-License-Identifier: MIT
 */

#include "clipftxt.h"

uint32_t clip_utf8_to_latin1(const uint8_t *in, uint32_t n,
                             uint8_t *out, uint32_t cap)
{
    uint32_t i = 0, o = 0;

    while (i < n && o < cap)
    {
        uint8_t  c = in[i];
        uint32_t cp, need, k;

        if (c == '\r')
        {
            out[o++] = '\n';
            i += (i + 1 < n && in[i + 1] == '\n') ? 2u : 1u;
            continue;
        }
        if (c < 0x80u)
        {
            out[o++] = c;
            i++;
            continue;
        }

        if ((c & 0xE0u) == 0xC0u)      { cp = c & 0x1Fu; need = 1; }
        else if ((c & 0xF0u) == 0xE0u) { cp = c & 0x0Fu; need = 2; }
        else if ((c & 0xF8u) == 0xF0u) { cp = c & 0x07u; need = 3; }
        else
        {
            out[o++] = '?';             /* a stray continuation byte */
            i++;
            continue;
        }

        for (k = 1; k <= need; k++)
        {
            if (i + k >= n || (in[i + k] & 0xC0u) != 0x80u)
                break;
            cp = (cp << 6) | (in[i + k] & 0x3Fu);
        }
        if (k <= need)
        {
            out[o++] = '?';             /* cut short */
            i += k;
            continue;
        }
        out[o++] = (cp <= 0xFFu && !(need == 1 && cp < 0x80u)) ? (uint8_t)cp
                                                               : (uint8_t)'?';
        i += need + 1u;
    }
    return o;
}

uint32_t clip_latin1_to_utf8(const uint8_t *in, uint32_t n,
                             uint8_t *out, uint32_t cap)
{
    uint32_t i, o = 0;

    for (i = 0; i < n; i++)
    {
        uint8_t c = in[i];

        if (c < 0x80u)
        {
            if (o + 1 > cap) break;
            out[o++] = c;
        }
        else
        {
            if (o + 2 > cap) break;
            out[o++] = (uint8_t)(0xC0u | (c >> 6));
            out[o++] = (uint8_t)(0x80u | (c & 0x3Fu));
        }
    }
    return o;
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

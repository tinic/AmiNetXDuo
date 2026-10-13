/*
 * The ssh client's console text conversion and OSC filter; see sshcs.h.
 *
 * SPDX-License-Identifier: MIT
 */

#include "sshcs.h"

#define ESC 0x1Bu
#define BEL 0x07u
#define CAN 0x18u
#define SUB 0x1Au

enum
{
    ST_TEXT = 0,        /* ordinary output                          */
    ST_ESC,             /* ESC seen, held back                       */
    ST_NUM,             /* ESC ] and digits                          */
    ST_STR,             /* after the ';', up to BEL or ESC \         */
    ST_STR_ESC          /* ESC inside the string                     */
};

/* A byte the console can show as text: never 0x80-0x9F (0x9B is its CSI),
   never a control code. */
static uint8_t printable(uint8_t b)
{
    return (b < 0x20u || b == 0x7Fu || (b >= 0x80u && b <= 0x9Fu)) ? '?' : b;
}

/* One code point >= 0x80 to the local codeset. */
static size_t map(const sshcs_codeset *cs, uint32_t cp, uint8_t *out)
{
    size_t i, n;

    if (cs == NULL || cs->upper == NULL)
    {
        if (cp >= 0xA0u && cp <= 0xFFu)
        {
            out[0] = (uint8_t)cp;
            return 1;
        }
    }
    else
    {
        for (i = 0x20; i < 0x80; i++)   /* bytes 0xA0-0xFF only */
            if (cs->upper[i] == cp)
            {
                out[0] = (uint8_t)(0x80u + i);
                return 1;
            }
    }

    if (cs != NULL && cs->foreign != NULL)
    {
        n = cs->foreign(cs->ctx, cp, out, SSHCS_CHAR_MAX);
        if (n > SSHCS_CHAR_MAX)
            n = SSHCS_CHAR_MAX;
        for (i = 0; i < n; i++)
            out[i] = printable(out[i]);
        if (n > 0)
            return n;
    }

    out[0] = '?';
    return 1;
}

/* One byte of UTF-8; at most 2 bytes out, or SSHCS_CHAR_MAX on a completed
   character. */
static size_t decode(sshcs_out *s, uint8_t b, uint8_t *out)
{
    static const uint32_t least[4] = { 0, 0x80u, 0x800u, 0x10000u };
    size_t o = 0;

    if (s->need > 0)
    {
        if ((b & 0xC0u) == 0x80u)
        {
            s->cp = (s->cp << 6) | (b & 0x3Fu);
            if (--s->need > 0)
                return 0;
            if (s->cp < least[s->len - 1] || s->cp > 0x10FFFFu
                || (s->cp >= 0xD800u && s->cp <= 0xDFFFu))
            {
                out[0] = '?';
                return 1;
            }
            return map(s->cs, s->cp, out);
        }
        s->need = 0;                       /* cut short */
        out[o++] = '?';
    }

    if (b < 0x80u)
        out[o++] = b;
    else if (b >= 0xC2u && b <= 0xDFu)
        s->cp = b & 0x1Fu, s->need = 1, s->len = 2;
    else if (b >= 0xE0u && b <= 0xEFu)
        s->cp = b & 0x0Fu, s->need = 2, s->len = 3;
    else if (b >= 0xF0u && b <= 0xF4u)
        s->cp = b & 0x07u, s->need = 3, s->len = 4;
    else
        out[o++] = '?';
    return o;
}

static size_t emit(sshcs_out *s, uint8_t b, uint8_t *out)
{
    if (!s->convert)
    {
        out[0] = b;
        return 1;
    }
    return decode(s, b, out);
}

static void osc_end(sshcs_out *s)
{
    if ((s->num == 0 || s->num == 2) && !s->overflow && s->title != NULL)
        s->title(s->title_ctx, s->title_buf, s->tlen);
    s->state = ST_TEXT;
}

size_t sshcs_out_run(sshcs_out *s, const uint8_t *in, size_t n,
                     uint8_t *out, size_t cap, size_t *used)
{
    size_t i = 0, o = 0;

    while (i < n && cap - o >= SSHCS_CHAR_MAX)
    {
        uint8_t b = in[i++];

        if (!s->osc || s->title_buf == NULL)
        {
            o += emit(s, b, out + o);
            continue;
        }

        switch (s->state)
        {
        case ST_TEXT:
            if (b != ESC)
            {
                o += emit(s, b, out + o);
                break;
            }
            if (s->convert && s->need > 0)  /* ESC cuts a sequence short */
            {
                s->need = 0;
                out[o++] = '?';
            }
            s->state = ST_ESC;
            break;

        case ST_ESC:
            if (b == ']')
            {
                s->state    = ST_NUM;
                s->num      = 0;
                s->tlen     = 0;
                s->overflow = 0;
                break;
            }
            out[o++] = ESC;                 /* not an OSC: let it through */
            if (b == ESC)
                break;                      /* and hold the new one */
            s->state = ST_TEXT;
            o += emit(s, b, out + o);
            break;

        case ST_NUM:
            if (b >= '0' && b <= '9')
            {
                if (s->num < 1000u)
                    s->num = (uint16_t)(s->num * 10u + (b - '0'));
                break;
            }
            if (b == ';')
                s->state = ST_STR;
            else if (b == BEL)
            {
                s->overflow = 1;            /* no text: no title */
                osc_end(s);
            }
            else if (b == ESC)
                s->state = ST_STR_ESC;
            else if (b == CAN || b == SUB)
                s->state = ST_TEXT;
            else
            {
                s->num   = 0xFFFFu;         /* malformed: strip, no title */
                s->state = ST_STR;
            }
            break;

        case ST_STR:
            if (b == BEL)
                osc_end(s);
            else if (b == ESC)
                s->state = ST_STR_ESC;
            else if (b == CAN || b == SUB)
                s->state = ST_TEXT;
            else if (s->tlen < SSHCS_TITLE_MAX)
                s->title_buf[s->tlen++] = b;
            else
                s->overflow = 1;
            break;

        case ST_STR_ESC:
            if (b == '\\')
                osc_end(s);
            else
            {
                /* ESC without '\': the OSC is dropped and the ESC starts
                   whatever comes next. */
                s->state = ST_ESC;
                i--;
            }
            break;
        }
    }

    *used = i;
    return o;
}

size_t sshcs_utf8_to_local(const sshcs_codeset *cs, const uint8_t *in,
                           size_t n, uint8_t *out, size_t cap)
{
    sshcs_out s = { 0 };
    uint8_t   tmp[SSHCS_CHAR_MAX];
    size_t    i, k, m, o = 0;

    s.cs      = cs;
    s.convert = 1;

    for (i = 0; i <= n; i++)
    {
        if (i < n)
            m = decode(&s, in[i], tmp);
        else if (s.need > 0)
            tmp[0] = '?', m = 1;            /* unfinished at the end */
        else
            break;
        if (m > cap - o)
            break;
        for (k = 0; k < m; k++)
            out[o++] = printable(tmp[k]);
    }
    return o;
}

size_t sshcs_local_to_utf8(const sshcs_codeset *cs, uint8_t b, uint8_t out[3])
{
    uint32_t cp = b;

    if (b < 0x80u)
    {
        out[0] = b;
        return 1;
    }
    if (cs != NULL && cs->upper != NULL)
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

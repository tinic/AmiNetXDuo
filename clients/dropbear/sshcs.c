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

static size_t emit(sshcs_out *s, uint8_t b, uint8_t *out)
{
    if (!s->convert)
    {
        out[0] = b;
        return 1;
    }
    return txt_dec_byte(&s->dec, s->cs, b, out);
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
            if (s->convert)                 /* ESC cuts a sequence short */
                o += txt_dec_flush(&s->dec, out + o);
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

size_t sshcs_utf8_to_local(const txt_codeset *cs, const uint8_t *in,
                           size_t n, uint8_t *out, size_t cap)
{
    size_t i, o = txt_utf8_to_local(cs, in, n, out, cap);

    for (i = 0; i < o; i++)
        if (out[i] < 0x20u || out[i] == 0x7Fu || (out[i] >= 0x80u && out[i] <= 0x9Fu))
            out[i] = '?';
    return o;
}

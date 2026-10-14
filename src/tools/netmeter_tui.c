/*
 * NetMeter's Shell window frame and input; see netmeter_tui.h.
 *
 * SPDX-License-Identifier: MIT
 */

#include "netmeter_tui.h"
#include "netmeter_fmt.h"

#include <stddef.h>

#define CSI     "\233"

/* Renditions, each a whole SGR so one never inherits another's colours. */
enum
{
    A_NORMAL, A_BOLD, A_BAR, A_BAR_KEY, A_BAR_DIM, A_TAG, A_RX, A_TX,
    A_RX_TAG, A_TX_TAG, A_RX_FILL, A_TX_FILL, A_ALERT, A_NONE
};

static const char *const nm_attr_sgr[A_NONE] =
{
    CSI "0m",               /* pen 1 on 0                                  */
    CSI "0;1m",
    CSI "0;32;43m",         /* title and key bars: white on blue           */
    CSI "0;1;32;43m",
    CSI "0;31;43m",
    CSI "0;1;31;42m",       /* interface name: black on white              */
    CSI "0;33m",            /* receive: blue                               */
    CSI "0;31m",            /* send: black                                 */
    CSI "0;1;32;43m",
    CSI "0;1;32;41m",
    CSI "0;43m",
    CSI "0;41m",
    CSI "0;1;7m"            /* link down, errors: reversed                 */
};

/* ---------------------------------------------------------- output, */

void nm_tui_flush(NmTuiOut *o)
{
    if (o->used != 0 && o->flush != NULL)
        o->flush(o->ctx, o->buf, o->used);
    o->used = 0;
}

static void nm_raw_n(NmTuiOut *o, const char *s, uint32_t n)
{
    while (n != 0)
    {
        if (o->used == o->size)
        {
            nm_tui_flush(o);
            if (o->size == 0)
                return;
        }
        o->buf[o->used++] = *s++;
        n--;
    }
}

static uint32_t nm_slen(const char *s)
{
    uint32_t n = 0;
    while (s[n] != '\0') n++;
    return n;
}

static void nm_raw(NmTuiOut *o, const char *s)
{
    nm_raw_n(o, s, nm_slen(s));
}

static void nm_attr(NmTuiOut *o, uint8_t a)
{
    if (o->attr == a)
        return;
    o->attr = a;
    nm_raw(o, nm_attr_sgr[a]);
}

/* Text that shows: what would pass the row's last column is dropped. */
static void nm_text(NmTuiOut *o, uint8_t a, const char *s)
{
    nm_attr(o, a);
    while (*s != '\0' && o->col < o->limit)
    {
        nm_raw_n(o, s++, 1);
        o->col++;
    }
}

static void nm_fill(NmTuiOut *o, uint8_t a, char c, uint32_t n)
{
    nm_attr(o, a);
    while (n-- != 0 && o->col < o->limit)
    {
        nm_raw_n(o, &c, 1);
        o->col++;
    }
}

static void nm_put_uint(char *out, uint32_t outlen, uint32_t v)
{
    char     rev[10];
    uint32_t n = 0, i = 0;

    do { rev[n++] = (char)('0' + v % 10u); v /= 10u; } while (v != 0);
    while (n != 0 && i + 1 < outlen)
        out[i++] = rev[--n];
    out[i] = '\0';
}

/* Right-aligned in `w` columns. */
static void nm_text_right(NmTuiOut *o, uint8_t a, const char *s, uint32_t w)
{
    uint32_t n = nm_slen(s);

    if (n < w)
        nm_fill(o, A_NORMAL, ' ', w - n);
    nm_text(o, a, s);
}

/* Row `row` from its first column: the cursor, and the count restarts. */
static void nm_row(NmTuiOut *o, uint32_t row, uint32_t cols)
{
    char s[11];

    nm_raw(o, CSI);
    nm_put_uint(s, sizeof(s), row);
    nm_raw(o, s);
    nm_raw(o, ";1H");
    o->col   = 0;
    o->limit = cols;
}

/* The rest of the row blank. */
static void nm_row_end(NmTuiOut *o)
{
    nm_attr(o, A_NORMAL);
    if (o->col < o->limit)
        nm_raw(o, CSI "K");
}

static void nm_cat(char *dst, uint32_t len, const char *s)
{
    uint32_t n = nm_slen(dst);

    while (*s != '\0' && n + 1 < len)
        dst[n++] = *s++;
    dst[n] = '\0';
}

/* ------------------------------------------------------------ text, */

void nm_format_bps(uint32_t bps, char *out, uint32_t outlen)
{
    static const uint32_t steps[] = { 1000000000u, 1000000u, 1000u, 1u };
    static const char *const units[] = { " Gbit/s", " Mbit/s", " kbit/s",
                                         " bit/s" };
    uint32_t k = 0, whole, tenth;
    char     s[4];

    if (out == NULL || outlen == 0)
        return;
    out[0] = '\0';
    while (k < 3u && bps < steps[k])
        k++;
    whole = bps / steps[k];
    tenth = (k < 3u) ? (bps % steps[k]) / (steps[k] / 10u) : 0;
    nm_put_uint(out, outlen, whole);
    if (tenth != 0 && whole < 100u)
    {
        s[0] = '.'; s[1] = (char)('0' + tenth); s[2] = '\0';
        nm_cat(out, outlen, s);
    }
    nm_cat(out, outlen, units[k]);
}

void nm_format_duration(uint32_t t, char *out, uint32_t outlen)
{
    char s[11];

    if (out == NULL || outlen == 0)
        return;
    nm_put_uint(out, outlen, t / 3600u);
    nm_cat(out, outlen, ":");
    s[0] = (char)('0' + (t / 600u) % 6u);
    s[1] = (char)('0' + (t / 60u) % 10u);
    s[2] = ':';
    s[3] = (char)('0' + (t % 60u) / 10u);
    s[4] = (char)('0' + t % 10u);
    s[5] = '\0';
    nm_cat(out, outlen, s);
}

/* ---------------------------------------------------------- layout, */

/* Rows an interface needs without its graph: a blank, its name, receive,
   send, totals. */
#define NM_IF_ROWS      5u
#define NM_GUTTER       12u     /* the graph's scale column               */

uint32_t nm_tui_graph_rows(uint32_t n_if, uint32_t rows)
{
    uint32_t room;

    if (n_if == 0 || rows < 2u + n_if * NM_IF_ROWS)
        return 0;
    room = (rows - 2u - n_if * NM_IF_ROWS) / n_if;
    room &= ~1u;
    return (room >= 2u) ? room : 0;
}

/* ----------------------------------------------------------- frame, */

static void nm_header(NmTuiOut *o, const NmTuiView *v)
{
    char     left[80], right[40], t[16];
    uint32_t ln, rn;

    left[0] = '\0';
    nm_cat(left, sizeof(left), "  ");
    if (v->host[0] != '\0')
        nm_cat(left, sizeof(left), v->host);
    else
        nm_cat(left, sizeof(left), "(no host name)");
    right[0] = '\0';
    if (v->paused)
        nm_cat(right, sizeof(right), "PAUSED  ");
    nm_cat(right, sizeof(right), "Session ");
    nm_format_duration(v->session_s, t, sizeof(t));
    nm_cat(right, sizeof(right), t);
    nm_cat(right, sizeof(right), " ");

    nm_row(o, 1, v->cols);
    nm_text(o, A_BAR_KEY, " NetMeter ");
    nm_text(o, A_BAR, v->version);
    ln = 10u + nm_slen(v->version) + nm_slen(left);
    rn = nm_slen(right);
    nm_text(o, A_BAR, left);
    if (ln + rn <= v->cols)
    {
        nm_fill(o, A_BAR, ' ', v->cols - ln - rn);
        nm_text(o, v->paused ? A_BAR_KEY : A_BAR, right);
    }
    else
        nm_fill(o, A_BAR, ' ', v->cols);
}

static void nm_footer(NmTuiOut *o, const NmTuiView *v)
{
    /* Short of the last column: a character there could scroll a console
       that ignores the wrap mode. */
    nm_row(o, v->rows, v->cols - 1u);
    nm_text(o, A_BAR_KEY, " Q");
    nm_text(o, A_BAR_DIM, " Quit  ");
    nm_text(o, A_BAR_KEY, "R");
    nm_text(o, A_BAR_DIM, " Reset  ");
    nm_text(o, A_BAR_KEY, "Space");
    nm_text(o, A_BAR_DIM, v->paused ? " Resume" : " Pause");
    nm_fill(o, A_BAR, ' ', v->cols);
}

/* `label` and `value`, or nothing when both do not fit. */
static void nm_piece(NmTuiOut *o, const char *label, uint8_t a,
                     const char *value)
{
    if (o->col + nm_slen(label) + nm_slen(value) > o->limit)
    {
        o->limit = o->col;          /* and nothing after it either */
        return;
    }
    nm_text(o, A_NORMAL, label);
    nm_text(o, a, value);
}

static void nm_if_title(NmTuiOut *o, const NmTuiIf *f)
{
    char s[48];

    nm_text(o, A_NORMAL, " ");
    nm_text(o, A_TAG, " ");
    nm_text(o, A_TAG, f->name);
    nm_text(o, A_TAG, " ");
    if (f->up)
        nm_piece(o, "  ", A_BOLD, "Up");
    else
        nm_piece(o, "  ", A_ALERT, " Down ");
    if (f->bps != 0)
    {
        nm_format_bps(f->bps, s, sizeof(s));
        nm_piece(o, "  ", A_NORMAL, s);
    }
    if (f->ip4[0] != '\0')
        nm_piece(o, "  ", A_BOLD, f->ip4);
    if (f->device[0] != '\0')
    {
        s[0] = '\0';
        nm_cat(s, sizeof(s), f->device);
        nm_cat(s, sizeof(s), " unit ");
        nm_put_uint(s + nm_slen(s), (uint32_t)(sizeof(s) - nm_slen(s)),
                    f->unit);
        nm_piece(o, "  ", A_NORMAL, s);
    }
    if (f->ip6[0] != '\0')
        nm_piece(o, "  ", A_NORMAL, f->ip6);
}

/* "   RX   2.70 MB/s  ######.....  Peak 2.71 MB/s  Avg  1.20 MB/s" */
static void nm_if_rate(NmTuiOut *o, const NmTuiIf *f, int rx)
{
    char     s[24];
    uint32_t avail, bw, fill;
    int      more;

    nm_text(o, A_NORMAL, "  ");
    nm_text(o, rx ? A_RX_TAG : A_TX_TAG, rx ? " RX " : " TX ");
    if (f->have_bytes)
        nm_format_rate(rx ? f->rx_rate : f->tx_rate, s, sizeof(s));
    else
    {
        s[0] = '-';
        s[1] = '\0';
    }
    nm_text_right(o, A_BOLD, s, 10u);

    avail = (o->limit > o->col + 1u) ? o->limit - o->col - 1u : 0;
    more  = avail >= 2u + 10u + 31u;
    bw    = more ? avail - 2u - 31u : (avail >= 6u ? avail - 2u : 0);
    if (bw != 0)
    {
        fill = nm_bar_fill(rx ? f->rx_rate : f->tx_rate, f->scale, bw);
        nm_text(o, A_NORMAL, "  ");
        nm_fill(o, rx ? A_RX_FILL : A_TX_FILL, ' ', fill);
        nm_fill(o, A_NORMAL, '.', bw - fill);
    }
    if (more && f->have_bytes)
    {
        nm_text(o, A_NORMAL, "  Peak ");
        nm_format_rate(rx ? f->rx_peak : f->tx_peak, s, sizeof(s));
        nm_text_right(o, A_BOLD, s, 9u);
        nm_text(o, A_NORMAL, "  Avg ");
        nm_format_rate(rx ? f->rx_avg : f->tx_avg, s, sizeof(s));
        nm_text_right(o, A_BOLD, s, 9u);
    }
}

/* "      In 1.20 GB  1234567 pkts   Out 52.3 MB  234567 pkts   Errors 0..." */
static void nm_if_totals(NmTuiOut *o, const NmTuiIf *f)
{
    char s[24];

    if (f->have_bytes)
    {
        nm_format_total(f->rx_hi, f->rx_lo, s, sizeof(s));
        nm_piece(o, "      In ", A_BOLD, s);
        nm_put_uint(s, sizeof(s), f->rx_pkts);
        nm_cat(s, sizeof(s), " pkts");
        nm_piece(o, "  ", A_NORMAL, s);
        nm_format_total(f->tx_hi, f->tx_lo, s, sizeof(s));
        nm_piece(o, "   Out ", A_BOLD, s);
        nm_put_uint(s, sizeof(s), f->tx_pkts);
        nm_cat(s, sizeof(s), " pkts");
        nm_piece(o, "  ", A_NORMAL, s);
    }
    else
    {
        nm_put_uint(s, sizeof(s), f->rx_pkts);
        nm_piece(o, "      Packets in ", A_BOLD, s);
        nm_put_uint(s, sizeof(s), f->tx_pkts);
        nm_piece(o, "  out ", A_BOLD, s);
    }
    nm_put_uint(s, sizeof(s), f->errors);
    nm_piece(o, "   Errors ", f->errors != 0 ? A_ALERT : A_BOLD, s);
    nm_put_uint(s, sizeof(s), f->dropped);
    nm_piece(o, "  Dropped ", f->dropped != 0 ? A_ALERT : A_BOLD, s);
}

static uint32_t nm_hist_at(const uint32_t *h, uint32_t head, uint32_t age)
{
    return h[(head + NM_TUI_HIST - 1u - age) % NM_TUI_HIST];
}

/* Half-cells a rate fills of `levels`: at least one for any traffic. */
static uint32_t nm_levels(uint32_t rate, uint32_t scale, uint32_t levels)
{
    uint32_t n;

    if (rate == 0)
        return 0;
    n = nm_bar_fill(rate, scale, levels);
    return (n == 0) ? 1u : n;
}

/*
 * The last seconds, newest on the right: receive grows up from the middle,
 * send grows down from it, each half to its own scale with a quarter's
 * headroom, printed at its far end.  A half cell is '.' above a receive column and '\'' below a send one.
 */
static void nm_if_graph(NmTuiOut *o, const NmTuiView *v, const NmTuiIf *f,
                        uint32_t row, uint32_t g)
{
    uint32_t half = g / 2u, w, n, k, r, c;
    uint32_t rx_scale = NM_SCALE_FLOOR, tx_scale = NM_SCALE_FLOOR;
    char     rx_s[24], tx_s[24];

    w = (v->cols > NM_GUTTER + 1u) ? v->cols - NM_GUTTER - 1u : 0;
    if (w > NM_TUI_HIST)
        w = NM_TUI_HIST;
    n = (f->hist_count < w) ? f->hist_count : w;
    for (k = 0; k < n; k++)
    {
        uint32_t a = nm_hist_at(f->rx_hist, f->hist_head, k);
        uint32_t b = nm_hist_at(f->tx_hist, f->hist_head, k);

        if (a > rx_scale) rx_scale = a;
        if (b > tx_scale) tx_scale = b;
    }
    /* A quarter's headroom: a steady transfer is not a solid block. */
    rx_scale += (rx_scale < 0xCCCCCCCCu) ? rx_scale >> 2 : 0;
    tx_scale += (tx_scale < 0xCCCCCCCCu) ? tx_scale >> 2 : 0;
    nm_format_rate(rx_scale, rx_s, sizeof(rx_s));
    nm_format_rate(tx_scale, tx_s, sizeof(tx_s));

    for (r = 0; r < g; r++)
    {
        int      up = r < half;
        uint32_t level = up ? half - 1u - r : r - half;

        nm_row(o, row + r, v->cols);
        if (w < 8u)
        {
            nm_row_end(o);
            continue;
        }
        if (r == 0 && half > 1u)
            nm_text_right(o, A_NORMAL, rx_s, NM_GUTTER - 2u);
        else if (r == g - 1u && half > 1u)
            nm_text_right(o, A_NORMAL, tx_s, NM_GUTTER - 2u);
        else if (r == half - 1u)
            nm_text_right(o, A_RX_TAG, " RX ", NM_GUTTER - 2u);
        else if (r == half)
            nm_text_right(o, A_TX_TAG, " TX ", NM_GUTTER - 2u);
        else
            nm_fill(o, A_NORMAL, ' ', NM_GUTTER - 2u);
        nm_fill(o, A_NORMAL, ' ', 2u);

        for (c = 0; c < w; c++)
        {
            uint32_t age = w - 1u - c, lv;

            if (age >= n)
            {
                nm_fill(o, A_NORMAL, ' ', 1u);
                continue;
            }
            lv = nm_levels(nm_hist_at(up ? f->rx_hist : f->tx_hist,
                                      f->hist_head, age),
                           up ? rx_scale : tx_scale, g);
            if (level < lv / 2u)
                nm_fill(o, up ? A_RX_FILL : A_TX_FILL, ' ', 1u);
            else if (level == lv / 2u && (lv & 1u))
                nm_fill(o, up ? A_RX : A_TX, up ? '.' : '\'', 1u);
            else
                nm_fill(o, A_NORMAL, ' ', 1u);
        }
        nm_row_end(o);
    }
}

void nm_tui_frame(NmTuiOut *o, const NmTuiView *v)
{
    uint32_t last, row, i, g;

    if (v->rows == 0 || v->cols < 2u)
        return;
    o->attr = A_NONE;
    nm_header(o, v);
    last = (v->rows >= 2u) ? v->rows - 1u : 1u;    /* the content's last */
    row  = 2;

    if (!v->running || v->n == 0)
    {
        if (row <= last) { nm_row(o, row, v->cols); nm_row_end(o); row++; }
        if (row <= last)
        {
            nm_row(o, row, v->cols);
            nm_text(o, A_NORMAL, "  ");
            nm_text(o, A_BOLD, !v->running ? "The network is not running."
                                           : "No interface is online.");
            nm_row_end(o);
            row++;
        }
    }
    else
    {
        g = nm_tui_graph_rows(v->n, v->rows);
        for (i = 0; i < v->n && row <= last; i++)
        {
            const NmTuiIf *f = &v->ifs[i];
            uint32_t       line;

            for (line = 0; line < 5u && row <= last; line++, row++)
            {
                nm_row(o, row, v->cols);
                switch (line)
                {
                case 1: nm_if_title(o, f);     break;
                case 2: nm_if_rate(o, f, 1);   break;
                case 3: nm_if_rate(o, f, 0);   break;
                case 4: nm_if_totals(o, f);    break;
                default:                       break;
                }
                nm_row_end(o);
            }
            if (g != 0 && row + g - 1u <= last)
            {
                nm_if_graph(o, v, f, row, g);
                row += g;
            }
        }
    }
    for (; row <= last; row++)
    {
        nm_row(o, row, v->cols);
        nm_row_end(o);
    }
    if (v->rows >= 2u)
        nm_footer(o, v);
    nm_attr(o, A_NORMAL);
    nm_tui_flush(o);
}

void nm_tui_enter(NmTuiOut *o)
{
    /* Cursor off, no wrap, no scroll, tell of a resize; then a clear window. */
    nm_raw(o, CSI "0 p" CSI "?7l" CSI ">1l" CSI "12{" CSI "0m" "\f");
    o->attr = A_NORMAL;
    nm_tui_flush(o);
}

void nm_tui_leave(NmTuiOut *o)
{
    nm_raw(o, CSI "12}" CSI "0m" "\f" CSI "?7h" CSI ">1h" CSI " p");
    o->attr = A_NORMAL;
    nm_tui_flush(o);
}

void nm_tui_ask_bounds(NmTuiOut *o)
{
    nm_raw(o, CSI "0 q");
    nm_tui_flush(o);
}

/* ----------------------------------------------------------- input, */

enum { IN_GROUND, IN_ESC, IN_CSI };

int nm_tui_feed(NmTuiIn *in, unsigned char c, NmTuiEvent *ev)
{
    ev->kind = NM_IN_NONE;
    switch (in->state)
    {
    case IN_ESC:
        if (c == '[')
            break;                  /* below: a CSI begins */
        in->state = IN_GROUND;
        ev->kind  = NM_IN_KEY;      /* Esc, and what followed it is lost */
        ev->key   = 27;
        return 1;
    case IN_CSI:
        if (c >= '0' && c <= '9')
        {
            if (in->np < 4u)
                in->p[in->np] = in->p[in->np] * 10u + (uint32_t)(c - '0');
            return 0;
        }
        if (c == ';')
        {
            if (in->np < 4u)
                in->np++;
            return 0;
        }
        if (c >= 0x20 && c <= 0x2f)
        {
            in->inter = c;
            return 0;
        }
        in->state = IN_GROUND;
        if (c == 'r' && in->inter == ' ' && in->np >= 3u &&
            in->p[2] != 0 && in->p[3] != 0)
        {
            ev->kind = NM_IN_BOUNDS;
            ev->rows = in->p[2] - (in->p[0] > 1u ? in->p[0] - 1u : 0);
            ev->cols = in->p[3] - (in->p[1] > 1u ? in->p[1] - 1u : 0);
            return 1;
        }
        if (c == '|' && in->p[0] == 12u)
        {
            ev->kind = NM_IN_RESIZE;
            return 1;
        }
        return 0;                   /* a function key, a cursor key: unused */
    default:
        if (c == 0x1b)
        {
            in->state = IN_ESC;
            return 0;
        }
        if (c == 0x9b)
            break;
        ev->kind = NM_IN_KEY;
        ev->key  = c;
        return 1;
    }
    in->state = IN_CSI;
    in->inter = 0;
    in->np    = 0;
    in->p[0] = in->p[1] = in->p[2] = in->p[3] = 0;
    return 0;
}

int nm_tui_idle(NmTuiIn *in, NmTuiEvent *ev)
{
    ev->kind = NM_IN_NONE;
    if (in->state != IN_ESC)
        return 0;
    in->state = IN_GROUND;
    ev->kind  = NM_IN_KEY;
    ev->key   = 27;
    return 1;
}

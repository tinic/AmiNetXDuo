/*
 * NetWirelessPrefs: window geometry and the hidden passphrase.  See the
 * header.
 *
 * SPDX-License-Identifier: MIT
 */

#include "netwireless_layout.h"

#define MARGIN_X    8               /* window edge to content              */
#define MARGIN_Y    5
#define ROW_GAP     4
#define COL_GAP     8
#define LABEL_GAP   8               /* label to its gadget                 */
#define BUTTON_PAD  10              /* each side of a button's caption     */
#define FIELD_PAD   12              /* string gadget frame and inset       */
#define CYCLE_GLYPH 20
#define SCROLLER_W  16
#define CHECK_W     26
#define CHECK_H     11

const NwItem nw_items[NW_L_COUNT] =
{
    [NW_M_NETWORK]    = { "Network",           NW_PLACE_LEFT,  NW_WIN_MAIN },
    [NW_M_SIGNAL]     = { "Signal",            NW_PLACE_LEFT,  NW_WIN_MAIN },
    [NW_M_LIST]       = { "Networks in range", NW_PLACE_ABOVE, NW_WIN_MAIN },
    [NW_M_SCAN]       = { "Scan",              NW_PLACE_IN,    NW_WIN_MAIN },
    [NW_M_JOIN]       = { "Join...",           NW_PLACE_IN,    NW_WIN_MAIN },
    [NW_M_KNOWN]      = { "Known...",          NW_PLACE_IN,    NW_WIN_MAIN },
    [NW_M_DISCONNECT] = { "Disconnect",        NW_PLACE_IN,    NW_WIN_MAIN },
    [NW_J_SSID]       = { "Network",           NW_PLACE_LEFT,  NW_WIN_JOIN },
    [NW_J_SECURITY]   = { "Security",          NW_PLACE_LEFT,  NW_WIN_JOIN },
    [NW_J_PASS]       = { "Passphrase",        NW_PLACE_LEFT,  NW_WIN_JOIN },
    [NW_J_SHOW]       = { "Show passphrase",   NW_PLACE_RIGHT, NW_WIN_JOIN },
    [NW_J_HIDDEN]     = { "Hidden network",    NW_PLACE_RIGHT, NW_WIN_JOIN },
    [NW_J_STATUS]     = { 0,                   NW_PLACE_NONE,  NW_WIN_JOIN },
    [NW_J_JOIN]       = { "Join",              NW_PLACE_IN,    NW_WIN_JOIN },
    [NW_J_CANCEL]     = { "Cancel",            NW_PLACE_IN,    NW_WIN_JOIN },
    [NW_K_LIST]       = { "Known networks",    NW_PLACE_ABOVE, NW_WIN_KNOWN },
    [NW_K_UP]         = { "Up",                NW_PLACE_IN,    NW_WIN_KNOWN },
    [NW_K_DOWN]       = { "Down",              NW_PLACE_IN,    NW_WIN_KNOWN },
    [NW_K_REMOVE]     = { "Remove",            NW_PLACE_IN,    NW_WIN_KNOWN },
    [NW_K_SAVE]       = { "Save",              NW_PLACE_IN,    NW_WIN_KNOWN },
    [NW_K_USE]        = { "Use",               NW_PLACE_IN,    NW_WIN_KNOWN },
    [NW_K_CANCEL]     = { "Cancel",            NW_PLACE_IN,    NW_WIN_KNOWN },
    [NW_K_CONNECT]    = { "Connect",           NW_PLACE_IN,    NW_WIN_KNOWN },
};

const char *const nw_security_choices[] = { "WPA/WPA2 Personal", "Open", 0 };

static int text_w(const NwFont *f, const char *s)
{
    int n = 0;
    while (s[n] != '\0') n++;
    return f->measure(f->ctx, s, n);
}

static int max2(int a, int b)
{
    return a > b ? a : b;
}

static void put(NwLayout *l, int id, int x, int y, int w, int h)
{
    l->box[id].x = x; l->box[id].y = y; l->box[id].w = w; l->box[id].h = h;
}

/* Where the label of id lands, as GadTools places it. */
static void place_label(const NwFont *f, NwLayout *l, int id)
{
    const NwItem *it = &nw_items[id];
    const NwBox  *b = &l->box[id];
    NwBox        *t = &l->label[id];
    int           w;

    t->w = 0;
    if (it->label == 0 || it->place == NW_PLACE_NONE) return;
    w = text_w(f, it->label);
    t->w = w;
    t->h = f->height;
    switch (it->place)
    {
        case NW_PLACE_LEFT:  t->x = b->x - LABEL_GAP - w; t->y = b->y + (b->h - f->height) / 2; break;
        case NW_PLACE_RIGHT: t->x = b->x + b->w + LABEL_GAP; t->y = b->y + (b->h - f->height) / 2; break;
        case NW_PLACE_ABOVE: t->x = b->x + (b->w - w) / 2; t->y = b->y - f->height - 2; break;
        default:             t->x = b->x + (b->w - w) / 2; t->y = b->y + (b->h - f->height) / 2; break;
    }
}

/* The widest caption of ids, plus the button's padding. */
static int button_w(const NwFont *f, const int *ids, int n)
{
    int i, w = 0;

    for (i = 0; i < n; i++) w = max2(w, text_w(f, nw_items[ids[i]].label));
    return w + 2 * BUTTON_PAD;
}

/* n buttons of width bw spread over width cw from x, the ends flush. */
static void button_row(NwLayout *l, const int *ids, int n, int x, int y,
                       int cw, int bw, int h)
{
    int i;

    for (i = 0; i < n; i++)
        put(l, ids[i], n == 1 ? x : x + i * (cw - bw) / (n - 1), y, bw, h);
}

static void layout_main(const NwFont *f, NwLayout *l)
{
    static const int buttons[] = { NW_M_SCAN, NW_M_JOIN, NW_M_KNOWN, NW_M_DISCONNECT };
    int fh = f->height, nw = text_w(f, "n");
    int lw = max2(text_w(f, nw_items[NW_M_NETWORK].label),
                  text_w(f, nw_items[NW_M_SIGNAL].label));
    int bw = button_w(f, buttons, 4);
    int row_w, list_w, cw, x, y;

    /* A list row: bars, the SSID at 24 average characters, the level, the
       padlock. */
    l->bars_w = 2 * fh;
    l->level_x = l->bars_w + 6 + 24 * nw + COL_GAP;
    l->lock_x = l->level_x + text_w(f, "-100 dBm") + COL_GAP;
    row_w = l->lock_x + fh;
    list_w = row_w + 8 + SCROLLER_W;

    cw = max2(list_w, 4 * bw + 3 * COL_GAP);
    cw = max2(cw, lw + LABEL_GAP + 20 * nw);
    x = MARGIN_X;
    y = MARGIN_Y;
    put(l, NW_M_NETWORK, x + lw + LABEL_GAP, y, cw - lw - LABEL_GAP, fh + 6);
    y += fh + 6 + ROW_GAP;
    put(l, NW_M_SIGNAL, x + lw + LABEL_GAP, y, cw - lw - LABEL_GAP, fh + 6);
    y += fh + 6 + ROW_GAP + fh + 2;
    put(l, NW_M_LIST, x, y, cw, NW_LIST_ROWS * fh + 4);
    y += NW_LIST_ROWS * fh + 4 + ROW_GAP + 2;
    button_row(l, buttons, 4, x, y, cw, bw, fh + 6);
    l->inner_w[NW_WIN_MAIN] = cw + 2 * MARGIN_X;
    l->inner_h[NW_WIN_MAIN] = y + fh + 6 + MARGIN_Y;
}

static void layout_join(const NwFont *f, NwLayout *l)
{
    static const int buttons[] = { NW_J_JOIN, NW_J_CANCEL };
    int fh = f->height, nw = text_w(f, "n");
    int lw = max2(max2(text_w(f, nw_items[NW_J_SSID].label),
                       text_w(f, nw_items[NW_J_SECURITY].label)),
                  text_w(f, nw_items[NW_J_PASS].label));
    int fw = max2(28 * nw + FIELD_PAD,
                  max2(text_w(f, nw_security_choices[0]),
                       text_w(f, nw_security_choices[1])) +
                  CYCLE_GLYPH + 2 * BUTTON_PAD);
    int check = max2(text_w(f, nw_items[NW_J_SHOW].label),
                     text_w(f, nw_items[NW_J_HIDDEN].label));
    int bw = button_w(f, buttons, 2);
    int cw, fx, y, ch = max2(CHECK_H, fh + 2);

    cw = max2(lw + LABEL_GAP + fw, 2 * bw + COL_GAP);
    cw = max2(cw, lw + LABEL_GAP + CHECK_W + LABEL_GAP + check);
    fx = MARGIN_X + lw + LABEL_GAP;
    fw = cw - lw - LABEL_GAP;
    y = MARGIN_Y;
    put(l, NW_J_SSID, fx, y, fw, fh + 6);           y += fh + 6 + ROW_GAP;
    put(l, NW_J_SECURITY, fx, y, fw, fh + 6);       y += fh + 6 + ROW_GAP;
    put(l, NW_J_PASS, fx, y, fw, fh + 6);           y += fh + 6 + ROW_GAP;
    put(l, NW_J_SHOW, fx, y, CHECK_W, ch);          y += ch + ROW_GAP;
    put(l, NW_J_HIDDEN, fx, y, CHECK_W, ch);        y += ch + ROW_GAP;
    put(l, NW_J_STATUS, MARGIN_X, y, cw, fh + 6);   y += fh + 6 + ROW_GAP + 2;
    button_row(l, buttons, 2, MARGIN_X, y, cw, bw, fh + 6);
    l->inner_w[NW_WIN_JOIN] = cw + 2 * MARGIN_X;
    l->inner_h[NW_WIN_JOIN] = y + fh + 6 + MARGIN_Y;
}

static void layout_known(const NwFont *f, NwLayout *l)
{
    static const int side[] = { NW_K_CONNECT, NW_K_UP, NW_K_DOWN, NW_K_REMOVE };
    static const int bottom[] = { NW_K_SAVE, NW_K_USE, NW_K_CANCEL };
    int fh = f->height, nw = text_w(f, "n");
    int sw = button_w(f, side, 4), bw = button_w(f, bottom, 3);
    int list_w = 28 * nw + 8 + SCROLLER_W, list_h = NW_LIST_ROWS * fh + 4;
    int cw, y, i;

    cw = max2(list_w + COL_GAP + sw, 3 * bw + 2 * COL_GAP);
    list_w = cw - COL_GAP - sw;
    y = MARGIN_Y + fh + 2;
    put(l, NW_K_LIST, MARGIN_X, y, list_w, list_h);
    for (i = 0; i < 4; i++)
        put(l, side[i], MARGIN_X + list_w + COL_GAP, y + i * (fh + 6 + ROW_GAP),
            sw, fh + 6);
    y += max2(list_h, 4 * (fh + 6) + 3 * ROW_GAP) + ROW_GAP + 2;
    button_row(l, bottom, 3, MARGIN_X, y, cw, bw, fh + 6);
    l->inner_w[NW_WIN_KNOWN] = cw + 2 * MARGIN_X;
    l->inner_h[NW_WIN_KNOWN] = y + fh + 6 + MARGIN_Y;
}

int nw_layout(const NwFont *font, int max_w, int max_h, NwLayout *out)
{
    int i, fits = 1;

    layout_main(font, out);
    layout_join(font, out);
    layout_known(font, out);
    for (i = 0; i < NW_L_COUNT; i++) place_label(font, out, i);
    for (i = 0; i < NW_WIN_COUNT; i++)
        if (out->inner_w[i] > max_w || out->inner_h[i] > max_h) fits = 0;
    return fits;
}

int nw_secret_edit(char *real, int *len, int cap, int op, int pos_after,
                   int len_after, char code)
{
    int i, n = *len;

    if (op == NW_EDIT_INSERT)
    {
        int at = pos_after - 1;

        if (len_after != n + 1 || n + 1 >= cap || at < 0 || at > n) return 0;
        for (i = n; i > at; i--) real[i] = real[i - 1];
        real[at] = code;
        *len = n + 1;
    }
    else if (op == NW_EDIT_REPLACE)
    {
        if (len_after != n || pos_after < 1 || pos_after > n) return 0;
        real[pos_after - 1] = code;
    }
    else if (len_after < n)
    {
        /* Deletion of one run, which starts where the cursor is left. */
        int gone = n - len_after;

        if (pos_after < 0 || pos_after + gone > n) return 0;
        for (i = pos_after; i + gone < n; i++) real[i] = real[i + gone];
        *len = len_after;
    }
    else if (len_after != n)
        return 0;                    /* a paste: nothing to mirror it from */
    real[*len] = '\0';
    return 1;
}

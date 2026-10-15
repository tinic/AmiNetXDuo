/* SPDX-License-Identifier: MIT */
#include "netswitch_layout.h"

/* Spacing in pixels; everything else comes from the font. */
#define MARGIN_X     8              /* window edge to content */
#define MARGIN_Y     5
#define GAP          8
#define CELL_PAD     4              /* list frame to its first row and column */
#define COL_GAP      8              /* between the list's columns */
#define ROW_LEAD     5              /* a row's text height to the next row */
#define GLYPH_PAD    2              /* the state glyph's width past the font */
#define LEGEND_PAD   2              /* above and below the legend's text */
#define STATUS_PAD   2
#define SCROLLER_W   16             /* a scroll strip, only when scrolling */
#define PANEL_LEAD   2              /* between the panel's lines */

/* The list's columns, in characters of the font. */
#define NAME_CHARS    8
#define DEVICE_CHARS  18            /* "anxzz9000.device 0" */
#define ADDRESS_CHARS 15            /* a dotted quad */

/*
 * The panel shows a whole link-local address: it is what an interface has
 * while DHCP is still running, and it is the line that must not be elided.
 * A longer global address elides rather than widening the window past the
 * screen.
 */
#define PANEL_ADDR_CHARS 25

/* The panel's fixed lines: Selected / device / Addresses: / three addresses /
   Last error. */
#define PANEL_LINES  7

#define TARGET_W    480             /* a target, not a floor: see need_w */

static int max_i(int a, int b) { return a > b ? a : b; }
static int min_i(int a, int b) { return a < b ? a : b; }

static int text_w(const NsFont *font, const char *s)
{
    int n = 0;

    if (s == 0) return 0;
    while (s[n] != '\0') n++;
    return font->measure(font->ctx, s, n);
}

int ns_layout(const NsFont *font, int max_w, int max_h, unsigned count,
              NsLayout *out)
{
    const int status_h = font ? font->height + 2 * STATUS_PAD : 0;
    int fh, digit_w, glyph_w, name_w, device_w, address_w;
    int list_w, panel_min, need_w, inner_w, right;
    int row_h, legend_h, list_base, panel_end, rows, scroll, strip;
    int list_y, list_h, status_y, content_h;
    int panel_x, panel_lines_h, button_h, buttons_y, hint_y;
    int fits = 1;

    if (font == 0 || out == 0 || max_w <= 0 || max_h <= 0) return 0;
    fh = font->height;

    digit_w   = (text_w(font, "0123456789") + 9) / 10;
    if (digit_w < 1) digit_w = 1;
    glyph_w   = fh + GLYPH_PAD;
    name_w    = NAME_CHARS * digit_w;
    device_w  = DEVICE_CHARS * digit_w;
    address_w = ADDRESS_CHARS * digit_w;
    panel_min = PANEL_ADDR_CHARS * digit_w;

    row_h    = fh + ROW_LEAD;
    legend_h = fh + 2 * LEGEND_PAD;

    /* The panel's lines never move: only the list grows and scrolls.  The two
       buttons and the hint hang under the panel and are part of the same
       column, so they set the window's floor along with it. */
    panel_lines_h = PANEL_LINES * (fh + PANEL_LEAD);
    button_h      = fh + 7;
    buttons_y     = MARGIN_Y + panel_lines_h + GAP;
    hint_y        = buttons_y + 2 * button_h + GAP / 2 + 3;
    panel_end     = hint_y + fh;
    list_base     = MARGIN_Y + legend_h + 1 + 2 * CELL_PAD;

    /* Rows that fit while the window stays inside the screen. */
    {
        int floor_h = max_i(list_base, panel_end);
        int room = max_h - MARGIN_Y - GAP - status_h - MARGIN_Y - floor_h;

        if (room < row_h) return 0;                 /* not even one row */
        rows = room / row_h;
    }
    /* The list shows whole rows and no more than there are interfaces: an
       empty list keeps its frame and shows no row at all. */
    rows = min_i(rows, (int)count);
    scroll = (unsigned)rows < count;
    strip = scroll ? SCROLLER_W : 0;

    list_w = 2 * CELL_PAD + glyph_w + COL_GAP + name_w + COL_GAP + device_w +
             COL_GAP + address_w + strip;

    /* What the window has to be for the panel to show an address whole, and
       what this font and screen have room for. */
    need_w = 2 * MARGIN_X + list_w + GAP + panel_min;
    inner_w = max_i(need_w, TARGET_W);
    if (inner_w > max_w)
    {
        inner_w = max_w;
        fits = 0;                                   /* too wide for this font */
    }
    right = inner_w - MARGIN_X;

    list_y = MARGIN_Y + legend_h + 1;
    list_h = 2 * CELL_PAD + rows * row_h;

    panel_x = MARGIN_X + list_w + GAP;
    content_h = max_i(list_y + list_h, panel_end) - MARGIN_Y;
    status_y = MARGIN_Y + content_h + GAP;
    out->inner_h = status_y + status_h + MARGIN_Y;
    if (out->inner_h > max_h) fits = 0;

    out->inner_w = inner_w;
    out->row_h = row_h;
    out->rows = rows;
    out->scroll = scroll;
    out->cell_pad = CELL_PAD;
    out->strip_w = strip;
    out->panel_lead = PANEL_LEAD;
    out->glyph_x = CELL_PAD;
    out->glyph_w = glyph_w;
    out->name_x = out->glyph_x + glyph_w + COL_GAP;
    out->name_chars = NAME_CHARS;
    out->device_x = out->name_x + name_w + COL_GAP;
    out->device_chars = DEVICE_CHARS;
    out->address_x = out->device_x + device_w + COL_GAP;
    out->address_chars = ADDRESS_CHARS;
    out->panel_w = right - panel_x;
    if (out->panel_w < panel_min && fits) fits = 0;

    out->box[NS_L_LEGEND].x = MARGIN_X;
    out->box[NS_L_LEGEND].y = MARGIN_Y;
    out->box[NS_L_LEGEND].w = list_w - strip;
    out->box[NS_L_LEGEND].h = legend_h;

    out->box[NS_L_LIST].x = MARGIN_X;
    out->box[NS_L_LIST].y = list_y;
    out->box[NS_L_LIST].w = list_w;
    out->box[NS_L_LIST].h = list_h;

    out->box[NS_L_PANEL].x = panel_x;
    out->box[NS_L_PANEL].y = MARGIN_Y;
    out->box[NS_L_PANEL].w = out->panel_w;
    out->box[NS_L_PANEL].h = panel_lines_h;

    /* The two buttons take the panel's width so they line up with each other
       and with the lines above them; the hint goes under them. */
    out->box[NS_L_ACTION].x = panel_x;
    out->box[NS_L_ACTION].y = buttons_y;
    out->box[NS_L_ACTION].w = out->panel_w;
    out->box[NS_L_ACTION].h = button_h;

    out->box[NS_L_ALL].x = panel_x;
    out->box[NS_L_ALL].y = out->box[NS_L_ACTION].y + out->box[NS_L_ACTION].h +
                           GAP / 2;
    out->box[NS_L_ALL].w = out->panel_w;
    out->box[NS_L_ALL].h = button_h;

    out->box[NS_L_HINT].x = panel_x;
    out->box[NS_L_HINT].y = hint_y;
    out->box[NS_L_HINT].w = out->panel_w;
    out->box[NS_L_HINT].h = fh;

    if (panel_end > status_y && fits) fits = 0;

    out->box[NS_L_STATUS].x = MARGIN_X;
    out->box[NS_L_STATUS].y = status_y;
    out->box[NS_L_STATUS].w = inner_w - 2 * MARGIN_X;
    out->box[NS_L_STATUS].h = status_h;

    return fits;
}

int ns_first_row(const NsLayout *l, unsigned count, unsigned selected)
{
    int rows, first;

    if (l == 0 || l->rows < 1) return 0;
    if (count <= (unsigned)l->rows) return 0;
    if (selected >= count) selected = count - 1;

    rows = l->rows;
    first = (int)selected - rows + 1;
    if (first < 0) first = 0;
    if (first > (int)count - rows) first = (int)count - rows;
    return first < 0 ? 0 : first;
}

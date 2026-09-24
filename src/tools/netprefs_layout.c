/* SPDX-License-Identifier: MIT */
#include "netprefs_layout.h"

/* Spacing in pixels; everything else comes from the font. */
#define MARGIN_X    8               /* window edge to content */
#define MARGIN_Y    5
#define GAP         8               /* GadTools INTERWIDTH */
#define LABEL_GAP   8               /* label to its gadget */
#define CELL_GAP    12              /* between cells sharing a row */
#define PAD_X       12              /* bevel to page content */
#define PAD_Y       10
#define ROW_GAP     7
#define BUTTON_PAD  10              /* each side of a button's caption */
#define TEXT_PAD    8               /* each side of a text display */
#define FIELD_PAD   12              /* string gadget frame and inset */
#define CYCLE_GLYPH 20              /* cycle arrow and its divider */
#define SCROLLER_W  16              /* GTLV_ScrollWidth */
#define FIELD_CHARS 15              /* a dotted quad, a 15-character name */
#define TARGET_W    632             /* 640 wide on a stock Workbench */
#define CHECK_W     26              /* GadTools CHECKBOX_WIDTH */
#define CHECK_H     11              /* GadTools CHECKBOX_HEIGHT */

const NpItem np_items[NP_L_COUNT] =
{
    [NP_L_INTERFACE]     = { "Interfaces",      NP_PLACE_ABOVE, NP_PAGE_COMMON },
    [NP_L_NEW]           = { "New",             NP_PLACE_IN,    NP_PAGE_COMMON },
    [NP_L_REMOVE]        = { "Remove",          NP_PLACE_IN,    NP_PAGE_COMMON },
    [NP_L_PANEL]         = { "Settings",        NP_PLACE_LEFT,  NP_PAGE_COMMON },
    [NP_L_STATUS]        = { 0,                 NP_PLACE_NONE,  NP_PAGE_COMMON },
    [NP_L_SAVE]          = { "Save",            NP_PLACE_IN,    NP_PAGE_COMMON },
    [NP_L_APPLY]         = { "Save & Start",    NP_PLACE_IN,    NP_PAGE_COMMON },
    [NP_L_LIVE_STATUS]   = { 0,                 NP_PLACE_NONE,  NP_PAGE_COMMON },
    [NP_L_LIVE_ACTION]   = { 0,                 NP_PLACE_IN,    NP_PAGE_COMMON },
    [NP_L_CLOSE]         = { "Close",           NP_PLACE_IN,    NP_PAGE_COMMON },
    [NP_L_NAME]          = { "Name",            NP_PLACE_LEFT,  NP_PAGE_GENERAL },
    [NP_L_ID]            = { "ID",              NP_PLACE_LEFT,  NP_PAGE_GENERAL },
    [NP_L_PRIORITY]      = { "Priority",        NP_PLACE_LEFT,  NP_PAGE_GENERAL },
    [NP_L_STATE]         = { "Start online",    NP_PLACE_RIGHT, NP_PAGE_GENERAL },
    [NP_L_BOOT]          = { "At boot",         NP_PLACE_RIGHT, NP_PAGE_GENERAL },
    [NP_L_MDNS]          = { "mDNS",            NP_PLACE_RIGHT, NP_PAGE_GENERAL },
    [NP_L_IPV4]          = { "Mode",            NP_PLACE_LEFT,  NP_PAGE_IPV4 },
    [NP_L_ADDRESS]       = { "Address",         NP_PLACE_LEFT,  NP_PAGE_IPV4 },
    [NP_L_NETMASK]       = { "Netmask",         NP_PLACE_LEFT,  NP_PAGE_IPV4 },
    [NP_L_GATEWAY]       = { "Gateway",         NP_PLACE_LEFT,  NP_PAGE_IPV4 },
    [NP_L_IPV6]          = { "Mode",            NP_PLACE_LEFT,  NP_PAGE_IPV6 },
    [NP_L_ADDRESS6_1]    = { "Address 1",       NP_PLACE_LEFT,  NP_PAGE_IPV6 },
    [NP_L_ADDRESS6_2]    = { "Address 2",       NP_PLACE_LEFT,  NP_PAGE_IPV6 },
    [NP_L_GATEWAY6]      = { "Gateway",         NP_PLACE_LEFT,  NP_PAGE_IPV6 },
    [NP_L_DEVICE]        = { "Device",          NP_PLACE_LEFT,  NP_PAGE_DEVICE },
    [NP_L_DEVICE_BROWSE] = { 0,                 NP_PLACE_NONE,  NP_PAGE_DEVICE },
    [NP_L_UNIT]          = { "Unit",            NP_PLACE_LEFT,  NP_PAGE_DEVICE },
    [NP_L_CARD]          = { "Card",            NP_PLACE_LEFT,  NP_PAGE_DEVICE },
    [NP_L_HWADDRESS]     = { "MAC",             NP_PLACE_LEFT,  NP_PAGE_DEVICE },
    [NP_L_DOWN_OFFLINE]  = { "Offline on down", NP_PLACE_RIGHT, NP_PAGE_DEVICE },
    [NP_L_INIT_DELAY]    = { "Init delay",      NP_PLACE_RIGHT, NP_PAGE_DEVICE },
    [NP_L_PROMISCUOUS]   = { "Promiscuous",     NP_PLACE_RIGHT, NP_PAGE_DEVICE },
    [NP_L_MTU]           = { "MTU",             NP_PLACE_LEFT,  NP_PAGE_TUNING },
    [NP_L_RXBUFFER]      = { "RX buffer",       NP_PLACE_LEFT,  NP_PAGE_TUNING },
    [NP_L_IPREQUESTS]    = { "IP reads",        NP_PLACE_LEFT,  NP_PAGE_TUNING },
    [NP_L_ARPREQUESTS]   = { "ARP reads",       NP_PLACE_LEFT,  NP_PAGE_TUNING },
    [NP_L_WRITEREQUESTS] = { "Writes",          NP_PLACE_LEFT,  NP_PAGE_TUNING },
    [NP_L_NOTE]          = { "0 = automatic",   NP_PLACE_IN,    NP_PAGE_TUNING },
};

const char *const np_panel_labels[] =
    { "General", "IPv4", "IPv6", "Device", "Tuning", 0 };
const char *const np_ipv4_labels[] =
    { "Static", "DHCP", "Link-local", "Off", 0 };
const char *const np_ipv6_labels[] =
    { "Off", "Link-local", "Automatic", "Static", "DHCPv6", 0 };
/* Indexed by NetPrefs' live state; the last is also the initial text. */
const char *const np_live_labels[] =
    { "New", "Stack off", "Not added", "Offline", "Online", "Unknown", 0 };
const char *const np_action_labels[] = { "Online", "Offline", 0 };

enum { K_FILL, K_INT, K_CYCLE_IPV4, K_CYCLE_IPV6, K_CHECK, K_TEXT, K_ICON };
enum { AT_FIELD, AT_PANEL, AT_NEXT, AT_RIGHT };

/* One gadget on a page row.  AT_FIELD: the shared field column; AT_PANEL:
   the page's left edge; AT_NEXT: after the previous cell; AT_RIGHT: flush
   with the right edge.  A K_FILL cell stretches to the right edge. */
typedef struct NpCell
{
    unsigned char item, row, kind, digits, at;
} NpCell;

static const NpCell cells[] =
{
    { NP_L_NAME,          0, K_FILL,       0,  AT_FIELD },
    { NP_L_ID,            1, K_FILL,       0,  AT_FIELD },
    { NP_L_PRIORITY,      2, K_INT,        4,  AT_FIELD },
    { NP_L_STATE,         2, K_CHECK,      0,  AT_NEXT  },
    { NP_L_BOOT,          2, K_CHECK,      0,  AT_NEXT  },
    { NP_L_MDNS,          3, K_CHECK,      0,  AT_FIELD },

    { NP_L_IPV4,          0, K_CYCLE_IPV4, 0,  AT_FIELD },
    { NP_L_ADDRESS,       1, K_FILL,       0,  AT_FIELD },
    { NP_L_NETMASK,       2, K_FILL,       0,  AT_FIELD },
    { NP_L_GATEWAY,       3, K_FILL,       0,  AT_FIELD },

    { NP_L_IPV6,          0, K_CYCLE_IPV6, 0,  AT_FIELD },
    { NP_L_ADDRESS6_1,    1, K_FILL,       0,  AT_FIELD },
    { NP_L_ADDRESS6_2,    2, K_FILL,       0,  AT_FIELD },
    { NP_L_GATEWAY6,      3, K_FILL,       0,  AT_FIELD },

    { NP_L_DEVICE,        0, K_FILL,       0,  AT_FIELD },
    { NP_L_DEVICE_BROWSE, 0, K_ICON,       0,  AT_RIGHT },
    { NP_L_UNIT,          1, K_INT,        3,  AT_FIELD },
    { NP_L_CARD,          1, K_FILL,       0,  AT_NEXT  },
    { NP_L_HWADDRESS,     2, K_FILL,       0,  AT_FIELD },
    { NP_L_DOWN_OFFLINE,  3, K_CHECK,      0,  AT_PANEL },
    { NP_L_INIT_DELAY,    3, K_CHECK,      0,  AT_NEXT  },
    { NP_L_PROMISCUOUS,   3, K_CHECK,      0,  AT_NEXT  },

    /* Same digits per column so the fields line up. */
    { NP_L_MTU,           0, K_INT,        6,  AT_FIELD },
    { NP_L_RXBUFFER,      0, K_INT,        10, AT_RIGHT },
    { NP_L_IPREQUESTS,    1, K_INT,        6,  AT_FIELD },
    { NP_L_ARPREQUESTS,   1, K_INT,        10, AT_RIGHT },
    { NP_L_WRITEREQUESTS, 2, K_INT,        6,  AT_FIELD },
    { NP_L_NOTE,          2, K_TEXT,       0,  AT_RIGHT },
};

#define CELL_COUNT ((int)(sizeof(cells) / sizeof(cells[0])))
#define PAGE_ROWS  4

typedef struct Metrics
{
    const NpFont *font;
    int gh;                         /* gadget height */
    int bh;                         /* bottom-row button height */
    int cb_w, cb_h;                 /* scaled checkbox */
    int digit_w;
    int field_min;
} Metrics;

static int max_i(int a, int b) { return a > b ? a : b; }

static int text_w(const Metrics *m, const char *s)
{
    int n = 0;

    if (s == 0) return 0;
    while (s[n] != '\0') n++;
    return m->font->measure(m->font->ctx, s, n);
}

static int widest(const Metrics *m, const char *const *list)
{
    int w = 0;

    while (*list != 0) w = max_i(w, text_w(m, *list++));
    return w;
}

static int label_w(const Metrics *m, int item)
{
    return text_w(m, np_items[item].label);
}

static int button_w(int text)
{
    return text + 2 * BUTTON_PAD;
}

static int cell_w(const Metrics *m, const NpCell *c)
{
    switch (c->kind)
    {
    case K_INT:        return (c->digits + 1) * m->digit_w + FIELD_PAD;
    case K_CYCLE_IPV4: return widest(m, np_ipv4_labels) + CYCLE_GLYPH +
                              2 * BUTTON_PAD;
    case K_CYCLE_IPV6: return widest(m, np_ipv6_labels) + CYCLE_GLYPH +
                              2 * BUTTON_PAD;
    case K_CHECK:      return m->cb_w;
    case K_TEXT:       return label_w(m, c->item);
    case K_ICON:       return m->gh + 8;
    default:           return m->field_min;
    }
}

/* Label cell for item at box b, the span GadTools may draw it in. */
static void place_label(const Metrics *m, NpLayout *l, int item)
{
    const NpBox *b = &l->box[item];
    NpBox *t = &l->label[item];
    int w = label_w(m, item);

    t->x = t->y = t->w = t->h = 0;
    if (w == 0) return;
    switch (np_items[item].place)
    {
    case NP_PLACE_LEFT:
        t->x = b->x - LABEL_GAP - w; t->w = w;
        t->y = b->y + (b->h - m->font->height) / 2; t->h = m->font->height;
        break;
    case NP_PLACE_RIGHT:
        t->x = b->x + b->w + LABEL_GAP; t->w = w;
        t->y = b->y + (b->h - m->font->height) / 2; t->h = m->font->height;
        break;
    case NP_PLACE_ABOVE:
        t->x = b->x + (b->w - w) / 2; t->w = w;
        t->y = b->y - m->font->height - 4; t->h = m->font->height;
        break;
    case NP_PLACE_IN:
        t->x = b->x + (b->w - w) / 2; t->w = w;
        t->y = b->y + (b->h - m->font->height) / 2; t->h = m->font->height;
        break;
    default:
        break;
    }
}

/* Places one page row and returns the rightmost pixel it needs. */
static int place_row(const Metrics *m, NpLayout *l, int page, int row,
                     int panel_x, int field_x, int right, int row_y)
{
    int pos = panel_x, need = panel_x;
    int i;

    for (i = 0; i < CELL_COUNT; i++)
    {
        const NpCell *c = &cells[i];
        NpBox *b = &l->box[c->item];
        int lead, trail, w, x;

        if (c->row != row || np_items[c->item].page != page) continue;
        w = cell_w(m, c);
        lead = np_items[c->item].place == NP_PLACE_LEFT
             ? label_w(m, c->item) + LABEL_GAP : 0;
        trail = np_items[c->item].place == NP_PLACE_RIGHT
              ? LABEL_GAP + label_w(m, c->item) : 0;

        switch (c->at)
        {
        case AT_FIELD: x = field_x; break;
        case AT_PANEL: x = panel_x + lead; break;
        case AT_NEXT:  x = pos + CELL_GAP + lead; break;
        default:
            x = right - w;
            need = max_i(need, pos + CELL_GAP + lead + w);
            break;
        }
        if (c->kind == K_FILL)
        {
            int cell_right = right;

            if (c->item == NP_L_DEVICE)
                cell_right -= m->gh + 8 + CELL_GAP;
            need = max_i(need, x + w);
            w = max_i(w, cell_right - x);
        }
        else if (c->at != AT_RIGHT)
            need = max_i(need, x + w + trail);

        b->x = x;
        b->w = w;
        b->h = c->kind == K_CHECK ? m->cb_h
             : c->kind == K_TEXT  ? m->font->height : m->gh;
        b->y = row_y + (m->gh - b->h) / 2;
        pos = x + w + trail;
        place_label(m, l, c->item);
    }
    return need;
}

static void place_common(const Metrics *m, NpLayout *l, int x0, int right)
{
    const int fh = m->font->height;
    const int top = MARGIN_Y;
    const int list_top = top + max_i(m->gh, fh + 4);
    const int bevel_y = list_top + 5;
    const int bevel_h = 2 * PAD_Y + PAGE_ROWS * m->gh + (PAGE_ROWS - 1) * ROW_GAP;
    const int status_y = bevel_y + bevel_h + 4;
    const int bottom_y = status_y + m->gh + 5;
    int left_w = l->sep_x - GAP - MARGIN_X;
    int half = (left_w - GAP) / 2;
    int x;
    NpBox *b = l->box;

    l->sep_top = top;
    l->sep_bottom = status_y + m->gh - 1;
    l->bevel.x = x0; l->bevel.y = bevel_y;
    l->bevel.w = right - x0; l->bevel.h = bevel_h;

    b[NP_L_PANEL].x = x0 + label_w(m, NP_L_PANEL) + LABEL_GAP;
    b[NP_L_PANEL].y = top;
    b[NP_L_PANEL].w = widest(m, np_panel_labels) + CYCLE_GLYPH + 2 * BUTTON_PAD;
    b[NP_L_PANEL].h = m->gh;

    b[NP_L_INTERFACE].x = MARGIN_X; b[NP_L_INTERFACE].y = list_top;
    b[NP_L_INTERFACE].w = left_w;
    b[NP_L_INTERFACE].h = bevel_y + bevel_h - list_top;

    b[NP_L_NEW].x = MARGIN_X; b[NP_L_NEW].y = status_y;
    b[NP_L_NEW].w = half; b[NP_L_NEW].h = m->gh;
    b[NP_L_REMOVE].x = MARGIN_X + left_w - half; b[NP_L_REMOVE].y = status_y;
    b[NP_L_REMOVE].w = half; b[NP_L_REMOVE].h = m->gh;

    b[NP_L_STATUS].x = x0; b[NP_L_STATUS].y = status_y;
    b[NP_L_STATUS].w = right - x0; b[NP_L_STATUS].h = m->gh;

    x = MARGIN_X;
    b[NP_L_SAVE].x = x;
    b[NP_L_SAVE].w = button_w(label_w(m, NP_L_SAVE));
    x += b[NP_L_SAVE].w + GAP;
    b[NP_L_APPLY].x = x;
    b[NP_L_APPLY].w = button_w(label_w(m, NP_L_APPLY));
    x += b[NP_L_APPLY].w + GAP;
    b[NP_L_LIVE_STATUS].x = x;
    b[NP_L_LIVE_STATUS].w = widest(m, np_live_labels) + 2 * TEXT_PAD;
    x += b[NP_L_LIVE_STATUS].w + GAP;
    b[NP_L_LIVE_ACTION].x = x;
    b[NP_L_LIVE_ACTION].w = button_w(widest(m, np_action_labels));
    b[NP_L_CLOSE].w = button_w(label_w(m, NP_L_CLOSE));
    b[NP_L_CLOSE].x = right - b[NP_L_CLOSE].w;
    b[NP_L_SAVE].y = b[NP_L_APPLY].y = b[NP_L_LIVE_ACTION].y =
        b[NP_L_CLOSE].y = bottom_y;
    b[NP_L_SAVE].h = b[NP_L_APPLY].h = b[NP_L_LIVE_ACTION].h =
        b[NP_L_CLOSE].h = m->bh;
    b[NP_L_LIVE_STATUS].y = bottom_y + (m->bh - m->gh) / 2;
    b[NP_L_LIVE_STATUS].h = m->gh;

    l->inner_h = bottom_y + m->bh + 4;
    for (x = 0; x < NP_L_COUNT; x++)
        if (np_items[x].page == NP_PAGE_COMMON) place_label(m, l, x);
}

/* Width the common controls need, right edge included. */
static int common_need(const Metrics *m, int x0)
{
    int bottom = MARGIN_X
               + button_w(label_w(m, NP_L_SAVE)) + GAP
               + button_w(label_w(m, NP_L_APPLY)) + GAP
               + widest(m, np_live_labels) + 2 * TEXT_PAD + GAP
               + button_w(widest(m, np_action_labels)) + 2 * GAP
               + button_w(label_w(m, NP_L_CLOSE));
    int header = x0 + label_w(m, NP_L_PANEL) + LABEL_GAP
               + widest(m, np_panel_labels) + CYCLE_GLYPH + 2 * BUTTON_PAD;
    return max_i(bottom, header);
}

static int page_y(const NpLayout *l, const Metrics *m, int row)
{
    return l->bevel.y + PAD_Y + row * (m->gh + ROW_GAP);
}

int np_layout(const NpFont *font, int max_w, int max_h, NpLayout *out)
{
    Metrics m;
    int left_w, x0, panel_x, field_x, need, right, target, i, page, row;

    m.font = font;
    m.gh = font->height + 7;
    m.bh = m.gh + 2;
    m.cb_h = font->height + 3;
    /* Taller, not wider: the row width is the scarce dimension. */
    m.cb_w = CHECK_W + m.cb_h - CHECK_H;
    m.digit_w = (text_w(&m, "0123456789") + 9) / 10;
    m.field_min = FIELD_CHARS * m.digit_w + FIELD_PAD;

    left_w = max_i(14 * m.digit_w + SCROLLER_W + 8, label_w(&m, NP_L_INTERFACE));
    left_w = max_i(left_w, 2 * max_i(button_w(label_w(&m, NP_L_NEW)),
                                     button_w(label_w(&m, NP_L_REMOVE)))
                           + GAP);
    out->sep_x = MARGIN_X + left_w + GAP;
    x0 = out->sep_x + 2 + GAP;
    panel_x = x0 + PAD_X;

    field_x = 0;
    for (i = 0; i < CELL_COUNT; i++)
        if (cells[i].at == AT_FIELD &&
            np_items[cells[i].item].place == NP_PLACE_LEFT)
            field_x = max_i(field_x, label_w(&m, cells[i].item));
    field_x += panel_x + LABEL_GAP;

    /* First pass with no room to spare finds the width every row needs. */
    need = common_need(&m, x0);
    for (page = NP_PAGE_GENERAL; page < NP_PAGE_COMMON; page++)
        for (row = 0; row < PAGE_ROWS; row++)
            need = max_i(need, place_row(&m, out, page, row, panel_x, field_x,
                                         0, 0) + PAD_X);

    target = TARGET_W < max_w ? TARGET_W : max_w;
    out->inner_w = max_i(need + MARGIN_X, target);
    right = out->inner_w - MARGIN_X;

    place_common(&m, out, x0, right);
    for (page = NP_PAGE_GENERAL; page < NP_PAGE_COMMON; page++)
        for (row = 0; row < PAGE_ROWS; row++)
            (void)place_row(&m, out, page, row, panel_x, field_x,
                            right - PAD_X, page_y(out, &m, row));

    return out->inner_w <= max_w && out->inner_h <= max_h;
}

/*
 * NetMeter, a small Workbench window with what a user asks of the network:
 * each interface's name and addresses, and how fast it is receiving and
 * sending.  Hovering shows the details -- the definition file, the totals --
 * and a click on an address puts it on the clipboard.
 *
 * Plain Intuition and graphics, no GadTools: nothing in the window is a
 * control except the two addresses, and those are hit-tested here.  The
 * numbers come from the same snapshot ShowNetStatus prints (tool_snapshot),
 * once a second; the hover is polled ten times a second from the screen's
 * pointer position, so a tooltip appears over an inactive window too.
 *
 * Its place on the screen is kept the way a Workbench tool keeps it: the
 * LEFT, TOP, WIDTH and HEIGHT ToolTypes of its own icon, written by the
 * Project menu's Snapshot and read at start.  From a Shell the same four are
 * arguments, and they win over the icon.
 *
 * A screen mode change closes Workbench's screen and opens it again, and it
 * cannot close while this window is on it.  With screennotify.library
 * installed, NetMeter is told first: it closes its window and lets the screen
 * go, and opens again where it was once Workbench is back.
 *
 * SPDX-License-Identifier: MIT
 */

#include "tools.h"
#include "tools_nx.h"
#include "netmeter_fmt.h"
#include "aminetxduo/version.h"

#include <exec/io.h>
#include <exec/memory.h>
#include <exec/tasks.h>
#include <devices/clipboard.h>
#include <devices/timer.h>
#include <graphics/gfxbase.h>
#include <graphics/gfxmacros.h>
#include <graphics/rastport.h>
#include <graphics/text.h>
#include <graphics/view.h>
#include <intuition/intuition.h>
#include <intuition/intuitionbase.h>
#include <intuition/screens.h>
#include <libraries/gadtools.h>
#include <workbench/icon.h>
#include <workbench/startup.h>
#include <workbench/workbench.h>
#include <proto/dos.h>
#include <proto/gadtools.h>
#include <proto/icon.h>
#include <proto/exec.h>
#include <proto/graphics.h>
#include <proto/intuition.h>
#include <proto/layers.h>
#include <utility/tagitem.h>
#include <inline/macros.h>

const char *const tool_name = "NetMeter";

static const char version_tag[] __attribute__((used)) =
    TOOL_VERSTAG("NetMeter");

struct IntuitionBase *IntuitionBase;
struct GfxBase       *GfxBase;
struct Library       *LayersBase;
struct Library       *GadToolsBase;
struct Library       *IconBase;

/*
 * screennotify.library, Stefan Becker 1995, Aminet util/libs/ScreenNotify10.lha.
 * Its licence lets no part of the package be included in other software, so
 * neither the library nor its headers ship with AmiNetXDuo: this declares the
 * documented interface only, and NetMeter uses the library where the user has
 * installed it.  AmigaOS 3.2 has no equivalent of its own (IntuitionControlA()
 * defines no public tags), and TinyMeter used the same library for this.
 */
struct Library       *ScreenNotifyBase;

#define NM_SN_TYPE_WORKBENCH    3       /* snm_Value FALSE: closing, TRUE: open */

typedef struct
{
    struct Message snm_Message;
    ULONG          snm_Type;
    APTR           snm_Value;
} NmScreenNotifyMessage;

#define AddWorkbenchClient(port, pri) \
    LP2(0x36, APTR, AddWorkbenchClient, struct MsgPort *, port, a0, \
        BYTE, pri, d0, , ScreenNotifyBase)
#define RemWorkbenchClient(handle) \
    LP1(0x3c, BOOL, RemWorkbenchClient, APTR, handle, a0, , ScreenNotifyBase)

#define NM_PAD          4       /* window edge to content                  */
#define NM_TICK_US      100000UL
#define NM_TICKS_SEC    10      /* ticks between snapshots                 */
#define NM_HOVER_TICKS  5       /* half a second over a zone opens its tip */
#define NM_COPIED_TICKS 15
#define NM_TIP_LINES    3
#define NM_TIP_LEN      160
#define NM_ARROW_W      7

enum { NM_ZONE_NONE, NM_ZONE_NAME, NM_ZONE_IP4, NM_ZONE_IP6, NM_ZONE_BARS };

enum { NM_MENU_SNAPSHOT = 1, NM_MENU_QUIT };

/* The four geometry ToolTypes, in the order nm.want keeps them. */
#define NM_GEOMETRY     4
#define NM_TT_MAX       32      /* the icon's other ToolTypes kept on a save */
#define NM_NAME_LEN     108

static const char *const nm_geometry_keys[NM_GEOMETRY] =
    { "LEFT", "TOP", "WIDTH", "HEIGHT" };

static struct NewMenu nm_menu[] =
{
    { NM_TITLE, (STRPTR)"Project",  NULL,       0, 0, NULL },
    { NM_ITEM,  (STRPTR)"Snapshot", (STRPTR)"S", 0, 0, (APTR)NM_MENU_SNAPSHOT },
    { NM_ITEM,  NM_BARLABEL,        NULL,       0, 0, NULL },
    { NM_ITEM,  (STRPTR)"Quit",     (STRPTR)"Q", 0, 0, (APTR)NM_MENU_QUIT },
    { NM_END,   NULL,               NULL,       0, 0, NULL }
};

typedef struct
{
    WORD x, y, w, h;
} NmRect;

typedef struct
{
    UWORD   nx_index;
    char    name[NETSTATUS_NAME_LEN];
    char    ip4[16];
    char    ip6[48];
    char    device[NETSTATUS_FILE_LEN];
    ULONG   unit;
    ULONG   bps;
    BOOL    up;
    BOOL    have_bytes;
    BOOL    have_prev;
    ULONG   rx_hi, rx_lo, tx_hi, tx_lo;
    ULONG   rx_rate, tx_rate;
    ULONG   scale;
    /* Where the last full draw put things; the hover and the click test them. */
    NmRect  r_name, r_ip4, r_ip6, r_bars, r_rx, r_tx, r_rxt, r_txt;
} NmIf;

static struct
{
    struct Screen    *screen;
    struct DrawInfo  *dri;
    struct Window    *win;
    struct Window    *tip;
    struct MsgPort   *tport;
    struct timerequest *treq;
    BOOL              timer_open;
    BOOL              timer_armed;

    UWORD             pen_text, pen_back, pen_shadow, pen_shine, pen_dim;
    LONG              pen_rx, pen_tx, pen_tipback;     /* -1: not obtained */
    UWORD             fh, base;
    UWORD             name_w;      /* widest name, for the address column */
    UWORD             rate_w;      /* "999 KB/s" at its widest            */

    NmIf              ifs[TOOL_MAX_IF];
    UWORD             count;
    BOOL              running;
    struct DateStamp  last;

    LONG              zone_if;
    UWORD             zone;
    UWORD             hover_ticks;
    UWORD             copied_ticks;
    UWORD             ticks;

    struct WBStartup *wbs;          /* NULL from a Shell                   */
    APTR              vi;
    struct Menu      *menu;
    LONG              want[NM_GEOMETRY];    /* -1: not given             */

    struct MsgPort   *sn_port;          /* screennotify.library, if any    */
    APTR              sn_handle;
} nm = { .pen_rx = -1, .pen_tx = -1, .pen_tipback = -1, .zone_if = -1,
         .want = { -1, -1, -1, -1 } };

static ToolSnapshot nm_snap;

/* The screen's font -- Font prefs' "Screen text", proportional or not --
   set on every RastPort NetMeter draws in and on this one, which measures
   before the window exists. */
static struct RastPort nm_mrp;

/* ----------------------------------------------------------- text, */

static ULONG nm_len(const char *s)
{
    ULONG n = 0;
    while (s[n] != '\0') n++;
    return n;
}

static VOID nm_cat(char *dst, ULONG dstlen, const char *s)
{
    ULONG n = nm_len(dst);
    while (*s != '\0' && n + 1 < dstlen) dst[n++] = *s++;
    dst[n] = '\0';
}

static VOID nm_cat_ulong(char *dst, ULONG dstlen, ULONG v)
{
    char  rev[11];
    char  out[12];
    ULONG n = 0, i = 0;

    do { rev[n++] = (char)('0' + v % 10UL); v /= 10UL; }
    while (v != 0 && n < sizeof(rev));
    while (n != 0) out[i++] = rev[--n];
    out[i] = '\0';
    nm_cat(dst, dstlen, out);
}

static UWORD nm_text_w(struct RastPort *rp, const char *s)
{
    return (UWORD)TextLength(rp, (CONST_STRPTR)s, (UWORD)nm_len(s));
}

/*
 * Text at (x, baseline) in at most `w` pixels: what fits is drawn JAM2 over
 * the background and the rest of the field is cleared, so a shorter value
 * replaces a longer one without a redraw of the whole window.  `right`
 * aligns it to the field's right edge.
 */
static VOID nm_text_field(struct RastPort *rp, WORD x, WORD y, WORD w,
                          const char *s, UWORD pen, BOOL right)
{
    struct TextExtent te;
    ULONG             n = nm_len(s);
    ULONG             fit;
    WORD              tw;

    if (w <= 0)
        return;
    fit = TextFit(rp, (CONST_STRPTR)s, (UWORD)n, &te, NULL, 1,
                  (UWORD)w, (UWORD)(nm.fh + 1));
    tw  = (WORD)TextLength(rp, (CONST_STRPTR)s, (UWORD)fit);

    SetAPen(rp, nm.pen_back);
    if (right)
    {
        if (w - tw > 0)
            RectFill(rp, x, y - nm.base, x + w - tw - 1, y - nm.base + nm.fh - 1);
        x += w - tw;
    }
    else if (w - tw > 0)
        RectFill(rp, x + tw, y - nm.base, x + w - 1, y - nm.base + nm.fh - 1);

    SetAPen(rp, pen);
    SetBPen(rp, nm.pen_back);
    SetDrMd(rp, JAM2);
    Move(rp, x, y);
    Text(rp, (CONST_STRPTR)s, (UWORD)fit);
}

/* ---------------------------------------------------------- pens, */

static LONG nm_pen(ULONG rgb)
{
    struct TagItem tags[2];
    ULONG r = (rgb >> 16) & 0xFF, g = (rgb >> 8) & 0xFF, b = rgb & 0xFF;

    if (GfxBase->LibNode.lib_Version < 39)
        return -1;
    tags[0].ti_Tag = OBP_Precision; tags[0].ti_Data = PRECISION_GUI;
    tags[1].ti_Tag = TAG_DONE;      tags[1].ti_Data = 0;
    return ObtainBestPenA(nm.screen->ViewPort.ColorMap,
                          r * 0x01010101UL, g * 0x01010101UL,
                          b * 0x01010101UL, tags);
}

static VOID nm_pens(VOID)
{
    UWORD *p = nm.dri->dri_Pens;

    nm.pen_text   = p[TEXTPEN];
    nm.pen_back   = p[BACKGROUNDPEN];
    nm.pen_shadow = p[SHADOWPEN];
    nm.pen_shine  = p[SHINEPEN];
    nm.pen_dim    = p[SHADOWPEN];

    nm.pen_rx      = nm_pen(0x38B048UL);    /* green  */
    nm.pen_tx      = nm_pen(0xE88A20UL);    /* orange */
    nm.pen_tipback = nm_pen(0xFFF8C8UL);    /* pale yellow */
}

static UWORD nm_rx_pen(VOID)
{ return (UWORD)(nm.pen_rx >= 0 ? nm.pen_rx : nm.dri->dri_Pens[FILLPEN]); }
static UWORD nm_tx_pen(VOID)
{ return (UWORD)(nm.pen_tx >= 0 ? nm.pen_tx
                                : nm.dri->dri_Pens[HIGHLIGHTTEXTPEN]); }

static VOID nm_release_pens(VOID)
{
    struct ColorMap *cm = nm.screen->ViewPort.ColorMap;

    if (nm.pen_rx >= 0)      ReleasePen(cm, (ULONG)nm.pen_rx);
    if (nm.pen_tx >= 0)      ReleasePen(cm, (ULONG)nm.pen_tx);
    if (nm.pen_tipback >= 0) ReleasePen(cm, (ULONG)nm.pen_tipback);
    nm.pen_rx = nm.pen_tx = nm.pen_tipback = -1;
}

/* ------------------------------------------------------------ data, */

/* The global address when there is one: that is the one worth copying. */
static VOID nm_pick_ip6(UWORD nx_index, char *out, ULONG outlen)
{
    UWORD i;
    const char *link = NULL;

    out[0] = '\0';
    for (i = 0; i < nm_snap.addr6_count; i++)
    {
        const ToolAddr6Info *a = &nm_snap.addr6[i];

        if (a->nx_index != nx_index || a->state == NETSTATUS_IP6_TENTATIVE)
            continue;
        if ((a->text[0] == 'f' || a->text[0] == 'F') &&
            (a->text[1] == 'e' || a->text[1] == 'E') &&
            (a->text[2] == '8'))
        {
            if (link == NULL) link = a->text;
            continue;
        }
        tool_copy_string(out, outlen, a->text);
        return;
    }
    if (link != NULL)
        tool_copy_string(out, outlen, link);
}

/*
 * One snapshot.  TRUE when what the window shows changed shape -- an
 * interface came or went, an address changed -- and needs a full redraw;
 * the rates alone are redrawn in place.
 */
static BOOL nm_sample(VOID)
{
    static NmIf      prev[TOOL_MAX_IF];
    struct DateStamp now;
    ULONG            ms;
    UWORD            prev_count = nm.count;
    BOOL             reshaped = FALSE;
    UWORD            i, n = 0;

    /* DateStamp's 20 ms is resolution enough for a rate over a second, and
       it needs no timer.device base of its own. */
    DateStamp(&now);
    ms = (ULONG)((now.ds_Days - nm.last.ds_Days) * 1440L +
                 (now.ds_Minute - nm.last.ds_Minute)) * 60000UL +
         (ULONG)(now.ds_Tick - nm.last.ds_Tick) * (1000UL / TICKS_PER_SECOND);
    nm.last = now;

    for (i = 0; i < nm.count; i++)
        prev[i] = nm.ifs[i];

    if (tool_snapshot(&nm_snap, FALSE) != 0)
    {
        reshaped   = nm.running || nm.count != 0;
        nm.running = FALSE;
        nm.count   = 0;
        return reshaped;
    }
    if (!nm.running)
        reshaped = TRUE;
    nm.running = TRUE;

    for (i = 0; i < nm_snap.iface_count && n < TOOL_MAX_IF; i++)
    {
        const ToolIfInfo *in = &nm_snap.iface[i];
        NmIf             *out = &nm.ifs[n];
        const NmIf       *was = NULL;
        UWORD             j;

        if (!in->attached || !in->have_sana2 || in->nx_name[0] == '\0')
            continue;

        for (j = 0; j < prev_count; j++)
            if (tool_stricmp(prev[j].name, in->nx_name) == 0)
                was = &prev[j];

        out->nx_index = in->nx_index;
        tool_copy_string(out->name, sizeof(out->name), in->nx_name);
        if (in->address != 0)
            ami_config_format_ip(in->address, out->ip4, sizeof(out->ip4));
        else
            out->ip4[0] = '\0';
        nm_pick_ip6(in->nx_index, out->ip6, sizeof(out->ip6));
        tool_copy_string(out->device, sizeof(out->device), in->nx_device);
        out->unit       = in->nx_unit;
        out->bps        = in->bps;
        out->up         = (BOOL)(in->link_up && in->sana2_online);
        out->have_bytes = in->have_bytes;

        out->rx_rate = out->tx_rate = 0;
        out->scale   = (was != NULL) ? was->scale : NM_SCALE_FLOOR;
        if (was != NULL && was->have_prev && in->have_bytes)
        {
            out->rx_rate = nm_rate(nm_delta(in->stats.rx_bytes_hi,
                                            in->stats.rx_bytes,
                                            was->rx_hi, was->rx_lo), ms);
            out->tx_rate = nm_rate(nm_delta(in->stats.tx_bytes_hi,
                                            in->stats.tx_bytes,
                                            was->tx_hi, was->tx_lo), ms);
        }
        out->scale     = nm_scale_next(out->scale, out->rx_rate, out->tx_rate);
        out->rx_hi     = in->stats.rx_bytes_hi;
        out->rx_lo     = in->stats.rx_bytes;
        out->tx_hi     = in->stats.tx_bytes_hi;
        out->tx_lo     = in->stats.tx_bytes;
        out->have_prev = in->have_bytes;

        if (was == NULL || was->up != out->up ||
            tool_stricmp(was->ip4, out->ip4) != 0 ||
            tool_stricmp(was->ip6, out->ip6) != 0 ||
            (was->ip6[0] == '\0') != (out->ip6[0] == '\0'))
            reshaped = TRUE;
        if (was != NULL)
        {
            out->r_name = was->r_name; out->r_ip4 = was->r_ip4;
            out->r_ip6  = was->r_ip6;  out->r_bars = was->r_bars;
            out->r_rx   = was->r_rx;   out->r_tx  = was->r_tx;
            out->r_rxt  = was->r_rxt;  out->r_txt = was->r_txt;
        }
        n++;
    }
    if (n != prev_count)
        reshaped = TRUE;
    nm.count = n;
    return reshaped;
}

/* ---------------------------------------------------------- layout, */

static VOID nm_measure(struct RastPort *rp)
{
    UWORD i, w;

    nm.fh   = rp->TxHeight;
    nm.base = rp->TxBaseline;
    /* The widest a rate can print, in a font whose digits and letters need
       not be the same width. */
    {
        static const char *const widest[] =
            { "1023 B/s", "999 KB/s", "99.9 KB/s", "9.99 MB/s", "99.9 MB/s" };

        nm.rate_w = 0;
        for (i = 0; i < sizeof(widest) / sizeof(widest[0]); i++)
        {
            w = nm_text_w(rp, widest[i]);
            if (w > nm.rate_w) nm.rate_w = w;
        }
    }

    nm.name_w = nm_text_w(rp, "eth0");
    for (i = 0; i < nm.count; i++)
    {
        w = nm_text_w(rp, nm.ifs[i].name);
        if (w > nm.name_w) nm.name_w = w;
    }
    /* Bold is a pixel wider per glyph run; leave it room. */
    nm.name_w += 2;
}

/* Rows of text an interface takes: name and IPv4, IPv6 if any, the bars. */
static UWORD nm_rows(const NmIf *f)
{
    return (UWORD)(f->ip6[0] != '\0' ? 3 : 2);
}

static UWORD nm_content_h(VOID)
{
    UWORD i, h = 0;

    if (!nm.running || nm.count == 0)
        return (UWORD)(nm.fh + 2 * NM_PAD);
    for (i = 0; i < nm.count; i++)
        h = (UWORD)(h + nm_rows(&nm.ifs[i]) * (nm.fh + 2) + 2 * NM_PAD);
    return h;
}

static UWORD nm_content_w(struct RastPort *rp)
{
    UWORD a = (UWORD)(nm_text_w(rp, "2001:db8:1234:5678::abcd") + 1);
    UWORD i, lw;

    for (i = 0; i < nm.count; i++)
    {
        lw = (UWORD)(nm_text_w(rp, nm.ifs[i].ip6) + 1);
        if (lw > a) a = lw;
        lw = (UWORD)(nm_text_w(rp, nm.ifs[i].ip4) + 1);
        if (lw > a) a = lw;
    }
    UWORD dot = (UWORD)(nm.fh / 2 + 4);
    UWORD addr = (UWORD)(2 * NM_PAD + dot + nm.name_w + 8 + a);
    UWORD bars = (UWORD)(2 * NM_PAD + 2 * (NM_ARROW_W + 3 + 40 + 4 + nm.rate_w)
                         + 10);
    return addr > bars ? addr : bars;
}

/* ------------------------------------------------------------ draw, */

static VOID nm_rect(NmRect *r, WORD x, WORD y, WORD w, WORD h)
{
    r->x = x; r->y = y; r->w = w; r->h = h;
}

static VOID nm_arrow(struct RastPort *rp, WORD cx, WORD top, BOOL down,
                     UWORD pen)
{
    WORD i;

    SetAPen(rp, pen);
    for (i = 0; i < 4; i++)
    {
        WORD y = down ? (WORD)(top + i) : (WORD)(top + 3 - i);
        Move(rp, (WORD)(cx - 3 + i), y);
        Draw(rp, (WORD)(cx + 3 - i), y);
    }
}

static VOID nm_bar(struct RastPort *rp, const NmRect *r, ULONG rate,
                   ULONG scale, UWORD pen)
{
    WORD  inner = (WORD)(r->w - 2);
    WORD  fill;

    if (inner <= 0)
        return;
    fill = (WORD)nm_bar_fill(rate, scale, (ULONG)inner);
    if (rate != 0 && fill == 0)
        fill = 1;

    SetAPen(rp, nm.pen_shadow);
    Move(rp, r->x, (WORD)(r->y + r->h - 1));
    Draw(rp, r->x, r->y);
    Draw(rp, (WORD)(r->x + r->w - 1), r->y);
    SetAPen(rp, nm.pen_shine);
    Draw(rp, (WORD)(r->x + r->w - 1), (WORD)(r->y + r->h - 1));
    Draw(rp, (WORD)(r->x + 1), (WORD)(r->y + r->h - 1));

    if (fill > 0)
    {
        SetAPen(rp, pen);
        RectFill(rp, (WORD)(r->x + 1), (WORD)(r->y + 1),
                 (WORD)(r->x + fill), (WORD)(r->y + r->h - 2));
    }
    if (fill < inner)
    {
        SetAPen(rp, nm.pen_back);
        RectFill(rp, (WORD)(r->x + 1 + fill), (WORD)(r->y + 1),
                 (WORD)(r->x + inner), (WORD)(r->y + r->h - 2));
    }
}

static VOID nm_draw_rates(struct RastPort *rp, NmIf *f)
{
    char s[24];

    nm_bar(rp, &f->r_rx, f->rx_rate, f->scale, nm_rx_pen());
    nm_bar(rp, &f->r_tx, f->tx_rate, f->scale, nm_tx_pen());
    nm_format_rate(f->rx_rate, s, sizeof(s));
    nm_text_field(rp, f->r_rxt.x, (WORD)(f->r_rxt.y + nm.base), f->r_rxt.w,
                  s, nm.pen_text, TRUE);
    nm_format_rate(f->tx_rate, s, sizeof(s));
    nm_text_field(rp, f->r_txt.x, (WORD)(f->r_txt.y + nm.base), f->r_txt.w,
                  s, nm.pen_text, TRUE);
}

static VOID nm_draw_all(VOID)
{
    struct Window   *w = nm.win;
    struct RastPort *rp = w->RPort;
    WORD             x0 = (WORD)(w->BorderLeft + NM_PAD);
    WORD             x1 = (WORD)(w->Width - w->BorderRight - NM_PAD);
    WORD             y  = (WORD)(w->BorderTop + NM_PAD);
    WORD             bottom = (WORD)(w->Height - w->BorderBottom - 1);
    WORD             row = (WORD)(nm.fh + 2);
    WORD             dot = (WORD)(nm.fh / 2);
    WORD             col = (WORD)(x0 + dot + 4 + nm.name_w + 8);
    UWORD            i;

    SetAPen(rp, nm.pen_back);
    RectFill(rp, w->BorderLeft, w->BorderTop,
             (WORD)(w->Width - w->BorderRight - 1), bottom);
    nm_measure(rp);
    col = (WORD)(x0 + dot + 4 + nm.name_w + 8);

    if (!nm.running || nm.count == 0)
    {
        nm_text_field(rp, x0, (WORD)(y + nm.base), (WORD)(x1 - x0),
                      nm.running ? "No interfaces" : "Network not running",
                      nm.pen_text, FALSE);
        return;
    }

    for (i = 0; i < nm.count; i++)
    {
        NmIf *f = &nm.ifs[i];
        WORD  half, bw, bx;

        if (i != 0)
        {
            SetAPen(rp, nm.pen_shadow);
            Move(rp, x0, (WORD)(y - NM_PAD));
            Draw(rp, x1, (WORD)(y - NM_PAD));
        }

        /* The state dot, then the name in bold. */
        SetAPen(rp, f->up ? nm_rx_pen() : nm.pen_shadow);
        RectFill(rp, x0, (WORD)(y + (nm.fh - dot) / 2),
                 (WORD)(x0 + dot - 1), (WORD)(y + (nm.fh - dot) / 2 + dot - 1));
        SetSoftStyle(rp, FSF_BOLD, AskSoftStyle(rp));
        nm_text_field(rp, (WORD)(x0 + dot + 4), (WORD)(y + nm.base),
                      (WORD)nm.name_w, f->name, nm.pen_text, FALSE);
        SetSoftStyle(rp, FS_NORMAL, AskSoftStyle(rp));
        nm_rect(&f->r_name, x0, y, (WORD)(dot + 4 + nm.name_w), (WORD)nm.fh);

        nm_text_field(rp, col, (WORD)(y + nm.base), (WORD)(x1 - col),
                      f->ip4[0] != '\0' ? f->ip4 : "no address",
                      f->ip4[0] != '\0' ? nm.pen_text : nm.pen_dim, FALSE);
        nm_rect(&f->r_ip4, col, y,
                (WORD)(f->ip4[0] != '\0' ? nm_text_w(rp, f->ip4) : 0),
                (WORD)nm.fh);
        y = (WORD)(y + row);

        nm_rect(&f->r_ip6, 0, 0, 0, 0);
        if (f->ip6[0] != '\0')
        {
            nm_text_field(rp, col, (WORD)(y + nm.base), (WORD)(x1 - col),
                          f->ip6, nm.pen_text, FALSE);
            nm_rect(&f->r_ip6, col, y, (WORD)nm_text_w(rp, f->ip6),
                    (WORD)nm.fh);
            y = (WORD)(y + row);
        }

        /* Two halves: receive, then send, each arrow, bar, rate. */
        half = (WORD)((x1 - x0 - 10) / 2);
        bw   = (WORD)(half - NM_ARROW_W - 3 - 4 - nm.rate_w);
        if (bw < 8) bw = 8;

        bx = x0;
        nm_arrow(rp, (WORD)(bx + 3), (WORD)(y + (nm.fh - 4) / 2), TRUE,
                 nm_rx_pen());
        nm_rect(&f->r_rx, (WORD)(bx + NM_ARROW_W + 3), (WORD)(y + 1), bw,
                (WORD)(nm.fh - 2));
        nm_rect(&f->r_rxt, (WORD)(f->r_rx.x + bw + 4), y, (WORD)nm.rate_w,
                (WORD)nm.fh);

        bx = (WORD)(x0 + half + 10);
        nm_arrow(rp, (WORD)(bx + 3), (WORD)(y + (nm.fh - 4) / 2), FALSE,
                 nm_tx_pen());
        nm_rect(&f->r_tx, (WORD)(bx + NM_ARROW_W + 3), (WORD)(y + 1), bw,
                (WORD)(nm.fh - 2));
        nm_rect(&f->r_txt, (WORD)(f->r_tx.x + bw + 4), y, (WORD)nm.rate_w,
                (WORD)nm.fh);
        nm_rect(&f->r_bars, x0, y, (WORD)(x1 - x0), (WORD)nm.fh);

        nm_draw_rates(rp, f);
        y = (WORD)(y + row + 2 * NM_PAD);
    }
}

static VOID nm_draw_rates_all(VOID)
{
    UWORD i;

    for (i = 0; i < nm.count; i++)
        nm_draw_rates(nm.win->RPort, &nm.ifs[i]);
}

/* ------------------------------------------------------------- tip, */

static VOID nm_tip_close(VOID)
{
    if (nm.tip != NULL)
        CloseWindow(nm.tip);
    nm.tip = NULL;
}

static VOID nm_tip_open(char lines[][NM_TIP_LEN], UWORD n)
{
    struct RastPort *srp = &nm_mrp;
    struct TagItem   tags[10];
    UWORD            i, w = 0, h;
    WORD             x, y;
    UWORD            back;

    nm_tip_close();
    for (i = 0; i < n; i++)
    {
        UWORD lw = nm_text_w(srp, lines[i]);
        if (lw > w) w = lw;
    }
    w = (UWORD)(w + 2 * NM_PAD + 2);
    h = (UWORD)(n * (nm.fh + 1) + 2 * 3 + 1);

    x = (WORD)(nm.screen->MouseX + 4);
    y = (WORD)(nm.screen->MouseY + 20);
    if (x + w > nm.screen->Width)  x = (WORD)(nm.screen->Width - w);
    if (y + h > nm.screen->Height) y = (WORD)(nm.screen->MouseY - h - 4);
    if (x < 0) x = 0;
    if (y < 0) y = 0;

    tags[0].ti_Tag = WA_Left;        tags[0].ti_Data = (ULONG)x;
    tags[1].ti_Tag = WA_Top;         tags[1].ti_Data = (ULONG)y;
    tags[2].ti_Tag = WA_Width;       tags[2].ti_Data = w;
    tags[3].ti_Tag = WA_Height;      tags[3].ti_Data = h;
    tags[4].ti_Tag = WA_Borderless;  tags[4].ti_Data = TRUE;
    tags[5].ti_Tag = WA_Activate;    tags[5].ti_Data = FALSE;
    tags[6].ti_Tag = WA_PubScreen;   tags[6].ti_Data = (ULONG)nm.screen;
    tags[7].ti_Tag = WA_NoCareRefresh; tags[7].ti_Data = TRUE;
    tags[8].ti_Tag = WA_RMBTrap;     tags[8].ti_Data = TRUE;
    tags[9].ti_Tag = TAG_DONE;       tags[9].ti_Data = 0;
    nm.tip = OpenWindowTagList(NULL, tags);
    if (nm.tip == NULL)
        return;
    SetFont(nm.tip->RPort, nm.dri->dri_Font);

    back = (UWORD)(nm.pen_tipback >= 0 ? nm.pen_tipback : nm.pen_shine);
    SetAPen(nm.tip->RPort, back);
    RectFill(nm.tip->RPort, 0, 0, (WORD)(w - 1), (WORD)(h - 1));
    SetAPen(nm.tip->RPort, nm.pen_shadow);
    Move(nm.tip->RPort, 0, 0);
    Draw(nm.tip->RPort, (WORD)(w - 1), 0);
    Draw(nm.tip->RPort, (WORD)(w - 1), (WORD)(h - 1));
    Draw(nm.tip->RPort, 0, (WORD)(h - 1));
    Draw(nm.tip->RPort, 0, 0);

    SetAPen(nm.tip->RPort, nm.pen_text);
    SetDrMd(nm.tip->RPort, JAM1);
    for (i = 0; i < n; i++)
    {
        Move(nm.tip->RPort, (WORD)(NM_PAD + 1),
             (WORD)(3 + i * (nm.fh + 1) + nm.base));
        Text(nm.tip->RPort, (CONST_STRPTR)lines[i], (UWORD)nm_len(lines[i]));
    }
}

static VOID nm_tip_for(const NmIf *f, UWORD zone)
{
    static char lines[NM_TIP_LINES][NM_TIP_LEN];
    char        path[AMI_CFG_PATH_LEN];
    char        rel[AMI_CFG_PATH_LEN];
    char        total[24];
    UWORD       n = 0;

    lines[0][0] = lines[1][0] = lines[2][0] = '\0';
    switch (zone)
    {
    case NM_ZONE_NAME:
        tool_copy_string(rel, sizeof(rel), "DEVS:NetInterfaces/");
        nm_cat(rel, sizeof(rel), f->name);
        tool_copy_string(lines[n++], NM_TIP_LEN,
                         ami_cfg_resolve(rel, path, sizeof(path)));
        if (f->device[0] != '\0')
        {
            tool_copy_string(lines[n], NM_TIP_LEN, f->device);
            nm_cat(lines[n], NM_TIP_LEN, " unit ");
            nm_cat_ulong(lines[n++], NM_TIP_LEN, f->unit);
        }
        if (f->bps != 0)
        {
            nm_cat_ulong(lines[n], NM_TIP_LEN, f->bps / 1000000UL);
            nm_cat(lines[n++], NM_TIP_LEN,
                   f->up ? " Mbit/s, link up" : " Mbit/s, link down");
        }
        break;
    case NM_ZONE_IP4:
    case NM_ZONE_IP6:
        tool_copy_string(lines[n++], NM_TIP_LEN,
                         zone == NM_ZONE_IP4 ? f->ip4 : f->ip6);
        tool_copy_string(lines[n++], NM_TIP_LEN, "Click to copy");
        break;
    case NM_ZONE_BARS:
        if (!f->have_bytes)
        {
            tool_copy_string(lines[n++], NM_TIP_LEN,
                             "This stack does not count bytes");
            break;
        }
        tool_copy_string(lines[n], NM_TIP_LEN, "Received ");
        nm_format_total(f->rx_hi, f->rx_lo, total, sizeof(total));
        nm_cat(lines[n++], NM_TIP_LEN, total);
        tool_copy_string(lines[n], NM_TIP_LEN, "Sent ");
        nm_format_total(f->tx_hi, f->tx_lo, total, sizeof(total));
        nm_cat(lines[n++], NM_TIP_LEN, total);
        break;
    default:
        return;
    }
    nm_tip_open(lines, n);
}

/* -------------------------------------------------------- clipboard, */

/*
 * The clipboard holds IFF: a FORM FTXT with one CHRS chunk is what a string
 * gadget, a Shell or an editor pastes.  Twenty bytes of header and the text,
 * written to clipboard.device unit 0 directly.
 */
static BOOL nm_copy(const char *s)
{
    struct MsgPort  *port;
    struct IOClipReq *req;
    ULONG            n = nm_len(s);
    ULONG            odd = n & 1UL;
    ULONG            form = 4UL + 8UL + n + odd;
    UBYTE            hdr[20];
    UBYTE            pad = 0;
    BOOL             ok = FALSE;

    hdr[0] = 'F'; hdr[1] = 'O'; hdr[2] = 'R'; hdr[3] = 'M';
    hdr[4] = (UBYTE)(form >> 24); hdr[5] = (UBYTE)(form >> 16);
    hdr[6] = (UBYTE)(form >> 8);  hdr[7] = (UBYTE)form;
    hdr[8] = 'F'; hdr[9] = 'T'; hdr[10] = 'X'; hdr[11] = 'T';
    hdr[12] = 'C'; hdr[13] = 'H'; hdr[14] = 'R'; hdr[15] = 'S';
    hdr[16] = (UBYTE)(n >> 24); hdr[17] = (UBYTE)(n >> 16);
    hdr[18] = (UBYTE)(n >> 8);  hdr[19] = (UBYTE)n;

    port = CreateMsgPort();
    if (port == NULL)
        return FALSE;
    req = (struct IOClipReq *)CreateIORequest(port, sizeof(struct IOClipReq));
    if (req != NULL &&
        OpenDevice((CONST_STRPTR)"clipboard.device", PRIMARY_CLIP,
                   (struct IORequest *)req, 0) == 0)
    {
        req->io_Offset  = 0;
        req->io_ClipID  = 0;
        req->io_Error   = 0;
        req->io_Command = CMD_WRITE;
        req->io_Data    = (STRPTR)hdr;
        req->io_Length  = sizeof(hdr);
        ok = (DoIO((struct IORequest *)req) == 0);
        if (ok)
        {
            req->io_Command = CMD_WRITE;
            req->io_Data    = (STRPTR)s;
            req->io_Length  = n;
            ok = (DoIO((struct IORequest *)req) == 0);
        }
        if (ok && odd)
        {
            req->io_Command = CMD_WRITE;
            req->io_Data    = (STRPTR)&pad;
            req->io_Length  = 1;
            ok = (DoIO((struct IORequest *)req) == 0);
        }
        /* Always: an update ends the write the device is holding open. */
        req->io_Command = CMD_UPDATE;
        if (DoIO((struct IORequest *)req) != 0)
            ok = FALSE;
        CloseDevice((struct IORequest *)req);
    }
    if (req != NULL)
        DeleteIORequest((struct IORequest *)req);
    DeleteMsgPort(port);
    return ok;
}

/* ------------------------------------------------------------ hover, */

static BOOL nm_in(const NmRect *r, WORD x, WORD y)
{
    return (BOOL)(r->w > 0 && x >= r->x && x < r->x + r->w &&
                  y >= r->y && y < r->y + r->h);
}

static UWORD nm_hit(WORD x, WORD y, LONG *which)
{
    UWORD i;

    *which = -1;
    if (!nm.running)
        return NM_ZONE_NONE;
    for (i = 0; i < nm.count; i++)
    {
        const NmIf *f = &nm.ifs[i];

        *which = (LONG)i;
        if (nm_in(&f->r_name, x, y)) return NM_ZONE_NAME;
        if (nm_in(&f->r_ip4, x, y))  return NM_ZONE_IP4;
        if (nm_in(&f->r_ip6, x, y))  return NM_ZONE_IP6;
        if (nm_in(&f->r_bars, x, y)) return NM_ZONE_BARS;
    }
    *which = -1;
    return NM_ZONE_NONE;
}

/* Ten times a second: is the pointer over a zone of this window, and not
   over a window in front of it? */
static VOID nm_hover(VOID)
{
    struct Screen *s = nm.screen;
    WORD           mx = (WORD)(s->MouseX - nm.win->LeftEdge);
    WORD           my = (WORD)(s->MouseY - nm.win->TopEdge);
    struct Layer  *top;
    LONG           which = -1;
    UWORD          zone = NM_ZONE_NONE;

    if (nm.copied_ticks != 0)
    {
        if (--nm.copied_ticks == 0)
            nm_tip_close();
        return;
    }

    top = WhichLayer(&s->LayerInfo, s->MouseX, s->MouseY);
    if (top == nm.win->WLayer)
        zone = nm_hit(mx, my, &which);

    if (zone != nm.zone || which != nm.zone_if)
    {
        nm_tip_close();
        nm.zone        = zone;
        nm.zone_if     = which;
        nm.hover_ticks = 0;
        return;
    }
    if (zone != NM_ZONE_NONE && nm.tip == NULL &&
        ++nm.hover_ticks >= NM_HOVER_TICKS)
        nm_tip_for(&nm.ifs[which], zone);
}

static VOID nm_click(WORD x, WORD y)
{
    static char lines[1][NM_TIP_LEN];
    LONG        which;
    UWORD       zone = nm_hit(x, y, &which);
    const char *addr;

    if (zone != NM_ZONE_IP4 && zone != NM_ZONE_IP6)
        return;
    addr = (zone == NM_ZONE_IP4) ? nm.ifs[which].ip4 : nm.ifs[which].ip6;
    tool_copy_string(lines[0], NM_TIP_LEN,
                     nm_copy(addr) ? "Copied " : "Could not copy ");
    nm_cat(lines[0], NM_TIP_LEN, addr);
    nm_tip_open(lines, 1);
    nm.copied_ticks = NM_COPIED_TICKS;
}

/* ------------------------------------------------------------ timer, */

static VOID nm_timer_arm(VOID)
{
    nm.treq->tr_node.io_Command = TR_ADDREQUEST;
    nm.treq->tr_time.tv_secs    = 0;
    nm.treq->tr_time.tv_micro   = NM_TICK_US;
    SendIO((struct IORequest *)nm.treq);
    nm.timer_armed = TRUE;
}

static BOOL nm_timer_open(VOID)
{
    nm.tport = CreateMsgPort();
    if (nm.tport == NULL)
        return FALSE;
    nm.treq = (struct timerequest *)CreateIORequest(nm.tport,
                                                   sizeof(struct timerequest));
    if (nm.treq == NULL)
        return FALSE;
    if (OpenDevice((CONST_STRPTR)TIMERNAME, UNIT_VBLANK,
                   (struct IORequest *)nm.treq, 0) != 0)
        return FALSE;
    nm.timer_open = TRUE;
    return TRUE;
}

static VOID nm_timer_close(VOID)
{
    if (nm.timer_armed)
    {
        if (!CheckIO((struct IORequest *)nm.treq))
            AbortIO((struct IORequest *)nm.treq);
        WaitIO((struct IORequest *)nm.treq);
    }
    if (nm.timer_open)
        CloseDevice((struct IORequest *)nm.treq);
    if (nm.treq != NULL)
        DeleteIORequest((struct IORequest *)nm.treq);
    if (nm.tport != NULL)
        DeleteMsgPort(nm.tport);
}

/* ---------------------------------------------------- icon geometry, */

/*
 * The program's icon: the directory and name Workbench started it from, or
 * from a Shell PROGDIR: and the command's own name.  The lock is borrowed.
 */
static BPTR nm_icon_dir(char *name, ULONG len)
{
    char prog[NM_NAME_LEN];

    if (nm.wbs != NULL && nm.wbs->sm_NumArgs > 0)
    {
        tool_copy_string(name, len, (const char *)nm.wbs->sm_ArgList[0].wa_Name);
        return nm.wbs->sm_ArgList[0].wa_Lock;
    }
    if (!GetProgramName((STRPTR)prog, (LONG)sizeof(prog)))
        return (BPTR)0;
    tool_copy_string(name, len, (const char *)FilePart((STRPTR)prog));
    return GetProgramDir();
}

static BOOL nm_is_geometry(const char *tt)
{
    UWORD k;

    for (k = 0; k < NM_GEOMETRY; k++)
    {
        const char *key = nm_geometry_keys[k];
        ULONG       i = 0;

        while (key[i] != '\0' &&
               (tt[i] == key[i] || tt[i] == key[i] + ('a' - 'A')))
            i++;
        if (key[i] == '\0' && (tt[i] == '=' || tt[i] == '\0'))
            return TRUE;
    }
    return FALSE;
}

/* LEFT, TOP, WIDTH and HEIGHT from the icon, where it has them. */
static VOID nm_read_icon(VOID)
{
    char               name[NM_NAME_LEN];
    BPTR               dir, old;
    struct DiskObject *dob;
    UWORD              k;

    if (IconBase == NULL)
        return;
    dir = nm_icon_dir(name, sizeof(name));
    if (dir == (BPTR)0)
        return;
    old = CurrentDir(dir);
    dob = GetDiskObject((STRPTR)name);
    (VOID)CurrentDir(old);
    if (dob == NULL)
        return;

    for (k = 0; k < NM_GEOMETRY && dob->do_ToolTypes != NULL; k++)
    {
        STRPTR v = (STRPTR)FindToolType((CONST_STRPTR *)dob->do_ToolTypes,
                                        (CONST_STRPTR)nm_geometry_keys[k]);
        LONG   n;

        if (v != NULL && StrToLong(v, &n) > 0 && n >= 0)
            nm.want[k] = n;
    }
    FreeDiskObject(dob);
}

/*
 * Snapshot: the window's place into the icon's ToolTypes, every other
 * ToolType and the image kept.  An icon that is not there is made from the
 * default tool icon.  The DiskObject's own array is put back before it is
 * freed: FreeDiskObject() frees what GetDiskObject() allocated, not ours.
 */
static BOOL nm_snapshot(VOID)
{
    static char        val[NM_GEOMETRY][24];
    static STRPTR      tt[NM_TT_MAX + NM_GEOMETRY + 1];
    char               name[NM_NAME_LEN];
    LONG               v[NM_GEOMETRY];
    BPTR               dir, old;
    struct DiskObject *dob;
    STRPTR            *was;
    UWORD              i, k, n = 0;
    BOOL               ok = FALSE;

    if (IconBase == NULL)
        return FALSE;
    dir = nm_icon_dir(name, sizeof(name));
    if (dir == (BPTR)0)
        return FALSE;

    v[0] = nm.win->LeftEdge; v[1] = nm.win->TopEdge;
    v[2] = nm.win->Width;    v[3] = nm.win->Height;

    old = CurrentDir(dir);
    dob = GetDiskObject((STRPTR)name);
    if (dob == NULL)
        dob = GetDefDiskObject(WBTOOL);
    if (dob != NULL)
    {
        was = dob->do_ToolTypes;
        for (i = 0; was != NULL && was[i] != NULL && n < NM_TT_MAX; i++)
            if (!nm_is_geometry((const char *)was[i]))
                tt[n++] = was[i];
        for (k = 0; k < NM_GEOMETRY; k++)
        {
            tool_copy_string(val[k], sizeof(val[k]), nm_geometry_keys[k]);
            nm_cat(val[k], sizeof(val[k]), "=");
            nm_cat_ulong(val[k], sizeof(val[k]), (ULONG)(v[k] < 0 ? 0 : v[k]));
            tt[n++] = (STRPTR)val[k];
        }
        tt[n] = NULL;

        dob->do_ToolTypes = tt;
        ok = PutDiskObject((STRPTR)name, dob) ? TRUE : FALSE;
        dob->do_ToolTypes = was;
        FreeDiskObject(dob);
    }
    (VOID)CurrentDir(old);
    return ok;
}

/* From a Shell the four are arguments too, and they win over the icon. */
static BOOL nm_read_args(VOID)
{
    LONG           args[NM_GEOMETRY] = { 0, 0, 0, 0 };
    struct RDArgs *rda;
    UWORD          k;

    if (nm.wbs != NULL)
        return TRUE;
    rda = ReadArgs((CONST_STRPTR)"LEFT/N,TOP/N,WIDTH/N,HEIGHT/N", args, NULL);
    if (rda == NULL)
    {
        PrintFault(IoErr(), (CONST_STRPTR)tool_name);
        return FALSE;
    }
    for (k = 0; k < NM_GEOMETRY; k++)
        if (args[k] != 0 && *(LONG *)args[k] >= 0)
            nm.want[k] = *(LONG *)args[k];
    FreeArgs(rda);
    return TRUE;
}

static VOID nm_snapshot_feedback(VOID)
{
    static char lines[1][NM_TIP_LEN];

    tool_copy_string(lines[0], NM_TIP_LEN,
                     nm_snapshot() ? "Position and size saved in the icon"
                                   : "Could not save to the icon");
    nm_tip_open(lines, 1);
    nm.copied_ticks = NM_COPIED_TICKS;
}

/* ----------------------------------------------------------- window, */

static BOOL nm_open_window(VOID)
{
    struct TagItem tags[20];
    UWORD          w, h;
    int            t = 0;

    WORD           left, top, sw = nm.screen->Width, sh = nm.screen->Height;
    BOOL           outer = FALSE;

    nm_measure(&nm_mrp);
    w = nm_content_w(&nm_mrp);
    h = nm_content_h();
    left = (WORD)(sw - w - 40);
    top  = (WORD)(nm.screen->BarHeight + 20);

    /* A snapshot: outer size, kept on a screen that may have shrunk since. */
    if (nm.want[2] > 0 && nm.want[3] > 0)
    {
        w = (UWORD)(nm.want[2] < sw ? nm.want[2] : sw);
        h = (UWORD)(nm.want[3] < sh ? nm.want[3] : sh);
        outer = TRUE;
    }
    if (nm.want[0] >= 0) left = (WORD)nm.want[0];
    if (nm.want[1] >= 0) top  = (WORD)nm.want[1];
    if (outer)
    {
        if (left + (WORD)w > sw) left = (WORD)(sw - w);
        if (top + (WORD)h > sh)  top  = (WORD)(sh - h);
    }
    if (left < 0) left = 0;
    if (top < 0)  top = 0;

#define NM_TAG(k, v) do { tags[t].ti_Tag = (k); tags[t].ti_Data = (ULONG)(v); t++; } while (0)
    NM_TAG(WA_Title,        "Network");
    NM_TAG(WA_PubScreen,    nm.screen);
    NM_TAG(outer ? WA_Width : WA_InnerWidth,   w);
    NM_TAG(outer ? WA_Height : WA_InnerHeight, h);
    NM_TAG(WA_Left,         left);
    NM_TAG(WA_Top,          top);
    NM_TAG(WA_NewLookMenus, TRUE);
    NM_TAG(WA_DragBar,      TRUE);
    NM_TAG(WA_DepthGadget,  TRUE);
    NM_TAG(WA_CloseGadget,  TRUE);
    NM_TAG(WA_SizeGadget,   TRUE);
    NM_TAG(WA_SizeBBottom,  TRUE);
    NM_TAG(WA_SmartRefresh, TRUE);
    NM_TAG(WA_AutoAdjust,   TRUE);
    NM_TAG(WA_MinWidth,     120);
    NM_TAG(WA_MinHeight,    nm.screen->BarHeight + nm.fh + 2 * NM_PAD + 10);
    NM_TAG(WA_MaxWidth,     ~0UL);
    NM_TAG(WA_MaxHeight,    ~0UL);
    NM_TAG(WA_IDCMP,        IDCMP_CLOSEWINDOW | IDCMP_NEWSIZE |
                            IDCMP_REFRESHWINDOW | IDCMP_MOUSEBUTTONS |
                            IDCMP_MENUPICK);
    NM_TAG(TAG_DONE,        0);
#undef NM_TAG

    nm.win = OpenWindowTagList(NULL, tags);
    if (nm.win == NULL)
        return FALSE;
    SetFont(nm.win->RPort, nm.dri->dri_Font);

    /* The menu is a convenience: a window without one still works. */
    nm.vi = GetVisualInfoA(nm.screen, NULL);
    if (nm.vi != NULL)
        nm.menu = CreateMenusA(nm_menu, NULL);
    if (nm.menu != NULL)
    {
        struct TagItem lt[2];

        lt[0].ti_Tag = GTMN_NewLookMenus; lt[0].ti_Data = TRUE;
        lt[1].ti_Tag = TAG_DONE;          lt[1].ti_Data = 0;
        if (!LayoutMenusA(nm.menu, nm.vi, lt) || !SetMenuStrip(nm.win, nm.menu))
        {
            FreeMenus(nm.menu);
            nm.menu = NULL;
        }
    }
    return TRUE;
}

/* The window grows to fit what arrived -- another interface, a longer name or
   address -- moving left if the screen's edge is in the way; it never shrinks
   a size the user chose. */
static VOID nm_fit(VOID)
{
    struct Window *w = nm.win;
    WORD in_w  = (WORD)(w->Width - w->BorderLeft - w->BorderRight);
    WORD in_h  = (WORD)(w->Height - w->BorderTop - w->BorderBottom);
    WORD gw    = (WORD)(nm_content_w(w->RPort) - in_w);
    WORD gh    = (WORD)(nm_content_h() - in_h);
    WORD dx    = 0;
    WORD room;

    if (gw < 0) gw = 0;
    if (gh < 0) gh = 0;
    if (gw == 0 && gh == 0)
        return;

    room = (WORD)(nm.screen->Width - w->LeftEdge - w->Width);
    if (gw > room)
    {
        dx = (WORD)(gw - room);
        if (dx > w->LeftEdge) dx = w->LeftEdge;
        gw = (WORD)(room + dx);
    }
    room = (WORD)(nm.screen->Height - w->TopEdge - w->Height);
    if (gh > room)
        gh = room;

    if (dx > 0)
        MoveWindow(w, (WORD)-dx, 0);
    if (gw > 0 || gh > 0)
        SizeWindow(w, gw, gh);      /* IDCMP_NEWSIZE redraws */
}

/*
 * Everything that holds the screen: the window and its menus, the pens, the
 * DrawInfo and the public-screen lock itself, which would keep Workbench from
 * closing as surely as the window would.  The window's place is kept, so the
 * next attach opens it there.
 */
static VOID nm_screen_detach(VOID)
{
    nm_tip_close();
    if (nm.win != NULL)
    {
        nm.want[0] = nm.win->LeftEdge;
        nm.want[1] = nm.win->TopEdge;
        nm.want[2] = nm.win->Width;
        nm.want[3] = nm.win->Height;
        if (nm.menu != NULL)
            ClearMenuStrip(nm.win);
        CloseWindow(nm.win);
        nm.win = NULL;
    }
    if (nm.menu != NULL)
        FreeMenus(nm.menu);
    nm.menu = NULL;
    if (nm.vi != NULL)
        FreeVisualInfo(nm.vi);
    nm.vi = NULL;
    if (nm.screen != NULL)
    {
        nm_release_pens();
        if (nm.dri != NULL)
            FreeScreenDrawInfo(nm.screen, nm.dri);
        UnlockPubScreen(NULL, nm.screen);
    }
    nm.dri    = NULL;
    nm.screen = NULL;
}

static BOOL nm_screen_attach(VOID)
{
    nm.screen = LockPubScreen(NULL);
    if (nm.screen == NULL)
        return FALSE;
    nm.dri = GetScreenDrawInfo(nm.screen);
    if (nm.dri == NULL)
        return FALSE;
    nm_pens();
    InitRastPort(&nm_mrp);
    SetFont(&nm_mrp, nm.dri->dri_Font);
    if (!nm_open_window())
        return FALSE;
    nm.zone         = NM_ZONE_NONE;
    nm.zone_if      = -1;
    nm.hover_ticks  = 0;
    nm.copied_ticks = 0;
    nm_draw_all();
    return TRUE;
}

/* Told when Workbench closes and reopens, if the window is on Workbench. */
static VOID nm_sn_start(VOID)
{
    struct Screen *wb;

    ScreenNotifyBase = OpenLibrary((CONST_STRPTR)"screennotify.library", 1);
    if (ScreenNotifyBase == NULL)
        return;
    wb = LockPubScreen((CONST_STRPTR)"Workbench");
    if (wb != NULL)
        UnlockPubScreen(NULL, wb);
    if (wb == NULL || wb != nm.screen)
        return;
    nm.sn_port = CreateMsgPort();
    if (nm.sn_port != NULL)
        nm.sn_handle = AddWorkbenchClient(nm.sn_port, 0);
}

static VOID nm_sn_stop(VOID)
{
    struct Message *m;

    if (nm.sn_handle != NULL)
    {
        /* Busy while a notification is out: answer it and try again. */
        for (;;)
        {
            while ((m = GetMsg(nm.sn_port)) != NULL)
                ReplyMsg(m);
            if (RemWorkbenchClient(nm.sn_handle))
                break;
            Delay(10);
        }
        nm.sn_handle = NULL;
    }
    if (nm.sn_port != NULL)
    {
        while ((m = GetMsg(nm.sn_port)) != NULL)
            ReplyMsg(m);
        DeleteMsgPort(nm.sn_port);
        nm.sn_port = NULL;
    }
    if (ScreenNotifyBase != NULL)
        CloseLibrary(ScreenNotifyBase);
    ScreenNotifyBase = NULL;
}

/* A Workbench notification.  FALSE from attach is the end: Workbench came
   back and the window could not open on it. */
static BOOL nm_sn_take(VOID)
{
    NmScreenNotifyMessage *m;
    BOOL                   reopen = FALSE;

    while ((m = (NmScreenNotifyMessage *)GetMsg(nm.sn_port)) != NULL)
    {
        if (m->snm_Type == NM_SN_TYPE_WORKBENCH)
        {
            if (m->snm_Value == NULL)
            {
                /* Closed before the reply: the reply is what lets
                   Workbench go on and close its screen. */
                nm_screen_detach();
                reopen = FALSE;
            }
            else if (nm.win == NULL)
                reopen = TRUE;
        }
        ReplyMsg((struct Message *)m);
    }
    if (!reopen)
        return TRUE;
    /* TinyMeter's second: Workbench's own windows go up first. */
    Delay(50);
    if (nm_screen_attach())
        return TRUE;
    nm_screen_detach();
    return FALSE;
}

static VOID nm_loop(VOID)
{
    ULONG tsig = 1UL << nm.tport->mp_SigBit;
    ULONG ssig = (nm.sn_port != NULL) ? 1UL << nm.sn_port->mp_SigBit : 0UL;
    BOOL  done = FALSE;

    nm_timer_arm();
    while (!done)
    {
        /* No window while Workbench is closed: the counts are still taken,
           so the rates are right the moment it opens again. */
        ULONG wsig = (nm.win != NULL) ? 1UL << nm.win->UserPort->mp_SigBit
                                      : 0UL;
        ULONG got = Wait(wsig | tsig | ssig | SIGBREAKF_CTRL_C);
        struct IntuiMessage *msg;

        if (got & SIGBREAKF_CTRL_C)
            done = TRUE;

        if ((got & ssig) && !nm_sn_take())
            done = TRUE;

        if ((got & tsig) && GetMsg(nm.tport) != NULL)
        {
            nm.timer_armed = FALSE;
            if (++nm.ticks >= NM_TICKS_SEC)
            {
                BOOL reshaped;

                nm.ticks = 0;
                reshaped = nm_sample();
                if (nm.win != NULL && reshaped)
                {
                    nm_tip_close();
                    nm_draw_all();
                    nm_fit();
                }
                else if (nm.win != NULL)
                    nm_draw_rates_all();
            }
            if (nm.win != NULL)
                nm_hover();
            if (!done)
                nm_timer_arm();
        }

        while (nm.win != NULL &&
               (msg = (struct IntuiMessage *)GetMsg(nm.win->UserPort)) != NULL)
        {
            ULONG cls  = msg->Class;
            UWORD code = msg->Code;
            WORD  x = msg->MouseX, y = msg->MouseY;

            ReplyMsg((struct Message *)msg);
            switch (cls)
            {
            case IDCMP_CLOSEWINDOW:
                done = TRUE;
                break;
            case IDCMP_NEWSIZE:
                nm_tip_close();
                nm_draw_all();
                break;
            case IDCMP_REFRESHWINDOW:
                BeginRefresh(nm.win);
                EndRefresh(nm.win, TRUE);
                nm_draw_all();
                break;
            case IDCMP_MOUSEBUTTONS:
                if (code == SELECTUP)
                    nm_click(x, y);
                break;
            case IDCMP_MENUPICK:
                while (code != MENUNULL && nm.menu != NULL)
                {
                    struct MenuItem *it = ItemAddress(nm.menu, code);

                    if (it == NULL)
                        break;
                    switch ((ULONG)GTMENUITEM_USERDATA(it))
                    {
                    case NM_MENU_SNAPSHOT: nm_snapshot_feedback(); break;
                    case NM_MENU_QUIT:     done = TRUE;            break;
                    default:                                       break;
                    }
                    code = it->NextSelect;
                }
                break;
            default:
                break;
            }
        }
    }
}

static int netmeter_run(VOID)
{
    int rc = RETURN_FAIL;

    IntuitionBase = (struct IntuitionBase *)OpenLibrary("intuition.library", 37);
    GfxBase       = (struct GfxBase *)OpenLibrary("graphics.library", 37);
    LayersBase    = OpenLibrary("layers.library", 37);
    GadToolsBase  = OpenLibrary("gadtools.library", 37);
    IconBase      = OpenLibrary("icon.library", 37);
    if (IntuitionBase == NULL || GfxBase == NULL || LayersBase == NULL ||
        GadToolsBase == NULL)
        goto out;

    nm_read_icon();
    if (!nm_read_args())
    {
        rc = RETURN_ERROR;
        goto out;
    }

    if (!nm_timer_open())
        goto out;
    DateStamp(&nm.last);
    (VOID)nm_sample();

    if (!nm_screen_attach())
        goto out;
    nm_sn_start();
    nm_loop();
    rc = RETURN_OK;

out:
    nm_sn_stop();
    nm_screen_detach();
    nm_timer_close();
    if (IconBase != NULL) CloseLibrary(IconBase);
    if (GadToolsBase != NULL) CloseLibrary(GadToolsBase);
    if (LayersBase != NULL) CloseLibrary(LayersBase);
    if (GfxBase != NULL) CloseLibrary((struct Library *)GfxBase);
    if (IntuitionBase != NULL) CloseLibrary((struct Library *)IntuitionBase);
    return rc;
}

/*
 * The same trampoline as NetPrefs: a Shell gives 4 KB, and Intuition and
 * graphics calls run on the caller's stack on top of this command's own.
 */
#define NETMETER_STACK_SIZE (16UL * 1024UL)
#define NETMETER_SAFE_STACK (8UL * 1024UL)

static struct StackSwapStruct nm_sss;
static int                    nm_result;

static __attribute__((noinline)) VOID netmeter_trampoline(VOID)
{
    StackSwap(&nm_sss);
    nm_result = netmeter_run();
    StackSwap(&nm_sss);
}

int main(int argc, char **argv)
{
    struct Task *me = FindTask(NULL);
    ULONG        have = (ULONG)me->tc_SPUpper - (ULONG)me->tc_SPLower;
    APTR         stack;

    /* Workbench hands the WBStartup over as argv (tool_startup.S). */
    nm.wbs = (argc == 0) ? (struct WBStartup *)argv : NULL;

    if (have >= NETMETER_SAFE_STACK)
        return netmeter_run();

    stack = AllocMem(NETMETER_STACK_SIZE, MEMF_ANY);
    if (stack == NULL)
    {
        tool_error("not enough memory for a 16 KB stack");
        return RETURN_FAIL;
    }
    nm_sss.stk_Lower   = stack;
    nm_sss.stk_Upper   = (ULONG)stack + NETMETER_STACK_SIZE;
    nm_sss.stk_Pointer = (APTR)((ULONG)stack + NETMETER_STACK_SIZE);
    netmeter_trampoline();
    FreeMem(stack, NETMETER_STACK_SIZE);
    return nm_result;
}

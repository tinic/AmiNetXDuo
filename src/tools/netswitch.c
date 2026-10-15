/*
 * NetSwitch, the Workbench switch for AmiNetXDuo interfaces.
 *
 * It lists every configured interface and shows, per interface, which of the
 * three states it is in: a definition the running stack has never attached, a
 * live interface that is down, or one that is up.  A single button takes the
 * selected interface online or offline, and one guarded action takes every
 * online interface down.  It talks to the running stack directly through the
 * netstatus device protocol, so it cannot acquire a second, subtly different
 * interface lifecycle; the CLI Online/Offline commands and this window answer
 * from the same running state.
 *
 * The window is drawn by hand, not from a form: the three states are
 * shape-coded (filled disc / hollow disc / hollow square), which no GadTools
 * list can draw, and the stock 4-colour Workbench palette carries no colour
 * for the distinction.  All geometry comes from netswitch_layout.c and every
 * phrase from netswitch_text.c, both of which run on the build host.
 *
 * SPDX-License-Identifier: MIT
 */

#include "tools.h"
#include "netswitch_text.h"
#include "netswitch_layout.h"
#include "aminetxduo/version.h"

#include <exec/libraries.h>
#include <exec/memory.h>
#include <exec/tasks.h>
#include <graphics/gfxbase.h>
#include <graphics/gfxmacros.h>
#include <graphics/rastport.h>
#include <graphics/text.h>
#include <intuition/intuition.h>
#include <intuition/intuitionbase.h>
#include <intuition/screens.h>
#include <libraries/gadtools.h>
#include <proto/exec.h>
#include <proto/gadtools.h>
#include <proto/graphics.h>
#include <proto/intuition.h>
#include <utility/tagitem.h>

const char *const tool_name = "NetSwitch";

static const char version_tag[] __attribute__((used)) =
    TOOL_VERSTAG("NetSwitch");

struct IntuitionBase *IntuitionBase;
struct GfxBase       *GfxBase;
struct Library       *GadToolsBase;

/* The Amiga raw key codes (RKM Input Device Support).  The NDK defines only
   IDCMP_RAWKEY and the cursor keys, so the ones the window needs are local.
   Bit 7 of IDCMP_RAWKEY's Code is the key-up prefix. */
#define RAWKEY_SPACE    0x40
#define RAWKEY_TAB      0x42
#define RAWKEY_ENTER    0x43    /* keypad enter */
#define RAWKEY_RETURN   0x44
#define RAWKEY_ESC      0x45

/* As many interface files as the drawer is scanned for. */
#define NS_MAX_INTERFACES   16

/* A row in the list: one configured interface joined with its live state. */
typedef struct NsRow
{
    char        name[TOOL_NAME_LEN];     /* the interface file name */
    char        device[TOOL_NAME_LEN];   /* basename, "anxzz9000.device" */
    char        devunit[TOOL_NAME_LEN + 8];  /* "anxzz9000.device 0" */
    ULONG       unit;
    NsState     state;
    UWORD       index;                   /* live NX index, 0xFFFF detached */
    char        ipv4[16];                /* dotted quad, "" when none */
    char        ipv6[AMI_CFG_IP6_STRLEN];    /* first usable v6, "" none */
    char        addrs[3][AMI_CFG_IP6_STRLEN]; /* the panel's three lines */
    unsigned    naddrs;
} NsRow;

enum
{
    NS_FOCUS_LIST,
    NS_FOCUS_ACTION,
    NS_FOCUS_ALL
};

static struct
{
    struct Screen     *screen;
    struct Window     *window;
    struct TextFont   *font;
    struct VisualInfo *visual;
    struct DrawInfo   *dri;
    NsLayout           layout;
    NsRow              row[NS_MAX_INTERFACES];
    unsigned           count;
    unsigned           selected;
    unsigned           first;
    int                focus;
    struct Library    *base;             /* resident stack, NULL when down */
    int                stack_down;
    char               last_error[64];
    ULONG              last_click_row;   /* double-click detection */
    ULONG              last_click_us;
} ns;

/* One snapshot of each live table the library answers. */
static struct
{
    NetStatusHeader     hdr;
    NetStatusInterface  e[NX_MAX_PHYSICAL_INTERFACES];
} ns_ifaces;

static struct
{
    NetStatusHeader     hdr;
    NetStatusAddress6   e[NX_MAX_PHYSICAL_INTERFACES * 3];
} ns_addr6;

static size_t text_len(const char *s)
{
    size_t n = 0;

    while (s != NULL && s[n] != '\0') n++;
    return n;
}

static void decimal(ULONG value, char *out, ULONG outlen)
{
    char tmp[12];
    int  n = 0;

    do
    {
        tmp[n++] = (char)('0' + value % 10);
        value /= 10;
    }
    while (value != 0);

    while (outlen > 1 && n > 0)
    {
        *out++ = tmp[--n];
        outlen--;
    }
    *out = '\0';
}

/* ------------------------------------------------- the configured list --- */

static void enumerate_interfaces(void)
{
    char      where[AMI_CFG_PATH_LEN];
    char      names[NS_MAX_INTERFACES][TOOL_NAME_LEN];
    AmiIfConfig ifc;
    ULONG     n, i;
    char      unit[12];

    n = tool_list_dir(ami_cfg_resolve("DEVS:NetInterfaces", where,
                                      sizeof(where)),
                      names, (ULONG)NS_MAX_INTERFACES, NULL);

    ns.count = 0;
    for (i = 0; i < n && ns.count < NS_MAX_INTERFACES; i++)
    {
        NsRow *r = &ns.row[ns.count];

        if (ami_config_load_interface(names[i], &ifc) != AMI_CFG_OK)
            continue;

        tool_copy_string(r->name, sizeof(r->name), names[i]);
        tool_copy_string(r->device, sizeof(r->device),
                         tool_basename(ifc.device));
        r->unit = ifc.unit;
        tool_copy_string(r->devunit, sizeof(r->devunit), r->device);
        decimal(ifc.unit, unit, sizeof(unit));
        tool_copy_string(r->devunit + text_len(r->devunit),
                         sizeof(r->devunit) - text_len(r->devunit), " ");
        tool_copy_string(r->devunit + text_len(r->devunit),
                         sizeof(r->devunit) - text_len(r->devunit), unit);

        r->state = NS_STATE_DETACHED;
        r->index = 0xFFFF;
        r->ipv4[0] = '\0';
        r->ipv6[0] = '\0';
        r->naddrs = 0;
        ns.count++;
    }
}

/* Re-read the live tables and re-join every row.  With no resident stack every
 * definition reads as detached: it is configured, and nothing is running. */
static void refresh_rows(void)
{
    LONG     n, n6;
    unsigned i;

    if (ns.base == NULL)
    {
        for (i = 0; i < ns.count; i++)
        {
            ns.row[i].state = NS_STATE_DETACHED;
            ns.row[i].index = 0xFFFF;
            ns.row[i].ipv4[0] = '\0';
            ns.row[i].ipv6[0] = '\0';
            ns.row[i].naddrs = 0;
        }
        return;
    }

    n  = tool_netstatus_query(ns.base, NETSTATUS_INTERFACES, &ns_ifaces,
                              sizeof(ns_ifaces), sizeof(NetStatusInterface));
    n6 = tool_netstatus_query(ns.base, NETSTATUS_ADDRESSES6, &ns_addr6,
                              sizeof(ns_addr6), sizeof(NetStatusAddress6));

    for (i = 0; i < ns.count; i++)
    {
        NsRow                  *r = &ns.row[i];
        const NetStatusInterface *nsi = NULL;
        LONG                    j;

        r->state = NS_STATE_DETACHED;
        r->index = 0xFFFF;
        r->ipv4[0] = '\0';
        r->ipv6[0] = '\0';
        r->naddrs = 0;

        for (j = 0; n > 0 && j < n &&
                    j < (LONG)NX_MAX_PHYSICAL_INTERFACES; j++)
        {
            const NetStatusInterface *e = &ns_ifaces.e[j];

            if (!(e->nsi_Flags & NETSTATUS_IF_NAMED))
                continue;
            if (tool_stricmp(e->nsi_Name, r->name) != 0)
                continue;
            nsi = e;
            break;
        }
        if (nsi == NULL)
            continue;

        r->index = nsi->nsi_Index;
        /*
         * NETSTATUS_IF_LINKUP, not NETSTATUS_IF_ONLINE: LINKUP is the flag
         * UP and DOWN set (onoff.c:300).  The SANA-II shim's own online flag
         * is a layer below and does not follow in step.
         */
        r->state = (nsi->nsi_Flags & NETSTATUS_IF_LINKUP) ? NS_STATE_UP
                                                           : NS_STATE_DOWN;

        if (nsi->nsi_Address != 0)
        {
            ami_config_format_ip(nsi->nsi_Address, r->ipv4,
                                 sizeof(r->ipv4));
            tool_copy_string(r->addrs[r->naddrs++], sizeof(r->addrs[0]),
                             r->ipv4);
        }

        if (n6 > 0)
        {
            LONG k;

            for (k = 0; k < n6 &&
                        k < (LONG)(NX_MAX_PHYSICAL_INTERFACES * 3); k++)
            {
                const NetStatusAddress6 *a6 = &ns_addr6.e[k];
                char                     txt[AMI_CFG_IP6_STRLEN];

                if (a6->nsn_Interface != r->index)
                    continue;
                if (a6->nsn_State == NETSTATUS_IP6_TENTATIVE)
                    continue;

                tool_format_ip6(a6->nsn_Address, txt, sizeof(txt));
                if (r->ipv6[0] == '\0')
                    tool_copy_string(r->ipv6, sizeof(r->ipv6), txt);
                if (r->naddrs < 3)
                    tool_copy_string(r->addrs[r->naddrs++],
                                     sizeof(r->addrs[0]), txt);
            }
        }
    }
}

/* ------------------------------------------------- mutating the stack ---- */

static void set_last_error(const char *text)
{
    tool_copy_string(ns.last_error, sizeof(ns.last_error),
                     (text != NULL && text[0] != '\0') ? text : "");
}

/* UP or DOWN on an already-attached interface.  Returns errno text, or NULL. */
static const char *control_switch(ULONG op, UWORD index)
{
    NetStatusControl ctl;
    LONG             err = 0;
    LONG             rc;
    ULONG            i;

    for (i = 0; i < sizeof(ctl) / sizeof(ULONG); i++)
        ((ULONG *)&ctl)[i] = 0;
    ctl.nsc_Index = index;

    rc = tool_netstatus_control(ns.base, op, &ctl, &err);
    if (rc == 0)
        return NULL;
    return tool_code_errno(err);
}

static unsigned online_count(void)
{
    unsigned i, n = 0;

    for (i = 0; i < ns.count; i++)
        if (ns.row[i].state == NS_STATE_UP &&
            tool_stricmp(ns.row[i].name, "loopback") != 0)
            n++;
    return n;
}

static BOOL action_enabled(void)
{
    return ns.base != NULL && ns.count != 0;
}

static BOOL take_all_enabled(void)
{
    return ns.base != NULL && online_count() != 0;
}

/* The one action button: takes the selected interface the other way. */
static void do_action(void)
{
    NsRow      *r;
    const char *err = NULL;

    if (!action_enabled())
        return;

    r = &ns.row[ns.selected];

    if (r->state == NS_STATE_UP)
    {
        err = control_switch(NETCTRL_INTERFACE_DOWN, r->index);
    }
    else if (r->state == NS_STATE_DOWN)
    {
        err = control_switch(NETCTRL_INTERFACE_UP, r->index);
    }
    else
    {
        /*
         * Defined but never attached: Online attaches the definition and
         * brings it up in one call, as AddNetInterface does.  EEXIST means
         * another tool attached it in the meantime.
         */
        LONG e = tool_stack_add_interface(ns.base, r->name, NULL, TRUE);

        if (e != 0 && e != EEXIST)
        {
            err = tool_code_errno(e);
        }
        else if (e == EEXIST)
        {
            refresh_rows();
            r = &ns.row[ns.selected];
            if (r->state != NS_STATE_DETACHED)
                err = control_switch(NETCTRL_INTERFACE_UP, r->index);
        }
    }

    set_last_error(err);
    refresh_rows();
}

/* Yes-or-cancel, as netprefs.c's confirm().  The requester states what will
 * happen and warns, and the yes button carries the count in its label. */
static BOOL confirm(const char *body, const char *yes)
{
    struct EasyStruct easy;
    char              buttons[48];

    tool_copy_string(buttons, sizeof(buttons), yes);
    tool_copy_string(buttons + text_len(buttons),
                     sizeof(buttons) - text_len(buttons), "|Cancel");
    easy.es_StructSize   = sizeof(easy);
    easy.es_Flags        = 0;
    easy.es_Title        = (STRPTR)"AmiNetXDuo NetSwitch";
    easy.es_TextFormat   = (STRPTR)body;
    easy.es_GadgetFormat = (STRPTR)buttons;
    return (BOOL)(EasyRequestArgs(ns.window, &easy, NULL, NULL) == 1);
}

/* Take every online interface down, after asking.  Loopback is never a
 * definition file so it cannot be in the list, but skip it defensively. */
static void take_all_offline(void)
{
    char     prompt[96];
    char     body[160];
    unsigned n, i;

    if (!take_all_enabled())
        return;

    n = online_count();
    ns_take_all_prompt(prompt, sizeof(prompt), n);
    tool_copy_string(body, sizeof(body), prompt);
    tool_copy_string(body + text_len(body), sizeof(body) - text_len(body),
                     "\n");
    tool_copy_string(body + text_len(body), sizeof(body) - text_len(body),
                     ns_take_all_warning);
    if (!confirm(body, ns_offline_button))
        return;

    for (i = 0; i < ns.count; i++)
        if (ns.row[i].state == NS_STATE_UP &&
            tool_stricmp(ns.row[i].name, "loopback") != 0)
            (VOID)control_switch(NETCTRL_INTERFACE_DOWN, ns.row[i].index);

    set_last_error(NULL);
    refresh_rows();
}

/* ------------------------------------------------- window and layout ----- */

static struct TextAttr topaz8 =
{
    (STRPTR)"topaz.font", 8, FS_NORMAL, FPF_ROMFONT
};

static int measure_text(void *ctx, const char *text, int len)
{
    return (int)TextLength((struct RastPort *)ctx, (CONST_STRPTR)text,
                           (ULONG)len);
}

static BOOL layout_in(struct TextAttr *ta)
{
    struct Screen   *s = ns.screen;
    struct TextFont *font;
    struct RastPort  rp;
    NsFont           metrics;
    int              max_w, max_h;

    font = OpenFont(ta);
    if (font == NULL) return FALSE;
    InitRastPort(&rp);
    SetFont(&rp, font);
    metrics.height   = font->tf_YSize;
    metrics.baseline = font->tf_Baseline;
    metrics.measure  = measure_text;
    metrics.ctx      = &rp;

    max_w = s->Width - s->WBorLeft - s->WBorRight;
    max_h = s->Height - s->WBorTop - s->Font->ta_YSize - 1 - s->WBorBottom;
    if (!ns_layout(&metrics, max_w, max_h, ns.count, &ns.layout))
    {
        CloseFont(font);
        return FALSE;
    }
    ns.font = font;
    return TRUE;
}

static BOOL make_window(void)
{
    struct TagItem win[12];
    WORD            width, height;

    ns.screen = LockPubScreen(NULL);
    if (ns.screen == NULL) return FALSE;
    if (!layout_in(ns.screen->Font) && !layout_in(&topaz8)) return FALSE;
    ns.visual = GetVisualInfoA(ns.screen, NULL);
    if (ns.visual == NULL) return FALSE;
    ns.dri = GetScreenDrawInfo(ns.screen);

    width  = (WORD)(ns.layout.inner_w + ns.screen->WBorLeft +
                    ns.screen->WBorRight);
    height = (WORD)(ns.layout.inner_h + ns.screen->WBorTop +
                    ns.screen->Font->ta_YSize + 1 + ns.screen->WBorBottom);
    win[0].ti_Tag = WA_Left; win[0].ti_Data = (ns.screen->Width - width) / 2;
    win[1].ti_Tag = WA_Top;  win[1].ti_Data = (ns.screen->Height - height) / 2;
    win[2].ti_Tag = WA_InnerWidth;  win[2].ti_Data = ns.layout.inner_w;
    win[3].ti_Tag = WA_InnerHeight; win[3].ti_Data = ns.layout.inner_h;
    win[4].ti_Tag = WA_Title; win[4].ti_Data = (ULONG)"AmiNetXDuo NetSwitch";
    win[5].ti_Tag = WA_IDCMP; win[5].ti_Data = IDCMP_CLOSEWINDOW |
        IDCMP_REFRESHWINDOW | IDCMP_INTUITICKS | IDCMP_MOUSEBUTTONS |
        IDCMP_RAWKEY;
    win[6].ti_Tag = WA_Flags; win[6].ti_Data = WFLG_DRAGBAR |
        WFLG_DEPTHGADGET | WFLG_CLOSEGADGET | WFLG_ACTIVATE |
        WFLG_SMART_REFRESH | WFLG_GIMMEZEROZERO;
    win[7].ti_Tag = WA_PubScreen; win[7].ti_Data = (ULONG)ns.screen;
    win[8].ti_Tag = WA_AutoAdjust; win[8].ti_Data = TRUE;
    win[9].ti_Tag = TAG_DONE; win[9].ti_Data = 0;
    ns.window = OpenWindowTagList(NULL, win);
    if (ns.window == NULL) return FALSE;

    ns.selected = 0;
    ns.first = 0;
    ns.focus = NS_FOCUS_LIST;
    ns.last_click_row = ~0UL;
    ns.last_click_us = 0;
    GT_RefreshWindow(ns.window, NULL);
    return TRUE;
}

static void close_ui(void)
{
    if (ns.window != NULL) CloseWindow(ns.window);
    ns.window = NULL;
    if (ns.dri != NULL) FreeScreenDrawInfo(ns.screen, ns.dri);
    ns.dri = NULL;
    if (ns.visual != NULL) FreeVisualInfo(ns.visual);
    ns.visual = NULL;
    if (ns.font != NULL) CloseFont(ns.font);
    ns.font = NULL;
    if (ns.screen != NULL) UnlockPubScreen(NULL, ns.screen);
    ns.screen = NULL;
}

/* ------------------------------------------------- drawing -------------- */

static void text_at(struct RastPort *rp, int x, int y, const char *s)
{
    if (s != NULL && s[0] != '\0')
    {
        Move(rp, x, y);
        Text(rp, (CONST_STRPTR)s, (ULONG)text_len(s));
    }
}

static int text_width(struct RastPort *rp, const char *s)
{
    return TextLength(rp, (CONST_STRPTR)s, (ULONG)text_len(s));
}

static void draw_glyph(struct RastPort *rp, int cx, int cy, NsState state,
                       UBYTE pen)
{
    int r = ns.font->tf_YSize / 2;

    SetAPen(rp, pen);
    SetDrMd(rp, JAM1);
    switch (state)
    {
    case NS_STATE_UP:                       /* filled disc */
        for (; r >= 0; r--)
            DrawCircle(rp, cx, cy, r);
        break;
    case NS_STATE_DOWN:                     /* hollow disc */
        DrawCircle(rp, cx, cy, r);
        break;
    case NS_STATE_DETACHED:                 /* hollow square */
        Move(rp, cx - r, cy - r);
        Draw(rp, cx + r, cy - r);
        Draw(rp, cx + r, cy + r);
        Draw(rp, cx - r, cy + r);
        Draw(rp, cx - r, cy - r);
        break;
    }
}

/* DrawBevelBoxA with the window's visual info; recessed FALSE is a button. */
static void draw_bevel(struct RastPort *rp, const NsBox *b, BOOL recessed)
{
    struct TagItem tags[3];

    tags[0].ti_Tag = GT_VisualInfo;
    tags[0].ti_Data = (ULONG)ns.visual;
    if (recessed)
    {
        tags[1].ti_Tag = GTBB_Recessed;
        tags[1].ti_Data = TRUE;
        tags[2].ti_Tag = TAG_DONE;
        tags[2].ti_Data = 0;
    }
    else
    {
        tags[1].ti_Tag = TAG_DONE;
        tags[1].ti_Data = 0;
    }
    DrawBevelBoxA(rp, (WORD)b->x, (WORD)b->y, (WORD)b->w, (WORD)b->h, tags);
}

static void draw_legend(struct RastPort *rp, const NsLayout *l, UBYTE textpen)
{
    static const NsState order[3] =
    {
        NS_STATE_UP, NS_STATE_DOWN, NS_STATE_DETACHED
    };
    const NsBox *lg = &l->box[NS_L_LEGEND];
    int          cy = lg->y + ns.font->tf_YSize / 2;
    int          ty = lg->y + ns.font->tf_Baseline;
    int          x = lg->x;
    int          s;

    SetAPen(rp, textpen);
    for (s = 0; s < 3; s++)
    {
        const char *word = ns_legend_word(order[s]);

        draw_glyph(rp, x + l->glyph_x + l->glyph_w / 2, cy, order[s], textpen);
        text_at(rp, x + l->name_x, ty, word);
        x += l->name_x + text_width(rp, word) + 16;
    }
}

static void draw_row(struct RastPort *rp, const NsLayout *l, int slot,
                     unsigned row, UBYTE textpen, UBYTE fillpen,
                     UBYTE filltext)
{
    const NsRow  *r = &ns.row[row];
    const NsBox  *list = &l->box[NS_L_LIST];
    int           y0 = list->y + l->cell_pad + slot * l->row_h;
    int           cy = y0 + ns.font->tf_YSize / 2;
    int           ty = y0 + ns.font->tf_Baseline;
    int           right = list->x + list->w - 1 - l->strip_w;
    BOOL          sel = (row == ns.selected);
    UBYTE         pen = sel ? filltext : textpen;
    char          buf[AMI_CFG_IP6_STRLEN + 16];

    if (sel)
    {
        SetAPen(rp, fillpen);
        RectFill(rp, list->x + 1, y0, right, y0 + l->row_h - 1);
    }

    draw_glyph(rp, list->x + l->glyph_x + l->glyph_w / 2, cy, r->state, pen);
    SetAPen(rp, pen);

    ns_elide(buf, sizeof(buf), r->name, (unsigned)l->name_chars);
    text_at(rp, list->x + l->name_x, ty, buf);

    ns_elide(buf, sizeof(buf), r->devunit, (unsigned)l->device_chars);
    text_at(rp, list->x + l->device_x, ty, buf);

    ns_list_address(buf, sizeof(buf), r->ipv4, r->ipv6, r->state);
    ns_elide(buf, sizeof(buf), buf, (unsigned)l->address_chars);
    text_at(rp, list->x + l->address_x, ty, buf);
}

/* The corner arrows in the scroller strip, bottom-right of the list. */
static void draw_scroller(struct RastPort *rp, const NsLayout *l, int strip_x,
                          UBYTE textpen)
{
    const NsBox *list = &l->box[NS_L_LIST];
    int          h = ns.font->tf_YSize;
    int          up_y = list->y + list->h - 2 * h;

    SetAPen(rp, textpen);
    /* up */
    Move(rp, strip_x + 3, up_y + h - 4);
    Draw(rp, strip_x + l->strip_w / 2, up_y + 3);
    Draw(rp, strip_x + l->strip_w - 3, up_y + h - 4);
    Draw(rp, strip_x + 3, up_y + h - 4);
    /* down */
    Move(rp, strip_x + 3, up_y + h + 3);
    Draw(rp, strip_x + l->strip_w / 2, up_y + 2 * h - 4);
    Draw(rp, strip_x + l->strip_w - 3, up_y + h + 3);
    Draw(rp, strip_x + 3, up_y + h + 3);
}

static void draw_panel(struct RastPort *rp, const NsLayout *l, UBYTE textpen)
{
    const NsBox *p = &l->box[NS_L_PANEL];
    int          lead = ns.font->tf_YSize + l->panel_lead;
    int          x = p->x;
    int          line = 0;
    int          ty = p->y + ns.font->tf_Baseline;
    const NsRow *r = (ns.selected < ns.count) ? &ns.row[ns.selected] : NULL;
    char         buf[192];

    SetAPen(rp, textpen);

    ns_selected_line(buf, sizeof(buf), (r != NULL) ? r->name : "");
    text_at(rp, x, ty, buf);

    text_at(rp, x, ty + ++line * lead, (r != NULL) ? r->devunit : "");

    text_at(rp, x, ty + ++line * lead, ns_addresses_head);

    if (r != NULL)
    {
        unsigned i;

        for (i = 0; i < 3; i++)
        {
            const char *a = NULL;

            if (i < r->naddrs)
                a = r->addrs[i];
            else if (i == 0)
                a = (r->state == NS_STATE_UP) ? ns_panel_pending
                                              : ns_panel_none;
            text_at(rp, x, ty + ++line * lead, a);
        }
    }
    else
    {
        line += 3;
    }

    {
        char ebuf[96];

        ns_last_error(ebuf, sizeof(ebuf), ns.last_error);
        text_at(rp, x, ty + ++line * lead, ebuf);
    }
}

static void draw_button(struct RastPort *rp, const NsBox *b, const char *label,
                        BOOL enabled)
{
    int ty = b->y + (b->h - ns.font->tf_YSize) / 2 + ns.font->tf_Baseline;

    if (enabled)
    {
        draw_bevel(rp, b, FALSE);
        SetAPen(rp, ns.dri->dri_Pens[TEXTPEN]);
    }
    else
    {
        /* Flat grey box with a dark border; the label ghosts in shadow. */
        SetAPen(rp, ns.dri->dri_Pens[BACKGROUNDPEN]);
        RectFill(rp, b->x, b->y, b->x + b->w - 1, b->y + b->h - 1);
        SetAPen(rp, ns.dri->dri_Pens[SHADOWPEN]);
        Move(rp, b->x, b->y);
        Draw(rp, b->x + b->w - 1, b->y);
        Draw(rp, b->x + b->w - 1, b->y + b->h - 1);
        Draw(rp, b->x, b->y + b->h - 1);
        Draw(rp, b->x, b->y);
    }
    text_at(rp, b->x + (b->w - text_width(rp, label)) / 2, ty, label);
}

static void draw_focus(struct RastPort *rp, const NsLayout *l)
{
    const NsBox *b;

    if (ns.focus == NS_FOCUS_ACTION)
        b = &l->box[NS_L_ACTION];
    else if (ns.focus == NS_FOCUS_ALL)
        b = &l->box[NS_L_ALL];
    else
        b = &l->box[NS_L_LIST];

    SetAPen(rp, ns.dri->dri_Pens[DETAILPEN]);
    Move(rp, b->x - 1, b->y - 1);
    Draw(rp, b->x + b->w, b->y - 1);
    Draw(rp, b->x + b->w, b->y + b->h);
    Draw(rp, b->x - 1, b->y + b->h);
    Draw(rp, b->x - 1, b->y - 1);
}

static void draw_all(void)
{
    struct RastPort *rp;
    const NsLayout  *l = &ns.layout;
    UBYTE            textpen, fillpen, filltext;
    unsigned         i;
    int              strip_x;
    char             buf[192];

    if (ns.window == NULL)
        return;
    rp = ns.window->RPort;
    SetFont(rp, ns.font);
    SetDrMd(rp, JAM1);

    textpen  = ns.dri->dri_Pens[TEXTPEN];
    fillpen  = ns.dri->dri_Pens[FILLPEN];
    filltext = ns.dri->dri_Pens[FILLTEXTPEN];

    draw_legend(rp, l, textpen);

    draw_bevel(rp, &l->box[NS_L_LIST], TRUE);
    strip_x = l->box[NS_L_LIST].x + l->box[NS_L_LIST].w - l->strip_w;
    for (i = 0; i < (unsigned)l->rows; i++)
    {
        unsigned row = ns.first + i;

        if (row >= ns.count)
            break;
        draw_row(rp, l, (int)i, row, textpen, fillpen, filltext);
    }
    if (l->scroll)
        draw_scroller(rp, l, strip_x, textpen);

    draw_panel(rp, l, textpen);

    {
        const char *action;
        const char *hint;

        action = (ns.selected < ns.count)
                     ? ns_action_word(ns.row[ns.selected].state)
                     : ns_action_word(NS_STATE_DETACHED);
        draw_button(rp, &l->box[NS_L_ACTION], action, action_enabled());

        ns_take_all_label(buf, sizeof(buf), online_count());
        draw_button(rp, &l->box[NS_L_ALL], buf, take_all_enabled());

        hint = ns.stack_down ? ns_stack_down_hint : ns_hint_text;
        SetAPen(rp, textpen);
        text_at(rp, l->box[NS_L_HINT].x,
                l->box[NS_L_HINT].y + ns.font->tf_Baseline, hint);
    }

    draw_bevel(rp, &l->box[NS_L_STATUS], TRUE);
    {
        unsigned up = 0, down = 0, detached = 0;

        for (i = 0; i < ns.count; i++)
        {
            if (ns.row[i].state == NS_STATE_UP) up++;
            else if (ns.row[i].state == NS_STATE_DOWN) down++;
            else detached++;
        }
        ns_status_line(buf, sizeof(buf), ns.count, up, down, detached);
        SetAPen(rp, textpen);
        text_at(rp, l->box[NS_L_STATUS].x + 4,
                l->box[NS_L_STATUS].y + 2 + ns.font->tf_Baseline, buf);
    }

    draw_focus(rp, l);
}

/* ------------------------------------------------- interaction ---------- */

static BOOL in_box(int x, int y, const NsBox *b)
{
    return x >= b->x && x < b->x + b->w && y >= b->y && y < b->y + b->h;
}

static void scroll_by(int delta)
{
    int max_first = (int)ns.count - ns.layout.rows;

    if (max_first < 0) max_first = 0;
    ns.first = (unsigned)((int)ns.first + delta);
    if ((int)ns.first < 0) ns.first = 0;
    if ((int)ns.first > max_first) ns.first = (unsigned)max_first;
}

static void focus_next(void)
{
    int f = ns.focus;
    int i;

    for (i = 0; i < 3; i++)
    {
        f = (f + 1) % 3;
        if (f == NS_FOCUS_LIST)
            break;
        if (f == NS_FOCUS_ACTION && action_enabled())
            break;
        if (f == NS_FOCUS_ALL && take_all_enabled())
            break;
    }
    ns.focus = f;
}

static void on_click(WORD mx, WORD my, struct IntuiMessage *msg)
{
    const NsLayout *l = &ns.layout;
    ULONG           now = (ULONG)msg->Seconds * 1000000UL +
                          (ULONG)msg->Micros;

    if (in_box(mx, my, &l->box[NS_L_ACTION]) && action_enabled())
    {
        ns.focus = NS_FOCUS_ACTION;
        do_action();
        return;
    }
    if (in_box(mx, my, &l->box[NS_L_ALL]) && take_all_enabled())
    {
        ns.focus = NS_FOCUS_ALL;
        take_all_offline();
        return;
    }
    if (l->scroll &&
        mx >= l->box[NS_L_LIST].x + l->box[NS_L_LIST].w - l->strip_w &&
        in_box(mx, my, &l->box[NS_L_LIST]))
    {
        int h = ns.font->tf_YSize;
        int up_y = l->box[NS_L_LIST].y + l->box[NS_L_LIST].h - 2 * h;

        if (my >= up_y && my < up_y + h) scroll_by(-1);
        else scroll_by(1);
        return;
    }
    if (in_box(mx, my, &l->box[NS_L_LIST]))
    {
        int      slot = (my - l->box[NS_L_LIST].y - l->cell_pad) / l->row_h;
        unsigned row = (unsigned)((int)ns.first + slot);

        ns.focus = NS_FOCUS_LIST;
        if (slot >= 0 && row < ns.count)
        {
            if (row == ns.selected && row == ns.last_click_row &&
                now - ns.last_click_us < 500000UL)
            {
                if (action_enabled()) do_action();
            }
            else
            {
                ns.selected = row;
                ns.first = (unsigned)ns_first_row(l, ns.count, ns.selected);
            }
            ns.last_click_row = row;
            ns.last_click_us = now;
        }
        return;
    }
}

static void on_key(UWORD code)
{
    const NsLayout *l = &ns.layout;

    if (code & 0x80)                        /* key-up */
        return;

    switch (code)
    {
    case CURSORUP:
        if (ns.focus == NS_FOCUS_LIST && ns.selected > 0)
        {
            ns.selected--;
            ns.first = (unsigned)ns_first_row(l, ns.count, ns.selected);
        }
        break;
    case CURSORDOWN:
        if (ns.focus == NS_FOCUS_LIST && ns.selected + 1 < ns.count)
        {
            ns.selected++;
            ns.first = (unsigned)ns_first_row(l, ns.count, ns.selected);
        }
        break;
    case RAWKEY_TAB:
        focus_next();
        break;
    case RAWKEY_RETURN:
    case RAWKEY_ENTER:
    case RAWKEY_SPACE:
        if (ns.focus == NS_FOCUS_LIST || ns.focus == NS_FOCUS_ACTION)
        {
            if (action_enabled()) do_action();
        }
        else if (take_all_enabled())
        {
            take_all_offline();
        }
        break;
    case RAWKEY_ESC:
        if (ns.focus != NS_FOCUS_LIST)
            ns.focus = NS_FOCUS_LIST;
        break;
    default:
        break;
    }
}

static void event_loop(void)
{
    BOOL  done = FALSE;
    UWORD ticks = 0;

    while (!done)
    {
        struct IntuiMessage *msg;

        Wait(1UL << ns.window->UserPort->mp_SigBit);
        while ((msg = GT_GetIMsg(ns.window->UserPort)) != NULL)
        {
            ULONG cls = msg->Class;
            UWORD code = msg->Code;
            WORD  mx = msg->MouseX;
            WORD  my = msg->MouseY;

            GT_ReplyIMsg(msg);

            if (cls == IDCMP_CLOSEWINDOW)
            {
                done = TRUE;
            }
            else if (cls == IDCMP_REFRESHWINDOW)
            {
                GT_BeginRefresh(ns.window);
                GT_EndRefresh(ns.window, TRUE);
                draw_all();
            }
            else if (cls == IDCMP_INTUITICKS)
            {
                /* IntuiTicks arrive roughly ten times a second.  A five-second
                   poll keeps external Online/Offline commands visible, and
                   notices the stack itself coming up. */
                if (++ticks >= 50)
                {
                    ticks = 0;
                    if (ns.base == NULL)
                    {
                        ns.base = tool_netstatus_open_resident();
                        ns.stack_down = (ns.base == NULL);
                    }
                    refresh_rows();
                    draw_all();
                }
            }
            else if (cls == IDCMP_MOUSEBUTTONS)
            {
                if (code == SELECTDOWN)
                    on_click(mx, my, msg);
                draw_all();
            }
            else if (cls == IDCMP_RAWKEY)
            {
                on_key(code);
                draw_all();
            }
        }
    }
}

/* ------------------------------------------------- startup -------------- */

static void requester(const char *body)
{
    struct EasyStruct easy;

    easy.es_StructSize   = sizeof(easy);
    easy.es_Flags        = 0;
    easy.es_Title        = (STRPTR)"AmiNetXDuo NetSwitch";
    easy.es_TextFormat   = (STRPTR)body;
    easy.es_GadgetFormat = (STRPTR)"OK";
    (VOID)EasyRequestArgs(ns.window, &easy, NULL, NULL);
}

/*
 * Intuition and the requesters run on the caller's stack, and a Shell gives a
 * command 4,096 bytes.  The icon asks for 8 KB, which is already sufficient;
 * below that the switch runs on its own 16 KB stack, the same trampoline the
 * other Workbench tools use (netprefs.c).
 */
#define NS_STACK_SIZE (16UL * 1024UL)
#define NS_SAFE_STACK (8UL * 1024UL)

static struct StackSwapStruct ns_sss;
static int                    ns_result;

static int netswitch_run(void);

static __attribute__((noinline)) VOID netswitch_trampoline(VOID)
{
    StackSwap(&ns_sss);
    ns_result = netswitch_run();
    StackSwap(&ns_sss);
}

int main(int argc, char **argv)
{
    struct Task *me = FindTask(NULL);
    ULONG        have = (ULONG)me->tc_SPUpper - (ULONG)me->tc_SPLower;
    APTR         stack;

    (VOID)argc;
    (VOID)argv;

    if (have >= NS_SAFE_STACK)
        return netswitch_run();

    stack = AllocMem(NS_STACK_SIZE, MEMF_ANY);
    if (stack == NULL)
    {
        tool_error("not enough memory for NetSwitch's 16 KB working stack");
        return RETURN_FAIL;
    }

    ns_sss.stk_Lower   = stack;
    ns_sss.stk_Upper   = (ULONG)stack + NS_STACK_SIZE;
    ns_sss.stk_Pointer = (APTR)((ULONG)stack + NS_STACK_SIZE);
    netswitch_trampoline();
    FreeMem(stack, NS_STACK_SIZE);
    return ns_result;
}

static int netswitch_run(void)
{
    IntuitionBase = (struct IntuitionBase *)OpenLibrary("intuition.library", 37);
    GfxBase = (struct GfxBase *)OpenLibrary("graphics.library", 37);
    GadToolsBase = OpenLibrary("gadtools.library", 37);
    if (IntuitionBase == NULL || GfxBase == NULL || GadToolsBase == NULL)
    {
        if (GadToolsBase != NULL) CloseLibrary(GadToolsBase);
        if (GfxBase != NULL) CloseLibrary((struct Library *)GfxBase);
        if (IntuitionBase != NULL) CloseLibrary((struct Library *)IntuitionBase);
        return RETURN_FAIL;
    }

    enumerate_interfaces();
    ns.base = tool_netstatus_open_resident();
    ns.stack_down = (ns.base == NULL);
    ns.last_error[0] = '\0';
    refresh_rows();

    if (!make_window())
    {
        requester("Cannot open window: needs a 640x200 Workbench screen and "
                  "OS 2.04.");
        close_ui();
        if (ns.base != NULL) tool_netstatus_close(ns.base);
        CloseLibrary(GadToolsBase);
        CloseLibrary((struct Library *)GfxBase);
        CloseLibrary((struct Library *)IntuitionBase);
        return RETURN_FAIL;
    }

    draw_all();
    event_loop();
    close_ui();
    if (ns.base != NULL) tool_netstatus_close(ns.base);
    CloseLibrary(GadToolsBase);
    CloseLibrary((struct Library *)GfxBase);
    CloseLibrary((struct Library *)IntuitionBase);
    return RETURN_OK;
}

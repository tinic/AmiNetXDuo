/*
 * NetSwitch: every interface in DEVS:NetInterfaces with its live state and
 * first IPv4 address, and Online and Offline for the selected one.  It reads
 * and switches through the netstatus calls C:Online and C:Offline use
 * (onoff.c), and does not wait for a link or a lease: the list shows them.
 *
 * SPDX-License-Identifier: MIT
 */

#include "tools.h"
#include "netswitch_banner.h"

#include <exec/memory.h>
#include <graphics/gfxbase.h>
#include <graphics/rastport.h>
#include <graphics/text.h>
#include <intuition/intuition.h>
#include <intuition/intuitionbase.h>
#include <libraries/gadtools.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <proto/gadtools.h>
#include <proto/graphics.h>
#include <proto/intuition.h>

const char *const tool_name = "NetSwitch";

static const char version_tag[] __attribute__((used)) =
    TOOL_VERSTAG("NetSwitch");

struct IntuitionBase *IntuitionBase;
struct GfxBase       *GfxBase;
struct Library       *GadToolsBase;

#define NS_MAX_ROWS     16      /* as many interface files as Online scans */
#define NS_NAME_CHARS   15      /* the live status ABI's name length */
#define NS_STATE_CHARS  12      /* "not attached" */
#define NS_ROW_CHARS    (NS_NAME_CHARS + 2 + NS_STATE_CHARS + 2 + 15)
#define NS_LIST_ROWS    6

/* Spacing in pixels, as NetPrefs has it; everything else is the font's. */
#define MARGIN_X        8
#define MARGIN_Y        5
#define GAP             8
#define BUTTON_PAD      10
#define SCROLLER_W      16
#define LIST_PAD        12      /* listview frame and text inset */

enum { GID_LIST = 1, GID_ONLINE, GID_OFFLINE };

/* NS_UNKNOWN: the stack is up and did not answer.  NS_DETACHED: no stack,
   or not in it. */
enum { NS_UNKNOWN, NS_DETACHED, NS_OFFLINE, NS_ONLINE };

static const char *const ns_state_text[] =
    { "unknown", "not attached", "offline", "online" };

typedef struct NsRow
{
    char  name[TOOL_NAME_LEN];
    UWORD state;
    UWORD index;                /* NX interface index, when attached */
    ULONG address;              /* host order, 0 for none */
} NsRow;

static struct
{
    struct Screen      *screen;
    APTR                visual;
    struct Window      *window;
    struct Gadget      *gadgets;
    struct Gadget      *g_list, *g_online, *g_offline;
    NsBanner            banner;     /* netswitch_banner.c; bm NULL: none */
    WORD                banner_x, banner_y;
    struct TextAttr     list_ta;    /* fixed width, so the columns line up */
    struct List         list;
    struct Node         nodes[NS_MAX_ROWS];
    NsRow               rows[NS_MAX_ROWS];
    char                text[NS_MAX_ROWS][NS_ROW_CHARS + 1];
    ULONG               count;
    LONG                selected;
    ULONG               click_secs, click_micros;
} ns = { .selected = -1 };

/* The live snapshot; static, as onoff.c keeps it. */
static struct
{
    NetStatusHeader     hdr;
    NetStatusInterface  e[NX_MAX_PHYSICAL_INTERFACES];
} ns_live;

static struct TextAttr topaz8 = { (STRPTR)"topaz.font", 8, FS_NORMAL, FPF_ROMFONT };

/* One requester; `fmt` takes RawDoFmt() arguments from `args`. */
static VOID requester(const char *fmt, const ULONG *args)
{
    struct EasyStruct easy = { sizeof(struct EasyStruct), 0,
        (STRPTR)"NetSwitch", (STRPTR)fmt, (STRPTR)"OK" };

    (VOID)EasyRequestArgs(ns.window, &easy, NULL, (APTR)args);
}

static VOID set_attr(struct Gadget *g, ULONG tag, ULONG value)
{
    struct TagItem tags[2] = { { tag, value }, { TAG_DONE, 0 } };

    GT_SetGadgetAttrsA(g, ns.window, NULL, tags);
}

/* `text` left-justified in a column of `width`, cut at the width, then a
   two-space gutter. */
static char *column(char *out, const char *text, ULONG width)
{
    ULONG i;

    for (i = 0; i < width && text[i] != '\0'; i++) out[i] = text[i];
    for (; i < width; i++) out[i] = ' ';
    out[width] = ' ';
    out[width + 1] = ' ';
    return out + width + 2;
}

static VOID format_row(const NsRow *row, char *out)
{
    out = column(out, row->name, NS_NAME_CHARS);
    out = column(out, ns_state_text[row->state], NS_STATE_CHARS);
    if (row->address != 0)
        ami_config_format_ip(row->address, out, 16);
    else
        tool_copy_string(out, 16, "-");
}

/*
 * Every interface file, sorted, with its state in the running stack.  The
 * live row is found by name, never by position, as live_index() in onoff.c.
 */
static ULONG read_rows(NsRow *rows)
{
    static char     names[NS_MAX_ROWS][TOOL_NAME_LEN];
    char            where[AMI_CFG_PATH_LEN];
    struct Library *base;
    LONG            live = -1;
    ULONG           count, i;
    LONG            j;
    UWORD           fallback = NS_DETACHED;

    count = tool_list_dir(ami_cfg_resolve("DEVS:NetInterfaces", where,
                                          sizeof(where)),
                          names, NS_MAX_ROWS, NULL);

    if (tool_stack_library_running())
    {
        base = tool_netstatus_open(TRUE);
        if (base != NULL)
        {
            live = tool_netstatus_query(base, NETSTATUS_INTERFACES, &ns_live,
                                        sizeof(ns_live),
                                        sizeof(NetStatusInterface));
            tool_netstatus_close(base);
        }
        if (live < 0) fallback = NS_UNKNOWN;
    }

    for (i = 0; i < count; i++)
    {
        NsRow *row;

        /* Insertion by name, case-blind, as NetPrefs lists them. */
        for (j = (LONG)i - 1;
             j >= 0 && tool_stricmp(rows[j].name, names[i]) > 0; j--)
            rows[j + 1] = rows[j];
        row = &rows[j + 1];
        tool_copy_string(row->name, sizeof(row->name), names[i]);
        row->state   = fallback;
        row->index   = 0;
        row->address = 0;

        for (j = 0; j < live && j < (LONG)NX_MAX_PHYSICAL_INTERFACES; j++)
        {
            const NetStatusInterface *e = &ns_live.e[j];

            if (!(e->nsi_Flags & NETSTATUS_IF_NAMED) ||
                tool_stricmp(e->nsi_Name, names[i]) != 0)
                continue;
            /* LINKUP is the flag Online and Offline set (onoff.c). */
            row->state   = (e->nsi_Flags & NETSTATUS_IF_LINKUP)
                               ? NS_ONLINE : NS_OFFLINE;
            row->index   = e->nsi_Index;
            row->address = e->nsi_Address;
            break;
        }
    }
    return count;
}

static LONG find_row(const char *name)
{
    ULONG i;

    for (i = 0; i < ns.count; i++)
        if (tool_stricmp(ns.rows[i].name, name) == 0) return (LONG)i;
    return -1;
}

static BOOL can_go(UWORD state, BOOL up)
{
    return up ? (BOOL)(state == NS_DETACHED || state == NS_OFFLINE)
              : (BOOL)(state == NS_ONLINE);
}

static VOID update_buttons(VOID)
{
    UWORD state = ns.selected >= 0 ? ns.rows[ns.selected].state : NS_UNKNOWN;

    set_attr(ns.g_online, GA_Disabled, (ULONG)!can_go(state, TRUE));
    set_attr(ns.g_offline, GA_Disabled, (ULONG)!can_go(state, FALSE));
}

/* NewList(), which is amiga.lib's and not linked. */
static VOID empty_list(VOID)
{
    ns.list.lh_Head     = (struct Node *)&ns.list.lh_Tail;
    ns.list.lh_Tail     = NULL;
    ns.list.lh_TailPred = (struct Node *)&ns.list.lh_Head;
}

/* Read the list again and redraw it if its text changed, keeping the
   selection by name. */
static VOID refresh(VOID)
{
    static NsRow rows[NS_MAX_ROWS];
    static char  text[NS_MAX_ROWS][NS_ROW_CHARS + 1];
    char         selected[TOOL_NAME_LEN];
    ULONG        count = read_rows(rows);
    ULONG        i;
    BOOL         same = (BOOL)(count == ns.count);

    for (i = 0; i < count; i++)
    {
        format_row(&rows[i], text[i]);
        if (same && tool_stricmp(text[i], ns.text[i]) != 0) same = FALSE;
    }

    selected[0] = '\0';
    if (ns.selected >= 0)
        tool_copy_string(selected, sizeof(selected), ns.rows[ns.selected].name);

    /* Copied even when the text is the same: an index can move unseen. */
    for (i = 0; i < count; i++) ns.rows[i] = rows[i];

    if (!same)
    {
        /* GadTools may walk the old list while it repaints: detach it before
           rebuilding it in place. */
        if (ns.window != NULL) set_attr(ns.g_list, GTLV_Labels, ~0UL);
        ns.count = count;
        empty_list();
        for (i = 0; i < count; i++)
        {
            tool_copy_string(ns.text[i], sizeof(ns.text[i]), text[i]);
            ns.nodes[i].ln_Name = ns.text[i];
            AddTail(&ns.list, &ns.nodes[i]);
        }
    }

    ns.selected = selected[0] != '\0' ? find_row(selected) : -1;
    if (ns.window == NULL) return;
    if (!same)
    {
        set_attr(ns.g_list, GTLV_Labels, (ULONG)&ns.list);
        set_attr(ns.g_list, GTLV_Selected,
                 ns.selected >= 0 ? (ULONG)ns.selected : ~0UL);
    }
    update_buttons();
}

/*
 * One interface up or down, as C:Online and C:Offline do it: attach a
 * definition the stack does not have, starting the stack first when nothing
 * runs, else NETCTRL_INTERFACE_UP or _DOWN on its index.  A failure is said
 * in one requester.
 */
static VOID switch_row(const NsRow *row, BOOL up)
{
    struct Library  *base;
    NetStatusControl ctl;
    ULONG            args[4];
    ULONG            w;
    LONG             err = 0;
    BOOL             started = !tool_stack_library_running();
    const char      *why = NULL;

    args[0] = (ULONG)row->name;
    base = started ? tool_stack_start() : tool_netstatus_open(TRUE);
    if (base == NULL || (started && !tool_stack_is_ours(base)))
    {
        if (base != NULL) tool_stack_release(base);
        requester(base != NULL ? "%s: another TCP/IP stack is installed."
                  : started ? "%s: bsdsocket.library did not open."
                  : "%s: the network did not answer.", args);
        return;
    }

    if (row->state == NS_DETACHED)
    {
        err = tool_stack_add_interface(base, row->name, NULL, TRUE);
        if (err == EEXIST) err = 0;
    }
    else
    {
        for (w = 0; w < (ULONG)(sizeof(ctl) / sizeof(ULONG)); w++)
            ((ULONG *)&ctl)[w] = 0;
        ctl.nsc_Index = row->index;
        if (tool_netstatus_control(base, up ? NETCTRL_INTERFACE_UP
                                            : NETCTRL_INTERFACE_DOWN,
                                   &ctl, &err) != 0 && err == 0)
            err = EIO;
    }

    if (err != 0)
        why = "%s did not go %s: %s (%ld).";
    /* A stack started here outlives this window only with a hold. */
    else if (started && !tool_stack_hold(base))
        why = "%s: network hold failed.";

    if (started) tool_stack_release(base);
    else tool_netstatus_close(base);

    if (why != NULL)
    {
        args[1] = (ULONG)(up ? "online" : "offline");
        args[2] = (ULONG)tool_code_errno(err);
        args[3] = (ULONG)err;
        requester(why, args);
    }
}

/*
 * The selected row up or down.  The state is read again first: when the
 * interface has gone or changed since the list was drawn, from a Shell say,
 * the list shows that and nothing is done.
 */
static VOID act(BOOL up)
{
    NsRow seen;
    LONG  now;
    BOOL  busy = (BOOL)(IntuitionBase->LibNode.lib_Version >= 39);

    if (ns.selected < 0) return;
    seen = ns.rows[ns.selected];
    refresh();
    now = find_row(seen.name);
    if (now < 0 || ns.rows[now].state != seen.state || !can_go(seen.state, up))
        return;

    /* Attaching opens the driver, which takes a moment.  The busy pointer is
       V39. */
    if (busy) SetWindowPointer(ns.window, WA_BusyPointer, TRUE, TAG_DONE);
    switch_row(&ns.rows[now], up);
    if (busy) SetWindowPointer(ns.window, WA_Pointer, 0, TAG_DONE);
    refresh();
}

/* ---------------------------------------------------------------- window -- */

static struct Gadget *add_gadget(ULONG kind, struct Gadget *previous,
                                 UWORD id, WORD x, WORD y, WORD w, WORD h,
                                 const char *label, struct TextAttr *ta,
                                 struct TagItem *tags)
{
    struct NewGadget ng;

    if (previous == NULL) return NULL;
    ng.ng_LeftEdge = x; ng.ng_TopEdge = y;
    ng.ng_Width = w; ng.ng_Height = h;
    ng.ng_GadgetText = (STRPTR)label;
    ng.ng_TextAttr = ta;
    ng.ng_GadgetID = id; ng.ng_Flags = PLACETEXT_IN;
    ng.ng_VisualInfo = ns.visual; ng.ng_UserData = NULL;
    return CreateGadgetA(kind, previous, &ng, tags);
}

/* The list in a fixed-width font: the buttons' when it is one, else the
   system default (the Shell's), else topaz 8. */
static VOID pick_list_font(struct TextAttr *ta, const struct TextFont *bf)
{
    struct TextFont *df = GfxBase->DefaultFont;

    if (!(bf->tf_Flags & FPF_PROPORTIONAL))
        ns.list_ta = *ta;
    else if (!(df->tf_Flags & FPF_PROPORTIONAL))
    {
        ns.list_ta.ta_Name  = (STRPTR)df->tf_Message.mn_Node.ln_Name;
        ns.list_ta.ta_YSize = df->tf_YSize;
        ns.list_ta.ta_Style = df->tf_Style;
        ns.list_ta.ta_Flags = df->tf_Flags;
    }
    else
        ns.list_ta = topaz8;
}

/* The banner over the list (netswitch_banner.c), beside the program.
   Optional: without it, or when the window would not fit with it, the
   window opens without it. */
static VOID draw_banner(VOID)
{
    if (ns.banner.bm != NULL && ns.window != NULL)
        BltBitMapRastPort(ns.banner.bm, 0, 0, ns.window->RPort, ns.banner_x,
                          ns.banner_y, ns.banner.w, ns.banner.h, 0xC0);
}

/* The gadgets and the window, with the buttons in `ta`.  FALSE when it does
   not fit the screen or something failed. */
static BOOL make_window(struct TextAttr *ta)
{
    struct Screen   *s = ns.screen;
    struct TextFont *bf, *lf;
    struct RastPort  rp;
    struct Gadget   *g;
    WORD left, top, list_w, list_h, button_w, button_h, inner_w, inner_h;
    WORD width, height, w;
    WORD above = ns.banner.h != 0 ? (WORD)(ns.banner.h + MARGIN_Y) : 0;

    if ((bf = OpenFont(ta)) == NULL) return FALSE;
    pick_list_font(ta, bf);
    if ((lf = OpenFont(&ns.list_ta)) == NULL) { CloseFont(bf); return FALSE; }

    InitRastPort(&rp);
    SetFont(&rp, bf);
    button_w = (WORD)TextLength(&rp, (CONST_STRPTR)"Offline", 7);
    w        = (WORD)TextLength(&rp, (CONST_STRPTR)"Online", 6);
    if (w > button_w) button_w = w;
    button_w = (WORD)(button_w + 2 * BUTTON_PAD);
    button_h = (WORD)(bf->tf_YSize + 6);
    list_w   = (WORD)(NS_ROW_CHARS * lf->tf_XSize + SCROLLER_W + LIST_PAD);
    list_h   = (WORD)(NS_LIST_ROWS * lf->tf_YSize + 4);
    CloseFont(bf);
    CloseFont(lf);

    if (list_w < 2 * button_w + GAP) list_w = (WORD)(2 * button_w + GAP);
    if (list_w < ns.banner.w) list_w = ns.banner.w;
    button_w = (WORD)((list_w - GAP) / 2);
    inner_w  = (WORD)(list_w + 2 * MARGIN_X);
    inner_h  = (WORD)(above + MARGIN_Y + list_h + GAP / 2 + button_h +
                      MARGIN_Y);

    /* The title bar is in the screen font whatever the gadgets use. */
    left   = s->WBorLeft;
    top    = (WORD)(s->WBorTop + s->Font->ta_YSize + 1);
    width  = (WORD)(inner_w + left + s->WBorRight);
    height = (WORD)(inner_h + top + s->WBorBottom);
    if (width > s->Width || height > s->Height) return FALSE;
    ns.banner_x = (WORD)(left + (inner_w - ns.banner.w) / 2);
    ns.banner_y = (WORD)(top + MARGIN_Y);
    top = (WORD)(top + above);

    {
        struct TagItem lv[] = {
            { GTLV_Labels, (ULONG)&ns.list }, { GTLV_ShowSelected, 0 },
            { GTLV_Selected, ~0UL }, { GTLV_ScrollWidth, SCROLLER_W },
            { TAG_DONE, 0 } };
        struct TagItem off[] = { { GA_Disabled, TRUE }, { TAG_DONE, 0 } };
        WORD y = (WORD)(top + MARGIN_Y + list_h + GAP / 2);

        g = CreateContext(&ns.gadgets);
        ns.g_list = g = add_gadget(LISTVIEW_KIND, g, GID_LIST,
                                   (WORD)(left + MARGIN_X),
                                   (WORD)(top + MARGIN_Y), list_w, list_h,
                                   NULL, &ns.list_ta, lv);
        ns.g_online = g = add_gadget(BUTTON_KIND, g, GID_ONLINE,
                                     (WORD)(left + MARGIN_X), y, button_w,
                                     button_h, "Online", ta, off);
        ns.g_offline = g = add_gadget(BUTTON_KIND, g, GID_OFFLINE,
                                      (WORD)(left + inner_w - MARGIN_X -
                                             button_w), y, button_w,
                                      button_h, "Offline", ta, off);
        if (g == NULL) return FALSE;
    }
    {
        struct TagItem win[] = {
            { WA_Left, (ULONG)((s->Width - width) / 2) },
            { WA_Top, (ULONG)((s->Height - height) / 2) },
            { WA_InnerWidth, (ULONG)inner_w },
            { WA_InnerHeight, (ULONG)inner_h },
            { WA_Title, (ULONG)"NetSwitch" },
            { WA_IDCMP, IDCMP_CLOSEWINDOW | IDCMP_REFRESHWINDOW |
                        BUTTONIDCMP | LISTVIEWIDCMP | IDCMP_INTUITICKS },
            { WA_Flags, WFLG_DRAGBAR | WFLG_DEPTHGADGET | WFLG_CLOSEGADGET |
                        WFLG_ACTIVATE | WFLG_SMART_REFRESH },
            { WA_Gadgets, (ULONG)ns.gadgets },
            { WA_PubScreen, (ULONG)s },
            { TAG_DONE, 0 } };

        ns.window = OpenWindowTagList(NULL, win);
    }
    if (ns.window == NULL) return FALSE;
    GT_RefreshWindow(ns.window, NULL);
    draw_banner();
    return TRUE;
}

static VOID free_window(VOID)
{
    if (ns.window != NULL) CloseWindow(ns.window);
    ns.window = NULL;
    if (ns.gadgets != NULL) FreeGadgets(ns.gadgets);
    ns.gadgets = NULL;
}

/* A font too large for the screen falls back to topaz 8, which fits the
   stock 640x200 NTSC Workbench. */
static BOOL open_ui(VOID)
{
    if ((ns.screen = LockPubScreen(NULL)) == NULL) return FALSE;
    if ((ns.visual = GetVisualInfoA(ns.screen, NULL)) == NULL) return FALSE;
    if (ns_banner_load(ns.screen, "PROGDIR:NetSwitch.ilbm", &ns.banner) &&
        make_window(ns.screen->Font)) return TRUE;
    free_window();
    ns_banner_free(ns.screen, &ns.banner);
    if (make_window(ns.screen->Font)) return TRUE;
    free_window();
    return make_window(&topaz8);
}

static VOID close_ui(VOID)
{
    free_window();
    ns_banner_free(ns.screen, &ns.banner);
    if (ns.visual != NULL) FreeVisualInfo(ns.visual);
    if (ns.screen != NULL) UnlockPubScreen(NULL, ns.screen);
}

static VOID list_clicked(const struct IntuiMessage *m)
{
    LONG row = (LONG)m->Code;

    if (row >= (LONG)ns.count) return;

    /* A double-click on a row switches it the other way. */
    if (row == ns.selected &&
        DoubleClick(ns.click_secs, ns.click_micros, m->Seconds, m->Micros))
    {
        ns.click_secs = ns.click_micros = 0;
        act((BOOL)(ns.rows[row].state != NS_ONLINE));
        return;
    }
    ns.selected     = row;
    ns.click_secs   = m->Seconds;
    ns.click_micros = m->Micros;
    update_buttons();
}

static VOID event_loop(VOID)
{
    ULONG win_sig = 1UL << ns.window->UserPort->mp_SigBit;
    UWORD ticks   = 0;
    BOOL  done    = FALSE;

    while (!done)
    {
        struct IntuiMessage *msg;

        if (Wait(win_sig | SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C) done = TRUE;
        while ((msg = GT_GetIMsg(ns.window->UserPort)) != NULL)
        {
            struct IntuiMessage m = *msg;
            UWORD id = m.IAddress != NULL
                           ? ((struct Gadget *)m.IAddress)->GadgetID : 0;

            GT_ReplyIMsg(msg);
            if (m.Class == IDCMP_CLOSEWINDOW) done = TRUE;
            /* About ten a second, while the window is active. */
            else if (m.Class == IDCMP_INTUITICKS)
            {
                if (++ticks >= 10) { ticks = 0; refresh(); }
            }
            else if (m.Class == IDCMP_REFRESHWINDOW)
            {
                GT_BeginRefresh(ns.window);
                draw_banner();
                GT_EndRefresh(ns.window, TRUE);
            }
            else if (m.Class == IDCMP_GADGETUP)
            {
                if (id == GID_LIST) list_clicked(&m);
                else if (id == GID_ONLINE) act(TRUE);
                else if (id == GID_OFFLINE) act(FALSE);
            }
        }
    }
}

/* A Shell gives a command 4 KB, too little for GadTools and EasyRequest:
   below 8 KB the window runs on its own 16 KB, as NetPrefs does.  The
   trampoline has no locals and stays noinline (netprefs.c). */
#define NETSWITCH_STACK_SIZE (16UL * 1024UL)
#define NETSWITCH_SAFE_STACK (8UL * 1024UL)

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
    APTR         stack;

    (VOID)argc;
    (VOID)argv;
    if ((ULONG)me->tc_SPUpper - (ULONG)me->tc_SPLower >= NETSWITCH_SAFE_STACK)
        return netswitch_run();
    if ((stack = AllocMem(NETSWITCH_STACK_SIZE, MEMF_ANY)) == NULL)
    {
        tool_error("not enough memory for NetSwitch's 16 KB working stack");
        return RETURN_FAIL;
    }
    ns_sss.stk_Lower   = stack;
    ns_sss.stk_Upper   = (ULONG)stack + NETSWITCH_STACK_SIZE;
    ns_sss.stk_Pointer = (APTR)((ULONG)stack + NETSWITCH_STACK_SIZE);
    netswitch_trampoline();
    FreeMem(stack, NETSWITCH_STACK_SIZE);
    return ns_result;
}

static int netswitch_run(void)
{
    int rc = RETURN_FAIL;

    IntuitionBase = (struct IntuitionBase *)OpenLibrary("intuition.library", 37);
    GfxBase = (struct GfxBase *)OpenLibrary("graphics.library", 37);
    GadToolsBase = OpenLibrary("gadtools.library", 37);

    if (IntuitionBase != NULL && GfxBase != NULL && GadToolsBase != NULL)
    {
        empty_list();
        refresh();
        if (open_ui())
        {
            refresh();
            event_loop();
            rc = RETURN_OK;
        }
        else
            requester("Cannot open the window: needs OS 2.04.", NULL);
        close_ui();
    }

    if (GadToolsBase != NULL) CloseLibrary(GadToolsBase);
    if (GfxBase != NULL) CloseLibrary((struct Library *)GfxBase);
    if (IntuitionBase != NULL) CloseLibrary((struct Library *)IntuitionBase);
    return rc;
}

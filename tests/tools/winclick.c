/*
 * WinClick: click one gadget of a window on the default public screen.
 *
 *     WinClick TITLE/A,GADGET/N,TIMES/N,ZIP/S,AT/K/N,X/K/N,Y/K/N,TEXT/K,DUMP/S
 *
 * ZIP clicks the window's zoom gadget rather than GADGET.  AT clicks that
 * many pixels below the gadget's top edge instead of its middle (a list row).
 * X and Y click that point inside the window's borders instead of a gadget:
 * an icon in a Workbench window, TIMES 2 to open it.
 *
 * TITLE is a prefix of the window title, GADGET the GadgetID.  The pointer is
 * placed with injected IECLASS_POINTERPOS events and corrected against
 * Screen->MouseX/Y, so no display-mode scaling rule is assumed.  TIMES 0
 * only reports the window's box.  Prints key=value; RC 0 when every click
 * landed on the gadget.
 *
 * TEXT clicks a string or integer gadget once, clears it (right Amiga X),
 * types the text through the keymap and presses Return.  DUMP clicks
 * nothing and prints the window's box and what GadTools reports for every
 * gadget in it: g<id>.str, .num, .checked, .cycle, .list, .text and
 * .disabled.
 *
 * SPDX-License-Identifier: MIT
 */

#include <devices/input.h>
#include <devices/inputevent.h>
#include <dos/dos.h>
#include <exec/io.h>
#include <exec/types.h>
#include <intuition/intuition.h>
#include <intuition/intuitionbase.h>
#include <intuition/screens.h>
#include <libraries/gadtools.h>

#include <proto/dos.h>
#include <proto/exec.h>
#include <proto/gadtools.h>
#include <proto/intuition.h>
#include <proto/keymap.h>

#include <string.h>

/* Set by GadTools V39+ on its own gadgets; the 3.1 NDK does not name it. */
#ifndef GTYP_GADTOOLS
#define GTYP_GADTOOLS 0x0100
#endif

struct IntuitionBase *IntuitionBase;
struct Library       *GadToolsBase;
struct Library       *KeymapBase;

static struct MsgPort   *in_port;
static struct IOStdReq  *in_req;
static BOOL              in_open;
static struct InputEvent ev;

static BOOL input_open(VOID)
{
    in_port = CreateMsgPort();
    if (in_port == NULL) return FALSE;
    in_req = (struct IOStdReq *)CreateIORequest(in_port,
                                                sizeof(struct IOStdReq));
    if (in_req == NULL) return FALSE;
    if (OpenDevice((CONST_STRPTR)"input.device", 0,
                   (struct IORequest *)in_req, 0) != 0)
        return FALSE;
    in_open = TRUE;
    return TRUE;
}

static VOID input_close(VOID)
{
    if (in_open) CloseDevice((struct IORequest *)in_req);
    if (in_req != NULL) DeleteIORequest((struct IORequest *)in_req);
    if (in_port != NULL) DeleteMsgPort(in_port);
}

static VOID send_event(UBYTE cls, UWORD code, UWORD qual, WORD x, WORD y)
{
    memset(&ev, 0, sizeof(ev));
    ev.ie_Class     = cls;
    ev.ie_Code      = code;
    ev.ie_Qualifier = qual;
    ev.ie_X         = x;
    ev.ie_Y         = y;
    CurrentTime((ULONG *)&ev.ie_TimeStamp.tv_secs,
                (ULONG *)&ev.ie_TimeStamp.tv_micro);
    in_req->io_Command = IND_WRITEEVENT;
    in_req->io_Flags   = 0;
    in_req->io_Length  = (LONG)sizeof(struct InputEvent);
    in_req->io_Data    = (APTR)&ev;
    (VOID)DoIO((struct IORequest *)in_req);
}

/* Moves the pointer to screen pixel (x, y); TRUE once it is there. */
static BOOL point_at(struct Screen *sc, WORD x, WORD y)
{
    LONG ix = x, iy = y;
    int  tries;

    for (tries = 0; tries < 6; tries++)
    {
        LONG gx, gy;

        send_event(IECLASS_POINTERPOS, IECODE_NOBUTTON, 0, (WORD)ix, (WORD)iy);
        Delay(3);
        gx = sc->MouseX;
        gy = sc->MouseY;
        if (gx == x && gy == y) return TRUE;
        /* Linear in both axes: rescale by what the last event produced. */
        ix = gx > 0 ? ix * x / gx : ix + (x - gx);
        iy = gy > 0 ? iy * y / gy : iy + (y - gy);
    }
    return FALSE;
}

static struct Window *find_window(struct Screen *sc, const char *prefix)
{
    struct Window *w;
    ULONG n = strlen(prefix);

    for (w = sc->FirstWindow; w != NULL; w = w->NextWindow)
        if (w->Title != NULL && strncmp((const char *)w->Title, prefix, n) == 0)
            return w;
    return NULL;
}

static VOID key(UWORD code, UWORD qual)
{
    send_event(IECLASS_RAWKEY, code, qual, 0, 0);
    Delay(1);
    send_event(IECLASS_RAWKEY, (UWORD)(code | IECODE_UP_PREFIX), qual, 0, 0);
    Delay(1);
}

/* The active gadget takes the keys: clear, type, Return. */
static BOOL type_text(const char *text)
{
    UBYTE map[8];
    LONG  n;

    key(0x32, IEQUALIFIER_RCOMMAND);
    for (; *text != '\0'; text++)
    {
        n = MapANSI((STRPTR)text, 1, (STRPTR)map, 4, NULL);
        if (n != 1) return FALSE;
        key(map[0], map[1]);
    }
    key(0x44, 0);
    Delay(5);
    return TRUE;
}

/* One GT_GetGadgetAttrs tag; TRUE when the gadget's kind has it. */
static BOOL gt_get(struct Gadget *g, struct Window *w, ULONG tag, ULONG *value)
{
    struct TagItem tags[2];

    tags[0].ti_Tag = tag; tags[0].ti_Data = (ULONG)value;
    tags[1].ti_Tag = TAG_DONE; tags[1].ti_Data = 0;
    return (BOOL)(GT_GetGadgetAttrsA(g, w, NULL, tags) == 1);
}

static VOID dump(struct Window *w)
{
    struct Gadget *g;
    ULONG v;

    Printf((CONST_STRPTR)"window=%ld,%ld,%ld,%ld\n", (LONG)w->LeftEdge,
           (LONG)w->TopEdge, (LONG)w->Width, (LONG)w->Height);
    for (g = w->FirstGadget; g != NULL; g = g->NextGadget)
    {
        LONG id = (LONG)g->GadgetID;

        if ((g->GadgetType & GTYP_SYSGADGET) ||
            !(g->GadgetType & GTYP_GADTOOLS)) continue;
        if (gt_get(g, w, GTST_String, &v))
            Printf((CONST_STRPTR)"g%ld.str=%s\n", id,
                   (LONG)(v != 0 ? (char *)v : ""));
        if (gt_get(g, w, GTIN_Number, &v))
            Printf((CONST_STRPTR)"g%ld.num=%ld\n", id, (LONG)v);
        if (gt_get(g, w, GTCB_Checked, &v))
            Printf((CONST_STRPTR)"g%ld.checked=%ld\n", id, (LONG)(v != 0));
        if (gt_get(g, w, GTCY_Active, &v))
            Printf((CONST_STRPTR)"g%ld.cycle=%ld\n", id, (LONG)v);
        if (gt_get(g, w, GTLV_Selected, &v))
            Printf((CONST_STRPTR)"g%ld.list=%ld\n", id, (LONG)v);
        if (gt_get(g, w, GTTX_Text, &v))
            Printf((CONST_STRPTR)"g%ld.text=%s\n", id,
                   (LONG)(v != 0 ? (char *)v : ""));
        if (gt_get(g, w, GA_Disabled, &v))
            Printf((CONST_STRPTR)"g%ld.disabled=%ld\n", id, (LONG)(v != 0));
    }
}

int main(int argc, char **argv)
{
    struct RDArgs *rda;
    LONG           args[9] = { 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    struct Screen *sc = NULL;
    struct Window *w;
    struct Gadget *g;
    LONG           id, times, i, landed = 0;
    WORD           x, y;
    int            rc = RETURN_FAIL;

    (VOID)argc; (VOID)argv;

    IntuitionBase = (struct IntuitionBase *)OpenLibrary(
        (CONST_STRPTR)"intuition.library", 37);
    if (IntuitionBase == NULL) return RETURN_FAIL;

    GadToolsBase = OpenLibrary((CONST_STRPTR)"gadtools.library", 39);
    KeymapBase = OpenLibrary((CONST_STRPTR)"keymap.library", 37);

    rda = ReadArgs((CONST_STRPTR)"TITLE/A,GADGET/N,TIMES/N,ZIP/S,AT/K/N,X/K/N,Y/K/N,TEXT/K,DUMP/S", args, NULL);
    if (rda == NULL || (args[1] == 0 && args[3] == 0 && args[8] == 0 &&
                        (args[5] == 0 || args[6] == 0)) ||
        (args[8] != 0 && GadToolsBase == NULL) ||
        (args[7] != 0 && KeymapBase == NULL))
    {
        Printf((CONST_STRPTR)"error=bad arguments\n");
        goto out;
    }
    id = args[1] != 0 ? *(LONG *)args[1] : -1;
    times = args[2] != 0 ? *(LONG *)args[2] : 1;
    if (args[7] != 0) times = 1;

    sc = LockPubScreen(NULL);
    if (sc == NULL || !input_open())
    {
        Printf((CONST_STRPTR)"error=no screen or input.device\n");
        goto out;
    }
    w = find_window(sc, (const char *)args[0]);
    if (w == NULL)
    {
        Printf((CONST_STRPTR)"error=no window\n");
        goto out;
    }
    if (args[8] != 0)
    {
        dump(w);
        rc = RETURN_OK;
        goto out;
    }
    /* X and Y: a point of the window, no gadget. */
    if (args[5] != 0 && args[6] != 0)
    {
        x = (WORD)(w->LeftEdge + w->BorderLeft + *(LONG *)args[5]);
        y = (WORD)(w->TopEdge + w->BorderTop + *(LONG *)args[6]);
        goto click;
    }
    /* ZIP: the window's zoom gadget instead of one of its own. */
    for (g = w->FirstGadget; g != NULL; g = g->NextGadget)
        if (args[3] != 0
            ? (g->GadgetType & GTYP_SYSTYPEMASK) == GTYP_WZOOM
            : (g->GadgetID == id && !(g->GadgetType & GTYP_SYSGADGET)))
            break;
    if (g == NULL)
    {
        Printf((CONST_STRPTR)"error=no gadget\n");
        goto out;
    }

    /* GZZ gadgets are relative to the inner window; the zoom gadget sits in
       the border, placed from the right edge. */
    x = (WORD)(w->LeftEdge + g->LeftEdge + g->Width / 2);
    y = (WORD)(w->TopEdge + g->TopEdge +
               (args[4] != 0 ? *(LONG *)args[4] : g->Height / 2));
    if (g->Flags & GFLG_RELRIGHT) x = (WORD)(x + w->Width - 1);
    if ((w->Flags & WFLG_GIMMEZEROZERO) && !(g->GadgetType & GTYP_SYSGADGET))
    {
        x += w->BorderLeft;
        y += w->BorderTop;
    }
click:
    Printf((CONST_STRPTR)"window=%ld,%ld,%ld,%ld\ntarget=%ld,%ld\n",
           (LONG)w->LeftEdge, (LONG)w->TopEdge, (LONG)w->Width,
           (LONG)w->Height, (LONG)x, (LONG)y);

    for (i = 0; i < times; i++)
    {
        if (!point_at(sc, x, y)) continue;
        send_event(IECLASS_RAWMOUSE, IECODE_LBUTTON, IEQUALIFIER_RELATIVEMOUSE,
                   0, 0);
        Delay(2);
        send_event(IECLASS_RAWMOUSE, IECODE_LBUTTON | IECODE_UP_PREFIX,
                   IEQUALIFIER_RELATIVEMOUSE, 0, 0);
        Delay(10);
        landed++;
    }
    Printf((CONST_STRPTR)"clicks=%ld\n", landed);
    rc = landed == times ? RETURN_OK : RETURN_ERROR;
    if (rc == RETURN_OK && args[7] != 0)
    {
        BOOL typed = type_text((const char *)args[7]);

        Printf((CONST_STRPTR)"typed=%ld\n", (LONG)typed);
        if (!typed) rc = RETURN_ERROR;
    }

out:
    input_close();
    if (sc != NULL) UnlockPubScreen(NULL, sc);
    if (rda != NULL) FreeArgs(rda);
    if (KeymapBase != NULL) CloseLibrary(KeymapBase);
    if (GadToolsBase != NULL) CloseLibrary(GadToolsBase);
    CloseLibrary((struct Library *)IntuitionBase);
    return rc;
}

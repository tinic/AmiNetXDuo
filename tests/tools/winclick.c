/*
 * WinClick: click one gadget of a window on the default public screen.
 *
 *     WinClick TITLE/A,GADGET/N/A,TIMES/N
 *
 * TITLE is a prefix of the window title, GADGET the GadgetID.  The pointer is
 * placed with injected IECLASS_POINTERPOS events and corrected against
 * Screen->MouseX/Y, so no display-mode scaling rule is assumed.  TIMES 0
 * only reports the window's box.  Prints key=value; RC 0 when every click
 * landed on the gadget.
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

#include <proto/dos.h>
#include <proto/exec.h>
#include <proto/intuition.h>

#include <string.h>

struct IntuitionBase *IntuitionBase;

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

int main(int argc, char **argv)
{
    struct RDArgs *rda;
    LONG           args[3] = { 0, 0, 0 };
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

    rda = ReadArgs((CONST_STRPTR)"TITLE/A,GADGET/N/A,TIMES/N", args, NULL);
    if (rda == NULL)
    {
        Printf((CONST_STRPTR)"error=bad arguments\n");
        goto out;
    }
    id = *(LONG *)args[1];
    times = args[2] != 0 ? *(LONG *)args[2] : 1;

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
    for (g = w->FirstGadget; g != NULL; g = g->NextGadget)
        if (g->GadgetID == id && !(g->GadgetType & GTYP_SYSGADGET)) break;
    if (g == NULL)
    {
        Printf((CONST_STRPTR)"error=no gadget\n");
        goto out;
    }

    /* GZZ gadgets are relative to the inner window. */
    x = (WORD)(w->LeftEdge + g->LeftEdge + g->Width / 2);
    y = (WORD)(w->TopEdge + g->TopEdge + g->Height / 2);
    if (w->Flags & WFLG_GIMMEZEROZERO)
    {
        x += w->BorderLeft;
        y += w->BorderTop;
    }
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

out:
    input_close();
    if (sc != NULL) UnlockPubScreen(NULL, sc);
    if (rda != NULL) FreeArgs(rda);
    CloseLibrary((struct Library *)IntuitionBase);
    return rc;
}

/*
 * nsclose, close an Intuition window by title.
 *
 *     nsclose [TITLE/K]
 *
 * The close half of the NetSwitch hardware leg: a Shell cannot click a window's
 * close gadget, so this posts an IDCMP_CLOSEWINDOW message the way a click
 * would and waits for the window to answer it.  The reply is answered before
 * the window tears itself down, so the wait is the close completing.
 *
 * SPDX-License-Identifier: MIT
 */

#include "tools.h"

#include <exec/memory.h>
#include <intuition/intuition.h>

#include <proto/exec.h>
#include <proto/intuition.h>

#include <string.h>

const char *const tool_name = "nsclose";

static const char version_tag[] __attribute__((used)) =
    TOOL_VERSTAG("nsclose");

#define TEMPLATE        "TITLE/K"

#define DEFAULT_TITLE   "AmiNetXDuo NetSwitch"

/* FindWindow is not in the NDK's headers, so walk the locked screen's window
   list the same way it would.  The screen lock holds the screen itself; a
   window leaving mid-walk cannot happen here because the target window is
   exactly what is waiting to be closed. */
static struct Window *find_window(struct Screen *sc, const char *title)
{
    struct Window *w;

    for (w = sc->FirstWindow; w != NULL; w = w->NextWindow)
    {
        if (w->Title != NULL &&
            strcmp((const char *)w->Title, title) == 0)
            return w;
    }

    return NULL;
}

int main(int argc, char **argv)
{
    LONG                  args[1];
    struct RDArgs        *rda;
    struct IntuitionBase *IntuitionBase = NULL;
    struct MsgPort       *reply         = NULL;
    struct IntuiMessage  *im            = NULL;
    struct Screen        *sc            = NULL;
    const char           *title         = DEFAULT_TITLE;
    int                   rc            = RETURN_FAIL;

    (VOID)argv;

    if (tool_from_workbench(argc))
        return RETURN_FAIL;

    args[0] = 0;
    rda = ReadArgs((CONST_STRPTR)TEMPLATE, args, NULL);
    if (rda == NULL)
    {
        tool_fault(IoErr());
        tool_usage("[TITLE <name>]", "Close an Intuition window by title.");
        return RETURN_ERROR;
    }

    if (args[0] != 0)
        title = (const char *)args[0];

    IntuitionBase = (struct IntuitionBase *)
        OpenLibrary((CONST_STRPTR)"intuition.library", 39);
    if (IntuitionBase == NULL)
    {
        tool_error("requires Kickstart 3.0 (V39 intuition)");
        goto out;
    }

    /* The default public screen, the one NetSwitch opened on. */
    sc = LockPubScreen(NULL);
    if (sc == NULL)
    {
        tool_error("no public screen");
        goto out;
    }

    if (find_window(sc, title) == NULL)
    {
        tool_error("no window titled \"%s\"", (LONG)title);
        goto out;
    }

    reply = CreateMsgPort();
    if (reply == NULL)
    {
        tool_error("no message port");
        goto out;
    }

    im = (struct IntuiMessage *)
        AllocVec(sizeof(struct IntuiMessage), MEMF_PUBLIC | MEMF_CLEAR);
    if (im == NULL)
    {
        tool_error("no memory for the close message");
        goto out;
    }

    /* The window reads it, answers it, then closes.  The reply port is ours,
       so the answer is how this tool knows the close is done. */
    {
        struct Window *w = find_window(sc, title);

        im->ExecMessage.mn_Node.ln_Type = NT_MESSAGE;
        im->ExecMessage.mn_ReplyPort    = reply;
        im->ExecMessage.mn_Length       = sizeof(struct IntuiMessage);
        im->Class                       = IDCMP_CLOSEWINDOW;
        im->Code                        = 0;
        im->IDCMPWindow                 = w;

        PutMsg(w->UserPort, &im->ExecMessage);
    }

    {
        struct Message *back;

        Wait(1UL << reply->mp_SigBit);
        back = GetMsg(reply);
        if (back == &im->ExecMessage)
            rc = RETURN_OK;
        else
            tool_error("unexpected reply");
    }

out:
    if (im != NULL)    { FreeVec(im); im = NULL; }
    if (reply != NULL) { DeleteMsgPort(reply); reply = NULL; }
    if (sc != NULL)    { UnlockPubScreen(NULL, sc); sc = NULL; }
    if (IntuitionBase != NULL)
    {
        CloseLibrary((struct Library *)IntuitionBase);
        IntuitionBase = NULL;
    }
    FreeArgs(rda);

    tool_printf("RESULT=%s\n", (LONG)((rc == RETURN_OK) ? "OK" : "FAIL"));
    return rc;
}

/* Deliberate failures used ONLY in isolated supervisor negative controls.
 * SPDX-License-Identifier: MIT */
#include <exec/types.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <intuition/intuition.h>
#include <proto/intuition.h>
struct IntuitionBase *IntuitionBase;
int main(void)
{
    LONG args[7] = {0, 0, 0, 0, 0, 0, 0};
    struct RDArgs *ra = ReadArgs((CONST_STRPTR)"BADRC/S,HANG/S,CRASH/S,BREAK/S,WINDOW/S,GADGET/S,KEY/S", args, NULL);
    if (ra == NULL) return 20;
    if (args[0]) { FreeArgs(ra); return 42; }
    if (args[2]) __asm volatile("illegal");
    if (args[1]) for (;;) Delay(5);
    if (args[3]) while (!(SetSignal(0, 0) & SIGBREAKF_CTRL_C)) Delay(2);
    if (args[4] || args[5] || args[6]) {
        struct IntuiText text = {1, 0, JAM1, 0, 0, NULL, (UBYTE *)"Cancel", NULL};
        struct Gadget gadget = {NULL, 10, 15, 100, 20, GFLG_GADGHCOMP,
            GACT_RELVERIFY, GTYP_BOOLGADGET, NULL, NULL, &text, 0, NULL, 1, NULL};
        struct Window *w;
        struct IntuiMessage *msg;
        BOOL done = FALSE;
        IntuitionBase = (struct IntuitionBase *)OpenLibrary((CONST_STRPTR)"intuition.library", 37);
        if (!IntuitionBase) { FreeArgs(ra); return 20; }
        w = OpenWindowTags(NULL, WA_Width, 160, WA_Height, 80,
                          WA_Flags, (args[5] || args[6]) ? WFLG_ACTIVATE : WFLG_CLOSEGADGET | WFLG_ACTIVATE,
                          WA_Gadgets, args[5] ? (ULONG)&gadget : 0,
                          WA_IDCMP, args[6] ? IDCMP_VANILLAKEY : args[5] ? IDCMP_GADGETUP : IDCMP_CLOSEWINDOW, TAG_DONE);
        if (!w) { CloseLibrary((struct Library *)IntuitionBase); FreeArgs(ra); return 20; }
        while (!done) {
            WaitPort(w->UserPort);
            while ((msg = (struct IntuiMessage *)GetMsg(w->UserPort)) != NULL) {
                if (msg->Class == IDCMP_CLOSEWINDOW ||
                    (msg->Class == IDCMP_VANILLAKEY && msg->Code == 27) ||
                    (msg->Class == IDCMP_GADGETUP && msg->IAddress == (APTR)&gadget)) done = TRUE;
                ReplyMsg((struct Message *)msg);
            }
        }
        CloseWindow(w);
        CloseLibrary((struct Library *)IntuitionBase);
    }
    FreeArgs(ra);
    return 0;
}

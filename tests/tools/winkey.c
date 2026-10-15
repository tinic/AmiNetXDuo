/*
 * WinKey: press a key in the active window on the default public screen.
 *
 *     WinKey KEY/A,TIMES/N,QUAL/K/N
 *
 * KEY is a name from the rawkey table the web shell's keyboard sends
 * (src/tools/web/client/console/rawkey.ts, the standard US positions of the
 * Amiga hardware reference appendix), or its number in decimal or 0x hex:
 * SPACE 0x40, TAB 0x42, RETURN 0x44, ESC 0x45, the cursor keys 0x4c-0x4f.
 * What goes down the wire is IECLASS_RAWKEY and that code, so the window reads
 * the key the keyboard controller would have produced -- the keymap and the
 * qualifiers are the machine's business, not this tool's.  WinClick is the
 * other half of the same idea for the mouse.
 *
 * QUAL sets the IEQUALIFIER_* bits the events carry (0x0001 left shift, 0x0002
 * right shift, 0x0008 control, 0x0010 left alt ...).  A chord's modifier
 * presses are not synthesized: the keys this drives are read from the event's
 * code and its qualifiers.
 *
 * TIMES presses it that many times.  The key goes to whichever window is
 * active, so a sequence clicks the window it means first.  Prints key=value;
 * RC 0 when every press was written.
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

#include <stdlib.h>
#include <string.h>

struct IntuitionBase *IntuitionBase;

static struct MsgPort   *in_port;
static struct IOStdReq  *in_req;
static BOOL              in_open;
static struct InputEvent ev;

/* The printable keys, named as rawkey.ts names them, plus the editing and
   cursor keys a window's gadgets are driven with.  The gaps in the numbering
   are gaps on the machine too. */
static const struct RawName
{
    const char *name;
    UWORD       code;
} raw_names[] =
{
    { "0", 0x0a }, { "1", 0x01 }, { "2", 0x02 }, { "3", 0x03 }, { "4", 0x04 },
    { "5", 0x05 }, { "6", 0x06 }, { "7", 0x07 }, { "8", 0x08 }, { "9", 0x09 },
    { "A", 0x20 }, { "B", 0x35 }, { "C", 0x33 }, { "D", 0x22 }, { "E", 0x12 },
    { "F", 0x23 }, { "G", 0x24 }, { "H", 0x25 }, { "I", 0x17 }, { "J", 0x26 },
    { "K", 0x27 }, { "L", 0x28 }, { "M", 0x37 }, { "N", 0x36 }, { "O", 0x18 },
    { "P", 0x19 }, { "Q", 0x10 }, { "R", 0x13 }, { "S", 0x21 }, { "T", 0x14 },
    { "U", 0x16 }, { "V", 0x34 }, { "W", 0x11 }, { "X", 0x32 }, { "Y", 0x15 },
    { "Z", 0x31 },
    { "SPACE", 0x40 }, { "BACKSPACE", 0x41 }, { "TAB", 0x42 },
    { "ENTER", 0x43 },          /* the keypad's, beside RETURN's */
    { "RETURN", 0x44 }, { "ESC", 0x45 }, { "DELETE", 0x46 },
    { "UP", 0x4c }, { "DOWN", 0x4d }, { "RIGHT", 0x4e }, { "LEFT", 0x4f },
    { "HELP", 0x5f }
};

static UWORD raw_code(const char *name)
{
    const char *p = name;
    size_t      i;

    for (i = 0; i < sizeof(raw_names) / sizeof(raw_names[0]); i++)
    {
        const char *n = raw_names[i].name;

        p = name;
        while (*n != '\0' && *p != '\0')
        {
            char c = *p;
            if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
            if (c != *n) break;
            p++;
            n++;
        }
        if (*n == '\0' && *p == '\0') return raw_names[i].code;
    }
    /* A number, decimal or 0x hex, for anything the table has no name for. */
    if (name[0] >= '0' && name[0] <= '9')
    {
        long v = strtol(name, NULL, 0);
        if (v >= 0 && v <= 0x7f) return (UWORD)v;
    }
    return 0xffff;
}

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

/* The active window's title, so the log says where the key went. */
static const char *active_title(struct Screen *sc)
{
    struct Window *w;

    for (w = sc->FirstWindow; w != NULL; w = w->NextWindow)
        if ((w->Flags & WFLG_WINDOWACTIVE) != 0 && w->Title != NULL)
            return (const char *)w->Title;
    return "(none)";
}

int main(int argc, char **argv)
{
    struct RDArgs *rda;
    LONG           args[3] = { 0, 0, 0 };
    struct Screen *sc = NULL;
    UWORD          code;
    LONG           times, qual, i, sent = 0;
    int            rc = RETURN_FAIL;

    (VOID)argc; (VOID)argv;

    IntuitionBase = (struct IntuitionBase *)OpenLibrary(
        (CONST_STRPTR)"intuition.library", 37);
    if (IntuitionBase == NULL) return RETURN_FAIL;

    rda = ReadArgs((CONST_STRPTR)"KEY/A,TIMES/N,QUAL/K/N", args, NULL);
    if (rda == NULL)
    {
        Printf((CONST_STRPTR)"error=bad arguments\n");
        goto out;
    }

    code = raw_code((const char *)args[0]);
    if (code == 0xffff)
    {
        Printf((CONST_STRPTR)"error=no such key\n");
        goto out;
    }
    times = args[1] != 0 ? *(LONG *)args[1] : 1;
    qual  = args[2] != 0 ? *(LONG *)args[2] : 0;

    sc = LockPubScreen(NULL);
    if (sc == NULL || !input_open())
    {
        Printf((CONST_STRPTR)"error=no screen or input.device\n");
        goto out;
    }

    Printf((CONST_STRPTR)"key=%s code=%ld qual=%ld active=%s\n",
           (STRPTR)args[0], (LONG)code, qual, (STRPTR)active_title(sc));

    for (i = 0; i < times; i++)
    {
        /* Down then up, the pair a real keyboard sends. */
        send_event(IECLASS_RAWKEY, code, (UWORD)qual, 0, 0);
        Delay(2);
        send_event(IECLASS_RAWKEY, (UWORD)(code | IECODE_UP_PREFIX),
                   (UWORD)qual, 0, 0);
        Delay(10);
        sent++;
    }
    Printf((CONST_STRPTR)"presses=%ld\n", sent);
    rc = sent == times ? RETURN_OK : RETURN_ERROR;

out:
    input_close();
    if (sc != NULL) UnlockPubScreen(NULL, sc);
    if (rda != NULL) FreeArgs(rda);
    CloseLibrary((struct Library *)IntuitionBase);
    return rc;
}

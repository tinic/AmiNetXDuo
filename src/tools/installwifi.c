/*
 * InstallWiFi, the Installer's Wi-Fi page: the network name and passphrase
 * for WirelessManager, written as a wpa_supplicant network block.
 *
 * A program rather than two askstring pages because Installer writes every
 * askstring answer into its log file, and the passphrase must not reach a
 * file the user did not ask for.  The passphrase gadget shows one '*' per
 * character; the text lives only in this program until it is written to TO.
 * Like InstallNetProbe it ships beside the Installer, not in C:.
 *
 *   InstallWiFi TO <file>
 *   InstallWiFi PRIORITY <interface file>
 *
 * TO: RETURN_OK when the file was written, RETURN_WARN when the user
 * skipped, RETURN_FAIL when there was no window to ask in or the file could
 * not be written.
 *
 * PRIORITY: the kept Ethernet definition of a reinstall where the Wi-Fi now
 * starts beside it.  A line PRIORITY=10 is appended unless the file has a
 * live PRIORITY or PRI pair, found as the interface parser finds it
 * (ami_cfg_key_count); the Installer cannot tell a commented line from a
 * live one (Search matches anywhere in a line).
 * RETURN_OK when set or already set, RETURN_FAIL when the file could not be
 * read or written.
 *
 * Any form: RETURN_ERROR when the arguments do not parse (ReadArgs).
 *
 * SPDX-License-Identifier: MIT
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/rdargs.h>
#include <graphics/text.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <intuition/sghooks.h>
#include <libraries/gadtools.h>
#include <utility/hooks.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <proto/gadtools.h>
#include <proto/graphics.h>
#include <proto/intuition.h>

#include <string.h>

#include "aminetxduo/version.h"
#include "netwireless_prefs.h"
#include "aminetxduo/config.h"

const char *const tool_name = "InstallWiFi";

static const char version_tag[] __attribute__((used)) =
    TOOL_VERSTAG("InstallWiFi");

struct IntuitionBase *IntuitionBase;
struct GfxBase       *GfxBase;
struct Library       *GadToolsBase;

/* wpa_supplicant's limits: an SSID is 1 to 32 bytes, a WPA passphrase 8 to
   63 characters from 32 to 126.  An empty passphrase is an open network. */
#define SSID_MAX   32
#define PASS_MIN   8
#define PASS_MAX   63

enum { GID_SSID = 1, GID_PASS, GID_SAVE, GID_SKIP, GID_STATUS };

/* What the passphrase gadget hides.  Reached from the hook through h_Data,
   never as a global: the hook runs on Intuition's input task. */
struct Secret
{
    char text[PASS_MAX + 1];
    LONG len;
};

static VOID mask_clear(struct SGWork *sgw, struct Secret *s)
{
    s->len = 0;
    s->text[0] = '\0';
    sgw->WorkBuffer[0] = '\0';
    sgw->NumChars = 0;
    sgw->BufferPos = 0;
    sgw->Actions |= SGA_REDISPLAY;
}

/*
 * The gadget is append-and-backspace only, which keeps the hidden copy in
 * step without guessing: a character typed at the end is taken and shown as
 * '*', a backspace at the end drops the last one, the cursor stays at the
 * end, and any other edit (Amiga-X, a paste, a delete in the middle) clears
 * both, so what is saved is always what was typed.
 */
static ULONG mask_hook(struct Hook *hook __asm("a0"),
                       struct SGWork *sgw __asm("a2"),
                       ULONG *msg __asm("a1"))
{
    struct Secret *s = (struct Secret *)hook->h_Data;

    if (*msg != SGH_KEY)
        return 0;

    switch (sgw->EditOp)
    {
    case EO_INSERTCHAR:
    case EO_REPLACECHAR:
        if (sgw->NumChars == s->len + 1 && sgw->BufferPos == sgw->NumChars &&
            s->len < PASS_MAX)
        {
            s->text[s->len++] = sgw->WorkBuffer[sgw->BufferPos - 1];
            s->text[s->len] = '\0';
            sgw->WorkBuffer[sgw->BufferPos - 1] = '*';
        }
        else
            mask_clear(sgw, s);
        break;

    case EO_DELBACKWARD:
        if (sgw->NumChars == s->len - 1 && sgw->BufferPos == sgw->NumChars)
            s->text[--s->len] = '\0';
        else
            mask_clear(sgw, s);
        break;

    case EO_MOVECURSOR:
        sgw->BufferPos = sgw->NumChars;
        break;

    case EO_NOOP:
    case EO_ENTER:
        break;

    default:
        if (sgw->NumChars != s->len)
            mask_clear(sgw, s);
        break;
    }
    return ~0UL;
}

/* A Workbench string gadget holds ISO-8859-1; wpa_supplicant compares the
   SSID's bytes with the access point's, which are UTF-8.  Returns the UTF-8
   length, or -1 when it is over cap. */
static LONG latin1_to_utf8(const char *in, unsigned char *out, LONG cap)
{
    const unsigned char *p;
    LONG                 n = 0;

    for (p = (const unsigned char *)in; *p != '\0'; p++)
    {
        if (*p < 0x80)
        {
            if (n + 1 > cap) return -1;
            out[n++] = *p;
        }
        else
        {
            if (n + 2 > cap) return -1;
            out[n++] = (unsigned char)(0xc0 | (*p >> 6));
            out[n++] = (unsigned char)(0x80 | (*p & 0x3f));
        }
    }
    return n;
}

/*
 * wpa_supplicant takes a quoted value up to the LAST quote on its line and
 * keeps a '#' between the quotes, so any printable ASCII is written as it
 * is.  Anything else in the (UTF-8) SSID is written as hex, which it takes
 * unquoted: nw_prefs_ssid_value(), as NetWirelessPrefs writes it.
 */
static BOOL write_prefs(const char *to, const char *ssid, const char *pass)
{
    BPTR          fh = Open((CONST_STRPTR)to, MODE_NEWFILE);
    unsigned char utf8[NW_SSID_MAX];
    char          value[2 * NW_SSID_MAX + 3];
    LONG          n = latin1_to_utf8(ssid, utf8, NW_SSID_MAX);
    BOOL          ok;

    if (fh == 0)
        return FALSE;
    if (n < 0)
        n = 0;
    nw_prefs_ssid_value(utf8, (int)n, value);

    ok = FPuts(fh, (CONST_STRPTR)
        "# ENVARC:Sys/Wireless.prefs -- written by the AmiNetXDuo installer.\n"
        "#\n"
        "# WirelessManager reads ENV:Sys/Wireless.prefs, which AmigaOS\n"
        "# copies from here at every boot.  The format is wpa_supplicant's;\n"
        "# one network={ } block per network.  An open network takes\n"
        "# key_mgmt=NONE in place of psk.\n"
        "\n"
        "network={\n") == 0;

    ok = ok && FPuts(fh, (CONST_STRPTR)"\tssid=") == 0 &&
         FPuts(fh, (CONST_STRPTR)value) == 0 &&
         FPuts(fh, (CONST_STRPTR)"\n") == 0;

    if (pass[0] != '\0')
        ok = ok && FPuts(fh, (CONST_STRPTR)"\tpsk=\"") == 0 &&
             FPuts(fh, (CONST_STRPTR)pass) == 0 &&
             FPuts(fh, (CONST_STRPTR)"\"\n") == 0;
    else
        ok = ok && FPuts(fh, (CONST_STRPTR)"\tkey_mgmt=NONE\n") == 0;

    ok = ok && FPuts(fh, (CONST_STRPTR)"}\n") == 0;
    if (!Close(fh))
        ok = FALSE;
    if (!ok)
        DeleteFile((CONST_STRPTR)to);
    return ok;
}

/* Why the answers cannot be saved, or NULL when they can.  *in_pass says
   which field the reason is about. */
static const char *refusal(const char *ssid, const struct Secret *s,
                           BOOL *in_pass)
{
    LONG i;

    *in_pass = FALSE;
    if (ssid[0] == '\0')
        return "Type the network name, or Skip.";
    {
        unsigned char utf8[NW_SSID_MAX];

        if (latin1_to_utf8(ssid, utf8, NW_SSID_MAX) < 0)
            return "A network name has at most 32 bytes in UTF-8.";
    }
    *in_pass = TRUE;
    if (s->len != 0 && (s->len < PASS_MIN || s->len > PASS_MAX))
        return "A passphrase has 8 to 63 characters, or none.";
    for (i = 0; i < s->len; i++)
        if ((unsigned char)s->text[i] < 0x20 ||
            (unsigned char)s->text[i] > 0x7e)
            return "A passphrase takes only ASCII characters.";
    return NULL;
}

static struct Gadget *add(struct Gadget *prev, APTR vi,
                          struct TextAttr *ta, ULONG kind, UWORD id,
                          WORD x, WORD y, WORD w, WORD h, const char *label,
                          ULONG flags, struct TagItem *tags)
{
    struct NewGadget ng;

    memset(&ng, 0, sizeof(ng));
    ng.ng_LeftEdge   = x;
    ng.ng_TopEdge    = y;
    ng.ng_Width      = w;
    ng.ng_Height     = h;
    ng.ng_GadgetText = (UBYTE *)label;
    ng.ng_TextAttr   = ta;
    ng.ng_GadgetID   = id;
    ng.ng_Flags      = flags;
    ng.ng_VisualInfo = vi;
    return CreateGadgetA(kind, prev, &ng, tags);
}

static LONG ask(const char *to)
{
    static struct Secret secret;
    static struct Hook   hook;
    struct Screen       *scr;
    APTR                 vi = NULL;
    struct Gadget       *glist = NULL, *g, *g_ssid, *g_pass, *g_status;
    struct Window       *win = NULL;
    struct TextAttr     *ta;
    struct TagItem       tags[4];
    WORD                 fh, lw, gw, bw, x0, y, top;
    LONG                 result = RETURN_FAIL;
    BOOL                 done = FALSE;

    hook.h_Entry = (HOOKFUNC)(void (*)(void))mask_hook;
    hook.h_Data  = &secret;

    scr = LockPubScreen(NULL);
    if (scr == NULL)
        return RETURN_FAIL;
    ta  = scr->Font;
    fh  = (WORD)ta->ta_YSize;
    lw  = (WORD)TextLength(&scr->RastPort, (STRPTR)"Passphrase ", 11);
    gw  = (WORD)(TextLength(&scr->RastPort, (STRPTR)"M", 1) * 34);
    bw  = (WORD)TextLength(&scr->RastPort, (STRPTR)"  Save  ", 8);
    x0  = (WORD)(scr->WBorLeft + 8 + lw);
    top = (WORD)(scr->WBorTop + fh + 1 + 6);

    vi = GetVisualInfoA(scr, NULL);
    g = (vi != NULL) ? CreateContext(&glist) : NULL;

    y = top;
    tags[0].ti_Tag = GTTX_Text;
    tags[0].ti_Data = (ULONG)"WirelessManager joins this network at boot.";
    tags[1].ti_Tag = TAG_DONE;
    g = g ? add(g, vi, ta, TEXT_KIND, 0, (WORD)(scr->WBorLeft + 8), y,
                (WORD)(lw + gw), (WORD)(fh + 4), NULL, 0, tags) : NULL;

    y = (WORD)(y + fh + 10);
    tags[0].ti_Tag = GTST_MaxChars; tags[0].ti_Data = SSID_MAX;
    tags[1].ti_Tag = TAG_DONE;
    g_ssid = g = g ? add(g, vi, ta, STRING_KIND, GID_SSID, x0, y, gw,
                         (WORD)(fh + 6), "_Network", PLACETEXT_LEFT,
                         tags) : NULL;

    y = (WORD)(y + fh + 10);
    tags[0].ti_Tag = GTST_MaxChars; tags[0].ti_Data = PASS_MAX;
    tags[1].ti_Tag = GTST_EditHook; tags[1].ti_Data = (ULONG)&hook;
    tags[2].ti_Tag = TAG_DONE;
    g_pass = g = g ? add(g, vi, ta, STRING_KIND, GID_PASS, x0, y, gw,
                         (WORD)(fh + 6), "_Passphrase", PLACETEXT_LEFT,
                         tags) : NULL;

    y = (WORD)(y + fh + 10);
    tags[0].ti_Tag = GTTX_Text; tags[0].ti_Data = (ULONG)"";
    tags[1].ti_Tag = TAG_DONE;
    g_status = g = g ? add(g, vi, ta, TEXT_KIND, GID_STATUS,
                           (WORD)(scr->WBorLeft + 8), y, (WORD)(lw + gw),
                           (WORD)(fh + 4), NULL, 0, tags) : NULL;

    y = (WORD)(y + fh + 10);
    tags[0].ti_Tag = TAG_DONE;
    g = g ? add(g, vi, ta, BUTTON_KIND, GID_SAVE, (WORD)(scr->WBorLeft + 8), y,
                bw, (WORD)(fh + 6), "_Save", PLACETEXT_IN, tags) : NULL;
    g = g ? add(g, vi, ta, BUTTON_KIND, GID_SKIP,
                (WORD)(scr->WBorLeft + 8 + lw + gw - bw), y, bw,
                (WORD)(fh + 6), "S_kip", PLACETEXT_IN, tags) : NULL;

    if (g != NULL)
    {
        struct TagItem wt[] = {
            { WA_Title,       (ULONG)"AmiNetXDuo Wi-Fi network" },
            { WA_PubScreen,   (ULONG)scr },
            { WA_Gadgets,     (ULONG)glist },
            { WA_InnerWidth,  (ULONG)(lw + gw + 16) },
            { WA_InnerHeight, (ULONG)(y + fh + 12 - scr->WBorTop - fh - 1) },
            { WA_IDCMP,       BUTTONIDCMP | STRINGIDCMP | IDCMP_CLOSEWINDOW |
                              IDCMP_REFRESHWINDOW | IDCMP_VANILLAKEY },
            { WA_Flags,       WFLG_DRAGBAR | WFLG_DEPTHGADGET |
                              WFLG_CLOSEGADGET | WFLG_ACTIVATE |
                              WFLG_SMART_REFRESH },
            { WA_AutoAdjust,  TRUE },
            { TAG_DONE,       0 }
        };
        win = OpenWindowTagList(NULL, wt);
    }
    UnlockPubScreen(NULL, scr);

    if (win == NULL)
        goto out;
    GT_RefreshWindow(win, NULL);
    ActivateGadget(g_ssid, win, NULL);

    while (!done)
    {
        struct IntuiMessage *m;

        WaitPort(win->UserPort);
        while (!done && (m = GT_GetIMsg(win->UserPort)) != NULL)
        {
            ULONG          cls = m->Class;
            UWORD          code = m->Code;
            struct Gadget *gad = (struct Gadget *)m->IAddress;
            BOOL           save = FALSE;

            GT_ReplyIMsg(m);
            switch (cls)
            {
            case IDCMP_CLOSEWINDOW:
                result = RETURN_WARN;
                done = TRUE;
                break;
            case IDCMP_REFRESHWINDOW:
                GT_BeginRefresh(win);
                GT_EndRefresh(win, TRUE);
                break;
            case IDCMP_VANILLAKEY:
                if (code == 's' || code == 'S')
                    save = TRUE;
                else if (code == 'k' || code == 'K' || code == 0x1b)
                {
                    result = RETURN_WARN;
                    done = TRUE;
                }
                else if (code == 'n' || code == 'N')
                    ActivateGadget(g_ssid, win, NULL);
                else if (code == 'p' || code == 'P')
                    ActivateGadget(g_pass, win, NULL);
                break;
            case IDCMP_GADGETUP:
                if (gad->GadgetID == GID_SSID)
                    ActivateGadget(g_pass, win, NULL);
                else if (gad->GadgetID == GID_PASS && code != 0x09)
                    save = TRUE;
                else if (gad->GadgetID == GID_SAVE)
                    save = TRUE;
                else if (gad->GadgetID == GID_SKIP)
                {
                    result = RETURN_WARN;
                    done = TRUE;
                }
                break;
            default:
                break;
            }

            if (save)
            {
                const char *ssid =
                    (const char *)((struct StringInfo *)g_ssid->SpecialInfo)->Buffer;
                BOOL        in_pass;
                const char *why = refusal(ssid, &secret, &in_pass);

                if (why != NULL)
                {
                    struct TagItem st[] = { { GTTX_Text, (ULONG)why },
                                            { TAG_DONE, 0 } };
                    GT_SetGadgetAttrsA(g_status, win, NULL, st);
                    ActivateGadget(in_pass ? g_pass : g_ssid, win, NULL);
                }
                else
                {
                    result = write_prefs(to, ssid, secret.text) ?
                             RETURN_OK : RETURN_FAIL;
                    done = TRUE;
                }
            }
        }
    }

out:
    memset(&secret, 0, sizeof(secret));
    if (win != NULL)
        CloseWindow(win);
    FreeGadgets(glist);
    if (vi != NULL)
        FreeVisualInfo(vi);
    return result;
}

/* Appended only when the interface parser would find no PRIORITY or PRI
   pair at all: ami_cfg_key_count() lexes the file as it does, comments and
   several pairs to a line included.  With any, valid or not, the file says
   what it wants and is left alone. */
static LONG set_priority(const char *path)
{
    static char text[16384];
    static char scan[16384];
    static const char line[] = "PRIORITY=10\n";
    BPTR        fh = Open((CONST_STRPTR)path, MODE_OLDFILE);
    LONG        len;
    BOOL        ok;

    if (fh == 0)
        return RETURN_FAIL;
    len = Read(fh, text, sizeof(text) - 1);
    Close(fh);
    if (len < 0 || len == (LONG)sizeof(text) - 1)
        return RETURN_FAIL;
    memcpy(scan, text, (size_t)len);
    scan[len] = '\0';
    if (ami_cfg_key_count(scan, "PRIORITY", "PRI") != 0)
        return RETURN_OK;

    fh = Open((CONST_STRPTR)path, MODE_READWRITE);
    if (fh == 0)
        return RETURN_FAIL;
    ok = Seek(fh, 0, OFFSET_END) >= 0;
    if (ok && len > 0 && text[len - 1] != '\n')
        ok = Write(fh, (APTR)"\n", 1) == 1;
    ok = ok && Write(fh, (APTR)line, sizeof(line) - 1) == (LONG)(sizeof(line) - 1);
    if (!Close(fh))
        ok = FALSE;
    return ok ? RETURN_OK : RETURN_FAIL;
}

int main(int argc, char **argv)
{
    LONG           args[2] = { 0, 0 };
    struct RDArgs *rda;
    LONG           rc = RETURN_FAIL;

    (void)argc;
    (void)argv;

    rda = ReadArgs((CONST_STRPTR)"TO/K,PRIORITY/K", args, NULL);
    if (rda == NULL)
        return RETURN_ERROR;
    if ((args[0] != 0) + (args[1] != 0) != 1)
    {
        FreeArgs(rda);
        return RETURN_ERROR;
    }
    if (args[1] != 0)
    {
        rc = set_priority((const char *)args[1]);
        FreeArgs(rda);
        return (int)rc;
    }

    IntuitionBase = (struct IntuitionBase *)OpenLibrary("intuition.library", 37);
    GfxBase = (struct GfxBase *)OpenLibrary("graphics.library", 37);
    GadToolsBase = OpenLibrary("gadtools.library", 37);
    if (IntuitionBase != NULL && GfxBase != NULL && GadToolsBase != NULL)
        rc = ask((const char *)args[0]);

    if (GadToolsBase != NULL) CloseLibrary(GadToolsBase);
    if (GfxBase != NULL) CloseLibrary((struct Library *)GfxBase);
    if (IntuitionBase != NULL) CloseLibrary((struct Library *)IntuitionBase);
    FreeArgs(rda);
    return (int)rc;
}

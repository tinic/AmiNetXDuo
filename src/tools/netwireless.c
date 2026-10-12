/*
 * NetWireless, the Wi-Fi commodity: the link's state and signal as an
 * AppIcon, a Tools menu item and an environment variable, and the hotkey
 * that opens NetWirelessPrefs.  Status only; scanning, joining and the
 * window are NetWirelessPrefs', run on demand, so none of that stays
 * resident.
 *
 * Cost while running: one S2_ONEVENT request waits for CONNECT, DISCONNECT
 * and OFFLINE (or ONLINE); the signal is read every INTERVAL seconds only
 * while connected.  Not connected, nothing is polled.
 *
 * Kickstart 2.04: commodities.library, workbench.library and icon.library
 * are each optional.  On Workbench 3.9 and later the AppIcon is redrawn in
 * place through WBAPPICONA_RenderHook; before that it is removed and added
 * again, which costs a position the user dragged it to.
 *
 * SPDX-License-Identifier: MIT
 */

#include "tools.h"
#include "netwireless_dev.h"
#include "netwireless_sig.h"
#include "aminetxduo/version.h"

#include <exec/memory.h>
#include <devices/timer.h>
#include <dos/dostags.h>
#include <dos/var.h>
#include <intuition/intuition.h>
#include <libraries/commodities.h>
#include <utility/hooks.h>
#include <workbench/icon.h>
#include <workbench/startup.h>
#include <workbench/workbench.h>
#include <proto/commodities.h>
#include <proto/icon.h>
#include <proto/intuition.h>
#include <proto/wb.h>

const char *const tool_name = "NetWireless";

static const char version_tag[] __attribute__((used)) =
    TOOL_VERSTAG("NetWireless");

struct Library       *CxBase;
struct Library       *WorkbenchBase;
struct Library       *IconBase;
struct IntuitionBase *IntuitionBase;

#define TEMPLATE "CX_PRIORITY/K/N,CX_POPKEY/K,CX_POPUP/K,DEVICE/K,UNIT/K/N," \
                 "INTERFACE/K,INTERVAL/K/N,APPICON/K,APPMENU/K,ENVVAR/K,QUIET/S"

enum
{
    ARG_PRI, ARG_POPKEY, ARG_POPUP, ARG_DEVICE, ARG_UNIT, ARG_INTERFACE,
    ARG_INTERVAL, ARG_APPICON, ARG_APPMENU, ARG_ENVVAR, ARG_QUIET, ARG_COUNT
};


#define NW_ICON_W   32
#define NW_ICON_H   16
#define NW_PLANE    (((NW_ICON_W + 15) / 16) * 2 * NW_ICON_H)
#define NW_HOTKEY   1

static struct
{
    NwDev               dev;
    BOOL                dev_open;
    char                device[128];
    ULONG               unit;
    ULONG               interval;
    const char         *envvar;

    LONG                link;
    LONG                bars;
    LONG                icon;               /* NW_ICON_*                    */
    char                label[NW_SSID_MAX + 1];
    char                env[NW_SSID_MAX + 8];
    BOOL                events_broken;

    struct MsgPort     *cxport, *appport, *tport;
    CxObj              *broker;
    struct timerequest *treq;
    BOOL                tarmed;

    struct AppIcon     *appicon;
    struct AppMenuItem *appmenu;
    BOOL                want_icon;
    UBYTE              *planes;             /* chip memory                  */
    struct Hook         render;
} nw;

/* The AppIcon: a two-plane image whose planes are filled in chip memory. */
static struct Image nw_img = { 0, 0, NW_ICON_W, NW_ICON_H, 2, NULL, 3, 0, NULL };

static struct DiskObject nw_dob =
{
    WB_DISKMAGIC, WB_DISKVERSION,
    { NULL, 0, 0, NW_ICON_W, NW_ICON_H, GFLG_GADGIMAGE | GFLG_GADGHCOMP,
      GACT_RELVERIFY | GACT_IMMEDIATE, GTYP_BOOLGADGET, &nw_img, NULL, NULL,
      0, NULL, 0, NULL },
    WBTOOL, NULL, NULL, NO_ICON_POSITION, NO_ICON_POSITION, NULL, NULL, 0
};

static ULONG nw_len(const char *s)
{
    ULONG n = 0;
    while (s[n] != '\0') n++;
    return n;
}

/* ---------------------------------------------------------- options -- */

static LONG               nw_args[ARG_COUNT];
static struct DiskObject *nw_tool;      /* the program's icon, from Workbench */

/* Option k: the ToolType of the template's k-th keyword when started from
   Workbench, the argument from a Shell. */
static const char *nw_opt(int k)
{
    const char *t = TEMPLATE;
    char        key[16];
    int         n = 0;

    if (nw_tool == NULL) return (const char *)nw_args[k];
    if (nw_tool->do_ToolTypes == NULL) return NULL;
    while (k > 0) if (*t++ == ',') k--;
    while (*t != '/' && n < (int)sizeof(key) - 1) key[n++] = *t++;
    key[n] = '\0';
    return (const char *)FindToolType((CONST_STRPTR *)nw_tool->do_ToolTypes,
                                      (CONST_STRPTR)key);
}

static LONG nw_opt_number(int k, LONG fallback)
{
    LONG n;

    if (nw_tool == NULL)
        return nw_args[k] != 0 ? *(LONG *)nw_args[k] : fallback;
    return (nw_opt(k) != NULL && StrToLong((CONST_STRPTR)nw_opt(k), &n) > 0)
           ? n : fallback;
}

static BOOL nw_opt_yes(int k, BOOL fallback)
{
    const char *v = nw_opt(k);

    if (v == NULL) return fallback;
    return !(v[0] == 'N' || v[0] == 'n' || v[0] == 'F' || v[0] == 'f' || v[0] == '0');
}

/* ------------------------------------------------------------- show -- */

/* NetWirelessPrefs finds the unit the way this program did.  A second one
   brings the first one's window forward. */
static VOID nw_launch(VOID)
{
    BPTR in = Open((CONST_STRPTR)"NIL:", MODE_OLDFILE);
    BPTR out = Open((CONST_STRPTR)"NIL:", MODE_NEWFILE);

    if (in == (BPTR)0 || out == (BPTR)0 ||
        SystemTags((CONST_STRPTR)"NetWirelessPrefs", SYS_Input, (ULONG)in,
                   SYS_Output, (ULONG)out, SYS_Asynch, TRUE, TAG_DONE) == -1)
    {
        if (in != (BPTR)0) Close(in);
        if (out != (BPTR)0) Close(out);
    }
}

/* The picture, drawn by Workbench 3.9 and later from the current planes. */
static ULONG nw_render(register struct Hook *h __asm("a0"),
                       register APTR reserved __asm("a2"),
                       register struct AppIconRenderMsg *arm __asm("a1"))
{
    (VOID)h;
    (VOID)reserved;
    DrawImage(arm->arm_RastPort, &nw_img, arm->arm_Left, arm->arm_Top);
    return FALSE;
}

static VOID nw_icon_remove(VOID)
{
    if (nw.appicon != NULL) RemoveAppIcon(nw.appicon);
    nw.appicon = NULL;
}

static VOID nw_icon_add(VOID)
{
    struct TagItem t[2];

    t[0].ti_Tag = TAG_DONE;
    if (WorkbenchBase->lib_Version >= 44)
    {
        nw.render.h_Entry = (ULONG (*)())(APTR)nw_render;
        t[0].ti_Tag = WBAPPICONA_RenderHook;
        t[0].ti_Data = (ULONG)&nw.render;
        t[1].ti_Tag = TAG_DONE;
    }
    nw.appicon = AddAppIconA(0, 0, (STRPTR)nw.label, nw.appport, (BPTR)0,
                             &nw_dob, t);
}

/* The AppIcon and the variable after a state change: the picture redrawn
   in place where Workbench can, re-added where it cannot or the label
   changed. */
static VOID nw_show(BOOL label_changed)
{
    char  env[sizeof(nw.env)];
    int   n;

    if (nw.planes != NULL)
        nw_icon_render((int)nw.icon, (int)nw.bars, NW_ICON_W, NW_ICON_H, nw.planes);
    if (nw.appicon != NULL && !label_changed && WorkbenchBase->lib_Version >= 44)
        (VOID)WorkbenchControl(NULL, WBCTRLA_RedrawAppIcon, (ULONG)nw.appicon,
                               TAG_DONE);
    else if (nw.want_icon && nw.planes != NULL)
    {
        nw_icon_remove();
        nw_icon_add();
    }

    if (nw.envvar == NULL || nw.envvar[0] == '\0') return;
    switch (nw.icon)
    {
        case NW_ICON_CONNECTED:
            tool_copy_string(env, sizeof(env), nw.label);
            n = (int)nw_len(env);
            env[n++] = ' ';
            env[n++] = (char)('0' + nw.bars);
            env[n++] = '/';
            env[n++] = '4';
            env[n] = '\0';
            break;
        case NW_ICON_NODEV:   tool_copy_string(env, sizeof(env), "No Wi-Fi unit"); break;
        case NW_ICON_JOINING: tool_copy_string(env, sizeof(env), "Not connected"); break;
        case NW_ICON_ERROR:   tool_copy_string(env, sizeof(env), "No answer"); break;
        default:              tool_copy_string(env, sizeof(env), "Off"); break;
    }
    for (n = 0; env[n] == nw.env[n] && env[n] != '\0'; n++)
        ;
    if (env[n] == nw.env[n]) return;            /* unchanged: nothing written */
    tool_copy_string(nw.env, sizeof(nw.env), env);
    (VOID)SetVar((CONST_STRPTR)nw.envvar, (CONST_STRPTR)nw.env, -1,
                 GVF_GLOBAL_ONLY);
}

/* ------------------------------------------------------------ state -- */

static VOID nw_timer_arm(VOID)
{
    if (nw.treq == NULL || nw.tarmed) return;
    nw.treq->tr_node.io_Command = TR_ADDREQUEST;
    nw.treq->tr_time.tv_secs = nw.interval;
    nw.treq->tr_time.tv_micro = 0;
    SendIO((struct IORequest *)nw.treq);
    nw.tarmed = TRUE;
}

/* The link read again: after an event, at start, and once an INTERVAL
   while connected. */
static VOID nw_refresh(VOID)
{
    LONG  signal = 0, noise = 0, len = -1, icon, bars = 0;
    UBYTE ssid[NW_SSID_MAX], bssid[6];
    char  label[NW_SSID_MAX + 1];
    BOOL  was_up = nw.link == NW_LINK_UP, label_changed;
    int   i;

    nw.link = nw.dev_open ? nw_dev_signal(&nw.dev, &signal, &noise) : NW_LINK_NODEV;
    tool_copy_string(label, sizeof(label), "Wi-Fi");
    switch (nw.link)
    {
        case NW_LINK_UP:
            icon = NW_ICON_CONNECTED;
            bars = nw_bars(signal);
            if (was_up)
                tool_copy_string(label, sizeof(label), nw.label);
            else if (nw_dev_network(&nw.dev, ssid, &len, bssid) && len > 0)
                nw_ssid_text(ssid, (int)len, label);
            break;
        case NW_LINK_DOWN:  icon = NW_ICON_JOINING; break;
        case NW_LINK_OFF:   icon = NW_ICON_OFF; break;
        case NW_LINK_NODEV: icon = NW_ICON_NODEV; break;
        default:            icon = NW_ICON_ERROR; break;
    }
    /* Drawn and written only when the picture or the label changes. */
    for (i = 0; label[i] == nw.label[i] && label[i] != '\0'; i++)
        ;
    label_changed = label[i] != nw.label[i];
    if (icon != nw.icon || label_changed || nw.bars != bars)
    {
        nw.icon = icon;
        nw.bars = bars;
        tool_copy_string(nw.label, sizeof(nw.label), label);
        nw_show(label_changed);
    }

    /* Polled only while connected, or when the unit has no events. */
    if (nw.link == NW_LINK_UP || (nw.events_broken && nw.dev_open)) nw_timer_arm();
}

static VOID nw_event_arm(VOID)
{
    if (!nw.dev_open || nw.events_broken) return;
    (VOID)nw_dev_event_arm(&nw.dev, nw.link == NW_LINK_OFF
                           ? S2EVENT_ONLINE
                           : NW_S2EVENT_CONNECT | NW_S2EVENT_DISCONNECT |
                             S2EVENT_OFFLINE);
}

/* -------------------------------------------------------- commodity -- */

static VOID nw_broker(LONG pri, const char *popkey)
{
    static struct NewBroker nb =
    {
        NB_VERSION, (STRPTR)"NetWireless", (STRPTR)"NetWireless",
        (STRPTR)"Wi-Fi state and signal", NBU_UNIQUE | NBU_NOTIFY,
        COF_SHOW_HIDE, 0, NULL, 0
    };
    CxObj *filter;

    if (CxBase == NULL || (nw.cxport = CreateMsgPort()) == NULL) return;
    nb.nb_Pri = (BYTE)pri;
    nb.nb_Port = nw.cxport;
    nw.broker = CxBroker(&nb, NULL);
    if (nw.broker == NULL) return;
    if (popkey != NULL && popkey[0] != '\0' &&
        (filter = CxFilter(popkey)) != NULL)
    {
        AttachCxObj(nw.broker, filter);
        AttachCxObj(filter, CxSender(nw.cxport, NW_HOTKEY));
        AttachCxObj(filter, CxTranslate(NULL));
        if (CxObjError(filter) != 0)
        {
            RemoveCxObj(filter);
            DeleteCxObjAll(filter);
        }
    }
    (VOID)ActivateCxObj(nw.broker, 1);
}

/* FALSE when told to quit. */
static BOOL nw_cx_msgs(VOID)
{
    CxMsg *m;
    BOOL   run = TRUE;

    while (nw.cxport != NULL && (m = (CxMsg *)GetMsg(nw.cxport)) != NULL)
    {
        ULONG type = CxMsgType(m);
        LONG  id = CxMsgID(m);

        ReplyMsg((struct Message *)m);
        if (type == CXM_IEVENT && id == NW_HOTKEY)
            nw_launch();
        else if (type == CXM_COMMAND)
            switch (id)
            {
                case CXCMD_DISABLE: (VOID)ActivateCxObj(nw.broker, 0); break;
                case CXCMD_ENABLE:  (VOID)ActivateCxObj(nw.broker, 1); break;
                case CXCMD_APPEAR:
                case CXCMD_UNIQUE:  nw_launch(); break;
                case CXCMD_KILL:    run = FALSE; break;
                default: break;
            }
    }
    return run;
}

/* ------------------------------------------------------------- main -- */

int main(int argc, char **argv)
{
    struct RDArgs *rda = NULL;
    const char    *device, *iface;
    ULONG          sigs;
    BOOL           run = TRUE;

    IconBase = OpenLibrary("icon.library", 37);
    if (argc == 0)
    {
        struct WBStartup *wbs = (struct WBStartup *)argv;
        BPTR              old;

        if (IconBase != NULL && wbs->sm_NumArgs > 0)
        {
            old = CurrentDir(wbs->sm_ArgList[0].wa_Lock);
            nw_tool = GetDiskObject((STRPTR)wbs->sm_ArgList[0].wa_Name);
            (VOID)CurrentDir(old);
        }
    }
    else if ((rda = ReadArgs((CONST_STRPTR)TEMPLATE, nw_args, NULL)) == NULL)
    {
        PrintFault(IoErr(), (CONST_STRPTR)tool_name);
        if (IconBase != NULL) CloseLibrary(IconBase);
        return RETURN_ERROR;
    }

    device = nw_opt(ARG_DEVICE);
    iface = nw_opt(ARG_INTERFACE);
    if (device != NULL)
        tool_copy_string(nw.device, sizeof(nw.device), device);
    else if (!nw_dev_from_interface(iface != NULL ? iface : NW_DEFAULT_INTERFACE,
                                    nw.device, sizeof(nw.device), &nw.unit))
        tool_copy_string(nw.device, sizeof(nw.device), NW_DEFAULT_DEVICE);
    nw.unit = (ULONG)nw_opt_number(ARG_UNIT, (LONG)nw.unit);
    nw.interval = (ULONG)nw_opt_number(ARG_INTERVAL, 5);
    if (nw.interval < 1) nw.interval = 1;
    nw.envvar = nw_opt(ARG_ENVVAR);
    if (nw.envvar == NULL) nw.envvar = "WiFi";

    nw.dev_open = nw_dev_open(&nw.dev, nw.device, nw.unit) == 0;
    if (!nw.dev_open && (nw_tool != NULL ? nw_opt(ARG_QUIET) != NULL
                                         : nw_args[ARG_QUIET] != 0))
        goto out;

    CxBase = OpenLibrary("commodities.library", 37);
    WorkbenchBase = OpenLibrary("workbench.library", 37);
    IntuitionBase = (struct IntuitionBase *)OpenLibrary("intuition.library", 37);
    nw_broker(nw_opt_number(ARG_PRI, 0), nw_opt(ARG_POPKEY) != NULL
                                         ? nw_opt(ARG_POPKEY) : "ctrl alt w");
    if (CxBase != NULL && nw.broker == NULL)
        goto out;                       /* already running: told it, done */

    nw.tport = CreateMsgPort();
    nw.treq = (struct timerequest *)CreateIORequest(nw.tport, sizeof(*nw.treq));
    if (nw.treq != NULL &&
        OpenDevice((CONST_STRPTR)"timer.device", UNIT_VBLANK,
                   (struct IORequest *)nw.treq, 0) != 0)
    {
        DeleteIORequest((struct IORequest *)nw.treq);
        nw.treq = NULL;
    }

    nw.appport = CreateMsgPort();
    if (WorkbenchBase != NULL && IntuitionBase != NULL && nw.appport != NULL)
    {
        if (nw_opt_yes(ARG_APPMENU, TRUE))
            nw.appmenu = AddAppMenuItemA(0, 0, (STRPTR)"Wi-Fi...", nw.appport, NULL);
        nw.want_icon = nw_opt_yes(ARG_APPICON, TRUE);
        if (nw.want_icon &&
            (nw.planes = (UBYTE *)AllocMem(2 * NW_PLANE, MEMF_CHIP)) != NULL)
        {
            nw_img.ImageData = (UWORD *)nw.planes;
        }
    }

    nw.icon = -1;
    nw.link = NW_LINK_NODEV;
    nw.label[0] = 1;                    /* differs from every label: shown */
    nw_refresh();
    nw_event_arm();
    if (nw_opt_yes(ARG_POPUP, FALSE)) nw_launch();

    while (run)
    {
        ULONG cx = nw.cxport != NULL ? 1UL << nw.cxport->mp_SigBit : 0;
        ULONG app = nw.appport != NULL ? 1UL << nw.appport->mp_SigBit : 0;
        ULONG tim = nw.tport != NULL ? 1UL << nw.tport->mp_SigBit : 0;
        ULONG dev = nw.dev_open ? 1UL << nw.dev.async->mp_SigBit : 0;
        struct AppMessage *am;

        sigs = Wait(cx | app | tim | dev | SIGBREAKF_CTRL_C);
        if (sigs & SIGBREAKF_CTRL_C) run = FALSE;
        if (sigs & cx) run = nw_cx_msgs() && run;
        if (sigs & app)
            while ((am = (struct AppMessage *)GetMsg(nw.appport)) != NULL)
            {
                /* A double-click on the icon or the menu item; dropped
                   icons are not for this program. */
                BOOL open = am->am_NumArgs == 0;

                ReplyMsg((struct Message *)am);
                if (open) nw_launch();
            }
        if ((sigs & dev) && nw_dev_reply(&nw.dev) == NW_REPLY_EVENT)
        {
            if (nw.dev.event->ios2_Req.io_Error != 0) nw.events_broken = TRUE;
            nw_refresh();
            nw_event_arm();
        }
        if ((sigs & tim) && GetMsg(nw.tport) != NULL)
        {
            nw.tarmed = FALSE;
            nw_refresh();
        }
    }

out:
    nw_icon_remove();
    if (nw.appmenu != NULL) RemoveAppMenuItem(nw.appmenu);
    if (nw.appport != NULL)
    {
        struct Message *m;

        while ((m = GetMsg(nw.appport)) != NULL) ReplyMsg(m);
        DeleteMsgPort(nw.appport);
    }
    if (nw.planes != NULL) FreeMem(nw.planes, 2 * NW_PLANE);
    if (nw.tarmed)
    {
        AbortIO((struct IORequest *)nw.treq);
        WaitIO((struct IORequest *)nw.treq);
    }
    if (nw.treq != NULL)
    {
        CloseDevice((struct IORequest *)nw.treq);
        DeleteIORequest((struct IORequest *)nw.treq);
    }
    if (nw.tport != NULL) DeleteMsgPort(nw.tport);
    if (nw.broker != NULL) DeleteCxObjAll(nw.broker);
    if (nw.cxport != NULL)
    {
        struct Message *m;

        while ((m = GetMsg(nw.cxport)) != NULL) ReplyMsg(m);
        DeleteMsgPort(nw.cxport);
    }
    if (nw.dev_open) nw_dev_close(&nw.dev);
    if (IntuitionBase != NULL) CloseLibrary((struct Library *)IntuitionBase);
    if (WorkbenchBase != NULL) CloseLibrary(WorkbenchBase);
    if (CxBase != NULL) CloseLibrary(CxBase);
    if (nw_tool != NULL) FreeDiskObject(nw_tool);
    if (rda != NULL) FreeArgs(rda);
    if (IconBase != NULL) CloseLibrary(IconBase);
    return RETURN_OK;
}

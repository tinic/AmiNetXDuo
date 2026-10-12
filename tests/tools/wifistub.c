/*
 * WifiStub: a Wi-Fi SANA-II unit made at run time, "wifistub.device", and
 * the NetWireless tools run against it.  Amiberry has no Pi Wi-Fi; this
 * answers the wireless commands the way anxwifipi.device does
 * (third_party/wifipi/src/unit.c, packet.c) and counts what it is asked:
 *
 *   - S2_GETNETWORKS is queued and answered a second later from a process
 *     of its own, from the caller's pool, as the driver's receiver task
 *     does.  AbortIO() on one is counted as a violation and refused: the
 *     real driver corrupts its scan queue there.
 *   - S2_GETSIGNALQUALITY and S2_GETNETWORKINFO answer as the driver does
 *     for offline, not associated and associated.
 *   - S2_ONEVENT answers at once when the unit's online/offline state is in
 *     the mask, else waits for CONNECT or DISCONNECT.
 *
 * The verdict is the last line, "<N> checks, <M> failures".
 *
 * SPDX-License-Identifier: MIT
 */

#include <exec/types.h>
#include <exec/devices.h>
#include <exec/errors.h>
#include <exec/execbase.h>
#include <exec/io.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/dostags.h>
#include <dos/var.h>
#include <utility/tagitem.h>

#include <proto/exec.h>
#include <proto/dos.h>

#include "sana2_device.h"

#define WS_NAME          "wifistub.device"

#define S2INFO_SSID          (TAG_USER + 0)
#define S2INFO_BSSID         (TAG_USER + 1)
#define S2INFO_Channel       (TAG_USER + 7)
#define S2INFO_Signal        (TAG_USER + 8)
#define S2INFO_Capabilities  (TAG_USER + 10)
#define S2INFO_InfoElements  (TAG_USER + 11)
#define S2INFO_Band          (TAG_USER + 13)
#define S2_GETSIGNALQUALITY  0xc010
#define S2_GETNETWORKS       0xc011
#define S2_SETOPTIONS        0xc012
#define S2_GETNETWORKINFO    0xc014
#define S2EVENT_CONNECT      (1L << 9)
#define S2EVENT_DISCONNECT   (1L << 10)
#define WS_EVENTS (S2EVENT_CONNECT | S2EVENT_DISCONNECT | S2EVENT_ONLINE | \
                   S2EVENT_OFFLINE)

typedef struct WsDevice
{
    struct Device dd;
    struct Unit   unit;
    struct List   scans;
    struct List   events;
    BOOL          configured, online, connected;
    LONG          signal;
    ULONG         opens;
    ULONG         n_config, n_online, n_scan, n_signal, n_info, n_event;
    ULONG         n_scan_abort;
    BOOL          break_next;           /* Ctrl-C the next scanner         */
    char          ssid[33];             /* what S2_SETOPTIONS joined       */
    APTR          assoc;                /* the request that joined it      */
    ULONG         n_setoptions;
    ULONG         scan_stamp;           /* ticks when the first scan queued */
} WsDevice;

static WsDevice          *ws;
static volatile BOOL      ws_stop, ws_alive;
static volatile ULONG     ws_ticks;
static const char         ws_name[] = WS_NAME;
static const char         ws_id[]   = WS_NAME " 1.0 (NetWireless test)";
static const UBYTE        ws_mac[6] = { 0x02, 0x57, 0x49, 0x46, 0x49, 0x01 };

static VOID ws_new_list(struct List *l)
{
    l->lh_Head = (struct Node *)&l->lh_Tail;
    l->lh_Tail = NULL;
    l->lh_TailPred = (struct Node *)&l->lh_Head;
    l->lh_Type = NT_MESSAGE;
}

static VOID ws_reply(struct IOSana2Req *io, LONG err, ULONG wire)
{
    io->ios2_Req.io_Error = (BYTE)err;
    io->ios2_WireError = wire;
    if ((io->ios2_Req.io_Flags & IOF_QUICK) == 0)
        ReplyMsg(&io->ios2_Req.io_Message);
}

static VOID ws_queue(struct List *l, struct IOSana2Req *io)
{
    io->ios2_Req.io_Flags &= (UBYTE)~IOF_QUICK;
    io->ios2_Req.io_Message.mn_Node.ln_Type = NT_MESSAGE;
    Forbid();
    AddTail(l, &io->ios2_Req.io_Message.mn_Node);
    Permit();
}

/* Events waiting for any bit of mask are answered with those bits. */
static VOID ws_report(ULONG mask)
{
    struct Node *n, *next;

    Forbid();
    for (n = ws->events.lh_Head; (next = n->ln_Succ) != NULL; n = next)
    {
        struct IOSana2Req *io = (struct IOSana2Req *)n;

        if ((io->ios2_WireError & mask) != 0)
        {
            Remove(n);
            io->ios2_Req.io_Error = 0;
            io->ios2_WireError &= mask;
            ReplyMsg(&io->ios2_Req.io_Message);
        }
    }
    Permit();
}

static struct Device *ws_open(register struct Device *dev __asm("a6"),
                              register struct IOSana2Req *io __asm("a1"),
                              register ULONG unit __asm("d0"),
                              register ULONG flags __asm("d1"))
{
    (VOID)flags;
    if (unit != 0)
    {
        io->ios2_Req.io_Error = IOERR_OPENFAIL;
        return dev;
    }
    io->ios2_Req.io_Device = dev;
    io->ios2_Req.io_Unit = &ws->unit;
    io->ios2_Req.io_Error = 0;
    dev->dd_Library.lib_OpenCnt++;
    ws->opens++;
    return dev;
}

static BPTR ws_close(register struct Device *dev __asm("a6"),
                     register struct IOSana2Req *io __asm("a1"))
{
    /* The supplicant going takes the association with it. */
    if ((APTR)io == ws->assoc)
    {
        ws->assoc = NULL;
        ws->connected = FALSE;
        ws_report(S2EVENT_DISCONNECT);
    }
    io->ios2_Req.io_Device = (struct Device *)-1;
    io->ios2_Req.io_Unit = (struct Unit *)-1;
    if (dev->dd_Library.lib_OpenCnt != 0) dev->dd_Library.lib_OpenCnt--;
    return (BPTR)0;
}

static BPTR ws_expunge(register struct Device *dev __asm("a6"))
{
    (VOID)dev;
    return (BPTR)0;
}

static ULONG ws_null(VOID)
{
    return 0;
}

static VOID ws_network_info(struct IOSana2Req *io)
{
    APTR            pool = io->ios2_Data;
    struct TagItem *t = AllocPooled(pool, sizeof(struct TagItem) * 5);
    UBYTE          *ssid = AllocPooled(pool, 33);
    UBYTE          *bssid = AllocPooled(pool, 6);

    if (t == NULL || ssid == NULL || bssid == NULL)
    {
        ws_reply(io, S2ERR_NO_RESOURCES, S2WERR_BUFF_ERROR);
        return;
    }
    CopyMem(ws->ssid, ssid, 33);
    CopyMem((APTR)ws_mac, bssid, 6);
    t[0].ti_Tag = S2INFO_SSID;  t[0].ti_Data = (ULONG)ssid;
    t[1].ti_Tag = S2INFO_BSSID; t[1].ti_Data = (ULONG)bssid;
    /* As anxwifipi.device reports them from WiFiPi 70eae25 on. */
    t[2].ti_Tag = S2INFO_Channel; t[2].ti_Data = 6;
    t[3].ti_Tag = S2INFO_Band;    t[3].ti_Data = 1;
    t[4].ti_Tag = TAG_DONE;     t[4].ti_Data = 0;
    io->ios2_StatData = t;
    ws_reply(io, 0, 0);
}

static VOID ws_begin_io(register struct Device *dev __asm("a6"),
                        register struct IOSana2Req *io __asm("a1"))
{
    (VOID)dev;
    io->ios2_Req.io_Error = 0;
    if (io->ios2_Req.io_Command != S2_ONEVENT)
        io->ios2_WireError = 0;         /* S2_ONEVENT's mask is in it */

    switch (io->ios2_Req.io_Command)
    {
        case S2_GETSTATIONADDRESS:
            CopyMem((APTR)ws_mac, io->ios2_DstAddr, 6);
            CopyMem((APTR)ws_mac, io->ios2_SrcAddr, 6);
            ws_reply(io, 0, 0);
            break;
        case S2_CONFIGINTERFACE:
            ws->n_config++;
            if (ws->configured)
            {
                ws_reply(io, S2ERR_BAD_STATE, S2WERR_IS_CONFIGURED);
                break;
            }
            ws->configured = ws->online = TRUE;
            ws_reply(io, 0, 0);
            ws_report(S2EVENT_ONLINE);
            break;
        case S2_ONLINE:
            ws->n_online++;
            ws->online = TRUE;
            ws_reply(io, 0, 0);
            ws_report(S2EVENT_ONLINE);
            break;
        case S2_ONEVENT:
        {
            ULONG preset = ws->online ? S2EVENT_ONLINE : S2EVENT_OFFLINE;

            ws->n_event++;
            if (io->ios2_WireError & ~WS_EVENTS)
                ws_reply(io, S2ERR_NOT_SUPPORTED, S2WERR_BAD_EVENT);
            else if (io->ios2_WireError & preset)
            {
                io->ios2_WireError &= preset;
                ws_reply(io, 0, io->ios2_WireError);
            }
            else
                ws_queue(&ws->events, io);
            break;
        }
        case S2_GETSIGNALQUALITY:
            ws->n_signal++;
            if (!ws->online)
                ws_reply(io, S2ERR_OUTOFSERVICE, S2WERR_UNIT_OFFLINE);
            else if (!ws->connected)
                ws_reply(io, S2ERR_OUTOFSERVICE, S2WERR_NOT_CONFIGURED);
            else
            {
                LONG *q = (LONG *)io->ios2_StatData;
                q[0] = ws->signal;
                q[1] = -92;
                ws_reply(io, 0, 0);
            }
            break;
        case S2_GETNETWORKINFO:
            ws->n_info++;
            if (!ws->online)
                ws_reply(io, S2ERR_OUTOFSERVICE, S2WERR_UNIT_OFFLINE);
            else if (!ws->connected)
                ws_reply(io, S2ERR_OUTOFSERVICE, S2WERR_NOT_CONFIGURED);
            else
                ws_network_info(io);
            break;
        case S2_SETOPTIONS:
        {
            /* WirelessManager's join, the SSID tag alone. */
            struct TagItem *t = (struct TagItem *)io->ios2_Data;
            ULONG           k;

            ws->n_setoptions++;
            for (; t != NULL && t->ti_Tag != TAG_DONE; t++)
                if (t->ti_Tag == S2INFO_SSID)
                {
                    const char *s = (const char *)t->ti_Data;

                    for (k = 0; k < 32 && s[k] != 0; k++) ws->ssid[k] = s[k];
                    ws->ssid[k] = 0;
                    ws->assoc = io;
                    ws->connected = TRUE;
                    ws->signal = -61;
                }
            ws_reply(io, 0, 0);
            if (ws->connected) ws_report(S2EVENT_CONNECT);
            break;
        }
        case S2_GETNETWORKS:
            ws->n_scan++;
            if (!ws->configured)
            {
                ws_reply(io, S2ERR_OUTOFSERVICE, S2WERR_NOT_CONFIGURED);
                break;
            }
            io->ios2_DataLength = 0;
            io->ios2_Req.io_Message.mn_Node.ln_Pri = 0;
            io->ios2_Req.io_Message.mn_Node.ln_Name = (char *)ws_ticks;
            ws_queue(&ws->scans, io);
            if (ws->break_next)
            {
                /* As if the user pressed Ctrl-C while the scan runs. */
                ws->break_next = FALSE;
                Signal(io->ios2_Req.io_Message.mn_ReplyPort->mp_SigTask,
                       SIGBREAKF_CTRL_C);
            }
            break;
        default:
            ws_reply(io, IOERR_NOCMD, S2WERR_GENERIC_ERROR);
            break;
    }
}

static LONG ws_abort_io(register struct Device *dev __asm("a6"),
                        register struct IOSana2Req *io __asm("a1"))
{
    struct Node *n;
    LONG         rc = -1;

    (VOID)dev;
    Forbid();
    for (n = ws->scans.lh_Head; n->ln_Succ != NULL; n = n->ln_Succ)
        if (n == &io->ios2_Req.io_Message.mn_Node)
            ws->n_scan_abort++;     /* refused: the scan completes as usual */
    for (n = ws->events.lh_Head; n->ln_Succ != NULL; n = n->ln_Succ)
        if (n == &io->ios2_Req.io_Message.mn_Node)
        {
            Remove(n);
            io->ios2_Req.io_Error = IOERR_ABORTED;
            ReplyMsg(&io->ios2_Req.io_Message);
            rc = 0;
            break;
        }
    Permit();
    return rc;
}

static const APTR ws_vectors[] =
{
    (APTR)ws_open, (APTR)ws_close, (APTR)ws_expunge, (APTR)ws_null,
    (APTR)ws_begin_io, (APTR)ws_abort_io, (APTR)-1
};

/* ------------------------------------------------------ scan answers -- */

typedef struct WsNet
{
    const char   *ssid;
    UBYTE         last;             /* BSSID's last byte                  */
    LONG          signal;
    ULONG         channel;
    ULONG         caps;
    const UBYTE  *ie;
    UWORD         ie_len;
} WsNet;

#define RSN_HEAD 0x01, 0x00, 0x00, 0x0f, 0xac, 0x04, 0x01, 0x00, \
                 0x00, 0x0f, 0xac, 0x04
static const UBYTE ie_wpa2[] = {
    48, 20, RSN_HEAD, 0x01, 0x00, 0x00, 0x0f, 0xac, 0x02, 0x00, 0x00 };
static const UBYTE ie_mixed[] = {
    221, 22, 0x00, 0x50, 0xf2, 0x01, 0x01, 0x00, 0x00, 0x50, 0xf2, 0x02,
    0x01, 0x00, 0x00, 0x50, 0xf2, 0x02, 0x01, 0x00, 0x00, 0x50, 0xf2, 0x02,
    48, 20, RSN_HEAD, 0x01, 0x00, 0x00, 0x0f, 0xac, 0x02, 0x00, 0x00 };

static const WsNet ws_nets[] =
{
    { "Home",      1, -48,  6, 0x0411, ie_wpa2,  sizeof(ie_wpa2) },
    { "Cafe",      2, -70, 11, 0x0401, NULL,     0 },
    { "Home",      3, -80,  1, 0x0411, ie_wpa2,  sizeof(ie_wpa2) },
    { "Neighbour", 4, -85,  1, 0x0411, ie_mixed, sizeof(ie_mixed) },
    { "\0\0\0\0",  5, -40,  6, 0x0411, ie_wpa2,  sizeof(ie_wpa2) },
};
#define WS_NETS (sizeof(ws_nets) / sizeof(ws_nets[0]))

static VOID ws_answer_scan(struct IOSana2Req *io)
{
    APTR             pool = io->ios2_Data;
    struct TagItem **nets = AllocPooled(pool, 4 * WS_NETS);
    ULONG            i;

    if (nets == NULL) { ws_reply(io, S2ERR_NO_RESOURCES, S2WERR_BUFF_ERROR); return; }
    for (i = 0; i < WS_NETS; i++)
    {
        const WsNet    *w = &ws_nets[i];
        struct TagItem *t = AllocPooled(pool, sizeof(struct TagItem) * 9);
        UBYTE          *ssid = AllocPooled(pool, 33);
        UBYTE          *bssid = AllocPooled(pool, 6);
        UWORD          *ie = AllocPooled(pool, 2 + w->ie_len + 1);
        ULONG           k = 0;

        if (t == NULL || ssid == NULL || bssid == NULL || ie == NULL)
        {
            ws_reply(io, S2ERR_NO_RESOURCES, S2WERR_BUFF_ERROR);
            return;
        }
        for (k = 0; k < 4 || w->ssid[k] != 0; k++) ssid[k] = (UBYTE)w->ssid[k];
        ssid[k] = 0;
        k = 0;
        CopyMem((APTR)ws_mac, bssid, 5);
        bssid[5] = w->last;
        *ie = w->ie_len;
        if (w->ie_len != 0) CopyMem((APTR)w->ie, ie + 1, w->ie_len);

        t[k].ti_Tag = S2INFO_SSID;    t[k++].ti_Data = (ULONG)ssid;
        t[k].ti_Tag = S2INFO_BSSID;   t[k++].ti_Data = (ULONG)bssid;
        t[k].ti_Tag = TAG_IGNORE;     t[k++].ti_Data = 0;
        t[k].ti_Tag = S2INFO_Signal;  t[k++].ti_Data = (ULONG)w->signal;
        if (w->ie_len != 0)
        {
            t[k].ti_Tag = S2INFO_InfoElements; t[k++].ti_Data = (ULONG)ie;
        }
        t[k].ti_Tag = S2INFO_Capabilities; t[k++].ti_Data = w->caps;
        t[k].ti_Tag = S2INFO_Channel; t[k++].ti_Data = w->channel;
        t[k].ti_Tag = S2INFO_Band;    t[k++].ti_Data = 1;
        t[k].ti_Tag = TAG_DONE;       t[k].ti_Data = 0;
        nets[i] = t;
    }
    io->ios2_StatData = nets;
    io->ios2_DataLength = WS_NETS;
    ws_reply(io, 0, 0);
}

/* The receiver task: a scan is answered a second after it was queued. */
static VOID ws_replier(VOID)
{
    ws_alive = TRUE;
    while (!ws_stop)
    {
        struct Node *n;

        Delay(5);
        ws_ticks += 5;
        Forbid();
        n = ws->scans.lh_Head;
        if (n->ln_Succ != NULL && ws_ticks - (ULONG)n->ln_Name >= 50)
        {
            Remove(n);
            Permit();
            ws_answer_scan((struct IOSana2Req *)n);
        }
        else
            Permit();
    }
    /* Under Forbid() to the end: the segment this runs in goes once
       ws_alive is clear, and the exit drops the Forbid(). */
    Forbid();
    ws_alive = FALSE;
}

static BOOL ws_install(VOID)
{
    WsDevice *d = (WsDevice *)MakeLibrary((APTR)ws_vectors, NULL, NULL,
                                          sizeof(WsDevice), (BPTR)0);
    if (d == NULL) return FALSE;
    ws_new_list(&d->scans);
    ws_new_list(&d->events);
    d->dd.dd_Library.lib_Node.ln_Type = NT_DEVICE;
    d->dd.dd_Library.lib_Node.ln_Name = (char *)ws_name;
    d->dd.dd_Library.lib_Flags = LIBF_SUMUSED | LIBF_CHANGED;
    d->dd.dd_Library.lib_Version = 1;
    d->dd.dd_Library.lib_IdString = (char *)ws_id;
    ws = d;
    AddDevice(&d->dd);
    return CreateNewProcTags(NP_Entry, (ULONG)ws_replier,
                             NP_Name, (ULONG)"wifistub replier",
                             NP_Priority, 1, TAG_DONE) != NULL;
}

static VOID ws_remove(VOID)
{
    ws_stop = TRUE;
    while (ws_alive) Delay(2);
    Forbid();
    Remove(&ws->dd.dd_Library.lib_Node);
    Permit();
    FreeMem((APTR)((ULONG)ws - ws->dd.dd_Library.lib_NegSize),
            (ULONG)(ws->dd.dd_Library.lib_NegSize + ws->dd.dd_Library.lib_PosSize));
}

/* --------------------------------------------------------- the checks -- */

static int  checks, failures;
static char out[4096];

static VOID check(BOOL ok, const char *what)
{
    checks++;
    if (!ok) failures++;
    Printf((CONST_STRPTR)"%s %s\n", (LONG)(ok ? "ok  " : "FAIL"), (LONG)what);
}

static BOOL has(const char *needle)
{
    const char *h;
    ULONG       n = 0, i;

    while (needle[n] != 0) n++;
    for (h = out; *h != 0; h++)
    {
        for (i = 0; i < n && h[i] == needle[i]; i++)
            ;
        if (i == n) return TRUE;
    }
    return FALSE;
}

static ULONG count(const char *needle)
{
    const char *h;
    ULONG       n = 0, i, c = 0;

    while (needle[n] != 0) n++;
    for (h = out; *h != 0; h++)
    {
        for (i = 0; i < n && h[i] == needle[i]; i++)
            ;
        if (i == n) c++;
    }
    return c;
}

/* Runs a command with its output in T:wsout, prints that, and keeps it in
   out for has(). */
static LONG run(const char *command)
{
    BPTR fh = Open((CONST_STRPTR)"T:wsout", MODE_NEWFILE);
    LONG rc, got = 0;

    if (fh == (BPTR)0) return -1;
    rc = SystemTags((CONST_STRPTR)command, SYS_Output, (ULONG)fh,
                    SYS_Input, 0, TAG_DONE);
    Close(fh);
    fh = Open((CONST_STRPTR)"T:wsout", MODE_OLDFILE);
    if (fh != (BPTR)0)
    {
        got = Read(fh, out, sizeof(out) - 1);
        Close(fh);
    }
    out[got > 0 ? got : 0] = 0;
    Printf((CONST_STRPTR)"--- %s (rc %ld)\n%s---\n", (LONG)command, rc, (LONG)out);
    return rc;
}

#define PREFS "DH0:NetWirelessPrefs DEVICE=" WS_NAME

/* A file into out, for has(). */
static VOID slurp(const char *path)
{
    BPTR fh = Open((CONST_STRPTR)path, MODE_OLDFILE);
    LONG got = 0;

    if (fh != (BPTR)0)
    {
        got = Read(fh, out, sizeof(out) - 1);
        Close(fh);
    }
    out[got > 0 ? got : 0] = 0;
    Printf((CONST_STRPTR)"--- %s\n%s---\n", (LONG)path, (LONG)out);
}

static BOOL put_file(const char *path, const char *text)
{
    BPTR fh = Open((CONST_STRPTR)path, MODE_NEWFILE);
    LONG n = 0;

    if (fh == (BPTR)0) return FALSE;
    while (text[n] != 0) n++;
    Write(fh, (APTR)text, n);
    Close(fh);
    return TRUE;
}

/* Ctrl-C to every Shell command whose name ends in tail. */
static VOID stop_named(const char *tail)
{
    ULONG n, t, k = 0;

    while (tail[k] != 0) k++;
    for (t = 0; t < 50; t++)
    {
        BOOL seen = FALSE;

        Forbid();
        for (n = 1; n <= MaxCli(); n++)
        {
            struct Process *p = FindCliProc(n);
            struct CommandLineInterface *cli;
            const UBYTE *b;
            ULONG i;

            if (p == NULL) continue;
            cli = (struct CommandLineInterface *)BADDR(p->pr_CLI);
            if (cli == NULL || cli->cli_Module == (BPTR)0) continue;
            b = (const UBYTE *)BADDR(cli->cli_CommandName);
            if (b == NULL || b[0] < k) continue;
            for (i = 0; i < k && b[b[0] - k + 1 + i] == (UBYTE)tail[i]; i++)
                ;
            if (i == k)
            {
                Signal(&p->pr_Task, SIGBREAKF_CTRL_C);
                seen = TRUE;
            }
        }
        Permit();
        if (!seen) return;
        Delay(10);
    }
}

static BOOL env_is(const char *want)
{
    char  v[48];
    LONG  n = GetVar((CONST_STRPTR)"WiFi", (STRPTR)v, sizeof(v), GVF_GLOBAL_ONLY);
    ULONG i;

    if (n < 0) v[0] = 0;
    Printf((CONST_STRPTR)"ENV:WiFi=\"%s\"\n", (LONG)v);
    for (i = 0; want[i] != 0 && v[i] == want[i]; i++)
        ;
    return want[i] == 0 && v[i] == 0;
}

/* Ctrl-C to C:WirelessManager, the stub, as a shutdown would send it. */
static VOID stop_wm(VOID)
{
    ULONG n, t;

    for (t = 0; t < 50; t++)
    {
        BOOL seen = FALSE;

        Forbid();
        for (n = 1; n <= MaxCli(); n++)
        {
            struct Process *p = FindCliProc(n);
            struct CommandLineInterface *cli;
            const UBYTE *b;

            if (p == NULL) continue;
            cli = (struct CommandLineInterface *)BADDR(p->pr_CLI);
            if (cli == NULL || cli->cli_Module == (BPTR)0) continue;
            b = (const UBYTE *)BADDR(cli->cli_CommandName);
            if (b != NULL && b[0] >= 15 && b[b[0]] == 'r' && b[b[0] - 6] == 'M')
            {
                Signal(&p->pr_Task, SIGBREAKF_CTRL_C);
                seen = TRUE;
            }
        }
        Permit();
        if (!seen) return;
        Delay(10);
    }
}

/* "WifiStub SERVE": the unit alone, associated with Home, for a person or
   a screenshot to look at; Ctrl-C removes it. */
static int ws_serve(VOID)
{
    CopyMem("Home", ws->ssid, 5);
    ws->configured = ws->online = ws->connected = TRUE;
    ws->signal = -52;
    /* Four bars, then two after ten seconds: a picture that changes.  Run
       can leave a Ctrl-C pending, so the wait starts clear. */
    Delay(500);
    ws->signal = -70;
    SetSignal(0, SIGBREAKF_CTRL_C);
    Wait(SIGBREAKF_CTRL_C);
    while (ws->dd.dd_Library.lib_OpenCnt != 0) Delay(10);
    ws_remove();
    return RETURN_OK;
}

int main(void)
{
    LONG        rc;
    const char *a = (const char *)GetArgStr();

    if (!ws_install())
    {
        Printf((CONST_STRPTR)"could not make %s\n0 checks, 1 failures\n", (LONG)WS_NAME);
        return RETURN_FAIL;
    }
    while (a != NULL && *a == ' ') a++;
    if (a != NULL && a[0] == 'S' && a[1] == 'E' && a[2] == 'R' && a[3] == 'V')
        return ws_serve();

    /* Before anything configured the unit: off. */
    rc = run(PREFS " STATUS");
    check(rc == RETURN_WARN && has("Link          off"), "status of an unconfigured unit");
    check(ws->n_config == 0, "STATUS does not configure the unit");

    /* LIST brings the unit up the way WirelessManager does, then scans. */
    rc = run(PREFS " LIST");
    check(rc == 0, "list rc");
    check(ws->n_config == 1 && ws->configured, "LIST configured the unit once");
    check(has("Home") && has("-48 dBm") && has("WPA2"), "strongest Home listed");
    check(count("Home") == 1, "one line for two Home access points");
    check(has("Cafe") && has("Open"), "open network");
    check(has("Neighbour") && has("WPA/WPA2"), "mixed WPA network");
    check(!has("-40 dBm"), "hidden network left out");

    /* Associated. */
    CopyMem("Home", ws->ssid, 5);
    ws->connected = TRUE;
    ws->signal = -61;
    rc = run(PREFS " STATUS");
    check(rc == 0 && has("connected") && has("Network       Home") &&
          has("-61 dBm, 3 of 4 bars"), "status of an associated unit");
    check(has("Channel       6, 2.4 GHz"), "channel and band where the driver gives them");

    /* Configured and online: no second configure, and no S2_ONLINE, which
       zeroes anxwifipi.device's statistics. */
    rc = run(PREFS " LIST");
    check(rc == 0 && ws->n_config == 2 && ws->n_online == 0,
          "a configured online unit is left alone");

    /* Configured and taken offline: put online, as WirelessManager does. */
    ws->online = FALSE;
    rc = run(PREFS " LIST");
    check(rc == 0 && ws->n_online == 1 && ws->online,
          "a configured offline unit is put online");

    /* Ctrl-C while the scan runs: noted, the scan waited out, not aborted. */
    ws->break_next = TRUE;
    rc = run(PREFS " LIST");
    check(rc == RETURN_WARN && has("Waiting for the scan to finish"),
          "Ctrl-C during a scan waits it out");

    /* DEVICE and UNIT from an interface file with the installer's 2 KB of
       comments ahead of them. */
    rc = run("DH0:NetWirelessPrefs INTERFACE=wifitest STATUS");
    check(rc == 0 && has("wifistub.device, unit 0"), "the interface file's device");
    rc = run("DH0:NetWirelessPrefs INTERFACE=nosuch STATUS");
    check(rc == RETURN_ERROR, "a named interface that is not there");

    /* The refusal goes to the error stream, so the code is the check. */
    rc = run("DH0:NetWirelessPrefs DEVICE=" WS_NAME " UNIT=3 STATUS");
    check(rc == RETURN_ERROR, "a unit that is not there");

    /* JOIN: the file written with comments and other blocks kept,
       WirelessManager started with a 32 KB stack and the interface's
       device, and the association waited for. */
    CreateDir((CONST_STRPTR)"ENV:Sys");
    CreateDir((CONST_STRPTR)"ENVARC:Sys");
    put_file("ENV:Sys/Wireless.prefs",
             "# keep this comment\n"
             "network={\n\tssid=\"Other\"\n\tkey_mgmt=NONE\n\tpriority=4\n}\n");
    ws->connected = FALSE;
    rc = run(PREFS " JOIN=Cafe");
    check(rc == 0 && has("Connected."), "JOIN of an open network connects");
    slurp("ENVARC:Sys/Wireless.prefs");
    check(has("# keep this comment") && has("ssid=\"Other\"") &&
          has("ssid=\"Cafe\"\n\tkey_mgmt=NONE\n\tpriority=5\n}"),
          "Cafe added above Other, the rest kept");
    slurp("T:wmstub.log");
    check(has("start device=wifistub.device unit=0 stack=32768 ssid=Cafe"),
          "WirelessManager started with the unit and 32 KB");

    /* The key derived on this CPU: the host tests cannot see big-endian. */
    {
        struct DateStamp t0, t1;

        DateStamp(&t0);
        rc = run(PREFS " JOIN=Home PASSPHRASE=password");
        DateStamp(&t1);
        Printf((CONST_STRPTR)"join_ticks=%ld\n",
               (LONG)((t1.ds_Minute - t0.ds_Minute) * 3000 + t1.ds_Tick - t0.ds_Tick));
    }
    check(rc == 0 && ws->connected && ws->ssid[0] == 'H', "JOIN with a passphrase");
    slurp("ENV:Sys/Wireless.prefs");
    check(has("psk=e73550102e12a3328959f66713ca8d1e393643502666bdf6d416db71750f6f6a"),
          "the key is PBKDF2(passphrase, SSID), not the passphrase");
    check(!has("password"), "the passphrase is not stored");
    check(has("ssid=\"Home\"\n\tpsk=") && has("priority=6"), "Home on top");
    slurp("T:wmstub.log");
    check(count("start ") == 2 && count("stop") == 1,
          "WirelessManager stopped by Ctrl-C and started again");

    rc = run(PREFS " STATUS");
    check(has("WirelessManager running") && has("Network       Home"),
          "status names the joined network");
    rc = run(PREFS " JOIN=Bad PASSPHRASE=short");
    check(rc == RETURN_ERROR, "a five-character passphrase is refused");
    stop_wm();

    /* The commodity, with no Workbench and no commodities.library on this
       drive: the variable alone.  It polls the signal only while
       associated. */
    {
        BPTR  in = Open((CONST_STRPTR)"NIL:", MODE_OLDFILE);
        BPTR  o = Open((CONST_STRPTR)"NIL:", MODE_NEWFILE);
        ULONG before = AvailMem(MEMF_ANY), used, polls;

        CopyMem("Home", ws->ssid, 5);
        ws->online = ws->connected = TRUE;
        ws->signal = -61;
        SystemTags((CONST_STRPTR)"DH0:NetWireless DEVICE=wifistub.device INTERVAL=1",
                   SYS_Input, (ULONG)in, SYS_Output, (ULONG)o, SYS_Asynch, TRUE,
                   TAG_DONE);
        Delay(100);
        used = before - AvailMem(MEMF_ANY);
        Printf((CONST_STRPTR)"commodity_ram=%lu (with its Shell and stack)\n", used);
        check(env_is("Home 3/4"), "ENV:WiFi says the network and its bars");
        check(ws->events.lh_Head->ln_Succ != NULL, "an event request waits");
        polls = ws->n_signal;
        Delay(160);
        check(ws->n_signal >= polls + 2, "polled once a second while associated");
        ws->signal = -50;
        Delay(80);
        check(env_is("Home 4/4"), "a new bar count is written");

        ws->connected = FALSE;
        ws_report(S2EVENT_DISCONNECT);
        Delay(50);
        check(env_is("Not connected"), "DISCONNECT seen at once");
        polls = ws->n_signal;
        Delay(200);
        Printf((CONST_STRPTR)"polls_while_not_connected=%lu\n", ws->n_signal - polls);
        check(ws->n_signal == polls, "nothing polled while not associated");

        ws->connected = TRUE;
        ws_report(S2EVENT_CONNECT);
        Delay(50);
        check(env_is("Home 4/4"), "CONNECT seen at once");

        ws->online = FALSE;
        ws->connected = FALSE;
        ws_report(S2EVENT_OFFLINE);
        Delay(50);
        check(env_is("Off"), "OFFLINE seen at once");
        ws->online = TRUE;
        ws_report(S2EVENT_ONLINE);
        Delay(50);
        check(env_is("Not connected"), "ONLINE seen at once");

        stop_named("NetWireless");
        Delay(25);

        /* The same start around a program that only waits: what the
           commodity's figure has beyond this one is the commodity. */
        in = Open((CONST_STRPTR)"NIL:", MODE_OLDFILE);
        o = Open((CONST_STRPTR)"NIL:", MODE_NEWFILE);
        polls = AvailMem(MEMF_ANY);
        SystemTags((CONST_STRPTR)"DH0:WaitBreak", SYS_Input, (ULONG)in,
                   SYS_Output, (ULONG)o, SYS_Asynch, TRUE, TAG_DONE);
        Delay(100);
        polls -= AvailMem(MEMF_ANY);
        Printf((CONST_STRPTR)"commodity_own_ram=%lu (control %lu)\n",
               used - polls, polls);
        check(used - polls <= 16384, "the commodity holds at most 16 KB");
        stop_named("WaitBreak");
        Delay(25);
    }

    check(ws->n_scan_abort == 0, "no running scan was aborted");
    check(ws->dd.dd_Library.lib_OpenCnt == 0, "every opener closed");

    ws_remove();
    Printf((CONST_STRPTR)"%ld checks, %ld failures\n", (LONG)checks, (LONG)failures);
    return failures == 0 ? RETURN_OK : RETURN_ERROR;
}

/*
 * NetWireless: the Wi-Fi SANA-II unit.  See the header.
 *
 * SPDX-License-Identifier: MIT
 */

#include "tools.h"
#include "netwireless_dev.h"
#include "netwireless_prefs.h"

#include <exec/execbase.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>

extern struct ExecBase *SysBase;

/* SANA-II drivers are told at OpenDevice() time how to move packet data,
   and some refuse to open without the tags.  Nothing here reads a packet. */
static BOOL nw_copy(register APTR to __asm("a0"),
                    register APTR from __asm("a1"),
                    register ULONG len __asm("d0"))
{
    (VOID)to;
    (VOID)from;
    (VOID)len;
    return FALSE;
}

static struct TagItem nw_buffer_tags[] =
{
    { S2_CopyToBuff,   (ULONG)nw_copy },
    { S2_CopyFromBuff, (ULONG)nw_copy },
    { TAG_DONE,        0 }
};

BOOL nw_dev_from_interface(const char *name, char *device, ULONG cap,
                           ULONG *unit)
{
    static const char dir[] = "DEVS:NetInterfaces/";
    char          given[128], where[128], *text;
    const char   *path;
    BPTR          fh;
    LONG          got;
    ULONG         i = 0, n;
    unsigned long u = *unit;
    BOOL          found = FALSE;

    for (n = 0; dir[n] != '\0'; n++) given[i++] = dir[n];
    for (n = 0; name[n] != '\0'; n++)
    {
        if (i + 1 >= sizeof(given)) return FALSE;
        given[i++] = name[n];
    }
    given[i] = '\0';
    /* A self-contained installation's own Devs drawer first, as the stack
       reads it. */
    path = ami_cfg_resolve(given, where, sizeof(where));

    /* The installer's interface files carry 2 KB of comments ahead of
       DEVICE; NW_IFACE_MAX holds every one it writes. */
    fh = Open((CONST_STRPTR)path, MODE_OLDFILE);
    if (fh == (BPTR)0) return FALSE;
    text = (char *)AllocVec(NW_IFACE_MAX, MEMF_ANY);
    got = text != NULL ? Read(fh, text, NW_IFACE_MAX) : -1;
    Close(fh);
    if (got > 0 && nw_iface_device(text, (size_t)got, device, cap, &u))
    {
        *unit = (ULONG)u;
        found = TRUE;
    }
    if (text != NULL) FreeVec(text);
    return found;
}

/* One interface file's DEVICE and UNIT if the DEVICE is a Wi-Fi driver;
   nothing of a file that is not is kept. */
static BOOL nw_dev_wireless(const char *name, char *device, ULONG cap, ULONG *unit)
{
    char  found[128];
    ULONG u = 0;

    if (!nw_dev_from_interface(name, found, sizeof(found), &u) ||
        !nw_wireless_driver(found))
        return FALSE;
    tool_copy_string(device, cap, found);
    *unit = u;
    return TRUE;
}

#define NW_FIND_MAX 64

BOOL nw_dev_find(char *device, ULONG cap, ULONG *unit)
{
    char        (*names)[TOOL_NAME_LEN];
    char        t[TOOL_NAME_LEN], where[128];
    ULONG       n, i, k;
    BOOL        found = FALSE;

    if (nw_dev_wireless(NW_DEFAULT_INTERFACE, device, cap, unit)) return TRUE;
    /* A temporary list, not a resident one, in name order so that the same
       file is chosen every time. */
    names = AllocVec(NW_FIND_MAX * TOOL_NAME_LEN, MEMF_ANY);
    if (names == NULL) return FALSE;
    n = tool_list_dir(ami_cfg_resolve("DEVS:NetInterfaces", where, sizeof(where)),
                      names, NW_FIND_MAX, NULL);
    for (i = 1; i < n; i++)
        for (k = i; k > 0 && tool_stricmp(names[k - 1], names[k]) > 0; k--)
        {
            tool_copy_string(t, sizeof(t), names[k]);
            tool_copy_string(names[k], TOOL_NAME_LEN, names[k - 1]);
            tool_copy_string(names[k - 1], TOOL_NAME_LEN, t);
        }
    for (i = 0; i < n && !found; i++)
        if (tool_stricmp(names[i], NW_DEFAULT_INTERFACE) != 0)
            found = nw_dev_wireless(names[i], device, cap, unit);
    FreeVec(names);
    return found;
}

static struct IOSana2Req *nw_req_copy(const struct IOSana2Req *from,
                                      struct MsgPort *port)
{
    struct IOSana2Req *r = (struct IOSana2Req *)
        AllocVec(sizeof(*r), MEMF_PUBLIC | MEMF_CLEAR);

    if (r != NULL)
    {
        CopyMem((APTR)from, r, sizeof(*r));
        r->ios2_Req.io_Message.mn_ReplyPort = port;
    }
    return r;
}

LONG nw_dev_open(NwDev *d, const char *device, ULONG unit)
{
    LONG  err;

    d->port  = CreateMsgPort();
    d->async = CreateMsgPort();
    d->req   = (struct IOSana2Req *)AllocVec(sizeof(struct IOSana2Req),
                                             MEMF_PUBLIC | MEMF_CLEAR);
    if (d->port == NULL || d->async == NULL || d->req == NULL)
    {
        nw_dev_close(d);
        return IOERR_OPENFAIL;
    }
    d->req->ios2_Req.io_Message.mn_Node.ln_Type = NT_MESSAGE;
    d->req->ios2_Req.io_Message.mn_Length = sizeof(struct IOSana2Req);
    d->req->ios2_Req.io_Message.mn_ReplyPort = d->port;
    d->req->ios2_BufferManagement = nw_buffer_tags;

    /* As given (a relative name is looked for in DEVS:), then a bare name
       in DEVS:Networks/, where the installer puts drivers, unless a
       resident driver of that name refused. */
    err = (LONG)(BYTE)OpenDevice((CONST_STRPTR)device, unit,
                                 (struct IORequest *)d->req, 0);
    if (err != 0)
    {
        char        path[128] = "Networks/";
        const char *c;
        ULONG       i = 9;
        BOOL        bare = TRUE, resident;

        for (c = device; *c != '\0' && i + 1 < sizeof(path); c++)
        {
            if (*c == ':' || *c == '/') bare = FALSE;
            path[i++] = *c;
        }
        path[i] = '\0';
        Forbid();
        resident = FindName(&SysBase->DeviceList, (CONST_STRPTR)device) != NULL;
        Permit();
        if (bare && !resident && *c == '\0')
        {
            d->req->ios2_Req.io_Device = NULL;
            d->req->ios2_Req.io_Unit = NULL;
            if (OpenDevice((CONST_STRPTR)path, unit, (struct IORequest *)d->req, 0) == 0)
                err = 0;
        }
    }
    if (err != 0)
    {
        d->req->ios2_Req.io_Device = NULL;
        nw_dev_close(d);
        return err;
    }

    d->signal_says = nw_wireless_driver(device) != NW_DRIVER_PRISM2;
    d->scan  = nw_req_copy(d->req, d->async);
    d->event = nw_req_copy(d->req, d->async);
    if (d->scan == NULL || d->event == NULL)
    {
        nw_dev_close(d);
        return IOERR_OPENFAIL;
    }
    return 0;
}

VOID nw_dev_close(NwDev *d)
{
    nw_dev_scan_wait(d);
    if (d->event_busy)
    {
        /* An event request may be aborted; only a scan may not. */
        AbortIO((struct IORequest *)d->event);
        WaitIO((struct IORequest *)d->event);
        d->event_busy = FALSE;
    }
    if (d->req != NULL && d->req->ios2_Req.io_Device != NULL)
        CloseDevice((struct IORequest *)d->req);
    if (d->event != NULL) FreeVec(d->event);
    if (d->scan != NULL) FreeVec(d->scan);
    if (d->req != NULL) FreeVec(d->req);
    if (d->async != NULL) DeleteMsgPort(d->async);
    if (d->port != NULL) DeleteMsgPort(d->port);
    d->event = d->scan = d->req = NULL;
    d->async = d->port = NULL;
}

static LONG nw_do(NwDev *d, UWORD command)
{
    d->req->ios2_Req.io_Command = command;
    d->req->ios2_Req.io_Error = 0;
    d->req->ios2_WireError = 0;
    return (LONG)DoIO((struct IORequest *)d->req);
}

BOOL nw_dev_bring_up(NwDev *d)
{
    if (nw_do(d, S2_GETSTATIONADDRESS) != 0) return FALSE;
    CopyMem(d->req->ios2_DstAddr, d->req->ios2_SrcAddr, SANA2_MAX_ADDR_BYTES);
    if (nw_do(d, S2_CONFIGINTERFACE) == 0) return TRUE;
    if (d->req->ios2_WireError != S2WERR_IS_CONFIGURED) return FALSE;
    /* WirelessManager sends S2_ONLINE here unasked; anxwifipi.device then
       zeroes its statistics, so it goes only to a unit that is offline. */
    {
        LONG signal, noise;

        if (nw_dev_signal(d, &signal, &noise) == NW_LINK_OFF)
            (VOID)nw_do(d, S2_ONLINE);
    }
    return TRUE;
}

LONG nw_dev_signal(NwDev *d, LONG *signal, LONG *noise)
{
    struct { LONG signal, noise; } q;

    LONG rc;

    q.signal = q.noise = 0;
    d->req->ios2_StatData = &q;
    rc = nw_do(d, NW_S2_GETSIGNALQUALITY);
    d->req->ios2_StatData = NULL;
    if (rc == 0)
    {
        *signal = q.signal;
        *noise  = q.noise;
        return NW_LINK_UP;
    }
    if (d->req->ios2_WireError == S2WERR_UNIT_OFFLINE) return NW_LINK_OFF;
    if (d->req->ios2_WireError == S2WERR_NOT_CONFIGURED) return NW_LINK_DOWN;
    return NW_LINK_ERROR;
}

LONG nw_dev_link(NwDev *d, LONG *signal, LONG *noise)
{
    LONG link = nw_dev_signal(d, signal, noise);

    if (link != NW_LINK_UP || d->signal_says || d->event_busy) return link;
    /* ONLINE or OFFLINE is always true, so every driver answers this at
       once and nothing is aborted: a driver may already have taken the
       request off its queue.  prism2.device adds the association bit;
       anxwifipi.device adds none, and its signal answer stands. */
    d->event->ios2_Req.io_Command = S2_ONEVENT;
    d->event->ios2_Req.io_Error = 0;
    d->event->ios2_WireError = NW_S2EVENT_CONNECT | NW_S2EVENT_DISCONNECT |
                               S2EVENT_ONLINE | S2EVENT_OFFLINE;
    (VOID)DoIO((struct IORequest *)d->event);
    if (d->event->ios2_Req.io_Error != 0) link = NW_LINK_ONLINE;
    else if (d->event->ios2_WireError & S2EVENT_OFFLINE) link = NW_LINK_OFF;
    else if (d->event->ios2_WireError & NW_S2EVENT_DISCONNECT) link = NW_LINK_DOWN;
    else if (!(d->event->ios2_WireError & NW_S2EVENT_CONNECT)) link = NW_LINK_ONLINE;
    /* DoIO() may have taken the port's signal from a scan reply queued
       behind; the caller's Wait() must still see it. */
    if (d->async->mp_MsgList.lh_Head->ln_Succ != NULL)
        Signal(d->async->mp_SigTask, 1UL << d->async->mp_SigBit);
    return link;
}

LONG nw_dev_operational(NwDev *d)
{
    LONG r = -1;

    if (d->event_busy) return -1;
    d->event->ios2_Req.io_Command = S2_ONEVENT;
    d->event->ios2_Req.io_Error = 0;
    d->event->ios2_WireError = NW_S2EVENT_LINKUP | NW_S2EVENT_LINKDOWN |
                               S2EVENT_ONLINE | S2EVENT_OFFLINE;
    (VOID)DoIO((struct IORequest *)d->event);
    if (d->event->ios2_Req.io_Error == 0)
    {
        if (d->event->ios2_WireError & NW_S2EVENT_LINKUP) r = 1;
        else if (d->event->ios2_WireError & (NW_S2EVENT_LINKDOWN | S2EVENT_OFFLINE)) r = 0;
    }
    if (d->async->mp_MsgList.lh_Head->ln_Succ != NULL)
        Signal(d->async->mp_SigTask, 1UL << d->async->mp_SigBit);
    return r;
}

/* utility.library's NextTagItem(), without opening it. */
static struct TagItem *nw_next_tag(struct TagItem **tp)
{
    struct TagItem *t;

    while ((t = *tp) != NULL)
    {
        switch (t->ti_Tag)
        {
            case TAG_DONE:   *tp = NULL; return NULL;
            case TAG_IGNORE: (*tp)++; break;
            case TAG_MORE:   *tp = (struct TagItem *)t->ti_Data; break;
            case TAG_SKIP:   *tp += t->ti_Data + 1; break;
            default:         (*tp)++; return t;
        }
    }
    return NULL;
}

static LONG nw_copy_ssid(const UBYTE *s, UBYTE out[NW_SSID_MAX])
{
    LONG n = 0;

    if (s == NULL) return -1;
    while (n < NW_SSID_MAX && s[n] != 0)
    {
        out[n] = s[n];
        n++;
    }
    return n;
}

static APTR nw_pool(VOID)
{
    if (SysBase->LibNode.lib_Version < 39) return NULL;
    return CreatePool(MEMF_PUBLIC | MEMF_CLEAR, 2048, 1024);
}

BOOL nw_dev_network(NwDev *d, UBYTE ssid[NW_SSID_MAX], LONG *ssid_len,
                    UBYTE bssid[6])
{
    APTR            pool = nw_pool();
    struct TagItem *tags, *t;
    BOOL            ok = FALSE;

    *ssid_len = -1;
    d->net_channel = d->net_band5 = -1;
    if (pool == NULL) return FALSE;
    d->req->ios2_Data = pool;
    d->req->ios2_StatData = NULL;
    if (nw_do(d, NW_S2_GETNETWORKINFO) == 0)
    {
        ok = TRUE;
        tags = (struct TagItem *)d->req->ios2_StatData;
        while ((t = nw_next_tag(&tags)) != NULL)
        {
            if (t->ti_Tag == NW_S2INFO_SSID)
                *ssid_len = nw_copy_ssid((const UBYTE *)t->ti_Data, ssid);
            else if (t->ti_Tag == NW_S2INFO_BSSID && t->ti_Data != 0)
                CopyMem((APTR)t->ti_Data, bssid, 6);
            else if (t->ti_Tag == NW_S2INFO_Channel)
                d->net_channel = (LONG)t->ti_Data;
            else if (t->ti_Tag == NW_S2INFO_Band)
                /* A is 5 GHz, B and G 2.4 GHz; N is either. */
                d->net_band5 = t->ti_Data == NW_S2BAND_A ? 1 :
                               (t->ti_Data == NW_S2BAND_B ||
                                t->ti_Data == NW_S2BAND_G) ? 0 : -1;
        }
        /* A band the driver left out follows from the channel. */
        if (d->net_band5 < 0 && d->net_channel > 0)
            d->net_band5 = d->net_channel > 14;
    }
    d->req->ios2_Data = NULL;
    d->req->ios2_StatData = NULL;
    DeletePool(pool);
    return ok;
}

BOOL nw_dev_scan_start(NwDev *d, const UBYTE *ssid, LONG ssid_len)
{
    LONG i;

    if (d->scan_busy) return TRUE;
    d->scan_pool = nw_pool();
    if (d->scan_pool == NULL) return FALSE;

    d->scan->ios2_Req.io_Command = NW_S2_GETNETWORKS;
    d->scan->ios2_Req.io_Error = 0;
    d->scan->ios2_WireError = 0;
    d->scan->ios2_Data = d->scan_pool;
    d->scan->ios2_DataLength = 0;
    d->scan->ios2_StatData = NULL;
    if (ssid != NULL && ssid_len > 0 && ssid_len <= NW_SSID_MAX)
    {
        /* Read by the driver when the scan starts, which may be after other
           scans queued ahead of it: these live as long as d does. */
        for (i = 0; i < ssid_len; i++) d->scan_ssid[i] = ssid[i];
        d->scan_ssid[ssid_len] = 0;
        d->scan_tags[0].ti_Tag = NW_S2INFO_SSID;
        d->scan_tags[0].ti_Data = (ULONG)d->scan_ssid;
        d->scan_tags[1].ti_Tag = TAG_DONE;
        d->scan_tags[1].ti_Data = 0;
        d->scan->ios2_StatData = d->scan_tags;
    }
    SendIO((struct IORequest *)d->scan);
    d->scan_busy = TRUE;
    return TRUE;
}

static VOID nw_scan_entry(struct TagItem *tags, NwScan *e)
{
    struct TagItem *t;
    const UBYTE    *ie = NULL;
    ULONG           ie_len = 0, caps = 0;

    e->ssid_len = 0;
    for (ie_len = 0; ie_len < 6; ie_len++) e->bssid[ie_len] = 0;
    ie_len = 0;
    e->signal = -100;
    e->channel = 0;
    e->band5 = 0;
    e->aps = 1;
    while ((t = nw_next_tag(&tags)) != NULL)
    {
        switch (t->ti_Tag)
        {
            case NW_S2INFO_SSID:
                e->ssid_len = nw_copy_ssid((const UBYTE *)t->ti_Data, e->ssid);
                if (e->ssid_len < 0) e->ssid_len = 0;
                break;
            case NW_S2INFO_BSSID:
                if (t->ti_Data != 0) CopyMem((APTR)t->ti_Data, e->bssid, 6);
                break;
            case NW_S2INFO_Signal:       e->signal = (LONG)t->ti_Data; break;
            case NW_S2INFO_Channel:      e->channel = (int)t->ti_Data; break;
            case NW_S2INFO_Band:         e->band5 = t->ti_Data == NW_S2BAND_A; break;
            case NW_S2INFO_Capabilities: caps = t->ti_Data; break;
            case NW_S2INFO_InfoElements:
                /* A length word, then the elements. */
                if (t->ti_Data != 0)
                {
                    ie_len = *(const UWORD *)t->ti_Data;
                    ie = (const UBYTE *)t->ti_Data + 2;
                }
                break;
            default: break;
        }
    }
    e->sec = nw_security(ie, ie_len, (unsigned)caps);
}

LONG nw_dev_scan_collect(NwDev *d, NwScan *out, LONG max)
{
    struct TagItem **nets;
    NwScan           e;
    LONG             n = 0;
    ULONG            i, count;

    if (!d->scan_busy || !d->scan_replied) return -1;
    d->scan_busy = FALSE;
    d->scan_replied = FALSE;
    if (d->scan->ios2_Req.io_Error != 0)
    {
        n = -1;
    }
    else
    {
        nets = (struct TagItem **)d->scan->ios2_StatData;
        count = d->scan->ios2_DataLength;
        for (i = 0; nets != NULL && i < count; i++)
        {
            if (nets[i] == NULL) continue;
            nw_scan_entry(nets[i], &e);
            n = nw_scan_add(out, (int)n, (int)max, &e);
        }
    }
    DeletePool(d->scan_pool);
    d->scan_pool = NULL;
    return n;
}

LONG nw_dev_reply(NwDev *d)
{
    struct Message *m = GetMsg(d->async);

    if (m == &d->scan->ios2_Req.io_Message)
    {
        d->scan_replied = TRUE;
        return NW_REPLY_SCAN;
    }
    if (m == &d->event->ios2_Req.io_Message)
    {
        d->event_busy = FALSE;
        return NW_REPLY_EVENT;
    }
    return NW_REPLY_NONE;
}

VOID nw_dev_scan_wait(NwDev *d)
{
    if (!d->scan_busy) return;
    /* WaitIO() on a reply already taken off the port would Remove() it a
       second time. */
    if (!d->scan_replied) WaitIO((struct IORequest *)d->scan);
    d->scan_busy = FALSE;
    d->scan_replied = FALSE;
    DeletePool(d->scan_pool);
    d->scan_pool = NULL;
}

BOOL nw_dev_event_arm(NwDev *d, ULONG mask)
{
    if (d->event_busy) return TRUE;
    d->event->ios2_Req.io_Command = S2_ONEVENT;
    d->event->ios2_Req.io_Error = 0;
    d->event->ios2_WireError = mask;
    SendIO((struct IORequest *)d->event);
    d->event_busy = TRUE;
    return TRUE;
}

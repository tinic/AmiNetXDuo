/*
 * NetWireless: the Wi-Fi SANA-II unit.  See the header.
 *
 * SPDX-License-Identifier: MIT
 */

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
    char          path[96], text[1024];
    BPTR          fh;
    LONG          got;
    ULONG         i = 0, n;
    unsigned long u = *unit;
    BOOL          found;

    for (n = 0; dir[n] != '\0'; n++) path[i++] = dir[n];
    for (n = 0; name[n] != '\0' && i + 1 < sizeof(path); n++) path[i++] = name[n];
    path[i] = '\0';

    fh = Open((CONST_STRPTR)path, MODE_OLDFILE);
    if (fh == (BPTR)0) return FALSE;
    got = Read(fh, text, sizeof(text));
    Close(fh);
    if (got <= 0) return FALSE;
    found = nw_iface_device(text, (size_t)got, device, cap, &u) ? TRUE : FALSE;
    *unit = (ULONG)u;
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
    static const char prefix[] = "Networks/";
    char  path[128];
    LONG  err;
    ULONG i, n;

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

    err = (LONG)(BYTE)OpenDevice((CONST_STRPTR)device, unit,
                                 (struct IORequest *)d->req, 0);
    if (err != 0)
    {
        /* A bare name is also looked for where the installer puts it. */
        for (n = 0; device[n] != '\0'; n++)
            if (device[n] == ':' || device[n] == '/') break;
        if (device[n] == '\0' && n + sizeof(prefix) <= sizeof(path))
        {
            for (i = 0; prefix[i] != '\0'; i++) path[i] = prefix[i];
            for (n = 0; device[n] != '\0'; n++) path[i++] = device[n];
            path[i] = '\0';
            d->req->ios2_Req.io_Device = NULL;
            d->req->ios2_Req.io_Unit = NULL;
            err = (LONG)(BYTE)OpenDevice((CONST_STRPTR)path, unit,
                                         (struct IORequest *)d->req, 0);
        }
    }
    if (err != 0)
    {
        d->req->ios2_Req.io_Device = NULL;
        nw_dev_close(d);
        return err;
    }

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
    (VOID)nw_do(d, S2_ONLINE);
    return TRUE;
}

LONG nw_dev_signal(NwDev *d, LONG *signal, LONG *noise)
{
    struct { LONG signal, noise; } q;

    q.signal = q.noise = 0;
    d->req->ios2_StatData = &q;
    if (nw_do(d, NW_S2_GETSIGNALQUALITY) == 0)
    {
        *signal = q.signal;
        *noise  = q.noise;
        return NW_LINK_UP;
    }
    if (d->req->ios2_WireError == S2WERR_UNIT_OFFLINE) return NW_LINK_OFF;
    if (d->req->ios2_WireError == S2WERR_NOT_CONFIGURED) return NW_LINK_DOWN;
    return NW_LINK_ERROR;
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
        }
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

    if (!d->scan_busy) return -1;
    d->scan_busy = FALSE;
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

VOID nw_dev_scan_wait(NwDev *d)
{
    if (!d->scan_busy) return;
    WaitIO((struct IORequest *)d->scan);
    d->scan_busy = FALSE;
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

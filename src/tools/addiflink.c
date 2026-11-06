/*
 * AddNetInterface: the Wi-Fi join wait.  See addiflink.h.
 *
 * SPDX-License-Identifier: MIT
 */

#include "addiflink.h"
#include "netwireless_wm.h"
#include "sana2_device.h"

/* Never called: several drivers refuse an open without the copy hooks. */
static BOOL addif_link_copy(register APTR to __asm("a0"),
                            register APTR from __asm("a1"),
                            register ULONG len __asm("d0"))
{
    (VOID)to;
    (VOID)from;
    (VOID)len;
    return FALSE;
}

static struct TagItem addif_link_tags[] =
{
    { S2_CopyToBuff,   (ULONG)addif_link_copy },
    { S2_CopyFromBuff, (ULONG)addif_link_copy },
    { TAG_DONE,        0 }
};

VOID addif_link_close(AddifLink *link)
{
    if (link->open)
        CloseDevice((struct IORequest *)link->req);
    if (link->req != NULL)
        ami_free(link->req);
    if (link->port != NULL)
        DeleteMsgPort(link->port);
    link->port = NULL;
    link->req  = NULL;
    link->open = FALSE;
}

/* A WirelessManager that runs, or one S:Network-Startup has just Run. */
static BOOL addif_link_supplicant(ULONG elapsed)
{
    if (nw_wm_running(NULL, 0))
        return TRUE;
    return (BOOL)(elapsed < ADDIF_WM_GRACE && !nw_wm_marked_off() &&
                  tool_exists("ENV:Sys/Wireless.prefs"));
}

BOOL addif_link_wait(AddifLink *link, const AmiIfConfig *ifc, ToolWait *wait)
{
    link->port = NULL;
    link->req  = NULL;
    link->open = FALSE;

    if (!addif_link_driver(ifc->device))
        return FALSE;

    link->port = CreateMsgPort();
    link->req  = (struct IOSana2Req *)ami_alloc((ULONG)sizeof(struct IOSana2Req));
    if (link->port == NULL || link->req == NULL)
        return FALSE;

    link->req->ios2_Req.io_Message.mn_Node.ln_Type = NT_MESSAGE;
    link->req->ios2_Req.io_Message.mn_Length = (UWORD)sizeof(struct IOSana2Req);
    link->req->ios2_Req.io_Message.mn_ReplyPort = link->port;
    link->req->ios2_BufferManagement = addif_link_tags;

    if (ami_sana2_open_device(ifc->device, ifc->unit,
                              (struct IORequest *)link->req) != 0)
        return FALSE;
    link->open = TRUE;

    for (;;)
    {
        int verdict;

        link->req->ios2_Req.io_Command = S2_ONEVENT;
        link->req->ios2_Req.io_Error   = 0;
        link->req->ios2_WireError      = ADDIF_S2EVENT_ONLINE |
                                         ADDIF_S2EVENT_OFFLINE |
                                         ADDIF_S2EVENT_LINKUP |
                                         ADDIF_S2EVENT_LINKDOWN;
        (VOID)DoIO((struct IORequest *)link->req);

        verdict = addif_link_verdict(
            (long)link->req->ios2_Req.io_Error,
            (unsigned long)link->req->ios2_WireError,
            addif_link_supplicant(wait->elapsed));

        if (verdict == ADDIF_LINK_UP)
            return TRUE;
        if (verdict == ADDIF_LINK_GO || !tool_wait_second(wait))
            return FALSE;
    }
}

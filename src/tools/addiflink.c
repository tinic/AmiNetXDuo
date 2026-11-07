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

/* A WirelessManager running for `device`. */
static BOOL addif_wm_for(const char *device)
{
    static char args[256];
    ULONG       n = 0;

    while ((n = nw_wm_args(n + 1, args, (ULONG)sizeof(args))) != 0)
        if (addif_args_name(args, device))
            return TRUE;
    return FALSE;
}

/* One for `device`, or about to be: S:Network-Startup Runs it one line
   before AddNetInterface. */
static BOOL addif_gate_supplicant(const char *device, ULONG elapsed)
{
    if (addif_wm_for(device))
        return TRUE;
    return (BOOL)(elapsed < ADDIF_WM_GRACE && addif_wifi_driver(device) &&
                  !nw_wm_marked_off() &&
                  tool_exists("ENV:Sys/Wireless.prefs"));
}

/* The driver's unit process (named after the device), and an opener. */
static VOID addif_unit_state(const char *device, BOOL *unit, BOOL *opened)
{
    const char     *base = tool_basename(device);
    struct Library *dev;

    Forbid();
    *unit  = (BOOL)(FindTask((CONST_STRPTR)base) != NULL);
    dev    = (struct Library *)FindName(&SysBase->DeviceList,
                                        (CONST_STRPTR)base);
    *opened = (BOOL)(dev != NULL && dev->lib_OpenCnt != 0);
    Permit();
}

VOID addif_open_gate(const AmiIfConfig *ifc, ToolWait *wait)
{
    ULONG held   = 0;
    BOOL  waited = FALSE;

    for (;;)
    {
        BOOL unit, opened;

        addif_unit_state(ifc->device, &unit, &opened);
        if (addif_open_now(addif_gate_supplicant(ifc->device, wait->elapsed),
                           unit, held))
            break;
        held   = opened ? held + 1 : 0;
        waited = TRUE;
        if (!tool_wait_second(wait))
            return;
    }
    /* The unit process exists just before its creation has returned. */
    if (waited)
        (VOID)tool_wait_second(wait);
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
            addif_gate_supplicant(ifc->device, wait->elapsed));

        if (verdict == ADDIF_LINK_UP)
            return TRUE;
        if (verdict == ADDIF_LINK_GO || !tool_wait_second(wait))
            return FALSE;
    }
}

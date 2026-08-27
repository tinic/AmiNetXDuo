/* sana2_device.c on the host: open, S2_ONLINE, S2_OFFLINE, close.
   SPDX-License-Identifier: MIT */

#include "sana2_internal.h"

#include "aminetxduo/netstack.h"
#include "aminetxduo/netstatus.h"

/* BeginIO(); the shim declares it and this file defines it. */
#include <inline/alib.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --------------------------------------------------------------- harness -- */

static unsigned long h_checks;
static unsigned long h_failures;

static void h_check(int ok, const char *what)
{
    h_checks++;
    if (!ok)
    {
        h_failures++;
        printf("  FAIL %s\n", what);
    }
}

/* ------------------------------------------------------------ fake device -- */

#define H_READS     8       /* CMD_READs the device is given to hold */

typedef struct HostDevice
{
    int     online_cmds;
    int     offline_cmds;
    int     query_cmds;
    int     configure_cmds;
    int     flush_cmds;
    int     aborts;
    int     special_cmds;

    int     reads_held;         /* CMD_READs the device owns right now */
    BOOL    unit_online;        /* the DEVICE's state, not iface->online */
    BOOL    keeps_everything;   /* will not answer even S2_OFFLINE */
    ULONG   offline_wire_error;
    LONG    offline_error;

    int     closes;         /* CloseDevice() calls */
} HostDevice;

static HostDevice h_dev;

static UWORD h_last_event;
static ULONG h_events;
static ULONG h_retained_events;
/* A driver that refuses to return requests leaves pointers into the interface
   live.  The shipping code must retain that allocation, and this root models
   the same lifetime so LeakSanitizer does not classify it as unreachable. */
static AmiSana2If *volatile h_retained_iface;

static void h_device_reset(void)
{
    memset(&h_dev, 0, sizeof(h_dev));
    h_last_event      = 0;
    h_events          = 0;
    h_retained_events = 0;
}

/* ONE read back, the rest kept: what a pulled card does to a reader. */
static void h_device_out_of_service(void)
{
    h_dev.unit_online = FALSE;
    if (h_dev.reads_held > 0)
        h_dev.reads_held--;
}

/* -------------------------------------------------------------------- exec -- */

VOID ami_event(UWORD code, UWORD index, ULONG value)
{
    (VOID)index;
    (VOID)value;
    h_last_event = code;
    h_events++;
    if (code == NETEVENT_IFACE_RETAINED)
        h_retained_events++;
}

VOID Disable(VOID) { }
VOID Enable(VOID)  { }
VOID Forbid(VOID)  { }
VOID Permit(VOID)  { }

VOID ReplyMsg(struct Message *msg) { (VOID)msg; }
struct Message *GetMsg(struct MsgPort *port) { (VOID)port; return NULL; }

VOID NewList(struct List *list)
{
    list->lh_Head     = (struct Node *)&list->lh_Tail;
    list->lh_Tail     = NULL;
    list->lh_TailPred = (struct Node *)list;
}

VOID AddTail(struct List *list, struct Node *node) { (VOID)list; (VOID)node; }
struct Node *RemHead(struct List *list) { (VOID)list; return NULL; }

struct Task *FindTask(STRPTR name) { (VOID)name; return NULL; }
BYTE  AllocSignal(LONG num) { (VOID)num; return -1; }
VOID  FreeSignal(LONG num) { (VOID)num; }
ULONG Wait(ULONG mask) { return mask; }
VOID  Signal(struct Task *task, ULONG mask) { (VOID)task; (VOID)mask; }

/* One port per CreateMsgPort(), so a leak here is visible as a mismatch. */
static int h_ports_live;
static int h_ports_made;

struct MsgPort *CreateMsgPort(VOID)
{
    struct MsgPort *port = (struct MsgPort *)calloc(1, sizeof(struct MsgPort));

    if (port != NULL)
    {
        h_ports_live++;
        h_ports_made++;
    }

    return port;
}

VOID DeleteMsgPort(struct MsgPort *port)
{
    if (port != NULL)
    {
        h_ports_live--;
        free(port);
    }
}

VOID CloseDevice(struct IORequest *req)
{
    (VOID)req;
    h_dev.closes++;
}

APTR ami_alloc(ULONG size)
{
    return calloc(1, (size_t)size);
}

APTR ami_alloc_flags(ULONG size, ULONG memf)
{
    (VOID)memf;
    return calloc(1, (size_t)size);
}

VOID ami_free(APTR ptr) { free(ptr); }

VOID ami_log(int level, const char *fmt, ...) { (VOID)level; (VOID)fmt; }

VOID ami_random_arrival(VOID) { }

UINT tx_thread_sleep(ULONG ticks) { (VOID)ticks; return 0; }

/* --------------------------------------------------------- the device I/O -- */

LONG ami_sana2_open_device(const char *name, ULONG unit, struct IORequest *req)
{
    (VOID)name;
    (VOID)unit;

    req->io_Device = (struct Device *)&h_dev;
    req->io_Unit   = (struct Unit *)&h_dev;
    req->io_Error  = 0;

    return 0;
}

/* Only the commands sana2_device.c issues; the rest are IOERR_NOCMD. */
LONG DoIO(struct IORequest *ioreq)
{
    struct IOSana2Req *req = (struct IOSana2Req *)ioreq;

    req->ios2_Req.io_Error = 0;
    req->ios2_WireError    = 0;

    switch (req->ios2_Req.io_Command)
    {
    case S2_DEVICEQUERY:
    {
        struct Sana2DeviceQuery *q =
            (struct Sana2DeviceQuery *)req->ios2_StatData;

        h_dev.query_cmds++;

        if (q == NULL)
        {
            req->ios2_Req.io_Error = (BYTE)S2ERR_BAD_ARGUMENT;
            break;
        }

        q->SizeSupplied  = (ULONG)sizeof(*q);
        q->DevQueryFormat = 0;
        q->DeviceLevel   = 0;
        q->AddrFieldSize = 48;
        q->MTU           = 1500;
        q->BPS           = 10000000UL;
        q->HardwareType  = S2WireType_Ethernet;
        break;
    }

    case S2_GETSTATIONADDRESS:
    {
        UWORD i;

        for (i = 0; i < 6; i++)
        {
            req->ios2_SrcAddr[i] = (UBYTE)(0x02 + i);
            req->ios2_DstAddr[i] = (UBYTE)(0x02 + i);
        }
        break;
    }

    case S2_CONFIGINTERFACE:
        h_dev.configure_cmds++;
        break;

    case S2_ONLINE:
        h_dev.online_cmds++;
        h_dev.unit_online = TRUE;
        break;

    case S2_OFFLINE:
        h_dev.offline_cmds++;
        h_dev.unit_online = FALSE;

        /* The only command that returns the queued reads on this device. */
        if (!h_dev.keeps_everything)
            h_dev.reads_held = 0;

        req->ios2_Req.io_Error = (BYTE)h_dev.offline_error;
        req->ios2_WireError    = h_dev.offline_wire_error;
        break;

    case CMD_FLUSH:
        h_dev.flush_cmds++;     /* ignored, like a2065.device 2.16 */
        break;

    case S2_GETGLOBALSTATS:
    {
        struct Sana2DeviceStats *s =
            (struct Sana2DeviceStats *)req->ios2_StatData;

        if (s != NULL)
            memset(s, 0, sizeof(*s));
        break;
    }

    case S2_GETSPECIALSTATS:
    {
        struct Sana2SpecialStatHeader *hdr =
            (struct Sana2SpecialStatHeader *)req->ios2_StatData;
        struct Sana2SpecialStatRecord *rec;
        ULONG n = 0;

        h_dev.special_cmds++;
        if (hdr == NULL)
            break;

        rec = (struct Sana2SpecialStatRecord *)(hdr + 1);
        if (n < hdr->RecordCountMax)
        {
            rec[n].Type   = 15;
            rec[n].Count  = 1234;
            rec[n].String = (char *)"Vertical-blank interrupt polls";
            n++;
        }
        if (n < hdr->RecordCountMax)
        {
            rec[n].Type   = 16;
            rec[n].Count  = 7;
            rec[n].String = (char *)"PCMCIA deaf-receiver resets";
            n++;
        }
        hdr->RecordCountSupplied = n;
        break;
    }

    default:
        req->ios2_Req.io_Error = (BYTE)IOERR_NOCMD;
        break;
    }

    return (LONG)(BYTE)req->ios2_Req.io_Error;
}

VOID SendIO(struct IORequest *req) { (VOID)DoIO(req); }
VOID BeginIO(struct IORequest *req) { (VOID)DoIO(req); }

/* 0 is "accepted" and the request still never completes: a2065.device 2.16. */
LONG AbortIO(struct IORequest *req)
{
    (VOID)req;
    h_dev.aborts++;
    return 0;
}

/* --------------------------------------------- the rest of the sana2 shim -- */

/* The real one's middle phase, which is the only one without a ThreadX thread
   in it: offline, then reap what the device gave back. */
VOID ami_sana2_rx_stop(AmiSana2If *iface)
{
    (VOID)ami_sana2_offline(iface);

    if (h_dev.reads_held > 0)
    {
        (VOID)AbortIO(NULL);
        h_dev.flush_cmds++;
    }

    iface->rx_orphaned = (h_dev.reads_held > 0) ? TRUE : FALSE;
    iface->rx_running  = FALSE;
}

VOID ami_sana2_tx_init(AmiSana2If *iface) { (VOID)iface; }
VOID ami_sana2_tx_drain(AmiSana2If *iface) { iface->tx_orphaned = FALSE; }
VOID ami_sana2_unbind(AmiSana2If *iface) { (VOID)iface; }

/* Addresses only: the tag list carries them and only a device calls one. */
BOOL ami_sana2_copy_to_buff(register APTR to    __asm("a0"),
                            register APTR from  __asm("a1"),
                            register ULONG len  __asm("d0"))
{
    (VOID)to;
    (VOID)from;
    (VOID)len;
    return FALSE;
}

BOOL ami_sana2_copy_from_buff(register APTR to   __asm("a0"),
                              register APTR from __asm("a1"),
                              register ULONG len __asm("d0"))
{
    (VOID)to;
    (VOID)from;
    (VOID)len;
    return FALSE;
}

UBYTE *ami_sana2_rx_direct(APTR ios2_data, ULONG len)
{
    (VOID)ios2_data;
    (VOID)len;
    return NULL;
}

VOID ami_sana2_rx_filled(APTR ios2_data, ULONG len, ULONG sum, UBYTE summed)
{
    (VOID)ios2_data;
    (VOID)len;
    (VOID)sum;
    (VOID)summed;
}

/* ------------------------------------------------------------- bring-up -- */

static AmiIfConfig h_cfg;

static void h_config(void)
{
    memset(&h_cfg, 0, sizeof(h_cfg));
    strcpy(h_cfg.name, "eth0");
    strcpy(h_cfg.device, "test.device");
    h_cfg.unit = 0;
}

/* One opener of the fake unit, with no reads stocked: the shared-unit cases
   count commands, and reads_held is a device-wide number with no opener in it. */
static AmiSana2If *h_bring_up_unit(ULONG unit)
{
    AmiSana2If *iface;
    LONG        err = 0;

    h_config();
    h_cfg.unit = unit;

    iface = ami_sana2_open(&h_cfg, &err);
    if (iface == NULL)
        return NULL;

    if (ami_sana2_online(iface) != 0)
    {
        (VOID)ami_sana2_close(iface);
        return NULL;
    }

    return iface;
}

static AmiSana2If *h_bring_up(void)
{
    AmiSana2If *iface = h_bring_up_unit(0);

    if (iface != NULL)
        h_dev.reads_held = H_READS;

    return iface;
}

/* ami_sana2_close() runs rx_stop and tx_drain itself. */
static BOOL h_tear_down(AmiSana2If *iface)
{
    return ami_sana2_close(iface);
}

/* ----------------------------------------------------------------- cases -- */

/* 1. The healthy shutdown, which must not change. */
static void case_healthy(void)
{
    AmiSana2If *iface;
    BOOL        closed;

    h_device_reset();
    iface = h_bring_up();
    h_check(iface != NULL, "the interface opened and came online");
    if (iface == NULL)
        return;

    h_check(h_dev.online_cmds == 1, "S2_ONLINE was issued exactly once");
    h_check(h_dev.query_cmds == 1, "S2_DEVICEQUERY was issued");
    h_check(h_dev.configure_cmds == 1, "S2_CONFIGINTERFACE was issued");

    closed = h_tear_down(iface);

    h_check(closed, "a healthy interface closes");
    h_check(h_dev.offline_cmds == 1,
            "the healthy teardown issues exactly one S2_OFFLINE");
    h_check(h_dev.reads_held == 0, "the device gave every read back");
    h_check(h_dev.closes == 1, "CloseDevice() was called once");
}

/* 2. One S2ERR_OUTOFSERVICE, then the teardown. */
static void case_out_of_service_still_offlines(void)
{
    AmiSana2If *iface;
    BOOL        closed;

    h_device_reset();
    iface = h_bring_up();
    h_check(iface != NULL, "the interface opened for the out-of-service case");
    if (iface == NULL)
        return;

    iface->online = FALSE;       /* all sana2_rx.c:889 does */
    h_device_out_of_service();

    h_check(iface->offline_state == AMI_SANA2_OFFLINE_UP,
            "a reader taking S2ERR_OUTOFSERVICE does not tell the device");
    h_check(h_dev.reads_held == (H_READS - 1),
            "the device kept the reads it had not completed");

    /* netdev_pcmcia.c's worker on a reinserted card, with no way to say so. */
    h_dev.unit_online = TRUE;

    closed = h_tear_down(iface);

    h_check(h_dev.offline_cmds == 1,
            "S2_OFFLINE is issued even though the stack thought it was down");
    h_check(h_dev.reads_held == 0,
            "the device gave the queued reads back");
    h_check(closed, "the interface closes after one S2ERR_OUTOFSERVICE");
    h_check(h_dev.closes == 1,
            "CloseDevice() was called, so the unit is free to be reopened");
    h_check(h_retained_events == 0,
            "nothing was retained, so no NETEVENT_IFACE_RETAINED");
}

/* 3. A teardown reaches ami_sana2_offline() six or seven times. */
static void case_offline_is_idempotent(void)
{
    AmiSana2If *iface;
    int         i;

    h_device_reset();
    iface = h_bring_up();
    h_check(iface != NULL, "the interface opened for the idempotence case");
    if (iface == NULL)
        return;

    for (i = 0; i < 7; i++)
        (VOID)ami_sana2_offline(iface);

    h_check(h_dev.offline_cmds == 1,
            "seven ami_sana2_offline() calls issue one S2_OFFLINE");

    (VOID)h_tear_down(iface);

    h_check(h_dev.offline_cmds == 1,
            "and the close that follows issues no more");
}

/* 4. The open that failed its query or its configure and closes on the way out. */
static void case_never_online(void)
{
    AmiSana2If *iface;
    LONG        err = 0;

    h_device_reset();
    h_config();

    iface = ami_sana2_open(&h_cfg, &err);
    h_check(iface != NULL, "the interface opened without being brought up");
    if (iface == NULL)
        return;

    h_check(iface->offline_state == AMI_SANA2_OFFLINE_NEVER,
            "a freshly opened interface has never been online");

    (VOID)ami_sana2_offline(iface);
    h_check(h_dev.offline_cmds == 0,
            "an interface that was never online is not sent S2_OFFLINE");

    h_check(h_tear_down(iface), "and it closes");
    h_check(h_dev.offline_cmds == 0, "still no S2_OFFLINE after the close");
}

/* 5. An ISSUED that stuck would silence every teardown after the first. */
static void case_online_rearms(void)
{
    AmiSana2If *iface;

    h_device_reset();
    iface = h_bring_up();
    h_check(iface != NULL, "the interface opened for the re-arm case");
    if (iface == NULL)
        return;

    (VOID)ami_sana2_offline(iface);
    h_check(h_dev.offline_cmds == 1, "the first offline reached the device");

    h_check(ami_sana2_online(iface) == 0, "the interface came back online");
    h_check(iface->offline_state == AMI_SANA2_OFFLINE_UP,
            "S2_ONLINE re-arms the offline");

    (VOID)ami_sana2_offline(iface);
    h_check(h_dev.offline_cmds == 2, "the second offline reached the device");

    (VOID)h_tear_down(iface);
}

/* 6. The refusal ami_sana2_close() exists for: the requests point in here. */
static void case_device_keeps_everything(void)
{
    AmiSana2If *iface;
    BOOL        closed;

    h_device_reset();
    h_dev.keeps_everything = TRUE;

    iface = h_bring_up();
    h_check(iface != NULL, "the interface opened for the retain case");
    if (iface == NULL)
        return;

    closed = h_tear_down(iface);

    h_check(h_dev.offline_cmds == 1,
            "the device was asked, which is all the shim can do");
    h_check(!closed, "a device that keeps its reads is not closed");
    h_check(h_dev.closes == 0, "CloseDevice() was NOT called");
    h_check(h_retained_events == 1, "NETEVENT_IFACE_RETAINED was recorded");

    h_retained_iface = iface;
}

/* 7. A refused S2_OFFLINE still counts as told. */
static void case_offline_refused(void)
{
    AmiSana2If *iface;

    h_device_reset();
    iface = h_bring_up();
    h_check(iface != NULL, "the interface opened for the refusal case");
    if (iface == NULL)
        return;

    h_dev.offline_error      = (LONG)S2ERR_BAD_STATE;
    h_dev.offline_wire_error = S2WERR_GENERIC_ERROR;

    h_check(ami_sana2_offline(iface) != 0, "a refused S2_OFFLINE is reported");
    h_check(h_last_event == NETEVENT_OFFLINE_FAILED,
            "and recorded as NETEVENT_OFFLINE_FAILED");

    (VOID)ami_sana2_offline(iface);
    h_check(h_dev.offline_cmds == 1,
            "a refusal is not retried on every later call");

    h_dev.offline_error      = 0;
    h_dev.offline_wire_error = 0;
    h_dev.reads_held         = 0;

    (VOID)h_tear_down(iface);
}

/* 8. Two interfaces on ONE unit: the first one out must not take the wire. */
static void case_shared_unit(void)
{
    AmiSana2If *a;
    AmiSana2If *b;

    h_device_reset();

    a = h_bring_up_unit(0);
    h_check(a != NULL, "the first interface on the shared unit came up");
    if (a == NULL)
        return;

    h_check(h_dev.online_cmds == 1, "the first interface onlined the unit");

    b = h_bring_up_unit(0);
    h_check(b != NULL, "the second interface on the shared unit came up");
    if (b == NULL)
    {
        (VOID)h_tear_down(a);
        return;
    }

    h_check(h_dev.online_cmds == 1,
            "the second interface did not re-online a unit already up");
    h_check(b->online, "and it still counts itself online");

    h_check(h_tear_down(a), "the first interface closes");
    h_check(h_dev.offline_cmds == 0,
            "removing one interface issues no S2_OFFLINE while a sibling holds "
            "the unit");
    h_check(h_dev.unit_online, "the sibling still has a live wire");
    h_check(h_dev.closes == 1, "CloseDevice() was called for the first");

    h_check(h_tear_down(b), "the second interface closes");
    h_check(h_dev.offline_cmds == 1,
            "the last interface out issues exactly one S2_OFFLINE");
    h_check(!h_dev.unit_online, "and the wire is down");
    h_check(h_dev.closes == 2, "CloseDevice() was called for both");
}

/* 9. Two units are not one: neither may borrow the other's count. */
static void case_distinct_units(void)
{
    AmiSana2If *a;
    AmiSana2If *b;

    h_device_reset();

    a = h_bring_up_unit(0);
    b = h_bring_up_unit(1);
    h_check(a != NULL && b != NULL, "both units came up");
    if (a == NULL || b == NULL)
        return;

    h_check(h_dev.online_cmds == 2, "each unit was onlined on its own");

    h_check(h_tear_down(a), "unit 0 closes");
    h_check(h_dev.offline_cmds == 1,
            "a unit with one interface offlines when that one leaves");

    h_check(h_tear_down(b), "unit 1 closes");
    h_check(h_dev.offline_cmds == 2, "and so does the other");
}

/* 10. anxnet.device recovery evidence must cross the SANA-II boundary. */
static void case_special_recovery_stats(void)
{
    AmiSana2If    *iface;
    AmiSana2Stats  stats;

    h_device_reset();
    iface = h_bring_up_unit(0);
    h_check(iface != NULL, "the interface opened for special statistics");
    if (iface == NULL)
        return;

    ami_sana2_refresh_stats(iface);
    ami_sana2_get_stats(iface, &stats);

    h_check(h_dev.special_cmds > 0, "S2_GETSPECIALSTATS was issued");
    h_check(stats.tick_polls == 1234,
            "the vertical-blank poll count crossed the device boundary");
    h_check(stats.rx_kicks == 7,
            "the deaf-receiver reset count crossed the device boundary");

    h_check(h_tear_down(iface), "the statistics interface closes");
}

/* ------------------------------------------------------------------ main -- */

int main(void)
{
    printf("sana2 device: open, online, offline, close\n");

    case_healthy();
    case_out_of_service_still_offlines();
    case_offline_is_idempotent();
    case_never_online();
    case_online_rearms();
    case_device_keeps_everything();
    case_offline_refused();
    case_shared_unit();
    case_distinct_units();
    case_special_recovery_stats();

    h_check(h_ports_made > 0, "reply ports were created");
    h_check(h_ports_live == 0, "every reply port was deleted");

    printf("%lu checks, %lu failures, %s\n", h_checks, h_failures,
           (h_failures == 0) ? "PASS" : "FAIL");

    return (h_failures == 0) ? 0 : 1;
}

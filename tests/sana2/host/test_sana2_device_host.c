/* sana2_device.c on the host: open, S2_ONLINE, S2_OFFLINE, close.
   SPDX-License-Identifier: MIT */

#include "sana2_internal.h"

#include "aminetxduo/netstack.h"
#include "aminetxduo/netstatus.h"

/* BeginIO(); the shim declares it and this file defines it. */
#include <inline/alib.h>
#include <proto/exec.h>

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
    int     opens;          /* OpenDevice() calls */

    /* How S2_OFFLINE is answered when it arrives by SendIO(). */
    int     offline_mode;       /* H_OFF_* */
    ULONG   offline_after_ms;   /* H_OFF_LATE: the reply's delay */

    /* The rest of the interface the device can hold, for the retained list. */
    int     writes_held;        /* CMD_WRITEs it owns right now */
    BOOL    reader_running;     /* the reader has not exited, requests or no */
    int     tx_reaps;
    int     rx_reclaims;
    int     packets_released;   /* the read slots' packets, back to the pool */
    int     slot_frees;         /* the watched interface's read slots freed */
} HostDevice;

/* S2_OFFLINE by SendIO(): inside BeginIO(), later, only once aborted, or
   never (the test replies by hand, if at all). */
#define H_OFF_SYNC      0
#define H_OFF_LATE      1
#define H_OFF_ABORTABLE 2
#define H_OFF_DEAF      3

static HostDevice h_dev;
static UBYTE h_other_device;     /* distinct io_Device, same numeric unit */

static UWORD h_last_event;
static ULONG h_events;
static ULONG h_retained_events;
/* A driver that refuses to return requests leaves pointers into the interface
   live.  The shipping code must retain that allocation, and this root models
   the same lifetime so LeakSanitizer does not classify it as unreachable. */
static AmiSana2If *volatile h_retained_iface;

static void h_exec_reset(void);

static void h_device_reset(void)
{
    memset(&h_dev, 0, sizeof(h_dev));
    h_exec_reset();
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

/*
 * Enough of Exec for a request that completes later: a reply port list, the
 * port's action, signal bits, and a clock that Wait() moves on to the next
 * thing due.  The one task is h_task; PA_SIGNAL to any other, or on a bit
 * nobody holds, is a failure.
 */
static struct Task      h_task;
static ULONG            h_sigs_alloc;
static unsigned long    h_signals;          /* PA_SIGNAL replies, not the
                                               timer's */
static unsigned long    h_ignored;          /* PA_IGNORE replies: queued only */
static ULONG            h_now_ms;
static UBYTE            h_timer_dev;        /* timer.device's io_Device */
static int              h_timer_opens;
static int              h_timer_closes;
static BOOL             h_timer_open_fails;
static struct IORequest *h_timer_pending;
static ULONG            h_timer_due;
static struct IORequest *h_off_pending;     /* S2_OFFLINE the device holds */
static ULONG            h_off_due;          /* 0xFFFFFFFF: never */
static BYTE             h_off_error;        /* what it will reply with */
static int              h_disables;
/* One-shot: a reply lands inside the next Disable(), just before whatever
   it protects, or just after the next Enable(). */
static BOOL             h_reply_before_disable;
static BOOL             h_reply_after_enable;

#define H_NEVER 0xFFFFFFFFUL

static void h_exec_reset(void)
{
    h_sigs_alloc           = 0;
    h_signals              = 0;
    h_ignored              = 0;
    h_now_ms               = 0;
    h_timer_opens          = 0;
    h_timer_closes         = 0;
    h_timer_open_fails     = FALSE;
    h_timer_pending        = NULL;
    h_timer_due            = H_NEVER;
    h_off_pending          = NULL;
    h_off_due              = H_NEVER;
    h_off_error            = 0;
    h_disables             = 0;
    h_reply_before_disable = FALSE;
    h_reply_after_enable   = FALSE;
}

VOID ReplyMsg(struct Message *msg)
{
    struct MsgPort *port = msg->mn_ReplyPort;
    struct Node    *node = &msg->mn_Node;

    node->ln_Type = NT_REPLYMSG;

    node->ln_Succ = (struct Node *)&port->mp_MsgList.lh_Tail;
    node->ln_Pred = port->mp_MsgList.lh_TailPred;
    port->mp_MsgList.lh_TailPred->ln_Succ = node;
    port->mp_MsgList.lh_TailPred          = node;

    if ((port->mp_Flags & PF_ACTION) == PA_IGNORE)
    {
        h_ignored++;
        return;
    }

    h_check(port->mp_SigTask == (void *)&h_task,
            "a reply signalled only the live task");
    h_check((h_sigs_alloc & (1UL << port->mp_SigBit)) != 0,
            "a reply signalled only a bit still allocated");
    if (((struct IORequest *)msg)->io_Device != (struct Device *)&h_timer_dev)
        h_signals++;
}

static void h_remove(struct Node *node)
{
    node->ln_Pred->ln_Succ = node->ln_Succ;
    node->ln_Succ->ln_Pred = node->ln_Pred;
    node->ln_Succ = NULL;
    node->ln_Pred = NULL;
}

struct Message *GetMsg(struct MsgPort *port)
{
    struct Node *node = port->mp_MsgList.lh_Head;

    if (node->ln_Succ == NULL)
        return NULL;
    h_remove(node);
    return (struct Message *)node;
}

/* The held S2_OFFLINE comes back, as the device's own unit task would. */
static void h_offline_reply(void)
{
    struct IOSana2Req *req = (struct IOSana2Req *)h_off_pending;

    if (req == NULL)
        return;
    h_off_pending = NULL;
    h_off_due     = H_NEVER;

    if (!h_dev.keeps_everything)
        h_dev.reads_held = 0;
    req->ios2_Req.io_Error = h_off_error;
    if (h_off_error == 0)
    {
        req->ios2_Req.io_Error = (BYTE)h_dev.offline_error;
        req->ios2_WireError    = h_dev.offline_wire_error;
    }
    ReplyMsg(&req->ios2_Req.io_Message);
}

static void h_timer_reply(BYTE error)
{
    struct IORequest *tr = h_timer_pending;

    h_timer_pending = NULL;
    h_timer_due     = H_NEVER;
    tr->io_Error    = error;
    ReplyMsg(&tr->io_Message);
}

VOID Disable(VOID)
{
    if (h_reply_before_disable)
    {
        h_reply_before_disable = FALSE;
        h_offline_reply();
    }
    h_disables++;
}

VOID Enable(VOID)
{
    h_disables--;
    if (h_reply_after_enable && h_disables == 0)
    {
        h_reply_after_enable = FALSE;
        h_offline_reply();
    }
}

VOID Forbid(VOID)  { }
VOID Permit(VOID)  { }

VOID NewList(struct List *list)
{
    list->lh_Head     = (struct Node *)&list->lh_Tail;
    list->lh_Tail     = NULL;
    list->lh_TailPred = (struct Node *)list;
}

VOID AddTail(struct List *list, struct Node *node) { (VOID)list; (VOID)node; }
struct Node *RemHead(struct List *list) { (VOID)list; return NULL; }

struct Task *FindTask(STRPTR name) { (VOID)name; return &h_task; }

BYTE AllocSignal(LONG num)
{
    BYTE b;

    (VOID)num;
    for (b = 16; b < 32; b++)
    {
        if ((h_sigs_alloc & (1UL << b)) == 0)
        {
            h_sigs_alloc |= 1UL << b;
            return b;
        }
    }
    return -1;
}

VOID FreeSignal(LONG num)
{
    h_check((h_sigs_alloc & (1UL << num)) != 0, "FreeSignal() of a held bit");
    h_sigs_alloc &= ~(1UL << num);
}

/* The clock moves to whatever is due first, and that happens. */
ULONG Wait(ULONG mask)
{
    ULONG due = (h_off_due < h_timer_due) ? h_off_due : h_timer_due;

    if (due == H_NEVER)
    {
        h_check(FALSE, "Wait() with nothing that can ever wake it");
        if (h_timer_pending != NULL)
            h_timer_reply(0);
        return mask;
    }

    if (due > h_now_ms)
        h_now_ms = due;
    if (h_off_due <= h_now_ms)
        h_offline_reply();
    if (h_timer_due <= h_now_ms)
        h_timer_reply(0);

    return mask;
}

VOID Signal(struct Task *task, ULONG mask) { (VOID)task; (VOID)mask; }

struct IORequest *CheckIO(struct IORequest *req)
{
    if ((req->io_Flags & IOF_QUICK) == 0 &&
        req->io_Message.mn_Node.ln_Type == NT_MESSAGE)
        return NULL;
    return req;
}

BYTE WaitIO(struct IORequest *req)
{
    while (CheckIO(req) == NULL)
        (VOID)Wait(0);
    if ((req->io_Flags & IOF_QUICK) == 0 &&
        req->io_Message.mn_Node.ln_Succ != NULL)
        h_remove(&req->io_Message.mn_Node);
    return req->io_Error;
}

BYTE OpenDevice(STRPTR name, ULONG unit, struct IORequest *req, ULONG flags)
{
    (VOID)unit;
    (VOID)flags;
    h_check(strcmp((const char *)name, TIMERNAME) == 0,
            "OpenDevice() only for timer.device");
    if (h_timer_open_fails)
        return (BYTE)IOERR_OPENFAIL;
    req->io_Device = (struct Device *)&h_timer_dev;
    h_timer_opens++;
    return 0;
}

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
    if (req->io_Device == (struct Device *)&h_timer_dev)
    {
        h_timer_closes++;
        return;
    }
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

/* The one allocation a case is watching: a retained interface must not be
   freed while its device holds requests, and then exactly once. */
static APTR h_free_watch;
static int  h_free_watched;

VOID ami_free(APTR ptr)
{
    if (ptr != NULL && ptr == h_free_watch)
        h_free_watched++;
    free(ptr);
}

VOID ami_log(int level, const char *fmt, ...) { (VOID)level; (VOID)fmt; }

VOID ami_random_arrival(VOID) { }
ULONG ami_config_rx_batch(ULONG fallback) { return fallback; }
ULONG ami_config_tx_run(ULONG fallback)   { return fallback; }

UINT tx_thread_sleep(ULONG ticks) { (VOID)ticks; return 0; }

/* --------------------------------------------------------- the device I/O -- */

static ULONG h_open_flags;      /* what the last open asked the driver for */
static UBYTE h_units[2];        /* unit 0 is NULL, as plipbox reports it */

LONG ami_sana2_open_device_flags(const char *name, ULONG unit,
                                 struct IORequest *req, ULONG flags)
{
    h_open_flags   = flags;
    req->io_Device = (strcmp(name, "other.device") == 0)
                         ? (struct Device *)&h_other_device
                         : (struct Device *)&h_dev;
    req->io_Unit   = (unit == 0) ? NULL : (struct Unit *)&h_units[unit];
    req->io_Error  = 0;
    h_dev.opens++;

    return 0;
}

LONG ami_sana2_open_device(const char *name, ULONG unit, struct IORequest *req)
{
    return ami_sana2_open_device_flags(name, unit, req, 0UL);
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

/* SendIO() clears io_Flags and never completes quick: a request finished
   inside BeginIO() is replied all the same. */
VOID SendIO(struct IORequest *req)
{
    struct IOSana2Req *s2 = (struct IOSana2Req *)req;

    req->io_Flags = 0;
    req->io_Message.mn_Node.ln_Type = NT_MESSAGE;

    if (req->io_Device == (struct Device *)&h_timer_dev)
    {
        struct timerequest *tr = (struct timerequest *)req;

        h_check(h_timer_pending == NULL, "one timer request at a time");
        h_timer_pending = req;
        h_timer_due     = h_now_ms + tr->tr_time.tv_secs * 1000UL +
                          tr->tr_time.tv_micro / 1000UL;
        return;
    }

    if (s2->ios2_Req.io_Command == S2_OFFLINE &&
        h_dev.offline_mode != H_OFF_SYNC)
    {
        h_dev.offline_cmds++;
        h_dev.unit_online = FALSE;
        h_off_pending     = req;
        h_off_error       = 0;
        h_off_due         = (h_dev.offline_mode == H_OFF_LATE)
                                ? h_now_ms + h_dev.offline_after_ms : H_NEVER;
        return;
    }

    (VOID)DoIO(req);
    ReplyMsg(&req->io_Message);
}

VOID BeginIO(struct IORequest *req) { (VOID)DoIO(req); }

/* 0 is "accepted" and the request still never completes: a2065.device 2.16.
   timer.device always answers, and H_OFF_ABORTABLE answers 10 ms later. */
LONG AbortIO(struct IORequest *req)
{
    if (req != NULL && req == h_timer_pending)
    {
        h_timer_reply((BYTE)IOERR_ABORTED);
        return 0;
    }

    h_dev.aborts++;
    if (req != NULL && req == h_off_pending &&
        h_dev.offline_mode == H_OFF_ABORTABLE)
    {
        h_off_error = (BYTE)IOERR_ABORTED;
        h_off_due   = h_now_ms + 10UL;
    }
    return 0;
}

/* --------------------------------------------- the rest of the sana2 shim -- */

/* No reader ever started here, so there are no slot arrays to give back;
   when they would go is still counted, for the watched interface. */
VOID ami_sana2_rx_free_slots(AmiSana2If *iface)
{
    if (iface != NULL && (APTR)iface == h_free_watch)
        h_dev.slot_frees++;
}

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

    iface->rx_orphaned = (h_dev.reads_held > 0 || h_dev.reader_running)
                             ? TRUE : FALSE;
    iface->rx_running  = FALSE;
}

/* The real one's shape: every write the device has is a busy slot. */
VOID ami_sana2_tx_drain(AmiSana2If *iface)
{
    int i;

    for (i = 0; i < AMI_SANA2_TX_SLOTS; i++)
        iface->tx[i].busy = (i < h_dev.writes_held) ? TRUE : FALSE;
    iface->tx_orphaned = (h_dev.writes_held > 0) ? TRUE : FALSE;
}

/* Collects the writes the device has given back since: no AbortIO(). */
VOID ami_sana2_tx_reap(AmiSana2If *iface)
{
    int i;

    h_dev.tx_reaps++;
    for (i = h_dev.writes_held; i < AMI_SANA2_TX_SLOTS; i++)
        iface->tx[i].busy = FALSE;
}

UWORD ami_sana2_tx_collect(AmiSana2If *iface)
{
    UWORD busy = 0;
    int   i;

    ami_sana2_tx_reap(iface);
    for (i = 0; i < AMI_SANA2_TX_SLOTS; i++)
    {
        if (iface->tx[i].busy)
            busy++;
    }
    return busy;
}

/* sana2_rx.c's, whose own holds test_sana2_rx drives: here only the verdict. */
BOOL ami_sana2_rx_reclaim(AmiSana2If *iface, BOOL release_packets)
{
    h_dev.rx_reclaims++;

    if (!iface->rx_orphaned)
        return TRUE;
    if (h_dev.reads_held > 0 || h_dev.reader_running)
        return FALSE;

    if (release_packets)
        h_dev.packets_released++;
    iface->rx_orphaned = FALSE;
    return TRUE;
}

VOID ami_sana2_tx_init(AmiSana2If *iface) { (VOID)iface; }
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
static AmiSana2If *h_bring_up_device(const char *device, ULONG unit)
{
    AmiSana2If *iface;
    LONG        err = 0;

    h_config();
    strcpy(h_cfg.device, device);
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

static AmiSana2If *h_bring_up_unit(ULONG unit)
{
    return h_bring_up_device("test.device", unit);
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
    h_check(ami_sana2_retained_count() == 1,
            "and the interface is on the retained list");

    h_retained_iface = iface;

    /* It gives them back after all: the sweep closes it, once. */
    h_dev.keeps_everything = FALSE;
    h_dev.reads_held       = 0;
    h_check(ami_sana2_retained_sweep(TRUE) == 0, "the sweep empties the list");
    h_check(h_dev.closes == 1, "and CloseDevice() came from the sweep");
    h_retained_iface = NULL;
}

/* ------------------------------------------------ the retained list -- */

/* Open test.device unit 0 with `reads`, `writes` and a reader still running
   as the device will hold them at the close, and watch its memory. */
static AmiSana2If *h_retain(int reads, int writes, BOOL running)
{
    AmiSana2If *iface;

    h_device_reset();
    h_dev.keeps_everything = TRUE;

    iface = h_bring_up_unit(0);
    if (iface == NULL)
        return NULL;

    h_dev.reads_held     = reads;
    h_dev.writes_held    = writes;
    h_dev.reader_running = running;

    h_free_watch   = iface;
    h_free_watched = 0;

    return iface;
}

/* What a case leaves must be what it found: an empty list. */
static void h_retain_done(void)
{
    h_dev.reads_held     = 0;
    h_dev.writes_held    = 0;
    h_dev.reader_running = FALSE;
    (VOID)ami_sana2_retained_sweep(TRUE);
    h_free_watch = NULL;
}

/* A CMD_WRITE kept past the drain, and nothing else. */
static void case_retain_tx_only(void)
{
    AmiSana2If *iface;
    int         reaps;

    printf("  a write the device keeps\n");
    iface = h_retain(0, 2, FALSE);
    h_check(iface != NULL, "tx-only: the interface opened");
    if (iface == NULL)
        return;

    h_check(!ami_sana2_close(iface), "tx-only: the close is refused");
    h_check(h_dev.closes == 0, "tx-only: no CloseDevice() under a held write");
    h_check(h_free_watched == 0, "tx-only: the interface is not freed");
    h_check(ami_sana2_retained_count() == 1, "tx-only: it is retained");
    h_check(ami_sana2_retained_holds("test.device", 0) == NETEVENT_HELD_TX,
            "tx-only: and held on the write side only");

    reaps = h_dev.tx_reaps;
    h_check(ami_sana2_retained_sweep(TRUE) == 1 &&
            ami_sana2_retained_sweep(TRUE) == 1,
            "tx-only: sweeps while the write is out keep it");
    h_check(h_dev.tx_reaps == reaps + 2 && h_dev.aborts == 0,
            "tx-only: each sweep collects, none asks the device again");
    h_check(h_dev.closes == 0 && h_free_watched == 0 && h_dev.slot_frees == 0,
            "tx-only: still not closed; the interface and its slots not freed");

    h_dev.writes_held = 1;
    h_check(ami_sana2_retained_sweep(TRUE) == 1,
            "tx-only: one of two back is still a hold");

    h_dev.writes_held = 0;
    h_check(ami_sana2_retained_sweep(TRUE) == 0,
            "tx-only: the last write back empties the list");
    h_check(h_dev.closes == 1, "tx-only: CloseDevice() exactly once");
    h_check(h_free_watched == 1 && h_dev.slot_frees == 1,
            "tx-only: the interface and its slots freed exactly once");

    h_check(ami_sana2_retained_sweep(TRUE) == 0 && h_dev.closes == 1,
            "tx-only: a sweep of nothing does nothing");
    h_check(h_dev.opens == h_dev.closes,
            "tx-only: one CloseDevice() for the one OpenDevice()");
    h_retain_done();
}

/* Reads kept past S2_OFFLINE, and a repeated close. */
static void case_retain_rx_only(void)
{
    AmiSana2If *iface;
    ULONG       events;

    printf("  reads the device keeps, closed twice\n");
    iface = h_retain(H_READS, 0, FALSE);
    h_check(iface != NULL, "rx-only: the interface opened");
    if (iface == NULL)
        return;

    h_check(!ami_sana2_close(iface), "rx-only: the close is refused");
    h_check(ami_sana2_retained_holds("DEVS:Networks/TEST.device", 0) ==
                NETEVENT_HELD_RX,
            "rx-only: held on the read side, found by basename and any case");
    events = h_retained_events;

    h_check(!ami_sana2_close(iface), "rx-only: a second close is refused too");
    h_check(ami_sana2_retained_count() == 1, "rx-only: and not linked twice");
    h_check(h_retained_events == events && h_dev.offline_cmds == 1,
            "rx-only: nor torn down twice");
    h_check(h_dev.closes == 0 && h_free_watched == 0 && h_dev.slot_frees == 0,
            "rx-only: no CloseDevice(), no free of the interface or its slots");

    h_dev.reads_held = 0;
    h_check(ami_sana2_retained_sweep(TRUE) == 0,
            "rx-only: the reads back, the sweep releases it");
    h_check(h_dev.packets_released == 1,
            "rx-only: the slots' packets went back");
    h_check(h_dev.closes == 1 && h_free_watched == 1 && h_dev.slot_frees == 1,
            "rx-only: closed once, and it and its slots freed once");
    h_retain_done();
}

/* Both sides, given back one at a time. */
static void case_retain_mixed(void)
{
    AmiSana2If *iface;

    printf("  a read and a write kept\n");
    iface = h_retain(H_READS, 1, FALSE);
    h_check(iface != NULL, "mixed: the interface opened");
    if (iface == NULL)
        return;

    h_check(!ami_sana2_close(iface), "mixed: the close is refused");
    h_check(ami_sana2_retained_holds("test.device", 0) ==
                (NETEVENT_HELD_RX | NETEVENT_HELD_TX),
            "mixed: held on both sides");

    h_dev.reads_held = 0;
    h_check(ami_sana2_retained_sweep(TRUE) == 1,
            "mixed: the reads back, the write still holds it");
    h_check(ami_sana2_retained_holds("test.device", 0) == NETEVENT_HELD_TX,
            "mixed: and it is held on the write side alone now");
    h_check(h_dev.closes == 0 && h_free_watched == 0,
            "mixed: not closed, not freed");

    h_dev.writes_held = 0;
    h_check(ami_sana2_retained_sweep(TRUE) == 0 && h_dev.closes == 1 &&
            h_free_watched == 1,
            "mixed: the write back, closed and freed once");
    h_retain_done();
}

/* No request out, and the reader not yet joined: still a hold. */
static void case_retain_reader_join(void)
{
    AmiSana2If *iface;

    printf("  a reader not yet joined, nothing at the device\n");
    iface = h_retain(0, 0, TRUE);
    h_check(iface != NULL, "join: the interface opened");
    if (iface == NULL)
        return;

    h_check(!ami_sana2_close(iface), "join: the close is refused");
    h_check(ami_sana2_retained_sweep(TRUE) == 1,
            "join: zero requests and the reader running is a hold");
    h_check(h_dev.closes == 0 && h_free_watched == 0,
            "join: not closed, not freed");

    h_dev.reader_running = FALSE;
    h_check(ami_sana2_retained_sweep(TRUE) == 0 && h_dev.closes == 1 &&
            h_free_watched == 1,
            "join: joined, closed and freed once");
    h_retain_done();
}

/* The same device and unit is not opened under a retained one; another is. */
static void case_retain_blocks_reopen(void)
{
    AmiSana2If *iface;
    AmiSana2If *other;
    LONG        err = 0;
    int         opens;

    printf("  opening a unit that is still held\n");
    iface = h_retain(0, 1, FALSE);
    h_check(iface != NULL, "reopen: the interface opened");
    if (iface == NULL)
        return;
    h_check(!ami_sana2_close(iface), "reopen: the close is refused");

    opens = h_dev.opens;
    h_config();
    strcpy(h_cfg.device, "DEVS:Networks/Test.device");
    h_check(ami_sana2_open(&h_cfg, &err) == NULL &&
                err == AMI_NET_ERR_RETAINED,
            "reopen: the held unit is refused with AMI_NET_ERR_RETAINED");
    h_check(h_dev.opens == opens, "reopen: and OpenDevice() was never called");

    /* The fake device's holds are device-wide: these two give theirs back. */
    h_dev.keeps_everything = FALSE;
    h_dev.writes_held      = 0;

    h_config();
    h_cfg.unit = 1;
    other = ami_sana2_open(&h_cfg, &err);
    h_check(other != NULL, "reopen: another unit of that device opens");
    if (other != NULL)
        h_check(ami_sana2_close(other), "reopen: and closes");

    h_config();
    strcpy(h_cfg.device, "other.device");
    other = ami_sana2_open(&h_cfg, &err);
    h_check(other != NULL, "reopen: another device on unit 0 opens");
    if (other != NULL)
        h_check(ami_sana2_close(other), "reopen: and closes");

    h_check(ami_sana2_retained_holds("test.device", 0) != 0,
            "reopen: the first is still held");
    (VOID)ami_sana2_retained_sweep(TRUE);
    h_config();
    other = ami_sana2_open(&h_cfg, &err);
    h_check(other != NULL, "reopen: once released, the unit opens again");
    if (other != NULL)
        h_check(ami_sana2_close(other), "reopen: and closes");

    h_check(h_dev.opens == h_dev.closes,
            "reopen: one CloseDevice() per OpenDevice(), none under a hold");
    h_retain_done();
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

/* Different devices may both report io_Unit == NULL for numeric unit 0.
   Neither device may suppress the other's S2_ONLINE or S2_OFFLINE. */
static void case_distinct_devices_same_unit(void)
{
    AmiSana2If *a;
    AmiSana2If *b;

    h_device_reset();

    a = h_bring_up_device("test.device", 0);
    b = h_bring_up_device("other.device", 0);
    h_check(a != NULL && b != NULL,
            "two devices with the same unit identity opened");
    if (a == NULL || b == NULL)
    {
        if (a != NULL) (VOID)h_tear_down(a);
        if (b != NULL) (VOID)h_tear_down(b);
        return;
    }

    h_check(h_dev.online_cmds == 2,
            "each device received its own S2_ONLINE");
    h_check(h_tear_down(a), "the first device closes");
    h_check(h_dev.offline_cmds == 1,
            "the first device received S2_OFFLINE despite the second");
    h_check(h_tear_down(b), "the second device closes");
    h_check(h_dev.offline_cmds == 2,
            "the second device also received S2_OFFLINE");
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

/* A status query does not run the device commands itself: it asks reader 0
   (ami_sana2_stats_request()) and reads the epoch.  Without a reader there is
   nothing to ask and the answer says so; with one the flag is raised and the
   epoch is still the reader's to move. */
static void case_stats_request(void)
{
    AmiSana2If *iface;
    struct Task fake_task;
    ULONG       epoch;

    h_device_reset();
    iface = h_bring_up_unit(0);
    h_check(iface != NULL, "the interface opened for a statistics request");
    if (iface == NULL)
        return;

    epoch = ami_sana2_stats_epoch(iface);
    h_check(ami_sana2_stats_request(iface) == FALSE,
            "with no reader running the request is refused");
    h_check(iface->stats_want == FALSE, "and no flag is left raised");
    h_check(ami_sana2_stats_epoch(iface) == epoch, "and the epoch stands");

    memset(&fake_task, 0, sizeof(fake_task));
    iface->reader.running   = TRUE;
    iface->reader.stop      = FALSE;
    iface->reader.task      = &fake_task;
    iface->reader.wake_mask = 1UL << 20;

    h_check(ami_sana2_stats_request(iface) == TRUE,
            "with reader 0 running the request is accepted");
    h_check(iface->stats_want == TRUE, "and the flag is raised for it");
    h_check(ami_sana2_stats_epoch(iface) == epoch,
            "and the epoch has not moved: only the reader moves it");

    iface->reader.running   = FALSE;
    iface->reader.task      = NULL;
    iface->reader.wake_mask = 0;
    iface->stats_want      = FALSE;

    h_check(h_tear_down(iface), "the statistics-request interface closes");
}

/* genet.device takes 8000 000B on an S2_ONLINE that follows an S2_OFFLINE
   (sana2_device.c, ami_sana2_keeps_online), so it is never sent one: a link
   cycle on it is one S2_ONLINE at the start and one at the return, and the
   close sends none either.  Any other name still gets the offline. */
static void case_keeps_online(void)
{
    AmiSana2If *iface;
    LONG        err = 0;

    h_device_reset();
    h_config();
    strcpy(h_cfg.device, "genet.device");

    iface = ami_sana2_open(&h_cfg, &err);
    h_check(iface != NULL, "genet.device opens");
    if (iface == NULL)
        return;

    h_check(iface->keep_online == TRUE, "and is marked to stay online");
    h_check(ami_sana2_online(iface) == 0, "it goes online");
    h_check(h_dev.online_cmds == 1, "with one S2_ONLINE");

    h_check(ami_sana2_offline(iface) == 0, "taking it offline succeeds");
    h_check(h_dev.offline_cmds == 0, "and issues NO S2_OFFLINE");
    h_check(iface->online == FALSE, "though the stack sees it down");

    h_check(ami_sana2_online(iface) == 0, "it comes back online");
    h_check(h_dev.online_cmds == 2, "with a second S2_ONLINE");
    h_check(iface->online == TRUE, "and the stack sees it up");

    h_check(h_tear_down(iface), "the genet interface closes");
    h_check(h_dev.offline_cmds == 0, "still no S2_OFFLINE at the close");

    h_check(ami_sana2_keeps_online("a2065.device") == FALSE,
            "an a2065.device is not kept online");
    h_check(ami_sana2_keeps_online("genet.device") == TRUE,
            "genet.device is");
}

/* ------------------------------------------------------------------ main -- */

/* IPREQUESTS, ARPREQUESTS and WRITEREQUESTS reach the interface at open, and
   WRITEREQUESTS above the ring, or unsaid, is the whole ring. */
static void case_request_counts(void)
{
    AmiSana2If *iface;
    LONG        err = 0;

    printf("  the interface file's request counts reach the interface\n");

    h_config();
    h_cfg.ip_requests    = 16;
    h_cfg.arp_requests   = 3;
    h_cfg.write_requests = 6;
    iface = ami_sana2_open(&h_cfg, &err);
    h_check(iface != NULL, "opened with request counts");
    if (iface != NULL)
    {
        h_check(iface->rx_want_ip == 16 && iface->rx_want_arp == 3,
                "IPREQUESTS and ARPREQUESTS are carried to the readers");
        h_check(iface->tx_slots == 6, "WRITEREQUESTS=6 claims six slots");
        (VOID)ami_sana2_close(iface);
    }

    h_config();
    h_cfg.write_requests = 64;
    iface = ami_sana2_open(&h_cfg, &err);
    h_check(iface != NULL && iface->tx_slots == AMI_SANA2_TX_SLOTS,
            "WRITEREQUESTS above the ring is the ring");
    if (iface != NULL)
        (VOID)ami_sana2_close(iface);

    h_config();
    iface = ami_sana2_open(&h_cfg, &err);
    h_check(iface != NULL && iface->tx_slots == AMI_SANA2_TX_SLOTS &&
            iface->rx_want_ip == 0 && iface->rx_want_arp == 0,
            "unsaid is the whole ring and a plan the readers make");
    if (iface != NULL)
        (VOID)ami_sana2_close(iface);
}

/* FILTER=EVERYTHING is SANA2OPF_PROM at OpenDevice(), and nothing else in
   the file touches the flags. */
static void case_filter_everything(void)
{
    AmiSana2If *iface;
    LONG        err = 0;

    printf("  FILTER=EVERYTHING opens the driver promiscuous\n");

    h_config();
    h_open_flags = 0xFFFFFFFFUL;
    iface = ami_sana2_open(&h_cfg, &err);
    h_check(iface != NULL && h_open_flags == 0UL,
            "an interface file without FILTER opens with no flags");
    if (iface != NULL)
        (VOID)ami_sana2_close(iface);

    h_config();
    h_cfg.promiscuous = TRUE;
    h_open_flags = 0xFFFFFFFFUL;
    iface = ami_sana2_open(&h_cfg, &err);
    h_check(iface != NULL && h_open_flags == (ULONG)SANA2OPF_PROM,
            "FILTER=EVERYTHING opens with SANA2OPF_PROM and only that");
    if (iface != NULL)
        (VOID)ami_sana2_close(iface);
}

/* ----------------------------------------------- the bounded S2_OFFLINE -- */

/* Bring up unit 0 with S2_OFFLINE answered the `mode` way. */
static AmiSana2If *h_bring_up_offline(int mode)
{
    AmiSana2If *iface;

    h_device_reset();
    iface = h_bring_up_unit(0);
    h_dev.offline_mode = mode;
    return iface;
}

/* Nothing of the wait may outlive it: the signal, the timer, the lock. */
static void h_offline_clean(const char *what)
{
    char line[128];

    snprintf(line, sizeof(line), "%s: every signal bit was freed", what);
    h_check(h_sigs_alloc == 0, line);
    snprintf(line, sizeof(line), "%s: timer.device closed as often as opened",
             what);
    h_check(h_timer_opens == h_timer_closes && h_timer_pending == NULL, line);
    snprintf(line, sizeof(line), "%s: Disable() and Enable() pair", what);
    h_check(h_disables == 0, line);
}

/* a. Answered, late: the same result as a DoIO() would have given. */
static void case_offline_bounded_late_reply(void)
{
    AmiSana2If *iface = h_bring_up_offline(H_OFF_LATE);

    h_check(iface != NULL, "late: the interface opened");
    if (iface == NULL)
        return;

    h_dev.offline_after_ms = 500;
    h_check(ami_sana2_offline(iface) == 0, "late: S2_OFFLINE succeeded");
    h_check(h_now_ms == 500, "late: returned as the reply landed");
    h_check(h_dev.aborts == 0, "late: nothing was aborted");
    h_check(!iface->offline_held, "late: the request is ours again");
    h_check(h_signals == 1, "late: the reply signalled once");
    h_check(h_events == 0, "late: no event");
    h_offline_clean("late");

    h_check(h_tear_down(iface), "late: the close frees the interface");
    h_check(h_dev.closes == 1, "late: and closes the device once");
}

/* a'. Answered inside BeginIO(): no timer is ever opened. */
static void case_offline_bounded_sync(void)
{
    AmiSana2If *iface = h_bring_up_offline(H_OFF_SYNC);

    h_check(iface != NULL, "sync: the interface opened");
    if (iface == NULL)
        return;

    h_check(ami_sana2_offline(iface) == 0, "sync: S2_OFFLINE succeeded");
    h_check(h_timer_opens == 0, "sync: timer.device was not opened");
    h_check(h_now_ms == 0, "sync: no time passed");
    h_offline_clean("sync");
    h_check(h_tear_down(iface), "sync: the close frees the interface");
}

/* d. Errors come back as before: a refusal is reported, UNIT_OFFLINE is
   not a refusal. */
static void case_offline_bounded_errors(void)
{
    AmiSana2If *iface = h_bring_up_offline(H_OFF_LATE);

    h_check(iface != NULL, "errors: the interface opened");
    if (iface == NULL)
        return;

    h_dev.offline_after_ms   = 100;
    h_dev.offline_error      = (LONG)S2ERR_BAD_STATE;
    h_dev.offline_wire_error = S2WERR_GENERIC_ERROR;
    h_check(ami_sana2_offline(iface) == (LONG)S2ERR_BAD_STATE,
            "errors: io_Error is returned");
    h_check(h_last_event == NETEVENT_OFFLINE_FAILED,
            "errors: and recorded as NETEVENT_OFFLINE_FAILED");
    h_offline_clean("errors");

    (VOID)ami_sana2_online(iface);
    h_events                 = 0;
    h_dev.offline_wire_error = S2WERR_UNIT_OFFLINE;
    h_check(ami_sana2_offline(iface) == 0,
            "errors: S2WERR_UNIT_OFFLINE is not a failure");
    h_check(h_events == 0, "errors: and records nothing");
    h_offline_clean("errors, unit offline");

    h_dev.offline_error      = 0;
    h_dev.offline_wire_error = 0;
    h_check(h_tear_down(iface), "errors: the close frees the interface");
}

/* b. Ignored until aborted: the deadline, then the abort's answer. */
static void case_offline_bounded_abort(void)
{
    AmiSana2If *iface = h_bring_up_offline(H_OFF_ABORTABLE);

    h_check(iface != NULL, "abort: the interface opened");
    if (iface == NULL)
        return;

    h_check(ami_sana2_offline(iface) == (LONG)IOERR_ABORTED,
            "abort: IOERR_ABORTED comes back from the device");
    h_check(h_now_ms == AMI_SANA2_OFFLINE_WAIT_MS + 10UL,
            "abort: after the command's deadline and the abort's reply");
    h_check(h_dev.aborts == 1, "abort: AbortIO() once");
    h_check(!iface->offline_held, "abort: the request is ours again");
    h_check(h_last_event == NETEVENT_OFFLINE_FAILED,
            "abort: reported as NETEVENT_OFFLINE_FAILED");
    h_offline_clean("abort");

    h_check(h_tear_down(iface), "abort: the close frees the interface");
    h_check(h_dev.closes == 1, "abort: and closes the device once");
}

/* c. Answers neither: both deadlines, then the request is left to the device
   with the interface around it; the late reply queues and is reclaimed. */
static void case_offline_bounded_deaf(void)
{
    AmiSana2If *iface = h_bring_up_offline(H_OFF_DEAF);
    unsigned long signals;

    h_check(iface != NULL, "deaf: the interface opened");
    if (iface == NULL)
        return;
    h_free_watch   = iface;
    h_free_watched = 0;

    h_check(ami_sana2_offline(iface) == (LONG)IOERR_ABORTED,
            "deaf: the bounded offline fails");
    h_check(h_now_ms == AMI_SANA2_OFFLINE_WAIT_MS + AMI_SANA2_OFFLINE_ABORT_MS,
            "deaf: after exactly both deadlines");
    h_check(h_dev.aborts == 1, "deaf: AbortIO() once");
    h_check(iface->offline_held && iface->offline_orphans == 1,
            "deaf: the request is left with the device, and counted");
    h_check(iface->offline_port.mp_Flags == PA_IGNORE &&
            iface->offline_port.mp_SigTask == NULL,
            "deaf: its port signals nobody");
    h_offline_clean("deaf");

    /* No link-up while the device holds it, and so no second request. */
    h_check(ami_sana2_online(iface) == (LONG)IOERR_UNITBUSY,
            "deaf: online refused while the S2_OFFLINE is held");
    h_check(h_dev.online_cmds == 1, "deaf: no S2_ONLINE issued");
    h_check(!iface->online &&
            iface->offline_state == AMI_SANA2_OFFLINE_ISSUED,
            "deaf: still offline, S2_OFFLINE still the last word");
    h_check(ami_sana2_offline(iface) == 0 && h_dev.offline_cmds == 1,
            "deaf: no second S2_OFFLINE while the first is held");

    h_check(!h_tear_down(iface), "deaf: the close refuses");
    h_check(h_dev.closes == 0, "deaf: CloseDevice() was NOT called");
    h_check(h_free_watched == 0, "deaf: the interface was NOT freed");
    h_check(ami_sana2_retained_count() == 1, "deaf: it is retained");
    h_retained_iface = iface;

    h_check(ami_sana2_retained_sweep(TRUE) == 1,
            "deaf: the sweep keeps it while the device holds the request");
    h_check(h_dev.closes == 0, "deaf: still no CloseDevice()");

    /* The late reply: queued on the PA_IGNORE port, no signal. */
    signals = h_signals;
    h_offline_reply();
    h_check(h_signals == signals && h_ignored == 1,
            "deaf: the late reply signalled nobody");

    h_check(ami_sana2_retained_sweep(TRUE) == 0,
            "deaf: the next sweep reclaims it");
    h_check(h_dev.closes == 1, "deaf: and closes the device once");
    h_check(h_free_watched == 1, "deaf: and frees the interface once");
    h_retained_iface = NULL;
    h_free_watch     = NULL;
}

/* Online after an abandoned S2_OFFLINE: refused while the device holds it,
   then, once the late reply is back, S2_ONLINE in the order the device
   completed them; the interface then closes normally. */
static void case_offline_held_then_online(void)
{
    AmiSana2If *iface = h_bring_up_offline(H_OFF_DEAF);

    h_check(iface != NULL, "held-online: the interface opened");
    if (iface == NULL)
        return;

    h_check(ami_sana2_offline(iface) == (LONG)IOERR_ABORTED &&
            iface->offline_held, "held-online: S2_OFFLINE abandoned");
    h_check(ami_sana2_online(iface) == (LONG)IOERR_UNITBUSY,
            "held-online: online refused");
    h_check(h_dev.online_cmds == 1 && !h_dev.unit_online,
            "held-online: no S2_ONLINE reached the device");
    h_check(!iface->online && !iface->unit_counted &&
            iface->offline_state == AMI_SANA2_OFFLINE_ISSUED,
            "held-online: nothing joined or marked up");

    h_offline_reply();
    h_check(ami_sana2_online(iface) == 0, "held-online: online after the reply");
    h_check(!iface->offline_held, "held-online: the reply was reaped first");
    h_check(h_dev.online_cmds == 2 && h_dev.unit_online,
            "held-online: S2_ONLINE issued after S2_OFFLINE completed");
    h_check(iface->online && iface->offline_state == AMI_SANA2_OFFLINE_UP,
            "held-online: up");

    h_dev.offline_mode = H_OFF_SYNC;
    h_check(h_tear_down(iface), "held-online: closes normally");
    h_check(h_dev.closes == 1 && ami_sana2_retained_count() == 0,
            "held-online: CloseDevice() once, nothing retained");
}

/* A sibling on the same unit may not online it past another interface's
   abandoned S2_OFFLINE either. */
static void case_offline_held_sibling_online(void)
{
    AmiSana2If *a = h_bring_up_offline(H_OFF_DEAF);
    AmiSana2If *b;

    h_check(a != NULL, "held-sibling: the first interface opened");
    if (a == NULL)
        return;

    h_check(ami_sana2_offline(a) == (LONG)IOERR_ABORTED && a->offline_held,
            "held-sibling: its S2_OFFLINE abandoned");
    b = h_bring_up_unit(0);
    h_check(b == NULL, "held-sibling: the sibling's online is refused");
    h_check(h_dev.online_cmds == 1 && !h_dev.unit_online,
            "held-sibling: no S2_ONLINE reached the device");

    h_offline_reply();
    b = h_bring_up_unit(0);
    h_check(b != NULL, "held-sibling: the sibling comes up after the reply");
    h_check(!a->offline_held, "held-sibling: it reaped the first one's reply");
    h_check(h_dev.online_cmds == 2 && h_dev.unit_online,
            "held-sibling: S2_ONLINE after S2_OFFLINE completed");

    h_dev.offline_mode = H_OFF_SYNC;
    h_check(h_tear_down(a), "held-sibling: the first closes");
    if (b != NULL)
        h_check(h_tear_down(b), "held-sibling: the sibling closes");
    h_check(ami_sana2_retained_count() == 0, "held-sibling: nothing retained");
}

/* The reply lands inside the Disable() that would detach the port: it is
   taken as an answer, and the port never goes PA_IGNORE. */
static void case_offline_race_before_detach(void)
{
    AmiSana2If *iface = h_bring_up_offline(H_OFF_DEAF);

    h_check(iface != NULL, "race before: the interface opened");
    if (iface == NULL)
        return;

    h_reply_before_disable = TRUE;
    h_check(ami_sana2_offline(iface) == 0,
            "race before: the reply's result is returned");
    h_check(!iface->offline_held && iface->offline_orphans == 0,
            "race before: nothing is abandoned");
    h_check(h_signals == 1 && h_ignored == 0,
            "race before: it signalled the waiter");
    h_offline_clean("race before");

    h_check(h_tear_down(iface), "race before: the close frees the interface");
    h_check(h_dev.closes == 1, "race before: and closes the device once");
}

/* The reply lands the instant after the Enable(): PA_IGNORE already, so it
   only queues, and the close that follows reclaims it. */
static void case_offline_race_after_detach(void)
{
    AmiSana2If *iface = h_bring_up_offline(H_OFF_DEAF);

    h_check(iface != NULL, "race after: the interface opened");
    if (iface == NULL)
        return;

    h_reply_after_enable = TRUE;
    h_check(ami_sana2_offline(iface) == (LONG)IOERR_ABORTED,
            "race after: the wait gave up");
    h_check(iface->offline_held, "race after: the request was abandoned");
    h_check(h_signals == 0 && h_ignored == 1,
            "race after: the reply signalled nobody");
    h_offline_clean("race after");

    h_check(h_tear_down(iface), "race after: the close reclaims and frees");
    h_check(h_dev.closes == 1, "race after: and closes the device once");
}

/* No timer.device: nothing to bound a wait with, so no wait at all.  The
   abort is asked for at once and the request abandoned if it is not back. */
static void case_offline_no_timer(void)
{
    AmiSana2If *iface = h_bring_up_offline(H_OFF_ABORTABLE);

    h_check(iface != NULL, "no timer: the interface opened");
    if (iface == NULL)
        return;

    h_timer_open_fails = TRUE;
    h_check(ami_sana2_offline(iface) == (LONG)IOERR_ABORTED,
            "no timer: the offline fails");
    h_check(h_now_ms == 0, "no timer: without waiting");
    h_check(h_dev.aborts == 1 && iface->offline_held,
            "no timer: aborted, and abandoned");
    h_offline_clean("no timer");

    h_offline_reply();
    h_check(h_tear_down(iface), "no timer: the close reclaims and frees");
    h_check(h_dev.closes == 1, "no timer: and closes the device once");
}

static void case_bursty_wifi_reads(void)
{
    printf("  WiFiPi has a burst-aware read default without changing wired cards\n");
    h_check(ami_sana2_default_ip_reads("DEVS:Networks/anxwifipi.device") == 128,
            "the shipped WiFiPi driver gets 128 reads by default");
    h_check(ami_sana2_default_ip_reads("WIFIPI.DEVICE") == 128,
            "the upstream driver name is recognized case-insensitively");
    h_check(ami_sana2_default_ip_reads("genet.device") == 0,
            "a wired driver keeps the ordinary BPS ladder");
    h_check(ami_sana2_default_ip_reads(NULL) == 0,
            "an unnamed driver keeps the ordinary BPS ladder");

    /* The DEVICE= lines Install-AmiNetXDuo writes for Wi-Fi, system layout
       and self-contained drawer: neither carries IPREQUESTS. */
    h_check(ami_sana2_default_ip_reads("DEVS:Networks/anxwifipi.device") == 128 &&
            ami_sana2_default_ip_reads("SYS:AmiNetXDuo/Devs/Networks/anxwifipi.device") == 128,
            "both installer-written Wi-Fi DEVICE= forms get 128");

    /* Every DEVICE= in the archive's Examples/NetInterfaces, and our own
       wired drivers, keep the ladder. */
    {
        static const char *const wired[] =
        {
            "a2065.device", "ariadne.device", "ariadne_ii.device",
            "cnet.device", "hydra.device", "uaenet.device",
            "x-surf.device", "x-surf-100.device",
            "DEVS:Networks/anxnet.device", "anxgenet.device",
            "anxzz9000.device", "anxwifipi.device.old"
        };
        unsigned i;
        BOOL     all = TRUE;

        for (i = 0; i < sizeof(wired) / sizeof(wired[0]); i++)
            if (ami_sana2_default_ip_reads(wired[i]) != 0)
                all = FALSE;
        h_check(all, "every shipped wired profile keeps the ordinary ladder");
    }
}

int main(void)
{
    printf("sana2 device: open, online, offline, close\n");

    case_healthy();
    case_out_of_service_still_offlines();
    case_offline_is_idempotent();
    case_never_online();
    case_online_rearms();
    case_device_keeps_everything();
    case_retain_tx_only();
    case_retain_rx_only();
    case_retain_mixed();
    case_retain_reader_join();
    case_retain_blocks_reopen();
    case_offline_refused();
    case_shared_unit();
    case_distinct_units();
    case_distinct_devices_same_unit();
    case_special_recovery_stats();
    case_stats_request();
    case_keeps_online();
    case_request_counts();
    case_filter_everything();
    case_offline_bounded_sync();
    case_offline_bounded_late_reply();
    case_offline_bounded_errors();
    case_offline_bounded_abort();
    case_offline_bounded_deaf();
    case_offline_held_then_online();
    case_offline_held_sibling_online();
    case_offline_race_before_detach();
    case_offline_race_after_detach();
    case_offline_no_timer();
    case_bursty_wifi_reads();

    h_check(ami_sana2_retained_count() == 0, "nothing is left retained");
    h_check(h_ports_made > 0, "reply ports were created");
    h_check(h_ports_live == 0, "every reply port was deleted");

    printf("%lu checks, %lu failures, %s\n", h_checks, h_failures,
           (h_failures == 0) ? "PASS" : "FAIL");

    return (h_failures == 0) ? 0 : 1;
}

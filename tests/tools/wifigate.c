/*
 * WifiGate: a Wi-Fi SANA-II unit made at run time, "wifigate.device", in
 * front of a wired one, and the DHCP lease timed against its association.
 *
 * Not associated, the unit drops every frame the stack writes, as a radio
 * with no access point does, and answers S2_ONEVENT and the signal the way
 * prism2.device 2.9 does (level-triggered CONNECT/DISCONNECT, LINKUP/
 * LINKDOWN).  Associated, every request goes through to the inner device
 * unchanged, on a copy of its own, and the reply comes back the same way.
 *
 * The run: AddNetInterface <IFACE> with the unit not associated, then the
 * association ASSOC seconds after that started, then GetNetStatus
 * CHECK=DEFAULTROUTE once a second until a lease gives a default route.
 *
 *   WifiGate IFACE/K,INNER/K,ASSOC/N,LIMIT/N,WAIT/N,LEAVE/N
 *
 * LEAVE: after the lease, the association goes for that many seconds and
 * comes back; the lease must still be there, or come back inside LIMIT.
 *
 * Output is key=value, then "<N> checks, <M> failures".
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
#include <dos/rdargs.h>
#include <utility/tagitem.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <inline/alib.h>

#include <stddef.h>

#include "sana2_device.h"

#define WG_NAME              "wifigate.device"
#define S2_GETSIGNALQUALITY  0xc010
#define S2EVENT_CONNECT      (1L << 9)
#define S2EVENT_DISCONNECT   (1L << 10)
#define S2EVENT_LINKUP       (1L << 16)
#define S2EVENT_LINKDOWN     (1L << 17)
#define WG_KNOWN (S2EVENT_ERROR | S2EVENT_TX | S2EVENT_RX | S2EVENT_ONLINE | \
                  S2EVENT_OFFLINE | S2EVENT_BUFF | S2EVENT_HARDWARE |        \
                  S2EVENT_SOFTWARE | S2EVENT_CONNECT | S2EVENT_DISCONNECT |  \
                  S2EVENT_LINKUP | S2EVENT_LINKDOWN)

typedef struct WgDev
{
    struct Device   dd;
    struct Unit     unit;
    struct List     events;         /* S2_ONEVENT held                   */
    struct MinList  flight;         /* copies the inner device holds     */
    struct MsgPort *port;           /* their replies                     */
    BOOL            online, assoc;
    ULONG           dropped, forwarded, opens;
} WgDev;

/* What one opener of this unit opened on the inner device. */
typedef struct WgOpener
{
    struct IOSana2Req inner;
} WgOpener;

/* A request on its way through the inner device. */
typedef struct WgShadow
{
    struct IOSana2Req  req;
    struct MinNode     link;
    struct IOSana2Req *caller;
} WgShadow;

static WgDev             *wg;
static volatile BOOL      wg_stop, wg_alive;
static const char         wg_name[] = WG_NAME;
static const char         wg_id[]   = WG_NAME " 1.0 (DHCP association test)";
static char               wg_inner[64] = "a2065.device";

static VOID wg_new_list(struct List *l)
{
    l->lh_Head = (struct Node *)&l->lh_Tail;
    l->lh_Tail = NULL;
    l->lh_TailPred = (struct Node *)&l->lh_Head;
    l->lh_Type = NT_MESSAGE;
}

static VOID wg_reply(struct IOSana2Req *io, LONG err, ULONG wire)
{
    io->ios2_Req.io_Error = (BYTE)err;
    io->ios2_WireError = wire;
    if ((io->ios2_Req.io_Flags & IOF_QUICK) == 0)
        ReplyMsg(&io->ios2_Req.io_Message);
}

/* The unit's state as prism2.device answers S2_ONEVENT. */
static ULONG wg_level(VOID)
{
    ULONG ev = wg->online ? S2EVENT_ONLINE : S2EVENT_OFFLINE;

    ev |= wg->assoc ? (S2EVENT_CONNECT | S2EVENT_LINKUP)
                    : (S2EVENT_DISCONNECT | S2EVENT_LINKDOWN);
    return ev;
}

/* Held events waiting for any bit of ev are answered with those bits. */
static VOID wg_report(ULONG ev)
{
    struct Node *n, *next;

    Forbid();
    for (n = wg->events.lh_Head; (next = n->ln_Succ) != NULL; n = next)
    {
        struct IOSana2Req *io = (struct IOSana2Req *)n;

        if ((io->ios2_WireError & ev) != 0)
        {
            Remove(n);
            io->ios2_Req.io_Error = 0;
            io->ios2_WireError &= ev;
            ReplyMsg(&io->ios2_Req.io_Message);
        }
    }
    Permit();
}

static VOID wg_set_assoc(BOOL on)
{
    Forbid();
    wg->assoc = on;
    Permit();
    wg_report(on ? (S2EVENT_CONNECT | S2EVENT_LINKUP)
                 : (S2EVENT_DISCONNECT | S2EVENT_LINKDOWN));
}

static ULONG wg_copy(register APTR to __asm("a0"),
                     register APTR from __asm("a1"),
                     register ULONG len __asm("d0"))
{
    (VOID)to;
    (VOID)from;
    (VOID)len;
    return FALSE;
}

static struct TagItem wg_nocopy[] =
{
    { S2_CopyToBuff,   (ULONG)wg_copy },
    { S2_CopyFromBuff, (ULONG)wg_copy },
    { TAG_DONE,        0 }
};

static struct Device *wg_open(register struct Device *dev __asm("a6"),
                              register struct IOSana2Req *io __asm("a1"),
                              register ULONG unit __asm("d0"),
                              register ULONG flags __asm("d1"))
{
    WgOpener *op;

    io->ios2_Req.io_Error = IOERR_OPENFAIL;
    if (unit != 0)
        return dev;
    op = (WgOpener *)AllocVec(sizeof(*op), MEMF_PUBLIC | MEMF_CLEAR);
    if (op == NULL)
        return dev;
    /* The caller's buffer management goes to the inner device: its copy
       hooks are called with the caller's ios2_Data, as without the gate. */
    CopyMem(io, &op->inner, sizeof(op->inner));
    if (OpenDevice((CONST_STRPTR)wg_inner, 0, (struct IORequest *)&op->inner,
                   flags) != 0)
    {
        FreeVec(op);
        return dev;
    }
    io->ios2_Req.io_Device = dev;
    io->ios2_Req.io_Unit = &wg->unit;
    io->ios2_BufferManagement = op;
    io->ios2_Req.io_Error = 0;
    dev->dd_Library.lib_OpenCnt++;
    wg->opens++;
    return dev;
}

static BPTR wg_close(register struct Device *dev __asm("a6"),
                     register struct IOSana2Req *io __asm("a1"))
{
    WgOpener *op = (WgOpener *)io->ios2_BufferManagement;

    if (op != NULL)
    {
        CloseDevice((struct IORequest *)&op->inner);
        FreeVec(op);
    }
    io->ios2_Req.io_Device = (struct Device *)-1;
    io->ios2_Req.io_Unit = (struct Unit *)-1;
    if (dev->dd_Library.lib_OpenCnt != 0) dev->dd_Library.lib_OpenCnt--;
    return (BPTR)0;
}

static BPTR wg_expunge(register struct Device *dev __asm("a6"))
{
    (VOID)dev;
    return (BPTR)0;
}

static ULONG wg_null(VOID)
{
    return 0;
}

static VOID wg_forward(struct IOSana2Req *io)
{
    WgOpener *op = (WgOpener *)io->ios2_BufferManagement;
    WgShadow *sh = (WgShadow *)AllocVec(sizeof(*sh), MEMF_PUBLIC | MEMF_CLEAR);

    if (sh == NULL)
    {
        wg_reply(io, S2ERR_NO_RESOURCES, S2WERR_GENERIC_ERROR);
        return;
    }
    CopyMem(io, &sh->req, sizeof(sh->req));
    sh->req.ios2_Req.io_Message.mn_Node.ln_Type = NT_MESSAGE;
    sh->req.ios2_Req.io_Message.mn_ReplyPort = wg->port;
    sh->req.ios2_Req.io_Device = op->inner.ios2_Req.io_Device;
    sh->req.ios2_Req.io_Unit = op->inner.ios2_Req.io_Unit;
    sh->req.ios2_BufferManagement = op->inner.ios2_BufferManagement;
    sh->caller = io;
    /* IOF_QUICK as the caller sent it: a2065.device answers S2_DEVICEQUERY
       only when asked as DoIO() asks, and keeps one sent without it.  The
       relayer cannot run before the flag is settled. */
    Forbid();
    AddTail((struct List *)&wg->flight, (struct Node *)&sh->link);
    wg->forwarded++;
    BeginIO((struct IORequest *)&sh->req);
    if ((sh->req.ios2_Req.io_Flags & IOF_QUICK) != 0)
    {
        /* Done inside BeginIO(): the answer goes back the same way. */
        struct Message msg = io->ios2_Req.io_Message;
        struct Device *dev = io->ios2_Req.io_Device;
        struct Unit   *unit = io->ios2_Req.io_Unit;

        Remove((struct Node *)&sh->link);
        CopyMem(&sh->req, io, sizeof(*io));
        io->ios2_Req.io_Message = msg;
        io->ios2_Req.io_Device = dev;
        io->ios2_Req.io_Unit = unit;
        io->ios2_BufferManagement = op;
        Permit();
        FreeVec(sh);
        return;
    }
    io->ios2_Req.io_Flags &= (UBYTE)~IOF_QUICK;
    Permit();
}

static VOID wg_begin_io(register struct Device *dev __asm("a6"),
                        register struct IOSana2Req *io __asm("a1"))
{
    UWORD cmd = io->ios2_Req.io_Command;

    (VOID)dev;
    io->ios2_Req.io_Message.mn_Node.ln_Type = NT_MESSAGE;
    switch (cmd)
    {
        case S2_ONEVENT:
        {
            ULONG want = io->ios2_WireError, now;

            if ((want & ~WG_KNOWN) != 0)
            {
                wg_reply(io, S2ERR_NOT_SUPPORTED, S2WERR_BAD_EVENT);
                return;
            }
            Forbid();
            now = wg_level() & want;
            if (now == 0)
            {
                io->ios2_Req.io_Flags &= (UBYTE)~IOF_QUICK;
                AddTail(&wg->events, &io->ios2_Req.io_Message.mn_Node);
            }
            Permit();
            if (now != 0)
                wg_reply(io, 0, now);
            return;
        }
        case S2_GETSIGNALQUALITY:
            /* prism2.device: the signal whenever online. */
            if (!wg->online)
                wg_reply(io, S2ERR_OUTOFSERVICE, S2WERR_UNIT_OFFLINE);
            else
            {
                if (io->ios2_StatData != NULL)
                {
                    ((LONG *)io->ios2_StatData)[0] = wg->assoc ? -52 : -95;
                    ((LONG *)io->ios2_StatData)[1] = -95;
                }
                wg_reply(io, 0, 0);
            }
            return;
        case CMD_WRITE:
        case S2_BROADCAST:
        case S2_MULTICAST:
            if (!wg->assoc)
            {
                /* Into the air: no access point hears it. */
                Forbid();
                wg->dropped++;
                Permit();
                wg_reply(io, 0, 0);
                return;
            }
            break;
        default:
            break;
    }
    wg_forward(io);
}

static LONG wg_abort_io(register struct Device *dev __asm("a6"),
                        register struct IOSana2Req *io __asm("a1"))
{
    struct Node *n;
    WgShadow    *found = NULL;
    LONG         rc = -1;

    (VOID)dev;
    Forbid();
    for (n = wg->events.lh_Head; n->ln_Succ != NULL; n = n->ln_Succ)
        if (n == &io->ios2_Req.io_Message.mn_Node)
        {
            Remove(n);
            io->ios2_Req.io_Error = IOERR_ABORTED;
            ReplyMsg(&io->ios2_Req.io_Message);
            Permit();
            return 0;
        }
    for (n = (struct Node *)wg->flight.mlh_Head; n->ln_Succ != NULL;
         n = n->ln_Succ)
    {
        WgShadow *sh = (WgShadow *)((UBYTE *)n - offsetof(WgShadow, link));

        if (sh->caller == io)
        {
            found = sh;
            break;
        }
    }
    if (found != NULL)
    {
        AbortIO((struct IORequest *)&found->req);
        rc = 0;
    }
    Permit();
    return rc;
}

static const APTR wg_vectors[] =
{
    (APTR)wg_open, (APTR)wg_close, (APTR)wg_expunge, (APTR)wg_null,
    (APTR)wg_begin_io, (APTR)wg_abort_io, (APTR)-1
};

/* A reply from the inner device back to the request it was a copy of. */
static VOID wg_relay(WgShadow *sh)
{
    struct IOSana2Req *io = sh->caller;
    struct Message     msg = io->ios2_Req.io_Message;
    struct Device     *dev = io->ios2_Req.io_Device;
    struct Unit       *unit = io->ios2_Req.io_Unit;
    APTR               bm = io->ios2_BufferManagement;
    UWORD              cmd = sh->req.ios2_Req.io_Command;
    BYTE               err = sh->req.ios2_Req.io_Error;

    Forbid();
    Remove((struct Node *)&sh->link);
    Permit();
    CopyMem(&sh->req, io, sizeof(*io));
    io->ios2_Req.io_Message = msg;
    io->ios2_Req.io_Device = dev;
    io->ios2_Req.io_Unit = unit;
    io->ios2_BufferManagement = bm;
    FreeVec(sh);
    if (err == 0 && (cmd == S2_ONLINE || cmd == S2_CONFIGINTERFACE) &&
        !wg->online)
    {
        wg->online = TRUE;
        wg_report(S2EVENT_ONLINE);
    }
    else if (err == 0 && cmd == S2_OFFLINE && wg->online)
    {
        wg->online = FALSE;
        wg_report(S2EVENT_OFFLINE);
    }
    ReplyMsg(&io->ios2_Req.io_Message);
}

/* The inner device's replies, relayed.  Polled: a tick of latency is
   nothing to a DHCP exchange. */
static VOID wg_relayer(VOID)
{
    struct MsgPort *port = CreateMsgPort();

    Forbid();
    wg->port = port;
    wg_alive = TRUE;
    Permit();
    while (!wg_stop)
    {
        struct Message *m;

        while (port != NULL && (m = GetMsg(port)) != NULL)
            wg_relay((WgShadow *)m);
        Delay(1);
    }
    Forbid();
    wg_alive = FALSE;
}

static BOOL wg_install(VOID)
{
    WgDev *d = (WgDev *)MakeLibrary((APTR)wg_vectors, NULL, NULL,
                                    sizeof(WgDev), (BPTR)0);

    if (d == NULL) return FALSE;
    wg_new_list(&d->events);
    wg_new_list((struct List *)&d->flight);
    d->dd.dd_Library.lib_Node.ln_Type = NT_DEVICE;
    d->dd.dd_Library.lib_Node.ln_Name = (char *)wg_name;
    d->dd.dd_Library.lib_Flags = LIBF_SUMUSED | LIBF_CHANGED;
    d->dd.dd_Library.lib_Version = 1;
    d->dd.dd_Library.lib_IdString = (char *)wg_id;
    wg = d;
    wg_stop = FALSE;
    if (CreateNewProcTags(NP_Entry, (ULONG)wg_relayer,
                          NP_Name, (ULONG)"wifigate relayer",
                          NP_Priority, 1, TAG_DONE) == NULL)
        return FALSE;
    while (wg->port == NULL) Delay(1);
    AddDevice(&d->dd);
    return TRUE;
}

/* ---------------------------------------------------------- the run -- */

static int checks, failures;

/* A step, appended to SYS:wifigate.marks and closed: it outlives a crash
   that takes stdout.txt with it. */
static VOID mark(const char *what)
{
    BPTR fh = Open((CONST_STRPTR)"SYS:wifigate.marks", MODE_READWRITE);
    LONG n = 0;

    if (fh == (BPTR)0) return;
    (VOID)Seek(fh, 0, OFFSET_END);
    while (what[n] != '\0') n++;
    (VOID)Write(fh, (APTR)what, n);
    (VOID)Write(fh, (APTR)"\n", 1);
    Close(fh);
}

static VOID check(BOOL ok, const char *what)
{
    checks++;
    if (!ok) failures++;
    Printf((CONST_STRPTR)"%s %s\n", (LONG)(ok ? "ok  " : "FAIL"), (LONG)what);
}

/* Guest seconds, in ticks, since a fixed point. */
static ULONG wg_now(VOID)
{
    struct DateStamp ds;

    DateStamp(&ds);
    return (ULONG)ds.ds_Days * 4320000UL + (ULONG)ds.ds_Minute * 3000UL +
           (ULONG)ds.ds_Tick;
}

static LONG wg_run(const char *command)
{
    BPTR nil = Open((CONST_STRPTR)"NIL:", MODE_NEWFILE);
    LONG rc;

    rc = SystemTags((CONST_STRPTR)command, SYS_Output, (ULONG)nil,
                    SYS_Input, 0, TAG_DONE);
    if (nil != (BPTR)0) Close(nil);
    return rc;
}

/* The command's own output, into this program's. */
static LONG wg_say(const char *command)
{
    return SystemTags((CONST_STRPTR)command, SYS_Output, (ULONG)Output(),
                      SYS_Input, 0, TAG_DONE);
}

static BOOL wg_routed(VOID)
{
    return wg_run("SYS:GetNetStatus CHECK=DEFAULTROUTE QUIET") == 0;
}

/* Seconds from now until a default route, polled once a second; -1 when
   none came inside limit. */
static LONG wg_wait_route(LONG limit)
{
    ULONG start = wg_now();

    for (;;)
    {
        ULONG t = (wg_now() - start) / 50UL;

        if (wg_routed())
            return (LONG)t;
        if ((LONG)t >= limit)
            return -1;
        Delay(50);
    }
}

#define TEMPLATE "IFACE/K,INNER/K,ASSOC/N,LIMIT/N,WAIT/N,LEAVE/N"

int main(void)
{
    LONG           args[6] = { 0, 0, 0, 0, 0, 0 };
    struct RDArgs *rda = ReadArgs((CONST_STRPTR)TEMPLATE, args, NULL);
    const char    *iface = "wifi0";
    LONG           assoc_s = 70, limit = 15, wait_s = 150, leave = 0;
    struct IOSana2Req pin;
    ULONG          t0, t_assoc, dropped_before;
    LONG           rc, lease_after, back_after = -2;
    BOOL           early = FALSE;
    char           cmd[96];

    if (rda == NULL)
    {
        PutStr((CONST_STRPTR)"bad arguments\n0 checks, 1 failures\n");
        return RETURN_ERROR;
    }
    if (args[0] != 0) iface = (const char *)args[0];
    if (args[1] != 0)
    {
        const char *s = (const char *)args[1];
        ULONG       i;

        for (i = 0; s[i] != '\0' && i + 1 < sizeof(wg_inner); i++)
            wg_inner[i] = s[i];
        wg_inner[i] = '\0';
    }
    if (args[2] != 0) assoc_s = *(LONG *)args[2];
    if (args[3] != 0) limit = *(LONG *)args[3];
    if (args[4] != 0) wait_s = *(LONG *)args[4];
    if (args[5] != 0) leave = *(LONG *)args[5];

    /* The inner device stays open, so an opener of the gate never waits
       for it to load inside OpenDevice(). */
    {
        ULONG i;
        UBYTE *p = (UBYTE *)&pin;

        for (i = 0; i < sizeof(pin); i++) p[i] = 0;
    }
    pin.ios2_Req.io_Message.mn_Length = sizeof(pin);
    pin.ios2_BufferManagement = wg_nocopy;
    if (OpenDevice((CONST_STRPTR)wg_inner, 0, (struct IORequest *)&pin, 0) != 0)
    {
        Printf((CONST_STRPTR)"inner=%s open=failed\n0 checks, 1 failures\n",
               (LONG)wg_inner);
        FreeArgs(rda);
        return RETURN_FAIL;
    }
    if (!wg_install())
    {
        PutStr((CONST_STRPTR)"could not make " WG_NAME "\n0 checks, 1 failures\n");
        FreeArgs(rda);
        return RETURN_FAIL;
    }

    t0 = wg_now();
    {
        static const char head[] = "SYS:AddNetInterface ";
        ULONG i = 0, k;

        for (k = 0; head[k] != '\0'; k++) cmd[i++] = head[k];
        for (k = 0; iface[k] != '\0' && i + 8 < sizeof(cmd); k++)
            cmd[i++] = iface[k];
        cmd[i] = '\0';
    }
    rc = wg_say(cmd);
    Printf((CONST_STRPTR)"addnetinterface_rc=%ld addnetinterface_s=%ld\n", rc,
           (LONG)((wg_now() - t0) / 50UL));

    /* Not associated: no lease can come, whatever DHCP sends. */
    while ((LONG)((wg_now() - t0) / 50UL) < assoc_s)
    {
        if (wg_routed()) early = TRUE;
        Delay(50);
    }
    dropped_before = wg->dropped;
    mark("assoc");
    t_assoc = wg_now();
    wg_set_assoc(TRUE);
    lease_after = wg_wait_route(wait_s);
    Printf((CONST_STRPTR)"inner=%s assoc_s=%ld dropped_before_assoc=%lu "
           "forwarded=%lu lease_after_assoc_s=%ld limit_s=%ld\n",
           (LONG)wg_inner, assoc_s, dropped_before, wg->forwarded,
           lease_after, limit);
    (VOID)t_assoc;
    check(!early, "no default route while not associated");
    check(dropped_before > 0, "the stack sent while not associated");
    check(lease_after >= 0, "a lease after the association");
    check(lease_after >= 0 && lease_after <= limit,
          "the lease inside the limit after the association");

    if (leave > 0 && lease_after >= 0)
    {
        mark("leave");
        wg_set_assoc(FALSE);
        Delay(leave * 50);
        mark("rejoin");
        wg_set_assoc(TRUE);
        back_after = wg_wait_route(limit);
        Printf((CONST_STRPTR)"leave_s=%ld route_after_rejoin_s=%ld\n", leave,
               back_after);
        check(back_after >= 0, "a default route after leave and rejoin");
    }

    /* The unit's code is this program's: the stack lets go of it before
       the program goes. */
    mark("netshutdown");
    (VOID)wg_run("SYS:NetShutdown");
    mark("netshutdown done");
    {
        LONG n;

        for (n = 0; n < 500 && wg->dd.dd_Library.lib_OpenCnt != 0; n++)
            Delay(10);
        check(wg->dd.dd_Library.lib_OpenCnt == 0,
              "the stack closed the unit at shutdown");
        if (wg->dd.dd_Library.lib_OpenCnt != 0)
        {
            /* Still open: its code must stay, so the program does not go
               through the teardown below.  The verdict is already failed. */
            Printf((CONST_STRPTR)"%ld checks, %ld failures\n", (LONG)checks,
                   (LONG)failures);
            Flush(Output());
            FreeArgs(rda);
            return RETURN_FAIL;
        }
    }
    /* Nothing the inner device still holds may reply into a port that is
       gone. */
    {
        LONG n;

        for (n = 0; n < 250 && wg->flight.mlh_Head->mln_Succ != NULL; n++)
            Delay(2);
        check(wg->flight.mlh_Head->mln_Succ == NULL,
              "nothing left in the inner device at shutdown");
        if (wg->flight.mlh_Head->mln_Succ != NULL)
        {
            mark("flight not empty");
            Printf((CONST_STRPTR)"%ld checks, %ld failures\n", (LONG)checks,
                   (LONG)failures);
            Flush(Output());
            FreeArgs(rda);
            return RETURN_FAIL;
        }
    }
    mark("teardown");
    wg_stop = TRUE;
    while (wg_alive) Delay(2);
    Forbid();
    Remove(&wg->dd.dd_Library.lib_Node);
    Permit();
    FreeMem((APTR)((ULONG)wg - wg->dd.dd_Library.lib_NegSize),
            (ULONG)(wg->dd.dd_Library.lib_NegSize + wg->dd.dd_Library.lib_PosSize));
    CloseDevice((struct IORequest *)&pin);
    mark("done");

    Printf((CONST_STRPTR)"%ld checks, %ld failures\n", (LONG)checks,
           (LONG)failures);
    FreeArgs(rda);
    return failures == 0 ? RETURN_OK : RETURN_FAIL;
}

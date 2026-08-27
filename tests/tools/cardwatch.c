/*
 * CardWatch: what the driver does when the card is pulled out and put back.
 *
 * WHY THIS EXISTS
 *
 * netdev_pcmcia.c installs card.resource removal and insertion callbacks,
 * pc_on_removed() and pc_on_inserted(), and until now neither had ever fired:
 * Amiberry could not eject a PCMCIA network card, so the whole removal and
 * reinsertion path -- the offline event, the release that stays queued for the
 * next card, the reconfigure, the reattach, and the return to online -- was
 * code nothing had run.
 *
 * This program is the witness.  It holds the unit open for the whole cycle,
 * which matters twice over: the worker only brings the card back online if an
 * opener is still there (netdev_pcmcia.c checks nu_Openers), and a device with
 * no openers could be expunged between the eject and the insert, which would
 * test a fresh probe rather than a reinsertion.
 *
 * The order is fixed by SANA-II's own rule that a state event already true
 * completes at once:
 *
 *   1. Queue S2EVENT_ONLINE.  It completes immediately or the card was never
 *      up, and either way the starting state is recorded rather than assumed.
 *   2. Queue S2EVENT_OFFLINE and wait.  The harness ejects the card;
 *      pc_on_removed() runs and netdev_pcmcia_detached() posts it.
 *   3. Queue S2EVENT_ONLINE and wait.  The harness puts the card back;
 *      pc_on_inserted() runs, the worker reconfigures and reattaches, and
 *      netdev_online() posts it.
 *
 * Every wait is bounded by timer.device, because a path that has never run is
 * exactly the one that hangs, and a hung guest reports nothing at all.  A wait
 * that expires is printed as a timeout and the program carries on to the next
 * step, so one dead transition does not hide the state of the others.
 *
 * Output is key=value on stdout with a RESULT= line last.
 *
 * SPDX-License-Identifier: MIT
 */

#include <exec/types.h>
#include <exec/io.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <devices/timer.h>

#include <proto/exec.h>
#include <proto/dos.h>

#include "aminetxduo/anxnet.h"
#include "sana2_device.h"

static const char version_tag[] __attribute__((used)) =
    "$VER: CardWatch 1.0 (27.8.2026)";

#define TEMPLATE  "DEVICE/K,UNIT/K/N,OFFWAIT/K/N,ONWAIT/K/N,READY/K"

/*
 * The harness has to know when the wait is actually in place before it pulls
 * the card, and it cannot learn that from stdout: the guest's transcript is
 * one file the command shell writes when a command ENDS, and this command does
 * not end until the whole cycle is over.  So the signal is a file.  Creating
 * and closing it puts it on the drive the host can see at once, and the host
 * polls for it.
 */
#define READY_DEFAULT   "DH0:cardwatch.step"

static const char *ready_path = READY_DEFAULT;

/* Write one word into the ready file, replacing what was there.  A failure to
   write it is reported and is not fatal: the harness falls back to its own
   timing, and a run that ejected slightly too early fails a check rather than
   producing nothing. */
static VOID say_ready(const char *what)
{
    BPTR fh = Open((CONST_STRPTR)ready_path, MODE_NEWFILE);

    if (fh == 0)
    {
        Printf((CONST_STRPTR)"ready_write=0 step=%s\n", (LONG)what);
        return;
    }
    (VOID)FPuts(fh, (CONST_STRPTR)what);
    (VOID)FPutC(fh, '\n');
    (VOID)Close(fh);
    Printf((CONST_STRPTR)"ready_write=1 step=%s\n", (LONG)what);
}

/* Long enough that a slow reconfigure is not called a hang, short enough that
   a real hang still leaves the run time to print. */
#define DEFAULT_OFFWAIT   60
#define DEFAULT_ONWAIT    60

static struct MsgPort     *s2_port;
static struct IOSana2Req  *s2_req;
static struct MsgPort     *tm_port;
static struct timerequest *tm_req;
static BOOL                dev_open;
static BOOL                timer_open;

static int checks;
static int failed;

static VOID check(const char *what, int got, int want)
{
    checks++;
    if (got == want)
    {
        Printf((CONST_STRPTR)"ok   %s=%ld\n", (LONG)what, (LONG)got);
        return;
    }
    failed++;
    Printf((CONST_STRPTR)"FAIL %s=%ld want=%ld\n", (LONG)what, (LONG)got,
           (LONG)want);
}

/*
 * Queue one S2_ONEVENT and wait up to `secs` for it.  Returns 1 when the event
 * arrived, 0 on timeout.  On timeout the request is aborted and waited for, so
 * the caller can reuse it: an IORequest left in flight over a return is a
 * message port that the device writes to after this program has gone.
 */
/*
 * A completed request is not necessarily an event.  S2_ONEVENT answers a mask
 * it does not understand with io_Error S2ERR_NOT_SUPPORTED and ios2_WireError
 * S2WERR_BAD_EVENT, and it answers it AT ONCE -- so a driver that refused
 * every mask would complete inside SendIO() and be read here as three
 * instant, perfect transitions.  The refusal is recorded and reported as no
 * event, which is the only reading that cannot pass by accident.
 */
static LONG last_ioerr;

static int arrived(void)
{
    last_ioerr = (LONG)s2_req->ios2_Req.io_Error;
    if (last_ioerr == 0)
        return 1;

    Printf((CONST_STRPTR)"event_refused=%ld wire=%08lx\n",
           (LONG)last_ioerr, (LONG)s2_req->ios2_WireError);
    return 0;
}

static int wait_event(ULONG mask, ULONG secs, ULONG *got)
{
    ULONG sigs;

    *got = 0;
    last_ioerr = 0;

    s2_req->ios2_Req.io_Command = S2_ONEVENT;
    s2_req->ios2_Req.io_Flags   = 0;
    s2_req->ios2_WireError      = mask;
    SendIO((struct IORequest *)s2_req);

    /* CheckIO first: a state event that is already true is replied inside
       SendIO(), and the timer below would then be started for nothing. */
    if (CheckIO((struct IORequest *)s2_req) != NULL)
    {
        (VOID)WaitIO((struct IORequest *)s2_req);
        *got = s2_req->ios2_WireError;
        return arrived();
    }

    tm_req->tr_node.io_Command = TR_ADDREQUEST;
    tm_req->tr_time.tv_secs    = secs;
    tm_req->tr_time.tv_micro   = 0;
    SendIO((struct IORequest *)tm_req);

    /*
     * WAIT IN A LOOP, and believe CheckIO() rather than the signal.
     *
     * WaitIO() on a request that is already replied returns without ever
     * calling Wait(), so the port signal it was replied with STAYS SET.  Step
     * 1 of this program always completes inside SendIO() -- an online card
     * answers S2EVENT_ONLINE at once -- so step 2 arrived here with the
     * SANA-II port bit already set, Wait() returned immediately, the request
     * was still in flight, and a 90-second budget expired in under a
     * millisecond.  The whole cycle then ran to the end before the harness
     * had polled once, and every removal was reported as a timeout.
     */
    for (;;)
    {
        sigs = Wait((1UL << s2_port->mp_SigBit) | (1UL << tm_port->mp_SigBit));
        (VOID)sigs;

        if (CheckIO((struct IORequest *)s2_req) != NULL)
        {
            AbortIO((struct IORequest *)tm_req);
            (VOID)WaitIO((struct IORequest *)tm_req);
            (VOID)WaitIO((struct IORequest *)s2_req);
            *got = s2_req->ios2_WireError;
            return arrived();
        }

        if (CheckIO((struct IORequest *)tm_req) != NULL)
            break;
    }

    /* The timer won: the event did not arrive inside the budget. */
    (VOID)WaitIO((struct IORequest *)tm_req);
    AbortIO((struct IORequest *)s2_req);
    (VOID)WaitIO((struct IORequest *)s2_req);

    return 0;
}

static VOID cleanup(VOID)
{
    if (dev_open)
        CloseDevice((struct IORequest *)s2_req);
    if (s2_req != NULL)
        DeleteIORequest((struct IORequest *)s2_req);
    if (s2_port != NULL)
        DeleteMsgPort(s2_port);
    if (timer_open)
        CloseDevice((struct IORequest *)tm_req);
    if (tm_req != NULL)
        DeleteIORequest((struct IORequest *)tm_req);
    if (tm_port != NULL)
        DeleteMsgPort(tm_port);
}

int main(void)
{
    LONG        args[5];
    struct RDArgs *rda;
    const char *devname = "anxnet.device";
    ULONG       unit    = 8UL * ANXNET_UNIT_PIN;
    ULONG       offwait = DEFAULT_OFFWAIT;
    ULONG       onwait  = DEFAULT_ONWAIT;
    ULONG       got;
    LONG        err;
    int         ok;

    args[0] = args[1] = args[2] = args[3] = args[4] = 0;
    rda = ReadArgs((CONST_STRPTR)TEMPLATE, args, NULL);
    if (rda == NULL)
    {
        PrintFault(IoErr(), (CONST_STRPTR)"CardWatch");
        return RETURN_ERROR;
    }
    if (args[0] != 0) devname = (const char *)args[0];
    if (args[1] != 0) unit    = (ULONG)(*(LONG *)args[1]);
    if (args[2] != 0) offwait = (ULONG)(*(LONG *)args[2]);
    if (args[3] != 0) onwait  = (ULONG)(*(LONG *)args[3]);
    if (args[4] != 0) ready_path = (const char *)args[4];

    Printf((CONST_STRPTR)"device=%s\n", (LONG)devname);
    Printf((CONST_STRPTR)"unit=%lu\n", (LONG)unit);
    Printf((CONST_STRPTR)"offwait=%lu\n", (LONG)offwait);
    Printf((CONST_STRPTR)"onwait=%lu\n", (LONG)onwait);
    Printf((CONST_STRPTR)"ready_file=%s\n", (LONG)ready_path);

    tm_port = CreateMsgPort();
    s2_port = CreateMsgPort();
    if (tm_port == NULL || s2_port == NULL)
    {
        Printf((CONST_STRPTR)"reason=no message port\nRESULT=fail\n");
        cleanup(); FreeArgs(rda); return RETURN_FAIL;
    }

    tm_req = (struct timerequest *)
             CreateIORequest(tm_port, (ULONG)sizeof(*tm_req));
    s2_req = (struct IOSana2Req *)
             CreateIORequest(s2_port, (ULONG)sizeof(*s2_req));
    if (tm_req == NULL || s2_req == NULL)
    {
        Printf((CONST_STRPTR)"reason=no IO request\nRESULT=fail\n");
        cleanup(); FreeArgs(rda); return RETURN_FAIL;
    }

    if (OpenDevice((CONST_STRPTR)TIMERNAME, UNIT_VBLANK,
                   (struct IORequest *)tm_req, 0) != 0)
    {
        Printf((CONST_STRPTR)"reason=timer.device would not open\nRESULT=fail\n");
        cleanup(); FreeArgs(rda); return RETURN_FAIL;
    }
    timer_open = TRUE;

    err = (LONG)OpenDevice((CONST_STRPTR)devname, unit,
                           (struct IORequest *)s2_req, 0);
    Printf((CONST_STRPTR)"open_error=%ld\n", (LONG)err);
    if (err != 0)
    {
        /* No card in the slot at all is not a failed measurement.  The
           harness has nothing to eject and says so. */
        Printf((CONST_STRPTR)"reason=the unit would not open, so there is no card to pull\n");
        Printf((CONST_STRPTR)"RESULT=skip\n");
        cleanup(); FreeArgs(rda); return RETURN_WARN;
    }
    dev_open = TRUE;

    /* ---- 1. where we start ------------------------------------------- */
    ok = wait_event(S2EVENT_ONLINE, 5, &got);
    Printf((CONST_STRPTR)"start_online=%ld\n", (LONG)ok);
    Printf((CONST_STRPTR)"start_event=%08lx\n", (LONG)got);
    check("started_online", ok, 1);
    if (ok == 0)
    {
        Printf((CONST_STRPTR)"reason=the card was not online to begin with\n");
        Printf((CONST_STRPTR)"checks=%ld\nfailed=%ld\nRESULT=fail\n",
               (LONG)checks, (LONG)failed);
        cleanup(); FreeArgs(rda); return RETURN_FAIL;
    }

    /* The harness watches for this and only then ejects the card, so the
       wait below is already in place when the socket changes. */
    say_ready("eject");

    /*
     * ---- 2. the card comes out ----------------------------------------
     *
     * S2EVENT_OFFLINE ALONE, and the delivered mask is checked for it.  The
     * removal posts OFFLINE|ERROR|HARDWARE together, so asking for all three
     * looks equivalent -- and is not: netdev_event() replies with the mask
     * that was POSTED rather than the part of it the request asked for, and
     * one bit in common is a match.  A plain receive error posts ERROR|RX,
     * which shares the ERROR bit, so the wider request was satisfied by
     * ordinary traffic on a busy bridge.  Measured: the negative control,
     * with nothing ejected at all, reported a removal with mask $05.
     */
    ok = wait_event(S2EVENT_OFFLINE, offwait, &got);
    if (ok != 0 && (got & (ULONG)S2EVENT_OFFLINE) == 0)
    {
        Printf((CONST_STRPTR)"offline_wrong_event=%08lx\n", (LONG)got);
        ok = 0;
    }
    Printf((CONST_STRPTR)"offline_event=%ld\n", (LONG)ok);
    Printf((CONST_STRPTR)"offline_mask=%08lx\n", (LONG)got);
    check("saw_removal", ok, 1);

    say_ready("insert");

    /* ---- 3. and goes back in ----------------------------------------- */
    ok = wait_event(S2EVENT_ONLINE, onwait, &got);
    if (ok != 0 && (got & (ULONG)S2EVENT_ONLINE) == 0)
    {
        Printf((CONST_STRPTR)"online_wrong_event=%08lx\n", (LONG)got);
        ok = 0;
    }
    Printf((CONST_STRPTR)"online_event=%ld\n", (LONG)ok);
    Printf((CONST_STRPTR)"online_mask=%08lx\n", (LONG)got);
    check("came_back_online", ok, 1);

    Printf((CONST_STRPTR)"checks=%ld\n", (LONG)checks);
    Printf((CONST_STRPTR)"failed=%ld\n", (LONG)failed);
    say_ready("done");

    Printf((CONST_STRPTR)"RESULT=%s\n",
           (LONG)(failed == 0 ? (char *)"pass" : (char *)"fail"));

    cleanup();
    FreeArgs(rda);

    return failed == 0 ? RETURN_OK : RETURN_FAIL;
}

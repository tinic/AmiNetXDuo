/*
 * AmiNetXDuo, SANA-II carrier: whether the card sees a network.
 *
 * S2_ONLINE says a driver is ready, not that a cable is in, and
 * nx_interface_link_up is the stack's intent: Online and Offline set it and
 * wait on it.  Neither can say "no cable".  A driver that reads its PHY
 * answers S2_ONEVENT for S2EVENT_CONNECT | S2EVENT_DISCONNECT -- the pair
 * wireless drivers post for association -- at once with the state that
 * holds (anxnet.device: netdev_event.c).  These are states, not edges: asked
 * again for the same pair, the driver answers again at once, and a reader
 * that re-asked would spin.  So the first request names both, and every one
 * after it names only the other state, which completes when the cable
 * changes.  An answer that changes nothing ends the watch rather than
 * re-asking.  iface->carrier: 0 the driver does not say, 1 no link, 2 link.
 *
 * A driver that knows neither event should refuse them, but one can hold the
 * request instead, and a2065.device 2.16 gives it back to neither AbortIO()
 * nor CMD_FLUSH.  Such a request is parked on the interface rather than
 * counted against the reader: the interface still goes offline and online,
 * which is what matters, and only the cable stays unknown.
 *
 * Nothing NetX Duo does follows it.  ShowNetStatus reports it, through
 * NETSTATUS_IF_CARRIERKNOWN and NETSTATUS_IF_CARRIER.
 *
 * SPDX-License-Identifier: MIT
 */

#include "sana2_internal.h"

#include "aminetxduo/nxstatus.h"
#include <exec/memory.h>
#include <proto/exec.h>

/* Ticks of 40 ms a stop waits for each of AbortIO() and CMD_FLUSH.  A driver
   that honours either answers in the first; one that does not costs this
   twice on every Offline, so it is short. */
#define AMI_SANA2_CARRIER_REAP_TRIES 3

/* A refusal (S2ERR_NOT_SUPPORTED from a driver that knows no such event,
   IOERR_NOCMD from one without S2_ONEVENT) leaves the carrier not known and
   is not asked again. */
static VOID ami_sana2_carrier_post(AmiSana2Reader *rd, ULONG events)
{
    struct IOSana2Req *req = &rd->carrier_io->req;

    *req = rd->iface->templ;
    req->ios2_Req.io_Message.mn_Node.ln_Type = NT_MESSAGE;
    req->ios2_Req.io_Message.mn_ReplyPort    = &rd->carrier_io->port;
    req->ios2_Req.io_Message.mn_Length       = (UWORD)sizeof(*req);
    req->ios2_Req.io_Command                 = S2_ONEVENT;
    req->ios2_Req.io_Flags                   = 0;
    req->ios2_Req.io_Error                   = 0;
    req->ios2_WireError                      = events;

    rd->carrier_busy = TRUE;
    SendIO((struct IORequest *)req);
}

VOID ami_sana2_carrier_start(AmiSana2Reader *rd)
{
    AmiSana2If        *iface = rd->iface;
    AmiSana2CarrierIO *io;
    BYTE               bit;

    rd->carrier_io     = NULL;
    rd->carrier_sigbit = -1;
    rd->carrier_busy   = FALSE;
    rd->carrier_mask   = 0UL;
    iface->carrier     = 0;

    if (iface->carrier_parked != NULL)
    {
        /* Still the driver's: not asked twice. */
        if (CheckIO((struct IORequest *)&iface->carrier_parked->req) == NULL)
            return;
        FreeMem(iface->carrier_parked, sizeof(AmiSana2CarrierIO));
        iface->carrier_parked = NULL;
    }

    bit = AllocSignal(-1);
    if (bit < 0)
        return;                         /* not known, as before */

    io = (AmiSana2CarrierIO *)AllocMem(sizeof(*io), MEMF_PUBLIC | MEMF_CLEAR);
    if (io == NULL)
    {
        FreeSignal(bit);
        return;
    }

    ami_sana2_port_init(&io->port, rd->task, bit, PA_SIGNAL);
    rd->carrier_io     = io;
    rd->carrier_sigbit = bit;
    rd->carrier_mask   = 1UL << (ULONG)bit;
    ami_sana2_carrier_post(rd, S2EVENT_CONNECT | S2EVENT_DISCONNECT);
}

/* noinline: once per change of cable, not once per frame
   (tools/check-hotpath-budget.sh, _ami_sana2_rx_thread). */
VOID __attribute__((noinline)) ami_sana2_carrier_pass(AmiSana2Reader *rd)
{
    AmiSana2If     *iface = rd->iface;
    struct Message *msg;

    while ((msg = GetMsg(&rd->carrier_io->port)) != NULL)
    {
        struct IOSana2Req *req  = (struct IOSana2Req *)msg;
        ULONG              wire = req->ios2_WireError;
        UBYTE              was  = iface->carrier;

        rd->carrier_busy = FALSE;

        if (req->ios2_Req.io_Error != 0)
        {
            iface->carrier = 0;
            continue;
        }

        if ((wire & S2EVENT_DISCONNECT) != 0)
            iface->carrier = 1;
        else if ((wire & S2EVENT_CONNECT) != 0)
            iface->carrier = 2;
        else
        {
            /* Answered with neither: asked again, it would answer again. */
            iface->carrier = 0;
            continue;
        }

        if (iface->carrier == was)
        {
            /* Asked for the other state and told this one: asked again,
               the driver would answer again.  The last answer stands. */
            AMI_WARN("sana2: %s answered a link event with no change; "
                     "link state no longer followed", iface->device);
            continue;
        }

        AMI_INFO("sana2: %s unit %ld: %s", iface->device, (long)iface->unit,
                 iface->carrier == 2 ? "link" : "no link (cable?)");

        if (!rd->stop)
            ami_sana2_carrier_post(rd, (iface->carrier == 2)
                                           ? S2EVENT_DISCONNECT
                                           : S2EVENT_CONNECT);
    }
}

/* Up to `tries` ticks of 40 ms for the request to come back. */
static VOID ami_sana2_carrier_reap(AmiSana2Reader *rd, UWORD tries)
{
    UWORD t;

    for (t = 0; rd->carrier_busy; t++)
    {
        if (GetMsg(&rd->carrier_io->port) != NULL)
        {
            rd->carrier_busy = FALSE;
            break;
        }
        if (t >= tries)
            break;
        AMI_NX_CLEANUP(tx_thread_sleep(2));
    }
}

/*
 * After the reads are reclaimed, on the reader's own task.  AbortIO(), then
 * CMD_FLUSH, as for a read.  A request still held after both is parked: its
 * port stops signalling this task, whose bit is freed here.
 */
VOID ami_sana2_carrier_stop(AmiSana2Reader *rd)
{
    AmiSana2CarrierIO *io = rd->carrier_io;

    if (io == NULL)
        return;

    if (rd->carrier_busy)
    {
        (VOID)AbortIO((struct IORequest *)&io->req);
        ami_sana2_carrier_reap(rd, AMI_SANA2_CARRIER_REAP_TRIES);
    }
    if (rd->carrier_busy)
    {
        struct IOSana2Req flush = rd->iface->templ;

        (VOID)ami_sana2_command(rd->iface, &flush, CMD_FLUSH);
        ami_sana2_carrier_reap(rd, AMI_SANA2_CARRIER_REAP_TRIES);
    }

    rd->iface->carrier = 0;

    Disable();
    io->port.mp_Flags = PA_IGNORE;
    Enable();
    FreeSignal(rd->carrier_sigbit);

    if (rd->carrier_busy)
    {
        AMI_INFO("sana2: %s keeps its link-event request; the cable stays "
                 "unknown", rd->iface->device);
        rd->iface->carrier_parked = io;
    }
    else
    {
        FreeMem(io, sizeof(*io));
    }

    rd->carrier_io     = NULL;
    rd->carrier_sigbit = -1;
    rd->carrier_mask   = 0UL;
    rd->carrier_busy   = FALSE;
}

/* At close, after CloseDevice(): a parked request that came back is freed,
   one a driver still holds is left where it can write. */
VOID ami_sana2_carrier_release(AmiSana2If *iface)
{
    AmiSana2CarrierIO *io = iface->carrier_parked;

    if (io == NULL)
        return;

    iface->carrier_parked = NULL;
    if (CheckIO((struct IORequest *)&io->req) != NULL)
        FreeMem(io, sizeof(*io));
}

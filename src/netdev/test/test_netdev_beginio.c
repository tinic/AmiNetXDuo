/*
 * anxnet.device, what BeginIO does to a request before it dispatches it.
 *
 * THE DEFECT.  netdev_begin_io() cleared ios2_WireError for the life of the
 * driver, on the reasonable-sounding grounds that ios2_WireError is an
 * output.  For one command it is not: S2_ONEVENT carries the event mask the
 * caller is waiting for IN that field.  netdev_cmds.c was then handed a mask
 * of zero, correctly refused it as naming no condition, and every S2_ONEVENT
 * ever issued to this device came back S2ERR_NOT_SUPPORTED with
 * S2WERR_BAD_EVENT.  The events the driver posts had no reachable waiter.
 *
 * test_netdev_event.c stayed green over all of it because it calls
 * netdev_event.c directly and never enters BeginIO, so nothing in the tree
 * had an opinion about what BeginIO writes.  This enters the way Exec does,
 * through the real netdev_begin_io(), and asserts the general rule rather
 * than the one field: BeginIO may clear io_Error, it may clear ios2_WireError
 * for a command that does not read it, and it may touch nothing else at all.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <string.h>

#include <proto/exec.h>

#include "netdev_internal.h"

static int failures;
static int checks;


static void expect(int ok, const char *what)
{
    checks++;
    if (ok)
        return;

    printf("FAIL %s\n", what);
    failures++;
}


static void expect_u32(const char *what, unsigned long got, unsigned long want)
{
    checks++;
    if (got == want)
        return;

    printf("FAIL %s: got 0x%lx, want 0x%lx\n", what, got, want);
    failures++;
}


/* ------------------------------------------------------- the dispatcher -- */
/*
 * netdev_cmds.c replaced by a recorder.  What matters is not what the
 * dispatcher does with the request but what it is handed: the defect was
 * entirely in the fields as they arrived here.
 */

static int           seen_perform;
static int           seen_abort;
static NetdevOpener *seen_op;
static ULONG         seen_wire_error;
static BYTE          seen_io_error;
static BOOL          abort_answer = TRUE;

VOID netdev_perform(NetdevOpener *op, struct IOSana2Req *io)
{
    seen_perform++;
    seen_op         = op;
    seen_wire_error = io->ios2_WireError;
    seen_io_error   = io->ios2_Req.io_Error;
}

BOOL netdev_abort(NetdevOpener *op, struct IOSana2Req *io)
{
    (VOID)io;
    seen_abort++;
    seen_op = op;
    return abort_answer;
}


/* ------------------------------------------------------------ the caller -- */

static NetdevOpener opener;
static struct Device fake_device;

/*
 * A request with every byte set, so that any field BeginIO writes shows up as
 * a difference and not as a value that happened to match.  0xa5 is not a
 * plausible io_Error or wire error either way.
 */
static void fill(struct IOSana2Req *io, ULONG command, ULONG wire_error,
                 int attached)
{
    memset(io, 0xa5, sizeof(*io));

    io->ios2_Req.io_Command = (UWORD)command;
    io->ios2_Req.io_Error   = (BYTE)0x5a;
    io->ios2_Req.io_Unit    = attached ? &opener.op_Unit : NULL;
    io->ios2_WireError      = wire_error;
}


/* Every SANA-II command this device is ever handed, S2_ONEVENT apart. */
static const ULONG other_commands[] = {
    CMD_READ, CMD_WRITE, CMD_FLUSH,
    S2_DEVICEQUERY, S2_GETSTATIONADDRESS, S2_CONFIGINTERFACE,
    S2_ADDMULTICASTADDRESS, S2_DELMULTICASTADDRESS,
    S2_MULTICAST, S2_BROADCAST,
    S2_TRACKTYPE, S2_UNTRACKTYPE,
    S2_GETTYPESTATS, S2_GETSPECIALSTATS, S2_GETGLOBALSTATS,
    S2_READORPHAN, S2_ONLINE, S2_OFFLINE
};

#define OTHER_N ((int)(sizeof(other_commands) / sizeof(other_commands[0])))


/*
 * THE REGRESSION.  S2_ONEVENT's mask reaches the dispatcher as the caller
 * wrote it.  Every bit of it, because a driver that preserved only the events
 * it knows about would refuse a mask that also names one it does not, and the
 * specification says to refuse only a mask with no supported event in it.
 */
static void a_onevent_keeps_its_mask(void)
{
    static const ULONG masks[] = {
        S2EVENT_ERROR, S2EVENT_TX, S2EVENT_RX, S2EVENT_ONLINE,
        S2EVENT_OFFLINE, S2EVENT_BUFF, S2EVENT_HARDWARE, S2EVENT_SOFTWARE,
        S2EVENT_ONLINE | S2EVENT_OFFLINE,
        0xffffffffUL,
        0UL                             /* naming nothing is the caller's */
    };
    int i;

    for (i = 0; i < (int)(sizeof(masks) / sizeof(masks[0])); i++)
    {
        struct IOSana2Req io;

        seen_perform = 0;
        fill(&io, (ULONG)S2_ONEVENT, masks[i], 1);
        netdev_begin_io(&fake_device, &io);

        expect(seen_perform == 1, "S2_ONEVENT reached the dispatcher");
        expect_u32("S2_ONEVENT mask as the dispatcher received it",
                   seen_wire_error, masks[i]);
        expect_u32("S2_ONEVENT mask still in the request afterwards",
                   io.ios2_WireError, masks[i]);
    }
}


/* And the field really is an output for everything else: a caller that left
   rubbish in it must not see that rubbish reported back as a wire error. */
static void b_every_other_command_gets_a_cleared_wire_error(void)
{
    int i;

    for (i = 0; i < OTHER_N; i++)
    {
        struct IOSana2Req io;

        seen_perform = 0;
        fill(&io, other_commands[i], 0xdeadbeefUL, 1);
        netdev_begin_io(&fake_device, &io);

        expect(seen_perform == 1, "the command reached the dispatcher");
        expect_u32("wire error cleared before dispatch", seen_wire_error, 0UL);
    }
}


/* io_Error is cleared for every command including S2_ONEVENT: a request is
   dispatched with no verdict on it yet. */
static void c_io_error_is_always_cleared(void)
{
    struct IOSana2Req io;
    int i;

    seen_perform = 0;
    fill(&io, (ULONG)S2_ONEVENT, S2EVENT_ONLINE, 1);
    netdev_begin_io(&fake_device, &io);
    expect_u32("io_Error cleared for S2_ONEVENT",
               (unsigned long)(UBYTE)seen_io_error, 0UL);

    for (i = 0; i < OTHER_N; i++)
    {
        seen_perform = 0;
        fill(&io, other_commands[i], 0UL, 1);
        netdev_begin_io(&fake_device, &io);
        expect_u32("io_Error cleared before dispatch",
                   (unsigned long)(UBYTE)seen_io_error, 0UL);
    }
}


/*
 * THE GENERAL RULE, which is the one that would have caught this without
 * anybody knowing about S2_ONEVENT in advance: BeginIO writes io_Error, it
 * writes ios2_WireError for a command that does not read it, and it writes
 * nothing else.  Every other byte of the request is the caller's, including
 * on the commands whose fields are outputs -- filling those is the
 * dispatcher's job, after it has read the inputs beside them.
 */
static void d_beginio_touches_nothing_else(void)
{
    int i;

    for (i = 0; i <= OTHER_N; i++)
    {
        struct IOSana2Req io;
        struct IOSana2Req want;
        ULONG             command = (i == OTHER_N) ? (ULONG)S2_ONEVENT
                                                   : other_commands[i];

        fill(&io, command, 0x12345678UL, 1);
        want = io;
        want.ios2_Req.io_Error = 0;
        if (command != (ULONG)S2_ONEVENT)
            want.ios2_WireError = 0;

        netdev_begin_io(&fake_device, &io);

        expect(memcmp(&io, &want, sizeof(io)) == 0,
               "BeginIO wrote a field that is the caller's");
    }
}


/*
 * io_Unit is what carries the opener, and BeginIO is reached before OpenDevice
 * has set one on a request the caller built by hand.  NULL and -1 both mean
 * "no opener", and the field work still has to happen: the dispatcher is what
 * turns that into IOERR_OPENFAIL, and it cannot do so on a request whose
 * io_Error it was never allowed to see.
 */
static void e_an_unattached_request_still_dispatches(void)
{
    struct IOSana2Req io;

    seen_perform = 0;
    seen_op      = &opener;
    fill(&io, (ULONG)S2_ONLINE, 0xdeadbeefUL, 0);
    netdev_begin_io(&fake_device, &io);
    expect(seen_perform == 1 && seen_op == NULL,
           "a request with no unit dispatches with no opener");
    expect_u32("and its wire error was still cleared", seen_wire_error, 0UL);

    seen_perform = 0;
    seen_op      = &opener;
    fill(&io, (ULONG)S2_ONEVENT, S2EVENT_ERROR, 0);
    io.ios2_Req.io_Unit = (struct Unit *)-1;
    netdev_begin_io(&fake_device, &io);
    expect(seen_perform == 1 && seen_op == NULL,
           "io_Unit == -1 dispatches with no opener");
    expect_u32("and its S2_ONEVENT mask survived", seen_wire_error,
               (unsigned long)S2EVENT_ERROR);
}


/* An attached request reaches the dispatcher as its own opener, not as some
   other slot in the unit: NETDEV_OPENER() has to invert io_Unit exactly. */
static void f_the_opener_round_trips(void)
{
    struct IOSana2Req io;

    seen_perform = 0;
    seen_op      = NULL;
    fill(&io, CMD_READ, 0UL, 1);
    netdev_begin_io(&fake_device, &io);
    expect(seen_op == &opener, "io_Unit resolved back to its own opener");
}


/*
 * AbortIO shares the entry shape and answers Exec's convention: 0 when the
 * request was found and stopped, -1 when it was not.  It leaves both fields
 * alone -- an abort that cleared io_Error would erase the IOERR_ABORTED the
 * abort path is in the middle of setting.
 */
static void g_abort_io(void)
{
    struct IOSana2Req io;
    struct IOSana2Req want;

    fill(&io, CMD_READ, 0x12345678UL, 1);
    want = io;

    seen_abort   = 0;
    seen_op      = NULL;
    abort_answer = TRUE;
    expect(netdev_abort_io(&fake_device, &io) == 0,
           "AbortIO answers 0 when the request was stopped");
    expect(seen_abort == 1 && seen_op == &opener,
           "AbortIO reached the abort path with the right opener");
    expect(memcmp(&io, &want, sizeof(io)) == 0,
           "AbortIO wrote a field of the request itself");

    abort_answer = FALSE;
    expect(netdev_abort_io(&fake_device, &io) == -1,
           "AbortIO answers -1 when there was nothing to stop");

    seen_abort = 0;
    seen_op    = &opener;
    fill(&io, CMD_READ, 0UL, 0);
    (VOID)netdev_abort_io(&fake_device, &io);
    expect(seen_abort == 1 && seen_op == NULL,
           "AbortIO on an unattached request carries no opener");
}


int main(void)
{
    memset(&opener, 0, sizeof(opener));
    memset(&fake_device, 0, sizeof(fake_device));

    printf("anxnet.device BeginIO/AbortIO field handling\n");

    a_onevent_keeps_its_mask();
    b_every_other_command_gets_a_cleared_wire_error();
    c_io_error_is_always_cleared();
    d_beginio_touches_nothing_else();
    e_an_unattached_request_still_dispatches();
    f_the_opener_round_trips();
    g_abort_io();

    printf("%d checks, %d failures, %s\n", checks, failures,
           (failures == 0) ? "PASS" : "FAIL");

    return (failures == 0) ? 0 : 1;
}

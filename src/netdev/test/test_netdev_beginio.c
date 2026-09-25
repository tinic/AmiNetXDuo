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

/* WHICH handler took the request, not just that one did.  seen_perform stays a
   total so every assertion below keeps reading the same way. */
typedef enum { TOOK_NONE = 0, TOOK_PERFORM, TOOK_READ, TOOK_WRITE } Took;
static Took          seen_took;
static NetdevOpener *seen_op;
static ULONG         seen_wire_error;
static BYTE          seen_io_error;
static BOOL          abort_answer = TRUE;

VOID netdev_perform(NetdevOpener *op, struct IOSana2Req *io)
{
    seen_took = TOOK_PERFORM;
    seen_perform++;
    seen_op         = op;
    seen_wire_error = io->ios2_WireError;
    seen_io_error   = io->ios2_Req.io_Error;
}

/*
 * CMD_READ is dispatched straight to netdev_queue_read() and no longer reaches
 * netdev_perform(), so the observer has to follow the handoff.  It records the
 * SAME three things: what this file is checking is that BeginIO cleared the
 * fields BEFORE dispatching, and that question does not care which function
 * the request was dispatched to.  Counting into seen_perform keeps every
 * assertion below reading the same way for every command.
 */
VOID netdev_queue_read(NetdevOpener *op, struct IOSana2Req *io, UWORD cmd)
{
    (VOID)cmd;

    seen_took = TOOK_READ;
    seen_perform++;
    seen_op         = op;
    seen_wire_error = io->ios2_WireError;
    seen_io_error   = io->ios2_Req.io_Error;
}

/* CMD_WRITE is dispatched the same way, and for the same reason: on a receive
   it is the acknowledgement path.  Counted into seen_perform for the same
   reason CMD_READ is. */
VOID netdev_write_cmd(NetdevOpener *op, struct IOSana2Req *io, UWORD cmd)
{
    (VOID)cmd;

    seen_took = TOOK_WRITE;
    seen_perform++;
    seen_op         = op;
    seen_wire_error = io->ios2_WireError;
    seen_io_error   = io->ios2_Req.io_Error;
}

/* NSCMD_DEVICEQUERY's handler.  Records what it was handed; the answer itself
   is test_netdev_cmds.c's to check. */
static int              seen_query;
static struct IOStdReq *seen_query_req;
static BYTE             seen_query_error;

VOID netdev_nsd_query(struct IOStdReq *std)
{
    seen_query++;
    seen_query_req   = std;
    seen_query_error = std->io_Error;
}

/* A short request with any other command is answered in BeginIO itself. */
static int seen_reply;

VOID ReplyMsg(struct Message *msg)
{
    (VOID)msg;
    seen_reply++;
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
static NetdevUnit unit;
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
    io->ios2_Req.io_Unit    = attached ? &unit.nu_ExecUnit : NULL;
    io->ios2_BufferManagement = attached ? &opener : NULL;
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
 * io_Unit and the buffer-management cookie are installed by OpenDevice.
 * NULL and -1 units both mean "no opener", and the field work still has to
 * happen: the dispatcher is what turns that into IOERR_OPENFAIL, and it
 * cannot do so on a request whose io_Error it was never allowed to see.
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


/* Two openers share io_Unit, so the buffer-management cookie must select the
   one which submitted this request. */
static void f_the_opener_round_trips(void)
{
    struct IOSana2Req io;
    NetdevOpener other;

    seen_perform = 0;
    seen_op      = NULL;
    fill(&io, CMD_READ, 0UL, 1);
    netdev_begin_io(&fake_device, &io);
    expect(seen_op == &opener,
           "the buffer-management cookie resolved to its own opener");

    memset(&other, 0, sizeof(other));
    io.ios2_BufferManagement = &other;
    seen_op = NULL;
    netdev_begin_io(&fake_device, &io);
    expect(seen_op == &other,
           "the same io_Unit can dispatch a second opener's request");
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



/*
 * THE FAST PATH IS A CLAIM ABOUT WHICH FUNCTION RUNS, SO ASSERT THAT.
 *
 * netdev_begin_io() sends CMD_READ to netdev_queue_read() and CMD_WRITE to
 * netdev_write_cmd() rather than through netdev_perform()'s twenty-case jump
 * table, 40-byte frame and movem of five registers.  Those two are what this
 * device is asked for in bulk: one CMD_READ per received frame, and on an
 * inbound transfer one CMD_WRITE for roughly every second frame, which is the
 * acknowledgement that reopens the peer's window.
 *
 * NOTHING ELSE IN THE TREE CAN CHECK THIS.  tools/check-hot-calls.sh counts
 * jsr sites in bsdsocket.library; anxnet.device links with -flto and `nm` on a
 * KEEP_SYMBOLS build returns 136 entries with every local name collapsed onto
 * one address, so there is nothing there to count.  A `static` dropped, a
 * helper moved, or a stray edit to the dispatch in netdev_io.c would put the
 * jump table back on a path that runs 480 times a second and no other gate
 * would notice.
 *
 * S2_MULTICAST and S2_BROADCAST share netdev_write_cmd()'s body but are rare,
 * so they are asserted to keep taking the generic path -- if that ever changes
 * it should change deliberately.
 */
static void h_the_two_bulk_commands_skip_the_jump_table(void)
{
    struct IOSana2Req io;

    printf("  the two bulk commands skip the jump table\n");

    fill(&io, CMD_READ, 0x1234, 1);
    seen_took = TOOK_NONE;
    netdev_begin_io(&fake_device, &io);
    expect(seen_took == TOOK_READ,
           "CMD_READ goes straight to netdev_queue_read");

    fill(&io, CMD_WRITE, 0x1234, 1);
    seen_took = TOOK_NONE;
    netdev_begin_io(&fake_device, &io);
    expect(seen_took == TOOK_WRITE,
           "CMD_WRITE goes straight to netdev_write_cmd");

    /* Rare, and deliberately still generic. */
    fill(&io, S2_BROADCAST, 0x1234, 1);
    seen_took = TOOK_NONE;
    netdev_begin_io(&fake_device, &io);
    expect(seen_took == TOOK_PERFORM,
           "S2_BROADCAST still takes the generic path");

    fill(&io, S2_READORPHAN, 0x1234, 1);
    seen_took = TOOK_NONE;
    netdev_begin_io(&fake_device, &io);
    expect(seen_took == TOOK_PERFORM,
           "S2_READORPHAN still takes the generic path");

    /* An unattached CMD_WRITE has no opener to hand the fast path, so it must
       fall through to netdev_perform(), which answers it with an error. */
    fill(&io, CMD_WRITE, 0x1234, 0);
    seen_took = TOOK_NONE;
    netdev_begin_io(&fake_device, &io);
    expect(seen_took == TOOK_PERFORM,
           "an unattached CMD_WRITE falls back to the generic path");
}

/*
 * ISSUE #39.  NSCMD_DEVICEQUERY arrives in a plain IOStdReq, usually a copy of
 * the IOStdReq-sized head of the request the caller opened with.  BeginIO
 * derived the opener from ios2_BufferManagement -- offset 84 of a 48-byte
 * request -- found none, and the query came back IOERR_BADADDRESS.
 *
 * The request is built at the head of a larger buffer whose tail is a canary,
 * so a write anywhere past the IOStdReq shows up.  The tail also plants a
 * plausible opener pointer at ios2_BufferManagement's offset: a BeginIO that
 * still read it would dispatch to the recorder with that opener.
 */
typedef struct
{
    struct IOStdReq std;
    UBYTE           tail[sizeof(struct IOSana2Req) + 32 - sizeof(struct IOStdReq)];
} StdFrame;

static void std_frame(StdFrame *f, UWORD command, UWORD length)
{
    struct IOSana2Req *wide = (struct IOSana2Req *)f;

    memset(f, 0xa5, sizeof(*f));
    wide->ios2_BufferManagement = &opener;       /* in the tail: a decoy */
    f->std.io_Message.mn_Length = length;
    f->std.io_Command           = command;
    f->std.io_Flags             = 0;
    f->std.io_Error             = (BYTE)0x5a;
    f->std.io_Unit              = &unit.nu_ExecUnit;
}

static int tail_intact(const StdFrame *f)
{
    StdFrame ref;
    struct IOSana2Req *wide = (struct IOSana2Req *)&ref;

    memset(&ref, 0xa5, sizeof(ref));
    wide->ios2_BufferManagement = &opener;
    return memcmp(f->tail, ref.tail, sizeof(ref.tail)) == 0;
}

static void i_a_plain_iostdreq_reaches_the_query(void)
{
    static const UWORD lengths[] = {
        (UWORD)sizeof(struct IOStdReq),     /* CreateIORequest(p, sizeof IOStdReq) */
        0,                                  /* built by hand, length unset */
        (UWORD)sizeof(struct IOSana2Req)    /* the full request, same answer */
    };
    int i;

    for (i = 0; i < (int)(sizeof(lengths) / sizeof(lengths[0])); i++)
    {
        StdFrame f;

        std_frame(&f, (UWORD)NSCMD_DEVICEQUERY, lengths[i]);
        seen_query = 0;
        seen_perform = 0;
        seen_took = TOOK_NONE;
        netdev_begin_io(&fake_device, (struct IOSana2Req *)&f);

        expect(seen_query == 1, "NSCMD_DEVICEQUERY reached the query handler");
        expect(seen_query_req == &f.std, "with the caller's own request");
        expect_u32("and io_Error cleared before it",
                   (unsigned long)(UBYTE)seen_query_error, 0);
        expect(seen_perform == 0,
               "and never the opener-bound dispatcher (no BADADDRESS)");
        if (lengths[i] == (UWORD)sizeof(struct IOStdReq))
            expect(tail_intact(&f),
                   "and not one byte past a 48-byte IOStdReq was written");
    }
}

static void j_a_short_request_is_answered_inside_it(void)
{
    static const UWORD commands[] = {
        CMD_READ, CMD_WRITE, S2_DEVICEQUERY, S2_ONEVENT, S2_GETSTATIONADDRESS
    };
    int i;

    for (i = 0; i < (int)(sizeof(commands) / sizeof(commands[0])); i++)
    {
        StdFrame f;

        std_frame(&f, commands[i], (UWORD)sizeof(struct IOStdReq));
        seen_perform = 0;
        seen_query = 0;
        seen_reply = 0;
        seen_took = TOOK_NONE;
        netdev_begin_io(&fake_device, (struct IOSana2Req *)&f);

        expect(seen_perform == 0 && seen_query == 0,
               "a short SANA-II command reaches no handler");
        expect_u32("it is refused as no such command",
                   (unsigned long)(UBYTE)f.std.io_Error,
                   (unsigned long)(UBYTE)IOERR_NOCMD);
        expect_u32("with io_Actual zero", (unsigned long)f.std.io_Actual, 0);
        expect(seen_reply == 1, "and replied to");
        expect(tail_intact(&f), "without writing past the request");
    }

    /* IOF_QUICK: answered in place, no reply. */
    {
        StdFrame f;

        std_frame(&f, CMD_READ, (UWORD)sizeof(struct IOStdReq));
        f.std.io_Flags = IOF_QUICK;
        seen_reply = 0;
        netdev_begin_io(&fake_device, (struct IOSana2Req *)&f);
        expect(seen_reply == 0, "a quick short request is not replied to");
        expect_u32("but is still refused", (unsigned long)(UBYTE)f.std.io_Error,
                   (unsigned long)(UBYTE)IOERR_NOCMD);
    }
}

static void k_abortio_reads_no_opener_it_cannot_have(void)
{
    StdFrame f;

    std_frame(&f, (UWORD)NSCMD_DEVICEQUERY, (UWORD)sizeof(struct IOStdReq));
    seen_abort = 0;
    expect(netdev_abort_io(&fake_device, (struct IOSana2Req *)&f) == -1,
           "AbortIO of a query reports nothing in progress");
    expect(seen_abort == 0, "without consulting the opener");

    std_frame(&f, CMD_READ, (UWORD)sizeof(struct IOStdReq));
    seen_abort = 0;
    expect(netdev_abort_io(&fake_device, (struct IOSana2Req *)&f) == -1,
           "AbortIO of a short request reports nothing in progress");
    expect(seen_abort == 0, "without reading past the request");
    expect(tail_intact(&f), "and without writing past it");
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
    h_the_two_bulk_commands_skip_the_jump_table();
    i_a_plain_iostdreq_reaches_the_query();
    j_a_short_request_is_answered_inside_it();
    k_abortio_reads_no_opener_it_cannot_have();

    printf("%d checks, %d failures, %s\n", checks, failures,
           (failures == 0) ? "PASS" : "FAIL");

    return (failures == 0) ? 0 : 1;
}

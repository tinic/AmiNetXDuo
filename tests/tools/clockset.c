/*
 * ClockSet -- move the guest's clock, so a test can stand where a real Amiga
 * stands.
 *
 *     ClockSet SECONDS/A/N
 *
 * SECONDS is seconds since the AmigaOS epoch, 1978-01-01, which is the same
 * count timer.device and battclock.resource keep.  `ClockSet 0` is therefore
 * the machine everybody actually has: no real-time clock, or a battery that
 * died, so AmigaOS starts at its epoch and stays there.
 *
 * WHY THIS EXISTS
 *
 *   tls.library skips certificate validity dates when the clock is outside a
 *   plausible window (src/tlslib/tls_time.c), which means an EXPIRED
 *   certificate is accepted on a machine with no clock.  `sntp` puts the clock
 *   right and the check comes back.  Proving that needs a machine that starts
 *   with the clock wrong -- and FS-UAE hands its guest the host's wall clock,
 *   so under the emulator every test starts with the clock already right and
 *   the interesting half never runs.
 *
 *   There is no C:Date on the harness disk to do it with: tools/fsuae-run.sh
 *   stages one executable and nothing else, so this is that executable's
 *   companion.  tests/tls/tls_api.c does the same thing inline for the same
 *   reason; this is the version a scripted run can call.
 *
 * It is a TEST tool.  It is not installed and it is not part of the command
 * set -- nothing a user should ever want does this.
 *
 * SPDX-License-Identifier: MIT
 */

#include <exec/types.h>
#include <exec/io.h>
#include <dos/dos.h>
#include <dos/datetime.h>
#include <devices/timer.h>
#include <proto/exec.h>
#include <proto/dos.h>

static const char version_tag[] __attribute__((used)) =
    "$VER: ClockSet 1.0 (26.7.2026)";

#define TEMPLATE    "SECONDS/A/N"

int main(int argc, char **argv)
{
    LONG                args[1];
    struct RDArgs      *rda;
    struct MsgPort     *port;
    struct timerequest *req;
    struct DateTime     dt;
    char                day[LEN_DATSTRING];
    char                date[LEN_DATSTRING];
    char                time[LEN_DATSTRING];
    ULONG               secs;

    (VOID)argv;

    if (argc == 0)
        return RETURN_FAIL;

    args[0] = 0;

    rda = ReadArgs((CONST_STRPTR)TEMPLATE, args, NULL);
    if (rda == NULL)
    {
        PrintFault(IoErr(), (CONST_STRPTR)"ClockSet");
        return RETURN_ERROR;
    }

    secs = (ULONG)(*(LONG *)args[0]);

    port = CreateMsgPort();
    if (port == NULL)
    {
        Printf((CONST_STRPTR)"ClockSet: no message port\n");
        FreeArgs(rda);
        return RETURN_FAIL;
    }

    req = (struct timerequest *)CreateIORequest(port, (ULONG)sizeof(*req));
    if (req == NULL)
    {
        DeleteMsgPort(port);
        Printf((CONST_STRPTR)"ClockSet: no IO request\n");
        FreeArgs(rda);
        return RETURN_FAIL;
    }

    if (OpenDevice((CONST_STRPTR)TIMERNAME, UNIT_VBLANK,
                   (struct IORequest *)req, 0) != 0)
    {
        DeleteIORequest((struct IORequest *)req);
        DeleteMsgPort(port);
        Printf((CONST_STRPTR)"ClockSet: timer.device would not open\n");
        FreeArgs(rda);
        return RETURN_FAIL;
    }

    req->tr_node.io_Command = TR_SETSYSTIME;
    req->tr_time.tv_secs    = secs;
    req->tr_time.tv_micro   = 0;
    (VOID)DoIO((struct IORequest *)req);

    CloseDevice((struct IORequest *)req);
    DeleteIORequest((struct IORequest *)req);
    DeleteMsgPort(port);

    /* Read it back through DateStamp(), which is what tls.library reads. */
    day[0] = date[0] = time[0] = '\0';
    (VOID)DateStamp(&dt.dat_Stamp);
    dt.dat_Format  = FORMAT_DOS;
    dt.dat_Flags   = 0;
    dt.dat_StrDay  = (STRPTR)day;
    dt.dat_StrDate = (STRPTR)date;
    dt.dat_StrTime = (STRPTR)time;

    if (DateToStr(&dt))
        Printf((CONST_STRPTR)"ClockSet: the clock now reads %s %s %s\n",
               (LONG)day, (LONG)date, (LONG)time);
    else
        Printf((CONST_STRPTR)"ClockSet: the clock was set to %lu\n", secs);

    FreeArgs(rda);
    return RETURN_OK;
}

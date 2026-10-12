/* SANA-II multicast delivery through an emulated Ethernet loopback backend.
 * Run in an isolated emulator with anxnet.device and no active stack.
 * SPDX-License-Identifier: MIT
 */
#include <proto/exec.h>
#include <proto/dos.h>
#include <string.h>
#include "sana2_device.h"

/* SANA-II hooks use a0/a1/d0, independently of the application's C ABI. */
asm(".text\n.globl _mtest_copy\n_mtest_copy:\n"
    "move.l %d0,%d1\nbeq.s 2f\n1: move.b (%a1)+,(%a0)+\n"
    "subq.l #1,%d1\nbne.s 1b\n2: moveq #1,%d0\nrts\n");
extern VOID mtest_copy(VOID);
static struct TagItem tags[] = {
    { S2_CopyToBuff, (ULONG)mtest_copy },
    { S2_CopyFromBuff, (ULONG)mtest_copy }, { TAG_DONE, 0 }
};

static int checks, failures;
static VOID check(BOOL ok, const char *label)
{
    checks++;
    if (!ok) failures++;
    Printf((CONST_STRPTR)"%s %s\n", (LONG)(ok ? "ok" : "FAIL"), (LONG)label);
}

static BOOL command(struct IOSana2Req *io, UWORD cmd)
{
    io->ios2_Req.io_Command = cmd;
    io->ios2_Req.io_Flags = 0;
    io->ios2_WireError = 0;
    return DoIO((struct IORequest *)io) == 0;
}

int main(void)
{
    static const UBYTE groups[][6] = {
        { 0x01, 0x00, 0x5e, 0, 0, 1 },
        { 0x33, 0x33, 0, 0, 0, 1 },
        { 0x33, 0x33, 0xff, 0x12, 0x34, 0x56 }
    };
    struct MsgPort *port = CreateMsgPort();
    struct IOSana2Req *io = NULL, *rx = NULL;
    UBYTE payload[46], received[128];
    BOOL opened = FALSE, pending = FALSE;
    UWORD g, phase, t;

    if (port == NULL) return 20;
    io = (struct IOSana2Req *)CreateIORequest(port, sizeof(*io));
    rx = (struct IOSana2Req *)CreateIORequest(port, sizeof(*rx));
    if (io == NULL || rx == NULL) goto done;
    io->ios2_BufferManagement = tags;
    opened = OpenDevice((CONST_STRPTR)"DEVS:Networks/anxnet.device", 0,
                        (struct IORequest *)io, 0) == 0;
    check(opened, "open anxnet.device without promiscuous mode");
    if (!opened) goto done;
    if (!command(io, S2_GETSTATIONADDRESS)) goto done;
    memcpy(io->ios2_SrcAddr, io->ios2_DstAddr, 6);
    check(command(io, S2_CONFIGINTERFACE), "configure station address");
    check(command(io, S2_ONLINE), "online");
    rx->ios2_Req.io_Device = io->ios2_Req.io_Device;
    rx->ios2_Req.io_Unit = io->ios2_Req.io_Unit;
    rx->ios2_BufferManagement = io->ios2_BufferManagement;
    memset(payload, 0x5a, sizeof(payload));

    for (g = 0; g < 3; g++)
    {
        memcpy(io->ios2_SrcAddr, groups[g], 6);
        check(command(io, S2_ADDMULTICASTADDRESS), "join multicast group");
        /* Joined reception, then leave and require the same frame to drop. */
        for (phase = 0; phase < 2; phase++)
        {
            memset(received, 0, sizeof(received));
            rx->ios2_Req.io_Command = CMD_READ;
            rx->ios2_Req.io_Flags = 0;
            rx->ios2_PacketType = 0x88b5;
            rx->ios2_Data = received;
            rx->ios2_DataLength = sizeof(received);
            SendIO((struct IORequest *)rx);
            pending = TRUE;
            memcpy(io->ios2_DstAddr, groups[g], 6);
            io->ios2_PacketType = 0x88b5;
            io->ios2_Data = payload;
            io->ios2_DataLength = sizeof(payload);
            check(command(io, CMD_WRITE), "transmit multicast frame");
            for (t = 0; t < 100 && CheckIO((struct IORequest *)rx) == NULL; t++)
                Delay(1);  /* bounded wait; never hold the CPU or disable Exec */
            if (phase == 0)
                check(CheckIO((struct IORequest *)rx) != NULL &&
                      rx->ios2_Req.io_Error == 0 &&
                      rx->ios2_DataLength == sizeof(payload) &&
                      memcmp(received, payload, sizeof(payload)) == 0 &&
                      memcmp(rx->ios2_DstAddr, groups[g], 6) == 0 &&
                      (rx->ios2_Req.io_Flags & SANA2IOF_MCAST) != 0,
                      "joined frame delivered intact with multicast flag");
            else
                check(CheckIO((struct IORequest *)rx) == NULL,
                      "left group is no longer delivered");
            if (CheckIO((struct IORequest *)rx) == NULL)
                AbortIO((struct IORequest *)rx);
            WaitIO((struct IORequest *)rx);
            pending = FALSE;
            if (phase == 0)
            {
                memcpy(io->ios2_SrcAddr, groups[g], 6);
                check(command(io, S2_DELMULTICASTADDRESS), "leave multicast group");
            }
        }
    }
done:
    if (pending) { AbortIO((struct IORequest *)rx); WaitIO((struct IORequest *)rx); }
    if (opened) CloseDevice((struct IORequest *)io);
    if (rx != NULL) DeleteIORequest((struct IORequest *)rx);
    if (io != NULL) DeleteIORequest((struct IORequest *)io);
    DeleteMsgPort(port);
    Printf((CONST_STRPTR)"%ld checks, %ld failures, %s\n", (LONG)checks, (LONG)failures,
           (LONG)(checks == 21 && failures == 0 ? "PASS" : "FAIL"));
    return checks == 21 && failures == 0 ? 0 : 20;
}

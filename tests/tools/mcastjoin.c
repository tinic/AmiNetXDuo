/*
 * McastJoin, does this card's driver accept a real multicast address?
 *
 * The whole of the defect anxnet.device was written to remove is one bit.
 * Individual Computers' x-surf.device and x-surf-100.device 1.16 test bit 7
 * of ios2_SrcAddr[0] in S2_ADDMULTICASTADDRESS; the Ethernet group bit is bit
 * 0.  Every address a real group has -- IPv4's 01:00:5e:.., IPv6's 33:33:.. --
 * has bit 7 clear, so every legitimate join is refused and the card's hash
 * filter stays empty.
 *
 * THE SWEEPS CANNOT SEE THIS, and that is why this exists.  Our own stack
 * works around it (src/sana2/sana2_device.c, ami_sana2_multicast): when a
 * driver answers S2ERR_BAD_ADDRESS/S2WERR_BAD_MULTICAST to an address with
 * bit 7 clear, it asks again for the SAME HASH BUCKET using a synthetic
 * 80:00:00:00:00:xx that the bit-7 test accepts.  So tests/tools/
 * run-cardsweep6.sh passes on those drivers through this stack, and would not
 * through Roadshow or AmiTCP.  What that workaround hides, this prints.
 *
 * It asks the driver directly, with no stack in the way:
 *
 *   33:33:ff:12:34:56   an IPv6 solicited-node address, the one whose loss
 *                       costs off-LAN IPv6
 *   33:33:00:00:00:01   IPv6 all-nodes
 *   01:00:5e:00:00:01   IPv4 all-hosts
 *   02:41:4d:49:00:01   a UNICAST address, which must be REFUSED
 *
 * Output is key=value, one line per address, plus a verdict.  rc 0 means the
 * three group addresses were accepted and the unicast one was not.
 *
 * A one-off probe rather than a command, so it has no CMake entry:
 *
 *   . tools/amiga-toolchain.sh
 *   "$AMIGA_GCC" -O2 -m68020 -I"$AMIGA_NDK" -o McastJoin \
 *       tests/tools/mcastjoin.c
 *
 * SPDX-License-Identifier: MIT
 */

#include <exec/types.h>
#include <exec/io.h>
#include <exec/memory.h>
#include <exec/ports.h>
#include <dos/dos.h>
#include <utility/tagitem.h>

#include <proto/exec.h>
#include <proto/dos.h>

#include <stdio.h>
#include <string.h>

#define SANA2_MAX_ADDR_BYTES    16

struct IOSana2Req
{
    struct IORequest ios2_Req;
    ULONG   ios2_WireError;
    ULONG   ios2_PacketType;
    UBYTE   ios2_SrcAddr[SANA2_MAX_ADDR_BYTES];
    UBYTE   ios2_DstAddr[SANA2_MAX_ADDR_BYTES];
    ULONG   ios2_DataLength;
    VOID   *ios2_Data;
    VOID   *ios2_StatData;
    VOID   *ios2_BufferManagement;
};

#define S2_START                (CMD_NONSTD)
#define S2_ADDMULTICASTADDRESS  (S2_START + 5)
#define S2_DELMULTICASTADDRESS  (S2_START + 6)

#define S2_Dummy                (TAG_USER + 0xB0000)
#define S2_CopyToBuff           (S2_Dummy + 1)
#define S2_CopyFromBuff         (S2_Dummy + 2)

/*
 * A driver may look at the buffer-management list at OpenDevice() time and
 * refuse an open without one.  These are never called: nothing here reads or
 * writes a packet.
 */
static ULONG copy_stub(VOID)
{
    return 0;
}

static struct TagItem buffer_tags[] =
{
    { S2_CopyToBuff,   (ULONG)copy_stub },
    { S2_CopyFromBuff, (ULONG)copy_stub },
    { TAG_DONE,        0                }
};

struct Case
{
    const char *name;
    UBYTE       addr[6];
    int         want_ok;        /* 1 = the driver must accept it */
};

static const struct Case cases[] =
{
    { "solicited-node", { 0x33, 0x33, 0xff, 0x12, 0x34, 0x56 }, 1 },
    { "all-nodes",      { 0x33, 0x33, 0x00, 0x00, 0x00, 0x01 }, 1 },
    { "all-hosts",      { 0x01, 0x00, 0x5e, 0x00, 0x00, 0x01 }, 1 },
    { "unicast",        { 0x02, 0x41, 0x4d, 0x49, 0x00, 0x01 }, 0 },
};

#define NCASES ((int)(sizeof(cases) / sizeof(cases[0])))

int main(int argc, char **argv)
{
    struct MsgPort    *port;
    struct IOSana2Req *req;
    const char        *device = "anxnet.device";
    ULONG              unit   = 0;
    int                i;
    int                wrong  = 0;
    LONG               rc     = 0;

    (VOID)argc;
    (VOID)argv;

    /* Amiga guest programs see argc == 1, so the arguments come from DOS. */
    {
        static char line[128];
        struct RDArgs *rda;
        LONG   args[2] = { 0, 0 };

        (VOID)line;
        rda = ReadArgs("DEVICE/K,UNIT/K/N", args, NULL);
        if (rda != NULL)
        {
            if (args[0] != 0)
                device = (const char *)args[0];
            if (args[1] != 0)
                unit = (ULONG)(*(LONG *)args[1]);
        }
        /* rda is freed at exit; the strings above point into it. */
        if (rda == NULL)
            printf("note=could-not-parse-arguments\n");

        port = CreateMsgPort();
        if (port == NULL)
        {
            printf("error=no-msgport\n");
            return 20;
        }

        req = (struct IOSana2Req *)CreateIORequest(port,
                                                   sizeof(struct IOSana2Req));
        if (req == NULL)
        {
            printf("error=no-iorequest\n");
            DeleteMsgPort(port);
            return 20;
        }

        req->ios2_BufferManagement = buffer_tags;

        if (OpenDevice((CONST_STRPTR)device, unit,
                       (struct IORequest *)req, 0) != 0)
        {
            printf("device=%s unit=%lu open=FAILED error=%ld\n",
                   device, (unsigned long)unit,
                   (long)(BYTE)req->ios2_Req.io_Error);
            DeleteIORequest((struct IORequest *)req);
            DeleteMsgPort(port);
            return 10;
        }

        printf("device=%s unit=%lu open=ok\n", device, (unsigned long)unit);

        for (i = 0; i < NCASES; i++)
        {
            LONG  err;
            ULONG wire;
            int   ok;

            memset(req->ios2_SrcAddr, 0, sizeof(req->ios2_SrcAddr));
            memcpy(req->ios2_SrcAddr, cases[i].addr, 6);
            req->ios2_Req.io_Command = S2_ADDMULTICASTADDRESS;
            req->ios2_Req.io_Error   = 0;
            req->ios2_Req.io_Flags   = 0;
            req->ios2_WireError      = 0;

            DoIO((struct IORequest *)req);

            err  = (LONG)(BYTE)req->ios2_Req.io_Error;
            wire = req->ios2_WireError;
            ok   = (err == 0);

            printf("addr=%02x:%02x:%02x:%02x:%02x:%02x name=%s "
                   "group_bit=%d bit7=%d error=%ld wire=%lu accepted=%s "
                   "expected=%s %s\n",
                   cases[i].addr[0], cases[i].addr[1], cases[i].addr[2],
                   cases[i].addr[3], cases[i].addr[4], cases[i].addr[5],
                   cases[i].name,
                   cases[i].addr[0] & 1,
                   (cases[i].addr[0] & 0x80) ? 1 : 0,
                   (long)err, (unsigned long)wire,
                   ok ? "yes" : "no",
                   cases[i].want_ok ? "yes" : "no",
                   (ok == cases[i].want_ok) ? "ok" : "WRONG");

            if (ok != cases[i].want_ok)
                wrong++;

            if (ok)
            {
                req->ios2_Req.io_Command = S2_DELMULTICASTADDRESS;
                req->ios2_Req.io_Error   = 0;
                req->ios2_WireError      = 0;
                DoIO((struct IORequest *)req);
            }
        }

        printf("cases=%d wrong=%d verdict=%s\n", NCASES, wrong,
               (wrong == 0) ? "PASS" : "FAIL");

        rc = (wrong == 0) ? 0 : 10;

        CloseDevice((struct IORequest *)req);
        DeleteIORequest((struct IORequest *)req);
        DeleteMsgPort(port);

        if (rda != NULL)
            FreeArgs(rda);
    }

    return (int)rc;
}

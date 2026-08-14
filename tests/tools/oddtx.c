/*
 * OddTx, does an odd-length transmit survive the remote DMA?
 *
 * THE QUESTION
 *
 *   ne2000_write_buf() programs RBCR with the frame length and then pushes
 *   (len + 1) & ~1 bytes, because the data port is 16 bits wide.  For an odd
 *   length those two numbers disagree, and what the part does about it decides
 *   whether ISR.RDC ever asserts.  If it does not, the driver's wait times
 *   out, the chip is reset and the write is answered S2ERR_TX_FAILURE -- so
 *   the symptom is visible from outside without any instrumentation:
 *
 *     - a CMD_WRITE that returns S2ERR_TX_FAILURE, and
 *     - "Chip resets" in S2_GETSPECIALSTATS climbing once per odd frame.
 *
 *   Both are read here, before and after, for a spread of odd and even
 *   lengths.  The even ones are the control: if they fail too, the fault is
 *   not the byte count.
 *
 * WHAT A PASS HERE DOES AND DOES NOT SETTLE
 *
 *   Amiberry's NE2000 terminates on count-exhausted rather than count-equals-
 *   zero (qemuvga/ne2000.cpp: "if (s->rcnt <= len) s->rcnt = 0"), so an odd
 *   RBCR still asserts RDC there.  A pass therefore proves no regression on
 *   this rig; it does NOT prove an exact-compare implementation would be
 *   happy.  That is the whole reason the result is worth writing down rather
 *   than assuming either way.
 *
 * A one-off probe rather than a command, so it has no CMake entry:
 *
 *   . tools/amiga-toolchain.sh
 *   "$AMIGA_GCC" -O2 -m68020 -I"$AMIGA_NDK" -o OddTx tests/tools/oddtx.c
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

struct Sana2SpecialStatRecord
{
    ULONG Type;
    ULONG Count;
    char *String;
};

struct Sana2SpecialStatHeader
{
    ULONG RecordCountMax;
    ULONG RecordCountSupplied;
};

#define S2_START                (CMD_NONSTD)
#define S2_GETSTATIONADDRESS    (S2_START +  1)
#define S2_CONFIGINTERFACE      (S2_START +  2)
#define S2_GETSPECIALSTATS      (S2_START + 12)
#define S2_ONLINE               (S2_START + 16)

#define S2_Dummy                (TAG_USER + 0xB0000)
#define S2_CopyToBuff           (S2_Dummy + 1)
#define S2_CopyFromBuff         (S2_Dummy + 2)

/*
 * The buffer-management hooks: a0 = to, a1 = from, d0 = len, d0 != 0 on
 * success.  Written out because only d0/d1/a0/a1 may be trashed and a C
 * function with register arguments miscompiles on this toolchain.
 */
asm("    .text                        \n"
    "    .globl _oddtx_copy           \n"
    "_oddtx_copy:                     \n"
    "    move.l %d0,%d1               \n"
    "    beq.s  2f                    \n"
    "1:  move.b (%a1)+,(%a0)+         \n"
    "    subq.l #1,%d1                \n"
    "    bne.s  1b                    \n"
    "2:  moveq  #1,%d0                \n"
    "    rts                          \n");

extern VOID oddtx_copy(VOID);

static struct TagItem buffer_tags[] =
{
    { S2_CopyToBuff,   (ULONG)oddtx_copy },
    { S2_CopyFromBuff, (ULONG)oddtx_copy },
    { TAG_DONE,        0                 }
};

static VOID p_str(const char *s)
{
    if (s != NULL)
        PutStr((CONST_STRPTR)s);
}

static VOID p_num(LONG v)
{
    char  buf[12];
    int   i = (int)sizeof(buf) - 1;
    ULONG u;

    buf[i] = '\0';
    u = (v < 0) ? (ULONG)(-v) : (ULONG)v;
    do
    {
        buf[--i] = (char)('0' + (int)(u % 10u));
        u /= 10u;
    }
    while (u != 0);
    if (v < 0)
        buf[--i] = '-';
    p_str(&buf[i]);
}

/* "Chip resets" is record 2, "Data transfer mode" record 0. */
#define STAT_MODE       0
#define STAT_RESETS     2
#define STAT_SLOTS      8

static ULONG read_stat(struct IOSana2Req *req, ULONG which)
{
    static struct
    {
        struct Sana2SpecialStatHeader hdr;
        struct Sana2SpecialStatRecord rec[STAT_SLOTS];
    } stats;

    stats.hdr.RecordCountMax      = STAT_SLOTS;
    stats.hdr.RecordCountSupplied = 0;

    req->ios2_Req.io_Command = S2_GETSPECIALSTATS;
    req->ios2_Req.io_Error   = 0;
    req->ios2_Req.io_Flags   = 0;
    req->ios2_WireError      = 0;
    req->ios2_StatData       = &stats;
    DoIO((struct IORequest *)req);

    if (req->ios2_Req.io_Error != 0 || which >= stats.hdr.RecordCountSupplied)
        return 0;

    return stats.rec[which].Count;
}

/* Payload lengths.  total = 14 + payload, so an odd payload is an odd frame. */
static const UWORD lengths[] =
{
    47, 49, 51, 101, 203, 517, 1001, 1499,      /* odd frames  */
    46, 50, 100, 200, 516, 1000, 1498, 1500     /* even frames */
};

#define NLEN    ((int)(sizeof(lengths) / sizeof(lengths[0])))
#define NODD    8

int main(void)
{
    struct MsgPort    *port;
    struct IOSana2Req *req;
    static UBYTE       payload[1500];
    const char        *device = "anxnet.device";
    ULONG              unit   = 0;
    struct RDArgs     *rda;
    LONG               args[2] = { 0, 0 };
    int                opened;
    int                i;
    ULONG              resets_before;
    ULONG              resets_after;
    ULONG              mode;
    int                odd_fail  = 0;
    int                even_fail = 0;

    rda = ReadArgs((CONST_STRPTR)"DEVICE/K,UNIT/K/N", args, NULL);
    if (rda != NULL)
    {
        if (args[0] != 0)
            device = (const char *)args[0];
        if (args[1] != 0)
            unit = (ULONG)(*(LONG *)args[1]);
    }

    for (i = 0; i < (int)sizeof(payload); i++)
        payload[i] = (UBYTE)i;

    port = CreateMsgPort();
    if (port == NULL)
    {
        p_str("error=no-msgport\n");
        return 20;
    }

    req = (struct IOSana2Req *)CreateIORequest(port, sizeof(struct IOSana2Req));
    if (req == NULL)
    {
        p_str("error=no-iorequest\n");
        DeleteMsgPort(port);
        return 20;
    }

    req->ios2_BufferManagement = buffer_tags;

    opened = (OpenDevice((CONST_STRPTR)device, unit,
                         (struct IORequest *)req, 0) == 0);
    if (!opened)
    {
        static char path[128];

        strcpy(path, "DEVS:Networks/");
        strncat(path, device, sizeof(path) - 20);
        opened = (OpenDevice((CONST_STRPTR)path, unit,
                             (struct IORequest *)req, 0) == 0);
    }
    if (!opened)
    {
        p_str("device=");
        p_str(device);
        p_str(" open=FAILED\n");
        DeleteIORequest((struct IORequest *)req);
        DeleteMsgPort(port);
        return 10;
    }

    /* The factory address, then configure with it and go online. */
    req->ios2_Req.io_Command = S2_GETSTATIONADDRESS;
    req->ios2_Req.io_Error   = 0;
    req->ios2_Req.io_Flags   = 0;
    DoIO((struct IORequest *)req);
    memcpy(req->ios2_SrcAddr, req->ios2_DstAddr, 6);

    req->ios2_Req.io_Command = S2_CONFIGINTERFACE;
    req->ios2_Req.io_Error   = 0;
    req->ios2_Req.io_Flags   = 0;
    req->ios2_WireError      = 0;
    DoIO((struct IORequest *)req);
    p_str("configinterface_error=");
    p_num((LONG)(BYTE)req->ios2_Req.io_Error);
    p_str("\n");

    req->ios2_Req.io_Command = S2_ONLINE;
    req->ios2_Req.io_Error   = 0;
    req->ios2_Req.io_Flags   = 0;
    req->ios2_WireError      = 0;
    DoIO((struct IORequest *)req);
    p_str("online_error=");
    p_num((LONG)(BYTE)req->ios2_Req.io_Error);
    p_str("\n");

    mode          = read_stat(req, STAT_MODE);
    resets_before = read_stat(req, STAT_RESETS);

    p_str("transfer_mode=");
    p_num((LONG)mode);
    p_str(" resets_before=");
    p_num((LONG)resets_before);
    p_str("\n");

    for (i = 0; i < NLEN; i++)
    {
        UWORD len   = lengths[i];
        int   isodd = (i < NODD);
        LONG  err;

        memset(req->ios2_DstAddr, 0, SANA2_MAX_ADDR_BYTES);
        /* Locally administered, nobody's, and not broadcast: this goes on a
           real LAN and should bother no one. */
        req->ios2_DstAddr[0] = 0x02;
        req->ios2_DstAddr[5] = 0x01;

        req->ios2_Req.io_Command = CMD_WRITE;
        req->ios2_Req.io_Error   = 0;
        req->ios2_Req.io_Flags   = 0;
        req->ios2_WireError      = 0;
        req->ios2_PacketType     = 0x88b5;      /* IEEE local experimental */
        req->ios2_DataLength     = len;
        req->ios2_Data           = payload;
        DoIO((struct IORequest *)req);

        err = (LONG)(BYTE)req->ios2_Req.io_Error;

        p_str("payload=");
        p_num((LONG)len);
        p_str(" frame=");
        p_num((LONG)len + 14);
        p_str(" parity=");
        p_str(((len + 14) & 1) ? "odd" : "even");
        p_str(" error=");
        p_num(err);
        p_str(" wire=");
        p_num((LONG)req->ios2_WireError);
        p_str("\n");

        if (err != 0)
        {
            if (isodd)
                odd_fail++;
            else
                even_fail++;
        }
    }

    resets_after = read_stat(req, STAT_RESETS);

    p_str("resets_after=");
    p_num((LONG)resets_after);
    p_str(" resets_delta=");
    p_num((LONG)(resets_after - resets_before));
    p_str(" odd_frames=");
    p_num(NODD);
    p_str(" odd_failed=");
    p_num((LONG)odd_fail);
    p_str(" even_frames=");
    p_num(NLEN - NODD);
    p_str(" even_failed=");
    p_num((LONG)even_fail);
    p_str("\n");

    p_str("verdict=");
    p_str((odd_fail == 0 && even_fail == 0 && resets_after == resets_before)
          ? "PASS" : "FAIL");
    p_str("\n");

    CloseDevice((struct IORequest *)req);
    DeleteIORequest((struct IORequest *)req);
    DeleteMsgPort(port);
    if (rda != NULL)
        FreeArgs(rda);

    return (odd_fail == 0 && even_fail == 0 &&
            resets_after == resets_before) ? 0 : 10;
}

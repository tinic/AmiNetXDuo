/*
 * hush SECS [UNIT]: digital silence through ahi.device CMD_WRITE, 44.1 kHz
 * 16-bit stereo, two 250 ms buffers double-buffered.  The xhci/usbaudio/AHI
 * path is driven exactly as a tone would drive it, and nothing is heard.
 * CTRL-C stops it.  One key=value line at the end.
 *
 * SPDX-License-Identifier: MIT
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <devices/ahi.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <stdlib.h>

#define RATE 44100
#define FR   (RATE / 4)

int main(int argc, char **argv)
{
    LONG               secs = argc > 1 ? atol(argv[1]) : 10;
    LONG               unit = argc > 2 ? atol(argv[2]) : 0;
    LONG               n, i, err = 0, o[4];
    struct MsgPort    *mp = CreateMsgPort();
    struct AHIRequest *r[2], *link = NULL;
    WORD              *b[2];

    if (!mp)
        return 20;
    r[0] = (struct AHIRequest *)CreateIORequest(mp, sizeof(struct AHIRequest));
    if (!r[0])
        return 20;
    r[0]->ahir_Version = 4;
    if (OpenDevice((CONST_STRPTR)"ahi.device", unit, (struct IORequest *)r[0], 0))
    {
        PutStr((CONST_STRPTR)"hush=open_failed\n");
        return 20;
    }
    r[1] = (struct AHIRequest *)AllocMem(sizeof(struct AHIRequest), MEMF_PUBLIC);
    b[0] = AllocMem(FR * 4, MEMF_PUBLIC | MEMF_CLEAR);
    b[1] = AllocMem(FR * 4, MEMF_PUBLIC | MEMF_CLEAR);
    if (!r[1] || !b[0] || !b[1])
    {
        CloseDevice((struct IORequest *)r[0]);
        return 20;
    }
    CopyMem(r[0], r[1], sizeof(struct AHIRequest));

    for (n = 0; n < secs * 4 && !(SetSignal(0, 0) & SIGBREAKF_CTRL_C); n++)
    {
        struct AHIRequest *q = r[n & 1];

        if (n >= 2)
        {
            WaitIO((struct IORequest *)q);
            if (q->ahir_Std.io_Error)
                err++;
        }
        q->ahir_Std.io_Command = CMD_WRITE;
        q->ahir_Std.io_Data    = b[n & 1];
        q->ahir_Std.io_Length  = FR * 4;
        q->ahir_Std.io_Offset  = 0;
        q->ahir_Frequency      = RATE;
        q->ahir_Type           = AHIST_S16S;
        q->ahir_Volume         = 0x10000;
        q->ahir_Position       = 0x8000;
        q->ahir_Link           = link;
        SendIO((struct IORequest *)q);
        link = q;
    }
    /* The two still out: n - 1 and n - 2. */
    for (i = n - 2; i < n; i++)
        if (i >= 0)
            WaitIO((struct IORequest *)r[i & 1]);

    o[0] = n / 4;
    o[1] = unit;
    o[2] = err;
    VPrintf((CONST_STRPTR)"hush=done secs=%ld unit=%ld errs=%ld\n", o);
    CloseDevice((struct IORequest *)r[0]);
    FreeMem(b[0], FR * 4);
    FreeMem(b[1], FR * 4);
    FreeMem(r[1], sizeof(struct AHIRequest));
    DeleteIORequest((struct IORequest *)r[0]);
    DeleteMsgPort(mp);
    return 0;
}

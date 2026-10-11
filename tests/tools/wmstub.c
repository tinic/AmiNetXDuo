/*
 * WmStub: WirelessManager as far as tests/tools/run-netwireless.sh needs
 * one.  Installed as C:WirelessManager; takes WirelessManager's DEVICE/A
 * and UNIT/K/N, reads ENV:Sys/Wireless.prefs once, "joins" the network with
 * the highest priority= by S2_SETOPTIONS {S2INFO_SSID} on wifistub.device,
 * and holds the unit until Ctrl-C, as the real one does.
 *
 * Each start appends "start device=<d> unit=<n> stack=<bytes> ssid=<s>" to
 * T:wmstub.log, and each stop "stop".
 *
 * SPDX-License-Identifier: MIT
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <utility/tagitem.h>

#include <proto/exec.h>
#include <proto/dos.h>

#include "sana2_device.h"

#define S2_SETOPTIONS   0xc012
#define S2INFO_SSID     (TAG_USER + 0)

static char prefs[4096];

/* The quoted ssid= of the block with the highest priority=. */
static VOID best_ssid(char *out, ULONG cap)
{
    BPTR  fh = Open((CONST_STRPTR)"ENV:Sys/Wireless.prefs", MODE_OLDFILE);
    LONG  got, i, best = -1, prio = 0;
    char  cur[40];

    out[0] = cur[0] = '\0';
    if (fh == (BPTR)0) return;
    got = Read(fh, prefs, sizeof(prefs) - 1);
    Close(fh);
    if (got <= 0) return;
    prefs[got] = '\0';
    for (i = 0; i < got; i++)
    {
        const char *l = prefs + i;

        while (*l == ' ' || *l == '\t') l++;
        if (l[0] == 's' && l[1] == 's' && l[2] == 'i' && l[3] == 'd' &&
            l[4] == '=' && l[5] == '"')
        {
            ULONG k = 0;

            l += 6;
            while (*l != '"' && *l != '\n' && *l != '\0' && k + 1 < sizeof(cur))
                cur[k++] = *l++;
            cur[k] = '\0';
            prio = 0;
        }
        else if (l[0] == 'p' && l[1] == 'r' && l[2] == 'i' && l[3] == 'o')
            StrToLong((CONST_STRPTR)(l + 9), &prio);
        else if (l[0] == '}' && prio > best)
        {
            ULONG k;

            best = prio;
            for (k = 0; cur[k] != '\0' && k + 1 < cap; k++) out[k] = cur[k];
            out[k] = '\0';
        }
        while (prefs[i] != '\n' && prefs[i] != '\0') i++;
        if (prefs[i] == '\0') break;
    }
}

static VOID logline(const char *fmt, LONG *args)
{
    BPTR fh = Open((CONST_STRPTR)"T:wmstub.log", MODE_READWRITE);

    if (fh == (BPTR)0) return;
    Seek(fh, 0, OFFSET_END);
    VFPrintf(fh, (CONST_STRPTR)fmt, args);
    Close(fh);
}

static BOOL nocopy(register APTR to __asm("a0"), register APTR from __asm("a1"),
                   register ULONG n __asm("d0"))
{
    (VOID)to; (VOID)from; (VOID)n;
    return FALSE;
}

int main(void)
{
    LONG               args[2] = { 0, 0 }, log[4];
    struct RDArgs     *rda = ReadArgs((CONST_STRPTR)"DEVICE/A,UNIT/K/N", args, NULL);
    struct Task       *me = FindTask(NULL);
    struct MsgPort    *port = CreateMsgPort();
    struct IOSana2Req *req = AllocVec(sizeof(*req), MEMF_PUBLIC | MEMF_CLEAR);
    struct TagItem     buf[3], opt[2];
    static char        ssid[40];

    if (rda == NULL || port == NULL || req == NULL) return RETURN_FAIL;
    best_ssid(ssid, sizeof(ssid));
    log[0] = args[0];
    log[1] = args[1] != 0 ? *(LONG *)args[1] : 0;
    log[2] = (LONG)((ULONG)me->tc_SPUpper - (ULONG)me->tc_SPLower);
    log[3] = (LONG)ssid;
    logline("start device=%s unit=%ld stack=%ld ssid=%s\n", log);

    buf[0].ti_Tag = S2_CopyToBuff;   buf[0].ti_Data = (ULONG)nocopy;
    buf[1].ti_Tag = S2_CopyFromBuff; buf[1].ti_Data = (ULONG)nocopy;
    buf[2].ti_Tag = TAG_DONE;
    req->ios2_Req.io_Message.mn_ReplyPort = port;
    req->ios2_Req.io_Message.mn_Length = sizeof(*req);
    req->ios2_BufferManagement = buf;
    if (OpenDevice((CONST_STRPTR)args[0], (ULONG)log[1], (struct IORequest *)req, 0) == 0)
    {
        if (ssid[0] != '\0')
        {
            opt[0].ti_Tag = S2INFO_SSID; opt[0].ti_Data = (ULONG)ssid;
            opt[1].ti_Tag = TAG_DONE;
            req->ios2_Req.io_Command = S2_SETOPTIONS;
            req->ios2_Data = opt;
            DoIO((struct IORequest *)req);
        }
        Wait(SIGBREAKF_CTRL_C);
        CloseDevice((struct IORequest *)req);
    }
    else
        Wait(SIGBREAKF_CTRL_C);
    logline("stop\n", log);
    FreeVec(req);
    DeleteMsgPort(port);
    FreeArgs(rda);
    return RETURN_OK;
}

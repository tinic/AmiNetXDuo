/*
 * NetWirelessPrefs: WirelessManager reads its configuration once, at start,
 * and has no reconfigure signal: Ctrl-C ends it.  A new configuration is
 * therefore taken up by stopping it and starting it again, with the DEVICE
 * and UNIT of the Wi-Fi interface, as S:Network-Startup starts it.
 *
 * It is found the way `Status COMMAND=` finds it: a Shell process running a
 * command whose name, path aside, is WirelessManager.
 *
 * SPDX-License-Identifier: MIT
 */

#include "netwireless_wm.h"

#include <dos/dosextens.h>
#include <dos/dostags.h>
#include <proto/exec.h>
#include <proto/dos.h>

#define NW_WM_NAME  "WirelessManager"

static int nw_lower(int c)
{
    return (c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c;
}

/* The Shell number running WirelessManager, or 0.  Call under Forbid(). */
static ULONG nw_wm_cli(char *path, ULONG cap)
{
    static const char name[] = NW_WM_NAME;
    ULONG n, max = MaxCli();

    for (n = 1; n <= max; n++)
    {
        struct Process *p = FindCliProc(n);
        struct CommandLineInterface *cli;
        const UBYTE *b;
        ULONG len, i, start;

        if (p == NULL) continue;
        cli = (struct CommandLineInterface *)BADDR(p->pr_CLI);
        if (cli == NULL || cli->cli_Module == (BPTR)0) continue;
        b = (const UBYTE *)BADDR(cli->cli_CommandName);
        if (b == NULL) continue;
        len = b[0];
        start = len;
        while (start > 0 && b[start] != ':' && b[start] != '/') start--;
        if (len - start != sizeof(name) - 1) continue;
        for (i = 0; i < sizeof(name) - 1; i++)
            if (nw_lower(b[start + 1 + i]) != nw_lower(name[i])) break;
        if (i != sizeof(name) - 1) continue;
        if (path != NULL && cap > len)
        {
            for (i = 0; i < len; i++) path[i] = (char)b[i + 1];
            path[len] = '\0';
        }
        return n;
    }
    return 0;
}

BOOL nw_wm_running(char *path, ULONG cap)
{
    ULONG n;

    Forbid();
    n = nw_wm_cli(path, cap);
    Permit();
    return n != 0;
}

BOOL nw_wm_stop(VOID)
{
    ULONG tries;

    for (tries = 0; tries < 100; tries++)
    {
        ULONG n;

        Forbid();
        n = nw_wm_cli(NULL, 0);
        if (n != 0 && tries % 25 == 0)
        {
            /* Two statements: an inline library call as the argument of
               another leaves a6 holding the inner one's base. */
            struct Process *p = FindCliProc(n);

            Signal(&p->pr_Task, SIGBREAKF_CTRL_C);
        }
        Permit();
        if (n == 0) return TRUE;
        Delay(5);
    }
    return FALSE;
}

static ULONG nw_cat(char *buf, ULONG n, ULONG cap, const char *s)
{
    while (*s != '\0' && n + 1 < cap) buf[n++] = *s++;
    buf[n] = '\0';
    return n;
}

VOID nw_wm_mark_off(VOID)
{
    (VOID)SetVar((CONST_STRPTR)"WiFiOff", (CONST_STRPTR)"1", 1, GVF_GLOBAL_ONLY);
}

BOOL nw_wm_marked_off(VOID)
{
    char v[4];

    return GetVar((CONST_STRPTR)"WiFiOff", (STRPTR)v, sizeof(v), GVF_GLOBAL_ONLY) >= 0;
}

BOOL nw_wm_start(const char *command, const char *device, ULONG unit)
{
    char line[256];
    BPTR in, out;
    LONG rc;

    (VOID)DeleteVar((CONST_STRPTR)"WiFiOff", GVF_GLOBAL_ONLY);
    /* The Shell's own Stack command, then WirelessManager, both in the new
       background Shell: the same two lines S:Network-Startup runs. */
    in = Open((CONST_STRPTR)"NIL:", MODE_OLDFILE);
    out = Open((CONST_STRPTR)"NIL:", MODE_NEWFILE);
    if (in == (BPTR)0 || out == (BPTR)0)
    {
        if (in != (BPTR)0) Close(in);
        if (out != (BPTR)0) Close(out);
        return FALSE;
    }
    {
        ULONG n = 0;
        char  digits[12];
        int   d = 0;

        n = nw_cat(line, n, sizeof(line), "Stack 32768\n");
        n = nw_cat(line, n, sizeof(line), command);
        n = nw_cat(line, n, sizeof(line), " DEVICE=\"");
        n = nw_cat(line, n, sizeof(line), device);
        n = nw_cat(line, n, sizeof(line), "\" UNIT=");
        do digits[d++] = (char)('0' + unit % 10); while ((unit /= 10) != 0 && d < 11);
        while (d > 0 && n + 1 < sizeof(line)) line[n++] = digits[--d];
        line[n] = '\0';
    }
    rc = SystemTags((CONST_STRPTR)line, SYS_Input, (ULONG)in,
                    SYS_Output, (ULONG)out, SYS_Asynch, TRUE,
                    SYS_UserShell, TRUE, TAG_DONE);
    if (rc == -1)
    {
        Close(in);
        Close(out);
        return FALSE;
    }
    return TRUE;
}

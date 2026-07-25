/*
 * icontest -- hand the generated .info files to the real icon.library and
 * see whether it agrees they are icons.
 *
 * install/tools/makeicon.py writes the DiskObject structure by hand, and
 * install/tools/showicon.py reads it back with an independently written
 * parser -- but two programs by the same author agreeing proves only that
 * they agree.  This asks Kickstart.
 *
 * For each icon it does GetDiskObject(), reports what came back, then
 * PutDiskObject()s it to RAM: and GetDiskObject()s that copy, because a
 * structure icon.library can read but not write is still wrong.
 *
 * Run with no arguments from the harness's Startup-Sequence.  Writes
 * DH0:icontest.txt.  Exit status 0 if every icon loaded, matched what it
 * claims, and survived the round trip.
 *
 * SPDX-License-Identifier: MIT
 */

#include <exec/types.h>
#include <workbench/workbench.h>
#include <workbench/icon.h>
#include <dos/dos.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/icon.h>

#define DRAWER "DH0:AmiNetXDuo"

struct Expect
{
    const char *name;       /* without the .info */
    UBYTE       type;
    const char *deftool;    /* NULL if there should not be one */
    LONG        stack;      /* 0: do not care */
    LONG        tooltypes;  /* -1: do not care */
};

static const struct Expect expected[] =
{
    { "Install-AmiNetXDuo", WBPROJECT, "Installer",           10000,  5 },
    { "AmiNetXDuo",         WBDRAWER,  NULL,                      0, -1 },
    { "Drawer",             WBDRAWER,  NULL,                      0, -1 },
    { "Document",           WBPROJECT, "SYS:Utilities/More",      0, -1 },
    { NULL,                 0,         NULL,                      0, -1 }
};

static BPTR out;

static VOID say(const char *fmt, LONG a, LONG b)
{
    LONG args[2];

    args[0] = a;
    args[1] = b;
    if (out != 0)
    {
        VFPrintf(out, (STRPTR)fmt, args);
        Flush(out);
    }
}

static BOOL same(const char *a, const char *b)
{
    if (a == NULL || b == NULL)
        return (BOOL)(a == b);
    while (*a != '\0' && *a == *b)
    {
        a++;
        b++;
    }
    return (BOOL)(*a == *b);
}

static LONG count_tooltypes(char **tt)
{
    LONG n = 0;

    if (tt != NULL)
        while (tt[n] != NULL)
            n++;
    return n;
}

static BOOL check(const struct Expect *e)
{
    char               path[128];
    struct DiskObject *dobj;
    struct DiskObject *again;
    LONG               tt;
    BOOL               ok = TRUE;

    /* path = DRAWER/name, no dos.library string helpers needed */
    {
        const char *p = DRAWER "/";
        char       *d = path;

        while (*p != '\0')
            *d++ = *p++;
        p = e->name;
        while (*p != '\0')
            *d++ = *p++;
        *d = '\0';
    }

    dobj = GetDiskObject((STRPTR)path);
    if (dobj == NULL)
    {
        say("%s: icon.library will not load it (IoErr %ld)\n",
            (LONG)e->name, IoErr());
        return FALSE;
    }

    tt = count_tooltypes((char **)dobj->do_ToolTypes);

    say("%s: type %ld", (LONG)e->name, (LONG)dobj->do_Type);
    say(" gadget %ldx%ld", (LONG)dobj->do_Gadget.Width,
        (LONG)dobj->do_Gadget.Height);
    say(" stack %ld tooltypes %ld", dobj->do_StackSize, tt);
    say(" defaulttool \"%s\"\n",
        (LONG)(dobj->do_DefaultTool != NULL
                   ? (const char *)dobj->do_DefaultTool : ""), 0);

    if (dobj->do_Type != e->type)
    {
        say("  ** type is %ld, should be %ld\n",
            (LONG)dobj->do_Type, (LONG)e->type);
        ok = FALSE;
    }
    if (!same((const char *)dobj->do_DefaultTool, e->deftool))
    {
        say("  ** default tool is not what was written\n", 0, 0);
        ok = FALSE;
    }
    if (e->stack != 0 && dobj->do_StackSize != e->stack)
    {
        say("  ** stack is %ld, should be %ld\n",
            dobj->do_StackSize, e->stack);
        ok = FALSE;
    }
    if (e->tooltypes >= 0 && tt != e->tooltypes)
    {
        say("  ** %ld tooltypes, should be %ld\n", tt, e->tooltypes);
        ok = FALSE;
    }
    if (dobj->do_Gadget.GadgetRender == NULL)
    {
        say("  ** no image\n", 0, 0);
        ok = FALSE;
    }
    if ((dobj->do_Type == WBDRAWER || dobj->do_Type == WBDISK) &&
        dobj->do_DrawerData == NULL)
    {
        say("  ** a drawer icon with no DrawerData\n", 0, 0);
        ok = FALSE;
    }

    /* Round trip: icon.library has to be able to write it back out. */
    if (!PutDiskObject((STRPTR)"RAM:icontest", dobj))
    {
        say("  ** PutDiskObject failed (IoErr %ld)\n", IoErr(), 0);
        ok = FALSE;
    }
    else
    {
        again = GetDiskObject((STRPTR)"RAM:icontest");
        if (again == NULL)
        {
            say("  ** the copy icon.library wrote will not load\n", 0, 0);
            ok = FALSE;
        }
        else
        {
            if (again->do_Type != dobj->do_Type ||
                again->do_Gadget.Width != dobj->do_Gadget.Width ||
                again->do_Gadget.Height != dobj->do_Gadget.Height)
            {
                say("  ** the round trip changed it\n", 0, 0);
                ok = FALSE;
            }
            FreeDiskObject(again);
        }
        DeleteFile((STRPTR)"RAM:icontest.info");
    }

    FreeDiskObject(dobj);
    return ok;
}

int main(void)
{
    const struct Expect *e;
    LONG                 bad = 0;

    out = Open((STRPTR)"DH0:icontest.txt", MODE_NEWFILE);
    if (out == 0)
        return RETURN_FAIL;

    for (e = expected; e->name != NULL; e++)
    {
        if (!check(e))
            bad++;
    }

    if (bad == 0)
        say("\nicontest: all icons load, match and round trip\n", 0, 0);
    else
        say("\nicontest: %ld icon(s) are wrong\n", bad, 0);

    Close(out);
    return bad == 0 ? RETURN_OK : RETURN_FAIL;
}

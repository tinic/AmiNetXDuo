/*
 * icontest, hand the generated .info files to the real icon.library and
 * see whether it agrees they are icons.
 *
 * install/tools/makeicon.py writes the DiskObject structure by hand, and
 * install/tools/showicon.py reads it back with an independently written
 * parser, but two programs by the same author agreeing proves only that
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
#include <string.h>

static const char *drawer = "DH0:AmiNetXDuo";

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
    { "Document",           WBPROJECT, "SYS:Utilities/More",   4096,  0 },
    { "Guide",              WBPROJECT, "SYS:Utilities/MultiView", 8192, 0 },
    { "NetPrefs",           WBTOOL,    NULL,                   8192,  0 },
    { "NetMeter",           WBTOOL,    NULL,                   8192,  1 },
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

static BOOL color_signature(struct DiskObject *dobj, ULONG *signature)
{
    LONG width = 0, height = 0, colors[2] = { 0, 0 }, transparent[2] = { -1, -1 };
    UBYTE *pixels[2] = { NULL, NULL }, *palette[2] = { NULL, NULL };
    struct TagItem tags[] =
    {
        { ICONCTRLA_GetWidth, (ULONG)&width },
        { ICONCTRLA_GetHeight, (ULONG)&height },
        { ICONCTRLA_GetPalette1, (ULONG)&palette[0] },
        { ICONCTRLA_GetPalette2, (ULONG)&palette[1] },
        { ICONCTRLA_GetPaletteSize1, (ULONG)&colors[0] },
        { ICONCTRLA_GetPaletteSize2, (ULONG)&colors[1] },
        { ICONCTRLA_GetImageData1, (ULONG)&pixels[0] },
        { ICONCTRLA_GetImageData2, (ULONG)&pixels[1] },
        { ICONCTRLA_GetTransparentColor1, (ULONG)&transparent[0] },
        { ICONCTRLA_GetTransparentColor2, (ULONG)&transparent[1] },
        { TAG_DONE, 0 }
    };
    ULONG hash = 2166136261UL;
    LONG state, i;

    *signature = 0;
    /* Stock 2.x/3.1 must load the fallback without calling V44 functions. */
    if (IconBase->lib_Version < 44)
        return TRUE;
    if (IconControlA(dobj, tags) != 10 || width != 48 || height != 48)
    {
        say("  ** color dimensions/API invalid: %ldx%ld\n", width, height);
        return FALSE;
    }
    for (state = 0; state < 2; state++)
    {
        if (pixels[state] == NULL || palette[state] == NULL ||
            colors[state] != 128 || transparent[state] != 0)
        {
            say("  ** color state %ld has invalid palette/transparency (%ld colors)\n",
                state, colors[state]);
            return FALSE;
        }
        for (i = 0; i < width * height; i++)
        {
            if (pixels[state][i] >= colors[state])
                return FALSE;
            hash = (hash ^ pixels[state][i]) * 16777619UL;
        }
        for (i = 0; i < colors[state] * 3; i++)
            hash = (hash ^ palette[state][i]) * 16777619UL;
    }
    say("  color      %ldx%ld, both states, 128 colors, transparent pen 0\n", width, height);
    *signature = hash;
    return TRUE;
}

static BOOL check(const struct Expect *e)
{
    char               path[128];
    struct DiskObject *dobj;
    struct DiskObject *again;
    LONG               tt;
    ULONG              color_before, color_after;
    BOOL               ok = TRUE;

    /* path = DRAWER/name, no dos.library string helpers needed */
    {
        const char *p = drawer;
        char       *d = path;

        if (strlen(drawer) + strlen(e->name) + 2 > sizeof(path))
            return FALSE;
        while (*p != '\0')
            *d++ = *p++;
        *d++ = '/';
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
    if (same(e->name, "NetMeter") &&
        FindToolType((CONST_STRPTR *)dobj->do_ToolTypes,
                     (CONST_STRPTR)"DONOTWAIT") == NULL)
    {
        say("  ** NetMeter lost DONOTWAIT\n", 0, 0);
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
    if (!color_signature(dobj, &color_before))
        ok = FALSE;

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
                again->do_Gadget.Height != dobj->do_Gadget.Height ||
                again->do_StackSize != dobj->do_StackSize ||
                !same((const char *)again->do_DefaultTool,
                      (const char *)dobj->do_DefaultTool) ||
                count_tooltypes((char **)again->do_ToolTypes) != tt)
            {
                say("  ** the round trip changed it\n", 0, 0);
                ok = FALSE;
            }
            if (!color_signature(again, &color_after) || color_after != color_before)
            {
                say("  ** the round trip changed color artwork\n", 0, 0);
                ok = FALSE;
            }
            {
                LONG i;
                for (i = 0; i < tt; i++)
                    if (again->do_ToolTypes == NULL || again->do_ToolTypes[i] == NULL ||
                        !same((const char *)again->do_ToolTypes[i],
                              (const char *)dobj->do_ToolTypes[i]))
                    {
                        say("  ** the round trip changed a tooltype\n", 0, 0);
                        ok = FALSE;
                        break;
                    }
            }
            FreeDiskObject(again);
        }
        DeleteFile((STRPTR)"RAM:icontest.info");
    }

    FreeDiskObject(dobj);
    return ok;
}

int main(int argc, char **argv)
{
    const struct Expect *e;
    LONG                 bad = 0;

    if (argc > 1)
        drawer = argv[1];
    out = Open((STRPTR)(argc > 2 ? argv[2] : "DH0:icontest.txt"), MODE_NEWFILE);
    if (out == 0)
        return RETURN_FAIL;
    say("icon.library %ld.%ld\n", IconBase->lib_Version, IconBase->lib_Revision);

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

/*
 * Every display mode the graphics card really publishes, as key=value.
 *
 * SPDX-License-Identifier: MIT
 */

#include <exec/types.h>
#include <dos/dos.h>
#include <graphics/displayinfo.h>
#include <graphics/modeid.h>

#include <proto/dos.h>
#include <proto/exec.h>
#include <proto/graphics.h>

#include <inline/macros.h>

#include "rtgout.h"

/* cybergraphics.library, hand-declared for the reason src/tools/httprtg.c
   declares it: the toolchain ships no header for it.  Both offsets here are
   ones that file already calls. */
#define CYBRIDATTR_PIXFMT       0x80000001UL
#define CYBRIDATTR_DEPTH        0x80000004UL

static const char *const pixfmt_name[] =
{
    "LUT8",     "RGB15",    "BGR15",    "RGB15PC",
    "BGR15PC",  "RGB16",    "BGR16",    "RGB16PC",
    "BGR16PC",  "RGB24",    "BGR24",    "ARGB32",
    "BGRA32",   "RGBA32"
};
#define PIXFMT_N  ((ULONG)(sizeof(pixfmt_name) / sizeof(pixfmt_name[0])))

/* OPENED HERE.  These bases are globals the proto/ inlines jump through and
   nothing in this executable's startup opens them; a first call to
   NextDisplayInfo() with a NULL GfxBase is a jump through zero. */
struct GfxBase *GfxBase;

static struct Library *CyberGfxBase;

static BOOL cgx_is_cyber(ULONG id)
{
    return LP1(0x36, BOOL, IsCyberModeID, ULONG, id, d0, , CyberGfxBase);
}

static ULONG cgx_id_attr(ULONG attr, ULONG id)
{
    return LP2(0x66, ULONG, GetCyberIDAttr, ULONG, attr, d0, ULONG, id, d1,
               , CyberGfxBase);
}

static const char *fmt_name(ULONG f)
{
    return (f < PIXFMT_N) ? pixfmt_name[f] : "unknown";
}

int main(VOID)
{
    ULONG id = (ULONG)INVALID_ID;
    ULONG n = 0, rtg = 0;
    ULONG a[6];

    GfxBase = (struct GfxBase *)
        OpenLibrary((CONST_STRPTR)"graphics.library", 39);
    if (GfxBase == NULL)
    {
        rtg_say("result=no graphics.library at V39\n", NULL);
        return RETURN_FAIL;
    }

    CyberGfxBase = OpenLibrary((CONST_STRPTR)"cybergraphics.library", 40);
    a[0] = (ULONG)(CyberGfxBase != NULL ? "yes" : "no");
    rtg_say("cybergraphics=%s\n", a);

    while ((id = NextDisplayInfo(id)) != (ULONG)INVALID_ID)
    {
        struct DimensionInfo dim;
        DisplayInfoHandle    handle;
        ULONG                depth, fmt;

        n++;
        if (CyberGfxBase == NULL || !cgx_is_cyber(id))
            continue;
        rtg++;

        handle = FindDisplayInfo(id);
        if (handle == NULL)
            continue;

        if (GetDisplayInfoData(handle, (UBYTE *)&dim, sizeof(dim), DTAG_DIMS,
                               0) == 0)
            continue;

        depth = cgx_id_attr(CYBRIDATTR_DEPTH, id);
        fmt   = cgx_id_attr(CYBRIDATTR_PIXFMT, id);

        /* The first three fields are the shape tests/tools/run-console.sh
           parses; anything added goes after them. */
        a[0] = id;
        a[1] = (ULONG)(dim.Nominal.MaxX - dim.Nominal.MinX + 1);
        a[2] = (ULONG)(dim.Nominal.MaxY - dim.Nominal.MinY + 1);
        a[3] = depth;
        a[4] = fmt;
        a[5] = (ULONG)fmt_name(fmt);
        rtg_say("rtgmode=%08lx %ldx%ld depth=%ld pixfmt=%ld %s\n", a);
    }

    a[0] = n;
    rtg_say("modes_total=%ld\n", a);
    a[0] = rtg;
    rtg_say("modes_rtg=%ld\n", a);
    rtg_say("result=listed\n", NULL);

    if (CyberGfxBase != NULL)
        CloseLibrary(CyberGfxBase);
    CloseLibrary((struct Library *)GfxBase);
    return RETURN_OK;
}

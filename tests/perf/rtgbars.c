/*
 * A KNOWN PICTURE on a truecolour graphics-card screen.
 *
 * SPDX-License-Identifier: MIT
 */

#include <exec/types.h>
#include <dos/dos.h>
#include <graphics/displayinfo.h>
#include <graphics/modeid.h>
#include <graphics/gfxbase.h>
#include <graphics/rastport.h>
#include <intuition/intuition.h>
#include <intuition/intuitionbase.h>
#include <intuition/screens.h>
#include <utility/tagitem.h>

#include <proto/dos.h>
#include <proto/exec.h>
#include <proto/graphics.h>
#include <proto/intuition.h>

#include <inline/macros.h>

#include "rtgout.h"

#define CYBRBIDTG_TB            (TAG_USER + 0x50000)
#define CYBRBIDTG_Depth         (CYBRBIDTG_TB + 0)
#define CYBRBIDTG_NominalWidth  (CYBRBIDTG_TB + 1)
#define CYBRBIDTG_NominalHeight (CYBRBIDTG_TB + 2)

#define CYBRMATTR_PIXFMT        0x80000004UL
#define CYBRMATTR_ISCYBERGFX    0x80000008UL

#define P96BMA_DEPTH            2UL
#define P96BMA_RGBFORMAT        7UL
#define P96BMA_ISP96            8UL

static struct Library *CyberGfxBase;
static struct Library *P96Base;

struct GfxBase       *GfxBase;
struct IntuitionBase *IntuitionBase;

static ULONG cgx_best(struct TagItem *tags)
{
    return LP1(0x3c, ULONG, BestCModeIDTagList, struct TagItem *, tags, a0,
               , CyberGfxBase);
}

static ULONG cgx_map_attr(struct BitMap *bm, ULONG attr)
{
    return LP2(0x60, ULONG, GetCyberMapAttr, struct BitMap *, bm, a0,
               ULONG, attr, d0, , CyberGfxBase);
}

static ULONG p96_map_attr(struct BitMap *bm, ULONG attr)
{
    return LP2(0x2a, ULONG, p96GetBitMapAttr, struct BitMap *, bm, a0,
               ULONG, attr, d0, , P96Base);
}

/* Picasso96's RGBFTYPE in its own order, and CyberGraphX's pixel formats in
   theirs.  The two are different numberings of the same idea, and the names go
   beside the numbers so a log line needs no lookup. */
static const char *const rgbfb_name[] =
{
    "NONE",     "CLUT",     "R8G8B8",   "B8G8R8",
    "R5G6B5PC", "R5G5B5PC", "A8R8G8B8", "A8B8G8R8",
    "R8G8B8A8", "B8G8R8A8", "R5G6B5",   "R5G5B5",
    "B5G6R5PC", "B5G5R5PC", "Y4U2V2",   "Y4U1V1"
};

static const char *const pixfmt_name[] =
{
    "LUT8",     "RGB15",    "BGR15",    "RGB15PC",
    "BGR15PC",  "RGB16",    "BGR16",    "RGB16PC",
    "BGR16PC",  "RGB24",    "BGR24",    "ARGB32",
    "BGRA32",   "RGBA32"
};

#define NELEMS(a)  ((ULONG)(sizeof(a) / sizeof((a)[0])))

static const char *name_of(const char *const *tab, ULONG n, ULONG v)
{
    return (v < n) ? tab[v] : "unknown";
}

#define BARS  8

static const UBYTE bar_rgb[BARS][3] =
{
    { 0x00, 0x00, 0x00 },       /* black   */
    { 0xFF, 0x00, 0x00 },       /* red     */
    { 0x00, 0xFF, 0x00 },       /* green   */
    { 0x00, 0x00, 0xFF },       /* blue    */
    { 0xFF, 0xFF, 0x00 },       /* yellow  */
    { 0xFF, 0x00, 0xFF },       /* magenta */
    { 0x00, 0xFF, 0xFF },       /* cyan    */
    { 0xFF, 0xFF, 0xFF }        /* white   */
};

static const char *const bar_name[BARS] =
{
    "black", "red", "green", "blue", "yellow", "magenta", "cyan", "white"
};

static ULONG bar_table[1 + BARS * 3 + 1];

static VOID build_table(VOID)
{
    ULONG i, at = 0;

    bar_table[at++] = ((ULONG)BARS << 16) | 0UL;
    for (i = 0; i < (ULONG)BARS; i++)
    {
        bar_table[at++] = (ULONG)bar_rgb[i][0] * 0x01010101UL;
        bar_table[at++] = (ULONG)bar_rgb[i][1] * 0x01010101UL;
        bar_table[at++] = (ULONG)bar_rgb[i][2] * 0x01010101UL;
    }
    bar_table[at] = 0UL;
}

static ULONG arg_word(const char **p, ULONG fallback, int hex)
{
    const char *s = *p;
    ULONG v = 0;
    int   digits = 0;

    while (*s == ' ' || *s == '\t')
        s++;
    if (hex && s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        s += 2;
    for (;;)
    {
        ULONG d;

        if (*s >= '0' && *s <= '9')
            d = (ULONG)(*s - '0');
        else if (hex && *s >= 'a' && *s <= 'f')
            d = (ULONG)(*s - 'a') + 10UL;
        else if (hex && *s >= 'A' && *s <= 'F')
            d = (ULONG)(*s - 'A') + 10UL;
        else
            break;
        v = v * (hex ? 16UL : 10UL) + d;
        s++;
        digits++;
    }
    *p = s;
    return digits ? v : fallback;
}

int main(VOID)
{
    const char      *args = (const char *)GetArgStr();
    struct Screen   *sc;
    struct RastPort *rp;
    ULONG depth, width, height, id, i;
    ULONG got_depth, p96fmt, cgxfmt;
    ULONG a[8];
    int   blink = 0;

    if (args == NULL)
        args = "";

    rtg_out_fh = Open((CONST_STRPTR)"DH0:rtgbars.txt", MODE_NEWFILE);

    depth  = arg_word(&args, 32UL, 0);
    width  = arg_word(&args, 640UL, 0);
    height = arg_word(&args, 480UL, 0);
    id     = arg_word(&args, (ULONG)INVALID_ID, 1);

    a[0] = depth; a[1] = width; a[2] = height; a[3] = id;
    rtg_say("args=%ld %ld %ld %08lx\n", a);

    GfxBase = (struct GfxBase *)
        OpenLibrary((CONST_STRPTR)"graphics.library", 39);
    IntuitionBase = (struct IntuitionBase *)
        OpenLibrary((CONST_STRPTR)"intuition.library", 39);
    if (GfxBase == NULL || IntuitionBase == NULL)
    {
        rtg_say("result=no graphics.library or intuition.library at V39\n",
                NULL);
        if (rtg_out_fh != (BPTR)0)
            Close(rtg_out_fh);
        return RETURN_FAIL;
    }

    CyberGfxBase = OpenLibrary((CONST_STRPTR)"cybergraphics.library", 40);
    P96Base      = OpenLibrary((CONST_STRPTR)"Picasso96API.library", 2);
    a[0] = (ULONG)(CyberGfxBase != NULL ? "yes" : "no");
    rtg_say("cybergraphics=%s\n", a);
    a[0] = (ULONG)(P96Base != NULL ? "yes" : "no");
    rtg_say("picasso96api=%s\n", a);

    if (id == (ULONG)INVALID_ID && CyberGfxBase != NULL)
    {
        struct TagItem tags[4];

        tags[0].ti_Tag = CYBRBIDTG_Depth;         tags[0].ti_Data = depth;
        tags[1].ti_Tag = CYBRBIDTG_NominalWidth;  tags[1].ti_Data = width;
        tags[2].ti_Tag = CYBRBIDTG_NominalHeight; tags[2].ti_Data = height;
        tags[3].ti_Tag = TAG_DONE;                tags[3].ti_Data = 0;

        id = cgx_best(tags);
        a[0] = id;
        rtg_say("best_mode=%08lx\n", a);
    }

    if (id == (ULONG)INVALID_ID)
    {
        a[0] = depth; a[1] = width; a[2] = height;
        rtg_say("result=no mode of depth %ld at %ldx%ld\n", a);
        if (rtg_out_fh != (BPTR)0)
            Close(rtg_out_fh);
        return RETURN_FAIL;
    }

    sc = OpenScreenTags(NULL,
                        SA_DisplayID, id,
                        SA_Width,     width,
                        SA_Height,    height,
                        SA_Depth,     depth,
                        SA_Type,      PUBLICSCREEN,
                        SA_PubName,   (ULONG)"RTGBARS",
                        SA_Title,     (ULONG)"AmiNetXDuo RTG colour bars",
                        SA_Quiet,     TRUE,
                        TAG_DONE);

    if (sc == NULL)
    {
        a[0] = id; a[1] = depth;
        rtg_say("result=OpenScreen refused mode %08lx at depth %ld\n", a);
        if (rtg_out_fh != (BPTR)0)
            Close(rtg_out_fh);
        return RETURN_FAIL;
    }

    rp = &sc->RastPort;

    got_depth = (P96Base != NULL && p96_map_attr(rp->BitMap, P96BMA_ISP96) != 0UL)
                  ? p96_map_attr(rp->BitMap, P96BMA_DEPTH)
                  : (ULONG)rp->BitMap->Depth;

    p96fmt = (P96Base != NULL && p96_map_attr(rp->BitMap, P96BMA_ISP96) != 0UL)
               ? p96_map_attr(rp->BitMap, P96BMA_RGBFORMAT) : (ULONG)-1;
    cgxfmt = (CyberGfxBase != NULL &&
              cgx_map_attr(rp->BitMap, CYBRMATTR_ISCYBERGFX) != 0UL)
               ? cgx_map_attr(rp->BitMap, CYBRMATTR_PIXFMT) : (ULONG)-1;

    a[0] = (ULONG)sc->Width; a[1] = (ULONG)sc->Height;
    a[2] = got_depth;        a[3] = id;
    rtg_say("screen=%ldx%ldx%ld mode=%08lx\n", a);

    if (p96fmt == (ULONG)-1)
    {
        rtg_say("p96_rgbformat=none\n", NULL);
    }
    else
    {
        a[0] = p96fmt;
        a[1] = (ULONG)name_of(rgbfb_name, NELEMS(rgbfb_name), p96fmt);
        rtg_say("p96_rgbformat=%ld %s\n", a);
    }

    if (cgxfmt == (ULONG)-1)
    {
        rtg_say("cgx_pixfmt=none\n", NULL);
    }
    else
    {
        a[0] = cgxfmt;
        a[1] = (ULONG)name_of(pixfmt_name, NELEMS(pixfmt_name), cgxfmt);
        rtg_say("cgx_pixfmt=%ld %s\n", a);
    }

    build_table();
    LoadRGB32(&sc->ViewPort, bar_table);

    for (i = 0; i < (ULONG)BARS; i++)
    {
        ULONG x0 = ((ULONG)sc->Width * i) / (ULONG)BARS;
        ULONG x1 = (i + 1 == (ULONG)BARS)
                     ? (ULONG)sc->Width - 1UL
                     : (((ULONG)sc->Width * (i + 1)) / (ULONG)BARS) - 1UL;

        SetAPen(rp, (ULONG)i);
        RectFill(rp, (WORD)x0, 0, (WORD)x1, (WORD)(sc->Height - 1));
    }

    /* The layout, so the checker and this file cannot drift apart in silence:
       whoever reads the PNG reads its expectation from this line. */
    for (i = 0; i < (ULONG)BARS; i++)
    {
        a[0] = i;
        a[1] = (ULONG)bar_name[i];
        rtg_say("bar%ld=%s\n", a);
    }
    rtg_say("result=open\n", NULL);

    /* CLOSED BEFORE THE SCREEN GOES UP.  Everything worth reading has been
       written, and the reader is a shell script watching the host directory
       DH0 lives in. */
    if (rtg_out_fh != (BPTR)0)
    {
        Close(rtg_out_fh);
        rtg_out_fh = (BPTR)0;
    }

    PubScreenStatus(sc, 0);
    ScreenToFront(sc);

    for (;;)
    {
        Delay(25);
        if ((SetSignal(0UL, 0UL) & SIGBREAKF_CTRL_C) != 0UL)
            break;

        blink = !blink;
        SetAPen(rp, blink ? 7UL : 0UL);
        RectFill(rp, 8, (WORD)(sc->Height - 40), 39, (WORD)(sc->Height - 9));
    }

    PubScreenStatus(sc, PSNF_PRIVATE);
    CloseScreen(sc);
    if (CyberGfxBase != NULL)
        CloseLibrary(CyberGfxBase);
    if (P96Base != NULL)
        CloseLibrary(P96Base);
    return RETURN_OK;
}

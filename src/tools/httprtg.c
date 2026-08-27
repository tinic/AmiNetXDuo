/*
 * Reading a graphics card's framebuffer back. See httprtg.h.
 *
 * Picasso96 and CyberGraphX headers are not in the NDK, so the nine calls used
 * here are hand-declared against inline/macros.h; each library offset is beside
 * its call, from Picasso96API_lib.fd and cybergraphics.fd. Neither library is
 * required to be present.
 *
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include "tools.h"
#include "httprtg.h"

#include <devices/timer.h>
#include <exec/memory.h>
#include <graphics/gfx.h>
#include <graphics/rastport.h>
#include <utility/tagitem.h>

#include <proto/exec.h>
#include <proto/graphics.h>
#include <proto/timer.h>

#include <inline/macros.h>

/* ------------------------------------------------- Picasso96API.library --- */

/* struct RenderInfo, as libraries/Picasso96.h has it. BytesPerRow is a WORD,
   which is what bounds the staging stride below. */
struct RtgRenderInfo
{
    APTR  Memory;
    WORD  BytesPerRow;
    WORD  pad;
    ULONG RGBFormat;
};

/* p96GetBitMapAttr attributes. BYTESPERROW, MEMORY and ISONBOARD are valid
   only while the bitmap is locked, which is why ISONBOARD is asked inside one. */
#define P96BMA_WIDTH         0UL
#define P96BMA_HEIGHT        1UL
#define P96BMA_DEPTH         2UL
#define P96BMA_MEMORY        3UL
#define P96BMA_BYTESPERROW   4UL
#define P96BMA_RGBFORMAT     7UL
#define P96BMA_ISP96         8UL
#define P96BMA_ISONBOARD     9UL

/* RGBFTYPE, in libraries/Picasso96.h's order. A "PC" name is the same channel
   order with the two bytes of a 16-bit pixel the other way round. */
#define RGBFB_NONE           0UL    /* planar, under P96's historical name   */
#define RGBFB_CLUT           1UL    /* palette indexed, one byte a pixel     */
#define RGBFB_R8G8B8         2UL
#define RGBFB_B8G8R8         3UL
#define RGBFB_R5G6B5PC       4UL
#define RGBFB_R5G5B5PC       5UL
#define RGBFB_A8R8G8B8       6UL
#define RGBFB_A8B8G8R8       7UL
/* 8 is RGBA and 9 is BGRA, in that order, taken from the RGBFTYPE enum itself:
   the other way round exchanges red and blue in every pixel. */
#define RGBFB_R8G8B8A8       8UL
#define RGBFB_B8G8R8A8       9UL
#define RGBFB_R5G6B5        10UL
#define RGBFB_R5G5B5        11UL
#define RGBFB_B5G6R5PC      12UL
#define RGBFB_B5G5R5PC      13UL
#define RGBFB_Y4U2V2        14UL
#define RGBFB_Y4U1V1        15UL
#define RGBFB_N             16UL

/* p96AllocBitMap: never displayed, never moves, needs no lock. */
#define P96_BMF_USERPRIVATE  0x8000UL
#define P96_BMF_CLEAR        0x0001UL

static struct Library *RtgP96Base;
static struct Library *RtgCgxBase;

static ULONG rtg_p96_attr(struct BitMap *bm, ULONG attr)
{
    return LP2(0x2a, ULONG, p96GetBitMapAttr, struct BitMap *, bm, a0,
               ULONG, attr, d0, , RtgP96Base);
}

static LONG rtg_p96_lock(struct BitMap *bm, struct RtgRenderInfo *ri)
{
    return LP3(0x30, LONG, p96LockBitMap, struct BitMap *, bm, a0,
               UBYTE *, (UBYTE *)ri, a1, ULONG, (ULONG)sizeof(*ri), d0,
               , RtgP96Base);
}

static VOID rtg_p96_unlock(struct BitMap *bm, LONG lock)
{
    LP2NR(0x36, p96UnlockBitMap, struct BitMap *, bm, a0, LONG, lock, d0,
          , RtgP96Base);
}

static VOID rtg_p96_read(struct RtgRenderInfo *ri, UWORD dx, UWORD dy,
                         struct RastPort *rp, UWORD sx, UWORD sy,
                         UWORD w, UWORD h)
{
    LP8NR(0x6c, p96ReadPixelArray, struct RtgRenderInfo *, ri, a0,
          UWORD, dx, d0, UWORD, dy, d1, struct RastPort *, rp, a1,
          UWORD, sx, d2, UWORD, sy, d3, UWORD, w, d4, UWORD, h, d5,
          , RtgP96Base);
}

static struct BitMap *rtg_p96_alloc(ULONG w, ULONG h, ULONG depth, ULONG flags,
                                    ULONG fmt)
{
    return LP6(0x1e, struct BitMap *, p96AllocBitMap, ULONG, w, d0,
               ULONG, h, d1, ULONG, depth, d2, ULONG, flags, d3,
               struct BitMap *, (struct BitMap *)NULL, a0, ULONG, fmt, d7,
               , RtgP96Base);
}

static VOID rtg_p96_free(struct BitMap *bm)
{
    LP1NR(0x24, p96FreeBitMap, struct BitMap *, bm, a0, , RtgP96Base);
}

/* --------------------------------------------------- cybergraphics.library - */

#define CYBRMATTR_PIXFMT      0x80000004UL
#define CYBRMATTR_WIDTH       0x80000005UL
#define CYBRMATTR_HEIGHT      0x80000006UL
#define CYBRMATTR_DEPTH       0x80000007UL
#define CYBRMATTR_ISCYBERGFX  0x80000008UL
#define CYBRMATTR_ISLINEARMEM 0x80000009UL

/* CyberGraphX numbers its pixel formats separately from Picasso96 and in a
   different order, so each has its own table below. */
#define PIXFMT_LUT8           0UL
#define PIXFMT_RGB15          1UL
#define PIXFMT_BGR15          2UL
#define PIXFMT_RGB15PC        3UL
#define PIXFMT_BGR15PC        4UL
#define PIXFMT_RGB16          5UL
#define PIXFMT_BGR16          6UL
#define PIXFMT_RGB16PC        7UL
#define PIXFMT_BGR16PC        8UL
#define PIXFMT_RGB24          9UL
#define PIXFMT_BGR24         10UL
#define PIXFMT_ARGB32        11UL
#define PIXFMT_BGRA32        12UL
#define PIXFMT_RGBA32        13UL
#define PIXFMT_N             14UL

#define RECTFMT_LUT8          3UL

#define LBMI_BYTESPERROW      0x84001006UL
#define LBMI_BASEADDRESS      0x84001007UL

/* graphics.library AllocBitMap's extended flags: how a CyberGraphX bitmap of a
   named pixel format is asked for. */
#define CGX_BMF_SPECIALFMT    (1UL << 7)
#define CGX_SHIFT_PIXFMT(f)   (((ULONG)(f)) << 24)

static ULONG rtg_cgx_attr(struct BitMap *bm, ULONG attr)
{
    return LP2(0x60, ULONG, GetCyberMapAttr, struct BitMap *, bm, a0,
               ULONG, attr, d0, , RtgCgxBase);
}

static APTR rtg_cgx_lock(struct BitMap *bm, struct TagItem *tags)
{
    return LP2(0xa8, APTR, LockBitMapTagList, APTR, (APTR)bm, a0,
               struct TagItem *, tags, a1, , RtgCgxBase);
}

static VOID rtg_cgx_unlock(APTR handle)
{
    LP1NR(0xae, UnLockBitMap, APTR, handle, a0, , RtgCgxBase);
}

static ULONG rtg_cgx_read(APTR dst, UWORD dx, UWORD dy, UWORD dmod,
                          struct RastPort *rp, UWORD sx, UWORD sy,
                          UWORD w, UWORD h, UBYTE fmt)
{
    return LP10(0x78, ULONG, ReadPixelArray, APTR, dst, a0, UWORD, dx, d0,
                UWORD, dy, d1, UWORD, dmod, d2, struct RastPort *, rp, a1,
                UWORD, sx, d3, UWORD, sy, d4, UWORD, w, d5, UWORD, h, d6,
                UBYTE, fmt, d7, , RtgCgxBase);
}

/* ------------------------------------------------------- pixel formats --- */

/*
 * A palette screen is delivered a byte a pixel; 15, 16, 24 and 32-bit screens
 * all become big-endian R5G6B5, so the encoder, the wire and the browser have
 * one truecolour path. A format neither table names is not converted and not
 * guessed at: the route fails and the screen is refused.
 */

#define RTG_L_R565  0U          /* rrrrrggg gggbbbbb */
#define RTG_L_R555  1U          /* xrrrrrgg gggbbbbb */
#define RTG_L_B565  2U          /* bbbbbggg gggrrrrr */
#define RTG_L_B555  3U          /* xbbbbbgg gggrrrrr */

/* One source pixel, once both libraries' numbering is on the same footing.
   `step` is source bytes a pixel, 0 meaning a format this does not read;
   r, g, b are the byte offsets in a 24 or 32-bit pixel. */
typedef struct RtgSrcFmt
{
    UBYTE step;
    UBYTE layout;
    UBYTE swap;
    UBYTE r;
    UBYTE g;
    UBYTE b;
} RtgSrcFmt;

static const RtgSrcFmt rtg_fmt_p96_tab[RGBFB_N] =
{
    { 0, 0,          0, 0, 0, 0 },      /* RGBFB_NONE, planar               */
    { 1, 0,          0, 0, 0, 0 },      /* RGBFB_CLUT                       */
    { 3, 0,          0, 0, 1, 2 },      /* RGBFB_R8G8B8                     */
    { 3, 0,          0, 2, 1, 0 },      /* RGBFB_B8G8R8                     */
    { 2, RTG_L_R565, 1, 0, 0, 0 },      /* RGBFB_R5G6B5PC                   */
    { 2, RTG_L_R555, 1, 0, 0, 0 },      /* RGBFB_R5G5B5PC                   */
    { 4, 0,          0, 1, 2, 3 },      /* RGBFB_A8R8G8B8                   */
    { 4, 0,          0, 3, 2, 1 },      /* RGBFB_A8B8G8R8                   */
    { 4, 0,          0, 0, 1, 2 },      /* RGBFB_R8G8B8A8                   */
    { 4, 0,          0, 2, 1, 0 },      /* RGBFB_B8G8R8A8                   */
    { 2, RTG_L_R565, 0, 0, 0, 0 },      /* RGBFB_R5G6B5, the wire format    */
    { 2, RTG_L_R555, 0, 0, 0, 0 },      /* RGBFB_R5G5B5                     */
    { 2, RTG_L_B565, 1, 0, 0, 0 },      /* RGBFB_B5G6R5PC                   */
    { 2, RTG_L_B555, 1, 0, 0, 0 },      /* RGBFB_B5G5R5PC                   */
    { 0, 0,          0, 0, 0, 0 },      /* RGBFB_Y4U2V2, not RGB at all     */
    { 0, 0,          0, 0, 0, 0 }       /* RGBFB_Y4U1V1                     */
};

static const RtgSrcFmt rtg_fmt_cgx_tab[PIXFMT_N] =
{
    { 1, 0,          0, 0, 0, 0 },      /* PIXFMT_LUT8                      */
    { 2, RTG_L_R555, 0, 0, 0, 0 },      /* PIXFMT_RGB15                     */
    { 2, RTG_L_B555, 0, 0, 0, 0 },      /* PIXFMT_BGR15                     */
    { 2, RTG_L_R555, 1, 0, 0, 0 },      /* PIXFMT_RGB15PC                   */
    { 2, RTG_L_B555, 1, 0, 0, 0 },      /* PIXFMT_BGR15PC                   */
    { 2, RTG_L_R565, 0, 0, 0, 0 },      /* PIXFMT_RGB16, the wire format    */
    { 2, RTG_L_B565, 0, 0, 0, 0 },      /* PIXFMT_BGR16                     */
    { 2, RTG_L_R565, 1, 0, 0, 0 },      /* PIXFMT_RGB16PC                   */
    { 2, RTG_L_B565, 1, 0, 0, 0 },      /* PIXFMT_BGR16PC                   */
    { 3, 0,          0, 0, 1, 2 },      /* PIXFMT_RGB24                     */
    { 3, 0,          0, 2, 1, 0 },      /* PIXFMT_BGR24                     */
    { 4, 0,          0, 1, 2, 3 },      /* PIXFMT_ARGB32                    */
    { 4, 0,          0, 2, 1, 0 },      /* PIXFMT_BGRA32                    */
    { 4, 0,          0, 0, 1, 2 }       /* PIXFMT_RGBA32                    */
};

static BOOL rtg_fmt_p96(ULONG fmt, RtgSrcFmt *out)
{
    if (fmt >= RGBFB_N || rtg_fmt_p96_tab[fmt].step == 0)
        return FALSE;
    *out = rtg_fmt_p96_tab[fmt];
    return TRUE;
}

static BOOL rtg_fmt_cgx(ULONG fmt, RtgSrcFmt *out)
{
    if (fmt >= PIXFMT_N || rtg_fmt_cgx_tab[fmt].step == 0)
        return FALSE;
    *out = rtg_fmt_cgx_tab[fmt];
    return TRUE;
}

/* A 16-bit pixel repacked as R5G6B5. Five bits of green become six by repeating
   the top bit at the bottom, so 0x1F stays full scale. */
static UWORD rtg_565_from16(UWORD v, UBYTE layout)
{
    UWORD r = 0, g = 0, b = 0;

    switch (layout)
    {
    case RTG_L_R555:
        r = (UWORD)((v >> 10) & 0x1FU);
        g = (UWORD)((v >> 5) & 0x1FU);
        b = (UWORD)(v & 0x1FU);
        g = (UWORD)((g << 1) | (g >> 4));
        break;

    case RTG_L_B565:
        b = (UWORD)((v >> 11) & 0x1FU);
        g = (UWORD)((v >> 5) & 0x3FU);
        r = (UWORD)(v & 0x1FU);
        break;

    case RTG_L_B555:
        b = (UWORD)((v >> 10) & 0x1FU);
        g = (UWORD)((v >> 5) & 0x1FU);
        r = (UWORD)(v & 0x1FU);
        g = (UWORD)((g << 1) | (g >> 4));
        break;

    default:
        return v;                       /* RTG_L_R565 is the wire format */
    }

    return (UWORD)((r << 11) | (g << 5) | b);
}

/* Byte loads throughout: a 24-bit mode puts the odd pixel on an odd address,
   and a 68000 traps on that. */
static VOID rtg_row16(UBYTE *dst, const UBYTE *src, UWORD pixels,
                      UBYTE layout, UBYTE swap)
{
    UWORD i;

    for (i = 0; i < pixels; i++)
    {
        UWORD v;

        if (swap != 0)
            v = (UWORD)(((UWORD)src[1] << 8) | (UWORD)src[0]);
        else
            v = (UWORD)(((UWORD)src[0] << 8) | (UWORD)src[1]);

        v = rtg_565_from16(v, layout);
        dst[0] = (UBYTE)(v >> 8);
        dst[1] = (UBYTE)v;
        src += 2;
        dst += 2;
    }
}

static VOID rtg_row8(UBYTE *dst, const UBYTE *src, UWORD pixels,
                     const RtgSrcFmt *f)
{
    UBYTE step = f->step;
    UBYTE ro = f->r;
    UBYTE go = f->g;
    UBYTE bo = f->b;
    UWORD i;

    for (i = 0; i < pixels; i++)
    {
        UWORD v = (UWORD)((((UWORD)(src[ro] >> 3)) << 11) |
                          (((UWORD)(src[go] >> 2)) << 5) |
                          ((UWORD)(src[bo] >> 3)));

        dst[0] = (UBYTE)(v >> 8);
        dst[1] = (UBYTE)v;
        src += step;
        dst += 2;
    }
}

/* R5G6B5 with the two bytes of every pixel the other way round, which is what
   Picasso96 gives a 16-bit screen on nearly every board -- the mode is named
   R5G6B5PC.  The channels are already the wire's, so the whole conversion is a
   byte swap, and a longword at a time does two pixels in one pass over the row.
   Through rtg_row16() below this was the slowest thing in the readback: it held
   every lock route to 2.7 MB/s on an emulated 68030, which is under an eighth
   of what p96ReadPixelArray converts at, so the probe picked the driver call
   and a mapping the console could have read directly went unused. */
static VOID rtg_row_swap16(UBYTE *dst, const UBYTE *src, UWORD pixels)
{
    UWORD i = pixels;

    /* Both ends longword-aligned, which a card row and the staging buffer are
       unless one of them starts on an odd pixel.  A 68000 traps on the wide
       load when they do not, so the test is correctness before it is speed. */
    if ((((ULONG)dst | (ULONG)src) & 3UL) == 0UL)
    {
        ULONG       *d = (ULONG *)dst;
        const ULONG *s = (const ULONG *)src;

        /* Eight pixels a turn.  The swap itself is four instructions on a
           68020 and the loop around it was most of what a two-pixel body
           cost. */
        while (i >= 8u)
        {
            ULONG a = s[0], b = s[1], c = s[2], e = s[3];

            d[0] = ((a & 0xFF00FF00UL) >> 8) | ((a & 0x00FF00FFUL) << 8);
            d[1] = ((b & 0xFF00FF00UL) >> 8) | ((b & 0x00FF00FFUL) << 8);
            d[2] = ((c & 0xFF00FF00UL) >> 8) | ((c & 0x00FF00FFUL) << 8);
            d[3] = ((e & 0xFF00FF00UL) >> 8) | ((e & 0x00FF00FFUL) << 8);
            s += 4;
            d += 4;
            i = (UWORD)(i - 8u);
        }

        while (i >= 2u)
        {
            ULONG v = *s++;

            *d++ = ((v & 0xFF00FF00UL) >> 8) | ((v & 0x00FF00FFUL) << 8);
            i = (UWORD)(i - 2u);
        }

        dst = (UBYTE *)d;
        src = (const UBYTE *)s;
    }

    while (i-- != 0u)
    {
        UBYTE lo = src[0];

        dst[0] = src[1];
        dst[1] = lo;
        src += 2;
        dst += 2;
    }
}

/* One row of `pixels` from the card's format into the wire's. */
static VOID rtg_cvt_row(UBYTE *dst, const UBYTE *src, UWORD pixels,
                        const RtgSrcFmt *f)
{
    if (f->step != 2)
    {
        rtg_row8(dst, src, pixels, f);
        return;
    }

    /* The two cases a 16-bit screen is really in.  Neither reaches the general
       loop, which is left for the 15-bit layouts that have to repack green. */
    if (f->layout == RTG_L_R565)
    {
        if (f->swap == 0)
            memcpy(dst, src, (size_t)pixels * 2u);   /* already the wire's */
        else
            rtg_row_swap16(dst, src, pixels);
        return;
    }

    rtg_row16(dst, src, pixels, f->layout, f->swap);
}

/* --------------------------------------------------------------- routes --- */

enum
{
    RTG_R_P96_RPA = 0,      /* p96ReadPixelArray into a RenderInfo over dst */
    RTG_R_P96_LOCK,         /* p96LockBitMap, then read the mapping         */
    RTG_R_CGX_RPA,          /* cybergraphics ReadPixelArray, RECTFMT_LUT8   */
    RTG_R_CGX_LOCK,         /* LockBitMapTagList, then read the mapping     */
    RTG_R_BLIT,             /* BltBitMap to an offscreen copy, read that    */
    RTG_N_ROUTE
};

/* Short, because they go out in a control word a viewer logs on one line. */
static const char *const rtg_route_name[RTG_N_ROUTE] =
{
    "p96r", "p96l", "cgxr", "cgxl", "blit"
};

/* -------------------------------------------------------------- session --- */

static struct RastPort *rtg_rp;         /* the screen's, for the driver reads */
static struct BitMap   *rtg_bm;         /* the bitmap whose routes were probed */
static UWORD           rtg_w;           /* pixels a row                       */
static UWORD           rtg_h;
static ULONG           rtg_stride;      /* the caller's staging row stride    */
static UBYTE           rtg_bpp;         /* staging bytes a pixel: 1 or 2      */
static ULONG           rtg_row_bytes;   /* rtg_w * rtg_bpp, the delivered row */
static UWORD           rtg_depth;       /* the card's bits a pixel            */
/* The screen's own pixel format under each library, or -1 where that library
   does not claim the bitmap.  Resolved once at attach, because it decides how
   the lock routes convert and what format the snapshot offscreen is in. */
static LONG            rtg_native_p96 = -1;
static LONG            rtg_native_cgx = -1;
static UBYTE           rtg_on_board;    /* P96BMA_ISONBOARD / ISLINEARMEM     */
static UBYTE           rtg_on_board_known;
static int             rtg_route = -1;
static ULONG           rtg_kbs[RTG_N_ROUTE];   /* 0 = the route was not there */
/* Routes that answered, and answered with something that was not the screen.
   One bit each, kept rather than folded into rtg_kbs, because the rate is the
   evidence: a route that reads a band seventy times faster than every other
   route on the same board is not fast, and both numbers have to be visible in
   the same word for that to be readable. */
static ULONG           rtg_stale;

/* The offscreen the snapshot route blits into.  Allocated only when the
   screen's bitmap is really in card memory: when P96 kept it in system RAM
   there is nothing to snapshot away from and it would be a megabyte to copy
   Fast RAM to Fast RAM. */
static struct BitMap  *rtg_off;
static UBYTE           rtg_off_is_p96;

/* How tall one fetch is.  The read is whole rows either way.  This bounds how
   long a single library call holds the bitmap locked, which on P96 stops
   screen switching for as long as it lasts.  128 rows of a 1280-wide screen
   is 160 KB, which at the worst read rate a card has been reported at is
   about a fortieth of a second. */
#define RTG_STRIP_ROWS      128u

/* What the probe reads, and how many times.  The fastest of three passes:
   preemption can only make a pass slower, so the minimum is the one that
   measured the hardware rather than the scheduler. */
#define RTG_PROBE_ROWS      64u
#define RTG_PROBE_PASSES    3u

extern struct Device *TimerBase;        /* compat.c, for the ReadEClock inline */

/* --------------------------------------------------------------- opening -- */

BOOL http_rtg_open(VOID)
{
    if (RtgP96Base == NULL)
        RtgP96Base = OpenLibrary((CONST_STRPTR)"Picasso96API.library", 2);
    if (RtgCgxBase == NULL)
        RtgCgxBase = OpenLibrary((CONST_STRPTR)"cybergraphics.library", 40);

    return (BOOL)(RtgP96Base != NULL || RtgCgxBase != NULL);
}

VOID http_rtg_close(VOID)
{
    http_rtg_detach();

    if (RtgP96Base != NULL)
    {
        CloseLibrary(RtgP96Base);
        RtgP96Base = NULL;
    }
    if (RtgCgxBase != NULL)
    {
        CloseLibrary(RtgCgxBase);
        RtgCgxBase = NULL;
    }
}

BOOL http_rtg_present(VOID)
{
    return (BOOL)(RtgP96Base != NULL || RtgCgxBase != NULL);
}

/* ------------------------------------------------------------ describing -- */

/*
 * Asked before BMF_STANDARD: a card's bitmap can carry that flag too, and its
 * Planes[] are not eight bitplanes. Both libraries answer for any bitmap,
 * including one that belongs to neither, which makes this safe to ask first.
 */
BOOL http_rtg_owns(struct BitMap *bm)
{
    if (bm == NULL)
        return FALSE;

    if (RtgP96Base != NULL && rtg_p96_attr(bm, P96BMA_ISP96) != 0UL)
        return TRUE;
    if (RtgCgxBase != NULL && rtg_cgx_attr(bm, CYBRMATTR_ISCYBERGFX) != 0UL)
        return TRUE;

    return FALSE;
}

BOOL http_rtg_describe(struct BitMap *bm, UWORD visible_w, HttpRtgScreen *s,
                       const char **why)
{
    RtgSrcFmt fmt = { 0, 0, 0, 0, 0, 0 };
    ULONG     w = 0, h = 0, depth = 0, native = 0;
    int       answered = 0;
    int       is_p96 = 0;

    if (why != NULL)
        *why = NULL;
    if (bm == NULL || s == NULL)
        return FALSE;

    if (RtgP96Base != NULL && rtg_p96_attr(bm, P96BMA_ISP96) != 0UL)
    {
        w      = rtg_p96_attr(bm, P96BMA_WIDTH);
        h      = rtg_p96_attr(bm, P96BMA_HEIGHT);
        depth  = rtg_p96_attr(bm, P96BMA_DEPTH);
        native = rtg_p96_attr(bm, P96BMA_RGBFORMAT);
        is_p96 = 1;
        answered = 1;
    }
    else if (RtgCgxBase != NULL && rtg_cgx_attr(bm, CYBRMATTR_ISCYBERGFX) != 0UL)
    {
        w      = rtg_cgx_attr(bm, CYBRMATTR_WIDTH);
        h      = rtg_cgx_attr(bm, CYBRMATTR_HEIGHT);
        depth  = rtg_cgx_attr(bm, CYBRMATTR_DEPTH);
        native = rtg_cgx_attr(bm, CYBRMATTR_PIXFMT);
        answered = 1;
    }

    if (!answered)
    {
        if (why != NULL)
            *why = "the front screen is not a bitmap this can read: it has no "
                   "bitplanes and neither Picasso96 nor CyberGraphX claims it";
        return FALSE;
    }

    if (w < 1UL || w > 16384UL || h < 1UL || h > 16384UL)
    {
        if (why != NULL)
            *why = "the front screen's size does not fit the wire format";
        return FALSE;
    }

    /*
     * The pixel format decides this, not the depth. What is left to refuse is
     * a bitmap with no chunky pixels, a YUV overlay, and a format nobody here
     * has seen. The refusal names what it found.
     */
    if (!(is_p96 ? rtg_fmt_p96(native, &fmt) : rtg_fmt_cgx(native, &fmt)))
    {
        if (why != NULL)
        {
            if (is_p96 && native == RGBFB_NONE)
                *why = "the front screen is a planar bitmap Picasso96 is "
                       "holding, so there are no chunky pixels here to read";
            else if (is_p96 && (native == RGBFB_Y4U2V2 ||
                                native == RGBFB_Y4U1V1))
                *why = "the front screen is a YUV overlay screen. The console "
                       "serves palette and truecolour RTG screens";
            else
                *why = "the front screen is an RTG screen in a pixel format "
                       "this cannot read";
        }
        return FALSE;
    }

    /*
     * THE BITMAP IS NOT THE SCREEN. P96BMA_WIDTH and CYBRMATTR_WIDTH answer for
     * the allocation, which a board rounds up to its own pitch. The screen's
     * own width wins whenever it is smaller; larger is not honoured.
     */
    if (visible_w != 0 && (ULONG)visible_w < w)
        w = (ULONG)visible_w;

    s->width  = (UWORD)w;
    s->height = (UWORD)h;
    s->depth  = (UWORD)depth;
    s->bpp    = (UBYTE)((fmt.step == 1) ? 1 : 2);
    return TRUE;
}

/* -------------------------------------------------------- the five reads -- */

/* One strip through one route: rows [y0, y0+rows) of the full width, into `dst`
   at the attach stride. Every route delivers exactly `rtg_row_bytes` a row and
   leaves the padding past it alone. */

static BOOL rtg_read_p96_rpa(UBYTE *dst, UWORD y0, UWORD rows)
{
    struct RtgRenderInfo ri;

    if (RtgP96Base == NULL)
        return FALSE;

    ri.Memory      = (APTR)dst;
    ri.BytesPerRow = (WORD)rtg_stride;
    ri.pad         = 0;
    /* The destination format, not the screen's: p96ReadPixelArray converts
       between the two, so this is the one route with no conversion of its own. */
    ri.RGBFormat   = (rtg_bpp == 2) ? RGBFB_R5G6B5 : RGBFB_CLUT;

    rtg_p96_read(&ri, 0, 0, rtg_rp, 0, y0, rtg_w, rows);
    return TRUE;
}

static BOOL rtg_read_p96_lock(struct BitMap *bm, UBYTE *dst,
                              UWORD y0, UWORD rows)
{
    struct RtgRenderInfo ri;
    RtgSrcFmt            fmt = { 0, 0, 0, 0, 0, 0 };
    const UBYTE         *src;
    LONG                 lock;
    UWORD                r;

    if (RtgP96Base == NULL)
        return FALSE;

    memset(&ri, 0, sizeof(ri));
    lock = rtg_p96_lock(bm, &ri);
    if (lock == 0L || ri.Memory == NULL || ri.BytesPerRow <= 0)
    {
        if (lock != 0L)
            rtg_p96_unlock(bm, lock);
        return FALSE;
    }

    /* The mapping is in the screen's format. A format the converter does not
       name, or one that disagrees with the attach, fails the route. */
    if (!rtg_fmt_p96(ri.RGBFormat, &fmt) ||
        (fmt.step == 1) != (rtg_bpp == 1))
    {
        rtg_p96_unlock(bm, lock);
        return FALSE;
    }

    src = (const UBYTE *)ri.Memory + (ULONG)y0 * (ULONG)(UWORD)ri.BytesPerRow;
    for (r = 0; r < rows; r++)
    {
        if (rtg_bpp == 1)
            memcpy(dst, src, (size_t)rtg_w);
        else
            rtg_cvt_row(dst, src, rtg_w, &fmt);
        dst += rtg_stride;
        src += (UWORD)ri.BytesPerRow;
    }

    rtg_p96_unlock(bm, lock);
    return TRUE;
}

/*
 * THE COUNT IS THE ONLY THING THAT SAYS IT READ ANYTHING. Picasso96's
 * cybergraphics emulation declines RECTFMT_LUT8 and returns 0 at once, so a
 * route that ignored the count came back instantly having copied nothing and
 * won the probe.
 */
static BOOL rtg_read_cgx_rpa(UBYTE *dst, UWORD y0, UWORD rows)
{
    ULONG got;

    if (RtgCgxBase == NULL)
        return FALSE;

    /* Not offered for a truecolour screen: cybergraphics has no big-endian
       R5G6B5 rectangle format to ask for. */
    if (rtg_bpp != 1)
        return FALSE;

    got = rtg_cgx_read((APTR)dst, 0, 0, (UWORD)rtg_stride, rtg_rp,
                       0, y0, rtg_w, rows, (UBYTE)RECTFMT_LUT8);
    return got == (ULONG)rtg_w * (ULONG)rows;
}

static BOOL rtg_read_cgx_lock(struct BitMap *bm, UBYTE *dst,
                              UWORD y0, UWORD rows)
{
    struct TagItem tags[3];
    RtgSrcFmt      fmt = { 0, 0, 0, 0, 0, 0 };
    ULONG          base = 0;
    ULONG          bpr = 0;
    APTR           handle;
    const UBYTE   *src;
    UWORD          r;

    if (RtgCgxBase == NULL)
        return FALSE;

    /* Asked of the bitmap rather than carried in from attach, because the blit
       route brings its own offscreen through here. */
    if (!rtg_fmt_cgx(rtg_cgx_attr(bm, CYBRMATTR_PIXFMT), &fmt) ||
        (fmt.step == 1) != (rtg_bpp == 1))
        return FALSE;

    tags[0].ti_Tag = LBMI_BASEADDRESS; tags[0].ti_Data = (ULONG)&base;
    tags[1].ti_Tag = LBMI_BYTESPERROW; tags[1].ti_Data = (ULONG)&bpr;
    tags[2].ti_Tag = TAG_DONE;         tags[2].ti_Data = 0;

    handle = rtg_cgx_lock(bm, tags);
    if (handle == NULL || base == 0UL || bpr == 0UL)
    {
        if (handle != NULL)
            rtg_cgx_unlock(handle);
        return FALSE;
    }

    src = (const UBYTE *)base + (ULONG)y0 * bpr;
    for (r = 0; r < rows; r++)
    {
        if (rtg_bpp == 1)
            memcpy(dst, src, (size_t)rtg_w);
        else
            rtg_cvt_row(dst, src, rtg_w, &fmt);
        dst += rtg_stride;
        src += bpr;
    }

    rtg_cgx_unlock(handle);
    return TRUE;
}

/*
 * BltBitMap to an offscreen bitmap and read the copy. The layer lock is only
 * attempted, never waited for, so a frame read straight out of VRAM can carry
 * half of somebody else's redraw. The blit is one card-side call, so what it
 * snapshots is one moment.
 */
static BOOL rtg_read_blit(struct BitMap *bm, UBYTE *dst, UWORD y0, UWORD rows)
{
    if (rtg_off == NULL)
        return FALSE;

    BltBitMap(bm, 0, (WORD)y0, rtg_off, 0, 0, (WORD)rtg_w, (WORD)rows,
              0xC0, 0xFF, NULL);
    WaitBlit();

    /* The copy is a plain memory bitmap in the screen's own format, so reading
       it is a lock and the same conversion a direct lock does. */
    if (rtg_off_is_p96)
        return rtg_read_p96_lock(rtg_off, dst, 0, rows);
    return rtg_read_cgx_lock(rtg_off, dst, 0, rows);
}

static BOOL rtg_read_via(int route, struct BitMap *bm, UBYTE *dst,
                         UWORD y0, UWORD rows)
{
    switch (route)
    {
    case RTG_R_P96_RPA:  return rtg_read_p96_rpa(dst, y0, rows);
    case RTG_R_P96_LOCK: return rtg_read_p96_lock(bm, dst, y0, rows);
    case RTG_R_CGX_RPA:  return rtg_read_cgx_rpa(dst, y0, rows);
    case RTG_R_CGX_LOCK: return rtg_read_cgx_lock(bm, dst, y0, rows);
    case RTG_R_BLIT:     return rtg_read_blit(bm, dst, y0, rows);
    default:             return FALSE;
    }
}

/* ---------------------------------------------------------- the offscreen -- */

static VOID rtg_off_free(VOID)
{
    if (rtg_off == NULL)
        return;

    if (rtg_off_is_p96)
        rtg_p96_free(rtg_off);
    else
        FreeBitMap(rtg_off);

    rtg_off = NULL;
    rtg_off_is_p96 = 0;
}

static BOOL rtg_off_take(UWORD w, UWORD rows)
{
    rtg_off_free();

    /*
     * BMF_USERPRIVATE is Picasso96's "never displayed, never moved". Allocated
     * in the screen's own pixel format: BltBitMap between two bitmaps of
     * different formats moves bits, not colours, and is not a conversion.
     */
    if (RtgP96Base != NULL && rtg_native_p96 >= 0)
    {
        rtg_off = rtg_p96_alloc((ULONG)w, (ULONG)rows, (ULONG)rtg_depth,
                                P96_BMF_USERPRIVATE | P96_BMF_CLEAR,
                                (ULONG)rtg_native_p96);
        if (rtg_off != NULL)
        {
            rtg_off_is_p96 = 1;
            return TRUE;
        }
    }

    if (RtgCgxBase != NULL && rtg_native_cgx >= 0)
    {
        rtg_off = AllocBitMap((ULONG)w, (ULONG)rows, (ULONG)rtg_depth,
                              BMF_MINPLANES | CGX_BMF_SPECIALFMT |
                              CGX_SHIFT_PIXFMT((ULONG)rtg_native_cgx),
                              NULL);
        if (rtg_off != NULL)
        {
            rtg_off_is_p96 = 0;
            return TRUE;
        }
    }

    return FALSE;
}

/* ----------------------------------------------------------- the measure -- */

/*
 * A ROUTE THAT ANSWERS IS NOT A ROUTE THAT READ THE SCREEN. The same band, read
 * through the route and through a reference route that can only be a memcpy out
 * of mapped board memory. `ref` is packed, `dst` is strided. The screen is live,
 * so a disagreement only counts when the reference agrees with itself across
 * the same interval.
 */
static BOOL rtg_verify(int route, int ref_route, struct BitMap *bm,
                       UBYTE *dst, UWORD rows, UBYTE *ref)
{
    UWORD r;

    if (!rtg_read_via(ref_route, bm, dst, 0, rows))
        return TRUE;                    /* nothing to check against */
    for (r = 0; r < rows; r++)
        memcpy(ref + (ULONG)r * rtg_row_bytes, dst + (ULONG)r * rtg_stride,
               (size_t)rtg_row_bytes);

    /*
     * POISONED FIRST, or the check checks nothing: the reference was just read
     * into this same buffer, so a route that writes nothing would compare equal
     * to it.
     */
    for (r = 0; r < rows; r++)
        memset(dst + (ULONG)r * rtg_stride, 0xA5, (size_t)rtg_row_bytes);

    if (!rtg_read_via(route, bm, dst, 0, rows))
        return FALSE;

    for (r = 0; r < rows; r++)
        if (memcmp(ref + (ULONG)r * rtg_row_bytes, dst + (ULONG)r * rtg_stride,
                   (size_t)rtg_row_bytes) != 0)
            break;
    if (r == rows)
        return TRUE;

    /* They differ.  Did the screen? */
    if (!rtg_read_via(ref_route, bm, dst, 0, rows))
        return TRUE;
    for (r = 0; r < rows; r++)
        if (memcmp(ref + (ULONG)r * rtg_row_bytes, dst + (ULONG)r * rtg_stride,
                   (size_t)rtg_row_bytes) != 0)
            return TRUE;                /* the screen moved, not the route */

    return FALSE;
}

/*
 * Every route reads the same band three times and the fastest pass is kept: a
 * slow pass can be the scheduler, a fast one can only be the hardware. Every
 * route is then read against a reference route before it may be chosen, because
 * the fastest is the one most likely not to have read anything. The EClock is
 * ~709 kHz; DateStamp() ticks are fiftieths and would make these a guess.
 */
static VOID rtg_probe(struct BitMap *bm, UBYTE *dst)
{
    struct EClockVal a, b;
    ULONG            rate;
    UWORD            rows = (UWORD)((rtg_h < RTG_PROBE_ROWS)
                                    ? rtg_h : RTG_PROBE_ROWS);
    ULONG            bytes = rtg_row_bytes * (ULONG)rows;
    int              r;
    ULONG            pass;
    UBYTE           *ref;
    int              ref_route = -1;

    rtg_route = -1;
    rtg_stale = 0;
    for (r = 0; r < RTG_N_ROUTE; r++)
        rtg_kbs[r] = 0;

    (VOID)ami_millis();                 /* opens timer.device, sets TimerBase */
    if (TimerBase == NULL)
    {
        /* No clock to measure with. The order below is the one to prefer when
           nothing can be timed: a mapping and a memcpy beats a driver call. */
        static const int fallback[RTG_N_ROUTE] =
            { RTG_R_BLIT, RTG_R_P96_LOCK, RTG_R_CGX_LOCK,
              RTG_R_P96_RPA, RTG_R_CGX_RPA };

        for (r = 0; r < RTG_N_ROUTE; r++)
        {
            if (rtg_read_via(fallback[r], bm, dst, 0, rows))
            {
                rtg_route = fallback[r];
                return;
            }
        }
        return;
    }

    rate = ReadEClock(&a);
    if (rate == 0UL)
        rate = 709379UL;

    for (r = 0; r < RTG_N_ROUTE; r++)
    {
        ULONG best = 0;

        for (pass = 0; pass < RTG_PROBE_PASSES; pass++)
        {
            ULONG took;

            ReadEClock(&a);
            if (!rtg_read_via(r, bm, dst, 0, rows))
            {
                best = 0;
                break;
            }
            ReadEClock(&b);

            /* The low word alone: at ~709 kHz it wraps every hundred minutes
               and the subtraction is correct across one wrap. */
            took = b.ev_lo - a.ev_lo;
            if (took == 0UL)
                took = 1UL;

            /* KB a second without overflowing: the band is at most two
               megabytes, so bytes/1024 times a 709 kHz rate stays in 32 bits. */
            {
                ULONG kbs = (bytes / 1024UL) * rate / took;
                if (kbs > best)
                    best = kbs;
            }
        }

        rtg_kbs[r] = best;
    }

    /*
     * THE REFERENCE is a mapping and a memcpy rather than a driver call: a lock
     * route hands back the address of the pixels the card is displaying. The
     * blit route is last, because an offscreen is one more place to be stale.
     */
    {
        static const int prefer[3] =
            { RTG_R_P96_LOCK, RTG_R_CGX_LOCK, RTG_R_BLIT };
        int i;

        for (i = 0; i < 3; i++)
            if (rtg_kbs[prefer[i]] != 0UL)
            {
                ref_route = prefer[i];
                break;
            }
    }

    /* No reference, or no room to hold one: choose on the clock alone. */
    ref = (ref_route < 0) ? NULL : (UBYTE *)AllocVec(bytes, MEMF_ANY);

    for (r = 0; r < RTG_N_ROUTE; r++)
    {
        if (rtg_kbs[r] == 0UL)
            continue;
        if (ref != NULL && r != ref_route &&
            !rtg_verify(r, ref_route, bm, dst, rows, ref))
        {
            rtg_stale |= 1UL << (ULONG)r;
            continue;
        }
        if (rtg_route < 0 || rtg_kbs[r] > rtg_kbs[rtg_route])
            rtg_route = r;
    }

    if (ref != NULL)
        FreeVec((APTR)ref);
}

/* ---------------------------------------------------------------- attach -- */

BOOL http_rtg_attach(struct BitMap *bm, struct RastPort *rp,
                     UWORD width, UWORD height, ULONG stride, UBYTE *probe)
{
    RtgSrcFmt fmt = { 0, 0, 0, 0, 0, 0 };
    LONG      p96f = -1;
    LONG      cgxf = -1;
    ULONG     depth = 0;
    ULONG     bpp = 0;

    http_rtg_detach();

    if (bm == NULL || rp == NULL || probe == NULL)
        return FALSE;
    if (width == 0 || height == 0)
        return FALSE;

    /*
     * The screen's own pixel format, resolved once. Both attributes are
     * answered without the bitmap locked, and both libraries are asked, because
     * a Picasso96 screen answers CyberGraphX's questions too.
     */
    if (RtgP96Base != NULL && rtg_p96_attr(bm, P96BMA_ISP96) != 0UL)
    {
        p96f  = (LONG)rtg_p96_attr(bm, P96BMA_RGBFORMAT);
        depth = rtg_p96_attr(bm, P96BMA_DEPTH);
    }
    if (RtgCgxBase != NULL && rtg_cgx_attr(bm, CYBRMATTR_ISCYBERGFX) != 0UL)
    {
        cgxf = (LONG)rtg_cgx_attr(bm, CYBRMATTR_PIXFMT);
        if (depth == 0UL)
            depth = rtg_cgx_attr(bm, CYBRMATTR_DEPTH);
    }

    /* A format the converter does not name is dropped here rather than left to
       fail a route later, so the offscreen is never allocated in one. */
    if (p96f >= 0 && rtg_fmt_p96((ULONG)p96f, &fmt))
        bpp = (fmt.step == 1) ? 1UL : 2UL;
    else
        p96f = -1;

    if (cgxf >= 0 && rtg_fmt_cgx((ULONG)cgxf, &fmt))
    {
        if (bpp == 0UL)
            bpp = (fmt.step == 1) ? 1UL : 2UL;
    }
    else
        cgxf = -1;

    /* http_rtg_describe() refused anything this cannot place, so reaching here
       means the screen changed under the caller. */
    if (bpp == 0UL)
        return FALSE;
    if (depth == 0UL)
        depth = bpp * 8UL;

    if (stride < (ULONG)width * bpp)
        return FALSE;
    /* struct RenderInfo carries the stride in a WORD, and the P96 read route
       is the one that fills one in. */
    if (stride > 32767UL)
        return FALSE;

    rtg_rp         = rp;
    rtg_bm         = bm;
    rtg_w          = width;
    rtg_h          = height;
    rtg_stride     = stride;
    rtg_bpp        = (UBYTE)bpp;
    rtg_row_bytes  = (ULONG)width * bpp;
    rtg_depth      = (UWORD)depth;
    rtg_native_p96 = p96f;
    rtg_native_cgx = cgxf;

    /*
     * Whether the bitmap is really in card memory is the first branch: P96 may
     * keep it in system RAM, in which case the read is Fast RAM speed and the
     * snapshot buffer is a copy for nothing. P96 only answers while the bitmap
     * is locked, which is why it is asked here and not in http_rtg_describe().
     */
    rtg_on_board = 0;
    rtg_on_board_known = 0;

    if (RtgP96Base != NULL && rtg_p96_attr(bm, P96BMA_ISP96) != 0UL)
    {
        struct RtgRenderInfo ri;
        LONG                 lock;

        memset(&ri, 0, sizeof(ri));
        lock = rtg_p96_lock(bm, &ri);
        if (lock != 0L)
        {
            rtg_on_board = (UBYTE)(rtg_p96_attr(bm, P96BMA_ISONBOARD) != 0UL);
            rtg_on_board_known = 1;
            rtg_p96_unlock(bm, lock);
        }
    }
    else if (RtgCgxBase != NULL)
    {
        /* CyberGraphX has no on-the-board attribute. ISLINEARMEM is the nearest
           one, and a linearly addressable bitmap is one a memcpy can read. */
        rtg_on_board = (UBYTE)(rtg_cgx_attr(bm, CYBRMATTR_ISLINEARMEM) != 0UL);
        rtg_on_board_known = 1;
    }

    /* One strip's worth rather than a whole screen: the snapshot is per fetch. */
    if (rtg_on_board)
    {
        UWORD rows = (UWORD)((rtg_h < RTG_STRIP_ROWS) ? rtg_h : RTG_STRIP_ROWS);
        (VOID)rtg_off_take(rtg_w, rows);
    }

    rtg_probe(bm, probe);

    return (BOOL)(rtg_route >= 0);
}

VOID http_rtg_detach(VOID)
{
    rtg_off_free();
    rtg_rp = NULL;
    rtg_bm = NULL;
    rtg_w = 0;
    rtg_h = 0;
    rtg_stride = 0;
    rtg_bpp = 0;
    rtg_row_bytes = 0;
    rtg_depth = 0;
    rtg_native_p96 = -1;
    rtg_native_cgx = -1;
    rtg_route = -1;
    rtg_on_board = 0;
    rtg_on_board_known = 0;
}

BOOL http_rtg_attached_to(struct BitMap *bm)
{
    return (BOOL)(bm != NULL && bm == rtg_bm && rtg_route >= 0);
}

/* ------------------------------------------------------------ the fetch --- */

BOOL http_rtg_read(struct BitMap *bm, struct RastPort *rp, UBYTE *dst,
                   UWORD y0, UWORD rows)
{
    UWORD end;
    UWORD y;

    if (!http_rtg_attached_to(bm) || rp == NULL || dst == NULL)
        return FALSE;

    /* Clamped rather than refused: the caller's band is a tile grid rounded up
       and the last row of tiles hangs off the bottom of a screen whose height
       is not a multiple of the tile. */
    if (y0 >= rtg_h)
        return FALSE;

    end = (UWORD)(y0 + rows);
    if (end > rtg_h || end < y0)
        end = rtg_h;

    /* The RastPort here is the one the caller just locked. */
    rtg_rp = rp;

    for (y = y0; y < end; )
    {
        UWORD n = (UWORD)(end - y);

        if (n > RTG_STRIP_ROWS)
            n = RTG_STRIP_ROWS;

        if (!rtg_read_via(rtg_route, bm, dst + (ULONG)y * rtg_stride, y, n))
            return FALSE;

        y = (UWORD)(y + n);
    }

    return TRUE;
}

/* ------------------------------------------------------------ the report -- */

/*
 * The card's own depth and format go out in the readback word: everything is
 * downsampled to R5G6B5 before it is sent, so the geometry word reads depth 16
 * for all four truecolour depths and a client cannot tell which path ran.
 */
static const char *const rtg_rgbfb_name[RGBFB_N] =
{
    "NONE",     "CLUT",     "R8G8B8",   "B8G8R8",
    "R5G6B5PC", "R5G5B5PC", "A8R8G8B8", "A8B8G8R8",
    "R8G8B8A8", "B8G8R8A8", "R5G6B5",   "R5G5B5",
    "B5G6R5PC", "B5G5R5PC", "Y4U2V2",   "Y4U1V1"
};

static const char *const rtg_pixfmt_name[PIXFMT_N] =
{
    "LUT8",     "RGB15",    "BGR15",    "RGB15PC",
    "BGR15PC",  "RGB16",    "BGR16",    "RGB16PC",
    "BGR16PC",  "RGB24",    "BGR24",    "ARGB32",
    "BGRA32",   "RGBA32"
};

static ULONG rtg_put(char *out, ULONG cap, ULONG at, const char *s)
{
    ULONG i;

    for (i = 0; s[i] != '\0'; i++)
    {
        if (at + 1UL >= cap)
            return at;
        out[at++] = s[i];
    }
    return at;
}

static ULONG rtg_put_num(char *out, ULONG cap, ULONG at, ULONG v)
{
    char  tmp[12];
    ULONG n = 0;

    do { tmp[n++] = (char)('0' + (v % 10UL)); v /= 10UL; } while (v != 0UL);

    while (n > 0)
    {
        if (at + 1UL >= cap)
            return at;
        out[at++] = tmp[--n];
    }
    return at;
}

ULONG http_rtg_word(char *out, ULONG cap)
{
    ULONG at = 0;
    int   r;

    if (out == NULL || cap == 0UL)
        return 0;

    at = rtg_put(out, cap, at, "rtg lib=");
    if (RtgP96Base != NULL)
        at = rtg_put(out, cap, at, "p96");
    if (RtgP96Base != NULL && RtgCgxBase != NULL)
        at = rtg_put(out, cap, at, "+");
    if (RtgCgxBase != NULL)
        at = rtg_put(out, cap, at, "cgx");
    if (RtgP96Base == NULL && RtgCgxBase == NULL)
        at = rtg_put(out, cap, at, "none");

    at = rtg_put(out, cap, at, " board=");
    at = rtg_put(out, cap, at,
                 rtg_on_board_known ? (rtg_on_board ? "1" : "0") : "?");

    /* The screen's own depth and format, whichever library claims it. Neither
       survives the downsample, so this is the only place either appears. */
    at = rtg_put(out, cap, at, " depth=");
    at = rtg_put_num(out, cap, at, (ULONG)rtg_depth);

    at = rtg_put(out, cap, at, " fmt=");
    if (rtg_native_p96 >= 0 && (ULONG)rtg_native_p96 < RGBFB_N)
        at = rtg_put(out, cap, at, rtg_rgbfb_name[rtg_native_p96]);
    else if (rtg_native_cgx >= 0 && (ULONG)rtg_native_cgx < PIXFMT_N)
        at = rtg_put(out, cap, at, rtg_pixfmt_name[rtg_native_cgx]);
    else
        at = rtg_put(out, cap, at, "none");

    at = rtg_put(out, cap, at, " best=");
    at = rtg_put(out, cap, at,
                 (rtg_route >= 0) ? rtg_route_name[rtg_route] : "none");

    /* KB/s per route, and only the ones that answered. See httprtg.h. */
    for (r = 0; r < RTG_N_ROUTE; r++)
    {
        if (rtg_kbs[r] == 0UL)
            continue;
        at = rtg_put(out, cap, at, " ");
        at = rtg_put(out, cap, at, rtg_route_name[r]);
        at = rtg_put(out, cap, at, "=");
        at = rtg_put_num(out, cap, at, rtg_kbs[r]);
    }

    /* And the ones that answered with something other than the screen. Named
       rather than dropped: the rate above says why. */
    for (r = 0; r < RTG_N_ROUTE; r++)
    {
        if ((rtg_stale & (1UL << (ULONG)r)) == 0UL)
            continue;
        at = rtg_put(out, cap, at, " stale=");
        at = rtg_put(out, cap, at, rtg_route_name[r]);
    }

    if (at >= cap)
        return 0;
    out[at] = '\0';
    return at;
}

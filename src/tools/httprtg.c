/*
 * Reading a graphics card's framebuffer back.  See httprtg.h for what this is
 * and what it deliberately is not.
 *
 * The library calls are hand-declared.  Picasso96 and CyberGraphX are
 * third-party libraries and their headers are not in the NDK the toolchain
 * ships, so a build machine either has them or does not and the answer differs
 * between the Mac tree and the pinned Linux one.  What is needed is nine
 * functions and a dozen constants, so they are declared here against
 * inline/macros.h, the NDK's own LPn call macros, present in every toolchain
 * this builds with.  The library offsets and register assignments are taken
 * from Picasso96API_lib.fd and cybergraphics.fd, and each offset is beside its
 * call.
 *
 * Neither library is required to be present.  Each is opened if it is there,
 * and a machine with neither cannot have an RTG screen in front of it in the
 * first place.
 *
 * The read is whole rows rather than tiles.  The encoder's tile pass compares
 * against its shadow and reads nothing where nothing changed, which on the
 * chipset is right.  It cannot be done here, because the compare would be the
 * readback.  So the whole frame is fetched into a staging buffer in Fast RAM
 * and the encoder is pointed at that buffer, which puts every one of its
 * comparisons on ordinary memory and leaves exactly one readback a frame.
 *
 * That readback is contiguous full rows.  x11vnc measures adjacent rectangles
 * read together as up to twice the rate of the same bytes read separately, and
 * a card can be down at single-figure MB/s on reads, so the shape of the fetch
 * is most of its cost.  A loop of tile-sized rectangles is the wrong shape by
 * an order of magnitude and is not offered here.
 *
 * Two output formats leave here and no more.  A palette screen is a byte a
 * pixel, as it always was.  A 15, 16, 24 or 32-bit screen is two bytes a
 * pixel, big-endian R5G6B5, whatever the card holds it in, so the encoder, the
 * wire and the browser have one truecolour path rather than four.  The
 * converters and the two tables that drive them are under "pixel formats"
 * below, and every route delivers through them.
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

/* struct RenderInfo, as libraries/Picasso96.h has it.  BytesPerRow is a WORD,
   which is what bounds the staging stride below. */
struct RtgRenderInfo
{
    APTR  Memory;
    WORD  BytesPerRow;
    WORD  pad;
    ULONG RGBFormat;
};

/* p96GetBitMapAttr attributes, in the order libraries/Picasso96.h enumerates
   them.  BYTESPERROW, MEMORY and ISONBOARD are the three that are only valid
   while the bitmap is locked, which is why ISONBOARD is asked inside one. */
#define P96BMA_WIDTH         0UL
#define P96BMA_HEIGHT        1UL
#define P96BMA_DEPTH         2UL
#define P96BMA_MEMORY        3UL
#define P96BMA_BYTESPERROW   4UL
#define P96BMA_RGBFORMAT     7UL
#define P96BMA_ISP96         8UL
#define P96BMA_ISONBOARD     9UL

/* RGBFTYPE, in the order libraries/Picasso96.h enumerates it.  A "PC" name is
   the same channel order with the two bytes of a 16-bit pixel the other way
   round, which is what a card wired for a little-endian host hands back. */
#define RGBFB_NONE           0UL    /* planar, under P96's historical name   */
#define RGBFB_CLUT           1UL    /* palette indexed, one byte a pixel     */
#define RGBFB_R8G8B8         2UL
#define RGBFB_B8G8R8         3UL
#define RGBFB_R5G6B5PC       4UL
#define RGBFB_R5G5B5PC       5UL
#define RGBFB_A8R8G8B8       6UL
#define RGBFB_A8B8G8R8       7UL
#define RGBFB_B8G8R8A8       8UL
#define RGBFB_R8G8B8A8       9UL
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

/* CyberGraphX numbers its pixel formats separately from Picasso96 and puts
   them in a different order, so the two are kept apart and each is put on the
   same footing by its own table below. */
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

/* graphics.library AllocBitMap's extended flags, which is how a CyberGraphX
   bitmap of a named pixel format is asked for. */
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
 * A palette screen is delivered a byte a pixel, as it always was.  A 15, 16,
 * 24 or 32-bit screen is delivered two bytes a pixel, big-endian R5G6B5, which
 * is Picasso96's own RGBFB_R5G6B5 on a 68k.  All four truecolour depths
 * converge on that one output, so the encoder, the wire and the browser have a
 * single truecolour path rather than four.
 *
 * p96ReadPixelArray converts for itself and asks nothing of this.  The two
 * lock routes hand back the card's pixels in the screen's own format, and the
 * blit route reads a copy in that same format, so those three come through
 * here.  A format neither table names is not converted and not guessed at: the
 * route fails, the probe passes over it, and http_rtg_describe() refuses the
 * screen rather than serving colours that are not on it.
 */

#define RTG_L_R565  0U          /* rrrrrggg gggbbbbb */
#define RTG_L_R555  1U          /* xrrrrrgg gggbbbbb */
#define RTG_L_B565  2U          /* bbbbbggg gggrrrrr */
#define RTG_L_B555  3U          /* xbbbbbgg gggrrrrr */

/* One source pixel, once both libraries' numbering has been put on the same
   footing.  `step` is the source bytes a pixel and 0 means a format this does
   not read.  `layout` and `swap` describe a 16-bit pixel; r, g and b are the
   byte offsets of the channels of a 24 or 32-bit one. */
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
    { 4, 0,          0, 2, 1, 0 },      /* RGBFB_B8G8R8A8                   */
    { 4, 0,          0, 0, 1, 2 },      /* RGBFB_R8G8B8A8                   */
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

/* A 16-bit pixel, already assembled the right way round, repacked as R5G6B5.
   Five bits of green become six by repeating the top bit at the bottom, so
   0x1F stays full scale rather than landing one step short of it. */
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

/* Byte loads throughout.  A card's mapping is word aligned in every 16-bit
   mode there is, but a 24-bit one puts two pixels in three words and the odd
   pixel on an odd address, and a 68000 traps on that. */
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

/* One row of `pixels` from the card's format into the wire's. */
static VOID rtg_cvt_row(UBYTE *dst, const UBYTE *src, UWORD pixels,
                        const RtgSrcFmt *f)
{
    if (f->step != 2)
    {
        rtg_row8(dst, src, pixels, f);
        return;
    }

    /* A screen already in the wire format is the common case on a 68k, and
       there is nothing for the loop above to do to it. */
    if (f->layout == RTG_L_R565 && f->swap == 0)
        memcpy(dst, src, (size_t)pixels * 2u);
    else
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
 * Asked before BMF_STANDARD.  The planar path's first question is whether the
 * bitmap carries BMF_STANDARD, and on a chipset machine that is the right
 * question.  A card's bitmap can carry it too, since that flag is what makes
 * the OS treat the bitmap normally, and its Planes[] are not eight bitplanes.
 * So the owner is asked first.  Both libraries answer for any bitmap,
 * including one that belongs to neither of them, which is what makes the
 * question safe to put first.
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

BOOL http_rtg_describe(struct BitMap *bm, HttpRtgScreen *s, const char **why)
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
     * The pixel format decides this, not the depth.  A palette screen goes out
     * a byte a pixel and every truecolour format the converter names goes out
     * as R5G6B5, so what is left to refuse is a bitmap that has no chunky
     * pixels at all, a YUV overlay, and a format nobody here has seen.  The
     * refusal names what it found, so the reason does not have to be looked up
     * here.
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

    s->width  = (UWORD)w;
    s->height = (UWORD)h;
    s->depth  = (UWORD)depth;
    s->bpp    = (UBYTE)((fmt.step == 1) ? 1 : 2);
    return TRUE;
}

/* -------------------------------------------------------- the five reads -- */

/* One strip through one route: rows [y0, y0+rows) of the full width, into
   `dst` at the attach stride.  Every route delivers exactly `rtg_row_bytes` a
   row, in the one format the session settled on, and leaves the padding past
   it alone, so which route a session picked cannot show up as a difference in
   the bytes. */

static BOOL rtg_read_p96_rpa(UBYTE *dst, UWORD y0, UWORD rows)
{
    struct RtgRenderInfo ri;

    if (RtgP96Base == NULL)
        return FALSE;

    ri.Memory      = (APTR)dst;
    ri.BytesPerRow = (WORD)rtg_stride;
    ri.pad         = 0;
    /* The destination format, not the screen's.  p96ReadPixelArray converts
       between the two, whatever the screen turns out to be, which is what
       makes this the one route with no conversion of its own. */
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

    /* The mapping is in the screen's format.  A format the converter does not
       name, or one that disagrees with what the session was attached for,
       fails the route rather than producing a picture nobody can check. */
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
 * THE COUNT IS THE ONLY THING THAT SAYS IT READ ANYTHING.
 *
 * ReadPixelArray() returns the number of pixels it moved, and this discarded
 * it and reported success.  On Picasso96's cybergraphics emulation the call
 * declines RECTFMT_LUT8 and returns 0 at once, so the route came back
 * instantly having copied nothing -- 436540 KB/s against 5890 for the P96
 * route on the same board, seventy times every other route, which is not a
 * fast read but no read at all.  It therefore WON the probe above, and the
 * console then streamed the buffer's first contents for the life of the
 * session: a frozen Workbench that answers the keyboard, changes nothing, and
 * passes every assertion the harness makes on an idle screen.
 */
static BOOL rtg_read_cgx_rpa(UBYTE *dst, UWORD y0, UWORD rows)
{
    ULONG got;

    if (RtgCgxBase == NULL)
        return FALSE;

    /* Not offered for a truecolour screen at all, so it is neither probed nor
       chosen there.  cybergraphics has no big-endian R5G6B5 rectangle format
       to ask for, and the one rectangle format this route does ask for is
       already documented above as something Picasso96's cybergraphics
       emulation reports having moved without moving it. */
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

    /* Asked of the bitmap rather than carried in from attach, because the
       blit route brings its own offscreen through here. */
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
 * BltBitMap to an offscreen bitmap and read the copy.
 *
 * This does not reduce how many bytes cross the bus.  What it gains is that
 * the slow read comes off a buffer nothing is drawing into.  The console reads
 * the screen with the layer lock only attempted, never waited for, so a frame
 * read straight out of VRAM can carry half of somebody else's redraw.  The
 * blit is the card's own, and one call, so what it snapshots is one moment.
 *
 * ScreenRecorder does the same thing for the same reason.
 */
static BOOL rtg_read_blit(struct BitMap *bm, UBYTE *dst, UWORD y0, UWORD rows)
{
    if (rtg_off == NULL)
        return FALSE;

    BltBitMap(bm, 0, (WORD)y0, rtg_off, 0, 0, (WORD)rtg_w, (WORD)rows,
              0xC0, 0xFF, NULL);
    WaitBlit();

    /* The copy is a plain memory bitmap in the screen's own format, so
       reading it is a lock and the same conversion a direct lock does,
       through whichever library allocated it. */
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
     * BMF_USERPRIVATE is Picasso96's "never displayed, never moved", which is
     * what a snapshot buffer is, and the one kind of P96 bitmap the
     * documentation says does not have to be locked before it is touched.  It
     * is still locked below, because the matched lock costs two library calls
     * a frame and keeps this caller on the ordinary path.
     *
     * It is allocated in the screen's own pixel format.  BltBitMap between two
     * bitmaps of different formats moves bits, not colours, and is not a
     * conversion, so a 16-bit screen blitted into an 8-bit offscreen would
     * come back as half a picture with no way to tell.  The conversion belongs
     * to the read of the copy, which goes through the same converter every
     * other lock route does.
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
 * A ROUTE THAT ANSWERS IS NOT A ROUTE THAT READ THE SCREEN.
 *
 * The band a route just read, against the same band read through a route that
 * can only be a memcpy out of mapped board memory.  ReadPixelArray() reports
 * how many pixels it moved and Picasso96's cybergraphics emulation reports the
 * whole band for RECTFMT_LUT8 while leaving the buffer as it found it: the
 * route came back in no time at all, won the probe below at 429926 KB/s
 * against 5890 for the P96 route on the same board, and the console then
 * served one frame for the life of the session.  A Workbench that takes the
 * keyboard, runs what is typed at it and never changes on screen is what that
 * looks like from a browser, and every assertion an idle-screen harness makes
 * passes on it.  Neither the return count nor the clock catches it.  Reading
 * the same pixels twice, two ways, does.
 *
 * ref holds the reference band, packed; dst is the caller's staging buffer and
 * is strided.  The screen is live, so a disagreement is only evidence when the
 * reference agrees with ITSELF across the same interval: if the second
 * reference read differs from the first, something drew while this was
 * looking, and the route keeps the benefit of the doubt.
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
     * POISONED FIRST, or the check checks nothing.  The reference was just
     * read into this same buffer, so a route that writes nothing leaves the
     * reference's own pixels behind and compares equal to them -- which is
     * how the first version of this passed the very route it was written to
     * catch.  A byte no read can leave behind is what makes a silent route
     * visible.
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
 * Every route the machine offers reads the same band, three times, and the
 * fastest pass is kept.  A slow pass can be the scheduler and a fast one can
 * only be the hardware.  The rate is recorded for every route rather than for
 * the winner alone, because the spread is the unknown: which route a
 * particular board and driver make cheap is what nobody has published, and one
 * report of five numbers answers it for that board.
 *
 * Every route is then read against a reference route before it may be chosen,
 * because the fastest is the one most likely not to have read anything.
 *
 * The EClock is ~709 kHz, so a band that takes eight milliseconds is measured
 * to about a part in five thousand.  DateStamp() ticks are fiftieths and would
 * have made every one of these numbers a guess.
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
        /* No clock to measure with.  Every route still works, and the order
           below is the one to prefer when nothing can be timed.  A mapping and
           a memcpy beats a driver call on every board considered here, and the
           snapshot beats both when there is one. */
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

            /* KB a second, without overflowing on the way: the band is at
               most two megabytes, so bytes/1024 is at most 2048, and 2048
               times a 709 kHz rate is well inside 32 bits. */
            {
                ULONG kbs = (bytes / 1024UL) * rate / took;
                if (kbs > best)
                    best = kbs;
            }
        }

        rtg_kbs[r] = best;
    }

    /*
     * THE REFERENCE, and it is a mapping and a memcpy rather than a driver
     * call.  A lock route hands back the address of the pixels the card is
     * displaying; there is nowhere for it to be wrong that would not also make
     * the screen wrong.  The blit route is last because it goes through an
     * offscreen bitmap, which is one more place for a copy to be stale.
     *
     * The cybergraphics ReadPixelArray route was never a candidate here, so a
     * machine with only that library still finds its reference in the CGX lock
     * route on a truecolour screen, where the ReadPixelArray one is not
     * offered at all.
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

    /* No reference, or no room to hold one: choose on the clock alone, which
       is what this did before there was a check at all. */
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
     * The screen's own pixel format, resolved once.  It settles how many bytes
     * a pixel the staging buffer holds, which of the two output paths every
     * route takes, and what format the snapshot offscreen is allocated in.
     * Both attributes are answered without the bitmap locked, and both
     * libraries are asked, because a Picasso96 screen answers CyberGraphX's
     * questions too and either answer can be the one the offscreen needs.
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
       means the screen changed under the caller.  Nothing is read rather than
       something read in the wrong format. */
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
    rtg_w          = width;
    rtg_h          = height;
    rtg_stride     = stride;
    rtg_bpp        = (UBYTE)bpp;
    rtg_row_bytes  = (ULONG)width * bpp;
    rtg_depth      = (UWORD)depth;
    rtg_native_p96 = p96f;
    rtg_native_cgx = cgxf;

    /*
     * Whether the bitmap is really in card memory is the first branch.
     * Picasso96 keeps a bitmap in system RAM whenever it decides to, and when
     * it has, the read is ordinary Fast RAM at Fast RAM speed and none of the
     * rest of this matters.  It also decides whether the snapshot buffer is
     * worth its memory, since blitting Fast RAM to Fast RAM to read it back is
     * a copy for nothing.
     *
     * P96 only answers while the bitmap is locked, which is why it is asked
     * here and not in http_rtg_describe().  That one runs under LockIBase()
     * and this one does not.
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
        /* CyberGraphX answers no on-the-board attribute.  ISLINEARMEM is the
           nearest one it has, and a bitmap that is linearly addressable is the
           one whose lock route can be a memcpy. */
        rtg_on_board = (UBYTE)(rtg_cgx_attr(bm, CYBRMATTR_ISLINEARMEM) != 0UL);
        rtg_on_board_known = 1;
    }

    /* One strip's worth rather than a whole screen, because the snapshot is
       per fetch. */
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

/* ------------------------------------------------------------ the fetch --- */

BOOL http_rtg_read(struct BitMap *bm, struct RastPort *rp, UBYTE *dst)
{
    UWORD y;

    if (rtg_route < 0 || bm == NULL || rp == NULL || dst == NULL)
        return FALSE;

    /* The screen can have been re-resolved since attach.  The geometry is the
       caller's to check, and the RastPort here is whatever it just locked. */
    rtg_rp = rp;

    for (y = 0; y < rtg_h; )
    {
        UWORD rows = (UWORD)(rtg_h - y);

        if (rows > RTG_STRIP_ROWS)
            rows = RTG_STRIP_ROWS;

        if (!rtg_read_via(rtg_route, bm, dst + (ULONG)y * rtg_stride, y, rows))
            return FALSE;

        y = (UWORD)(y + rows);
    }

    return TRUE;
}

/* ------------------------------------------------------------ the report -- */

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

    at = rtg_put(out, cap, at, " best=");
    at = rtg_put(out, cap, at,
                 (rtg_route >= 0) ? rtg_route_name[rtg_route] : "none");

    /* KB/s per route, and only the ones that answered.  This is the figure the
       probe exists to produce.  See httprtg.h. */
    for (r = 0; r < RTG_N_ROUTE; r++)
    {
        if (rtg_kbs[r] == 0UL)
            continue;
        at = rtg_put(out, cap, at, " ");
        at = rtg_put(out, cap, at, rtg_route_name[r]);
        at = rtg_put(out, cap, at, "=");
        at = rtg_put_num(out, cap, at, rtg_kbs[r]);
    }

    /* And the ones that answered with something other than the screen.  Named
       rather than dropped: a board whose fastest route reads nothing is worth
       knowing about from one line of a log, and the rate above says why. */
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

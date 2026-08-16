/*
 * Reading a graphics card's framebuffer back.  See httprtg.h for what this is
 * and what it deliberately is not.
 *
 * THE LIBRARY CALLS ARE HAND-DECLARED, AND THAT IS ON PURPOSE
 *
 *   Picasso96 and CyberGraphX are third-party libraries and their headers are
 *   not in the NDK the toolchain ships, so a build machine either has them or
 *   does not and the answer differs between the Mac tree and the pinned Linux
 *   one.  What is needed is nine functions and a dozen constants, so they are
 *   declared here against inline/macros.h -- the NDK's own LPn call macros,
 *   present in every toolchain this builds with -- with the library offsets
 *   and register assignments taken from Picasso96API_lib.fd and
 *   cybergraphics.fd.  Nothing here is guessed; the offsets are beside each
 *   call.
 *
 *   Neither library is required to be present.  Each is opened if it is there,
 *   and a machine with neither cannot have an RTG screen in front of it in the
 *   first place.
 *
 * WHY THE READ IS WHOLE ROWS AND NOT TILES
 *
 *   The encoder's tile pass wants to compare against its shadow and read
 *   nothing where nothing changed, and on the chipset that is exactly right.
 *   It cannot be done here: the compare would be the readback.  So the whole
 *   frame is fetched into a staging buffer in Fast RAM and the encoder is
 *   pointed at THAT, which puts every one of its comparisons on ordinary
 *   memory and leaves exactly one readback a frame.
 *
 *   And that readback is contiguous full rows.  x11vnc measures adjacent
 *   rectangles read together as up to twice the rate of the same bytes read
 *   separately, and a card can be down at single-figure MB/s on reads, so the
 *   shape of the fetch is most of its cost.  A loop of tile-sized rectangles
 *   is the wrong shape by an order of magnitude and is not offered here.
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

#define RGBFB_NONE           0UL    /* planar; the name is P96's, historical */
#define RGBFB_CLUT           1UL    /* palette indexed, one byte a pixel     */

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

#define PIXFMT_LUT8           0UL
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
static UWORD           rtg_w;
static UWORD           rtg_h;
static ULONG           rtg_stride;      /* the caller's staging row stride    */
static UBYTE           rtg_on_board;    /* P96BMA_ISONBOARD / ISLINEARMEM     */
static UBYTE           rtg_on_board_known;
static int             rtg_route = -1;
static ULONG           rtg_kbs[RTG_N_ROUTE];   /* 0 = the route was not there */

/* The offscreen the snapshot route blits into.  Allocated only when the
   screen's bitmap is really in card memory: when P96 kept it in system RAM
   there is nothing to snapshot away from and it would be a megabyte to copy
   Fast RAM to Fast RAM. */
static struct BitMap  *rtg_off;
static UBYTE           rtg_off_is_p96;

/* How tall one fetch is.  The read is whole rows either way; this bounds how
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
 * ASKED BEFORE BMF_STANDARD, WHICH IS THE WHOLE POINT OF IT
 *
 * The planar path's first question is whether the bitmap carries BMF_STANDARD,
 * and on a chipset machine that is the right question.  A card's bitmap may
 * carry it too -- it is what makes the OS treat the thing normally -- and its
 * Planes[] are not eight bitplanes.  So the owner is asked first: both of
 * these answer for any bitmap, including one that belongs to neither of them,
 * which is what makes the question safe to put first.
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
    ULONG w = 0, h = 0, depth = 0;
    int   clut8 = 0;
    int   answered = 0;

    if (why != NULL)
        *why = NULL;
    if (bm == NULL || s == NULL)
        return FALSE;

    if (RtgP96Base != NULL && rtg_p96_attr(bm, P96BMA_ISP96) != 0UL)
    {
        w     = rtg_p96_attr(bm, P96BMA_WIDTH);
        h     = rtg_p96_attr(bm, P96BMA_HEIGHT);
        depth = rtg_p96_attr(bm, P96BMA_DEPTH);
        clut8 = (rtg_p96_attr(bm, P96BMA_RGBFORMAT) == RGBFB_CLUT);
        answered = 1;
    }
    else if (RtgCgxBase != NULL && rtg_cgx_attr(bm, CYBRMATTR_ISCYBERGFX) != 0UL)
    {
        w     = rtg_cgx_attr(bm, CYBRMATTR_WIDTH);
        h     = rtg_cgx_attr(bm, CYBRMATTR_HEIGHT);
        depth = rtg_cgx_attr(bm, CYBRMATTR_DEPTH);
        clut8 = (rtg_cgx_attr(bm, CYBRMATTR_PIXFMT) == PIXFMT_LUT8);
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
     * THE REFUSAL THAT MATTERS.  A 15, 16, 24 or 32-bit screen has no palette
     * and the encoder has no truecolour code, so what would come out of the
     * chunky path is the low byte of every pixel indexed into a colour map
     * that is not there -- a picture, and a wrong one.  Named by depth,
     * because "not supported" sends somebody to read this file.
     */
    if (!clut8 || depth != 8UL)
    {
        if (why != NULL)
        {
            switch (depth)
            {
            case 15UL: *why = "the front screen is a 15-bit RTG screen; the "
                              "console serves 8-bit palette screens and planar "
                              "ones"; break;
            case 16UL: *why = "the front screen is a 16-bit RTG screen; the "
                              "console serves 8-bit palette screens and planar "
                              "ones"; break;
            case 24UL: *why = "the front screen is a 24-bit RTG screen; the "
                              "console serves 8-bit palette screens and planar "
                              "ones"; break;
            case 32UL: *why = "the front screen is a 32-bit RTG screen; the "
                              "console serves 8-bit palette screens and planar "
                              "ones"; break;
            default:   *why = "the front screen is an RTG screen this cannot "
                              "read: it is not 8-bit palette indexed"; break;
            }
        }
        return FALSE;
    }

    s->width  = (UWORD)w;
    s->height = (UWORD)h;
    s->depth  = (UWORD)depth;
    s->clut8  = 1;
    return TRUE;
}

/* -------------------------------------------------------- the five reads -- */

/* One strip through one route: rows [y0, y0+rows) of the full width, into
   `dst` at the attach stride.  Every route delivers exactly `rtg_w` bytes a
   row and leaves the padding past it alone, so which route a session picked
   cannot show up as a difference in the bytes. */

static BOOL rtg_read_p96_rpa(UBYTE *dst, UWORD y0, UWORD rows)
{
    struct RtgRenderInfo ri;

    if (RtgP96Base == NULL)
        return FALSE;

    ri.Memory      = (APTR)dst;
    ri.BytesPerRow = (WORD)rtg_stride;
    ri.pad         = 0;
    ri.RGBFormat   = RGBFB_CLUT;

    rtg_p96_read(&ri, 0, 0, rtg_rp, 0, y0, rtg_w, rows);
    return TRUE;
}

static BOOL rtg_read_p96_lock(struct BitMap *bm, UBYTE *dst,
                              UWORD y0, UWORD rows)
{
    struct RtgRenderInfo ri;
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

    src = (const UBYTE *)ri.Memory + (ULONG)y0 * (ULONG)(UWORD)ri.BytesPerRow;
    for (r = 0; r < rows; r++)
    {
        memcpy(dst, src, (size_t)rtg_w);
        dst += rtg_stride;
        src += (UWORD)ri.BytesPerRow;
    }

    rtg_p96_unlock(bm, lock);
    return TRUE;
}

static BOOL rtg_read_cgx_rpa(UBYTE *dst, UWORD y0, UWORD rows)
{
    if (RtgCgxBase == NULL)
        return FALSE;

    (VOID)rtg_cgx_read((APTR)dst, 0, 0, (UWORD)rtg_stride, rtg_rp,
                       0, y0, rtg_w, rows, (UBYTE)RECTFMT_LUT8);
    return TRUE;
}

static BOOL rtg_read_cgx_lock(struct BitMap *bm, UBYTE *dst,
                              UWORD y0, UWORD rows)
{
    struct TagItem tags[3];
    ULONG          base = 0;
    ULONG          bpr = 0;
    APTR           handle;
    const UBYTE   *src;
    UWORD          r;

    if (RtgCgxBase == NULL)
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
        memcpy(dst, src, (size_t)rtg_w);
        dst += rtg_stride;
        src += bpr;
    }

    rtg_cgx_unlock(handle);
    return TRUE;
}

/*
 * BltBitMap to an offscreen bitmap and read the copy.
 *
 * It does NOT reduce how many bytes cross the bus.  What it buys is that the
 * slow read comes off a buffer nothing is drawing into: the console reads the
 * screen with the layer lock only ATTEMPTED, never waited for, so a frame read
 * straight out of VRAM can carry half of somebody else's redraw.  The blit is
 * the card's own, and one call, so what it snapshots is one moment.
 *
 * ScreenRecorder does exactly this and for the same reason.
 */
static BOOL rtg_read_blit(struct BitMap *bm, UBYTE *dst, UWORD y0, UWORD rows)
{
    if (rtg_off == NULL)
        return FALSE;

    BltBitMap(bm, 0, (WORD)y0, rtg_off, 0, 0, (WORD)rtg_w, (WORD)rows,
              0xC0, 0xFF, NULL);
    WaitBlit();

    /* The copy is a plain memory bitmap, so reading it is a lock and a
       memcpy through whichever library allocated it. */
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
     * BMF_USERPRIVATE is Picasso96's "never displayed, never moved": exactly
     * a snapshot buffer, and the one kind of P96 bitmap the documentation says
     * does not have to be locked before it is touched.  It is still locked
     * below, because a matched lock costs two library calls a frame and being
     * the odd caller that does not is not worth saving them.
     */
    if (RtgP96Base != NULL)
    {
        rtg_off = rtg_p96_alloc((ULONG)w, (ULONG)rows, 8UL,
                                P96_BMF_USERPRIVATE | P96_BMF_CLEAR,
                                RGBFB_CLUT);
        if (rtg_off != NULL)
        {
            rtg_off_is_p96 = 1;
            return TRUE;
        }
    }

    if (RtgCgxBase != NULL)
    {
        rtg_off = AllocBitMap((ULONG)w, (ULONG)rows, 8UL,
                              BMF_MINPLANES | CGX_BMF_SPECIALFMT |
                              CGX_SHIFT_PIXFMT(PIXFMT_LUT8),
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
 * THE NUMBER NOBODY HAS PUBLISHED.
 *
 * Every route the machine offers reads the same band, three times, and the
 * FASTEST pass is kept: a slow pass can be the scheduler and a fast one cannot
 * be anything but the hardware.  The rate is recorded for every route and not
 * just the winner, because the interesting fact is the SPREAD -- which route
 * a particular board and driver make cheap is what nobody knows, and one
 * user's report of five numbers answers it for that board.
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
    ULONG            bytes = (ULONG)rtg_w * rows;
    int              r;
    ULONG            pass;

    rtg_route = -1;
    for (r = 0; r < RTG_N_ROUTE; r++)
        rtg_kbs[r] = 0;

    (VOID)ami_millis();                 /* opens timer.device, sets TimerBase */
    if (TimerBase == NULL)
    {
        /* No clock to measure with.  Every route still works; the order below
           is the one to prefer when nothing can be timed -- a mapping and a
           memcpy beats a driver call on every board this has been reasoned
           about, and the snapshot beats both when there is one. */
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
               most a megabyte, so bytes/1024 is at most 1024, and 1024 times
               a 709 kHz rate is well inside 32 bits. */
            {
                ULONG kbs = (bytes / 1024UL) * rate / took;
                if (kbs > best)
                    best = kbs;
            }
        }

        rtg_kbs[r] = best;
        if (best != 0UL && (rtg_route < 0 || best > rtg_kbs[rtg_route]))
            rtg_route = r;
    }
}

/* ---------------------------------------------------------------- attach -- */

BOOL http_rtg_attach(struct BitMap *bm, struct RastPort *rp,
                     UWORD width, UWORD height, ULONG stride, UBYTE *probe)
{
    http_rtg_detach();

    if (bm == NULL || rp == NULL || probe == NULL)
        return FALSE;
    if (width == 0 || height == 0 || stride < (ULONG)width)
        return FALSE;
    /* struct RenderInfo carries the stride in a WORD, and the P96 read route
       is the one that fills one in. */
    if (stride > 32767UL)
        return FALSE;

    rtg_rp     = rp;
    rtg_w      = width;
    rtg_h      = height;
    rtg_stride = stride;

    /*
     * IS IT ACTUALLY IN CARD MEMORY.  This is the first branch and not an
     * afterthought: Picasso96 keeps a bitmap in system RAM whenever it decides
     * to, and when it has, the read is ordinary Fast RAM at Fast RAM speed and
     * none of the rest of this matters.  It is also what decides whether the
     * snapshot buffer is worth its memory -- blitting Fast RAM to Fast RAM to
     * read it back is a copy for nothing.
     *
     * P96 will only answer while the bitmap is locked, which is why it is
     * asked here and not in http_rtg_describe(): that one runs under
     * LockIBase() and this one does not.
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
        /* CyberGraphX has no "is it on the board"; ISLINEARMEM is the nearest
           thing it answers, and a bitmap that is linearly addressable is the
           one whose lock route can be a memcpy. */
        rtg_on_board = (UBYTE)(rtg_cgx_attr(bm, CYBRMATTR_ISLINEARMEM) != 0UL);
        rtg_on_board_known = 1;
    }

    /* One strip's worth, not a whole screen: the snapshot is per fetch. */
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

    /* The screen may have been re-resolved since attach; the geometry is the
       caller's to check and these two are simply whatever it just locked. */
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

    /* KB/s per route, and only the ones that answered.  This is the figure
       the whole probe exists to produce; see httprtg.h. */
    for (r = 0; r < RTG_N_ROUTE; r++)
    {
        if (rtg_kbs[r] == 0UL)
            continue;
        at = rtg_put(out, cap, at, " ");
        at = rtg_put(out, cap, at, rtg_route_name[r]);
        at = rtg_put(out, cap, at, "=");
        at = rtg_put_num(out, cap, at, rtg_kbs[r]);
    }

    if (at >= cap)
        return 0;
    out[at] = '\0';
    return at;
}

/*
 * wbgrab, the Workbench screen's bitplanes to a file.
 *
 *     wbgrab [FRAMES/N] [DELAY/N] [TO/K]
 *
 * FRAMES frames DELAY ticks apart into one .pfs file, big-endian throughout:
 *
 *      0  char  magic[4]     "PFS2"
 *      4  UWORD width
 *      6  UWORD height
 *      8  UBYTE depth
 *      9  UBYTE flags        0
 *     10  UWORD bytesPerRow  one plane, one row
 *     12  UWORD frameCount
 *     14  UWORD reserved     0
 *     16  UBYTE palette[3 << depth]     R,G,B
 *         frameCount x (depth * bytesPerRow * height), plane-major
 *         frameCount x 12-byte frame records: ULONG milliseconds from the
 *             first frame, WORD pointer x, WORD pointer y, UWORD pointer
 *             image, UWORD reserved
 *
 * The full layout, including the pointer image table this does not write, is
 * in src/tools/web/client/console/pfs.ts.  This is the capture half of a
 * remote framebuffer and does not read the sprite: it writes the position it
 * can see and image 0, which is "no pointer image in this file".
 *
 * bytesPerRow is the bitmap's, not width/8: a 640-wide screen has 80 and a
 * 644-wide one has 82, and the pixels past the width are real bytes in the
 * frame either way.
 *
 * THE TIMESTAMPS ARE MEASURED, NOT DERIVED FROM DELAY.  A grab of a 640x256x4
 * screen costs real milliseconds and a machine under load costs more of them,
 * so the interval between two frames is DELAY plus however long the grab took;
 * a reader handed DELAY would be told a schedule rather than what happened.
 * Each frame's time is read off DateStamp() as it is taken.
 *
 * The table goes at the END, after the last frame, which is what makes an
 * early exit -- a Ctrl-C, a screen that changed shape -- come out right: the
 * frames written are the frames counted and the table appended is exactly as
 * long.  Reserving it up front would mean compacting the file afterwards.
 *
 * The screen is locked per frame, not for the session: this is the grab half
 * of a remote-framebuffer server, which runs for hours between grabs, and a
 * held LockLayers stops every other task drawing.
 *
 * Only the Workbench screen, and only a standard planar BitMap.  An RTG
 * screen's BitMap has no Planes[] to read and GetBitMapAttr(BMA_FLAGS) is
 * what says so, so that case exits rather than reading whatever the pointers
 * happen to hold.
 *
 * Kickstart 3.0 (V39) or later.  GetBitMapAttr() is the check that keeps the
 * plane reads honest and it does not exist before V39, so there is no version
 * of this that runs on 2.x and is still safe; GetRGB32() comes from the same
 * release, which is why there is no GetRGB4() path either.
 *
 * SPDX-License-Identifier: MIT
 */

#include "tools.h"

#include <exec/memory.h>
#include <graphics/gfx.h>
#include <graphics/gfxbase.h>
#include <graphics/view.h>
#include <graphics/layers.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>

#include <proto/graphics.h>
#include <proto/intuition.h>
#include <proto/layers.h>

const char *const tool_name = "wbgrab";

static const char version_tag[] __attribute__((used)) =
    TOOL_VERSTAG("wbgrab");

#define TEMPLATE    "FRAMES/N,DELAY/N,TO/K"

enum
{
    ARG_FRAMES = 0,
    ARG_DELAY,
    ARG_TO,
    ARG_COUNT
};

#define PFS_HEADER_SIZE     16
#define PFS_FRAMEREC        12
#define PFS_MAX_DEPTH       8
#define PFS_MAX_PALETTE     (3U * (1U << PFS_MAX_DEPTH))

#define DEFAULT_FRAMES      1
#define DEFAULT_DELAY       2
#define DEFAULT_FILE        "RAM:wbgrab.pfs"

#define PUBSCREEN_NAME      "Workbench"

struct GfxBase       *GfxBase;
struct IntuitionBase *IntuitionBase;
struct Library       *LayersBase;

/*
 * What the header says and what the copy loop needs.  row_stride and
 * row_bytes differ only on an interleaved bitmap, where BytesPerRow spans
 * every plane and one plane's row is that divided by the depth.
 */
typedef struct Geometry
{
    UWORD width;
    UWORD height;
    UWORD depth;
    UWORD row_bytes;
    ULONG row_stride;
    ULONG frame_bytes;
} Geometry;

static UBYTE header[PFS_HEADER_SIZE + PFS_MAX_PALETTE];

/* ------------------------------------------------------------- big-endian */

static VOID put_be16(UBYTE *p, ULONG v)
{
    p[0] = (UBYTE)((v >> 8) & 0xFF);
    p[1] = (UBYTE)(v & 0xFF);
}

static VOID put_be32(UBYTE *p, ULONG v)
{
    p[0] = (UBYTE)((v >> 24) & 0xFF);
    p[1] = (UBYTE)((v >> 16) & 0xFF);
    p[2] = (UBYTE)((v >> 8) & 0xFF);
    p[3] = (UBYTE)(v & 0xFF);
}

/*
 * Milliseconds since midnight, from the system clock.  DateStamp() counts
 * minutes and ticks-in-the-minute, and a tick is a fiftieth; the caller takes
 * differences, and a run that spans midnight has one frame's interval wrong on
 * a file whose other thousand are right.
 */
static ULONG wb_millis(VOID)
{
    struct DateStamp ds;

    (VOID)DateStamp(&ds);

    return (ULONG)ds.ds_Minute * 60000UL + (ULONG)ds.ds_Tick * 20UL;
}

/* ---------------------------------------------------------------- library */

static BOOL open_libraries(VOID)
{
    GfxBase = (struct GfxBase *)
        OpenLibrary((CONST_STRPTR)"graphics.library", 39);
    IntuitionBase = (struct IntuitionBase *)
        OpenLibrary((CONST_STRPTR)"intuition.library", 39);
    LayersBase = OpenLibrary((CONST_STRPTR)"layers.library", 39);

    if (GfxBase != NULL && IntuitionBase != NULL && LayersBase != NULL)
        return TRUE;

    tool_error("this needs Kickstart 3.0 or later: graphics, intuition and "
               "layers must all answer OpenLibrary() at version 39");
    return FALSE;
}

static VOID close_libraries(VOID)
{
    if (LayersBase != NULL)      { CloseLibrary(LayersBase); LayersBase = NULL; }
    if (IntuitionBase != NULL)
    {
        CloseLibrary((struct Library *)IntuitionBase);
        IntuitionBase = NULL;
    }
    if (GfxBase != NULL)
    {
        CloseLibrary((struct Library *)GfxBase);
        GfxBase = NULL;
    }
}

/* --------------------------------------------------------------- geometry */

/*
 * FALSE having said why.  Every refusal here is a bitmap this cannot read
 * correctly, so none of them may fall through to a grab of something else.
 */
static BOOL geometry_of(struct BitMap *bm, Geometry *g)
{
    ULONG flags;
    ULONG depth;
    ULONG width;
    ULONG height;
    ULONG stride;
    UWORD plane;

    if (bm == NULL)
    {
        tool_error("the Workbench screen has no bitmap");
        return FALSE;
    }

    flags  = GetBitMapAttr(bm, BMA_FLAGS);
    depth  = GetBitMapAttr(bm, BMA_DEPTH);
    width  = GetBitMapAttr(bm, BMA_WIDTH);
    height = GetBitMapAttr(bm, BMA_HEIGHT);

    if ((flags & BMF_STANDARD) == 0)
    {
        tool_error("the Workbench screen is not a standard planar bitmap "
                   "(BMA_FLAGS=0x%lx), so it has no bitplanes to read; this "
                   "grabs planar screens only", (LONG)flags);
        return FALSE;
    }

    if (depth < 1 || depth > PFS_MAX_DEPTH)
    {
        tool_error("the Workbench screen is %ld planes deep; this handles "
                   "1 to %ld", (LONG)depth, (LONG)PFS_MAX_DEPTH);
        return FALSE;
    }

    if (width < 1 || width > 65535 || height < 1 || height > 65535)
    {
        tool_error("the Workbench screen is %ldx%ld, which does not fit the "
                   "file format", (LONG)width, (LONG)height);
        return FALSE;
    }

    stride = (ULONG)(UWORD)bm->BytesPerRow;

    /* An interleaved bitmap's BytesPerRow spans every plane; the stride from
       one row to the next in a plane is that, and a plane's row is a depth of
       it.  A non-interleaved one has the two equal. */
    if ((flags & BMF_INTERLEAVED) != 0)
    {
        if (stride == 0 || (stride % depth) != 0)
        {
            tool_error("interleaved bitmap with BytesPerRow=%ld over %ld "
                       "planes, which does not divide", (LONG)stride,
                       (LONG)depth);
            return FALSE;
        }
        g->row_bytes = (UWORD)(stride / depth);
    }
    else
    {
        g->row_bytes = (UWORD)stride;
    }

    if ((ULONG)g->row_bytes * 8UL < width)
    {
        tool_error("bitmap says %ld bytes a row for %ld pixels, which is too "
                   "few; refusing to read past the plane", (LONG)g->row_bytes,
                   (LONG)width);
        return FALSE;
    }

    for (plane = 0; plane < (UWORD)depth; plane++)
    {
        if (bm->Planes[plane] == NULL)
        {
            tool_error("bitplane %ld of %ld is not allocated",
                       (LONG)plane, (LONG)depth);
            return FALSE;
        }
    }

    g->width       = (UWORD)width;
    g->height      = (UWORD)height;
    g->depth       = (UWORD)depth;
    g->row_stride  = stride;
    g->frame_bytes = (ULONG)g->row_bytes * (ULONG)g->height * depth;

    return TRUE;
}

static BOOL geometry_same(const Geometry *a, const Geometry *b)
{
    return (BOOL)(a->width == b->width && a->height == b->height &&
                  a->depth == b->depth && a->row_bytes == b->row_bytes &&
                  a->row_stride == b->row_stride);
}

/* ---------------------------------------------------------------- palette */

/*
 * 3 * (1 << depth) bytes whatever the ColorMap holds: a screen whose map is
 * shorter than its depth leaves the tail black rather than shortening the
 * file, so the header arithmetic is the only thing a reader needs.
 */
static VOID read_palette(struct ColorMap *cm, UWORD depth, UBYTE *pal)
{
    ULONG colours = 1UL << depth;
    ULONG have    = (cm != NULL) ? (ULONG)cm->Count : 0;
    ULONG first;

    memset(pal, 0, (size_t)(3UL * colours));

    if (have > colours)
        have = colours;

    for (first = 0; first < have; first += 16)
    {
        ULONG table[3 * 16];
        ULONG n = have - first;
        ULONG i;

        if (n > 16)
            n = 16;

        GetRGB32(cm, first, n, table);

        for (i = 0; i < n; i++)
        {
            pal[(first + i) * 3 + 0] = (UBYTE)(table[i * 3 + 0] >> 24);
            pal[(first + i) * 3 + 1] = (UBYTE)(table[i * 3 + 1] >> 24);
            pal[(first + i) * 3 + 2] = (UBYTE)(table[i * 3 + 2] >> 24);
        }
    }
}

/* ------------------------------------------------------------------- grab */

/*
 * Plane-major into dst, which is frame_bytes long.  Called with the layers
 * locked and the blitter idle, and it does nothing but read memory, so it
 * cannot be what a lock is held across.
 */
static VOID copy_frame(struct BitMap *bm, const Geometry *g, UBYTE *dst)
{
    UWORD plane;

    for (plane = 0; plane < g->depth; plane++)
    {
        const UBYTE *src = (const UBYTE *)bm->Planes[plane];
        UWORD        y;

        for (y = 0; y < g->height; y++)
        {
            CopyMem((APTR)src, dst, (LONG)g->row_bytes);
            src += g->row_stride;
            dst += g->row_bytes;
        }
    }
}

/*
 * One frame into buf.  The screen is locked, read and unlocked here and
 * nowhere else, so the caller may take as long as it likes over the file.
 * FALSE having said why; *changed is TRUE when the refusal is that the screen
 * is no longer the one the header describes.
 */
static BOOL grab_frame(const Geometry *want, UBYTE *buf, BOOL *changed)
{
    struct Screen *sc;
    Geometry       now;
    BOOL           ok;

    *changed = FALSE;

    sc = LockPubScreen((CONST_STRPTR)PUBSCREEN_NAME);
    if (sc == NULL)
    {
        tool_error("there is no Workbench screen: LockPubScreen(\"%s\") "
                   "found nothing", (LONG)PUBSCREEN_NAME);
        return FALSE;
    }

    ok = geometry_of(sc->RastPort.BitMap, &now);
    if (ok && !geometry_same(want, &now))
    {
        tool_error("the Workbench screen changed under us: %ldx%ldx%ld, was "
                   "%ldx%ldx%ld", (LONG)now.width, (LONG)now.height,
                   (LONG)now.depth, (LONG)want->width, (LONG)want->height,
                   (LONG)want->depth);
        *changed = TRUE;
        ok = FALSE;
    }

    if (ok)
    {
        LockLayers(&sc->LayerInfo);
        WaitBlit();
        copy_frame(sc->RastPort.BitMap, want, buf);
        UnlockLayers(&sc->LayerInfo);
    }

    UnlockPubScreen(NULL, sc);
    return ok;
}

/* ------------------------------------------------------------------- file */

static BOOL write_all(BPTR fh, const void *data, ULONG len)
{
    LONG written = Write(fh, (APTR)data, (LONG)len);

    if (written == (LONG)len)
        return TRUE;

    tool_error("write failed after %ld of %ld bytes", (LONG)written,
               (LONG)len);
    tool_fault(IoErr());
    return FALSE;
}

/* frameCount, once the run is over and the count is known. */
static BOOL patch_frame_count(BPTR fh, UWORD frames)
{
    UBYTE be[2];

    put_be16(be, frames);

    if (Seek(fh, 12, OFFSET_BEGINNING) < 0)
    {
        tool_error("cannot seek back to the frame count");
        tool_fault(IoErr());
        return FALSE;
    }

    return write_all(fh, be, sizeof(be));
}

/* ------------------------------------------------------------------- main */

int main(int argc, char **argv)
{
    LONG            args[ARG_COUNT];
    struct RDArgs  *rda;
    struct Screen  *sc;
    struct ColorMap *cm;
    Geometry        g;
    const char     *path;
    UBYTE          *buf     = NULL;
    ULONG          *times   = NULL;
    ULONG           first_ms = 0;
    BPTR            fh      = 0;
    ULONG           frames  = DEFAULT_FRAMES;
    ULONG           delay   = DEFAULT_DELAY;
    ULONG           written = 0;
    ULONG           header_bytes;
    int             rc      = RETURN_FAIL;
    BOOL            changed = FALSE;

    (VOID)argv;

    if (tool_from_workbench(argc))
        return RETURN_FAIL;

    tool_break_arm();

    args[ARG_FRAMES] = 0;
    args[ARG_DELAY]  = 0;
    args[ARG_TO]     = 0;

    rda = ReadArgs((CONST_STRPTR)TEMPLATE, args, NULL);
    if (rda == NULL)
    {
        tool_fault(IoErr());
        tool_usage("[FRAMES <n>] [DELAY <ticks>] [TO <file>]",
                   "Write the Workbench screen's bitplanes to a file.");
        return RETURN_ERROR;
    }

    if (args[ARG_FRAMES] != 0)
    {
        LONG n = *(LONG *)args[ARG_FRAMES];

        if (n < 1 || n > 65535)
        {
            tool_error("FRAMES is %ld; the file format counts 1 to 65535",
                       (LONG)n);
            FreeArgs(rda);
            return RETURN_ERROR;
        }
        frames = (ULONG)n;
    }

    if (args[ARG_DELAY] != 0)
    {
        LONG n = *(LONG *)args[ARG_DELAY];

        if (n < 0)
        {
            tool_error("DELAY is %ld; ticks cannot be negative", (LONG)n);
            FreeArgs(rda);
            return RETURN_ERROR;
        }
        delay = (ULONG)n;
    }

    path = (args[ARG_TO] != 0) ? (const char *)args[ARG_TO] : DEFAULT_FILE;

    if (!open_libraries())
    {
        close_libraries();
        FreeArgs(rda);
        return RETURN_FAIL;
    }

    /* The first lock settles the geometry and the palette, and everything
       after it is checked against what this saw. */
    sc = LockPubScreen((CONST_STRPTR)PUBSCREEN_NAME);
    if (sc == NULL)
    {
        tool_error("there is no Workbench screen: LockPubScreen(\"%s\") "
                   "found nothing", (LONG)PUBSCREEN_NAME);
        goto done;
    }

    if (!geometry_of(sc->RastPort.BitMap, &g))
    {
        UnlockPubScreen(NULL, sc);
        goto done;
    }

    cm = sc->ViewPort.ColorMap;

    header[0] = 'P'; header[1] = 'F'; header[2] = 'S'; header[3] = '2';
    put_be16(header + 4, g.width);
    put_be16(header + 6, g.height);
    header[8] = (UBYTE)g.depth;
    header[9] = 0;
    put_be16(header + 10, g.row_bytes);
    put_be16(header + 12, frames);
    put_be16(header + 14, 0);
    read_palette(cm, g.depth, header + PFS_HEADER_SIZE);
    header_bytes = PFS_HEADER_SIZE + 3UL * (1UL << g.depth);

    tool_printf("screen_width=%ld\n", (LONG)sc->Width);
    tool_printf("screen_height=%ld\n", (LONG)sc->Height);

    UnlockPubScreen(NULL, sc);

    /* One frame, reused.  Frames go to the file as they are taken, so the
       cost of a hundred of them is the cost of one. */
    buf = (UBYTE *)AllocVec(g.frame_bytes, MEMF_ANY);
    if (buf == NULL)
    {
        tool_error("no memory for a %ld byte frame", (LONG)g.frame_bytes);
        goto done;
    }

    /* The timestamps, held until the frames are all written, because the table
       goes after them.  Four bytes a frame, so the whole of the biggest file
       this can write is 256 KB of them beside a frame buffer that is already
       larger than that. */
    times = (ULONG *)AllocVec(frames * sizeof(ULONG), MEMF_ANY);
    if (times == NULL)
    {
        tool_error("no memory for %ld timestamps", (LONG)frames);
        goto done;
    }

    fh = Open((CONST_STRPTR)path, MODE_NEWFILE);
    if (fh == 0)
    {
        tool_error("cannot write %s", (LONG)path);
        tool_fault(IoErr());
        goto done;
    }

    if (!write_all(fh, header, header_bytes))
        goto done;

    while (written < frames)
    {
        ULONG now;

        if (!grab_frame(&g, buf, &changed))
        {
            if (changed)
                tool_printf("screen_changed=1\n");
            break;
        }

        /* Read after the grab and before the write, so the number is when the
           pixels were taken rather than when the disk finished with them. */
        now = wb_millis();
        if (written == 0)
            first_ms = now;

        if (!write_all(fh, buf, g.frame_bytes))
            goto done;

        /* Never backwards, so a run over midnight cannot produce a table a
           reader has to second-guess. */
        times[written] = (now >= first_ms) ? (now - first_ms)
                                           : ((written > 0) ? times[written - 1]
                                                            : 0UL);
        written++;

        if (written < frames && delay > 0 && tool_delay_ticks(delay))
        {
            tool_printf("interrupted=1\n");
            break;
        }

        if (tool_break())
        {
            tool_printf("interrupted=1\n");
            break;
        }
    }

    if (written == 0)
        goto done;

    /* The frame records, after the last frame.  Written in batches so a
       hundred frames are not a hundred Write() calls. */
    {
        UBYTE be[PFS_FRAMEREC * 16];
        ULONG done_ts = 0;

        while (done_ts < written)
        {
            ULONG n = written - done_ts;
            ULONG i;

            if (n > 16UL)
                n = 16UL;

            memset(be, 0, (size_t)(n * PFS_FRAMEREC));
            for (i = 0; i < n; i++)
                put_be32(be + i * PFS_FRAMEREC, times[done_ts + i]);

            if (!write_all(fh, be, n * PFS_FRAMEREC))
                goto done;

            done_ts += n;
        }
    }

    if (written != frames && !patch_frame_count(fh, (UWORD)written))
        goto done;

    rc = (written == frames) ? RETURN_OK : RETURN_WARN;

    tool_printf("file=%s\n", (LONG)path);
    tool_printf("width=%ld\n", (LONG)g.width);
    tool_printf("height=%ld\n", (LONG)g.height);
    tool_printf("depth=%ld\n", (LONG)g.depth);
    tool_printf("bytesperrow=%ld\n", (LONG)g.row_bytes);
    tool_printf("framebytes=%ld\n", (LONG)g.frame_bytes);
    tool_printf("frames=%ld\n", (LONG)written);
    tool_printf("milliseconds=%ld\n", (LONG)times[written - 1]);
    tool_printf("filebytes=%ld\n",
                (LONG)(header_bytes +
                       (g.frame_bytes + PFS_FRAMEREC) * written));

done:
    if (fh != 0)
        Close(fh);
    if (times != NULL)
        FreeVec(times);
    if (buf != NULL)
        FreeVec(buf);
    close_libraries();
    FreeArgs(rda);

    tool_printf("RESULT=%s\n", (LONG)((rc == RETURN_OK) ? "OK" : "FAIL"));
    return rc;
}

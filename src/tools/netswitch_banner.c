/*
 * NetSwitch's banner: one 256-colour ILBM, rendered once into a bitmap for
 * the screen the window opens on and blitted on every refresh.
 *
 * A cybergraphics screen deeper than 8 bits gets the RGB as it is
 * (WritePixelArray; Picasso96 answers through its cybergraphics emulation).
 * A palette screen first gets up to NS_BANNER_PENS of the banner's most
 * common colours as shared pens, where it has them to give, then the image
 * is Atkinson-dithered to every colour the screen shows.  The nearest pen is
 * cached a 12-bit RGB at a time, so a 68000 searches the palette at most
 * once for each.  Graphics V40 (WriteChunkyPixels) is needed; earlier
 * systems open the window without the banner.
 *
 * SPDX-License-Identifier: MIT
 */

#include "tools.h"
#include "netswitch_banner.h"

#include <exec/memory.h>
#include <graphics/gfxbase.h>
#include <graphics/rastport.h>
#include <graphics/view.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <proto/graphics.h>

#include <inline/macros.h>

extern struct GfxBase *GfxBase;

#define NS_FILE_MAX (64UL * 1024UL)

/* cybergraphics.library, hand-declared as httprtg.c has it (cybergraphics.fd). */
static struct Library *ns_cgx_base;

#define CYBRMATTR_ISCYBERGFX 0x80000008UL
#define RECTFMT_RGB          0UL

static ULONG ns_cgx_attr(struct BitMap *bm, ULONG attr)
{
    return LP2(0x60, ULONG, GetCyberMapAttr, struct BitMap *, bm, a0,
               ULONG, attr, d0, , ns_cgx_base);
}

static ULONG ns_cgx_write(APTR src, UWORD sx, UWORD sy, UWORD smod,
                          struct RastPort *rp, UWORD dx, UWORD dy,
                          UWORD w, UWORD h, UBYTE fmt)
{
    return LP10(0x7e, ULONG, WritePixelArray, APTR, src, a0, UWORD, sx, d0,
                UWORD, sy, d1, UWORD, smod, d2, struct RastPort *, rp, a1,
                UWORD, dx, d3, UWORD, dy, d4, UWORD, w, d5, UWORD, h, d6,
                UBYTE, fmt, d7, , ns_cgx_base);
}

/* The decoded file: an index a pixel and the CMAP. */
typedef struct NsImage
{
    UWORD  w, h;
    UBYTE *pix;
    UBYTE  cmap[256 * 3];
} NsImage;

static ULONG be32(const UBYTE *p)
{
    return ((ULONG)p[0] << 24) | ((ULONG)p[1] << 16) | ((ULONG)p[2] << 8) | p[3];
}

/* One row of one plane, ByteRun1 or stored.  NULL when the data runs out. */
static const UBYTE *unpack_row(const UBYTE *in, const UBYTE *end, UBYTE *out,
                               ULONG n, UBYTE packed)
{
    ULONG i = 0;

    while (i < n)
    {
        BYTE c;

        if (!packed)
        {
            if (in + n > end) return NULL;
            CopyMem((APTR)in, out, n);
            return in + n;
        }
        if (in >= end) return NULL;
        c = (BYTE)*in++;
        if (c >= 0)
        {
            ULONG k = (ULONG)c + 1;

            if (in + k > end || i + k > n) return NULL;
            CopyMem((APTR)in, out + i, k);
            in += k;
            i += k;
        }
        else if (c != -128)
        {
            ULONG k = (ULONG)(1 - c);

            if (in >= end || i + k > n) return NULL;
            while (k--) out[i++] = *in;
            in++;
        }
    }
    return in;
}

/* FORM ILBM with BMHD, CMAP and BODY; no mask plane. */
static BOOL parse_ilbm(const UBYTE *f, ULONG len, NsImage *img)
{
    const UBYTE *p = f + 12, *end = f + len, *body = NULL;
    ULONG body_len = 0, rowbytes, x, y, pl;
    UBYTE planes = 0, packed = 0;
    UBYTE *row;

    if (len < 12 || be32(f) != 0x464F524DUL || be32(f + 8) != 0x494C424DUL)
        return FALSE;
    while (p + 8 <= end)
    {
        ULONG id = be32(p), n = be32(p + 4);

        p += 8;
        if (n > (ULONG)(end - p)) return FALSE;
        if (id == 0x424D4844UL && n >= 20)              /* BMHD */
        {
            img->w = (UWORD)((p[0] << 8) | p[1]);
            img->h = (UWORD)((p[2] << 8) | p[3]);
            planes = p[8];
            if (p[9] == 1) return FALSE;                /* mskHasMask */
            packed = p[10];
        }
        else if (id == 0x434D4150UL)                    /* CMAP */
            CopyMem((APTR)p, img->cmap, n < sizeof(img->cmap) ? n : sizeof(img->cmap));
        else if (id == 0x424F4459UL)                    /* BODY */
        {
            body = p;
            body_len = n;
        }
        p += n + (n & 1);
    }
    if (body == NULL || planes == 0 || planes > 8 || img->w == 0 ||
        img->w > 640 || img->h == 0 || img->h > 128 || packed > 1)
        return FALSE;

    rowbytes = (((ULONG)img->w + 15) >> 4) << 1;
    img->pix = (UBYTE *)AllocVec((ULONG)img->w * img->h + rowbytes,
                                 MEMF_ANY | MEMF_CLEAR);
    if (img->pix == NULL) return FALSE;
    row = img->pix + (ULONG)img->w * img->h;
    end = body + body_len;
    for (y = 0, p = body; y < img->h; y++)
    {
        UBYTE *dst = img->pix + y * img->w;

        for (pl = 0; pl < planes; pl++)
        {
            if ((p = unpack_row(p, end, row, rowbytes, packed)) == NULL)
                return FALSE;
            for (x = 0; x < img->w; x++)
                if (row[x >> 3] & (0x80 >> (x & 7)))
                    dst[x] |= (UBYTE)(1 << pl);
        }
    }
    return TRUE;
}

static UBYTE *read_file(const char *name, ULONG *len)
{
    BPTR   fh = Open((CONST_STRPTR)name, MODE_OLDFILE);
    UBYTE *buf = NULL;
    LONG   n;

    if (fh == (BPTR)0) return NULL;
    if (Seek(fh, 0, OFFSET_END) >= 0 && (n = Seek(fh, 0, OFFSET_BEGINNING)) > 0 &&
        (ULONG)n <= NS_FILE_MAX && (buf = (UBYTE *)AllocVec((ULONG)n, MEMF_ANY)) != NULL)
    {
        if (Read(fh, buf, n) != n)
        {
            FreeVec(buf);
            buf = NULL;
        }
        *len = (ULONG)n;
    }
    Close(fh);
    return buf;
}

/* Truecolour: a row at a time through a small RGB buffer. */
static BOOL render_rgb(const NsImage *img, struct RastPort *rp)
{
    UBYTE *rgb = (UBYTE *)AllocVec((ULONG)img->w * 3, MEMF_ANY);
    UWORD  x, y;

    if (rgb == NULL) return FALSE;
    for (y = 0; y < img->h; y++)
    {
        for (x = 0; x < img->w; x++)
            CopyMem((APTR)&img->cmap[img->pix[(ULONG)y * img->w + x] * 3],
                    rgb + x * 3, 3);
        ns_cgx_write(rgb, 0, 0, (UWORD)(img->w * 3), rp, 0, y, img->w, 1,
                     RECTFMT_RGB);
    }
    FreeVec(rgb);
    return TRUE;
}

/* Shared pens for the banner's most common colours, by 12-bit bin; a pick
   clears its neighbours so the set spreads.  `hist` is 4096 UWORDs. */
static VOID obtain_pens(struct Screen *s, const NsImage *img, UWORD *hist,
                        NsBanner *b, ULONG want)
{
    ULONG i, n = (ULONG)img->w * img->h;

    for (i = 0; i < 4096; i++) hist[i] = 0;
    for (i = 0; i < n; i++)
    {
        const UBYTE *c = &img->cmap[img->pix[i] * 3];
        UWORD *h = &hist[((c[0] >> 4) << 8) | ((c[1] >> 4) << 4) | (c[2] >> 4)];

        if (*h != 0xFFFF) (*h)++;
    }
    while (b->npens < want)
    {
        struct TagItem tags[] = { { OBP_Precision, PRECISION_IMAGE },
                                  { TAG_DONE, 0 } };
        ULONG best = 0, r, g, bl;
        LONG  pen, dr, dg, db;

        for (i = 1; i < 4096; i++) if (hist[i] > hist[best]) best = i;
        if (hist[best] == 0) break;
        r = (best >> 8) * 0x11111111UL;
        g = ((best >> 4) & 15) * 0x11111111UL;
        bl = (best & 15) * 0x11111111UL;
        pen = ObtainBestPenA(s->ViewPort.ColorMap, r, g, bl, tags);
        if (pen >= 0) b->pens[b->npens++] = (UBYTE)pen;
        else break;
        for (dr = -2; dr <= 2; dr++)
            for (dg = -2; dg <= 2; dg++)
                for (db = -2; db <= 2; db++)
                {
                    LONG R = (LONG)(best >> 8) + dr;
                    LONG G = (LONG)((best >> 4) & 15) + dg;
                    LONG B = (LONG)(best & 15) + db;

                    if (R >= 0 && R < 16 && G >= 0 && G < 16 && B >= 0 && B < 16)
                        hist[(R << 8) | (G << 4) | B] = 0;
                }
    }
}

/* Atkinson: 1/8 of the error to six neighbours, two rows ahead. */
static BOOL render_dither(struct Screen *s, const NsImage *img,
                          struct RastPort *rp, NsBanner *b)
{
    ULONG   ncol = 1UL << GetBitMapAttr(s->RastPort.BitMap, BMA_DEPTH);
    ULONG   w = img->w, stride = (w + 4) * 3;
    ULONG   size = 4096 * 2 + 256 * 3 * 4 + 3 * stride * 2 + w;
    UBYTE  *mem = (UBYTE *)AllocVec(size, MEMF_ANY | MEMF_CLEAR);
    UWORD  *lut;
    ULONG  *pal;
    WORD   *err[3];
    UBYTE  *out;
    ULONG   x, y, i, c;

    if (mem == NULL) return FALSE;
    lut = (UWORD *)mem;
    pal = (ULONG *)(mem + 4096 * 2);
    err[0] = (WORD *)(mem + 4096 * 2 + 256 * 3 * 4);
    err[1] = err[0] + stride;
    err[2] = err[1] + stride;
    out = (UBYTE *)(err[2] + stride);

    obtain_pens(s, img, lut, b, ncol < NS_BANNER_PENS ? ncol : NS_BANNER_PENS);
    GetRGB32(s->ViewPort.ColorMap, 0, ncol, pal);
    for (i = 0; i < ncol * 3; i++) pal[i] >>= 24;
    for (i = 0; i < 4096; i++) lut[i] = 0xFFFF;

    for (y = 0; y < img->h; y++)
    {
        WORD *e0 = err[0] + 3, *e1 = err[1] + 3, *e2 = err[2] + 3;

        for (x = 0; x < w; x++)
        {
            const UBYTE *src = &img->cmap[img->pix[y * w + x] * 3];
            LONG  v[3];
            UWORD key, pen;

            for (c = 0; c < 3; c++)
            {
                v[c] = (LONG)src[c] + e0[x * 3 + c];
                if (v[c] < 0) v[c] = 0; else if (v[c] > 255) v[c] = 255;
            }
            key = (UWORD)(((v[0] >> 4) << 8) | ((v[1] >> 4) << 4) | (v[2] >> 4));
            if ((pen = lut[key]) == 0xFFFF)
            {
                ULONG best = ~0UL;

                for (i = 0; i < ncol; i++)
                {
                    LONG dr = (LONG)pal[i * 3] - (LONG)((v[0] & 0xF0) | 8);
                    LONG dg = (LONG)pal[i * 3 + 1] - (LONG)((v[1] & 0xF0) | 8);
                    LONG db = (LONG)pal[i * 3 + 2] - (LONG)((v[2] & 0xF0) | 8);
                    ULONG d = (ULONG)(dr * dr * 3 + dg * dg * 4 + db * db * 2);

                    if (d < best) { best = d; pen = (UWORD)i; }
                }
                lut[key] = pen;
            }
            out[x] = (UBYTE)pen;
            for (c = 0; c < 3; c++)
            {
                WORD d = (WORD)((v[c] - (LONG)pal[pen * 3 + c]) >> 3);

                e0[(x + 1) * 3 + c] += d;
                e0[(x + 2) * 3 + c] += d;
                e1[(LONG)x * 3 - 3 + (LONG)c] += d;    /* left pad */
                e1[x * 3 + c] += d;
                e1[(x + 1) * 3 + c] += d;
                e2[x * 3 + c] += d;
            }
        }
        WriteChunkyPixels(rp, 0, y, w - 1, y, out, w);
        /* The rows move up one; the new last row starts clean. */
        {
            WORD *t = err[0];

            err[0] = err[1];
            err[1] = err[2];
            err[2] = t;
            for (i = 0; i < stride; i++) t[i] = 0;
        }
    }
    FreeVec(mem);
    return TRUE;
}

BOOL ns_banner_load(struct Screen *s, const char *file, NsBanner *b)
{
    struct BitMap *sbm = s->RastPort.BitMap;
    ULONG          depth;
    struct RastPort rp;
    NsImage        img;
    UBYTE         *f;
    ULONG          len = 0;
    BOOL           ok = FALSE, rgb;

    img.pix = NULL;
    b->bm = NULL;
    b->npens = 0;
    if (GfxBase->LibNode.lib_Version < 40) return FALSE;
    depth = GetBitMapAttr(sbm, BMA_DEPTH);
    if (depth > 8)
    {
        ns_cgx_base = OpenLibrary((CONST_STRPTR)"cybergraphics.library", 40);
        if (ns_cgx_base == NULL) return FALSE;
        if (ns_cgx_attr(sbm, CYBRMATTR_ISCYBERGFX) == 0) goto out;
    }
    rgb = (BOOL)(depth > 8);

    if ((f = read_file(file, &len)) == NULL) goto out;
    ok = parse_ilbm(f, len, &img);
    FreeVec(f);
    if (!ok) goto out;
    ok = FALSE;

    b->bm = AllocBitMap(img.w, img.h, depth, BMF_CLEAR, sbm);
    if (b->bm == NULL) goto out;
    InitRastPort(&rp);
    rp.BitMap = b->bm;
    b->w = (WORD)img.w;
    b->h = (WORD)img.h;
    ok = rgb ? render_rgb(&img, &rp) : render_dither(s, &img, &rp, b);
out:
    if (img.pix != NULL) FreeVec(img.pix);
    if (ns_cgx_base != NULL) CloseLibrary(ns_cgx_base);
    ns_cgx_base = NULL;
    if (!ok) ns_banner_free(s, b);
    return ok;
}

VOID ns_banner_free(struct Screen *s, NsBanner *b)
{
    while (b->npens > 0)
        ReleasePen(s->ViewPort.ColorMap, b->pens[--b->npens]);
    if (b->bm != NULL) FreeBitMap(b->bm);
    b->bm = NULL;
    b->w = b->h = 0;
}

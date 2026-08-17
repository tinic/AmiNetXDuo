/* Synthetic .pfs sequences, so the encoder can be measured before a real
 * capture exists.  Host-only, loose on purpose.
 *
 *   rfbgen <outdir>
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aminetxduo/rfb_encode.h"

static unsigned g_w, g_h, g_depth, g_bpr;
static unsigned char *g_idx;   /* w*h colour indices */

/* Which of the three source layouts the frames go out as: RFB_FMT_PLANAR,
   RFB_FMT_CLUT8 or RFB_FMT_RGB565, which is also what the .pfs header carries
   in its byte 9.  g_idx is indices whichever it is, so this changes only what
   emit() writes and how wide a row is. */
static unsigned g_fmt;

#define GEN_FMT_PLANAR  0u
#define GEN_FMT_CLUT8   1u
#define GEN_FMT_RGB565  2u
/* The three chipset modes.  Their frames are a planar screen's, byte for
   byte, so emit() does not know about them at all; what they change is the
   header's byte 9 and how long the palette in front of the frames is. */
#define GEN_FMT_HAM6    3u
#define GEN_FMT_HAM8    4u
#define GEN_FMT_EHB     5u

/* What the drawing may put in g_idx, which is a byte array whatever the
   format is.  It follows the depth on a planar screen and is a byte
   everywhere else, including truecolour, where an index is a way of choosing
   a colour and not something the frames carry. */
static unsigned g_idxmask;

static unsigned hash32(unsigned x)
{
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

/* Sparse, text-like 8x8 glyphs: a couple of stems and a couple of bars. */
static void glyph(unsigned ch, unsigned char *rows)
{
    unsigned hs = hash32(ch * 2654435761u);
    unsigned char stem = 0;
    unsigned r;

    if (hs & 1u) stem |= 0x40u;
    if (hs & 2u) stem |= 0x02u;
    if (hs & 4u) stem |= 0x10u;
    if (!stem)   stem  = 0x20u;

    for (r = 0; r < 8; r++) {
        unsigned hr = hash32(ch * 97u + r * 31u + 7u);
        if (r == 7) { rows[r] = 0; continue; }
        if ((hr & 7u) == 0u)
            rows[r] = 0x7Eu;                      /* a bar */
        else if ((hr & 0x30u) == 0x30u)
            rows[r] = (unsigned char)(stem | 0x08u);
        else
            rows[r] = stem;
    }
}

static void px(unsigned x, unsigned y, unsigned c)
{
    if (x < g_w && y < g_h)
        g_idx[y * g_w + x] = (unsigned char)c;
}

static void fill(unsigned x0, unsigned y0, unsigned w, unsigned h, unsigned c)
{
    unsigned x, y;
    for (y = y0; y < y0 + h; y++)
        for (x = x0; x < x0 + w; x++)
            px(x, y, c);
}

static void frame_rect(unsigned x0, unsigned y0, unsigned w, unsigned h,
                       unsigned c)
{
    unsigned i;
    for (i = 0; i < w; i++) { px(x0 + i, y0, c); px(x0 + i, y0 + h - 1, c); }
    for (i = 0; i < h; i++) { px(x0, y0 + i, c); px(x0 + w - 1, y0 + i, c); }
}

static void text(unsigned x0, unsigned y0, unsigned seed, unsigned n,
                 unsigned fg, unsigned bg)
{
    unsigned i, r, b;
    unsigned char rows[8];

    for (i = 0; i < n; i++) {
        glyph(hash32(seed + i * 5701u) & 0x3Fu, rows);
        for (r = 0; r < 8; r++)
            for (b = 0; b < 8; b++)
                px(x0 + i * 8 + b, y0 + r,
                   (rows[r] >> (7 - b)) & 1u ? fg : bg);
    }
}

/* Ordered 4x4 dither of a vertical gradient: byte-level entropy the way a
 * dithered Workbench backdrop has it, which is where PackBits loses. */
static void backdrop(void)
{
    static const unsigned char bayer[16] = {
         0,  8,  2, 10, 12,  4, 14,  6,
         3, 11,  1,  9, 15,  7, 13,  5 };
    unsigned x, y;
    for (y = 0; y < g_h; y++) {
        unsigned lvl = (y * 16u) / g_h;
        for (x = 0; x < g_w; x++)
            g_idx[y * g_w + x] =
                (unsigned char)(bayer[(y & 3u) * 4u + (x & 3u)] < lvl ? 2 : 1);
    }
}

static void desktop(void)
{
    backdrop();
    fill(0, 0, g_w, 11, 3);
    text(8, 2, 11, 20, 0, 3);
}

static void window(unsigned x0, unsigned y0, unsigned w, unsigned h,
                   unsigned seed)
{
    fill(x0, y0, w, h, 0);
    frame_rect(x0, y0, w, h, 4);
    fill(x0 + 1, y0 + 1, w - 2, 10, 5);
    text(x0 + 4, y0 + 2, seed, (w - 8) / 8 > 12 ? 12 : (w - 8) / 8, 0, 5);
}

/* ------------------------------------------------------------- output ---- */

static unsigned char *g_planes;   /* depth * bpr * h */

static void pack(void)
{
    unsigned p, y, x;
    memset(g_planes, 0, (size_t)g_depth * g_bpr * g_h);
    for (y = 0; y < g_h; y++) {
        const unsigned char *row = g_idx + (size_t)y * g_w;
        for (x = 0; x < g_w; x++) {
            unsigned c = row[x];
            unsigned byte = x >> 3, bit = 7u - (x & 7u);
            for (p = 0; p < g_depth; p++)
                if ((c >> p) & 1u)
                    g_planes[(size_t)p * g_bpr * g_h + (size_t)y * g_bpr + byte]
                        |= (unsigned char)(1u << bit);
        }
    }
}

/* How many frames have gone into the file that is open, so close_pfs() can
   write exactly that many timestamps after them. */
static unsigned g_emitted;

/* The cadence that the synthetic captures claim.  They are generated, not
   recorded, so there is no real timing to carry.  40 ms is the interval that
   DELAY 2 in wbgrab produces, so a generated file plays at the speed of the
   lab captures. */
#define GEN_FRAME_MS    40u

/*
 * The generator's palette entry for an index, which is the one open_pfs()
 * writes.  A function so that the truecolour frames carry the same colours
 * the palette formats do, and a sequence looks the same whichever format it
 * was written in.
 */
static void gen_rgb(unsigned i, unsigned char *out)
{
    out[0] = (unsigned char)(i * 37u);
    out[1] = (unsigned char)(i * 91u);
    out[2] = (unsigned char)(i * 53u);
}

static FILE *open_pfs(const char *dir, const char *name, unsigned frames)
{
    char path[512];
    unsigned char hdr[16];
    unsigned i, ncol;
    FILE *f;

    /* Asked rather than computed.  1 << depth is the answer on one format of
       the six: truecolour has no palette at all and the three chipset modes
       carry fewer colours than their depth implies.  Getting it wrong puts
       every frame in the file at the wrong offset. */
    {
        rfb_geom q;
        memset(&q, 0, sizeof(q));
        q.depth  = (rfb_u8)g_depth;
        q.format = (rfb_u8)g_fmt;
        ncol = rfb_pal_colours(&q);
    }

    snprintf(path, sizeof(path), "%s/%s.pfs", dir, name);
    f = fopen(path, "wb");
    if (!f) { perror(path); exit(1); }

    g_emitted = 0;

    memcpy(hdr, "PFS2", 4);
    hdr[4] = (unsigned char)(g_w >> 8);   hdr[5] = (unsigned char)g_w;
    hdr[6] = (unsigned char)(g_h >> 8);   hdr[7] = (unsigned char)g_h;
    hdr[8] = (unsigned char)g_depth;
    hdr[9] = (unsigned char)g_fmt;
    hdr[10] = (unsigned char)(g_bpr >> 8); hdr[11] = (unsigned char)g_bpr;
    hdr[12] = (unsigned char)(frames >> 8); hdr[13] = (unsigned char)frames;
    hdr[14] = 0; hdr[15] = 0;
    fwrite(hdr, 1, 16, f);
    for (i = 0; i < ncol; i++) {
        unsigned char rgb[3];
        gen_rgb(i, rgb);
        fwrite(rgb, 1, 3, f);
    }
    return f;
}

static void emit(FILE *f)
{
    if (g_fmt == GEN_FMT_CLUT8) {
        unsigned y;
        /* One byte a pixel, at the row stride, and the padding past the width
           left as it was allocated.  That is the shape of a card framebuffer,
           and the encoder codes every byte of it. */
        memset(g_planes, 0, (size_t)g_bpr * g_h);
        for (y = 0; y < g_h; y++)
            memcpy(g_planes + (size_t)y * g_bpr, g_idx + (size_t)y * g_w, g_w);
        fwrite(g_planes, 1, (size_t)g_bpr * g_h, f);
    } else if (g_fmt == GEN_FMT_RGB565) {
        unsigned y, x;
        /*
         * Two bytes a pixel, big-endian, the same padding rule.  The colour
         * is the index's palette entry with a gradient mixed into the blue,
         * so the frames are not 256 flat colours: a synthetic truecolour
         * screen made of nothing but palette entries would compress far
         * better than any real one and would make the round trip agree with
         * itself for the wrong reason.
         */
        memset(g_planes, 0, (size_t)g_bpr * g_h);
        for (y = 0; y < g_h; y++) {
            unsigned char *row = g_planes + (size_t)y * g_bpr;
            for (x = 0; x < g_w; x++) {
                unsigned char rgb[3];
                unsigned v;

                gen_rgb(g_idx[(size_t)y * g_w + x], rgb);
                rgb[2] = (unsigned char)((rgb[2] + x * 255u / g_w) / 2u);

                v = ((unsigned)(rgb[0] >> 3) << 11)
                  | ((unsigned)(rgb[1] >> 2) << 5)
                  | (unsigned)(rgb[2] >> 3);
                row[x * 2u]      = (unsigned char)(v >> 8);
                row[x * 2u + 1u] = (unsigned char)v;
            }
        }
        fwrite(g_planes, 1, (size_t)g_bpr * g_h, f);
    } else {
        pack();
        fwrite(g_planes, 1, (size_t)g_depth * g_bpr * g_h, f);
    }
    g_emitted++;
}

/* The frame records go after the frames.  See pfs.ts for why they are there
   and not in front of them.  Twelve bytes each: when, where the pointer was,
   and which image it was.  A generated capture has no pointer, so the image
   is 0 and the position with it. */
static void close_pfs(FILE *f)
{
    unsigned i;

    for (i = 0; i < g_emitted; i++) {
        unsigned long t = (unsigned long)i * GEN_FRAME_MS;
        unsigned char rec[12];

        memset(rec, 0, sizeof(rec));
        rec[0] = (unsigned char)(t >> 24); rec[1] = (unsigned char)(t >> 16);
        rec[2] = (unsigned char)(t >> 8);  rec[3] = (unsigned char)t;
        fwrite(rec, 1, sizeof(rec), f);
    }
    fclose(f);
}

static void setup(unsigned w, unsigned h, unsigned depth)
{
    g_w = w; g_h = h; g_depth = depth; g_fmt = GEN_FMT_PLANAR;
    g_idxmask = (1u << depth) - 1u;
    g_bpr = ((w + 15u) / 16u) * 2u;
    free(g_idx); free(g_planes);
    g_idx = malloc((size_t)w * h);
    g_planes = malloc((size_t)depth * g_bpr * h);
    if (!g_idx || !g_planes) { fprintf(stderr, "oom\n"); exit(1); }
}

/*
 * The same, as an 8-bit chunky screen.  The row is padded to a multiple of
 * eight bytes on purpose: a board rounds its stride, and the encoder codes the
 * padding.  A generator that made bytes_per_row equal to the width exercises
 * neither the padding nor the clipped tile at the right edge.  804 wide comes
 * out 808, which is not a whole number of 16- or 32-byte tiles.
 */
static void setup_chunky(unsigned w, unsigned h)
{
    g_w = w; g_h = h; g_depth = 8; g_fmt = GEN_FMT_CLUT8;
    g_idxmask = 255u;
    g_bpr = ((w + 7u) / 8u) * 8u;
    free(g_idx); free(g_planes);
    g_idx = malloc((size_t)w * h);
    g_planes = malloc((size_t)g_bpr * h);
    if (!g_idx || !g_planes) { fprintf(stderr, "oom\n"); exit(1); }
}

/*
 * And as a truecolour card screen: two bytes a pixel, no palette, and the row
 * rounded up to a longword, which is what the server does with the width it
 * gets from the card.  The depth is 16 and is bits a pixel, so nothing here
 * may size a palette from it.
 */
static void setup_rgb565(unsigned w, unsigned h)
{
    g_w = w; g_h = h; g_depth = 16; g_fmt = GEN_FMT_RGB565;
    g_idxmask = 255u;
    g_bpr = ((w * 2u + 3u) / 4u) * 4u;
    free(g_idx); free(g_planes);
    g_idx = malloc((size_t)w * h);
    g_planes = malloc((size_t)g_bpr * h);
    if (!g_idx || !g_planes) { fprintf(stderr, "oom\n"); exit(1); }
}

/*
 * And the three chipset modes, which are a planar screen with a different
 * meaning laid over it.  The planes, the stride and the frames are what
 * setup() makes; only the format changes, and with it how long the palette in
 * front of the frames is -- 16 colours at HAM6, 64 at HAM8 and 32 at EHB,
 * none of them 1 << depth.
 *
 * The index mask stays the full depth on purpose.  A HAM6 index runs 0..63 and
 * only its low four bits are a colour; the other two say what to do with them,
 * so a generator that masked to 15 would emit no modify codes at all and the
 * round trip would never carry one.
 */
static void setup_chipset(unsigned w, unsigned h, unsigned depth, unsigned fmt)
{
    setup(w, h, depth);
    g_fmt = fmt;
}


/* --------------------------------------------------------- sequences ----- */

static void seq_idle(const char *dir, const char *name)
{
    FILE *f = open_pfs(dir, name, 60);
    unsigned i;
    for (i = 0; i < 60; i++) {
        desktop();
        window(64, 64, 400, 300, 3);
        text(72, 80, 100, 40, 1, 0);
        fill(72, 96, 8, 8, (i / 5u) & 1u ? 1 : 0);   /* blinking cursor */
        emit(f);
    }
    close_pfs(f);
}

static void seq_type(const char *dir)
{
    FILE *f = open_pfs(dir, "type", 60);
    unsigned i, k;
    for (i = 0; i < 60; i++) {
        desktop();
        window(64, 64, 400, 300, 3);
        for (k = 0; k < 10; k++)
            text(72, 80 + k * 8, 200 + k * 13, 40, 1, 0);
        text(72, 160, 900, i % 40u, 1, 0);          /* the line being typed */
        fill(72 + (i % 40u) * 8, 160, 8, 8, 1);
        emit(f);
    }
    close_pfs(f);
}

/* A Shell window that scrolls text up by `step` pixels a frame: everything
 * inside the window moves, which is the case a tile diff cannot win. */
static void seq_scroll(const char *dir, const char *name, unsigned step,
                       unsigned frames)
{
    FILE *f = open_pfs(dir, name, frames);
    unsigned i, k, wy = 96, wh = 320, lines = (320 - 16) / 8;
    for (i = 0; i < frames; i++) {
        unsigned scrolled = i * step;
        desktop();
        window(32, wy, 576, wh, 7);
        for (k = 0; k < lines; k++) {
            unsigned line = (scrolled / 8u) + k;
            unsigned yy = wy + 12 + k * 8 - (scrolled % 8u);
            if (yy >= wy + 12 && yy + 8 <= wy + wh - 2)
                text(40, yy, 4000 + line * 71u, 68, 1, 0);
        }
        emit(f);
    }
    close_pfs(f);
}

static void seq_drag(const char *dir)
{
    FILE *f = open_pfs(dir, "drag", 60);
    unsigned i;
    for (i = 0; i < 60; i++) {
        desktop();
        window(20 + i * 4, 40 + i * 3, 300, 200, 21);
        text(28 + i * 4, 60 + i * 3, 500, 30, 1, 0);
        emit(f);
    }
    close_pfs(f);
}

static void seq_menu(const char *dir, const char *name)
{
    FILE *f = open_pfs(dir, name, 40);
    unsigned i, k;
    for (i = 0; i < 40; i++) {
        desktop();
        window(64, 64, 400, 300, 3);
        if ((i / 10u) & 1u) {
            fill(100, 11, 180, 160, 5);
            frame_rect(100, 11, 180, 160, 4);
            for (k = 0; k < 18; k++)
                text(104, 14 + k * 8, 700 + k * 17u, 20, 0, 5);
        }
        emit(f);
    }
    close_pfs(f);
}

static void seq_full(const char *dir, const char *name)
{
    FILE *f = open_pfs(dir, name, 20);
    unsigned i, x, y;
    for (i = 0; i < 20; i++) {
        for (y = 0; y < g_h; y++)
            for (x = 0; x < g_w; x++)
                g_idx[y * g_w + x] =
                    (unsigned char)(hash32(x + y * 1237u + i * 99991u)
                                    & g_idxmask);
        emit(f);
    }
    close_pfs(f);
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : ".";

    setup(640, 480, 3);
    seq_idle(dir, "idle");
    seq_type(dir);
    seq_scroll(dir, "scroll", 8, 60);
    seq_scroll(dir, "scroll16", 16, 40);
    seq_scroll(dir, "scroll_slow", 2, 40);
    seq_drag(dir);
    seq_menu(dir, "menu");
    seq_full(dir, "full");

    setup(800, 600, 8);
    seq_idle(dir, "idle8");
    seq_scroll(dir, "scroll8", 8, 40);

    /* 128 bytes to a row, which is the widest a screen gets: a 1024-pixel
       Super-High Res screen, four planes.  The first frame of any sequence is
       encoded whole, so this walks every tile at that stride. */
    setup(1024, 768, 4);
    seq_idle(dir, "idle1024");
    seq_scroll(dir, "scroll1024", 8, 20);

    /* And the RTG shape: 8-bit chunky, at the two sizes a graphics card runs
       at.  640x480 has a bytes_per_row that is a whole number of 16-byte
       tiles, and 804 wide does not.  The clipped tile at the right edge is
       therefore walked too. */
    setup_chunky(640, 480);
    seq_idle(dir, "idle_c8");
    seq_scroll(dir, "scroll_c8", 8, 40);
    seq_menu(dir, "menu_c8");
    setup_chunky(804, 300);
    seq_idle(dir, "idle_c8pad");
    seq_full(dir, "full_c8pad");

    /*
     * And the truecolour shape: two bytes a pixel and no palette.  640 wide
     * is 1280 bytes to a row, a whole number of 16-byte tiles; 404 wide is
     * 808 and is not, so the clipped tile at the right edge is walked with a
     * pixel straddling it.
     *
     * Shorter sequences than the palette ones on purpose.  A frame here is
     * twice the bytes of an 8-bit one of the same size, and the round trip
     * encodes and decodes every frame of every sequence under three tile
     * sizes and two layouts, so 60 frames of 640x480 would put a quarter of a
     * gigabyte through it to test what 20 frames test.
     */
    setup_rgb565(640, 480);
    seq_scroll(dir, "scroll_rgb", 8, 20);
    seq_full(dir, "full_rgb");
    setup_rgb565(404, 200);
    seq_idle(dir, "idle_rgbpad");
    seq_menu(dir, "menu_rgbpad");

    /*
     * And the chipset modes.  Nothing in the encoder tells them from a planar
     * screen, which is the point: what the round trip checks here is that the
     * palette in front of the frames is the length the format says, so the
     * frames are found, and that a six or eight plane screen at those formats
     * is accepted rather than refused at init.
     *
     * 320x256 is what HAM6 and half-brite actually run at, and the 40-byte
     * row it gives is not a whole number of 16-byte tiles, so the clipped
     * tile at the right edge is walked as well.
     */
    setup_chipset(320, 256, 6, GEN_FMT_HAM6);
    seq_idle(dir, "idle_ham6");
    seq_menu(dir, "menu_ham6");
    setup_chipset(320, 256, 6, GEN_FMT_EHB);
    seq_idle(dir, "idle_ehb");
    seq_scroll(dir, "scroll_ehb", 8, 20);
    setup_chipset(640, 480, 8, GEN_FMT_HAM8);
    seq_idle(dir, "idle_ham8");
    seq_full(dir, "full_ham8");

    free(g_idx); free(g_planes);
    return 0;
}

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

static unsigned g_w, g_h, g_depth, g_bpr;
static unsigned char *g_idx;   /* w*h colour indices */

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
 * dithered Workbench backdrop has it, which is what makes PackBits lose. */
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

static FILE *open_pfs(const char *dir, const char *name, unsigned frames)
{
    char path[512];
    unsigned char hdr[16];
    unsigned i, ncol = 1u << g_depth;
    FILE *f;

    snprintf(path, sizeof(path), "%s/%s.pfs", dir, name);
    f = fopen(path, "wb");
    if (!f) { perror(path); exit(1); }

    memcpy(hdr, "PFS1", 4);
    hdr[4] = (unsigned char)(g_w >> 8);   hdr[5] = (unsigned char)g_w;
    hdr[6] = (unsigned char)(g_h >> 8);   hdr[7] = (unsigned char)g_h;
    hdr[8] = (unsigned char)g_depth;      hdr[9] = 0;
    hdr[10] = (unsigned char)(g_bpr >> 8); hdr[11] = (unsigned char)g_bpr;
    hdr[12] = (unsigned char)(frames >> 8); hdr[13] = (unsigned char)frames;
    hdr[14] = 0; hdr[15] = 0;
    fwrite(hdr, 1, 16, f);
    for (i = 0; i < ncol; i++) {
        unsigned char rgb[3];
        rgb[0] = (unsigned char)(i * 37u); rgb[1] = (unsigned char)(i * 91u);
        rgb[2] = (unsigned char)(i * 53u);
        fwrite(rgb, 1, 3, f);
    }
    return f;
}

static void emit(FILE *f)
{
    pack();
    fwrite(g_planes, 1, (size_t)g_depth * g_bpr * g_h, f);
}

static void setup(unsigned w, unsigned h, unsigned depth)
{
    g_w = w; g_h = h; g_depth = depth;
    g_bpr = ((w + 15u) / 16u) * 2u;
    free(g_idx); free(g_planes);
    g_idx = malloc((size_t)w * h);
    g_planes = malloc((size_t)depth * g_bpr * h);
    if (!g_idx || !g_planes) { fprintf(stderr, "oom\n"); exit(1); }
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
    fclose(f);
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
    fclose(f);
}

/* A Shell window scrolling text up by `step` pixels a frame: everything inside
 * the window moves, which is the case tile diffing cannot win. */
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
    fclose(f);
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
    fclose(f);
}

static void seq_menu(const char *dir)
{
    FILE *f = open_pfs(dir, "menu", 40);
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
    fclose(f);
}

static void seq_full(const char *dir)
{
    FILE *f = open_pfs(dir, "full", 20);
    unsigned i, x, y;
    for (i = 0; i < 20; i++) {
        for (y = 0; y < g_h; y++)
            for (x = 0; x < g_w; x++)
                g_idx[y * g_w + x] =
                    (unsigned char)(hash32(x + y * 1237u + i * 99991u)
                                    & ((1u << g_depth) - 1u));
        emit(f);
    }
    fclose(f);
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
    seq_menu(dir);
    seq_full(dir);

    setup(800, 600, 8);
    seq_idle(dir, "idle8");
    seq_scroll(dir, "scroll8", 8, 40);

    free(g_idx); free(g_planes);
    return 0;
}

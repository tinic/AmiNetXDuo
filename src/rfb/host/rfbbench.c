/* Host measurement CLI for the remote-framebuffer encoder.
 *
 *   rfbbench [--tiles WxH,...] [--strat name,...] [--csv] file.pfs ...
 *
 * Reads .pfs captures, runs every strategy over every sequence, decodes each
 * encoded frame back with the reference decoder below and compares it with the
 * frame that went in.  Output is key=value, one line per (sequence, strategy).
 *
 * Host-only and loose on purpose; the encoder next door is the portable half.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <aminetxduo/rfb_encode.h>

/* ------------------------------------------------------------- decoder --- */
/* The receiver's side of the wire format, kept here so a compression number
 * can never come from an encoder nobody checked. */

typedef struct {
    rfb_geom g;
    unsigned tiles_x, tiles_y;
    unsigned plane_bytes;
    unsigned char *fb;
} rfb_dec;

static unsigned rd16(const unsigned char *p) { return ((unsigned)p[0] << 8) | p[1]; }

static int dec_frame(rfb_dec *d, const unsigned char *in, unsigned n)
{
    const unsigned bpr = d->g.bytes_per_row;
    unsigned i = 4;
    unsigned char tile[RFB_MAX_TILE_W * RFB_MAX_TILE_H];

    if (n < 5 || in[0] != RFB_VERSION)
        return -1;

    for (;;) {
        unsigned op;
        if (i >= n) return -2;
        op = in[i++];
        if (op == RFB_OP_END)
            break;

        if (op == RFB_OP_COPY) {
            unsigned x0, w, y0, h, p, r;
            int dy;
            if (i + 10 > n) return -3;
            x0 = rd16(in + i); w = rd16(in + i + 2);
            y0 = rd16(in + i + 4); h = rd16(in + i + 6);
            dy = (int)(short)rd16(in + i + 8);
            i += 10;
            if (getenv("RFB_DEBUG"))
                fprintf(stderr, "copy x0=%u w=%u y0=%u h=%u dy=%d\n",
                        x0, w, y0, h, dy);
            if (x0 + w > bpr || y0 + h > d->g.height) return -4;
            for (p = 0; p < d->g.depth; p++) {
                unsigned char *pl = d->fb + (size_t)p * d->plane_bytes;
                if (dy > 0) {
                    for (r = 0; r < h; r++)
                        memmove(pl + (size_t)(y0 + r) * bpr + x0,
                                pl + (size_t)(y0 + r + (unsigned)dy) * bpr + x0, w);
                } else {
                    for (r = h; r > 0; r--) {
                        unsigned dr = y0 + r - 1;
                        memmove(pl + (size_t)dr * bpr + x0,
                                pl + (size_t)((int)dr + dy) * bpr + x0, w);
                    }
                }
            }
            continue;
        }

        if (op != RFB_OP_TILE)
            return -5;

        {
            unsigned idx, mask, tx, ty, x0, y0, tw, th, p, r;
            if (i + 3 > n) return -6;
            idx = rd16(in + i); mask = in[i + 2]; i += 3;
            if (idx >= d->tiles_x * d->tiles_y) return -7;
            tx = idx % d->tiles_x; ty = idx / d->tiles_x;
            x0 = tx * d->g.tile_w; y0 = ty * d->g.tile_h;
            tw = bpr - x0;   if (tw > d->g.tile_w) tw = d->g.tile_w;
            th = d->g.height - y0; if (th > d->g.tile_h) th = d->g.tile_h;

            for (p = 0; p < d->g.depth; p++) {
                unsigned code, len;
                unsigned char *pl;
                if (!((mask >> p) & 1u))
                    continue;
                if (i >= n) return -8;
                code = in[i++];
                if (code == RFB_CODE_RAW) {
                    len = tw * th;
                    if (i + len > n) return -9;
                    memcpy(tile, in + i, len);
                    i += len;
                } else {
                    long got;
                    if (i + 2 > n) return -10;
                    len = rd16(in + i); i += 2;
                    if (i + len > n) return -11;
                    got = rfb_unpackbits(in + i, len, tile, tw * th);
                    i += len;
                    if (got != (long)(tw * th)) return -12;
                }
                pl = d->fb + (size_t)p * d->plane_bytes;
                for (r = 0; r < th; r++) {
                    unsigned char *dst = pl + (size_t)(y0 + r) * bpr + x0;
                    const unsigned char *s = tile + (size_t)r * tw;
                    unsigned c;
                    if (code == RFB_CODE_PB_XOR)
                        for (c = 0; c < tw; c++) dst[c] ^= s[c];
                    else
                        memcpy(dst, s, tw);
                }
            }
        }
    }
    return (int)i;
}

/* ---------------------------------------------------------------- pfs ---- */

typedef struct {
    char name[64];
    rfb_geom g;
    unsigned frames;
    unsigned frame_bytes;
    unsigned char *data;
} pfs;

static int pfs_load(const char *path, pfs *s)
{
    FILE *f = fopen(path, "rb");
    unsigned char hdr[16];
    const char *base, *dot;
    size_t palette, want;

    if (!f) { perror(path); return -1; }
    if (fread(hdr, 1, 16, f) != 16 || memcmp(hdr, "PFS1", 4) != 0) {
        fprintf(stderr, "%s: not a PFS1 file\n", path); fclose(f); return -1;
    }
    memset(s, 0, sizeof(*s));
    s->g.width = (rfb_u16)rd16(hdr + 4);
    s->g.height = (rfb_u16)rd16(hdr + 6);
    s->g.depth = hdr[8];
    s->g.bytes_per_row = (rfb_u16)rd16(hdr + 10);
    s->frames = rd16(hdr + 12);
    if (s->g.depth < 1 || s->g.depth > RFB_MAX_DEPTH || s->g.bytes_per_row == 0) {
        fprintf(stderr, "%s: bad geometry\n", path); fclose(f); return -1;
    }
    palette = (size_t)3u << s->g.depth;
    if (fseek(f, (long)(16 + palette), SEEK_SET) != 0) { fclose(f); return -1; }

    s->frame_bytes = (unsigned)s->g.bytes_per_row * s->g.height * s->g.depth;
    want = (size_t)s->frame_bytes * s->frames;
    s->data = malloc(want ? want : 1);
    if (!s->data) { fclose(f); return -1; }
    if (fread(s->data, 1, want, f) != want) {
        fprintf(stderr, "%s: short read\n", path); fclose(f); return -1;
    }
    fclose(f);

    base = strrchr(path, '/');
    base = base ? base + 1 : path;
    dot = strrchr(base, '.');
    snprintf(s->name, sizeof(s->name), "%.*s",
             (int)(dot ? (size_t)(dot - base) : strlen(base)), base);
    return 0;
}

/* ------------------------------------------------------------ strategy --- */

/* busy and backoff are 0 for "leave the default"; they only matter to the
 * copy arms. */
typedef struct {
    const char *name;
    rfb_u32 flags;
    int is_raw;
    int busy;
    int backoff;
} strategy;

#define CP (RFB_F_BASELINE | RFB_F_COPYRECT)
#define CPA (CP | RFB_F_SCROLL_ADAPTIVE)

static const strategy STRATS[] = {
    { "raw",            0,                                        1, 0, 0 },
    { "tile",           0,                                        0, 0, 0 },
    { "tile_pb",        RFB_F_PACKBITS,                           0, 0, 0 },
    { "tile_xor_pb",    RFB_F_XOR,                                0, 0, 0 },
    { "baseline",       RFB_F_BASELINE,                           0, 0, 0 },
    { "no_planemask",   RFB_F_PACKBITS | RFB_F_XOR | RFB_F_BESTOF, 0, 0, 0 },
    { "no_bestof",      RFB_F_XOR | RFB_F_PLANEMASK,              0, 0, 0 },
    { "xor_only",       RFB_F_XOR | RFB_F_PLANEMASK | RFB_F_BESTOF, 0, 0, 0 },
    { "pbraw_only",     RFB_F_PACKBITS | RFB_F_PLANEMASK | RFB_F_BESTOF, 0, 0, 0 },
    { "rle2",           RFB_F_BASELINE | RFB_F_RLE2,              0, 0, 0 },
    { "copy",           CP,                                       0, 0, 0 },
    { "copy_adaptive",  CPA,                                      0, 0, 0 },
    { "copy_busy1",     CPA,                                      0, 1, 16 },
    { "copy_nobackoff", CPA,                                      0, 8, -1 },
    { "copy_b1_nobk",   CPA,                                      0, 1, -1 },
    { "copy_b1_bk4",    CPA,                                      0, 1, 4 },
};
#define NSTRAT ((int)(sizeof(STRATS) / sizeof(STRATS[0])))

static double now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e6 + (double)ts.tv_nsec / 1e3;
}

typedef struct {
    unsigned tile_w, tile_h;
} tiling;

static int run(const pfs *s, const strategy *st, tiling t, int reps)
{
    rfb_encoder e;
    rfb_dec d;
    rfb_scroll_cfg cfg;
    rfb_geom g = s->g;
    unsigned char *shadow, *scratch, *out, *decfb;
    unsigned i, rep;
    unsigned long total = 0, mx = 0;
    unsigned rt_ok = 0, rt_fail = 0;
    double t0, us;

    g.tile_w = (rfb_u8)t.tile_w;
    g.tile_h = (rfb_u8)t.tile_h;

    if (st->is_raw) {
        unsigned char *dst = malloc(s->frame_bytes);
        if (!dst) return -1;
        t0 = now_us();
        for (rep = 0; rep < (unsigned)reps; rep++)
            for (i = 0; i < s->frames; i++)
                memcpy(dst, s->data + (size_t)i * s->frame_bytes, s->frame_bytes);
        us = (now_us() - t0) / (double)(reps ? reps : 1);
        total = (unsigned long)s->frame_bytes * s->frames;
        mx = s->frame_bytes;
        rt_ok = s->frames;
        free(dst);
        printf("strat=%s seq=%s tile=%ux%u total=%lu mean=%.1f max=%lu "
               "ratio=%.3f us_per_frame=%.1f rt_ok=%u rt_fail=%u "
               "src_kb=%lu probe_kb=0 move_kb=0 dirty=0 sent=0 "
               "c_raw=0 c_pbraw=0 c_pbxor=0 copies=0\n",
               st->name, s->name, t.tile_w, t.tile_h, total,
               (double)total / (double)s->frames, mx, 1.0,
               us / (double)s->frames, rt_ok, rt_fail,
               (unsigned long)((unsigned long)s->frame_bytes * s->frames / 1024));
        return 0;
    }

    rfb_scroll_defaults(&cfg);
    if (st->busy)
        cfg.busy_tiles = (rfb_u16)st->busy;
    if (st->backoff)
        cfg.max_backoff = (rfb_u8)(st->backoff < 0 ? 0 : st->backoff);
    shadow = calloc(1, rfb_shadow_size(&g));
    scratch = calloc(1, rfb_scratch_size(&g, st->flags, &cfg));
    out = malloc(rfb_worst_case_frame(&g));
    decfb = calloc(1, rfb_shadow_size(&g));
    if (!shadow || !scratch || !out || !decfb) return -1;

    /* Timed loop: encode only, `reps` passes, shadow reset between passes. */
    t0 = now_us();
    for (rep = 0; rep < (unsigned)reps; rep++) {
        memset(shadow, 0, rfb_shadow_size(&g));
        if (rfb_encoder_init(&e, &g, st->flags, &cfg, shadow,
                             rfb_shadow_size(&g), scratch,
                             rfb_scratch_size(&g, st->flags, &cfg)) != 0) {
            fprintf(stderr, "init failed\n");
            return -1;
        }
        for (i = 0; i < s->frames; i++) {
            long n = rfb_encode_frame(&e, s->data + (size_t)i * s->frame_bytes,
                                      out, rfb_worst_case_frame(&g));
            if (n < 0) { fprintf(stderr, "encode error %ld\n", n); return -1; }
        }
    }
    us = (now_us() - t0) / (double)(reps ? reps : 1);

    /* Untimed pass: the same frames again, this time decoded and compared. */
    memset(shadow, 0, rfb_shadow_size(&g));
    memset(decfb, 0, rfb_shadow_size(&g));
    rfb_encoder_init(&e, &g, st->flags, &cfg, shadow, rfb_shadow_size(&g),
                     scratch, rfb_scratch_size(&g, st->flags, &cfg));
    memset(&d, 0, sizeof(d));
    d.g = g;
    d.tiles_x = e.tiles_x; d.tiles_y = e.tiles_y;
    d.plane_bytes = e.plane_bytes;
    d.fb = decfb;

    for (i = 0; i < s->frames; i++) {
        const unsigned char *src = s->data + (size_t)i * s->frame_bytes;
        long n = rfb_encode_frame(&e, src, out, rfb_worst_case_frame(&g));
        int used;
        if (n < 0) { fprintf(stderr, "encode error %ld\n", n); return -1; }
        total += (unsigned long)n;
        if ((unsigned long)n > mx) mx = (unsigned long)n;
        used = dec_frame(&d, out, (unsigned)n);
        if (used != (int)n || memcmp(decfb, src, s->frame_bytes) != 0)
            rt_fail++;
        else
            rt_ok++;
    }

    printf("strat=%s seq=%s tile=%ux%u total=%lu mean=%.1f max=%lu "
           "ratio=%.3f us_per_frame=%.1f rt_ok=%u rt_fail=%u "
           "src_kb=%lu probe_kb=%lu move_kb=%lu dirty=%lu sent=%lu "
           "c_raw=%lu c_pbraw=%lu c_pbxor=%lu copies=%lu\n",
           st->name, s->name, t.tile_w, t.tile_h, total,
           (double)total / (double)s->frames, mx,
           (double)((unsigned long)s->frame_bytes * s->frames) / (double)(total ? total : 1),
           us / (double)s->frames, rt_ok, rt_fail,
           (unsigned long)e.st.src_bytes / 1024, (unsigned long)e.st.probe_bytes / 1024,
           (unsigned long)e.st.shadow_move / 1024,
           (unsigned long)e.st.tiles_dirty, (unsigned long)e.st.planes_sent,
           (unsigned long)e.st.code_raw, (unsigned long)e.st.code_pb_raw,
           (unsigned long)e.st.code_pb_xor, (unsigned long)e.st.copies);

    free(shadow); free(scratch); free(out); free(decfb);
    return rt_fail ? 1 : 0;
}

/* Exact token match against a comma list, so --strat tile does not also select
 * tile_pb. */
static int selected(const char *list, const char *name)
{
    size_t n = strlen(name);
    const char *p = list;
    if (!list)
        return 1;
    while (*p) {
        const char *e = strchr(p, ',');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        if (len == n && memcmp(p, name, n) == 0)
            return 1;
        if (!e) break;
        p = e + 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    tiling tiles[16];
    int ntiles = 0, a, rc = 0, reps = 3;
    const char *strat_filter = NULL;

    for (a = 1; a < argc; a++) {
        if (strcmp(argv[a], "--tiles") == 0 && a + 1 < argc) {
            char *p = argv[++a];
            while (p && *p && ntiles < 16) {
                unsigned w, h;
                if (sscanf(p, "%ux%u", &w, &h) == 2) {
                    tiles[ntiles].tile_w = w; tiles[ntiles].tile_h = h; ntiles++;
                }
                p = strchr(p, ',');
                if (p) p++;
            }
        } else if (strcmp(argv[a], "--strat") == 0 && a + 1 < argc) {
            strat_filter = argv[++a];
        } else if (strcmp(argv[a], "--reps") == 0 && a + 1 < argc) {
            reps = atoi(argv[++a]);
        } else {
            break;
        }
    }
    if (ntiles == 0) { tiles[0].tile_w = 16; tiles[0].tile_h = 8; ntiles = 1; }

    for (; a < argc; a++) {
        pfs s;
        int ti, si;
        if (pfs_load(argv[a], &s) != 0) { rc = 1; continue; }
        printf("seq=%s file=%s w=%u h=%u depth=%u bpr=%u frames=%u "
               "frame_bytes=%u\n",
               s.name, argv[a], s.g.width, s.g.height, s.g.depth,
               s.g.bytes_per_row, s.frames, s.frame_bytes);
        for (ti = 0; ti < ntiles; ti++) {
            for (si = 0; si < NSTRAT; si++) {
                if (!selected(strat_filter, STRATS[si].name))
                    continue;
                if (STRATS[si].is_raw && ti > 0)
                    continue;
                if (run(&s, &STRATS[si], tiles[ti], reps) != 0)
                    rc = 1;
            }
        }
        free(s.data);
    }
    return rc;
}

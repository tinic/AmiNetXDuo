/* Host measurement CLI for the remote-framebuffer encoder.
 *
 *   rfbbench [--tiles WxH,...] [--strat name,...] [--csv] file.pfs ...
 *
 * Reads .pfs captures, runs every strategy over every sequence, decodes each
 * encoded frame back with the reference decoder below and compares it with the
 * frame that went in.  Output is key=value, one line per (sequence, strategy).
 *
 * Host-only and loose on purpose.  The encoder next door is the portable half.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <aminetxduo/rfb_encode.h>

/* --deflate measures what an entropy coder on top of the encoder output gains
 * and what it costs, so the case for its exclusion rests on a measurement.
 * Host-only.  Nothing in rfb_encode.c knows about zlib. */
#ifdef RFBBENCH_ZLIB
#include <zlib.h>
#endif

/* ------------------------------------------------------------- decoder --- */
/* The receiver side of the wire format, kept here so that a compression number
 * can never come from an unchecked encoder. */

typedef struct {
    rfb_geom g;
    unsigned tiles_x, tiles_y;
    unsigned plane_bytes;
    unsigned row_stride;     /* bytes_per_row, or * depth when interleaved */
    unsigned plane_stride;
    unsigned char *fb;
} rfb_dec;

static unsigned rd16(const unsigned char *p) { return ((unsigned)p[0] << 8) | p[1]; }

static int dec_frame(rfb_dec *d, const unsigned char *in, unsigned n)
{
    const unsigned bpr = d->g.bytes_per_row;
    const unsigned rs = d->row_stride;
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
            for (p = 0; p < rfb_planes(&d->g); p++) {
                unsigned char *pl = d->fb + (size_t)p * d->plane_stride;
                if (dy > 0) {
                    for (r = 0; r < h; r++)
                        memmove(pl + (size_t)(y0 + r) * rs + x0,
                                pl + (size_t)(y0 + r + (unsigned)dy) * rs + x0, w);
                } else {
                    for (r = h; r > 0; r--) {
                        unsigned dr = y0 + r - 1;
                        memmove(pl + (size_t)dr * rs + x0,
                                pl + (size_t)((int)dr + dy) * rs + x0, w);
                    }
                }
            }
            continue;
        }

        /* One op or the other, and the geometry decides which: a chunky frame
         * carries no plane mask, because there is one plane. */
        if (op != RFB_OP_TILE && op != RFB_OP_TILE8)
            return -5;
        if ((op == RFB_OP_TILE8) != RFB_FMT_IS_CHUNKY(d->g.format))
            return -13;

        {
            unsigned idx, mask, tx, ty, x0, y0, tw, th, p, r;
            unsigned chunky = (op == RFB_OP_TILE8) ? 1u : 0u;
            if (i + (chunky ? 2u : 3u) > n) return -6;
            idx = rd16(in + i);
            mask = chunky ? 1u : in[i + 2];
            i += chunky ? 2u : 3u;
            if (idx >= d->tiles_x * d->tiles_y) return -7;
            tx = idx % d->tiles_x; ty = idx / d->tiles_x;
            x0 = tx * d->g.tile_w; y0 = ty * d->g.tile_h;
            tw = bpr - x0;   if (tw > d->g.tile_w) tw = d->g.tile_w;
            th = d->g.height - y0; if (th > d->g.tile_h) th = d->g.tile_h;

            for (p = 0; p < rfb_planes(&d->g); p++) {
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
                pl = d->fb + (size_t)p * d->plane_stride;
                for (r = 0; r < th; r++) {
                    unsigned char *dst = pl + (size_t)(y0 + r) * rs + x0;
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
    if (fread(hdr, 1, 16, f) != 16 || memcmp(hdr, "PFS2", 4) != 0) {
        fprintf(stderr, "%s: not a PFS2 file\n", path); fclose(f); return -1;
    }
    memset(s, 0, sizeof(*s));
    s->g.width = (rfb_u16)rd16(hdr + 4);
    s->g.height = (rfb_u16)rd16(hdr + 6);
    s->g.depth = hdr[8];
    /* Byte 9 is rfb_geom.format, which is what it has always been: the two
       values a writer has ever put there, 0 and 1, are planar and CLUT8. */
    s->g.format = hdr[9];
    s->g.bytes_per_row = (rfb_u16)rd16(hdr + 10);
    s->frames = rd16(hdr + 12);
    /* Which depths go with which format is the encoder's rule and it knows
       all three, so it is asked rather than copied here where the copy can
       drift.  The tile size is not in the file -- the sweep sets it per run --
       so a legal one is filled in to ask about the fields that are. */
    {
        rfb_geom probe = s->g;
        probe.tile_w = 16;
        probe.tile_h = 16;
        if (rfb_shadow_size(&probe) == 0) {
            fprintf(stderr, "%s: bad geometry, format %u depth %u bpr %u\n",
                    path, s->g.format, s->g.depth, s->g.bytes_per_row);
            fclose(f); return -1;
        }
    }
    /* Not 3 << depth: a truecolour capture carries no palette and its depth
       is bits a pixel. */
    palette = (size_t)3u * rfb_pal_colours(&s->g);
    if (fseek(f, (long)(16 + palette), SEEK_SET) != 0) { fclose(f); return -1; }

    s->frame_bytes = (unsigned)s->g.bytes_per_row * s->g.height *
                     rfb_planes(&s->g);
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

/* busy and backoff are 0 for "leave the default".  They only matter to the
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

static int g_deflate;
static int g_interleaved;

/* --bands N encodes each screen pass as N messages instead of one, which is
   what the Amiga does when it has to service its socket between them.  1 is
   the whole frame and is the default. */
static int g_bands = 1;

/* The measured wire rate that the frame-rate column divides by. */
#define WIRE_BYTES_PER_SEC 407552.0   /* 398 KB/s */

/* Rearrange a plane-major sequence into the BMF_INTERLEAVED layout, so the
 * stride path of the encoder is round-tripped and not assumed. */
static int pfs_interleave(pfs *s)
{
    unsigned char *tmp = malloc(s->frame_bytes);
    unsigned i, p, y;
    unsigned bpr = s->g.bytes_per_row, h = s->g.height, d = s->g.depth;
    if (!tmp) return -1;
    for (i = 0; i < s->frames; i++) {
        unsigned char *f = s->data + (size_t)i * s->frame_bytes;
        memcpy(tmp, f, s->frame_bytes);
        for (p = 0; p < d; p++)
            for (y = 0; y < h; y++)
                memcpy(f + ((size_t)y * d + p) * bpr,
                       tmp + ((size_t)p * h + y) * bpr, bpr);
    }
    free(tmp);
    return 0;
}

#ifdef RFBBENCH_ZLIB
/* One deflate stream for the whole sequence, which is the best case for it:
 * the dictionary carries across frames.  Reported beside the PackBits numbers
 * of the same sequence. */
typedef struct {
    z_stream z;
    unsigned char *out;
    unsigned long total;
    double us;
    int level;
    int live;
} dfl;

static void dfl_start(dfl *d, int level, unsigned cap)
{
    memset(d, 0, sizeof(*d));
    d->level = level;
    d->out = malloc(cap + 4096);
    if (!d->out) return;
    if (deflateInit(&d->z, level) != Z_OK) { free(d->out); d->out = NULL; return; }
    d->live = 1;
}

static void dfl_frame(dfl *d, const unsigned char *in, unsigned n, unsigned cap)
{
    double t0;
    if (!d->live) return;
    d->z.next_in = (Bytef *)in;
    d->z.avail_in = n;
    d->z.next_out = d->out;
    d->z.avail_out = cap + 4096;
    t0 = now_us();
    deflate(&d->z, Z_SYNC_FLUSH);
    d->us += now_us() - t0;
    d->total += (unsigned long)(cap + 4096 - d->z.avail_out);
}

static void dfl_end(dfl *d)
{
    if (!d->live) return;
    deflateEnd(&d->z);
    free(d->out);
    d->live = 0;
}
#endif

/* One screen pass: whole, or g_bands messages, which is what the Amiga sends
 * when it services its socket between them.  d NULL = timed loop, no decode;
 * bmax non-NULL collects the slowest single band, the quantum a keystroke
 * waits behind. */
static long encode_pass(rfb_encoder *e, const unsigned char *src,
                        unsigned char *out, rfb_dec *d, int *bad, double *bmax)
{
    const unsigned cap = rfb_worst_case_frame(&e->g);
    const rfb_u8 *planes[RFB_MAX_DEPTH];
    unsigned p, b;
    rfb_u16 rows;
    long total = 0;
    double t0, dt;

    if (g_bands <= 1) {
        long n;
        t0 = now_us();
        n = rfb_encode_frame(e, src, out, cap);
        dt = now_us() - t0;
        if (bmax && dt > *bmax) *bmax = dt;
        if (n < 0) return n;
        if (d && dec_frame(d, out, (unsigned)n) != (int)n) *bad = 1;
        return n;
    }

    for (p = 0; p < rfb_planes(&e->g); p++)
        planes[p] = src + (size_t)p * e->plane_stride;
    rows = (rfb_u16)((e->tiles_y + g_bands - 1) / g_bands);
    if (rows == 0)
        rows = 1;
    for (b = 0; b * rows < e->tiles_y; b++) {
        rfb_u16 ty0 = (rfb_u16)(b * rows);
        rfb_u16 ty1 = (rfb_u16)(ty0 + rows);
        long bn;
        if (ty1 > e->tiles_y)
            ty1 = e->tiles_y;
        t0 = now_us();
        bn = rfb_encode_band(e, planes, out, cap, ty0, ty1);
        dt = now_us() - t0;
        if (bmax && dt > *bmax) *bmax = dt;
        if (bn < 0) return bn;
        if (d && dec_frame(d, out, (unsigned)bn) != (int)bn) *bad = 1;
        total += bn;
    }
    return total;
}

static int run(const pfs *s, const strategy *st, tiling t, int reps)
{
    rfb_encoder e;
    rfb_dec d;
    rfb_scroll_cfg cfg;
    rfb_geom g = s->g;
    unsigned char *shadow, *scratch, *out, *decfb;
    unsigned i, rep;
    unsigned long total = 0, mx = 0, f0 = 0, mx_after = 0;
    unsigned rt_ok = 0, rt_fail = 0;
    double t0, us, band_us_max = 0.0;

    rfb_u32 flags = st->flags | (g_interleaved ? RFB_F_INTERLEAVED : 0u);

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
        printf("strat=%s seq=%s tile=%ux%u total=%lu mean=%.1f mean_after_f0=%.1f "
               "f0=%lu max=%lu max_after_f0=%lu fps_at_398k=%.1f "
               "ratio=%.3f us_per_frame=%.1f rt_ok=%u rt_fail=%u "
               "src_kb=%lu probe_kb=0 move_kb=0 dirty=0 sent=0 "
               "c_raw=0 c_pbraw=0 c_pbxor=0 copies=0 "
               "bands=1 band_us_max=0.0\n",
               st->name, s->name, t.tile_w, t.tile_h, total,
               (double)total / (double)s->frames, (double)s->frame_bytes,
               (unsigned long)s->frame_bytes, mx, mx,
               WIRE_BYTES_PER_SEC / (double)s->frame_bytes, 1.0,
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
    scratch = calloc(1, rfb_scratch_size(&g, flags, &cfg));
    out = malloc(rfb_worst_case_frame(&g));
    decfb = calloc(1, rfb_shadow_size(&g));
    if (!shadow || !scratch || !out || !decfb) return -1;

    /* Timed loop: encode only, `reps` passes, shadow reset between passes.
     * Banded when --bands says so -- what is timed has to be what ships. */
    t0 = now_us();
    for (rep = 0; rep < (unsigned)reps; rep++) {
        memset(shadow, 0, rfb_shadow_size(&g));
        if (rfb_encoder_init(&e, &g, flags, &cfg, shadow,
                             rfb_shadow_size(&g), scratch,
                             rfb_scratch_size(&g, flags, &cfg)) != 0) {
            fprintf(stderr, "init failed\n");
            return -1;
        }
        for (i = 0; i < s->frames; i++) {
            long n = encode_pass(&e, s->data + (size_t)i * s->frame_bytes,
                                 out, NULL, NULL, &band_us_max);
            if (n < 0) { fprintf(stderr, "encode error %ld\n", n); return -1; }
        }
    }
    us = (now_us() - t0) / (double)(reps ? reps : 1);

    /* Untimed pass: the same frames again, this time decoded and compared. */
    memset(shadow, 0, rfb_shadow_size(&g));
    memset(decfb, 0, rfb_shadow_size(&g));
    rfb_encoder_init(&e, &g, flags, &cfg, shadow, rfb_shadow_size(&g),
                     scratch, rfb_scratch_size(&g, flags, &cfg));
    memset(&d, 0, sizeof(d));
    d.g = g;
    d.tiles_x = e.tiles_x; d.tiles_y = e.tiles_y;
    d.plane_bytes = e.plane_bytes;
    d.row_stride = e.row_stride;
    d.plane_stride = e.plane_stride;
    d.fb = decfb;

#ifdef RFBBENCH_ZLIB
    dfl d1, d6;
    if (g_deflate) {
        dfl_start(&d1, 1, rfb_worst_case_frame(&g));
        dfl_start(&d6, 6, rfb_worst_case_frame(&g));
    }
#endif

    for (i = 0; i < s->frames; i++) {
        const unsigned char *src = s->data + (size_t)i * s->frame_bytes;
        long n;
        int bad = 0;

        /* Each band decoded as it is produced: a band that got its tile
         * indices, its shadow or its clipped bottom row wrong shows up here as
         * a decode that does not match the source.  The bytes are counted
         * together, because the screen pass costs a frame's worth of wire
         * whether it went in one message or five. */
        n = encode_pass(&e, src, out, &d, &bad, NULL);
        if (n < 0) { fprintf(stderr, "encode error %ld\n", n); return -1; }

        total += (unsigned long)n;
        if ((unsigned long)n > mx) mx = (unsigned long)n;
        if (i == 0)
            f0 = (unsigned long)n;
        else if ((unsigned long)n > mx_after)
            mx_after = (unsigned long)n;
        if (bad || memcmp(decfb, src, s->frame_bytes) != 0)
            rt_fail++;
        else
            rt_ok++;
#ifdef RFBBENCH_ZLIB
        if (g_deflate) {
            dfl_frame(&d1, out, (unsigned)n, rfb_worst_case_frame(&g));
            dfl_frame(&d6, out, (unsigned)n, rfb_worst_case_frame(&g));
        }
#endif
    }

    {
    double mean_after = s->frames > 1
        ? (double)(total - f0) / (double)(s->frames - 1) : (double)total;
    printf("strat=%s seq=%s tile=%ux%u total=%lu mean=%.1f mean_after_f0=%.1f "
           "f0=%lu max=%lu max_after_f0=%lu fps_at_398k=%.0f "
           "ratio=%.3f us_per_frame=%.1f rt_ok=%u rt_fail=%u "
           "src_kb=%lu probe_kb=%lu move_kb=%lu dirty=%lu sent=%lu "
           "c_raw=%lu c_pbraw=%lu c_pbxor=%lu copies=%lu "
           "bands=%d band_us_max=%.1f\n",
           st->name, s->name, t.tile_w, t.tile_h, total,
           (double)total / (double)s->frames, mean_after, f0, mx, mx_after,
           mean_after > 0.0 ? WIRE_BYTES_PER_SEC / mean_after : 99999.0,
           (double)((unsigned long)s->frame_bytes * s->frames) / (double)(total ? total : 1),
           us / (double)s->frames, rt_ok, rt_fail,
           (unsigned long)e.st.src_bytes / 1024, (unsigned long)e.st.probe_bytes / 1024,
           (unsigned long)e.st.shadow_move / 1024,
           (unsigned long)e.st.tiles_dirty, (unsigned long)e.st.planes_sent,
           (unsigned long)e.st.code_raw, (unsigned long)e.st.code_pb_raw,
           (unsigned long)e.st.code_pb_xor, (unsigned long)e.st.copies,
           g_bands, band_us_max);
    }

#ifdef RFBBENCH_ZLIB
    if (g_deflate) {
        printf("deflate seq=%s strat=%s tile=%ux%u packbits_total=%lu "
               "z1_total=%lu z1_gain=%.3f z1_us_per_frame=%.1f "
               "z6_total=%lu z6_gain=%.3f z6_us_per_frame=%.1f "
               "encode_us_per_frame=%.1f\n",
               s->name, st->name, t.tile_w, t.tile_h, total,
               d1.total, (double)total / (double)(d1.total ? d1.total : 1),
               d1.us / (double)s->frames,
               d6.total, (double)total / (double)(d6.total ? d6.total : 1),
               d6.us / (double)s->frames,
               us / (double)s->frames);
        dfl_end(&d1); dfl_end(&d6);
    }
#endif

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
        } else if (strcmp(argv[a], "--interleaved") == 0) {
            g_interleaved = 1;
        } else if (strcmp(argv[a], "--deflate") == 0) {
            g_deflate = 1;
        } else if (strcmp(argv[a], "--reps") == 0 && a + 1 < argc) {
            reps = atoi(argv[++a]);
        } else if (strcmp(argv[a], "--bands") == 0 && a + 1 < argc) {
            g_bands = atoi(argv[++a]);
        } else {
            break;
        }
    }
    if (ntiles == 0) { tiles[0].tile_w = 16; tiles[0].tile_h = 8; ntiles = 1; }

    for (; a < argc; a++) {
        pfs s;
        int ti, si;
        if (pfs_load(argv[a], &s) != 0) { rc = 1; continue; }
        /* BMF_INTERLEAVED is a statement about eight planes and a chunky
           source has one, so the interleaved pass of the sweep skips those
           files. */
        if (g_interleaved && RFB_FMT_IS_CHUNKY(s.g.format)) { free(s.data); continue; }
        if (g_interleaved && pfs_interleave(&s) != 0) { rc = 1; continue; }
        printf("seq=%s file=%s w=%u h=%u depth=%u bpr=%u fmt=%u frames=%u "
               "frame_bytes=%u\n",
               s.name, argv[a], s.g.width, s.g.height, s.g.depth,
               s.g.bytes_per_row, s.g.format, s.frames, s.frame_bytes);
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

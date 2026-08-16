/* Frame encoder for the remote framebuffer.  Wire format is in the header.
 *
 * The shape of the thing: one pass over the source bitmap, tile by tile, and
 * the pass asks before it writes.  Each tile-plane is compared against the
 * shadow copy of the previous frame and stops at the first word that
 * disagrees; a tile nobody drew on is never written to at all.  That is the
 * frame a live Workbench mostly serves -- 49 out of 50 of them change nothing.
 *
 * A tile that DID change is read once, into rawbuf, and that single copy is
 * what the XOR is taken from, what the shadow is written from, and what goes
 * on the wire.  The source is never read twice, which is what lets the Amiga
 * caller point this at the live bitplanes with no drawing lock held: a torn
 * read can produce a torn frame, and always could, but it cannot leave a
 * shadow here that disagrees with the bytes the far end was sent.
 *
 * On the Amiga the source is chip RAM and the shadow is fast, so the compare
 * is the expensive half and everything is arranged to do less of it.  The
 * scroll probe is the one thing that reads the source separately, and it is
 * priced separately in rfb_stats.probe_bytes.
 *
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include <aminetxduo/rfb_encode.h>

/* rfb_u32 has to be exactly 32 bits: the word loop below and the wire format
 * both depend on it. */
typedef char rfb_u32_is_four_bytes[(sizeof(rfb_u32) == 4) ? 1 : -1];

#define RFB_PB_FAIL     ((rfb_u32)0xFFFFFFFFu)
#define RFB_PB_BOUND(n) ((n) + ((n) + 127u) / 128u + 1u)

/* Sampled rows the scroll probe will keep masks for.  A taller screen raises
 * the effective probe_step rather than the scratch bill. */
#define RFB_PROBE_MAX_SAMPLES 256
#define RFB_PROBE_MAX_BLK     32

/* And a FLOOR on how narrow a probe column may be.  Without one the sizing
 * below divides the row into RFB_PROBE_MAX_BLK columns whatever the row is,
 * and an 80-byte Workbench row came out at four bytes a column: twenty
 * columns, each of them a function's worth of setup to compare one longword.
 * Sixteen bytes is 128 pixels, which is finer than any window edge the band
 * detector can act on -- the copy rectangle it produces is a prediction the
 * tile pass corrects anyway. */
#define RFB_PROBE_MIN_BLK     16

/* ------------------------------------------------------------- PackBits --- */

rfb_u32 rfb_packbits(const rfb_u8 *in, rfb_u32 n, rfb_u8 *out, rfb_u32 cap,
                     int min_run)
{
    rfb_u32 i = 0, o = 0;

    if (min_run < 2)
        min_run = 2;

    while (i < n) {
        rfb_u32 run = 1;
        rfb_u8 b = in[i];

        while (i + run < n && in[i + run] == b && run < 128u)
            run++;

        if (run >= (rfb_u32)min_run) {
            if (o + 2u > cap)
                return RFB_PB_FAIL;
            out[o++] = (rfb_u8)(257u - run);
            out[o++] = b;
            i += run;
        } else {
            rfb_u32 start = i, lit = 0;
            /* A literal stops where a codeable run starts.  min_run is 2 or 3;
             * either way the first byte here cannot start one, because the run
             * scan above already said it does not. */
            while (i < n && lit < 128u) {
                if (min_run == 2) {
                    if (i + 1u < n && in[i] == in[i + 1u] && lit > 0)
                        break;
                } else {
                    if (i + 2u < n && in[i] == in[i + 1u] && in[i] == in[i + 2u])
                        break;
                }
                i++;
                lit++;
            }
            if (o + 1u + lit > cap)
                return RFB_PB_FAIL;
            out[o++] = (rfb_u8)(lit - 1u);
            memcpy(out + o, in + start, (size_t)lit);
            o += lit;
        }
    }
    return o;
}

long rfb_unpackbits(const rfb_u8 *in, rfb_u32 n, rfb_u8 *out, rfb_u32 out_n)
{
    rfb_u32 i = 0, o = 0;

    while (i < n) {
        rfb_u8 c = in[i++];
        if (c < 128u) {
            rfb_u32 lit = (rfb_u32)c + 1u;
            if (i + lit > n || o + lit > out_n)
                return RFB_E_OVERFLOW;
            memcpy(out + o, in + i, (size_t)lit);
            i += lit;
            o += lit;
        } else if (c > 128u) {
            rfb_u32 run = 257u - (rfb_u32)c;
            if (i >= n || o + run > out_n)
                return RFB_E_OVERFLOW;
            memset(out + o, in[i++], (size_t)run);
            o += run;
        }
    }
    return (long)o;
}

/* ----------------------------------------------------------- geometry ---- */

static int rfb_geom_ok(const rfb_geom *g)
{
    if (!g || g->depth < 1 || g->depth > RFB_MAX_DEPTH)
        return 0;
    if (g->height == 0 || g->bytes_per_row == 0)
        return 0;
    if (g->tile_w < 1 || g->tile_w > RFB_MAX_TILE_W)
        return 0;
    if (g->tile_h < 1 || g->tile_h > RFB_MAX_TILE_H)
        return 0;
    if (g->format != RFB_FMT_PLANAR && g->format != RFB_FMT_CLUT8)
        return 0;
    /* A chunky byte IS the palette index, so the depth is the palette's and
     * not a plane count -- and eight bits is the only width of it this
     * format has.  Refusing here rather than deriving a depth means a caller
     * that gets it wrong is told so at init instead of sending a picture
     * against a palette of the wrong length. */
    if (g->format == RFB_FMT_CLUT8 && g->depth != 8)
        return 0;
    return 1;
}

rfb_u8 rfb_planes(const rfb_geom *g)
{
    if (!g)
        return 0;
    return (rfb_u8)((g->format == RFB_FMT_CLUT8) ? 1u : (rfb_u32)g->depth);
}

rfb_u32 rfb_shadow_size(const rfb_geom *g)
{
    if (!rfb_geom_ok(g))
        return 0;
    return (rfb_u32)g->bytes_per_row * g->height * rfb_planes(g);
}

static rfb_u32 rfb_probe_step(const rfb_geom *g, const rfb_scroll_cfg *cfg)
{
    rfb_u32 step = cfg->probe_step ? cfg->probe_step : 1u;
    while ((rfb_u32)g->height / step > RFB_PROBE_MAX_SAMPLES)
        step++;
    return step;
}

rfb_u32 rfb_scratch_size(const rfb_geom *g, rfb_u32 flags,
                         const rfb_scroll_cfg *cfg)
{
    rfb_scroll_cfg def;
    rfb_u32 tb, need;

    if (!rfb_geom_ok(g))
        return 0;
    if (!cfg) {
        rfb_scroll_defaults(&def);
        cfg = &def;
    }
    tb = (rfb_u32)g->tile_w * g->tile_h;
    need = tb * rfb_planes(g)          /* xorbuf, one tile per plane */
         + tb * rfb_planes(g)          /* rawbuf, likewise */
         + 2u * RFB_PB_BOUND(tb)       /* two candidate PackBits outputs */
         + 4u * 4u;                    /* alignment slack */

    if (flags & RFB_F_COPYRECT) {
        rfb_u32 step = rfb_probe_step(g, cfg);
        rfb_u32 samples = (rfb_u32)g->height / step + 1u;
        rfb_u32 ncand = cfg->n_cand ? cfg->n_cand : 1u;
        need += g->bytes_per_row + samples * (ncand + 1u) * 4u + 8u;
    }
    return need;
}

rfb_u32 rfb_worst_case_frame(const rfb_geom *g)
{
    rfb_u32 tx, ty, tiles, tb, head;

    if (!rfb_geom_ok(g))
        return 0;
    tx = ((rfb_u32)g->bytes_per_row + g->tile_w - 1u) / g->tile_w;
    ty = ((rfb_u32)g->height + g->tile_h - 1u) / g->tile_h;
    tiles = tx * ty;
    tb = (rfb_u32)g->tile_w * g->tile_h;

    /* What a tile op costs before its first plane: op and index either way,
     * and the plane mask on top of that when there are planes to mask. */
    head = (g->format == RFB_FMT_CLUT8) ? 3u : 4u;

    /* Header, one copy op, every tile carrying every plane at the PackBits
     * expansion bound, and the terminator. */
    return 4u + 11u
         + tiles * (head + (rfb_u32)rfb_planes(g) * (3u + RFB_PB_BOUND(tb)))
         + 1u;
}

void rfb_scroll_defaults(rfb_scroll_cfg *cfg)
{
    static const rfb_s16 def[] = { 8, 16, 24, 32, 4, 2, 1, -8, -16 };
    unsigned i;

    if (!cfg)
        return;
    memset(cfg, 0, sizeof(*cfg));
    cfg->probe_step = 4;
    cfg->match_pct  = 75;
    cfg->min_band   = 32;
    cfg->busy_tiles = 8;
    cfg->max_backoff = 16;
    cfg->n_cand     = (rfb_u8)(sizeof(def) / sizeof(def[0]));
    for (i = 0; i < sizeof(def) / sizeof(def[0]); i++)
        cfg->cand[i] = def[i];
}

/* ---------------------------------------------------------------- init --- */

static rfb_u8 *rfb_align4(rfb_u8 *p)
{
    while (((unsigned long)p & 3ul) != 0ul)
        p++;
    return p;
}

long rfb_encoder_init(rfb_encoder *e, const rfb_geom *g, rfb_u32 flags,
                      const rfb_scroll_cfg *cfg,
                      rfb_u8 *shadow, rfb_u32 shadow_len,
                      rfb_u8 *scratch, rfb_u32 scratch_len)
{
    rfb_u32 tb;
    rfb_u8 *p;

    if (!e || !rfb_geom_ok(g) || !shadow || !scratch)
        return RFB_E_GEOM;

    /* A chunky source has one plane, and BMF_INTERLEAVED is a statement about
     * how eight of them are laid out.  Taking it would compute a row stride of
     * bytes_per_row * 8 and read one row in eight. */
    if (g->format == RFB_FMT_CLUT8 && (flags & RFB_F_INTERLEAVED))
        return RFB_E_GEOM;

    memset(e, 0, sizeof(*e));
    e->g = *g;
    e->flags = flags;
    e->nplanes = rfb_planes(g);
    if (cfg)
        e->scroll = *cfg;
    else
        rfb_scroll_defaults(&e->scroll);
    if (e->scroll.n_cand > RFB_MAX_CAND)
        e->scroll.n_cand = RFB_MAX_CAND;

    if (shadow_len < rfb_shadow_size(g))
        return RFB_E_BUFFER;
    if (scratch_len < rfb_scratch_size(g, flags, &e->scroll))
        return RFB_E_BUFFER;

    e->tiles_x = (rfb_u16)(((rfb_u32)g->bytes_per_row + g->tile_w - 1u) / g->tile_w);
    e->tiles_y = (rfb_u16)(((rfb_u32)g->height + g->tile_h - 1u) / g->tile_h);
    e->plane_bytes = (rfb_u32)g->bytes_per_row * g->height;
    e->frame_bytes = e->plane_bytes * e->nplanes;
    if (flags & RFB_F_INTERLEAVED) {
        e->row_stride = (rfb_u32)g->bytes_per_row * g->depth;
        e->plane_stride = g->bytes_per_row;
    } else {
        e->row_stride = g->bytes_per_row;
        e->plane_stride = e->plane_bytes;
    }
    e->shadow = shadow;
    e->scratch = scratch;
    e->scratch_len = scratch_len;
    /* Nothing has scrolled yet, and memset() left this saying "one frame ago".
       A session that never scrolls should be strict about the probe from its
       first busy frame, not from its sixty-fifth. */
    e->since_copy = 255u;

    tb = (rfb_u32)g->tile_w * g->tile_h;
    p = rfb_align4(scratch);
    e->xorbuf = p; p = rfb_align4(p + tb * e->nplanes);
    e->rawbuf = p; p = rfb_align4(p + tb * e->nplanes);
    e->pb_a   = p; p = rfb_align4(p + RFB_PB_BOUND(tb));
    e->pb_b   = p; p = rfb_align4(p + RFB_PB_BOUND(tb));

    if (flags & RFB_F_COPYRECT) {
        e->probe_step = (rfb_u16)rfb_probe_step(g, &e->scroll);
        e->n_samples  = (rfb_u16)((rfb_u32)g->height / e->probe_step + 1u);
        /* Rounded up to a multiple of four so rfb_blk_match() can compare
         * whole longwords. */
        e->blk_bytes  = (rfb_u16)((((rfb_u32)g->bytes_per_row
                                    + RFB_PROBE_MAX_BLK - 1u)
                                   / RFB_PROBE_MAX_BLK + 3u) & ~3u);
        if (e->blk_bytes < RFB_PROBE_MIN_BLK)
            e->blk_bytes = RFB_PROBE_MIN_BLK;
        e->n_blk = (rfb_u16)(((rfb_u32)g->bytes_per_row + e->blk_bytes - 1u)
                             / e->blk_bytes);
        e->probe_row = p; p = rfb_align4(p + g->bytes_per_row);
        e->probe_mask = (rfb_u32 *)(void *)p;
    }
    return 0;
}

/* ------------------------------------------------------------- output ---- */

typedef struct {
    rfb_u8 *buf;
    rfb_u32 cap;
    rfb_u32 len;
    int     over;
} rfb_out;

static void rfb_put8(rfb_out *o, rfb_u8 v)
{
    if (o->len + 1u > o->cap) { o->over = 1; return; }
    o->buf[o->len++] = v;
}

static void rfb_put16(rfb_out *o, rfb_u32 v)
{
    if (o->len + 2u > o->cap) { o->over = 1; return; }
    o->buf[o->len++] = (rfb_u8)(v >> 8);
    o->buf[o->len++] = (rfb_u8)(v & 0xFFu);
}

static void rfb_putblk(rfb_out *o, const rfb_u8 *src, rfb_u32 n)
{
    if (o->len + n > o->cap) { o->over = 1; return; }
    memcpy(o->buf + o->len, src, (size_t)n);
    o->len += n;
}

/* ------------------------------------------------------- the tile pass --- */

/* DOES THIS TILE-PLANE DIFFER FROM THE SHADOW.  Nothing is written and it
 * stops at the first word that disagrees, so the answer for a screen where
 * nothing moved costs one read of the source and one of the shadow and no
 * more.  That is the whole idle frame: on a live Workbench 49 frames out of
 * 50 reach no further than this.
 *
 * `word` is the caller's frame-constant verdict on alignment -- see
 * rfb_words_ok().  It is not rederived here because it was the same answer on
 * every row of every tile of every plane, 2560 times a frame. */
static int rfb_cmp_plane(const rfb_u8 *src, const rfb_u8 *sh,
                         rfb_u32 bpr, rfb_u32 tw, rfb_u32 th, int word)
{
    rfb_u32 r = th;

    if (word) {
        const rfb_u32 words = tw >> 2;
        const rfb_u32 tail = tw & 3u;

        /* The rows are walked by adding the stride, not by multiplying the row
         * number by it: bpr is a variable, so `src + r * bpr` on the -m68000
         * build the archive ships is a call to __mulsi3, once per row of every
         * tile of every plane. */
        do {
            rfb_u32 n = words;
            const rfb_u32 *sw = (const rfb_u32 *)(const void *)src;
            const rfb_u32 *dw = (const rfb_u32 *)(const void *)sh;

            /* ONE LONGWORD AT A TIME, AND THAT IS THE FAST ONE.  Unrolling
             * this was tried twice and lost both times, measured on the idle
             * frame at depth 2: 25.3 ms like this, 30.7 ms unrolled four wide,
             * 31.9 ms unrolled four wide and out of line.  A tile row is four
             * longwords, so an unrolled body pays its setup once per four
             * iterations and saves three loop tests, and CMPM.L with a
             * predictable branch is already about as cheap as the 68020 gets.
             * Do not unroll this without measuring it. */
            while (n--)
                if (*sw++ != *dw++)
                    return 1;

            if (tail) {
                const rfb_u8 *s = (const rfb_u8 *)(const void *)sw;
                const rfb_u8 *d = (const rfb_u8 *)(const void *)dw;
                rfb_u32 c = tail;
                while (c--)
                    if (*s++ != *d++)
                        return 1;
            }
            src += bpr; sh += bpr;
        } while (--r);
        return 0;
    }

    do {
        rfb_u32 c = tw;
        const rfb_u8 *s = src, *d = sh;
        while (c--)
            if (*s++ != *d++)
                return 1;
        src += bpr; sh += bpr;
    } while (--r);
    return 0;
}

/* TAKE THE TILE-PLANE.  One read of the source into raw, which is then the
 * only copy anybody uses: the XOR is computed from it against the old shadow,
 * the shadow is written from it, and it is what goes on the wire.  The source
 * is never read twice, so a screen being drawn on underneath cannot put a
 * shadow on this end that disagrees with the bytes the far end was sent. */
static void rfb_take_plane(const rfb_u8 *src, rfb_u8 *sh, rfb_u8 *raw,
                           rfb_u8 *xb, rfb_u32 bpr, rfb_u32 tw, rfb_u32 th,
                           int keep_xor, int word)
{
    rfb_u32 r = th;

    /* raw and xb are tile-shaped, so their rows are simply consecutive: they
     * are walked with the same pointer that finished the row before, and the
     * source and the shadow get the stride added.  No row index, and so no
     * multiply by a variable stride. */
    if (word) {
        const rfb_u32 words = tw >> 2;
        const rfb_u32 tail = tw & 3u;

        do {
            const rfb_u32 *sw = (const rfb_u32 *)(const void *)src;
            rfb_u32 *dw = (rfb_u32 *)(void *)sh;
            rfb_u32 *ww = (rfb_u32 *)(void *)raw;
            rfb_u32 n = words;

            if (keep_xor) {
                rfb_u32 *xw = (rfb_u32 *)(void *)xb;
                while (n--) {
                    rfb_u32 sv = *sw++;
                    *xw++ = sv ^ *dw;
                    *dw++ = sv;
                    *ww++ = sv;
                }
                xb = (rfb_u8 *)(void *)xw;
            } else {
                while (n--) {
                    rfb_u32 sv = *sw++;
                    *dw++ = sv;
                    *ww++ = sv;
                }
            }
            raw = (rfb_u8 *)(void *)ww;

            if (tail) {
                const rfb_u8 *s = (const rfb_u8 *)(const void *)sw;
                rfb_u8 *d = (rfb_u8 *)(void *)dw;
                rfb_u32 c = tail;
                while (c--) {
                    rfb_u8 sv = *s++;
                    if (keep_xor)
                        *xb++ = (rfb_u8)(sv ^ *d);
                    *d++ = sv;
                    *raw++ = sv;
                }
            }
            src += bpr; sh += bpr;
        } while (--r);
        return;
    }

    do {
        const rfb_u8 *s = src;
        rfb_u8 *d = sh;
        rfb_u32 c = tw;
        while (c--) {
            rfb_u8 sv = *s++;
            if (keep_xor)
                *xb++ = (rfb_u8)(sv ^ *d);
            *d++ = sv;
            *raw++ = sv;
        }
        src += bpr; sh += bpr;
    } while (--r);
}

/* A plane that did not change but has to be sent anyway (no RFB_F_PLANEMASK).
 * The shadow already holds the bytes, and the XOR against them is zero. */
static void rfb_take_clean(const rfb_u8 *sh, rfb_u8 *raw, rfb_u8 *xb,
                           rfb_u32 bpr, rfb_u32 tw, rfb_u32 th, int keep_xor)
{
    rfb_u32 r = th;

    do {
        memcpy(raw, sh, (size_t)tw);
        raw += tw;
        if (keep_xor) {
            memset(xb, 0, (size_t)tw);
            xb += tw;
        }
        sh += bpr;
    } while (--r);
}

/* Can the whole frame use the longword path.  Every tile row starts at
 * plane_base + y * row_stride + tx * tile_w, so if the plane bases, the row
 * stride and the tile width are all multiples of four then every one of them
 * is, and the tile buffers are four-aligned by construction.  One answer per
 * frame instead of one per row. */
static int rfb_words_ok(const rfb_encoder *e, const rfb_u8 *const *planes)
{
    unsigned long bits = (unsigned long)e->row_stride
                       | (unsigned long)e->g.tile_w
                       | (unsigned long)e->plane_stride
                       | (unsigned long)e->shadow
                       | (unsigned long)e->xorbuf
                       | (unsigned long)e->rawbuf;
    rfb_u32 p;

    for (p = 0; p < e->nplanes; p++)
        bits |= (unsigned long)planes[p];

    return ((bits & 3ul) == 0ul);
}

/* --------------------------------------------------------- scroll probe --- */

typedef struct {
    rfb_u16 x0, w, y0, h;
    rfb_s16 dy;
} rfb_copy;

/* Which column blocks of a row are byte-identical.  Bit b set = block b
 * matched.  This runs n_samples * (n_cand + 1) times a frame and IS the cost
 * of the detector, so everything constant is hoisted out of it: the two
 * pointers walk, the block count is a countdown, and the whole-block case
 * carries no bounds arithmetic at all.
 *
 * It used to recompute `bpr - off` and rederive both pointers from a base and
 * an offset for every block, which at the four-byte block size the old sizing
 * produced for an 80-byte row was about twenty-four instructions to compare a
 * single longword. */
static rfb_u32 rfb_blk_match(const rfb_u8 *a, const rfb_u8 *b, rfb_u32 bpr,
                             rfb_u32 blk, rfb_u32 nblk)
{
    const rfb_u32 words = blk >> 2;
    const int aligned = ((((unsigned long)a) | ((unsigned long)b) | blk)
                         & 3ul) == 0ul;
    rfb_u32 mask = 0, bit = 1u, whole, left = bpr;

    /* Blocks that are wholly inside the row, and the short one at the end. */
    whole = (nblk && bpr / blk >= nblk) ? nblk : bpr / blk;

    if (aligned) {
        const rfb_u32 *pa = (const rfb_u32 *)(const void *)a;
        const rfb_u32 *pb = (const rfb_u32 *)(const void *)b;
        rfb_u32 i = whole;

        while (i--) {
            rfb_u32 w = words;
            rfb_u32 acc = 0;

            while (w--)
                acc |= *pa++ ^ *pb++;
            if (!acc)
                mask |= bit;
            bit <<= 1;
        }
        a = (const rfb_u8 *)(const void *)pa;
        b = (const rfb_u8 *)(const void *)pb;
        left = bpr - whole * blk;
    } else {
        rfb_u32 i = whole;

        while (i--) {
            rfb_u32 n = blk;
            rfb_u32 acc = 0;

            while (n--)
                acc |= (rfb_u32)(*a++ ^ *b++);
            if (!acc)
                mask |= bit;
            bit <<= 1;
        }
        left = bpr - whole * blk;
    }

    /* The short block at the end of the row, a byte at a time rather than
     * reading past it. */
    if (whole < nblk && left) {
        rfb_u32 acc = 0;

        while (left--)
            acc |= (rfb_u32)(*a++ ^ *b++);
        if (!acc)
            mask |= bit;
    }
    return mask;
}

/* One streamed read of the sampled plane-0 rows.  Every candidate offset is
 * then answered out of the shadow, which is fast RAM, so the probe costs one
 * strided read of one plane however many offsets it carries. */
static rfb_u32 rfb_probe_pass(rfb_encoder *e, const rfb_u8 *src,
                              const rfb_s16 *cand, rfb_u32 ncand,
                              rfb_u32 *chg, rfb_u32 *out_nsamp)
{
    const rfb_u32 bpr = e->g.bytes_per_row;
    const rfb_u32 stride = e->row_stride;
    const rfb_u32 h = e->g.height;
    const rfb_u32 nsl = e->n_samples;
    const rfb_u32 blk = e->blk_bytes;
    const rfb_u32 nblk = e->n_blk;
    const rfb_u32 keep = (nblk >= 32u) ? 0xFFFFFFFFu : ((1u << nblk) - 1u);
    const rfb_u8 *srow = src;
    const rfb_u8 *shrow = e->shadow;
    const rfb_u32 sstep = (rfb_u32)e->probe_step * stride;
    rfb_u32 y, c, nsamp = 0, dirty = 0;

    for (y = 0; y < h && nsamp < nsl; y += e->probe_step, nsamp++) {
        rfb_u32 *pm = e->probe_mask + nsamp;
        rfb_u32 cm;

        memcpy(e->probe_row, srow, (size_t)bpr);
        e->st.probe_bytes += bpr;

        cm = ~rfb_blk_match(e->probe_row, shrow, bpr, blk, nblk);
        cm &= keep;
        chg[nsamp] = cm;
        if (cm)
            dirty++;

        /* pm walks by n_samples rather than being indexed with
         * c * n_samples + nsamp, which was a 32-bit multiply per candidate
         * per sample -- 585 of them a frame at the default candidate list. */
        for (c = 0; c < ncand; c++, pm += nsl) {
            rfb_s32 sy = (rfb_s32)y + cand[c];
            rfb_u32 mask = 0;
            if (sy >= 0 && (rfb_u32)sy < h)
                mask = rfb_blk_match(e->probe_row,
                                     e->shadow + (rfb_u32)sy * stride, bpr,
                                     blk, nblk);
            *pm = mask;
        }
        srow += sstep;
        shrow += sstep;
    }
    e->st.probes++;
    *out_nsamp = nsamp;
    return dirty;
}

static int rfb_detect_scroll(rfb_encoder *e, const rfb_u8 *src, rfb_copy *cp)
{
    const rfb_u32 bpr = e->g.bytes_per_row;
    const rfb_u32 h = e->g.height;
    const rfb_u32 nblk = e->n_blk;
    rfb_u32 *chg = e->probe_mask + (rfb_u32)e->scroll.n_cand * e->n_samples;
    rfb_s16 sticky[1];
    const rfb_s16 *cand = e->scroll.cand;
    rfb_u32 ncand = e->scroll.n_cand;
    rfb_u32 s, c, b;
    rfb_u32 dirtyblk[RFB_PROBE_MAX_BLK], scrollblk[RFB_PROBE_MAX_BLK];
    rfb_u32 nsamp = 0, dirty, changed = 0;
    rfb_u32 best = 0, best_score = 0;
    rfb_u32 lo, hi, run, run_start, best_run, best_start;
    const rfb_u32 *bestmask;
    rfb_u32 sel;
    int retried = 0;

    if (ncand == 0 || nblk == 0)
        return 0;

    /* A scroll runs for many frames, so the offset that worked last frame is
     * tried alone first.  That is one candidate instead of nine for every
     * frame of a scroll after the first, and the full list is only paid for
     * when the shortcut does not explain what moved. */
    if (e->last_copy && e->last_dy != 0) {
        sticky[0] = e->last_dy;
        cand = sticky;
        ncand = 1;
    }

again:
    dirty = rfb_probe_pass(e, src, cand, ncand, chg, &nsamp);

    if (nsamp < 4 || dirty * 4u < nsamp)   /* too little moved to be a scroll */
        return 0;

    changed = 0;
    for (s = 0; s < nsamp; s++) {
        rfb_u32 m = chg[s];
        while (m) { changed++; m &= m - 1u; }
    }

    /* Score a candidate on blocks that CHANGED and still matched at the
     * offset.  Scoring on matches alone elects any offset under which a static
     * background happens to agree with itself, and that is most of the
     * screen. */
    best = 0; best_score = 0;
    {
        const rfb_u32 *pm = e->probe_mask;
        for (c = 0; c < ncand; c++, pm += e->n_samples) {
            rfb_u32 score = 0;
            for (s = 0; s < nsamp; s++) {
                rfb_u32 m = pm[s] & chg[s];
                while (m) { score++; m &= m - 1u; }
            }
            if (score > best_score) { best_score = score; best = c; }
        }
    }

    if (best_score * 100u < changed * e->scroll.match_pct && !retried &&
        cand != e->scroll.cand) {
        /* The shortcut did not explain the frame.  Pay for the full list. */
        cand = e->scroll.cand;
        ncand = e->scroll.n_cand;
        retried = 1;
        goto again;
    }
    if (best_score == 0)
        return 0;

    /* A column is in the copy when it changes often and, when it changes, the
     * offset explains it.  Columns that never change are left out: including
     * them widens the op for nothing and lets an unrelated change anywhere in
     * them cut the band in half. */
    bestmask = e->probe_mask + best * e->n_samples;
    lo = nblk; hi = 0;
    for (b = 0; b < nblk; b++) {
        rfb_u32 bitb = 1u << b;
        dirtyblk[b] = 0; scrollblk[b] = 0;
        for (s = 0; s < nsamp; s++) {
            if (chg[s] & bitb) {
                dirtyblk[b]++;
                if (bestmask[s] & bitb)
                    scrollblk[b]++;
            }
        }
        if (dirtyblk[b] * 8u >= nsamp &&
            scrollblk[b] * 100u >= dirtyblk[b] * e->scroll.match_pct) {
            if (b < lo) lo = b;
            hi = b;
        }
    }
    if (lo > hi)
        return 0;

    sel = 0;
    for (b = lo; b <= hi; b++)
        sel |= 1u << b;

    /* Longest run of consecutive samples where the span mostly matched.  A
     * sloppy band is safe: the copy is a prediction, and the tile pass runs
     * against the shadow the copy left behind, so a row the copy got wrong
     * costs the tiles it dirties and nothing else. */
    {
        rfb_u32 nsel = hi - lo + 1u;
        rfb_u32 hit_need = (nsel * e->scroll.match_pct + 99u) / 100u;
        if (hit_need == 0)
            hit_need = 1;
        run = 0; run_start = 0; best_run = 0; best_start = 0;
        for (s = 0; s < nsamp; s++) {
            rfb_u32 m = bestmask[s] & sel;
            rfb_u32 hits = 0;
            while (m) { hits++; m &= m - 1u; }
            if (hits >= hit_need) {
                if (run == 0)
                    run_start = s;
                run++;
                if (run > best_run) { best_run = run; best_start = run_start; }
            } else {
                run = 0;
            }
        }
    }
    if (best_run < 2)
        return 0;

    cp->x0 = (rfb_u16)(lo * e->blk_bytes);
    cp->w  = (rfb_u16)((hi + 1u) * e->blk_bytes - cp->x0);
    if ((rfb_u32)cp->x0 + cp->w > bpr)
        cp->w = (rfb_u16)(bpr - cp->x0);
    cp->y0 = (rfb_u16)(best_start * e->probe_step);
    cp->h  = (rfb_u16)(best_run * e->probe_step);
    cp->dy = cand[best];

    /* Clip so both ends of the move stay on the screen. */
    if (cp->dy > 0) {
        if ((rfb_u32)cp->y0 + cp->h + cp->dy > h) {
            if ((rfb_u32)cp->y0 + cp->dy >= h)
                return 0;
            cp->h = (rfb_u16)(h - cp->y0 - cp->dy);
        }
    } else {
        rfb_u32 back = (rfb_u32)(-cp->dy);
        if (cp->y0 < back) {
            if ((rfb_u32)cp->h <= back - cp->y0)
                return 0;
            cp->h = (rfb_u16)(cp->h - (back - cp->y0));
            cp->y0 = (rfb_u16)back;
        }
        if ((rfb_u32)cp->y0 + cp->h > h)
            cp->h = (rfb_u16)(h - cp->y0);
    }
    if (cp->h < e->scroll.min_band || cp->w == 0)
        return 0;
    return 1;
}

static void rfb_apply_copy(rfb_encoder *e, const rfb_copy *cp)
{
    const rfb_u32 stride = e->row_stride;
    rfb_u32 p, r;

    for (p = 0; p < e->nplanes; p++) {
        rfb_u8 *plane = e->shadow + p * e->plane_stride;
        if (cp->dy > 0) {
            for (r = 0; r < cp->h; r++) {
                rfb_u32 dstr = cp->y0 + r;
                rfb_u32 srcr = dstr + (rfb_u32)cp->dy;
                memmove(plane + dstr * stride + cp->x0,
                        plane + srcr * stride + cp->x0, (size_t)cp->w);
            }
        } else {
            for (r = cp->h; r > 0; r--) {
                rfb_u32 dstr = cp->y0 + r - 1u;
                rfb_u32 srcr = (rfb_u32)((rfb_s32)dstr + cp->dy);
                memmove(plane + dstr * stride + cp->x0,
                        plane + srcr * stride + cp->x0, (size_t)cp->w);
            }
        }
    }
    e->st.shadow_move += (rfb_u32)cp->h * cp->w * e->nplanes;
}

/* --------------------------------------------------------------- frame --- */

long rfb_encode_frame(rfb_encoder *e, const rfb_u8 *src,
                      rfb_u8 *out, rfb_u32 out_cap)
{
    const rfb_u8 *planes[RFB_MAX_DEPTH];
    rfb_u32 p;

    if (!e || !src)
        return RFB_E_GEOM;
    if (e->nplanes == 0 || e->nplanes > RFB_MAX_DEPTH)
        return RFB_E_GEOM;

    for (p = 0; p < e->nplanes; p++)
        planes[p] = src + p * e->plane_stride;

    return rfb_encode_frame_planes(e, planes, out, out_cap);
}

long rfb_encode_frame_planes(rfb_encoder *e, const rfb_u8 *const *planes,
                             rfb_u8 *out, rfb_u32 out_cap)
{
    rfb_u32 bpr, depth, tb, tile_row;
    int keep_xor, min_run, word, chunky;
    rfb_out o;
    rfb_u32 ty, tx, p, y0, top = 0, tile_index;
    rfb_u32 dirty_tiles = 0;
    rfb_u32 dirty_plane[RFB_MAX_DEPTH];
    rfb_u8 *sh_plane[RFB_MAX_DEPTH];
    rfb_u8 *raw_plane[RFB_MAX_DEPTH];
    rfb_u8 *xor_plane[RFB_MAX_DEPTH];
    int did_copy = 0;

    if (!e || !planes || !out)
        return RFB_E_GEOM;

    bpr = e->g.bytes_per_row;
    /* PLANES, not the depth: a chunky source is one eight-bit plane and its
     * depth is what sizes the palette at the far end, nothing here. */
    depth = e->nplanes;
    chunky = (e->g.format == RFB_FMT_CLUT8) ? 1 : 0;
    tb = (rfb_u32)e->g.tile_w * e->g.tile_h;
    keep_xor = (e->flags & RFB_F_XOR) ? 1 : 0;
    min_run = (e->flags & RFB_F_RLE2) ? 2 : 3;

    /* Both of these were being recomputed inside the tile walk: the alignment
     * verdict on every row of every tile of every plane, and a 32-bit multiply
     * per tile for the row offset, which on the -m68000 build the whole
     * archive is compiled with is a call to __mulsi3. */
    word = rfb_words_ok(e, planes);
    tile_row = (rfb_u32)e->g.tile_h * e->row_stride;

    o.buf = out; o.cap = out_cap; o.len = 0; o.over = 0;
    rfb_put8(&o, RFB_VERSION);
    rfb_put8(&o, 0);
    rfb_put16(&o, e->seq++);

    if (e->flags & RFB_F_COPYRECT) {
        int probe = 1;
        if (e->flags & RFB_F_SCROLL_ADAPTIVE) {
            /* Nothing much moved last frame, so nothing can be scrolling. */
            probe = (e->last_dirty >= e->scroll.busy_tiles) || e->last_copy;
            /* A busy screen that is not scrolling -- a window being dragged --
             * would otherwise pay for the probe on every frame of the drag.
             * Back off doubling after each miss, and reset on a hit. */
            if (probe && e->backoff_left) {
                e->backoff_left--;
                probe = 0;
            }
        }
        if (probe) {
            rfb_copy cp;
            if (rfb_detect_scroll(e, planes[0], &cp)) {
                rfb_put8(&o, RFB_OP_COPY);
                rfb_put16(&o, cp.x0);
                rfb_put16(&o, cp.w);
                rfb_put16(&o, cp.y0);
                rfb_put16(&o, cp.h);
                rfb_put16(&o, (rfb_u32)(rfb_u16)cp.dy);
                rfb_apply_copy(e, &cp);
                e->st.copies++;
                e->st.copy_rows += cp.h;
                e->last_dy = cp.dy;
                e->backoff = 0;
                e->backoff_left = 0;
                e->miss_run = 0;
                e->since_copy = 0;
                did_copy = 1;
            } else if (e->scroll.max_backoff) {
                /* A scroll misses the odd frame -- a redraw lands in the band,
                 * the window resizes -- and backing off on the first miss
                 * costs the whole burst that follows it.  Three in a row is a
                 * screen that is busy with something else.
                 *
                 * THAT TOLERANCE IS FOR A SCROLL THAT EXISTS.  A screen that
                 * is merely busy -- windows opening, an icon being dragged --
                 * never produces a copy at all, and paying three full probes
                 * before starting to back off, and three more every time the
                 * backoff expires, was most of what the detector cost on that
                 * kind of activity: measured at 54 ms a frame, for a capture
                 * on which it found nothing whatsoever.  So the tolerance is
                 * extended only to a screen that has actually scrolled
                 * recently; anything else backs off from the first miss. */
                rfb_u32 tolerate = (e->since_copy < 64u) ? 3u : 1u;

                if (e->miss_run < 255u)
                    e->miss_run++;
                if (e->miss_run >= tolerate) {
                    rfb_u32 b = e->backoff ? e->backoff * 2u : 1u;
                    if (b > e->scroll.max_backoff)
                        b = e->scroll.max_backoff;
                    e->backoff = (rfb_u8)b;
                    e->backoff_left = e->backoff;
                }
            }
        }
    }

    /* Every per-plane base the tile walk needs, worked out once for the frame.
     * These were `e->shadow + p * e->plane_stride`, `e->rawbuf + p * tb` and
     * `e->xorbuf + p * tb` INSIDE the walk, so a 640x256x2 frame paid 240
     * 32-bit multiplies -- which the -m68000 build the archive ships compiles
     * to calls to __mulsi3 -- to recompute eight pointers. */
    for (p = 0; p < depth; p++) {
        sh_plane[p]  = e->shadow + p * e->plane_stride;
        raw_plane[p] = e->rawbuf + p * tb;
        xor_plane[p] = e->xorbuf + p * tb;
    }

    y0 = 0;
    tile_index = 0;
    for (ty = 0; ty < e->tiles_y; ty++, y0 += tile_row) {
        rfb_u32 th = e->g.height - top;
        rfb_u32 x0 = 0;

        if (th > e->g.tile_h)
            th = e->g.tile_h;
        top += e->g.tile_h;

        for (tx = 0; tx < e->tiles_x; tx++, tile_index++,
                                       x0 += e->g.tile_w) {
            rfb_u32 tw = bpr - x0;
            rfb_u32 off = y0 + x0;
            rfb_u32 raw_len, mask = 0;

            if (tw > e->g.tile_w)
                tw = e->g.tile_w;
            /* Both fit a word, so this is a MULU.W and not a helper call. */
            raw_len = (rfb_u32)((rfb_u16)tw * (rfb_u16)th);
            e->st.tiles_scanned++;

            /* Ask first, write nothing.  A tile nobody drew on ends here. */
            for (p = 0; p < depth; p++) {
                dirty_plane[p] = (rfb_u32)rfb_cmp_plane(
                        planes[p] + off, sh_plane[p] + off,
                        e->row_stride, tw, th, word);
                if (dirty_plane[p])
                    mask |= 1u << p;
            }
            if (mask == 0)
                continue;

            dirty_tiles++;
            e->st.tiles_dirty++;
            if (!(e->flags & RFB_F_PLANEMASK))
                mask = (1u << depth) - 1u;

            /* One read of the source per changed plane, into rawbuf, which is
             * then what updates the shadow and what is encoded below. */
            for (p = 0; p < depth; p++) {
                if (!(mask & (1u << p)))
                    continue;
                if (dirty_plane[p])
                    rfb_take_plane(planes[p] + off, sh_plane[p] + off,
                                   raw_plane[p], xor_plane[p],
                                   e->row_stride, tw, th, keep_xor, word);
                else
                    rfb_take_clean(sh_plane[p] + off,
                                   raw_plane[p], xor_plane[p],
                                   e->row_stride, tw, th, keep_xor);
            }

            /* One plane means the mask has one value, so it is not sent.
             * That is the whole of the chunky wire format: same tile grid,
             * same codes, same payloads, one byte fewer per tile. */
            if (chunky) {
                rfb_put8(&o, RFB_OP_TILE8);
                rfb_put16(&o, tile_index);
            } else {
                rfb_put8(&o, RFB_OP_TILE);
                rfb_put16(&o, tile_index);
                rfb_put8(&o, (rfb_u8)mask);
            }

            for (p = 0; p < depth; p++) {
                rfb_u32 best_len, la, lb;
                rfb_u8 *raw = raw_plane[p];
                rfb_u8 *best_ptr;
                int best_code;

                if (!(mask & (1u << p)))
                    continue;

                best_code = RFB_CODE_RAW;
                best_len = raw_len;
                best_ptr = raw;

                if (e->flags & RFB_F_BESTOF) {
                    /* Each candidate is capped at what is already the best, so
                     * a losing one stops early instead of finishing. */
                    if (keep_xor) {
                        la = rfb_packbits(xor_plane[p], raw_len,
                                          e->pb_a, best_len - 1u, min_run);
                        if (la != RFB_PB_FAIL) {
                            best_code = RFB_CODE_PB_XOR;
                            best_len = la;
                            best_ptr = e->pb_a;
                        }
                    }
                    if (e->flags & RFB_F_PACKBITS) {
                        lb = rfb_packbits(raw, raw_len,
                                          e->pb_b, best_len - 1u, min_run);
                        if (lb != RFB_PB_FAIL) {
                            best_code = RFB_CODE_PB_RAW;
                            best_len = lb;
                            best_ptr = e->pb_b;
                        }
                    }
                } else if (keep_xor) {
                    la = rfb_packbits(xor_plane[p], raw_len, e->pb_a,
                                      RFB_PB_BOUND(raw_len), min_run);
                    if (la != RFB_PB_FAIL) {
                        best_code = RFB_CODE_PB_XOR;
                        best_len = la;
                        best_ptr = e->pb_a;
                    }
                } else if (e->flags & RFB_F_PACKBITS) {
                    lb = rfb_packbits(raw, raw_len, e->pb_b,
                                      RFB_PB_BOUND(raw_len), min_run);
                    if (lb != RFB_PB_FAIL) {
                        best_code = RFB_CODE_PB_RAW;
                        best_len = lb;
                        best_ptr = e->pb_b;
                    }
                }

                rfb_put8(&o, (rfb_u8)best_code);
                if (best_code == RFB_CODE_RAW) {
                    e->st.code_raw++;
                } else {
                    rfb_put16(&o, best_len);
                    if (best_code == RFB_CODE_PB_RAW)
                        e->st.code_pb_raw++;
                    else
                        e->st.code_pb_xor++;
                }
                rfb_putblk(&o, best_ptr, best_len);
                e->st.planes_sent++;
            }

            if (o.over)
                return RFB_E_OVERFLOW;
        }
    }

    rfb_put8(&o, RFB_OP_END);
    if (o.over)
        return RFB_E_OVERFLOW;

    /* Every tile of every plane was read, and the clipped tiles at the edges
       sum to exactly one frame, so this is the frame rather than a running
       total with a 32-bit multiply per tile in it. */
    e->st.src_bytes += e->frame_bytes;
    if (!did_copy && e->since_copy < 255u)
        e->since_copy++;
    e->last_dirty = (rfb_u16)(dirty_tiles > 0xFFFFu ? 0xFFFFu : dirty_tiles);
    e->last_copy = (rfb_u8)did_copy;
    e->st.frames++;
    e->st.bytes_out += o.len;
    return (long)o.len;
}

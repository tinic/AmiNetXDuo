/* Frame encoder for the remote framebuffer.  Wire format is in the header.
 *
 * The shape of the thing: one pass over the source bitmap, tile by tile.  For
 * each tile the pass reads the new bytes once, XORs them against the shadow
 * copy of the previous frame, ORs the result into an accumulator and writes
 * the new bytes back to the shadow.  On the Amiga that single read is from
 * chip RAM and costs more than everything done with the bytes afterwards, so
 * nothing here reads the source twice -- except the scroll probe, which is
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
    return 1;
}

rfb_u32 rfb_shadow_size(const rfb_geom *g)
{
    if (!rfb_geom_ok(g))
        return 0;
    return (rfb_u32)g->bytes_per_row * g->height * g->depth;
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
    need = tb * g->depth               /* xorbuf, one tile per plane */
         + tb                          /* rawbuf */
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
    rfb_u32 tx, ty, tiles, tb;

    if (!rfb_geom_ok(g))
        return 0;
    tx = ((rfb_u32)g->bytes_per_row + g->tile_w - 1u) / g->tile_w;
    ty = ((rfb_u32)g->height + g->tile_h - 1u) / g->tile_h;
    tiles = tx * ty;
    tb = (rfb_u32)g->tile_w * g->tile_h;

    /* Header, one copy op, every tile carrying every plane at the PackBits
     * expansion bound, and the terminator. */
    return 4u + 11u
         + tiles * (3u + (rfb_u32)g->depth * (3u + RFB_PB_BOUND(tb)))
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

    memset(e, 0, sizeof(*e));
    e->g = *g;
    e->flags = flags;
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
    e->frame_bytes = e->plane_bytes * g->depth;
    e->shadow = shadow;
    e->scratch = scratch;
    e->scratch_len = scratch_len;

    tb = (rfb_u32)g->tile_w * g->tile_h;
    p = rfb_align4(scratch);
    e->xorbuf = p; p = rfb_align4(p + tb * g->depth);
    e->rawbuf = p; p = rfb_align4(p + tb);
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
        if (e->blk_bytes == 0)
            e->blk_bytes = 4;
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

/* One plane of one tile: read src once, XOR against shadow, accumulate the
 * difference, write the new bytes into the shadow, and keep the XOR when the
 * caller can use it. */
static rfb_u32 rfb_diff_plane(const rfb_u8 *src, rfb_u8 *sh, rfb_u8 *xb,
                              rfb_u32 bpr, rfb_u32 tw, rfb_u32 th, int keep_xor)
{
    rfb_u32 acc = 0, r;

    for (r = 0; r < th; r++) {
        const rfb_u8 *s = src + r * bpr;
        rfb_u8 *d = sh + r * bpr;
        rfb_u8 *x = xb + r * tw;
        rfb_u32 c = 0;

        /* The word path needs all three pointers 4-aligned.  With a BitMap
         * bytes_per_row (always even, usually a multiple of 4) and a tile_w
         * that is a multiple of 4 it is taken on every row. */
        if ((((unsigned long)s | (unsigned long)d | (unsigned long)x) & 3ul) == 0ul) {
            if (keep_xor) {
                while (c + 4u <= tw) {
                    rfb_u32 sv = *(const rfb_u32 *)(const void *)(s + c);
                    rfb_u32 xv = sv ^ *(const rfb_u32 *)(const void *)(d + c);
                    acc |= xv;
                    *(rfb_u32 *)(void *)(d + c) = sv;
                    *(rfb_u32 *)(void *)(x + c) = xv;
                    c += 4u;
                }
            } else {
                while (c + 4u <= tw) {
                    rfb_u32 sv = *(const rfb_u32 *)(const void *)(s + c);
                    acc |= sv ^ *(const rfb_u32 *)(const void *)(d + c);
                    *(rfb_u32 *)(void *)(d + c) = sv;
                    c += 4u;
                }
            }
        }
        for (; c < tw; c++) {
            rfb_u8 sv = s[c];
            rfb_u8 xv = (rfb_u8)(sv ^ d[c]);
            acc |= xv;
            d[c] = sv;
            if (keep_xor)
                x[c] = xv;
        }
    }
    return acc;
}

static void rfb_gather(const rfb_u8 *sh, rfb_u8 *dst, rfb_u32 bpr,
                       rfb_u32 tw, rfb_u32 th)
{
    rfb_u32 r;
    for (r = 0; r < th; r++)
        memcpy(dst + r * tw, sh + r * bpr, (size_t)tw);
}

/* --------------------------------------------------------- scroll probe --- */

typedef struct {
    rfb_u16 x0, w, y0, h;
    rfb_s16 dy;
} rfb_copy;

/* Which column blocks of a row are byte-identical.  Bit b set = block b
 * matched.  Word compares with an early exit per block: this runs
 * n_samples * (n_cand + 1) times a frame and is the whole cost of the
 * detector, so it does not call memcmp per block. */
static rfb_u32 rfb_blk_match(const rfb_u8 *a, const rfb_u8 *b, rfb_u32 bpr,
                             rfb_u32 blk, rfb_u32 nblk)
{
    const rfb_u32 words = blk >> 2;
    const int aligned = ((((unsigned long)a) | ((unsigned long)b) | blk)
                         & 3ul) == 0ul;
    rfb_u32 mask = 0, i, off = 0;

    for (i = 0; i < nblk; i++, off += blk) {
        rfb_u32 n = bpr - off;
        if (n > blk) {
            if (aligned) {
                const rfb_u32 *pa = (const rfb_u32 *)(const void *)(a + off);
                const rfb_u32 *pb = (const rfb_u32 *)(const void *)(b + off);
                rfb_u32 w;
                for (w = 0; w < words; w++)
                    if (pa[w] != pb[w])
                        break;
                if (w == words)
                    mask |= 1u << i;
                continue;
            }
            n = blk;
        }
        /* The tail block runs a byte at a time rather than reading past the
         * end of the row. */
        {
            rfb_u32 c;
            for (c = 0; c < n; c++)
                if (a[off + c] != b[off + c])
                    break;
            if (c == n)
                mask |= 1u << i;
        }
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
    const rfb_u32 h = e->g.height;
    rfb_u32 y, c, nsamp = 0, dirty = 0;

    for (y = 0; y < h && nsamp < e->n_samples; y += e->probe_step, nsamp++) {
        rfb_u32 cm;
        memcpy(e->probe_row, src + y * bpr, (size_t)bpr);
        e->st.probe_bytes += bpr;

        cm = ~rfb_blk_match(e->probe_row, e->shadow + y * bpr, bpr,
                            e->blk_bytes, e->n_blk);
        cm &= (e->n_blk >= 32u) ? 0xFFFFFFFFu : ((1u << e->n_blk) - 1u);
        chg[nsamp] = cm;
        if (cm)
            dirty++;

        for (c = 0; c < ncand; c++) {
            rfb_s32 sy = (rfb_s32)y + cand[c];
            rfb_u32 mask = 0;
            if (sy >= 0 && (rfb_u32)sy < h)
                mask = rfb_blk_match(e->probe_row,
                                     e->shadow + (rfb_u32)sy * bpr, bpr,
                                     e->blk_bytes, e->n_blk);
            e->probe_mask[c * e->n_samples + nsamp] = mask;
        }
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
    for (c = 0; c < ncand; c++) {
        rfb_u32 score = 0;
        for (s = 0; s < nsamp; s++) {
            rfb_u32 m = e->probe_mask[c * e->n_samples + s] & chg[s];
            while (m) { score++; m &= m - 1u; }
        }
        if (score > best_score) { best_score = score; best = c; }
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
    lo = nblk; hi = 0;
    for (b = 0; b < nblk; b++) {
        dirtyblk[b] = 0; scrollblk[b] = 0;
        for (s = 0; s < nsamp; s++) {
            if (chg[s] & (1u << b)) {
                dirtyblk[b]++;
                if (e->probe_mask[best * e->n_samples + s] & (1u << b))
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
            rfb_u32 m = e->probe_mask[best * e->n_samples + s] & sel;
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
    const rfb_u32 bpr = e->g.bytes_per_row;
    rfb_u32 p, r;

    for (p = 0; p < e->g.depth; p++) {
        rfb_u8 *plane = e->shadow + p * e->plane_bytes;
        if (cp->dy > 0) {
            for (r = 0; r < cp->h; r++) {
                rfb_u32 dstr = cp->y0 + r;
                rfb_u32 srcr = dstr + (rfb_u32)cp->dy;
                memmove(plane + dstr * bpr + cp->x0,
                        plane + srcr * bpr + cp->x0, (size_t)cp->w);
            }
        } else {
            for (r = cp->h; r > 0; r--) {
                rfb_u32 dstr = cp->y0 + r - 1u;
                rfb_u32 srcr = (rfb_u32)((rfb_s32)dstr + cp->dy);
                memmove(plane + dstr * bpr + cp->x0,
                        plane + srcr * bpr + cp->x0, (size_t)cp->w);
            }
        }
    }
    e->st.shadow_move += (rfb_u32)cp->h * cp->w * e->g.depth;
}

/* --------------------------------------------------------------- frame --- */

long rfb_encode_frame(rfb_encoder *e, const rfb_u8 *src,
                      rfb_u8 *out, rfb_u32 out_cap)
{
    rfb_u32 bpr, depth, tb;
    int keep_xor, min_run;
    rfb_out o;
    rfb_u32 ty, tx, p;
    rfb_u32 dirty_tiles = 0;
    rfb_u32 accs[RFB_MAX_DEPTH];
    int did_copy = 0;

    if (!e || !src || !out)
        return RFB_E_GEOM;

    bpr = e->g.bytes_per_row;
    depth = e->g.depth;
    tb = (rfb_u32)e->g.tile_w * e->g.tile_h;
    keep_xor = (e->flags & RFB_F_XOR) ? 1 : 0;
    min_run = (e->flags & RFB_F_RLE2) ? 2 : 3;

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
            if (rfb_detect_scroll(e, src, &cp)) {
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
                did_copy = 1;
            } else {
                e->backoff = (rfb_u8)(e->backoff ? (e->backoff < 16u
                                                    ? e->backoff * 2u : 16u)
                                                 : 1u);
                e->backoff_left = e->backoff;
            }
        }
    }

    for (ty = 0; ty < e->tiles_y; ty++) {
        rfb_u32 y0 = ty * e->g.tile_h;
        rfb_u32 th = e->g.height - y0;
        if (th > e->g.tile_h)
            th = e->g.tile_h;

        for (tx = 0; tx < e->tiles_x; tx++) {
            rfb_u32 x0 = tx * e->g.tile_w;
            rfb_u32 tw = bpr - x0;
            rfb_u32 off = y0 * bpr + x0;
            rfb_u32 raw_len, mask = 0;

            if (tw > e->g.tile_w)
                tw = e->g.tile_w;
            raw_len = tw * th;
            e->st.tiles_scanned++;

            for (p = 0; p < depth; p++) {
                accs[p] = rfb_diff_plane(src + p * e->plane_bytes + off,
                                         e->shadow + p * e->plane_bytes + off,
                                         e->xorbuf + p * tb,
                                         bpr, tw, th, keep_xor);
                if (accs[p])
                    mask |= 1u << p;
            }
            e->st.src_bytes += raw_len * depth;
            if (mask == 0)
                continue;

            dirty_tiles++;
            e->st.tiles_dirty++;
            if (!(e->flags & RFB_F_PLANEMASK))
                mask = (1u << depth) - 1u;

            rfb_put8(&o, RFB_OP_TILE);
            rfb_put16(&o, ty * e->tiles_x + tx);
            rfb_put8(&o, (rfb_u8)mask);

            for (p = 0; p < depth; p++) {
                rfb_u32 best_len, la, lb;
                rfb_u8 *best_ptr;
                int best_code;

                if (!(mask & (1u << p)))
                    continue;

                rfb_gather(e->shadow + p * e->plane_bytes + off,
                           e->rawbuf, bpr, tw, th);

                best_code = RFB_CODE_RAW;
                best_len = raw_len;
                best_ptr = e->rawbuf;

                if (e->flags & RFB_F_BESTOF) {
                    /* Each candidate is capped at what is already the best, so
                     * a losing one stops early instead of finishing. */
                    if (keep_xor) {
                        la = rfb_packbits(e->xorbuf + p * tb, raw_len,
                                          e->pb_a, best_len - 1u, min_run);
                        if (la != RFB_PB_FAIL) {
                            best_code = RFB_CODE_PB_XOR;
                            best_len = la;
                            best_ptr = e->pb_a;
                        }
                    }
                    if (e->flags & RFB_F_PACKBITS) {
                        lb = rfb_packbits(e->rawbuf, raw_len,
                                          e->pb_b, best_len - 1u, min_run);
                        if (lb != RFB_PB_FAIL) {
                            best_code = RFB_CODE_PB_RAW;
                            best_len = lb;
                            best_ptr = e->pb_b;
                        }
                    }
                } else if (keep_xor) {
                    la = rfb_packbits(e->xorbuf + p * tb, raw_len, e->pb_a,
                                      RFB_PB_BOUND(raw_len), min_run);
                    if (la != RFB_PB_FAIL) {
                        best_code = RFB_CODE_PB_XOR;
                        best_len = la;
                        best_ptr = e->pb_a;
                    }
                } else if (e->flags & RFB_F_PACKBITS) {
                    lb = rfb_packbits(e->rawbuf, raw_len, e->pb_b,
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

    e->last_dirty = (rfb_u16)(dirty_tiles > 0xFFFFu ? 0xFFFFu : dirty_tiles);
    e->last_copy = (rfb_u8)did_copy;
    e->st.frames++;
    e->st.bytes_out += o.len;
    return (long)o.len;
}

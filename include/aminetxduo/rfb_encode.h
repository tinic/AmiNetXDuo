/* Remote-framebuffer frame encoder for Amiga planar bitmaps.
 *
 * Portable C99: no Amiga headers, no stdint, no floating point, and no
 * allocation in the per-frame path.  The caller owns the shadow copy of the
 * previous frame and one scratch block; rfb_shadow_size() and
 * rfb_scratch_size() say how big each has to be.
 *
 * Source layout is the Amiga BitMap: `depth` planes, plane-major, each plane
 * bytes_per_row * height bytes.  bytes_per_row comes from the BitMap and is
 * not width/8, so it is passed in rather than derived.  Every byte of a row is
 * encoded, padding included, so a decoded frame is byte-identical to the one
 * that went in.
 *
 * OR ONE CHUNKY PLANE, which is what an RTG screen is.  rfb_geom.format
 * selects between them and it is the only field that changes what the source
 * looks like:
 *
 *   RFB_FMT_PLANAR  depth planes of one bit, the Amiga BitMap above.
 *   RFB_FMT_CLUT8   ONE plane of eight bits: a palette index a byte, rows
 *                   bytes_per_row apart.  depth stays 8 because it is what
 *                   sizes the palette, but the encoder walks one plane and
 *                   not eight.
 *   RFB_FMT_RGB565  ONE plane of sixteen bits: a colour a pixel, two bytes,
 *                   big-endian ((r5 << 11) | (g6 << 5) | b5), rows
 *                   bytes_per_row apart.  depth is 16, and it is bits a pixel
 *                   rather than the size of anything: there is no palette on
 *                   this format and the receiver is sent none.
 *
 * Everything else is deliberately shared.  A tile is a rectangle of BYTES
 * whichever it is, the tile grid is over bytes_per_row whichever it is,
 * PackBits is a byte RLE that earns its keep on a palette index and on half a
 * colour exactly as it does on a bitplane, and XOR against the shadow and
 * RFB_OP_COPY do not know the difference.  What changes on the wire is one op
 * code and the loss of the per-plane changed mask, which a format with one
 * plane has nothing to say with.
 *
 * RGB565 IS WHAT A 15, 16, 24 OR 32-BIT CARD SCREEN ARRIVES AS, all four of
 * them, converted on the Amiga.  The alternative was to quantise truecolour
 * down to a palette on the guest, which is one encoder and one receiver path
 * but costs a colour search per pixel on a 68030 every frame.  This costs
 * shifts and masks, is exact within the format, and makes the four depths one
 * case here instead of four.  Two bytes a pixel against four is also half the
 * wire, which on a 10 Mbit card is the number that decides whether a frame
 * arrives.
 *
 * Wire format, big-endian throughout, one message per frame:
 *
 *   u8  version        RFB_VERSION
 *   u8  flags          0
 *   u16 seq            frame counter, wraps
 *   ops, in stream order, until RFB_OP_END
 *
 *   RFB_OP_END  0x00   end of frame
 *
 *   RFB_OP_COPY 0x01   u16 x0    first byte column
 *                      u16 w     byte columns
 *                      u16 y0    first row
 *                      u16 h     rows
 *                      s16 dy    source row = destination row + dy
 *                    In every plane, rows [y0,y0+h) columns [x0,x0+w) take the
 *                    value those columns had at rows [y0+dy, y0+h+dy) in the
 *                    previous frame.  It reads the previous frame, so a
 *                    decoder moving in place has to handle overlap.  Copies
 *                    never overlap each other and always precede the tiles.
 *
 *   RFB_OP_TILE 0x02   u16 index  ty * tiles_x + tx
 *                      u8  planes bit p set = plane p follows
 *                      then, for each set bit from 0 upwards:
 *                      u8  code
 *                        0 RAW     tw*th bytes, tile rows concatenated
 *                        1 PB_RAW  u16 len, len bytes: PackBits of the above
 *                        2 PB_XOR  u16 len, len bytes: PackBits of the same
 *                                  bytes XORed with the previous frame's
 *                    Tile origin is (tx*tile_w, ty*tile_h) in bytes and rows;
 *                    tw and th are clipped at the right and bottom edges.
 *
 *   RFB_OP_TILE8 0x03  u16 index  ty * tiles_x + tx
 *                      u8  code   the same three codes, same payloads
 *                    RFB_OP_TILE with the plane mask taken out, and the only
 *                    op RFB_FMT_CLUT8 and RFB_FMT_RGB565 emit.  There is one
 *                    plane and it is always the one that changed, so a mask
 *                    byte would carry the value 1 on every tile of every
 *                    frame.  A decoder that meets this op is being told the
 *                    source is one plane of bytes; it is never mixed with
 *                    RFB_OP_TILE in one stream.  Which of the two one-plane
 *                    formats it is comes from the geometry and not from the
 *                    op, because nothing in a tile depends on it: the tile is
 *                    a rectangle of bytes either way and only the receiver's
 *                    last step, bytes to pixels, differs.
 *
 * PackBits is the IFF ILBM byte RLE: n in 0..127 copies n+1 literal bytes,
 * n in 129..255 repeats the next byte 257-n times, 128 is a no-op and is never
 * emitted.  A tile is one continuous byte stream across its rows, not one run
 * per row.
 *
 * PB_XOR and RFB_OP_COPY are both against the SHADOW -- the frame the decoder
 * last applied -- so a frame decodes correctly only where the decoder's shadow
 * is the one the encoder had.  A full frame, every tile and no copies, is
 * therefore decodable only from an all-zero shadow, and is what the encoder
 * emits once its shadow has been cleared.  Nothing in a frame says whether it
 * is a full one, so a decoder cannot tell and must not guess: the barrier is
 * `geom`, which zeroes the shadow at both ends, and the frame after one is
 * always full.  See rfb_words.h for the words that move that barrier.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_RFB_ENCODE_H
#define AMINETXDUO_RFB_ENCODE_H

#ifdef __cplusplus
extern "C" {
#endif

/* Own typedefs rather than <stdint.h>: the encoder has to build under every
 * m68k toolchain the project supports, and these three widths are all it
 * needs. */
typedef unsigned char  rfb_u8;
typedef unsigned short rfb_u16;
typedef unsigned int   rfb_u32;   /* int, not long: long is 64 bits on the host */
typedef signed short   rfb_s16;
typedef signed int     rfb_s32;

#define RFB_VERSION      1

#define RFB_OP_END       0x00
#define RFB_OP_COPY      0x01
#define RFB_OP_TILE      0x02
#define RFB_OP_TILE8     0x03   /* either chunky format: no plane mask */

#define RFB_CODE_RAW     0
#define RFB_CODE_PB_RAW  1
#define RFB_CODE_PB_XOR  2

/*
 * What a pixel means.  A small enum and not a flag word: the values are not
 * independent bits and never will be, and a number leaves room that a boolean
 * does not.  Zero is planar so that a caller which predates this field, or
 * clears its rfb_geom, gets the Amiga BitMap it always got.
 *
 * Three questions are answered from this one field, and they are separate
 * questions.  Anything reading a format has to ask the one it means:
 *
 *   layout        how the source bytes are arranged.  RFB_FMT_IS_CHUNKY()
 *                 says one plane of bytes; everything else is depth planes of
 *                 one bit, the Amiga BitMap.  This is the only one the
 *                 ENCODER asks, and it is why adding a pixel meaning costs
 *                 the encoder nothing.
 *   bytes a pixel RFB_FMT_PIXEL_BYTES(), for a chunky format.  Zero on a
 *                 planar one, where a pixel is not made of whole bytes.
 *   colours       rfb_pal_colours(), how long the `pal` word is.  NOT
 *                 1 << depth: that happens to be right for a plain planar
 *                 screen and is wrong for anything whose planes carry
 *                 something other than a palette index.
 *
 * Two of the three are deliberately not derivable from the depth, because the
 * modes that are coming are exactly the ones where the depth stops being a
 * palette width.  A HAM6 screen is six planes deep with sixteen base colours
 * and a HAM8 one is eight deep with sixty-four; an EHB screen is six deep
 * with thirty-two.  None of those changes what the encoder does -- they are
 * ordinary planar bitmaps and it walks them as it always has -- and all of
 * them change what the receiver draws.  So the room for them is here, in the
 * meaning, and a new one is a value plus a line in rfb_pal_colours() plus a
 * branch in the browser, not a change to the wire.
 */
#define RFB_FMT_PLANAR   0      /* depth one-bit planes, pixel is an index  */
#define RFB_FMT_CLUT8    1      /* one plane, a byte is an index            */
#define RFB_FMT_RGB565   2      /* one plane, two bytes are a colour        */

/* Planes, not bits: the chunky formats are one plane of bytes and differ only
 * in how many bytes make a pixel. */
#define RFB_FMT_IS_CHUNKY(f) ((f) == RFB_FMT_CLUT8 || (f) == RFB_FMT_RGB565)

/* Source bytes one pixel occupies, and 0 on a planar format, where it is not
 * a whole number.  A receiver turns a byte column into a pixel column with
 * this. */
#define RFB_FMT_PIXEL_BYTES(f) \
    ((f) == RFB_FMT_CLUT8 ? 1u : ((f) == RFB_FMT_RGB565 ? 2u : 0u))

/* The longest `pal` there is, which is what sizes a caller's buffer.  It is
 * not the ceiling on rfb_geom.depth: RFB_FMT_RGB565 is 16 deep and carries no
 * palette at all. */
#define RFB_MAX_DEPTH    8

/* What rfb_geom.depth is on the truecolour format, which is the one place a
 * depth is neither a plane count nor a palette width. */
#define RFB_RGB565_DEPTH 16
#define RFB_MAX_TILE_W   64   /* bytes */
#define RFB_MAX_TILE_H   64   /* rows */
#define RFB_MAX_CAND     16

/* Encoder layers.  Each one can be switched off on its own so the bench can
 * price it; the shipping set is RFB_F_BASELINE. */
#define RFB_F_PACKBITS         0x0001u  /* PB_RAW is allowed */
#define RFB_F_XOR              0x0002u  /* PB_XOR is allowed */
#define RFB_F_PLANEMASK        0x0004u  /* send only the planes that changed */
#define RFB_F_BESTOF           0x0008u  /* pick the smallest allowed code */
#define RFB_F_COPYRECT         0x0010u  /* detect a vertical scroll */
#define RFB_F_SCROLL_ADAPTIVE  0x0020u  /* probe only after a busy frame */
#define RFB_F_RLE2             0x0040u  /* PackBits runs start at 2, not 3 */
/* BMF_INTERLEAVED: the plane rows of one screen row sit together, so a plane
 * advances by bytes_per_row and a row by bytes_per_row * depth.  The wire
 * format does not change -- a tile index and a plane number mean the same
 * thing either way -- only how the encoder walks the bitmap and its shadow. */
#define RFB_F_INTERLEAVED      0x0080u

#define RFB_F_BASELINE (RFB_F_PACKBITS | RFB_F_XOR | RFB_F_PLANEMASK | \
                        RFB_F_BESTOF)

#define RFB_E_GEOM       (-1L)   /* geometry rejected */
#define RFB_E_BUFFER     (-2L)   /* shadow or scratch too small */
#define RFB_E_OVERFLOW   (-3L)   /* output capacity exhausted */

typedef struct {
    rfb_u16 width;          /* pixels, for the receiver; not used to index */
    rfb_u16 height;         /* rows */
    rfb_u16 bytes_per_row;  /* from the BitMap; one plane's row either way */
    rfb_u8  depth;          /* 1..8 planar, 8 for CLUT8, 16 for RGB565      */
    rfb_u8  tile_w;         /* tile width in BYTES, 1..RFB_MAX_TILE_W */
    rfb_u8  tile_h;         /* tile height in rows, 1..RFB_MAX_TILE_H */
    rfb_u8  format;         /* one of the three RFB_FMT_ */
} rfb_geom;

/* Counters.  src_bytes and probe_bytes are the two that matter on the Amiga:
 * both are reads of the live bitmap, which is chip RAM. */
typedef struct {
    rfb_u32 frames;
    rfb_u32 bytes_out;
    rfb_u32 tiles_scanned;
    rfb_u32 tiles_dirty;
    rfb_u32 planes_sent;
    rfb_u32 code_raw;
    rfb_u32 code_pb_raw;
    rfb_u32 code_pb_xor;
    rfb_u32 copies;
    rfb_u32 copy_rows;
    rfb_u32 probes;         /* frames the scroll detector ran on */
    rfb_u32 src_bytes;      /* chip reads, tile pass */
    rfb_u32 probe_bytes;    /* chip reads, scroll probe */
    rfb_u32 shadow_move;    /* shadow bytes moved for copy ops */
} rfb_stats;

typedef struct {
    rfb_u8  probe_step;     /* sample every Nth row, 1..16 */
    rfb_u8  match_pct;      /* rows that must match for a candidate to win */
    rfb_u16 min_band;       /* fewest rows worth a copy op */
    rfb_u8  n_cand;
    rfb_s16 cand[RFB_MAX_CAND];  /* row offsets to try */
    rfb_u16 busy_tiles;     /* RFB_F_SCROLL_ADAPTIVE threshold */
    rfb_u8  max_backoff;    /* frames skipped after a miss, 0 = never skip */
} rfb_scroll_cfg;

typedef struct {
    rfb_geom g;
    rfb_u32  flags;
    rfb_u8   nplanes;       /* g.depth planar, 1 chunky.  What the walk uses */
    rfb_u16  tiles_x;
    rfb_u16  tiles_y;
    rfb_u32  plane_bytes;   /* bytes_per_row * height */
    rfb_u32  frame_bytes;   /* plane_bytes * depth */
    rfb_u32  row_stride;    /* source bytes from one row to the next */
    rfb_u32  plane_stride;  /* source bytes from one plane to the next */
    rfb_u16  seq;

    rfb_u8  *shadow;
    rfb_u8  *scratch;
    rfb_u32  scratch_len;

    /* Carved out of scratch by rfb_encoder_init(). */
    rfb_u8  *xorbuf;        /* depth * tile_w * tile_h */
    rfb_u8  *rawbuf;        /* depth * tile_w * tile_h */
    rfb_u8  *pb_a;
    rfb_u8  *pb_b;
    rfb_u8  *probe_row;     /* one source row, RFB_F_COPYRECT only */
    rfb_u32 *probe_mask;    /* n_cand * n_samples column-match masks */

    rfb_scroll_cfg scroll;
    rfb_u16  probe_step;    /* effective, after the sample-count clamp */
    rfb_u16  n_samples;
    rfb_u16  blk_bytes;     /* probe column block width */
    rfb_u16  n_blk;
    rfb_u16  last_dirty;    /* dirty tiles in the previous frame */
    rfb_u8   last_copy;
    rfb_s16  last_dy;       /* the offset that worked, tried first next time */
    rfb_u8   backoff;       /* frames to skip after a probe found nothing */
    rfb_u8   backoff_left;
    rfb_u8   miss_run;      /* consecutive probes that found nothing */
    rfb_u8   since_copy;    /* frames since a copy, saturating at 255 */

    /* Accumulated across the bands of one screen pass, because they describe
     * the pass and not the message.  Closed and cleared by the last band. */
    rfb_u32  band_dirty;
    rfb_u8   band_copy;

    rfb_stats st;
} rfb_encoder;

/* How many planes the source actually has: g->depth, or 1 for a chunky
 * format.  A caller sizing its own buffers wants this and not the depth. */
rfb_u8  rfb_planes(const rfb_geom *g);

/*
 * How many entries the `pal` word carries for this geometry, and 0 when the
 * format has no palette.
 *
 * Asked instead of computing 1 << depth, which is right only where the planes
 * hold a palette index and nothing else.  It is the one number both ends have
 * to agree on without having seen a frame -- the server sizes the word it
 * sends and the receiver sizes the array it parses into -- so it is answered
 * in one place rather than spelled out in each.  0 on RFB_FMT_RGB565, and no
 * `pal` word is sent at all on a format that answers 0.
 */
rfb_u32 rfb_pal_colours(const rfb_geom *g);

rfb_u32 rfb_shadow_size(const rfb_geom *g);
rfb_u32 rfb_scratch_size(const rfb_geom *g, rfb_u32 flags,
                         const rfb_scroll_cfg *cfg);
rfb_u32 rfb_worst_case_frame(const rfb_geom *g);

/* Fills cfg with the defaults: every 4th row sampled, 75% of sampled rows must
 * match, 32-row floor, offsets 8 16 24 32 4 2 1 -8 -16. */
void rfb_scroll_defaults(rfb_scroll_cfg *cfg);

/* cfg may be NULL for the defaults.  shadow must be zeroed for the first
 * frame; the frame then codes as a delta from an all-zero screen, which is
 * what a receiver starts from. */
long rfb_encoder_init(rfb_encoder *e, const rfb_geom *g, rfb_u32 flags,
                      const rfb_scroll_cfg *cfg,
                      rfb_u8 *shadow, rfb_u32 shadow_len,
                      rfb_u8 *scratch, rfb_u32 scratch_len);

long rfb_encode_frame(rfb_encoder *e, const rfb_u8 *src,
                      rfb_u8 *out, rfb_u32 out_cap);

/* The same, for a source whose planes are not one block: planes[p] is the base
 * of plane p and rows within it are row_stride apart.  This is what the Amiga
 * caller uses -- a BitMap's Planes[] are separate allocations -- and it is what
 * lets the encoder read the screen ITSELF rather than a copy of it.
 *
 * A tile whose planes all match the shadow is read once and nothing is written.
 * A tile that differs is copied ONCE into rawbuf, and that one copy is what
 * updates the shadow and what goes on the wire, so the two cannot disagree
 * however much the screen moves underneath -- which matters because the caller
 * reads the planes without holding a drawing lock.
 *
 * rfb_encode_frame() above is this with planes[p] = src + p * plane_stride. */
/*
 * One BAND of a frame: the same message, carrying only the tiles of tile rows
 * [ty0, ty1).
 *
 * It exists because a whole frame is one uninterruptible piece of work, and on
 * a 68030 that is around a fifth of a second during which the caller services
 * nothing -- so a person moving the mouse or typing while a frame is being
 * built waits for it to finish.  A caller that encodes a band at a time can
 * read its socket between them.
 *
 * Nothing on the wire changes and no receiver needs to know.  A message
 * carries the ops it carries; there has never been anything in one that says
 * it covers the whole screen.  The shadow is per tile, so a band leaves it
 * exactly as the same tiles would have left it inside a whole frame.
 *
 * What the caller must not do is interleave two screen passes.  Bands are
 * expected in order, 0 to tiles_y, and the scroll probe runs on the first
 * while the counters that describe a frame are closed by the last.  The
 * SOURCE may move underneath between bands -- that is a torn frame, which
 * this has always allowed and which the next pass corrects.
 */
long rfb_encode_band(rfb_encoder *e, const rfb_u8 *const *planes,
                     rfb_u8 *out, rfb_u32 out_cap,
                     rfb_u16 ty0, rfb_u16 ty1);

long rfb_encode_frame_planes(rfb_encoder *e, const rfb_u8 *const *planes,
                             rfb_u8 *out, rfb_u32 out_cap);

/* PackBits, exposed because the decoder and the tests want the same pair. */
rfb_u32 rfb_packbits(const rfb_u8 *in, rfb_u32 n, rfb_u8 *out, rfb_u32 cap,
                     int min_run);
long rfb_unpackbits(const rfb_u8 *in, rfb_u32 n, rfb_u8 *out, rfb_u32 out_n);

#ifdef __cplusplus
}
#endif

#endif /* AMINETXDUO_RFB_ENCODE_H */

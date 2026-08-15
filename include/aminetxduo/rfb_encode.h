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
 * PackBits is the IFF ILBM byte RLE: n in 0..127 copies n+1 literal bytes,
 * n in 129..255 repeats the next byte 257-n times, 128 is a no-op and is never
 * emitted.  A tile is one continuous byte stream across its rows, not one run
 * per row.
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

#define RFB_CODE_RAW     0
#define RFB_CODE_PB_RAW  1
#define RFB_CODE_PB_XOR  2

#define RFB_MAX_DEPTH    8
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
    rfb_u16 bytes_per_row;  /* from the BitMap */
    rfb_u8  depth;          /* 1..8 */
    rfb_u8  tile_w;         /* tile width in BYTES, 1..RFB_MAX_TILE_W */
    rfb_u8  tile_h;         /* tile height in rows, 1..RFB_MAX_TILE_H */
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

    rfb_stats st;
} rfb_encoder;

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

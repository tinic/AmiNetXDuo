/*
 * ZZ9000 console-encode offload -- the host half.
 *
 * When a ZZ9000 is present with zz9k.library (SDK v2) and the console-encode
 * vendor service loaded, httpfb.c hands framebuffer encoding to the card: the
 * Zynq reads its own framebuffer locally (no Zorro readback) and runs the RFB
 * encoder there, returning the compact stream the browser already decodes.
 * Everything here is optional -- if the library, the framebuffer-surface
 * capability, or the service is missing, httpzz_available() is FALSE and
 * httpfb.c keeps the existing httprtg.c readback + rfb_encode path.
 *
 * The ARM module and this file share ONE contract: the 0x8200 vendor service
 * and the request/reply below.  See docs/plans/zz9000-console-offload.md and
 * third_party/zz9000-sdk/docs/zz9k-vendor-services.md.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_HTTPZZ_H
#define AMINETXDUO_HTTPZZ_H

#include <exec/types.h>

/* Vendor service id (0x8000+ range; JEDI owns 0x8100).  Recorded in the SDK
   vendor table as the AmiNetXDuo console encoder. */
#define HTTPZZ_SERVICE_ID       0x8200UL
#define HTTPZZ_OP_ENCODE        (HTTPZZ_SERVICE_ID + 0x00UL)

/* Wire codec wrapped around the RFB stream.  v1 ships NONE; LZ4/zstd are a
   measured bandwidth refinement (browser must advertise a decoder). */
#define HTTPZZ_CODEC_NONE       0
#define HTTPZZ_CODEC_LZ4        1
#define HTTPZZ_CODEC_ZSTD       2
#define HTTPZZ_CODEC_DEFLATE    3

/* Request flag bits (HttpZzEncodeReq.flags). */
#define HTTPZZ_F_RESET          0x0001U   /* drop the delta baseline first (a fresh screen) */

/* Reply flag bits (HttpZzEncodeReply.flags). */
#define HTTPZZ_RF_KEYFRAME      0x0001U   /* the answer is a full frame          */

/*
 * The 0x8200 request payload, carried inline in the mailbox entry.  BIG-ENDIAN
 * on the wire (68k native): a vendor payload is opaque to zz9k.library, which
 * passes the bytes through, so the ARM module byte-swaps on read.  Both halves
 * are ours, so the contract is ours to fix.
 *
 * The field order -- 4-byte, then 2-byte, then 1-byte -- is deliberate: every
 * field lands on its natural boundary with no internal padding, so the host's
 * raw struct copy and the ARM's fixed-offset decode (which does not rely on the
 * ARM compiler's struct layout) see the same bytes.  Keep it that way.
 */
typedef struct HttpZzEncodeReq
{
    ULONG  surface_handle;   /* from ZZ9KMapFramebufferSurface (the pixel base) */
    ULONG  out_handle;       /* shared buffer the encoder writes into          */
    ULONG  out_capacity;     /* its length                                     */
    ULONG  enc_flags;        /* rfb_encoder_init() flags (RFB_F_*): the host's */
    UWORD  width;            /* rfb_geom, verbatim from the host's encoder     */
    UWORD  height;           /*   configuration -- see httpzz_configure().  The*/
    UWORD  bytes_per_row;    /*   card frames bands from THESE, not from the   */
    UWORD  ty0, ty1;         /*   surface, so tiles_x/tiles_y and every delta  */
    UWORD  flags;            /*   match the host byte for byte.  HTTPZZ_F_*    */
    UWORD  codec;            /* requested wire codec (HTTPZZ_CODEC_*)          */
    UBYTE  depth;            /* rfb_geom.depth (plane count; 1 for chunky)     */
    UBYTE  tile_w;           /* rfb_geom.tile_w, in BYTES                      */
    UBYTE  tile_h;           /* rfb_geom.tile_h, in rows                       */
    UBYTE  fmt;              /* rfb_geom.format (RFB_FMT_*)                     */
} HttpZzEncodeReq;

typedef struct HttpZzEncodeReply
{
    ULONG  out_len;          /* bytes the encoder wrote into the shared buffer */
    UWORD  codec;            /* codec actually used                            */
    UWORD  flags;            /* HTTPZZ_RF_*                                    */
} HttpZzEncodeReply;

/* TRUE when the offload can run now: zz9k.library >= v2 opened, the framebuffer
   surface capability is advertised, and the 0x8200 service is registered.
   Probed once and cached; safe to call every frame. */
BOOL httpzz_available(VOID);

/*
 * Hand the card the host encoder's exact geometry and flags before the first
 * band of a screen (httpfb.c calls this from fb_take_buffers with fb_rg and
 * fb_flags).  The card configures an identical rfb_encoder, so its band framing
 * and deltas match the host's to the byte; the scroll config is the shared
 * rfb_scroll_defaults() on both sides and so is not sent.  Implies a reset: the
 * next band is a keyframe.
 */
VOID httpzz_configure(UWORD width, UWORD height, UWORD bytes_per_row,
                      UBYTE depth, UBYTE tile_w, UBYTE tile_h, UBYTE fmt,
                      ULONG enc_flags);

/*
 * Encode tile-row band [ty0,ty1) of the displayed framebuffer on the card --
 * the same banding rfb_encode_band() uses -- and copy the RFB bytes into
 * out[0..out_max).  The card holds the delta baseline; httpzz_reset() makes the
 * next band a keyframe.  Returns bytes written, or -1 when the offload could
 * not run this band (the caller then falls back).  *codec_out gets the wire
 * codec used.
 */
LONG httpzz_encode(UWORD ty0, UWORD ty1, UBYTE *out, ULONG out_max,
                   UWORD *codec_out);

/* Drop the delta baseline; the next httpzz_encode is a keyframe.  Called when
   the screen, its geometry, or its format changes. */
VOID httpzz_reset(VOID);

/* Free the shared buffer and close the library.  Safe to call when nothing was
   opened. */
VOID httpzz_cleanup(VOID);

#endif /* AMINETXDUO_HTTPZZ_H */

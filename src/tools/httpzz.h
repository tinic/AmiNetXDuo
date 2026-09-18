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
#define HTTPZZ_F_KEEP_DELTA     0x0001U   /* delta against the card's prev frame */

/* Reply flag bits (HttpZzEncodeReply.flags). */
#define HTTPZZ_RF_KEYFRAME      0x0001U   /* the answer is a full frame          */

/*
 * The 0x8200 request payload, carried inline in the mailbox entry.  BIG-ENDIAN
 * on the wire (68k native): a vendor payload is opaque to zz9k.library, which
 * passes the bytes through, so the ARM module byte-swaps on read.  Both halves
 * are ours, so the contract is ours to fix.
 */
typedef struct HttpZzEncodeReq
{
    ULONG  surface_handle;   /* from ZZ9KMapFramebufferSurface                 */
    ULONG  out_handle;       /* shared buffer the encoder writes into          */
    ULONG  out_capacity;     /* its length                                     */
    UWORD  x, y, w, h;       /* the rectangle to encode                        */
    UWORD  flags;            /* HTTPZZ_F_*                                     */
    UWORD  codec;            /* requested wire codec (HTTPZZ_CODEC_*)          */
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
 * Encode rect (x,y,w,h) of the displayed framebuffer on the card and copy the
 * result into out[0..out_max).  keep_delta asks the card to diff against the
 * frame it is holding; otherwise it produces a keyframe.  Returns the number of
 * bytes written, or -1 when the offload could not run this frame (the caller
 * then falls back).  *codec_out receives the wire codec used.
 */
LONG httpzz_encode(UWORD x, UWORD y, UWORD w, UWORD h, BOOL keep_delta,
                   UBYTE *out, ULONG out_max, UWORD *codec_out);

/* Drop the delta baseline; the next httpzz_encode is a keyframe.  Called when
   the screen, its geometry, or its format changes. */
VOID httpzz_reset(VOID);

/* Free the shared buffer and close the library.  Safe to call when nothing was
   opened. */
VOID httpzz_cleanup(VOID);

#endif /* AMINETXDUO_HTTPZZ_H */

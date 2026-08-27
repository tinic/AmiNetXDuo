/*
 * Reading a graphics card's framebuffer back, for the console.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_HTTPRTG_H
#define AMINETXDUO_HTTPRTG_H

#include "tools.h"

struct BitMap;
struct RastPort;

/* What a card's bitmap turned out to be.  Everything here is answered without
   locking the bitmap, so it can be read under LockIBase(). */
typedef struct HttpRtgScreen
{
    UWORD width;        /* pixels */
    UWORD height;
    UWORD depth;        /* bits a pixel, as the card reports it */
    UBYTE bpp;          /* bytes a pixel in the staging buffer the caller sets
                           up and http_rtg_read() fills: 1 for a palette
                           screen, one byte an index, and 2 for every
                           truecolour one, big-endian R5G6B5 */
} HttpRtgScreen;

BOOL  http_rtg_open(VOID);
VOID  http_rtg_close(VOID);
BOOL  http_rtg_present(VOID);

/* TRUE when this bitmap belongs to Picasso96 or CyberGraphX.  Asked before
   BMF_STANDARD, because a card's bitmap can carry that flag and its Planes[]
   are not bitplanes. */
BOOL  http_rtg_owns(struct BitMap *bm);

/* `visible_w` is the screen's own width, NOT the bitmap's: an RTG board rounds
   an allocation up to its pitch, so columns past the edge hold stale pixels.
   Pass 0 when there is no screen to ask. */
BOOL  http_rtg_describe(struct BitMap *bm, UWORD visible_w, HttpRtgScreen *s,
                        const char **why);

BOOL  http_rtg_attach(struct BitMap *bm, struct RastPort *rp,
                      UWORD width, UWORD height, ULONG stride, UBYTE *probe);
VOID  http_rtg_detach(VOID);

/* Whether the measured routes and any snapshot bitmap belong to `bm`.
   Equal wire geometry is not enough: two same-sized screens can have
   different native formats or live on different boards. */
BOOL  http_rtg_attached_to(struct BitMap *bm);

/* Rows [y0, y0+rows) of the card's screen into `dst`, which is the base of the
   whole staging frame and not the band: every row lands at the attach stride,
   so a band read leaves the rest of the buffer as it was.  `rows` past the
   bottom is clamped, because a tile grid rounds the height up. */
BOOL  http_rtg_read(struct BitMap *bm, struct RastPort *rp, UBYTE *dst,
                    UWORD y0, UWORD rows);

ULONG http_rtg_word(char *out, ULONG cap);

#endif /* AMINETXDUO_HTTPRTG_H */

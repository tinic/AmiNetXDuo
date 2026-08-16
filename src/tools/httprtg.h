/*
 * Reading a graphics card's framebuffer back, for the console.
 *
 * This is not the planar path with a different loop.  On the chipset the
 * console's constraint is the CPU: a 68020 comparing 40 KB of chip RAM against
 * a shadow, which is why httpfb.c is arranged to do as little of that as it
 * can.  On a card the machine is a 040 or a 060 with CPU to spare and the
 * constraint moves to readback.  Graphics cards are built to be written to.
 * Reading VRAM back over Zorro III is slow, and how slow depends on the board,
 * on the driver and on which of several routes the read takes.  Nobody has
 * published that number for an Amiga card.
 *
 * So this module is a measurement as much as it is a reader.  It times every
 * readback route the machine offers, once, at session start, uses the fastest,
 * and reports what each one managed.  See http_rtg_word(), which sends the
 * report so the figure can be pasted back.
 *
 * It does not hook, patch or wrap anything.  The BoardInfo route, catching the
 * driver's own drawing primitives, cannot be complete by construction:
 * Picasso96's driver documentation says rtg.library "always provides a safe
 * default which performs the corresponding graphics primitive by the CPU", so
 * every operation a board's driver does not implement is done by the CPU
 * writing VRAM directly, and no API-level hook sees it.  A hooked mirror
 * misses whatever a particular board does not accelerate.
 *
 * It serves 8-bit palette-indexed screens, and nothing else yet.  Those are
 * chunky the way the planar path is planar, a byte is an index, so the
 * encoder's tile grid, PackBits, XOR and copy-rectangle all apply unchanged
 * with the plane count set to one.  15, 16, 24 and 32-bit screens are refused
 * by name.  The encoder has no truecolour code, so those depths get a sentence
 * instead of a picture.
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
    UWORD width;
    UWORD height;
    UWORD depth;        /* bits a pixel, as the card reports it */
    UBYTE clut8;        /* 8-bit palette indexed: the one this serves */
} HttpRtgScreen;

/* Both libraries, if either is there.  TRUE when at least one opened, which
   is what decides whether an RTG screen can be served at all. */
BOOL  http_rtg_open(VOID);
VOID  http_rtg_close(VOID);
BOOL  http_rtg_present(VOID);

/* TRUE when this bitmap belongs to Picasso96 or CyberGraphX.  Asked before
   BMF_STANDARD, because a card's bitmap can carry that flag and its Planes[]
   are not bitplanes. */
BOOL  http_rtg_owns(struct BitMap *bm);

/* Shape and format, or FALSE with *why set to a sentence naming what it is.
   Only 8-bit palette-indexed comes back TRUE. */
BOOL  http_rtg_describe(struct BitMap *bm, HttpRtgScreen *s, const char **why);

/*
 * Take the session: work out whether the bitmap is in card memory or in
 * system RAM, allocate what the snapshot route needs, then time every readback
 * route this machine offers and keep the fastest.
 *
 * `stride` is the row stride of the caller's staging buffer, which is what
 * every route delivers into.  `probe` is that buffer, used as scratch here
 * because it holds nothing yet.  FALSE when no route worked at all.
 */
BOOL  http_rtg_attach(struct BitMap *bm, struct RastPort *rp,
                      UWORD width, UWORD height, ULONG stride, UBYTE *probe);
VOID  http_rtg_detach(VOID);

/* One whole frame into `dst`, contiguous full rows at the attach stride.
   Rows and not tiles: adjacent reads coalesce and a loop of small rectangles
   is the shape that does not. */
BOOL  http_rtg_read(struct BitMap *bm, struct RastPort *rp, UBYTE *dst);

/*
 * What the probe measured, as a control word: which libraries answered,
 * whether the bitmap is on the board, which route won and what every route
 * managed in KB/s.  Sent once a session and logged by the viewer.
 */
ULONG http_rtg_word(char *out, ULONG cap);

#endif /* AMINETXDUO_HTTPRTG_H */

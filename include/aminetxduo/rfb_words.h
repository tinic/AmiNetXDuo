/* The control words either end of a remote-framebuffer session sends.
 *
 * Text frames are control and binary frames are the data stream; that split is
 * the whole convention, and rfb_encode.h documents the binary half.  This is
 * the other one, and it is here rather than in the server because it is what
 * the browser has to agree with byte for byte: the receiver's spelling is in
 * src/tools/web/client/console/tiles.ts and src/tools/web/client/console/
 * input.ts, and a host test can compile this and check the strings against
 * them without an Amiga in the room.
 *
 * Server to client:
 *
 *   geom W H DEPTH BYTESPERROW TILEWBYTES TILEH FORMAT
 *                     seven decimal numbers, one space between each.  Sent
 *                     when the session opens and again whenever the screen
 *                     changes under it.  TILEWBYTES is BYTES and not pixels,
 *                     and the tile grid is over BYTESPERROW rather than W:
 *                     every byte of a row is encoded, padding included.
 *                     FORMAT is rfb_geom.format, and it says what a pixel
 *                     means:
 *                       0 planar, DEPTH one-bit planes, a pixel is a palette
 *                         index.
 *                       1 chunky, ONE eight-bit plane, a byte is a palette
 *                         index, DEPTH is 8.
 *                       2 truecolour, ONE plane of two-byte pixels,
 *                         big-endian ((r5 << 11) | (g6 << 5) | b5), DEPTH is
 *                         16 and is bits a pixel.  NO `pal` FOLLOWS ONE OF
 *                         THESE, ever; see below.
 *                     It decides which tile op the binary frames carry and
 *                     how a byte column turns into a pixel column, so a
 *                     viewer that ignored it would draw a chunky screen eight
 *                     times too wide.
 *                     A number rather than a flag because the list is going
 *                     to grow: the chipset modes where a plane is not a
 *                     palette index -- HAM6, HAM8, extra half-brite -- are
 *                     distinct pixel meanings over the same planar layout,
 *                     and each is a value here.  An unrecognised FORMAT is
 *                     refused by the viewer rather than drawn, which is the
 *                     one place the ignore-what-you-do-not-know rule below
 *                     does not apply: a format guessed wrong is a picture
 *                     made of the wrong bytes and it looks like a fault in
 *                     the server.
 *                     It is also the stream's only barrier: both ends zero
 *                     their shadow on it, and the next frame is a full one.
 *                     Frames still in flight when it arrives belong to the
 *                     picture it replaces and are discarded with it.
 *   pal RRGGBB...     hex, one triple per colour.  Sent after geom and again
 *                     whenever the ColorMap moves.  A geom leaves the viewer
 *                     on a grey palette until one arrives, so a geom that
 *                     wants a palette is always followed by a pal.
 *                     HOW MANY COLOURS IS A PROPERTY OF THE FORMAT AND NOT OF
 *                     THE DEPTH.  rfb_pal_colours() is the rule and both ends
 *                     use it; 1 << DEPTH is right only on FORMAT 0.  A format
 *                     whose answer is 0 -- truecolour today -- sends no `pal`
 *                     at all, and a viewer that waits for one before it draws
 *                     shows nothing for the whole session.  The modes coming
 *                     after truecolour are the other side of the same point:
 *                     HAM6 is six planes deep with sixteen base colours and
 *                     HAM8 is eight deep with sixty-four, so both send a
 *                     `pal` far shorter than their depth implies.
 *   ptr W H DEPTH XS YS HOTX HOTY RRGGBB... BITS
 *                     the mouse pointer's IMAGE, which is a hardware sprite
 *                     and is therefore in no frame this ever sends.  Seven
 *                     decimal numbers, then the sprite's own colours as hex
 *                     triples -- (1 << DEPTH) - 1 of them, because index 0 is
 *                     transparent -- then the planar bits as hex, plane-major,
 *                     DEPTH * ((W + 15) / 16 * 2) * H bytes of them.
 *                     W and H are SPRITE pixels and XS and YS are how many
 *                     screen pixels one sprite pixel covers across and down: a
 *                     sprite pixel is a lores pixel whatever the screen is.
 *                     HOTX and HOTY are the hotspot, in sprite pixels.
 *                     Sent when the session opens and again only when the
 *                     image changes, which is almost never -- it is the
 *                     viewer that tracks the mouse, from its own pointer, so
 *                     nothing here is sent per movement.
 *
 * Client to server:
 *
 *   refresh           forget the shadow.  What a viewer sends when a frame was
 *                     lost, because every XOR after a gap is applied to bytes
 *                     that are not what the encoder thought were there.
 *                     Answered with geom, then pal, then a full frame, so the
 *                     shadow is zeroed at both ends at one point in the stream
 *                     rather than at two.
 *                     NEVER send it on `geom`: the answer to a refresh is a
 *                     geom, so that is a loop, and it shipped once as a viewer
 *                     that went blank one frame after every reconnect.
 *                     A refresh arriving while one is already in flight is
 *                     coalesced, and they are floored at one a second; a
 *                     request inside that window is remembered and honoured
 *                     when it passes, not dropped.
 *   reset             reboot the Amiga.  Answered with a close frame saying so
 *                     and nothing else: the machine goes, so there is no later
 *                     word to send.  The server flushes every mounted volume
 *                     before it calls ColdReboot(), which is as much as an
 *                     Amiga can do about work in flight -- a program holding
 *                     unsaved data in memory loses it, and the viewer says so
 *                     before it sends this.
 *                     Only the session holding the screen may send it; there
 *                     is no authentication in front of that, and anyone who
 *                     can reach the port holds it as soon as nobody else does.
 *   m X Y BUTTONS     pointer, screen pixels.  1 left, 2 right, 4 middle.
 *   w DX DY           wheel, in notches, either sign.
 *   kd RAW QUAL       key down, Amiga rawkey code and qualifier bits.
 *   ku RAW QUAL       key up.
 *
 * An unrecognised word is IGNORED at both ends and is never an error, which is
 * what lets either side learn a word without the other being rebuilt on the
 * same afternoon.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_RFB_WORDS_H
#define AMINETXDUO_RFB_WORDS_H

#include "aminetxduo/rfb_encode.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The longest `geom` there is: seven numbers of five digits, seven spaces and
 * the keyword.  A `pal` is 4 + 6 * (1 << depth) and the caller sizes that one
 * from the depth it has. */
#define RFB_WORD_GEOM_MAX   51
#define RFB_WORD_PAL_MAX    (5u + 6u * (1u << RFB_MAX_DEPTH))

/*
 * What a pointer sprite may be, which is what sizes the `ptr` word.
 *
 * 64 wide and 64 high is four times the classic 16x16 in each direction and
 * more than any pointer a 3.x machine has been seen to carry; an image past it
 * is REFUSED rather than truncated, because half a pointer drawn at the wrong
 * scale is worse than the viewer's own arrow.  Four planes is the same
 * judgement: a 16-colour pointer is an AGA extreme and anything past it is not
 * something to guess the colour registers for.
 */
#define RFB_PTR_MAX_W       64u
#define RFB_PTR_MAX_H       64u
#define RFB_PTR_MAX_DEPTH   4u
#define RFB_PTR_ROW_BYTES(w) ((((w) + 15u) / 16u) * 2u)
#define RFB_PTR_MAX_BITS    (RFB_PTR_MAX_DEPTH * \
                             RFB_PTR_ROW_BYTES(RFB_PTR_MAX_W) * RFB_PTR_MAX_H)
#define RFB_PTR_MAX_COLOURS ((1u << RFB_PTR_MAX_DEPTH) - 1u)

/* "ptr " and seven numbers with their spaces, then two hex digits a byte. */
#define RFB_WORD_PTR_MAX    (4u + 7u * 7u + \
                             2u * (3u * RFB_PTR_MAX_COLOURS + RFB_PTR_MAX_BITS))

enum {
    RFB_IN_NONE = 0,
    RFB_IN_REFRESH,
    RFB_IN_POINTER,     /* a = x, b = y, c = button bitmask               */
    RFB_IN_WHEEL,       /* a = dx, b = dy, notches, either sign           */
    RFB_IN_KEYDOWN,     /* a = rawkey, b = qualifier bits                 */
    RFB_IN_KEYUP,
    RFB_IN_RESET        /* reboot the machine                             */
};

/* The browser's own bitmask, which is also the order IEQUALIFIER_ puts them
 * in. */
#define RFB_BUTTON_LEFT     0x01u
#define RFB_BUTTON_RIGHT    0x02u
#define RFB_BUTTON_MIDDLE   0x04u

typedef struct {
    rfb_u8  kind;
    rfb_s32 a;
    rfb_s32 b;
    rfb_s32 c;
} rfb_input;

/* Characters written, not counting the terminator, or 0 when it would not
 * fit.  Both write a NUL. */
rfb_u32 rfb_word_geom(char *out, rfb_u32 cap, const rfb_geom *g);
rfb_u32 rfb_word_pal(char *out, rfb_u32 cap, const rfb_u8 *rgb, rfb_u32 colours);

/*
 * The pointer image.  `rgb` is 3 * ((1 << depth) - 1) bytes and `bits` is
 * depth * RFB_PTR_ROW_BYTES(width) * height, plane-major.  0 when the shape is
 * one this refuses or the buffer is too small.
 */
typedef struct {
    rfb_u16 width;
    rfb_u16 height;
    rfb_u16 depth;
    rfb_u16 x_scale;    /* screen pixels one sprite pixel covers across */
    rfb_u16 y_scale;    /* screen rows one sprite row covers            */
    rfb_s16 hot_x;      /* sprite pixels from the left                  */
    rfb_s16 hot_y;
} rfb_pointer;

rfb_u32 rfb_word_ptr(char *out, rfb_u32 cap, const rfb_pointer *p,
                     const rfb_u8 *rgb, const rfb_u8 *bits);

/* Fills `ev` and returns 1 when `w` is a word this knows, 0 when it is not.
 * `w` need not be terminated; `len` is what arrived in the text frame.  A word
 * whose numbers are missing or malformed is not one of ours and returns 0
 * rather than acting on half of it. */
int rfb_word_parse(const char *w, rfb_u32 len, rfb_input *ev);

#ifdef __cplusplus
}
#endif

#endif /* AMINETXDUO_RFB_WORDS_H */

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
 *   geom W H DEPTH BYTESPERROW TILEWBYTES TILEH
 *                     six decimal numbers, one space between each.  Sent when
 *                     the session opens and again whenever the screen changes
 *                     under it.  TILEWBYTES is BYTES and not pixels, and the
 *                     tile grid is over BYTESPERROW rather than W: every byte
 *                     of a row is encoded, padding included.
 *                     It is also the stream's only barrier: both ends zero
 *                     their shadow on it, and the next frame is a full one.
 *                     Frames still in flight when it arrives belong to the
 *                     picture it replaces and are discarded with it.
 *   pal RRGGBB...     hex, one triple per colour, 3 << DEPTH bytes of it.
 *                     Sent after geom and again whenever the ColorMap moves.
 *                     A geom leaves the viewer on a grey palette until one
 *                     arrives, so a geom is always followed by a pal.
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

/* The longest `geom` there is: six numbers of five digits, five spaces and the
 * keyword.  A `pal` is 4 + 6 * (1 << depth) and the caller sizes that one from
 * the depth it has. */
#define RFB_WORD_GEOM_MAX   44
#define RFB_WORD_PAL_MAX    (5u + 6u * (1u << RFB_MAX_DEPTH))

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

/* Fills `ev` and returns 1 when `w` is a word this knows, 0 when it is not.
 * `w` need not be terminated; `len` is what arrived in the text frame.  A word
 * whose numbers are missing or malformed is not one of ours and returns 0
 * rather than acting on half of it. */
int rfb_word_parse(const char *w, rfb_u32 len, rfb_input *ev);

#ifdef __cplusplus
}
#endif

#endif /* AMINETXDUO_RFB_WORDS_H */

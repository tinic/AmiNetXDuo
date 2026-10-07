/*
 * Text on the Amiga clipboard and text in a browser.  The clipboard holds IFF:
 * a FORM FTXT with CHRS chunks of ISO-8859-1, lines ending in LF.  A browser
 * has UTF-8, often with CRLF.  These convert between the two and build and
 * read the IFF; clipboard.device itself is the caller's (httpfb.c).  No
 * AmigaOS calls, so src/tools/test/test_clipftxt.c runs it on the build host.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef CLIPFTXT_H
#define CLIPFTXT_H

#include <stdint.h>

/* FORM, size, FTXT, CHRS, size. */
#define CLIP_FTXT_HEADER    20u

/*
 * UTF-8 to ISO-8859-1, at most `cap` bytes out.  CRLF and a lone CR become LF;
 * a character above U+00FF and a malformed sequence become '?'.  `out` may be
 * `in`: the result is never longer than what it was made from.
 */
uint32_t clip_utf8_to_latin1(const uint8_t *in, uint32_t n,
                             uint8_t *out, uint32_t cap);

/* ISO-8859-1 to UTF-8, at most `cap` bytes out, never splitting a character. */
uint32_t clip_latin1_to_utf8(const uint8_t *in, uint32_t n,
                             uint8_t *out, uint32_t cap);

/* The 20-byte header for `n` bytes of text; the caller writes the text after
   it, and one zero byte when `n` is odd. */
void clip_ftxt_header(uint32_t n, uint8_t out[CLIP_FTXT_HEADER]);

/*
 * The text of a FORM FTXT: every CHRS chunk, in order, at most `cap` bytes.
 * Other chunks are skipped.  0 for anything that is not an FTXT, and a chunk
 * that claims more than the buffer holds is read as far as it goes.
 */
uint32_t clip_ftxt_text(const uint8_t *iff, uint32_t n,
                        uint8_t *out, uint32_t cap);

#endif /* CLIPFTXT_H */

/*
 * Text between the machine's 8-bit codeset and UTF-8, for ssh, httpd's
 * WebDAV names, its Shell page and its clipboard.  No AmigaOS calls:
 * src/common/test/test_textconv.c runs it on the build host.  The codeset
 * itself comes from src/common/textconv_cs.c (codesets.library when it opens,
 * ISO-8859-1 otherwise).
 *
 * Bytes 0x80-0x9F are the C1 controls U+0080-U+009F both ways, whatever the
 * codeset: 0x9B is the console's CSI and must stay one.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_TEXTCONV_H
#define AMINETXDUO_TEXTCONV_H

#include <stddef.h>
#include <stdint.h>

#define TXT_CHAR_MAX    8u          /* most bytes one decoded character makes */

/*
 * The local codeset: upper[i] is the code point of byte 0x80 + i, 0 when the
 * byte has none (only 0x20-0x7F are read).  NULL, or a NULL codeset, is
 * ISO-8859-1.  foreign() is asked for a code point no byte has (codesets'
 * look-alike text); NULL, or 0 back, is '?'.
 */
/* foreign() is pinned to the stack like every shared function-pointer type
   (tools/check-call-abi.py); __stdargs on a GCC for AmigaOS, nothing on a
   host compiler, which passes on the stack already. */
#if defined(__GNUC__) && defined(__stdargs)
#define TXT_STDARGS __stdargs
#else
#define TXT_STDARGS
#endif

typedef TXT_STDARGS size_t (*txt_foreign_fn)(void *ctx, uint32_t cp,
                                             uint8_t *out, size_t cap);

typedef struct txt_codeset
{
    const uint16_t *upper;
    txt_foreign_fn  foreign;
    void           *ctx;
} txt_codeset;

/* A UTF-8 decoder that keeps a sequence across buffers.  Zero it; c1 set
   keeps U+0080-U+009F as bytes 0x80-0x9F, clear makes them '?' (a console). */
typedef struct txt_dec
{
    uint32_t cp;
    uint8_t  need;
    uint8_t  len;
    uint8_t  c1;
} txt_dec;

/* One byte of UTF-8: up to TXT_CHAR_MAX local bytes out.  Invalid UTF-8 and
   a sequence cut short are '?'. */
size_t txt_dec_byte(txt_dec *d, const txt_codeset *cs, uint8_t b, uint8_t *out);

/* A sequence still open: '?' and the decoder reset.  0 when there is none. */
size_t txt_dec_flush(txt_dec *d, uint8_t *out);

/* A whole UTF-8 string, at most cap bytes, never splitting a character. */
size_t txt_utf8_to_local(const txt_codeset *cs, const uint8_t *in, size_t n,
                         uint8_t *out, size_t cap);

/* One local byte to UTF-8: 1-3 bytes. */
size_t txt_local_to_utf8(const txt_codeset *cs, uint8_t b, uint8_t out[3]);

/* A local string to UTF-8, at most cap bytes, never splitting a character;
   *used is how much of in went. */
size_t txt_local_to_utf8_str(const txt_codeset *cs, const uint8_t *in, size_t n,
                             uint8_t *out, size_t cap, size_t *used);

/*
 * UTF-8 to local exactly, or not at all: a name, where a look-alike would
 * be another file.  out may be in.  *outlen on success.
 */
#define TXT_OK          0
#define TXT_BAD_UTF8    1
#define TXT_UNMAPPABLE  2
#define TXT_TOO_LONG    3
int txt_utf8_to_local_exact(const txt_codeset *cs, const uint8_t *in, size_t n,
                            uint8_t *out, size_t cap, size_t *outlen);

/* AmigaOS half (textconv_cs.c).  The machine's codeset, or `name` when
   codesets.library knows it; never NULL.  *flags says what happened.  Close
   at exit; the codeset is valid until then. */
#define TXT_CS_LIBRARY  1           /* codesets.library opened             */
#define TXT_CS_UTF8     2           /* the local codeset is UTF-8 already  */
#define TXT_CS_UNKNOWN  4           /* `name` was not found                */
const txt_codeset *txt_codeset_open(const char *name, int *flags);
void               txt_codeset_close(void);

#endif /* AMINETXDUO_TEXTCONV_H */

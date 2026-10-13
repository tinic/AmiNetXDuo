/*
 * The ssh client's console text: the server's UTF-8 to the local 8-bit
 * codeset, keystrokes back to UTF-8, and OSC sequences taken out of the
 * server's stream (OSC 0 and 2 become the window title).  No AmigaOS calls;
 * tests/tools/host/test_sshcs_host.c runs it on the build host, and
 * amiga_dropbear.c supplies the codeset and the title.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef SSHCS_H
#define SSHCS_H

#include <stddef.h>
#include <stdint.h>

#define SSHCS_TITLE_MAX   256u     /* longer titles are dropped, not cut */
#define SSHCS_CHAR_MAX    8u       /* most bytes one character becomes  */

/*
 * The local codeset: upper[i] is the code point of byte 0x80 + i, 0 when the
 * byte has none.  NULL is ISO-8859-1.  foreign() is asked for a code point no
 * byte has (codesets.library's look-alike text); NULL, or 0 back, is '?'.
 */
typedef struct sshcs_codeset
{
    const uint16_t *upper;
    size_t        (*foreign)(void *ctx, uint32_t cp, uint8_t *out, size_t cap);
    void           *ctx;
} sshcs_codeset;

/* One output stream.  Zero it, then set the fields above the state. */
typedef struct sshcs_out
{
    const sshcs_codeset *cs;
    uint8_t   convert;          /* UTF-8 to cs; off passes bytes unchanged   */
    uint8_t   osc;              /* strip OSC; needs title_buf                */
    uint8_t  *title_buf;        /* SSHCS_TITLE_MAX bytes                     */
    void    (*title)(void *ctx, const uint8_t *utf8, size_t len);
    void     *title_ctx;

    uint32_t  cp;               /* UTF-8 sequence so far                     */
    uint8_t   need;             /* continuation bytes still to come          */
    uint8_t   len;              /* its full length, for the overlong check   */
    uint8_t   state;            /* escape state, see sshcs.c                 */
    uint8_t   overflow;
    uint16_t  num;              /* the OSC number                            */
    uint16_t  tlen;
} sshcs_out;

/*
 * Convert in[0..n) into out[0..cap), cap >= SSHCS_CHAR_MAX.  Stops early when
 * out has less than SSHCS_CHAR_MAX bytes left; *used is what was consumed and
 * the return value what was written.  State carries across calls.
 */
size_t sshcs_out_run(sshcs_out *s, const uint8_t *in, size_t n,
                     uint8_t *out, size_t cap, size_t *used);

/* A whole UTF-8 string (a title) to cs, at most cap bytes, never splitting a
   character.  An unfinished sequence at the end is '?'. */
size_t sshcs_utf8_to_local(const sshcs_codeset *cs, const uint8_t *in,
                           size_t n, uint8_t *out, size_t cap);

/* One local byte (a keystroke) to UTF-8: 1-3 bytes into out. */
size_t sshcs_local_to_utf8(const sshcs_codeset *cs, uint8_t b, uint8_t out[3]);

#endif /* SSHCS_H */

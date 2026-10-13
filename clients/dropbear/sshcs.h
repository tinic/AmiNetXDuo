/*
 * The ssh client's console text: the server's UTF-8 to the local 8-bit
 * codeset (aminetxduo/textconv.h) and OSC sequences taken out of the
 * server's stream (OSC 0 and 2 become the window title).  No AmigaOS calls;
 * tests/tools/host/test_sshcs_host.c runs it on the build host, and
 * amiga_dropbear.c supplies the codeset and the title.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef SSHCS_H
#define SSHCS_H

#include "aminetxduo/textconv.h"

#define SSHCS_TITLE_MAX   256u     /* longer titles are dropped, not cut */
#define SSHCS_CHAR_MAX    TXT_CHAR_MAX

/* One output stream.  Zero it, then set the fields above the state. */
typedef struct sshcs_out
{
    const txt_codeset *cs;
    uint8_t   convert;          /* UTF-8 to cs; off passes bytes unchanged   */
    uint8_t   osc;              /* strip OSC; needs title_buf                */
    uint8_t  *title_buf;        /* SSHCS_TITLE_MAX bytes                     */
    void    (*title)(void *ctx, const uint8_t *utf8, size_t len);
    void     *title_ctx;

    txt_dec   dec;              /* console: C1 is '?'                        */
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
   character; control codes are '?'. */
size_t sshcs_utf8_to_local(const txt_codeset *cs, const uint8_t *in,
                           size_t n, uint8_t *out, size_t cap);

#endif /* SSHCS_H */

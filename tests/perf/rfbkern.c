/*
 * The candidate inner loops for the framebuffer diff, priced one against
 * another on the machine.  Compiled TWICE into rfbprof, once with the tree's
 * shipping -m68000 codegen and once with -m68020, so the two are in one
 * binary and one run rather than two builds a host's load can separate.
 *
 * RFBK_PFX is the symbol prefix the second copy is compiled with.  Nothing
 * here is Amiga-specific: it is C99 over three pointers, which is the point,
 * the shapes measured are the shapes rfb_encode.c could adopt.
 *
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#ifdef RFBK_020
#define RFBK_PFX(n) rfbk020_##n
#else
#define RFBK_PFX(n) rfbk_##n
#endif

typedef unsigned long  rfbk_u32;
typedef unsigned char  rfbk_u8;

typedef char rfbk_u32_is_four[(sizeof(rfbk_u32) == 4) ? 1 : -1];

/* The shape rfb_diff_plane has today: read source, XOR against the shadow, OR
 * into an accumulator, store the source into the shadow.  Linear over a whole
 * plane rather than a 16-byte tile row, so this is the BEST the current shape
 * could do -- it pays none of the per-row setup the real one pays. */
rfbk_u32 RFBK_PFX(xorstore)(const rfbk_u8 *s, rfbk_u8 *d, rfbk_u32 n)
{
    const rfbk_u32 *sp = (const rfbk_u32 *)(const void *)s;
    rfbk_u32 *dp = (rfbk_u32 *)(void *)d;
    rfbk_u32 acc = 0, i = n >> 2;

    while (i--) {
        rfbk_u32 sv = *sp++;
        acc |= sv ^ *dp;
        *dp++ = sv;
    }
    return acc;
}

/* The same, keeping the XOR in a third buffer -- what the encoder does when
 * RFB_F_XOR is on, which it is. */
rfbk_u32 RFBK_PFX(xorkeep)(const rfbk_u8 *s, rfbk_u8 *d, rfbk_u8 *x, rfbk_u32 n)
{
    const rfbk_u32 *sp = (const rfbk_u32 *)(const void *)s;
    rfbk_u32 *dp = (rfbk_u32 *)(void *)d;
    rfbk_u32 *xp = (rfbk_u32 *)(void *)x;
    rfbk_u32 acc = 0, i = n >> 2;

    while (i--) {
        rfbk_u32 sv = *sp++;
        rfbk_u32 xv = sv ^ *dp;
        acc |= xv;
        *dp++ = sv;
        *xp++ = xv;
    }
    return acc;
}

/* Compare only: no store at all.  This is what an unchanged screen could cost
 * if the shadow were only written where it differs. */
rfbk_u32 RFBK_PFX(cmponly)(const rfbk_u8 *s, const rfbk_u8 *d, rfbk_u32 n)
{
    const rfbk_u32 *sp = (const rfbk_u32 *)(const void *)s;
    const rfbk_u32 *dp = (const rfbk_u32 *)(const void *)d;
    rfbk_u32 acc = 0, i = n >> 2;

    while (i--)
        acc |= *sp++ ^ *dp++;
    return acc;
}

/* Compare, and store only where it differs. */
rfbk_u32 RFBK_PFX(cmpstore)(const rfbk_u8 *s, rfbk_u8 *d, rfbk_u32 n)
{
    const rfbk_u32 *sp = (const rfbk_u32 *)(const void *)s;
    rfbk_u32 *dp = (rfbk_u32 *)(void *)d;
    rfbk_u32 acc = 0, i = n >> 2;

    while (i--) {
        rfbk_u32 sv = *sp++;
        rfbk_u32 xv = sv ^ *dp;
        if (xv) { acc |= xv; *dp = sv; }
        dp++;
    }
    return acc;
}

/* Compare with an early exit, the question a tile pass actually asks: did
 * ANYTHING in this run differ.  Stops at the first difference. */
rfbk_u32 RFBK_PFX(cmpearly)(const rfbk_u8 *s, const rfbk_u8 *d, rfbk_u32 n)
{
    const rfbk_u32 *sp = (const rfbk_u32 *)(const void *)s;
    const rfbk_u32 *dp = (const rfbk_u32 *)(const void *)d;
    rfbk_u32 i = n >> 2;

    while (i--)
        if (*sp++ != *dp++)
            return 1;
    return 0;
}

/* Eight longwords a turn, so the loop test is paid once per 32 bytes rather
 * than once per 4.  Compare only. */
rfbk_u32 RFBK_PFX(cmponly8)(const rfbk_u8 *s, const rfbk_u8 *d, rfbk_u32 n)
{
    const rfbk_u32 *sp = (const rfbk_u32 *)(const void *)s;
    const rfbk_u32 *dp = (const rfbk_u32 *)(const void *)d;
    rfbk_u32 acc = 0, i = n >> 5;

    while (i--) {
        acc |= sp[0] ^ dp[0];
        acc |= sp[1] ^ dp[1];
        acc |= sp[2] ^ dp[2];
        acc |= sp[3] ^ dp[3];
        acc |= sp[4] ^ dp[4];
        acc |= sp[5] ^ dp[5];
        acc |= sp[6] ^ dp[6];
        acc |= sp[7] ^ dp[7];
        sp += 8; dp += 8;
    }
    return acc;
}

/* Eight at a turn, keeping the XOR and writing the shadow: the unrolled form
 * of exactly what the encoder does today. */
rfbk_u32 RFBK_PFX(xorkeep8)(const rfbk_u8 *s, rfbk_u8 *d, rfbk_u8 *x,
                            rfbk_u32 n)
{
    const rfbk_u32 *sp = (const rfbk_u32 *)(const void *)s;
    rfbk_u32 *dp = (rfbk_u32 *)(void *)d;
    rfbk_u32 *xp = (rfbk_u32 *)(void *)x;
    rfbk_u32 acc = 0, i = n >> 5, k;

    while (i--) {
        for (k = 0; k < 8u; k++) {
            rfbk_u32 sv = sp[k];
            rfbk_u32 xv = sv ^ dp[k];
            acc |= xv;
            dp[k] = sv;
            xp[k] = xv;
        }
        sp += 8; dp += 8; xp += 8;
    }
    return acc;
}

/* Eight at a turn, comparing, and touching the shadow only where the run
 * differs: the shape an unchanged screen wants. */
rfbk_u32 RFBK_PFX(cmpstore8)(const rfbk_u8 *s, rfbk_u8 *d, rfbk_u32 n)
{
    const rfbk_u32 *sp = (const rfbk_u32 *)(const void *)s;
    rfbk_u32 *dp = (rfbk_u32 *)(void *)d;
    rfbk_u32 acc = 0, i = n >> 5, k;

    while (i--) {
        rfbk_u32 any = 0;
        for (k = 0; k < 8u; k++)
            any |= sp[k] ^ dp[k];
        if (any) {
            acc |= any;
            for (k = 0; k < 8u; k++)
                dp[k] = sp[k];
        }
        sp += 8; dp += 8;
    }
    return acc;
}

/* Read one buffer and nothing else: the floor any pass over the source has to
 * pay, and the number that says whether the source being in chip RAM is what
 * costs. */
rfbk_u32 RFBK_PFX(readonly)(const rfbk_u8 *s, rfbk_u32 n)
{
    const rfbk_u32 *sp = (const rfbk_u32 *)(const void *)s;
    rfbk_u32 acc = 0, i = n >> 2;

    while (i--)
        acc |= *sp++;
    return acc;
}

/* Plain copy, the shape fb_copy_frame's CopyMem() has. */
void RFBK_PFX(copy)(const rfbk_u8 *s, rfbk_u8 *d, rfbk_u32 n)
{
    memcpy(d, s, (size_t)n);
}

/* The row-at-a-time copy fb_copy_frame really does: one call per row. */
void RFBK_PFX(copyrows)(const rfbk_u8 *s, rfbk_u8 *d, rfbk_u32 rowlen,
                        rfbk_u32 rows, rfbk_u32 stride)
{
    rfbk_u32 r;
    for (r = 0; r < rows; r++) {
        memcpy(d, s, (size_t)rowlen);
        s += stride;
        d += rowlen;
    }
}

/* The tile-shaped version of xorkeep: the same work, but broken into 16-byte
 * rows sixteen at a time the way the encoder walks it.  The difference
 * between this and xorkeep is what the tile geometry costs. */
rfbk_u32 RFBK_PFX(xorkeep_tiled)(const rfbk_u8 *s, rfbk_u8 *d, rfbk_u8 *x,
                                 rfbk_u32 bpr, rfbk_u32 rows,
                                 rfbk_u32 tw, rfbk_u32 th)
{
    rfbk_u32 acc = 0, ty, tx, r, c;
    rfbk_u32 tiles_x = bpr / tw;
    rfbk_u32 tiles_y = rows / th;

    for (ty = 0; ty < tiles_y; ty++) {
        for (tx = 0; tx < tiles_x; tx++) {
            rfbk_u32 off = ty * th * bpr + tx * tw;
            for (r = 0; r < th; r++) {
                const rfbk_u32 *sp =
                    (const rfbk_u32 *)(const void *)(s + off + r * bpr);
                rfbk_u32 *dp = (rfbk_u32 *)(void *)(d + off + r * bpr);
                rfbk_u32 *xp = (rfbk_u32 *)(void *)(x + r * tw);
                for (c = 0; c < (tw >> 2); c++) {
                    rfbk_u32 sv = sp[c];
                    rfbk_u32 xv = sv ^ dp[c];
                    acc |= xv;
                    dp[c] = sv;
                    xp[c] = xv;
                }
            }
        }
    }
    return acc;
}

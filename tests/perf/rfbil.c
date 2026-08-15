/*
 * AmiNetXDuo, the frame encoder against a REAL interleaved BitMap.
 *
 *   rfbil
 *
 * WHY THIS EXISTS
 *
 *   The encoder used to read a plane-major copy the grab made, so an
 *   interleaved screen was de-interleaved before it ever saw one and
 *   RFB_F_INTERLEAVED could not be needed.  It reads the bitplanes where they
 *   are now, which means it depends on being told the truth about the layout,
 *   and the layout arithmetic -- a plane advances by bytes_per_row, a row by
 *   bytes_per_row * depth -- is the kind of thing that produces a sheared or
 *   repeated picture rather than an error.
 *
 *   Every .pfs capture in the tree is a plane-major screen.  The host round
 *   trip can interleave one synthetically, and does, but that proves the
 *   arithmetic against our own idea of interleaving.  This asks
 *   graphics.library for a real one.
 *
 * WHAT IT CHECKS
 *
 *   1. What layout the Workbench screen on this machine ACTUALLY has, printed
 *      rather than assumed, because that is the question of whether the live
 *      path has ever run interleaved at all.
 *
 *   2. AllocBitMap(..., BMF_INTERLEAVED | BMF_DISPLAYABLE, NULL), a pattern
 *      drawn into it through the same Planes[] the encoder will read, then
 *      encode -> decode -> compare EVERY BYTE of the decoded frame against
 *      the bitmap.  A wrong stride shears the picture, and a comparison of
 *      the pixels is the only thing that catches that; a byte count would
 *      pass.
 *
 *   3. The same against a plane-major bitmap, so a pass on the interleaved
 *      one cannot be a test that passes on anything.
 *
 * SPDX-License-Identifier: MIT
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <graphics/gfx.h>
#include <graphics/gfxbase.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/graphics.h>
#include <proto/intuition.h>

#include <stdarg.h>
#include <string.h>

#include "aminetxduo/compat.h"
#include "aminetxduo/rfb_encode.h"

#ifndef RawPutChar
#  define RawPutChar(c) \
      LP1NR(0x204, RawPutChar, UBYTE, (c), d0, , EXEC_BASE_NAME)
#endif

struct GfxBase       *GfxBase;
struct IntuitionBase *IntuitionBase;

#define IL_W        640
#define IL_H        256
#define IL_BPR      (IL_W / 8)

static char  il_log[8192];
static ULONG il_used;
static int   il_fail;

static VOID il_put(UBYTE ch)
{
    RawPutChar(ch);
    if (il_used < (ULONG)sizeof(il_log) - 1UL)
        il_log[il_used++] = (char)ch;
}

static VOID il_putc(register UBYTE ch __asm("d0"), register APTR u __asm("a3"))
{
    (VOID)u;
    if (ch != '\0')
        il_put(ch);
}

static VOID il_say(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    RawDoFmt((STRPTR)fmt, ap, (void (*)())il_putc, NULL);
    va_end(ap);
    il_put('\n');
}

/* --------------------------------------------------------- the decoder --- */

/* The receiver's side, so what is checked is the picture and not the byte
 * count.  Plane-major output whatever the source layout was, which is also
 * what the browser draws. */
typedef struct {
    rfb_u8 *fb;                 /* depth * IL_BPR * IL_H, plane-major */
    rfb_u32 depth;
} il_dec;

static rfb_u32 il_rd16(const rfb_u8 *p)
{
    return ((rfb_u32)p[0] << 8) | p[1];
}

static int il_decode(il_dec *d, const rfb_u8 *in, rfb_u32 n)
{
    const rfb_u32 tiles_x = (IL_BPR + 15u) / 16u;
    rfb_u8 tile[16 * 16];
    rfb_u32 i = 4;

    if (n < 5u || in[0] != RFB_VERSION)
        return -1;

    for (;;) {
        rfb_u32 op;

        if (i >= n)
            return -2;
        op = in[i++];
        if (op == RFB_OP_END)
            break;

        if (op == RFB_OP_COPY) {
            rfb_u32 x0, w, y0, h, p, r;
            rfb_s32 dy;

            if (i + 10u > n)
                return -3;
            x0 = il_rd16(in + i); w = il_rd16(in + i + 2);
            y0 = il_rd16(in + i + 4); h = il_rd16(in + i + 6);
            dy = (rfb_s32)(rfb_s16)il_rd16(in + i + 8);
            i += 10u;
            for (p = 0; p < d->depth; p++) {
                rfb_u8 *pl = d->fb + p * IL_BPR * IL_H;
                if (dy > 0)
                    for (r = 0; r < h; r++)
                        memmove(pl + (y0 + r) * IL_BPR + x0,
                                pl + (y0 + r + (rfb_u32)dy) * IL_BPR + x0, w);
                else
                    for (r = h; r > 0; r--) {
                        rfb_u32 dr = y0 + r - 1u;
                        memmove(pl + dr * IL_BPR + x0,
                                pl + (rfb_u32)((rfb_s32)dr + dy) * IL_BPR + x0,
                                w);
                    }
            }
            continue;
        }

        if (op != RFB_OP_TILE)
            return -5;
        if (i + 3u > n)
            return -6;
        {
            rfb_u32 idx = il_rd16(in + i);
            rfb_u32 mask = in[i + 2];
            rfb_u32 tx = idx % tiles_x, ty = idx / tiles_x;
            rfb_u32 x0 = tx * 16u, y0 = ty * 16u;
            rfb_u32 tw = IL_BPR - x0, th = IL_H - y0;
            rfb_u32 p, r;

            i += 3u;
            if (tw > 16u) tw = 16u;
            if (th > 16u) th = 16u;

            for (p = 0; p < d->depth; p++) {
                rfb_u32 code, len, c;
                rfb_u8 *pl;

                if (!(mask & (1u << p)))
                    continue;
                if (i >= n)
                    return -7;
                code = in[i++];
                if (code == RFB_CODE_RAW) {
                    len = tw * th;
                    if (i + len > n)
                        return -8;
                    memcpy(tile, in + i, (size_t)len);
                    i += len;
                } else {
                    long got;
                    if (i + 2u > n)
                        return -9;
                    len = il_rd16(in + i);
                    i += 2u;
                    if (i + len > n)
                        return -10;
                    got = rfb_unpackbits(in + i, len, tile, sizeof(tile));
                    if (got != (long)(tw * th))
                        return -11;
                    i += len;
                }

                pl = d->fb + p * IL_BPR * IL_H;
                if (code == RFB_CODE_PB_XOR)
                    for (r = 0; r < th; r++)
                        for (c = 0; c < tw; c++)
                            pl[(y0 + r) * IL_BPR + x0 + c] ^= tile[r * tw + c];
                else
                    for (r = 0; r < th; r++)
                        memcpy(pl + (y0 + r) * IL_BPR + x0, tile + r * tw,
                               (size_t)tw);
            }
        }
    }
    return 0;
}

/* ------------------------------------------------------------- the test --- */

/* The bitmap, read the way the encoder reads it, flattened plane-major so it
 * can be compared with what the decoder produced. */
static VOID il_flatten(struct BitMap *bm, ULONG depth, ULONG row_stride,
                       rfb_u8 *out)
{
    ULONG p, y;

    for (p = 0; p < depth; p++) {
        const UBYTE *src = (const UBYTE *)bm->Planes[p];
        for (y = 0; y < IL_H; y++)
            memcpy(out + (p * IL_H + y) * IL_BPR, src + y * row_stride,
                   (size_t)IL_BPR);
    }
}

static VOID il_pattern(struct BitMap *bm, ULONG depth, ULONG row_stride,
                       ULONG salt)
{
    ULONG p, y, x;

    for (p = 0; p < depth; p++) {
        UBYTE *dst = (UBYTE *)bm->Planes[p];
        for (y = 0; y < IL_H; y++) {
            UBYTE *row = dst + y * row_stride;
            for (x = 0; x < IL_BPR; x++) {
                /* Depends on the plane AND the row AND the column, so a
                   stride that mixes planes or rows up cannot come out
                   looking right. */
                row[x] = (UBYTE)(p * 37UL + y * 11UL + x * 7UL + salt);
            }
        }
    }
}

static VOID il_one(ULONG depth, ULONG bmflags, const char *what)
{
    struct BitMap *bm;
    rfb_encoder    e;
    rfb_geom       g;
    rfb_scroll_cfg cfg;
    rfb_u8        *shadow = NULL, *scratch = NULL, *out = NULL;
    rfb_u8        *want = NULL;
    il_dec         d;
    ULONG          shadow_len, scratch_len, out_cap, plane_bytes;
    ULONG          row_stride, real_flags, bpr;
    rfb_u32        flags;
    ULONG          pass;
    int            bad = 0;

    bm = AllocBitMap(IL_W, IL_H, depth, bmflags, NULL);
    if (bm == NULL) {
        il_say("rfbil case=%s RESULT=SKIP why=AllocBitMap_failed", what);
        return;
    }

    real_flags = GetBitMapAttr(bm, BMA_FLAGS);
    row_stride = (ULONG)(UWORD)bm->BytesPerRow;

    /* Exactly the derivation httpfb.c uses, off the BitMap's own answer. */
    if ((real_flags & BMF_INTERLEAVED) != 0) {
        bpr = row_stride / depth;
        flags = RFB_F_BASELINE | RFB_F_COPYRECT | RFB_F_SCROLL_ADAPTIVE
              | RFB_F_INTERLEAVED;
    } else {
        bpr = row_stride;
        flags = RFB_F_BASELINE | RFB_F_COPYRECT | RFB_F_SCROLL_ADAPTIVE;
    }

    il_say("rfbil case=%s depth=%lu asked_interleaved=%lu got_interleaved=%lu "
           "BytesPerRow=%lu bpr=%lu",
           what, depth, (bmflags & BMF_INTERLEAVED) ? 1UL : 0UL,
           (real_flags & BMF_INTERLEAVED) ? 1UL : 0UL, row_stride, bpr);

    if (bpr != IL_BPR) {
        il_say("rfbil case=%s RESULT=SKIP why=bpr_%lu_not_%lu",
               what, bpr, (ULONG)IL_BPR);
        FreeBitMap(bm);
        return;
    }

    g.width = IL_W; g.height = IL_H; g.depth = (rfb_u8)depth;
    g.bytes_per_row = (rfb_u16)bpr; g.tile_w = 16; g.tile_h = 16;

    rfb_scroll_defaults(&cfg);
    shadow_len  = rfb_shadow_size(&g);
    scratch_len = rfb_scratch_size(&g, flags, &cfg);
    out_cap     = rfb_worst_case_frame(&g);
    plane_bytes = (ULONG)bpr * IL_H;

    shadow  = (rfb_u8 *)ami_alloc(shadow_len);
    scratch = (rfb_u8 *)ami_alloc(scratch_len);
    out     = (rfb_u8 *)ami_alloc(out_cap);
    want    = (rfb_u8 *)ami_alloc(plane_bytes * depth);
    d.fb    = (rfb_u8 *)ami_alloc(plane_bytes * depth);
    d.depth = depth;

    if (!shadow || !scratch || !out || !want || !d.fb) {
        il_say("rfbil case=%s RESULT=SKIP why=no_memory", what);
        goto done;
    }

    if (rfb_encoder_init(&e, &g, flags, &cfg, shadow, shadow_len,
                         scratch, scratch_len) != 0L) {
        il_say("rfbil case=%s RESULT=FAIL why=encoder_init", what);
        il_fail = 1;
        goto done;
    }

    /* The receiver starts from an all-zero screen, and so does the shadow. */
    memset(d.fb, 0, (size_t)(plane_bytes * depth));

    for (pass = 0; pass < 4UL; pass++) {
        const rfb_u8 *planes[RFB_MAX_DEPTH];
        ULONG p;
        long  n;

        /* A different pattern every pass, so the second and later frames are
           deltas against a shadow rather than a first frame. */
        il_pattern(bm, depth, row_stride, pass * 53UL);
        il_flatten(bm, depth, row_stride, want);

        for (p = 0; p < depth; p++)
            planes[p] = (const rfb_u8 *)bm->Planes[p];

        n = rfb_encode_frame_planes(&e, planes, out, out_cap);
        if (n < 0) {
            il_say("rfbil case=%s pass=%lu RESULT=FAIL why=encode_%ld",
                   what, pass, n);
            il_fail = 1;
            goto done;
        }
        if (il_decode(&d, out, (rfb_u32)n) != 0) {
            il_say("rfbil case=%s pass=%lu RESULT=FAIL why=decode", what, pass);
            il_fail = 1;
            goto done;
        }
        if (memcmp(d.fb, want, (size_t)(plane_bytes * depth)) != 0) {
            ULONG i;
            for (i = 0; i < plane_bytes * depth; i++)
                if (d.fb[i] != want[i])
                    break;
            il_say("rfbil case=%s pass=%lu RESULT=FAIL why=pixels "
                   "first_bad=%lu got=%lu want=%lu bytes=%ld",
                   what, pass, i, (ULONG)d.fb[i], (ULONG)want[i], n);
            bad = 1;
            il_fail = 1;
            goto done;
        }
        il_say("rfbil case=%s pass=%lu bytes=%ld pixels=match",
               what, pass, n);
    }

done:
    if (!bad && !il_fail)
        il_say("rfbil case=%s RESULT=PASS", what);
    ami_free(d.fb); ami_free(want); ami_free(out);
    ami_free(scratch); ami_free(shadow);
    FreeBitMap(bm);
}

/* What the live path actually gets on this machine. */
static VOID il_workbench(VOID)
{
    struct Screen *sc = LockPubScreen((CONST_STRPTR)"Workbench");
    struct BitMap *bm;
    ULONG flags, depth, stride;

    if (sc == NULL) {
        il_say("rfbil workbench=none");
        return;
    }
    bm = sc->RastPort.BitMap;
    flags  = GetBitMapAttr(bm, BMA_FLAGS);
    depth  = GetBitMapAttr(bm, BMA_DEPTH);
    stride = (ULONG)(UWORD)bm->BytesPerRow;

    il_say("rfbil workbench w=%lu h=%lu depth=%lu BytesPerRow=%lu "
           "interleaved=%lu standard=%lu",
           (ULONG)sc->Width, (ULONG)sc->Height, depth, stride,
           (flags & BMF_INTERLEAVED) ? 1UL : 0UL,
           (flags & BMF_STANDARD) ? 1UL : 0UL);

    UnlockPubScreen(NULL, sc);
}

int main(void)
{
    BPTR o;

    GfxBase = (struct GfxBase *)OpenLibrary((CONST_STRPTR)"graphics.library", 39);
    IntuitionBase = (struct IntuitionBase *)
        OpenLibrary((CONST_STRPTR)"intuition.library", 39);

    if (GfxBase == NULL || IntuitionBase == NULL) {
        il_say("rfbil RESULT=SKIP why=no_v39_libraries");
        goto out;
    }

    il_workbench();

    il_one(2, BMF_INTERLEAVED | BMF_DISPLAYABLE | BMF_CLEAR, "interleaved_d2");
    il_one(4, BMF_INTERLEAVED | BMF_DISPLAYABLE | BMF_CLEAR, "interleaved_d4");
    il_one(8, BMF_INTERLEAVED | BMF_DISPLAYABLE | BMF_CLEAR, "interleaved_d8");
    il_one(2, BMF_DISPLAYABLE | BMF_CLEAR, "planar_d2");
    il_one(4, BMF_DISPLAYABLE | BMF_CLEAR, "planar_d4");

    il_say("rfbil RESULT=%s", il_fail ? "FAIL" : "PASS");

out:
    o = Output();
    if (o != (BPTR)0)
        (VOID)Write(o, (APTR)il_log, (LONG)il_used);
    if (IntuitionBase) CloseLibrary((struct Library *)IntuitionBase);
    if (GfxBase) CloseLibrary((struct Library *)GfxBase);
    return il_fail ? RETURN_FAIL : RETURN_OK;
}

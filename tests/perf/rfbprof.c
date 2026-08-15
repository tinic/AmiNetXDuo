/*
 * AmiNetXDuo, where the /console frame encoder's time goes, on the machine.
 *
 *   rfbprof [FILE=<pfs>] [REPS=n] [QUIET]
 *
 * WHY THIS AND NOT THE SAMPLING PROFILER
 *
 *   Profile ranks functions.  The questions here are differences between
 *   configurations of the same functions -- what the scroll probe costs on a
 *   screen where nothing moved, what best-of costs, what the shipping
 *   -m68000 codegen costs against -m68020 -- and a ranking cannot answer any
 *   of them.  Each arm is the real rfb_encode_frame() over the real capture,
 *   timed with ReadEClock, and the arms are INTERLEAVED IN ONE RUN so a busy
 *   host cannot separate them.
 *
 *   rfb_encode.c is linked in TWICE: once compiled the way the tree ships it
 *   (-m68000, because AMINETXDUO_CPU=any) and once at -m68020, the second
 *   copy's eight public symbols renamed on the command line.  One binary,
 *   both codegens, same data, same second.
 *
 * WHAT IT READS
 *
 *   A .pfs capture, the same file format tests the host bench uses, so the
 *   frames are the ones the numbers elsewhere in this project were taken on.
 *   Without FILE= it makes a blank screen and a scrolled one, which measures
 *   the idle path exactly (the diff loop has no data-dependent exit) and the
 *   scroll path only approximately.
 *
 *   Every buffer reports TypeOfMem(), because "the shadow is in fast RAM" is
 *   an assumption this exists to remove.
 *
 * SPDX-License-Identifier: MIT
 */

#include <exec/types.h>
#include <exec/execbase.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <devices/timer.h>
#include <proto/exec.h>
#include <inline/macros.h>
#include <proto/dos.h>
#include <proto/timer.h>

#include <stdarg.h>
#include <string.h>

#include "aminetxduo/compat.h"
#include "aminetxduo/rfb_encode.h"

/* The second copy of the encoder, same source, -m68020 codegen. */
extern rfb_u32 rfb020_shadow_size(const rfb_geom *g);
extern rfb_u32 rfb020_scratch_size(const rfb_geom *g, rfb_u32 flags,
                                   const rfb_scroll_cfg *cfg);
extern void    rfb020_scroll_defaults(rfb_scroll_cfg *cfg);
extern long    rfb020_encoder_init(rfb_encoder *e, const rfb_geom *g,
                                   rfb_u32 flags, const rfb_scroll_cfg *cfg,
                                   rfb_u8 *shadow, rfb_u32 shadow_len,
                                   rfb_u8 *scratch, rfb_u32 scratch_len);
extern long    rfb020_encode_frame(rfb_encoder *e, const rfb_u8 *src,
                                   rfb_u8 *out, rfb_u32 out_cap);

/* And the same source at -O2, on both instruction sets. */
extern long rfbo2_encoder_init(rfb_encoder *, const rfb_geom *, rfb_u32,
                               const rfb_scroll_cfg *, rfb_u8 *, rfb_u32,
                               rfb_u8 *, rfb_u32);
extern long rfbo2_encode_frame(rfb_encoder *, const rfb_u8 *, rfb_u8 *,
                               rfb_u32);
extern long rfb220_encoder_init(rfb_encoder *, const rfb_geom *, rfb_u32,
                                const rfb_scroll_cfg *, rfb_u8 *, rfb_u32,
                                rfb_u8 *, rfb_u32);
extern long rfb220_encode_frame(rfb_encoder *, const rfb_u8 *, rfb_u8 *,
                                rfb_u32);

/* The candidate loops, both codegens. */
#define KDECL(p)                                                              \
    extern ULONG p##xorstore(const UBYTE *, UBYTE *, ULONG);                  \
    extern ULONG p##xorkeep(const UBYTE *, UBYTE *, UBYTE *, ULONG);          \
    extern ULONG p##cmponly(const UBYTE *, const UBYTE *, ULONG);             \
    extern ULONG p##cmpstore(const UBYTE *, UBYTE *, ULONG);                  \
    extern ULONG p##cmpearly(const UBYTE *, const UBYTE *, ULONG);            \
    extern ULONG p##cmponly8(const UBYTE *, const UBYTE *, ULONG);            \
    extern ULONG p##xorkeep8(const UBYTE *, UBYTE *, UBYTE *, ULONG);         \
    extern ULONG p##cmpstore8(const UBYTE *, UBYTE *, ULONG);                 \
    extern ULONG p##readonly(const UBYTE *, ULONG);                           \
    extern void  p##copy(const UBYTE *, UBYTE *, ULONG);                      \
    extern void  p##copyrows(const UBYTE *, UBYTE *, ULONG, ULONG, ULONG);    \
    extern ULONG p##xorkeep_tiled(const UBYTE *, UBYTE *, UBYTE *,            \
                                  ULONG, ULONG, ULONG, ULONG);
KDECL(rfbk_)
KDECL(rfbk020_)

/* ------------------------------------------------------------- logging --- */

#ifndef RawPutChar
#  define RawPutChar(c) \
      LP1NR(0x204, RawPutChar, UBYTE, (c), d0, , EXEC_BASE_NAME)
#endif

#define R_LOG_SIZE      32768

static char     r_log_buffer[R_LOG_SIZE];
static ULONG    r_log_used;

static VOID r_put(UBYTE ch)
{
    RawPutChar(ch);
    if (r_log_used < (ULONG)(R_LOG_SIZE - 1))
        r_log_buffer[r_log_used++] = (char)ch;
}

static VOID r_put_char(register UBYTE ch     __asm("d0"),
                       register APTR  unused __asm("a3"))
{
    (VOID)unused;
    if (ch != '\0')
        r_put(ch);
}

static VOID r_log(const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    RawDoFmt((STRPTR)fmt, args, (void (*)())r_put_char, NULL);
    va_end(args);
    r_put('\n');
}

static VOID r_flush(VOID)
{
    BPTR out = Output();

    if (out != (BPTR)0)
        (VOID)Write(out, (APTR)r_log_buffer, (LONG)r_log_used);
}

/* -------------------------------------------------------------- timing --- */

extern struct Device *TimerBase;        /* src/common/compat.c owns it */

static ULONG r_rate;                    /* E-Clock ticks a second */
static ULONG r_bracket;                 /* what one measurement costs */

static ULONG r_now(VOID)
{
    struct EClockVal ev;

    (VOID)ReadEClock(&ev);
    return ev.ev_lo;
}

static VOID r_timer_init(VOID)
{
    struct EClockVal ev;
    ULONG i, t0, t1, total = 0UL;

    (VOID)ami_millis();                 /* opens timer.device, sets TimerBase */
    r_rate = ReadEClock(&ev);

    for (i = 0UL; i < 256UL; i++)
    {
        t0 = r_now();
        t1 = r_now();
        total += (t1 - t0);
    }
    r_bracket = total / 256UL;
}

/* Microseconds from E-Clock ticks, without a 64-bit divide: the rate is about
   709379 on PAL and 715909 on NTSC, so ticks*1000 overflows past 4.2M ticks
   (six seconds).  Every arm here is well under a second. */
static ULONG r_us(ULONG ticks)
{
    if (ticks > r_bracket)
        ticks -= r_bracket;
    else
        ticks = 0UL;

    /* Whole seconds first, then the remainder scaled -- (ticks % rate) * 1000
       is at most 7.1e8 and fits, where ticks * 1000 stops fitting at six
       seconds.  The first cut of this dropped the remainder entirely and
       printed every arm over six seconds as a whole number of seconds. */
    return (ticks / r_rate) * 1000000UL
         + (((ticks % r_rate) * 1000UL) / (r_rate / 1000UL));
}

/* --------------------------------------------------------------- state --- */

#define R_MAXFRAMES     16

static rfb_geom  r_g;
static ULONG     r_frame_bytes;
static ULONG     r_frames;
static UBYTE    *r_data;                /* r_frames * r_frame_bytes, fast RAM */
static UBYTE    *r_chip;                /* one frame, chip RAM: the bitmap    */
static UBYTE    *r_grab;                /* the grab buffer httpfb.c keeps     */
static UBYTE    *r_shadow;
static UBYTE    *r_scratch;
static UBYTE    *r_out;
static UBYTE    *r_shadow2;
static UBYTE    *r_scratch2;
static ULONG     r_shadow_len, r_scratch_len, r_out_cap;
static ULONG     r_reps = 8UL;   /* scaled to the frame count in main() */
static char      r_name[32] = "synthetic";

static const char *r_memtype(APTR p)
{
    ULONG t;

    if (p == NULL)
        return "none";
    t = TypeOfMem(p);
    if (t == 0UL)
        return "unknown";
    if ((t & MEMF_CHIP) != 0UL)
        return "CHIP";
    if ((t & MEMF_FAST) != 0UL)
        return "FAST";
    return "other";
}

/* ---------------------------------------------------------------- .pfs --- */

/* PFS1: magic, w, h, depth, pad, bytes_per_row, frames, then 3<<depth bytes
   of palette, then frames * frame_bytes of plane-major pixels. */
static BOOL r_load_pfs(const char *path)
{
    BPTR  fh;
    UBYTE hdr[16];
    ULONG palette, want, i;
    LONG  n;

    fh = Open((CONST_STRPTR)path, MODE_OLDFILE);
    if (fh == (BPTR)0)
    {
        r_log("rfbprof error=cannot_open file=%s", path);
        return FALSE;
    }

    if (Read(fh, hdr, 16L) != 16L || memcmp(hdr, "PFS1", 4) != 0)
    {
        r_log("rfbprof error=not_pfs1 file=%s", path);
        Close(fh);
        return FALSE;
    }

    r_g.width         = (rfb_u16)(((ULONG)hdr[4] << 8) | hdr[5]);
    r_g.height        = (rfb_u16)(((ULONG)hdr[6] << 8) | hdr[7]);
    r_g.depth         = hdr[8];
    r_g.bytes_per_row = (rfb_u16)(((ULONG)hdr[10] << 8) | hdr[11]);
    r_frames          = ((ULONG)hdr[12] << 8) | hdr[13];

    if (r_g.depth < 1 || r_g.depth > RFB_MAX_DEPTH || r_g.bytes_per_row == 0)
    {
        r_log("rfbprof error=bad_geometry");
        Close(fh);
        return FALSE;
    }
    if (r_frames > (ULONG)R_MAXFRAMES)
        r_frames = (ULONG)R_MAXFRAMES;

    palette = 3UL << r_g.depth;
    if (Seek(fh, (LONG)(16UL + palette), OFFSET_BEGINNING) < 0)
    {
        Close(fh);
        return FALSE;
    }

    r_frame_bytes = (ULONG)r_g.bytes_per_row * r_g.height * r_g.depth;
    want = r_frame_bytes * r_frames;
    r_data = (UBYTE *)ami_alloc(want);
    if (r_data == NULL)
    {
        r_log("rfbprof error=no_memory bytes=%lu", want);
        Close(fh);
        return FALSE;
    }

    for (i = 0UL; i < want; i += (ULONG)n)
    {
        ULONG chunk = want - i;

        if (chunk > 32768UL)
            chunk = 32768UL;
        n = Read(fh, r_data + i, (LONG)chunk);
        if (n <= 0)
        {
            r_log("rfbprof error=short_read at=%lu", i);
            Close(fh);
            return FALSE;
        }
    }
    Close(fh);

    {
        const char *b = path, *p = path;

        while (*p != '\0')
        {
            if (*p == '/' || *p == ':')
                b = p + 1;
            p++;
        }
        for (i = 0UL; i < (ULONG)sizeof(r_name) - 1UL && b[i] != '\0'; i++)
            r_name[i] = b[i];
        r_name[i] = '\0';
    }
    return TRUE;
}

/* Two frames with no capture: an empty screen, and the same screen with a
   band moved up eight rows.  The idle arm is exact -- the diff loop's cost
   does not depend on the bytes -- and the scroll arm is a lower bound on the
   real one, which is said where it is printed. */
static BOOL r_make_synthetic(UWORD depth)
{
    ULONG i, y, p;

    r_g.width = 640; r_g.height = 256; r_g.depth = (rfb_u8)depth;
    r_g.bytes_per_row = 80;
    r_frames = 3UL;
    r_frame_bytes = (ULONG)r_g.bytes_per_row * r_g.height * r_g.depth;

    r_data = (UBYTE *)ami_alloc(r_frame_bytes * r_frames);
    if (r_data == NULL)
        return FALSE;

    /* Frame 0 and 1 identical: text-like sparse content. */
    for (p = 0UL; p < (ULONG)r_g.depth; p++)
        for (y = 0UL; y < (ULONG)r_g.height; y++)
        {
            UBYTE *row = r_data + (p * r_g.height + y) * r_g.bytes_per_row;

            if ((y & 7UL) < 6UL)
                for (i = 0UL; i < (ULONG)r_g.bytes_per_row; i += 3UL)
                    row[i] = (UBYTE)(0x6EUL + ((y * 31UL + i * 7UL) & 0x11UL));
        }
    memcpy(r_data + r_frame_bytes, r_data, (size_t)r_frame_bytes);

    /* Frame 2: everything eight rows up. */
    for (p = 0UL; p < (ULONG)r_g.depth; p++)
        for (y = 0UL; y < (ULONG)r_g.height; y++)
        {
            ULONG sy = (y + 8UL < (ULONG)r_g.height) ? y + 8UL : y;
            memcpy(r_data + 2UL * r_frame_bytes
                       + (p * r_g.height + y) * r_g.bytes_per_row,
                   r_data + (p * r_g.height + sy) * r_g.bytes_per_row,
                   (size_t)r_g.bytes_per_row);
        }
    return TRUE;
}

/* --------------------------------------------------------------- arms ---- */

/* One encode arm: reset the encoder, then run the sequence `reps` times and
   report the mean per frame.  `first` is skipped in the timing when the
   sequence has more than one frame -- frame 0 against a zero shadow is the
   whole screen and is not what a session spends its time doing. */
typedef long (*r_encfn)(rfb_encoder *, const rfb_u8 *, rfb_u8 *, rfb_u32);
typedef long (*r_initfn)(rfb_encoder *, const rfb_geom *, rfb_u32,
                         const rfb_scroll_cfg *, rfb_u8 *, rfb_u32,
                         rfb_u8 *, rfb_u32);

static const char *const r_cpu_name[4] = { "68000-Os", "68020-Os",
                                           "68000-O2", "68020-O2" };
static const r_encfn r_enc_fn[4] = { rfb_encode_frame, rfb020_encode_frame,
                                     rfbo2_encode_frame, rfb220_encode_frame };
static const r_initfn r_init_fn[4] = { rfb_encoder_init, rfb020_encoder_init,
                                       rfbo2_encoder_init, rfb220_encoder_init };

/* The same arm, but reading the frame out of CHIP RAM through the plane
   pointers -- which is what httpfb.c does now that there is no grab buffer.
   The frame is staged into chip OUTSIDE the timed region, because staging it
   is the copy this exists to show nobody has to make. */
static VOID r_encode_chip_arm(const char *tag, rfb_u32 flags,
                              ULONG from, ULONG to)
{
    rfb_encoder     e;
    rfb_scroll_cfg  cfg;
    const rfb_u8   *planes[RFB_MAX_DEPTH];
    ULONG           rep, i, p, t0, t1, ticks = 0UL, nframes = 0UL;
    ULONG           bytes = 0UL;
    long            n = 0;

    rfb_scroll_defaults(&cfg);
    for (p = 0UL; p < (ULONG)r_g.depth; p++)
        planes[p] = r_chip + p * (ULONG)r_g.bytes_per_row * r_g.height;

    for (rep = 0UL; rep < r_reps; rep++) {
        memset(r_shadow, 0, (size_t)r_shadow_len);
        (VOID)rfb_encoder_init(&e, &r_g, flags, &cfg, r_shadow, r_shadow_len,
                               r_scratch, r_scratch_len);

        for (i = 0UL; i <= from; i++) {
            memcpy(r_chip, r_data + i * r_frame_bytes, (size_t)r_frame_bytes);
            n = rfb_encode_frame_planes(&e, planes, r_out, r_out_cap);
        }

        for (i = from + 1UL; i <= to; i++) {
            memcpy(r_chip, r_data + i * r_frame_bytes, (size_t)r_frame_bytes);
            t0 = r_now();
            n = rfb_encode_frame_planes(&e, planes, r_out, r_out_cap);
            t1 = r_now();
            ticks += (t1 - t0);
            nframes++;
            if (n > 0)
                bytes += (ULONG)n;
        }
    }

    if (nframes == 0UL)
        nframes = 1UL;
    r_log("rfbprof seq=%s arm=%s cpu=%s us=%lu bytes=%lu frames=%lu",
          r_name, tag, "chip", r_us(ticks) / nframes, bytes / nframes,
          nframes);
}

static VOID r_encode_arm(const char *tag, rfb_u32 flags, int use020,
                         ULONG from, ULONG to)
{
    rfb_encoder     e;
    rfb_scroll_cfg  cfg;
    r_encfn         enc  = r_enc_fn[use020 & 3];
    r_initfn        init = r_init_fn[use020 & 3];
    ULONG           rep, i, t0, t1, ticks = 0UL, nframes = 0UL;
    ULONG           bytes = 0UL;
    long            n = 0;

    rfb_scroll_defaults(&cfg);

    for (rep = 0UL; rep < r_reps; rep++)
    {
        memset(r_shadow, 0, (size_t)r_shadow_len);
        (VOID)init(&e, &r_g, flags, &cfg, r_shadow, r_shadow_len,
                   r_scratch, r_scratch_len);

        /* Prime: every frame up to `from` runs untimed, so the shadow the
           timed frames diff against is the one a live session would have. */
        for (i = 0UL; i <= from; i++)
            n = enc(&e, r_data + i * r_frame_bytes, r_out, r_out_cap);

        for (i = from + 1UL; i <= to; i++)
        {
            t0 = r_now();
            n = enc(&e, r_data + i * r_frame_bytes, r_out, r_out_cap);
            t1 = r_now();
            ticks += (t1 - t0);
            nframes++;
            if (n > 0)
                bytes += (ULONG)n;
        }
    }

    if (nframes == 0UL)
        nframes = 1UL;
    r_log("rfbprof seq=%s arm=%s cpu=%s us=%lu bytes=%lu frames=%lu",
          r_name, tag, r_cpu_name[use020 & 3],
          r_us(ticks) / nframes, bytes / nframes, nframes);
}

/* One kernel arm: `n` bytes, `r_reps` times, mean microseconds. */
#define R_KERN(tag, cpu, call)                                                \
    do {                                                                      \
        ULONG _r, _t0, _t1, _tk = 0UL;                                        \
        for (_r = 0UL; _r < r_reps; _r++) {                                   \
            _t0 = r_now();                                                    \
            (void)(call);                                                     \
            _t1 = r_now();                                                    \
            _tk += (_t1 - _t0);                                               \
        }                                                                     \
        r_log("rfbprof kern=%s cpu=%s bytes=%lu us=%lu",                      \
              tag, cpu, plane, r_us(_tk) / r_reps);                           \
    } while (0)

static VOID r_kernels(VOID)
{
    const ULONG plane = r_frame_bytes;
    UBYTE *a = r_data;
    UBYTE *b = r_shadow;
    UBYTE *x = r_grab;

    /* The shadow holds a copy of the source, so every compare arm below is
       measuring the unchanged case -- the one that matters. */
    memcpy(b, a, (size_t)plane);

    R_KERN("readonly_fast", "68000", rfbk_readonly(a, plane));
    R_KERN("readonly_fast", "68020", rfbk020_readonly(a, plane));
    R_KERN("readonly_chip", "68000", rfbk_readonly(r_chip, plane));
    R_KERN("readonly_chip", "68020", rfbk020_readonly(r_chip, plane));

    R_KERN("copy_chip_fast", "68000", (rfbk_copy(r_chip, x, plane), 0));
    R_KERN("copy_chip_fast", "68020", (rfbk020_copy(r_chip, x, plane), 0));
    R_KERN("copyrows_chip_fast", "68000",
           (rfbk_copyrows(r_chip, x, r_g.bytes_per_row,
                          (ULONG)r_g.height * r_g.depth,
                          r_g.bytes_per_row), 0));
    R_KERN("copy_fast_fast", "68000", (rfbk_copy(a, x, plane), 0));
    R_KERN("copy_fast_fast", "68020", (rfbk020_copy(a, x, plane), 0));

    R_KERN("xorstore", "68000", rfbk_xorstore(a, b, plane));
    R_KERN("xorstore", "68020", rfbk020_xorstore(a, b, plane));
    R_KERN("xorkeep", "68000", rfbk_xorkeep(a, b, x, plane));
    R_KERN("xorkeep", "68020", rfbk020_xorkeep(a, b, x, plane));
    R_KERN("xorkeep_tiled", "68000",
           rfbk_xorkeep_tiled(a, b, x, r_g.bytes_per_row,
                              (ULONG)r_g.height * r_g.depth, 16UL, 16UL));
    R_KERN("xorkeep_tiled", "68020",
           rfbk020_xorkeep_tiled(a, b, x, r_g.bytes_per_row,
                                 (ULONG)r_g.height * r_g.depth, 16UL, 16UL));
    R_KERN("cmponly", "68000", rfbk_cmponly(a, b, plane));
    R_KERN("cmponly", "68020", rfbk020_cmponly(a, b, plane));
    R_KERN("cmponly8", "68000", rfbk_cmponly8(a, b, plane));
    R_KERN("cmponly8", "68020", rfbk020_cmponly8(a, b, plane));
    R_KERN("cmpstore", "68000", rfbk_cmpstore(a, b, plane));
    R_KERN("cmpstore", "68020", rfbk020_cmpstore(a, b, plane));
    R_KERN("cmpstore8", "68000", rfbk_cmpstore8(a, b, plane));
    R_KERN("cmpstore8", "68020", rfbk020_cmpstore8(a, b, plane));
    R_KERN("xorkeep8", "68000", rfbk_xorkeep8(a, b, x, plane));
    R_KERN("xorkeep8", "68020", rfbk020_xorkeep8(a, b, x, plane));
    R_KERN("cmpearly", "68000", rfbk_cmpearly(a, b, plane));
    R_KERN("cmpearly", "68020", rfbk020_cmpearly(a, b, plane));

    /* The fused shape: diff the CHIP RAM planes against the shadow with no
       grab buffer in between.  The shadow holds the chip frame first, so this
       is again the unchanged case. */
    memcpy(r_chip, a, (size_t)plane);
    R_KERN("fused_cmponly_chip", "68000", rfbk_cmponly(r_chip, b, plane));
    R_KERN("fused_cmponly_chip", "68020", rfbk020_cmponly(r_chip, b, plane));
    R_KERN("fused_xorkeep_chip", "68020",
           rfbk020_xorkeep(r_chip, b, x, plane));
    R_KERN("fused_cmponly8_chip", "68000", rfbk_cmponly8(r_chip, b, plane));
    R_KERN("fused_cmponly8_chip", "68020", rfbk020_cmponly8(r_chip, b, plane));
    R_KERN("fused_cmpstore8_chip", "68000", rfbk_cmpstore8(r_chip, b, plane));
    R_KERN("fused_xorkeep8_chip", "68000", rfbk_xorkeep8(r_chip, b, x, plane));
}

/* --------------------------------------------------------------- main ---- */

static VOID r_report_setup(VOID)
{
    struct ExecBase *eb = (struct ExecBase *)SysBase;

    r_log("rfbprof build=%s attnflags=0x%04lx eclock=%lu bracket=%lu",
          "one-binary-two-codegens",
          (ULONG)eb->AttnFlags, r_rate, r_bracket);
    r_log("rfbprof geom w=%lu h=%lu depth=%lu bpr=%lu frame_bytes=%lu "
          "frames=%lu reps=%lu",
          (ULONG)r_g.width, (ULONG)r_g.height, (ULONG)r_g.depth,
          (ULONG)r_g.bytes_per_row, r_frame_bytes, r_frames, r_reps);
    r_log("rfbprof mem data=%s chip=%s grab=%s shadow=%s scratch=%s out=%s",
          r_memtype(r_data), r_memtype(r_chip), r_memtype(r_grab),
          r_memtype(r_shadow), r_memtype(r_scratch), r_memtype(r_out));
}

#define FB_FLAGS (RFB_F_BASELINE | RFB_F_COPYRECT | RFB_F_SCROLL_ADAPTIVE)

static VOID r_run_sequence(ULONG from, ULONG to, const char *what)
{
    r_log("rfbprof case=%s from=%lu to=%lu", what, from, to);

    /* Interleaved: the two codegens of each arm are adjacent, so a host that
       slows down halfway through cannot make one look better than the other. */
    r_encode_arm("shipping", FB_FLAGS, 0, from, to);
    r_encode_chip_arm("shipping_chip", FB_FLAGS, from, to);
    r_encode_arm("shipping", FB_FLAGS, 1, from, to);
    r_encode_arm("shipping", FB_FLAGS, 2, from, to);
    r_encode_arm("shipping", FB_FLAGS, 3, from, to);
    r_encode_arm("noprobe", FB_FLAGS & ~(RFB_F_COPYRECT
                                         | RFB_F_SCROLL_ADAPTIVE), 0, from, to);
    r_encode_chip_arm("noprobe_chip", FB_FLAGS & ~(RFB_F_COPYRECT
                                         | RFB_F_SCROLL_ADAPTIVE), from, to);
    r_encode_arm("noprobe", FB_FLAGS & ~(RFB_F_COPYRECT
                                         | RFB_F_SCROLL_ADAPTIVE), 1, from, to);
    r_encode_arm("noprobe", FB_FLAGS & ~(RFB_F_COPYRECT
                                         | RFB_F_SCROLL_ADAPTIVE), 2, from, to);
    r_encode_arm("noprobe", FB_FLAGS & ~(RFB_F_COPYRECT
                                         | RFB_F_SCROLL_ADAPTIVE), 3, from, to);
    r_encode_arm("probe_always", (FB_FLAGS & ~RFB_F_SCROLL_ADAPTIVE),
                 0, from, to);
    r_encode_arm("nobestof", FB_FLAGS & ~RFB_F_BESTOF, 0, from, to);
    r_encode_arm("nobestof", FB_FLAGS & ~RFB_F_BESTOF, 1, from, to);
    r_encode_arm("xoronly", (FB_FLAGS & ~(RFB_F_PACKBITS | RFB_F_BESTOF)),
                 0, from, to);
    r_encode_arm("rawtiles", RFB_F_PLANEMASK, 0, from, to);
    r_encode_arm("rawtiles", RFB_F_PLANEMASK, 1, from, to);
    r_encode_arm("diffonly", 0u, 0, from, to);
    r_encode_arm("diffonly", 0u, 1, from, to);
    r_encode_arm("diffonly", 0u, 2, from, to);
    r_encode_arm("diffonly", 0u, 3, from, to);
}

int main(void)
{
    const char *file = NULL;
    char       *args = (char *)GetArgStr();
    rfb_scroll_cfg cfg;
    char        path[128];
    UWORD       depth = 2;

    r_timer_init();

    /* argc is 1 for a guest program; the command line is GetArgStr(). */
    if (args != NULL)
    {
        const char *p = args;

        while (*p == ' ' || *p == '\t')
            p++;
        if (*p != '\0' && *p != '\n' && *p != '\r')
        {
            ULONG i = 0UL;

            while (p[i] != '\0' && p[i] != '\n' && p[i] != '\r'
                   && p[i] != ' ' && i < (ULONG)sizeof(path) - 1UL)
            {
                path[i] = p[i];
                i++;
            }
            path[i] = '\0';
            if (i > 0UL)
                file = path;
        }
    }

    if (file != NULL)
    {
        if (!r_load_pfs(file))
        {
            r_flush();
            return RETURN_FAIL;
        }
    }
    else if (!r_make_synthetic(depth))
    {
        r_log("rfbprof error=no_memory");
        r_flush();
        return RETURN_FAIL;
    }

    r_g.tile_w = 16;
    r_g.tile_h = 16;

    /* A short capture is the wrong instrument for the scroll probe: its gate
       backs off over frames, so a six-frame sequence never lets the backoff
       develop and charges the probe to every frame.  Sixteen frames, and the
       repeat count falls so the wall clock does not. */
    r_reps = 48UL / (r_frames ? r_frames : 1UL);
    if (r_reps == 0UL)
        r_reps = 1UL;

    rfb_scroll_defaults(&cfg);
    r_shadow_len  = rfb_shadow_size(&r_g);
    r_scratch_len = rfb_scratch_size(&r_g, FB_FLAGS, &cfg);
    r_out_cap     = rfb_worst_case_frame(&r_g);

    r_chip    = (UBYTE *)AllocMem(r_frame_bytes, MEMF_CHIP | MEMF_CLEAR);
    r_grab    = (UBYTE *)ami_alloc(r_frame_bytes);
    r_shadow  = (UBYTE *)ami_alloc(r_shadow_len);
    r_scratch = (UBYTE *)ami_alloc(r_scratch_len);
    r_shadow2 = (UBYTE *)ami_alloc(r_shadow_len);
    r_scratch2= (UBYTE *)ami_alloc(r_scratch_len);
    r_out     = (UBYTE *)ami_alloc(r_out_cap);

    if (r_chip == NULL || r_grab == NULL || r_shadow == NULL
        || r_scratch == NULL || r_out == NULL || r_shadow2 == NULL
        || r_scratch2 == NULL)
    {
        r_log("rfbprof error=no_memory shadow=%lu scratch=%lu out=%lu",
              r_shadow_len, r_scratch_len, r_out_cap);
        r_flush();
        return RETURN_FAIL;
    }
    memcpy(r_chip, r_data, (size_t)r_frame_bytes);

    r_report_setup();

    /* THE IDLE CASE FIRST.  Frame 0 primes the shadow and the timed frame is
       frame 1, which for an idle capture is byte-identical to it: this is the
       cost of discovering that nothing happened. */
    if (r_frames >= 2UL)
        r_run_sequence(0UL, 1UL, "second_frame");

    /* And the whole sequence after the first frame, which for a scroll or a
       windows capture is what a session actually pays. */
    if (r_frames >= 3UL)
        r_run_sequence(0UL, r_frames - 1UL, "sequence");

    r_kernels();

    r_log("rfbprof RESULT=OK");
    r_flush();

    FreeMem(r_chip, r_frame_bytes);
    ami_free(r_grab);
    ami_free(r_shadow);
    ami_free(r_scratch);
    ami_free(r_shadow2);
    ami_free(r_scratch2);
    ami_free(r_out);
    ami_free(r_data);
    return RETURN_OK;
}

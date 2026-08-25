/*
 * Open a chipset screen -- HAM6, HAM8 or extra half-brite -- with a picture on
 * it that a wrong decode cannot fake.
 *
 *   chipscreen [MODEID] [WIDTH] [HEIGHT] [DEPTH] [REPORT]
 *                                              default 00021800 320 256 6
 *
 * MODEID is hex, the next three decimal.  DEPTH 0 reports the Workbench screen
 * and returns without opening anything.
 *
 * REPORT must be a file this program opens itself: `Run C:chipscreen >file`
 * binds the redirection to Run, not to this process.
 *
 * It does not return: the screen stays in front for the session and CTRL-C is
 * how the harness takes it down.
 *
 * SPDX-License-Identifier: MIT
 */

#include <exec/types.h>
#include <dos/dos.h>
#include <graphics/displayinfo.h>
#include <graphics/gfxbase.h>
#include <graphics/modeid.h>
#include <intuition/intuition.h>
#include <intuition/intuitionbase.h>
#include <intuition/screens.h>
#include <utility/tagitem.h>

#include <proto/dos.h>
#include <proto/exec.h>
#include <proto/graphics.h>
#include <proto/intuition.h>

#include <stdarg.h>

/* These are the globals the proto/ inlines jump through and nothing in this
   executable's startup fills them in. */
struct GfxBase       *GfxBase;
struct IntuitionBase *IntuitionBase;

/*
 * DOS, never stdio: a Shell command gets 4096 bytes of stack from this
 * toolchain's crt0 and newlib's vfprintf does not fit in it -- the first
 * printf() crashes.  Formats below are RawDoFmt's: every argument 32 bits.
 */
static BPTR rep;

static VOID say(const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    /* Every vararg here is 32 bits, so the list is already the array
       RawDoFmt wants. */
    VFPrintf(rep, (CONST_STRPTR)fmt, (APTR)args);
    va_end(args);
    Flush(rep);
}

/* Must close before the wait that never ends, so a reader on the other side of
   the emulated drive sees a whole file. */
static VOID report_done(VOID)
{
    if (rep != Output())
    {
        Close(rep);
        rep = Output();
    }
}

/* Must match fb_planar_format() in src/tools/httpfb.c: guest and server have
   to agree about what the picture is made of. */
enum { PAT_PLANAR = 0, PAT_HAM6, PAT_HAM8, PAT_EHB };

static const char *pattern_name[] = { "planar", "ham6", "ham8", "ehb" };

#define MAX_WIDTH 1024

static UBYTE rowvals[MAX_WIDTH];

/* Static because a Shell command has 4 KB of stack and this is 776 bytes. */
static ULONG cregs[1 + 3 * 64 + 1];

/* Every component is a multiple of 17, so an ECS colour register -- four bits
   a gun -- holds it without rounding and GetRGB32() hands back what
   LoadRGB32() was given.  Entry 0 must be black: a HAM row starts from it. */
static VOID base_rgb(ULONG i, ULONG *r, ULONG *g, ULONG *b)
{
    if (i == 0UL)
    {
        *r = 0UL; *g = 0UL; *b = 0UL;
        return;
    }
    if (i == 1UL)
    {
        *r = 255UL; *g = 255UL; *b = 255UL;
        return;
    }
    *r = 17UL * (i & 15UL);
    *g = 17UL * (15UL - (i & 15UL));
    *b = 17UL * (((i * 7UL) + (i >> 4)) & 15UL);
}

static VOID load_palette(struct Screen *sc, ULONG colours)
{
    ULONG i;
    ULONG *p = cregs;

    *p++ = (colours << 16) | 0UL;
    for (i = 0; i < colours; i++)
    {
        ULONG r, g, b;

        base_rgb(i, &r, &g, &b);
        /* LoadRGB32 wants 32 bits a gun and takes the top of them, so the byte
           must be replicated, not shifted up. */
        *p++ = r * 0x01010101UL;
        *p++ = g * 0x01010101UL;
        *p++ = b * 0x01010101UL;
    }
    *p = 0UL;

    LoadRGB32(&sc->ViewPort, cregs);
}

/* `shift` is 4 for HAM6 and 6 for HAM8: where the two control bits sit.
   Control codes 2, 3 and 1 are red, green and blue -- the hardware's order. */
static VOID ham_row(ULONG w, ULONG h, ULONG y, ULONG shift)
{
    ULONG n = 1UL << shift;             /* base colours, and ramp steps */
    ULONG band = (y * 4UL) / h;
    ULONG x;

    for (x = 0; x < w; x++)
    {
        ULONG k = (x * n) / w;

        switch (band)
        {
        case 0:  rowvals[x] = (UBYTE)k; break;
        case 1:  rowvals[x] = (UBYTE)((2UL << shift) | k); break;
        case 2:  rowvals[x] = (UBYTE)((3UL << shift) | k); break;
        default: rowvals[x] = (UBYTE)((1UL << shift) | k); break;
        }
    }
}

static VOID ehb_row(ULONG w, ULONG h, ULONG y)
{
    ULONG band = (y * 4UL) / h;
    ULONG x;

    for (x = 0; x < w; x++)
    {
        ULONG k32 = (x * 32UL) / w;
        ULONG k64 = (x * 64UL) / w;

        switch (band)
        {
        case 0:  rowvals[x] = (UBYTE)k32; break;
        case 1:  rowvals[x] = (UBYTE)(32UL + k32); break;
        case 2:  rowvals[x] = (UBYTE)k64; break;
        default: rowvals[x] = (UBYTE)((y + k64) & 63UL); break;
        }
    }
}

static VOID planar_row(ULONG w, ULONG h, ULONG y, ULONG depth)
{
    ULONG n = 1UL << depth;
    ULONG band = (y * 4UL) / h;
    ULONG x;

    for (x = 0; x < w; x++)
    {
        ULONG k = (x * n) / w;

        rowvals[x] = (UBYTE)((band == 3UL) ? ((y + k) & (n - 1UL)) : k);
    }
}

/* Planes[p] + y * BytesPerRow is the row's address in BOTH bitmap layouts: an
   interleaved bitmap's BytesPerRow spans every plane, which is the distance
   from one row of a plane to the next row of the same plane. */
static VOID draw(struct Screen *sc, ULONG w, ULONG h, ULONG depth, int kind)
{
    struct BitMap *bm = sc->RastPort.BitMap;
    ULONG bpr = (ULONG)(UWORD)bm->BytesPerRow;
    ULONG bytes = (w + 7UL) / 8UL;
    ULONG y, b, i, p;

    if (bytes > bpr)
        bytes = bpr;

    /* The blitter may still be finishing the clear Intuition started, and
       these are writes to the same chip RAM. */
    WaitBlit();

    for (y = 0; y < h; y++)
    {
        switch (kind)
        {
        case PAT_HAM6: ham_row(w, h, y, 4UL); break;
        case PAT_HAM8: ham_row(w, h, y, 6UL); break;
        case PAT_EHB:  ehb_row(w, h, y); break;
        default:       planar_row(w, h, y, depth); break;
        }

        for (b = 0; b < bytes; b++)
        {
            UBYTE acc[8];
            ULONG x0 = b * 8UL;

            for (p = 0; p < depth; p++)
                acc[p] = 0;

            for (i = 0; i < 8UL; i++)
            {
                UBYTE v = ((x0 + i) < w) ? rowvals[x0 + i] : (UBYTE)0;

                for (p = 0; p < depth; p++)
                    acc[p] = (UBYTE)((acc[p] << 1) | ((v >> p) & 1U));
            }

            for (p = 0; p < depth; p++)
                bm->Planes[p][y * bpr + b] = acc[p];
        }
    }
}

/* NOT argv: a program started from S:Startup-Sequence sees argc == 1 whatever
   the line said.  GetArgStr() is the line as AmigaDOS passed it. */
static ULONG arg_word(const char **p, ULONG fallback)
{
    const char *s = *p;
    ULONG v = 0;
    int digits = 0;

    while (*s == ' ' || *s == '\t')
        s++;
    while (*s >= '0' && *s <= '9')
    {
        v = v * 10UL + (ULONG)(*s - '0');
        s++;
        digits++;
    }
    *p = s;
    return digits ? v : fallback;
}

static ULONG arg_hex(const char **p, ULONG fallback)
{
    const char *s = *p;
    ULONG v = 0;
    int digits = 0;

    while (*s == ' ' || *s == '\t')
        s++;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        s += 2;
    for (;;)
    {
        ULONG d;

        if (*s >= '0' && *s <= '9')      d = (ULONG)(*s - '0');
        else if (*s >= 'a' && *s <= 'f') d = (ULONG)(*s - 'a') + 10UL;
        else if (*s >= 'A' && *s <= 'F') d = (ULONG)(*s - 'A') + 10UL;
        else break;
        v = (v << 4) | d;
        s++;
        digits++;
    }
    *p = s;
    return digits ? v : fallback;
}

/* Copied out because GetArgStr()'s buffer is not this program's to write a
   terminator into. */
static const char *arg_name(const char **p)
{
    static char name[128];
    const char *s = *p;
    int n = 0;

    while (*s == ' ' || *s == '\t')
        s++;
    while (*s != '\0' && *s != ' ' && *s != '\t' && *s != '\n' &&
           n < (int)sizeof(name) - 1)
        name[n++] = *s++;
    name[n] = '\0';
    *p = s;
    return (n > 0) ? name : NULL;
}

/* Must run before anything is opened: after that the front screen is this
   program's own. */
static VOID report_workbench(VOID)
{
    struct Screen *wb = LockPubScreen(NULL);
    ULONG id;

    if (wb == NULL)
    {
        say("wb_screen=none\n");
        return;
    }

    id = GetVPModeID(&wb->ViewPort);
    say("wb_mode=%08lx\n", (unsigned long)id);
    say("wb_size=%ldx%ld\n", (long)wb->Width, (long)wb->Height);
    say("wb_depth=%ld\n", (long)wb->RastPort.BitMap->Depth);
    say("wb_ham=%s\n", (id != (ULONG)INVALID_ID && (id & HAM_KEY) != 0UL)
                          ? "yes" : "no");
    say("wb_ehb=%s\n",
           (id != (ULONG)INVALID_ID && (id & EXTRAHALFBRITE_KEY) != 0UL)
           ? "yes" : "no");
    UnlockPubScreen(NULL, wb);
}

/* Must stay in step with fb_planar_format() in src/tools/httpfb.c. */
static int pattern_for(ULONG id, ULONG depth)
{
    if ((id & HAM_KEY) != 0UL)
    {
        if (depth == 6UL)
            return PAT_HAM6;
        if (depth == 8UL)
            return PAT_HAM8;
        return PAT_PLANAR;
    }
    if ((id & EXTRAHALFBRITE_KEY) != 0UL && depth == 6UL)
        return PAT_EHB;
    return PAT_PLANAR;
}

/* HAM6 reads sixteen entries and HAM8 sixty-four whatever their depth says;
   EHB reads thirty-two and the hardware halves those itself. */
static ULONG colours_for(int kind, ULONG depth)
{
    switch (kind)
    {
    case PAT_HAM6: return 16UL;
    case PAT_HAM8: return 64UL;
    case PAT_EHB:  return 32UL;
    default:       return 1UL << depth;
    }
}

int main(VOID)
{
    struct Screen *sc;
    const char *args = (const char *)GetArgStr();
    const char *report;
    ULONG id, width, height, depth, colours, open_id;
    int kind;

    if (args == NULL)
        args = "";
    id     = arg_hex(&args, 0x00021800UL);
    width  = arg_word(&args, 320UL);
    height = arg_word(&args, 256UL);
    depth  = arg_word(&args, 6UL);
    report = arg_name(&args);

    rep = Output();
    if (report != NULL)
    {
        BPTR f = Open((CONST_STRPTR)report, MODE_NEWFILE);

        if (f != (BPTR)0)
            rep = f;
    }

    say("args_mode=%08lx\n", (unsigned long)id);
    say("args_size=%lux%lu\n", (unsigned long)width, (unsigned long)height);
    say("args_depth=%lu\n", (unsigned long)depth);

    GfxBase = (struct GfxBase *)
        OpenLibrary((CONST_STRPTR)"graphics.library", 39);
    IntuitionBase = (struct IntuitionBase *)
        OpenLibrary((CONST_STRPTR)"intuition.library", 39);
    if (GfxBase == NULL || IntuitionBase == NULL)
    {
        say("result=no graphics.library or intuition.library at V39\n");
        report_done();
        return RETURN_FAIL;
    }

    report_workbench();

    /* Must RETURN: the success path below never does, so a reporting run that
       fell into it would hang the boot here. */
    if (depth == 0UL)
    {
        say("result=reported\n");
        report_done();
        return RETURN_OK;
    }

    if (width < 16UL || width > MAX_WIDTH || height < 16UL || depth > 8UL)
    {
        say("result=%lux%lux%lu is outside what this draws\n",
               (unsigned long)width, (unsigned long)height,
               (unsigned long)depth);
        report_done();
        return RETURN_FAIL;
    }

    sc = OpenScreenTags(NULL,
                        SA_DisplayID, id,
                        SA_Width,     width,
                        SA_Height,    height,
                        SA_Depth,     depth,
                        SA_Type,      PUBLICSCREEN,
                        SA_PubName,   (ULONG)"CHIPTEST",
                        SA_Title,     (ULONG)"AmiNetXDuo chipset test screen",
                        SA_Quiet,     TRUE,
                        TAG_DONE);

    if (sc == NULL)
    {
        say("result=OpenScreen refused mode %08lx at %lux%lu depth %lu\n",
               (unsigned long)id, (unsigned long)width,
               (unsigned long)height, (unsigned long)depth);
        report_done();
        return RETURN_FAIL;
    }

    /* The mode it really got: Intuition is free to hand back a screen on
       another mode, and the pattern must follow that, not the request. */
    open_id = GetVPModeID(&sc->ViewPort);
    if (open_id == (ULONG)INVALID_ID)
        open_id = id;
    depth = (ULONG)sc->RastPort.BitMap->Depth;

    kind = pattern_for(open_id, depth);
    colours = colours_for(kind, depth);

    load_palette(sc, colours);
    draw(sc, (ULONG)sc->Width, (ULONG)sc->Height, depth, kind);

    PubScreenStatus(sc, 0);
    ScreenToFront(sc);

    say("screen_mode=%08lx\n", (unsigned long)open_id);
    say("screen_size=%ldx%ld\n", (long)sc->Width, (long)sc->Height);
    say("screen_depth=%lu\n", (unsigned long)depth);
    say("pattern=%s\n", pattern_name[kind]);
    say("palette_entries=%lu\n", (unsigned long)colours);
    say("result=open\n");
    report_done();

    Wait(SIGBREAKF_CTRL_C);

    PubScreenStatus(sc, PSNF_PRIVATE);
    CloseScreen(sc);
    return RETURN_OK;
}

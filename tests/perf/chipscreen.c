/*
 * Open a chipset screen -- HAM6, HAM8 or extra half-brite -- with a picture on
 * it that a wrong decode cannot fake.
 *
 *   chipscreen [MODEID] [WIDTH] [HEIGHT] [DEPTH] [REPORT]
 *                                              default 00021800 320 256 6
 *
 * MODEID is hex, the next three decimal.  DEPTH 0 reports the Workbench screen
 * and returns without opening anything, which is how to ask whether
 * screenmode.prefs moved Workbench at all.
 *
 * REPORT is a file to write the key=value lines to, and the harness always
 * names one.  `Run C:chipscreen >file` does NOT redirect this program: the
 * Shell binds that redirection to Run, and Run gives the process it starts an
 * output stream of its own -- the file gets Run's `[CLI 3]` line and nothing
 * else, which is exactly what the first run of this arm collected.  A file
 * this opens itself is the one route that does not depend on how a shell
 * parses the line it was started from.
 *
 * A TEST TOOL, for the same reason tests/perf/rtgscreen.c is one, and then for
 * a second reason that is the whole point of this file.
 *
 * The first is staging.  The ScreenMode editor filters HAM and EHB out of the
 * list it offers, so a Workbench asked for one through screenmode.prefs may
 * simply come up PAL hires two planes deep, and every check the console
 * harness makes passes on that: it is a screen, it has colours, it changes.
 * This opens the screen itself and says what it got.
 *
 * The second is that a HAM decode which is subtly wrong still draws a
 * plausible picture.  Swap the two control bits, take the data from the wrong
 * end of the index, mix up which code means red and which means blue, and the
 * result is still a smooth colourful image -- so a photograph on the screen
 * proves nothing.  What is drawn here is deterministic and asymmetric instead:
 * four horizontal bands, a base-colour band and then a red, a green and a blue
 * ramp in that order, each running dark to bright from left to right.  A
 * permuted control code swaps two bands' colours, a misplaced data field
 * breaks the ramp's monotonicity, and either is visible at a glance rather
 * than plausible.
 *
 * The palette is on a lattice of 17 -- 0x00, 0x11, 0x22 ... 0xff -- so that a
 * 4-bit ECS colour register holds it exactly and the byte read back through
 * GetRGB32() is the byte that was loaded.  Base colour 0 is black, so a HAM
 * row that starts from it has nothing but the ramp in it.
 *
 * It does not return: the screen has to stay in front for as long as the
 * session does, and CTRL-C is how the harness takes it down.
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

/* Opened here, and that is not a formality: these are the globals the proto/
   inlines jump through and nothing in this executable's startup fills them
   in.  See rtgscreen.c, where a NULL GfxBase was a jump through zero. */
struct GfxBase       *GfxBase;
struct IntuitionBase *IntuitionBase;

/*
 * Where every key=value goes: Output() until a REPORT argument opens a file,
 * so a run started by hand from a Shell still prints.
 *
 * DOS AND NOT STDIO, and that is the difference between this file working and
 * not.  This toolchain's crt0 gives a Shell command 4096 bytes of stack and
 * exports no __stack hook to ask for more, and newlib's vfprintf does not fit
 * in it: the first printf() jumps into low memory and takes the machine with
 * it, before one byte of output has been written.  That is why every tool in
 * src/tools goes through tool_printf() rather than printf(), and why the
 * format strings below are RawDoFmt's -- %ld, %lu, %lx, every argument 32
 * bits wide.  tests/perf/rtgscreen.c still uses stdio and dies the same way.
 */
static BPTR rep;

static VOID say(const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    /* (APTR)args is the same cast tool_printf() makes: every vararg here is
       32 bits, so the list is already the array RawDoFmt wants. */
    VFPrintf(rep, (CONST_STRPTR)fmt, (APTR)args);
    va_end(args);
    Flush(rep);
}

/* The report is closed before the wait that never ends, so a reader on the
   other side of the emulated drive sees a whole file rather than whatever the
   filesystem happened to have written out. */
static VOID report_done(VOID)
{
    if (rep != Output())
    {
        Close(rep);
        rep = Output();
    }
}

/* What the picture is made of, which follows the mode and the depth exactly
   as fb_planar_format() in src/tools/httpfb.c follows them.  The guest and the
   server must agree about this or the arm is asserting one thing and drawing
   another. */
enum { PAT_PLANAR = 0, PAT_HAM6, PAT_HAM8, PAT_EHB };

static const char *pattern_name[] = { "planar", "ham6", "ham8", "ehb" };

/* Widths past this are refused rather than drawn short: a row buffer that ran
   out would leave the right of the screen holding whatever chip RAM held, and
   that is a picture nobody can assert anything about. */
#define MAX_WIDTH 1024

static UBYTE rowvals[MAX_WIDTH];

/* 1 + 3 * 64 + 1 longwords.  Static because a Shell command has 4 KB of stack
   for everything, and this is 776 bytes of it. */
static ULONG cregs[1 + 3 * 64 + 1];

/*
 * The base palette, one rule for all three modes.
 *
 * Every component is a multiple of 17, so an ECS colour register -- four bits
 * a gun -- holds it without rounding and GetRGB32() hands back what LoadRGB32()
 * was given.  Entry 0 is black so that a HAM row, which starts from base
 * colour 0, carries only the ramp that follows.  Entry 1 is white, so a picture
 * that lost its palette entirely is obvious.
 *
 * Past those two the red gun counts up within each group of sixteen and the
 * blue gun separates the groups, so all sixty-four entries differ and no two
 * are a neighbour's near miss.
 */
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
        /* LoadRGB32 wants 32 bits a gun and takes the top of them, so the
           byte is replicated rather than shifted up: 0x11 becomes
           0x11111111 and truncating it anywhere still gives 0x11. */
        *p++ = r * 0x01010101UL;
        *p++ = g * 0x01010101UL;
        *p++ = b * 0x01010101UL;
    }
    *p = 0UL;

    LoadRGB32(&sc->ViewPort, cregs);
}

/*
 * One row of HAM indices.  `shift` is 4 for HAM6 and 6 for HAM8, which is
 * where the two control bits sit and how wide the data field under them is.
 *
 * Band 0 is the base colours in order, one block each, and it is the only band
 * that reads the palette.  Bands 1, 2 and 3 are a red, a green and a blue ramp,
 * written with control codes 2, 3 and 1 -- the hardware's order, and the order
 * a decoder gets wrong.  Each ramp runs dark at the left to bright at the right,
 * from a row that started at black.
 */
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

/*
 * One row of extra half-brite indices.
 *
 * Band 0 is 0..31 and band 1 is 32..63 directly under it, so a correct decode
 * puts each half-bright colour immediately below the colour it is half of and
 * the two bands read as one picture at two brightnesses.  A receiver that did
 * not build the second half draws them identical, and one that built it from
 * the wrong entries draws them unrelated.
 *
 * Band 2 is all sixty-four in narrower blocks and band 3 is a diagonal, which
 * is what catches a row stride read one byte wide or one byte narrow.
 */
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

/* A plain indexed screen, for a mode that is neither.  Same four bands, all
   of them indices, so a run that came up planar when it asked for HAM still
   produces something readable to look at. */
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

/*
 * The picture, straight into the bitplanes.
 *
 * WritePixel() per pixel is not an option: 320x256 of them is eighty thousand
 * library calls on an emulated 68020 and the harness would be waiting on the
 * drawing rather than on the boot.  Eight pixels at a time into one byte per
 * plane is the same picture at a fraction of the cost.
 *
 * Planes[p] + y * BytesPerRow is the row's address in both bitmap layouts.  An
 * interleaved bitmap's BytesPerRow spans every plane, which is exactly the
 * distance from one row of a plane to the next row of the same plane, and its
 * Planes[] already point at the right offsets inside the block.
 */
static VOID draw(struct Screen *sc, ULONG w, ULONG h, ULONG depth, int kind)
{
    struct BitMap *bm = sc->RastPort.BitMap;
    ULONG bpr = (ULONG)(UWORD)bm->BytesPerRow;
    ULONG bytes = (w + 7UL) / 8UL;
    ULONG y, b, i, p;

    if (bytes > bpr)
        bytes = bpr;

    /* Nothing else is drawing here -- the screen was opened a moment ago and
       is empty -- but the blitter may still be finishing the clear Intuition
       started, and these are writes to the same chip RAM. */
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

/* NOT argv.  A program the harness starts from S:Startup-Sequence sees
   argc == 1 whatever the line said, so an argv reader takes its defaults in
   silence; GetArgStr() is the line as AmigaDOS passed it.  rtgscreen.c has the
   full story of what that cost. */
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

/* The mode ID, hex, with or without a 0x in front of it. */
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

/* The rest of the line as a file name, or NULL when there is none.  Copied out
   because GetArgStr()'s buffer is not this program's to write a terminator
   into. */
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

/* What the front screen would be without this program: whether screenmode.prefs
   moved Workbench at all, and to what.  Printed before anything is opened,
   because after that the answer is this program's own screen. */
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

/* The same rule as fb_planar_format() in src/tools/httpfb.c, applied to the
   mode the screen really opened on rather than to the one that was asked for.
   Read them together: a change to one without the other is a guest drawing a
   picture the server will describe as something else. */
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

/* How many entries of the base palette the mode uses.  HAM6 reads sixteen of
   them and HAM8 sixty-four whatever their depth says, EHB thirty-two and the
   hardware halves those itself; a plain screen reads all 1 << depth. */
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

    /* Depth 0 reports and stops, and that call has to RETURN: the success path
       below never does, so a reporting run that fell into it would hang the
       boot on this line with the server still further down the sequence. */
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

    /* THE MODE IT REALLY GOT, not the one asked for.  Intuition is free to
       hand back a screen on another mode, and a picture drawn for HAM on a
       plain screen is noise -- so the pattern follows this and the arm can see
       the disagreement in the same output. */
    open_id = GetVPModeID(&sc->ViewPort);
    if (open_id == (ULONG)INVALID_ID)
        open_id = id;
    depth = (ULONG)sc->RastPort.BitMap->Depth;

    kind = pattern_for(open_id, depth);
    colours = colours_for(kind, depth);

    load_palette(sc, colours);
    draw(sc, (ULONG)sc->Width, (ULONG)sc->Height, depth, kind);

    /* Unlocked, so anything may use it, and in front, because the front screen
       is the one the console serves. */
    PubScreenStatus(sc, 0);
    ScreenToFront(sc);

    say("screen_mode=%08lx\n", (unsigned long)open_id);
    say("screen_size=%ldx%ld\n", (long)sc->Width, (long)sc->Height);
    say("screen_depth=%lu\n", (unsigned long)depth);
    say("pattern=%s\n", pattern_name[kind]);
    say("palette_entries=%lu\n", (unsigned long)colours);
    say("result=open\n");
    report_done();

    /* And it stays.  A screen that closed when this returned would be in front
       for no part of the session it exists for. */
    Wait(SIGBREAKF_CTRL_C);

    PubScreenStatus(sc, PSNF_PRIVATE);
    CloseScreen(sc);
    return RETURN_OK;
}

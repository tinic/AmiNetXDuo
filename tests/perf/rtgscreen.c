/*
 * Ask the machine what RTG screen modes it really has, and open one.
 *
 *   rtgscreen [DEPTH] [WIDTH] [HEIGHT]      default 8 640 480
 *
 * A TEST TOOL, and it exists because the console harness could not tell two
 * failures apart.  Staging Picasso96 onto a guest and then reading the depth
 * out of the server's `geom` word says only that Workbench came up on the
 * chipset; it does not say whether the card has no modes, whether the mode ID
 * written into screenmode.prefs was the wrong one, or whether Intuition
 * refused to open on it.  Three staging faults in a row all looked identical
 * from outside.
 *
 * So this asks the display database itself, prints what it finds as
 * key=value, and then OPENS a public screen of the requested depth and leaves
 * it in front.  That is what the console serves: the front screen, whatever
 * put it there, and a public one is the only kind it will read on a card.
 *
 * PUBLIC AND UNLOCKED, deliberately.  http_fb_grab_frame() reads an RTG screen
 * only while a real screen lock is held -- the fetch is a library call against
 * the screen's RastPort and a screen that closed under it is not the one bad
 * frame that reading freed chip RAM is -- so a CUSTOMSCREEN here would be
 * refused and would prove nothing.
 *
 * It does not return: the screen has to stay up for as long as the session
 * does, and CTRL-C is how the harness takes it down.
 *
 * SPDX-License-Identifier: MIT
 */

#include <exec/types.h>
#include <dos/dos.h>
#include <graphics/displayinfo.h>
#include <graphics/modeid.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <intuition/intuitionbase.h>
#include <graphics/gfxbase.h>
#include <utility/tagitem.h>

#include <proto/dos.h>
#include <proto/exec.h>
#include <proto/graphics.h>
#include <proto/intuition.h>

#include <stdio.h>
#include <stdlib.h>

/* cybergraphics.library, hand-declared for the reason src/tools/httprtg.c
   declares it: the toolchain ships no header for it.  Offsets from
   cybergraphics.fd. */
#include <inline/macros.h>

#define CYBRBIDTG_TB            (TAG_USER + 0x50000)
#define CYBRBIDTG_Depth         (CYBRBIDTG_TB + 0)
#define CYBRBIDTG_NominalWidth  (CYBRBIDTG_TB + 1)
#define CYBRBIDTG_NominalHeight (CYBRBIDTG_TB + 2)

#define CYBRIDATTR_PIXFMT       0x80000001UL
#define CYBRIDATTR_WIDTH        0x80000002UL
#define CYBRIDATTR_HEIGHT       0x80000003UL
#define CYBRIDATTR_DEPTH        0x80000004UL

/* OPENED HERE, and that is not a formality.  These bases are globals the
   proto/ inlines jump through, and nothing in this executable's startup opens
   them: a first call to NextDisplayInfo() with a NULL GfxBase is a jump
   through zero, which is a guest that writes not one line of the output this
   tool exists to produce. */
struct GfxBase       *GfxBase;
struct IntuitionBase *IntuitionBase;

static struct Library *CyberGfxBase;

static ULONG cgx_best(struct TagItem *tags)
{
    return LP1(0x3c, ULONG, BestCModeIDTagList, struct TagItem *, tags, a0,
               , CyberGfxBase);
}

static ULONG cgx_id_attr(ULONG attr, ULONG id)
{
    return LP2(0x66, ULONG, GetCyberIDAttr, ULONG, attr, d0, ULONG, id, d1,
               , CyberGfxBase);
}

static BOOL cgx_is_cyber(ULONG id)
{
    return LP1(0x36, BOOL, IsCyberModeID, ULONG, id, d0, , CyberGfxBase);
}

/* Every mode the display database holds, and which of them are the card's.
   NextDisplayInfo() walks the lot; IsCyberModeID() is the one question that
   separates a graphics card's mode from a chipset one. */
static VOID list_modes(ULONG want_depth)
{
    ULONG id = (ULONG)INVALID_ID;
    ULONG n = 0, rtg = 0, match = 0;

    while ((id = NextDisplayInfo(id)) != (ULONG)INVALID_ID)
    {
        struct DimensionInfo dim;
        DisplayInfoHandle handle;
        ULONG depth;

        n++;
        if (CyberGfxBase == NULL || !cgx_is_cyber(id))
            continue;
        rtg++;

        /* THE HANDLE, NOT A NULL AND THE ID.  GetDisplayInfoData() accepts
           either, but the NULL form puts a compile-time zero in a0 -- and the
           NDK's LP5 already binds a0 to one of its own clobber operands, so
           gcc satisfies the argument's "rf" constraint from a data register
           instead and the library is entered with whatever a0 happened to
           hold.  That is not a theory: -Os put the zero in d7 here, this call
           returned 0 for every card mode, and moving an unrelated printf()
           changed it from "no dimensions" to an illegal instruction that took
           the machine down mid-boot.  FindDisplayInfo() hands over a real
           pointer, which gcc does keep in a0. */
        handle = FindDisplayInfo(id);
        if (handle == NULL)
            continue;

        if (GetDisplayInfoData(handle, (UBYTE *)&dim, sizeof(dim), DTAG_DIMS,
                               0) == 0)
            continue;

        depth = cgx_id_attr(CYBRIDATTR_DEPTH, id);
        printf("rtgmode=%08lx %ldx%ld depth=%ld\n", (unsigned long)id,
               (long)(dim.Nominal.MaxX - dim.Nominal.MinX + 1),
               (long)(dim.Nominal.MaxY - dim.Nominal.MinY + 1),
               (long)depth);
        if (depth == want_depth)
            match++;
    }

    printf("modes_total=%lu\n", (unsigned long)n);
    printf("modes_rtg=%lu\n", (unsigned long)rtg);
    printf("modes_at_depth=%lu\n", (unsigned long)match);
}

/* NOT argv.  A program this harness starts from S:Startup-Sequence sees
   argc == 1 whatever the line said -- this toolchain's crt0 does not build
   argv -- so an argv reader takes its defaults in silence.  That is not a
   theory either: `rtgscreen 0` read 8 from its own default, opened a screen
   instead of listing, and never returned, which stopped the boot at that line
   with the server three lines further down.  GetArgStr() is the line as
   AmigaDOS passed it. */
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

int main(VOID)
{
    struct Screen *sc;
    const char *args = (const char *)GetArgStr();
    ULONG depth, width, height;
    ULONG id = (ULONG)INVALID_ID;

    if (args == NULL)
        args = "";
    depth  = arg_word(&args, 8UL);
    width  = arg_word(&args, 640UL);
    height = arg_word(&args, 480UL);

    /* Unbuffered, because the success path does not return: it opens the
       screen and waits, so anything still sitting in a stdio buffer is
       output nobody ever sees. */
    setvbuf(stdout, NULL, _IONBF, 0);

    /* Printed, because the defaults and what the harness passes are the same
       numbers: without this line a run that read nothing looks exactly like a
       run that read its arguments. */
    printf("args=%lu %lu %lu\n", (unsigned long)depth, (unsigned long)width,
           (unsigned long)height);

    GfxBase = (struct GfxBase *)
        OpenLibrary((CONST_STRPTR)"graphics.library", 39);
    IntuitionBase = (struct IntuitionBase *)
        OpenLibrary((CONST_STRPTR)"intuition.library", 39);
    if (GfxBase == NULL || IntuitionBase == NULL)
    {
        printf("result=no graphics.library or intuition.library at V39\n");
        return RETURN_FAIL;
    }

    CyberGfxBase = OpenLibrary((CONST_STRPTR)"cybergraphics.library", 40);
    printf("cybergraphics=%s\n", CyberGfxBase != NULL ? "yes" : "no");

    list_modes(depth);

    /* DEPTH 0 LISTS AND STOPS, which is how the harness asks the question
       from S:Startup-Sequence.  That call has to RETURN: the success path
       below never does, so a listing run that fell into it hung the boot on
       that line and everything the sequence starts after it -- the server
       included -- was simply never reached. */
    if (depth == 0)
    {
        printf("result=listed\n");
        if (CyberGfxBase != NULL)
            CloseLibrary(CyberGfxBase);
        return RETURN_OK;
    }

    /* ASKED FOR, NOT ASSUMED.  The harness wrote a mode ID into
       screenmode.prefs that it worked out from the emulator's source, which is
       a guess about somebody else's numbering; this asks the machine which
       mode is best for the shape wanted and prints what it answered, so a
       disagreement between the two is visible rather than silent. */
    if (CyberGfxBase != NULL)
    {
        struct TagItem tags[4];

        tags[0].ti_Tag = CYBRBIDTG_Depth;         tags[0].ti_Data = depth;
        tags[1].ti_Tag = CYBRBIDTG_NominalWidth;  tags[1].ti_Data = width;
        tags[2].ti_Tag = CYBRBIDTG_NominalHeight; tags[2].ti_Data = height;
        tags[3].ti_Tag = TAG_DONE;                tags[3].ti_Data = 0;

        id = cgx_best(tags);
        printf("best_mode=%08lx\n", (unsigned long)id);
    }

    if (id == (ULONG)INVALID_ID)
    {
        printf("result=no mode of depth %lu at %lux%lu\n",
               (unsigned long)depth, (unsigned long)width,
               (unsigned long)height);
        if (CyberGfxBase != NULL)
            CloseLibrary(CyberGfxBase);
        return RETURN_FAIL;
    }

    sc = OpenScreenTags(NULL,
                        SA_DisplayID, id,
                        SA_Width,     width,
                        SA_Height,    height,
                        SA_Depth,     depth,
                        SA_Type,      PUBLICSCREEN,
                        SA_PubName,   (ULONG)"RTGTEST",
                        SA_Title,     (ULONG)"AmiNetXDuo RTG test screen",
                        SA_Quiet,     TRUE,
                        TAG_DONE);

    if (sc == NULL)
    {
        printf("result=OpenScreen refused mode %08lx at depth %lu\n",
               (unsigned long)id, (unsigned long)depth);
        if (CyberGfxBase != NULL)
            CloseLibrary(CyberGfxBase);
        return RETURN_FAIL;
    }

    /* Unlocked, so anything may use it, and in FRONT, because the front screen
       is the one the console serves. */
    PubScreenStatus(sc, 0);
    ScreenToFront(sc);

    printf("screen=%ldx%ldx%d mode=%08lx\n", (long)sc->Width, (long)sc->Height,
           (int)sc->RastPort.BitMap->Depth, (unsigned long)id);
    printf("result=open\n");
    fflush(stdout);

    /* And it stays.  A screen that closed when this returned would be in
       front for no part of the session it exists for. */
    Wait(SIGBREAKF_CTRL_C);

    PubScreenStatus(sc, PSNF_PRIVATE);
    CloseScreen(sc);
    if (CyberGfxBase != NULL)
        CloseLibrary(CyberGfxBase);
    return RETURN_OK;
}

/*
 * The frontmost screen down a WebSocket.  See httpfb.h for what this is and
 * what it deliberately is not.
 *
 * THE FRONT SCREEN, THE WAY RTG DOES IT
 *
 *   IntuitionBase->FirstScreen is what is in front, and that is what is served.
 *   It used to be LockPubScreen("Workbench") and nothing else, which meant the
 *   Palette and Overscan editors -- each of which opens a screen of its own
 *   that is not public -- locked a remote viewer out: the screen opened in
 *   front on the machine and the browser went on showing an unchanged
 *   Workbench with no way to see or dismiss it.
 *
 *   The whole front screen, at its own origin, and nothing behind it.  That is
 *   the RTG metaphor -- on a graphics card the front screen owns the display
 *   and screen dragging does not exist -- and it is deliberately not a
 *   composite of a dragged-down screen over what is behind it.  sc->LeftEdge
 *   and sc->TopEdge are read for one purpose only, aiming injected pointer
 *   events at rows that have been dragged away from the top of the view; they
 *   do not move the picture.
 *
 * THE GRAB IS wbgrab's
 *
 *   open_libraries(), geometry_of(), read_palette() and grab_frame() are
 *   src/tools/wbgrab.c's, lifted rather than rewritten: that file is the grab
 *   half of this and is already right about the things that are easy to be
 *   wrong about -- BMF_STANDARD before Planes[] is read at all, an interleaved
 *   BitMap's BytesPerRow spanning every plane, a ColorMap shorter than the
 *   screen's depth, and the screen being re-examined on EVERY grab because the
 *   one it locked may not be the one it was told about.
 *
 * ONE MESSAGE IN FLIGHT
 *
 *   There is one transmit buffer and it holds one WebSocket frame.  Nothing is
 *   produced while it still has bytes in it, which is the whole of the flow
 *   control in this direction and is what a framebuffer needs rather than a
 *   queue: when the LAN cannot carry 25 frames a second, the right thing to
 *   drop is the frames that were never grabbed, not to build a backlog of
 *   pictures of what the screen looked like a second ago.
 *
 * SPDX-License-Identifier: MIT
 */

#include "tools.h"
#include "toolsock.h"
#include "httpws.h"
#include "httpfb.h"

#include "aminetxduo/rfb_encode.h"
#include "aminetxduo/rfb_words.h"

#include <devices/input.h>
#include <devices/inputevent.h>
#include <dos/dosextens.h>
#include <exec/io.h>
#include <exec/memory.h>
#include <graphics/gfx.h>
#include <graphics/gfxbase.h>
#include <graphics/layers.h>
#include <graphics/view.h>
#include <intuition/intuition.h>
#include <intuition/intuitionbase.h>
#include <intuition/preferences.h>
#include <intuition/screens.h>
#include <prefs/pointer.h>

#include <proto/graphics.h>
#include <proto/intuition.h>

#define FB_MAX_DEPTH        RFB_MAX_DEPTH
#define FB_MAX_COLOURS      (1U << FB_MAX_DEPTH)

/*
 * The encoder's shipping configuration.  BASELINE is PackBits, XOR, the plane
 * mask and best-of; COPYRECT is the scroll detector and SCROLL_ADAPTIVE is
 * what keeps it from probing on a frame where nothing moved.  Measured on real
 * captures: 5 bytes for an idle frame, 272 for windows opening, 1412 for a
 * shell scrolling.
 *
 * RFB_F_INTERLEAVED IS ADDED AT RUN TIME, in fb_take_buffers(), when the
 * screen's BitMap is one.  It used to be impossible to need: the grab
 * de-interleaved into a plane-major buffer of ours and the encoder only ever
 * saw that.  The encoder reads the bitplanes where they are now, so the layout
 * it is told about has to be the layout they are actually in -- and the flag is
 * what lays the shadow out to match, since the two are walked with one stride.
 */
#define FB_FLAGS    (RFB_F_BASELINE | RFB_F_COPYRECT | RFB_F_SCROLL_ADAPTIVE)

/* The most a text frame from a viewer can be worth reading.  Every word in the
   vocabulary is a keyword and at most three small numbers. */
#define FB_WORD_MAX         48

/* How often the session says what it is costing.  Frames, because a count is
   what the arithmetic afterwards divides by.  32 and not 128: a screen that is
   scrolling costs half a second a frame, so a twenty-second run of the case
   the figure is most wanted for produces thirty-five frames. */
#define FB_STAT_EVERY       32

/* The floor between grabs, in fiftieths.  One tick, so a screen nothing is
 * drawing on does not have 40 KB of chip RAM read on every pass of a loop
 * whose wait is two milliseconds.  It is not a frame rate: it is the point
 * past which grabbing again cannot produce anything a viewer could see. */
#define FB_GRAB_FLOOR       1

/*
 * HOW OFTEN A `refresh` CAN ACTUALLY FORCE A FULL FRAME.  Fiftieths.
 *
 * A refresh is expensive and asymmetric: the answer is a whole screen, about
 * 7 KB at 640x256x4, against the 5 bytes an idle frame costs.  A viewer that
 * asks once -- it lost sync, it saw a sequence gap -- must get one AT ONCE,
 * and does: the floor only applies to the second and later ask inside a
 * second.  A viewer that asks on every frame degrades to one re-sync a
 * second instead of a saturated link that never carries anything else.
 *
 * The number is one second because that is well above the grab rate and well
 * below anything a person would notice as a stall in a picture that is, by
 * construction, already correct.
 */
#define FB_RESYNC_FLOOR     50UL

/*
 * HOW LONG "THERE IS NO SCREEN" IS ALLOWED TO LAST.  Fiftieths.
 *
 * A RESOLUTION change closes the Workbench screen and opens a new one, and
 * between the two Intuition's screen list is EMPTY.  With no other screen open
 * -- no Shell window holding one, nothing else running -- that is a real
 * moment with nothing to serve, and it used to end the session: "there are no
 * screens left to show", on a machine that was perfectly well and about to
 * show a bigger screen.  Reproduced first time by copying a lace prefs file
 * over ENV:Sys/screenmode.prefs with the boot Shell ended; a DEPTH change does
 * not do it, because Intuition rebuilds the bitmap without closing the screen.
 *
 * So an empty list is a moment to wait through, not an error.  Measured over
 * eight resolution changes and two overscan changes, the gap is ONE OR TWO
 * passes of this loop -- gn= counted 2 across two reopens -- which is tens of
 * milliseconds.  Ten seconds is three orders of magnitude beyond that, and it
 * is the same order as the session's own liveness timeout: a viewer never
 * waits longer to find out the screen is gone for good than it would wait to
 * find out its peer is dead.
 */
#define FB_GONE_GRACE       500UL

struct GfxBase       *GfxBase;
struct IntuitionBase *IntuitionBase;

/*
 * What the header says and what the copy loop needs.  row_stride and row_bytes
 * differ only on an interleaved bitmap, where BytesPerRow spans every plane
 * and one plane's row is that divided by the depth.
 */
typedef struct FbGeometry
{
    UWORD width;
    UWORD height;
    UWORD depth;
    UWORD row_bytes;
    ULONG row_stride;
    ULONG frame_bytes;
    UWORD interleaved;      /* BMF_INTERLEAVED, as the BitMap reported it */
} FbGeometry;

/* ------------------------------------------------------------- the module -- */

static BOOL fb_on;
static char fb_why[200];

static FbGeometry fb_open_geom;

/* ------------------------------------------------------------ the session -- */

static BOOL            fb_live;
static struct Library *fb_sb;
static LONG            fb_sock = -1;

static HttpWsIn        fb_in;
static UBYTE           fb_ctl[HTTP_FB_CTL];
static UWORD           fb_ctl_n;
static UWORD           fb_ctl_at;
static UBYTE           fb_pinged;
static UBYTE           fb_closing;
static UWORD           fb_close_code;
static ULONG           fb_progress;

static char            fb_word[FB_WORD_MAX + 1];
static UWORD           fb_word_n;
static UBYTE           fb_word_over;    /* this message is longer than we read */

static FbGeometry      fb_geom;
static rfb_geom        fb_rg;
static rfb_encoder     fb_enc;
static rfb_scroll_cfg  fb_cfg;
static rfb_u32         fb_flags;        /* FB_FLAGS, plus the layout's own */

static UBYTE          *fb_shadow;
static UBYTE          *fb_scratch;
static UBYTE          *fb_tx;
static ULONG           fb_shadow_len;
static ULONG           fb_scratch_len;
static ULONG           fb_tx_cap;
static ULONG           fb_tx_len;
static ULONG           fb_tx_sent;

static UBYTE           fb_pal[3U * FB_MAX_COLOURS];
static UBYTE           fb_want_geom;
static UBYTE           fb_want_pal;
static UBYTE           fb_want_stat;

static ULONG           fb_next_tick;

/*
 * A RESYNC IS A SEQUENCE, NOT AN EVENT, AND ASKING TWICE MUST NOT RESTART IT
 *
 * Honouring a refresh queues geom, then pal, then a full frame, and clears
 * the shadow so that frame is decodable from zero.  A second refresh arriving
 * inside that sequence has nothing to add -- the shadow is already zero and
 * the full frame is already coming -- and acting on it would re-queue geom
 * and re-clear a shadow the viewer has not yet been given a frame from.  A
 * client that asks on every geom would then be answered with a geom, ask
 * again, and never get past the handshake.  No client here does that; the
 * guard is on this side because it is the side that protects every client
 * that will ever be written.
 */
static UBYTE           fb_resync;      /* the sequence is under way        */
static UBYTE           fb_resync_due;  /* asked under the floor; owed one  */
static UBYTE           fb_resync_ever; /* fb_resync_at means something     */
static ULONG           fb_resync_at;   /* when the last one was honoured   */

static ULONG           fb_frames;
static ULONG           fb_bytes;
static ULONG           fb_grab_ticks;
static ULONG           fb_encode_ticks;
static ULONG           fb_since_stat;

/* Frames read without the layer lock, because somebody else had it.  Reported
   in `fbstat` so a session that looks torn can be told apart from one that is
   dropping frames. */
static ULONG           fb_torn;

/*
 * The screen list was empty on the last pass, and when it first was.  A
 * screen being reopened in another resolution is the ordinary reason; see
 * FB_GONE_GRACE.  `fb_gone_passes` is reported in `fbstat` as gn=, because
 * "we sometimes lose the connection" is a report that needs a number behind
 * it and this is the number.
 */
static UBYTE           fb_gone;
static ULONG           fb_gone_at;
static ULONG           fb_gone_passes;

/* A `reset` arrived and the machine goes as soon as the close frame telling
   the viewer so has been handed to the socket.  See fb_reboot(). */
static UBYTE           fb_reset;

/* ------------------------------------------------------------------ input -- */

/*
 * input.device, opened with -C and held for the server's life.  One port, one
 * request and one event, taken once: an event is written per mouse move and
 * allocating for each would be a thousand AllocMem/FreeMem pairs a minute.
 *
 * The event is STATIC and not automatic for the reason every buffer here is:
 * a Shell command has 4 KB of stack on a stock Kickstart 3.1, and this one is
 * also the thing io_Data points at while DoIO() runs.
 */
static struct MsgPort   *fb_in_port;
static struct IOStdReq  *fb_in_req;
static BOOL              fb_in_open;
static struct InputEvent fb_event;

/*
 * WHAT ie_X AND ie_Y ARE MEASURED IN, WHICH IS NOT SCREEN PIXELS
 *
 *   IECLASS_POINTERPOS carries a position in the VIEW's units, and a screen's
 *   pixels are only the same thing when the screen is hires and interlaced.
 *   A stock Workbench is 640x256 -- hires, NOT laced -- and its 256 rows are
 *   512 view lines, so a row handed straight through lands at half its
 *   number.  Measured: `m 300 200` put the pointer on screen row 100, an
 *   error of nothing at the top of the screen and 128 rows at the bottom,
 *   which is what a viewer whose clicks miss by more the further down you go
 *   is actually reporting.
 *
 *   The two factors are read off the ViewPort's Modes and kept here, because
 *   the injectors have a word from a viewer and no screen in hand.  A screen
 *   that changes mode under a live session changes them on the next grab.
 *   Halves, not whole numbers: superhires is HALF a view unit per pixel.
 */
static UWORD             fb_x_halves = 2;   /* view units per pixel, doubled */
static UWORD             fb_y_halves = 4;

/*
 * WHERE THE FRONT SCREEN SITS IN THE VIEW, in its own pixels.
 *
 *   Zero for every screen nobody has dragged, which is every screen this
 *   normally sees.  The picture is sent whole at the screen's own origin
 *   whatever these are -- the front screen owns the display, the way it does
 *   on RTG -- so they move nothing about what is drawn.  They exist because an
 *   injected IECLASS_POINTERPOS is a position in the VIEW, and a screen
 *   dragged down by fifty rows has its row 0 fifty rows into the view: without
 *   this the picture would be right and every click in it fifty rows high.
 */
static WORD              fb_left;
static WORD              fb_top;

/* What the far end is holding down, as IEQUALIFIER_ bits.  Intuition reads the
   button state off the qualifier of every RAWMOUSE event, not off a history of
   the codes, so a button that goes down and is not carried in the qualifier of
   what follows reads as released. */
static UWORD             fb_buttons;

/* ------------------------------------------------------------ diagnostics -- */

static VOID fb_say(const char *text)
{
    ULONG i;

    for (i = 0; i + 1U < sizeof(fb_why) && text[i] != '\0'; i++)
        fb_why[i] = text[i];
    fb_why[i] = '\0';
}

/* Decimal, into a caller's buffer, returning where it ended.  Two callers: a
   sentence for a person, and the `fbstat` word.  No printf: this module has to
   build the same message into a close frame, and a Shell command has 4 KB of
   stack to do it on. */
static ULONG fb_put_num(UBYTE *out, ULONG cap, ULONG at, ULONG v)
{
    char  digits[12];
    ULONG n = 0;

    do {
        digits[n++] = (char)('0' + (v % 10UL));
        v /= 10UL;
    } while (v != 0UL);

    while (n > 0UL && at < cap)
        out[at++] = (UBYTE)digits[--n];

    return at;
}

/* One number in a sentence, which is all the reporting here needs beyond a
   fixed string. */
static VOID fb_say3(const char *a, ULONG v, const char *b)
{
    ULONG at = 0;
    ULONG i;

    for (i = 0; a[i] != '\0' && at + 1U < sizeof(fb_why); i++)
        fb_why[at++] = a[i];

    at = fb_put_num((UBYTE *)fb_why, (ULONG)sizeof(fb_why) - 1U, at, v);

    for (i = 0; b[i] != '\0' && at + 1U < sizeof(fb_why); i++)
        fb_why[at++] = b[i];

    fb_why[at] = '\0';
}

const char *http_fb_fault(VOID)
{
    return fb_why;
}

/* ---------------------------------------------------------------- the clock */

/* Fiftieths, wrapping at midnight the way httpterm.c's does.  A wrap makes one
   slice measurement wrong once a day, on a counter that is a mean over
   hundreds of them. */
static ULONG fb_ticks(VOID)
{
    struct DateStamp ds;

    (VOID)DateStamp(&ds);

    return (ULONG)ds.ds_Minute * 3000UL + (ULONG)ds.ds_Tick;
}

/* ---------------------------------------------------------------- library -- */

static BOOL fb_open_libraries(VOID)
{
    GfxBase = (struct GfxBase *)
        OpenLibrary((CONST_STRPTR)"graphics.library", 39);
    IntuitionBase = (struct IntuitionBase *)
        OpenLibrary((CONST_STRPTR)"intuition.library", 39);

    if (GfxBase != NULL && IntuitionBase != NULL)
        return TRUE;

    fb_say("this needs Kickstart 3.0 or later: graphics and intuition must "
           "both answer OpenLibrary() at version 39");
    return FALSE;
}

static VOID fb_close_libraries(VOID)
{
    if (IntuitionBase != NULL)
    {
        CloseLibrary((struct Library *)IntuitionBase);
        IntuitionBase = NULL;
    }
    if (GfxBase != NULL)
    {
        CloseLibrary((struct Library *)GfxBase);
        GfxBase = NULL;
    }
}

/* --------------------------------------------------- which screen is front -- */

/*
 * TRUE when `sc` is still one of Intuition's screens.  THE CALLER MUST HOLD
 * LockIBase(), and that is the whole point of it: CloseScreen() takes
 * IntuitionBase to unlink a screen before it frees it, so a screen found in
 * the list under that lock cannot be freed for as long as the lock is held.
 */
static BOOL fb_listed(struct Screen *sc)
{
    struct Screen *s;

    for (s = IntuitionBase->FirstScreen; s != NULL; s = s->NextScreen)
    {
        if (s == sc)
            return TRUE;
    }

    return FALSE;
}

/*
 * The frontmost screen, locked if it is one that CAN be locked.
 *
 * IntuitionBase->FirstScreen is a field and not a function, so it is read
 * under LockIBase() and the lock is dropped at once: Intuition is blocked for
 * as long as it is held and this runs many times a second.
 *
 * A REAL LOCK IS TAKEN WHENEVER ONE EXISTS.  A public screen can be held by
 * name for the whole of a grab, and the screen then cannot close underneath --
 * which is the guarantee the Workbench-only version had and is worth keeping
 * unchanged for the case that is nearly always in front.  Its name comes off
 * the public screen list rather than being assumed to be "Workbench", so any
 * public screen in front gets the same treatment.
 *
 * A screen that is NOT public -- which the Palette and Overscan editors' are,
 * and which is exactly why LockPubScreen() could not reach them -- has no such
 * handle in 3.1.  It comes back unlocked and fb_grab_frame() reads it under
 * the shorter guarantee LockIBase() gives instead.
 */
static struct Screen *fb_lock_front(BOOL *pub)
{
    /* Static for the reason every buffer in this file is: a Shell command has
       4 KB of stack and MAXPUBSCREENNAME is 139 of them. */
    static char           name[MAXPUBSCREENNAME + 1];
    struct List          *list;
    struct PubScreenNode *node;
    struct Screen        *front;
    struct Screen        *got;
    ULONG                 ilock;
    UWORD                 i;
    BOOL                  named = FALSE;

    *pub = FALSE;

    ilock = LockIBase(0);
    front = IntuitionBase->FirstScreen;
    UnlockIBase(ilock);

    if (front == NULL)
        return NULL;

    list = LockPubScreenList();
    if (list != NULL)
    {
        for (node = (struct PubScreenNode *)list->lh_Head;
             node->psn_Node.ln_Succ != NULL;
             node = (struct PubScreenNode *)node->psn_Node.ln_Succ)
        {
            if (node->psn_Screen != front || node->psn_Node.ln_Name == NULL)
                continue;

            for (i = 0; i < (UWORD)MAXPUBSCREENNAME &&
                        node->psn_Node.ln_Name[i] != '\0'; i++)
                name[i] = node->psn_Node.ln_Name[i];
            name[i] = '\0';
            named = TRUE;
            break;
        }
        UnlockPubScreenList();
    }

    if (named)
    {
        got = LockPubScreen((CONST_STRPTR)name);

        if (got == front)
        {
            *pub = TRUE;
            return front;
        }

        /* The name is now somebody else's screen, so the lock is not a lock on
           the one in front.  Given back, and the front pointer goes out
           unlocked to be re-checked against the screen list. */
        if (got != NULL)
            UnlockPubScreen(NULL, got);
    }

    return front;
}

/* --------------------------------------------------------------- geometry -- */

/*
 * FALSE having said why.  Every refusal here is a bitmap this cannot read
 * correctly, so none of them may fall through to a grab of something else.
 */
static BOOL fb_geometry_of(struct BitMap *bm, FbGeometry *g)
{
    ULONG flags;
    ULONG depth;
    ULONG width;
    ULONG height;
    ULONG stride;
    UWORD plane;

    if (bm == NULL)
    {
        fb_say("the front screen has no bitmap");
        return FALSE;
    }

    flags  = GetBitMapAttr(bm, BMA_FLAGS);
    depth  = GetBitMapAttr(bm, BMA_DEPTH);
    width  = GetBitMapAttr(bm, BMA_WIDTH);
    height = GetBitMapAttr(bm, BMA_HEIGHT);

    if ((flags & BMF_STANDARD) == 0)
    {
        fb_say("the front screen is not a standard planar bitmap, so it has "
               "no bitplanes to read; this serves planar screens only");
        return FALSE;
    }

    if (depth < 1 || depth > FB_MAX_DEPTH)
    {
        fb_say3("the front screen is ", depth,
                " planes deep; this handles 1 to 8");
        return FALSE;
    }

    if (width < 1 || width > 65535 || height < 1 || height > 65535)
    {
        fb_say("the front screen does not fit the wire format");
        return FALSE;
    }

    stride = (ULONG)(UWORD)bm->BytesPerRow;

    /* An interleaved bitmap's BytesPerRow spans every plane; the stride from
       one row to the next in a plane is that, and a plane's row is a depth of
       it.  A non-interleaved one has the two equal. */
    g->interleaved = (UWORD)(((flags & BMF_INTERLEAVED) != 0) ? 1 : 0);

    if (g->interleaved)
    {
        if (stride == 0 || (stride % depth) != 0)
        {
            fb_say3("interleaved bitmap with BytesPerRow=", stride,
                    ", which does not divide by the depth");
            return FALSE;
        }
        g->row_bytes = (UWORD)(stride / depth);
    }
    else
    {
        g->row_bytes = (UWORD)stride;
    }

    if ((ULONG)g->row_bytes * 8UL < width)
    {
        fb_say3("the bitmap says ", (ULONG)g->row_bytes,
                " bytes a row, which is too few for its width");
        return FALSE;
    }

    for (plane = 0; plane < (UWORD)depth; plane++)
    {
        if (bm->Planes[plane] == NULL)
        {
            fb_say3("bitplane ", (ULONG)plane, " is not allocated");
            return FALSE;
        }
    }

    g->width       = (UWORD)width;
    g->height      = (UWORD)height;
    g->depth       = (UWORD)depth;
    g->row_stride  = stride;
    g->frame_bytes = (ULONG)g->row_bytes * (ULONG)g->height * depth;

    return TRUE;
}

static BOOL fb_geometry_same(const FbGeometry *a, const FbGeometry *b)
{
    return (BOOL)(a->width == b->width && a->height == b->height &&
                  a->depth == b->depth && a->row_bytes == b->row_bytes &&
                  a->row_stride == b->row_stride &&
                  a->interleaved == b->interleaved);
}

VOID http_fb_geometry(UWORD *w, UWORD *h, UWORD *depth)
{
    if (w != NULL)     *w = fb_open_geom.width;
    if (h != NULL)     *h = fb_open_geom.height;
    if (depth != NULL) *depth = fb_open_geom.depth;
}

/* ---------------------------------------------------------------- palette -- */

/*
 * 3 * (1 << depth) bytes whatever the ColorMap holds: a screen whose map is
 * shorter than its depth leaves the tail black rather than shortening the
 * word, so the receiver's arithmetic is the depth and nothing else.
 *
 * TRUE when it changed, which is what decides whether a `pal` word goes out.
 */
static BOOL fb_read_palette(struct ColorMap *cm, UWORD depth, UBYTE *pal)
{
    /* Static, not automatic: 768 bytes at depth 8, and a Shell command has
       4 KB of stack for everything httpd already has on it. */
    static UBYTE fresh[3U * FB_MAX_COLOURS];
    ULONG colours = 1UL << depth;
    ULONG have    = (cm != NULL) ? (ULONG)cm->Count : 0;
    ULONG first;
    ULONG i;
    BOOL  moved = FALSE;

    memset(fresh, 0, (size_t)(3UL * colours));

    if (have > colours)
        have = colours;

    for (first = 0; first < have; first += 16)
    {
        ULONG table[3 * 16];
        ULONG n = have - first;

        if (n > 16)
            n = 16;

        GetRGB32(cm, first, n, table);

        for (i = 0; i < n; i++)
        {
            fresh[(first + i) * 3 + 0] = (UBYTE)(table[i * 3 + 0] >> 24);
            fresh[(first + i) * 3 + 1] = (UBYTE)(table[i * 3 + 1] >> 24);
            fresh[(first + i) * 3 + 2] = (UBYTE)(table[i * 3 + 2] >> 24);
        }
    }

    for (i = 0; i < 3UL * colours; i++)
    {
        if (pal[i] != fresh[i])
        {
            moved = TRUE;
            pal[i] = fresh[i];
        }
    }

    return moved;
}

/* ---------------------------------------------------------- the pointer -- */

/*
 * THE MOUSE POINTER, WHICH IS A SPRITE AND IS THEREFORE IN NO FRAME
 *
 *   Every frame this sends is the screen's bitplanes.  The pointer is drawn by
 *   the hardware from a sprite that is not in them, so a viewer that showed
 *   only what arrives has no pointer at all -- which is why the browser drew an
 *   arrow of its own, at the local mouse, and why that arrow was never the
 *   Amiga's.
 *
 *   The image is sent as its own word, once, and again only if it changes.
 *   The POSITION is not sent at all: the viewer puts the Amiga's pointer where
 *   the browser's is, so the browser already knows where it is and drawing it
 *   locally is what keeps it moving without a round trip.
 *
 * WHERE THE IMAGE COMES FROM, AND WHY NOT FROM THE POINTER ON SCREEN
 *
 *   There is no way to read the pointer that is actually being displayed.
 *   struct Window's Pointer, PtrWidth and PtrHeight are filled by SetPointer()
 *   and are NOT touched by SetWindowPointer(): a window that used the newer
 *   call leaves them stale from an earlier SetPointer() or zero, and 3.1's
 *   Workbench, the busy pointer and every pointer wider than 16 pixels all use
 *   the newer call.  Reading them as sprite data reads memory somebody else
 *   freed.  The pointerclass object that does hold the imagery answers no
 *   OM_GET, so there is nothing to ask it either.
 *
 *   So this reads what the pointer was CONFIGURED to be, which is a file:
 *   ENV:Sys/pointer.prefs, an IFF PREF with a PNTR chunk, written by the
 *   Pointer editor and read by IPrefs.  A machine that has never had one saved
 *   has no such file, and the pointer there came from devs:system-configuration
 *   instead -- GetPrefs() hands that over as PointerMatrix, a 16x16 two-plane
 *   sprite with its hotspot and its three colours.  Both are read; the file
 *   wins when it is there, because it is the newer answer.
 *
 * WHAT THIS CANNOT SHOW, AND IT MATTERS
 *
 *   THE BUSY POINTER, and any pointer an application set for itself.  Both
 *   live in pointerclass objects that cannot be read back, so the browser goes
 *   on showing the configured arrow while the machine shows a stopwatch.  That
 *   disagreement lands during a long operation, which is exactly when somebody
 *   is most likely to be watching the screen and waiting.  The file HAS a
 *   second PNTR chunk carrying the busy pointer's image -- pp_Which is
 *   WBP_BUSY -- and it is deliberately not read: having the picture is no use
 *   without knowing WHEN it is up, and nothing here can know that.
 */

/* An IFF PREF file for a pointer is a few hundred bytes; this is room for a
   64x64 four-plane pointer twice over and then some.  Static, for the reason
   every buffer in this file is: a Shell command has 4 KB of stack. */
#define FB_PREFS_MAX        8192U

/* struct PointerPrefs, up to its colour table, as it is on disk. */
#define PP_WHICH            16
#define PP_SIZE             18
#define PP_WIDTH            20
#define PP_HEIGHT           22
#define PP_DEPTH            24
#define PP_YSIZE            26
#define PP_X                28
#define PP_Y                30
#define PP_HEAD             32

typedef struct FbPointer
{
    UWORD width;
    UWORD height;
    UWORD depth;
    UWORD row_bytes;
    UWORD x_scale;
    UWORD y_scale;
    WORD  hot_x;
    WORD  hot_y;
    ULONG bits_len;
    UBYTE rgb[3U * RFB_PTR_MAX_COLOURS];
    UBYTE bits[RFB_PTR_MAX_BITS];
} FbPointer;

static FbPointer fb_ptr;        /* what the viewer was last told  */
static UBYTE     fb_ptr_have;   /* fb_ptr holds something         */
static UBYTE     fb_want_ptr;   /* a `ptr` word is queued         */
static ULONG     fb_ptr_next;   /* when to look at the prefs again */

/*
 * HOW OFTEN THE PREFERENCES ARE RE-READ.  Fiftieths.
 *
 * IPrefs rewrites the file when somebody changes the setting, so noticing
 * means looking at the file again -- and looking means a Lock, an Examine and
 * an UnLock on ENV:, which is RAM: and therefore fast but is still three DOS
 * calls on the one task that also serves every connection.  Once a second is
 * far more often than a person changes their pointer and is a rounding error
 * against the twenty grabs a second beside it.
 */
#define FB_PTR_EVERY        50UL

/* Big-endian, out of a byte buffer, because that is how the file is and this
   must not assume the reader's alignment. */
static UWORD fb_be16(const UBYTE *p)
{
    return (UWORD)(((UWORD)p[0] << 8) | p[1]);
}

static ULONG fb_be32(const UBYTE *p)
{
    return ((ULONG)p[0] << 24) | ((ULONG)p[1] << 16) |
           ((ULONG)p[2] << 8) | p[3];
}

/*
 * How many screen pixels one sprite pixel covers.
 *
 * A sprite pixel is NOT always a lores pixel, which is the assumption that
 * would put the pointer at half width on a superhires screen.  V39's ColorMap
 * carries the answer: SpriteResolution, or SpriteResDefault when it is
 * SPRITERESN_DEFAULT.  Both are in the same units as the screen's own pixel --
 * 140ns, 70ns, 35ns -- so the scale is one divided by the other.
 *
 * SPRITERESN_ECS is 140ns except on a 35ns screen where it is 70ns, which is
 * what makes superhires come out at two and not four.
 */
static VOID fb_pointer_scale(struct Screen *sc, UWORD *xs, UWORD *ys)
{
    struct ColorMap *cm    = sc->ViewPort.ColorMap;
    UWORD            modes = (UWORD)sc->ViewPort.Modes;
    UWORD            screen_ns;
    UWORD            sprite_ns;
    UBYTE            resn = (UBYTE)SPRITERESN_ECS;

    if ((modes & SUPERHIRES) != 0)
        screen_ns = 35;
    else if ((modes & HIRES) != 0)
        screen_ns = 70;
    else
        screen_ns = 140;

    /* Only a V39 ColorMap has the fields; an older one is an ECS sprite. */
    if (cm != NULL && cm->Type >= (UBYTE)COLORMAP_TYPE_V39)
    {
        resn = cm->SpriteResolution;
        if (resn == (UBYTE)SPRITERESN_DEFAULT)
            resn = cm->SpriteResDefault;
    }

    switch (resn)
    {
    case SPRITERESN_140NS: sprite_ns = 140; break;
    case SPRITERESN_70NS:  sprite_ns = 70;  break;
    case SPRITERESN_35NS:  sprite_ns = 35;  break;
    case SPRITERESN_ECS:
    default:
        sprite_ns = (screen_ns == 35) ? 70 : 140;
        break;
    }

    *xs = (UWORD)((sprite_ns >= screen_ns) ? (sprite_ns / screen_ns) : 1);

    /*
     * Down the screen the sprite is not scaled by a resolution field: a sprite
     * line is a display line, so on an interlaced screen -- two display fields
     * to one picture -- one sprite row covers two screen rows.
     */
    *ys = (UWORD)(((modes & LACE) != 0) ? 2 : 1);
}

/*
 * The sprite's own colours, which are NOT the screen's first four.
 *
 * The pointer is sprite 0 and its three pens are colour registers 17, 18 and
 * 19, with 16 transparent.  Those registers are the live truth -- a program
 * that recoloured the pointer moved them and moved nothing in any prefs file
 * -- so they are preferred, and the configured colours are the fallback for a
 * ColorMap too short to hold them.
 *
 * Only at depth 2.  A deeper sprite is an attached pair and which registers it
 * lands on is not something to guess at, so those keep the colours the file
 * supplied.
 */
static VOID fb_pointer_colours(struct Screen *sc, FbPointer *p)
{
    struct ColorMap *cm = sc->ViewPort.ColorMap;
    ULONG            table[3 * 3];
    ULONG            i;

    if (p->depth != 2 || cm == NULL || (ULONG)cm->Count < 20UL)
        return;

    GetRGB32(cm, 17, 3, table);

    for (i = 0; i < 3UL; i++)
    {
        p->rgb[i * 3 + 0] = (UBYTE)(table[i * 3 + 0] >> 24);
        p->rgb[i * 3 + 1] = (UBYTE)(table[i * 3 + 1] >> 24);
        p->rgb[i * 3 + 2] = (UBYTE)(table[i * 3 + 2] >> 24);
    }
}

/*
 * devs:system-configuration's pointer, through GetPrefs().  The stock answer:
 * a 16x16 two-plane sprite in PointerMatrix, its hotspot in XOffset/YOffset
 * and its colours in color17..19 as RGB4.
 *
 * PointerMatrix is (1 + 16 + 1) word PAIRS: a control pair, sixteen rows of
 * one word per plane, and a terminating pair.  The rows are INTERLEAVED and
 * everything downstream of here is plane-major, so this is where they are
 * separated.
 */
static BOOL fb_pointer_from_prefs(FbPointer *p)
{
    /* Static: struct Preferences is over 300 bytes and this runs on httpd's
       stack, which is a Shell command's. */
    static struct Preferences prefs;
    UWORD row;

    if (GetPrefs(&prefs, (LONG)sizeof(prefs)) == NULL)
        return FALSE;

    p->width     = 16;
    p->height    = 16;
    p->depth     = 2;
    p->row_bytes = 2;
    p->bits_len  = 2UL * 2UL * 16UL;
    p->hot_x     = (WORD)prefs.XOffset;
    p->hot_y     = (WORD)prefs.YOffset;

    memset(p->bits, 0, (size_t)p->bits_len);

    for (row = 0; row < 16; row++)
    {
        UWORD p0 = prefs.PointerMatrix[2 + row * 2 + 0];
        UWORD p1 = prefs.PointerMatrix[2 + row * 2 + 1];

        p->bits[row * 2 + 0]      = (UBYTE)(p0 >> 8);
        p->bits[row * 2 + 1]      = (UBYTE)p0;
        p->bits[32 + row * 2 + 0] = (UBYTE)(p1 >> 8);
        p->bits[32 + row * 2 + 1] = (UBYTE)p1;
    }

    /* RGB4 to RGB8, by repeating the nibble, so 0xF becomes 0xFF and not
       0xF0 -- the second is a colour a quarter of a step dark on everything
       and it shows on a white pointer. */
    {
        const UWORD c[3] = { prefs.color17, prefs.color18, prefs.color19 };
        ULONG i;

        for (i = 0; i < 3UL; i++)
        {
            UBYTE r = (UBYTE)((c[i] >> 8) & 0x0F);
            UBYTE g = (UBYTE)((c[i] >> 4) & 0x0F);
            UBYTE b = (UBYTE)(c[i] & 0x0F);

            p->rgb[i * 3 + 0] = (UBYTE)(r * 17U);
            p->rgb[i * 3 + 1] = (UBYTE)(g * 17U);
            p->rgb[i * 3 + 2] = (UBYTE)(b * 17U);
        }
    }

    return TRUE;
}

/*
 * ENV:Sys/pointer.prefs, if it is there.  An IFF FORM PREF holding one PNTR
 * chunk per pointer; the one wanted is pp_Which == WBP_NORMAL, and the BUSY
 * one is stepped over for the reason in the block at the top of this section.
 *
 * The whole file is read and then walked, rather than seeking around it: it is
 * a few hundred bytes on RAM: and one Read is one DOS call instead of a dozen.
 */
static BOOL fb_pointer_from_env(FbPointer *p)
{
    static UBYTE buf[FB_PREFS_MAX];
    BPTR  fh;
    LONG  got;
    ULONG at;
    BOOL  found = FALSE;

    fh = Open((CONST_STRPTR)"ENV:Sys/pointer.prefs", MODE_OLDFILE);
    if (fh == 0)
        return FALSE;

    got = Read(fh, buf, (LONG)sizeof(buf));
    Close(fh);

    if (got < 12 || memcmp(buf, "FORM", 4) != 0 || memcmp(buf + 8, "PREF", 4) != 0)
        return FALSE;

    at = 12;
    while (at + 8UL <= (ULONG)got && !found)
    {
        ULONG len  = fb_be32(buf + at + 4);
        ULONG body = at + 8UL;

        if (len > (ULONG)got || body + len > (ULONG)got)
            break;                          /* truncated; take nothing */

        if (memcmp(buf + at, "PNTR", 4) == 0 && len >= (ULONG)PP_HEAD)
        {
            const UBYTE *c = buf + body;

            if (fb_be16(c + PP_WHICH) == (UWORD)WBP_NORMAL)
            {
                UWORD w = fb_be16(c + PP_WIDTH);
                UWORD h = fb_be16(c + PP_HEIGHT);
                UWORD d = fb_be16(c + PP_DEPTH);
                ULONG cols;
                ULONG rb;
                ULONG need;

                if (w == 0U || w > RFB_PTR_MAX_W ||
                    h == 0U || h > RFB_PTR_MAX_H ||
                    d == 0U || d > RFB_PTR_MAX_DEPTH)
                {
                    /* A shape past what the wire carries.  Refused here and
                       not truncated: half a sprite at the wrong scale is a
                       worse answer than the viewer keeping its own arrow. */
                    return FALSE;
                }

                cols = (1UL << d) - 1UL;
                rb   = ((w + 15UL) / 16UL) * 2UL;
                need = (ULONG)PP_HEAD + cols * 3UL + (ULONG)d * rb * h;

                if (len < need)
                    return FALSE;           /* the chunk is shorter than it says */

                p->width     = w;
                p->height    = h;
                p->depth     = d;
                p->row_bytes = (UWORD)rb;
                p->bits_len  = (ULONG)d * rb * h;
                p->hot_x     = (WORD)fb_be16(c + PP_X);
                p->hot_y     = (WORD)fb_be16(c + PP_Y);

                memcpy(p->rgb, c + PP_HEAD, (size_t)(cols * 3UL));
                memcpy(p->bits, c + PP_HEAD + cols * 3UL, (size_t)p->bits_len);
                found = TRUE;
            }
        }

        /* IFF chunks are word aligned. */
        at = body + len + (len & 1UL);
    }

    return found;
}

/*
 * THE HOTSPOT, KEPT INSIDE THE SPRITE.
 *
 * devs:system-configuration's stock arrow carries XOffset -1 with its tip in
 * column 0, and the two readings of that field -- the hotspot is at
 * (XOffset, YOffset) in the sprite, or the sprite is drawn at the mouse PLUS
 * (XOffset, YOffset) -- put the tip one lores pixel either side of the mouse.
 * Neither can be settled from here: the sprite is not in any bitmap this can
 * see, so there is nothing to measure the drawn position against.
 *
 * What IS certain is that a hotspot outside the image is not a hotspot for
 * anything this draws, so it is clamped in.  For the stock arrow that lands it
 * on (0,0), which is the tip, which is where an arrow points from.
 *
 * It costs nothing if it is wrong: a click goes to the coordinate the viewer
 * SENDS and not to where the image was drawn, so the worst this can be is the
 * picture of the pointer sitting one lores pixel off.
 */
static VOID fb_pointer_hotspot(FbPointer *p)
{
    if (p->hot_x < 0)                       p->hot_x = 0;
    if (p->hot_x > (WORD)(p->width - 1))    p->hot_x = (WORD)(p->width - 1);
    if (p->hot_y < 0)                       p->hot_y = 0;
    if (p->hot_y > (WORD)(p->height - 1))   p->hot_y = (WORD)(p->height - 1);
}

/*
 * The current pointer, whatever it takes.  TRUE when `p` holds one.
 *
 * The file first, because it is the newer answer and the one the Pointer
 * editor writes; devs:system-configuration when there is no file, which is a
 * machine nobody has changed the pointer on.
 *
 * NO SCREEN IS TOUCHED HERE and nothing is locked, because this opens a file:
 * a DOS call is not something to make with Intuition stopped.  The scale and
 * the live sprite colours are added afterwards, in fb_pointer_poll(), which is
 * where a screen is resolved.
 */
static BOOL fb_pointer_read(FbPointer *p)
{
    memset(p, 0, sizeof(*p));

    if (!fb_pointer_from_env(p) && !fb_pointer_from_prefs(p))
        return FALSE;

    fb_pointer_hotspot(p);
    return TRUE;
}

/* Two pointers are the same picture.  Compared over the fields and the bytes
   rather than with one memcmp of the struct, because the tail of bits[] past
   bits_len is not written and would compare as noise. */
static BOOL fb_pointer_same(const FbPointer *a, const FbPointer *b)
{
    if (a->width != b->width || a->height != b->height ||
        a->depth != b->depth || a->x_scale != b->x_scale ||
        a->y_scale != b->y_scale || a->hot_x != b->hot_x ||
        a->hot_y != b->hot_y || a->bits_len != b->bits_len)
        return FALSE;

    if (memcmp(a->rgb, b->rgb, (size_t)(3UL * ((1UL << a->depth) - 1UL))) != 0)
        return FALSE;

    return (BOOL)(memcmp(a->bits, b->bits, (size_t)a->bits_len) == 0);
}

/*
 * Look again, and queue a word if the picture moved.
 *
 * Once a second, not once a pass: this opens a file, and three DOS calls
 * twenty times a second on the task that also answers every connection is a
 * cost with nothing to show for it.  A person changes their pointer about as
 * often as they change their wallpaper.
 *
 * The file is read with nothing held; the screen is resolved afterwards and
 * only the scale and the sprite's live colours come out of it, which is a
 * short lock rather than one held across DOS.
 */
static VOID fb_pointer_poll(ULONG now)
{
    /* Static, and 2 KB of it: this is the second of these buffers and it is
       not going on a 4 KB stack. */
    static FbPointer fresh;
    struct Screen   *sc;
    ULONG            ilock = 0;
    BOOL             pub;

    if (fb_ptr_next != 0UL && (LONG)(now - fb_ptr_next) < 0L)
        return;

    fb_ptr_next = now + FB_PTR_EVERY;
    if (fb_ptr_next == 0UL)
        fb_ptr_next = 1UL;              /* 0 means "never looked" */

    if (!fb_pointer_read(&fresh))
        return;

    sc = fb_lock_front(&pub);
    if (sc == NULL)
        return;

    if (!pub)
        ilock = LockIBase(0);

    if (pub || fb_listed(sc))
    {
        fb_pointer_scale(sc, &fresh.x_scale, &fresh.y_scale);
        fb_pointer_colours(sc, &fresh);
    }
    else
    {
        fresh.x_scale = 0;              /* refused below */
    }

    if (!pub)
        UnlockIBase(ilock);
    else
        UnlockPubScreen(NULL, sc);

    if (fresh.x_scale == 0)
        return;

    if (fb_ptr_have && fb_pointer_same(&fb_ptr, &fresh))
        return;

    fb_ptr      = fresh;
    fb_ptr_have = 1;
    fb_want_ptr = 1;
}

/* ------------------------------------------------------------------- grab -- */

/*
 * THE SCREEN IS NOT COPIED ANY MORE.  The encoder reads the bitplanes where
 * they are.
 *
 * There used to be a frame_bytes buffer here and a CopyMem per 80-byte row
 * into it, and then the encoder read that copy: 40 KB of chip RAM read, 40 KB
 * written, 40 KB read again, to answer a question -- did anything change --
 * that on a live Workbench is "no" 49 frames out of 50.  Measured on an A1200,
 * two planes: the copy 14.9 ms and the encode 56.9 ms, against 13.6 ms for one
 * pass that compares the planes against the shadow and stops.
 *
 * WHAT THE ENCODER PROMISES IN RETURN, because this reads the planes without
 * holding a drawing lock: a tile it decides has changed is read ONCE, into a
 * buffer, and that one copy is both what updates the shadow and what goes on
 * the wire.  So a screen being drawn on underneath can produce a torn frame --
 * it always could -- but it cannot produce a shadow here that disagrees with
 * the bytes the far end was sent, which is the failure that would not correct
 * itself on the next frame.
 */
static LONG fb_encode_planes(const UBYTE **planes, UBYTE *out, ULONG out_cap)
{
    return rfb_encode_frame_planes(&fb_enc, planes, out, out_cap);
}

/*
 * The view's units per screen pixel, doubled so superhires can be a half, and
 * where the screen starts in the view.  Straight off the ViewPort: SUPERHIRES
 * is two pixels to a view unit, HIRES one to one, and anything else is lores
 * at two view units to a pixel; a screen that is not interlaced is two view
 * lines to a row.
 */
static VOID fb_view_units(struct Screen *sc)
{
    UWORD modes = (UWORD)sc->ViewPort.Modes;

    if ((modes & SUPERHIRES) != 0)
        fb_x_halves = 1;
    else if ((modes & HIRES) != 0)
        fb_x_halves = 2;
    else
        fb_x_halves = 4;

    fb_y_halves = (UWORD)(((modes & LACE) != 0) ? 2 : 4);

    fb_left = sc->LeftEdge;
    fb_top  = sc->TopEdge;
}

enum
{
    FB_GRAB_OK = 0,
    FB_GRAB_GONE,       /* there are no screens at all any more              */
    FB_GRAB_CHANGED,    /* it is not the screen the client was told about   */
    FB_GRAB_REFUSED,    /* it is a bitmap this cannot read; fb_why says why */
    FB_GRAB_VANISHED    /* it closed while we were resolving it             */
};

/*
 * Everything that dereferences the Screen, its BitMap or its ColorMap.  The
 * caller holds either a public screen lock or LockIBase(), so this must not
 * block and must not call Intuition -- and it does not: GetBitMapAttr() and
 * GetRGB32() are reads of structures the screen already owns.
 *
 * What leaves here is the geometry, the palette, the view units and the
 * bitplane ADDRESSES.  The encode that follows touches nothing but those
 * addresses, which is what lets it run with the lock given back.
 */
static int fb_examine(struct Screen *sc, const FbGeometry *want,
                      FbGeometry *now, const UBYTE **planes,
                      BOOL *palette_moved)
{
    UWORD plane;

    if (!fb_geometry_of(sc->RastPort.BitMap, now))
        return FB_GRAB_REFUSED;

    if (!fb_geometry_same(want, now))
        return FB_GRAB_CHANGED;

    fb_view_units(sc);

    *palette_moved = fb_read_palette(sc->ViewPort.ColorMap,
                                     want->depth, fb_pal);

    /* Colours before pixels, and the encode is skipped entirely on the pass
       that finds them moved. */
    if (*palette_moved)
        return FB_GRAB_OK;

    for (plane = 0; plane < want->depth; plane++)
        planes[plane] = (const UBYTE *)sc->RastPort.BitMap->Planes[plane];

    return FB_GRAB_OK;
}

/*
 * One frame into `buf`, and the palette.  Everything downstream -- the encode,
 * the socket -- runs with nothing held.
 *
 * THE LAYER LOCK IS ATTEMPTED, NEVER WAITED FOR, AND NOT REQUIRED
 *
 *   LockLayers() was here and it wedged the whole server.  The lock it takes
 *   is sc->LayerInfo.Lock, and it is held for as long as a mouse button is
 *   down: Intuition holds it while a menu is up or a window is being dragged
 *   or sized, and the Workbench task holds it through an icon drag or a
 *   rubber-band selection.  Those last as long as a person holds a button,
 *   and httpd serves every connection from one task, so for that whole time
 *   nothing was answered -- not the console, not plain HTTP, not to anybody.
 *   A guest was found forty minutes into exactly that: ss_Owner the Workbench
 *   task, and the one SemaphoreRequest queued on it httpd's own stack.
 *
 *   Waiting is out, and so is giving up.  A grab that returned empty-handed
 *   whenever the lock was busy would freeze the picture for the whole of every
 *   drag -- the console would stop moving at the exact moment there is
 *   something to watch.  So the lock is ATTEMPTED, and the frame is read
 *   either way.
 *
 *   Reading it unlocked is safe and is what a mirror wants.  The planes are
 *   memory; the lock serialises the tasks DRAWING into them, not the reading
 *   of them, and the screen lock above is what keeps the screen and its bitmap
 *   in existence.  The cost is that a frame read while somebody is drawing may
 *   carry half of a change -- and the encoder diffs the next grab against what
 *   it actually sent, so the very next frame that differs puts it right.  A
 *   torn frame for two milliseconds is the price of a console that keeps up
 *   during a drag and a server that never stops.
 *
 * AND WHAT KEEPS A SCREEN NOTHING CAN LOCK IN EXISTENCE
 *
 *   A public screen is held for the whole of this and cannot close.  The
 *   Palette and Overscan editors' screens are not public and 3.1 has no handle
 *   for one, so the guarantee is shorter: everything read out of the Screen is
 *   read under LockIBase() with the screen verified still listed, which is
 *   enough because CloseScreen() has to take IntuitionBase to unlink it before
 *   it frees anything.
 *
 *   What is left outside that is the encode, which reads the bitplane
 *   addresses and nothing else.  A screen that closes in that window leaves
 *   those pointing at freed chip RAM: bytes, on a machine with no MMU, so the
 *   worst it can produce is ONE wrong frame, and the pass after it resolves the
 *   front screen again and the geom barrier corrects the viewer.  Making that
 *   window zero would mean holding LockIBase() across a 15 ms encode ten times
 *   a second, which is Intuition stopped for a sixth of the time.
 *
 *   AND SUCH A SCREEN IS READ WITHOUT THE LAYER LOCK, counted in tn=.  Taking
 *   it would mean releasing it after the encode, and the only way to know the
 *   screen is still there to release it on is LockIBase() -- while holding a
 *   layer lock.  That is a deadlock and it was measured, not reasoned about:
 *   Intuition holds IntuitionBase and wants the layer lock, this holds the
 *   layer lock and wants IntuitionBase.  The guest stopped dead, the whole GUI
 *   frozen with a gadget stuck in its selected state and httpd answering
 *   nothing, on the click that was closing an Overscan editor screen.
 */
static int fb_grab_frame(const FbGeometry *want, FbGeometry *now,
                         BOOL *palette_moved, UBYTE *out, ULONG out_cap,
                         LONG *encoded)
{
    struct Screen *sc;
    const UBYTE   *planes[FB_MAX_DEPTH];
    ULONG          ilock = 0;
    int            rc;
    BOOL           pub;
    BOOL           locked = FALSE;

    *palette_moved = FALSE;
    *encoded = -1L;                     /* nothing encoded this pass */

    sc = fb_lock_front(&pub);
    if (sc == NULL)
    {
        fb_say("there are no screens left: Intuition's screen list is empty");
        return FB_GRAB_GONE;
    }

    if (!pub)
        ilock = LockIBase(0);

    if (pub || fb_listed(sc))
    {
        rc = fb_examine(sc, want, now, planes, palette_moved);

        /* Only on a screen that is held, and see above for why.  Attempted and
           never waited for: the lock is held for as long as a mouse button is
           down and this server has one task. */
        if (pub && rc == FB_GRAB_OK && !*palette_moved)
            locked = (BOOL)(AttemptSemaphore(&sc->LayerInfo.Lock) != 0);
    }
    else
    {
        /* It closed between being read out of FirstScreen and being looked
           at.  Nothing this pass; the next one resolves the front screen
           again, which by then is whatever is really in front. */
        rc = FB_GRAB_VANISHED;
    }

    if (!pub)
        UnlockIBase(ilock);

    if (rc == FB_GRAB_OK && !*palette_moved)
    {
        if (!locked)
            fb_torn++;

        WaitBlit();
        *encoded = fb_encode_planes(planes, out, out_cap);

        if (locked)
            ReleaseSemaphore(&sc->LayerInfo.Lock);
    }

    if (pub)
        UnlockPubScreen(NULL, sc);

    return rc;
}

/* ------------------------------------------------------------- the buffers -- */

static VOID fb_free_buffers(VOID)
{
    ami_free(fb_tx);
    ami_free(fb_scratch);
    ami_free(fb_shadow);

    fb_tx = NULL;
    fb_scratch = NULL;
    fb_shadow = NULL;
    fb_tx_cap = 0;
    fb_tx_len = 0;
    fb_tx_sent = 0;
    fb_shadow_len = 0;
    fb_scratch_len = 0;
}

/*
 * Everything the session holds, sized from one geometry.  Called again when
 * the screen changes shape under a live session, which is why it frees first:
 * a 640x256x4 screen becoming 640x480x8 is a different four buffers and
 * keeping the old ones would be reading a frame into two thirds of one.
 */
static BOOL fb_take_buffers(const FbGeometry *g)
{
    ULONG worst;

    fb_free_buffers();

    fb_rg.width         = g->width;
    fb_rg.height        = g->height;
    fb_rg.bytes_per_row = g->row_bytes;
    fb_rg.depth         = (rfb_u8)g->depth;
    fb_rg.tile_w        = HTTP_FB_TILE_W;
    fb_rg.tile_h        = HTTP_FB_TILE_H;

    rfb_scroll_defaults(&fb_cfg);

    /* Straight off what the BitMap said, not inferred from the strides.  The
       encoder walks the real bitplanes now, so this flag has to be the
       screen's own answer -- GetBitMapAttr(BMA_FLAGS) & BMF_INTERLEAVED, in
       fb_geometry_of() and nowhere else -- and not something derived from a
       relationship that happens to hold. */
    fb_flags = (rfb_u32)FB_FLAGS;
    if (g->interleaved)
        fb_flags |= RFB_F_INTERLEAVED;

    fb_shadow_len  = rfb_shadow_size(&fb_rg);
    fb_scratch_len = rfb_scratch_size(&fb_rg, fb_flags, &fb_cfg);
    worst          = rfb_worst_case_frame(&fb_rg);

    if (fb_shadow_len == 0UL || fb_scratch_len == 0UL || worst == 0UL)
    {
        fb_say("the encoder will not take this screen's geometry");
        return FALSE;
    }

    /* Ten bytes of room ahead of the payload so a WebSocket header can be
       written BACKWARDS into it: the frame is then contiguous and the send
       cursor simply starts wherever the header turned out to begin. */
    fb_tx_cap = worst + 10UL;

    fb_shadow  = (UBYTE *)ami_alloc(fb_shadow_len);
    fb_scratch = (UBYTE *)ami_alloc(fb_scratch_len);
    fb_tx      = (UBYTE *)ami_alloc(fb_tx_cap);

    if (fb_shadow == NULL || fb_scratch == NULL || fb_tx == NULL)
    {
        fb_say3("not enough memory for a ", fb_shadow_len / 1024UL,
                " KB screen and the buffers around it");
        fb_free_buffers();
        return FALSE;
    }

    /* ami_alloc() clears, so the shadow starts as the all-zero screen the
       receiver starts from and the first frame codes as a delta from it. */
    if (rfb_encoder_init(&fb_enc, &fb_rg, fb_flags, &fb_cfg,
                         fb_shadow, fb_shadow_len,
                         fb_scratch, fb_scratch_len) != 0L)
    {
        fb_say("the encoder refused this screen's geometry");
        fb_free_buffers();
        return FALSE;
    }

    fb_geom = *g;

    /*
     * The shape is queued; the colours are NOT.  Zeroing the remembered
     * palette is what makes the next grab's comparison report a change, and
     * that is where the `pal` word comes from -- queueing one here would send
     * 3 << depth zeroes, which a viewer draws as a black screen until the real
     * one arrives a frame later.  http_fb_start() reads the ColorMap itself,
     * because it is the one caller that still holds the screen.
     */
    memset(fb_pal, 0, sizeof(fb_pal));
    fb_want_geom = 1;
    fb_want_pal  = 0;

    return TRUE;
}

/*
 * Forget the shadow.  What `refresh` asks for: the receiver has lost a frame
 * and every XOR after it would be applied to bytes that are not what the
 * encoder thought were there, so the next frame has to be a full one.
 *
 * The sequence number is NOT reset.  It is what the receiver checks for gaps
 * with, and taking it back to zero would look like exactly the fault this is
 * recovering from.
 */
static VOID fb_forget_shadow(VOID)
{
    rfb_u16 seq;

    if (fb_shadow == NULL)
        return;

    seq = fb_enc.seq;
    memset(fb_shadow, 0, (size_t)fb_shadow_len);

    (VOID)rfb_encoder_init(&fb_enc, &fb_rg, fb_flags, &fb_cfg,
                           fb_shadow, fb_shadow_len,
                           fb_scratch, fb_scratch_len);
    fb_enc.seq = seq;
}

/* ------------------------------------------------------------ the framing -- */

/*
 * Queue one control frame.  There is room for exactly one, so nothing may
 * overtake a close: a ping arriving after the session has decided to end --
 * which a browser sends on a timer and is therefore ordinary -- would answer
 * it with a pong written over the close frame, and http_fb_write() would then
 * see fb_closing and shut the socket with the reason still in the buffer.
 * The viewer would be told the connection dropped instead of why.
 */
static VOID fb_control(HttpWsEvent ev, const UBYTE *payload, ULONG len)
{
    unsigned long head;
    ULONG         i;

    if (fb_closing)
        return;

    if (len > (unsigned long)HTTP_WS_CTL_MAX)
        len = (unsigned long)HTTP_WS_CTL_MAX;

    head = http_ws_head(fb_ctl, sizeof(fb_ctl), ev, len, 1);
    if (head == 0UL)
        return;

    for (i = 0; i < len; i++)
        fb_ctl[head + i] = payload[i];

    fb_ctl_n  = (UWORD)(head + len);
    fb_ctl_at = 0;
}

static VOID fb_close_session(UWORD code)
{
    if (fb_closing)
        return;

    fb_ctl_n      = (UWORD)http_ws_close_frame(fb_ctl, sizeof(fb_ctl), code,
                                               http_ws_close_reason(code));
    fb_ctl_at     = 0;
    fb_closing    = 1;
    fb_close_code = code;
}

/* A close carrying our own sentence rather than the codec's, for the refusals
   a person has to be able to read: a screen that went away, a mode change this
   could not follow. */
static VOID fb_close_saying(UWORD code, const char *reason)
{
    if (fb_closing)
        return;

    fb_ctl_n      = (UWORD)http_ws_close_frame(fb_ctl, sizeof(fb_ctl), code,
                                               reason);
    fb_ctl_at     = 0;
    fb_closing    = 1;
    fb_close_code = code;
}

/* The header, backwards from offset 10, over a payload already at fb_tx+10. */
static VOID fb_frame_payload(HttpWsEvent ev, ULONG len)
{
    UBYTE         head[10];
    unsigned long hn;
    unsigned long i;

    hn = http_ws_head(head, sizeof(head), ev, (unsigned long)len, 1);

    for (i = 0; i < hn; i++)
        fb_tx[10UL - hn + i] = head[i];

    fb_tx_sent = 10UL - hn;
    fb_tx_len  = 10UL + len;
}

/* ------------------------------------------------------------------ input -- */

static BOOL fb_input_open(VOID)
{
    fb_in_port = CreateMsgPort();
    if (fb_in_port == NULL)
    {
        fb_say("no message port for input.device");
        return FALSE;
    }

    fb_in_req = (struct IOStdReq *)
        CreateIORequest(fb_in_port, (ULONG)sizeof(struct IOStdReq));
    if (fb_in_req == NULL)
    {
        fb_say("no IORequest for input.device");
        return FALSE;
    }

    if (OpenDevice((CONST_STRPTR)"input.device", 0,
                   (struct IORequest *)fb_in_req, 0) != 0)
    {
        fb_say("input.device would not open, so the console can show the "
               "screen but not be typed at");
        return FALSE;
    }

    fb_in_open = TRUE;
    return TRUE;
}

static VOID fb_input_close(VOID)
{
    if (fb_in_open)
    {
        CloseDevice((struct IORequest *)fb_in_req);
        fb_in_open = FALSE;
    }
    if (fb_in_req != NULL)
    {
        DeleteIORequest((struct IORequest *)fb_in_req);
        fb_in_req = NULL;
    }
    if (fb_in_port != NULL)
    {
        DeleteMsgPort(fb_in_port);
        fb_in_port = NULL;
    }
}

/*
 * One event into the input stream.
 *
 * Synchronous.  IND_WRITEEVENT hands the event to the input task and returns;
 * it is not a transfer and there is nothing to wait for the far end of, so the
 * asynchronous form would only add a second trip through the loop for a call
 * that has already finished.  Nothing is locked here -- input is read in
 * http_fb_read(), and the screen is locked only inside the grab -- so this
 * cannot be the thing a lock is held across.
 */
static VOID fb_write_event(VOID)
{
    if (!fb_in_open)
        return;

    fb_event.ie_NextEvent = NULL;
    /* Double-click and key repeat are both decided from this, so an event with
       no time on it is an event Intuition cannot group with the one before. */
    CurrentTime((ULONG *)&fb_event.ie_TimeStamp.tv_secs,
                (ULONG *)&fb_event.ie_TimeStamp.tv_micro);

    fb_in_req->io_Command = IND_WRITEEVENT;
    fb_in_req->io_Flags   = 0;
    fb_in_req->io_Length  = (LONG)sizeof(struct InputEvent);
    fb_in_req->io_Data    = (APTR)&fb_event;

    (VOID)DoIO((struct IORequest *)fb_in_req);
}

/* The pointer, absolutely.  IECLASS_POINTERPOS is the one class that takes a
   screen coordinate rather than a delta, which is what a viewer has: a browser
   knows where in the canvas the mouse is and has no idea how far it moved on
   the far side of a coalesced frame. */
static VOID fb_inject_pointer(rfb_s32 x, rfb_s32 y)
{
    memset(&fb_event, 0, sizeof(fb_event));
    fb_event.ie_Class     = IECLASS_POINTERPOS;
    fb_event.ie_Code      = IECODE_NOBUTTON;
    fb_event.ie_Qualifier = fb_buttons;
    /* Screen pixels in, view units out; see fb_x_halves.  The screen's own
       origin goes in first, which is zero unless somebody dragged it. */
    fb_event.ie_X         = (WORD)(((x + (rfb_s32)fb_left) *
                                    (rfb_s32)fb_x_halves) / 2);
    fb_event.ie_Y         = (WORD)(((y + (rfb_s32)fb_top) *
                                    (rfb_s32)fb_y_halves) / 2);
    fb_write_event();
}

/*
 * The buttons, as the difference between what is held now and what was held
 * before.  A viewer sends the whole mask on every move, so a press and a
 * release are both "this bit changed" rather than events of their own -- and a
 * mask that arrives with two bits changed at once produces two events, because
 * IECODE carries one button.
 */
static VOID fb_inject_buttons(rfb_s32 mask)
{
    static const struct {
        rfb_u32 bit;        /* the browser's, which is also the viewer's */
        UWORD   qualifier;
        UWORD   code;
    } map[3] = {
        { RFB_BUTTON_LEFT,   IEQUALIFIER_LEFTBUTTON,  IECODE_LBUTTON },
        { RFB_BUTTON_RIGHT,  IEQUALIFIER_RBUTTON,     IECODE_RBUTTON },
        { RFB_BUTTON_MIDDLE, IEQUALIFIER_MIDBUTTON,   IECODE_MBUTTON }
    };
    ULONG i;

    for (i = 0; i < 3UL; i++)
    {
        BOOL now  = (BOOL)((((rfb_u32)mask) & map[i].bit) != 0);
        BOOL was  = (BOOL)((fb_buttons & map[i].qualifier) != 0);

        if (now == was)
            continue;

        if (now)
            fb_buttons |= map[i].qualifier;
        else
            fb_buttons &= (UWORD)~map[i].qualifier;

        memset(&fb_event, 0, sizeof(fb_event));
        fb_event.ie_Class = IECLASS_RAWMOUSE;
        fb_event.ie_Code  = now ? map[i].code
                                : (UWORD)(map[i].code | IECODE_UP_PREFIX);
        /*
         * RELATIVEMOUSE with no movement.  Without it ie_X and ie_Y are read
         * as an absolute position, and a button event carrying (0,0) would
         * take the pointer to the top left corner on every click.
         */
        fb_event.ie_Qualifier = (UWORD)(fb_buttons |
                                        IEQUALIFIER_RELATIVEMOUSE);
        fb_event.ie_X = 0;
        fb_event.ie_Y = 0;
        fb_write_event();
    }
}

/* A key, as the Amiga rawkey code the viewer already sends.  IECODE_UP_PREFIX
   is the release, which is the same convention the keyboard itself uses. */
static VOID fb_inject_key(rfb_s32 raw, rfb_s32 qual, BOOL down)
{
    memset(&fb_event, 0, sizeof(fb_event));
    fb_event.ie_Class = IECLASS_RAWKEY;
    fb_event.ie_Code  = (UWORD)((raw & 0x7F) |
                                (down ? 0 : IECODE_UP_PREFIX));
    /* The mouse buttons stay in it: a drag with a qualifier held is one state
       and Intuition reads all of it off this field. */
    fb_event.ie_Qualifier = (UWORD)(((ULONG)qual & 0xFFFFUL) | fb_buttons);
    fb_write_event();
}

/*
 * A refresh, honoured, coalesced or deferred.
 *
 * The shadow goes and the viewer is TOLD, because a full frame is not
 * distinguishable from an ordinary one and the tiles in it are XOR against
 * the shadow.  A viewer that still has the last picture when one arrives XORs
 * the two together and gets the all-zero screen: grey, on a stock Workbench,
 * and it stays grey because an idle desktop produces no more tiles to correct
 * it.  geom is the barrier that already exists for that -- everything the
 * viewer had is discarded when one arrives -- so re-queuing geom and pal puts
 * a known zero on both sides at the same point in the stream.
 */
static VOID fb_ask_resync(VOID)
{
    ULONG now = fb_ticks();

    /* One is already on its way; a second changes nothing but the timing. */
    if (fb_resync)
        return;

    /* Too soon after the last, so it is remembered rather than dropped: a
       viewer asking constantly still re-syncs, just not every frame. */
    if (fb_resync_ever && (LONG)(now - fb_resync_at) < (LONG)FB_RESYNC_FLOOR)
    {
        fb_resync_due = 1;
        return;
    }

    fb_forget_shadow();
    fb_want_geom   = 1;
    fb_want_pal    = 1;
    fb_resync      = 1;
    fb_resync_due  = 0;
    fb_resync_ever = 1;
    fb_resync_at   = now;
}

/* ------------------------------------------------------------- input words -- */

static VOID fb_take_word(const char *w, ULONG len)
{
    rfb_input ev;

    if (!rfb_word_parse(w, (rfb_u32)len, &ev))
        return;                     /* not ours; ignored, never an error */

    switch (ev.kind)
    {
    case RFB_IN_REFRESH:
        fb_ask_resync();
        break;

    case RFB_IN_POINTER:
        /* Buttons first: a press that arrives in the same word as a move is a
           press AT that position, and the other order clicks where the pointer
           used to be. */
        fb_inject_pointer(ev.a, ev.b);
        fb_inject_buttons(ev.c);
        break;

    case RFB_IN_KEYDOWN:
        fb_inject_key(ev.a, ev.b, TRUE);
        break;

    case RFB_IN_KEYUP:
        fb_inject_key(ev.a, ev.b, FALSE);
        break;

    case RFB_IN_RESET:
        fb_reset = 1;
        fb_close_saying(HTTP_WS_CLOSE_GOING, "the Amiga is rebooting");
        break;

    case RFB_IN_WHEEL:
        /*
         * Dropped, deliberately.  AmigaOS 3.1 has no wheel: there is no input
         * class for one and nothing in a stock Workbench reads the rawkey
         * codes a third-party driver invented for it, so injecting those would
         * send keystrokes that some programs would act on as keystrokes.  The
         * word is read and refused rather than left to fail as a framing
         * error, so a viewer that sends it costs nothing.
         */
        break;

    default:
        break;
    }
}

static VOID fb_sink(void *ctx, HttpWsEvent ev, const unsigned char *data,
                    long len, int final)
{
    (VOID)ctx;

    switch (ev)
    {
    case HTTP_WS_EV_TEXT:
    {
        long i;

        for (i = 0; i < len; i++)
        {
            if (fb_word_n < (UWORD)FB_WORD_MAX)
                fb_word[fb_word_n++] = (char)data[i];
            else
                fb_word_over = 1;
        }

        if (final)
        {
            if (!fb_word_over)
                fb_take_word(fb_word, (ULONG)fb_word_n);
            fb_word_n = 0;
            fb_word_over = 0;
        }
        break;
    }

    case HTTP_WS_EV_BINARY:
        /* There is no inbound data stream on this socket: a viewer sends
           input and asks for redraws, and both are control.  A binary frame
           is a client sending something this does not speak. */
        break;

    case HTTP_WS_EV_PING:
        fb_control(HTTP_WS_EV_PONG, data, (ULONG)len);
        break;

    case HTTP_WS_EV_PONG:
        break;

    case HTTP_WS_EV_CLOSE:
        fb_close_session(HTTP_WS_CLOSE_NORMAL);
        break;

    default:
        break;
    }
}

/* ------------------------------------------------------------ the lifetime -- */

BOOL http_fb_open(VOID)
{
    struct Screen *sc;
    ULONG          ilock = 0;
    BOOL           pub;
    BOOL           ok;

    fb_why[0] = '\0';

    if (!fb_open_libraries())
    {
        fb_close_libraries();
        return FALSE;
    }

    sc = fb_lock_front(&pub);
    if (sc == NULL)
    {
        fb_say("there are no screens: Intuition's screen list is empty.  -C "
               "serves the frontmost screen, so a machine that has opened "
               "none has nothing to serve");
        fb_close_libraries();
        return FALSE;
    }

    if (!pub)
        ilock = LockIBase(0);

    ok = (BOOL)(pub || fb_listed(sc));
    if (ok)
        ok = fb_geometry_of(sc->RastPort.BitMap, &fb_open_geom);
    else
        fb_say("the front screen closed while it was being looked at");

    if (!pub)
        UnlockIBase(ilock);
    else
        UnlockPubScreen(NULL, sc);

    if (!ok)
    {
        fb_close_libraries();
        return FALSE;
    }

    /*
     * And the input stream.  Held for the server's life beside the libraries
     * and for the same reason: a machine that cannot be typed at says so
     * before it is serving anything, rather than showing a screen that does
     * not answer the mouse and leaving somebody to work out why.
     */
    if (!fb_input_open())
    {
        fb_input_close();
        fb_close_libraries();
        return FALSE;
    }

    fb_on = TRUE;
    return TRUE;
}

VOID http_fb_close(VOID)
{
    http_fb_stop();
    fb_input_close();
    fb_close_libraries();
    fb_on = FALSE;
}

BOOL http_fb_enabled(VOID)
{
    return fb_on;
}

BOOL http_fb_available(VOID)
{
    return (BOOL)(fb_on && !fb_live);
}

BOOL http_fb_start(struct Library *sb, LONG sock,
                   const UBYTE *first, ULONG first_len, ULONG now)
{
    struct Screen *sc;
    FbGeometry     g;
    FbGeometry     again;
    const UBYTE   *planes[FB_MAX_DEPTH];
    ULONG          ilock = 0;
    BOOL           pub;
    BOOL           moved;
    BOOL           ok;

    if (!fb_on || fb_live)
    {
        fb_say("the console is not available");
        return FALSE;
    }

    fb_why[0] = '\0';

    /* The geometry is read again rather than taken from startup: a screen mode
       changed since then, or another screen having come to the front, is the
       ordinary case and not an error. */
    sc = fb_lock_front(&pub);
    if (sc == NULL)
    {
        fb_say("there is no screen to serve");
        return FALSE;
    }

    if (!pub)
        ilock = LockIBase(0);

    ok = (BOOL)(pub || fb_listed(sc));
    if (ok)
        ok = fb_geometry_of(sc->RastPort.BitMap, &g);
    else
        fb_say("the front screen closed while it was being looked at");

    if (!pub)
        UnlockIBase(ilock);
    else
        UnlockPubScreen(NULL, sc);

    if (!ok)
        return FALSE;

    /* Outside every lock, because it allocates: LockIBase() stops Intuition
       and AllocVec() is not something to stop it across. */
    if (!fb_take_buffers(&g))
        return FALSE;

    /*
     * The colours, before the first grab.  fb_take_buffers() clears the
     * remembered palette so that grab's comparison is against nothing, and
     * without this the first `pal` word out is 3 << depth zeroes -- a black
     * palette, which is what a viewer draws until the second one arrives a
     * frame later.
     *
     * The front screen is resolved a second time rather than kept from above,
     * and the read is skipped if anything about it has changed in between:
     * that leaves fb_pal at zero, the first grab finds the colours moved, and
     * the viewer gets them one frame late instead of getting a screen's
     * geometry with another screen's palette.
     */
    sc = fb_lock_front(&pub);
    if (sc != NULL)
    {
        if (!pub)
            ilock = LockIBase(0);

        if ((pub || fb_listed(sc)) &&
            fb_examine(sc, &g, &again, planes, &moved) == FB_GRAB_OK)
            fb_want_pal = 1;

        if (!pub)
            UnlockIBase(ilock);
        else
            UnlockPubScreen(NULL, sc);
    }

    fb_sb         = sb;
    fb_sock       = sock;
    fb_ctl_n      = 0;
    fb_ctl_at     = 0;
    fb_pinged     = 0;
    fb_closing    = 0;
    fb_close_code = 0;
    fb_progress   = now;
    fb_word_n     = 0;
    fb_word_over  = 0;
    fb_tx_len     = 0;
    fb_tx_sent    = 0;
    fb_next_tick  = 0;
    fb_frames     = 0;
    fb_bytes      = 0;
    fb_grab_ticks = 0;
    fb_encode_ticks = 0;
    fb_since_stat = 0;
    fb_torn       = 0;
    fb_gone       = 0;
    fb_gone_at    = 0;
    fb_gone_passes = 0;
    fb_ptr_have   = 0;
    fb_want_ptr   = 0;
    fb_ptr_next   = 0;
    fb_want_stat  = 0;
    fb_reset      = 0;
    /* A session opens with geom, pal and a full frame already queued, which
       is a resync by any other name: a refresh arriving before that frame has
       gone -- which is exactly what a viewer asking on `geom` produces --
       must not restart it. */
    fb_resync      = 1;
    fb_resync_due  = 0;
    fb_resync_ever = 0;
    fb_resync_at   = 0;

    http_ws_reset(&fb_in);

    fb_live = TRUE;

    if (first_len > 0UL)
        (VOID)http_ws_feed(&fb_in, first, (long)first_len, fb_sink, NULL);

    return TRUE;
}

/*
 * ACTION_FLUSH to every mounted volume, and then the machine goes.
 *
 * ColdReboot() on its own can lose work: AmigaDOS filesystems hold dirty
 * buffers for about half a second, and httpd is a WebDAV server, so a file
 * written a moment ago may well be one of them.  ACTION_FLUSH is the packet
 * that empties them and it is what a handler answers when a person clicks the
 * disk's icon and chooses to flush.
 *
 * The ports are collected under the DosList lock and the packets sent with it
 * given back: DoPkt() waits for the handler to answer, the handler may want
 * the DosList to do so, and a caller that held it across the packet would be
 * the deadlock.
 *
 * WHAT THIS DOES NOT DO.  A program holding unsaved work in memory loses it
 * -- there is no Amiga convention for asking every task to save, and
 * NetShutdown's CTRL_C is about network resources, not about documents.  The
 * viewer's dialog says so before it sends the word.
 */
#define FB_FLUSH_MAX    16

static VOID fb_reboot(VOID)
{
    struct MsgPort *ports[FB_FLUSH_MAX];
    struct DosList *dl;
    ULONG           n = 0;
    ULONG           i;

    dl = LockDosList(LDF_VOLUMES | LDF_READ);
    if (dl != NULL)
    {
        while (n < (ULONG)FB_FLUSH_MAX &&
               (dl = NextDosEntry(dl, LDF_VOLUMES | LDF_READ)) != NULL)
        {
            if (dl->dol_Task != NULL)
                ports[n++] = dl->dol_Task;
        }
        UnLockDosList(LDF_VOLUMES | LDF_READ);
    }

    for (i = 0; i < n; i++)
        (VOID)DoPkt(ports[i], ACTION_FLUSH, 0, 0, 0, 0, 0);

    /* Half a second for the close frame and the FIN to leave: the viewer is
       told it is a reboot rather than left to call it a dropped connection,
       and that only works if the bytes get out first. */
    Delay(25);

    ColdReboot();
}

VOID http_fb_stop(VOID)
{
    if (!fb_live)
    {
        /* Belt and braces: a start that failed half way frees its own, but a
           shutdown must not depend on that having been the only path. */
        fb_free_buffers();
        return;
    }

    fb_live = FALSE;
    fb_sb   = NULL;
    fb_sock = -1;

    /* A viewer that goes away mid-drag leaves a button down, and the machine
       then behaves as if somebody were holding the mouse: menus stay up and
       nothing else can be clicked.  Released here, which is the only place
       that knows the far end has gone. */
    if (fb_buttons != 0)
        fb_inject_buttons(0);

    fb_free_buffers();

    /* Last, because this does not return.  Here and not where the word was
       read: the session had to end first, so the close frame saying why has
       already gone to the socket. */
    if (fb_reset)
    {
        fb_reset = 0;
        fb_reboot();
    }
}

UWORD http_fb_why(VOID)
{
    return fb_close_code;
}

VOID http_fb_stats(ULONG *frames, ULONG *bytes, ULONG *grab_ticks,
                   ULONG *encode_ticks)
{
    if (frames != NULL)       *frames = fb_frames;
    if (bytes != NULL)        *bytes = fb_bytes;
    if (grab_ticks != NULL)   *grab_ticks = fb_grab_ticks;
    if (encode_ticks != NULL) *encode_ticks = fb_encode_ticks;
}

/* --------------------------------------------------------------- the passes -- */

BOOL http_fb_wants_write(VOID)
{
    if (!fb_live)
        return FALSE;

    return (BOOL)(fb_tx_sent < fb_tx_len || fb_ctl_at < fb_ctl_n ||
                  fb_closing);
}

BOOL http_fb_read(ULONG now)
{
    /* Static for the reason the palette scratch is.  One connection is read
       per pass of the server's loop, so there is never a second reader. */
    static UBYTE scratch[512];
    LONG  got;

    if (!fb_live)
        return FALSE;

    got = tool_sock_recv(fb_sb, fb_sock, scratch, (LONG)sizeof(scratch));

    if (got == 0)
        return FALSE;                   /* the browser hung up */

    if (got < 0)
    {
        LONG err = tool_sock_errno(fb_sb);

        return (err == TOOL_EWOULDBLOCK || err == TOOL_EINTR) ? TRUE : FALSE;
    }

    fb_progress = now;
    fb_pinged   = 0;                    /* anything at all is a live peer */

    (VOID)http_ws_feed(&fb_in, scratch, got, fb_sink, NULL);

    if (fb_in.failed != 0)
    {
        /* The framing is lost and cannot be resynchronised. */
        fb_close_session((UWORD)fb_in.failed);
        return TRUE;
    }

    return TRUE;
}

/*
 * One grab and one encode, or one control word, per pass of the server's loop.
 *
 * Nothing is produced while the transmit buffer still holds bytes, so a slow
 * client cannot make this build a backlog; and nothing is grabbed twice inside
 * one tick, so an idle screen does not have 40 KB of chip RAM read on every
 * pass of a loop whose wait is two milliseconds.
 */
BOOL http_fb_slice(ULONG now)
{
    ULONG      t0;
    ULONG      t1;
    long       n;
    FbGeometry seen;
    BOOL       palette_moved;
    int        rc;

    (VOID)now;

    if (!fb_live || fb_closing)
        return TRUE;

    /* Still draining, or a pong is waiting to overtake: either way there is
       nowhere to put anything. */
    if (fb_tx_sent < fb_tx_len || fb_ctl_at < fb_ctl_n)
        return TRUE;

    /* A refresh that was asked for under the floor is owed one, and this is
       where it comes due. */
    if (fb_resync_due && !fb_resync)
        fb_ask_resync();

    /* And the pointer, which is its own clock: see FB_PTR_EVERY. */
    fb_pointer_poll(fb_ticks());

    if (fb_want_geom)
    {
        rfb_u32 len = rfb_word_geom((char *)&fb_tx[10],
                                    fb_tx_cap - 10UL, &fb_rg);

        if (len == 0UL)
        {
            fb_close_saying(HTTP_WS_CLOSE_PROTOCOL,
                            "the geometry word would not fit");
            return TRUE;
        }

        fb_frame_payload(HTTP_WS_EV_TEXT, (ULONG)len);
        fb_want_geom = 0;
        return TRUE;
    }

    if (fb_want_pal)
    {
        rfb_u32 len = rfb_word_pal((char *)&fb_tx[10], fb_tx_cap - 10UL,
                                   fb_pal, 1UL << fb_geom.depth);

        if (len == 0UL)
        {
            fb_close_saying(HTTP_WS_CLOSE_PROTOCOL,
                            "the palette word would not fit");
            return TRUE;
        }

        fb_frame_payload(HTTP_WS_EV_TEXT, (ULONG)len);
        fb_want_pal = 0;
        return TRUE;
    }

    if (fb_want_ptr)
    {
        rfb_pointer w;
        rfb_u32     len;

        w.width   = (rfb_u16)fb_ptr.width;
        w.height  = (rfb_u16)fb_ptr.height;
        w.depth   = (rfb_u16)fb_ptr.depth;
        w.x_scale = (rfb_u16)fb_ptr.x_scale;
        w.y_scale = (rfb_u16)fb_ptr.y_scale;
        w.hot_x   = (rfb_s16)fb_ptr.hot_x;
        w.hot_y   = (rfb_s16)fb_ptr.hot_y;

        len = rfb_word_ptr((char *)&fb_tx[10], fb_tx_cap - 10UL, &w,
                           fb_ptr.rgb, fb_ptr.bits);

        /* A pointer this cannot carry is DROPPED and not fatal: the viewer
           keeps whatever it had, which is its own arrow at worst, and nothing
           else about the session is affected. */
        fb_want_ptr = 0;

        if (len != 0UL)
        {
            fb_frame_payload(HTTP_WS_EV_TEXT, (ULONG)len);
            return TRUE;
        }
    }

    if (fb_want_stat)
    {
        /* An unrecognised word is ignored at the far end, which is what lets
           this go down the same channel without the viewer knowing it. */
        static const char *const tags[6] = { "fbstat f=", " b=", " gt=", " et=",
                                            " tn=", " gn=" };
        const ULONG values[6] = { fb_frames, fb_bytes, fb_grab_ticks,
                                  fb_encode_ticks, fb_torn, fb_gone_passes };
        ULONG at = 0;
        ULONG f;
        ULONG i;

        for (f = 0; f < 6UL; f++)
        {
            for (i = 0; tags[f][i] != '\0'; i++)
                fb_tx[10 + at++] = (UBYTE)tags[f][i];

            at = fb_put_num(&fb_tx[10], fb_tx_cap - 10UL, at, values[f]);
        }

        fb_frame_payload(HTTP_WS_EV_TEXT, at);
        fb_want_stat = 0;
        return TRUE;
    }

    {
        ULONG tick = fb_ticks();

        /* A signed difference, so midnight is a wrap and not a stall: the
           clock goes back to zero once a day and a plain `<` would stop
           grabbing until the next day's ticks caught up. */
        if (fb_next_tick != 0UL && (LONG)(tick - fb_next_tick) < 0L)
            return TRUE;

        fb_next_tick = tick + FB_GRAB_FLOOR;
        if (fb_next_tick == 0UL)
            fb_next_tick = 1UL;         /* 0 means "never grabbed" */
    }

    /*
     * gt= is what is left of the grab -- locking the public screen, checking
     * the geometry, reading the ColorMap -- now that there is no copy of the
     * screen to make.  et= is the pass that reads the bitplanes.  The two used
     * to be a 40 KB copy and then a walk over the copy; slice (gt+et) is the
     * figure that stayed comparable across that change.
     */
    t0 = fb_ticks();
    rc = fb_grab_frame(&fb_geom, &seen, &palette_moved,
                       &fb_tx[10], fb_tx_cap - 10UL, &n);
    t1 = fb_ticks();
    if (t1 >= t0)
        fb_encode_ticks += t1 - t0;

    switch (rc)
    {
    case FB_GRAB_OK:
        break;

    case FB_GRAB_GONE:
        /*
         * NO SCREEN THIS PASS, WHICH IS WHAT A RESOLUTION CHANGE LOOKS LIKE.
         *
         * The session is NOT ended and the viewer is told nothing: it stops
         * receiving frames for as long as the gap lasts and then gets the geom
         * that announces the screen that came back.  Ending it here is the
         * defect this replaces -- a person changing the screen mode from the
         * browser lost the browser, which is the one moment they cannot
         * recover from by hand.
         */
        fb_gone_passes++;

        if (!fb_gone)
        {
            fb_gone    = 1;
            fb_gone_at = fb_ticks();
            return TRUE;
        }

        if ((LONG)(fb_ticks() - fb_gone_at) < (LONG)FB_GONE_GRACE)
            return TRUE;

        fb_close_saying(HTTP_WS_CLOSE_GOING,
                        "there has been no screen to show for ten seconds");
        return TRUE;

    case FB_GRAB_VANISHED:
        /* The screen closed while it was being resolved.  Nothing this pass;
           the next one resolves whatever is in front now, and that is either
           the same shape -- in which case the stream simply carries on -- or a
           different one, which comes back as CHANGED below. */
        return TRUE;

    case FB_GRAB_CHANGED:
        /*
         * The screen changed shape under a live session, or another screen of
         * a different shape came to the front.  Everything the receiver has is
         * now about a screen it is not being shown, so the buffers are retaken
         * at the new geometry and it is told again -- which is what stops a
         * frame going out that disagrees with the last geometry it was given.
         *
         * This is also how a session comes back from an empty screen list: the
         * screen that reopens in the new resolution is a different shape, so
         * the barrier that already existed is the one that carries it.
         */
        fb_gone = 0;

        if (!fb_take_buffers(&seen))
        {
            fb_close_saying(HTTP_WS_CLOSE_GOING,
                            "the screen changed and this could not follow it");
            return TRUE;
        }
        return TRUE;

    case FB_GRAB_REFUSED:
    default:
        fb_close_saying(HTTP_WS_CLOSE_GOING,
                        "the front screen is not one this can read");
        return TRUE;
    }

    /*
     * A screen came back into an empty list at the SAME shape -- an overscan
     * change that did not move the bitmap, or a reopen caught between two
     * grabs.  Nothing above fires, because nothing about the geometry changed,
     * and yet this is a different screen with a different bitmap: the shadow
     * describes a picture that is not there any more.  So the barrier is
     * raised by hand, which is the same three things fb_ask_resync() does and
     * without its floor, because this is not a viewer asking twice.
     *
     * The frame just encoded is dropped.  It cost a pass, and the shadow it
     * updated is zeroed on the line below, so the full frame that follows the
     * geom is a delta from zero at both ends.
     */
    if (fb_gone)
    {
        fb_gone = 0;
        fb_forget_shadow();
        fb_want_geom = 1;
        fb_want_pal  = 1;
        return TRUE;
    }

    if (palette_moved)
    {
        fb_want_pal = 1;
        return TRUE;                    /* colours before the pixels */
    }

    if (n < 0)
    {
        fb_close_saying(HTTP_WS_CLOSE_PROTOCOL,
                        "the frame encoder would not encode this screen");
        return TRUE;
    }

    fb_frame_payload(HTTP_WS_EV_BINARY, (ULONG)n);

    /* The frame the resync promised has gone; the next refresh is a new
       question rather than a repeat of this one. */
    fb_resync = 0;

    fb_frames++;
    fb_bytes += (ULONG)n;

    if (++fb_since_stat >= (ULONG)FB_STAT_EVERY)
    {
        fb_since_stat = 0;
        fb_want_stat  = 1;
    }

    return TRUE;
}

BOOL http_fb_write(ULONG now)
{
    /* Not a liveness signal.  What this end managed to hand the local stack
       says the socket buffer had room and nothing whatever about whether
       anybody is still on the far end; see http_ws_live_stale(). */
    (VOID)now;

    if (!fb_live)
        return FALSE;

    for (;;)
    {
        if (fb_tx_sent < fb_tx_len)
        {
            LONG sent = tool_sock_send(fb_sb, fb_sock, &fb_tx[fb_tx_sent],
                                       (LONG)(fb_tx_len - fb_tx_sent));

            if (sent < 0)
            {
                LONG err = tool_sock_errno(fb_sb);

                return (err == TOOL_EWOULDBLOCK || err == TOOL_EINTR)
                           ? TRUE : FALSE;
            }

            fb_tx_sent += (ULONG)sent;

            if (fb_tx_sent < fb_tx_len)
                return TRUE;            /* the socket is full for now */
        }

        fb_tx_len  = 0;
        fb_tx_sent = 0;

        /* A pong or a close does not wait behind a frame. */
        if (fb_ctl_at < fb_ctl_n)
        {
            ULONG i;

            for (i = 0; i + fb_ctl_at < fb_ctl_n; i++)
                fb_tx[i] = fb_ctl[fb_ctl_at + i];

            fb_tx_len  = i;
            fb_tx_sent = 0;
            fb_ctl_at  = fb_ctl_n;
            continue;
        }

        /* RFC 6455 7.1.1 lets the server shut the socket once it has both
           sent and received a close, and this server is always the one that
           sent last. */
        if (fb_closing)
            return FALSE;

        return TRUE;
    }
}

BOOL http_fb_stale(ULONG now, ULONG timeout)
{
    if (!fb_live)
        return FALSE;

    return (BOOL)(http_ws_live_stale(fb_progress, (int)fb_pinged, now, timeout)
                      ? TRUE : FALSE);
}

BOOL http_fb_idle(ULONG now, ULONG timeout)
{
    if (!fb_live)
        return FALSE;

    if (http_fb_stale(now, timeout))
        return FALSE;

    if (http_ws_live_ping_due(fb_progress, (int)fb_pinged, now, timeout) &&
        fb_ctl_at >= fb_ctl_n)
    {
        fb_control(HTTP_WS_EV_PING, (const UBYTE *)"", 0);
        fb_pinged   = 1;
        fb_progress = now;
    }

    return TRUE;
}

/*
 * A CLOSE FRAME MAY ONLY GO AT A FRAME BOUNDARY.
 *
 * This writes to the socket itself rather than queueing, because the caller
 * closes the connection on the next line and there is no later pass to drain
 * anything in.  So it has to look at what is already half out: a frame here
 * is up to a whole screen -- 7 KB at 640x256x4, four times that at depth 8 --
 * and a viewer on a link whose window is smaller than one of them has the
 * rest of it still to come.  Writing the close after those bytes SPLICES it
 * into somebody's payload, and everything the receiver reads from there on is
 * a mis-framed stream: the browser reports a protocol error instead of the
 * sentence saying who took the screen, which is the one thing this function
 * exists to deliver.
 *
 * The rest of the frame is pushed first, as far as the socket will take it in
 * one go and no further -- this is called from the middle of the server's
 * loop and nothing here may block.  If it will not all go, the close is not
 * sent at all: a truncated frame and a FIN is an abnormal close, which is
 * honest, and is what the far end concludes anyway.
 */
VOID http_fb_evict(UWORD code)
{
    UBYTE         frame[HTTP_FB_CTL];
    unsigned long n;

    if (!fb_live || fb_closing)
        return;

    fb_closing    = 1;
    fb_close_code = code;

    while (fb_tx_sent < fb_tx_len)
    {
        LONG sent = tool_sock_send(fb_sb, fb_sock, &fb_tx[fb_tx_sent],
                                   (LONG)(fb_tx_len - fb_tx_sent));

        if (sent <= 0)
            return;

        fb_tx_sent += (ULONG)sent;
    }

    n = http_ws_close_frame(frame, sizeof(frame), code,
                            "the console was taken over from another browser");

    if (n > 0UL)
        (VOID)tool_sock_send(fb_sb, fb_sock, frame, (LONG)n);
}

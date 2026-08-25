/* The frontmost screen down a WebSocket.  See httpfb.h.
 * SPDX-License-Identifier: MIT
 */

#include "tools.h"
#include "toolsock.h"
#include "httpws.h"
#include "httpfb.h"
#include "httprtg.h"

#include "aminetxduo/rfb_encode.h"
#include "aminetxduo/rfb_words.h"

#include <devices/input.h>
#include <devices/inputevent.h>
#include <dos/dosextens.h>
#include <exec/io.h>
#include <exec/memory.h>
#include <graphics/displayinfo.h>
#include <graphics/gfx.h>
#include <graphics/gfxbase.h>
#include <graphics/layers.h>
#include <graphics/modeid.h>
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

/* The encoder's shipping configuration.  RFB_F_INTERLEAVED is added at run
   time, in fb_take_buffers(), because the encoder reads the bitplanes where
   they lie and the layout it is told about has to be the layout they are in. */
#define FB_FLAGS    (RFB_F_BASELINE | RFB_F_COPYRECT | RFB_F_SCROLL_ADAPTIVE)

/* The most a text frame from a viewer can be worth reading.  Every word in the
   vocabulary is a keyword and at most three small numbers. */
#define FB_WORD_MAX         48

/* How often the session says what it is costing, in frames. */
#define FB_STAT_EVERY       32

/* The floor between grabs, in fiftieths.  A floor rather than a frame rate: the
   point past which grabbing again cannot produce anything a viewer can see. */
#define FB_GRAB_FLOOR       1

/* The share of the machine this may take, as the divisor of the idle owed after
   a frame: a frame costing T ticks is followed by at least T/3 of nothing, so
   the console settles at 75%.  Enforced against measured cost, not a rate. */
#define FB_IDLE_DIVISOR     3

/* How often a `refresh` can force a full frame, in fiftieths.  The first ask is
   answered at once; the floor applies to the second and later inside a second,
   so a viewer that asks every frame degrades to one re-sync a second. */
#define FB_RESYNC_FLOOR     50UL

/* How long a run with no screen at all is allowed to last, in fiftieths.  A
   resolution change closes one screen and opens another, and between the two
   Intuition's screen list is empty: a moment to wait through, not an error. */
#define FB_GONE_GRACE       500UL

struct GfxBase       *GfxBase;
struct IntuitionBase *IntuitionBase;

/* What the header says and what the copy loop needs.  row_stride and row_bytes
   differ only on an interleaved bitmap, where BytesPerRow spans every plane. */
typedef struct FbGeometry
{
    UWORD width;
    UWORD height;
    UWORD depth;
    UWORD row_bytes;
    ULONG row_stride;
    ULONG frame_bytes;
    UWORD interleaved;      /* BMF_INTERLEAVED, as the BitMap reported it */
    /* What a pixel is, as one of the RFB_FMT_ values.  PLANAR, HAM6, HAM8 and
       EHB are the same planes at the same stride and differ only in how long
       `pal` is; the chunky ones come through fb_stage, not the bitplanes. */
    UWORD format;
} FbGeometry;

/* ------------------------------------------------------------- the module -- */

static BOOL fb_on;
static char fb_why[200];

static FbGeometry fb_open_geom;

/* TRUE when -C started on a machine that had no screen open at all.  Not a
   failure: http_fb_start() reads the front screen again per session, so the
   console starts working by itself when Workbench arrives. */
static BOOL fb_open_screenless;

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
static UBYTE           fb_word_over;    /* the message is longer than is read  */

/* Why the last geometry was refused, for the close frame.  It points either at
   a string literal or at fb_why, which is filled by the same call that sets
   this and read by the close frame in the same pass. */
static const char     *fb_refuse_why;

static FbGeometry      fb_geom;
static rfb_geom        fb_rg;
static rfb_encoder     fb_enc;
static rfb_scroll_cfg  fb_cfg;
static rfb_u32         fb_flags;        /* FB_FLAGS, plus the layout's own */

static UBYTE          *fb_shadow;
static UBYTE          *fb_scratch;
static UBYTE          *fb_tx;
/* Where a card's screen lands before it is encoded.  A card cannot be compared
   in place, because the compare would itself be the readback, so the screen is
   fetched whole once a frame into Fast RAM and the encoder reads that. */
static UBYTE          *fb_stage;
static ULONG           fb_stage_len;
static ULONG           fb_shadow_len;
static ULONG           fb_scratch_len;
static ULONG           fb_tx_cap;
static ULONG           fb_tx_len;
static ULONG           fb_tx_sent;

static UBYTE           fb_pal[3U * FB_MAX_COLOURS];
static UBYTE           fb_want_geom;
static UBYTE           fb_want_pal;
static UBYTE           fb_want_stat;

/* httprtg.c has measured this screen's readback routes and picked one.  Cleared
   with the buffers, so a screen change re-probes. */
static UBYTE           fb_rtg_ready;
static UBYTE           fb_want_rtg;

static ULONG           fb_next_tick;

/* The duty cycle, in ticks.  fb_frame_t0 is when the work for the frame in
   flight began, which is the grab and not the send, and is 0 when no frame is
   in flight: a tick of 0 is nudged to 1 by its writer, as fb_next_tick's is. */
static ULONG           fb_frame_t0;
static ULONG           fb_busy_ticks;

/* And how much idle has actually been handed back on account of it, so the
   share is enforced against the session's totals and not against whatever the
   last band happened to round to. */
static ULONG           fb_idle_given;

/* Which tile row the next band starts at; 0 means the next pass begins a fresh
   screen.  Producing a band at a time puts the server's read back on the path
   between them, at five bytes of message header per band. */
static UWORD           fb_band_ty0;

/* Whether the band just produced closed a screen pass.  Only that one counts
   a frame, so the frame counter keeps meaning screens and not messages. */
static UBYTE           fb_band_last;

/* What the last complete screen pass cost, in ticks, which is what decides
   whether the next one is worth banding.  Zero until one has finished, so the
   first pass of a session is whole. */
static ULONG           fb_pass_ticks;

/* And what the pass in progress has cost so far, since it is charged a band
   at a time. */
static ULONG           fb_pass_acc;

/* Tile rows in a band.  Four rows bounds one uninterrupted encode at an eighth
   of a 640x480 screen. */
#ifndef FB_BAND_ROWS
#define FB_BAND_ROWS        4
#endif

/* How expensive a screen pass has to have been for the next one to be banded,
   in fiftieths.  Banding a cheap pass is all cost: a change is only encoded
   when its own band comes round. */
#define FB_BAND_WHEN        2

/* A resync is a sequence, and asking twice must not restart it: a second
   refresh inside one would re-queue geom and re-clear a shadow the viewer has
   not yet been given a frame from. */
static UBYTE           fb_resync;      /* the sequence is under way        */
static UBYTE           fb_resync_due;  /* asked under the floor, so owed one */
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

/* Frames a card's screen was not read on because nothing could lock it.  See
   fb_grab_frame().  Reported in `fbstat` as nl=, so that a report of the
   picture stopping has a number behind it. */
static ULONG           fb_nolock;

/* The screen list was empty on the last pass, and when it first was.  See
   FB_GONE_GRACE.  `fb_gone_passes` is reported in `fbstat` as gn=. */
static UBYTE           fb_gone;
static ULONG           fb_gone_at;
static ULONG           fb_gone_passes;

/* A `reset` arrived and the machine goes as soon as the close frame telling
   the viewer so has been handed to the socket.  See fb_reboot(). */
static UBYTE           fb_reset;

/* ------------------------------------------------------------------ input -- */

/* input.device, opened with -C and held for the server's life.  One port, one
   request and one event, taken once.  The event is static because io_Data
   points at it while DoIO() runs and a Shell command has 4 KB of stack. */
static struct MsgPort   *fb_in_port;
static struct IOStdReq  *fb_in_req;
static BOOL              fb_in_open;
static struct InputEvent fb_event;

/* ie_X and ie_Y are not screen pixels: IECLASS_POINTERPOS carries mouse units,
   and the conversion is ticks-per-pixel over ticks-per-mouse-unit.  Both come
   from the display database; the mode bits cannot answer it. */
static UWORD             fb_res_x  = 22;    /* ticks per screen pixel        */
static UWORD             fb_res_y  = 44;
static UWORD             fb_tick_x = 22;    /* ticks per POINTERPOS unit     */
static UWORD             fb_tick_y = 22;
static UWORD             fb_spr_x  = 44;    /* ticks per sprite pixel        */
static UWORD             fb_spr_y  = 44;
static UWORD             fb_pixel_ns = 70;  /* what one screen pixel lasts   */
static ULONG             fb_mode_id  = INVALID_ID;

/* Where the front screen sits in the view, in its own pixels.  They change
   nothing about what is drawn: an injected IECLASS_POINTERPOS is a position in
   the view, so a dragged screen's row 0 is not the view's row 0. */
static WORD              fb_left;
static WORD              fb_top;

/* What the far end is holding down, as IEQUALIFIER_ bits.  Intuition reads the
   button state off the qualifier of every RAWMOUSE event, so a button not
   carried in the qualifier of what follows reads as released. */
static UWORD             fb_buttons;

/* ------------------------------------------------------------ diagnostics -- */

static VOID fb_say(const char *text)
{
    ULONG i;

    for (i = 0; i + 1U < sizeof(fb_why) && text[i] != '\0'; i++)
        fb_why[i] = text[i];
    fb_why[i] = '\0';
}

/* Decimal, into a caller's buffer, returning where it ended.  No printf: this
   module builds the same message into a close frame on a 4 KB stack. */
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

/* The same sentence to the log and to the close frame.  Every refusal goes
   through one of these two. */
static VOID fb_refuse(const char *text)
{
    fb_say(text);
    fb_refuse_why = fb_why;
}

static VOID fb_refuse3(const char *a, ULONG v, const char *b)
{
    fb_say3(a, v, b);
    fb_refuse_why = fb_why;
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

    /* Neither is required and neither is an error: what this decides is only
       whether an RTG screen in front can be read at all. */
    (VOID)http_rtg_open();

    if (GfxBase != NULL && IntuitionBase != NULL)
        return TRUE;

    fb_say("this needs Kickstart 3.0 or later: graphics and intuition must "
           "both answer OpenLibrary() at version 39");
    return FALSE;
}

static VOID fb_close_libraries(VOID)
{
    http_rtg_close();

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

/* TRUE when `sc` is still one of Intuition's screens.  The caller must hold
   LockIBase(): CloseScreen() takes IntuitionBase to unlink a screen before it
   frees it, so one found under that lock cannot be freed while it is held. */
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

/* The frontmost screen, locked when a lock is available.  FirstScreen is read
   under LockIBase() and the lock dropped at once, since Intuition is blocked
   while it is held.  A screen that is not public comes back unlocked. */
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

/* What a screen's bitmap is, or why it is not one that can be read.  Three
   answers: FB_GEOM_UNSURE is a bitmap this could not identify from where it
   was standing, which is not a refusal and must not end a session. */
enum
{
    FB_GEOM_OK = 0,
    FB_GEOM_NO,         /* a bitmap this cannot read, and fb_why says why    */
    FB_GEOM_UNSURE      /* the card that owns it could not be asked from here */
};

static int fb_unsure(VOID)
{
    fb_refuse("the front screen offers no lock and is not a planar bitmap, so "
              "the graphics card that owns it cannot be asked from here");
    return FB_GEOM_UNSURE;
}

/* Which of the planar pixel meanings a chipset screen carries.  The answer is
   on the ViewPort and not in the bitmap, so it is read from GetVPModeID() with
   the ViewPort's Modes word as the fallback.  Never inferred from the depth. */
static UWORD fb_planar_format(struct ViewPort *vp, ULONG depth)
{
    ULONG id;
    ULONG modes;

    if (vp == NULL)
        return (UWORD)RFB_FMT_PLANAR;

    id    = GetVPModeID(vp);
    modes = (id != (ULONG)INVALID_ID) ? id : (ULONG)(UWORD)vp->Modes;

    if ((modes & HAM_KEY) != 0UL)
    {
        if (depth == 6UL)
            return (UWORD)RFB_FMT_HAM6;
        if (depth == 8UL)
            return (UWORD)RFB_FMT_HAM8;
        return (UWORD)RFB_FMT_PLANAR;
    }

    if ((modes & EXTRAHALFBRITE_KEY) != 0UL && depth == 6UL)
        return (UWORD)RFB_FMT_EHB;

    return (UWORD)RFB_FMT_PLANAR;
}

static int fb_geometry_of(struct BitMap *bm, struct ViewPort *vp,
                          FbGeometry *g, BOOL may_ask_rtg)
{
    ULONG flags;
    ULONG depth;
    ULONG width;
    ULONG height;
    ULONG stride;
    UWORD plane;
    BOOL  blind;

    /* Cleared here, so a refusal from an earlier screen is never the sentence
       a later one closes with. */
    fb_refuse_why = NULL;

    if (bm == NULL)
    {
        fb_refuse("the front screen has no bitmap");
        return FB_GEOM_NO;
    }

    /* The card is asked before BMF_STANDARD is read, because a Picasso96 or
       CyberGraphX bitmap can carry that flag too.  `may_ask_rtg` is FALSE on
       the pass under LockIBase(): those calls must not be made there. */
    if (may_ask_rtg && http_rtg_owns(bm))
    {
        HttpRtgScreen rs;
        const char   *why = NULL;

        if (!http_rtg_describe(bm, (UWORD)(vp != NULL ? vp->DWidth : 0),
                               &rs, &why))
        {
            fb_refuse((why != NULL) ? why
                                    : "the front screen is an RTG screen this "
                                      "cannot read");
            return FB_GEOM_NO;
        }

        /* The staging buffer's row, and therefore the tile grid's.  Not the
           card's own stride, which is only valid while the bitmap is locked,
           and rounded up to a longword for the encoder's compare. */
        g->interleaved = 0;
        g->width       = rs.width;
        g->height      = rs.height;

        if (rs.bpp == 2)
        {
            g->format = RFB_FMT_RGB565;
            g->depth  = RFB_RGB565_DEPTH;
        }
        else
        {
            g->format = RFB_FMT_CLUT8;
            g->depth  = 8;
        }

        g->row_bytes   = (UWORD)((((ULONG)rs.width * rs.bpp) + 3UL) & ~3UL);
        g->row_stride  = (ULONG)g->row_bytes;
        g->frame_bytes = (ULONG)g->row_bytes * (ULONG)g->height;
        return FB_GEOM_OK;
    }

    /* Whether a refusal below is a finding or a guess.  Without the two calls
       above, a card's bitmap fails every planar test for reasons that say
       nothing about whether it can be read, so a blind refusal is UNSURE. */
    blind = (BOOL)(!may_ask_rtg && http_rtg_present());

    flags  = GetBitMapAttr(bm, BMA_FLAGS);
    depth  = GetBitMapAttr(bm, BMA_DEPTH);
    width  = GetBitMapAttr(bm, BMA_WIDTH);
    height = GetBitMapAttr(bm, BMA_HEIGHT);

    if ((flags & BMF_STANDARD) == 0)
    {
        if (blind)
            return fb_unsure();

        fb_refuse(http_rtg_present()
                  ? "the front screen is not a standard planar bitmap and "
                    "neither Picasso96 nor CyberGraphX claims it, so there are "
                    "no pixels here anything can read"
                  : "the front screen is not a standard planar bitmap, so it "
                    "has no bitplanes to read. A graphics card needs "
                    "Picasso96API.library or cybergraphics.library, and neither "
                    "answered OpenLibrary()");
        return FB_GEOM_NO;
    }

    g->format = RFB_FMT_PLANAR;

    if (depth < 1 || depth > FB_MAX_DEPTH)
    {
        if (blind)
            return fb_unsure();

        fb_refuse3("the front screen is ", depth,
                   " planes deep. This handles 1 to 8");
        return FB_GEOM_NO;
    }

    if (width < 1 || width > 65535 || height < 1 || height > 65535)
    {
        if (blind)
            return fb_unsure();

        fb_refuse("the front screen does not fit the wire format");
        return FB_GEOM_NO;
    }

    stride = (ULONG)(UWORD)bm->BytesPerRow;

    /* An interleaved bitmap's BytesPerRow spans every plane, so that is the
       stride from one row to the next in a plane, and a plane's row is that
       divided by the depth.  A non-interleaved one has the two equal. */
    g->interleaved = (UWORD)(((flags & BMF_INTERLEAVED) != 0) ? 1 : 0);

    if (g->interleaved)
    {
        if (stride == 0 || (stride % depth) != 0)
        {
            if (blind)
                return fb_unsure();

            fb_refuse3("interleaved bitmap with BytesPerRow=", stride,
                       ", which does not divide by the depth");
            return FB_GEOM_NO;
        }
        g->row_bytes = (UWORD)(stride / depth);
    }
    else
    {
        g->row_bytes = (UWORD)stride;
    }

    if ((ULONG)g->row_bytes * 8UL < width)
    {
        if (blind)
            return fb_unsure();

        fb_refuse3("the bitmap says ", (ULONG)g->row_bytes,
                   " bytes a row, which is too few for its width");
        return FB_GEOM_NO;
    }

    for (plane = 0; plane < (UWORD)depth; plane++)
    {
        if (bm->Planes[plane] == NULL)
        {
            if (blind)
                return fb_unsure();

            fb_refuse3("bitplane ", (ULONG)plane, " is not allocated");
            return FB_GEOM_NO;
        }
    }

    /* BMA_WIDTH answers for the allocation, which a planar screen rounds up
       to 16, so a 1368-wide screen sits in a 1376-wide bitmap and the last
       8 columns are not the screen's. */
    if (vp != NULL && vp->DWidth > 0 && (ULONG)vp->DWidth < width)
        width = (ULONG)vp->DWidth;

    g->width       = (UWORD)width;
    g->height      = (UWORD)height;
    g->depth       = (UWORD)depth;
    g->row_stride  = stride;
    g->frame_bytes = (ULONG)g->row_bytes * (ULONG)g->height * depth;

    /* Last, because it needs the depth the checks above have settled.  It
       changes nothing measured here, only what the receiver is told to make
       of it. */
    g->format = fb_planar_format(vp, depth);

    return FB_GEOM_OK;
}

static BOOL fb_geometry_same(const FbGeometry *a, const FbGeometry *b)
{
    return (BOOL)(a->width == b->width && a->height == b->height &&
                  a->depth == b->depth && a->row_bytes == b->row_bytes &&
                  a->row_stride == b->row_stride &&
                  a->interleaved == b->interleaved &&
                  a->format == b->format);
}

VOID http_fb_geometry(UWORD *w, UWORD *h, UWORD *depth)
{
    if (w != NULL)     *w = fb_open_geom.width;
    if (h != NULL)     *h = fb_open_geom.height;
    if (depth != NULL) *depth = fb_open_geom.depth;
}

BOOL http_fb_screenless(VOID)
{
    return fb_open_screenless;
}

/* ---------------------------------------------------------------- palette -- */

/* How many colours this screen's `pal` word carries, and 0 when it has none.
   Asked of rfb_pal_colours() rather than computed here, because 1 << depth is
   right on a plain planar screen and on nothing else. */
static ULONG fb_colours(const FbGeometry *g)
{
    rfb_geom q;

    memset(&q, 0, sizeof(q));
    q.depth  = (rfb_u8)g->depth;
    q.format = (rfb_u8)g->format;

    return (ULONG)rfb_pal_colours(&q);
}

/* 3 * `colours` bytes whatever the ColorMap holds; a shorter map leaves the
   tail black rather than shortening the word.  TRUE when it changed, which is
   what decides whether a `pal` word goes out. */
static BOOL fb_read_palette(struct ColorMap *cm, ULONG colours, UBYTE *pal)
{
    /* Static rather than automatic: 768 bytes at depth 8, and a Shell command
       has 4 KB of stack for everything httpd already has on it. */
    static UBYTE fresh[3U * FB_MAX_COLOURS];
    ULONG have    = (cm != NULL) ? (ULONG)cm->Count : 0;
    ULONG first;
    ULONG i;
    BOOL  moved = FALSE;

    /* A truecolour screen has no palette, so there is nothing to compare and
       nothing ever moves.  Answering FALSE here is what keeps the `pal` word
       off the wire for the whole session. */
    if (colours == 0UL)
        return FALSE;
    if (colours > FB_MAX_COLOURS)
        colours = FB_MAX_COLOURS;

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

/* The mouse pointer is a sprite, so it is in no frame, and the one on screen
   cannot be read back: SetWindowPointer() leaves struct Window's Pointer
   fields stale and pointerclass answers no OM_GET.  So the prefs are read. */

/* An IFF PREF file for a pointer is a few hundred bytes.  This is room for a
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

/* How often the preferences are re-read, in fiftieths.  Looking means three DOS
   calls on the one task that also serves every connection. */
#define FB_PTR_EVERY        50UL

/* Big-endian, out of a byte buffer, because that is how the file is and
   nothing here can assume the reader's alignment. */
static UWORD fb_be16(const UBYTE *p)
{
    return (UWORD)(((UWORD)p[0] << 8) | p[1]);
}

static ULONG fb_be32(const UBYTE *p)
{
    return ((ULONG)p[0] << 24) | ((ULONG)p[1] << 16) |
           ((ULONG)p[2] << 8) | p[3];
}

/* The four numbers the display database has and the mode bits do not, each a
   ratio of two lengths in ticks.  Re-read only when the mode changes; the
   screen's position in the view is re-read every time. */
static VOID fb_display_units(struct Screen *sc)
{
    struct DisplayInfo di;
    struct MonitorInfo mi;
    ULONG              id;
    UWORD              modes;
    BOOL               have_disp = FALSE;

    fb_left = sc->LeftEdge;
    fb_top  = sc->TopEdge;

    id = GetVPModeID(&sc->ViewPort);
    if (id != (ULONG)INVALID_ID && id == fb_mode_id)
        return;

    fb_mode_id = id;
    modes      = (UWORD)sc->ViewPort.Modes;

    /* The mode bits, which are what is left when there is no database entry to
       ask.  Right for PAL and NTSC. */
    if ((modes & SUPERHIRES) != 0)
    {
        fb_res_x    = 11;
        fb_pixel_ns = 35;
    }
    else if ((modes & HIRES) != 0)
    {
        fb_res_x    = 22;
        fb_pixel_ns = 70;
    }
    else
    {
        fb_res_x    = 44;
        fb_pixel_ns = 140;
    }
    fb_res_y  = (UWORD)(((modes & LACE) != 0) ? 22 : 44);
    fb_spr_x  = (UWORD)(fb_res_x * 2U);
    fb_spr_y  = fb_res_y;
    fb_tick_x = 22;
    fb_tick_y = 22;

    if (id == (ULONG)INVALID_ID)
        return;

    /* A short answer is not a failure.  GetDisplayInfoData() fills what the
       database record holds, which is 48 bytes of a struct that is longer than
       that, so the fields are checked over a struct zeroed first. */
    memset(&di, 0, sizeof(di));
    memset(&mi, 0, sizeof(mi));

    if (GetDisplayInfoData(NULL, (UBYTE *)&di, sizeof(di), DTAG_DISP, id) > 0 &&
        di.Resolution.x > 0 && di.Resolution.y > 0)
    {
        have_disp    = TRUE;
        fb_res_x    = (UWORD)di.Resolution.x;
        fb_res_y    = (UWORD)di.Resolution.y;
        if (di.PixelSpeed > 0)
            fb_pixel_ns = (UWORD)di.PixelSpeed;
        if (di.SpriteResolution.x > 0 && di.SpriteResolution.y > 0)
        {
            fb_spr_x = (UWORD)di.SpriteResolution.x;
            fb_spr_y = (UWORD)di.SpriteResolution.y;
        }
    }

    /* The two halves of the position must come from the same place, so a mode
       with no record of its own keeps the pair that was there before. */
    if (!have_disp)
        return;

    if (GetDisplayInfoData(NULL, (UBYTE *)&mi, sizeof(mi), DTAG_MNTR, id) > 0 &&
        mi.MouseTicks.x > 0 && mi.MouseTicks.y > 0)
    {
        fb_tick_x = (UWORD)mi.MouseTicks.x;
        fb_tick_y = (UWORD)mi.MouseTicks.y;
        return;
    }

    switch (id & MONITOR_ID_MASK)
    {
    case NTSC_MONITOR_ID:
    case DBLNTSC_MONITOR_ID:
        fb_tick_y = 26;
        break;
    case DEFAULT_MONITOR_ID:
    case A2024_MONITOR_ID:
        if ((GfxBase->DisplayFlags & PAL) == 0)
            fb_tick_y = 26;
        break;
    default:
        break;
    }
}

/* How many screen pixels one sprite pixel covers.  Both ratios come off the
   database, in ticks.  A sprite whose resolution was changed is in the V39
   ColorMap instead, in nanoseconds, so it divides the pixel speed. */
static VOID fb_pointer_scale(struct Screen *sc, UWORD *xs, UWORD *ys)
{
    struct ColorMap *cm = sc->ViewPort.ColorMap;
    UWORD            sprite_ns;
    UBYTE            resn = (UBYTE)SPRITERESN_ECS;

    fb_display_units(sc);

    /* Only a V39 ColorMap has the fields.  An older one is an ECS sprite. */
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
        sprite_ns = 0;      /* the default sprite: the database has it */
        break;
    }

    if (sprite_ns != 0 && fb_pixel_ns != 0)
        *xs = (UWORD)((sprite_ns >= fb_pixel_ns) ? (sprite_ns / fb_pixel_ns)
                                                 : 1);
    else
        *xs = (UWORD)((fb_spr_x >= fb_res_x) ? (fb_spr_x / fb_res_x) : 1);

    *ys = (UWORD)((fb_spr_y >= fb_res_y) ? (fb_spr_y / fb_res_y) : 1);
}

/* The sprite's own colours: pens 17, 18 and 19, with 16 transparent.  The
   registers carry the live values and are preferred.  Only at depth 2: a
   deeper sprite is an attached pair whose registers cannot be told here. */
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

/* devs:system-configuration's pointer, through GetPrefs(): a 16x16 two-plane
   sprite.  PointerMatrix is (1 + 16 + 1) word pairs, a control pair, sixteen
   interleaved rows and a terminator, and here is where the planes separate. */
static BOOL fb_pointer_from_prefs(FbPointer *p)
{
    /* Static, because struct Preferences is over 300 bytes and this runs on
       httpd's stack, which is a Shell command's. */
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

    /* RGB4 to RGB8, by repeating the nibble, so 0xF becomes 0xFF rather than
       0xF0.  0xF0 is a quarter of a step dark on everything and it shows on a
       white pointer. */
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

/* ENV:Sys/pointer.prefs, if it is there: an IFF FORM PREF holding one PNTR
   chunk per pointer.  The one wanted is pp_Which == WBP_NORMAL. */
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
            break;                          /* truncated, so take nothing */

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
                    /* A shape past what the wire carries.  Refused rather than
                       truncated, so the viewer keeps its own arrow instead of
                       drawing half a sprite at the wrong scale. */
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

/* The hotspot, kept inside the sprite.  A hotspot outside the image is not a
   hotspot for anything this draws.  A wrong one costs nothing: a click goes to
   the coordinate the viewer sends, not to where the image was drawn. */
static VOID fb_pointer_hotspot(FbPointer *p)
{
    if (p->hot_x < 0)                       p->hot_x = 0;
    if (p->hot_x > (WORD)(p->width - 1))    p->hot_x = (WORD)(p->width - 1);
    if (p->hot_y < 0)                       p->hot_y = 0;
    if (p->hot_y > (WORD)(p->height - 1))   p->hot_y = (WORD)(p->height - 1);
}

/* The current pointer, whatever it takes.  No screen is touched and nothing is
   locked, because this opens a file and a DOS call must not be made with
   Intuition stopped; the scale and colours are added in fb_pointer_poll(). */
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

/* Look again, and queue a word if the picture moved.  The file is read with
   nothing held; the screen is resolved afterwards and only the scale and the
   sprite's live colours come out of it. */
static VOID fb_pointer_poll(ULONG now)
{
    /* Static, and 2 KB of it.  This is the second of these buffers and it does
       not go on a 4 KB stack. */
    static FbPointer fresh;
    struct Screen   *sc;
    ULONG            ilock = 0;
    BOOL             pub;

    if (fb_ptr_next != 0UL && (LONG)(now - fb_ptr_next) < 0L)
        return;

    fb_ptr_next = now + FB_PTR_EVERY;
    if (fb_ptr_next == 0UL)
        fb_ptr_next = 1UL;              /* 0 means it has never looked */

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

/* The screen is not copied: the encoder reads the bitplanes where they are,
   without a drawing lock, and reads a tile it decides has changed once into a
   buffer, so the shadow cannot disagree with what the far end was sent. */
static LONG fb_encode_planes(const UBYTE **planes, UBYTE *out, ULONG out_cap,
                             UWORD ty0, UWORD ty1)
{
    return rfb_encode_band(&fb_enc, planes, out, out_cap,
                           (rfb_u16)ty0, (rfb_u16)ty1);
}

enum
{
    FB_GRAB_OK = 0,
    FB_GRAB_GONE,       /* there are no screens at all any more              */
    FB_GRAB_CHANGED,    /* it is not the screen the client was told about   */
    FB_GRAB_REFUSED,    /* a bitmap this cannot read, fb_why says why       */
    FB_GRAB_VANISHED,   /* it closed while it was being resolved            */
    FB_GRAB_UNREADABLE  /* an RTG screen nothing here can safely read       */
};

/* Everything that dereferences the Screen, its BitMap or its ColorMap.  The
   caller holds a public screen lock or LockIBase(), so this must not block and
   must not call Intuition.  What leaves is geometry, palette, units, planes. */
static int fb_examine(struct Screen *sc, const FbGeometry *want,
                      FbGeometry *now, const UBYTE **planes,
                      BOOL *palette_moved, BOOL locked)
{
    UWORD plane;
    int   what;

    what = fb_geometry_of(sc->RastPort.BitMap, &sc->ViewPort, now, locked);

    /* Nothing this pass, rather than the end of the session: the next pass
       resolves the front screen again with the card asked. */
    if (what == FB_GEOM_UNSURE)
        return FB_GRAB_UNREADABLE;

    if (what != FB_GEOM_OK)
        return FB_GRAB_REFUSED;

    if (!fb_geometry_same(want, now))
        return FB_GRAB_CHANGED;

    fb_display_units(sc);

    *palette_moved = fb_read_palette(sc->ViewPort.ColorMap,
                                     fb_colours(want), fb_pal);

    /* Colours before pixels, and the encode is skipped entirely on the pass
       that finds them moved. */
    if (*palette_moved)
        return FB_GRAB_OK;

    /* A card's pixels are not addressable from here, so they are read in
       fb_grab_frame() where the lock that makes the call safe is still held. */
    if (RFB_FMT_IS_CHUNKY(want->format))
        return FB_GRAB_OK;

    for (plane = 0; plane < want->depth; plane++)
        planes[plane] = (const UBYTE *)sc->RastPort.BitMap->Planes[plane];

    return FB_GRAB_OK;
}

/* One frame into `buf`.  The layer lock is attempted and never waited for: it
   is held for as long as a mouse button is down.  A screen with no public lock
   is read under LockIBase() and never layer-locked -- that pair deadlocks. */
static int fb_grab_frame(const FbGeometry *want, FbGeometry *now,
                         BOOL *palette_moved, UBYTE *out, ULONG out_cap,
                         LONG *encoded, UWORD ty0, UWORD ty1)
{
    struct Screen *sc;
    const UBYTE   *planes[FB_MAX_DEPTH];
    ULONG          ilock = 0;
    int            rc;
    BOOL           pub;
    BOOL           locked = FALSE;

    *palette_moved = FALSE;
    *encoded = -1L;                     /* nothing encoded this pass */

    /* A band after the first of a chunky pass never touches the screen: the
       fetch took the whole frame into fb_stage once and every band encodes
       that copy.  The planar path has no copy and holds the screen for each. */
    if (ty0 != 0 && RFB_FMT_IS_CHUNKY(want->format))
    {
        const UBYTE *stage = fb_stage;

        *now = *want;
        *encoded = fb_encode_planes(&stage, out, out_cap, ty0, ty1);
        return FB_GRAB_OK;
    }

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
        rc = fb_examine(sc, want, now, planes, palette_moved, pub);

        /* Only on a screen that is held, and see above for why.  Attempted and
           never waited for, because the lock is held for as long as a mouse
           button is down and this server has one task. */
        if (pub && rc == FB_GRAB_OK && !*palette_moved)
            locked = (BOOL)(AttemptSemaphore(&sc->LayerInfo.Lock) != 0);

        /* A card's screen is read only while a real lock is held: the fetch is
           a library call against the RastPort, and handing a driver one whose
           screen has just closed costs more than a bad frame. */
        if (rc == FB_GRAB_OK && !*palette_moved &&
            RFB_FMT_IS_CHUNKY(want->format) && !pub)
            rc = FB_GRAB_UNREADABLE;

        /* Equal wire geometry does not make two RTG bitmaps interchangeable:
           the readback route and the snapshot offscreen belong to the bitmap
           that was probed.  So that screen change is an attach barrier. */
        if (rc == FB_GRAB_OK && !*palette_moved &&
            RFB_FMT_IS_CHUNKY(want->format) && fb_rtg_ready &&
            !http_rtg_attached_to(sc->RastPort.BitMap))
            fb_rtg_ready = 0;

        /* Attach on the first frame of a bitmap rather than in
           fb_take_buffers(), because the probe is library calls against a
           screen and this is where one is held. */
        if (rc == FB_GRAB_OK && !*palette_moved &&
            RFB_FMT_IS_CHUNKY(want->format) &&
            !fb_rtg_ready)
        {
            fb_rtg_ready = (UBYTE)http_rtg_attach(sc->RastPort.BitMap,
                                                  &sc->RastPort,
                                                  want->width, want->height,
                                                  want->row_stride, fb_stage);
            if (fb_rtg_ready)
                fb_want_rtg = 1;
            else
                rc = FB_GRAB_UNREADABLE;
        }

        /* The fetch itself: whole contiguous rows into Fast RAM, with the
           screen still held.  Everything after this, the compare, the PackBits
           and the socket, runs on the copy. */
        if (rc == FB_GRAB_OK && !*palette_moved &&
            RFB_FMT_IS_CHUNKY(want->format))
        {
            /* Once a screen pass and not once a band.  The whole frame in
               contiguous rows is the shape a card reads back fastest, and
               the copy holds one moment of the screen for the whole pass. */
            if (http_rtg_read(sc->RastPort.BitMap, &sc->RastPort, fb_stage))
                planes[0] = fb_stage;
            else
                rc = FB_GRAB_UNREADABLE;
        }
    }
    else
    {
        /* It closed between being read out of FirstScreen and being looked at.
           Nothing this pass.  The next one resolves the front screen again,
           which by then is whatever is really in front. */
        rc = FB_GRAB_VANISHED;
    }

    if (!pub)
        UnlockIBase(ilock);

    if (rc == FB_GRAB_OK && !*palette_moved)
    {
        if (!locked)
            fb_torn++;

        WaitBlit();
        *encoded = fb_encode_planes(planes, out, out_cap, ty0, ty1);

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
    /* Before the staging buffer goes, because the probe was given that buffer
       as scratch and the offscreen snapshot bitmap is the module's own. */
    http_rtg_detach();
    fb_rtg_ready = 0;

    ami_free(fb_tx);
    ami_free(fb_scratch);
    ami_free(fb_shadow);
    ami_free(fb_stage);

    fb_tx = NULL;
    fb_scratch = NULL;
    fb_shadow = NULL;
    fb_stage = NULL;
    fb_stage_len = 0;
    fb_tx_cap = 0;
    fb_tx_len = 0;
    fb_tx_sent = 0;
    fb_shadow_len = 0;
    fb_scratch_len = 0;
}

/* Everything the session holds, sized from one geometry.  Called again when the
   screen changes shape under a live session, which is why it frees first. */
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
    /* One eight-bit plane or eight one-bit ones.  The depth above stays 8 on a
       card because it is what sizes the palette.  rfb_planes() is what says
       there is one plane. */
    fb_rg.format        = (rfb_u8)g->format;

    rfb_scroll_defaults(&fb_cfg);

    /* Straight off what the BitMap said.  The encoder walks the real bitplanes,
       so this has to be GetBitMapAttr(BMA_FLAGS) & BMF_INTERLEAVED and nothing
       inferred from the strides. */
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
       written backwards into it.  The frame is then contiguous and the send
       cursor starts wherever the header turned out to begin. */
    fb_tx_cap = worst + 10UL;

    fb_shadow  = (UBYTE *)ami_alloc(fb_shadow_len);
    fb_scratch = (UBYTE *)ami_alloc(fb_scratch_len);
    fb_tx      = (UBYTE *)ami_alloc(fb_tx_cap);

    /* One frame of the card, in Fast RAM.  Only on a card, because the chipset
       path has no copy of the screen and this would be 40 KB for nothing. */
    if (RFB_FMT_IS_CHUNKY(g->format))
    {
        fb_stage_len = (ULONG)g->row_bytes * (ULONG)g->height;
        fb_stage = (UBYTE *)ami_alloc(fb_stage_len);
        if (fb_stage == NULL)
        {
            fb_say3("not enough memory for a ", fb_stage_len / 1024UL,
                    " KB copy of the card's screen");
            fb_free_buffers();
            return FALSE;
        }
    }

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

    /* The shape is queued and the colours are not.  Zeroing the remembered
       palette is what makes the next grab report a change; queueing one here
       would send 3 << depth zeroes, which a viewer draws as a black screen. */
    memset(fb_pal, 0, sizeof(fb_pal));
    fb_want_geom = 1;
    fb_want_pal  = 0;

    return TRUE;
}

/* Forget the shadow, which is what `refresh` asks for.  The sequence number is
   not reset: it is what the receiver checks for gaps with. */
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

/* Queue one control frame.  There is room for exactly one, so nothing must
   overtake a close: a pong written over it would leave the viewer told the
   connection dropped rather than why. */
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

/* A close this end sends because the far end broke the framing.  It goes to
   fb_why as well, because fb_close_code is what the log keys off. */
static VOID fb_close_session(UWORD code)
{
    if (fb_closing)
        return;

    fb_say(http_ws_close_reason(code));

    fb_ctl_n      = (UWORD)http_ws_close_frame(fb_ctl, sizeof(fb_ctl), code,
                                               http_ws_close_reason(code));
    fb_ctl_at     = 0;
    fb_closing    = 1;
    fb_close_code = code;
}

/* A close carrying this module's own sentence rather than the codec's.
   `reason` is what the browser is told; the log is told fb_why, so nothing
   passed here overwrites fb_why. */
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
        fb_say("input.device did not open, so the console can show the "
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

/* One event into the input stream.  Synchronous: IND_WRITEEVENT hands the event
   to the input task and returns.  No lock is held across this. */
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
   screen coordinate rather than a delta, which is what a viewer has. */
static VOID fb_inject_pointer(rfb_s32 x, rfb_s32 y)
{
    memset(&fb_event, 0, sizeof(fb_event));
    fb_event.ie_Class     = IECLASS_POINTERPOS;
    fb_event.ie_Code      = IECODE_NOBUTTON;
    fb_event.ie_Qualifier = fb_buttons;
    /* Screen pixels in, mouse units out: ticks per pixel over ticks per unit.
       The screen's own origin goes in first, in the screen's pixels, because
       that is what Intuition scales it as. */
    fb_event.ie_X         = (WORD)(((x + (rfb_s32)fb_left) *
                                    (rfb_s32)fb_res_x) / (rfb_s32)fb_tick_x);
    fb_event.ie_Y         = (WORD)(((y + (rfb_s32)fb_top) *
                                    (rfb_s32)fb_res_y) / (rfb_s32)fb_tick_y);
    fb_write_event();
}

/* The buttons, as the difference between what is held now and what was held
   before.  A mask that arrives with two bits changed produces two events,
   because IECODE carries one button. */
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
        /* RELATIVEMOUSE with no movement.  Without it ie_X and ie_Y read as
           an absolute position and every click would jump to the corner. */
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

/* A refresh, honoured, coalesced or deferred.  The shadow goes and geom and pal
   are re-queued: a full frame is XOR against the shadow, so both ends need a
   known zero at the same point in the stream. */
static VOID fb_ask_resync(VOID)
{
    ULONG now = fb_ticks();

    /* One is already on its way, and a second changes only the timing. */
    if (fb_resync)
        return;

    /* Too soon after the last, so it is remembered rather than dropped.  A
       viewer asking constantly still re-syncs, but not every frame. */
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
        return;                     /* not a word of ours, and not an error */

    switch (ev.kind)
    {
    case RFB_IN_REFRESH:
        fb_ask_resync();
        break;

    case RFB_IN_POINTER:
        /* Position first: a press that arrives in the same word as a move is a
           press at that position, and the other order clicks where the pointer
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
        fb_say("the Amiga is rebooting");
        fb_close_saying(HTTP_WS_CLOSE_GOING, fb_why);
        break;

    case RFB_IN_WHEEL:
        /* Dropped, deliberately.  AmigaOS 3.1 has no wheel, and the rawkey
           codes a third-party driver invented for one would be acted on as
           keystrokes.  Read and refused rather than left to fail. */
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
        /* There is no inbound data stream on this socket.  A viewer sends
           input and asks for redraws, and both are text.  A binary frame is a
           client sending something this does not read. */
        break;

    case HTTP_WS_EV_PING:
        fb_control(HTTP_WS_EV_PONG, data, (ULONG)len);
        break;

    case HTTP_WS_EV_PONG:
        break;

    case HTTP_WS_EV_CLOSE:
    {
        UWORD code = HTTP_WS_CLOSE_NORMAL;

        /* The decoder has already validated a status that is present.  Echo
           it the way the terminal endpoint does, so a clean 1001 or an
           application close does not come back as an unrelated 1000. */
        if (len >= 2)
            code = (UWORD)(((UWORD)data[0] << 8) | (UWORD)data[1]);

        fb_close_session(code);
        break;
    }

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
    fb_open_screenless = FALSE;

    if (!fb_open_libraries())
    {
        fb_close_libraries();
        return FALSE;
    }

    /* No screen is not no, it is not yet.  Out of S:User-Startup the screen
       list is empty until LoadWB, so this comes up serving and remembers that
       it did; http_fb_start() reads the front screen again per session. */
    sc = fb_lock_front(&pub);
    if (sc == NULL)
    {
        fb_open_screenless = TRUE;
        fb_open_geom.width  = 0;
        fb_open_geom.height = 0;
        fb_open_geom.depth  = 0;

        if (!fb_input_open())
        {
            fb_input_close();
            fb_close_libraries();
            return FALSE;
        }

        fb_on = TRUE;
        return TRUE;
    }

    if (!pub)
        ilock = LockIBase(0);

    ok = (BOOL)(pub || fb_listed(sc));
    if (ok)
        ok = (BOOL)(fb_geometry_of(sc->RastPort.BitMap, &sc->ViewPort,
                                   &fb_open_geom, pub) == FB_GEOM_OK);
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

    /* And the input stream, held for the server's life beside the libraries: a
       machine that cannot be typed at says so before it is serving anything. */
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

/* The button state the far end is holding.  Set before the event that makes
   Intuition take the layer lock is written and cleared before the release is
   written, so it is never FALSE while Intuition holds it on our account. */
BOOL http_fb_buttons_held(VOID)
{
    return (BOOL)(fb_live && fb_buttons != 0);
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

    /* The geometry is read again rather than taken from startup.  A screen
       mode changed since then, or another screen come to the front, is the
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
        ok = (BOOL)(fb_geometry_of(sc->RastPort.BitMap, &sc->ViewPort, &g,
                                   pub) == FB_GEOM_OK);
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

    /* The colours, before the first grab.  fb_take_buffers() cleared the
       remembered palette, and without this the first `pal` word out is a black
       palette.  The screen is resolved again and skipped if it changed. */
    sc = fb_lock_front(&pub);
    if (sc != NULL)
    {
        if (!pub)
            ilock = LockIBase(0);

        if ((pub || fb_listed(sc)) &&
            fb_examine(sc, &g, &again, planes, &moved, pub) == FB_GRAB_OK)
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
    fb_frame_t0   = 0;
    fb_busy_ticks = 0;
    fb_idle_given = 0;
    fb_band_ty0   = 0;
    fb_pass_ticks = 0;
    fb_pass_acc   = 0;
    fb_frames     = 0;
    fb_bytes      = 0;
    fb_grab_ticks = 0;
    fb_encode_ticks = 0;
    fb_since_stat = 0;
    fb_torn       = 0;
    fb_nolock     = 0;
    fb_gone       = 0;
    fb_gone_at    = 0;
    fb_gone_passes = 0;
    fb_ptr_have   = 0;
    fb_want_ptr   = 0;
    fb_ptr_next   = 0;
    fb_want_stat  = 0;
    fb_want_rtg   = 0;
    fb_reset      = 0;
    /* A session opens with geom, pal and a full frame already queued, which
       is the sequence a resync produces.  A refresh arriving before that frame
       has gone must not restart it. */
    fb_resync      = 1;
    fb_resync_due  = 0;
    fb_resync_ever = 0;
    fb_resync_at   = 0;

    http_ws_reset(&fb_in);

    fb_live = TRUE;

    if (first_len > 0UL)
    {
        (VOID)http_ws_feed(&fb_in, first, (long)first_len, fb_sink, NULL);

        /* The bytes behind the upgrade are already WebSocket input and can
           fail framing.  Without this the decoder is dead but the screen
           producer keeps running, because no second read is needed. */
        if (fb_in.failed != 0)
            fb_close_session((UWORD)fb_in.failed);
    }

    return TRUE;
}

/* ACTION_FLUSH to every mounted volume, and then the machine goes.  The ports
   are collected under the DosList lock and the packets sent with it given
   back: DoPkt() waits, and the handler can want the DosList to answer. */
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

    /* Half a second for the close frame and the FIN to leave.  The viewer is
       told it is a reboot rather than left to call it a dropped connection,
       and that only works if the bytes get out first. */
    Delay(25);

    ColdReboot();
}

VOID http_fb_stop(VOID)
{
    if (!fb_live)
    {
        /* A start that failed half way frees its own buffers, but a shutdown
           must not depend on that having been the only path. */
        fb_free_buffers();
        return;
    }

    fb_live = FALSE;
    fb_sb   = NULL;
    fb_sock = -1;

    /* A viewer that goes away mid-drag leaves a button down, and the machine
       then behaves as if somebody held the mouse.  Released here, the only
       place that knows the far end has gone. */
    if (fb_buttons != 0)
        fb_inject_buttons(0);

    fb_free_buffers();

    /* Last, because this does not return.  Here rather than where the word was
       read, because the session had to end first, so the close frame saying
       why has already gone to the socket. */
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

/* One grab and one encode, or one control word, per pass of the server's loop.
   Nothing is produced while the transmit buffer still holds bytes, so a slow
   client cannot make this build a backlog. */
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

    /* Still draining, or a pong is waiting to overtake.  Either way there is
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
            fb_say("the geometry word did not fit");
            fb_close_saying(HTTP_WS_CLOSE_PROTOCOL, fb_why);
            return TRUE;
        }

        fb_frame_payload(HTTP_WS_EV_TEXT, (ULONG)len);
        fb_want_geom = 0;
        return TRUE;
    }

    /* A format with no palette never has one to send, and every place that
       raises the flag raises it without asking what the format is.  Dropped
       here, in the one place that would act on it. */
    if (fb_want_pal && fb_colours(&fb_geom) == 0UL)
        fb_want_pal = 0;

    if (fb_want_pal)
    {
        rfb_u32 len = rfb_word_pal((char *)&fb_tx[10], fb_tx_cap - 10UL,
                                   fb_pal, (rfb_u32)fb_colours(&fb_geom));

        if (len == 0UL)
        {
            fb_say("the palette word did not fit");
            fb_close_saying(HTTP_WS_CLOSE_PROTOCOL, fb_why);
            return TRUE;
        }

        fb_frame_payload(HTTP_WS_EV_TEXT, (ULONG)len);
        fb_want_pal = 0;
        return TRUE;
    }

    /* What the readback probe measured, once a screen.  Sent rather than
       kept, because nobody has published what an Amiga graphics card costs
       to read back.  An unrecognised word is ignored at the far end. */
    if (fb_want_rtg)
    {
        ULONG len = http_rtg_word((char *)&fb_tx[10], fb_tx_cap - 10UL);

        fb_want_rtg = 0;
        if (len != 0UL)
        {
            fb_frame_payload(HTTP_WS_EV_TEXT, len);
            return TRUE;
        }
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

        /* A pointer this cannot carry is dropped rather than fatal.  The
           viewer keeps whatever it had, which is its own arrow at worst, and
           nothing else about the session is affected. */
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
        static const char *const tags[8] = { "fbstat f=", " b=", " gt=", " et=",
                                            " tn=", " gn=", " nl=", " bt=" };
        const ULONG values[8] = { fb_frames, fb_bytes, fb_grab_ticks,
                                  fb_encode_ticks, fb_torn, fb_gone_passes,
                                  fb_nolock, fb_busy_ticks };
        ULONG at = 0;
        ULONG f;
        ULONG i;

        for (f = 0; f < 8UL; f++)
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

        /* A signed difference, so midnight is a wrap rather than a stall.  The
           clock goes back to zero once a day and a plain `<` would stop
           grabbing until the next day's ticks caught up. */
        if (fb_next_tick != 0UL && (LONG)(tick - fb_next_tick) < 0L)
            return TRUE;

        /* The floor stands only until the frame this pass produces has been
           sent and its real cost is known.  http_fb_write() replaces it. */
        fb_next_tick = tick + ((fb_band_ty0 == 0) ? (ULONG)FB_GRAB_FLOOR : 0UL);
        if (fb_next_tick == 0UL)
            fb_next_tick = 1UL;         /* 0 means it has never grabbed */

        /* The clock the duty cycle is charged against starts here, at the
           grab, and stops when the last byte of the frame has gone. */
        fb_frame_t0 = tick;
        if (fb_frame_t0 == 0UL)
            fb_frame_t0 = 1UL;          /* 0 means no frame is in flight */
    }

    /* gt= is the rest of the grab; et= is the pass that reads the planes. */
    {
        /* The band to produce this pass: FB_BAND_ROWS tile rows, so the work
           between two reads of the socket is bounded by a strip.  A grid
           shorter than one band is a single band covering all of it. */
        UWORD rows = (UWORD)((fb_pass_ticks >= (ULONG)FB_BAND_WHEN)
                             ? FB_BAND_ROWS : fb_enc.tiles_y);
        UWORD ty1 = (UWORD)(fb_band_ty0 + rows);

        if (ty1 > fb_enc.tiles_y)
            ty1 = (UWORD)fb_enc.tiles_y;

        fb_band_last = (UBYTE)(ty1 >= fb_enc.tiles_y);

        t0 = fb_ticks();
        rc = fb_grab_frame(&fb_geom, &seen, &palette_moved,
                           &fb_tx[10], fb_tx_cap - 10UL, &n,
                           fb_band_ty0, ty1);
        t1 = fb_ticks();

        /* Where the next one starts.  Anything but a clean band restarts the
           pass: half of one picture followed by half of another is worse
           than a repeated frame. */
        if (rc == FB_GRAB_OK && !palette_moved)
            fb_band_ty0 = fb_band_last ? 0 : ty1;
        else
            fb_band_ty0 = 0;
    }
    if (t1 >= t0)
        fb_encode_ticks += t1 - t0;

    switch (rc)
    {
    case FB_GRAB_OK:
        break;

    case FB_GRAB_GONE:
        /* No screen this pass, which is what a resolution change looks like.
           The session is not ended and the viewer is told nothing; it gets
           the geom that announces the screen that came back. */
        fb_gone_passes++;

        if (!fb_gone)
        {
            fb_gone    = 1;
            fb_gone_at = fb_ticks();
            return TRUE;
        }

        if ((LONG)(fb_ticks() - fb_gone_at) < (LONG)FB_GONE_GRACE)
            return TRUE;

        fb_say("there has been no screen to show for ten seconds");
        fb_close_saying(HTTP_WS_CLOSE_GOING, fb_why);
        return TRUE;

    case FB_GRAB_UNREADABLE:
        /* A card's screen that could not be read this pass.  Not fatal and
           not a geom: the viewer keeps the picture it has and the next pass
           tries again. */
        fb_nolock++;
        return TRUE;

    case FB_GRAB_VANISHED:
        /* The screen closed while it was being resolved.  Nothing this pass;
           the next one resolves whatever is in front now. */
        return TRUE;

    case FB_GRAB_CHANGED:
        /* The screen changed shape under a live session.  Everything the
           receiver has is about a screen it is not being shown, so the
           buffers are retaken and it is told again. */
        fb_gone = 0;

        if (!fb_take_buffers(&seen))
        {
            fb_close_saying(HTTP_WS_CLOSE_GOING,
                            "the new screen could not be read");
            return TRUE;
        }
        return TRUE;

    case FB_GRAB_REFUSED:
    default:
        fb_close_saying(HTTP_WS_CLOSE_GOING,
                        (fb_refuse_why != NULL)
                        ? fb_refuse_why
                        : "the front screen could not be read");
        return TRUE;
    }

    /* A screen came back into an empty list at the same shape, so nothing above
       fires and yet the shadow describes a picture that is not there.  The
       barrier is raised by hand, and the frame just encoded is dropped. */
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
        fb_say("the frame encoder did not encode this screen");
        fb_close_saying(HTTP_WS_CLOSE_PROTOCOL, fb_why);
        return TRUE;
    }

    fb_frame_payload(HTTP_WS_EV_BINARY, (ULONG)n);

    /* The frame the resync promised has gone, so the next refresh is a new
       question rather than a repeat of this one. */
    fb_resync = 0;

    /* Screens and not messages, so f= keeps meaning what it meant before the
       frame was broken into bands.  Bytes are every band's. */
    if (fb_band_last)
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
       says the socket buffer had room and nothing about whether anybody is
       still on the far end.  See http_ws_live_stale(). */
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

        /* The frame is out, so what it cost is known: everything from the
           grab to this byte.  Only a frame is charged -- a word queued by
           the slice leaves fb_frame_t0 at zero and passes through. */
        if (fb_frame_t0 != 0UL)
        {
            ULONG done = fb_ticks();
            ULONG cost = (done >= fb_frame_t0) ? (done - fb_frame_t0) : 0UL;
            int   pass_done = (fb_band_ty0 == 0);
            /* Against the running totals rather than against this band
               alone: a band costs a handful of ticks and dividing one by
               three rounds wrong either way.  What was granted is taken off. */
            ULONG owed;
            ULONG idle;

            fb_busy_ticks += cost;
            fb_pass_acc += cost;
            fb_frame_t0 = 0;

            /* At the end of a screen pass and not between its bands.
               Waiting after every band adds the wait to how long input
               takes to be acted on, with no long encode to interrupt. */
            if (!pass_done)
                return TRUE;

            /* What the pass that just ended cost, which is what decides
               whether the next one is banded.  See FB_BAND_WHEN. */
            fb_pass_ticks = fb_pass_acc;
            fb_pass_acc = 0;

            owed = fb_busy_ticks / (ULONG)FB_IDLE_DIVISOR;
            idle = (owed > fb_idle_given) ? (owed - fb_idle_given) : 0UL;
            fb_idle_given += idle;

            /* The floor is about not re-reading a screen nobody drew on,
               so it belongs between screen passes and not between the
               bands of one: mid-pass it would add a tick per band. */
            if (fb_band_ty0 == 0 && idle < (ULONG)FB_GRAB_FLOOR)
                idle = (ULONG)FB_GRAB_FLOOR;

            fb_next_tick = done + idle;
            if (fb_next_tick == 0UL)
                fb_next_tick = 1UL;
        }

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

/* A close frame can only go at a frame boundary, so what is already half out is
   pushed first, as far as the socket will take it in one go and no further.  If
   it will not all go, the close is not sent at all. */
VOID http_fb_evict(UWORD code)
{
    UBYTE         frame[HTTP_FB_CTL];
    unsigned long n;
    unsigned long at = 0;

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

    /* Best effort, still without waiting.  Keep consuming positive short
       writes: the caller closes the socket as soon as this returns, so there
       is no later write pass that can finish this control frame. */
    while (at < n)
    {
        LONG sent = tool_sock_send(fb_sb, fb_sock, &frame[at], (LONG)(n - at));

        if (sent <= 0)
            break;

        at += (unsigned long)sent;
    }
}

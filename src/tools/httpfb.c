/*
 * The frontmost screen down a WebSocket.  See httpfb.h for what this is and
 * what it deliberately is not.
 *
 * IntuitionBase->FirstScreen is what is in front, and that is what is served.
 * It used to be LockPubScreen("Workbench") and nothing else, which meant the
 * Palette and Overscan editors, each of which opens a screen of its own that
 * is not public, locked a remote viewer out: the screen opened in front on the
 * machine and the browser went on showing an unchanged Workbench with no way
 * to see or dismiss it.
 *
 * The whole front screen goes out at its own origin, with nothing behind it,
 * the way RTG works: on a graphics card the front screen owns the display and
 * screen dragging does not exist.  It is deliberately not a composite of a
 * dragged-down screen over what is behind it.  sc->LeftEdge and sc->TopEdge
 * are read for one purpose only, aiming injected pointer events at rows that
 * have been dragged away from the top of the view.  They do not move the
 * picture.
 *
 * open_libraries(), geometry_of(), read_palette() and grab_frame() are
 * src/tools/wbgrab.c's, lifted rather than rewritten.  That file is the grab
 * half of this and is already right about the things that are easy to get
 * wrong: BMF_STANDARD before Planes[] is read at all, an interleaved BitMap's
 * BytesPerRow spanning every plane, a ColorMap shorter than the screen's
 * depth, and the screen being re-examined on every grab because the one it
 * locked can differ from the one it was told about.
 *
 * There is one transmit buffer and it holds one WebSocket frame.  Nothing is
 * produced while it still has bytes in it, which is the whole of the flow
 * control in this direction, and is what a framebuffer needs rather than a
 * queue.  When the LAN cannot carry 25 frames a second, what gets dropped is
 * the frames that were never grabbed, rather than a backlog of pictures of
 * what the screen looked like a second ago.
 *
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

/*
 * The encoder's shipping configuration.  BASELINE is PackBits, XOR, the plane
 * mask and best-of.  COPYRECT is the scroll detector, and SCROLL_ADAPTIVE is
 * what keeps it from probing on a frame where nothing moved.  Measured on real
 * captures: 5 bytes for an idle frame, 272 for windows opening, 1412 for a
 * shell scrolling.
 *
 * RFB_F_INTERLEAVED is added at run time, in fb_take_buffers(), when the
 * screen's BitMap is interleaved.  It used to be impossible to need, because
 * the grab de-interleaved into a plane-major buffer and the encoder only ever
 * saw that.  The encoder now reads the bitplanes where they are, so the layout
 * it is told about has to be the layout they are in.  The flag is what lays
 * the shadow out to match, since the two are walked with one stride.
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
 * whose wait is two milliseconds.  It is a floor rather than a frame rate: the
 * point past which grabbing again cannot produce anything a viewer can see. */
#define FB_GRAB_FLOOR       1

/*
 * The share of the machine this may take, as the divisor of the idle owed
 * after a frame.
 *
 * A frame that cost T ticks is followed by at least T / FB_IDLE_DIVISOR of
 * doing nothing, so the console settles at T / (T + T/3), which is 75%.  The
 * machine has to stay usable for whatever its owner is doing while somebody
 * watches it, and on a 68030 a 640x480 frame is a fifth of a second of solid
 * work: without this the console takes everything it can get and the guest
 * belongs to the browser rather than to the person sitting at it.
 *
 * It is enforced against MEASURED cost and not against a frame rate, because
 * the cost is what varies -- an idle screen is a few milliseconds and a
 * scrolling one is hundreds -- and a fixed rate would either throttle the
 * cheap case for nothing or fail to cap the expensive one at all.
 *
 * Task priority is not the mechanism.  A lower priority yields to a task that
 * wants to run and caps nothing when the machine is otherwise idle, which is
 * exactly when a long encode still makes the pointer stutter.  This is a
 * duty cycle and it holds whether or not anything else is runnable.
 */
#define FB_IDLE_DIVISOR     3

/*
 * How often a `refresh` can force a full frame, in fiftieths.
 *
 * A refresh is expensive and asymmetric.  The answer is a whole screen, about
 * 7 KB at 640x256x4, against the 5 bytes an idle frame costs.  A viewer that
 * asks once, having lost sync or seen a sequence gap, gets one at once: the
 * floor only applies to the second and later ask inside a second.  A viewer
 * that asks on every frame degrades to one re-sync a second rather than
 * saturating the link.
 *
 * The number is one second because that is well above the grab rate and well
 * below anything a person notices as a stall in a picture that is already
 * correct.
 */
#define FB_RESYNC_FLOOR     50UL

/*
 * How long a run with no screen at all is allowed to last, in fiftieths.
 *
 * A resolution change closes the Workbench screen and opens a new one, and
 * between the two Intuition's screen list is empty.  With no other screen
 * open, no Shell window holding one and nothing else running, that is a real
 * moment with nothing to serve, and it used to end the session with "there are
 * no screens left" on a machine that was about to show a bigger screen.
 * Reproduced first time by copying a lace prefs file over
 * ENV:Sys/screenmode.prefs with the boot Shell ended.  A depth change does not
 * do it, because Intuition rebuilds the bitmap without closing the screen.
 *
 * So an empty list is a moment to wait through rather than an error.  Measured
 * over eight resolution changes and two overscan changes, the gap is one or two
 * passes of this loop, tens of milliseconds, and gn= counted 2 across two
 * reopens.  Ten seconds is three orders of magnitude beyond that, and it is the
 * same order as the session's own liveness timeout, so a viewer never waits
 * longer to find out the screen is gone for good than it waits to find out its
 * peer is dead.
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
    /*
     * What a pixel is, as one of the RFB_FMT_ values, and it is the field the
     * whole of the rest of this file branches on.
     *
     * RFB_FMT_PLANAR is the chipset: depth one-bit planes read where they
     * lie.  The two chunky ones are a graphics card, one plane of bytes,
     * where the bitplanes cannot be read where they are at all -- a staging
     * buffer in Fast RAM stands in for them and httprtg.c fills it, see
     * fb_grab_frame().  RFB_FMT_CLUT8 is a byte a pixel and a palette;
     * RFB_FMT_RGB565 is two bytes a pixel and no palette, which is what a 15,
     * 16, 24 or 32-bit card screen is converted to before it gets here.
     *
     * row_bytes is bytes either way, so everything downstream that counts in
     * bytes is unchanged by any of this.  Only three things ask: how many
     * planes there are, how wide a row is, and whether there is a palette to
     * send.
     */
    UWORD format;
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
static UBYTE           fb_word_over;    /* the message is longer than is read  */

/*
 * Why the last geometry was refused, for the close frame.
 *
 * httprtg.h promises that a 15, 16, 24 or 32-bit screen is refused by name,
 * and the sentence it hands back went to fb_say(), the server's own log, which
 * on a guest the harness starts with `Run >DH0:httpd.txt` is a file nobody
 * sees.  What the person watching got instead, when a 16-bit screen came to
 * the front of a live session, was the generic "the front screen is not one
 * this can read".  A string literal, so no copy.  Only the RTG refusals set
 * it, because they are the ones with a name to give.
 */
static const char     *fb_refuse_why;

static FbGeometry      fb_geom;
static rfb_geom        fb_rg;
static rfb_encoder     fb_enc;
static rfb_scroll_cfg  fb_cfg;
static rfb_u32         fb_flags;        /* FB_FLAGS, plus the layout's own */

static UBYTE          *fb_shadow;
static UBYTE          *fb_scratch;
static UBYTE          *fb_tx;
/*
 * Where a card's screen lands before it is encoded.
 *
 * The chipset path points the encoder straight at the bitplanes and lets it
 * read nothing where nothing changed, which is right when the source is chip
 * RAM.  On a card it cannot be done, because the compare would itself be the
 * readback and the readback is the expensive part.  So a card's screen is
 * fetched whole, once a frame, into this buffer in ordinary Fast RAM, and the
 * encoder is pointed at that, which leaves one read of VRAM a frame and puts
 * every comparison on memory that is cheap to read.
 */
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

/* httprtg.c has measured this screen's readback routes and picked one, and
   the `rtg` word saying what it measured is queued.  Cleared with the buffers,
   so a screen change re-probes rather than carrying a figure for a card
   configuration that is not the one in front. */
static UBYTE           fb_rtg_ready;
static UBYTE           fb_want_rtg;

static ULONG           fb_next_tick;

/*
 * The duty cycle, in ticks.
 *
 * fb_frame_t0 is when the work for the frame in flight began, which is the
 * grab and not the send: the cost this has to cap is everything the task does
 * on the console's account, and the send is part of it.  fb_busy_ticks is the
 * running total of that cost and is what fbstat reports, so the share taken
 * can be checked from outside rather than trusted.
 *
 * fb_frame_t0 is 0 when no frame is in flight, and a tick of 0 is therefore
 * nudged to 1 by its writer the way fb_next_tick's is.
 */
static ULONG           fb_frame_t0;
static ULONG           fb_busy_ticks;

/*
 * Which tile row the next band starts at, and 0 means the next pass begins a
 * fresh screen.
 *
 * A whole frame is one uninterruptible piece of work, and on a 68030 that is
 * around a fifth of a second in which this task reads no socket -- so a click
 * or a keystroke sent while a frame is being built is not looked at until the
 * frame is finished.  Producing a band at a time puts the server's read back
 * on the path between them, at the cost of five bytes of message header per
 * band and nothing else.
 *
 * The screen is re-resolved and its geometry re-checked on every band, so a
 * screen that changes shape half way through a pass is caught there rather
 * than producing bands of two different pictures.
 */
static UWORD           fb_band_ty0;

/* Whether the band just produced closed a screen pass.  Only that one counts
   a frame, so the frame counter keeps meaning screens and not messages. */
static UBYTE           fb_band_last;

/*
 * Tile rows in a band.
 *
 * A 640x480 screen at 16-row tiles is 30 tile rows, so four rows is eight
 * bands and bounds one uninterrupted encode at an eighth of a screen.  Small
 * enough to keep the socket answered on a 68030, large enough that the five
 * bytes of header a band costs stay lost in the noise -- an idle 640x480
 * screen measured 1840 bytes a pass whole and 1875 in eight bands.
 */
#ifndef FB_BAND_ROWS
#define FB_BAND_ROWS        4
#endif

/*
 * A resync is a sequence, and asking twice must not restart it.
 *
 * Honouring a refresh queues geom, then pal, then a full frame, and clears the
 * shadow so that frame is decodable from zero.  A second refresh arriving
 * inside that sequence has nothing to add, since the shadow is already zero
 * and the full frame is already coming, and acting on it would re-queue geom
 * and re-clear a shadow the viewer has not yet been given a frame from.  A
 * client that asks on every geom would then be answered with a geom, ask
 * again, and never get past the handshake.  No client here does that.  The
 * guard is on this side because this side answers every client.
 */
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

/*
 * The screen list was empty on the last pass, and when it first was.  A screen
 * being reopened in another resolution is the ordinary reason.  See
 * FB_GONE_GRACE.  `fb_gone_passes` is reported in `fbstat` as gn=, so that a
 * report of connections being lost has a number behind it.
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
 * The event is static rather than automatic for the reason every buffer here
 * is: a Shell command has 4 KB of stack on a stock Kickstart 3.1, and this one
 * is also what io_Data points at while DoIO() runs.
 */
static struct MsgPort   *fb_in_port;
static struct IOStdReq  *fb_in_req;
static BOOL              fb_in_open;
static struct InputEvent fb_event;

/*
 * ie_X and ie_Y are not measured in screen pixels.
 *
 * IECLASS_POINTERPOS carries a position in mouse units, and Intuition turns
 * one into a pixel with two numbers that both come out of the display
 * database.  It multiplies ie_X by the monitor's ticks-per-mouse-unit and
 * divides by the screen mode's ticks-per-pixel.  A tick is 1/44 of a lores
 * pixel on a 15 kHz monitor.  A hires pixel is 22 of them, superhires 11, and
 * a PAL row is 44 non-interlaced or 22 interlaced.
 *
 * The mode bits cannot answer this.  A Multiscan Productivity screen is
 * 640x480 with the SUPERHIRES and LACE bits both set, which is how the chipset
 * makes it, and its pixels are 22 ticks wide and 22 high, exactly a PAL
 * hires-interlaced pixel.  Reading the bits gave it half the width and twice
 * the height, which is a pointer that reaches the middle of the screen at the
 * right edge of the browser and is pinned to the bottom for the lower half of
 * it.  Measured, on Multiscan:ProductivityLace: sending (320,480) put the
 * pointer at (160,959).  Every doublescan, productivity and multisync mode is
 * that shape, which is what a reporter on a 1024x768 Super-High Res Laced
 * screen was seeing.
 *
 * So they are read from the database instead, per mode, and kept here because
 * the injectors have a word from a viewer and no screen in hand.  A screen
 * that changes mode under a live session changes them on the next grab.
 */
static UWORD             fb_res_x  = 22;    /* ticks per screen pixel        */
static UWORD             fb_res_y  = 44;
static UWORD             fb_tick_x = 22;    /* ticks per POINTERPOS unit     */
static UWORD             fb_tick_y = 22;
static UWORD             fb_spr_x  = 44;    /* ticks per sprite pixel        */
static UWORD             fb_spr_y  = 44;
static UWORD             fb_pixel_ns = 70;  /* what one screen pixel lasts   */
static ULONG             fb_mode_id  = INVALID_ID;

/*
 * Where the front screen sits in the view, in its own pixels.
 *
 * Zero for every screen nobody has dragged, which is every screen this
 * normally sees.  The picture is sent whole at the screen's own origin
 * whatever these are, because the front screen owns the display the way it
 * does on RTG, so they change nothing about what is drawn.  They exist because
 * an injected IECLASS_POINTERPOS is a position in the view, and a screen
 * dragged down by fifty rows has its row 0 fifty rows into the view.  Without
 * this the picture would be right and every click in it fifty rows high.  The
 * unit is the screen's own pixels, because that is the unit Intuition scales
 * them in.
 */
static WORD              fb_left;
static WORD              fb_top;

/* What the far end is holding down, as IEQUALIFIER_ bits.  Intuition reads the
   button state off the qualifier of every RAWMOUSE event rather than off a
   history of the codes, so a button that goes down and is not carried in the
   qualifier of what follows reads as released. */
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
   sentence for a person, and the `fbstat` word.  No printf, because this
   module has to build the same message into a close frame and a Shell command
   has 4 KB of stack to do it on. */
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

    /* Neither is required and neither is an error: a machine with no graphics
       card has neither, and one with a card has whichever its driver installed.
       What this decides is only whether an RTG screen in front can be read at
       all -- see fb_geometry_of(). */
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

/*
 * TRUE when `sc` is still one of Intuition's screens.  The caller must hold
 * LockIBase().  CloseScreen() takes IntuitionBase to unlink a screen before it
 * frees it, so a screen found in the list under that lock cannot be freed for
 * as long as the lock is held.
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
 * The frontmost screen, locked when a lock is available.
 *
 * IntuitionBase->FirstScreen is a field rather than a function, so it is read
 * under LockIBase() and the lock is dropped at once.  Intuition is blocked for
 * as long as it is held and this runs many times a second.
 *
 * A real lock is taken whenever one exists.  A public screen can be held by
 * name for the whole of a grab, and the screen then cannot close underneath,
 * which is the guarantee the Workbench-only version had for the case that is
 * nearly always in front.  Its name comes off the public screen list rather
 * than being assumed to be "Workbench", so any public screen in front gets the
 * same treatment.
 *
 * A screen that is not public has no such handle in 3.1.  The Palette and
 * Overscan editors open one each, which is why LockPubScreen() could not reach
 * them.  Such a screen comes back unlocked and fb_grab_frame() reads it under
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
 * correctly, so none of them must fall through to a grab of something else.
 */
static BOOL fb_geometry_of(struct BitMap *bm, FbGeometry *g, BOOL may_ask_rtg)
{
    ULONG flags;
    ULONG depth;
    ULONG width;
    ULONG height;
    ULONG stride;
    UWORD plane;

    /* Cleared here, so a refusal from an earlier screen is never the sentence
       a later one closes with. */
    fb_refuse_why = NULL;

    if (bm == NULL)
    {
        fb_say("the front screen has no bitmap");
        return FALSE;
    }

    /*
     * The card is asked before BMF_STANDARD is read.  The planar path's first
     * question has always been whether the bitmap carries BMF_STANDARD, and on
     * a chipset machine that is the right one.  A Picasso96 or CyberGraphX
     * bitmap can carry it too, since that flag is what makes the rest of the
     * OS treat the bitmap normally, and its Planes[] are not eight bitplanes.
     * So whoever owns the bitmap is asked first.  http_rtg_owns() answers
     * FALSE for every bitmap that is not a card's, and on a machine with
     * neither library open it does not run at all, so the chipset path reaches
     * BMF_STANDARD exactly as it did.
     *
     * `may_ask_rtg` is why the question is not always asked.  The two library
     * calls behind it are documented as ownership queries that need no lock,
     * and every program that touches a card makes them freely, but they are
     * still calls into Picasso96, and this file has one caller that runs under
     * LockIBase(): the pass that resolves a screen nothing can lock.  Whether
     * p96GetBitMapAttr() can take a semaphore is not settled by the autodoc,
     * and a global scan of the binary does not settle it either, so it is not
     * relied on.
     *
     * Nothing is lost by not asking there.  A card's screen is read only while
     * a real screen lock is held, see fb_grab_frame(), so the answer for a
     * screen that offers none is discarded anyway.  What is left is the
     * behaviour the planar path has always had for a bitmap it cannot
     * identify.
     */
    if (may_ask_rtg && http_rtg_owns(bm))
    {
        HttpRtgScreen rs;
        const char   *why = NULL;

        if (!http_rtg_describe(bm, &rs, &why))
        {
            fb_refuse_why = (why != NULL) ? why
                                          : "the front screen is an RTG screen "
                                            "this cannot read";
            fb_say(fb_refuse_why);
            return FALSE;
        }

        /*
         * The staging buffer's row, and therefore the tile grid's.  The card's
         * own stride is not used, so nothing here depends on a value that is
         * only valid while the bitmap is locked.  Rounded up to a longword so
         * the encoder's word-at-a-time compare stays on the fast path.
         *
         * rs.bpp is what httprtg.c will deliver into that buffer, a byte a
         * pixel for a palette screen and two for a truecolour one, and it is
         * the only thing that differs between the two here.  The depth
         * follows it rather than the card's own: a 24 or 32-bit screen
         * arrives as RGB565 and 16 is what the receiver is told, because 16
         * is what the bytes are.
         */
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
        return TRUE;
    }

    flags  = GetBitMapAttr(bm, BMA_FLAGS);
    depth  = GetBitMapAttr(bm, BMA_DEPTH);
    width  = GetBitMapAttr(bm, BMA_WIDTH);
    height = GetBitMapAttr(bm, BMA_HEIGHT);

    if ((flags & BMF_STANDARD) == 0)
    {
        fb_say(http_rtg_present()
               ? "the front screen is not a standard planar bitmap and "
                 "neither Picasso96 nor CyberGraphX claims it, so there are "
                 "no pixels here anything can read"
               : "the front screen is not a standard planar bitmap, so it "
                 "has no bitplanes to read. A graphics card needs "
                 "Picasso96API.library or cybergraphics.library, and neither "
                 "answered OpenLibrary()");
        return FALSE;
    }

    g->format = RFB_FMT_PLANAR;

    if (depth < 1 || depth > FB_MAX_DEPTH)
    {
        fb_say3("the front screen is ", depth,
                " planes deep. This handles 1 to 8");
        return FALSE;
    }

    if (width < 1 || width > 65535 || height < 1 || height > 65535)
    {
        fb_say("the front screen does not fit the wire format");
        return FALSE;
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
                  a->interleaved == b->interleaved &&
                  a->format == b->format);
}

VOID http_fb_geometry(UWORD *w, UWORD *h, UWORD *depth)
{
    if (w != NULL)     *w = fb_open_geom.width;
    if (h != NULL)     *h = fb_open_geom.height;
    if (depth != NULL) *depth = fb_open_geom.depth;
}

/* ---------------------------------------------------------------- palette -- */

/*
 * 3 * (1 << depth) bytes whatever the ColorMap holds.  A screen whose map is
 * shorter than its depth leaves the tail black rather than shortening the
 * word, so the receiver's arithmetic is the depth and nothing else.
 *
 * TRUE when it changed, which is what decides whether a `pal` word goes out.
 */
/*
 * How many colours this screen's `pal` word carries, and 0 when it has none.
 *
 * Asked of the wire format rather than computed here, because 1 << depth is
 * right on a plain planar screen and on nothing else: a truecolour screen is
 * 16 deep with no palette at all, and the chipset modes coming after it are
 * deeper than their base palette rather than equal to it.  One rule, in
 * rfb_pal_colours(), and both ends read it.
 */
static ULONG fb_colours(const FbGeometry *g)
{
    rfb_geom q;

    memset(&q, 0, sizeof(q));
    q.depth  = (rfb_u8)g->depth;
    q.format = (rfb_u8)g->format;

    return (ULONG)rfb_pal_colours(&q);
}

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

/*
 * The mouse pointer is a sprite, so it is in no frame.
 *
 * Every frame this sends is the screen's bitplanes.  The pointer is drawn by
 * the hardware from a sprite that is not in them, so a viewer that showed only
 * what arrives has no pointer at all.  That is why the browser drew an arrow of
 * its own, at the local mouse, and why that arrow was never the Amiga's.
 *
 * The image is sent as its own word, once, and again only if it changes.  The
 * position is not sent at all.  The viewer puts the Amiga's pointer where the
 * browser's is, so the browser already knows where it is, and drawing it
 * locally is what keeps it moving without a round trip.
 *
 * The image cannot come from the pointer on screen.  There is no way to read
 * the pointer that is being displayed.  struct Window's Pointer, PtrWidth and
 * PtrHeight are filled by SetPointer() and are not touched by
 * SetWindowPointer(), so a window that used the newer call leaves them stale
 * from an earlier SetPointer() or zero, and 3.1's Workbench, the busy pointer
 * and every pointer wider than 16 pixels all use the newer call.  Reading them
 * as sprite data reads memory somebody else freed.  The pointerclass object
 * that does hold the imagery answers no OM_GET, so there is nothing to ask it
 * either.
 *
 * So this reads what the pointer was configured to be, which is a file:
 * ENV:Sys/pointer.prefs, an IFF PREF with a PNTR chunk, written by the Pointer
 * editor and read by IPrefs.  A machine that has never had one saved has no
 * such file, and the pointer there came from devs:system-configuration
 * instead.  GetPrefs() hands that over as PointerMatrix, a 16x16 two-plane
 * sprite with its hotspot and its three colours.  Both are read, and the file
 * wins when it is there, because it is the newer answer.
 *
 * The busy pointer cannot be shown, nor any pointer an application set for
 * itself.  Both live in pointerclass objects that cannot be read back, so the
 * browser goes on showing the configured arrow while the machine shows a
 * stopwatch.  That disagreement lands during a long operation.  The file has a
 * second PNTR chunk carrying the busy pointer's image, with pp_Which set to
 * WBP_BUSY, and it is deliberately not read: the picture is no use without
 * knowing when it is up, and nothing here can know that.
 */

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

/*
 * How often the preferences are re-read, in fiftieths.
 *
 * IPrefs rewrites the file when somebody changes the configuration, so
 * noticing means looking at the file again, and looking means a Lock, an
 * Examine and an UnLock on ENV:, which is RAM: and therefore fast but is still
 * three DOS calls on the one task that also serves every connection.  Once a
 * second is far more often than a person changes their pointer and is a
 * rounding error against the twenty grabs a second beside it.
 */
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

/*
 * The four numbers the display database has and the mode bits do not.
 *
 * A tick is graphics.library's unit of displayed distance, 1/44 of a lores
 * pixel across and 1/44 of a PAL row down, and the database gives three
 * lengths in it per mode: what a screen pixel is worth (DisplayInfo's
 * Resolution), what a sprite pixel is worth (SpriteResolution), and what one
 * unit of the mouse is worth (MonitorInfo's MouseTicks, per monitor rather
 * than per mode).  Everything below is a ratio of two of those.
 *
 * Re-read only when the mode changes.  The position of the screen in the view
 * is re-read every time, because a screen can be dragged without changing
 * mode, and an injected position that ignored that would be right in the
 * picture and wrong on the machine.
 *
 * A monitor whose file predates 3.01 has no MouseTicks, and Intuition then
 * uses 22 across and 22 down, 26 down on an NTSC-rate monitor.  The same
 * fallback is used here, or the pointer would be scaled by one number and read
 * back by another.
 */
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

    /* The two halves of the position have to come from the same place.  A
       monitor's ticks against a mode's guessed pixels is a ratio of two
       different things, so a mode with no record of its own keeps the pair
       that was there before. */
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

/*
 * How many screen pixels one sprite pixel covers.
 *
 * A sprite pixel is not always a lores pixel.  That assumption would put the
 * pointer at half width on a superhires screen.  It is not always two screen
 * rows on an interlaced screen either: a Productivity screen carries the LACE
 * bit and its sprite rows are one row each.  Both ratios come off the
 * database, in ticks.
 *
 * The one thing the database cannot answer is a sprite whose resolution was
 * changed, by the Pointer editor's preference or by an application.  That
 * lives in the V39 ColorMap, as SpriteResolution or SpriteResDefault under it,
 * and it is in nanoseconds rather than ticks, so it divides the screen's own
 * pixel speed instead.  This is Intuition's own arithmetic for the same
 * question.
 */
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

/*
 * The sprite's own colours, which are not the screen's first four.
 *
 * The pointer is sprite 0 and its three pens are colour registers 17, 18 and
 * 19, with 16 transparent.  Those registers carry the live values, because a
 * program that recoloured the pointer moved them and moved nothing in any
 * prefs file, so they are preferred.  The configured colours are the fallback
 * for a ColorMap too short to hold them.
 *
 * Only at depth 2.  A deeper sprite is an attached pair and which registers it
 * lands on cannot be worked out here, so those keep the colours the file
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
 * PointerMatrix is (1 + 16 + 1) word pairs: a control pair, sixteen rows of
 * one word per plane, and a terminating pair.  The rows are interleaved and
 * everything downstream of here is plane-major, so this is where they are
 * separated.
 */
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

/*
 * ENV:Sys/pointer.prefs, if it is there.  An IFF FORM PREF holding one PNTR
 * chunk per pointer.  The one wanted is pp_Which == WBP_NORMAL, and the busy
 * one is stepped over for the reason in the block at the top of this section.
 *
 * The whole file is read and then walked, rather than seeking around it.  It
 * is a few hundred bytes on RAM:, and one Read is one DOS call instead of a
 * dozen.
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

/*
 * The hotspot, kept inside the sprite.
 *
 * devs:system-configuration's stock arrow carries XOffset -1 with its tip in
 * column 0.  The field has two readings, that the hotspot is at (XOffset,
 * YOffset) in the sprite, or that the sprite is drawn at the mouse plus
 * (XOffset, YOffset), and they put the tip one lores pixel either side of the
 * mouse.  Neither can be settled from here, because the sprite is not in any
 * bitmap this can see and there is nothing to measure the drawn position
 * against.
 *
 * A hotspot outside the image is not a hotspot for anything this draws, so it
 * is clamped in.  For the stock arrow that lands it on (0,0), which is the tip
 * an arrow points from.
 *
 * A wrong hotspot costs nothing, because a click goes to the coordinate the
 * viewer sends rather than to where the image was drawn.  The worst it
 * produces is the picture of the pointer sitting one lores pixel off.
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
 * editor writes.  devs:system-configuration when there is no file, which is a
 * machine nobody has changed the pointer on.
 *
 * No screen is touched here and nothing is locked, because this opens a file
 * and a DOS call must not be made with Intuition stopped.  The scale and the
 * live sprite colours are added afterwards, in fb_pointer_poll(), which is
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
 * Once a second rather than once a pass, because this opens a file and three
 * DOS calls twenty times a second on the task that also answers every
 * connection is a cost with nothing to show for it.
 *
 * The file is read with nothing held.  The screen is resolved afterwards and
 * only the scale and the sprite's live colours come out of it, which is a
 * short lock rather than one held across DOS.
 */
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

/*
 * The screen is not copied.  The encoder reads the bitplanes where they are.
 *
 * There used to be a frame_bytes buffer here and a CopyMem per 80-byte row
 * into it, and then the encoder read that copy: 40 KB of chip RAM read, 40 KB
 * written, 40 KB read again, to answer whether anything changed, which on a
 * live Workbench is no 49 frames out of 50.  Measured on an A1200, two planes:
 * the copy 14.9 ms and the encode 56.9 ms, against 13.6 ms for one pass that
 * compares the planes against the shadow and stops.
 *
 * This reads the planes without holding a drawing lock, and the encoder
 * answers that by reading a tile it decides has changed once, into a buffer,
 * so that one copy is both what updates the shadow and what goes on the wire.
 * A screen being drawn on underneath can therefore produce a torn frame, as it
 * always could, but it cannot produce a shadow here that disagrees with the
 * bytes the far end was sent.  That failure would not correct itself on the
 * next frame.
 */
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

/*
 * Everything that dereferences the Screen, its BitMap or its ColorMap.  The
 * caller holds either a public screen lock or LockIBase(), so this must not
 * block and must not call Intuition.  It does neither: GetBitMapAttr() and
 * GetRGB32() are reads of structures the screen already owns, and
 * GetVPModeID() and GetDisplayInfoData() are reads of the display database,
 * which is the order Intuition takes them in itself.
 *
 * What leaves here is the geometry, the palette, the display units and the
 * bitplane addresses.  The encode that follows touches nothing but those
 * addresses, which is what lets it run with the lock given back.
 */
static int fb_examine(struct Screen *sc, const FbGeometry *want,
                      FbGeometry *now, const UBYTE **planes,
                      BOOL *palette_moved, BOOL locked)
{
    UWORD plane;

    if (!fb_geometry_of(sc->RastPort.BitMap, now, locked))
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

    /* A card's pixels are not addressable from here and the fetch is a
       library call, so they are read in fb_grab_frame() where the lock that
       makes the call safe is still held.  planes[0] is the staging buffer and
       is filled there. */
    if (RFB_FMT_IS_CHUNKY(want->format))
        return FB_GRAB_OK;

    for (plane = 0; plane < want->depth; plane++)
        planes[plane] = (const UBYTE *)sc->RastPort.BitMap->Planes[plane];

    return FB_GRAB_OK;
}

/*
 * One frame into `buf`, and the palette.  Everything downstream, the encode
 * and the socket, runs with nothing held.
 *
 * The layer lock is attempted, never waited for, and not required.
 * LockLayers() was here and it wedged the whole server.  The lock it takes is
 * sc->LayerInfo.Lock, and it is held for as long as a mouse button is down.
 * Intuition holds it while a menu is up or a window is being dragged or sized,
 * and the Workbench task holds it through an icon drag or a rubber-band
 * selection.  Those last as long as a person holds a button, and httpd serves
 * every connection from one task, so for that whole time nothing was answered,
 * not the console and not plain HTTP.  A guest was found forty minutes into
 * exactly that, with ss_Owner the Workbench task and the one SemaphoreRequest
 * queued on it httpd's own stack.
 *
 * Waiting is out, and so is giving up.  A grab that returned empty-handed
 * whenever the lock was busy would freeze the picture for the whole of every
 * drag, so the console would stop moving at the moment there is something to
 * watch.  So the lock is attempted, and the frame is read either way.
 *
 * Reading it unlocked is safe.  The planes are memory, the lock serialises the
 * tasks drawing into them rather than the reading of them, and the screen lock
 * above is what keeps the screen and its bitmap in existence.  The cost is that
 * a frame read while somebody is drawing can carry half of a change, and the
 * encoder diffs the next grab against what it sent, so the next frame that
 * differs puts it right.  A torn frame for two milliseconds is the price of a
 * console that keeps up during a drag and a server that never stops.
 *
 * A screen nothing can lock is kept in existence a different way.  A public
 * screen is held for the whole of this and cannot close.  The Palette and
 * Overscan editors' screens are not public and 3.1 has no handle for one, so
 * the guarantee is shorter: everything read out of the Screen is read under
 * LockIBase() with the screen checked as still listed, which is enough because
 * CloseScreen() has to take IntuitionBase to unlink it before it frees
 * anything.
 *
 * What is left outside that is the encode, which reads the bitplane addresses
 * and nothing else.  A screen that closes in that window leaves those pointing
 * at freed chip RAM, which on a machine with no MMU is bytes, so the worst it
 * produces is one wrong frame.  The pass after it resolves the front screen
 * again and the geom barrier corrects the viewer.  Making that window zero
 * would mean holding LockIBase() across a 15 ms encode ten times a second,
 * which is Intuition stopped for a sixth of the time.
 *
 * Such a screen is also read without the layer lock, counted in tn=.  Taking
 * it would mean releasing it after the encode, and the only way to know the
 * screen is still there to release it on is LockIBase(), while holding a layer
 * lock.  That is a deadlock, and it was measured: Intuition holds
 * IntuitionBase and wants the layer lock, this holds the layer lock and wants
 * IntuitionBase.  The guest stopped dead, the whole GUI frozen with a gadget
 * stuck in its selected state and httpd answering nothing, on the click that
 * was closing an Overscan editor screen.
 */
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

        /*
         * A card's screen is read only while a real lock is held.
         *
         * The planar path deliberately reads the bitplanes with nothing but
         * LockIBase() behind it on a screen that cannot be locked, and a
         * screen closing in that window costs one wrong frame.  The planes are
         * bytes, and reading freed memory on a machine with no MMU produces a
         * bad picture and nothing worse.  Here the fetch is a library call
         * against the screen's RastPort, and handing a graphics driver a
         * RastPort whose screen has just closed costs more than one bad frame.
         *
         * So an RTG screen that offers no lock is not read.  The picture stops
         * until something lockable is in front again and the count comes out
         * in fbstat as nl=.  That is a real limitation.  A non-public screen
         * on a card is rare, and a frozen picture is recoverable where a guru
         * is not.
         */
        if (rc == FB_GRAB_OK && !*palette_moved &&
            RFB_FMT_IS_CHUNKY(want->format) && !pub)
            rc = FB_GRAB_UNREADABLE;

        /* Attach on the first frame of a geometry rather than in
           fb_take_buffers(), because the probe is library calls against a
           screen and this is where one is held.  It also has to happen again
           after a screen change, which is what fb_rtg_ready being cleared with
           the buffers arranges. */
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
            /*
             * Once a screen pass and not once a band.  The fetch is the whole
             * frame in contiguous rows because that is the shape a card reads
             * back fastest -- see httprtg.c, where a loop of small rectangles
             * is measured as the wrong shape by an order of magnitude -- so
             * the bands that follow encode the copy this one took rather than
             * going back to the board four more times.
             *
             * The staging buffer therefore holds one moment of the screen for
             * the whole pass, which is if anything better than re-reading:
             * the bands cannot disagree with each other about what the screen
             * was.
             */
            if (ty0 != 0)
                planes[0] = fb_stage;
            else if (http_rtg_read(sc->RastPort.BitMap, &sc->RastPort,
                                   fb_stage))
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

/*
 * Everything the session holds, sized from one geometry.  Called again when
 * the screen changes shape under a live session, which is why it frees first.
 * A 640x256x4 screen becoming 640x480x8 needs a different four buffers, and
 * keeping the old ones would read a frame into two thirds of one.
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
    /* One eight-bit plane or eight one-bit ones.  The depth above stays 8 on a
       card because it is what sizes the palette.  rfb_planes() is what says
       there is one plane. */
    fb_rg.format        = (rfb_u8)g->format;

    rfb_scroll_defaults(&fb_cfg);

    /* Straight off what the BitMap said, rather than inferred from the
       strides.  The encoder walks the real bitplanes, so this flag has to be
       the screen's own answer, GetBitMapAttr(BMA_FLAGS) & BMF_INTERLEAVED, in
       fb_geometry_of() and nowhere else. */
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

    /*
     * The shape is queued and the colours are not.  Zeroing the remembered
     * palette is what makes the next grab's comparison report a change, and
     * that is where the `pal` word comes from.  Queueing one here would send
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
 * Forget the shadow.  This is what `refresh` asks for.  The receiver has lost
 * a frame, and every XOR after it would be applied to bytes that are not what
 * the encoder thought were there, so the next frame has to be a full one.
 *
 * The sequence number is not reset.  It is what the receiver checks for gaps
 * with, and taking it back to zero would look like the fault this is
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
 * Queue one control frame.  There is room for exactly one, so nothing must
 * overtake a close.  A browser sends a ping on a timer, so one arriving after
 * the session has decided to end is ordinary, and answering it would write a
 * pong over the close frame.  http_fb_write() would then see fb_closing and
 * shut the socket with the reason still in the buffer, so the viewer would be
 * told the connection dropped rather than why.
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

/* A close carrying this module's own sentence rather than the codec's, for the
   refusals a person has to be able to read: a screen that went away, a mode
   change this could not follow. */
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

/*
 * One event into the input stream.
 *
 * Synchronous.  IND_WRITEEVENT hands the event to the input task and returns.
 * It is not a transfer and there is nothing to wait for the far end of, so the
 * asynchronous form would only add a second trip through the loop for a call
 * that has already finished.  Nothing is locked here, since input is read in
 * http_fb_read() and the screen is locked only inside the grab, so no lock is
 * held across this.
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
    /* Screen pixels in, mouse units out: ticks per pixel over ticks per unit,
       see fb_display_units().  The screen's own origin goes in first, which is
       zero unless somebody dragged it, and is in the screen's pixels because
       that is what Intuition scales it as. */
    fb_event.ie_X         = (WORD)(((x + (rfb_s32)fb_left) *
                                    (rfb_s32)fb_res_x) / (rfb_s32)fb_tick_x);
    fb_event.ie_Y         = (WORD)(((y + (rfb_s32)fb_top) *
                                    (rfb_s32)fb_res_y) / (rfb_s32)fb_tick_y);
    fb_write_event();
}

/*
 * The buttons, as the difference between what is held now and what was held
 * before.  A viewer sends the whole mask on every move, so a press and a
 * release both arrive as a changed bit rather than as events of their own.  A
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
 * The shadow goes and the viewer is told, because a full frame is not
 * distinguishable from an ordinary one and the tiles in it are XOR against the
 * shadow.  A viewer that still has the last picture when one arrives XORs the
 * two together and gets the all-zero screen, which is grey on a stock
 * Workbench, and it stays grey because an idle desktop produces no more tiles
 * to correct it.  geom is the barrier that already exists for that, since
 * everything the viewer had is discarded when one arrives, so re-queuing geom
 * and pal puts a known zero on both sides at the same point in the stream.
 */
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
               "serves the frontmost screen, so a machine with no screen "
               "open has nothing to serve");
        fb_close_libraries();
        return FALSE;
    }

    if (!pub)
        ilock = LockIBase(0);

    ok = (BOOL)(pub || fb_listed(sc));
    if (ok)
        ok = fb_geometry_of(sc->RastPort.BitMap, &fb_open_geom, pub);
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
     * not answer the mouse.
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

/*
 * The button state the far end is holding, as a yes or no.  Set before the
 * event that makes Intuition take the layer lock is written, see
 * fb_inject_buttons(), and cleared before the release is written, so it is
 * never FALSE while Intuition is holding the lock on this server's account.
 */
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
        ok = fb_geometry_of(sc->RastPort.BitMap, &g, pub);
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
     * without this the first `pal` word out is 3 << depth zeroes, a black
     * palette, which is what a viewer draws until the second one arrives a
     * frame later.
     *
     * The front screen is resolved a second time rather than kept from above,
     * and the read is skipped if anything about it has changed in between.
     * That leaves fb_pal at zero, the first grab finds the colours moved, and
     * the viewer gets them one frame late rather than getting a screen's
     * geometry with another screen's palette.
     */
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
    fb_band_ty0   = 0;
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
    /* A session opens with geom, pal and a full frame already queued, which is
       the same sequence a resync produces.  A refresh arriving before that
       frame has gone, which is what a viewer asking on `geom` produces, must
       not restart it. */
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
 * ColdReboot() on its own can lose work.  AmigaDOS filesystems hold dirty
 * buffers for about half a second, and httpd is a WebDAV server, so a file
 * written a moment ago can be one of them.  ACTION_FLUSH is the packet that
 * empties them and it is what a handler answers when a person clicks the
 * disk's icon and chooses to flush.
 *
 * The ports are collected under the DosList lock and the packets sent with it
 * given back.  DoPkt() waits for the handler to answer, the handler can want
 * the DosList to do so, and a caller that held it across the packet would
 * deadlock.
 *
 * A program holding unsaved work in memory loses it.  There is no Amiga
 * convention for asking every task to save, and NetShutdown's CTRL_C is about
 * network resources rather than documents.  The viewer's dialog says so before
 * it sends the word.
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
       then behaves as if somebody were holding the mouse: menus stay up and
       nothing else can be clicked.  Released here, which is the only place
       that knows the far end has gone. */
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

/*
 * One grab and one encode, or one control word, per pass of the server's loop.
 *
 * Nothing is produced while the transmit buffer still holds bytes, so a slow
 * client cannot make this build a backlog.  Nothing is grabbed twice inside
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
            fb_close_saying(HTTP_WS_CLOSE_PROTOCOL,
                            "the geometry word did not fit");
            return TRUE;
        }

        fb_frame_payload(HTTP_WS_EV_TEXT, (ULONG)len);
        fb_want_geom = 0;
        return TRUE;
    }

    /*
     * A format with no palette never has one to send, and every place that
     * raises the flag -- a refresh, a screen change, the start of a session --
     * raises it without asking what the format is.  Dropped here, in the one
     * place that would act on it, and the pass then carries on to the grab
     * below rather than spending itself on a word that does not exist.
     */
    if (fb_want_pal && fb_colours(&fb_geom) == 0UL)
        fb_want_pal = 0;

    if (fb_want_pal)
    {
        rfb_u32 len = rfb_word_pal((char *)&fb_tx[10], fb_tx_cap - 10UL,
                                   fb_pal, (rfb_u32)fb_colours(&fb_geom));

        if (len == 0UL)
        {
            fb_close_saying(HTTP_WS_CLOSE_PROTOCOL,
                            "the palette word did not fit");
            return TRUE;
        }

        fb_frame_payload(HTTP_WS_EV_TEXT, (ULONG)len);
        fb_want_pal = 0;
        return TRUE;
    }

    /*
     * What the readback probe measured, once a screen.
     *
     * Nobody has published what an Amiga graphics card costs to read back,
     * which is the number the whole RTG design turns on, so the probe's
     * findings are sent rather than kept.  The viewer logs every word it does
     * not know, so this lands in front of somebody who can report it.  An
     * unrecognised word is ignored at the far end, which is what lets it go
     * down the same channel without the viewer being taught it.
     */
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

        /* The floor stands only until the frame this pass is about to produce
           has been sent and its real cost is known.  http_fb_write() replaces
           it then with the idle the duty cycle owes, which on anything but a
           trivial frame is the larger of the two. */
        fb_next_tick = tick + ((fb_band_ty0 == 0) ? (ULONG)FB_GRAB_FLOOR : 0UL);
        if (fb_next_tick == 0UL)
            fb_next_tick = 1UL;         /* 0 means it has never grabbed */

        /* The clock the duty cycle is charged against starts here, at the
           grab, and stops when the last byte of the frame has gone. */
        fb_frame_t0 = tick;
        if (fb_frame_t0 == 0UL)
            fb_frame_t0 = 1UL;          /* 0 means no frame is in flight */
    }

    /*
     * gt= is what is left of the grab, locking the public screen, checking the
     * geometry and reading the ColorMap, now that there is no copy of the
     * screen to make.  et= is the pass that reads the bitplanes.  The two used
     * to be a 40 KB copy and then a walk over the copy.  slice (gt+et) is the
     * figure that stayed comparable across that change.
     */
    {
        /*
         * The band to produce this pass.  FB_BAND_ROWS tile rows of it, so
         * the work between two reads of the socket is bounded by a strip and
         * not by a screen.  The last band of a pass is whatever is left, and
         * a grid shorter than one band is a single band covering all of it,
         * which is what a small screen gets.
         */
        UWORD ty1 = (UWORD)(fb_band_ty0 + FB_BAND_ROWS);

        if (ty1 > fb_enc.tiles_y)
            ty1 = (UWORD)fb_enc.tiles_y;

        fb_band_last = (UBYTE)(ty1 >= fb_enc.tiles_y);

        t0 = fb_ticks();
        rc = fb_grab_frame(&fb_geom, &seen, &palette_moved,
                           &fb_tx[10], fb_tx_cap - 10UL, &n,
                           fb_band_ty0, ty1);
        t1 = fb_ticks();

        /* Where the next one starts.  Anything but a clean band restarts the
           pass, because whatever went wrong resolved a different screen or
           none, and half of one picture followed by half of another is worse
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
        /*
         * No screen this pass, which is what a resolution change looks like.
         *
         * The session is not ended and the viewer is told nothing.  It stops
         * receiving frames for as long as the gap lasts and then gets the geom
         * that announces the screen that came back.  Ending it here is the
         * defect this replaces: a person changing the screen mode from the
         * browser lost the browser, at the one moment it cannot be recovered
         * by hand.
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

    case FB_GRAB_UNREADABLE:
        /*
         * A card's screen that could not be read this pass, because nothing
         * would lock it or the readback route failed.  Not fatal and not a
         * geom.  The viewer keeps the picture it has and the next pass tries
         * again, which is what happens while a non-public screen is in front.
         * The count is what says the picture stopped on purpose.
         */
        fb_nolock++;
        return TRUE;

    case FB_GRAB_VANISHED:
        /* The screen closed while it was being resolved.  Nothing this pass.
           The next one resolves whatever is in front now, which is either the
           same shape, and the stream carries on, or a different one, which
           comes back as FB_GRAB_CHANGED below. */
        return TRUE;

    case FB_GRAB_CHANGED:
        /*
         * The screen changed shape under a live session, or another screen of
         * a different shape came to the front.  Everything the receiver has is
         * now about a screen it is not being shown, so the buffers are retaken
         * at the new geometry and the receiver is told again.  That is what
         * stops a frame going out that disagrees with the last geometry it was
         * given.
         *
         * This is also how a session comes back from an empty screen list.  The
         * screen that reopens in the new resolution is a different shape, so
         * the barrier that already existed is the one that carries it.
         */
        fb_gone = 0;

        if (!fb_take_buffers(&seen))
        {
            fb_close_saying(HTTP_WS_CLOSE_GOING,
                            "the screen changed and this cannot follow it");
            return TRUE;
        }
        return TRUE;

    case FB_GRAB_REFUSED:
    default:
        fb_close_saying(HTTP_WS_CLOSE_GOING,
                        (fb_refuse_why != NULL)
                        ? fb_refuse_why
                        : "the front screen is not one this can read");
        return TRUE;
    }

    /*
     * A screen came back into an empty list at the same shape, after an
     * overscan change that did not move the bitmap, or a reopen caught between
     * two grabs.  Nothing above fires, because nothing about the geometry
     * changed, and yet this is a different screen with a different bitmap and
     * the shadow describes a picture that is not there any more.  So the
     * barrier is raised by hand, which is the same three things
     * fb_ask_resync() does, without its floor, because this is not a viewer
     * asking twice.
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
                        "the frame encoder did not encode this screen");
        return TRUE;
    }

    fb_frame_payload(HTTP_WS_EV_BINARY, (ULONG)n);

    /* The frame the resync promised has gone, so the next refresh is a new
       question rather than a repeat of this one. */
    fb_resync = 0;

    /* Screens and not messages, so f= keeps meaning what it meant before the
       frame was broken into bands and the rate derived from it stays
       comparable across the change.  Bytes are every band's, because every
       band's went on the wire. */
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

        /*
         * The frame is out, so what it cost is now known: everything from the
         * grab to this byte.  The idle owed against it is that over
         * FB_IDLE_DIVISOR, and setting the next grab that far out is the whole
         * of the duty cycle.  Charged here and not after the encode because
         * the send is work this task did too.
         *
         * Only a frame is charged.  A word queued by the slice -- geom, pal,
         * the pointer, fbstat -- leaves fb_frame_t0 at zero and passes
         * through, so a session that is only talking is not throttled as if
         * it were drawing.
         */
        if (fb_frame_t0 != 0UL)
        {
            ULONG done = fb_ticks();
            ULONG cost = (done >= fb_frame_t0) ? (done - fb_frame_t0) : 0UL;
            /*
             * Rounded UP, which is the difference between a cap that holds
             * and one that is nearly right.  A tick is a fiftieth and most
             * bands cost a handful of them, so truncating the division throws
             * away most of a tick every time: measured on the A3000 that came
             * out at 78.5% against the 75% intended.  Rounding up can only
             * leave more idle than the share demands, never less.
             */
            ULONG idle = (cost + (ULONG)FB_IDLE_DIVISOR - 1UL)
                       / (ULONG)FB_IDLE_DIVISOR;

            fb_busy_ticks += cost;
            fb_frame_t0 = 0;

            /*
             * The floor is about not re-reading a screen nobody drew on, so
             * it belongs between screen passes and not between the bands of
             * one.  fb_band_ty0 is zero exactly when the pass just finished,
             * which is where a whole tick of waiting is the right answer;
             * mid-pass the duty cycle is the only thing pacing, and a floor
             * there would add a tick per band and quarter the frame rate for
             * no reason.
             */
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

/*
 * A close frame can only go at a frame boundary.
 *
 * This writes to the socket itself rather than queueing, because the caller
 * closes the connection on the next line and there is no later pass to drain
 * anything in.  So it has to look at what is already half out.  A frame here
 * is up to a whole screen, 7 KB at 640x256x4 and four times that at depth 8,
 * and a viewer on a link whose window is smaller than one of them has the rest
 * of it still to come.  Writing the close after those bytes splices it into
 * somebody's payload, and everything the receiver reads from there on is a
 * mis-framed stream, so the browser reports a protocol error rather than the
 * sentence saying who took the screen.
 *
 * The rest of the frame is pushed first, as far as the socket will take it in
 * one go and no further, because this is called from the middle of the
 * server's loop and nothing here must block.  If it will not all go, the close
 * is not sent at all.  A truncated frame and a FIN is an abnormal close, which
 * is what the far end concludes anyway.
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

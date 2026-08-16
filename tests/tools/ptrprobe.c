/*
 * PtrProbe: what an injected IECLASS_POINTERPOS actually does, per display mode.
 *
 * The console injects an absolute pointer position for every mouse move a
 * browser sends.  IECLASS_POINTERPOS is not in screen pixels: Intuition
 * multiplies ie_X by IntuitionBase's MouseScaleX (the monitor's ticks per
 * mouse unit) and divides by the screen's ticks-per-pixel to land on a pixel.
 * Both numbers are in the graphics database, and neither is derivable from
 * the ViewPort's Modes bits, which is what src/tools/httpfb.c used to do.
 *
 * This opens a screen per mode, sends a position through input.device, and
 * reads back where the pointer landed in that screen's own pixels
 * (Screen->MouseX/MouseY).  Every row is printed for two rules at once, so
 * one boot produces the before and after table:
 *
 *   old   the Modes-bit rule: SUPERHIRES halves X, HIRES leaves it, else
 *         doubles; LACE leaves Y, else doubles
 *   new   ticks: ie = pixel * DisplayInfo.Resolution / MonitorInfo.MouseTicks
 *
 * SPDX-License-Identifier: MIT
 */

#include <devices/input.h>
#include <devices/inputevent.h>
#include <dos/dos.h>
#include <exec/io.h>
#include <exec/types.h>
#include <graphics/displayinfo.h>
#include <graphics/gfxbase.h>
#include <graphics/modeid.h>
#include <graphics/view.h>
#include <intuition/intuition.h>
#include <intuition/intuitionbase.h>
#include <intuition/screens.h>

#include <proto/dos.h>
#include <proto/exec.h>
#include <proto/graphics.h>
#include <proto/intuition.h>

#include <string.h>

struct GfxBase       *GfxBase;
struct IntuitionBase *IntuitionBase;

static struct MsgPort  *in_port;
static struct IOStdReq *in_req;
static BOOL             in_open;
static struct InputEvent ev;

/* The monitor files are executables that add a monitor to the display
   database, and nothing has run them: this boots to a Shell, not Workbench.
   Everything but PAL and NTSC needs one. */
static const char *const monitors[] = {
    "DEVS:Monitors/Multiscan",
    "DEVS:Monitors/Super72",
    "DEVS:Monitors/Euro72",
    "DEVS:Monitors/Euro36",
    "DEVS:Monitors/DblPAL",
    "DEVS:Monitors/DblNTSC",
    "DEVS:Monitors/VGAOnly",
    "DEVS:Monitors/A2024",
    "SYS:Devs/Monitors/Multiscan",
    "SYS:Devs/Monitors/Super72",
    "SYS:Devs/Monitors/Euro72",
    "SYS:Devs/Monitors/Euro36",
    "SYS:Devs/Monitors/DblPAL",
    "SYS:Devs/Monitors/DblNTSC",
    NULL
};

static const struct {
    ULONG       id;
    const char *name;
} modes[] = {
    { 0x00021000UL, "PAL:LoRes"                },
    { 0x00029000UL, "PAL:HiRes"                },
    { 0x00029004UL, "PAL:HiResLace"            },
    { 0x00029020UL, "PAL:SuperHiRes"           },
    { 0x00029024UL, "PAL:SuperHiResLace"       },
    { 0x00019000UL, "NTSC:HiRes"               },
    { 0x00019004UL, "NTSC:HiResLace"           },
    { 0x00039024UL, "Multiscan:Productivity"   },
    { 0x00039025UL, "Multiscan:ProductivityLace" },
    { 0x00039004UL, "Multiscan:LoRes"          },
    { 0x00089000UL, "Super72:HiRes"            },
    { 0x00089024UL, "Super72:SuperHiResLace"   },
    { 0x00069024UL, "Euro72:Productivity"      },
    { 0x000a9004UL, "DblPAL:HiResFF"           },
    { 0x00099004UL, "DblNTSC:HiResFF"          },
    { 0UL, NULL }
};

static BOOL input_open(VOID)
{
    in_port = CreateMsgPort();
    if (in_port == NULL)
        return FALSE;
    in_req = (struct IOStdReq *)CreateIORequest(in_port,
                                                sizeof(struct IOStdReq));
    if (in_req == NULL)
        return FALSE;
    if (OpenDevice((CONST_STRPTR)"input.device", 0,
                   (struct IORequest *)in_req, 0) != 0)
        return FALSE;
    in_open = TRUE;
    return TRUE;
}

static VOID input_close(VOID)
{
    if (in_open)
        CloseDevice((struct IORequest *)in_req);
    if (in_req != NULL)
        DeleteIORequest((struct IORequest *)in_req);
    if (in_port != NULL)
        DeleteMsgPort(in_port);
}

static VOID inject(WORD x, WORD y)
{
    memset(&ev, 0, sizeof(ev));
    ev.ie_Class     = IECLASS_POINTERPOS;
    ev.ie_Code      = IECODE_NOBUTTON;
    ev.ie_Qualifier = 0;
    ev.ie_X         = x;
    ev.ie_Y         = y;
    ev.ie_NextEvent = NULL;
    CurrentTime((ULONG *)&ev.ie_TimeStamp.tv_secs,
                (ULONG *)&ev.ie_TimeStamp.tv_micro);

    in_req->io_Command = IND_WRITEEVENT;
    in_req->io_Flags   = 0;
    in_req->io_Length  = (LONG)sizeof(struct InputEvent);
    in_req->io_Data    = (APTR)&ev;
    (VOID)DoIO((struct IORequest *)in_req);
}

/* The rule httpfb.c shipped with, in halves of a view unit per pixel. */
static VOID old_halves(struct Screen *sc, UWORD *xh, UWORD *yh)
{
    UWORD m = (UWORD)sc->ViewPort.Modes;

    if ((m & SUPERHIRES) != 0)
        *xh = 1;
    else if ((m & HIRES) != 0)
        *xh = 2;
    else
        *xh = 4;

    *yh = (UWORD)(((m & LACE) != 0) ? 2 : 4);
}

/* The rule it has now: src/tools/httpfb.c fb_display_units(), verbatim, so a
   row of this table is the shipped arithmetic and not a restatement of it. */
static UWORD res_x, res_y, tick_x, tick_y, spr_x, spr_y, pixel_ns;

static VOID display_units(struct Screen *sc)
{
    struct DisplayInfo di;
    struct MonitorInfo mi;
    ULONG              id;
    UWORD              modes;
    BOOL               have_disp = FALSE;

    id    = GetVPModeID(&sc->ViewPort);
    modes = (UWORD)sc->ViewPort.Modes;

    if ((modes & SUPERHIRES) != 0)
    {
        res_x    = 11;
        pixel_ns = 35;
    }
    else if ((modes & HIRES) != 0)
    {
        res_x    = 22;
        pixel_ns = 70;
    }
    else
    {
        res_x    = 44;
        pixel_ns = 140;
    }
    res_y  = (UWORD)(((modes & LACE) != 0) ? 22 : 44);
    spr_x  = (UWORD)(res_x * 2U);
    spr_y  = res_y;
    tick_x = 22;
    tick_y = 22;

    if (id == (ULONG)INVALID_ID)
        return;

    /* A short answer is not a failure: GetDisplayInfoData() fills what the
       database record holds, which is 48 bytes of a struct that is longer than
       that, so what is checked is the FIELDS, over a struct zeroed first. */
    memset(&di, 0, sizeof(di));
    memset(&mi, 0, sizeof(mi));

    if (GetDisplayInfoData(NULL, (UBYTE *)&di, sizeof(di), DTAG_DISP, id) > 0 &&
        di.Resolution.x > 0 && di.Resolution.y > 0)
    {
        have_disp    = TRUE;
        res_x    = (UWORD)di.Resolution.x;
        res_y    = (UWORD)di.Resolution.y;
        if (di.PixelSpeed > 0)
            pixel_ns = (UWORD)di.PixelSpeed;
        if (di.SpriteResolution.x > 0 && di.SpriteResolution.y > 0)
        {
            spr_x = (UWORD)di.SpriteResolution.x;
            spr_y = (UWORD)di.SpriteResolution.y;
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
        tick_x = (UWORD)mi.MouseTicks.x;
        tick_y = (UWORD)mi.MouseTicks.y;
        return;
    }

    switch (id & MONITOR_ID_MASK)
    {
    case NTSC_MONITOR_ID:
    case DBLNTSC_MONITOR_ID:
        tick_y = 26;
        break;
    case DEFAULT_MONITOR_ID:
    case A2024_MONITOR_ID:
        if ((GfxBase->DisplayFlags & PAL) == 0)
            tick_y = 26;
        break;
    default:
        break;
    }
}

/* The sprite scale, before and after, the same two ways httpfb.c had them. */
static VOID old_pointer_scale(struct Screen *sc, UWORD *xs, UWORD *ys)
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
    *ys = (UWORD)(((modes & LACE) != 0) ? 2 : 1);
}

static VOID new_pointer_scale(struct Screen *sc, UWORD *xs, UWORD *ys)
{
    struct ColorMap *cm = sc->ViewPort.ColorMap;
    UWORD            sprite_ns;
    UBYTE            resn = (UBYTE)SPRITERESN_ECS;

    display_units(sc);

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
        sprite_ns = 0;
        break;
    }

    if (sprite_ns != 0 && pixel_ns != 0)
        *xs = (UWORD)((sprite_ns >= pixel_ns) ? (sprite_ns / pixel_ns) : 1);
    else
        *xs = (UWORD)((spr_x >= res_x) ? (spr_x / res_x) : 1);

    *ys = (UWORD)((spr_y >= res_y) ? (spr_y / res_y) : 1);
}

int main(VOID)
{
    ULONG i;

    GfxBase = (struct GfxBase *)
        OpenLibrary((CONST_STRPTR)"graphics.library", 39);
    IntuitionBase = (struct IntuitionBase *)
        OpenLibrary((CONST_STRPTR)"intuition.library", 39);
    if (GfxBase == NULL || IntuitionBase == NULL)
    {
        Printf((CONST_STRPTR)"no v39 libraries\n");
        return 20;
    }

    if (!input_open())
    {
        Printf((CONST_STRPTR)"no input.device\n");
        input_close();
        return 20;
    }

    Printf((CONST_STRPTR)"machine_pal=%ld\n",
           (LONG)((GfxBase->DisplayFlags & PAL) != 0));

    for (i = 0; monitors[i] != NULL; i++)
    {
        BPTR lock = Lock((CONST_STRPTR)monitors[i], ACCESS_READ);
        if (lock == 0)
            continue;
        UnLock(lock);
        Printf((CONST_STRPTR)"monitor_run=%s rc=%ld\n", (LONG)monitors[i],
               (LONG)SystemTags((CONST_STRPTR)monitors[i], TAG_DONE));
    }

    for (i = 0; modes[i].name != NULL; i++)
    {
        struct DisplayInfo   di;
        struct MonitorInfo   mi;
        struct DimensionInfo dim;
        struct Screen       *sc;
        struct TagItem       tags[8];
        LONG                 gotdi, gotmi, gotdim;
        UWORD                xh, yh;
        WORD                 tx[6], ty[6];
        ULONG                t;
        LONG                 mtx, mty;

        if (ModeNotAvailable(modes[i].id) != 0)
        {
            Printf((CONST_STRPTR)"mode=%s id=%08lx unavailable\n",
                   (LONG)modes[i].name, (LONG)modes[i].id);
            continue;
        }

        memset(&di, 0, sizeof(di));
        memset(&mi, 0, sizeof(mi));
        memset(&dim, 0, sizeof(dim));
        gotdi  = GetDisplayInfoData(NULL, (UBYTE *)&di, sizeof(di),
                                    DTAG_DISP, modes[i].id);
        gotmi  = GetDisplayInfoData(NULL, (UBYTE *)&mi, sizeof(mi),
                                    DTAG_MNTR, modes[i].id);
        gotdim = GetDisplayInfoData(NULL, (UBYTE *)&dim, sizeof(dim),
                                    DTAG_DIMS, modes[i].id);

        tags[0].ti_Tag = SA_DisplayID;  tags[0].ti_Data = modes[i].id;
        tags[1].ti_Tag = SA_Depth;      tags[1].ti_Data = 2;
        tags[2].ti_Tag = SA_Overscan;   tags[2].ti_Data = OSCAN_TEXT;
        tags[3].ti_Tag = SA_Type;       tags[3].ti_Data = CUSTOMSCREEN;
        tags[4].ti_Tag = SA_Quiet;      tags[4].ti_Data = TRUE;
        tags[5].ti_Tag = SA_ShowTitle;  tags[5].ti_Data = FALSE;
        tags[6].ti_Tag = SA_AutoScroll; tags[6].ti_Data = FALSE;
        tags[7].ti_Tag = TAG_DONE;      tags[7].ti_Data = 0;

        sc = OpenScreenTagList(NULL, tags);
        if (sc == NULL)
        {
            Printf((CONST_STRPTR)"mode=%s id=%08lx openscreen_failed\n",
                   (LONG)modes[i].name, (LONG)modes[i].id);
            continue;
        }

        Delay(25);
        old_halves(sc, &xh, &yh);
        display_units(sc);

        mtx = (LONG)mi.MouseTicks.x;
        mty = (LONG)mi.MouseTicks.y;

        Printf((CONST_STRPTR)
               "mode=%s id=%08lx vpmodes=%04lx w=%ld h=%ld left=%ld top=%ld\n",
               (LONG)modes[i].name, (LONG)modes[i].id,
               (LONG)(UWORD)sc->ViewPort.Modes,
               (LONG)sc->Width, (LONG)sc->Height,
               (LONG)sc->LeftEdge, (LONG)sc->TopEdge);
        Printf((CONST_STRPTR)
               "  db di=%ld res=%ld,%ld sprres=%ld,%ld pxspeed=%ld"
               " mi=%ld ticks=%ld,%ld viewres=%ld,%ld dims=%ld nominal=%ld,%ld\n",
               gotdi, (LONG)di.Resolution.x, (LONG)di.Resolution.y,
               (LONG)di.SpriteResolution.x, (LONG)di.SpriteResolution.y,
               (LONG)di.PixelSpeed,
               gotmi, mtx, mty,
               (LONG)mi.ViewResolution.x, (LONG)mi.ViewResolution.y,
               gotdim,
               (LONG)(dim.Nominal.MaxX - dim.Nominal.MinX + 1),
               (LONG)(dim.Nominal.MaxY - dim.Nominal.MinY + 1));
        Printf((CONST_STRPTR)"  old_halves=%ld,%ld vpmodeid=%08lx"
               " new_res=%ld,%ld new_ticks=%ld,%ld new_spr=%ld,%ld ns=%ld\n",
               (LONG)xh, (LONG)yh, (LONG)GetVPModeID(&sc->ViewPort),
               (LONG)res_x, (LONG)res_y, (LONG)tick_x, (LONG)tick_y,
               (LONG)spr_x, (LONG)spr_y, (LONG)pixel_ns);
        {
            UWORD oxs, oys, nxs, nys;

            old_pointer_scale(sc, &oxs, &oys);
            new_pointer_scale(sc, &nxs, &nys);
            Printf((CONST_STRPTR)"  ptr_scale old=%ld,%ld new=%ld,%ld\n",
                   (LONG)oxs, (LONG)oys, (LONG)nxs, (LONG)nys);
        }

        tx[0] = 0;                     ty[0] = 0;
        tx[1] = 100;                   ty[1] = 50;
        tx[2] = (WORD)(sc->Width / 2); ty[2] = (WORD)(sc->Height / 2);
        tx[3] = (WORD)(sc->Width - 1); ty[3] = (WORD)(sc->Height - 1);
        tx[4] = 320;                   ty[4] = 200;
        tx[5] = 101;                   ty[5] = 51;

        for (t = 0; t < 6UL; t++)
        {
            WORD wx = tx[t], wy = ty[t];
            WORD ie_ox, ie_oy, ie_nx, ie_ny;
            WORD go_x, go_y, gn_x, gn_y;

            if (wx >= sc->Width || wy >= sc->Height)
                continue;

            ie_ox = (WORD)(((LONG)(wx + sc->LeftEdge) * (LONG)xh) / 2L);
            ie_oy = (WORD)(((LONG)(wy + sc->TopEdge) * (LONG)yh) / 2L);
            inject(ie_ox, ie_oy);
            Delay(5);
            go_x = sc->MouseX;
            go_y = sc->MouseY;

            ie_nx = (WORD)(((LONG)(wx + sc->LeftEdge) * (LONG)res_x) /
                           (LONG)tick_x);
            ie_ny = (WORD)(((LONG)(wy + sc->TopEdge) * (LONG)res_y) /
                           (LONG)tick_y);
            inject(ie_nx, ie_ny);
            Delay(5);
            gn_x = sc->MouseX;
            gn_y = sc->MouseY;

            Printf((CONST_STRPTR)
                   "  want=%ld,%ld old_ie=%ld,%ld old_got=%ld,%ld"
                   " new_ie=%ld,%ld new_got=%ld,%ld\n",
                   (LONG)wx, (LONG)wy, (LONG)ie_ox, (LONG)ie_oy,
                   (LONG)go_x, (LONG)go_y,
                   (LONG)ie_nx, (LONG)ie_ny, (LONG)gn_x, (LONG)gn_y);
        }

        CloseScreen(sc);
        Delay(25);
    }

    input_close();
    CloseLibrary((struct Library *)IntuitionBase);
    CloseLibrary((struct Library *)GfxBase);
    return 0;
}

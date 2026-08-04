/*
 * installdrive, run Commodore's Installer on the AmiNetXDuo script under
 * FS-UAE, without a human at the mouse.
 *
 * The Installer has no batch mode.  Every page it puts up waits for a click
 * on "Proceed", and the buttons carry no keyboard shortcut: find_key_gadget()
 * in the Installer's layout.c looks for an underscore in the button label,
 * and the English catalog has none in "Proceed" or "Abort Install".  Only
 * HELP and ESC are bound to keys.  So neither a keystroke injector nor a
 * fixed screen coordinate will do.
 *
 * What does work is to find the Proceed gadget by its gadget ID.  The
 * Installer numbers its own gadgets (window.h: PROCEED_ID 90, ABORT_ID 91),
 * and its event loop reads plain IntuiMessages off window->UserPort and
 * dispatches on Class and IAddress.  So this program walks Intuition's
 * window list looking for a window that has both of those gadgets, and posts
 * that window a GADGETUP naming the Proceed one, the same message
 * Intuition would have sent had the user clicked it.  No geometry and no
 * guessing where a button landed, so it works whatever font or screen size
 * the emulated machine came up with.
 *
 * Run from the harness's Startup-Sequence with no arguments.  It:
 *
 *   1. starts the Installer asynchronously with a 40000 byte stack (the
 *      documented requirement is 10000; the Shell's default 4000 is not
 *      enough);
 *   2. clicks Proceed on whatever page is up, once a second;
 *   3. stops once the Installer's window has been gone for several polls in
 *      a row, or on a hard timeout;
 *   4. writes DH0:installdrive.txt saying what it saw.
 *
 * Exit status is 0 only if the Installer appeared, was driven, and went away
 * of its own accord.
 *
 * SPDX-License-Identifier: MIT
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/ports.h>
#include <intuition/intuition.h>
#include <intuition/intuitionbase.h>
#include <dos/dos.h>
#include <dos/dostags.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>

/*
 * Gadget IDs, from the Installer's own window.h and the GadgetDef tables in
 * window.c.  Most pages carry a Proceed button with ID 90, but not all:
 * yesno_page() (which is what askbool draws) has no Proceed at all.  Its two
 * answer buttons are IDs 2 and 1, in that order, and yesno_page returns
 * "gadget id minus one", so ID 2 is the FIRST of the two (choices) strings.
 * Every askbool in this script has (default 1), which is that same first
 * choice, so clicking ID 2 is "take the default" throughout.
 *
 * ID 1 also appears as "Skip This Part" on the copy-confirmation page and as
 * the only button on the About page, which is why it is the last resort and
 * why ID 90 is always preferred when a page has one.
 */
#define PROCEED_ID  90
#define ABORT_ID    91
#define HELP_ID     100
#define YESNO_FIRST 2
#define SINGLE_ID   1

/*
 * Where the unpacked archive is staged.  Not "DH0:AmiNetXDuo", because that
 * is where the installer's own drawer will end up (@default-dest is the boot
 * volume on a machine with no Work: partition) and the source drawer must
 * not be the destination drawer.
 */
#define ARCHIVE_DRAWER  "DH0:Unpacked/AmiNetXDuo"

/*
 * Set at compile time so that the same program can drive a novice, average
 * or expert run, the harness starts it with no arguments.
 */
#ifndef DRIVE_LEVEL
#define DRIVE_LEVEL     "NOVICE"
#endif

/*
 * How many times to run the Installer.  Two is the interesting number: the
 * second run lands on a machine that is already installed, which is the
 * path where the script has to notice the existing DEVS:NetInterfaces and
 * offer to keep it rather than overwrite it.
 */
#ifndef DRIVE_RUNS
#define DRIVE_RUNS      1
#endif

/*
 * Which yes/no page to answer with the SECOND choice instead of the first.
 * 0 means "always take the first", which is the default in every askbool
 * this script has.  Set to 1 to answer "No, I will type them" to the
 * "does your network hand out addresses automatically?" question, which is
 * the only way to reach the static-address branch and its four validated
 * address prompts.
 */
#ifndef DRIVE_NO_ON_YESNO
#define DRIVE_NO_ON_YESNO 0
#endif

#define POLL_TICKS      50      /* Delay() counts 1/50 s, so: one second */

/*
 * Windowless polls before we call the run finished.
 *
 * The Installer takes its window DOWN while it copies, and puts it back for
 * the next page, so a gap here is normal and its length is however long the
 * copy takes on an emulated 68020.  Six was enough for the four-file payload
 * this started with; adding tls.library (273 KB) and the certificate store
 * (140 KB) pushed the gap past it, and the run was reported as "did not
 * complete cleanly" with everything up to the copy correctly installed --
 * which reads exactly like a script that aborted, and is not.
 *
 * Twenty is not a licence to hang: MAX_POLLS still bounds the whole run, and a
 * genuinely stuck Installer keeps its window up, so it fails on the cap
 * instead.
 */
#define GONE_LIMIT      20
#define MAX_POLLS       150     /* hard cap: two and a half minutes */

static struct MsgPort *reply_port;
static BPTR            report;

static LONG clicks;
static LONG saw_window;
static LONG yesno_pages;

static VOID say(const char *fmt, LONG a)
{
    LONG args[1];

    args[0] = a;
    if (report != 0)
    {
        VFPrintf(report, (STRPTR)fmt, args);
        /* Flushed line by line: if this run is killed by the harness
           timeout, the report so far is still on the host's disk. */
        Flush(report);
    }
}

/*
 * Look for a window carrying an Installer page.  Intuition's window list is
 * only stable under LockIBase(), so this copies out the two pointers it
 * needs and gets out again before doing anything else with them.
 */
static struct Window *find_installer_window(struct Gadget **click_out)
{
    struct Screen *screen;
    struct Window *found  = NULL;
    struct Gadget *choice = NULL;
    ULONG          ilock;

    ilock = LockIBase(0);

    for (screen = IntuitionBase->FirstScreen;
         screen != NULL && found == NULL;
         screen = screen->NextScreen)
    {
        struct Window *window;

        for (window = screen->FirstWindow;
             window != NULL;
             window = window->NextWindow)
        {
            struct Gadget *gad;
            struct Gadget *proceed = NULL;
            struct Gadget *yes     = NULL;
            struct Gadget *no      = NULL;
            struct Gadget *single  = NULL;
            BOOL           is_page = FALSE;

            for (gad = window->FirstGadget; gad != NULL; gad = gad->NextGadget)
            {
                switch (gad->GadgetID)
                {
                case PROCEED_ID:  proceed = gad; is_page = TRUE; break;
                case ABORT_ID:                   is_page = TRUE; break;
                case HELP_ID:                    is_page = TRUE; break;
                case YESNO_FIRST: yes     = gad;                 break;
                case SINGLE_ID:   single  = gad; no = gad;       break;
                default: break;
                }
            }

            if (!is_page)
                continue;

            found  = window;
            choice = proceed;
            if (choice == NULL && yes != NULL)
            {
                /* a yes/no page: ID 2 is the first (choices) string */
                yesno_pages++;
                choice = (yesno_pages == DRIVE_NO_ON_YESNO && no != NULL)
                             ? no : yes;
            }
            if (choice == NULL)
                choice = single;
            break;
        }
    }

    UnlockIBase(ilock);

    *click_out = choice;
    return found;
}

/*
 * Say which page we are looking at.  The Installer's buttons are
 * struct Button { struct Gadget Gadget; char *Text; ... } (window.h), so the
 * label is one pointer past the end of the Gadget, which makes it possible
 * to log what a page actually offers rather than guessing from the order
 * things happen in.
 */
struct InstButton
{
    struct Gadget  Gadget;
    char          *Text;
};

static VOID describe(struct Window *window)
{
    struct Gadget *gad;
    LONG           n = 0;

    if (window->Title != NULL)
        say("installdrive:   window \"%s\"\n", (LONG)window->Title);

    for (gad = window->FirstGadget; gad != NULL && n < 12; gad = gad->NextGadget)
    {
        char *text = ((struct InstButton *)gad)->Text;

        if (gad->GadgetID < 87)                 /* FIRSTRESV_ID: not a button */
            continue;
        n++;
        if (text != NULL && ((ULONG)text & 1) == 0)
            say("installdrive:   button \"%s\"\n", (LONG)text);
    }
}

static VOID drain_replies(VOID)
{
    struct Message *back;

    while ((back = GetMsg(reply_port)) != NULL)
        FreeMem(back, sizeof(struct IntuiMessage));
}

/*
 * Post the message Intuition would have posted had the user let go of the
 * mouse over the gadget.  The Installer replies to it, so the memory comes
 * back here and is freed rather than leaked, and getting it back is also
 * the proof that the Installer really consumed it.
 */
static BOOL click(struct Window *window, struct Gadget *gadget)
{
    struct IntuiMessage *msg;
    LONG                 spin;

    msg = (struct IntuiMessage *)
              AllocMem(sizeof(struct IntuiMessage), MEMF_PUBLIC | MEMF_CLEAR);
    if (msg == NULL)
        return FALSE;

    msg->ExecMessage.mn_Node.ln_Type = NT_MESSAGE;
    msg->ExecMessage.mn_ReplyPort    = reply_port;
    msg->ExecMessage.mn_Length       = sizeof(struct IntuiMessage);

    msg->Class       = IDCMP_GADGETUP;
    msg->IAddress    = (APTR)gadget;
    msg->IDCMPWindow = window;

    PutMsg(window->UserPort, &msg->ExecMessage);
    clicks++;

    /*
     * Bounded rather than WaitPort(): a page that is busy copying files can
     * take a while to look at its message port, and blocking here forever
     * would turn a slow install into a hang with no output.
     */
    for (spin = 0; spin < 50; spin++)
    {
        if (reply_port->mp_MsgList.lh_Head->ln_Succ != NULL)
            break;
        Delay(2);
    }
    drain_replies();

    return TRUE;
}

/*
 * One complete Installer run: start it, click Proceed on every page it puts
 * up, and return once its window has been gone for a few polls running.
 */
static BOOL drive_once(LONG run_number, BPTR nil_in, BPTR nil_out)
{
    struct TagItem tags[5];
    LONG polls;
    LONG gone   = 0;
    LONG seen   = 0;
    LONG settle = 0;

    say("installdrive: run %ld: starting the Installer\n", run_number);

    /*
     * SYS_Asynch hands the file handles to the new process, which closes
     * them, which is why they are not this program's own.  The output one
     * is a file rather than NIL: so that anything the Installer complains
     * about on the way up is readable from the host afterwards.
     *
     * Keyword and value are separate words, not KEYWORD=value.  Installer
     * 2.17 parses its command line by hand, comparing whole argv entries
     * against "SCRIPT", "APPNAME" and so on (window.c, the CLI branch of
     * main); it does not use ReadArgs, so "SCRIPT=foo" is not recognised as
     * a keyword and gets taken as the script's file name instead.  Later
     * Installers do use ReadArgs, which accepts the space-separated form
     * too, so this spelling is the one that works with both.
     *
     * NP_StackSize is the point of doing this from a program at all: the
     * Installer documents a 10000 byte stack requirement, and a command
     * started from the Shell gets 4000 unless somebody says otherwise.
     */
    tags[0].ti_Tag = SYS_Asynch;    tags[0].ti_Data = TRUE;
    tags[1].ti_Tag = SYS_Input;     tags[1].ti_Data = (ULONG)nil_in;
    tags[2].ti_Tag = SYS_Output;    tags[2].ti_Data = (ULONG)nil_out;
    tags[3].ti_Tag = NP_StackSize;  tags[3].ti_Data = 40000UL;
    tags[4].ti_Tag = TAG_DONE;      tags[4].ti_Data = 0;

    if (SystemTagList((STRPTR)
            "Installer "
            "SCRIPT Install-AmiNetXDuo "
            "APPNAME AmiNetXDuo "
            "MINUSER " DRIVE_LEVEL " DEFUSER " DRIVE_LEVEL " "
            "LOGFILE DH0:install-log.txt",
            tags) != 0)
    {
        say("installdrive: could not start the Installer (IoErr %ld)\n",
            IoErr());
        return FALSE;
    }

    for (polls = 0; polls < MAX_POLLS; polls++)
    {
        struct Gadget *target = NULL;
        struct Window *window;

        Delay(POLL_TICKS);

        window = find_installer_window(&target);

        /*
         * Let the first page settle before touching it.  A window exists,
         * with its gadgets in it, a little before the Installer is actually
         * waiting on its message port, and a GADGETUP posted into that gap
         * was observed once to make the whole run exit after the first page
         * with nothing done.  One extra second at the start is cheap
         * insurance against a flaky test.
         */
        if (window != NULL && seen == 0 && settle == 0)
        {
            settle = 1;
            say("installdrive: poll %ld: window is up, letting it settle\n",
                polls);
            continue;
        }

        if (window != NULL && target != NULL)
        {
            seen++;
            saw_window++;
            gone = 0;
            say("installdrive: poll %ld: clicking\n", polls);
            say("installdrive:   gadget id %ld\n", (LONG)target->GadgetID);
            describe(window);
            click(window, target);
        }
        else if (window != NULL)
        {
            seen++;
            saw_window++;
            gone = 0;
            say("installdrive: poll %ld: a page with no button I know\n",
                polls);
            describe(window);
        }
        else if (seen > 0)
        {
            if (++gone >= GONE_LIMIT)
                break;
        }
        else if ((polls % 10) == 9)
        {
            say("installdrive: poll %ld: still no Installer window\n", polls);
        }
    }

    if (seen == 0)
    {
        say("installdrive: run %ld never put a window up\n", run_number);
        return FALSE;
    }
    if (gone < GONE_LIMIT)
    {
        say("installdrive: run %ld timed out, still running\n", run_number);
        return FALSE;
    }

    say("installdrive: run %ld finished and closed down\n", run_number);
    return TRUE;
}

int main(void)
{
    BPTR  nil_in  = 0;
    BPTR  nil_out = 0;
    LONG  run_number;
    LONG  rc = RETURN_FAIL;

    report = Open((STRPTR)"DH0:installdrive.txt", MODE_NEWFILE);

    say("installdrive: driving the Installer at user level " DRIVE_LEVEL "\n",
        0);

    reply_port = CreateMsgPort();
    if (reply_port == NULL)
    {
        say("installdrive: no message port\n", 0);
        goto done;
    }

    /*
     * The Installer draws on the default public screen, which is the
     * Workbench screen, and on a machine booted to a bare Shell there is
     * not one.  LockPubScreen(NULL) is documented to open it, but relying on
     * that in theory is not the same as having seen it work, so ask for it
     * up front and say so if it does not arrive.
     */
    if (OpenWorkBench() == 0)
        say("installdrive: OpenWorkBench() failed -- no screen to draw on\n",
            0);
    else
        say("installdrive: Workbench screen is open\n", 0);

    /*
     * Started from the Shell, @icon is empty, so the script's relative
     * source paths resolve against the current directory.  A user who
     * double-clicks the icon gets that for free; from a Shell they would cd
     * into the drawer first, and so do we.  The process SystemTagList
     * creates inherits this.
     */
    {
        BPTR drawer = Lock((STRPTR)ARCHIVE_DRAWER, ACCESS_READ);

        if (drawer == 0)
        {
            say("installdrive: no " ARCHIVE_DRAWER " to install from\n", 0);
            goto done;
        }
        CurrentDir(drawer);
    }

    rc = RETURN_OK;
    for (run_number = 1; run_number <= DRIVE_RUNS; run_number++)
    {
        nil_in  = Open((STRPTR)"NIL:", MODE_OLDFILE);
        nil_out = Open((STRPTR)"DH0:installer-out.txt", MODE_NEWFILE);
        if (nil_out == 0)
            nil_out = Open((STRPTR)"NIL:", MODE_NEWFILE);

        if (!drive_once(run_number, nil_in, nil_out))
        {
            rc = RETURN_FAIL;
            break;
        }
    }

    say("installdrive: %ld pages driven in total\n", clicks);

done:
    if (reply_port != NULL)
    {
        drain_replies();
        DeleteMsgPort(reply_port);
    }

    if (report != 0)
        Close(report);

    return (int)rc;
}

/*
 * CardGrab: contend for the PCMCIA socket, then hand it over.
 *
 * WHY THIS EXISTS
 *
 * netdev_pcmcia.c has two paths nothing had ever driven.  The first is the
 * refusal: OwnCard() is asked for the slot with CARDF_IFAVAILABLE, so a slot
 * somebody else already holds is declined instead of queued, and the driver
 * has to come away with no unit and no latent handle.  The second is the
 * recovery: netdev_device.c retries the claim on a later OpenDevice(), so a
 * slot that was busy at probe time is still usable once its owner lets go.
 *
 * Both need a second owner, and every lab run so far has had exactly one.
 * This program is that owner.  It runs in one process and in one order:
 *
 *   1. OwnCard() the slot outright.  No CARDF_IFAVAILABLE here -- this is the
 *      program that is supposed to win.
 *   2. OpenDevice(anxnet.device, the PCMCIA unit pin).  This MUST fail: the
 *      driver's own OwnCard() is refused and it has no card to bind.
 *   3. ReleaseCard().
 *   4. OpenDevice() again.  This MUST now succeed, through the retry path.
 *
 * Step 2 is also what proves the refusal left nothing behind.  A driver that
 * queued a handle instead of declining one would be handed the slot by the
 * ReleaseCard() in step 3, and step 4 would then pass for the wrong reason --
 * which is why step 4 checks that the unit it opened is the PCMCIA row.
 *
 * Output is key=value on stdout and a RESULT= line last.  No card.resource,
 * or an empty socket, is RESULT=skip: a machine with no slot has nothing to
 * contend for and that is not a failure.
 *
 * SPDX-License-Identifier: MIT
 */

#include <exec/types.h>
#include <exec/io.h>
#include <exec/nodes.h>
#include <exec/interrupts.h>
#include <exec/libraries.h>
#include <dos/dos.h>
#include <resources/card.h>

#include <proto/exec.h>
#include <proto/dos.h>

#include "aminetxduo/anxnet.h"

static const char version_tag[] __attribute__((used)) =
    "$VER: CardGrab 1.0 (27.8.2026)";

#define TEMPLATE    "DEVICE/K,UNIT/K/N"

/*
 * card.resource stubs.  The same two netdev_pcmcia.c carries and for the same
 * reason: no proto header for this resource works on both toolchains.
 * OwnCard is -0x06(a1) and ReleaseCard -0x0c(a1,d0).
 */
static struct Library *CardResource;

static struct CardHandle *cg_own_card(struct CardHandle *h)
{
    register struct Library    *_a6 __asm("a6") = CardResource;
    register struct CardHandle *_a1 __asm("a1") = h;
    register struct CardHandle *res __asm("d0");

    __asm __volatile ("jsr a6@(-0x6)"
                      : "=r" (res)
                      : "r" (_a6), "r" (_a1)
                      : "d1", "a0", "cc", "memory");

    return res;
}

static VOID cg_release_card(struct CardHandle *h, ULONG flags)
{
    register struct Library    *_a6 __asm("a6") = CardResource;
    register struct CardHandle *_a1 __asm("a1") = h;
    register ULONG              _d0 __asm("d0") = flags;

    __asm __volatile ("jsr a6@(-0xc)"
                      : "+r" (_d0)
                      : "r" (_a6), "r" (_a1)
                      : "d1", "a0", "cc", "memory");
}

/* The three callbacks a CardHandle must carry.  They do nothing: this program
   holds the slot, it does not drive a card. */
static ULONG cg_ignore(VOID)
{
    return 0;
}

static struct Interrupt cg_removed;
static struct Interrupt cg_inserted;
static struct Interrupt cg_status;
static struct CardHandle cg_handle;

static int checks;
static int failed;

static VOID check(const char *what, int got, int want)
{
    checks++;
    if (got == want)
    {
        Printf((CONST_STRPTR)"ok   %s=%ld\n", (LONG)what, (LONG)got);
        return;
    }
    failed++;
    Printf((CONST_STRPTR)"FAIL %s=%ld want=%ld\n",
           (LONG)what, (LONG)got, (LONG)want);
}

/* Open one unit of the device and say whether it opened.  The request is
   built and torn down here so a refused open leaks nothing. */
static LONG cg_try_open(const char *devname, ULONG unit, LONG *err)
{
    struct MsgPort   *port;
    struct IORequest *req;
    LONG              rc;

    *err = -1;

    port = CreateMsgPort();
    if (port == NULL)
        return -1;

    req = CreateIORequest(port, (ULONG)sizeof(struct IOStdReq));
    if (req == NULL)
    {
        DeleteMsgPort(port);
        return -1;
    }

    rc = (LONG)OpenDevice((CONST_STRPTR)devname, unit, req, 0);
    *err = rc;
    if (rc == 0)
        CloseDevice(req);

    DeleteIORequest(req);
    DeleteMsgPort(port);

    return rc == 0 ? 1 : 0;
}

int main(void)
{
    LONG               args[2];
    struct RDArgs     *rda;
    struct CardHandle *owner;
    const char        *devname = "anxnet.device";
    ULONG              unit;
    LONG               err;
    LONG               opened;

    /* A guest program is started with argc == 1, so the arguments come from
       ReadArgs over the command line and never from argv. */
    args[0] = 0;
    args[1] = 0;
    rda = ReadArgs((CONST_STRPTR)TEMPLATE, args, NULL);
    if (rda == NULL)
    {
        PrintFault(IoErr(), (CONST_STRPTR)"CardGrab");
        return RETURN_ERROR;
    }
    if (args[0] != 0)
        devname = (const char *)args[0];

    /*
     * The PCMCIA row's unit pin.  ANXNET_CARD_NAMES documents the Nth name as
     * the card (N + 1) * ANXNET_UNIT_PIN names, and "pcmcia" is the eighth,
     * so the default is 800.  Overridable because "3c589" is the tenth and a
     * machine with that card in the slot answers on 1000 instead.
     */
    unit = 8UL * ANXNET_UNIT_PIN;
    if (args[1] != 0)
        unit = (ULONG)(*(LONG *)args[1]);

    Printf((CONST_STRPTR)"device=%s\n", (LONG)devname);
    Printf((CONST_STRPTR)"unit=%lu\n", (LONG)unit);

    CardResource = OpenResource((CONST_STRPTR)CARDRESNAME);
    Printf((CONST_STRPTR)"card_resource=%lu\n", (LONG)CardResource);
    if (CardResource == NULL)
    {
        Printf((CONST_STRPTR)"reason=no card.resource on this machine\n");
        Printf((CONST_STRPTR)"RESULT=skip\n");
        FreeArgs(rda);
        return RETURN_WARN;
    }
    Printf((CONST_STRPTR)"card_resource_version=%lu\n",
           (LONG)CardResource->lib_Version);

    cg_removed.is_Node.ln_Type = NT_INTERRUPT;
    cg_removed.is_Node.ln_Name = (char *)"CardGrab removed";
    cg_removed.is_Data         = NULL;
    cg_removed.is_Code         = (VOID (*)())cg_ignore;

    cg_inserted.is_Node.ln_Type = NT_INTERRUPT;
    cg_inserted.is_Node.ln_Name = (char *)"CardGrab inserted";
    cg_inserted.is_Data         = NULL;
    cg_inserted.is_Code         = (VOID (*)())cg_ignore;

    cg_status.is_Node.ln_Type = NT_INTERRUPT;
    cg_status.is_Node.ln_Name = (char *)"CardGrab status";
    cg_status.is_Data         = NULL;
    cg_status.is_Code         = (VOID (*)())cg_ignore;

    cg_handle.cah_CardNode.ln_Type = 0;
    cg_handle.cah_CardNode.ln_Pri  = 0;
    cg_handle.cah_CardNode.ln_Name = (char *)"CardGrab";
    /* No CARDF_IFAVAILABLE: this program is meant to take the slot, and
       taking it is the whole experiment. */
    cg_handle.cah_CardFlags   = 0;
    cg_handle.cah_CardRemoved  = &cg_removed;
    cg_handle.cah_CardInserted = &cg_inserted;
    cg_handle.cah_CardStatus   = &cg_status;

    /* ---- 1. take the slot ------------------------------------------- */
    owner = cg_own_card(&cg_handle);
    Printf((CONST_STRPTR)"owncard=%lu\n", (LONG)owner);
    if (owner != NULL)
    {
        /* Somebody already has it, and it is not us.  Nothing below can be
           read as an answer about the driver. */
        cg_handle.cah_CardNode.ln_Name = NULL;
        Printf((CONST_STRPTR)"reason=the slot is already owned by %s\n",
               (LONG)(owner->cah_CardNode.ln_Name != NULL
                                ? owner->cah_CardNode.ln_Name
                                : (char *)"an unnamed handle"));
        Printf((CONST_STRPTR)"RESULT=skip\n");
        FreeArgs(rda);
        return RETURN_WARN;
    }

    /* ---- 2. the driver must be refused ------------------------------- */
    opened = cg_try_open(devname, unit, &err);
    Printf((CONST_STRPTR)"contended_open=%ld\n", (LONG)opened);
    Printf((CONST_STRPTR)"contended_error=%ld\n", (LONG)err);
    check("refused_while_owned", (int)opened, 0);

    /* A second attempt while still owned: a driver that queued a handle on
       the first refusal would behave differently on the second. */
    opened = cg_try_open(devname, unit, &err);
    Printf((CONST_STRPTR)"contended_open2=%ld\n", (LONG)opened);
    check("refused_twice_while_owned", (int)opened, 0);

    /* ---- 3. give it back --------------------------------------------- */
    cg_release_card(&cg_handle, CARDF_REMOVEHANDLE);
    cg_handle.cah_CardNode.ln_Name = NULL;
    Printf((CONST_STRPTR)"released=1\n");

    /* card.resource hands a released slot to the next queued handle before
       ReleaseCard() returns.  The driver declined rather than queued, so
       nothing is queued and the slot is free -- which is exactly what the
       open below is asking. */

    /* ---- 4. the retry must now succeed -------------------------------- */
    opened = cg_try_open(devname, unit, &err);
    Printf((CONST_STRPTR)"released_open=%ld\n", (LONG)opened);
    Printf((CONST_STRPTR)"released_error=%ld\n", (LONG)err);
    check("claimed_after_release", (int)opened, 1);

    /* And it stays claimed: a second open of the same unit must find the unit
       the first one built rather than claim the slot over again. */
    opened = cg_try_open(devname, unit, &err);
    Printf((CONST_STRPTR)"reopen=%ld\n", (LONG)opened);
    check("reopen_after_claim", (int)opened, 1);

    Printf((CONST_STRPTR)"checks=%ld\n", (LONG)checks);
    Printf((CONST_STRPTR)"failed=%ld\n", (LONG)failed);
    Printf((CONST_STRPTR)"RESULT=%s\n",
           (LONG)(failed == 0 ? (char *)"pass" : (char *)"fail"));

    FreeArgs(rda);

    return failed == 0 ? RETURN_OK : RETURN_FAIL;
}

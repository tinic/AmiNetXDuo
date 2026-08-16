/*
 * anxnet.device: recording the probe, and publishing it.
 *
 * See include/aminetxduo/anxdiag.h for why this exists and why it is a
 * semaphore rather than a port.  What is here is the parts that touch Exec:
 *
 *   netdev_diag_note()      called from every step of the probe, on every
 *                           path, whatever the outcome.  A bounds test and
 *                           three stores; no allocation, no DOS, nothing that
 *                           can fail, so it is safe to call from anywhere the
 *                           probe reaches.
 *   netdev_diag_publish()   at the end of the romtag init, whether or not a
 *                           single unit came up -- the interesting case is
 *                           the one where none did.
 *   netdev_diag_unpublish() in the expunge, under Forbid(), BEFORE the memory
 *                           holding the record can go.
 *
 * No dos.library anywhere.  This driver is loaded by whichever stack wants it,
 * and that can be before a filesystem is up.  The derived station address does
 * not read the boot volume for the same reason.  The record lives in the
 * device base and goes when the device goes.
 *
 * This driver has already made the lifetime mistake once: netdev_pcmcia.c used
 * to ReleaseCard() without CARDF_REMOVEHANDLE and left card.resource holding a
 * Node in freed BSS.  A published semaphore is the same shape of mistake, so
 * removal is in the expunge path and nowhere else, and the reader copies the
 * record whole rather than keeping a pointer into it.
 *
 * SPDX-License-Identifier: MIT
 */

#include "netdev_internal.h"
#include "netdev_cards.h"

#include <proto/exec.h>

/*
 * The one record, reached by the layers below without a pointer threaded
 * through them.  There is one device base per machine, and the probe is not
 * re-entrant: it runs once, inside the romtag init.  A global here is the same
 * shape as SysBase and ExpansionBase beside it.  It is NULL until
 * netdev_diag_reset(), which makes netdev_diag_note() safe to call from a path
 * that runs before or after the probe.
 */
static AnxDiagMark *netdev_diag;

/* The published name, as a writable array: ln_Name is char *, and a Node
   must not point at a string literal. */
static char netdev_diag_name[] = ANXDIAG_NAME;

VOID netdev_diag_reset(AnxDiagMark *mark)
{
    UWORD i;
    UWORD n;

    mark->ad_Magic   = 0;       /* not published yet */
    mark->ad_Version = (UWORD)ANXDIAG_VERSION;
    mark->ad_Size    = (UWORD)sizeof(AnxDiagMark);
    mark->ad_Used    = 0;
    mark->ad_Lost    = 0;
    mark->ad_Units   = 0;
    mark->ad_Dropped = 0;

    /*
     * The row names, copied once.  The tool indexes them by ds_Card and so
     * never has to be built from the same table the driver was: a driver
     * carrying a card row the tool has never heard of still names it.
     */
    n = netdev_card_count;
    if (n > 16u)
        n = 16u;
    mark->ad_Cards = n;
    mark->ad_Pad   = 0;

    for (i = 0; i < n; i++)
    {
        const char *src = netdev_cards[i].name;
        UWORD       j;

        for (j = 0; j + 1u < (UWORD)sizeof(mark->ad_Name[0]) &&
                    src[j] != '\0'; j++)
            mark->ad_Name[i][j] = src[j];
        mark->ad_Name[i][j] = '\0';
    }

    netdev_diag = mark;
}

UWORD netdev_diag_card(const NetdevCard *card)
{
    UWORD i;

    for (i = 0; i < netdev_card_count; i++)
    {
        if (&netdev_cards[i] == card)
            return i;
    }

    return (UWORD)ANXDIAG_NOCARD;
}

VOID netdev_diag_note(UWORD code, UWORD card, ULONG value)
{
    AnxDiagMark *mark = netdev_diag;

    if (mark == NULL)
        return;

    /*
     * A full record counts, it does not wrap.  A ring would keep the end of
     * the probe and throw the beginning away, and the beginning is where
     * "expansion.library did not open" and "there is no card.resource on this
     * machine" live.  Those two answers explain everything after them.
     * Overflow is a number the tool prints, so a machine that produced more
     * steps than this holds reports that rather than a plausible half.
     */
    if (mark->ad_Used >= (UWORD)ANXDIAG_STEPS)
    {
        mark->ad_Lost++;
        return;
    }

    mark->ad_Step[mark->ad_Used].ds_Code  = code;
    mark->ad_Step[mark->ad_Used].ds_Card  = card;
    mark->ad_Step[mark->ad_Used].ds_Value = value;
    mark->ad_Used++;
}

VOID netdev_diag_counts(UWORD units, UWORD dropped)
{
    if (netdev_diag == NULL)
        return;

    netdev_diag->ad_Units   = units;
    netdev_diag->ad_Dropped = dropped;
}

VOID netdev_diag_publish(AnxDiagMark *mark)
{
    InitSemaphore(&mark->ad_Semaphore);
    mark->ad_Semaphore.ss_Link.ln_Name = netdev_diag_name;
    mark->ad_Semaphore.ss_Link.ln_Pri  = 0;

    /*
     * A second anxnet.device on one machine -- two copies in DEVS:, or one
     * resident and one loaded -- leaves the first one's record published and
     * this one unpublished, rather than giving FindSemaphore() two answers.
     * Same rule as the health mark in src/netstack/netstack_baton.c.
     *
     * ad_Magic is the "this one is ours to remove" flag as well as the
     * reader's sanity check, which is why it is set here and not in reset().
     */
    Forbid();
    if (FindSemaphore((STRPTR)netdev_diag_name) == NULL)
    {
        AddSemaphore(&mark->ad_Semaphore);
        mark->ad_Magic = ANXDIAG_MAGIC;
    }
    Permit();
}

VOID netdev_diag_unpublish(AnxDiagMark *mark)
{
    if (mark->ad_Magic != ANXDIAG_MAGIC)
        return;         /* never published, or already taken out */

    /*
     * Before the device base can be freed.  A reader holds Forbid() across
     * FindSemaphore() and its copy, so this cannot take the record out from
     * under one that is halfway through it.
     */
    Forbid();
    RemSemaphore(&mark->ad_Semaphore);
    mark->ad_Magic = 0;
    netdev_diag    = NULL;
    Permit();
}

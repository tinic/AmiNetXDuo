/*
 * The probe record, and the semaphore it is published on.
 *
 * WHY THIS IS WORTH MORE THAN ITS 148 LINES.  This record is the ONLY channel
 * by which a machine nobody here has can report what the probe decided.  Three
 * of the twelve card rows are marked UNVERIFIED ON HARDWARE in netdev_cards.c,
 * the X-Surf 500 has no emulator at all, and the ISA PnP sequence records its
 * vendor id, serial number, checksum comparison and settle rounds here and
 * nowhere else.  When a user says "it does not come up", this is the answer.
 * A record that silently loses the beginning of the probe, or that a second
 * driver quietly overwrites, is a diagnostic that lies at exactly the moment
 * it is needed.
 *
 * BOTH OF THOSE ARE DESIGN DECISIONS THE SOURCE STATES AND NOTHING ASSERTED:
 *
 *   netdev_diag.c:86  "A full record counts, it does not wrap.  A ring would
 *                      keep the end of the probe and throw the beginning away,
 *                      and the beginning is where 'expansion.library did not
 *                      open' lives."
 *   netdev_diag.c:122 "A second anxnet.device on one machine leaves the first
 *                      one's record published and this one unpublished, rather
 *                      than giving FindSemaphore() two answers."
 *
 * PROVEN TO CATCH, six mutations of netdev_diag.c: a full record wrapping
 * instead of counting, the ad_Magic guard dropped from unpublish, publish
 * adding unconditionally, the Forbid() around the list gone, reset() setting
 * the magic itself, and netdev_diag_card() answering 0 for a card that is not
 * in the table.
 *
 * AND ONE IT DOES NOT: removing the clamp of the name count to sixteen.  The
 * table has twelve rows, so that branch does not run at all, and no fixture
 * here can give it seventeen without linking a second card table.  A tripwire
 * on the row count stands in for it -- see a_reset_describes_the_record().
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <string.h>

#include <proto/exec.h>

#include "netdev_internal.h"
#include "netdev_cards.h"

/*
 * The four chip cores netdev_cards.c names.  Referenced by address only, so
 * empty tables are enough -- the same tentative definitions test_netdev_cards.c
 * and test_netdev_isapnp.c carry.
 */
#include "netdev_nic.h"
const struct NetdevNicOps netdev_nic_ne2000;
const struct NetdevNicOps netdev_nic_ed;
const struct NetdevNicOps netdev_nic_lance;
const struct NetdevNicOps netdev_nic_el3;

static int failures;
static int checks;

static void expect(int ok, const char *what)
{
    checks++;
    if (ok)
        return;

    printf("FAIL %s\n", what);
    failures++;
}

static void expect_u32(const char *what, unsigned long got, unsigned long want)
{
    checks++;
    if (got == want)
        return;

    printf("FAIL %s: got %lu, want %lu\n", what, got, want);
    failures++;
}

/* ---------------------------------------------------------------- exec --- */

/*
 * The semaphore list, which is the whole point of the publish path: a second
 * driver must find the first one's record and leave it alone.  Exec's real
 * list is public and named; this one is the same shape.
 */
#define SEM_MAX 4
static struct SignalSemaphore *sems[SEM_MAX];
static int    sem_n;
static int    forbid_depth;

VOID Forbid(VOID)
{
    forbid_depth++;
}

VOID Permit(VOID)
{
    forbid_depth--;
    if (forbid_depth < 0)
    {
        printf("FAIL Permit() without a matching Forbid()\n");
        failures++;
        forbid_depth = 0;
    }
}

VOID InitSemaphore(struct SignalSemaphore *s)
{
    memset(s, 0, sizeof(*s));
    s->ss_Link.ln_Type = 0;
}

VOID AddSemaphore(struct SignalSemaphore *s)
{
    /* Adding or removing a public semaphore outside Forbid() is the race the
       driver's Forbid()/Permit() pair exists to close. */
    if (forbid_depth < 1)
    {
        printf("FAIL AddSemaphore() outside Forbid()\n");
        failures++;
    }
    if (sem_n < SEM_MAX)
        sems[sem_n++] = s;
}

VOID RemSemaphore(struct SignalSemaphore *s)
{
    int i;

    if (forbid_depth < 1)
    {
        printf("FAIL RemSemaphore() outside Forbid()\n");
        failures++;
    }

    for (i = 0; i < sem_n; i++)
    {
        if (sems[i] == s)
        {
            int j;

            for (j = i; j + 1 < sem_n; j++)
                sems[j] = sems[j + 1];
            sem_n--;
            return;
        }
    }

    printf("FAIL RemSemaphore() on a semaphore that is not on the list\n");
    failures++;
}

struct SignalSemaphore *FindSemaphore(STRPTR name)
{
    int i;

    for (i = 0; i < sem_n; i++)
    {
        const char *n = sems[i]->ss_Link.ln_Name;

        if (n != NULL && strcmp(n, (const char *)name) == 0)
            return sems[i];
    }

    return NULL;
}

/* ------------------------------------------------------------- fixtures - */

static AnxDiagMark mark_a;
static AnxDiagMark mark_b;

static void world_reset(void)
{
    sem_n = 0;
    forbid_depth = 0;
    memset(&mark_a, 0xa5, sizeof(mark_a));
    memset(&mark_b, 0xa5, sizeof(mark_b));
}

/* ============================================================= the reset = */

static void a_reset_describes_the_record(void)
{
    UWORD i;
    UWORD want_cards;

    world_reset();
    netdev_diag_reset(&mark_a);

    expect_u32("the record is not published by reset alone",
               mark_a.ad_Magic, 0);
    expect_u32("it carries the version a reader checks",
               mark_a.ad_Version, (unsigned long)ANXDIAG_VERSION);
    expect_u32("and its own size", mark_a.ad_Size,
               (unsigned long)sizeof(AnxDiagMark));
    expect_u32("with nothing recorded", mark_a.ad_Used, 0);
    expect_u32("and nothing lost", mark_a.ad_Lost, 0);
    expect_u32("no units yet", mark_a.ad_Units, 0);
    expect_u32("and none dropped", mark_a.ad_Dropped, 0);

    /*
     * The names are COPIED, and the count is capped at the array: a table that
     * grew past sixteen rows would otherwise write past ad_Name.
     */
    want_cards = netdev_card_count > 16u ? 16u : netdev_card_count;
    expect_u32("the card names are copied, capped at the array",
               mark_a.ad_Cards, want_cards);

    /*
     * A TRIPWIRE, not a check of the cap.  netdev_diag_reset() clamps the
     * count to the sixteen ad_Name holds, and with twelve rows in the table
     * that branch never runs -- removing it entirely passes every assertion
     * in this file, which is recorded in the header above rather than papered
     * over.  What CAN be asserted is the condition under which the clamp
     * starts to matter: past sixteen rows the record carries names for the
     * first sixteen cards only, and CheckNetDevice indexes ad_Name by
     * ds_Card, so card 16 and up would print the wrong name or none.  When
     * this fires, ad_Name needs to grow with the table.
     */
    expect(netdev_card_count <= 16u,
           "the card table still fits the names the record publishes");

    for (i = 0; i < want_cards; i++)
    {
        const char *want = netdev_cards[i].name;
        char        what[64];
        size_t      n = strlen(want);

        snprintf(what, sizeof(what), "name[%u] is row %u's", i, i);

        if (n < sizeof(mark_a.ad_Name[0]))
            expect(strcmp(mark_a.ad_Name[i], want) == 0, what);
        else
            expect(strncmp(mark_a.ad_Name[i], want,
                           sizeof(mark_a.ad_Name[0]) - 1u) == 0, what);

        snprintf(what, sizeof(what), "and name[%u] is terminated", i);
        expect(mark_a.ad_Name[i][sizeof(mark_a.ad_Name[0]) - 1u] == '\0' ||
               strlen(mark_a.ad_Name[i]) < sizeof(mark_a.ad_Name[0]), what);
    }
}

/* An index into that array, and a pointer that is not a row at all. */
static void b_card_index(void)
{
    UWORD i;

    world_reset();
    netdev_diag_reset(&mark_a);

    for (i = 0; i < netdev_card_count; i++)
    {
        char what[64];

        snprintf(what, sizeof(what), "row %u indexes to %u", i, i);
        expect_u32(what, netdev_diag_card(&netdev_cards[i]), i);
    }

    {
        NetdevCard stranger;

        memset(&stranger, 0, sizeof(stranger));
        expect_u32("a card that is not in the table has no index",
                   netdev_diag_card(&stranger),
                   (unsigned long)ANXDIAG_NOCARD);
    }
}

/* ============================================================ the notes == */

/*
 * A FULL RECORD COUNTS, IT DOES NOT WRAP.  The beginning of a probe is where
 * "expansion.library did not open" lives, so a ring would keep the least
 * useful end of it.  This fills the record past its capacity and checks that
 * the FIRST steps are the ones that survived.
 */
static void c_overflow_keeps_the_beginning(void)
{
    UWORD i;
    int   good = 1;

    world_reset();
    netdev_diag_reset(&mark_a);

    for (i = 0; i < (UWORD)ANXDIAG_STEPS + 10u; i++)
        netdev_diag_note((UWORD)(0x100u + i), (UWORD)(i & 0x0fu), i);

    expect_u32("the record fills to its capacity and stops",
               mark_a.ad_Used, (unsigned long)ANXDIAG_STEPS);
    expect_u32("and counts what it could not take", mark_a.ad_Lost, 10);

    for (i = 0; i < (UWORD)ANXDIAG_STEPS; i++)
    {
        if (mark_a.ad_Step[i].ds_Code  != (UWORD)(0x100u + i) ||
            mark_a.ad_Step[i].ds_Card  != (UWORD)(i & 0x0fu) ||
            mark_a.ad_Step[i].ds_Value != (ULONG)i)
        {
            good = 0;
            break;
        }
    }
    expect(good, "the steps kept are the FIRST ones, in order");
}

/*
 * A note before any record exists is dropped, not written through a NULL.
 * The probe runs inside the romtag init and the layers below reach this
 * without a pointer threaded through them, so "not yet" is a real state.
 */
static void d_a_note_with_no_record_is_dropped(void)
{
    world_reset();

    /* Take the record away first, so the static inside the file is NULL. */
    netdev_diag_reset(&mark_a);
    netdev_diag_publish(&mark_a);
    netdev_diag_unpublish(&mark_a);

    mark_a.ad_Used = 0;
    netdev_diag_note(1, 2, 3);
    expect_u32("a note after unpublish reaches nothing", mark_a.ad_Used, 0);

    netdev_diag_counts(5, 6);
    expect(mark_a.ad_Units != 5, "and so does a count");
}

/* The unit counts land where the tool reads them. */
static void e_counts(void)
{
    world_reset();
    netdev_diag_reset(&mark_a);

    netdev_diag_counts(3, 2);
    expect_u32("units that came up", mark_a.ad_Units, 3);
    expect_u32("and supported boards with no unit", mark_a.ad_Dropped, 2);
}

/* =========================================================== publishing == */

static void f_publish_and_unpublish(void)
{
    world_reset();
    netdev_diag_reset(&mark_a);

    expect_u32("nothing is published yet", (unsigned long)sem_n, 0);

    netdev_diag_publish(&mark_a);

    expect_u32("the record is on the semaphore list", (unsigned long)sem_n, 1);
    expect_u32("and carries the magic a reader checks",
               mark_a.ad_Magic, ANXDIAG_MAGIC);
    expect(FindSemaphore((STRPTR)ANXDIAG_NAME) == &mark_a.ad_Semaphore,
           "and is found under the published name");
    expect_u32("with Forbid balanced", (unsigned long)forbid_depth, 0);

    netdev_diag_unpublish(&mark_a);

    expect_u32("unpublish takes it off the list", (unsigned long)sem_n, 0);
    expect_u32("and clears the magic", mark_a.ad_Magic, 0);
    expect_u32("with Forbid balanced", (unsigned long)forbid_depth, 0);

    /* Idempotent: a second unpublish must not remove somebody else's. */
    netdev_diag_unpublish(&mark_a);
    expect_u32("a second unpublish does nothing", (unsigned long)sem_n, 0);
}

/*
 * TWO DRIVERS, ONE MACHINE.  The second must find the first's record and
 * leave it alone -- publishing both would give FindSemaphore() two answers,
 * and the tool would read whichever Exec happened to return.  The second
 * record staying unpublished is also what makes ad_Magic the "ours to remove"
 * flag: without it, the second driver's expunge would take the FIRST one's
 * record out from under a reader.
 */
static void g_a_second_driver_does_not_displace_the_first(void)
{
    world_reset();
    netdev_diag_reset(&mark_a);
    netdev_diag_reset(&mark_b);

    netdev_diag_publish(&mark_a);
    netdev_diag_publish(&mark_b);

    expect_u32("only one record is published", (unsigned long)sem_n, 1);
    expect(FindSemaphore((STRPTR)ANXDIAG_NAME) == &mark_a.ad_Semaphore,
           "and it is the FIRST one");
    expect_u32("the first carries the magic", mark_a.ad_Magic, ANXDIAG_MAGIC);
    expect_u32("the second does not", mark_b.ad_Magic, 0);

    /*
     * And the second driver's expunge is harmless.  Without the magic guard
     * this RemSemaphore()s a semaphore it never added -- which on the real
     * Exec list means unlinking the FIRST driver's record.
     */
    netdev_diag_unpublish(&mark_b);
    expect_u32("the second driver's expunge removes nothing",
               (unsigned long)sem_n, 1);
    expect(FindSemaphore((STRPTR)ANXDIAG_NAME) == &mark_a.ad_Semaphore,
           "and the first record is still published");

    netdev_diag_unpublish(&mark_a);
    expect_u32("the first driver's expunge removes its own",
               (unsigned long)sem_n, 0);
}

/* The published name is the one the tool looks for, and it is a writable
   array rather than a literal: ln_Name is char * and a Node must not point
   at read-only storage. */
static void h_the_published_name(void)
{
    world_reset();
    netdev_diag_reset(&mark_a);
    netdev_diag_publish(&mark_a);

    expect(mark_a.ad_Semaphore.ss_Link.ln_Name != NULL,
           "the semaphore is named");
    expect(strcmp(mark_a.ad_Semaphore.ss_Link.ln_Name, ANXDIAG_NAME) == 0,
           "with the name the tool looks up");
    expect_u32("at priority 0", (unsigned long)(UBYTE)
               mark_a.ad_Semaphore.ss_Link.ln_Pri, 0);

    netdev_diag_unpublish(&mark_a);
}

int main(void)
{
    a_reset_describes_the_record();
    b_card_index();
    c_overflow_keeps_the_beginning();
    d_a_note_with_no_record_is_dropped();
    e_counts();
    f_publish_and_unpublish();
    g_a_second_driver_does_not_displace_the_first();
    h_the_published_name();

    if (failures != 0)
    {
        printf("netdev_diag: %d of %d checks failed\n", failures, checks);
        return 1;
    }

    printf("netdev_diag: %d checks ok\n", checks);

    return 0;
}

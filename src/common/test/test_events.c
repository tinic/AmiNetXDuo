/*
 * AmiNetXDuo, host-side test for the event ring and the table that decodes it.
 *
 * THE TWO HALVES THIS HOLDS TOGETHER.  src/common/events.c records a number
 * and src/tools/tool_events.c turns it into a sentence, and they are in
 * different binaries on the machine: the library keeps the ring, a Shell
 * command prints it.  Nothing on the Amiga links both, so nothing on the Amiga
 * can notice that a code was added to one and not the other.  This links both
 * and does.
 *
 * Neither file makes an AmigaDOS call.  events.c needs Disable()/Enable() and
 * ami_millis_quick(), all three stubbed below; tool_events.c needs nothing at
 * all.  So the same sources that ship are the sources under test, rather than
 * a copy of the table.
 *
 *   cc -std=c99 -Wall -Wextra -I../../../include -Ishim \
 *      test_events.c ../events.c ../../tools/tool_events.c -o test_events
 *
 * SPDX-License-Identifier: MIT
 */

#include "aminetxduo/events.h"
#include "tool_events.h"

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ stubs */

/*
 * Counted rather than ignored: recording must leave the machine as it found
 * it, and a path that returns still inside Disable() stops every interrupt on
 * a real one.
 */
static int stub_disable_depth;
static int stub_disable_worst;

void Disable(void)
{
    stub_disable_depth++;
    if (stub_disable_depth > stub_disable_worst)
        stub_disable_worst = stub_disable_depth;
}

void Enable(void)
{
    stub_disable_depth--;
}

/* The clock the library has when the timer is not open. */
static ULONG stub_millis;

ULONG ami_millis_quick(void)
{
    return stub_millis;
}

/* ------------------------------------------------------------------ checks */

static int failures;

static void check(int ok, const char *what)
{
    if (!ok)
    {
        printf("FAIL: %s\n", what);
        failures++;
    }
}

static void check_str(const char *got, const char *want, const char *what)
{
    if (got == NULL || strcmp(got, want) != 0)
    {
        printf("FAIL: %s\n  want: %s\n  got:  %s\n", what, want,
               (got != NULL) ? got : "(none)");
        failures++;
    }
}

/* Every code the library can record.  A code added to netstatus.h and not to
   the table is what this list exists to catch, so it is written out rather
   than derived. */
static const UWORD every_code[] =
{
    NETEVENT_BRINGUP,
    NETEVENT_SHUTDOWN,
    NETEVENT_NOTIFY,
    NETEVENT_RELEASE,
    NETEVENT_DEVICE_OPEN,
    NETEVENT_DEVICE_REFUSED,
    NETEVENT_ATTACH_FAILED,
    NETEVENT_LINK_DOWN,
    NETEVENT_OUT_OF_SERVICE,
    NETEVENT_OFFLINE_SKIPPED,
    NETEVENT_OFFLINE_FAILED,
    NETEVENT_IFACE_RETAINED,
    NETEVENT_STACK_RETAINED,
    NETEVENT_EXPUNGE_DECLINED,
};

#define EVERY_CODE_COUNT (sizeof(every_code) / sizeof(every_code[0]))

/* --------------------------------------------------------- the decode side */

static void test_table_is_complete(void)
{
    ULONG i;
    ULONG j;

    for (i = 0; i < (ULONG)EVERY_CODE_COUNT; i++)
    {
        const char *text = tool_event_text(every_code[i]);

        if (text == NULL)
        {
            printf("FAIL: code %lu has no sentence in tool_events.c\n",
                   (unsigned long)every_code[i]);
            failures++;
            continue;
        }

        check(text[0] != '\0', "a sentence is not empty");

        /* House style: these are printed in a list under a heading, so each
           one is a clause and not a sentence with a capital and a stop. */
        /* Lower case, because each is a clause in a list under a heading
           rather than a sentence of its own.  An identifier is the one
           exception, and it is recognisable by its second letter: S2_OFFLINE
           is a name and "the" would be a capital nobody asked for. */
        check((text[0] >= 'a' && text[0] <= 'z') ||
              !(text[1] >= 'a' && text[1] <= 'z'),
              "a sentence starts lower case unless it starts with a name");
        check(text[strlen(text) - 1] != '.',
              "a sentence has no full stop");
    }

    /* No two codes share a number. */
    for (i = 0; i < (ULONG)EVERY_CODE_COUNT; i++)
        for (j = i + 1; j < (ULONG)EVERY_CODE_COUNT; j++)
            check(every_code[i] != every_code[j], "codes are distinct");

    /* A code from a newer library decodes to nothing, so the command prints
       the number rather than dropping the entry. */
    check(tool_event_text(999) == NULL, "an unknown code has no sentence");
    check(tool_event_detail(999) == NULL, "an unknown code has no detail");
}

static void test_value_names(void)
{
    check_str(tool_event_value_name(NETEVENT_IFACE_RETAINED,
                                    NETEVENT_HELD_RX),
              "a read", "a retained read");
    check_str(tool_event_value_name(NETEVENT_IFACE_RETAINED,
                                    NETEVENT_HELD_TX),
              "a write", "a retained write");
    check_str(tool_event_value_name(NETEVENT_IFACE_RETAINED,
                                    NETEVENT_HELD_RX | NETEVENT_HELD_TX),
              "a read and a write", "both sides retained");

    check_str(tool_event_value_name(NETEVENT_EXPUNGE_DECLINED,
                                    NETEVENT_EXP_TCP),
              "the TCP: handler is running", "the TCP: handler decline");
    check_str(tool_event_value_name(NETEVENT_EXPUNGE_DECLINED,
                                    NETEVENT_EXP_NETMON),
              "a monitoring hook is installed", "the netmon decline");

    /* A value the table does not know, on a code whose values are named:
       nothing rather than a wrong name. */
    check(tool_event_value_name(NETEVENT_EXPUNGE_DECLINED, 99) == NULL,
          "an unknown reason has no name");

    /* A code whose value is a number carries no name. */
    check(tool_event_value_name(NETEVENT_BRINGUP, 2) == NULL,
          "a counted value has no name");
}

/* ---------------------------------------------------------- the ring side */

static NetStatusEvent out[AMINETXDUO_EVENT_RING * 4];

static void test_ring_records(void)
{
    ULONG held = 0;
    ULONG n;

    stub_millis = 1234;
    ami_event(NETEVENT_BRINGUP, NETEVENT_NOINDEX, 2UL);
    stub_millis = 1240;
    ami_event(NETEVENT_LINK_DOWN, 1, 0UL);

    n = ami_event_snapshot(out, (ULONG)(sizeof(out) / sizeof(out[0])), &held);

    check(n == 2, "two events recorded");
    check(held == 2, "two events held");

    check(out[0].nse_Code == NETEVENT_BRINGUP, "oldest first");
    check(out[0].nse_Index == (UWORD)NETEVENT_NOINDEX, "no interface");
    check(out[0].nse_Value == 2UL, "the value survives");
    check(out[0].nse_Tick == 1234UL, "the time survives");
    check(out[0].nse_Seq == 1UL, "the first is sequence 1");

    check(out[1].nse_Code == NETEVENT_LINK_DOWN, "then the second");
    check(out[1].nse_Index == 1, "the interface survives");
    check(out[1].nse_Seq == 2UL, "the second is sequence 2");

    check(stub_disable_depth == 0, "recording left Disable() balanced");
    check(stub_disable_worst == 1, "recording did not nest Disable()");
}

/*
 * More events than the ring holds.  The oldest go, the newest stay, and
 * nse_Seq is what says how many went: this is the only thing that stops a
 * reader believing a machine's history began where the ring did.
 */
static void test_ring_wraps(void)
{
    ULONG held = 0;
    ULONG n;
    ULONG i;
    ULONG extra = (ULONG)AMINETXDUO_EVENT_RING + 5UL;

    for (i = 0; i < extra; i++)
    {
        stub_millis = 2000UL + i;
        ami_event(NETEVENT_OFFLINE_SKIPPED, (UWORD)i, i);
    }

    n = ami_event_snapshot(out, (ULONG)(sizeof(out) / sizeof(out[0])), &held);

    check(n == (ULONG)AMINETXDUO_EVENT_RING, "the ring is full and no more");
    check(held == (ULONG)AMINETXDUO_EVENT_RING, "and says so");

    /* Two from test_ring_records() plus these. */
    check(out[n - 1].nse_Seq == 2UL + extra, "the last is the newest");
    check(out[n - 1].nse_Value == extra - 1UL, "and carries its value");

    /* Contiguous, oldest first, with no repeats: a ring that reports the same
       entry twice is worse than one that drops it. */
    for (i = 1; i < n; i++)
        check(out[i].nse_Seq == out[i - 1].nse_Seq + 1UL,
              "sequence numbers are contiguous");

    check(out[0].nse_Seq > 1UL, "the loss is visible in the first entry");
    check(out[0].nse_Seq - 1UL == (2UL + extra) - (ULONG)AMINETXDUO_EVENT_RING,
          "and says exactly how many went past");
}

/* A buffer smaller than the ring: what fits is written, and the caller still
   learns the size it needed. */
static void test_short_buffer(void)
{
    ULONG held = 0;
    ULONG n    = ami_event_snapshot(out, 3UL, &held);

    check(n == 3UL, "three written");
    check(held == (ULONG)AMINETXDUO_EVENT_RING, "and the size still reported");

    /* Room for none is a legitimate way to ask how many there are. */
    held = 0;
    n = ami_event_snapshot(out, 0UL, &held);
    check(n == 0UL, "nothing written");
    check(held == (ULONG)AMINETXDUO_EVENT_RING, "the count still comes back");
}

/* ------------------------------------------------------------- end to end */

/*
 * The reported complaint: NetShutdown runs, the card's LED goes on blinking,
 * and nothing on the machine says why.  Recorded here as the library would
 * record it, then read back and decoded as ShowNetStatus decodes it.
 *
 * The three sentences below are what a user would be able to quote.  If the
 * table is edited so that they no longer say this, that is a decision, and
 * this is where it has to be made deliberately.
 */
static void test_the_shutdown_a_user_reported(void)
{
    ULONG held = 0;
    ULONG n;

    /* A fresh ring is not available -- it is a static in another translation
       unit -- so this runs past the wrap above and reads the tail. */
    stub_millis = 60000;
    ami_event(NETEVENT_OUT_OF_SERVICE, 0, 0UL);
    stub_millis = 61000;
    ami_event(NETEVENT_NOTIFY, NETEVENT_NOINDEX, 2UL);
    stub_millis = 61010;
    ami_event(NETEVENT_SHUTDOWN, NETEVENT_NOINDEX, 1UL);
    stub_millis = 61020;
    ami_event(NETEVENT_OFFLINE_SKIPPED, 0, 0UL);
    stub_millis = 61030;
    ami_event(NETEVENT_IFACE_RETAINED, 0, NETEVENT_HELD_RX);
    stub_millis = 61040;
    ami_event(NETEVENT_STACK_RETAINED, NETEVENT_NOINDEX, 1UL);
    stub_millis = 61050;
    ami_event(NETEVENT_EXPUNGE_DECLINED, NETEVENT_NOINDEX,
              NETEVENT_EXP_OPEN);

    n = ami_event_snapshot(out, (ULONG)(sizeof(out) / sizeof(out[0])), &held);
    check(n >= 7UL, "the sequence is in the ring");

    if (n < 7UL)
        return;

    {
        const NetStatusEvent *e = &out[n - 7];

        check(e[0].nse_Code == NETEVENT_OUT_OF_SERVICE, "the wire went first");
        check_str(tool_event_text(e[0].nse_Code),
                  "the device went out of service and the link was marked down",
                  "the out-of-service sentence");
        check(e[0].nse_Index == 0, "on interface 0");

        check_str(tool_event_text(e[1].nse_Code),
                  "every program using the network was told it is stopping",
                  "the notify sentence");
        check_str(tool_event_detail(e[1].nse_Code), "programs signalled",
                  "what the notify value counts");
        check(e[1].nse_Value == 2UL, "two programs");

        check_str(tool_event_text(e[3].nse_Code),
                  "S2_OFFLINE was not sent, the interface was marked offline"
                  " already",
                  "the skipped-offline sentence, which is the answer");

        check_str(tool_event_text(e[4].nse_Code),
                  "the interface was left in memory, the device still holds"
                  " requests inside it",
                  "the retained-interface sentence");
        check_str(tool_event_value_name(e[4].nse_Code, e[4].nse_Value),
                  "a read", "which side the device kept");

        check_str(tool_event_text(e[6].nse_Code),
                  "the library declined to be unloaded",
                  "the expunge sentence");
        check_str(tool_event_value_name(e[6].nse_Code, e[6].nse_Value),
                  "a program still has it open", "why it declined");

        /* The times are the reader's only ordering beyond the sequence, and
           the whole sequence has to fit inside a shutdown a user watched. */
        check(e[6].nse_Tick - e[1].nse_Tick == 50UL,
              "the shutdown took 50 ms from the notify");
    }
}

int main(void)
{
    test_table_is_complete();
    test_value_names();
    test_ring_records();
    test_ring_wraps();
    test_short_buffer();
    test_the_shutdown_a_user_reported();

    if (failures != 0)
    {
        printf("%d failure(s)\n", failures);
        return 1;
    }

    printf("events: ring and table agree\n");
    return 0;
}

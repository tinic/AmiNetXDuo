#include "netswitch_dispatch.h"

#include <stdio.h>
#include <string.h>

static int failures;

static void eq(const char *what, const char *got, const char *want)
{
    if (strcmp(got, want) != 0)
    {
        fprintf(stderr, "FAIL: %s: got \"%s\", wanted \"%s\"\n", what, got, want);
        failures++;
    }
}

static void eqi(const char *what, long got, long want)
{
    if (got != want)
    {
        fprintf(stderr, "FAIL: %s: got %ld, wanted %ld\n", what, got, want);
        failures++;
    }
}

/* One definition against a small live table.  The shapes that matter are the
   ones the emulator arm never produces: a definition whose interface was
   removed, a slot another interface has reused, and a padding slot. */
static void join(void)
{
    NsLive live[4];
    long   m;

    live[0].name  = "eth0";
    live[0].named = 1;
    live[0].index = 0;
    live[0].up    = 1;
    live[1].name  = "eth1";
    live[1].named = 1;
    live[1].index = 1;
    live[1].up    = 0;
    live[2].name  = "eth0";          /* padding: same name, not a real row */
    live[2].named = 0;
    live[2].index = 5;
    live[2].up    = 1;
    live[3].name  = "eth2";
    live[3].named = 1;
    live[3].index = 2;
    live[3].up    = 1;

    eqi("join names eth0", ns_join_live("eth0", live, 4), 0);
    eqi("join names eth1", ns_join_live("eth1", live, 4), 1);
    eqi("join is case-blind", ns_join_live("ETH1", live, 4), 1);

    /* A definition whose interface was removed reads detached (-1). */
    eqi("removed reads detached", ns_join_live("eth9", live, 4), -1);

    /* A slot reused by another interface: eth0 now holds index 0, but eth1
       still joins to its own entry by name, never to the stale slot. */
    m = ns_join_live("eth1", live, 4);
    eqi("reused slot still finds eth1's entry", m, 1);
    eqi("reused slot's index is eth1's", (long)live[m].index, 1);

    /* A padding slot that happens to carry the same name never matches. */
    {
        NsLive only_padding[1];

        only_padding[0] = live[2];
        eqi("padding never matches", ns_join_live("eth0", only_padding, 1), -1);
    }

    eqi("no name is detached", ns_join_live(NULL, live, 4), -1);
    eqi("no table is detached", ns_join_live("eth0", NULL, 4), -1);
    eqi("empty table is detached", ns_join_live("eth0", live, 0), -1);
}

/* The one bulk error line names the interface and yields its errno text to
   the cap, never the other way round. */
static void named_error(void)
{
    char buf[64];

    ns_named_error(buf, sizeof(buf), "eth0", "No space left on device");
    eq("named error fits", buf, "eth0: No space left on device");

    ns_named_error(buf, sizeof(buf), "eth0", NULL);
    eq("named error without errno", buf, "eth0");
    ns_named_error(buf, sizeof(buf), "eth0", "");
    eq("named error with empty errno", buf, "eth0");

    /* A long errno text is cut, but the name and the separator are not. */
    ns_named_error(buf, sizeof(buf), "anxzz9000.device 0",
                   "A very long error message that runs well past the line");
    eqi("named error keeps the name",
        strncmp(buf, "anxzz9000.device 0: ",
                strlen("anxzz9000.device 0: ")) == 0 ? 0 : 1, 0);
    eqi("named error fills the cap", (long)strlen(buf),
        (long)(sizeof(buf) - 1));

    /* A cap too small for the separator drops the errno text rather than
       gluing it to the bare name. */
    ns_named_error(buf, 6, "eth0", "boom");
    eq("named error tight cap", buf, "eth0");
}

/* The gates refuse exactly after a failed live-table read, so the buttons
   cannot act on state that has gone stale. */
static void gates(void)
{
    eqi("action with a good read", ns_can_act(1, 3, 0), 1);
    eqi("action refuses a failed read", ns_can_act(1, 3, 1), 0);
    eqi("action refuses with no stack", ns_can_act(0, 3, 0), 0);
    eqi("action refuses an empty list", ns_can_act(1, 0, 0), 0);

    eqi("take-all with one online", ns_can_take_all(1, 0, 1), 1);
    eqi("take-all refuses a failed read", ns_can_take_all(1, 1, 1), 0);
    eqi("take-all refuses none online", ns_can_take_all(1, 0, 0), 0);
    eqi("take-all refuses no stack", ns_can_take_all(0, 0, 1), 0);
}

/* The click maps a state to the action it asks for, and the re-read only
   answers for the index: when the fresh state no longer asks for the intended
   action, the click is refused rather than reversed. */
static void dispatch(void)
{
    eqi("up asks Go Offline", (long)ns_op_for_state(NS_STATE_UP),
        (long)NS_OP_DOWN);
    eqi("down asks Go Online", (long)ns_op_for_state(NS_STATE_DOWN),
        (long)NS_OP_UP);
    eqi("detached asks Online", (long)ns_op_for_state(NS_STATE_DETACHED),
        (long)NS_OP_ADD);

    /* The fresh state still asks for the same action: act. */
    eqi("Go Offline on a still-up row", (long)ns_dispatch_action(NS_OP_DOWN,
        NS_STATE_UP), (long)NS_OP_DOWN);
    eqi("Go Online on a still-down row", (long)ns_dispatch_action(NS_OP_UP,
        NS_STATE_DOWN), (long)NS_OP_UP);
    eqi("Online on a still-detached row", (long)ns_dispatch_action(NS_OP_ADD,
        NS_STATE_DETACHED), (long)NS_OP_ADD);

    /* The state moved under the click: the button said one thing, the fresh
       state says the opposite, and acting on the fresh state would send the
       reverse of what the user clicked.  Refuse. */
    eqi("Go Offline refused when already down",
        (long)ns_dispatch_action(NS_OP_DOWN, NS_STATE_DOWN), (long)NS_OP_NONE);
    eqi("Go Online refused when already up",
        (long)ns_dispatch_action(NS_OP_UP, NS_STATE_UP), (long)NS_OP_NONE);
    eqi("Online refused once attached up",
        (long)ns_dispatch_action(NS_OP_ADD, NS_STATE_UP), (long)NS_OP_NONE);
    eqi("Online refused once attached down",
        (long)ns_dispatch_action(NS_OP_ADD, NS_STATE_DOWN), (long)NS_OP_NONE);

    /* No intended action is never promoted to one. */
    eqi("no intended action stays none",
        (long)ns_dispatch_action(NS_OP_NONE, NS_STATE_UP), (long)NS_OP_NONE);
}

int main(void)
{
    join();
    named_error();
    gates();
    dispatch();

    if (failures == 0) printf("netswitch dispatch: all checks passed\n");
    return failures != 0;
}

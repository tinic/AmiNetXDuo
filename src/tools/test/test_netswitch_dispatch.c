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

static void eqcall(const char *what, NsCall got, NsDispatchOp op,
                   unsigned index)
{
    if (got.op != op || got.index != index)
    {
        fprintf(stderr,
                "FAIL: %s: got {op=%d, index=%u}, wanted {op=%d, index=%u}\n",
                what, (int)got.op, got.index, (int)op, index);
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

    /* A target that was removed re-reads detached: neither switch op can send
       to a stale index, so both are refused. */
    eqi("Go Offline refused on a removed target",
        (long)ns_dispatch_action(NS_OP_DOWN, NS_STATE_DETACHED),
        (long)NS_OP_NONE);
    eqi("Go Online refused on a removed target",
        (long)ns_dispatch_action(NS_OP_UP, NS_STATE_DETACHED), (long)NS_OP_NONE);

    /* No intended action is never promoted to one. */
    eqi("no intended action stays none",
        (long)ns_dispatch_action(NS_OP_NONE, NS_STATE_UP), (long)NS_OP_NONE);

    /* The index routes through too, so the test asserts the whole control
       call -- DOWN at index 0, UP at index 1 -- and not only the op. */
    eqcall("Go Offline on eth0 sends DOWN at index 0",
           ns_resolve_call(NS_OP_DOWN, NS_STATE_UP, 0), NS_OP_DOWN, 0);
    eqcall("Go Online on eth1 sends UP at index 1",
           ns_resolve_call(NS_OP_UP, NS_STATE_DOWN, 1), NS_OP_UP, 1);
    eqcall("a reversed click sends no call",
           ns_resolve_call(NS_OP_DOWN, NS_STATE_DOWN, 0), NS_OP_NONE, 0);
    eqcall("an already-up click sends no call",
           ns_resolve_call(NS_OP_UP, NS_STATE_UP, 3), NS_OP_NONE, 0);
    eqcall("attach acts by name, index unused",
           ns_resolve_call(NS_OP_ADD, NS_STATE_DETACHED, 5), NS_OP_ADD, 0);
    eqcall("a removed target sends no switch call",
           ns_resolve_call(NS_OP_DOWN, NS_STATE_DETACHED, 0), NS_OP_NONE, 0);
}

/* Recording stubs for ns_run_action(): they stand in for refresh_rows() and
   control_switch(), counting and capturing each call so the test asserts the
   call effects the emulator arm cannot reach -- a failed read, a removed
   target, a reused slot. */
struct recorder
{
    NsState     state;                  /* first reread's answer */
    unsigned    index;
    NsState     second_state;           /* second reread's answer (EEXIST) */
    unsigned    second_index;
    int         read_fails;
    int         second_read_fails;
    long        attach_errno;
    int         reads;
    int         downs;
    int         ups;
    int         attaches;
    unsigned    down_index;
    unsigned    up_index;
    const char *attach_name;
    const char *down_err;
    const char *up_err;
};

static int rec_reread(void *ctx, NsState *state, unsigned *index)
{
    struct recorder *r = ctx;

    r->reads++;
    if ((r->reads == 1 && r->read_fails) ||
        (r->reads == 2 && r->second_read_fails))
        return 1;
    if (r->reads == 1)
    {
        *state = r->state;
        *index = r->index;
    }
    else
    {
        *state = r->second_state;
        *index = r->second_index;
    }
    return 0;
}

static const char *rec_down(void *ctx, unsigned index)
{
    struct recorder *r = ctx;

    r->downs++;
    r->down_index = index;
    return r->down_err;
}

static const char *rec_up(void *ctx, unsigned index)
{
    struct recorder *r = ctx;

    r->ups++;
    r->up_index = index;
    return r->up_err;
}

static long rec_attach(void *ctx, const char *name)
{
    struct recorder *r = ctx;

    r->attaches++;
    r->attach_name = name;
    return r->attach_errno;
}

static const char *rec_errtext(long err)
{
    if (err == 17) return "EEXIST";
    if (err == 16) return "EBUSY";
    if (err == 22) return "EINVAL";
    return "errno";
}

/* The call effect: which device call ns_run_action() turns an action into,
   or that none goes out on a failed read or a moved target. */
static void action(void)
{
    static const char *const name = "anxzz9000.device 0";
    struct recorder rec;
    NsActionHooks   hooks;
    NsActionResult  ar;

    hooks.ctx     = &rec;
    hooks.reread  = rec_reread;
    hooks.down    = rec_down;
    hooks.up      = rec_up;
    hooks.attach  = rec_attach;
    hooks.errtext = rec_errtext;

    /* A failed re-read refuses before any call: the rows are stale and
       neither a switch nor an attach may go out on them. */
    memset(&rec, 0, sizeof(rec));
    rec.read_fails = 1;
    ar = ns_run_action(NS_OP_DOWN, name, &hooks);
    eqi("failed read refuses", ar.acted, 0);
    eq("failed read says the read failed", ar.hint, ns_query_failed_hint);
    eqi("failed read makes no call", rec.downs + rec.ups + rec.attaches, 0);

    /* A target that moved under the click is refused: the button said Go
       Offline, the fresh state is detached (the interface was removed), and
       acting would be a guess. */
    memset(&rec, 0, sizeof(rec));
    rec.state = NS_STATE_DETACHED;
    rec.index = 0xFFFF;
    ar = ns_run_action(NS_OP_DOWN, name, &hooks);
    eqi("moved target refuses", ar.acted, 0);
    eq("moved target says it moved", ar.hint, ns_state_moved_error);
    eqi("moved target makes no call", rec.downs + rec.ups + rec.attaches, 0);

    /* A still-up row takes the intended Go Offline: one DOWN call at the
       fresh index, nothing else. */
    memset(&rec, 0, sizeof(rec));
    rec.state = NS_STATE_UP;
    rec.index = 3;
    ar = ns_run_action(NS_OP_DOWN, name, &hooks);
    eqi("Go Offline acts", ar.acted, 1);
    eqi("Go Offline sends one DOWN", rec.downs, 1);
    eqi("Go Offline sends the fresh index", (long)rec.down_index, 3);
    eqi("Go Offline sends nothing else", rec.ups + rec.attaches, 0);

    /* A still-down row takes Go Online: one UP call. */
    memset(&rec, 0, sizeof(rec));
    rec.state = NS_STATE_DOWN;
    rec.index = 2;
    ar = ns_run_action(NS_OP_UP, name, &hooks);
    eqi("Go Online acts", ar.acted, 1);
    eqi("Go Online sends one UP", rec.ups, 1);
    eqi("Go Online sends the fresh index", (long)rec.up_index, 2);
    eqi("Go Online sends nothing else", rec.downs + rec.attaches, 0);

    /* A switch failure propagates its errno text. */
    memset(&rec, 0, sizeof(rec));
    rec.state = NS_STATE_UP;
    rec.down_err = "EBUSY";
    ar = ns_run_action(NS_OP_DOWN, name, &hooks);
    eqi("switch error acts", ar.acted, 1);
    eq("switch error propagates", ar.err, "EBUSY");

    /* A still-detached row takes Online: one attach by name, no switch. */
    memset(&rec, 0, sizeof(rec));
    rec.state = NS_STATE_DETACHED;
    rec.attach_errno = 0;
    ar = ns_run_action(NS_OP_ADD, name, &hooks);
    eqi("Online acts", ar.acted, 1);
    eqi("Online sends one attach", rec.attaches, 1);
    eq("Online attaches by name", rec.attach_name, name);
    eqi("Online sends no switch", rec.downs + rec.ups, 0);
    eqi("Online success has no error", ar.err == NULL, 1);

    /* Attach reports EEXIST: another tool attached it in the meantime, so
       the path re-reads for the fresh index and brings it up. */
    memset(&rec, 0, sizeof(rec));
    rec.state = NS_STATE_DETACHED;          /* first read: still detached */
    rec.attach_errno = 17;                  /* 4.4BSD EEXIST */
    rec.second_state = NS_STATE_UP;         /* second read: now attached */
    rec.second_index = 5;
    ar = ns_run_action(NS_OP_ADD, name, &hooks);
    eqi("EEXIST re-reads", rec.reads, 2);
    eqi("EEXIST then ups", rec.ups, 1);
    eqi("EEXIST ups the fresh index", (long)rec.up_index, 5);
    eqi("EEXIST attaches once", rec.attaches, 1);
    eqi("EEXIST sends no down", rec.downs, 0);

    /* A non-EEXIST attach failure names its errno and sends no switch. */
    memset(&rec, 0, sizeof(rec));
    rec.state = NS_STATE_DETACHED;
    rec.attach_errno = 22;                  /* EINVAL */
    ar = ns_run_action(NS_OP_ADD, name, &hooks);
    eqi("attach error acts", ar.acted, 1);
    eq("attach error names the errno", ar.err, "EINVAL");
    eqi("attach error sends no switch", rec.downs + rec.ups, 0);

    /* EEXIST then a failed re-read refuses the up with the read hint. */
    memset(&rec, 0, sizeof(rec));
    rec.state = NS_STATE_DETACHED;
    rec.attach_errno = 17;
    rec.second_read_fails = 1;
    ar = ns_run_action(NS_OP_ADD, name, &hooks);
    eqi("EEXIST failed re-read acts", ar.acted, 1);
    eq("EEXIST failed re-read names the read", ar.err, ns_query_failed_hint);
    eqi("EEXIST failed re-read sends no up", rec.ups, 0);
}

/* Recording stubs for ns_bulk_down(): the take-all loop's rows and switches,
   so the test asserts the bulk call effects the emulator arm cannot reach --
   a failed read, a removed row, a partial failure. */
struct bulkrec
{
    NsBulkRow    rows[4];
    unsigned     nrows;
    int          read_fails;
    int          reads;
    int          downs;
    unsigned     down_index[4];
    const char  *down_errs[4];      /* per-call errno text, or NULL */
};

static int bulk_reread(void *ctx, NsBulkRow *rows, unsigned count)
{
    struct bulkrec *b = ctx;
    unsigned        i;

    b->reads++;
    if (b->read_fails)
        return 1;
    for (i = 0; i < count && i < b->nrows; i++)
        rows[i] = b->rows[i];
    return 0;
}

static int bulk_is_loopback(void *ctx, const char *name)
{
    (void)ctx;
    return strcmp(name, "loopback") == 0;
}

static const char *bulk_down(void *ctx, unsigned index)
{
    struct bulkrec *b = ctx;

    b->down_index[b->downs] = index;
    return b->down_errs[b->downs++];
}

/* The take-all call effect: which rows get a DOWN switch, and which one a
   partial failure reports. */
static void bulk(void)
{
    char           first[64];
    NsBulkHooks    hooks;
    NsActionResult ar;
    unsigned       switched = 0;
    struct bulkrec b;

    hooks.ctx         = &b;
    hooks.reread      = bulk_reread;
    hooks.is_loopback = bulk_is_loopback;
    hooks.down        = bulk_down;

    /* A failed re-read refuses before any switch. */
    memset(&b, 0, sizeof(b));
    b.read_fails = 1;
    ar = ns_bulk_down(&hooks, b.rows, 3, first, sizeof(first), &switched);
    eqi("bulk failed read refuses", ar.acted, 0);
    eq("bulk failed read says the read failed", ar.hint, ns_query_failed_hint);
    eqi("bulk failed read makes no switch", b.downs, 0);

    /* A removed interface re-reads detached and a down one is not up: both
       are skipped, so no stale-index switch goes out. */
    memset(&b, 0, sizeof(b));
    b.nrows = 3;
    b.rows[0] = (NsBulkRow){ "eth0", NS_STATE_UP, 0 };
    b.rows[1] = (NsBulkRow){ "eth1", NS_STATE_DETACHED, 0xFFFF };
    b.rows[2] = (NsBulkRow){ "eth2", NS_STATE_DOWN, 7 };
    ar = ns_bulk_down(&hooks, b.rows, 3, first, sizeof(first), &switched);
    eqi("bulk skips detached and down", b.downs, 1);
    eqi("bulk switches only the up row", (long)b.down_index[0], 0);
    eqi("bulk counts one switch", (long)switched, 1);
    eqi("bulk reports no error", ar.err == NULL, 1);

    /* The loopback definition is skipped even when up. */
    memset(&b, 0, sizeof(b));
    b.nrows = 2;
    b.rows[0] = (NsBulkRow){ "loopback", NS_STATE_UP, 0 };
    b.rows[1] = (NsBulkRow){ "eth0", NS_STATE_UP, 1 };
    ar = ns_bulk_down(&hooks, b.rows, 2, first, sizeof(first), &switched);
    eqi("bulk skips loopback", b.downs, 1);
    eqi("bulk switches the real row", (long)b.down_index[0], 1);

    /* A partial bulk continues past the first failure and reports it as
       "name: err", keeping the first failed interface and dropping later
       failures. */
    memset(&b, 0, sizeof(b));
    b.nrows = 3;
    b.rows[0] = (NsBulkRow){ "eth0", NS_STATE_UP, 0 };
    b.rows[1] = (NsBulkRow){ "eth1", NS_STATE_UP, 1 };
    b.rows[2] = (NsBulkRow){ "eth2", NS_STATE_UP, 2 };
    b.down_errs[0] = NULL;
    b.down_errs[1] = "EBUSY";
    b.down_errs[2] = "EPERM";
    ar = ns_bulk_down(&hooks, b.rows, 3, first, sizeof(first), &switched);
    eqi("partial bulk switches all three", b.downs, 3);
    eqi("partial bulk counts all three", (long)switched, 3);
    eq("partial bulk reports the first failure", ar.err, "eth1: EBUSY");
}

/* Coverage boundary: the join, gates, wording, and op/index routing are the
   *decision*; ns_run_action() and ns_bulk_down() above are the *call effect*
   -- which device calls the decision turns into, or that none goes out on a
   failed read or a moved/removed target.  All run here.  What remains to the
   emulator arm (run-wbgrab.sh -s netswitch-reverse) is the Amiga-side
   plumbing in netswitch.c: the hooks that hand refresh_rows() and
   control_switch() to the pure functions, and the repaint after a call. */
int main(void)
{
    join();
    named_error();
    gates();
    dispatch();
    action();
    bulk();

    if (failures == 0) printf("netswitch dispatch: all checks passed\n");
    return failures != 0;
}

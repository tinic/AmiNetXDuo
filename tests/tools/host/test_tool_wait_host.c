/* Regression tests for the shipping tools' DHCP readiness wait. */

#include <stdio.h>

static const LONG  *states;
static const ULONG *addresses;
static unsigned     state_count;
static unsigned     state_at;
static unsigned     delays;
static BOOL         break_delay;
static int          failures;

#define CHECK(expr, message)                                                \
    do                                                                       \
    {                                                                        \
        if (!(expr))                                                         \
        {                                                                    \
            fprintf(stderr, "FAIL: %s\n", message);                         \
            failures++;                                                      \
        }                                                                    \
    } while (0)

LONG tool_netstatus_dhcp_state(struct Library *base, UWORD index,
                               ULONG *addr_out)
{
    unsigned at = state_at;

    (void)base;
    CHECK(index == 2, "the requested interface index is retained");

    if (at >= state_count)
        at = state_count - 1;
    if (addr_out != NULL)
        *addr_out = addresses[at];
    state_at++;
    return states[at];
}

BOOL tool_delay_ticks(ULONG ticks)
{
    CHECK(ticks == TICKS_PER_SECOND, "the wait advances one second at a time");
    delays++;
    return break_delay;
}

static VOID begin(const LONG *next_states, const ULONG *next_addresses,
                  unsigned count)
{
    states      = next_states;
    addresses   = next_addresses;
    state_count = count;
    state_at    = 0;
    delays      = 0;
    break_delay = FALSE;
}

static VOID delayed_bound_ignores_addresses(VOID)
{
    static const LONG sequence[] = {
        NETSTATUS_DHCP_OFF,
        NETSTATUS_DHCP_WORKING,
        NETSTATUS_DHCP_WORKING,
        NETSTATUS_DHCP_BOUND
    };
    static const ULONG addr[] = {
        0, 0xA9FE0101UL, 0x0A00020FUL, 0x0A00020FUL
    };
    ToolWait wait;
    ULONG    got = 0;

    begin(sequence, addr, 4);
    tool_wait_init(&wait, 10);

    CHECK(tool_wait_dhcp_bound(NULL, 2, &wait, &got),
          "the delayed lease eventually becomes ready");
    CHECK(state_at == 4, "OFF and WORKING are not mistaken for readiness");
    CHECK(delays == 3, "the wait spans all three pre-BOUND states");
    CHECK(wait.elapsed == 3, "elapsed time records the shared allowance");
    CHECK(got == 0x0A00020FUL, "the BOUND row supplies the reported address");
}

static VOID zero_is_unlimited(VOID)
{
    static const LONG sequence[] = {
        NETSTATUS_DHCP_WORKING,
        NETSTATUS_DHCP_WORKING,
        NETSTATUS_DHCP_BOUND
    };
    static const ULONG addr[] = { 0, 0, 0xC0000201UL };
    ToolWait wait;
    ULONG    got = 0;

    begin(sequence, addr, 3);
    tool_wait_init(&wait, 0);

    CHECK(tool_wait_dhcp_bound(NULL, 2, &wait, &got),
          "TIMEOUT 0 waits through WORKING to BOUND");
    CHECK(delays == 2 && wait.elapsed == 2,
          "TIMEOUT 0 does not expire at elapsed zero");
}

static VOID finite_allowance_is_shared(VOID)
{
    static const LONG sequence[] = { NETSTATUS_DHCP_WORKING };
    static const ULONG addr[] = { 0xA9FE0101UL };
    ToolWait wait;
    ULONG    got = 0;

    begin(sequence, addr, 1);
    tool_wait_init(&wait, 3);
    wait.elapsed = 1;                    /* the link transition spent one */

    CHECK(!tool_wait_dhcp_bound(NULL, 2, &wait, &got),
          "WORKING past the total allowance times out");
    CHECK(delays == 2 && wait.elapsed == 3,
          "DHCP receives only the unspent part of TIMEOUT");
    CHECK(!wait.broken, "an ordinary timeout is not reported as Ctrl-C");
}

static VOID break_is_distinct_from_timeout(VOID)
{
    static const LONG sequence[] = { NETSTATUS_DHCP_WORKING };
    static const ULONG addr[] = { 0 };
    ToolWait wait;
    ULONG    got = 0;

    begin(sequence, addr, 1);
    tool_wait_init(&wait, 10);
    break_delay = TRUE;

    CHECK(!tool_wait_dhcp_bound(NULL, 2, &wait, &got),
          "Ctrl-C stops the wait");
    CHECK(wait.broken, "Ctrl-C is preserved for the command's return code");
    CHECK(wait.elapsed == 0, "an interrupted second is not charged as elapsed");
}

int main(void)
{
    delayed_bound_ignores_addresses();
    zero_is_unlimited();
    finite_allowance_is_shared();
    break_is_distinct_from_timeout();

    if (failures != 0)
        return 1;

    puts("DHCP readiness wait host tests passed");
    return 0;
}

/*
 * netstack_interface_dhcp_start() and its re-arm.
 *
 * WHY THIS EXISTS.  The function has two arms.  The one every lab run takes is
 * a first start on an interface the client has never seen, and it answers
 * NX_SUCCESS.  The other is NX_DHCP_ALREADY_STARTED, which means the client's
 * own record says the interface is running while ns_DhcpState[] says it is
 * not: the record is armed, no DISCOVER is on the way, and returning that
 * status would leave the interface with no address and nothing trying to get
 * one.  The shipping code stops the interface and starts it again.
 *
 * Nothing exercised that.  A DHCP client only reaches the disagreeing state
 * after a stop that the state table did not see, and the lab cannot produce
 * one on demand -- the segment's server would have to be driven into it.  Here
 * the client answers ALREADY_STARTED when the test says so.
 *
 * netstack.c is compiled whole; see tests/netstack/host/netstack_host_env.h.
 *
 * SPDX-License-Identifier: MIT
 */

#include "netstack_host_env.h"

#include "aminetxduo/netstack.h"

#include <stdio.h>
#include <string.h>

static unsigned long h_checks;
static unsigned long h_failures;

#define CHECK(cond, what)                                                     \
    do {                                                                      \
        h_checks++;                                                           \
        if (!(cond)) {                                                        \
            h_failures++;                                                     \
            printf("  FAIL %s\n", (what));                                    \
        }                                                                     \
    } while (0)

/*
 * A machine that is up with a static address, so the DHCP client has not been
 * created and nothing has written the trace.  netstack_interface_dhcp_start()
 * is then the first thing to touch it, which is the shape the ARexx and
 * NETCTRL callers arrive in.
 */
static void h_up_static(void)
{
    nsh_reset();
    nsh.cfg_iptype = (UWORD)AMI_IPTYPE_STATIC;

    if (netstack_startup() != AMI_NET_OK)
    {
        printf("  FAIL the stack did not come up\n");
        h_failures++;
    }

    nsh_trace_clear();
    nsh.dhcp_starts    = 0;
    nsh.dhcp_stops     = 0;
    nsh.dhcp_enables   = 0;
    nsh.dhcp_discovers = 0;
}

static void h_down(void)
{
    nsh.tx_stop_status = TX_SUCCESS;
    netstack_shutdown();
    netstack_shutdown();
}

/*
 * The arm that runs today, so the re-arm below is measured against something.
 */
static void t_first_start(void)
{
    printf("dhcp restart: a first start\n");

    h_up_static();

    CHECK(netstack_interface_dhcp_start(0, 0UL) == AMI_NET_OK,
          "the interface starts");
    CHECK(nsh_dhcp_trace_is("esd"),
          "enabled, started, and the DISCOVER timer re-armed, in that order");
    CHECK(nsh.dhcp_stops == 0, "nothing was stopped");
    CHECK(netstack_get()->ns_DhcpStarted == TRUE,
          "the client is recorded as started");
    CHECK(netstack_get()->ns_Config.interfaces[0].iptype == AMI_IPTYPE_DHCP,
          "and the interface is now a DHCP one");

    h_down();
}

/*
 * A requested address is a wish, not a demand: skip_discover is 0, so DISCOVER
 * still goes out and a server that disagrees offers something else instead of
 * answering NAK.
 */
static void t_requested_address(void)
{
    printf("dhcp restart: with a requested address\n");

    h_up_static();

    CHECK(netstack_interface_dhcp_start(0, 0x0A000042UL) == AMI_NET_OK,
          "the interface starts");
    CHECK(nsh_dhcp_trace_is("ersd"),
          "the address is requested between the enable and the start");
    CHECK(nsh.dhcp_request_addr == 0x0A000042UL, "the address asked for");
    CHECK(nsh.dhcp_request_skip == 0,
          "skip_discover is 0, so the wish does not become a demand");

    h_down();
}

/*
 * THE RE-ARM.  The client answers ALREADY_STARTED, so the shipping code stops
 * the interface and starts it again rather than returning with the record
 * armed and no DISCOVER on the wire.
 */
static void t_already_started_rearms(void)
{
    printf("dhcp restart: ALREADY_STARTED is stopped and started again\n");

    h_up_static();
    nsh.dhcp_start_status = NX_DHCP_ALREADY_STARTED;

    CHECK(netstack_interface_dhcp_start(0, 0UL) == AMI_NET_OK,
          "the restart succeeds");
    CHECK(nsh.dhcp_starts == 2, "the start was attempted twice");
    CHECK(nsh.dhcp_stops == 1, "with one stop between them");
    CHECK(nsh_dhcp_trace_is("esxsd"),
          "enable, start, stop, start, DISCOVER: the record is re-armed and a "
          "DISCOVER leaves");
    CHECK(nsh.dhcp_discovers == 1,
          "the DISCOVER kick happens once, after the second start");
    CHECK(netstack_get()->ns_DhcpStarted == TRUE,
          "and the client is recorded as started");

    h_down();
}

/*
 * The re-arm with an address wish: the request is made once, before the first
 * start, and the stop does not lose it.
 */
static void t_already_started_with_address(void)
{
    printf("dhcp restart: ALREADY_STARTED with a requested address\n");

    h_up_static();
    nsh.dhcp_start_status = NX_DHCP_ALREADY_STARTED;

    CHECK(netstack_interface_dhcp_start(0, 0x0A000042UL) == AMI_NET_OK,
          "the restart succeeds");
    CHECK(nsh.dhcp_requests == 1, "the address is requested once");
    CHECK(nsh_dhcp_trace_is("ersxsd"),
          "and before the first of the two starts");

    h_down();
}

/*
 * The re-arm can fail too.  Then the call reports it, and nothing claims the
 * client is running.
 */
static void t_rearm_fails(void)
{
    printf("dhcp restart: the second start fails as well\n");

    h_up_static();
    nsh.dhcp_start_status   = NX_DHCP_ALREADY_STARTED;
    nsh.dhcp_restart_status = NX_DHCP_NOT_STARTED;

    CHECK(netstack_interface_dhcp_start(0, 0UL) == AMI_NET_ERR_STATE,
          "a failed re-arm is reported");
    CHECK(nsh.dhcp_starts == 2, "both starts were attempted");
    CHECK(nsh.dhcp_stops == 1, "and the stop between them happened");
    CHECK(nsh.dhcp_discovers == 0,
          "no DISCOVER is kicked for a client that did not start");
    CHECK(netstack_get()->ns_DhcpStarted == FALSE,
          "and the client is not recorded as started");
    CHECK(netstack_get()->ns_Config.interfaces[0].iptype == AMI_IPTYPE_STATIC,
          "nor is the interface turned into a DHCP one");

    h_down();
}

/*
 * A first start that fails for some other reason is NOT retried: the re-arm
 * is for the disagreeing record only.
 */
static void t_other_failure_is_not_retried(void)
{
    printf("dhcp restart: another failure is not a restart\n");

    h_up_static();
    nsh.dhcp_start_status = NX_PTR_ERROR;

    CHECK(netstack_interface_dhcp_start(0, 0UL) == AMI_NET_ERR_STATE,
          "the failure is reported");
    CHECK(nsh.dhcp_starts == 1, "the start was attempted once");
    CHECK(nsh.dhcp_stops == 0, "and nothing was stopped");

    h_down();
}

/*
 * The refusals around it, so the re-arm cannot be reached by accident.
 */
static void t_refusals(void)
{
    printf("dhcp restart: the refusals\n");

    nsh_reset();

    CHECK(netstack_interface_dhcp_start(0, 0UL) == AMI_NET_ERR_STATE,
          "no stack, no start");
    CHECK(nsh.dhcp_starts == 0, "and the client was not touched");

    h_up_static();

    CHECK(netstack_interface_dhcp_start((UWORD)AMI_CFG_MAX_ATTACHED, 0UL)
              == AMI_NET_ERR_STATE,
          "an index past the attached slots is refused");
    CHECK(netstack_interface_dhcp_start(1, 0UL) == AMI_NET_ERR_STATE,
          "so is a slot with no interface in it");
    CHECK(nsh.dhcp_starts == 0, "neither reached the client");

    /* Already working: the state table, not the client, is what says so. */
    CHECK(netstack_interface_dhcp_start(0, 0UL) == AMI_NET_OK, "one start");
    netstack_get()->ns_DhcpState[0] = NX_DHCP_STATE_SELECTING;

    {
        ULONG starts = nsh.dhcp_starts;

        CHECK(netstack_interface_dhcp_start(0, 0UL) == AMI_NET_ERR_BUSY,
              "an interface already working is busy, not restarted");
        CHECK(nsh.dhcp_starts == starts, "and the client was not touched");
    }

    /* Bound is not working, so a start is allowed again: that is what a
       release-and-renew does. */
    netstack_get()->ns_DhcpState[0] = NX_DHCP_STATE_BOUND;
    CHECK(netstack_interface_dhcp_start(0, 0UL) == AMI_NET_OK,
          "a bound interface can be started again");

    h_down();
}

/*
 * nx_dhcp_create() failing has to come back as a refusal rather than as a
 * start on a client that does not exist.
 */
static void t_create_fails(void)
{
    printf("dhcp restart: the client cannot be created\n");

    h_up_static();
    nsh.dhcp_create_status = NX_PTR_ERROR;

    CHECK(netstack_interface_dhcp_start(0, 0UL) == AMI_NET_ERR_STATE,
          "the create failure is reported");
    CHECK(nsh.dhcp_enables == 0, "and nothing was enabled");
    CHECK(nsh.dhcp_starts == 0, "or started");

    h_down();
}

int main(void)
{
    printf("netstack DHCP restart host checks\n\n");

    t_first_start();
    t_requested_address();
    t_already_started_rearms();
    t_already_started_with_address();
    t_rearm_fails();
    t_other_failure_is_not_retried();
    t_refusals();
    t_create_fails();

    printf("\n%lu checks, %lu failures\n", h_checks, h_failures);

    return (h_failures == 0) ? 0 : 1;
}

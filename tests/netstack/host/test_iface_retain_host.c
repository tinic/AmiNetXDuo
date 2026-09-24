/*
 * An interface whose SANA-II device keeps requests in it, at the netstack.
 *
 * ami_sana2_close() answering FALSE used to be ignored by the removal: the
 * slot was cleared and the interface forgotten while a CMD_WRITE still
 * pointed into it, ami_ns_destroy() could no longer see it, and
 * netstack_can_unload() let the hunk holding the copy hooks go.  src/sana2 now
 * keeps such an interface on a retained list (modelled by
 * netstack_host_env.c, which follows the contract in aminetxduo/sana2.h).
 * What this file holds the netstack to: the removal says so, the same device
 * and unit is not opened again under it, the stack's pool is kept, the sweep
 * runs where it should, and the library stays loaded until the last hold
 * clears.  src/sana2's own list is tests/sana2/host/test_sana2_device_host.c.
 *
 * netstack.c is compiled whole; see tests/netstack/host/netstack_host_env.h.
 *
 * SPDX-License-Identifier: MIT
 */

#include "netstack_host_env.h"

#include "aminetxduo/netstack.h"
#include "aminetxduo/netstatus.h"

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

/* A stack kept because a device holds requests into its pool is never freed:
   the harness keeps it reachable, as the retained interface does. */
static AmiNetStack *volatile h_graveyard[8];
static unsigned              h_graves;

static void h_bury(AmiNetStack *ns)
{
    if (ns != NULL && h_graves < 8)
        h_graveyard[h_graves++] = ns;
}

static void h_iface(AmiIfConfig *cfg, const char *name, const char *device,
                    ULONG unit)
{
    memset(cfg, 0, sizeof(*cfg));
    strcpy(cfg->name, name);
    strcpy(cfg->device, device);
    cfg->unit       = unit;
    cfg->iptype     = AMI_IPTYPE_STATIC;
    cfg->address    = 0xC0A80105UL;
    cfg->netmask    = 0xFFFFFF00UL;
    cfg->up         = TRUE;
    cfg->configured = TRUE;
}

/* The loopback stack with one interface in slot 0. */
static BOOL h_up_with(const char *device, ULONG unit)
{
    AmiIfConfig cfg;
    UWORD       index = 99;

    nsh_reset();
    if (netstack_startup_loopback() != AMI_NET_OK)
        return FALSE;

    h_iface(&cfg, "eth", device, unit);
    return (netstack_interface_start(&cfg, &index) == AMI_NET_OK &&
            index == 0) ? TRUE : FALSE;
}

/* Everything back and every reference given: what the next test starts on. */
static void h_teardown(void)
{
    UWORD i;

    for (i = 0; i < (UWORD)NSH_SANA2_IFACES; i++)
    {
        nsh.sana2[i].held         = 0;
        nsh.sana2[i].join_pending = FALSE;
    }
    nsh.sana2_close_held = 0;
    nsh.sana2_close_join = FALSE;
    nsh.tx_stop_status   = TX_SUCCESS;

    netstack_shutdown();
    netstack_shutdown();
}

/* A write the device keeps past nx_ip_interface_detach(): removed, retained. */
static void t_remove_keeps_a_held_write(void)
{
    NshSana2If *m = &nsh.sana2[0];
    ULONG       closes;

    printf("retain: a removal whose device keeps a write\n");

    CHECK(h_up_with("a2065.device", 0), "the interface is up in slot 0");

    nsh.sana2_close_held = NETEVENT_HELD_TX;
    CHECK(netstack_interface_remove(0, TRUE) == AMI_NET_ERR_RETAINED,
          "the removal answers AMI_NET_ERR_RETAINED, not success");
    CHECK(nsh.iface_detaches >= 1, "the interface did leave NetX Duo");
    CHECK(m->state == NSH_IF_RETAINED, "src/sana2 holds it on its list");
    CHECK(m->device_closes == 0 && m->frees == 0,
          "no CloseDevice(), and its memory is not freed");
    CHECK(nsh.iface_retained_events >= 1,
          "NETEVENT_IFACE_RETAINED was recorded");

    closes = nsh.sana2_closes;
    CHECK(netstack_interface_remove(0, TRUE) == AMI_NET_ERR_STATE,
          "a second removal finds the slot empty");
    CHECK(nsh.sana2_closes == closes && nsh.sana2_close_twice == 0,
          "and closes nothing a second time");

    h_teardown();
    CHECK(m->state == NSH_IF_FREE && m->device_closes == 1 && m->frees == 1,
          "given back, it is closed and freed exactly once");
}

/* The same device and unit, added while held; then another device. */
static void t_add_same_unit_refused(void)
{
    AmiIfConfig cfg;
    UWORD       index = 99;
    ULONG       opens;
    ULONG       sweeps;
    ULONG       attaches;

    printf("retain: adding the held unit, and another device\n");

    CHECK(h_up_with("a2065.device", 0), "the interface is up in slot 0");
    nsh.sana2_close_held = NETEVENT_HELD_RX | NETEVENT_HELD_TX;
    CHECK(netstack_interface_remove(0, TRUE) == AMI_NET_ERR_RETAINED,
          "removed with a read and a write held");
    nsh.sana2_close_held = 0;

    opens    = nsh.sana2_opens;
    sweeps   = nsh.sweeps;
    attaches = nsh.iface_attaches;
    h_iface(&cfg, "eth", "a2065.device", 0);
    CHECK(netstack_interface_start(&cfg, &index) == AMI_NET_ERR_RETAINED,
          "the same device and unit is refused with AMI_NET_ERR_RETAINED");
    CHECK(nsh.sweeps > sweeps, "the add swept the list first");
    CHECK(nsh.sana2_opens == opens, "and never asked for OpenDevice()");
    CHECK(nsh.iface_attaches == attaches, "and attached nothing");
    CHECK(nsh.last_event == NETEVENT_IFACE_RETAINED &&
          nsh.last_event_index == NETEVENT_NOINDEX &&
          nsh.last_event_value == (NETEVENT_HELD_RX | NETEVENT_HELD_TX),
          "and said which unit held what, in the event ring");

    h_iface(&cfg, "eth1", "a2065.device", 1);
    CHECK(netstack_interface_start(&cfg, &index) == AMI_NET_OK && index == 0,
          "another unit of that device takes the slot the retained one left");
    CHECK(netstack_interface_remove(0, TRUE) == AMI_NET_OK,
          "and goes again, cleanly");

    h_iface(&cfg, "eth2", "x-surf-100.device", 0);
    CHECK(netstack_interface_start(&cfg, &index) == AMI_NET_OK && index == 0,
          "another device on unit 0 takes it too");
    CHECK(netstack_interface_remove(0, TRUE) == AMI_NET_OK, "and goes");

    /* The device gives everything back: the next add sweeps it away. */
    nsh.sana2[0].held = 0;
    opens = nsh.sana2_opens;
    h_iface(&cfg, "eth", "a2065.device", 0);
    CHECK(netstack_interface_start(&cfg, &index) == AMI_NET_OK,
          "once released, the unit is added again");
    CHECK(nsh.sana2_opens == opens + 1, "with one OpenDevice()");
    CHECK(nsh_retained() == 0, "and nothing is retained");

    h_teardown();
    CHECK(nsh.sana2_device_closes == nsh.sana2_device_opens,
          "one CloseDevice() for every OpenDevice(), none under a hold");
}

/* The read preflight stands: a reader the device kept refuses the removal
   before anything is detached. */
static void t_read_preflight_refuses(void)
{
    ULONG detaches;

    printf("retain: a reader orphaned before the detach\n");

    CHECK(h_up_with("a2065.device", 0), "the interface is up in slot 0");

    detaches = nsh.iface_detaches;
    nsh.sana2_orphaned = TRUE;
    CHECK(netstack_interface_remove(0, TRUE) == AMI_NET_ERR_STATE,
          "refused before the detach");
    CHECK(nsh.iface_detaches == detaches, "nothing was detached");
    nsh.sana2_orphaned = FALSE;

    h_teardown();
}

/*
 * Retained at shutdown: the stack's pool stays, and the library does not
 * unload until the device gives everything back AND the reader is joined.
 */
static void t_shutdown_holds_the_unload(void)
{
    NshSana2If *m = &nsh.sana2[0];
    ULONG       sweeps;

    printf("retain: shutdown with a read held, then the unload\n");

    CHECK(h_up_with("a2065.device", 0), "the interface is up in slot 0");

    nsh.sana2_close_held = NETEVENT_HELD_RX;
    nsh.sana2_close_join = TRUE;
    h_bury(netstack_get());
    netstack_shutdown();

    CHECK(netstack_get() == NULL, "the stack is down");
    CHECK(m->state == NSH_IF_RETAINED, "the interface is retained");
    CHECK(nsh.stack_retained_events == 1,
          "NETEVENT_STACK_RETAINED: the stack memory is kept");
    CHECK(nsh.packet_pool_deletes == 0,
          "the pool the held reads point into is not deleted");
    CHECK(m->device_closes == 0 && m->frees == 0,
          "and the interface is neither closed nor freed");
    CHECK(nsh.tx_stops_ok >= 1, "ThreadX itself stopped");
    CHECK(netstack_can_unload() == FALSE,
          "the library is not unloaded under a held request");
    CHECK(netstack_retained_count() == 1, "and the reason is countable");

    sweeps = nsh.sweeps;
    netstack_shutdown();
    CHECK(nsh.sweeps > sweeps, "a later shutdown sweeps again");
    CHECK(m->state == NSH_IF_RETAINED && netstack_can_unload() == FALSE,
          "and it is still held");

    m->held = 0;
    netstack_shutdown();
    CHECK(m->state == NSH_IF_RETAINED,
          "no request out, but the reader not joined: still held");
    CHECK(netstack_can_unload() == FALSE, "so still no unload");

    m->join_pending = FALSE;
    netstack_shutdown();
    CHECK(m->state == NSH_IF_FREE && m->device_closes == 1 && m->frees == 1,
          "joined: closed and freed exactly once");
    CHECK(netstack_can_unload() == TRUE, "and now the library may go");

    netstack_shutdown();
    CHECK(m->device_closes == 1 && m->frees == 1,
          "a further shutdown does nothing more to it");

    h_teardown();
}

/* A stack brought up while an earlier one's interface is still held keeps
   its own pool too: the sweep cannot tell whose pool a held packet is in. */
static void t_new_stack_over_a_held_one(void)
{
    ULONG deletes;

    printf("retain: a new stack over a held interface\n");

    CHECK(h_up_with("a2065.device", 0), "the first stack is up");
    nsh.sana2_close_held = NETEVENT_HELD_TX;
    h_bury(netstack_get());
    netstack_shutdown();
    nsh.sana2_close_held = 0;
    CHECK(nsh_retained() == 1, "its interface is retained");

    deletes = nsh.packet_pool_deletes;
    CHECK(netstack_startup_loopback() == AMI_NET_OK,
          "a new stack comes up beside it");
    h_bury(netstack_get());
    netstack_shutdown();
    CHECK(nsh.packet_pool_deletes == deletes,
          "and keeps its pool while anything is retained");

    nsh.sana2[0].held = 0;
    netstack_shutdown();
    CHECK(nsh_retained() == 0 && netstack_can_unload() == TRUE,
          "released by the next sweep, and the library may go");

    h_teardown();
}

/*
 * The kernel is running and the bracket cannot be had: no sweep at all, since
 * joining a reader is a ThreadX call.  Nothing is released, and a later sweep
 * point that can enter releases it.
 */
static void t_no_bracket_defers_the_sweep(void)
{
    NshSana2If *m = &nsh.sana2[0];
    ULONG       sweeps;

    printf("retain: a sweep point that cannot enter ThreadX\n");

    CHECK(h_up_with("a2065.device", 0), "the interface is up in slot 0");
    nsh.sana2_close_held = NETEVENT_HELD_TX;
    CHECK(netstack_interface_remove(0, TRUE) == AMI_NET_ERR_RETAINED,
          "removed with a write held");
    nsh.sana2_close_held = 0;
    m->held = 0;                        /* the device has given it back */

    sweeps = nsh.sweeps;
    nsh.tx_adopt_status = TX_NOT_DONE;
    CHECK(netstack_interface_remove(0, TRUE) == AMI_NET_ERR_STATE,
          "a removal that cannot enter ThreadX");
    CHECK(nsh.sweeps == sweeps, "did not sweep without a bracket");
    CHECK(m->state == NSH_IF_RETAINED && m->device_closes == 0 &&
          m->frees == 0 && nsh_retained() == 1,
          "nothing released, closed or freed; the count is unchanged");

    nsh.tx_adopt_status = TX_SUCCESS;
    (VOID)netstack_interface_remove(0, TRUE);
    CHECK(nsh.sweeps == sweeps + 1 && m->state == NSH_IF_FREE &&
          m->device_closes == 1 && m->frees == 1,
          "the next sweep point that can enter releases it, once");

    h_teardown();
}

/* The same at shutdown: the stack goes down without the sweep, and the next
   shutdown, with the kernel stopped, collects it. */
static void t_shutdown_without_a_bracket(void)
{
    NshSana2If *m = &nsh.sana2[0];
    ULONG       sweeps;

    printf("retain: shutdown that cannot enter ThreadX\n");

    CHECK(h_up_with("a2065.device", 0), "the interface is up in slot 0");
    nsh.sana2_close_held = NETEVENT_HELD_TX;
    CHECK(netstack_interface_remove(0, TRUE) == AMI_NET_ERR_RETAINED,
          "removed with a write held");
    nsh.sana2_close_held = 0;
    m->held = 0;

    sweeps = nsh.sweeps;
    nsh.tx_adopt_status = TX_NOT_DONE;
    nsh.tx_stop_status  = TX_NOT_DONE;      /* the kernel stays running */
    h_bury(netstack_get());
    netstack_shutdown();
    CHECK(nsh.sweeps == sweeps, "shutdown did not sweep without a bracket");
    CHECK(m->state == NSH_IF_RETAINED && m->frees == 0 &&
          m->device_closes == 0 && nsh.packet_pool_deletes == 0,
          "the interface and the pool are kept");
    CHECK(netstack_can_unload() == FALSE, "and the library stays");

    netstack_shutdown();
    CHECK(nsh.sweeps == sweeps && m->state == NSH_IF_RETAINED,
          "again, kernel running and no bracket: still deferred");

    nsh.tx_adopt_status = TX_SUCCESS;
    nsh.tx_stop_status  = TX_SUCCESS;
    netstack_shutdown();
    CHECK(m->state == NSH_IF_FREE && m->device_closes == 1 && m->frees == 1,
          "once the bracket can be had, released exactly once");
    CHECK(netstack_can_unload() == TRUE, "and the library may go");

    h_teardown();
}

int main(void)
{
    t_remove_keeps_a_held_write();
    t_add_same_unit_refused();
    t_read_preflight_refuses();
    t_shutdown_holds_the_unload();
    t_new_stack_over_a_held_one();
    t_no_bracket_defers_the_sweep();
    t_shutdown_without_a_bracket();

    printf("%lu checks, %lu failures, %s\n", h_checks, h_failures,
           (h_failures == 0) ? "PASS" : "FAIL");

    return (h_failures == 0) ? 0 : 1;
}

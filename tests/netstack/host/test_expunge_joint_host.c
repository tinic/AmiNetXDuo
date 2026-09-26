/*
 * The expunge refusal at the joint.
 *
 * WHY THIS EXISTS.  The refusal has two halves and each was already proved on
 * its own.  bsd_lib_expunge() declines when netstack_can_unload() answers
 * FALSE, and tests/bsdsocket/host/test_expunge_host.c drives that with the
 * answer scripted.  netstack.c keeps ami_ns_kernel_started set when
 * tx_amiga_kernel_stop() fails, which is what makes the answer FALSE.  Nothing
 * joined them: no test asked whether a FAILED stop actually leaves the flag
 * set, and that is the whole of the claim.  If ami_ns_kernel_stop_locked()
 * cleared the flag before checking the status, both existing tests would stay
 * green and the library would agree to be expunged with ThreadX Tasks still
 * running on code in the hunk that is about to be freed.  There is no memory
 * protection, so that is a dead machine and not an error message.
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

/* Leave nothing running for the next test, whatever this one did. */
static void h_teardown(void)
{
    nsh.tx_stop_status = TX_SUCCESS;
    netstack_shutdown();
    netstack_shutdown();
}

/*
 * Nothing has ever started: unloading is safe, and that is the direction the
 * whole mechanism has to be able to reach or the library can never expunge.
 */
static void t_idle(void)
{
    printf("expunge joint: nothing started\n");

    nsh_reset();

    CHECK(netstack_can_unload() == TRUE,
          "an untouched machine may be expunged");
    CHECK(nsh.tx_stops == 0, "and nothing was stopped to find that out");
}

/*
 * Up: ami_ns is not NULL, so unloading is refused however the kernel is.
 */
static void t_running(void)
{
    printf("expunge joint: while the stack is up\n");

    nsh_reset();

    CHECK(netstack_startup() == AMI_NET_OK, "the stack comes up");
    CHECK(nsh.tx_starts == 1, "ThreadX was started once");
    CHECK(netstack_can_unload() == FALSE,
          "a running stack refuses to be expunged");

    h_teardown();
}

/* Opening the library creates a real IP instance with loopback, while the
 * first explicitly named physical interface still occupies slot zero. */
static void t_loopback_then_selected(void)
{
    AmiIfConfig cfg;
    UWORD       index = 99;

    printf("netstack startup: loopback, then one selected interface\n");

    nsh_reset();
    CHECK(netstack_startup_loopback() == AMI_NET_OK,
          "the loopback-only stack comes up");
    CHECK(nsh.cfg_base_loads == 1, "only machine-wide configuration was read");
    CHECK(nsh.cfg_full_loads == 0, "the interface drawer was not loaded");
    CHECK(nsh.sana2_opens == 0, "no SANA-II device was opened");
    CHECK(nsh.iface_detaches == 1,
          "the temporary primary slot was returned");
    CHECK(netstack_get() != NULL, "a complete stack instance exists");
    CHECK(netstack_interface_count() == 0, "no physical interface is present");

    memset(&cfg, 0, sizeof(cfg));
    strcpy(cfg.name, "genet");
    strcpy(cfg.device, "genet.device");
    cfg.unit = 3;
    cfg.iptype = AMI_IPTYPE_STATIC;
    cfg.address = 0xC0A80105UL;
    cfg.netmask = 0xFFFFFF00UL;
    cfg.up = TRUE;
    cfg.configured = TRUE;

    CHECK(netstack_interface_start(&cfg, &index) == AMI_NET_OK,
          "the selected interface joins the stack");
    CHECK(index == 0, "the first physical interface owns slot zero");
    CHECK(nsh.iface_attaches == 1, "one physical interface was attached");
    CHECK(nsh.sana2_opens == 1, "exactly one SANA-II device was opened");
    CHECK(strcmp(nsh.opened_cfg.name, "genet") == 0,
          "the named interface reached SANA-II");
    CHECK(strcmp(nsh.opened_cfg.device, "genet.device") == 0 &&
          nsh.opened_cfg.unit == 3,
          "the named device and unit reached SANA-II unchanged");

    h_teardown();
}

/*
 * The joint.  A stop that fails leaves ami_ns_kernel_started set, and
 * netstack_can_unload() must keep answering FALSE even though the singleton
 * is gone.  This is the claim nothing tested.
 */
static void t_failed_stop_holds_the_flag(void)
{
    printf("expunge joint: a failed stop keeps the refusal\n");

    nsh_reset();

    CHECK(netstack_startup() == AMI_NET_OK, "up");

    nsh.tx_stop_status = TX_NOT_DONE;
    netstack_shutdown();

    CHECK(netstack_get() == NULL, "the singleton is gone");
    CHECK(nsh.tx_stops == 1, "the kernel stop was attempted");
    CHECK(nsh.tx_stops_ok == 0, "and it failed");

    CHECK(netstack_can_unload() == FALSE,
          "a failed stop still refuses the expunge");

    CHECK(nsh.baton_resets == 0,
          "the baton is not reset after a failed stop: a thread can still be "
          "inside a bracket");

    /* And it stays refused for as long as the stop keeps failing. */
    netstack_shutdown();
    CHECK(nsh.tx_stops == 2, "a later shutdown retries the stop");
    CHECK(netstack_can_unload() == FALSE, "and is still refused");

    /* The retry is what clears it.  Nothing else does. */
    nsh.tx_stop_status = TX_SUCCESS;
    netstack_shutdown();

    CHECK(nsh.tx_stops_ok == 1, "the retry stopped the kernel");
    CHECK(nsh.baton_resets == 1, "and only then is the baton reset");
    CHECK(netstack_can_unload() == TRUE,
          "a successful stop releases the refusal");
}

/*
 * The other direction, and the one a green test suite must not lose: a clean
 * shutdown has to let go.  A refusal that never lifts is a library that can
 * never be expunged, which is the failure mode the flag's owner has to avoid
 * as much as the one above.
 */
static void t_clean_stop_releases(void)
{
    printf("expunge joint: a clean stop lets go\n");

    nsh_reset();

    CHECK(netstack_startup() == AMI_NET_OK, "up");
    netstack_shutdown();

    CHECK(nsh.tx_stops_ok == 1, "the kernel stopped");
    CHECK(nsh.baton_resets == 1, "the baton was reset");
    CHECK(netstack_can_unload() == TRUE, "and the expunge is allowed");
}

/*
 * netstack_startup() runs the same stop first, to clear a kernel left behind
 * by a previous load.  When that stop fails there is nothing safe to do, so
 * bring-up is refused rather than started on top of the old one.
 */
static void t_startup_refuses_over_a_failed_stop(void)
{
    printf("expunge joint: bring-up over a kernel that will not stop\n");

    nsh_reset();

    CHECK(netstack_startup() == AMI_NET_OK, "up once");

    nsh.tx_stop_status = TX_NOT_DONE;
    netstack_shutdown();

    CHECK(netstack_can_unload() == FALSE, "refused, as above");

    {
        ULONG starts = nsh.tx_starts;

        CHECK(netstack_startup() == AMI_NET_ERR_KERNEL,
              "a second bring-up refuses while the old kernel is up");
        CHECK(nsh.tx_starts == starts,
              "and does not start ThreadX a second time");
        CHECK(netstack_get() == NULL, "nor publish a singleton");
    }

    h_teardown();
}

/*
 * bsd_lib_expunge() runs under Forbid() and cannot Wait(), so
 * netstack_can_unload() takes the lock with AttemptSemaphore().  A contended
 * lock is "cannot prove it is safe", which must read as a refusal.
 */
static void t_contended_lock_refuses(void)
{
    printf("expunge joint: a contended lock\n");

    nsh_reset();

    CHECK(netstack_can_unload() == TRUE, "idle, so allowed");

    nsh.attempt_semaphore_fails = TRUE;
    CHECK(netstack_can_unload() == FALSE,
          "a lock that cannot be taken refuses the expunge");

    nsh.attempt_semaphore_fails = FALSE;
    CHECK(netstack_can_unload() == TRUE, "and allows it again once free");
}

/*
 * A second opener holds the stack up.  The refusal must follow the reference
 * count, not the first close.
 */
static void t_refcount(void)
{
    printf("expunge joint: two openers\n");

    nsh_reset();

    CHECK(netstack_startup() == AMI_NET_OK, "first open");
    CHECK(netstack_startup() == AMI_NET_OK, "second open");
    CHECK(nsh.tx_starts == 1, "ThreadX is started once for both");

    netstack_shutdown();

    CHECK(netstack_get() != NULL, "one close does not take the stack down");
    CHECK(nsh.tx_stops == 0, "and does not stop the kernel");
    CHECK(netstack_can_unload() == FALSE, "so the expunge is still refused");

    netstack_shutdown();

    CHECK(netstack_get() == NULL, "the second close takes it down");
    CHECK(netstack_can_unload() == TRUE, "and the expunge is allowed");
}

/*
 * Issue #53, link 3.  Sockets still created on the IP instance at teardown
 * made nx_ip_delete() answer NX_SOCKETS_BOUND and leave the IP thread
 * running: the SANA-II units were never closed, the kernel could not stop,
 * and every later open failed until reboot.  They are torn down first -- a
 * TCP socket in each kind of state, and what refuses (here a UDP orphan, or
 * every socket) leaked uncounted -- so the delete succeeds, SANA-II is closed,
 * the kernel stops, nothing is retained, and the stack comes up again.
 */
#define H_TCP 3
static NX_TCP_SOCKET h_tcp[H_TCP];
static NX_UDP_SOCKET h_udp;

static void h_sockets_create(NX_IP *ip)
{
    static const UINT state[H_TCP] =
        { NX_TCP_ESTABLISHED, NX_TCP_LISTEN_STATE, NX_TCP_FIN_WAIT_1 };
    UINT i;

    memset(h_tcp, 0, sizeof(h_tcp));
    memset(&h_udp, 0, sizeof(h_udp));

    /* A client connected, a server listening, a client closing; the two
       clients bound. */
    for (i = 0; i < H_TCP; i++)
    {
        h_tcp[i].nx_tcp_socket_state       = state[i];
        h_tcp[i].nx_tcp_socket_client_type = (i != 1) ? NX_TRUE : NX_FALSE;
        h_tcp[i].nx_tcp_socket_bound_next  = (i != 1) ? &h_tcp[i] : NX_NULL;
        h_tcp[i].nx_tcp_socket_ip_ptr      = ip;
        h_tcp[i].nx_tcp_socket_created_next     = &h_tcp[(i + 1) % H_TCP];
        h_tcp[i].nx_tcp_socket_created_previous =
            &h_tcp[(i + H_TCP - 1) % H_TCP];
    }
    ip->nx_ip_tcp_created_sockets_ptr   = &h_tcp[0];
    ip->nx_ip_tcp_created_sockets_count = H_TCP;

    /* A bound UDP socket. */
    h_udp.nx_udp_socket_bound_next       = &h_udp;
    h_udp.nx_udp_socket_ip_ptr           = ip;
    h_udp.nx_udp_socket_created_next     = &h_udp;
    h_udp.nx_udp_socket_created_previous = &h_udp;
    ip->nx_ip_udp_created_sockets_ptr    = &h_udp;
    ip->nx_ip_udp_created_sockets_count  = 1;
}

static void t_bound_socket_torn_down(void)
{
    AmiNetStack *ns;
    NX_IP       *ip;
    int          pass;

    for (pass = 0; pass < 2; pass++)
    {
        printf("expunge joint: sockets still created at teardown%s\n",
               pass ? ", each refusing its delete" : "");

        nsh_reset();
        nsh.sock_stuck = (pass != 0);

        CHECK(netstack_startup() == AMI_NET_OK, "up");
        ns = netstack_get();
        ip = netstack_ip();
        CHECK(ns != NULL && ip != NULL, "a stack and its IP instance");
        if (ns == NULL || ip == NULL)
            return;

        h_sockets_create(ip);
        nsh.watch_block = ns;

        netstack_shutdown();

        printf("teardown stuck=%d ip_delete_status=%lu resets=%lu deletes=%lu "
               "sana2_closes=%lu retained=%u tx_stops=%lu ns_freed=%ld\n",
               pass, (unsigned long)nsh.ip_delete_status,
               (unsigned long)nsh.sock_resets, (unsigned long)nsh.sock_deletes,
               (unsigned long)nsh.sana2_closes, (unsigned)nsh_retained(),
               (unsigned long)nsh.tx_stops, (long)nsh.watch_freed);
        if (pass == 0)
        {
            CHECK(nsh.sock_resets == 1, "the connection was reset");
            CHECK(nsh.sock_deletes == H_TCP, "and every TCP socket deleted");
        }
        CHECK(nsh.ip_deletes == 1 && nsh.ip_delete_status == NX_SUCCESS,
              "nx_ip_delete() succeeds");
        CHECK(nsh.sana2_opens > 0 &&
                  nsh.sana2_device_closes == nsh.sana2_device_opens,
              "every SANA-II device was closed");
        CHECK(nsh_retained() == 0, "and none retained");
        CHECK(nsh.tx_stops == 1, "the kernel stopped");
        CHECK(nsh.watch_freed, "the stack block is freed");
        CHECK(netstack_get() == NULL && netstack_can_unload() == TRUE,
              "and the library may unload");

        nsh.sock_stuck = FALSE;
        CHECK(netstack_startup() == AMI_NET_OK, "a later open brings it up");
        h_teardown();
    }
}

int main(void)
{
    printf("netstack expunge joint host checks\n\n");

    t_idle();
    t_running();
    t_loopback_then_selected();
    t_failed_stop_holds_the_flag();
    t_clean_stop_releases();
    t_startup_refuses_over_a_failed_stop();
    t_contended_lock_refuses();
    t_refcount();
    t_bound_socket_torn_down();

    printf("\n%lu checks, %lu failures\n", h_checks, h_failures);

    return (h_failures == 0) ? 0 : 1;
}

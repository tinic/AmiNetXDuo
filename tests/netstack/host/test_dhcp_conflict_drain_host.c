/*
 * AmiNetXDuo, DHCP conflict-flag drain (N-059 lost-update race).
 *
 * _nx_dhcp_thread_entry drained nx_dhcp_interface_conflict_flag with an
 * unguarded read-modify-write: it tested a bit, declined the interface, then
 * cleared the bit with `flag &= ~bit`.  The writer (_nx_dhcp_ip_conflict) sets
 * a bit under TX_DISABLE from the SANA2 RX thread (priority 1), which preempts
 * the DHCP thread (priority 3).  Between the DHCP thread's load of the flag and
 * its store, the RX thread can set another interface's bit; the store is
 * computed from the stale load and clobbers the newly set bit, silently dropping
 * that interface's conflict with no DHCPDECLINE.
 *
 * The fix claims every pending bit in one read-and-clear under TX_DISABLE
 * (matching the writer's protection), then declines each claimed interface
 * outside the critical section.  A conflict arriving after the claim -- during
 * the decline -- lands on the now-empty flag and is preserved for the next
 * event wake-up.
 *
 * This drives the REAL _nx_dhcp_thread_entry through two bounded interleavings:
 *
 *   INJECT_CLAIM   -- the writer fires while the claim holds Forbid, so its
 *                     write is deferred to the claim's TX_RESTORE (modelled in
 *                     the _tx_thread_interrupt_restore stub); the claim has
 *                     already read-and-cleared, so the bit survives.
 *
 *   INJECT_DECLINE -- the writer fires during the decline of interface 0,
 *                     after the claim (modelled in _txe_mutex_get, the first
 *                     tx_mutex_get the decline itself takes); the decline runs
 *                     outside the claim's Forbid, so the write lands on the
 *                     empty flag and survives.
 *
 * The writer is the real _nx_dhcp_ip_conflict, not a re-created model, and the
 * decline is the real _nx_dhcp_interface_decline (it returns early because the
 * instance has no interface record, but its tx_mutex_get/tx_mutex_put are what
 * the harness observes).  On the old source the conflict block has no claim at
 * all, so INJECT_CLAIM never fires, no second bit is set, and the "bit survives"
 * assertion fails.
 *
 * x86_64 only: the DHCP instance travels to _nx_dhcp_thread_entry through a
 * ULONG, which tests/perf/host/shim makes 32 bits to match the m68k, so the
 * instance must sit below 4 GiB and is placed there with MAP_32BIT (see main).
 *
 * SPDX-License-Identifier: MIT
 */

#include "nx_api.h"
#include "nxd_dhcp_client.h"

#include <stdio.h>
#include <string.h>
#include <setjmp.h>
#include <stdint.h>
#include <sys/mman.h>

/* The unit under test and the writer's anchor are static in the source; the
   harness compiles it with -Dstatic= so these bind to the real bodies. */
VOID _nx_dhcp_thread_entry(ULONG dhcp_instance);
VOID _nx_dhcp_ip_conflict(NX_IP *ip_ptr, UINT interface_index, ULONG ip_address,
                          ULONG physical_msw, ULONG physical_lsw);
extern struct NX_DHCP_STRUCT *_nx_dhcp_created_ptr;

static unsigned long h_checks;
static unsigned long h_failures;

static void h_check(int ok, const char *what)
{
    h_checks++;

    if (!ok)
    {
        h_failures++;
        printf("FAIL %s\n", what);
    }
}

/* ---- test state ---- */

static NX_DHCP *h_dhcp;                    /* the DHCP instance under test  */
static NX_IP   h_ip;                       /* its associated IP instance    */

static int     h_forbid_depth;            /* Forbid nesting depth           */
static ULONG   h_event_pending;           /* host-side DHCP event flags     */
static int     h_injected;                /* the second conflict was issued */
static int     h_inject_pending;          /* deferred RX-thread write       */
static int     h_inject_mode;             /* INJECT_CLAIM or INJECT_DECLINE */
static int     h_mutex_gets;              /* _txe_mutex_get call count      */
static int     h_declines;                /* how many declines ran          */
static int     h_decline_forbid_depth;    /* forbid depth at the decline    */

/* Where the second interface's conflict is issued. */
enum { INJECT_CLAIM, INJECT_DECLINE };

/* ---- ThreadX / NetX stubs (the _txe_/_nxe_ names the DHCP client calls
       through its public macros) ---- */

/* The thread entry takes the DHCP mutex three times in a single-interface
   conflict drain: (1) once on entry, (2) once after the event wait, and (3)
   once inside _nx_dhcp_interface_decline.  The third is the first tx_mutex_get
   after the claim's TX_RESTORE, i.e. it marks the decline running outside the
   claim's critical section. */
UINT _txe_mutex_get(TX_MUTEX *mutex_ptr, ULONG wait_option)
{
    (void)mutex_ptr; (void)wait_option;

    h_mutex_gets++;
    if (h_mutex_gets == 3)
    {
        h_declines++;
        h_decline_forbid_depth = h_forbid_depth;

        if (h_inject_mode == INJECT_DECLINE && !h_injected)
        {
            h_injected = 1;
            _nx_dhcp_ip_conflict(&h_ip, 1, 0, 0, 0);
        }
    }
    return TX_SUCCESS;
}

UINT _txe_mutex_put(TX_MUTEX *mutex_ptr)
{ (void)mutex_ptr; return TX_SUCCESS; }

UINT _txe_event_flags_set(TX_EVENT_FLAGS_GROUP *group_ptr, ULONG flags_to_set,
                          UINT set_option)
{
    (void)group_ptr; (void)set_option;
    h_event_pending |= flags_to_set;
    return TX_SUCCESS;
}

/* The writer defers its write while the DHCP thread holds Forbid: on a real
   system the RX thread cannot run between TX_DISABLE and TX_RESTORE.  We model
   that by issuing the write when the claim's Forbid is lifted. */
UINT _tx_thread_interrupt_disable(void)
{
    h_forbid_depth++;
    if (h_forbid_depth == 1 && h_inject_mode == INJECT_CLAIM && !h_injected)
    {
        h_injected = 1;
        h_inject_pending = 1;           /* RX thread wants to run now        */
    }
    return 0;
}

VOID _tx_thread_interrupt_restore(UINT previous_posture)
{
    (void)previous_posture;
    h_forbid_depth--;
    if (h_forbid_depth == 0 && h_inject_pending)
    {
        h_inject_pending = 0;
        _nx_dhcp_ip_conflict(&h_ip, 1, 0, 0, 0);   /* the deferred real write */
    }
}

/* Drive one pass of the thread-entry loop: the first event fetch reports the
   conflict event, the second unwinds back to the caller. */
static jmp_buf h_exit;
static int     h_event_gets;

UINT _txe_event_flags_get(TX_EVENT_FLAGS_GROUP *group_ptr, ULONG requested_flags,
                          UINT get_option, ULONG *actual_flags_ptr, ULONG wait_option)
{
    (void)group_ptr; (void)requested_flags; (void)get_option; (void)wait_option;
    h_event_gets++;
    if (h_event_gets == 1)
    {
        *actual_flags_ptr = NX_DHCP_CLIENT_CONFLICT_EVENT;
        return TX_SUCCESS;
    }
    longjmp(h_exit, 1);
}

/* ---- link-only stubs ---- */
/*
 * The source is compiled with -Dstatic= and --gc-sections; ASan's global
 * registration keeps the receive/timer arms of _nx_dhcp_thread_entry (and the
 * DHCP machinery they reach) alive even though h_event_gets never reports a
 * receive or timer event.  These satisfy the linker for their nx/tx externs.
 * None is reached by the conflict drain under test.
 */
UINT   _nx_udp_socket_receive(NX_UDP_SOCKET *socket_ptr, NX_PACKET **packet_ptr, ULONG wait_option)
{ (void)socket_ptr; (void)packet_ptr; (void)wait_option; return NX_NOT_SUCCESSFUL; }
UINT   _nxe_udp_packet_info_extract(NX_PACKET *packet_ptr, ULONG *ip_address, UINT *protocol,
                                    UINT *port, UINT *interface_index)
{ (void)packet_ptr; (void)ip_address; (void)protocol; (void)port; (void)interface_index; return NX_NOT_SUCCESSFUL; }
UINT   _nxe_packet_release(NX_PACKET **packet_ptr_ptr)
{ (void)packet_ptr_ptr; return NX_SUCCESS; }
UINT   _nx_arp_probe_send(NX_IP *ip_ptr, UINT interface_index, ULONG probe_address)
{ (void)ip_ptr; (void)interface_index; (void)probe_address; return 0; }
USHORT _nx_ip_checksum_compute(NX_PACKET *packet_ptr, ULONG protocol, UINT data_length,
                               ULONG *src, ULONG *dst)
{ (void)packet_ptr; (void)protocol; (void)data_length; (void)src; (void)dst; return 0; }
UINT   _nx_ip_interface_address_get(NX_IP *ip_ptr, UINT interface_index,
                                    ULONG *ip_address, ULONG *network_mask)
{ (void)ip_ptr; (void)interface_index; (void)ip_address; (void)network_mask; return 0; }
UINT   _nx_ip_interface_address_set(NX_IP *ip_ptr, UINT interface_index,
                                    ULONG ip_address, ULONG network_mask)
{ (void)ip_ptr; (void)interface_index; (void)ip_address; (void)network_mask; return 0; }
UINT   _nx_packet_allocate(NX_PACKET_POOL *pool_ptr, NX_PACKET **packet_ptr,
                           ULONG packet_type, ULONG wait_option)
{ (void)pool_ptr; (void)packet_ptr; (void)packet_type; (void)wait_option; return 0; }
UINT   _nx_packet_release(NX_PACKET *packet_ptr)
{ (void)packet_ptr; return 0; }
UINT   _nx_udp_socket_source_send(NX_UDP_SOCKET *socket_ptr, NX_PACKET *packet_ptr,
                                  ULONG ip_address, UINT port, UINT address_index)
{ (void)socket_ptr; (void)packet_ptr; (void)ip_address; (void)port; (void)address_index; return 0; }
UINT   _nx_utility_string_length_check(CHAR *input_string, UINT *string_length,
                                       UINT max_string_length)
{ (void)input_string; (void)string_length; (void)max_string_length; return 0; }
UINT   _nx_packet_data_extract_offset(NX_PACKET *packet_ptr, ULONG offset,
                                      VOID *buffer_start, ULONG buffer_length,
                                      ULONG *bytes_copied)
{ (void)packet_ptr; (void)offset; (void)buffer_start; (void)buffer_length; (void)bytes_copied; return 0; }
UINT   _nx_udp_packet_info_extract(NX_PACKET *packet_ptr, ULONG *ip_address, UINT *protocol,
                                   UINT *port, UINT *interface_index)
{ (void)packet_ptr; (void)ip_address; (void)protocol; (void)port; (void)interface_index; return 0; }
UINT   _tx_thread_sleep(ULONG timer_ticks)
{ (void)timer_ticks; return TX_SUCCESS; }

/* Run one conflict-drain pass and return where it left the flag. */
static UINT h_run_drain(int inject_mode)
{
    h_inject_mode = inject_mode;
    h_forbid_depth = 0;
    h_event_pending = 0;
    h_injected = 0;
    h_inject_pending = 0;
    h_mutex_gets = 0;
    h_declines = 0;
    h_decline_forbid_depth = -1;
    h_event_gets = 0;

    if (setjmp(h_exit) == 0)
    {
        _nx_dhcp_thread_entry((ULONG)(uintptr_t)h_dhcp);
    }

    return h_dhcp->nx_dhcp_interface_conflict_flag;
}

int main(void)
{
    UINT flag;

    setbuf(stdout, NULL);

    printf("DHCP conflict-flag drain, thread-entry bounded interleaving\n");

    /* The DHCP instance reaches _nx_dhcp_thread_entry through a ULONG: the
       host shim makes ULONG 32 bits to match the m68k, so the instance must sit
       below 4 GiB for the entry argument to round-trip.  Place it there
       explicitly (MAP_32BIT) rather than relying on the linker's layout. */
#ifdef MAP_32BIT
    h_dhcp = mmap(NULL, sizeof(NX_DHCP), PROT_READ | PROT_WRITE,
                  MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
#else
    h_dhcp = mmap(NULL, sizeof(NX_DHCP), PROT_READ | PROT_WRITE,
                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
#endif
    if (h_dhcp == MAP_FAILED ||
        (uintptr_t)h_dhcp != (uintptr_t)(ULONG)(uintptr_t)h_dhcp)
    {
        printf("cannot place NX_DHCP in a 32-bit address\n");
        return 2;
    }
    memset(h_dhcp, 0, sizeof(NX_DHCP));
    memset(&h_ip, 0, sizeof(h_ip));

    /* The writer walks _nx_dhcp_created_ptr and matches nx_dhcp_ip_ptr. */
    h_dhcp->nx_dhcp_ip_ptr = &h_ip;
    h_dhcp->nx_dhcp_created_next = NX_NULL;
    _nx_dhcp_created_ptr = h_dhcp;

    /* Interleaving 1: the second interface conflicts while the claim holds
       Forbid, so its write is deferred until the claim's TX_RESTORE.  The
       claim has already read-and-cleared by then, so the bit survives. */
    h_dhcp->nx_dhcp_interface_conflict_flag = (UINT)(1 << 0);
    flag = h_run_drain(INJECT_CLAIM);
    h_check(flag == (UINT)(1 << 1),
            "deferred-during-claim: interface 1 conflict survives the claim");
    h_check(h_declines == 1,
            "deferred-during-claim: exactly one decline ran (bit 0 only)");
    h_check(h_decline_forbid_depth == 0,
            "deferred-during-claim: decline ran outside the claim's Forbid");
    h_check((h_event_pending & NX_DHCP_CLIENT_CONFLICT_EVENT) != 0,
            "deferred-during-claim: conflict event re-signaled");

    /* Interleaving 2: the second interface conflicts during the decline of
       interface 0 (after the claim), landing on the empty flag. */
    h_dhcp->nx_dhcp_interface_conflict_flag = (UINT)(1 << 0);
    flag = h_run_drain(INJECT_DECLINE);
    h_check(flag == (UINT)(1 << 1),
            "during-decline: interface 1 conflict survives the decline");
    h_check(h_declines == 1,
            "during-decline: exactly one decline ran (bit 0 only)");
    h_check(h_decline_forbid_depth == 0,
            "during-decline: decline ran outside the claim's Forbid");
    h_check((h_event_pending & NX_DHCP_CLIENT_CONFLICT_EVENT) != 0,
            "during-decline: conflict event re-signaled");

    printf("%lu checks, %lu failures, %s\n",
           h_checks, h_failures, (h_failures == 0UL) ? "PASS" : "FAIL");

    return (h_failures == 0UL) ? 0 : 1;
}

/*
 * AmiNetXDuo, the DHCP lease lifecycle, and RFC 3927 link-local addressing.
 *
 * SPDX-License-Identifier: MIT
 */

#include "netstack_internal.h"

#include "nx_arp.h"

#include "tx_amiga.h"
#include "tx_thread.h"

#include "aminetxduo/crashguard.h"

#include <exec/execbase.h>
#include <dos/dos.h>
#include <dos/dostags.h>
#include <dos/var.h>
#include <proto/exec.h>
#include <inline/macros.h>
#include <proto/dos.h>

#include <stdarg.h>


#ifndef RawPutChar
#  define RawPutChar(c) \
      LP1NR(0x204, RawPutChar, UBYTE, (c), d0, , EXEC_BASE_NAME)
#endif

#define T_LOG_SIZE      16384

static char     t_log_buffer[T_LOG_SIZE];
static ULONG    t_log_used;
static ULONG    t_log_flushed;

static VOID t_put(UBYTE c)
{

    RawPutChar(c);

    if (t_log_used < (ULONG) (T_LOG_SIZE - 1))
    {
        t_log_buffer[t_log_used++] = (char) c;
    }
}

static VOID t_put_char(register UBYTE c      __asm("d0"),
                       register APTR  unused __asm("a3"))
{

    (VOID) unused;
    if (c != '\0')
    {
        t_put(c);
    }
}

static VOID t_log(const char *fmt, ...)
{

va_list args;


    va_start(args, fmt);
    RawDoFmt((STRPTR) fmt, args, (void (*)()) t_put_char, NULL);
    va_end(args);

    t_put('\n');
}

static VOID t_flush(VOID)
{

BPTR    out;


    out =  Output();
    if ((out != (BPTR) 0) && (t_log_used > t_log_flushed))
    {
        (VOID) Write(out, (APTR) &t_log_buffer[t_log_flushed],
                     (LONG) (t_log_used - t_log_flushed));
    }
    t_log_flushed =  t_log_used;
}


static volatile ULONG   t_checks;
static volatile ULONG   t_failures;

static UINT t_check(UINT ok, const char *what, ULONG detail)
{

    Forbid();
    t_checks++;
    if (!ok)
    {
        t_failures++;
    }
    Permit();

    if (ok)
    {
        t_log("  ok   %s", what);
    }
    else
    {
        t_log("  FAIL %s (0x%lx)", what, detail);
    }

    return(ok);
}

static VOID t_log_ip(const char *label, ULONG addr)
{

    t_log("  %s %lu.%lu.%lu.%lu", label,
          (addr >> 24) & 0xFFUL, (addr >> 16) & 0xFFUL,
          (addr >>  8) & 0xFFUL,  addr        & 0xFFUL);
}


static const char *const t_dhcp_state_name[] =
{
    "NOT_STARTED", "BOOT", "INIT", "SELECTING", "REQUESTING", "BOUND",
    "RENEWING", "REBINDING", "FORCERENEW", "ADDRESS_PROBING"
};

#define T_TRACE_MAX     32

static volatile UBYTE   t_dhcp_trace[T_TRACE_MAX];
static volatile ULONG   t_dhcp_trace_used;
static volatile UBYTE   t_dhcp_state_last;

static VOID t_dhcp_state_change(NX_DHCP *dhcp_ptr, UCHAR new_state)
{

    (VOID) dhcp_ptr;

    t_dhcp_state_last =  (UBYTE) new_state;

    if (t_dhcp_trace_used < (ULONG) T_TRACE_MAX)
    {
        t_dhcp_trace[t_dhcp_trace_used++] =  (UBYTE) new_state;
    }

    t_log("  [dhcp] -> %s",
          (new_state < (UCHAR) (sizeof(t_dhcp_state_name) /
                                sizeof(t_dhcp_state_name[0])))
              ? t_dhcp_state_name[new_state] : "?");
}

/* True if the client passed through `state` after trace position `from`.  The
   state machine can cross RENEWING and come back inside one poll interval, so
   an instantaneous read misses it and only the trace is reliable. */
static BOOL t_saw_state(UCHAR state, ULONG from)
{

ULONG   i;


    for (i = from; i < t_dhcp_trace_used; i++)
    {
        if (t_dhcp_trace[i] == (UBYTE) state)
            return(TRUE);
    }

    return(FALSE);
}

static volatile BOOL    t_killlink;
static volatile ULONG   t_link_downs;
static volatile BOOL    t_link_restored;

#define T_KILL_SECONDS      42UL        /* > AMI_DHCP_TIMEOUT_TICKS (30 s) */

static VOID t_linkctl_entry(VOID)
{

ULONG   i;


    /* Wait for the interface to be up, taking it down before the IP thread
       has finished bringing it up is a race, and losing it means the reader
       threads are torn down mid-start. */
    for (i = 0; i < 3000UL; i++)
    {
        if (netstack_interface_is_up(0))
            break;
        Delay(1UL);
    }

    for (i = 0; i < 200000UL; i++)
    {
        if (netstack_interface_dhcp_state(0) != AMI_DHCP_IDLE)
            break;
        if ((i % 20000UL) == 19999UL)
            Delay(1UL);
    }

    for (i = 0; i < (T_KILL_SECONDS * 25UL); i++)
    {
        if (netstack_interface_is_up(0))
        {
            if (netstack_interface_down(0) == AMI_NET_OK)
                t_link_downs++;
        }
        Delay(2UL);                     /* 40 ms */
    }

    (VOID) netstack_interface_up(0);
    t_link_restored =  TRUE;
    t_log("  [link] restored after %ld seconds, %ld disable(s)",
          T_KILL_SECONDS, t_link_downs);
}

static VOID t_linkctl_start(VOID)
{

struct TagItem  tags[5];


    tags[0].ti_Tag  =  NP_Entry;
    tags[0].ti_Data =  (ULONG) t_linkctl_entry;
    tags[1].ti_Tag  =  NP_Name;
    tags[1].ti_Data =  (ULONG) "aminetxduo linkctl";
    tags[2].ti_Tag  =  NP_StackSize;
    tags[2].ti_Data =  16384UL;
    tags[3].ti_Tag  =  NP_Priority;
    tags[3].ti_Data =  5UL;
    tags[4].ti_Tag  =  TAG_DONE;
    tags[4].ti_Data =  0UL;

    (VOID) CreateNewProc(tags);
}


static TX_THREAD    t_main_thread;

#define T_SEC(n)    ((ULONG) (n) * (ULONG) NX_IP_PERIODIC_RATE)

static AmiNetStack *t_ns;
static NX_IP       *t_ip;

static NX_DHCP_INTERFACE_RECORD *t_record(VOID)
{

UINT    i;


    if ((t_ns == NULL) || !t_ns->ns_DhcpCreated)
        return(NX_NULL);

    for (i = 0; i < NX_DHCP_CLIENT_MAX_RECORDS; i++)
    {
        if ((t_ns->ns_Dhcp.nx_dhcp_interface_record[i].nx_dhcp_record_valid) &&
            (t_ns->ns_Dhcp.nx_dhcp_interface_record[i].nx_dhcp_interface_index == 0))
        {
            return(&t_ns->ns_Dhcp.nx_dhcp_interface_record[i]);
        }
    }

    return(NX_NULL);
}

static ULONG t_address(VOID)
{

ULONG   addr =  0UL;
ULONG   mask =  0UL;


    (VOID) nx_ip_address_get(t_ip, &addr, &mask);
    return(addr);
}

static BOOL t_is_linklocal(ULONG addr)
{

    return((addr >= IP_ADDRESS(169, 254, 1, 0)) &&
           (addr <= IP_ADDRESS(169, 254, 254, 255))) ? TRUE : FALSE;
}

static BOOL t_wait_state(UCHAR want, ULONG seconds)
{

NX_DHCP_INTERFACE_RECORD   *rec;
ULONG                       i;


    for (i = 0; i < (seconds * 10UL); i++)
    {
        rec =  t_record();
        if ((rec != NX_NULL) && (rec->nx_dhcp_state == want))
            return(TRUE);
        tx_thread_sleep(NX_IP_PERIODIC_RATE / 10UL);
    }

    return(FALSE);
}

static BOOL t_wait_address(ULONG seconds)
{

ULONG   i;


    for (i = 0; i < (seconds * 10UL); i++)
    {
        if (t_address() != 0UL)
            return(TRUE);
        tx_thread_sleep(NX_IP_PERIODIC_RATE / 10UL);
    }

    return(FALSE);
}


static VOID t_phase_a(VOID)
{

ULONG   addr;
ULONG   i;


    t_log("");
    t_log("A. the DHCP-timeout fallback (RFC 3927 section 1.7)");

    t_log("  link was held down %ld time(s); restored=%ld",
          t_link_downs, (ULONG) t_link_restored);

    (VOID) t_check((UINT) (t_ns->ns_AutoIpCreated != FALSE),
                   "AutoIP instance was created", 0UL);

    if (!t_ns->ns_AutoIpCreated)
    {
        t_log("  the fallback never fired, DHCP must have got in first");
        return;
    }

    (VOID) t_wait_address(60UL);

    addr =  t_address();
    t_log_ip("address after startup", addr);
    t_log("  autoip candidate %lu.%lu.%lu.%lu, probes %ld, announces %ld, "
          "conflicts %ld, defends %ld",
          (t_ns->ns_AutoIp.nx_auto_ip_current_local_address >> 24) & 0xFFUL,
          (t_ns->ns_AutoIp.nx_auto_ip_current_local_address >> 16) & 0xFFUL,
          (t_ns->ns_AutoIp.nx_auto_ip_current_local_address >>  8) & 0xFFUL,
          (t_ns->ns_AutoIp.nx_auto_ip_current_local_address      ) & 0xFFUL,
          t_ns->ns_AutoIp.nx_auto_ip_probe_count,
          t_ns->ns_AutoIp.nx_auto_ip_announce_count,
          t_ns->ns_AutoIp.nx_auto_ip_conflict_count,
          t_ns->ns_AutoIp.nx_auto_ip_defend_count);

    (VOID) t_check((UINT) t_is_linklocal(
                       t_ns->ns_AutoIp.nx_auto_ip_current_local_address),
                   "candidate is inside 169.254.1.0-169.254.254.255",
                   t_ns->ns_AutoIp.nx_auto_ip_current_local_address);

    (VOID) t_check((UINT) (t_ns->ns_AutoIp.nx_auto_ip_probe_count >=
                           (ULONG) NX_AUTO_IP_PROBE_NUM),
                   "at least NX_AUTO_IP_PROBE_NUM ARP probes were sent",
                   t_ns->ns_AutoIp.nx_auto_ip_probe_count);

    /* The announcements follow ANNOUNCE_WAIT after the claim, so they are
       still to come when netstack_startup() returns. */
    for (i = 0; i < 200UL; i++)
    {
        if (t_ns->ns_AutoIp.nx_auto_ip_announce_count > 0UL)
            break;
        tx_thread_sleep(NX_IP_PERIODIC_RATE / 10UL);
    }

    (VOID) t_check((UINT) (t_ns->ns_AutoIp.nx_auto_ip_announce_count > 0UL),
                   "at least one ARP announcement was sent",
                   t_ns->ns_AutoIp.nx_auto_ip_announce_count);

    if (t_is_linklocal(addr) && t_ns->ns_DhcpCreated && t_killlink)
    {
        t_log("  waiting for DHCP to supersede the link-local address");

        for (i = 0; i < 900UL; i++)
        {
            addr =  t_address();
            if ((addr != 0UL) && !t_is_linklocal(addr))
                break;
            tx_thread_sleep(NX_IP_PERIODIC_RATE / 10UL);
        }

        t_log_ip("address now", addr);
        (VOID) t_check((UINT) ((addr != 0UL) && !t_is_linklocal(addr)),
                       "DHCP superseded the link-local address", addr);

        t_log("  AutoIP thread state after DHCP won: %ld",
              (ULONG) t_ns->ns_AutoIp.nx_auto_ip_thread.tx_thread_state);
    }
}


static VOID t_phase_b(VOID)
{

NX_DHCP_INTERFACE_RECORD   *rec;
ULONG                       acks_before;
ULONG                       addr_before;
ULONG                       trace_from;


    t_log("");
    t_log("B. renewal at T1 (RFC 2131 section 4.4.5)");

    rec =  t_record();
    if (!t_check((UINT) (rec != NX_NULL), "DHCP has a record for interface 0",
                 0UL))
        return;

    if (!t_check((UINT) (rec->nx_dhcp_state == NX_DHCP_STATE_BOUND),
                 "client is BOUND before the renewal test",
                 (ULONG) rec->nx_dhcp_state))
        return;

    t_log("  lease %ld s, T1 %ld s, T2 %ld s, countdown %ld ticks",
          rec->nx_dhcp_lease_time, rec->nx_dhcp_renewal_time,
          rec->nx_dhcp_rebind_time, rec->nx_dhcp_timeout);
    t_log_ip("server", rec->nx_dhcp_server_ip);

    acks_before =  rec->nx_dhcp_acks_received;
    addr_before =  t_address();
    trace_from  =  t_dhcp_trace_used;

    rec->nx_dhcp_renewal_time =  T_SEC(2);
    rec->nx_dhcp_rebind_time  =  T_SEC(8);
    rec->nx_dhcp_lease_time   =  T_SEC(12);
    rec->nx_dhcp_timeout      =  T_SEC(2);

    tx_thread_sleep(T_SEC(15));

    (VOID) t_check((UINT) t_saw_state(NX_DHCP_STATE_RENEWING, trace_from),
                   "T1 expiry moved the client to RENEWING",
                   (ULONG) t_dhcp_state_last);

    (VOID) t_check((UINT) (t_dhcp_state_last == NX_DHCP_STATE_BOUND),
                   "the server's ACK put it back into BOUND",
                   (ULONG) t_dhcp_state_last);

    rec =  t_record();
    if (rec != NX_NULL)
    {
        t_log("  acks %ld -> %ld, naks %ld", acks_before,
              rec->nx_dhcp_acks_received, rec->nx_dhcp_nacks_received);
        (VOID) t_check((UINT) (rec->nx_dhcp_acks_received > acks_before),
                       "the renewal was acknowledged by the server",
                       rec->nx_dhcp_acks_received);
        (VOID) t_check((UINT) (t_address() == addr_before),
                       "the address survived the renewal", t_address());
    }
}


static VOID t_phase_c(VOID)
{

NX_DHCP_INTERFACE_RECORD   *rec;
ULONG                       naks_before;
ULONG                       trace_from;
UINT                        status;


    t_log("");
    t_log("C. a NAK (RFC 2131 section 4.4.5, and what the user is told)");

    rec =  t_record();
    if (rec == NX_NULL)
        return;

    naks_before =  rec->nx_dhcp_nacks_received;
    trace_from  =  t_dhcp_trace_used;

    status =  nx_dhcp_stop(&t_ns->ns_Dhcp);
    t_log("  nx_dhcp_stop -> %ld", (ULONG) status);

    status =  nx_dhcp_interface_request_client_ip(&t_ns->ns_Dhcp, 0,
                                                  IP_ADDRESS(10, 0, 2, 231),
                                                  NX_TRUE);
    t_log("  asking for 10.0.2.231 (never leased to us) -> %ld", (ULONG) status);

    status =  nx_dhcp_start(&t_ns->ns_Dhcp);
    t_log("  nx_dhcp_start -> %ld", (ULONG) status);

    (VOID) t_wait_state(NX_DHCP_STATE_BOUND, 60UL);

    (VOID) t_check((UINT) t_saw_state(NX_DHCP_STATE_REQUESTING, trace_from),
                   "client REQUESTed the address it was told to ask for",
                   (ULONG) t_dhcp_state_last);

    rec =  t_record();
    if (rec != NX_NULL)
    {
        t_log("  naks %ld -> %ld, state %ld", naks_before,
              rec->nx_dhcp_nacks_received, (ULONG) rec->nx_dhcp_state);

        (VOID) t_check((UINT) (rec->nx_dhcp_nacks_received > naks_before),
                       "the server NAKed the request",
                       rec->nx_dhcp_nacks_received);
    }

    t_log_ip("address while recovering", t_address());

    (VOID) t_check((UINT) t_wait_state(NX_DHCP_STATE_BOUND, 40UL),
                   "the client recovered a lease after the NAK",
                   (ULONG) t_dhcp_state_last);
    t_log_ip("address after recovery", t_address());
}


static VOID t_phase_d(VOID)
{

NX_DHCP_INTERFACE_RECORD   *rec;
ULONG                       i;
ULONG                       addr;


    t_log("");
    t_log("D. T2 and the end of the lease, with nothing answering");

    rec =  t_record();
    if (rec == NX_NULL)
        return;

    if (!t_check((UINT) (rec->nx_dhcp_state == NX_DHCP_STATE_BOUND),
                 "client is BOUND before the expiry test",
                 (ULONG) rec->nx_dhcp_state))
        return;

    (VOID) netstack_interface_down(0);
    t_log("  interface 0 link down");

    rec->nx_dhcp_renewal_time =  2UL;
    rec->nx_dhcp_rebind_time  =  6UL;
    rec->nx_dhcp_lease_time   =  12UL;
    rec->nx_dhcp_timeout      =  T_SEC(2);

    (VOID) t_check((UINT) t_wait_state(NX_DHCP_STATE_RENEWING, 10UL),
                   "T1 expiry moved the client to RENEWING",
                   (ULONG) t_dhcp_state_last);

    (VOID) t_check((UINT) t_wait_state(NX_DHCP_STATE_REBINDING, 30UL),
                   "T2 expiry moved the client to REBINDING",
                   (ULONG) t_dhcp_state_last);

    (VOID) t_check((UINT) t_wait_state(NX_DHCP_STATE_INIT, 60UL),
                   "the lease ran out and the client went back to INIT",
                   (ULONG) t_dhcp_state_last);

    addr =  t_address();
    t_log_ip("address after the lease ran out", addr);
    (VOID) t_check((UINT) (addr == 0UL),
                   "the expired address was taken off the interface", addr);

    t_log("  AutoIP created after the lease expired: %ld, running %ld",
          (ULONG) t_ns->ns_AutoIpCreated, (ULONG) t_ns->ns_AutoIpRunning);

    (VOID) t_check((UINT) (t_ns->ns_AutoIpCreated != FALSE),
                   "losing the lease started RFC 3927 link-local addressing",
                   0UL);

    (VOID) netstack_interface_up(0);
    t_log("  interface 0 link up");

    for (i = 0; i < 600UL; i++)
    {
        if (t_address() != 0UL)
            break;
        tx_thread_sleep(NX_IP_PERIODIC_RATE / 10UL);
    }

    t_log_ip("address after the link came back", t_address());
    (VOID) t_check((UINT) (t_address() != 0UL),
                   "the client re-acquired an address once the wire returned",
                   t_address());
}


static NX_AUTO_IP   t_own_autoip;
static UCHAR        t_own_autoip_stack[2048];
static BOOL         t_own_autoip_created;
static NX_AUTO_IP  *t_ai;               /* whichever instance F and G drive */

static VOID t_phase_e(VOID)
{

NX_AUTO_IP     *ai;
ULONG           conflicts_before;
ULONG           i;
UINT            status;


    t_log("");
    t_log("E. RFC 3927 conflict detection at probe time");

    if (t_ns->ns_AutoIpCreated)
    {
        ai =  &t_ns->ns_AutoIp;
    }
    else
    {
        /* A DHCP-only configuration never creates one, so the test brings its
           own, the same vendored module, on the same live NX_IP. */
        status =  nx_auto_ip_create(&t_own_autoip, (CHAR *) "test AutoIP",
                                    t_ip, t_own_autoip_stack,
                                    (ULONG) sizeof(t_own_autoip_stack), 3);
        if (!t_check((UINT) (status == NX_SUCCESS),
                     "created an AutoIP instance for the conflict test",
                     (ULONG) status))
            return;

        t_own_autoip_created =  TRUE;
        ai =  &t_own_autoip;
    }

    t_ai =  ai;

    /* AutoIP only probes while the interface has no address, so stop DHCP
       fighting over it and clear it. */
    if (t_ns->ns_DhcpCreated)
        (VOID) nx_dhcp_stop(&t_ns->ns_Dhcp);
    (VOID) nx_ip_interface_address_set(t_ip, 0, 0UL, 0UL);
    tx_thread_sleep(T_SEC(1));

    if (!t_saw_state(NX_DHCP_STATE_BOUND, 0UL))
    {
        t_log("  nothing answers on this wire, skipped (see F and G)");
        return;
    }

    conflicts_before =  ai->nx_auto_ip_conflict_count;

    status =  nx_auto_ip_start(ai, IP_ADDRESS(10, 0, 2, 2));
    t_log("  nx_auto_ip_start(10.0.2.2) -> %ld", (ULONG) status);

    for (i = 0; i < 400UL; i++)
    {
        if (ai->nx_auto_ip_conflict_count > conflicts_before)
            break;
        tx_thread_sleep(NX_IP_PERIODIC_RATE / 10UL);
    }

    t_log("  conflicts %ld -> %ld, probes %ld", conflicts_before,
          ai->nx_auto_ip_conflict_count, ai->nx_auto_ip_probe_count);

    (VOID) t_check((UINT) (ai->nx_auto_ip_conflict_count > conflicts_before),
                   "the probe was answered and counted as a conflict",
                   ai->nx_auto_ip_conflict_count);

    /* After a conflict RFC 3927 section 2.2.1 says pick another address. */
    for (i = 0; i < 400UL; i++)
    {
        if (t_is_linklocal(ai->nx_auto_ip_current_local_address))
            break;
        tx_thread_sleep(NX_IP_PERIODIC_RATE / 10UL);
    }

    t_log_ip("next candidate", ai->nx_auto_ip_current_local_address);
    (VOID) t_check((UINT) t_is_linklocal(ai->nx_auto_ip_current_local_address),
                   "a fresh candidate was drawn from the link-local range",
                   ai->nx_auto_ip_current_local_address);

    if (t_wait_address(30UL))
    {
        t_log_ip("address after the retry", t_address());
        (VOID) t_check((UINT) t_is_linklocal(t_address()),
                       "the retry claimed a link-local address", t_address());

        for (i = 0; i < 200UL; i++)
        {
            if (ai->nx_auto_ip_announce_count > 0UL)
                break;
            tx_thread_sleep(NX_IP_PERIODIC_RATE / 10UL);
        }

        t_log("  probes %ld, announces %ld, defends %ld",
              ai->nx_auto_ip_probe_count, ai->nx_auto_ip_announce_count,
              ai->nx_auto_ip_defend_count);

        (VOID) t_check((UINT) (ai->nx_auto_ip_probe_count >=
                               (ULONG) (NX_AUTO_IP_PROBE_NUM + 1)),
                       "NX_AUTO_IP_PROBE_NUM probes went out for the retry",
                       ai->nx_auto_ip_probe_count);
        (VOID) t_check((UINT) (ai->nx_auto_ip_announce_count > 0UL),
                       "the claim was announced by ARP (RFC 3927 2.4)",
                       ai->nx_auto_ip_announce_count);
    }
    else
    {
        (VOID) t_check(0, "the retry claimed an address", 0UL);
    }

    t_log("  conflict count now %ld of NX_AUTO_IP_MAX_CONFLICTS %ld",
          ai->nx_auto_ip_conflict_count, (ULONG) NX_AUTO_IP_MAX_CONFLICTS);
}


extern VOID _nx_arp_packet_deferred_receive(NX_IP *ip_ptr, NX_PACKET *packet_ptr);

#define T_PEER_MAC_MSW      0x0000DEADUL
#define T_PEER_MAC_LSW      0xBEEF0001UL

static ULONG t_arp_injected;

static BOOL t_inject_arp(ULONG op, ULONG sender_ip, ULONG target_ip)
{

NX_PACKET  *packet =  NX_NULL;
ULONG      *msg;


    if (nx_packet_allocate(netstack_pool(), &packet, NX_RECEIVE_PACKET,
                           NX_NO_WAIT) != NX_SUCCESS)
        return(FALSE);

    packet->nx_packet_length      =  NX_ARP_MESSAGE_SIZE;
    packet->nx_packet_append_ptr  =  packet->nx_packet_prepend_ptr +
                                     NX_ARP_MESSAGE_SIZE;
    packet->nx_packet_address.nx_packet_interface_ptr =
        &t_ip->nx_ip_interface[0];

    msg =  (ULONG *) packet->nx_packet_prepend_ptr;

    /* 68k is big-endian, so NX_CHANGE_ULONG_ENDIAN is a no-op and the words
       go in exactly as nx_arp_packet_receive.c reads them back. */
    msg[0] =  ((ULONG) NX_ARP_HARDWARE_TYPE << 16) | (ULONG) NX_ARP_PROTOCOL_TYPE;
    msg[1] =  ((ULONG) NX_ARP_HARDWARE_SIZE << 24) |
              ((ULONG) NX_ARP_PROTOCOL_SIZE << 16) | op;
    msg[2] =  (T_PEER_MAC_MSW << 16) | (T_PEER_MAC_LSW >> 16);
    msg[3] =  (T_PEER_MAC_LSW << 16) | (sender_ip >> 16);
    msg[4] =  (sender_ip << 16);
    msg[5] =  0UL;
    msg[6] =  target_ip;

    t_arp_injected++;
    _nx_arp_packet_deferred_receive(t_ip, packet);

    return(TRUE);
}

static VOID t_phase_h(VOID)
{

NX_AUTO_IP *ai =  t_ai;
ULONG       probe_at[8];
ULONG       announce_at[8];
ULONG       probes =  0UL;
ULONG       announces =  0UL;
ULONG       claim_at =  0UL;
ULONG       start;
ULONG       last_probe;
ULONG       last_announce;
ULONG       i;
BOOL        spacing_ok =  TRUE;


    t_log("");
    t_log("H. probe and announce timing (RFC 3927 sections 2.2.1 and 2.4)");

    if (ai == NX_NULL)
        return;

    (VOID) nx_ip_interface_address_set(t_ip, 0, 0UL, 0UL);
    tx_thread_sleep(T_SEC(6));

    last_probe    =  ai->nx_auto_ip_probe_count;
    last_announce =  ai->nx_auto_ip_announce_count;

    start =  tx_time_get();
    (VOID) nx_auto_ip_start(ai, 0UL);

    for (i = 0; i < 2000UL; i++)
    {
        if ((ai->nx_auto_ip_probe_count != last_probe) && (probes < 8UL))
        {
            last_probe =  ai->nx_auto_ip_probe_count;
            probe_at[probes++] =  tx_time_get() - start;
        }

        if ((claim_at == 0UL) && t_is_linklocal(t_address()))
            claim_at =  tx_time_get() - start;

        if ((ai->nx_auto_ip_announce_count != last_announce) && (announces < 8UL))
        {
            last_announce =  ai->nx_auto_ip_announce_count;
            announce_at[announces++] =  tx_time_get() - start;
        }

        if (announces >= (ULONG) NX_AUTO_IP_ANNOUNCE_NUM)
            break;

        tx_thread_sleep(1);
    }

    t_log("  %ld probes, claim at %ld ticks, %ld announcements", probes,
          claim_at, announces);

    for (i = 0; i < probes; i++)
        t_log("    probe %ld at %ld ticks (%ld ms)", i + 1UL, probe_at[i],
              (probe_at[i] * 1000UL) / (ULONG) NX_IP_PERIODIC_RATE);
    for (i = 0; i < announces; i++)
        t_log("    announce %ld at %ld ticks (%ld ms)", i + 1UL, announce_at[i],
              (announce_at[i] * 1000UL) / (ULONG) NX_IP_PERIODIC_RATE);

    (VOID) t_check((UINT) (probes == (ULONG) NX_AUTO_IP_PROBE_NUM),
                   "exactly NX_AUTO_IP_PROBE_NUM probes were sent", probes);

    if (probes > 0UL)
        (VOID) t_check((UINT) (probe_at[0] <=
                               (T_SEC(NX_AUTO_IP_PROBE_WAIT) +
                                T_SEC(NX_AUTO_IP_PROBE_MAX))),
                       "the first probe waited no more than PROBE_WAIT",
                       probe_at[0]);

    for (i = 1UL; i < probes; i++)
    {
        ULONG gap =  probe_at[i] - probe_at[i - 1UL];

        if ((gap < (T_SEC(NX_AUTO_IP_PROBE_MIN) - 5UL)) ||
            (gap > (T_SEC(NX_AUTO_IP_PROBE_MAX) + 5UL)))
        {
            t_log("    probe gap %ld ticks is outside PROBE_MIN..PROBE_MAX",
                  gap);
            spacing_ok =  FALSE;
        }
    }

    (VOID) t_check((UINT) spacing_ok,
                   "probe spacing is inside PROBE_MIN..PROBE_MAX", 0UL);

    (VOID) t_check((UINT) (announces == (ULONG) NX_AUTO_IP_ANNOUNCE_NUM),
                   "exactly NX_AUTO_IP_ANNOUNCE_NUM announcements were sent",
                   announces);

    if ((announces > 0UL) && (claim_at != 0UL))
        (VOID) t_check((UINT) ((announce_at[0] - claim_at) >=
                               (T_SEC(NX_AUTO_IP_ANNOUNCE_WAIT) - 5UL)),
                       "the first announcement waited ANNOUNCE_WAIT",
                       announce_at[0] - claim_at);

    if (announces > 1UL)
    {
        ULONG gap =  announce_at[1] - announce_at[0];

        t_log("    announce interval %ld ticks (ANNOUNCE_INTERVAL %ld s)", gap,
              (ULONG) NX_AUTO_IP_ANNOUNCE_INTERVAL);
        (VOID) t_check((UINT) (gap >= (T_SEC(NX_AUTO_IP_ANNOUNCE_INTERVAL) - 5UL)),
                       "the announcements were ANNOUNCE_INTERVAL apart", gap);
    }
}


static VOID t_phase_f(VOID)
{

NX_AUTO_IP *ai =  t_ai;
ULONG       last_probe;
ULONG       i;
ULONG       start;
ULONG       gap;


    t_log("");
    t_log("F. rate limiting after repeated conflicts (RFC 3927 section 2.2.1)");

    if (ai == NX_NULL)
        return;

    (VOID) nx_ip_interface_address_set(t_ip, 0, 0UL, 0UL);
    (VOID) nx_auto_ip_start(ai, 0UL);

    last_probe =  ai->nx_auto_ip_probe_count;

    for (i = 0; i < 4000UL; i++)
    {
        if (ai->nx_auto_ip_probe_count != last_probe)
        {
            last_probe =  ai->nx_auto_ip_probe_count;
            (VOID) t_inject_arp((ULONG) NX_ARP_OPTION_RESPONSE,
                                t_ip->nx_ip_interface[0].nx_interface_ip_probe_address,
                                t_ip->nx_ip_interface[0].nx_interface_ip_probe_address);
        }

        if (ai->nx_auto_ip_conflict_count > (ULONG) NX_AUTO_IP_MAX_CONFLICTS)
            break;

        tx_thread_sleep(2);
    }

    t_log("  %ld conflicts after %ld injected replies, probes %ld",
          ai->nx_auto_ip_conflict_count, t_arp_injected,
          ai->nx_auto_ip_probe_count);

    if (!t_check((UINT) (ai->nx_auto_ip_conflict_count >
                         (ULONG) NX_AUTO_IP_MAX_CONFLICTS),
                 "more than NX_AUTO_IP_MAX_CONFLICTS conflicts were counted",
                 ai->nx_auto_ip_conflict_count))
        return;

    last_probe =  ai->nx_auto_ip_probe_count;
    start      =  tx_time_get();

    for (i = 0; i < 900UL; i++)
    {
        if (ai->nx_auto_ip_probe_count != last_probe)
            break;
        tx_thread_sleep(NX_IP_PERIODIC_RATE / 10UL);
    }

    gap =  (tx_time_get() - start) / (ULONG) NX_IP_PERIODIC_RATE;
    t_log("  next probe came %ld s later (NX_AUTO_IP_RATE_LIMIT_INTERVAL %ld)",
          gap, (ULONG) NX_AUTO_IP_RATE_LIMIT_INTERVAL);

    (VOID) t_check((UINT) (gap >= ((ULONG) NX_AUTO_IP_RATE_LIMIT_INTERVAL - 2UL)),
                   "the retry was held off for the rate-limit interval", gap);
}

static VOID t_phase_g(VOID)
{

NX_AUTO_IP *ai =  t_ai;
ULONG       defends_before;
ULONG       responses_before;
ULONG       requests_before;
ULONG       addr;
ULONG       i;


    t_log("");
    t_log("G. a conflict AFTER the address is in use (RFC 3927 section 2.5)");

    if (ai == NX_NULL)
        return;

    (VOID) nx_ip_interface_address_set(t_ip, 0, 0UL, 0UL);
    (VOID) nx_auto_ip_start(ai, 0UL);

    if (!t_check((UINT) t_wait_address(120UL),
                 "AutoIP settled on an address to defend", 0UL))
        return;

    addr =  t_address();
    t_log_ip("defending", addr);

    tx_thread_sleep(T_SEC(6));

    defends_before   =  ai->nx_auto_ip_defend_count;
    responses_before =  t_ip->nx_ip_arp_responses_sent;
    requests_before  =  t_ip->nx_ip_arp_requests_sent;

    t_log("  arp defend timeout before: %ld",
          t_ip->nx_ip_interface[0].nx_interface_arp_defend_timeout);

    (VOID) t_inject_arp((ULONG) NX_ARP_OPTION_REQUEST, addr, addr);

    for (i = 0; i < 200UL; i++)
    {
        if (ai->nx_auto_ip_defend_count != defends_before)
            break;
        tx_thread_sleep(NX_IP_PERIODIC_RATE / 10UL);
    }

    t_log("  defends %ld -> %ld, arp requests sent %ld -> %ld, "
          "responses sent %ld -> %ld", defends_before,
          ai->nx_auto_ip_defend_count, requests_before,
          t_ip->nx_ip_arp_requests_sent, responses_before,
          t_ip->nx_ip_arp_responses_sent);
    t_log("  arp defend timeout after: %ld (NX_ARP_DEFEND_INTERVAL %ld)",
          t_ip->nx_ip_interface[0].nx_interface_arp_defend_timeout,
          (ULONG) NX_ARP_DEFEND_INTERVAL);

    (VOID) t_check((UINT) (t_ip->nx_ip_arp_requests_sent > requests_before),
                   "an ARP announcement went out in defence",
                   t_ip->nx_ip_arp_requests_sent);

    (VOID) t_check((UINT) (ai->nx_auto_ip_defend_count > defends_before),
                   "AutoIP counted the late conflict as a defence",
                   ai->nx_auto_ip_defend_count);

    tx_thread_sleep(T_SEC(2));
    t_log_ip("address after the late conflict", t_address());
}


#ifdef NX_DHCP_CLIENT_SEND_ARP_PROBE
static VOID t_phase_i(VOID)
{

ULONG   probe_addr;
ULONG   i;
ULONG   trace_from;


    t_log("");
    t_log("I. DHCPDECLINE for an address already in use (RFC 2131 4.4.1)");

    if (!t_ns->ns_DhcpCreated)
        return;

    trace_from =  t_dhcp_trace_used;

    (VOID) nx_dhcp_stop(&t_ns->ns_Dhcp);
    (VOID) nx_dhcp_reinitialize(&t_ns->ns_Dhcp);
    (VOID) nx_ip_interface_address_set(t_ip, 0, 0UL, 0UL);
    (VOID) nx_dhcp_start(&t_ns->ns_Dhcp);

    probe_addr =  0UL;
    for (i = 0; i < 4000UL; i++)
    {
        if (t_dhcp_state_last == (UBYTE) NX_DHCP_STATE_ADDRESS_PROBING)
        {
            probe_addr =  t_ip->nx_ip_interface[0].nx_interface_ip_probe_address;
            if (probe_addr != 0UL)
                break;
        }
        tx_thread_sleep(1);
    }

    (VOID) t_check((UINT) (probe_addr != 0UL),
                   "the client ARP-probed the address it was offered",
                   probe_addr);
    if (probe_addr == 0UL)
        return;

    t_log_ip("client is probing", probe_addr);

    (VOID) t_inject_arp((ULONG) NX_ARP_OPTION_RESPONSE, probe_addr, probe_addr);

    for (i = 0; i < 600UL; i++)
    {
        if (t_saw_state(NX_DHCP_STATE_INIT, trace_from) &&
            (t_dhcp_state_last != (UBYTE) NX_DHCP_STATE_ADDRESS_PROBING))
            break;
        tx_thread_sleep(1);
    }

    t_log("  conflict flag %ld, state now %ld",
          (ULONG) t_ns->ns_Dhcp.nx_dhcp_interface_conflict_flag,
          (ULONG) t_dhcp_state_last);

    (VOID) t_check((UINT) t_saw_state(NX_DHCP_STATE_INIT, trace_from),
                   "the conflict sent the client back to INIT (a DECLINE)",
                   (ULONG) t_dhcp_state_last);

    (VOID) t_check((UINT) (t_address() != probe_addr),
                   "the disputed address was not put on the interface",
                   t_address());

    if (!t_wait_state(NX_DHCP_STATE_BOUND, 60UL))
    {
        (VOID) nx_dhcp_stop(&t_ns->ns_Dhcp);
        (VOID) nx_dhcp_reinitialize(&t_ns->ns_Dhcp);
        (VOID) nx_ip_interface_address_set(t_ip, 0, 0UL, 0UL);
        (VOID) nx_dhcp_start(&t_ns->ns_Dhcp);
    }

    (VOID) t_check((UINT) t_wait_state(NX_DHCP_STATE_BOUND, 60UL),
                   "the client leased an address again after the DECLINE",
                   (ULONG) t_dhcp_state_last);
    t_log_ip("address after the DECLINE", t_address());
}
#endif /* NX_DHCP_CLIENT_SEND_ARP_PROBE */


static VOID t_autoip_release(VOID)
{

    if (t_ai != NX_NULL)
        (VOID) nx_auto_ip_stop(t_ai);

    if (t_own_autoip_created)
    {
        (VOID) nx_auto_ip_delete(&t_own_autoip);
        t_own_autoip_created =  FALSE;
    }
    t_ai =  NX_NULL;

    (VOID) nx_ip_interface_address_set(t_ip, 0, 0UL, 0UL);

    if (t_ns->ns_DhcpCreated)
    {
        (VOID) nx_dhcp_reinitialize(&t_ns->ns_Dhcp);
        (VOID) nx_dhcp_start(&t_ns->ns_Dhcp);
        (VOID) t_check((UINT) t_wait_state(NX_DHCP_STATE_BOUND, 60UL),
                       "DHCP took the interface back after the AutoIP tests",
                       (ULONG) t_dhcp_state_last);
        t_log_ip("address handed back to DHCP", t_address());
    }
}


static VOID t_run(VOID)
{

UINT    status;


    t_ns =  ami_netstack_raw();
    t_ip =  netstack_ip();

    if (!t_check((UINT) ((t_ns != NULL) && (t_ip != NULL)), "the stack is up",
                 0UL))
        return;

    status =  tx_amiga_adopt_thread(&t_main_thread, "dhcp3927", 16);
    if (!t_check((UINT) (status == TX_SUCCESS), "adopted this Exec Task",
                 (ULONG) status))
        return;

    if (t_ns->ns_DhcpCreated)
    {
        (VOID) nx_dhcp_state_change_notify(&t_ns->ns_Dhcp,
                                           t_dhcp_state_change);
    }

    if (t_ns->ns_AutoIpCreated || t_killlink)
        t_phase_a();

    if (t_ns->ns_DhcpCreated)
    {
        if (t_wait_state(NX_DHCP_STATE_BOUND, 90UL))
        {
            t_phase_b();
            t_phase_c();
            t_phase_e();
            t_phase_h();
            t_phase_f();
            t_phase_g();
            t_autoip_release();
#ifdef NX_DHCP_CLIENT_SEND_ARP_PROBE
            t_phase_i();
#endif
            t_phase_d();
        }
        else
        {
            t_log("  no lease within 90 s, B, C and D skipped");
            (VOID) t_check(0, "a lease to test the lifecycle with",
                           (ULONG) t_dhcp_state_last);
            t_phase_e();
            t_phase_h();
            t_phase_f();
            t_phase_g();
        }
    }
    else
    {
        t_phase_e();
        t_phase_h();
        t_phase_f();
        t_phase_g();
    }

    (VOID) tx_amiga_orphan_thread(&t_main_thread);
}


int main(void)
{

LONG    status;
UBYTE   mode[32];


    mode[0] =  '\0';
    if (GetVar((CONST_STRPTR) "DHCP3927MODE", mode, (LONG) sizeof(mode) - 1,
               GVF_GLOBAL_ONLY) > 0)
    {
        if ((mode[0] == 'k') || (mode[0] == 'K'))
            t_killlink =  TRUE;
    }

    t_log("AmiNetXDuo, DHCP lifecycle and RFC 3927 link-local");
    t_log("  mode '%s', ticks/sec %ld", (mode[0] != '\0') ? (char *) mode
                                                          : "lease",
          (ULONG) NX_IP_PERIODIC_RATE);

    ami_crash_set_reference((APTR) main, "dhcp3927_test");
    if (!ami_crash_install())
    {
        t_log("FATAL: caught a CPU exception, see the dump above");
        t_flush();
        return(20);
    }

    if (t_killlink)
        t_linkctl_start();

    status =  netstack_startup();
    t_log("  netstack_startup() -> %ld, %ld ThreadX ticks since the kernel "
          "started", (ULONG) status, tx_time_get());

    if (netstack_get() == NULL)
    {
        t_log("FATAL: no stack, see the messages above");
        t_flush();
        return(20);
    }

    t_run();

    t_log("");
    t_log("%ld checks, %ld failures, %s",
          t_checks, t_failures, (t_failures == 0UL) ? "PASS" : "FAIL");
    t_flush();

    t_log("");
    t_log("tearing down (netstack_shutdown)");
    t_flush();

    netstack_shutdown();

    t_log("%ld checks, %ld failures after teardown, %s",
          t_checks, t_failures, (t_failures == 0UL) ? "PASS" : "FAIL");
    t_flush();
    ami_crash_remove();

    return((t_failures == 0UL) ? 0 : 20);
}

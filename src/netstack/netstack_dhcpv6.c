/*
 * AmiNetXDuo, the DHCPv6 client: the wiring and policy around NetX Duo's
 * addons/dhcp/nxd_dhcpv6_client.c.  Compiled only in an AMINETXDUO_IPV6 build.
 *
 * DUID-LL (RFC 8415 11.4), not the vendored default DUID-LLT: these machines
 * have no clock and no writable stable storage, so a DUID-LLT would be a fresh
 * random identity, and a new address, on every boot.
 *
 * Nothing that moves the client's state machine may run on the IP thread:
 * _nx_dhcpv6_request() sleeps until the client thread is idle, so the RA
 * callback only sets an event flag and the deferred-work thread does the rest.
 *
 * SPDX-License-Identifier: MIT
 */

#include "netstack_internal.h"
#include "dhcpv6_wire.h"

#include "nx_ipv6.h"
#include "nx_icmpv6.h"

#include <proto/exec.h>

_Static_assert(NX_DHCPV6_THREAD_PRIORITY == AMI_DHCPV6_PRIORITY,
               "NX_DHCPV6_THREAD_PRIORITY in nx_user.h must match "
               "AMI_DHCPV6_PRIORITY in thread_priorities.h");

#define AMI_DHCPV6_EV_STATEFUL      0x01UL
#define AMI_DHCPV6_EV_STATELESS     0x02UL
#define AMI_DHCPV6_EV_QUIT          0x80UL

/* The IA_NA identifier.  RFC 8415 12 wants it stable across restarts and
   distinct per interface; the pair (DUID, IAID) identifies the binding. */
#define AMI_DHCPV6_IAID_BASE        1UL

/* Zero, meaning "server, you choose": RFC 8415 21.4 says a client SHOULD set
   T1 and T2 to zero unless it has a reason of its own. */
#define AMI_DHCPV6_T1_HINT          0UL
#define AMI_DHCPV6_T2_HINT          0UL

static const char *ami_ns6_dhcp_state_name(UCHAR state)
{
    switch (state)
    {
    /* Prefixed, because these go to ami_netstack_mark() as well as to the
       log, and a bring-up mark is matched by name -- "bound" or "init" on
       their own belong to nobody. */
    case NX_DHCPV6_STATE_INIT:                   return "dhcp6-init";
    case NX_DHCPV6_STATE_SENDING_SOLICIT:        return "dhcp6-solicit";
    case NX_DHCPV6_STATE_SENDING_REQUEST:        return "dhcp6-request";
    case NX_DHCPV6_STATE_SENDING_RENEW:          return "dhcp6-renew";
    case NX_DHCPV6_STATE_SENDING_REBIND:         return "dhcp6-rebind";
    case NX_DHCPV6_STATE_SENDING_DECLINE:        return "dhcp6-decline";
    case NX_DHCPV6_STATE_SENDING_INFORM_REQUEST: return "dhcp6-inform";
    case NX_DHCPV6_STATE_SENDING_CONFIRM:        return "dhcp6-confirm";
    case NX_DHCPV6_STATE_SENDING_RELEASE:        return "dhcp6-release";
    case NX_DHCPV6_STATE_BOUND_TO_ADDRESS:       return "dhcp6-bound";
    default:                                     return "dhcp6-?";
    }
}

/* Runs on the DHCPv6 client's own thread, so nothing here calls back into
   NetX Duo or the DNS client: a BOUND sets a flag and a caller thread acts. */
static VOID ami_ns6_dhcp_state_changed(struct NX_DHCPV6_STRUCT *dhcpv6_ptr,
                                       UINT old_state, UINT new_state)
{
    AmiNetStack *ns = ami_netstack_raw();
    AmiDhcpv6OptionChange option_change;

    if (ns == NULL || dhcpv6_ptr != &ns->ns_Dhcpv6)
        return;

    ns->ns_Dhcpv6State = (UBYTE)new_state;

    AMI_INFO("netstack: DHCPv6 %s -> %s",
             ami_ns6_dhcp_state_name((UCHAR)old_state),
             ami_ns6_dhcp_state_name((UCHAR)new_state));

    ami_netstack_mark(ami_ns6_dhcp_state_name((UCHAR)new_state));

    /* The reply count is consumed on every transition because it is a
       watermark: the client's cumulative counter reads nonzero for every
       exchange after the first successful one. */
    option_change = ami_dhcpv6_option_change(
        new_state == NX_DHCPV6_STATE_BOUND_TO_ADDRESS,
        old_state == NX_DHCPV6_STATE_SENDING_INFORM_REQUEST,
        new_state == NX_DHCPV6_STATE_INIT,
        ami_dhcpv6_inform_reply_seen(
            dhcpv6_ptr->nx_dhcpv6_inform_req_responses,
            &ns->ns_Dhcpv6InformSeen));

    if (option_change == AMI_DHCPV6_OPTIONS_REPLACE)
    {
        /* BOUND and a successful Information-Request both leave coherent
           replacement option buffers in the client. */
        ns->ns_Dhcpv6OptionsValid = TRUE;
        ns->ns_Dhcpv6DnsPending = TRUE;
    }
    else if (option_change == AMI_DHCPV6_OPTIONS_WITHDRAW)
    {
        /* Lease loss, Release, Decline and failed stateful acquisition all
           leave INIT with no live ownership of the previous options. */
        ns->ns_Dhcpv6OptionsValid = FALSE;
        ns->ns_Dhcpv6DnsPending = TRUE;
    }
}

/* Also on the client's own thread.  Logged rather than acted on: the client's
   own retransmission is the response to every status code here. */
static VOID ami_ns6_dhcp_server_error(struct NX_DHCPV6_STRUCT *dhcpv6_ptr,
                                      UINT op_code, UINT status_code,
                                      UINT message_type)
{
    AmiNetStack *ns = ami_netstack_raw();

    if (ns == NULL || dhcpv6_ptr != &ns->ns_Dhcpv6)
        return;

    AMI_WARN("netstack: DHCPv6 server refused: status %ld on option %ld "
             "of message %ld", (long)status_code, (long)op_code,
             (long)message_type);
}

/* nx_dhcpv6_client_create() takes the single nxd_ipv6_address_change_notify()
   slot; ami_ns6_address_changed() is re-registered after it and chains here,
   so both this stack's handler and the vendored client's run. */
VOID ami_netstack_dhcpv6_address_notify(NX_IP *ip_ptr, UINT status,
                                        UINT interface_index,
                                        UINT address_index, ULONG *address)
{
    AmiNetStack *ns = ami_netstack_raw();

    if (ns == NULL || !ns->ns_Dhcpv6Created)
        return;

    _nx_dhcpv6_ipv6_address_DAD_notify(ip_ptr, status, interface_index,
                                       address_index, address);
}

/* IP thread: one tx_event_flags_set() and nothing else, because nothing that
   moves the client's state machine may block here. */
static VOID ami_ns6_ra_flags(NX_IP *ip_ptr, UINT ra_flag)
{
    AmiNetStack *ns = ami_netstack_raw();
    ULONG        want;

    if (ns == NULL || ip_ptr != &ns->ns_Ip || !ns->ns_Dhcpv6WorkReady)
        return;

    switch (ami_dhcpv6_action_for_ra(ra_flag))
    {
    case AMI_DHCPV6_ACT_STATEFUL:  want = AMI_DHCPV6_EV_STATEFUL;  break;
    case AMI_DHCPV6_ACT_STATELESS: want = AMI_DHCPV6_EV_STATELESS; break;
    default:                       return;
    }

    /* The router re-advertises for the life of the machine and every
       advertisement repeats the flags, so the first one that asks for
       something wins and the rest are dropped here. */
    if (ns->ns_Dhcpv6Asked)
        return;

    ns->ns_Dhcpv6Asked = TRUE;

    (VOID)tx_event_flags_set(&ns->ns_Dhcpv6Events, want, TX_OR);
}

/* A create succeeded but the object could not be configured: delete every
   resource and restore this stack's chained DAD notify.  ns_Dhcpv6Asked stays
   latched -- every call below refuses only on constants, so it always will. */
static LONG ami_ns6_dhcp_discard_partial(AmiNetStack *ns)
{
    if (ns != NULL && ns->ns_Dhcpv6Created)
    {
        (VOID)nx_dhcpv6_client_delete(&ns->ns_Dhcpv6);
        ns->ns_Dhcpv6Created = FALSE;
        ami_netstack_ipv6_reclaim_notify(ns);
    }

    return AMI_NET_ERR_KERNEL;
}

/* The link-layer address the DUID is built from, read before the client is
   created: an interface that cannot name a DUID-LL cannot run DHCPv6, and the
   address is fixed for as long as the interface stays attached. */
static BOOL ami_ns6_dhcp_link_address(const AmiNetStack *ns, ULONG *msw,
                                      ULONG *lsw)
{
    const NX_INTERFACE *ifp = &ns->ns_Ip.nx_ip_interface[ns->ns_Dhcpv6Iface];
    UBYTE               want[AMI_DHCPV6_DUID_LL_LEN];
    UBYTE               mac[6];

    *msw = ifp->nx_interface_physical_address_msw;
    *lsw = ifp->nx_interface_physical_address_lsw;

    mac[0] = (UBYTE)((*msw >> 8) & 0xFFUL);
    mac[1] = (UBYTE)(*msw & 0xFFUL);
    mac[2] = (UBYTE)((*lsw >> 24) & 0xFFUL);
    mac[3] = (UBYTE)((*lsw >> 16) & 0xFFUL);
    mac[4] = (UBYTE)((*lsw >> 8) & 0xFFUL);
    mac[5] = (UBYTE)(*lsw & 0xFFUL);

    return (BOOL)(ami_dhcpv6_duid_ll(mac, 6UL, want, sizeof(want)) != 0UL);
}

/*
 * Build and start the client. Runs on the deferred-work thread, or on the
 * bring-up thread for CONFIGURE6=DHCP, both of which may block.
 */
static LONG ami_ns6_dhcp_begin(AmiNetStack *ns, BOOL stateful)
{
    UINT  status;
    ULONG msw = 0UL;
    ULONG lsw = 0UL;

    if (!ns->ns_Dhcpv6Created)
    {
        /* This refusal does not clear ns_Dhcpv6Asked either: the address is
           fixed while the interface is attached, so it cannot change. */
        if (!ami_ns6_dhcp_link_address(ns, &msw, &lsw))
        {
            AMI_ERROR("netstack: DHCPv6 has no usable DUID, the card "
                      "reported no hardware address");
            return AMI_NET_ERR_NODEV;
        }

        /* Under the IP protection mutex: nx_dhcpv6_client_create() registers
           the client's DAD callback before assigning the file-static that
           callback dereferences, and the IP thread runs DAD under this lock. */
        tx_mutex_get(&ns->ns_Ip.nx_ip_protection, TX_WAIT_FOREVER);

        status = nx_dhcpv6_client_create(&ns->ns_Dhcpv6, &ns->ns_Ip,
                                         (CHAR *)"AmiNetXDuo DHCPv6",
                                         &ns->ns_Pool,
                                         ns->ns_Dhcpv6Stack,
                                         (ULONG)AMI_DHCPV6_STACK_SIZE,
                                         ami_ns6_dhcp_state_changed,
                                         ami_ns6_dhcp_server_error);

        if (status == NX_SUCCESS)
        {
            ns->ns_Dhcpv6Created = TRUE;

            /* The create memsets the whole NX_DHCPV6, so the reply counter
               ami_dhcpv6_inform_reply_seen() watches restarts at zero and the
               watermark has to restart with it. */
            ns->ns_Dhcpv6InformSeen = 0UL;

            /* Take the address-change slot back: the create above pointed it
               at the client's own DAD handler, and ours chains to that one. */
            ami_netstack_ipv6_reclaim_notify(ns);
        }

        tx_mutex_put(&ns->ns_Ip.nx_ip_protection);

        if (status != NX_SUCCESS)
        {
            AMI_ERROR("netstack: nx_dhcpv6_client_create failed (%ld)",
                      (long)status);

            /* The one failure here that may read differently next time: a
               resource shortage rather than an argument, and nothing was
               created, so unlatch and let the next advertisement try again. */
            ns->ns_Dhcpv6Asked = FALSE;
            return AMI_NET_ERR_KERNEL;
        }

        status = nx_dhcpv6_client_set_interface(&ns->ns_Dhcpv6,
                                                (UINT)ns->ns_Dhcpv6Iface);
        if (status != NX_SUCCESS)
        {
            AMI_ERROR("netstack: DHCPv6 interface selection failed (%ld)",
                      (long)status);
            return ami_ns6_dhcp_discard_partial(ns);
        }

        /* The DUID. See the file header for why DUID-LL and not DUID-LLT. */
        status = nx_dhcpv6_create_client_duid(&ns->ns_Dhcpv6,
                                              NX_DHCPV6_DUID_TYPE_LINK_ONLY,
                                              NX_DHCPV6_CLIENT_HARDWARE_TYPE_ETHERNET,
                                              0UL);
        if (status != NX_SUCCESS)
        {
            AMI_ERROR("netstack: DHCPv6 DUID failed (%ld)", (long)status);
            return ami_ns6_dhcp_discard_partial(ns);
        }

        /* Against ami_dhcpv6_duid_ll(), the wire form pinned by
           tests/ipv6/host/test_dhcpv6_host.c.  It warns rather than refuses:
           a DUID this stack did not predict is still a stable identity. */
        if (ns->ns_Dhcpv6.nx_dhcpv6_client_duid.nx_duid_type != (USHORT)3 ||
            ns->ns_Dhcpv6.nx_dhcpv6_client_duid.nx_hardware_type != (USHORT)1 ||
            ns->ns_Dhcpv6.nx_dhcpv6_client_duid.nx_option_length
                != (USHORT)AMI_DHCPV6_DUID_LL_LEN ||
            ns->ns_Dhcpv6.nx_dhcpv6_client_duid.nx_link_layer_address_msw
                != msw ||
            ns->ns_Dhcpv6.nx_dhcpv6_client_duid.nx_link_layer_address_lsw
                != lsw)
        {
            AMI_WARN("netstack: the DHCPv6 client built a DUID this "
                     "stack did not ask for (type %ld, hw %ld, len %ld)",
                     (long)ns->ns_Dhcpv6.nx_dhcpv6_client_duid.nx_duid_type,
                     (long)ns->ns_Dhcpv6.nx_dhcpv6_client_duid.nx_hardware_type,
                     (long)ns->ns_Dhcpv6.nx_dhcpv6_client_duid.nx_option_length);
        }

        /* nx_dhcpv6_start() refuses a zero-length IANA even for an
           Information-Request, which then carries no IA option at all
           (RFC 8415 21.4).  So this is built in both modes and used in one. */
        status = nx_dhcpv6_create_client_iana(&ns->ns_Dhcpv6,
                                              AMI_DHCPV6_IAID_BASE +
                                                  (ULONG)ns->ns_Dhcpv6Iface,
                                              AMI_DHCPV6_T1_HINT,
                                              AMI_DHCPV6_T2_HINT);
        if (status != NX_SUCCESS)
        {
            AMI_ERROR("netstack: DHCPv6 IA_NA failed (%ld)", (long)status);
            return ami_ns6_dhcp_discard_partial(ns);
        }

        /* Without these in the option request list a conforming server has no
           reason to send either.  Asked for in both modes. */
        status = nx_dhcpv6_request_option_DNS_server(&ns->ns_Dhcpv6,
                                                     NX_TRUE);
        if (status == NX_SUCCESS)
            status = nx_dhcpv6_request_option_domain_name(&ns->ns_Dhcpv6,
                                                          NX_TRUE);
        if (status != NX_SUCCESS)
        {
            AMI_ERROR("netstack: DHCPv6 option request setup failed (%ld)",
                      (long)status);
            return ami_ns6_dhcp_discard_partial(ns);
        }

    }

    /* A link-down stops, but deliberately does not delete, the client.  Its
       DUID and IA_NA remain valid and nx_dhcpv6_start() resumes its thread,
       socket and timers before the repeated request below. */
    if (!ns->ns_Dhcpv6Started)
    {
        status = nx_dhcpv6_start(&ns->ns_Dhcpv6);
        if (status != NX_SUCCESS)
        {
            AMI_ERROR("netstack: nx_dhcpv6_start failed (%ld)", (long)status);
            return AMI_NET_ERR_KERNEL;
        }

        ns->ns_Dhcpv6Started = TRUE;
    }

    ns->ns_Dhcpv6Stateful = stateful;

    if (stateful)
    {
        status = nx_dhcpv6_request_solicit(&ns->ns_Dhcpv6);
        AMI_INFO("netstack: DHCPv6 soliciting an address");
    }
    else
    {
        status = nx_dhcpv6_request_inform_request(&ns->ns_Dhcpv6);
        AMI_INFO("netstack: DHCPv6 asking for the other configuration");
    }

    if (status != NX_SUCCESS)
    {
        AMI_WARN("netstack: DHCPv6 request refused (%ld)", (long)status);
        return AMI_NET_ERR_KERNEL;
    }

    return AMI_NET_OK;
}

static VOID ami_ns6_dhcp_worker(ULONG arg)
{
    AmiNetStack *ns = (AmiNetStack *)arg;

    for (;;)
    {
        ULONG events = 0;

        if (tx_event_flags_get(&ns->ns_Dhcpv6Events,
                               AMI_DHCPV6_EV_STATEFUL |
                               AMI_DHCPV6_EV_STATELESS |
                               AMI_DHCPV6_EV_QUIT,
                               TX_OR_CLEAR, &events,
                               TX_WAIT_FOREVER) != TX_SUCCESS)
        {
            return;
        }

        if ((events & AMI_DHCPV6_EV_QUIT) != 0UL)
            return;

        if ((events & AMI_DHCPV6_EV_STATEFUL) != 0UL)
            (VOID)ami_ns6_dhcp_begin(ns, TRUE);
        else if ((events & AMI_DHCPV6_EV_STATELESS) != 0UL)
            (VOID)ami_ns6_dhcp_begin(ns, FALSE);
    }
}

/* Whether any interface wants DHCPv6 at all, outright or by asking to be told.
   FALSE keeps the thread, the stack and the flag group off a machine that will
   never use them. */
static UWORD ami_ns6_dhcp_interface(const AmiNetStack *ns, BOOL *outright)
{
    UWORD i;

    *outright = FALSE;

    for (i = 0; i < ns->ns_IfaceCount; i++)
    {
        AmiIp6Type t = ns->ns_Config.interfaces[i].ip6type;

        if (t == AMI_IP6TYPE_DHCP)
        {
            *outright = TRUE;
            return i;
        }
    }

    for (i = 0; i < ns->ns_IfaceCount; i++)
    {
        if (ns->ns_Config.interfaces[i].ip6type == AMI_IP6TYPE_AUTO)
            return i;
    }

    return (UWORD)ns->ns_IfaceCount;
}

VOID ami_netstack_dhcpv6_configure(AmiNetStack *ns)
{
    BOOL  outright = FALSE;
    UWORD iface;

    if (!ns->ns_Ipv6Enabled || ns->ns_Dhcpv6WorkReady)
        return;

    iface = ami_ns6_dhcp_interface(ns, &outright);
    if (iface >= ns->ns_IfaceCount)
        return;

    ns->ns_Dhcpv6Iface = (UBYTE)iface;

    ns->ns_Dhcpv6Stack = ami_alloc_flags((ULONG)AMI_DHCPV6_STACK_SIZE,
                                         MEMF_PUBLIC | MEMF_CLEAR);
    ns->ns_Dhcpv6WorkStack = ami_alloc_flags((ULONG)AMI_DHCPV6_WORK_STACK_SIZE,
                                             MEMF_PUBLIC | MEMF_CLEAR);
    if (ns->ns_Dhcpv6Stack == NULL || ns->ns_Dhcpv6WorkStack == NULL)
    {
        AMI_WARN("netstack: no memory for DHCPv6, IPv6 continues without it");
        ami_netstack_dhcpv6_destroy(ns);
        return;
    }

    if (tx_event_flags_create(&ns->ns_Dhcpv6Events, (CHAR *)"anxd dhcpv6")
            != TX_SUCCESS)
    {
        AMI_WARN("netstack: no DHCPv6 event group, IPv6 continues without it");
        ami_netstack_dhcpv6_destroy(ns);
        return;
    }
    ns->ns_Dhcpv6EventsReady = TRUE;

    if (tx_thread_create(&ns->ns_Dhcpv6Work, (CHAR *)"anxd dhcpv6 work",
                         ami_ns6_dhcp_worker, (ULONG)ns,
                         ns->ns_Dhcpv6WorkStack,
                         (ULONG)AMI_DHCPV6_WORK_STACK_SIZE,
                         AMI_DHCPV6_WORK_PRIORITY, AMI_DHCPV6_WORK_PRIORITY,
                         TX_NO_TIME_SLICE, TX_AUTO_START) != TX_SUCCESS)
    {
        AMI_WARN("netstack: no DHCPv6 thread, IPv6 continues without it");
        ami_netstack_dhcpv6_destroy(ns);
        return;
    }

    ns->ns_Dhcpv6WorkReady = TRUE;

    if (outright)
    {
        /* Through the worker even though CONFIGURE6=DHCP does not wait for a
           router: creating the client and moving it to SENDING_SOLICIT blocks,
           and the bring-up path is the one thing this must not cost. */
        ns->ns_Dhcpv6Asked = TRUE;
        (VOID)tx_event_flags_set(&ns->ns_Dhcpv6Events, AMI_DHCPV6_EV_STATEFUL,
                                 TX_OR);
        AMI_INFO("netstack: DHCPv6 asked for outright by CONFIGURE6");
        return;
    }

    /* AUTO.  Nothing is created and nothing is sent until a router
       advertisement asks for it. */
    (VOID)nxd_icmpv6_ra_flag_callback_set(&ns->ns_Ip, ami_ns6_ra_flags);

    AMI_INFO("netstack: DHCPv6 ready, waiting for the router to ask for it");
}

/* Give the address back (RFC 8415 18.2.7).  This blocks, and must run while
   the interface can still transmit: the call site in netstack.c is ahead of
   the NX_LINK_DISABLE rather than after it. */
VOID ami_netstack_dhcpv6_release(AmiNetStack *ns)
{
    ULONG waited;
    ULONG sent_before;
    ULONG answered_before;

    if (ns == NULL || !ns->ns_Dhcpv6Created || !ns->ns_Dhcpv6Started)
        return;

    /* Only from a ThreadX thread: the mutex this takes and the sleep below are
       both caller errors outside one. */
    if (tx_thread_identify() == TX_NULL)
        return;

    /* The client's own state, NOT ns_Dhcpv6State: that mirror is written on
       the client's thread and lags every transition, so it can miss a lease
       that has just been taken. */
    if (ns->ns_Dhcpv6.nx_dhcpv6_state != NX_DHCPV6_STATE_BOUND_TO_ADDRESS ||
        !ns->ns_Dhcpv6Stateful)
    {
        /* Nothing was assigned, so there is nothing to give back. An
           Information-Request holds no lease. */
        return;
    }

    sent_before     = ns->ns_Dhcpv6.nx_dhcpv6_releases_sent;
    answered_before = ns->ns_Dhcpv6.nx_dhcpv6_release_responses;

    if (nx_dhcpv6_request_release(&ns->ns_Dhcpv6) != NX_SUCCESS)
    {
        AMI_WARN("netstack: the DHCPv6 Release was refused, the address stays "
                 "leased until it expires");
        return;
    }

    /* Wait for something that happened, not for a state that is transiently
       false: the exit needs the send counter to have moved AND then either the
       server's Reply or the client leaving SENDING_RELEASE. */
    for (waited = 0; waited < AMI_DHCPV6_RELEASE_TICKS; waited++)
    {
        if (ns->ns_Dhcpv6.nx_dhcpv6_releases_sent != sent_before &&
            (ns->ns_Dhcpv6.nx_dhcpv6_release_responses != answered_before ||
             ns->ns_Dhcpv6.nx_dhcpv6_state != NX_DHCPV6_STATE_SENDING_RELEASE))
        {
            break;
        }

        tx_thread_sleep(1);
    }

    /* Bounded: this is on the path of a machine shutting down, and RFC 8415
       18.2.7 lets a client not wait for the Reply at all. */
    if (ns->ns_Dhcpv6.nx_dhcpv6_releases_sent == sent_before)
        AMI_WARN("netstack: the DHCPv6 Release never reached the wire");
    else if (ns->ns_Dhcpv6.nx_dhcpv6_release_responses != answered_before)
        AMI_INFO("netstack: DHCPv6 address released, the server answered");
    else
        AMI_INFO("netstack: DHCPv6 Release sent, the server did not answer");
}

/* Quiesce the client while its interface is down.  Stop is required in all
   modes so a Solicit or Information-Request does not keep retransmitting on an
   offline link; the client object is retained for a restart on link-up. */
VOID ami_netstack_dhcpv6_pause(AmiNetStack *ns)
{
    UINT status;

    if (ns == NULL || !ns->ns_Dhcpv6Created || !ns->ns_Dhcpv6Started)
        return;

    status = nx_dhcpv6_stop(&ns->ns_Dhcpv6);
    if (status == NX_SUCCESS)
    {
        ns->ns_Dhcpv6Started = FALSE;
        ns->ns_Dhcpv6OptionsValid = FALSE;
        ns->ns_Dhcpv6DnsPending = TRUE;
    }
    else
        AMI_WARN("netstack: DHCPv6 did not stop for link-down (%ld)",
                 (long)status);
}

/* Repeat the exchange that was active before link-down.  Only enqueue work
   here: callers are adopted tasks and the request may block. */
VOID ami_netstack_dhcpv6_resume(AmiNetStack *ns, UWORD interface_index)
{
    AmiDhcpv6Action action;
    ULONG           event;

    if (ns == NULL || !ns->ns_Dhcpv6WorkReady)
        return;

    action = ami_dhcpv6_resume_action(ns->ns_Dhcpv6Created,
                                      ns->ns_Dhcpv6Started,
                                      ns->ns_Dhcpv6Asked,
                                      ns->ns_Dhcpv6Stateful,
                                      (UINT)ns->ns_Dhcpv6Iface,
                                      (UINT)interface_index);
    if (action == AMI_DHCPV6_ACT_NONE)
        return;

    event = (action == AMI_DHCPV6_ACT_STATEFUL)
                ? AMI_DHCPV6_EV_STATEFUL : AMI_DHCPV6_EV_STATELESS;
    (VOID)tx_event_flags_set(&ns->ns_Dhcpv6Events, event, TX_OR);
}

/* The client's own nx_dhcpv6_state, NOT ns_Dhcpv6State: that mirror is written
   on the client's thread and lags every transition, which is the same reason
   ami_netstack_dhcpv6_release() reads the client. */
LONG netstack_interface_dhcp6_status(UWORD interface_index,
                                     AmiDhcp6Status *out)
{
    AmiNetStack                *ns = ami_netstack_raw();
    const NX_DHCPV6_IA_ADDRESS *ia;
    UINT                        state;

    if (out == NULL)
        return AMI_NET_ERR_STATE;

    memset(out, 0, sizeof(*out));
    out->ad6_State = (UWORD)AMI_DHCP_IDLE;

    if (ns == NULL || interface_index != (UWORD)ns->ns_Dhcpv6Iface)
        return AMI_NET_OK;

    if (!ns->ns_Dhcpv6Created)
    {
        /* Asked for by a router advertisement, client not built yet. */
        if (ns->ns_Dhcpv6Asked && ns->ns_Dhcpv6WorkReady)
            out->ad6_State = (UWORD)AMI_DHCP_WORKING;
        return AMI_NET_OK;
    }

    state = ns->ns_Dhcpv6.nx_dhcpv6_state;

    out->ad6_RawState = (UWORD)state;
    out->ad6_Stateful = ns->ns_Dhcpv6Stateful;

    if (state == NX_DHCPV6_STATE_BOUND_TO_ADDRESS)
        out->ad6_State = (UWORD)AMI_DHCP_BOUND;
    else if (state != NX_DHCPV6_STATE_INIT)
        out->ad6_State = (UWORD)AMI_DHCP_WORKING;

    if (out->ad6_State != (UWORD)AMI_DHCP_BOUND || !ns->ns_Dhcpv6Stateful)
        return AMI_NET_OK;

    ia = &ns->ns_Dhcpv6.nx_dhcpv6_ia[0];

    out->ad6_Address[0] = ia->nx_global_address.nxd_ip_address.v6[0];
    out->ad6_Address[1] = ia->nx_global_address.nxd_ip_address.v6[1];
    out->ad6_Address[2] = ia->nx_global_address.nxd_ip_address.v6[2];
    out->ad6_Address[3] = ia->nx_global_address.nxd_ip_address.v6[3];

    out->ad6_PreferredSeconds = ia->nx_preferred_lifetime;
    out->ad6_ValidSeconds     = ia->nx_valid_lifetime;
    out->ad6_T1               = ns->ns_Dhcpv6.nx_dhcpv6_iana.nx_T1;
    out->ad6_T2               = ns->ns_Dhcpv6.nx_dhcpv6_iana.nx_T2;

    return AMI_NET_OK;
}

LONG netstack_interface_dhcp6_release(UWORD interface_index)
{
    AmiNetStack    *ns = ami_netstack_raw();
    AmiNetCaller   *caller;
    AmiDhcp6Status  status;

    if (ns == NULL || !ns->ns_Dhcpv6Created || !ns->ns_Dhcpv6Started)
        return AMI_NET_ERR_STATE;

    if (netstack_interface_dhcp6_status(interface_index, &status) != AMI_NET_OK
        || status.ad6_State != (UWORD)AMI_DHCP_BOUND || !status.ad6_Stateful)
        return AMI_NET_ERR_STATE;

    /* ami_netstack_dhcpv6_release() takes the client's mutex and sleeps, both
       of which are caller errors outside a ThreadX thread. */
    caller = ami_netstack_enter_alloc();
    if (caller == NULL)
        return AMI_NET_ERR_KERNEL;

    ami_netstack_dhcpv6_release(ns);

    ami_netstack_leave_free(caller);

    return AMI_NET_OK;
}

VOID ami_netstack_dhcpv6_destroy(AmiNetStack *ns)
{
    if (ns == NULL)
        return;

    /* The RA callback first: it reaches ns_Dhcpv6Events and runs on the IP
       thread, which is still going. */
    if (ns->ns_Ipv6Enabled && ns->ns_IpCreated)
        (VOID)nxd_icmpv6_ra_flag_callback_set(&ns->ns_Ip, NX_NULL);

    ns->ns_Dhcpv6WorkReady = FALSE;

    if (ns->ns_Dhcpv6EventsReady)
    {
        (VOID)tx_event_flags_set(&ns->ns_Dhcpv6Events, AMI_DHCPV6_EV_QUIT,
                                 TX_OR);
    }

    if (ns->ns_Dhcpv6Work.tx_thread_id != 0)
    {
        /* Ask it to leave before taking it apart: a thread caught inside
           ami_ns6_dhcp_begin() holds the client's mutex, and terminating it
           there leaves that mutex owned by a dead thread. */
        if (tx_thread_identify() != TX_NULL)
        {
            ULONG waited;

            for (waited = 0; waited < (ULONG)NX_IP_PERIODIC_RATE; waited++)
            {
                UINT state = 0;

                if (tx_thread_info_get(&ns->ns_Dhcpv6Work, TX_NULL, &state,
                                       TX_NULL, TX_NULL, TX_NULL, TX_NULL,
                                       TX_NULL, TX_NULL) != TX_SUCCESS)
                    break;

                if (state == TX_COMPLETED || state == TX_TERMINATED)
                    break;

                tx_thread_sleep(1);
            }
        }

        (VOID)tx_thread_terminate(&ns->ns_Dhcpv6Work);
        (VOID)tx_thread_delete(&ns->ns_Dhcpv6Work);
    }

    if (ns->ns_Dhcpv6EventsReady)
    {
        (VOID)tx_event_flags_delete(&ns->ns_Dhcpv6Events);
        ns->ns_Dhcpv6EventsReady = FALSE;
    }

    if (ns->ns_Dhcpv6Created)
    {
        if (ns->ns_Dhcpv6Started)
        {
            (VOID)nx_dhcpv6_stop(&ns->ns_Dhcpv6);
            ns->ns_Dhcpv6Started = FALSE;
        }
        (VOID)nx_dhcpv6_client_delete(&ns->ns_Dhcpv6);
        ns->ns_Dhcpv6Created = FALSE;
    }

    if (ns->ns_Dhcpv6WorkStack != NULL)
    {
        ami_free(ns->ns_Dhcpv6WorkStack);
        ns->ns_Dhcpv6WorkStack = NULL;
    }
    if (ns->ns_Dhcpv6Stack != NULL)
    {
        ami_free(ns->ns_Dhcpv6Stack);
        ns->ns_Dhcpv6Stack = NULL;
    }
}

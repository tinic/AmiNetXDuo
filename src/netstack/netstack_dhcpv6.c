/*
 * AmiNetXDuo, the DHCPv6 client.
 *
 * Compiled only in an AMINETXDUO_IPV6 build, like netstack_ipv6.c. The engine
 * is NetX Duo's own addons/dhcp/nxd_dhcpv6_client.c; everything here is the
 * wiring, the policy, and the four places the vendored client and this stack
 * disagreed about who owns something or about what a state means.
 *
 * WHAT ASKS FOR IT
 *
 *   CONFIGURE6 = DHCP   stateful, immediately, without waiting for a router.
 *                       For a network that runs a server and whose router
 *                       does not say so.
 *   CONFIGURE6 = AUTO   whatever the router asks for.  RFC 4861 4.2 gives the
 *                       advertisement two flags: M ("managed") means get an
 *                       address from DHCPv6, O ("other") means get the rest of
 *                       the configuration from it.  M implies O -- a stateful
 *                       exchange already carries the name servers -- so the
 *                       two produce one Solicit or one Information-Request,
 *                       never both.
 *
 * Nothing happens under LINKLOCAL, STATIC or OFF. LINKLOCAL is the mode whose
 * whole point is that the machine talks to nobody, and STATIC is an operator
 * who wrote the address down; neither wants a server's opinion.
 *
 * THE DUID: DUID-LL, AND WHY NOT DUID-LLT
 *
 * RFC 8415 11 offers three. DUID-LLT (type 1) is the one the RFC recommends
 * and is the vendored client's default, and it is wrong here.
 *
 * A DUID must be stable for the life of the machine: a client that arrives
 * with a new DUID is a new client, gets a new address, and leaves the old
 * lease held until it expires. DUID-LLT achieves stability by generating a
 * timestamp once and storing it in non-volatile memory. This machine has no
 * non-volatile memory to store it in that survives a disk swap, and -- the
 * part that decides it -- most of these machines have no battery-backed
 * clock at all. An A500 or an A1200 with a flat battery boots at 1978-01-01
 * every time. _nx_dhcpv6_create_client_duid() answers a zero time by making
 * one up from SECONDS_SINCE_JAN_1_2000_MOD_32 plus NX_RAND(), so on this
 * target a DUID-LLT is a fresh random identity on every boot: a new address
 * every reboot, and a lease table that fills up with this machine.
 *
 * DUID-LL (type 3) is the MAC address and nothing else. It is stable for
 * exactly as long as the card is in the machine, needs no storage and no
 * clock, and RFC 8415 11.4 names this case -- "devices ... that have a
 * permanently connected network interface with a link-layer address, and
 * do not have nonvolatile, writable stable storage" -- as the one it is for.
 * Its stated drawback is that the identity moves with the card rather than
 * with the machine; on an Amiga with one Ethernet card that is a distinction
 * without a difference, and it is the same identity DHCPv4 already uses,
 * because ami_ns_dhcp_client_id() sends RFC 2132 option 61 as the MAC.
 *
 * DUID-EN (type 2) needs an IANA enterprise number, which this project does
 * not have.
 *
 * The consequence worth stating: HARDWAREADDRESS in DEVS:NetInterfaces
 * changes the DUID, because it changes the MAC the DUID is made of. That is
 * correct -- the operator has said this is a different machine on the wire --
 * and it is why the test harness pins a MAC.
 *
 * FOUR THINGS THE VENDORED CLIENT DOES THAT HAD TO BE ANSWERED
 *
 *   1. It takes the single nxd_ipv6_address_change_notify() slot for itself
 *      (nxd_dhcpv6_client.c:1315, whose comment says "other modules should
 *      not set the address change notify function again"). This stack has
 *      used that slot since IPv6 landed: ami_ns6_address_changed() is what
 *      reports every address and emits the ip6-linklocal and ip6-global marks
 *      tests/ipv6/run-bringup.sh measures the boot with. Creating the DHCPv6
 *      client would have silently switched all of that off. So ours is
 *      re-registered after the create and chains to the client's, which is
 *      declared in nxd_dhcpv6_client.h and can be called directly.
 *
 *   2. In that same create, the callback is registered BEFORE the file-static
 *      _nx_dhcpv6_DAD_ptr it dereferences is assigned (:1343), and the
 *      callback does not check it for NULL. Duplicate address detection runs
 *      on the IP thread and is in flight during bring-up, so that window is
 *      real on this target rather than theoretical: a DAD completing inside
 *      it is a null-pointer dereference. Closed here by holding
 *      nx_ip_protection across the create, which is the mutex the IP thread
 *      holds while it runs DAD; see ami_ns6_dhcp_begin().
 *
 *   3. nx_dhcpv6_client_delete() does not clear _nx_dhcpv6_DAD_ptr, so the
 *      pointer dangles at a deleted instance. Harmless here only because the
 *      client's callback is not registered after point 1 above and this
 *      module's chain checks ns_Dhcpv6Created, so nothing calls it. Left
 *      alone rather than worked around twice.
 *
 *   4. A successful Information-Request leaves the client in
 *      NX_DHCPV6_STATE_INIT rather than BOUND, because the state after a
 *      Reply is chosen from the IANA address status (:4147) and an
 *      Information-Request never asks for an address. A caller watching for
 *      BOUND to know the exchange worked never sees it, and the name servers
 *      it carried are recorded and never read. ami_ns6_dhcp_state_changed()
 *      watches the transition out of SENDING_INFORM_REQUEST instead.
 *
 * WHY THERE IS A THREAD HERE THAT DOES ALMOST NOTHING
 *
 * A router advertisement arrives on the IP thread. Everything that moves the
 * DHCPv6 client's state machine blocks: _nx_dhcpv6_request() sleeps a tick at
 * a time until the client thread is idle (nxd_dhcpv6_client.c:5894), and
 * creating the client binds a UDP socket with TX_WAIT_FOREVER. Doing either
 * from the IP thread stalls the thread the DHCPv6 client needs in order to
 * become idle, so it is not a slow path, it is a deadlock.
 *
 * So the RA callback does one non-blocking thing -- tx_event_flags_set() --
 * and this thread does the blocking work. It is created only when some
 * interface asks for AUTO or DHCP, and waits on the flag group forever.
 *
 * Both thread stacks are allocated at bring-up rather than when the client is
 * created, because allocating is an Exec AllocVec() and bring-up is the only
 * one of the two that runs on an adopted task where that is legal. A machine
 * whose CONFIGURE6 is AUTO and whose router asks for no DHCPv6 therefore
 * carries 6 KB it never uses, and sends no packets at all.
 *
 * SPDX-License-Identifier: MIT
 */

#include "netstack_internal.h"
#include "dhcpv6_wire.h"

#include "nx_ipv6.h"
#include "nx_icmpv6.h"

#include <proto/exec.h>

/*
 * The number the client is created with comes from nx_user.h, which the
 * vendored header reads through its own #ifndef. If the two ever disagree the
 * client runs at a priority the ladder in thread_priorities.h did not
 * approve, and nothing would say so.
 */
_Static_assert(NX_DHCPV6_THREAD_PRIORITY == AMI_DHCPV6_PRIORITY,
               "NX_DHCPV6_THREAD_PRIORITY in nx_user.h must match "
               "AMI_DHCPV6_PRIORITY in thread_priorities.h");

/* What the deferred-work thread is being asked to do. */
#define AMI_DHCPV6_EV_STATEFUL      0x01UL
#define AMI_DHCPV6_EV_STATELESS     0x02UL
#define AMI_DHCPV6_EV_QUIT          0x80UL

/*
 * The IA_NA identifier. RFC 8415 12 wants it stable across restarts and
 * distinct per interface; 1 and 2 are, and there is one interface on almost
 * every machine this runs on. It is not derived from the MAC because the DUID
 * already is, and the pair (DUID, IAID) is what identifies the binding.
 */
#define AMI_DHCPV6_IAID_BASE        1UL

/*
 * T1 and T2 asked for in the Solicit: zero, meaning "server, you choose".
 * RFC 8415 21.4 says a client SHOULD set them to zero unless it has a reason,
 * and a client that names its own renewal times on a network it knows nothing
 * about is inventing a policy the server already has.
 */
#define AMI_DHCPV6_T1_HINT          0UL
#define AMI_DHCPV6_T2_HINT          0UL

/* ------------------------------------------------------------- the state, */

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

/*
 * The client's state changed. Runs on the DHCPv6 client's own thread.
 *
 * Nothing here calls back into NetX Duo or the DNS client: a BOUND sets a
 * flag, and ami_ns_dns_absorb_dhcpv6() on a caller thread does the work, for
 * the reason stated above ns_Ra in netstack_internal.h.
 */
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

    option_change = ami_dhcpv6_option_change(
        new_state == NX_DHCPV6_STATE_BOUND_TO_ADDRESS,
        old_state == NX_DHCPV6_STATE_SENDING_INFORM_REQUEST,
        new_state == NX_DHCPV6_STATE_INIT,
        dhcpv6_ptr->nx_dhcpv6_inform_req_responses != 0UL);

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

/*
 * The server said no. Also on the client's own thread.
 *
 * Logged rather than acted on: every status code here means the exchange did
 * not produce what was asked for, and the client's own retransmission is the
 * response to all of them. Reporting it is the point -- a DHCPv6 machine that
 * silently has no address is the state this whole module exists to stop being
 * invisible.
 */
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

/* ------------------------------------------------------ the chained notify,
 *
 * See point 1 in the file header. ami_ns6_address_changed() in netstack_ipv6.c
 * calls this, and this calls the vendored client's, so both run.
 */
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

/* -------------------------------------------------------- the RA callback,
 *
 * IP thread. One tx_event_flags_set() and nothing else; see the file header.
 */
static VOID ami_ns6_ra_flags(NX_IP *ip_ptr, UINT ra_flag)
{
    AmiNetStack *ns = ami_netstack_raw();
    ULONG        want;

    if (ns == NULL || ip_ptr != &ns->ns_Ip || !ns->ns_Dhcpv6WorkReady)
        return;

    /* The mapping itself is in dhcpv6_wire.c, which the host test compiles. */
    switch (ami_dhcpv6_action_for_ra(ra_flag))
    {
    case AMI_DHCPV6_ACT_STATEFUL:  want = AMI_DHCPV6_EV_STATEFUL;  break;
    case AMI_DHCPV6_ACT_STATELESS: want = AMI_DHCPV6_EV_STATELESS; break;
    default:                       return;
    }

    /*
     * Every advertisement repeats the flags, and the router re-advertises
     * every few minutes for the life of the machine. Acting on each one would
     * restart the exchange over and over, so the first one that asks for
     * something wins and the rest are dropped here rather than in the worker.
     */
    if (ns->ns_Dhcpv6Asked)
        return;

    ns->ns_Dhcpv6Asked = TRUE;

    (VOID)tx_event_flags_set(&ns->ns_Dhcpv6Events, want, TX_OR);
}

/* ------------------------------------------------------------ the client, */

/*
 * Build and start the client. Runs on the deferred-work thread, or on the
 * bring-up thread for CONFIGURE6=DHCP, both of which may block.
 */
static LONG ami_ns6_dhcp_begin(AmiNetStack *ns, BOOL stateful)
{
    UINT status;

    if (!ns->ns_Dhcpv6Created)
    {
        /*
         * UNDER THE IP PROTECTION MUTEX, and this is the whole of the fix for
         * point 2 in the file header.
         *
         * nx_dhcpv6_client_create() registers the client's DAD callback and
         * only afterwards assigns the file-static that callback dereferences,
         * and the callback does not check it for NULL. The IP thread is what
         * calls it -- _nx_icmpv6_perform_DAD() runs from
         * nx_ip_thread_entry.c:452, inside the block that holds
         * nx_ip_protection from :242 to :236 -- so holding that mutex across
         * the create means the IP thread cannot be inside the window at all.
         *
         * It is held across the reclaim below as well, so there is no instant
         * at which the client's callback is installed and this stack's is not.
         * Nothing in here blocks: the create makes a socket, a thread, two
         * timers, a mutex and an event group, and the bind that could wait is
         * in nx_dhcpv6_start(), outside. ThreadX mutexes are recursive for the
         * owning thread, so the NetX calls inside taking the same mutex are
         * fine.
         */
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

            /*
             * Take the address-change slot back. The create above pointed it
             * at the client's own DAD handler; ours chains to that one, so
             * both run and every address is still reported.
             */
            ami_netstack_ipv6_reclaim_notify(ns);
        }

        tx_mutex_put(&ns->ns_Ip.nx_ip_protection);

        if (status != NX_SUCCESS)
        {
            AMI_ERROR("netstack: nx_dhcpv6_client_create failed (%ld)",
                      (long)status);
            return AMI_NET_ERR_KERNEL;
        }

        (VOID)nx_dhcpv6_client_set_interface(&ns->ns_Dhcpv6,
                                             (UINT)ns->ns_Dhcpv6Iface);

        /* The DUID. See the file header for why DUID-LL and not DUID-LLT. */
        status = nx_dhcpv6_create_client_duid(&ns->ns_Dhcpv6,
                                              NX_DHCPV6_DUID_TYPE_LINK_ONLY,
                                              NX_DHCPV6_CLIENT_HARDWARE_TYPE_ETHERNET,
                                              0UL);
        if (status != NX_SUCCESS)
        {
            AMI_ERROR("netstack: DHCPv6 DUID failed (%ld)", (long)status);
            return AMI_NET_ERR_KERNEL;
        }

        /*
         * And check that what it built is the DUID this stack means, because
         * "the identity is stable across reboots" is not something the wire
         * shows until the second boot and not something a log line proves.
         * ami_dhcpv6_duid_ll() is the wire form, pinned by
         * tests/ipv6/host/test_dhcpv6_host.c; this compares the fields
         * nx_dhcpv6_create_client_duid() actually stored against it, so a
         * vendored client that changed its mind about the type, the hardware
         * type or the length says so here rather than on somebody's network.
         */
        {
            UBYTE         want[AMI_DHCPV6_DUID_LL_LEN];
            UBYTE         mac[6];
            NX_INTERFACE *ifp = &ns->ns_Ip.nx_ip_interface[ns->ns_Dhcpv6Iface];
            ULONG         msw = ifp->nx_interface_physical_address_msw;
            ULONG         lsw = ifp->nx_interface_physical_address_lsw;

            mac[0] = (UBYTE)((msw >> 8) & 0xFFUL);
            mac[1] = (UBYTE)(msw & 0xFFUL);
            mac[2] = (UBYTE)((lsw >> 24) & 0xFFUL);
            mac[3] = (UBYTE)((lsw >> 16) & 0xFFUL);
            mac[4] = (UBYTE)((lsw >> 8) & 0xFFUL);
            mac[5] = (UBYTE)(lsw & 0xFFUL);

            if (ami_dhcpv6_duid_ll(mac, 6UL, want, sizeof(want)) == 0UL)
            {
                AMI_WARN("netstack: DHCPv6 has no usable DUID, the card "
                         "reported no hardware address");
            }
            else if (ns->ns_Dhcpv6.nx_dhcpv6_client_duid.nx_duid_type
                         != (USHORT)3 ||
                     ns->ns_Dhcpv6.nx_dhcpv6_client_duid.nx_hardware_type
                         != (USHORT)1 ||
                     ns->ns_Dhcpv6.nx_dhcpv6_client_duid.nx_option_length
                         != (USHORT)AMI_DHCPV6_DUID_LL_LEN ||
                     ns->ns_Dhcpv6.nx_dhcpv6_client_duid.nx_link_layer_address_msw
                         != msw ||
                     ns->ns_Dhcpv6.nx_dhcpv6_client_duid.nx_link_layer_address_lsw
                         != lsw)
            {
                AMI_ERROR("netstack: the DHCPv6 client built a DUID this "
                          "stack did not ask for (type %ld, hw %ld, len %ld)",
                          (long)ns->ns_Dhcpv6.nx_dhcpv6_client_duid.nx_duid_type,
                          (long)ns->ns_Dhcpv6.nx_dhcpv6_client_duid.nx_hardware_type,
                          (long)ns->ns_Dhcpv6.nx_dhcpv6_client_duid.nx_option_length);
            }
        }

        /*
         * The IA_NA, which nx_dhcpv6_start() requires even for an
         * Information-Request: it refuses to start with a zero-length IANA
         * (nxd_dhcpv6_client.c:9002), although the Information-Request it
         * then sends carries no IA option at all, which is what RFC 8415 21.4
         * requires. So this is built in both modes and used in one.
         */
        status = nx_dhcpv6_create_client_iana(&ns->ns_Dhcpv6,
                                              AMI_DHCPV6_IAID_BASE +
                                                  (ULONG)ns->ns_Dhcpv6Iface,
                                              AMI_DHCPV6_T1_HINT,
                                              AMI_DHCPV6_T2_HINT);
        if (status != NX_SUCCESS)
        {
            AMI_ERROR("netstack: DHCPv6 IA_NA failed (%ld)", (long)status);
            return AMI_NET_ERR_KERNEL;
        }

        /*
         * The two options worth asking for. Without them they are not in the
         * option request list and a conforming server has no reason to send
         * either, which is the same trap ami_ns_dhcp_configure() documents for
         * DHCPv4. Asked for in both modes: the stateless mode exists only to
         * get them, and the stateful Reply carries them too.
         */
        (VOID)nx_dhcpv6_request_option_DNS_server(&ns->ns_Dhcpv6, NX_TRUE);
        (VOID)nx_dhcpv6_request_option_domain_name(&ns->ns_Dhcpv6, NX_TRUE);

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

/* ------------------------------------------------- the deferred-work thread */

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

/* ---------------------------------------------------------------- bring-up */

/*
 * Whether any interface wants DHCPv6 at all, either outright or by asking to
 * be told. FALSE is the answer on an IPv4-only machine and on one whose only
 * IPv6 mode is LINKLOCAL or STATIC, and it is what keeps the thread, the
 * stack and the flag group off a machine that will never use them.
 */
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
        /*
         * CONFIGURE6=DHCP does not wait for a router to say so -- but it does
         * go through the worker, and that is deliberate rather than tidy.
         * Creating the client and moving it to SENDING_SOLICIT blocks: the
         * bind waits, and _nx_dhcpv6_request() sleeps a tick at a time until
         * the client's own thread has run once. Doing that here would put it
         * on the bring-up path, which is the one thing this was asked not to
         * cost, and the Solicit gets on the wire no sooner for having been
         * sent by this thread.
         */
        ns->ns_Dhcpv6Asked = TRUE;
        (VOID)tx_event_flags_set(&ns->ns_Dhcpv6Events, AMI_DHCPV6_EV_STATEFUL,
                                 TX_OR);
        AMI_INFO("netstack: DHCPv6 asked for outright by CONFIGURE6");
        return;
    }

    /*
     * AUTO. Nothing is created and nothing is sent until a router
     * advertisement asks for it, so a link with no IPv6 router, or one whose
     * router sets neither flag, pays a thread that never wakes and no packets
     * at all.  ami_ns6_arm_solicitation() in netstack_ipv6.c has already made
     * sure the advertisement is asked for rather than waited for.
     */
    (VOID)nxd_icmpv6_ra_flag_callback_set(&ns->ns_Ip, ami_ns6_ra_flags);

    AMI_INFO("netstack: DHCPv6 ready, waiting for the router to ask for it");
}

/* ---------------------------------------------------------------- teardown */

/*
 * Give the address back. RFC 8415 18.2.7: a client that is finished with an
 * address sends a Release so the server can hand it to somebody else, and one
 * that does not leaves it held for the whole valid lifetime.
 *
 * This blocks: nx_dhcpv6_request_release() moves the state machine and the
 * client's thread sends the Release and waits for the Reply. That is the
 * point. It must be called while the interface can still transmit -- see the
 * call site in netstack.c, which is deliberately ahead of the NX_LINK_DISABLE
 * rather than after it.
 */
VOID ami_netstack_dhcpv6_release(AmiNetStack *ns)
{
    ULONG waited;
    ULONG sent_before;
    ULONG answered_before;

    if (ns == NULL || !ns->ns_Dhcpv6Created || !ns->ns_Dhcpv6Started)
        return;

    /*
     * Only from a ThreadX thread. ami_ns_destroy() has seven call sites, four
     * of them before the kernel exists and one of them the fallback branch in
     * netstack_shutdown() that could not take a bracket, and both the mutex
     * this takes and the sleep below are caller errors outside one. There is
     * nothing to release on those paths anyway -- the client cannot have been
     * started -- but the check is the guard rather than the reasoning.
     */
    if (tx_thread_identify() == TX_NULL)
        return;

    /*
     * The client's own state, NOT ns_Dhcpv6State.
     *
     * ns_Dhcpv6State is a mirror written by ami_ns6_dhcp_state_changed(),
     * which runs on the client's thread, so it lags every transition by
     * however long that thread takes to get the CPU. Reading it here to
     * decide whether there is a lease to give back can miss one that has just
     * been taken.
     */
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

    /*
     * WAIT FOR SOMETHING THAT HAPPENED, NOT FOR A STATE THAT IS TRANSIENTLY
     * FALSE.
     *
     * This loop used to break on `ns_Dhcpv6State != SENDING_RELEASE`, which is
     * true before the client's thread has picked the request up as well as
     * after it has finished -- so it never waited at all, the interface went
     * down underneath the client, and no Release reached the wire. It passed
     * anyway on an AMINETXDUO_LOG build, because the log calls on this path
     * are RawDoFmt() to a serial port and cost enough time for the client
     * thread to run. A feature that works only when it is instrumented is
     * worse than one that does not work, because every measurement of it says
     * it is fine: measured on a shipping build, 0 Release packets and 440 ms;
     * with logging on, 1 packet and 620 ms.
     *
     * So the exit needs the send counter to have moved -- the client has
     * built a Release and handed it to _nx_dhcpv6_send_request() -- AND then
     * either the server's Reply or the client leaving the state. Neither of
     * those can be true before the client thread has run.
     */
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

    /*
     * Bounded, because this is on the path of a machine being shut down and a
     * server that does not answer must not hold it up.  RFC 8415 18.2.7 lets
     * a client not wait for the Reply at all, so timing out here is a
     * conforming outcome and is reported rather than retried.
     */
    if (ns->ns_Dhcpv6.nx_dhcpv6_releases_sent == sent_before)
        AMI_WARN("netstack: the DHCPv6 Release never reached the wire");
    else if (ns->ns_Dhcpv6.nx_dhcpv6_release_responses != answered_before)
        AMI_INFO("netstack: DHCPv6 address released, the server answered");
    else
        AMI_INFO("netstack: DHCPv6 Release sent, the server did not answer");
}

/*
 * Quiesce the client while its selected interface is down.  Release above is
 * only meaningful for a stateful client with a lease; stop is required in all
 * modes so a Solicit or Information-Request does not keep retransmitting on
 * an offline link.  The client object is retained for a restart on link-up.
 */
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

/*
 * Repeat the exchange that was active before link-down.  Only enqueue work
 * here: callers are adopted application tasks and the request may block while
 * the DHCPv6 thread changes state.
 */
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

VOID ami_netstack_dhcpv6_destroy(AmiNetStack *ns)
{
    if (ns == NULL)
        return;

    /*
     * The RA callback first: it reaches ns_Dhcpv6Events, and it runs on the IP
     * thread, which is still going.
     */
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
        /*
         * Ask it to leave, and give it a second to, before taking it apart.
         * The QUIT above is answered immediately by a thread waiting on the
         * flag group, which is where it is nearly always found; a thread
         * caught inside ami_ns6_dhcp_begin() is holding the client's mutex,
         * and terminating it there leaves that mutex owned by a dead thread
         * for nx_dhcpv6_client_delete() to find. Only reachable from a
         * ThreadX thread, so a shutdown from outside one falls through to the
         * terminate and accepts that.
         */
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

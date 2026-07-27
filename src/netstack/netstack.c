/*
 * AmiNetXDuo -- the stack singleton.
 *
 * Startup order, and why it is this order:
 *
 *   1. config          -- AmigaDOS file I/O, so it must happen on a Process
 *                         and before this task becomes a ThreadX thread.
 *   2. SANA-II opens   -- OpenDevice()/DoIO(), same reason.
 *   3. sizing          -- AvailMem() decides the packet pool; the 4 MB floor
 *                         (docs/RESEARCH.md 9) means NetX Duo's own defaults
 *                         are not usable.
 *   4. ThreadX         -- tx_amiga_kernel_start() returns once the scheduler
 *                         is live, unlike tx_kernel_enter().
 *   5. adoption        -- everything below suspends the calling thread inside
 *                         NetX Duo, and nx_dns_create() is threads-only.
 *   6. NetX Duo        -- pool, NX_IP, ARP/TCP/UDP/ICMP, extra interfaces.
 *   7. addresses       -- DHCP, or AutoIP, or the static config; block until
 *                         the first interface has one or DHCP gives up.
 *   8. DNS             -- needs the resolver config and a running IP.
 *
 * SPDX-License-Identifier: MIT
 */

#include "netstack_internal.h"

#include "tx_amiga.h"

#include "aminetxduo/random.h"

#include <exec/memory.h>
#include <exec/ports.h>
#include <exec/semaphores.h>
#include <proto/exec.h>

/*
 * tx_kernel_enter() calls this before the scheduler starts. AmiNetXDuo builds
 * everything from netstack_startup() instead -- on an adopted task, after the
 * kernel is live -- because the SANA-II devices have to be opened by a Process
 * and nx_dns_create() is threads-only. Weak, so a standalone test executable
 * (tests/ram_driver) can still supply its own.
 */
__attribute__((weak)) VOID tx_application_define(VOID *first_unused_memory)
{
    (VOID)first_unused_memory;
}

/* -------------------------------------------------------------- singleton */

static struct SignalSemaphore   ami_ns_lock;
static volatile BOOL            ami_ns_lock_ready;
static AmiNetStack             *ami_ns;
static BOOL                     ami_ns_system_initialised;

static VOID ami_ns_lock_init(VOID)
{
    Forbid();
    if (!ami_ns_lock_ready)
    {
        InitSemaphore(&ami_ns_lock);
        ami_ns_lock_ready = TRUE;
    }
    Permit();
}

AmiNetStack *ami_netstack_raw(VOID)
{
    return ami_ns;
}

/* ------------------------------------------------- the "is it up?" barrier */

/*
 * The AMITCP public message port (docs/RESEARCH.md 3.3 and 6.6).
 *
 * `WaitForPort AMITCP` in S:User-Startup is the conventional Amiga way of
 * saying "wait until the network exists", and every stack since AmiTCP has
 * created it. It is also the only cross-program way for a Shell command to
 * find out that a stack is running: the singleton is private to whichever
 * binary owns it, so the tools cannot see it, but they can see this.
 *
 * PA_IGNORE, with no signal task, on purpose. Nothing is meant to send to
 * this port -- it is a flag, not a service -- and the alternative is worse:
 * mp_SigTask would have to be the task that happened to bring the stack up,
 * which is usually a Shell command that exits seconds later, leaving anything
 * that did PutMsg() signalling a dead task.
 */
static char             ami_ns_port_name[] = "AMITCP";
static struct MsgPort  *ami_ns_port;

static VOID ami_ns_port_create(VOID)
{
    struct MsgPort *port;

    if (ami_ns_port != NULL)
        return;

    Forbid();
    port = FindPort((CONST_STRPTR)ami_ns_port_name);
    Permit();

    if (port != NULL)
    {
        /* Another TCP/IP stack is already on this machine. */
        AMI_WARN("netstack: an AMITCP port already exists; not adding ours");
        return;
    }

    port = (struct MsgPort *)ami_alloc((ULONG)sizeof(struct MsgPort));
    if (port == NULL)
        return;

    port->mp_Node.ln_Type = NT_MSGPORT;
    port->mp_Node.ln_Pri  = 0;
    port->mp_Node.ln_Name = ami_ns_port_name;
    port->mp_Flags        = PA_IGNORE;
    port->mp_SigBit       = 0;
    port->mp_SigTask      = NULL;

    /* NewList() lives in amiga.lib, which a shared library cannot reach. */
    port->mp_MsgList.lh_Head     = (struct Node *)&port->mp_MsgList.lh_Tail;
    port->mp_MsgList.lh_Tail     = NULL;
    port->mp_MsgList.lh_TailPred = (struct Node *)&port->mp_MsgList.lh_Head;
    port->mp_MsgList.lh_Type     = NT_MESSAGE;

    AddPort(port);
    ami_ns_port = port;

    AMI_INFO("netstack: AMITCP port added");
}

static VOID ami_ns_port_delete(VOID)
{
    if (ami_ns_port == NULL)
        return;

    RemPort(ami_ns_port);
    ami_free(ami_ns_port);
    ami_ns_port = NULL;
}

/* ----------------------------------------------------------- adoption glue */

LONG ami_netstack_enter(AmiNetCaller *caller)
{
    UINT status;

    caller->nc_Adopted = FALSE;

    if (tx_amiga_kernel_running() != TX_TRUE)
        return AMI_NET_ERR_STATE;

    /* Already a ThreadX thread (an adopted task deeper in the call chain, or
       a thread ThreadX created)? Then nothing to do. */
    if (tx_thread_identify() != TX_NULL)
        return AMI_NET_OK;

    status = tx_amiga_adopt_thread(&caller->nc_Thread, (CHAR *)"aminetxduo caller",
                                   AMI_CALLER_PRIORITY);
    if (status != TX_SUCCESS)
    {
        AMI_ERROR("netstack: cannot adopt calling task (%ld)", (long)status);
        return AMI_NET_ERR_KERNEL;
    }

    caller->nc_Adopted = TRUE;

    return AMI_NET_OK;
}

VOID ami_netstack_leave(AmiNetCaller *caller)
{
    if (caller->nc_Adopted)
    {
        (VOID)tx_amiga_orphan_thread(&caller->nc_Thread);
        caller->nc_Adopted = FALSE;
    }
}

/*
 * The same bracket, with the TX_THREAD kept between calls.
 *
 * The registration is what is expensive and what is repeatable: the same task
 * gets the same thread every time. The baton is still taken and given back per
 * call, so nothing about the concurrency model changes -- see the contract in
 * <aminetxduo/netstack.h> and tx_amiga_adopt_resume() in the port.
 *
 * Every failure here falls back to the per-call path, which is always correct.
 * That matters more than the speed: a stale or foreign cache must cost a
 * bracket, never a wrong answer.
 */
LONG ami_netstack_enter_cached(AmiNetCaller *caller)
{
    struct Task *me;
    UINT         status;

    caller->nc_Adopted = FALSE;

    if (tx_amiga_kernel_running() != TX_TRUE)
        return AMI_NET_ERR_STATE;

    if (tx_thread_identify() != TX_NULL)
        return AMI_NET_OK;                  /* nested */

    me = FindTask(NULL);

    if (caller->nc_Live && caller->nc_Task == me)
    {
        if (tx_amiga_adopt_resume(&caller->nc_Thread) == TX_SUCCESS)
        {
            caller->nc_Adopted = TRUE;
            return AMI_NET_OK;
        }

        /*
         * The cached thread is no longer usable -- torn down under us, or in
         * a state resume will not take. Drop it and adopt afresh. Orphan
         * rather than discard: this is the owning task, so the Exec signal
         * can be recovered, and tx_amiga_orphan_thread() handles a TX_THREAD
         * that has already been deleted.
         */
        if (tx_amiga_orphan_thread(&caller->nc_Thread) != TX_SUCCESS)
            (VOID)tx_amiga_discard_thread(&caller->nc_Thread);
        caller->nc_Live = FALSE;
        caller->nc_Task = NULL;
    }

    /*
     * A cache belonging to ANOTHER task. There is nowhere to put a second
     * adoption -- the TX_THREAD storage is this one -- and overwriting it
     * would deregister a thread that its owner is still using, so this fails
     * rather than improvising.
     *
     * It means a SocketBase used from a task other than the one that opened
     * it now reports ENETDOWN instead of working by accident. That is already
     * outside the contract: the descriptor table, the event signal and the
     * errno pointer all belong to the opener, and src/bsdsocket/tcp_handler.c
     * states the rule in its own header ("a SocketBase belongs to one task").
     */
    if (caller->nc_Live)
    {
        AMI_ERROR("netstack: bracket used from a second task");
        return AMI_NET_ERR_STATE;
    }

    status = tx_amiga_adopt_thread(&caller->nc_Thread,
                                   (CHAR *)"aminetxduo caller",
                                   AMI_CALLER_PRIORITY);
    if (status != TX_SUCCESS)
    {
        AMI_ERROR("netstack: cannot adopt calling task (%ld)", (long)status);
        return AMI_NET_ERR_KERNEL;
    }

    caller->nc_Live    = TRUE;
    caller->nc_Task    = me;
    caller->nc_Adopted = TRUE;

    return AMI_NET_OK;
}

VOID ami_netstack_leave_cached(AmiNetCaller *caller)
{
    if (!caller->nc_Adopted)
        return;

    caller->nc_Adopted = FALSE;

    if (caller->nc_Live && caller->nc_Task == FindTask(NULL))
    {
        if (tx_amiga_adopt_suspend(&caller->nc_Thread) == TX_SUCCESS)
            return;

        /* Suspending failed, so the thread is not in a state we understand.
           Orphaning it is the safe exit: it gives the baton back and takes
           the registration with it. */
        caller->nc_Live = FALSE;
        caller->nc_Task = NULL;
    }

    (VOID)tx_amiga_orphan_thread(&caller->nc_Thread);
}

VOID ami_netstack_release(AmiNetCaller *caller)
{
    if (caller == NULL || !caller->nc_Live)
        return;

    /*
     * Inside a bracket the thread is the baton holder, so it has to be given
     * back the ordinary way first. This should not happen -- release is a
     * teardown call -- but a half-released base is worse than a redundant
     * branch.
     */
    if (caller->nc_Adopted)
    {
        caller->nc_Adopted = FALSE;
        (VOID)tx_amiga_orphan_thread(&caller->nc_Thread);
        caller->nc_Live = FALSE;
        caller->nc_Task = NULL;
        return;
    }

    if (caller->nc_Task == FindTask(NULL))
    {
        /* The owner: resume so the signal can be freed by the task that
           allocated it, then orphan properly. */
        if (tx_amiga_adopt_resume(&caller->nc_Thread) == TX_SUCCESS)
            (VOID)tx_amiga_orphan_thread(&caller->nc_Thread);
        else
            (VOID)tx_amiga_discard_thread(&caller->nc_Thread);
    }
    else
    {
        /* Somebody else is tearing this down. The registration must go; the
           signal bit belongs to a task we may not touch. */
        (VOID)tx_amiga_discard_thread(&caller->nc_Thread);
    }

    caller->nc_Live = FALSE;
    caller->nc_Task = NULL;
}

/* ------------------------------------------------------------ pool sizing */

/*
 * NetX Duo lays a pool out as N * (payload rounded up to NX_PACKET_ALIGNMENT
 * + sizeof(NX_PACKET)), and rejects a block that is not big enough for one
 * packet. Rather than reproduce the arithmetic exactly, budget generously per
 * packet and let nx_packet_pool_create() report what it actually made.
 */
static ULONG ami_ns_packet_stride(VOID)
{
    ULONG stride;

    stride = (ULONG)AMI_POOL_PAYLOAD + (ULONG)sizeof(NX_PACKET) +
             (ULONG)NX_PACKET_ALIGNMENT;

    return (stride + 3UL) & ~3UL;
}

static ULONG ami_ns_pool_packets(VOID)
{
    ULONG avail;
    ULONG packets;

    avail = AvailMem(MEMF_PUBLIC);

    packets = (avail / AMI_POOL_MEM_DIVISOR) / ami_ns_packet_stride();

    if (packets < (ULONG)AMI_POOL_MIN_PACKETS)
        packets = (ULONG)AMI_POOL_MIN_PACKETS;
    if (packets > (ULONG)AMI_POOL_MAX_PACKETS)
        packets = (ULONG)AMI_POOL_MAX_PACKETS;

    AMI_INFO("netstack: %lu bytes free, pool = %lu x %lu",
             (unsigned long)avail, (unsigned long)packets,
             (unsigned long)AMI_POOL_PAYLOAD);

    return packets;
}

/* ---------------------------------------------------------------- teardown */

static VOID ami_ns_destroy(AmiNetStack *ns)
{
    UWORD i;

    if (ns == NULL)
        return;

#ifdef AMINETXDUO_BPF
    /* Before anything is deleted: the taps run on the SANA-II reader threads
       and the IP thread, both of which are still alive at this point. */
    ami_netstack_capture_stop(ns);
#endif

#ifdef AMINETXDUO_MDNS
    /*
     * Before the resolver and long before nx_ip_delete(): stopping the
     * responder sends the RFC 6762 10.1 goodbye packet, and a goodbye needs a
     * working IP instance to leave on.
     */
    ami_netstack_mdns_stop(ns);
#endif

    ami_netstack_dns_stop(ns);

    if (ns->ns_AutoIpCreated)
    {
        (VOID)nx_auto_ip_stop(&ns->ns_AutoIp);
        (VOID)nx_auto_ip_delete(&ns->ns_AutoIp);
        ns->ns_AutoIpCreated = FALSE;
        ns->ns_AutoIpRunning = FALSE;
    }

    if (ns->ns_DhcpCreated)
    {
        if (ns->ns_DhcpStarted)
        {
            (VOID)nx_dhcp_stop(&ns->ns_Dhcp);
            ns->ns_DhcpStarted = FALSE;
        }
        (VOID)nx_dhcp_delete(&ns->ns_Dhcp);
        ns->ns_DhcpCreated = FALSE;
    }

    if (ns->ns_IpCreated)
    {
        (VOID)nx_ip_delete(&ns->ns_Ip);
        ns->ns_IpCreated = FALSE;
    }

    /* The NX_IP teardown above has already taken the interfaces offline and
       stopped the reader threads; closing the devices is all that is left. */
    for (i = 0; i < AMI_CFG_MAX_INTERFACES; i++)
    {
        if (ns->ns_Iface[i] != NULL)
        {
            ami_sana2_close(ns->ns_Iface[i]);
            ns->ns_Iface[i] = NULL;
        }
    }
    ns->ns_IfaceCount = 0;

    if (ns->ns_PoolMemory != NULL)
    {
        (VOID)nx_packet_pool_delete(&ns->ns_Pool);
        ami_free(ns->ns_PoolMemory);
        ns->ns_PoolMemory = NULL;
    }

    if (ns->ns_AutoIpStack != NULL)
    {
        ami_free(ns->ns_AutoIpStack);
        ns->ns_AutoIpStack = NULL;
    }
    if (ns->ns_ArpCache != NULL)
    {
        ami_free(ns->ns_ArpCache);
        ns->ns_ArpCache = NULL;
    }
    if (ns->ns_IpStack != NULL)
    {
        ami_free(ns->ns_IpStack);
        ns->ns_IpStack = NULL;
    }

    ami_free(ns);
}

/* -------------------------------------------------------- device open pass */

static LONG ami_ns_open_devices(AmiNetStack *ns)
{
    UWORD i;
    UWORD opened = 0;
    LONG  err    = AMI_NET_ERR_NODEV;

    for (i = 0; i < ns->ns_Config.interface_count; i++)
    {
        const AmiIfConfig *cfg = &ns->ns_Config.interfaces[i];
        LONG               status;

        if (opened >= (UWORD)NX_MAX_PHYSICAL_INTERFACES)
        {
            AMI_WARN("netstack: only %ld interfaces supported, '%s' ignored",
                     (long)NX_MAX_PHYSICAL_INTERFACES, cfg->name);
            break;
        }

        ns->ns_Iface[opened] = ami_sana2_open(cfg, &status);
        if (ns->ns_Iface[opened] == NULL)
        {
            /*
             * The serial log is a developer's view, but it is also what a
             * user is asked to send in, so it says what to do as well as what
             * happened. The console version of this, with a probe of the
             * device behind it, is in src/tools/tool_diag.c.
             */
            AMI_ERROR("netstack: interface '%s' would not open: %s unit %lu "
                      "did not answer -- is the driver in DEVS:Networks/ and "
                      "is the card fitted on that unit?",
                      cfg->name, cfg->device, (unsigned long)cfg->unit);
            err = status;
            continue;
        }

        opened++;
    }

    ns->ns_IfaceCount = opened;

    return (opened > 0) ? AMI_NET_OK : err;
}

/* ---------------------------------------------------------- NetX Duo build */

static LONG ami_ns_create_ip(AmiNetStack *ns)
{
    const AmiIfConfig *cfg0 = &ns->ns_Config.interfaces[0];
    ULONG              addr0;
    ULONG              mask0;
    ULONG              actual;
    UINT               status;
    UWORD              i;

    if (!ami_ns_system_initialised)
    {
        nx_system_initialize();
        ami_ns_system_initialised = TRUE;
    }

    status = nx_packet_pool_create(&ns->ns_Pool, (CHAR *)"AmiNetXDuo",
                                   (ULONG)AMI_POOL_PAYLOAD,
                                   ns->ns_PoolMemory, ns->ns_PoolBytes);
    if (status != NX_SUCCESS)
    {
        AMI_ERROR("netstack: packet pool create failed (%ld)", (long)status);
        return AMI_NET_ERR_NOMEM;
    }

    /* A DHCP or link-local interface starts with no address at all. */
    addr0 = (cfg0->iptype == AMI_IPTYPE_STATIC) ? cfg0->address : 0UL;
    mask0 = (cfg0->iptype == AMI_IPTYPE_STATIC && cfg0->netmask != 0UL)
                ? cfg0->netmask : 0UL;

    /*
     * Bind interface 0 before nx_ip_create(): the IP thread calls the driver
     * for NX_LINK_INITIALIZE the moment it starts, and the driver finds its
     * AmiSana2If through the binding table (src/sana2/sana2_driver.c).
     */
    if (ami_sana2_attach(ns->ns_Iface[0], &ns->ns_Ip, 0) != AMI_NET_OK)
    {
        AMI_ERROR("netstack: cannot bind interface 0");
        return AMI_NET_ERR_STATE;
    }

    AMI_INFO("netstack: nx_ip_create");
    status = nx_ip_create(&ns->ns_Ip, (CHAR *)"AmiNetXDuo", addr0, mask0,
                          &ns->ns_Pool, ami_sana2_driver_entry,
                          ns->ns_IpStack, (ULONG)AMI_IP_STACK_SIZE,
                          AMI_IP_THREAD_PRIORITY);
    if (status != NX_SUCCESS)
    {
        AMI_ERROR("netstack: nx_ip_create failed (%ld)", (long)status);
        return AMI_NET_ERR_KERNEL;
    }
    ns->ns_IpCreated = TRUE;

    AMI_INFO("netstack: waiting for NX_IP initialisation");
    status = nx_ip_status_check(&ns->ns_Ip, NX_IP_INITIALIZE_DONE, &actual,
                                AMI_LINK_TIMEOUT_TICKS);
    if (status != NX_SUCCESS)
    {
        AMI_ERROR("netstack: IP instance did not initialise (%ld)", (long)status);
        return AMI_NET_ERR_NODEV;
    }

    /*
     * The IP identification field starts somewhere unpredictable.
     *
     * nx_ip_create() zeroes nx_ip_packet_id and nx_ip_header_add.c increments
     * it once per transmitted datagram, so without this the field is a global,
     * monotonic, boot-zeroed counter -- a fingerprint on its own, and a packet
     * counter for the whole machine readable from any one flow.
     *
     * One DRBG draw, once, moves the starting point.  It is deliberately NOT
     * NX_ENABLE_IP_ID_RANDOMIZATION, which redraws per packet and costs 5% of
     * loopback throughput on this machine (nx_user.h, docs/RESEARCH.md 29.4);
     * this is the half that is free.  It does not defeat RFC 6274's idle scan,
     * which reads the DELTA between two observations rather than the value,
     * and the note in nx_user.h says so rather than letting this look like a
     * complete answer.
     *
     * The field is 16 bits wide in the header and NetX Duo shifts it up by 16,
     * so only the low half is meaningful.
     */
    ns->ns_Ip.nx_ip_packet_id = (ami_random_ulong() & 0xFFFFUL);

    status = nx_arp_enable(&ns->ns_Ip, ns->ns_ArpCache, (ULONG)AMI_ARP_CACHE_SIZE);
    if (status != NX_SUCCESS)
        AMI_WARN("netstack: nx_arp_enable failed (%ld)", (long)status);

    status = nx_tcp_enable(&ns->ns_Ip);
    if (status != NX_SUCCESS)
        AMI_WARN("netstack: nx_tcp_enable failed (%ld)", (long)status);

    status = nx_udp_enable(&ns->ns_Ip);
    if (status != NX_SUCCESS)
        AMI_WARN("netstack: nx_udp_enable failed (%ld)", (long)status);

#ifdef AMINETXDUO_IPV6
    /*
     * The dual stack. nxd_icmp_enable() covers ICMPv4 as well, so it replaces
     * the nx_icmp_enable() below rather than adding to it -- calling both
     * would return NX_ALREADY_ENABLED from the second, harmlessly, but the
     * intent would be unclear.
     *
     * This must happen before the interfaces are attached below: address
     * configuration in ami_netstack_ipv6_configure() joins solicited-node
     * multicast groups, and those joins reach a driver that has to be there.
     */
    if (ami_netstack_ipv6_enable(ns) != AMI_NET_OK)
        AMI_WARN("netstack: continuing with IPv4 only");
#else
    status = nx_icmp_enable(&ns->ns_Ip);
    if (status != NX_SUCCESS)
        AMI_WARN("netstack: nx_icmp_enable failed (%ld)", (long)status);
#endif

    /* Secondary interfaces. nx_ip_interface_attach() drives the driver from
       this context, so the binding must exist first here too. */
    for (i = 1; i < ns->ns_IfaceCount; i++)
    {
        const AmiIfConfig *cfg = &ns->ns_Config.interfaces[i];
        ULONG              addr;
        ULONG              mask;

        if (ami_sana2_attach(ns->ns_Iface[i], &ns->ns_Ip, i) != AMI_NET_OK)
        {
            AMI_WARN("netstack: cannot bind interface %ld", (long)i);
            continue;
        }

        addr = (cfg->iptype == AMI_IPTYPE_STATIC) ? cfg->address : 0UL;
        mask = (cfg->iptype == AMI_IPTYPE_STATIC) ? cfg->netmask : 0UL;

        status = nx_ip_interface_attach(&ns->ns_Ip, (CHAR *)cfg->name,
                                        addr, mask, ami_sana2_driver_entry);
        if (status != NX_SUCCESS)
            AMI_WARN("netstack: interface '%s' attach failed (%ld)",
                     cfg->name, (long)status);
    }

    if (ns->ns_Config.default_gateway != 0UL)
    {
        status = nx_ip_gateway_address_set(&ns->ns_Ip,
                                           ns->ns_Config.default_gateway);
        if (status != NX_SUCCESS)
            AMI_WARN("netstack: gateway set failed (%ld)", (long)status);
    }

    return AMI_NET_OK;
}

/* ------------------------------------------------------------- addressing */

static BOOL ami_ns_wants(const AmiNetStack *ns, AmiIpType type)
{
    UWORD i;

    for (i = 0; i < ns->ns_IfaceCount; i++)
    {
        if (ns->ns_Config.interfaces[i].iptype == type)
            return TRUE;
    }

    return FALSE;
}

static VOID ami_ns_start_autoip(AmiNetStack *ns)
{
    UINT  status;
    UWORD i;

    /*
     * Already built. nx_auto_ip_stop() only SUSPENDS the module's thread, so
     * coming back is a start rather than a create -- and nx_auto_ip_start()
     * with a starting address of zero is what makes it draw a fresh candidate
     * instead of re-probing whichever one it had before.
     *
     * Guarded on ns_AutoIpRunning because a start while it is already running
     * resets its conflict count and throws away the address it is holding.
     */
    if (ns->ns_AutoIpCreated)
    {
        if (!ns->ns_AutoIpRunning)
        {
            status = nx_auto_ip_start(&ns->ns_AutoIp, 0UL);
            if (status == NX_SUCCESS)
            {
                ns->ns_AutoIpRunning = TRUE;
                AMI_INFO("netstack: RFC 3927 link-local configuration restarted");
            }
            else
            {
                AMI_WARN("netstack: nx_auto_ip_start failed (%ld)", (long)status);
            }
        }
        return;
    }

    ns->ns_AutoIpStack = ami_alloc_flags((ULONG)AMI_AUTOIP_STACK_SIZE, MEMF_PUBLIC);
    if (ns->ns_AutoIpStack == NULL)
    {
        AMI_WARN("netstack: no memory for the AutoIP thread");
        return;
    }

    status = nx_auto_ip_create(&ns->ns_AutoIp, (CHAR *)"AmiNetXDuo AutoIP",
                               &ns->ns_Ip, ns->ns_AutoIpStack,
                               (ULONG)AMI_AUTOIP_STACK_SIZE, AMI_AUTOIP_PRIORITY);
    if (status != NX_SUCCESS)
    {
        AMI_WARN("netstack: nx_auto_ip_create failed (%ld)", (long)status);
        ami_free(ns->ns_AutoIpStack);
        ns->ns_AutoIpStack = NULL;
        return;
    }
    ns->ns_AutoIpCreated = TRUE;

    /* First interface that asked for a link-local address, else interface 0. */
    for (i = 0; i < ns->ns_IfaceCount; i++)
    {
        if (ns->ns_Config.interfaces[i].iptype == AMI_IPTYPE_LINKLOCAL)
        {
            (VOID)nx_auto_ip_set_interface(&ns->ns_AutoIp, (UINT)i);
            break;
        }
    }

    status = nx_auto_ip_start(&ns->ns_AutoIp, 0UL);
    if (status != NX_SUCCESS)
    {
        AMI_WARN("netstack: nx_auto_ip_start failed (%ld)", (long)status);
    }
    else
    {
        ns->ns_AutoIpRunning = TRUE;
        AMI_INFO("netstack: RFC 3927 link-local configuration started");
    }
}

/* ------------------------------------------------- what actually happened --
 *
 * NetX Duo's DHCP client and its AutoIP module both change the interface
 * address from their own threads and announce NOTHING unless somebody
 * registers for it. Before these two existed the machine could lose its lease
 * -- address removed, gateway cleared, every socket dead -- and the only
 * record anywhere was that `netstat` started answering differently.
 *
 * Both run on a NetX Duo thread. AMI_INFO()/AMI_WARN() are RawPutChar() and
 * RawDoFmt(), which is Exec-only and legal from any Task, and nothing here
 * blocks.
 */

static BOOL ami_ns_is_linklocal(ULONG addr)
{
    return (addr >= IP_ADDRESS(169, 254, 0, 0) &&
            addr <= IP_ADDRESS(169, 254, 255, 255)) ? TRUE : FALSE;
}

static VOID ami_ns_log_address(const char *what, UWORD index, ULONG addr)
{
    AMI_INFO("netstack: interface %ld %s %lu.%lu.%lu.%lu",
             (long)index, what,
             (unsigned long)((addr >> 24) & 0xFFUL),
             (unsigned long)((addr >> 16) & 0xFFUL),
             (unsigned long)((addr >>  8) & 0xFFUL),
             (unsigned long)(addr & 0xFFUL));
}

/*
 * Called by NetX Duo whenever any interface's address or mask changes -- from
 * the DHCP thread on a lease, from the AutoIP thread on a link-local claim,
 * and from ami_ns_configure_addresses() for a static one.
 */
static VOID ami_ns_address_changed(NX_IP *ip_ptr, VOID *info)
{
    AmiNetStack *ns = ami_ns;
    UWORD        i;

    (VOID)info;

    if (ns == NULL || ip_ptr != &ns->ns_Ip)
        return;

    for (i = 0; i < ns->ns_IfaceCount; i++)
    {
        ULONG addr = 0UL;
        ULONG mask = 0UL;

        if (nx_ip_interface_address_get(&ns->ns_Ip, (UINT)i, &addr, &mask)
                != NX_SUCCESS)
            continue;

        if (addr == ns->ns_LastAddress[i])
            continue;

        ns->ns_LastAddress[i] = addr;

        if (addr == 0UL)
            AMI_WARN("netstack: interface %ld no longer has an address",
                     (long)i);
        else if (ami_ns_is_linklocal(addr))
            ami_ns_log_address("has the link-local address", i, addr);
        else
            ami_ns_log_address("address is now", i, addr);

        /*
         * RFC 3927 1.9: a routable address supersedes a link-local one. The
         * AutoIP thread does not watch for this itself -- it sits in an
         * indefinite wait for a conflict -- so it is stopped here, which
         * leaves it able to be restarted if the lease is later lost.
         *
         * Never from the AutoIP thread itself: nx_auto_ip_stop() is
         * tx_thread_suspend(), and calling it on the running thread would
         * suspend it in the middle of its own announcement.
         */
        if (ns->ns_AutoIpRunning && addr != 0UL && !ami_ns_is_linklocal(addr) &&
            tx_thread_identify() != &ns->ns_AutoIp.nx_auto_ip_thread)
        {
            (VOID)nx_auto_ip_stop(&ns->ns_AutoIp);
            ns->ns_AutoIpRunning = FALSE;
            AMI_INFO("netstack: link-local configuration stopped -- interface "
                     "%ld has a routable address now", (long)i);
        }
    }
}

/*
 * Called by NetX Duo's DHCP client on every state transition, per interface.
 *
 * The transition that matters is the one INTO INIT from a state that held an
 * address: that is a NAK, or a lease that ran out with nothing answering, and
 * _nx_dhcp_interface_reinitialize() has just taken the address and the gateway
 * off the interface. RFC 3927 1.7 is the answer to it, and it is the same
 * answer this file already gives when DHCP never answers at all.
 */
static VOID ami_ns_dhcp_state_changed(NX_DHCP *dhcp_ptr, UINT iface_index,
                                      UCHAR new_state)
{
    AmiNetStack *ns = ami_ns;
    UBYTE        previous;

    if (ns == NULL || dhcp_ptr != &ns->ns_Dhcp ||
        iface_index >= (UINT)AMI_CFG_MAX_INTERFACES)
        return;

    previous = ns->ns_DhcpState[iface_index];
    ns->ns_DhcpState[iface_index] = (UBYTE)new_state;

    switch (new_state)
    {
    case NX_DHCP_STATE_BOUND:
        AMI_INFO("netstack: interface %ld has a DHCP lease",
                 (long)iface_index);
        break;

    case NX_DHCP_STATE_RENEWING:
        AMI_INFO("netstack: interface %ld is renewing its DHCP lease",
                 (long)iface_index);
        break;

    case NX_DHCP_STATE_REBINDING:
        AMI_WARN("netstack: interface %ld -- the DHCP server that issued the "
                 "lease is not answering; asking any server on this network",
                 (long)iface_index);
        break;

    case NX_DHCP_STATE_INIT:
        /*
         * Only from a state that HELD a lease. A NAK in REQUESTING is an
         * ordinary part of acquiring one -- the server saying "not that
         * address, ask again" -- and reporting a first boot as a lost lease
         * would be a false alarm on the one message that has to be believed.
         */
        if (previous == (UBYTE)NX_DHCP_STATE_BOUND ||
            previous == (UBYTE)NX_DHCP_STATE_RENEWING ||
            previous == (UBYTE)NX_DHCP_STATE_REBINDING)
        {
            AMI_WARN("netstack: interface %ld has LOST its DHCP lease -- the "
                     "address and the gateway have been taken off it, and "
                     "every open connection through it is dead",
                     (long)iface_index);

            /* RFC 3927 1.7: keep the machine reachable on the local wire
               while the DHCP client tries again. */
            ami_ns_start_autoip(ns);
        }
        break;

    default:
        break;
    }
}

/*
 * Wait for ANY configured interface to have an address.
 *
 * nx_ip_status_check() is nx_ip_interface_status_check() on INTERFACE 0 -- it
 * says so in one line at the bottom of nx_ip_status_check.c -- so the wait
 * this replaced could not see an address arrive on interface 1. A machine
 * whose Ethernet is static and whose PPP link is the DHCP one waited out the
 * full timeout and then reported that nothing had an address.
 */
static BOOL ami_ns_wait_for_address(AmiNetStack *ns, ULONG timeout_ticks)
{
    ULONG waited = 0UL;

    for (;;)
    {
        UWORD i;

        for (i = 0; i < ns->ns_IfaceCount; i++)
        {
            ULONG addr = 0UL;
            ULONG mask = 0UL;

            if (nx_ip_interface_address_get(&ns->ns_Ip, (UINT)i, &addr, &mask)
                    == NX_SUCCESS && addr != 0UL)
                return TRUE;
        }

        if (waited >= timeout_ticks)
            return FALSE;

        tx_thread_sleep((ULONG)AMI_ADDRESS_POLL_TICKS);
        waited += (ULONG)AMI_ADDRESS_POLL_TICKS;
    }
}

static LONG ami_ns_configure_addresses(AmiNetStack *ns)
{
    UINT  status;
    UWORD i;
    BOOL  resolved = FALSE;

    /* Before anything can change an address, so the first one is announced
       too. */
    (VOID)nx_ip_address_change_notify(&ns->ns_Ip, ami_ns_address_changed,
                                      NX_NULL);

    /* Static interfaces are already addressed by nx_ip_create()/attach; make
       sure a static interface 0 really has what the config asked for. */
    for (i = 0; i < ns->ns_IfaceCount; i++)
    {
        const AmiIfConfig *cfg = &ns->ns_Config.interfaces[i];

        if (cfg->iptype != AMI_IPTYPE_STATIC || cfg->address == 0UL)
            continue;

        (VOID)nx_ip_interface_address_set(&ns->ns_Ip, (UINT)i, cfg->address,
                                          (cfg->netmask != 0UL) ? cfg->netmask
                                                                : 0xFFFFFF00UL);
        resolved = TRUE;
    }

    if (ami_ns_wants(ns, AMI_IPTYPE_DHCP))
    {
        /*
         * The name here is DHCP option 12, the host name the client announces
         * -- it is what the router's client list shows and what many of them
         * put in their local DNS. It was the string "amiga" for everybody,
         * which made two AmiNetXDuo machines on one network indistinguishable
         * and quietly discarded the HOSTNAME the user configured.
         *
         * NetX Duo keeps the POINTER rather than a copy, so this has to be
         * storage that outlives the NX_DHCP: ns_Config is inside the same
         * AmiNetStack and is not written again after ami_config_load().
         */
        status = nx_dhcp_create(&ns->ns_Dhcp, &ns->ns_Ip,
                                (ns->ns_Config.hostname[0] != '\0')
                                    ? (CHAR *)ns->ns_Config.hostname
                                    : (CHAR *)"amiga");
        if (status != NX_SUCCESS)
        {
            AMI_ERROR("netstack: nx_dhcp_create failed (%ld)", (long)status);
        }
        else
        {
            ns->ns_DhcpCreated = TRUE;

            /* Registered before the client starts, so the first BOUND is
               reported as well as everything after it. */
            (VOID)nx_dhcp_interface_state_change_notify(
                      &ns->ns_Dhcp, ami_ns_dhcp_state_changed);

            for (i = 0; i < ns->ns_IfaceCount; i++)
            {
                if (ns->ns_Config.interfaces[i].iptype != AMI_IPTYPE_DHCP)
                    continue;
                if (i == 0)
                    continue;   /* interface 0 is enabled by nx_dhcp_create() */

                (VOID)nx_dhcp_interface_enable(&ns->ns_Dhcp, (UINT)i);
            }

            status = nx_dhcp_start(&ns->ns_Dhcp);
            if (status != NX_SUCCESS)
            {
                AMI_ERROR("netstack: nx_dhcp_start failed (%ld)", (long)status);
            }
            else
            {
                ns->ns_DhcpStarted = TRUE;
                AMI_INFO("netstack: DHCP started, waiting up to %lu ticks",
                         (unsigned long)AMI_DHCP_TIMEOUT_TICKS);
            }
        }
    }

    if (ami_ns_wants(ns, AMI_IPTYPE_LINKLOCAL))
        ami_ns_start_autoip(ns);

    if (!resolved)
    {
        /* Block until some interface has an address, or DHCP gives up. */
        resolved = ami_ns_wait_for_address(ns, AMI_DHCP_TIMEOUT_TICKS);

        if (!resolved && ns->ns_DhcpStarted)
        {
            AMI_WARN("netstack: no DHCP server answered in %lu seconds",
                     (unsigned long)(AMI_DHCP_TIMEOUT_TICKS /
                                     (ULONG)NX_IP_PERIODIC_RATE));

            /* RFC 3927: fall back to a link-local address. */
            ami_ns_start_autoip(ns);

            resolved = ami_ns_wait_for_address(ns, AMI_AUTOIP_TIMEOUT_TICKS);

            if (!resolved)
                AMI_WARN("netstack: link-local configuration did not settle "
                         "either -- is the cable in?");
        }
    }

#ifdef AMINETXDUO_IPV6
    /*
     * IPv6 addressing runs after the IPv4 block and is never waited for.
     *
     * The link-local address is up before this returns (DAD is waited on
     * inside), which is enough for every IPv6 socket call to have a source
     * address. A global address from a router advertisement may take a second
     * or two more to arrive, and blocking startup on a router that may not
     * exist would make every IPv6 build slower to boot than the floor one for
     * no benefit. netstack_ipv6_address_get() reports what has arrived.
     *
     * Consequently `resolved` -- which is about IPv4 and gates the
     * AMI_NET_ERR_CONFIG return -- is deliberately not touched here. A machine
     * with IPv6 and no IPv4 address still reports "no interface has an
     * address", because that is what every IPv4 caller will find.
     */
    ami_netstack_ipv6_configure(ns);
#endif

    {
        ULONG addr = 0UL;
        ULONG mask = 0UL;

        (VOID)nx_ip_address_get(&ns->ns_Ip, &addr, &mask);

        AMI_INFO("netstack: address %lu.%lu.%lu.%lu mask %lu.%lu.%lu.%lu",
                 (unsigned long)((addr >> 24) & 0xFFUL),
                 (unsigned long)((addr >> 16) & 0xFFUL),
                 (unsigned long)((addr >>  8) & 0xFFUL),
                 (unsigned long)(addr & 0xFFUL),
                 (unsigned long)((mask >> 24) & 0xFFUL),
                 (unsigned long)((mask >> 16) & 0xFFUL),
                 (unsigned long)((mask >>  8) & 0xFFUL),
                 (unsigned long)(mask & 0xFFUL));
    }

    return resolved ? AMI_NET_OK : AMI_NET_ERR_CONFIG;
}

/* ------------------------------------------------------------------ bring-up */

static LONG ami_ns_bring_up(VOID)
{
    AmiNetCaller  caller;
    AmiNetStack  *ns;
    LONG          status;
    UINT          txstatus;

    ns = (AmiNetStack *)ami_alloc((ULONG)sizeof(AmiNetStack));
    if (ns == NULL)
    {
        AMI_ERROR("netstack: no memory for the stack (%lu bytes)",
                  (unsigned long)sizeof(AmiNetStack));
        return AMI_NET_ERR_NOMEM;
    }

    /* ---- 1. configuration (AmigaDOS; must precede adoption) ------------- */

    if (ami_config_load(&ns->ns_Config) != AMI_CFG_OK)
    {
        ami_free(ns);
        return AMI_NET_ERR_CONFIG;
    }

    if (ns->ns_Config.interface_count == 0)
    {
        AMI_ERROR("netstack: nothing to bring up -- DEVS:NetInterfaces holds "
                  "no usable interface file. Run NetSetup to write one, or "
                  "ShowNetStatus to see what is wrong with the one there");
        ami_free(ns);
        return AMI_NET_ERR_CONFIG;
    }

    /* ---- 2. SANA-II devices (OpenDevice/DoIO; also before adoption) ----- */

    status = ami_ns_open_devices(ns);
    if (status != AMI_NET_OK)
    {
        ami_ns_destroy(ns);
        return status;
    }

    /* ---- 3. memory ------------------------------------------------------ */

    ns->ns_PoolPackets = ami_ns_pool_packets();
    ns->ns_PoolBytes   = ns->ns_PoolPackets * ami_ns_packet_stride();

    ns->ns_PoolMemory = ami_alloc_flags(ns->ns_PoolBytes, MEMF_PUBLIC | MEMF_CLEAR);
    ns->ns_IpStack    = ami_alloc_flags((ULONG)AMI_IP_STACK_SIZE, MEMF_PUBLIC | MEMF_CLEAR);
    ns->ns_ArpCache   = ami_alloc_flags((ULONG)AMI_ARP_CACHE_SIZE, MEMF_PUBLIC | MEMF_CLEAR);

    if (ns->ns_PoolMemory == NULL || ns->ns_IpStack == NULL || ns->ns_ArpCache == NULL)
    {
        AMI_ERROR("netstack: out of memory sizing the stack");
        ami_ns_destroy(ns);
        return AMI_NET_ERR_NOMEM;
    }

    /*
     * The SANA-II readers and the driver control path block in exec Wait().
     * Teach the shim how to hand the ThreadX baton back first -- without this
     * the first CMD_READ freezes the whole kernel (netstack_baton.c).
     */
    ami_sana2_set_block_hooks(ami_netstack_baton_release,
                              ami_netstack_baton_acquire);

    /* ---- 4. ThreadX ----------------------------------------------------- */

    AMI_INFO("netstack: starting ThreadX");
    txstatus = tx_amiga_kernel_start();
    if (txstatus != TX_SUCCESS)
    {
        AMI_ERROR("netstack: tx_amiga_kernel_start failed (%ld)", (long)txstatus);
        ami_ns_destroy(ns);
        return AMI_NET_ERR_KERNEL;
    }

    /* ---- 5. become a ThreadX thread ------------------------------------- */

    AMI_INFO("netstack: kernel up, adopting this task");
    status = ami_netstack_enter(&caller);
    if (status != AMI_NET_OK)
    {
        ami_ns_destroy(ns);
        return status;
    }
    AMI_INFO("netstack: adopted, building NetX Duo");

    /* Publish before the IP thread starts: the driver entry, the reader
       threads and every accessor below expect to find the singleton. */
    ami_ns = ns;

    /* ---- 6. NetX Duo ---------------------------------------------------- */

    status = ami_ns_create_ip(ns);
    if (status != AMI_NET_OK)
    {
        ami_ns = NULL;
        ami_ns_destroy(ns);
        ami_netstack_leave(&caller);
        return status;
    }

#ifdef AMINETXDUO_BPF
    /*
     * Capture goes up with the interfaces and before any address is
     * configured, so that DHCP, ARP and IPv6 neighbour discovery are all
     * inside the trace rather than in front of it -- those are exactly the
     * exchanges a bring-up problem lives in.
     */
    ami_netstack_capture_start(ns);
#endif

    /* ---- 7. addresses --------------------------------------------------- */

    AMI_INFO("netstack: NX_IP up, configuring addresses");
    status = ami_ns_configure_addresses(ns);

    /* ---- 8. resolver ---------------------------------------------------- */

    AMI_INFO("netstack: starting the resolver");
    (VOID)ami_netstack_dns_start(ns);

#ifdef AMINETXDUO_MDNS
    /*
     * ---- 9. mDNS ---------------------------------------------------------
     *
     * After the addresses and after the resolver, and both orderings matter.
     *
     * After the addresses, because the record this machine announces IS its
     * address -- starting first would claim a name that resolves to 0.0.0.0
     * until the lease arrived. (The module does watch for later changes, so
     * this is about the first announcement being right, not about it being
     * possible.)
     *
     * After the resolver, because netstack_resolve() sends .local here and
     * everything else to the DNS client; the two are one lookup path and
     * bringing half of it up first would leave a window in which a .local
     * name went to the unicast servers, which is exactly what RFC 6762 6.7
     * says must not happen.
     *
     * Failure is not fatal and is not waited for. Probing takes about a
     * second (three probes, 250 ms apart) and it happens on the module's own
     * thread; blocking startup on it would add that second to every boot to
     * find out something no caller of netstack_startup() acts on.
     */
    AMI_INFO("netstack: starting mDNS");
    (VOID)ami_netstack_mdns_start(ns);
#endif

    ami_netstack_leave(&caller);

    /*
     * The stack exists from here on, address or not, so this is where anything
     * waiting for `WaitForPort AMITCP` is released.
     */
    ami_ns_port_create();

    if (status != AMI_NET_OK)
    {
        /*
         * No address. The stack is otherwise healthy, so keep it up: a caller
         * may still want loopback, and Online/AddNetInterface can fix the
         * interface later. Report the failure so bsdsocket does not pretend.
         */
        AMI_WARN("netstack: up, but no interface has an address -- check the "
                 "cable, or that something on this network hands out addresses");
        return status;
    }

    ns->ns_Refs = 1;

    return AMI_NET_OK;
}

/* --------------------------------------------------------------- the API -- */

LONG netstack_startup(VOID)
{
    LONG status;

    ami_ns_lock_init();

    ObtainSemaphore(&ami_ns_lock);

    if (ami_ns != NULL)
    {
        ami_ns->ns_Refs++;
        ReleaseSemaphore(&ami_ns_lock);
        return AMI_NET_OK;
    }

    status = ami_ns_bring_up();

    if (status != AMI_NET_OK && ami_ns != NULL)
    {
        /* Up but unaddressed: still a live stack, so hold the reference. */
        ami_ns->ns_Refs = 1;
    }

    ReleaseSemaphore(&ami_ns_lock);

    return status;
}

VOID netstack_shutdown(VOID)
{
    AmiNetCaller  caller;
    AmiNetStack  *ns;

    ami_ns_lock_init();

    ObtainSemaphore(&ami_ns_lock);

    ns = ami_ns;
    if (ns == NULL)
    {
        ReleaseSemaphore(&ami_ns_lock);
        return;
    }

    if (ns->ns_Refs > 0)
        ns->ns_Refs--;

    if (ns->ns_Refs > 0)
    {
        ReleaseSemaphore(&ami_ns_lock);
        return;
    }

    ami_ns = NULL;

    /* Take the barrier down before the stack behind it goes. */
    ami_ns_port_delete();

    /*
     * Teardown suspends the calling thread inside NetX Duo (nx_ip_delete()
     * waits for the IP thread), so it has to happen as a ThreadX thread.
     */
    if (ami_netstack_enter(&caller) == AMI_NET_OK)
    {
        ami_ns_destroy(ns);
        ami_netstack_leave(&caller);
    }
    else
    {
        ami_ns_destroy(ns);
    }

    ami_sana2_set_block_hooks(NULL, NULL);

    /*
     * Stop ThreadX last. We are a plain Exec Task at this point
     * (ami_netstack_leave() orphaned us), which is the position
     * tx_amiga_kernel_stop() documents for its caller.
     *
     * Only TX_SUCCESS means it is safe for this program to exit or for the
     * library to be expunged: the tick and scheduler Tasks run on stacks in
     * our own hunk, so leaving them alive past an unload means they execute
     * freed memory. Anything else and the caller must stay resident -- stop
     * refuses (leaving the kernel usable) if any application thread or a live
     * zombie remains, which is a legible failure rather than a silent hazard.
     *
     * This can block for up to ~5 s in the pathological case, with
     * ami_ns_lock held.
     */
    {
        UINT txstatus = tx_amiga_kernel_stop();

        if (txstatus != TX_SUCCESS)
        {
            AMI_ERROR("netstack: tx_amiga_kernel_stop failed (%ld) -- ThreadX "
                      "Tasks are still running; do not unload", (LONG)txstatus);
        }
    }

    ReleaseSemaphore(&ami_ns_lock);
}

AmiNetStack *netstack_get(VOID)
{
    return ami_ns;
}

NX_IP *netstack_ip(VOID)
{
    AmiNetStack *ns = ami_ns;

    return (ns != NULL && ns->ns_IpCreated) ? &ns->ns_Ip : NULL;
}

NX_PACKET_POOL *netstack_pool(VOID)
{
    AmiNetStack *ns = ami_ns;

    return (ns != NULL && ns->ns_PoolMemory != NULL) ? &ns->ns_Pool : NULL;
}

const AmiConfig *netstack_config(VOID)
{
    AmiNetStack *ns = ami_ns;

    return (ns != NULL) ? &ns->ns_Config : NULL;
}

UWORD netstack_interface_count(VOID)
{
    AmiNetStack *ns = ami_ns;

    return (ns != NULL) ? ns->ns_IfaceCount : 0;
}

LONG netstack_interface_up(UWORD index)
{
    AmiNetStack  *ns = ami_ns;
    AmiNetCaller  caller;
    ULONG         value = 0;
    UINT          status;

    if (ns == NULL || !ns->ns_IpCreated || index >= ns->ns_IfaceCount)
        return AMI_NET_ERR_STATE;

    if (ami_netstack_enter(&caller) != AMI_NET_OK)
        return AMI_NET_ERR_KERNEL;

    status = nx_ip_driver_interface_direct_command(&ns->ns_Ip, NX_LINK_ENABLE,
                                                   (UINT)index, &value);

    ami_netstack_leave(&caller);

    return (status == NX_SUCCESS) ? AMI_NET_OK : AMI_NET_ERR_NODEV;
}

LONG netstack_interface_down(UWORD index)
{
    AmiNetStack  *ns = ami_ns;
    AmiNetCaller  caller;
    ULONG         value = 0;
    UINT          status;

    if (ns == NULL || !ns->ns_IpCreated || index >= ns->ns_IfaceCount)
        return AMI_NET_ERR_STATE;

    if (ami_netstack_enter(&caller) != AMI_NET_OK)
        return AMI_NET_ERR_KERNEL;

    status = nx_ip_driver_interface_direct_command(&ns->ns_Ip, NX_LINK_DISABLE,
                                                   (UINT)index, &value);

    ami_netstack_leave(&caller);

    return (status == NX_SUCCESS) ? AMI_NET_OK : AMI_NET_ERR_NODEV;
}

BOOL netstack_interface_is_up(UWORD index)
{
    AmiNetStack *ns = ami_ns;

    if (ns == NULL || !ns->ns_IpCreated || index >= ns->ns_IfaceCount)
        return FALSE;

    return (ns->ns_Ip.nx_ip_interface[index].nx_interface_link_up != NX_FALSE)
               ? TRUE : FALSE;
}

/* ------------------------------------------- interfaces at run time ------
 *
 * bsdsocket.library's AddInterfaceTagList() and RemoveInterface() are the
 * only callers, and this is the only path by which ns_Iface[] changes after
 * netstack_startup() has returned. It lives here rather than in the library
 * because half the work is the library's (NetX Duo) and half is ours (the
 * SANA-II device, the BPF registration, the configuration slot), and an
 * interface that got only one half would not be closed by
 * netstack_shutdown().
 *
 * ns_IfaceCount IS NOT DECREMENTED BY A REMOVAL. It is the number of SLOTS
 * that have ever been populated, not the number that are live, so that a
 * removal in the middle does not renumber the ones above it -- an interface
 * index is a handle a caller may already be holding. Every loop over it that
 * touches ns_Iface[] checks the slot; the ones that read ns_Config or call a
 * NetX Duo API by index are safe on a hole either way.
 */

/*
 * Whether anything is still using this interface. This is the question
 * RemoveInterface()'s `force` parameter exists to override: "RemoveInterface()
 * will refuse to remove an interface which is still in use."
 *
 * Counted as TCP connections routed out of it. UDP sockets are deliberately
 * not counted: a datagram socket is not bound to an interface, so removing
 * one under a UDP socket costs the socket nothing it was promised. A TCP
 * connection is a different matter -- nx_ip_interface_detach() RESETS every
 * one of them, which is a visible event at the other end of the wire.
 *
 * Must be called inside a ThreadX bracket: the created-socket list is
 * circular, so the walk is bounded by NetX Duo's own count rather than by a
 * NULL that never comes.
 */
static UWORD ami_ns_interface_users(AmiNetStack *ns, UWORD index)
{
    const NX_INTERFACE *nxif = &ns->ns_Ip.nx_ip_interface[index];
    NX_TCP_SOCKET      *sock = ns->ns_Ip.nx_ip_tcp_created_sockets_ptr;
    UWORD               users = 0;
    ULONG               n;

    for (n = 0; n < ns->ns_Ip.nx_ip_tcp_created_sockets_count &&
                sock != NX_NULL; n++)
    {
        if (sock->nx_tcp_socket_connect_interface == nxif &&
            sock->nx_tcp_socket_state != NX_TCP_CLOSED)
            users++;

        sock = sock->nx_tcp_socket_created_next;
    }

    return users;
}

LONG netstack_interface_remove(UWORD index, BOOL force)
{
    AmiNetStack  *ns = ami_ns;
    AmiNetCaller  caller;
    AmiSana2If   *iface;
    UWORD         users;
    UINT          status;

    if (ns == NULL || !ns->ns_IpCreated ||
        index >= (UWORD)AMI_CFG_MAX_INTERFACES || ns->ns_Iface[index] == NULL)
        return AMI_NET_ERR_STATE;

    iface = ns->ns_Iface[index];

    if (ami_netstack_enter(&caller) != AMI_NET_OK)
        return AMI_NET_ERR_KERNEL;

    users = ami_ns_interface_users(ns, index);

    ami_netstack_leave(&caller);

    if (users != 0 && !force)
    {
        AMI_WARN("netstack: '%s' still carries %ld connection(s)",
                 ns->ns_Config.interfaces[index].name, (long)users);
        return AMI_NET_ERR_BUSY;
    }

    /*
     * Stop the readers BEFORE anything is detached. NX_LINK_DISABLE is what
     * takes the wire offline and reclaims the outstanding CMD_READs, and it
     * is also where a device that will not give them back declares itself --
     * which has to be known before nx_ip_interface_detach() zeroes the
     * NX_INTERFACE, not after.
     */
    (VOID)netstack_interface_down(index);

    if (ami_sana2_orphaned(iface))
    {
        /*
         * The device still holds read requests that point into this
         * allocation. Freeing it would hand the device memory that has been
         * given back to the system, so nothing is freed and the interface
         * stays registered -- down, and not removable until NetShutdown.
         * That is the state the published API warns about under `force`, and
         * it is reported rather than entered silently.
         */
        AMI_ERROR("netstack: '%s' cannot be removed -- the device still holds "
                  "read requests inside it",
                  ns->ns_Config.interfaces[index].name);
        return AMI_NET_ERR_STATE;
    }

    /* src/bpf/ holds the AmiSana2If as an opaque cookie, so it has to stop
       being reachable before the memory goes. */
    ami_netstack_capture_detach_one(ns, index);

    if (ami_netstack_enter(&caller) != AMI_NET_OK)
        return AMI_NET_ERR_KERNEL;

    /*
     * nx_ip_interface_detach() does the whole of NetX Duo's side: it resets
     * the TCP connections that went out of this interface, deletes its ARP
     * entries, drops the static routes and the default gateway that pointed
     * at it, leaves its multicast groups, calls the driver with
     * NX_LINK_INTERFACE_DETACH -- which is where sana2_driver.c unbinds -- and
     * zeroes the NX_INTERFACE.
     */
    status = nx_ip_interface_detach(&ns->ns_Ip, (UINT)index);

    ami_netstack_leave(&caller);

    if (status != NX_SUCCESS)
    {
        AMI_WARN("netstack: detach of interface %ld failed (%ld)",
                 (long)index, (long)status);
        return AMI_NET_ERR_STATE;
    }

    /* CloseDevice() and the reply-port teardown are exec I/O, so they happen
       outside the bracket. */
    ami_sana2_close(iface);

    ns->ns_Iface[index] = NULL;
    ns->ns_Config.interfaces[index].configured = FALSE;

    AMI_INFO("netstack: interface %ld removed", (long)index);

    return AMI_NET_OK;
}

/*
 * Interface names, compared the way the rest of the stack compares them: they
 * come from file names in DEVS:NetInterfaces and AmigaDOS file names are
 * case-insensitive, so "ETH0" and "eth0" are one interface and adding both
 * would give two names for one card.
 */
static BOOL ami_ns_same_name(const char *a, const char *b)
{
    ULONG i;

    for (i = 0; ; i++)
    {
        char ca = a[i];
        char cb = b[i];

        if (ca >= 'A' && ca <= 'Z')
            ca = (char)(ca + ('a' - 'A'));
        if (cb >= 'A' && cb <= 'Z')
            cb = (char)(cb + ('a' - 'A'));

        if (ca != cb)
            return FALSE;
        if (ca == '\0')
            return TRUE;
    }
}

/*
 * The slot a new interface will land in.
 *
 * nx_ip_interface_attach() scans nx_ip_interface[] from zero and takes the
 * first entry whose nx_interface_valid is clear, so predicting its choice is
 * the same scan -- and it has to be predicted, because ami_sana2_attach()
 * records the (NX_IP, index) binding that the driver entry looks itself up
 * by, and it must be in place BEFORE the attach calls the driver.
 *
 * Our own slot must be free too. The two can disagree only if a previous
 * removal left one of them behind, which is exactly the case worth refusing.
 */
static LONG ami_ns_free_interface_slot(AmiNetStack *ns)
{
    UWORD i;

    for (i = 0; i < (UWORD)NX_MAX_PHYSICAL_INTERFACES &&
                i < (UWORD)AMI_CFG_MAX_INTERFACES; i++)
    {
        if (ns->ns_Ip.nx_ip_interface[i].nx_interface_valid == 0 &&
            ns->ns_Iface[i] == NULL)
            return (LONG)i;
    }

    return -1;
}

LONG netstack_interface_add(const AmiIfConfig *cfg, UWORD *index_out)
{
    AmiNetStack  *ns = ami_ns;
    AmiNetCaller  caller;
    AmiIfConfig  *slot_cfg;
    AmiSana2If   *iface;
    LONG          slot;
    LONG          err = AMI_NET_OK;
    UINT          status;
    UWORD         i;

    if (ns == NULL || !ns->ns_IpCreated || cfg == NULL)
        return AMI_NET_ERR_STATE;

    if (cfg->name[0] == '\0' || cfg->device[0] == '\0')
        return AMI_NET_ERR_CONFIG;

    /* "Each such device must be assigned a unique interface name." */
    for (i = 0; i < (UWORD)AMI_CFG_MAX_INTERFACES; i++)
    {
        if (ns->ns_Iface[i] == NULL)
            continue;

        if (ami_ns_same_name(ns->ns_Config.interfaces[i].name, cfg->name))
            return AMI_NET_ERR_CONFIG;
    }

    slot = ami_ns_free_interface_slot(ns);
    if (slot < 0)
        return AMI_NET_ERR_STATE;

    /*
     * The configuration is COPIED into the netstack's own storage before the
     * device is opened, and it stays there: nx_ip_interface_attach() keeps the
     * NAME POINTER rather than the name, so the string has to outlive the
     * caller's tag list.
     */
    slot_cfg = &ns->ns_Config.interfaces[slot];
    *slot_cfg = *cfg;
    slot_cfg->configured = TRUE;

    if ((UWORD)slot >= ns->ns_Config.interface_count)
        ns->ns_Config.interface_count = (UWORD)(slot + 1);

    iface = ami_sana2_open(slot_cfg, &err);
    if (iface == NULL)
    {
        AMI_ERROR("netstack: interface \'%s\' would not open: %s unit %lu did "
                  "not answer", slot_cfg->name, slot_cfg->device,
                  (unsigned long)slot_cfg->unit);
        slot_cfg->configured = FALSE;
        return (err != AMI_NET_OK) ? err : AMI_NET_ERR_NODEV;
    }

    ns->ns_Iface[slot] = iface;

    if (ami_netstack_enter(&caller) != AMI_NET_OK)
    {
        ami_sana2_close(iface);
        ns->ns_Iface[slot] = NULL;
        slot_cfg->configured = FALSE;
        return AMI_NET_ERR_KERNEL;
    }

    /* The binding first, for the reason in ami_ns_free_interface_slot(). */
    if (ami_sana2_attach(iface, &ns->ns_Ip, (UINT)slot) != AMI_NET_OK)
    {
        ami_netstack_leave(&caller);
        ami_sana2_close(iface);
        ns->ns_Iface[slot] = NULL;
        slot_cfg->configured = FALSE;
        return AMI_NET_ERR_STATE;
    }

    status = nx_ip_interface_attach(&ns->ns_Ip, (CHAR *)slot_cfg->name,
                                    (slot_cfg->iptype == AMI_IPTYPE_STATIC)
                                        ? slot_cfg->address : 0UL,
                                    (slot_cfg->iptype == AMI_IPTYPE_STATIC)
                                        ? slot_cfg->netmask : 0UL,
                                    ami_sana2_driver_entry);

    /*
     * The slot NetX Duo actually took has to be the one predicted, because
     * the driver binding was made against the prediction. If they ever
     * disagree the interface would answer for the wrong device, so this is
     * checked rather than assumed.
     */
    if (status == NX_SUCCESS &&
        ns->ns_Ip.nx_ip_interface[slot].nx_interface_valid == 0)
        status = NX_INVALID_INTERFACE;

    ami_netstack_leave(&caller);

    if (status != NX_SUCCESS)
    {
        AMI_WARN("netstack: interface \'%s\' attach failed (%ld)",
                 slot_cfg->name, (long)status);
        ami_sana2_close(iface);
        ns->ns_Iface[slot] = NULL;
        slot_cfg->configured = FALSE;
        return AMI_NET_ERR_STATE;
    }

    if ((UWORD)slot >= ns->ns_IfaceCount)
        ns->ns_IfaceCount = (UWORD)(slot + 1);

    ami_netstack_capture_attach_one(ns, (UWORD)slot);

    if (index_out != NULL)
        *index_out = (UWORD)slot;

    AMI_INFO("netstack: interface \'%s\' added as %ld", slot_cfg->name,
             (long)slot);

    return AMI_NET_OK;
}

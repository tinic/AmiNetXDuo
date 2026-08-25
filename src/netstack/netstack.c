/*
 * AmiNetXDuo, the stack singleton.
 *
 * SPDX-License-Identifier: MIT
 */

#include "netstack_internal.h"

#include "aminetxduo/netstatus.h"
#include "aminetxduo/events.h"
#include "aminetxduo/budget.h"

#include "tx_amiga.h"

#ifdef AMINETXDUO_RX_VERIFY
#include "net68k.h"
#endif

#include "aminetxduo/random.h"

#include <exec/memory.h>
#include <exec/ports.h>
#include <exec/semaphores.h>
#include <proto/exec.h>

/*
 * tx_kernel_enter() calls this before the scheduler starts; this port builds
 * everything from netstack_startup() instead.  Weak, so a standalone test
 * executable can supply its own.
 */
__attribute__((weak)) VOID tx_application_define(VOID *first_unused_memory)
{
    (VOID)first_unused_memory;
}

static struct SignalSemaphore   ami_ns_lock;
static volatile BOOL            ami_ns_lock_ready;
static AmiNetStack             *ami_ns;
static BOOL                     ami_ns_system_initialised;
static BOOL                     ami_ns_kernel_started;

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

#ifdef AMINETXDUO_AREXX

static VOID ami_ns_port_create(VOID)
{
    ami_netstack_rexx_start();
}

static VOID ami_ns_port_delete(VOID)
{
    ami_netstack_rexx_stop();
}

#else /* !AMINETXDUO_AREXX */

static char            ami_ns_port_name[] = "AMITCP";
static struct MsgPort *ami_ns_bare_port;

static VOID ami_ns_port_create(VOID)
{
    struct MsgPort *port;

    if (ami_ns_bare_port != NULL)
        return;

    port = CreateMsgPort();
    if (port == NULL)
    {
        AMI_WARN("AMITCP: no public port. WaitForPort will not return");
        return;
    }

    port->mp_Node.ln_Name = ami_ns_port_name;
    port->mp_Node.ln_Pri  = 0;

    Forbid();
    if (FindPort((CONST_STRPTR)ami_ns_port_name) != NULL)
    {
        Permit();
        DeleteMsgPort(port);
        AMI_WARN("AMITCP: a port of that name already exists. "
                 "Ours is not added");
        return;
    }
    AddPort(port);
    ami_ns_bare_port = port;
    Permit();
}

static VOID ami_ns_port_delete(VOID)
{
    struct Message *msg;

    if (ami_ns_bare_port == NULL)
        return;

    RemPort(ami_ns_bare_port);

    while ((msg = GetMsg(ami_ns_bare_port)) != NULL)
        ReplyMsg(msg);

    DeleteMsgPort(ami_ns_bare_port);
    ami_ns_bare_port = NULL;
}

#endif /* AMINETXDUO_AREXX */

LONG ami_netstack_enter(AmiNetCaller *caller)
{
    UINT status;

    caller->nc_Adopted = FALSE;

    if (tx_amiga_kernel_running() != TX_TRUE)
        return AMI_NET_ERR_STATE;

    if (tx_amiga_caller_is_thread() != (UINT) TX_FALSE)
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
    netstack_pool_mark_low();

    if (caller->nc_Adopted)
    {
        (VOID)tx_amiga_orphan_thread(&caller->nc_Thread);
        caller->nc_Adopted = FALSE;
    }
}

AmiNetCaller *ami_netstack_enter_alloc(VOID)
{
    AmiNetCaller *caller = (AmiNetCaller *)AllocMem(sizeof(AmiNetCaller),
                                                    MEMF_PUBLIC | MEMF_CLEAR);

    if (caller == NULL)
        return NULL;

    AMI_CENSUS_ADD(caller, sizeof(AmiNetCaller));

    if (ami_netstack_enter(caller) != AMI_NET_OK)
    {
        AMI_CENSUS_DROP(caller);
        FreeMem(caller, sizeof(AmiNetCaller));
        return NULL;
    }

    return caller;
}

VOID ami_netstack_leave_free(AmiNetCaller *caller)
{
    if (caller == NULL)
        return;

    ami_netstack_leave(caller);
    AMI_CENSUS_DROP(caller);
    FreeMem(caller, sizeof(AmiNetCaller));
}

LONG ami_netstack_enter_cached(AmiNetCaller *caller)
{
    struct Task *me;
    UINT         status;

    caller->nc_Adopted = FALSE;

    if (tx_amiga_kernel_running() != TX_TRUE)
        return AMI_NET_ERR_STATE;

    if (tx_amiga_caller_is_thread() != (UINT) TX_FALSE)
        return AMI_NET_OK;                  /* nested */

    me = FindTask(NULL);

    if (caller->nc_Live && caller->nc_Task == me)
    {
        if (tx_amiga_adopt_resume(&caller->nc_Thread) == TX_SUCCESS)
        {
            caller->nc_Adopted = TRUE;
            return AMI_NET_OK;
        }

        if (tx_amiga_orphan_thread(&caller->nc_Thread) != TX_SUCCESS)
            (VOID)tx_amiga_discard_thread(&caller->nc_Thread);
        caller->nc_Live = FALSE;
        caller->nc_Task = NULL;
    }

    if (caller->nc_Live)
    {
        AMI_ERROR("netstack: bracket used from a second task");
        return AMI_NET_ERR_STATE;
    }

    /*
     * Publish the owner before adoption: a foreign RemTask() can run as soon as
     * tx_amiga_adopt_thread()'s Forbid() is released.  A failed adoption clears
     * the provisional record below.
     */
    caller->nc_Live = TRUE;
    caller->nc_Task = me;

    status = tx_amiga_adopt_thread(&caller->nc_Thread,
                                   (CHAR *)"aminetxduo caller",
                                   AMI_CALLER_PRIORITY);
    if (status != TX_SUCCESS)
    {
        caller->nc_Live = FALSE;
        caller->nc_Task = NULL;
        AMI_ERROR("netstack: cannot adopt calling task (%ld)", (long)status);
        return AMI_NET_ERR_KERNEL;
    }

    caller->nc_Adopted = TRUE;

    return AMI_NET_OK;
}

#ifdef AMINETXDUO_GREEN_REALM

/*
 * The free-baton fast path.  Declining must stay cheap and side-effect free:
 * a decline that mutated anything would race the request gate's bookkeeping.
 */
LONG ami_netstack_try_enter_cached(AmiNetCaller *caller)
{
    struct Task *me;
    UINT         status;

    caller->nc_Adopted = FALSE;

    if (tx_amiga_kernel_running() != TX_TRUE)
        return AMI_NET_ERR_STATE;

    if (tx_amiga_caller_is_thread() != (UINT) TX_FALSE)
        return AMI_NET_OK;                  /* nested */

    me = FindTask(NULL);

    if (caller->nc_Live && caller->nc_Task == me)
    {
        status = tx_amiga_adopt_try_resume(&caller->nc_Thread);

        if (status == TX_SUCCESS)
        {
            caller->nc_Adopted = TRUE;
            return AMI_NET_OK;
        }
        if (status == TX_NOT_DONE)
            return AMI_NET_ERR_BUSY;        /* contended; gate instead */

        if (tx_amiga_orphan_thread(&caller->nc_Thread) != TX_SUCCESS)
            (VOID)tx_amiga_discard_thread(&caller->nc_Thread);
        caller->nc_Live = FALSE;
        caller->nc_Task = NULL;
    }

    if (caller->nc_Live)
    {
        AMI_ERROR("netstack: bracket used from a second task");
        return AMI_NET_ERR_STATE;
    }

    if (tx_amiga_baton_free() != (UINT) TX_TRUE)
        return AMI_NET_ERR_BUSY;

    caller->nc_Live = TRUE;
    caller->nc_Task = me;

    status = tx_amiga_adopt_thread(&caller->nc_Thread,
                                   (CHAR *)"aminetxduo caller",
                                   AMI_CALLER_PRIORITY);
    if (status != TX_SUCCESS)
    {
        caller->nc_Live = FALSE;
        caller->nc_Task = NULL;
        AMI_ERROR("netstack: cannot adopt calling task (%ld)", (long)status);
        return AMI_NET_ERR_KERNEL;
    }

    caller->nc_Adopted = TRUE;

    return AMI_NET_OK;
}

#endif /* AMINETXDUO_GREEN_REALM */

VOID ami_netstack_leave_cached(AmiNetCaller *caller)
{
    netstack_pool_mark_low();           /* see ami_netstack_leave() */

    if (!caller->nc_Adopted)
        return;

    caller->nc_Adopted = FALSE;

    if (caller->nc_Live && caller->nc_Task == FindTask(NULL))
    {
        if (tx_amiga_adopt_suspend(&caller->nc_Thread) == TX_SUCCESS)
            return;

        caller->nc_Live = FALSE;
        caller->nc_Task = NULL;
    }

    (VOID)tx_amiga_orphan_thread(&caller->nc_Thread);
}

VOID ami_netstack_release(AmiNetCaller *caller)
{
    struct Task *me;
    UINT         status;

    if (caller == NULL || !caller->nc_Live)
        return;

    me = FindTask(NULL);

    /*
     * Release is a teardown call and must not be reached from inside a bracket,
     * so drop the baton the ordinary way first.
     */
    if (caller->nc_Adopted && caller->nc_Task == me)
    {
        caller->nc_Adopted = FALSE;
        (VOID)tx_amiga_orphan_thread(&caller->nc_Thread);
        caller->nc_Live = FALSE;
        caller->nc_Task = NULL;
        return;
    }

    if (caller->nc_Task == me)
    {
        if (tx_amiga_adopt_resume(&caller->nc_Thread) == TX_SUCCESS)
            (VOID)tx_amiga_orphan_thread(&caller->nc_Thread);
        else
            (VOID)tx_amiga_discard_thread(&caller->nc_Thread);
    }
    else
    {
        (VOID)ami_netstack_baton_abandon(&caller->nc_Thread);
        status = tx_amiga_discard_thread(&caller->nc_Thread);
        if (status != TX_SUCCESS && status != TX_THREAD_ERROR)
        {
            AMI_WARN("netstack: cannot discard dead task's ThreadX context "
                     "(%ld)", (LONG)status);
        }
    }

    caller->nc_Adopted = FALSE;
    caller->nc_Live = FALSE;
    caller->nc_Task = NULL;
}

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
    ULONG divisor;
    ULONG packets;

    avail = AvailMem(MEMF_PUBLIC);

    divisor = ami_config_pool_divisor((ULONG)AMI_POOL_MEM_DIVISOR);

    packets = (avail / divisor) / ami_ns_packet_stride();

    if (packets < (ULONG)AMI_POOL_WORKING_PACKETS)
    {
        ULONG afford = (avail / (ULONG)AMI_POOL_MEM_DIVISOR_LOW) /
                       ami_ns_packet_stride();

        if (afford > (ULONG)AMI_POOL_WORKING_PACKETS)
            afford = (ULONG)AMI_POOL_WORKING_PACKETS;
        if (afford > packets)
            packets = afford;
    }

    if (packets < (ULONG)AMI_POOL_MIN_PACKETS)
        packets = (ULONG)AMI_POOL_MIN_PACKETS;
    if (packets > (ULONG)AMI_POOL_MAX_PACKETS)
        packets = (ULONG)AMI_POOL_MAX_PACKETS;

    AMI_INFO("netstack: %lu bytes free / %lu, pool = %lu x %lu",
             (unsigned long)avail, (unsigned long)divisor,
             (unsigned long)packets, (unsigned long)AMI_POOL_PAYLOAD);

    return packets;
}

/*
 * TX_TIMER_PROCESS_IN_ISR is defined for this port, so this runs on the tick
 * task inside a Forbid() and counts as interrupt level to both ThreadX and
 * NetX Duo.  Nothing here may block or call NetX Duo.
 */
static VOID ami_ns_second_expired(ULONG id)
{
    (VOID)id;
    ami_second_notify();
}

static VOID ami_ns_destroy(AmiNetStack *ns)
{
    UWORD i;
    UWORD requests_retained = 0;

    if (ns == NULL)
        return;

    ami_event(NETEVENT_SHUTDOWN, NETEVENT_NOINDEX, (ULONG)ns->ns_IfaceCount);

#ifdef AMINETXDUO_RX_VERIFY
    AMI_ERROR("net68k rxverify: ip_ok %lu, transport_ok %lu, bad_ip %lu, "
             "bad_transport %lu; skip short %lu / ver %lu / len %lu / "
             "frag %lu / proto %lu / udp0 %lu",
             (unsigned long)n68k_rx_verify_stats.ip_ok,
             (unsigned long)n68k_rx_verify_stats.transport_ok,
             (unsigned long)n68k_rx_verify_stats.bad_ip,
             (unsigned long)n68k_rx_verify_stats.bad_transport,
             (unsigned long)n68k_rx_verify_stats.skip_short,
             (unsigned long)n68k_rx_verify_stats.skip_version,
             (unsigned long)n68k_rx_verify_stats.skip_length,
             (unsigned long)n68k_rx_verify_stats.skip_fragment,
             (unsigned long)n68k_rx_verify_stats.skip_protocol,
             (unsigned long)n68k_rx_verify_stats.skip_udp_nosum);
#endif

#ifdef AMINETXDUO_BPF
    ami_netstack_capture_stop(ns);
#endif

#ifdef AMINETXDUO_MDNS
    /*
     * Before nx_ip_delete(): stopping the responder sends the RFC 6762 10.1
     * goodbye, which needs a working IP instance to leave on.
     */
    ami_netstack_mdns_stop(ns);
#endif

    /*
     * Deactivate before delete, so an expiration already on the wheel of the
     * tick task cannot be dispatched into a deleted timer.
     */
    if (ns->ns_SecondCreated)
    {
        ns->ns_SecondCreated = FALSE;
        (VOID)tx_timer_deactivate(&ns->ns_Second);
        (VOID)tx_timer_delete(&ns->ns_Second);
    }

    /*
     * Stop the callback posting before the semaphore goes away;
     * ami_ns_address_changed() runs on the IP thread, which is still running.
     */
    if (ns->ns_AddrArrivedReady)
    {
        ns->ns_AddrArrivedReady = FALSE;
        (VOID)tx_semaphore_delete(&ns->ns_AddrArrived);
    }

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

    /* Delete the callback source before the DNS client it updates. */
    ami_netstack_dns_stop(ns);

#ifdef AMINETXDUO_IPV6
    /*
     * The DHCPv6 Release goes on the wire, so it must precede the teardown
     * below: nx_ip_delete() takes the interfaces down with it.
     */
    ami_netstack_dhcpv6_release(ns);
    ami_netstack_dhcpv6_destroy(ns);
#endif

    if (ns->ns_IpCreated)
    {
        (VOID)nx_ip_delete(&ns->ns_Ip);
        ns->ns_IpCreated = FALSE;
    }

    for (i = 0; i < AMI_CFG_MAX_ATTACHED; i++)
    {
        if (ns->ns_Iface[i] != NULL)
        {
            if (ami_sana2_close(ns->ns_Iface[i]))
            {
                ns->ns_Iface[i] = NULL;
            }
            else
            {
                requests_retained++;
            }
        }
    }

    /*
     * One past the highest slot still occupied, which is what the number means
     * everywhere that walks ns_Iface[].  A retained interface keeps its slot.
     */
    ns->ns_IfaceCount = 0;
    for (i = 0; i < AMI_CFG_MAX_ATTACHED; i++)
    {
        if (ns->ns_Iface[i] != NULL)
            ns->ns_IfaceCount = (UWORD)(i + 1);
    }

    /*
     * An orphaned SANA-II request still points into ns_PoolMemory and can still
     * reach ns_Pool, so once ami_sana2_close() retains an interface the whole
     * allocation set is retained with it.  A bounded leak is the only safe result.
     */
    if (requests_retained)
    {
        ami_event(NETEVENT_STACK_RETAINED, NETEVENT_NOINDEX,
                  (ULONG)requests_retained);
        AMI_ERROR("netstack: retaining packet pool and stack memory because "
                  "a SANA-II device still owns requests into them");
        return;
    }

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

    /*
     * nx_ip_interface_attach() kept a POINTER to each interface name inside this
     * list, so it may only be freed once nothing can read a name back.
     */
    ami_config_free(&ns->ns_Config);

    ami_free(ns);
}

/*
 * The PARSE has no interface limit and must not acquire one; only the attach
 * is capped (aminetxduo/config.h).
 */
_Static_assert((int)AMI_CFG_MAX_ATTACHED == (int)NX_MAX_PHYSICAL_INTERFACES,
               "AMI_CFG_MAX_ATTACHED must equal NX_MAX_PHYSICAL_INTERFACES");

static LONG ami_ns_open_devices(AmiNetStack *ns)
{
    UWORD i;
    UWORD opened = 0;
    LONG  err    = AMI_NET_ERR_NODEV;

    for (i = 0; i < ns->ns_Config.interface_count; i++)
    {
        const AmiIfConfig *cfg = &ns->ns_Config.interfaces[i];
        LONG               status;

        /*
         * AMI_CFG_MAX_ATTACHED, because the arrays this loop writes -- ns_Iface[],
         * ns_IfaceMdns[] -- are sized by it.
         */
        if (opened >= (UWORD)AMI_CFG_MAX_ATTACHED)
        {
            ami_event(NETEVENT_ATTACH_LIMIT, opened,
                      (ULONG)ns->ns_Config.interface_count);
            AMI_WARN("netstack: only %ld interfaces supported, '%s' ignored",
                     (long)NX_MAX_PHYSICAL_INTERFACES, cfg->name);
            break;
        }

        ns->ns_IfaceMdns[opened] = cfg->mdns;
        ns->ns_Iface[opened] = ami_sana2_open(cfg, &status);
        if (ns->ns_Iface[opened] == NULL)
        {
            ami_event((status == AMI_NET_ERR_DEVBAD)
                          ? NETEVENT_DEVICE_REFUSED : NETEVENT_DEVICE_OPEN,
                      opened, (ULONG)status);

            if (status == AMI_NET_ERR_DEVBAD)
            {
                AMI_ERROR("netstack: interface '%s' did not start: %s unit "
                          "%lu opened and then refused a SANA-II command",
                          cfg->name, cfg->device, (unsigned long)cfg->unit);
            }
            else
            {
                AMI_ERROR("netstack: interface '%s' did not open: %s unit "
                          "%lu did not answer, is the driver in "
                          "DEVS:Networks/ and is the card installed on that "
                          "unit?",
                          cfg->name, cfg->device, (unsigned long)cfg->unit);
            }
            err = status;
            continue;
        }

        /*
         * ns_Iface[k] and interfaces[k] are one interface everywhere else, so the
         * configuration has to move down with its slot.
         */
        if (opened != i)
            ns->ns_Config.interfaces[opened] = *cfg;

        ns->ns_IfaceCfg[opened] = opened;

        /*
         * Nobody asked for this one; it was merely found in the drawer.
         * ami_ns_yield_candidate() may offer its slot to an interface that IS named.
         */
        ns->ns_IfaceWanted[opened] = FALSE;

        opened++;
    }

    for (i = opened; i < ns->ns_Config.interface_count; i++)
        ns->ns_Config.interfaces[i].configured = FALSE;
    ns->ns_Config.interface_count = opened;

    ns->ns_IfaceCount = opened;

    return (opened > 0) ? AMI_NET_OK : err;
}

/*
 * Name the machine after its card when nothing else named it.  Runs once,
 * after the devices are open and before anything reads ns_Config.hostname.
 * hostname_source stays AMI_HOSTNAME_NONE, so a real source still outranks it.
 */
static VOID ami_ns_name_after_card(AmiNetStack *ns)
{
    UCHAR mac[AMI_ETH_ADDR_SIZE];
    char  derived[AMI_CFG_NAME_LEN];
    UWORD i;

    if (ns->ns_Config.hostname[0] != '\0')
        return;

    for (i = 0; i < ns->ns_IfaceCount; i++)
    {
        ami_sana2_get_mac(ns->ns_Iface[i], mac);

        if (!ami_config_hostname_from_hwaddr(mac, (ULONG)sizeof(mac),
                                             derived, (ULONG)sizeof(derived)))
            continue;

        ami_ns_copy_name(ns->ns_Config.hostname, derived,
                         (ULONG)sizeof(ns->ns_Config.hostname));

        AMI_INFO("netstack: nothing named this machine. It is called \"%s\" "
                 "after %s", ns->ns_Config.hostname,
                 ns->ns_Config.interfaces[i].name);
        return;
    }

    ami_ns_copy_name(ns->ns_Config.hostname, "amiga",
                     (ULONG)sizeof(ns->ns_Config.hostname));

    AMI_WARN("netstack: nothing named this machine, and no card gave a "
             "hardware address. It answers to \"amiga\". Another machine "
             "in the same state has the same name. Set a name with "
             "`hostname <name>`");
}

/*
 * Take one interface out of the tables and give its device back.  FALSE means
 * the device still owns SANA-II requests written into this stack's memory,
 * and the caller must then tear the whole stack down.
 */
static BOOL ami_ns_drop_iface(AmiNetStack *ns, UWORD slot)
{
    AmiSana2If *iface = ns->ns_Iface[slot];

    if (iface == NULL)
        return TRUE;

    if (!ami_sana2_close(iface))
        return FALSE;

    ns->ns_Iface[slot] = NULL;

    return TRUE;
}

#ifdef AMINETXDUO_RXPROBE
/*
 * The receive budget pickup boundary: nx_ip_packet_filter fires before any
 * protocol work.  The plain filter slot is also borrowed around a single OOB
 * send by src/bsdsocket/oob.c; the capture tap lives in the extended slot.
 */
static UINT ami_ns_budget_filter(VOID *ip_header_ptr, UINT direction)
{
    const UBYTE *ip4 = (const UBYTE *)ip_header_ptr;

    if (direction == NX_IP_PACKET_IN && ip4 != NULL &&
        (ip4[0] >> 4) == 4U && ip4[9] == 6U)
    {
        UINT  ihl   = (UINT)((ip4[0] & 0x0FU) << 2);
        ULONG total = ((ULONG)ip4[2] << 8) | ip4[3];

        if (ihl >= 20U && total > (ULONG)ihl + 20UL)
            ami_budget_pickup(ami_budget_clock());
    }

    return NX_SUCCESS;
}
#endif /* AMINETXDUO_RXPROBE */

static LONG ami_ns_create_ip(AmiNetStack *ns)
{
    const AmiIfConfig *cfg0;
    ULONG              addr0;
    ULONG              mask0;

    if (ns->ns_Config.interfaces == NULL || ns->ns_Config.interface_count == 0)
        return AMI_NET_ERR_CONFIG;

    cfg0 = &ns->ns_Config.interfaces[0];
    ULONG              actual;
    UINT               status;
    UWORD              i;
    UWORD              kept;

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

    addr0 = (cfg0->iptype == AMI_IPTYPE_STATIC) ? cfg0->address : 0UL;
    mask0 = (cfg0->iptype == AMI_IPTYPE_STATIC && cfg0->netmask != 0UL)
                ? cfg0->netmask : 0UL;

    /*
     * Bind interface 0 before nx_ip_create(): the IP thread calls the driver for
     * NX_LINK_INITIALIZE the moment it starts, and the driver finds its
     * AmiSana2If through the binding table.
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

#ifdef AMINETXDUO_RXPROBE
    ns->ns_Ip.nx_ip_packet_filter = ami_ns_budget_filter;
#endif

    AMI_INFO("netstack: waiting for NX_IP initialisation");
    status = nx_ip_status_check(&ns->ns_Ip, NX_IP_INITIALIZE_DONE, &actual,
                                AMI_LINK_TIMEOUT_TICKS);
    if (status != NX_SUCCESS)
    {
        AMI_ERROR("netstack: IP instance did not initialise (%ld)", (long)status);
        return AMI_NET_ERR_NODEV;
    }

    /*
     * Start the IP identification field at an unpredictable value.  The field is
     * 16 bits wide in the header and NetX Duo shifts it up by 16, so only the low
     * half is meaningful.
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

    /*
     * Inbound reassembly, both families; until this call a fragment is counted and
     * released.  It enables transmit fragmentation in the same call because there
     * is no receive-only arm.
     */
    status = nx_ip_fragment_enable(&ns->ns_Ip);
    if (status != NX_SUCCESS)
        AMI_WARN("netstack: nx_ip_fragment_enable failed (%ld)", (long)status);

    /*
     * Secondary interfaces.  nx_ip_interface_attach() drives the driver from this
     * context, so the binding must exist first here too.  One that does not attach
     * is moved out and the ones behind it move down over it, never left as a hole.
     */
    kept = 1;

    for (i = 1; i < ns->ns_IfaceCount; i++)
    {
        const AmiIfConfig *cfg;
        ULONG              addr;
        ULONG              mask;

        if (kept != i)
        {
            ns->ns_Config.interfaces[kept] = ns->ns_Config.interfaces[i];
            ns->ns_Iface[kept]             = ns->ns_Iface[i];
            ns->ns_IfaceMdns[kept]         = ns->ns_IfaceMdns[i];
            ns->ns_IfaceWanted[kept]       = ns->ns_IfaceWanted[i];
            ns->ns_IfaceCfg[kept]          = kept;
            ns->ns_Iface[i]                = NULL;
        }

        cfg = &ns->ns_Config.interfaces[kept];

        if (ami_sana2_attach(ns->ns_Iface[kept], &ns->ns_Ip, kept)
                != AMI_NET_OK)
        {
            AMI_ERROR("netstack: interface '%s' did not bind, it is not "
                      "part of this stack", cfg->name);
            if (!ami_ns_drop_iface(ns, kept))
                return AMI_NET_ERR_STATE;
            continue;
        }

        addr = (cfg->iptype == AMI_IPTYPE_STATIC) ? cfg->address : 0UL;
        mask = (cfg->iptype == AMI_IPTYPE_STATIC) ? cfg->netmask : 0UL;

        status = nx_ip_interface_attach(&ns->ns_Ip, (CHAR *)cfg->name,
                                        addr, mask, ami_sana2_driver_entry);
        if (status != NX_SUCCESS)
        {
            ami_event(NETEVENT_ATTACH_FAILED, kept, (ULONG)status);
            AMI_ERROR("netstack: interface '%s' attach failed (%ld), it is "
                      "not part of this stack", cfg->name, (long)status);
            if (!ami_ns_drop_iface(ns, kept))
                return AMI_NET_ERR_STATE;
            continue;
        }

        kept++;
    }

    if (kept < ns->ns_IfaceCount)
    {
        for (i = kept; i < ns->ns_IfaceCount; i++)
        {
            if (!ami_ns_drop_iface(ns, i))
                return AMI_NET_ERR_STATE;
            ns->ns_Config.interfaces[i].configured = FALSE;
        }

        ns->ns_IfaceCount             = kept;
        ns->ns_Config.interface_count = kept;
    }

    for (i = 0; i < ns->ns_IfaceCount; i++)
    {
        if (ns->ns_Ip.nx_ip_interface[i].nx_interface_link_up == NX_FALSE)
            ami_event(NETEVENT_LINK_DOWN, i, 0UL);
    }
    ami_event(NETEVENT_BRINGUP, NETEVENT_NOINDEX, (ULONG)ns->ns_IfaceCount);

    /*
     * Both protocol enables below run after every interface is attached, because
     * each walks the interface table once and skips a slot that is not valid yet.
     */

#ifdef AMINETXDUO_MULTICAST
    /*
     * RFC 1112 group membership; without it every multicast join returns
     * NX_NOT_ENABLED.  After the attach loop, because it only sets
     * NX_IP_IGMP_ENABLE_EVENT and the IP thread reads whichever slots are valid.
     */
    status = nx_igmp_enable(&ns->ns_Ip);
    if (status != NX_SUCCESS)
        AMI_WARN("netstack: nx_igmp_enable failed (%ld)", (long)status);
#endif

#ifdef AMINETXDUO_IPV6
    /*
     * MLD ahead of nxd_ipv6_enable(), not after it: a join taken before this call
     * is in the driver's address filter and not in MLD's table, so it is never
     * announced and no later query finds it.
     */
    status = nx_mld_enable(&ns->ns_Ip);
    if (status != NX_SUCCESS)
        AMI_WARN("netstack: nx_mld_enable failed (%ld)", (long)status);

    /*
     * nxd_icmp_enable() covers ICMPv4 as well, so it replaces nx_icmp_enable().
     * nxd_ipv6_enable() seeds the router solicitation counter and interval for
     * every interface valid when it runs; nx_ip_interface_attach() seeds nothing.
     */
    if (ami_netstack_ipv6_enable(ns) != AMI_NET_OK)
        AMI_WARN("netstack: continuing with IPv4 only");
#else
    status = nx_icmp_enable(&ns->ns_Ip);
    if (status != NX_SUCCESS)
        AMI_WARN("netstack: nx_icmp_enable failed (%ld)", (long)status);
#endif

    if (ns->ns_Config.default_gateway != 0UL)
    {
        status = nx_ip_gateway_address_set(&ns->ns_Ip,
                                           ns->ns_Config.default_gateway);
        if (status != NX_SUCCESS)
            AMI_WARN("netstack: gateway set failed (%ld)", (long)status);
    }

    /*
     * STATE=down is honoured last, because attaching is what brings an interface
     * up.  AMI_LINK_STACK_DISABLE and not NX_LINK_DISABLE, so the device stays
     * open and Online brings it back.
     */
    for (i = 0; i < ns->ns_IfaceCount; i++)
    {
        ULONG value = 0;

        if (ns->ns_Config.interfaces[i].up)
            continue;

        status = nx_ip_driver_interface_direct_command(&ns->ns_Ip,
                                                       AMI_LINK_STACK_DISABLE,
                                                       (UINT)i, &value);
        if (status != NX_SUCCESS)
            AMI_WARN("netstack: interface '%s' did not stay down (%ld)",
                     ns->ns_Config.interfaces[i].name, (long)status);
    }

    return AMI_NET_OK;
}

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

static BOOL ami_ns_wants_ipv4(const AmiNetStack *ns)
{
    UWORD i;

    for (i = 0; i < ns->ns_IfaceCount; i++)
    {
        if (ami_config_iface_wants_ipv4(&ns->ns_Config.interfaces[i]))
            return TRUE;
    }

    return FALSE;
}

static LONG ami_ns_autoip_select(AmiNetStack *ns, LONG requested_interface)
{
    UINT  status;
    UWORD i;

    if (requested_interface >= 0)
    {
        if ((ULONG)requested_interface >= (ULONG)ns->ns_IfaceCount ||
            !ami_config_iface_wants_ipv4(
                &ns->ns_Config.interfaces[requested_interface]))
            return AMI_NET_ERR_CONFIG;

        i = (UWORD)requested_interface;
    }
    else
    {
        for (i = 0; i < ns->ns_IfaceCount; i++)
        {
            if (ns->ns_Config.interfaces[i].iptype == AMI_IPTYPE_LINKLOCAL)
                break;
        }
        if (i == ns->ns_IfaceCount)
        {
            for (i = 0; i < ns->ns_IfaceCount; i++)
            {
                if (ami_config_iface_wants_ipv4(&ns->ns_Config.interfaces[i]))
                    break;
            }
        }
    }

    if (i == ns->ns_IfaceCount)
        return AMI_NET_ERR_CONFIG;

    status = nx_auto_ip_set_interface(&ns->ns_AutoIp, (UINT)i);
    if (status != NX_SUCCESS)
    {
        AMI_WARN("netstack: AutoIP could not select interface %ld (%ld)",
                 (long)i, (long)status);
        return AMI_NET_ERR_STATE;
    }

    return AMI_NET_OK;
}

static LONG ami_ns_start_autoip(AmiNetStack *ns, LONG requested_interface)
{
    UINT status;
    LONG rc;

    /*
     * Guarded on ns_AutoIpRunning: a start while it is already running resets its
     * conflict count and discards the address it is holding.
     */
    if (ns->ns_AutoIpCreated)
    {
        if (ns->ns_AutoIpRunning && requested_interface >= 0 &&
            ns->ns_AutoIp.nx_ip_interface_index != (UINT)requested_interface)
        {
            AMI_WARN("netstack: link-local fallback is already serving "
                     "interface %ld, not interface %ld",
                     (long)ns->ns_AutoIp.nx_ip_interface_index,
                     (long)requested_interface);
            return AMI_NET_ERR_STATE;
        }

        if (!ns->ns_AutoIpRunning)
        {
            rc = ami_ns_autoip_select(ns, requested_interface);
            if (rc != AMI_NET_OK)
                return rc;

            status = nx_auto_ip_start(&ns->ns_AutoIp, 0UL);
            if (status == NX_SUCCESS)
            {
                ns->ns_AutoIpRunning = TRUE;
                AMI_INFO("netstack: RFC 3927 link-local configuration restarted");
            }
            else
            {
                AMI_WARN("netstack: nx_auto_ip_start failed (%ld)", (long)status);
                return AMI_NET_ERR_STATE;
            }
        }
        return AMI_NET_OK;
    }

    ns->ns_AutoIpStack = ami_alloc_flags((ULONG)AMI_AUTOIP_STACK_SIZE, MEMF_PUBLIC);
    if (ns->ns_AutoIpStack == NULL)
    {
        AMI_WARN("netstack: no memory for the AutoIP thread");
        return AMI_NET_ERR_NOMEM;
    }

    status = nx_auto_ip_create(&ns->ns_AutoIp, (CHAR *)"AmiNetXDuo AutoIP",
                               &ns->ns_Ip, ns->ns_AutoIpStack,
                               (ULONG)AMI_AUTOIP_STACK_SIZE, AMI_AUTOIP_PRIORITY);
    if (status != NX_SUCCESS)
    {
        AMI_WARN("netstack: nx_auto_ip_create failed (%ld)", (long)status);
        ami_free(ns->ns_AutoIpStack);
        ns->ns_AutoIpStack = NULL;
        return AMI_NET_ERR_STATE;
    }
    ns->ns_AutoIpCreated = TRUE;

    rc = ami_ns_autoip_select(ns, requested_interface);
    if (rc != AMI_NET_OK)
    {
        (VOID)nx_auto_ip_delete(&ns->ns_AutoIp);
        ns->ns_AutoIpCreated = FALSE;
        ami_free(ns->ns_AutoIpStack);
        ns->ns_AutoIpStack = NULL;
        return rc;
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

    return (status == NX_SUCCESS) ? AMI_NET_OK : AMI_NET_ERR_STATE;
}

/*
 * Both address callbacks run on a NetX Duo thread.  AMI_INFO()/AMI_WARN() are
 * Exec-only and legal from any Task, and nothing here blocks.
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
 * Another host answered an ARP for an address of this machine.  Runs from
 * _nx_arp_packet_receive() on the IP thread; nothing here allocates or calls
 * back into NetX Duo.
 */
static VOID ami_ns_ip_conflict(NX_IP *ip_ptr, UINT interface_index,
                               ULONG probe_address,
                               ULONG sender_mac_msw, ULONG sender_mac_lsw)
{
    AmiNetStack *ns = ami_ns;

    (VOID)interface_index;

    if (ns == NULL || ip_ptr != &ns->ns_Ip)
        return;

    ns->ns_AddrConflicts++;
    ns->ns_LastConflictAddr = probe_address;

    AMI_ERROR("netstack: %lu.%lu.%lu.%lu is in use by %lx:%lx, two machines "
              "on one address",
              (unsigned long)((probe_address >> 24) & 0xFFUL),
              (unsigned long)((probe_address >> 16) & 0xFFUL),
              (unsigned long)((probe_address >> 8) & 0xFFUL),
              (unsigned long)(probe_address & 0xFFUL),
              (unsigned long)sender_mac_msw, (unsigned long)sender_mac_lsw);
}

VOID ami_netstack_mark(const char *event)
{
    AMI_INFO("netstack: mark %s %lu ms", event, (unsigned long)ami_millis());
}

static VOID ami_ns_address_changed(NX_IP *ip_ptr, VOID *info)
{
    AmiNetStack *ns      = ami_ns;
    BOOL         changed = FALSE;
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
        changed               = TRUE;

        /*
         * RFC 5227 2.3: announce a freshly configured address.  Safe here because
         * _nx_ip_interface_address_set() releases the protection mutex before calling
         * this notify, so re-taking it is not a recursion.
         */
        if (addr != 0UL)
            (VOID)nx_arp_gratuitous_send(&ns->ns_Ip, NX_NULL);

        /* tx_semaphore_put() does not block, which matters on the IP thread. */
        if (ns->ns_AddrArrivedReady)
            (VOID)tx_semaphore_put(&ns->ns_AddrArrived);

        if (addr == 0UL)
            AMI_WARN("netstack: interface %ld no longer has an address",
                     (long)i);
        else if (ami_ns_is_linklocal(addr))
            ami_ns_log_address("has the link-local address", i, addr);
        else
            ami_ns_log_address("address is now", i, addr);

        if (addr != 0UL)
            ami_netstack_mark("ipv4");

        /*
         * RFC 3927 1.9: a routable address supersedes a link-local one on the same
         * interface.  Never from the AutoIP thread itself -- nx_auto_ip_stop() is
         * tx_thread_suspend() and would suspend it mid-announcement.
         */
        if (ns->ns_AutoIpRunning &&
            (UINT)i == ns->ns_AutoIp.nx_ip_interface_index &&
            addr != 0UL && !ami_ns_is_linklocal(addr) &&
            tx_thread_identify() != &ns->ns_AutoIp.nx_auto_ip_thread)
        {
            (VOID)nx_auto_ip_stop(&ns->ns_AutoIp);
            ns->ns_AutoIpRunning = FALSE;
            AMI_INFO("netstack: link-local configuration stopped, interface "
                     "%ld has a routable address now", (long)i);
        }
    }

    /*
     * Once for the whole sweep.  The hook signals tasks and takes a semaphore, and
     * must not come back into NetX Duo: this is the IP thread, inside a
     * notification NetX Duo made.
     */
    if (changed)
        ami_address_change_notify();
}

static const char *ami_ns_dhcp_state_name(UCHAR state)
{
    static const char *const names[] = {
        "dhcp-notstarted", "dhcp-boot",       "dhcp-init",
        "dhcp-selecting",  "dhcp-requesting", "dhcp-bound",
        "dhcp-renewing",   "dhcp-rebinding",  "dhcp-forcerenew",
        "dhcp-probing"
    };

    if ((UINT)state >= (UINT)(sizeof(names) / sizeof(names[0])))
        return "dhcp-other";

    return names[state];
}

static VOID ami_ns_dhcp_state_changed(NX_DHCP *dhcp_ptr, UINT iface_index,
                                      UCHAR new_state)
{
    AmiNetStack *ns = ami_ns;
    UBYTE        previous;

    if (ns == NULL || dhcp_ptr != &ns->ns_Dhcp ||
        iface_index >= (UINT)AMI_CFG_MAX_ATTACHED)
        return;

    previous = ns->ns_DhcpState[iface_index];
    ns->ns_DhcpState[iface_index] = (UBYTE)new_state;

    ami_netstack_mark(ami_ns_dhcp_state_name((UCHAR)new_state));

    switch (new_state)
    {
    case NX_DHCP_STATE_BOUND:
        AMI_INFO("netstack: interface %ld has a DHCP lease",
                 (long)iface_index);
        ami_netstack_dns_dhcp_changed(ns, (UWORD)iface_index);
        break;

    case NX_DHCP_STATE_RENEWING:
        AMI_INFO("netstack: interface %ld is renewing its DHCP lease",
                 (long)iface_index);
        break;

    case NX_DHCP_STATE_REBINDING:
        AMI_WARN("netstack: interface %ld, the DHCP server that issued the "
                 "lease is not answering. The request goes to any server here",
                 (long)iface_index);
        break;

    case NX_DHCP_STATE_INIT:
        if (previous >= (UBYTE)NX_DHCP_STATE_BOUND)
        {
            AMI_WARN("netstack: interface %ld has LOST its DHCP lease. The "
                     "address and the gateway are off it. Every open "
                     "connection through it is dead",
                     (long)iface_index);

            ami_netstack_dns_dhcp_changed(ns, (UWORD)iface_index);

            /*
             * RFC 3927 1.7: keep the machine reachable on the local wire
             * while the DHCP client tries again.
             */
            if (ami_ns_start_autoip(ns, (LONG)iface_index) != AMI_NET_OK)
                AMI_WARN("netstack: no link-local fallback on this machine");
        }
        break;

    default:
        break;
    }
}

/*
 * Wait for any configured interface to have an address.  nx_ip_status_check()
 * only ever looks at interface 0, so every interface is walked here.
 */
static BOOL ami_ns_wait_for_address(AmiNetStack *ns, ULONG timeout_ticks)
{
    ULONG start = tx_time_get();

    for (;;)
    {
        UWORD i;
        ULONG spent;
        ULONG remaining;

        for (i = 0; i < ns->ns_IfaceCount; i++)
        {
            ULONG addr = 0UL;
            ULONG mask = 0UL;

            if (nx_ip_interface_address_get(&ns->ns_Ip, (UINT)i, &addr, &mask)
                    == NX_SUCCESS && addr != 0UL)
                return TRUE;
        }

        spent = tx_time_get() - start;      /* unsigned: correct across a wrap */
        if (spent >= timeout_ticks)
            return FALSE;
        remaining = timeout_ticks - spent;

        /*
         * Wait out the whole remaining timeout on the notification.  No wake is lost:
         * ns_AddrArrived counts, so a post landing between the scan above and the get
         * below is held and returns the get at once.
         */
        if (ns->ns_AddrArrivedReady)
        {
            UINT got = tx_semaphore_get(&ns->ns_AddrArrived, remaining);

            if (got != TX_SUCCESS && got != TX_NO_INSTANCE)
                return FALSE;
        }
        else
        {
            ULONG slice = (ULONG)AMI_ADDRESS_POLL_TICKS;

            if (slice == 0UL)
                slice = 1UL;
            if (slice > remaining)
                slice = remaining;

            tx_thread_sleep(slice);
        }
    }
}

/*
 * Send the first DISCOVER now rather than a second from now: RFC 2131 4.4.1's
 * random 1-10 s desynchronisation delay costs a flat second on every boot.
 * Re-arming the timer to expire on the next tick is the whole change.
 */
static VOID ami_ns_dhcp_discover_now(NX_DHCP *dhcp)
{
    (VOID)tx_timer_deactivate(&dhcp->nx_dhcp_timer);
    (VOID)tx_timer_change(&dhcp->nx_dhcp_timer, 1UL,
                          (ULONG)NX_DHCP_TIME_INTERVAL);
    (VOID)tx_timer_activate(&dhcp->nx_dhcp_timer);
}

/*
 * DHCP option 61, the client identifier, in the RFC 2132 9.14 form: hardware
 * type 0x01 then the six MAC bytes.  Returning anything but NX_TRUE makes
 * NetX Duo drop the whole message, so a short buffer costs only the option.
 */
static UINT ami_ns_dhcp_client_id(NX_DHCP *dhcp_ptr, UINT iface_index,
                                  UINT message_type, UCHAR *option_ptr,
                                  UINT *option_length)
{
    NX_INTERFACE *nxif;
    ULONG         msw, lsw;

    (VOID)message_type;

    if (*option_length < (NX_DHCP_OPTION_CLIENT_ID_SIZE + 2) ||
        dhcp_ptr->nx_dhcp_ip_ptr == NX_NULL ||
        iface_index >= NX_MAX_PHYSICAL_INTERFACES)
    {
        *option_length = 0;
        return NX_TRUE;
    }

    nxif = &dhcp_ptr->nx_dhcp_ip_ptr->nx_ip_interface[iface_index];
    msw  = nxif->nx_interface_physical_address_msw;
    lsw  = nxif->nx_interface_physical_address_lsw;

    option_ptr[0] = NX_DHCP_OPTION_CLIENT_ID;
    option_ptr[1] = NX_DHCP_OPTION_CLIENT_ID_SIZE;
    option_ptr[2] = 0x01;                       /* RFC 1700 hardware type */
    option_ptr[3] = (UCHAR)(msw >> 8);
    option_ptr[4] = (UCHAR)(msw);
    option_ptr[5] = (UCHAR)(lsw >> 24);
    option_ptr[6] = (UCHAR)(lsw >> 16);
    option_ptr[7] = (UCHAR)(lsw >> 8);
    option_ptr[8] = (UCHAR)(lsw);

    *option_length = NX_DHCP_OPTION_CLIENT_ID_SIZE + 2;

    return NX_TRUE;
}

static VOID ami_ns_dhcp_configure(AmiNetStack *ns)
{
    (VOID)nx_dhcp_interface_state_change_notify(&ns->ns_Dhcp,
                                                ami_ns_dhcp_state_changed);

    (VOID)nx_dhcp_user_option_add_callback_set(&ns->ns_Dhcp,
                                               ami_ns_dhcp_client_id);

    (VOID)nx_dhcp_user_option_request(&ns->ns_Dhcp, NX_DHCP_OPTION_GATEWAYS);
    (VOID)nx_dhcp_user_option_request(&ns->ns_Dhcp, NX_DHCP_OPTION_DNS_SVR);
    (VOID)nx_dhcp_user_option_request(&ns->ns_Dhcp, NX_DHCP_OPTION_HOST_NAME);
    (VOID)nx_dhcp_user_option_request(&ns->ns_Dhcp, AMI_DHCP_OPTION_DOMAIN);
    (VOID)nx_dhcp_user_option_request(&ns->ns_Dhcp, AMI_DHCP_OPTION_SEARCH);
    (VOID)nx_dhcp_user_option_request(&ns->ns_Dhcp, AMI_DHCP_OPTION_STATIC_ROUTE);
}

static LONG ami_ns_configure_addresses(AmiNetStack *ns)
{
    UINT  status;
    UWORD i;
    BOOL  resolved = FALSE;

    /*
     * Create the semaphore before registering the callback that posts it, and gate
     * that callback on ns_AddrArrivedReady: the notification can fire the moment a
     * static interface is addressed below.
     */
    if (tx_semaphore_create(&ns->ns_AddrArrived, (CHAR *)"nsaddr", 0)
            == TX_SUCCESS)
        ns->ns_AddrArrivedReady = TRUE;
    else
        AMI_WARN("netstack: no address-arrival semaphore. The first address "
                 "is found by polling instead");

    (VOID)nx_ip_address_change_notify(&ns->ns_Ip, ami_ns_address_changed,
                                      NX_NULL);

    /*
     * The conflict handler is per interface and there is no API that sets it, so
     * it is assigned into the NX_INTERFACE the way the AutoIP module assigns it.
     */
    for (i = 0; i < ns->ns_IfaceCount; i++)
        ns->ns_Ip.nx_ip_interface[i].nx_interface_ip_conflict_notify_handler =
            ami_ns_ip_conflict;

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
         * The literal guards nx_dhcp_create() against a zero-length name, which it has
         * no defence of its own against.  Into ns_DhcpName, the client's stable copy,
         * rather than a pointer into ns_Config.
         */
        ami_ns_copy_name(ns->ns_DhcpName,
                         (ns->ns_Config.hostname[0] != '\0')
                             ? ns->ns_Config.hostname : "amiga",
                         sizeof(ns->ns_DhcpName));

        status = nx_dhcp_create(&ns->ns_Dhcp, &ns->ns_Ip,
                                (CHAR *)ns->ns_DhcpName);
        if (status != NX_SUCCESS)
        {
            AMI_ERROR("netstack: nx_dhcp_create failed (%ld)", (long)status);
        }
        else
        {
            ns->ns_DhcpCreated = TRUE;

            /*
             * Before the client starts, so the first BOUND is reported and the
             * first DISCOVER already carries the option 61 and request list.
             */
            ami_ns_dhcp_configure(ns);

            for (i = 0; i < ns->ns_IfaceCount; i++)
            {
                if (ns->ns_Config.interfaces[i].iptype != AMI_IPTYPE_DHCP)
                    continue;

                status = nx_dhcp_interface_enable(&ns->ns_Dhcp, (UINT)i);
                if (status != NX_SUCCESS &&
                    status != NX_DHCP_INTERFACE_ALREADY_ENABLED)
                    AMI_WARN("netstack: DHCP did not enable interface %ld "
                             "(%ld)", (long)i, (long)status);
            }

            status = nx_dhcp_start(&ns->ns_Dhcp);
            if (status != NX_SUCCESS)
            {
                AMI_ERROR("netstack: nx_dhcp_start failed (%ld)", (long)status);
            }
            else
            {
                ns->ns_DhcpStarted = TRUE;
                ami_ns_dhcp_discover_now(&ns->ns_Dhcp);
                AMI_INFO("netstack: DHCP started, waiting up to %lu ticks",
                         (unsigned long)AMI_DHCP_TIMEOUT_TICKS);
            }
        }
    }

    if (ami_ns_wants(ns, AMI_IPTYPE_LINKLOCAL) &&
        ami_ns_start_autoip(ns, -1L) != AMI_NET_OK)
        AMI_WARN("netstack: an interface asked for a link-local address and "
                 "did not get one");

#ifdef AMINETXDUO_IPV6
    /*
     * Before the IPv4 wait, not after it: everything slow here runs afterwards on
     * the IP thread, so the two overlap.  `resolved` is about IPv4 and gates the
     * AMI_NET_ERR_CONFIG return, so it is not touched.
     */
    ami_netstack_ipv6_configure(ns);

    ami_netstack_dhcpv6_configure(ns);
#endif

    /*
     * The IPv6-only machine returns here.  Bring-up never waits for an IPv6
     * address: the link-local is TENTATIVE for a second of DAD and a SLAAC or
     * DHCPv6 global arrives later still.
     */
    if (!resolved && !ami_ns_wants_ipv4(ns))
    {
        AMI_INFO("netstack: no interface expects an IPv4 address, not waiting "
                 "for one");
        resolved = TRUE;
    }

    if (!resolved)
    {
        resolved = ami_ns_wait_for_address(ns, AMI_DHCP_TIMEOUT_TICKS);

        if (!resolved && ns->ns_DhcpStarted)
        {
            AMI_WARN("netstack: no DHCP server answered in %lu seconds",
                     (unsigned long)(AMI_DHCP_TIMEOUT_TICKS /
                                     (ULONG)NX_IP_PERIODIC_RATE));

            /*
             * RFC 3927: fall back to a link-local address.  If that could not be
             * started there is nothing on the way, so do not wait for it.
             */
            if (ami_ns_start_autoip(ns, -1L) != AMI_NET_OK)
            {
                AMI_WARN("netstack: no link-local fallback either, so this "
                         "interface has no address");
            }
            else
            {
                resolved = ami_ns_wait_for_address(ns, AMI_AUTOIP_TIMEOUT_TICKS);

                if (!resolved)
                    AMI_WARN("netstack: link-local configuration did not settle "
                             "either, is the cable in?");
            }
        }
    }

    {
        ULONG addr = 0UL;
        ULONG mask = 0UL;

        (VOID)nx_ip_address_get(&ns->ns_Ip, &addr, &mask);

        if (addr == 0UL && !ami_ns_wants_ipv4(ns))
        {
            AMI_INFO("netstack: no IPv4 address, this machine is IPv6-only");
        }
        else
        {
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
    }

    /*
     * Every interface configured down is not a failure.  Returning
     * AMI_NET_ERR_CONFIG makes bsd_lib_open() answer NULL, so nothing on the
     * machine could open bsdsocket.library to bring the interface back up.
     */
    if (!resolved)
    {
        UWORD i;
        BOOL  wanted_up = FALSE;

        for (i = 0; i < ns->ns_IfaceCount; i++)
        {
            if (ns->ns_Config.interfaces[i].up)
            {
                wanted_up = TRUE;
                break;
            }
        }

        if (!wanted_up)
        {
            AMI_INFO("netstack: no interface was configured up, so no address "
                     "is expected, the stack is running");
            resolved = TRUE;
        }
    }

    return resolved ? AMI_NET_OK : AMI_NET_ERR_CONFIG;
}

/*
 * Called with ami_ns_lock held.  A failed stop may leave one of the port's
 * Exec Tasks running on code or data in this hunk, so that fact is kept
 * separate from ami_ns and a later call can retry.
 */
static LONG ami_ns_kernel_stop_locked(VOID)
{
    UINT txstatus;

    if (!ami_ns_kernel_started)
        return AMI_NET_OK;

    txstatus = tx_amiga_kernel_stop();
    if (txstatus != TX_SUCCESS)
    {
        AMI_ERROR("netstack: tx_amiga_kernel_stop failed (%ld). ThreadX "
                  "Tasks are still running. Do not unload",
                  (LONG)txstatus);
        return AMI_NET_ERR_KERNEL;
    }

    ami_ns_kernel_started = FALSE;

    /* Only on success: on anything else a thread can still be inside a bracket. */
    ami_netstack_baton_reset();

    return AMI_NET_OK;
}

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

    if (ami_config_load(&ns->ns_Config) != AMI_CFG_OK)
    {
        ami_config_free(&ns->ns_Config);
        ami_free(ns);
        return AMI_NET_ERR_CONFIG;
    }

    if (ns->ns_Config.interface_count == 0)
    {
        AMI_ERROR("netstack: nothing to bring up, DEVS:NetInterfaces holds "
                  "no usable interface file. Run NetSetup to write one, or "
                  "ShowNetStatus to see what is wrong with the one there");
        ami_config_free(&ns->ns_Config);
        ami_free(ns);
        return AMI_NET_ERR_CONFIG;
    }

    status = ami_ns_open_devices(ns);
    if (status != AMI_NET_OK)
    {
        ami_ns_destroy(ns);
        return status;
    }

    ami_ns_name_after_card(ns);

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
     * The SANA-II readers and the driver control path block in Exec Wait(), so
     * the baton release/acquire hooks must be installed first: without them the
     * first CMD_READ freezes the whole kernel (netstack_baton.c).
     */
    ami_sana2_set_block_hooks(ami_netstack_baton_release,
                              ami_netstack_baton_acquire);

    ami_netstack_mark("start");

    AMI_INFO("netstack: starting ThreadX");
    txstatus = tx_amiga_kernel_start();
    if (txstatus != TX_SUCCESS)
    {
        AMI_ERROR("netstack: tx_amiga_kernel_start failed (%ld)", (long)txstatus);
        ami_ns_destroy(ns);
        return AMI_NET_ERR_KERNEL;
    }
    ami_ns_kernel_started = TRUE;

    AMI_INFO("netstack: kernel up, adopting this task");
    status = ami_netstack_enter(&caller);
    if (status != AMI_NET_OK)
    {
        ami_ns_destroy(ns);
        (VOID)ami_ns_kernel_stop_locked();
        return status;
    }
    AMI_INFO("netstack: adopted, building NetX Duo");

    /*
     * Publish before the IP thread starts: the driver entry, the reader threads
     * and every accessor below expect to find the singleton.
     */
    ami_ns = ns;

    status = ami_ns_create_ip(ns);
    if (status != AMI_NET_OK)
    {
        ami_ns = NULL;
        ami_ns_destroy(ns);
        ami_netstack_leave(&caller);
        (VOID)ami_ns_kernel_stop_locked();
        return status;
    }

#ifdef AMINETXDUO_BPF
    ami_netstack_capture_start(ns);
#endif

    AMI_INFO("netstack: NX_IP up, configuring addresses");
    status = ami_ns_configure_addresses(ns);

    AMI_INFO("netstack: starting the resolver");
    (VOID)ami_netstack_dns_start(ns);

#ifdef AMINETXDUO_MDNS
    /*
     * mDNS after the addresses and after the resolver: RFC 6762 6.7 forbids a
     * .local name going to the unicast servers.  Failure is not fatal and is not
     * waited for.
     */
    AMI_INFO("netstack: starting mDNS");
    (VOID)ami_netstack_mdns_start(ns);
#endif

    /*
     * The heartbeat timer must be created before the caller stops being a TX
     * thread.  Not fatal if it cannot be made.
     */
    if (tx_timer_create(&ns->ns_Second, (CHAR *)"anxd second",
                        ami_ns_second_expired, 0UL,
                        (ULONG)NX_IP_PERIODIC_RATE,
                        (ULONG)NX_IP_PERIODIC_RATE,
                        TX_AUTO_ACTIVATE) == TX_SUCCESS)
        ns->ns_SecondCreated = TRUE;
    else
        AMI_WARN("netstack: no one-second heartbeat. A task that exits "
                 "without closing bsdsocket.library will not be noticed");

    ami_netstack_leave(&caller);

    ami_ns_port_create();
    ami_netstack_baton_set_sampler(netstack_pool_mark_low);
    netstack_pool_sample();
    ami_netstack_health_publish();
#ifdef AMINETXDUO_AREXX
    ami_sana2_set_open_hooks(ami_netstack_rexx_suspend,
                             ami_netstack_rexx_resume);
#endif

    if (status != AMI_NET_OK)
    {
        AMI_WARN("netstack: up, but no interface has an address, check the "
                 "cable, or that something on this network hands out addresses");
        return status;
    }

    ns->ns_Refs = 1;

    return AMI_NET_OK;
}

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

    status = ami_ns_kernel_stop_locked();
    if (status != AMI_NET_OK)
    {
        ReleaseSemaphore(&ami_ns_lock);
        return status;
    }

    status = ami_ns_bring_up();

    if (status != AMI_NET_OK && ami_ns != NULL)
    {
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
        (VOID)ami_ns_kernel_stop_locked();
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

    /* The hooks point at the port that is about to be freed, so they go first. */
    ami_sana2_set_open_hooks(NULL, NULL);
    ami_ns_port_delete();
    ami_netstack_baton_set_sampler(NULL);
    ami_mem_stats()->ms_PoolTotal = 0UL;
    ami_netstack_health_unpublish();

    /*
     * nx_ip_delete() waits for the IP thread, so teardown has to happen as a
     * ThreadX thread.
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
     * Stop ThreadX last; the caller is a plain Exec Task by this point.  Only
     * TX_SUCCESS means this program may exit or the library be expunged -- the
     * tick and scheduler Tasks run on stacks in this hunk.  Can block ~5 s.
     */
    (VOID)ami_ns_kernel_stop_locked();

    ReleaseSemaphore(&ami_ns_lock);
}

BOOL netstack_can_unload(VOID)
{
    BOOL safe;

    ami_ns_lock_init();

    /*
     * Attempt, not Obtain: bsd_lib_expunge() runs under Forbid() and must not
     * Wait().  A contended lock means "cannot prove it is safe", which is the
     * direction to fail in.
     */
    if (!AttemptSemaphore(&ami_ns_lock))
        return FALSE;

    safe = (ami_ns == NULL && !ami_ns_kernel_started) ? TRUE : FALSE;
    ReleaseSemaphore(&ami_ns_lock);

    return safe;
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

/*
 * Plain loads of the NetX Duo counters, with no baton taken: a diagnostic that
 * blocks on the stack it describes is useless.  ms_PoolLow is a running
 * minimum, so it is sampled on the way out of each stack operation.
 */
VOID netstack_pool_mark_low(VOID)
{
    NX_PACKET_POOL *pool = netstack_pool();
    AmiMemStats    *m;
    ULONG           now;

    if (pool == NULL)
        return;

    m   = ami_mem_stats();
    now = pool->nx_packet_pool_available;

    if (m->ms_PoolTotal == 0UL || now < m->ms_PoolLow)
        m->ms_PoolLow = now;
}

VOID netstack_pool_sample(VOID)
{
    NX_PACKET_POOL *pool = netstack_pool();
    AmiMemStats    *m;
    ULONG           now;

    if (pool == NULL)
        return;

    m   = ami_mem_stats();
    now = pool->nx_packet_pool_available;

    if (m->ms_PoolTotal == 0UL)
        m->ms_PoolLow = now;
    else if (now < m->ms_PoolLow)
        m->ms_PoolLow = now;

    m->ms_PoolTotal      = pool->nx_packet_pool_total;
    m->ms_PoolFree       = now;
    m->ms_PoolPayload    = pool->nx_packet_pool_payload_size;
    m->ms_PoolEmpty      = pool->nx_packet_pool_empty_requests;
    m->ms_PoolWaited     = pool->nx_packet_pool_empty_suspensions;
    m->ms_PoolBadRelease = pool->nx_packet_pool_invalid_releases;
}

const AmiIfConfig *netstack_iface_config(UWORD nx_index)
{
    const AmiNetStack *ns = ami_netstack_raw();
    UWORD              slot;

    if (ns == NULL || nx_index >= ns->ns_IfaceCount ||
        nx_index >= (UWORD)AMI_CFG_MAX_ATTACHED)
        return NULL;

    slot = ns->ns_IfaceCfg[nx_index];
    if (slot >= (UWORD)AMI_CFG_MAX_ATTACHED ||
        !ns->ns_Config.interfaces[slot].configured)
        return NULL;

    return &ns->ns_Config.interfaces[slot];
}

BOOL netstack_iface_mdns(UWORD nx_index)
{
    const AmiNetStack *ns = ami_netstack_raw();

    if (ns == NULL || nx_index >= (UWORD)AMI_CFG_MAX_ATTACHED)
        return FALSE;

    return ns->ns_IfaceMdns[nx_index];
}

LONG netstack_iface_mdns_set(UWORD nx_index, BOOL enable)
{
#ifdef AMINETXDUO_MDNS
    AmiNetStack *ns = ami_netstack_raw();

    if (ns == NULL || !ns->ns_IpCreated ||
        nx_index >= (UWORD)AMI_CFG_MAX_ATTACHED ||
        ns->ns_Iface[nx_index] == NULL)
        return AMI_NET_ERR_STATE;

    return ami_netstack_mdns_iface_set(ns, nx_index, enable);
#else
    (VOID)nx_index;
    (VOID)enable;

    return AMI_NET_ERR_NODEV;
#endif
}

/*
 * A pointer to the running configuration, and nothing else.  The DHCP/RA
 * handoff is absorbed at the entry points that report live resolver state,
 * not here.
 */
const AmiConfig *netstack_config(VOID)
{
    AmiNetStack *ns = ami_ns;

    return (ns != NULL) ? &ns->ns_Config : NULL;
}

LONG netstack_hostname_offer(UWORD source, const char *name)
{
    AmiNetStack  *ns = ami_ns;
    AmiNetCaller *caller;
    BOOL          taken;

    if (ns == NULL)
        return AMI_NET_ERR_STATE;

    if (name == NULL || name[0] == '\0')
        return AMI_NET_ERR_CONFIG;

    /*
     * Inside the bracket: reports and DHCP lease reconciliation read this buffer
     * from the ThreadX side, and the baton also protects the DHCP client's
     * stable outgoing name while it is copied below.
     */
    caller = ami_netstack_enter_alloc();
    if (caller == NULL)
        return AMI_NET_ERR_KERNEL;

    taken = ami_config_hostname_offer(&ns->ns_Config, source, name);
    if (taken)
    {
        ami_ns_dhcp_hostname_displace(&ns->ns_DhcpHostname);

        if (ns->ns_DhcpCreated)
            ami_ns_copy_name(ns->ns_DhcpName, ns->ns_Config.hostname,
                             sizeof(ns->ns_DhcpName));
    }

    ami_netstack_leave_free(caller);

    if (!taken)
        return AMI_NET_ERR_CONFIG;

    AMI_INFO("netstack: host name is now \"%s\"", name);

    return AMI_NET_OK;
}

UWORD netstack_interface_count(VOID)
{
    AmiNetStack *ns = ami_ns;

    return (ns != NULL) ? ns->ns_IfaceCount : 0;
}

LONG netstack_interface_up(UWORD index)
{
    AmiNetStack  *ns = ami_ns;
    AmiNetCaller *caller;
    ULONG         value = 0;
    UINT          status;

    if (ns == NULL || !ns->ns_IpCreated || index >= ns->ns_IfaceCount)
        return AMI_NET_ERR_STATE;

    caller = ami_netstack_enter_alloc();
    if (caller == NULL)
        return AMI_NET_ERR_KERNEL;

    status = nx_ip_driver_interface_direct_command(&ns->ns_Ip, NX_LINK_ENABLE,
                                                   (UINT)index, &value);

#ifdef AMINETXDUO_IPV6
    if (status == NX_SUCCESS)
    {
        ami_netstack_ipv6_interface_up(ns, index);
        ami_netstack_dhcpv6_resume(ns, index);
    }
#endif

    ami_netstack_leave_free(caller);

    if (status != NX_SUCCESS)
    {
        ami_event(NETEVENT_ONLINE_FAILED, index, (ULONG)status);
        return AMI_NET_ERR_NODEV;
    }

    return AMI_NET_OK;
}

static LONG ami_ns_interface_disable(UWORD index, UINT command)
{
    AmiNetStack  *ns = ami_ns;
    AmiNetCaller *caller;
    ULONG         value = 0;
    UINT          status;

    if (ns == NULL || !ns->ns_IpCreated || index >= ns->ns_IfaceCount)
        return AMI_NET_ERR_STATE;

    caller = ami_netstack_enter_alloc();
    if (caller == NULL)
        return AMI_NET_ERR_KERNEL;

    status = nx_ip_driver_interface_direct_command(&ns->ns_Ip, command,
                                                   (UINT)index, &value);

    ami_netstack_leave_free(caller);

    return (status == NX_SUCCESS) ? AMI_NET_OK : AMI_NET_ERR_NODEV;
}

/*
 * Give the DHCPv6 address back before the wire goes away (RFC 8415 18.2.7).
 * Inside a ThreadX bracket, because the release sleeps while it waits for the
 * Reply and only a ThreadX thread may.
 */
static VOID ami_ns_release_dhcpv6(UWORD index)
{
#ifdef AMINETXDUO_IPV6
    AmiNetStack  *ns = ami_ns;
    AmiNetCaller *caller;

    if (ns == NULL || !ns->ns_Dhcpv6Started ||
        (UWORD)ns->ns_Dhcpv6Iface != index)
        return;

    caller = ami_netstack_enter_alloc();
    if (caller == NULL)
        return;

    ami_netstack_dhcpv6_release(ns);
    ami_netstack_dhcpv6_pause(ns);

    ami_netstack_leave_free(caller);
#else
    (VOID)index;
#endif
}

LONG netstack_interface_down(UWORD index)
{
    ami_ns_release_dhcpv6(index);

    return ami_ns_interface_disable(index, NX_LINK_DISABLE);
}

/*
 * The Roadshow SM_Down: stop transmitting, leave the device on the network.
 * IFA_DownGoesOffline turns this back into the offline path.
 */
LONG netstack_interface_stack_down(UWORD index)
{
    AmiNetStack *ns = ami_ns;

    if (ns != NULL && index < (UWORD)AMI_CFG_MAX_ATTACHED &&
        ns->ns_Config.interfaces[index].down_goes_offline)
        return netstack_interface_down(index);

    ami_ns_release_dhcpv6(index);

    return ami_ns_interface_disable(index, AMI_LINK_STACK_DISABLE);
}

BOOL netstack_interface_is_up(UWORD index)
{
    AmiNetStack *ns = ami_ns;

    if (ns == NULL || !ns->ns_IpCreated || index >= ns->ns_IfaceCount)
        return FALSE;

    return (ns->ns_Ip.nx_ip_interface[index].nx_interface_link_up != NX_FALSE)
               ? TRUE : FALSE;
}

/*
 * A removal does not decrement ns_IfaceCount: it counts slots ever populated
 * rather than slots live, so a removal in the middle does not renumber the
 * ones above it.  Every loop over ns_Iface[] checks the slot.
 */

/*
 * Whether anything is still using this interface, counted as TCP connections
 * routed out of it.  Must be called inside a ThreadX bracket; the created-
 * socket list is circular, so the walk is bounded by the NetX Duo count.
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

static BOOL ami_ns_same_name(const char *a, const char *b);

static LONG ami_ns_interface_remove_locked(UWORD index, BOOL force)
{
    AmiNetStack  *ns = ami_ns;
    AmiNetCaller *caller;
    AmiSana2If   *iface;
    UWORD         users;
    UINT          status;
    BOOL          autoip_removed = FALSE;

    if (ns == NULL || !ns->ns_IpCreated ||
        index >= (UWORD)AMI_CFG_MAX_ATTACHED || ns->ns_Iface[index] == NULL)
        return AMI_NET_ERR_STATE;

    if (ns->ns_IfaceClaims[index] != 0)
        return AMI_NET_ERR_BUSY;

    iface = ns->ns_Iface[index];

    caller = ami_netstack_enter_alloc();
    if (caller == NULL)
        return AMI_NET_ERR_KERNEL;

    users = ami_ns_interface_users(ns, index);

    ami_netstack_leave_free(caller);

    if (users != 0 && !force)
    {
        AMI_WARN("netstack: '%s' still carries %ld connection(s)",
                 ns->ns_Config.interfaces[index].name, (long)users);
        return AMI_NET_ERR_BUSY;
    }

#ifdef AMINETXDUO_MDNS
    /*
     * Disable mDNS before nx_ip_interface_detach() zeroes the NX_INTERFACE: both
     * the group leave and the RFC 6762 10.1 goodbye need it to still exist.
     */
    (VOID)ami_netstack_mdns_iface_set(ns, index, FALSE);
#endif

    /*
     * Stop the readers before anything is detached: NX_LINK_DISABLE reclaims the
     * outstanding CMD_READs, and a device that does not give them back has to be
     * known before nx_ip_interface_detach() zeroes the NX_INTERFACE.
     */
    (VOID)netstack_interface_down(index);

    if (ami_sana2_orphaned(iface))
    {
        AMI_ERROR("netstack: '%s' cannot be removed, the device still holds "
                  "read requests inside it",
                  ns->ns_Config.interfaces[index].name);
        return AMI_NET_ERR_STATE;
    }

    /*
     * Stop the DHCP client before the interface goes: nx_ip_interface_detach()
     * knows nothing about DHCP, and ns_DhcpState[] would keep saying BOUND.  The
     * lease is released rather than abandoned.
     */
    if (ns->ns_DhcpCreated)
        (VOID)netstack_interface_dhcp_stop(index, TRUE);

#ifdef AMINETXDUO_BPF
    /*
     * src/bpf/ holds the AmiSana2If as an opaque cookie, so it has to stop
     * being reachable before the memory goes.
     */
    ami_netstack_capture_detach_one(ns, index);
#endif

    caller = ami_netstack_enter_alloc();
    if (caller == NULL)
        return AMI_NET_ERR_KERNEL;

    /*
     * AutoIP keeps only the numeric NX_INTERFACE slot, so destroy it while the
     * selected interface still exists; a later request creates it again.
     */
    if (ns->ns_AutoIpCreated &&
        ns->ns_AutoIp.nx_ip_interface_index == (UINT)index)
    {
        (VOID)nx_auto_ip_stop(&ns->ns_AutoIp);
        (VOID)nx_auto_ip_delete(&ns->ns_AutoIp);
        ns->ns_AutoIpCreated = FALSE;
        ns->ns_AutoIpRunning = FALSE;
        autoip_removed = TRUE;
    }

    status = nx_ip_interface_detach(&ns->ns_Ip, (UINT)index);

    ami_netstack_leave_free(caller);

    if (autoip_removed && ns->ns_AutoIpStack != NULL)
    {
        ami_free(ns->ns_AutoIpStack);
        ns->ns_AutoIpStack = NULL;
    }

    if (status != NX_SUCCESS)
    {
        AMI_WARN("netstack: detach of interface %ld failed (%ld)",
                 (long)index, (long)status);
        return AMI_NET_ERR_STATE;
    }

    /*
     * CloseDevice() and the reply-port teardown are Exec I/O, so they happen
     * outside the bracket.
     */
    ami_sana2_close(iface);

    ns->ns_Iface[index] = NULL;
    ns->ns_Config.interfaces[index].configured = FALSE;

    AMI_INFO("netstack: interface %ld removed", (long)index);

    return AMI_NET_OK;
}

LONG netstack_interface_remove(UWORD index, BOOL force)
{
    LONG rc;

    ami_ns_lock_init();
    ObtainSemaphore(&ami_ns_lock);
    rc = ami_ns_interface_remove_locked(index, force);
    ReleaseSemaphore(&ami_ns_lock);

    return rc;
}

/*
 * Resolve and remove as one transaction.  A slot is deliberately reusable, so
 * resolving the name before taking ami_ns_lock can remove a different
 * interface that another task installed in the meantime.
 */
LONG netstack_interface_remove_named(const char *name, BOOL force)
{
    AmiNetStack *ns;
    LONG         rc = AMI_NET_ERR_NONAME;
    UWORD        i;

    if (name == NULL)
        return AMI_NET_ERR_CONFIG;

    ami_ns_lock_init();
    ObtainSemaphore(&ami_ns_lock);

    ns = ami_ns;
    if (ns == NULL || !ns->ns_IpCreated)
    {
        rc = AMI_NET_ERR_STATE;
        goto out;
    }

    for (i = 0; i < (UWORD)AMI_CFG_MAX_ATTACHED; i++)
    {
        if (ns->ns_Iface[i] == NULL ||
            !ns->ns_Config.interfaces[i].configured)
            continue;

        if (ami_ns_same_name(ns->ns_Config.interfaces[i].name, name))
        {
            rc = ami_ns_interface_remove_locked(i, force);
            break;
        }
    }

out:
    ReleaseSemaphore(&ami_ns_lock);
    return rc;
}

static VOID ami_ns_zero(APTR p, ULONG size)
{
    UBYTE *b = (UBYTE *)p;

    while (size-- > 0)
        *b++ = 0;
}


/*
 * The DHCP client, created on demand.  There can be only one, because there is
 * only one UDP port 68.  Must be called inside a ThreadX bracket.
 */
static LONG ami_ns_dhcp_ensure(AmiNetStack *ns)
{
    UINT status;

    if (ns->ns_DhcpCreated)
        return AMI_NET_OK;

    /*
     * NetX Duo keeps this pointer, so give it the client's stable outgoing
     * option-12 storage rather than live resolver configuration.
     */
    ami_ns_copy_name(ns->ns_DhcpName,
                     (ns->ns_Config.hostname[0] != '\0')
                         ? ns->ns_Config.hostname : "amiga",
                     sizeof(ns->ns_DhcpName));

    status = nx_dhcp_create(&ns->ns_Dhcp, &ns->ns_Ip,
                            (CHAR *)ns->ns_DhcpName);
    if (status != NX_SUCCESS)
    {
        AMI_ERROR("netstack: nx_dhcp_create failed (%ld)", (long)status);
        return AMI_NET_ERR_STATE;
    }

    ns->ns_DhcpCreated = TRUE;

    ami_ns_dhcp_configure(ns);

    return AMI_NET_OK;
}

LONG netstack_interface_dhcp_start(UWORD index, ULONG requested_address)
{
    AmiNetStack  *ns = ami_ns;
    AmiNetCaller *caller;
    UINT          status;
    LONG          rc;

    if (ns == NULL || !ns->ns_IpCreated ||
        index >= (UWORD)AMI_CFG_MAX_ATTACHED || ns->ns_Iface[index] == NULL)
        return AMI_NET_ERR_STATE;

    caller = ami_netstack_enter_alloc();
    if (caller == NULL)
        return AMI_NET_ERR_KERNEL;

    rc = ami_ns_dhcp_ensure(ns);
    if (rc != AMI_NET_OK)
    {
        ami_netstack_leave_free(caller);
        return rc;
    }

    if (netstack_interface_dhcp_state(index) == AMI_DHCP_WORKING)
    {
        ami_netstack_leave_free(caller);
        return AMI_NET_ERR_BUSY;
    }

    status = nx_dhcp_interface_enable(&ns->ns_Dhcp, (UINT)index);
    if (status != NX_SUCCESS && status != NX_DHCP_INTERFACE_ALREADY_ENABLED)
    {
        ami_netstack_leave_free(caller);
        AMI_WARN("netstack: DHCP did not enable interface %ld (%ld)",
                 (long)index, (long)status);
        return AMI_NET_ERR_STATE;
    }

    /*
     * skip_discover is 0: skipping DISCOVER turns the address wish into a demand,
     * and a server that disagrees answers NAK rather than offering another.
     */
    if (requested_address != 0)
        (VOID)nx_dhcp_interface_request_client_ip(&ns->ns_Dhcp, (UINT)index,
                                                  requested_address, 0);

    status = nx_dhcp_interface_start(&ns->ns_Dhcp, (UINT)index);

    /*
     * Record started before the accelerated timer can report BOUND: the resolver
     * handoff rejects callbacks from a client that is not marked started.
     */
    if (status == NX_SUCCESS)
    {
        ns->ns_DhcpStarted = TRUE;

        ami_ns_dhcp_discover_now(&ns->ns_Dhcp);
    }

    ami_netstack_leave_free(caller);

    if (status != NX_SUCCESS)
    {
        AMI_WARN("netstack: DHCP did not start on interface %ld (%ld)",
                 (long)index, (long)status);
        return AMI_NET_ERR_STATE;
    }

    ns->ns_Config.interfaces[index].iptype = AMI_IPTYPE_DHCP;

    AMI_INFO("netstack: DHCP started on interface %ld", (long)index);

    return AMI_NET_OK;
}

/*
 * Read from ns_DhcpState[], which the state-change callback maintains for
 * every interface, so no bracket is needed.
 */
LONG netstack_interface_dhcp_state(UWORD index)
{
    AmiNetStack *ns = ami_ns;

    if (ns == NULL || index >= (UWORD)AMI_CFG_MAX_ATTACHED)
        return AMI_NET_ERR_STATE;

    if (!ns->ns_DhcpCreated)
        return AMI_DHCP_IDLE;

    switch (ns->ns_DhcpState[index])
    {
        case NX_DHCP_STATE_NOT_STARTED:
            return AMI_DHCP_IDLE;

        case NX_DHCP_STATE_BOUND:
        case NX_DHCP_STATE_RENEWING:
        case NX_DHCP_STATE_REBINDING:
            return AMI_DHCP_BOUND;

        default:
            return AMI_DHCP_WORKING;
    }
}

static UWORD ami_ns_dhcp_addr_list(AmiNetStack *ns, UWORD index, UINT option,
                                   ULONG *out, UWORD max)
{
    UCHAR buffer[AMI_DHCP_MAX_ADDRS * 4];
    UINT  size = (UINT)sizeof(buffer);
    UWORD count = 0;
    UWORD i;

    if (nx_dhcp_interface_user_option_retrieve(&ns->ns_Dhcp, (UINT)index,
                                               option, buffer,
                                               &size) != NX_SUCCESS)
        return 0;

    for (i = 0; (ULONG)(i + 1) * 4UL <= (ULONG)size && count < max; i++)
    {
        ULONG addr = ((ULONG)buffer[i * 4] << 24) |
                     ((ULONG)buffer[i * 4 + 1] << 16) |
                     ((ULONG)buffer[i * 4 + 2] << 8) |
                      (ULONG)buffer[i * 4 + 3];

        /*
         * "A router address of 0 should be ignored", and the same for the
         * other two lists.
         */
        if (addr != 0)
            out[count++] = addr;
    }

    return count;
}

/* One DHCP option that is text.  Not NUL-terminated on the wire. */
VOID ami_ns_dhcp_text(AmiNetStack *ns, UWORD index, UINT option,
                      char *out, ULONG outlen)
{
    UCHAR buffer[128];
    UINT  size = (UINT)sizeof(buffer);
    ULONG n;

    out[0] = '\0';

    if (nx_dhcp_interface_user_option_retrieve(&ns->ns_Dhcp, (UINT)index,
                                               option, buffer,
                                               &size) != NX_SUCCESS)
        return;

    n = (ULONG)size;
    if (n >= outlen)
        n = outlen - 1;

    for (outlen = 0; outlen < n; outlen++)
        out[outlen] = (char)buffer[outlen];

    out[n] = '\0';
}

LONG netstack_interface_dhcp_lease(UWORD index, AmiDhcpLease *out)
{
    AmiNetStack  *ns = ami_ns;
    AmiNetCaller *caller;
    UCHAR         buffer[8];
    UINT          size;
    ULONG         addr = 0;
    ULONG         mask = 0;

    if (out == NULL)
        return AMI_NET_ERR_STATE;

    ami_ns_zero(out, sizeof(*out));

    if (ns == NULL || !ns->ns_IpCreated || !ns->ns_DhcpCreated ||
        index >= (UWORD)AMI_CFG_MAX_ATTACHED)
        return AMI_NET_ERR_STATE;

    if (netstack_interface_dhcp_state(index) != AMI_DHCP_BOUND)
        return AMI_NET_ERR_STATE;

    caller = ami_netstack_enter_alloc();
    if (caller == NULL)
        return AMI_NET_ERR_KERNEL;

    if (nx_ip_interface_address_get(&ns->ns_Ip, (UINT)index, &addr, &mask)
            == NX_SUCCESS)
    {
        out->adl_Address = addr;
        out->adl_NetMask = mask;
    }

    (VOID)nx_dhcp_interface_server_address_get(&ns->ns_Dhcp, (UINT)index,
                                               &out->adl_Server);

    size = (UINT)sizeof(buffer);
    if (nx_dhcp_interface_user_option_retrieve(&ns->ns_Dhcp, (UINT)index,
                                               NX_DHCP_OPTION_DHCP_LEASE,
                                               buffer, &size) == NX_SUCCESS &&
        size >= 4)
    {
        out->adl_LeaseSeconds = ((ULONG)buffer[0] << 24) |
                                ((ULONG)buffer[1] << 16) |
                                ((ULONG)buffer[2] << 8) |
                                 (ULONG)buffer[3];
    }

    out->adl_RouterCount =
        ami_ns_dhcp_addr_list(ns, index, NX_DHCP_OPTION_GATEWAYS,
                              out->adl_Router, AMI_DHCP_MAX_ADDRS);
    out->adl_DnsCount =
        ami_ns_dhcp_addr_list(ns, index, NX_DHCP_OPTION_DNS_SVR,
                              out->adl_Dns, AMI_DHCP_MAX_ADDRS);
    out->adl_StaticRouteCount =
        ami_ns_dhcp_addr_list(ns, index, AMI_DHCP_OPTION_STATIC_ROUTE,
                              out->adl_StaticRoute, AMI_DHCP_MAX_ADDRS);

    ami_ns_dhcp_text(ns, index, NX_DHCP_OPTION_HOST_NAME,
                     out->adl_HostName, sizeof(out->adl_HostName));
    ami_ns_dhcp_text(ns, index, AMI_DHCP_OPTION_DOMAIN,
                     out->adl_DomainName, sizeof(out->adl_DomainName));

    ami_netstack_leave_free(caller);

    return AMI_NET_OK;
}

/*
 * NETSTATUS_DHCPRAW_* is published as NX_DHCP_STATE_* verbatim, and this is
 * the only file that can see both: if a NetX Duo update renumbers them the
 * build stops here.
 */
_Static_assert(NETSTATUS_DHCPRAW_NOT_STARTED == NX_DHCP_STATE_NOT_STARTED,
               "DHCP state ABI");
_Static_assert(NETSTATUS_DHCPRAW_BOOT        == NX_DHCP_STATE_BOOT,
               "DHCP state ABI");
_Static_assert(NETSTATUS_DHCPRAW_INIT        == NX_DHCP_STATE_INIT,
               "DHCP state ABI");
_Static_assert(NETSTATUS_DHCPRAW_SELECTING   == NX_DHCP_STATE_SELECTING,
               "DHCP state ABI");
_Static_assert(NETSTATUS_DHCPRAW_REQUESTING  == NX_DHCP_STATE_REQUESTING,
               "DHCP state ABI");
_Static_assert(NETSTATUS_DHCPRAW_BOUND       == NX_DHCP_STATE_BOUND,
               "DHCP state ABI");
_Static_assert(NETSTATUS_DHCPRAW_RENEWING    == NX_DHCP_STATE_RENEWING,
               "DHCP state ABI");
_Static_assert(NETSTATUS_DHCPRAW_REBINDING   == NX_DHCP_STATE_REBINDING,
               "DHCP state ABI");
_Static_assert(NETSTATUS_DHCPRAW_FORCERENEW  == NX_DHCP_STATE_FORCERENEW,
               "DHCP state ABI");
_Static_assert(NETSTATUS_DHCPRAW_PROBING     == NX_DHCP_STATE_ADDRESS_PROBING,
               "DHCP state ABI");

UWORD netstack_interface_dhcp_raw_state(UWORD index)
{
    AmiNetStack *ns = ami_ns;

    if (ns == NULL || !ns->ns_DhcpCreated ||
        index >= (UWORD)AMI_CFG_MAX_ATTACHED)
        return (UWORD)NX_DHCP_STATE_NOT_STARTED;

    return (UWORD)ns->ns_DhcpState[index];
}

LONG netstack_interface_dhcp_renew(UWORD index)
{
    AmiNetStack  *ns = ami_ns;
    AmiNetCaller *caller;
    UINT          status;

    if (ns == NULL || !ns->ns_IpCreated || !ns->ns_DhcpCreated ||
        index >= (UWORD)AMI_CFG_MAX_ATTACHED)
        return AMI_NET_ERR_STATE;

    if (netstack_interface_dhcp_state(index) != AMI_DHCP_BOUND)
        return AMI_NET_ERR_STATE;

    caller = ami_netstack_enter_alloc();
    if (caller == NULL)
        return AMI_NET_ERR_KERNEL;

    status = nx_dhcp_interface_force_renew(&ns->ns_Dhcp, (UINT)index);

    ami_netstack_leave_free(caller);

    if (status != NX_SUCCESS)
    {
        AMI_WARN("netstack: DHCP did not renew interface %ld (%ld)",
                 (long)index, (long)status);
        return AMI_NET_ERR_STATE;
    }

    AMI_INFO("netstack: DHCP renewal asked for on interface %ld", (long)index);

    return AMI_NET_OK;
}

LONG netstack_interface_dhcp_stop(UWORD index, BOOL release)
{
    AmiNetStack  *ns = ami_ns;
    AmiNetCaller *caller;

    if (ns == NULL || !ns->ns_DhcpCreated ||
        index >= (UWORD)AMI_CFG_MAX_ATTACHED)
        return AMI_NET_ERR_STATE;

    caller = ami_netstack_enter_alloc();
    if (caller == NULL)
        return AMI_NET_ERR_KERNEL;

    if (release)
        (VOID)nx_dhcp_interface_release(&ns->ns_Dhcp, (UINT)index);

    (VOID)nx_dhcp_interface_stop(&ns->ns_Dhcp, (UINT)index);

    ns->ns_DhcpState[index] = NX_DHCP_STATE_NOT_STARTED;
    ami_netstack_dns_dhcp_changed(ns, index);

    ami_netstack_leave_free(caller);

    return AMI_NET_OK;
}

/*
 * Interface names come from file names in DEVS:NetInterfaces and AmigaDOS
 * file names are case-insensitive, so "ETH0" and "eth0" are one interface.
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
 * Turn a name into a stable numeric interface slot.  Once the count is raised
 * the slot cannot be detached and reused until the matching release.
 */
LONG netstack_interface_claim(const char *name, UWORD *index_out)
{
    AmiNetStack *ns;
    LONG         rc = AMI_NET_ERR_STATE;
    UWORD        i;

    if (name == NULL || name[0] == '\0' || index_out == NULL)
        return AMI_NET_ERR_CONFIG;

    ami_ns_lock_init();
    ObtainSemaphore(&ami_ns_lock);

    ns = ami_ns;
    if (ns != NULL && ns->ns_IpCreated)
    {
        for (i = 0; i < (UWORD)AMI_CFG_MAX_ATTACHED; i++)
        {
            UWORD cfg_index;

            if (ns->ns_Iface[i] == NULL)
                continue;

            cfg_index = ns->ns_IfaceCfg[i];
            if (cfg_index >= (UWORD)AMI_CFG_MAX_ATTACHED ||
                !ns->ns_Config.interfaces[cfg_index].configured ||
                !ami_ns_same_name(ns->ns_Config.interfaces[cfg_index].name,
                                  name))
                continue;

            if (ns->ns_IfaceClaims[i] == (UWORD)-1)
            {
                rc = AMI_NET_ERR_BUSY;
                break;
            }

            ns->ns_IfaceClaims[i]++;
            *index_out = i;
            rc = AMI_NET_OK;
            break;
        }
    }

    ReleaseSemaphore(&ami_ns_lock);
    return rc;
}

#ifdef AMINETXDUO_BPF
/*
 * The BPF table treats its SANA-II pointer as an opaque cookie.  Resolve and
 * claim it under ami_ns_lock: once the count is raised, RemoveInterface()
 * cannot detach or free the allocation until release.
 */
LONG ami_netstack_interface_claim_cookie(APTR cookie, UWORD *index_out)
{
    AmiNetStack *ns;
    LONG         rc = AMI_NET_ERR_STATE;
    UWORD        i;

    if (cookie == NULL || index_out == NULL)
        return AMI_NET_ERR_CONFIG;

    ami_ns_lock_init();
    ObtainSemaphore(&ami_ns_lock);

    ns = ami_ns;
    if (ns != NULL && ns->ns_IpCreated)
    {
        for (i = 0; i < (UWORD)AMI_CFG_MAX_ATTACHED; i++)
        {
            if ((APTR)ns->ns_Iface[i] != cookie)
                continue;

            if (ns->ns_IfaceClaims[i] == (UWORD)-1)
                rc = AMI_NET_ERR_BUSY;
            else
            {
                ns->ns_IfaceClaims[i]++;
                *index_out = i;
                rc = AMI_NET_OK;
            }
            break;
        }
    }

    ReleaseSemaphore(&ami_ns_lock);
    return rc;
}
#endif

VOID netstack_interface_release(UWORD index)
{
    AmiNetStack *ns;

    ami_ns_lock_init();
    ObtainSemaphore(&ami_ns_lock);

    ns = ami_ns;
    if (ns != NULL && index < (UWORD)AMI_CFG_MAX_ATTACHED &&
        ns->ns_IfaceClaims[index] != 0)
        ns->ns_IfaceClaims[index]--;

    ReleaseSemaphore(&ami_ns_lock);
}

/*
 * Predict the slot a new interface will land in -- the same first-free scan
 * nx_ip_interface_attach() does -- because ami_sana2_attach() must record the
 * (NX_IP, index) binding before the attach calls the driver.
 */
static LONG ami_ns_vacant_interface_slot(AmiNetStack *ns)
{
    UWORD i;

    for (i = 0; i < (UWORD)NX_MAX_PHYSICAL_INTERFACES &&
                i < (UWORD)AMI_CFG_MAX_ATTACHED; i++)
    {
        if (ns->ns_Ip.nx_ip_interface[i].nx_interface_valid == 0 &&
            ns->ns_Iface[i] == NULL && ns->ns_IfaceClaims[i] == 0)
            return (LONG)i;
    }

    return -1;
}

/*
 * A slot that could be made vacant for an interface that was NAMED, and which
 * one.  Only a slot the start-up pass claimed on its own may be offered, last
 * slot first.  A candidate, not a casualty: nothing is taken down here.
 */
static LONG ami_ns_yield_candidate(AmiNetStack *ns)
{
    LONG i;

    for (i = (LONG)AMI_CFG_MAX_ATTACHED - 1; i >= 0; i--)
    {
        if (ns->ns_Iface[i] != NULL && !ns->ns_IfaceWanted[i] &&
            ns->ns_IfaceClaims[i] == 0)
            return i;
    }

    return -1;
}

/*
 * Take the candidate's slot, now that the newcomer's device has opened.  The
 * removal is NOT forced: an interface carrying TCP connections keeps its slot
 * and this fails.
 */
static LONG ami_ns_take_interface_slot(AmiNetStack *ns, LONG victim)
{
    if (victim < 0 || victim >= (LONG)AMI_CFG_MAX_ATTACHED ||
        ns->ns_Iface[victim] == NULL || ns->ns_IfaceWanted[victim])
        return -1;

    AMI_INFO("netstack: '%s' was brought up on its own and gives up "
             "interface slot %ld", ns->ns_Config.interfaces[victim].name,
             (long)victim);

    if (ami_ns_interface_remove_locked((UWORD)victim, FALSE) != AMI_NET_OK)
        return -1;

    ami_event(NETEVENT_ATTACH_YIELD, (UWORD)victim,
              (ULONG)ns->ns_Config.interface_count);

    return ami_ns_vacant_interface_slot(ns);
}

static LONG ami_ns_interface_start_locked(const AmiIfConfig *cfg,
                                          UWORD *index_out, BOOL wanted);

/*
 * Put back the interface that stood down, because the one it stood down for
 * did not come up.  `wanted` is FALSE: it goes back to being one the boot
 * brought up.  Cannot recurse -- the newcomer's slot is already given back.
 */
static VOID ami_ns_restore_stood_down(const AmiIfConfig *cfg)
{
    LONG rc;

    if (cfg == NULL || cfg->name[0] == '\0')
        return;

    rc = ami_ns_interface_start_locked(cfg, NULL, FALSE);
    if (rc == AMI_NET_OK)
    {
        AMI_INFO("netstack: '%s' has its interface slot back", cfg->name);
        return;
    }

    ami_event(NETEVENT_ATTACH_FAILED, NETEVENT_NOINDEX, (ULONG)rc);
    AMI_ERROR("netstack: '%s' gave up its slot and could not be put back (%ld)",
              cfg->name, (long)rc);
}

/*
 * `wanted` says somebody NAMED this interface, as against the start-up pass
 * finding it in the drawer.  `stood_down` is filled in only on SUCCESS, so a
 * caller that fails later has one thing to undo and not two; NULL if unwanted.
 */
static LONG ami_ns_interface_add_locked(const AmiIfConfig *cfg,
                                        UWORD *index_out, BOOL wanted,
                                        AmiIfConfig *stood_down)
{
    AmiNetStack  *ns = ami_ns;
    AmiNetCaller *caller;
    AmiIfConfig  *slot_cfg;
    AmiIfConfig   open_cfg;
    AmiIfConfig   victim_cfg;
    AmiSana2If   *iface;
    LONG          slot;
    LONG          victim;
    LONG          err = AMI_NET_OK;
    UINT          status;
    UWORD         i;

    if (stood_down != NULL)
        stood_down->name[0] = '\0';

    victim_cfg.name[0] = '\0';

    if (ns == NULL || !ns->ns_IpCreated || cfg == NULL)
        return AMI_NET_ERR_STATE;

    if (cfg->name[0] == '\0' || cfg->device[0] == '\0')
        return AMI_NET_ERR_CONFIG;

    for (i = 0; i < (UWORD)AMI_CFG_MAX_ATTACHED; i++)
    {
        if (ns->ns_Iface[i] == NULL)
            continue;

        if (ami_ns_same_name(ns->ns_Config.interfaces[i].name, cfg->name))
        {
            /*
             * Naming an interface that is already up makes it yours, even though
             * there is nothing to attach.  The duplicate is still refused.
             */
            if (wanted)
                ns->ns_IfaceWanted[i] = TRUE;

            return AMI_NET_ERR_CONFIG;
        }
    }

    slot   = ami_ns_vacant_interface_slot(ns);
    victim = (slot < 0 && wanted) ? ami_ns_yield_candidate(ns) : -1;

    if (slot < 0 && victim < 0)
    {
        ami_event(NETEVENT_ATTACH_LIMIT, (UWORD)AMI_CFG_MAX_ATTACHED,
                  (ULONG)ns->ns_Config.interface_count);
        return AMI_NET_ERR_NOSLOT;
    }

    /*
     * The device opens before any slot changes hands.  ami_sana2_open() keeps no
     * pointer into the configuration, so it can be given a local copy and needs
     * no slot chosen yet; that ordering is the whole safety of the yield.
     */
    open_cfg = *cfg;

    iface = ami_sana2_open(&open_cfg, &err);
    if (iface == NULL)
    {
        ami_event((err == AMI_NET_ERR_DEVBAD)
                      ? NETEVENT_DEVICE_REFUSED : NETEVENT_DEVICE_OPEN,
                  NETEVENT_NOINDEX, (ULONG)err);
        AMI_ERROR("netstack: interface \'%s\' did not start: %s unit %lu %s",
                  open_cfg.name, open_cfg.device,
                  (unsigned long)open_cfg.unit,
                  (err == AMI_NET_ERR_DEVBAD) ? "refused a SANA-II command"
                                              : "did not answer");
        return (err != AMI_NET_OK) ? err : AMI_NET_ERR_NODEV;
    }

    if (slot < 0)
    {
        /*
         * Before the take and not after it: the newcomer's configuration is written
         * into the description this points at.
         */
        victim_cfg = ns->ns_Config.interfaces[victim];

        slot = ami_ns_take_interface_slot(ns, victim);
        if (slot < 0)
        {
            victim_cfg.name[0] = '\0';
            ami_sana2_close(iface);
            ami_event(NETEVENT_ATTACH_LIMIT, (UWORD)AMI_CFG_MAX_ATTACHED,
                      (ULONG)ns->ns_Config.interface_count);
            return AMI_NET_ERR_NOSLOT;
        }
    }

    /*
     * The slot is a NetX Duo index, not a position in the parsed list, so the list
     * has to be long enough to have that index at all.
     */
    if (!ami_config_reserve(&ns->ns_Config, (UWORD)(slot + 1)))
    {
        ami_sana2_close(iface);
        ami_ns_restore_stood_down(&victim_cfg);
        return AMI_NET_ERR_NOMEM;
    }

    /*
     * nx_ip_interface_attach() keeps the name pointer rather than the name, so the
     * string must outlive the caller's tag list.
     */
    slot_cfg = &ns->ns_Config.interfaces[slot];
    *slot_cfg = open_cfg;
    slot_cfg->configured = TRUE;

#ifdef AMINETXDUO_IPV6
    if (slot_cfg->ip6type == AMI_IP6TYPE_OFF)
    {
        slot_cfg->ip6type = AMI_IP6TYPE_AUTO;
        slot_cfg->prefix6 = 64;
    }
#endif

    if ((UWORD)slot >= ns->ns_Config.interface_count)
        ns->ns_Config.interface_count = (UWORD)(slot + 1);

    ns->ns_Iface[slot] = iface;

    caller = ami_netstack_enter_alloc();
    if (caller == NULL)
    {
        ami_sana2_close(iface);
        ns->ns_Iface[slot] = NULL;
        slot_cfg->configured = FALSE;
        ami_ns_restore_stood_down(&victim_cfg);
        return AMI_NET_ERR_KERNEL;
    }

    /* The binding first, for the reason in ami_ns_vacant_interface_slot(). */
    if (ami_sana2_attach(iface, &ns->ns_Ip, (UINT)slot) != AMI_NET_OK)
    {
        ami_netstack_leave_free(caller);
        ami_sana2_close(iface);
        ns->ns_Iface[slot] = NULL;
        slot_cfg->configured = FALSE;
        ami_ns_restore_stood_down(&victim_cfg);
        return AMI_NET_ERR_STATE;
    }

    status = nx_ip_interface_attach(&ns->ns_Ip, (CHAR *)slot_cfg->name,
                                    (slot_cfg->iptype == AMI_IPTYPE_STATIC)
                                        ? slot_cfg->address : 0UL,
                                    (slot_cfg->iptype == AMI_IPTYPE_STATIC)
                                        ? slot_cfg->netmask : 0UL,
                                    ami_sana2_driver_entry);

    /*
     * The slot NetX Duo took must be the predicted one, because the driver binding
     * was made against the prediction.
     */
    if (status == NX_SUCCESS &&
        ns->ns_Ip.nx_ip_interface[slot].nx_interface_valid == 0)
        status = NX_INVALID_INTERFACE;

#ifdef AMINETXDUO_IPV6
    /*
     * Use the bracket that attached the interface: a second allocation after the
     * attach can fail at the memory floor.
     */
    if (status == NX_SUCCESS)
        ami_netstack_ipv6_configure_one(ns, (UWORD)slot);
#endif

    ami_netstack_leave_free(caller);

    if (status != NX_SUCCESS)
    {
        ami_event(NETEVENT_ATTACH_FAILED, (UWORD)slot, (ULONG)status);
        AMI_WARN("netstack: interface \'%s\' attach failed (%ld)",
                 slot_cfg->name, (long)status);
        ami_sana2_close(iface);
        ns->ns_Iface[slot] = NULL;
        slot_cfg->configured = FALSE;
        ami_ns_restore_stood_down(&victim_cfg);
        return AMI_NET_ERR_STATE;
    }

    if ((UWORD)slot >= ns->ns_IfaceCount)
        ns->ns_IfaceCount = (UWORD)(slot + 1);

    ns->ns_IfaceCfg[slot]  = (UWORD)slot;

    ns->ns_IfaceWanted[slot] = wanted;

    /*
     * MDNS= is acted on and not merely recorded.  Cleared first, because a slot
     * re-used by a different interface must not inherit the last one's answer.
     */
    ns->ns_IfaceMdns[slot] = FALSE;
#ifdef AMINETXDUO_MDNS
    if (slot_cfg->mdns)
        (VOID)ami_netstack_mdns_iface_set(ns, (UWORD)slot, TRUE);
#endif

#ifdef AMINETXDUO_BPF
    ami_netstack_capture_attach_one(ns, (UWORD)slot);
#endif

    if (index_out != NULL)
        *index_out = (UWORD)slot;

    if (stood_down != NULL && victim_cfg.name[0] != '\0')
        *stood_down = victim_cfg;

    AMI_INFO("netstack: interface \'%s\' added as %ld", slot_cfg->name,
             (long)slot);

    return AMI_NET_OK;
}

/*
 * ami_ns_lock is the outer lock: the slot is picked and the SANA-II device
 * opened long before anything marks it taken.  Exec semaphores nest per task,
 * so a caller already holding it is not deadlocked by this.
 */
LONG netstack_interface_add(const AmiIfConfig *cfg, UWORD *index_out)
{
    LONG rc;

    ami_ns_lock_init();
    ObtainSemaphore(&ami_ns_lock);
    rc = ami_ns_interface_add_locked(cfg, index_out, TRUE, NULL);
    ReleaseSemaphore(&ami_ns_lock);

    return rc;
}

/*
 * The same add, plus everything start-up does to an interface it finds in
 * DEVS:NetInterfaces.  `wanted` is FALSE only from ami_ns_restore_stood_down();
 * that is also why this is split from netstack_interface_start().
 */
static LONG ami_ns_interface_start_locked(const AmiIfConfig *cfg,
                                          UWORD *index_out, BOOL wanted)
{
    AmiNetStack  *ns;
    AmiNetCaller *caller;
    AmiIfConfig   stood_down;
    UWORD         index                  = 0;
    ULONG         gateway                = 0UL;
    BOOL          autoip_created_before  = FALSE;
    BOOL          autoip_running_before  = FALSE;
    BOOL          added                  = FALSE;
    LONG          rc;

    stood_down.name[0] = '\0';

    ns = ami_ns;
    if (ns == NULL || !ns->ns_IpCreated || cfg == NULL)
        return AMI_NET_ERR_STATE;

    autoip_created_before = ns->ns_AutoIpCreated;
    autoip_running_before = ns->ns_AutoIpRunning;

    rc = ami_ns_interface_add_locked(cfg, &index, wanted, &stood_down);
    if (rc != AMI_NET_OK)
        goto out;
    added = TRUE;

    /*
     * nx_ip_interface_detach() takes the default gateway with the interface that
     * carried it, so it has to be reinstalled here.  Last, so a later
     * configuration failure cannot overwrite the machine-wide route.
     */
    if (cfg->iptype == AMI_IPTYPE_STATIC)
        gateway = (cfg->gateway != 0UL) ? cfg->gateway
                                        : ns->ns_Config.default_gateway;

    if (cfg->iptype == AMI_IPTYPE_DHCP)
    {
        rc = netstack_interface_dhcp_start(index, 0UL);
        if (rc != AMI_NET_OK)
            goto rollback;
    }
    else if (cfg->iptype == AMI_IPTYPE_LINKLOCAL)
    {
        caller = ami_netstack_enter_alloc();
        if (caller == NULL)
        {
            rc = AMI_NET_ERR_KERNEL;
            goto rollback;
        }

        /*
         * The index is named: the unqualified selector accepts an AutoIP object
         * already serving a different slot and reports success while this
         * interface receives no address.
         */
        rc = ami_ns_start_autoip(ns, (LONG)index);
        ami_netstack_leave_free(caller);

        /*
         * Not having a link-local address does not stop the interface.  AutoIP is one
         * machine-wide object serving one slot, so the second asker is refused;
         * start-up warns for the same condition and carries on.
         */
        if (rc != AMI_NET_OK)
            AMI_WARN("netstack: interface '%s' asked for a link-local "
                     "address and did not get one", cfg->name);

        rc = AMI_NET_OK;
    }

    if (!cfg->up)
    {
        rc = ami_ns_interface_disable(index, AMI_LINK_STACK_DISABLE);
        if (rc != AMI_NET_OK)
            goto rollback;
    }

    if (gateway != 0UL)
    {
        UINT status;

        caller = ami_netstack_enter_alloc();
        if (caller == NULL)
        {
            rc = AMI_NET_ERR_KERNEL;
            goto rollback;
        }

        status = nx_ip_gateway_address_set(&ns->ns_Ip, gateway);
        ami_netstack_leave_free(caller);

        if (status != NX_SUCCESS)
        {
            /*
             * A refused gateway is not a reason to destroy an interface.
             * nx_ip_gateway_address_set() refuses a next hop on no interface's
             * network, which is what a mistyped GATEWAY line looks like.
             */
            ami_event(NETEVENT_GATEWAY_REFUSED, index, (ULONG)status);
            AMI_WARN("netstack: '%s' is up, and the default route %lu.%lu.%lu."
                     "%lu it asked for was refused (%ld)", cfg->name,
                     (unsigned long)((gateway >> 24) & 0xFFUL),
                     (unsigned long)((gateway >> 16) & 0xFFUL),
                     (unsigned long)((gateway >> 8) & 0xFFUL),
                     (unsigned long)(gateway & 0xFFUL), (long)status);
        }
    }

    if (index_out != NULL)
        *index_out = index;

    rc = AMI_NET_OK;
    goto out;

rollback:
    /*
     * AutoIP is one machine-wide object rather than one per interface.  If this
     * transaction created or restarted it, restore the state that was present
     * before removing the interface it may currently name.
     */
    if (cfg->iptype == AMI_IPTYPE_LINKLOCAL &&
        ((!autoip_created_before && ns->ns_AutoIpCreated) ||
         (!autoip_running_before && ns->ns_AutoIpRunning)))
    {
        caller = ami_netstack_enter_alloc();
        if (caller != NULL)
        {
            if (!autoip_running_before && ns->ns_AutoIpRunning)
            {
                (VOID)nx_auto_ip_stop(&ns->ns_AutoIp);
                ns->ns_AutoIpRunning = FALSE;
            }

            if (!autoip_created_before && ns->ns_AutoIpCreated)
            {
                (VOID)nx_auto_ip_delete(&ns->ns_AutoIp);
                ns->ns_AutoIpCreated = FALSE;
            }

            ami_netstack_leave_free(caller);

            if (!autoip_created_before && ns->ns_AutoIpStack != NULL)
            {
                ami_free(ns->ns_AutoIpStack);
                ns->ns_AutoIpStack = NULL;
            }
        }
        else
        {
            AMI_WARN("netstack: could not restore AutoIP after interface "
                     "start failed");
        }
    }

    if (added)
    {
        LONG remove_rc = ami_ns_interface_remove_locked(index, TRUE);

        if (remove_rc != AMI_NET_OK)
            AMI_ERROR("netstack: rollback of interface %ld failed (%ld)",
                      (long)index, (long)remove_rc);
    }

    /*
     * After the removal above, which is what makes its slot free again; the slot
     * it goes back into is the same one, because ami_ns_yield_candidate() only
     * ever offers a slot that is now vacant.
     */
    ami_ns_restore_stood_down(&stood_down);

out:
    return rc;
}

LONG netstack_interface_start(const AmiIfConfig *cfg, UWORD *index_out)
{
    LONG rc;

    ami_ns_lock_init();
    ObtainSemaphore(&ami_ns_lock);
    rc = ami_ns_interface_start_locked(cfg, index_out, TRUE);
    ReleaseSemaphore(&ami_ns_lock);

    return rc;
}

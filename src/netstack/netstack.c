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

#include <exec/memory.h>
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

    ami_netstack_dns_stop(ns);

    if (ns->ns_AutoIpCreated)
    {
        (VOID)nx_auto_ip_stop(&ns->ns_AutoIp);
        (VOID)nx_auto_ip_delete(&ns->ns_AutoIp);
        ns->ns_AutoIpCreated = FALSE;
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
            AMI_ERROR("netstack: interface '%s' (%s unit %lu) would not open",
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

    status = nx_arp_enable(&ns->ns_Ip, ns->ns_ArpCache, (ULONG)AMI_ARP_CACHE_SIZE);
    if (status != NX_SUCCESS)
        AMI_WARN("netstack: nx_arp_enable failed (%ld)", (long)status);

    status = nx_tcp_enable(&ns->ns_Ip);
    if (status != NX_SUCCESS)
        AMI_WARN("netstack: nx_tcp_enable failed (%ld)", (long)status);

    status = nx_udp_enable(&ns->ns_Ip);
    if (status != NX_SUCCESS)
        AMI_WARN("netstack: nx_udp_enable failed (%ld)", (long)status);

    status = nx_icmp_enable(&ns->ns_Ip);
    if (status != NX_SUCCESS)
        AMI_WARN("netstack: nx_icmp_enable failed (%ld)", (long)status);

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

    if (ns->ns_AutoIpCreated)
        return;

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
        AMI_WARN("netstack: nx_auto_ip_start failed (%ld)", (long)status);
    else
        AMI_INFO("netstack: RFC 3927 link-local configuration started");
}

static LONG ami_ns_configure_addresses(AmiNetStack *ns)
{
    UINT  status;
    ULONG actual;
    UWORD i;
    BOOL  resolved = FALSE;

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
        status = nx_dhcp_create(&ns->ns_Dhcp, &ns->ns_Ip, (CHAR *)"amiga");
        if (status != NX_SUCCESS)
        {
            AMI_ERROR("netstack: nx_dhcp_create failed (%ld)", (long)status);
        }
        else
        {
            ns->ns_DhcpCreated = TRUE;

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
        /* Block until the first interface has an address, or DHCP gives up. */
        status = nx_ip_status_check(&ns->ns_Ip, NX_IP_ADDRESS_RESOLVED, &actual,
                                    AMI_DHCP_TIMEOUT_TICKS);
        if (status == NX_SUCCESS)
        {
            resolved = TRUE;
        }
        else if (ns->ns_DhcpStarted)
        {
            AMI_WARN("netstack: DHCP timed out after %lu ticks",
                     (unsigned long)AMI_DHCP_TIMEOUT_TICKS);

            /* RFC 3927: fall back to a link-local address. */
            ami_ns_start_autoip(ns);

            status = nx_ip_status_check(&ns->ns_Ip, NX_IP_ADDRESS_RESOLVED,
                                        &actual, AMI_DHCP_TIMEOUT_TICKS);
            resolved = (status == NX_SUCCESS) ? TRUE : FALSE;
        }
    }

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
        AMI_ERROR("netstack: no interfaces in DEVS:NetInterfaces");
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

    /* ---- 7. addresses --------------------------------------------------- */

    AMI_INFO("netstack: NX_IP up, configuring addresses");
    status = ami_ns_configure_addresses(ns);

    /* ---- 8. resolver ---------------------------------------------------- */

    AMI_INFO("netstack: starting the resolver");
    (VOID)ami_netstack_dns_start(ns);

    ami_netstack_leave(&caller);

    if (status != AMI_NET_OK)
    {
        /*
         * No address. The stack is otherwise healthy, so keep it up: a caller
         * may still want loopback, and Online/AddNetInterface can fix the
         * interface later. Report the failure so bsdsocket does not pretend.
         */
        AMI_WARN("netstack: up, but no interface has an address");
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

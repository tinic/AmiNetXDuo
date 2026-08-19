/*
 * AmiNetXDuo, the stack singleton.
 *
 * Startup order:
 *
 *   1. configuration, AmigaDOS file I/O, so it must happen on a Process
 *                         and before this task becomes a ThreadX thread.
 *   2. SANA-II opens, OpenDevice()/DoIO(), same reason.
 *   3. sizing, AvailMem() decides the packet pool.  The 1 MB floor
 *                         (docs/RESEARCH.md 81) means the NetX Duo defaults
 *                         are not usable.
 *   4. ThreadX, tx_amiga_kernel_start() returns once the scheduler
 *                         is live, unlike tx_kernel_enter().
 *   5. adoption, everything below suspends the calling thread inside
 *                         NetX Duo, and nx_dns_create() is threads-only.
 *   6. NetX Duo, pool, NX_IP, ARP/TCP/UDP/ICMP, extra interfaces.
 *   7. addresses, DHCP, or AutoIP, or the static configuration.  Block
 *                         until an interface has one or DHCP gives up.
 *   8. DNS, needs the resolver configuration and a running IP.
 *
 * SPDX-License-Identifier: MIT
 */

#include "netstack_internal.h"

/* NETSTATUS_DHCPRAW_*, which is published as NX_DHCP_STATE_* and is held to it
   by the _Static_asserts beside netstack_interface_dhcp_raw_state(). */
#include "aminetxduo/netstatus.h"

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
 * tx_kernel_enter() calls this before the scheduler starts. AmiNetXDuo builds
 * everything from netstack_startup() instead, on an adopted task, after the
 * kernel is live, because the SANA-II devices have to be opened by a Process
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

/* ------------------------------------------------- the "is it up?" barrier */

/*
 * The AMITCP public message port (docs/RESEARCH.md 3.3, 6.6 and 75.7).
 *
 * `WaitForPort AMITCP` in S:User-Startup is the conventional Amiga way to wait
 * until the network exists, and every stack since AmiTCP has created it. It is
 * also the only cross-program way for a Shell command to find out that a stack
 * is running: the singleton is private to whichever binary holds it, but the
 * port is visible to everyone.
 *
 * It is not only a flag. AMITCP is the ARexx host port of AmiTCP, and 31 of
 * the 2,149 archives surveyed in 75 send commands to it, the AmiTCP stopnet
 * and netstat among them. A port nobody services turns their clean "host
 * environment not found" into a hang, so netstack_rexx.c runs a process that
 * answers, and the port lives and dies with it.
 */
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

/*
 * The floor tier keeps the port and drops the host.  WaitForPort and a Shell
 * command looking for the stack want a public port of that name to exist, and
 * neither sends it anything.  A script that does gets no reply, which
 * CMakeLists.txt records as the cost of the option.
 *
 * The FindPort() under Forbid() comes from netstack_rexx.c: another stack
 * owning the name means this one must not be added, and the check and the
 * AddPort() are one atomic step, because two stacks coming up together
 * otherwise both pass it.
 */
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

    /* Anything that arrived is replied to rather than freed: the sender owns
       it and is waiting on its own reply port.  A RexxMsg keeps rm_Result1 at
       zero, which is the same "nothing happened" a script sees from a host
       that never ran a command. */
    while ((msg = GetMsg(ami_ns_bare_port)) != NULL)
        ReplyMsg(msg);

    DeleteMsgPort(ami_ns_bare_port);
    ami_ns_bare_port = NULL;
}

#endif /* AMINETXDUO_AREXX */

/* ----------------------------------------------------------- adoption glue */

LONG ami_netstack_enter(AmiNetCaller *caller)
{
    UINT status;

    caller->nc_Adopted = FALSE;

    if (tx_amiga_kernel_running() != TX_TRUE)
        return AMI_NET_ERR_STATE;

    /*
     * Already inside, an adopted task deeper in the call chain, or a thread
     * ThreadX created: then there is nothing to do.
     *
     * tx_thread_identify() cannot answer that here, and asking it was the
     * defect docs/RESEARCH.md 77.6 recorded as NX_CALLER_ERROR. It returns
     * _tx_thread_current_ptr, which on this port is the global baton holder and
     * not the caller. A second Task arriving while the first holds the baton
     * therefore reads "already a thread", skips the adoption and goes straight
     * into NetX Duo without ever acquiring the baton, two Tasks inside the
     * stack at once. Two processes sharing bsdsocket.library is all it takes.
     */
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
    /* On the way out of every stack operation, so the low-water mark in the
       health mark is as of the last thing the stack did.  The rest of the
       pool figures are read when asked.  See netstack_pool_mark_low(). */
    netstack_pool_mark_low();

    if (caller->nc_Adopted)
    {
        (VOID)tx_amiga_orphan_thread(&caller->nc_Thread);
        caller->nc_Adopted = FALSE;
    }
}

/* The same bracket with the TX_THREAD off the caller's stack, see the note
   in <aminetxduo/netstack.h>. */
AmiNetCaller *ami_netstack_enter_alloc(VOID)
{
    AmiNetCaller *caller = (AmiNetCaller *)AllocMem(sizeof(AmiNetCaller),
                                                    MEMF_PUBLIC | MEMF_CLEAR);

    if (caller == NULL)
        return NULL;

    /* Direct AllocMem(), so the census has to be told: this record belongs to
       a caller that can be a Task which exits without unwinding, which is the
       shape of every leak the census exists to name. */
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

/*
 * The same bracket, with the TX_THREAD kept between calls. Registration is the
 * expensive part and it is repeatable, so the same task gets the same thread
 * every time. The baton is still acquired and released per call, which leaves
 * the concurrency model unchanged.  See <aminetxduo/netstack.h> and
 * tx_amiga_adopt_resume() in the port.
 *
 * Every failure here falls back to the per-call path, so a stale or foreign
 * cache costs a bracket rather than producing a wrong answer.
 */
LONG ami_netstack_enter_cached(AmiNetCaller *caller)
{
    struct Task *me;
    UINT         status;

    caller->nc_Adopted = FALSE;

    if (tx_amiga_kernel_running() != TX_TRUE)
        return AMI_NET_ERR_STATE;

    /* Ours, not merely somebody's, see ami_netstack_enter(). */
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

        /*
         * The cached thread is no longer usable: torn down elsewhere, or in a
         * state resume does not take. Drop it and adopt afresh. Orphan rather
         * than discard, because this is the owning task and the Exec signal
         * can be recovered.  tx_amiga_orphan_thread() handles a TX_THREAD that
         * has already been deleted.
         */
        if (tx_amiga_orphan_thread(&caller->nc_Thread) != TX_SUCCESS)
            (VOID)tx_amiga_discard_thread(&caller->nc_Thread);
        caller->nc_Live = FALSE;
        caller->nc_Task = NULL;
    }

    /*
     * A cache belonging to another task. There is only one TX_THREAD slot, and
     * overwriting it deregisters a thread its owner is still using, so this
     * fails.
     *
     * A SocketBase used from a task other than the one that opened it
     * therefore reports ENETDOWN rather than working by accident. The
     * descriptor table, the event signal and the errno pointer all belong to
     * the opener.  src/bsdsocket/tcp_handler.c states the rule in its header
     * ("a SocketBase belongs to one task").
     */
    if (caller->nc_Live)
    {
        AMI_ERROR("netstack: bracket used from a second task");
        return AMI_NET_ERR_STATE;
    }

    /* Publish the owner before adoption. tx_amiga_adopt_thread() holds
       Forbid() while it creates the TX_THREAD, but a foreign RemTask() can run
       immediately after that Forbid() is released and before this function's
       next instruction. If nc_Live were set after the call, the dead-task
       sweep would see no registration to discard while ThreadX already had
       one. A failed adoption clears the provisional record below. */
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

        /* Suspending failed, so the thread is in an unknown state. Orphaning
           releases the baton and removes the registration with it. */
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
     * Inside a bracket the thread holds the baton, so release it the ordinary
     * way first. Release is a teardown call and must not be reached from
     * inside a bracket, and a half-released base is worse than this.
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
        /* The owner: resume so the signal can be freed by the task that
           allocated it, then orphan properly. */
        if (tx_amiga_adopt_resume(&caller->nc_Thread) == TX_SUCCESS)
            (VOID)tx_amiga_orphan_thread(&caller->nc_Thread);
        else
            (VOID)tx_amiga_discard_thread(&caller->nc_Thread);
    }
    else
    {
        /* Another task is tearing this down, normally the heartbeat after the
           owner was removed by Exec. It may have died while the baton was
           released around an Exec Wait(), in which case acquire() can never
           clear the slot. Remove that identity before deleting the ThreadX
           registration. The signal bit belongs to the old task and must not
           be freed from here; discard deliberately leaves it alone. */
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

/* ------------------------------------------------------------ pool sizing */

/*
 * NetX Duo lays a pool out as N * (payload rounded up to NX_PACKET_ALIGNMENT
 * + sizeof(NX_PACKET)), and rejects a block that is not big enough for one
 * packet. Rather than reproduce the arithmetic exactly, budget generously per
 * packet and let nx_packet_pool_create() report what it made.
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

/* --------------------------------------------------------------- heartbeat */

/*
 * One expiration a second, for housekeeping nobody asks for.
 *
 * It exists because bsdsocket.library needs to notice that a task has exited,
 * and AmigaOS never says so: the only way to know is to look, and the one
 * place that must not look is the per-packet path.  A second here is the
 * cheapest clock the stack already has, so the answer is computed off the
 * packet path and both Signal() sites in the library keep the NULL test they
 * already had.
 *
 * TX_TIMER_PROCESS_IN_ISR is defined for this port, so this runs on the tick
 * task inside a Forbid() and counts as interrupt level to both ThreadX and
 * NetX Duo.  Nothing here must block or call NetX Duo.  The contract is spelled
 * out over ami_set_second_hook() in include/aminetxduo/compat.h, and the whole
 * body is the notify, so that this file cannot grow work that breaks it.
 */
static VOID ami_ns_second_expired(ULONG id)
{
    (VOID)id;
    ami_second_notify();
}

/* ---------------------------------------------------------------- teardown */

static VOID ami_ns_destroy(AmiNetStack *ns)
{
    UWORD i;
    BOOL  requests_retained = FALSE;

    if (ns == NULL)
        return;

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

    /*
     * The heartbeat first: it reaches into the child-base list of
     * bsdsocket.library, and everything below here is the stack coming apart
     * underneath it.  Deactivate before delete, so an expiration already on
     * the wheel of the tick task cannot be dispatched into a deleted timer.
     */
    if (ns->ns_SecondCreated)
    {
        ns->ns_SecondCreated = FALSE;
        (VOID)tx_timer_deactivate(&ns->ns_Second);
        (VOID)tx_timer_delete(&ns->ns_Second);
    }

    /*
     * Stop the callback posting before the semaphore goes away.
     * ami_ns_address_changed() runs on the IP thread, which is still running
     * here, and addresses do change during teardown as interfaces lose theirs.
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

    /* DHCP's state callback imports option 6 into the DNS client. Stop and
       delete the callback source before deleting the object it updates. */
    ami_netstack_dns_stop(ns);

#ifdef AMINETXDUO_IPV6
    /*
     * The DHCPv6 Release goes on the wire, so it happens here rather than in
     * the teardown below: nx_ip_delete() takes the interfaces down with it and
     * a Release sent after that reaches nobody. The server keeps the address
     * bound for the whole valid lifetime when this is skipped.
     */
    ami_netstack_dhcpv6_release(ns);
    ami_netstack_dhcpv6_destroy(ns);
#endif

    if (ns->ns_IpCreated)
    {
        (VOID)nx_ip_delete(&ns->ns_Ip);
        ns->ns_IpCreated = FALSE;
    }

    /* The NX_IP teardown above has already taken the interfaces offline and
       stopped the reader threads.  Closing the devices is all that is left. */
    for (i = 0; i < AMI_CFG_MAX_INTERFACES; i++)
    {
        if (ns->ns_Iface[i] != NULL)
        {
            if (ami_sana2_close(ns->ns_Iface[i]))
            {
                ns->ns_Iface[i] = NULL;
            }
            else
            {
                requests_retained = TRUE;
            }
        }
    }
    ns->ns_IfaceCount = 0;

    /*
     * An orphaned SANA-II request reaches farther than AmiSana2If. RX slots
     * hold NX_PACKETs and destination pointers inside ns_PoolMemory; TX slots
     * hold packet chains there too. A reader that has not quite exited can
     * also call nx_packet_release(), which follows the packet's pool owner
     * back to ns_Pool. Deleting the pool and freeing either allocation after
     * ami_sana2_close() retained the interface turns its deliberate leak into
     * a use-after-free on the next device completion.
     *
     * This is already an unrecoverable driver failure: the device owns memory
     * it refused to return, and the kernel cannot safely unload while its
     * reader may remain. Retain the complete stack allocation set with the
     * interface. A bounded leak on shutdown is the only safe result; freeing
     * any subset requires proving which point the stuck request or thread has
     * reached, which the device gives us no way to do.
     */
    if (requests_retained)
    {
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

        ns->ns_IfaceMdns[opened] = cfg->mdns;
        ns->ns_Iface[opened] = ami_sana2_open(cfg, &status);
        if (ns->ns_Iface[opened] == NULL)
        {
            /* The serial log is also what a user is asked to send in, so it
               states what to do as well as what happened. The console version,
               with a device probe behind it, is in src/tools/tool_diag.c. */
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
         * Move the configuration down with its slot.  ns_Iface[k] and
         * interfaces[k] are one interface everywhere else: ns_DhcpState[],
         * ns_LastAddress[], the attach loops, the cfg0 in ami_ns_create_ip().
         * The two indices diverge the moment a device fails to open, and then
         * the machine comes up on the surviving card with the address of the
         * failed one, with no DHCP and no gateway.
         */
        if (opened != i)
            ns->ns_Config.interfaces[opened] = *cfg;

        /* The configuration moved with the device, so every mapping must
           name its compacted slot.  Keeping `i` here points at the stale
           source entry that the cleanup below marks unconfigured; lookups
           and interface claims then lose this otherwise live device. */
        ns->ns_IfaceCfg[opened] = opened;

        opened++;
    }

    /* Interfaces that did not open are not interfaces.  Leaving them counted
       shows a card that is not there, and leaves a stale duplicate of a moved
       entry behind the last live one. */
    for (i = opened; i < ns->ns_Config.interface_count; i++)
        ns->ns_Config.interfaces[i].configured = FALSE;
    ns->ns_Config.interface_count = opened;

    ns->ns_IfaceCount = opened;

    return (opened > 0) ? AMI_NET_OK : err;
}

/*
 * Nothing named this machine, so name it after its card.
 *
 * Runs once, after the devices are open (ami_sana2_get_mac() has an address
 * only then) and before anything reads ns_Config.hostname: option 12, the
 * mDNS label, gethostname(), ShowNetStatus and the ARexx HOSTNAME variable
 * all take it from there, so filling it here is what keeps them saying the
 * same thing.
 *
 * hostname_source stays AMI_HOSTNAME_NONE. This is not a fifth source and does
 * not outrank one: it fires only when the four found nothing, and NONE is
 * below every real source, so a DHCP option 12 or an ENV:HOSTNAME arriving
 * later still replaces it through ami_config_hostname_offer(). What a user
 * configured is untouched, and the report keeps saying "derived".
 *
 * The first interface with an address, in the same order ID= is taken in, so
 * a two-card machine picks the same one every boot.
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

    /*
     * No card gave an address, so "amiga", and the collision this exists to
     * avoid is back. That is still better than a name that differs after every
     * reboot.
     *
     * Written into ns_Config rather than left to the fallback each caller
     * carries, because those cover only two of them: with the field empty,
     * mDNS claims amiga.local and option 12 announces "amiga" while
     * gethostname() takes its own path, the interface address reverse-resolved
     * or "localhost", and ShowNetStatus then reports "Host name: localhost"
     * over "Known here as: amiga.local". The machine then calls itself one
     * thing to the network and another to a program.
     */
    ami_ns_copy_name(ns->ns_Config.hostname, "amiga",
                     (ULONG)sizeof(ns->ns_Config.hostname));

    AMI_WARN("netstack: nothing named this machine, and no card gave a "
             "hardware address. It answers to \"amiga\". Another machine "
             "in the same state has the same name. Set a name with "
             "`hostname <name>`");
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
     * Start the IP identification field at an unpredictable value.
     *
     * nx_ip_create() zeroes nx_ip_packet_id and nx_ip_header_add.c increments
     * it once per transmitted datagram, so without this the field is a global,
     * monotonic, boot-zeroed counter: a fingerprint, and a whole-machine packet
     * counter readable from any one flow.
     *
     * One DRBG draw moves the starting point. This is not
     * NX_ENABLE_IP_ID_RANDOMIZATION, which redraws per packet and costs 5% of
     * loopback throughput on this machine (nx_user.h, docs/RESEARCH.md 33.4).
     * It does not defeat the RFC 6274 idle scan, which reads the delta between
     * two observations rather than the value.  The note in nx_user.h says so.
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

    /*
     * Inbound reassembly, both families. Until this call the three fragment
     * function pointers are NULL, and a fragment arriving is counted and then
     * released: nx_ipv4_packet_receive.c tests nx_ip_fragment_assembly before
     * queueing, and _nx_ipv6_process_fragment_option() returns
     * NX_OPTION_HEADER_ERROR straight away. So a UDP answer larger than the
     * path MTU never arrived and the application saw a timeout with nothing
     * logged.
     *
     * It enables transmit fragmentation in the same call, because there is no
     * receive-only arm, and nothing reaches it from here: bsd_send_udp() and
     * the raw send both refuse an oversize datagram with EMSGSIZE before the
     * stack sees it, and TCP sizes to the MSS. The senders inside the stack
     * are all under one MTU.
     *
     * What reassembly holds is bounded in the fork: a fragment is refused
     * once the pool is half consumed, and the IPv4 hold time no longer
     * follows the sender's TTL.
     */
    status = nx_ip_fragment_enable(&ns->ns_Ip);
    if (status != NX_SUCCESS)
        AMI_WARN("netstack: nx_ip_fragment_enable failed (%ld)", (long)status);

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

    /*
     * Both protocol enables below run after every interface is attached,
     * because each walks the interface table once and does nothing for a slot
     * that is not valid yet.
     */

#ifdef AMINETXDUO_MULTICAST
    /*
     * RFC 1112 group membership. Without this call every _nxe_igmp_multicast_
     * *_join() returns NX_NOT_ENABLED, so setsockopt(IP_ADD_MEMBERSHIP) has
     * nothing to do, and the option surface in src/bsdsocket/mcast.c is
     * useless on its own.
     *
     * It also starts the periodic that answers a membership query from a
     * router.  Without an answer a querying switch stops forwarding the group
     * after its own timeout, so joining and never reporting works only until
     * the first query.
     *
     * Called from here rather than before the loop because it does not do the
     * work itself: it sets NX_IP_IGMP_ENABLE_EVENT and the IP thread registers
     * the all-hosts MAC filter for whichever interfaces are valid by the time
     * it wakes. Ahead of the loop that is a race the second interface loses
     * whenever the IP thread runs first.
     */
    status = nx_igmp_enable(&ns->ns_Ip);
    if (status != NX_SUCCESS)
        AMI_WARN("netstack: nx_igmp_enable failed (%ld)", (long)status);
#endif

#ifdef AMINETXDUO_IPV6
    /*
     * Multicast Listener Discovery, ahead of nxd_ipv6_enable() and not after
     * it: what MLD announces is a join, and the first joins this stack makes
     * are the ones nxd_ipv6_enable() and the address configuration below it
     * make themselves. A join taken before this call is in the driver's
     * address filter and not in MLD's table, so it is never announced and no
     * later query finds it.
     *
     * ff02::1 is the exception either way -- RFC 9777 section 6 excepts it,
     * every node is a member by definition -- so ordering only decides
     * whether the solicited-node groups are reported, which is the whole
     * point of the feature.
     */
    status = nx_mld_enable(&ns->ns_Ip);
    if (status != NX_SUCCESS)
        AMI_WARN("netstack: nx_mld_enable failed (%ld)", (long)status);

    /*
     * nxd_icmp_enable() covers ICMPv4 as well, so it replaces the
     * nx_icmp_enable() below rather than adding to it.
     *
     * nxd_ipv6_enable() seeds the router solicitation counter and interval
     * and joins ff02::1 for every interface that is valid when it runs, and
     * nx_ip_interface_attach() joins ff02::1 but seeds nothing. Before the
     * loop, only interface 0 ever solicited a router.
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
     * STATE=down in DEVS:NetInterfaces, honoured last because attaching is
     * what brings an interface up: nx_ip_interface_attach() drives NX_LINK_
     * ENABLE itself, so the only way to arrive down is to be taken down after.
     * AMI_LINK_STACK_DISABLE and not NX_LINK_DISABLE, to match SM_Down,
     * the device stays open and Online brings it back.
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

/*
 * Is any interface on this machine expecting an IPv4 address at all.
 *
 * FALSE is the IPv6-only machine, and it is the answer three separate pieces
 * of bring-up needed and none of them asked. Without it the boot waited 30
 * seconds for a DHCP lease nobody had asked for, then forced an RFC 3927
 * 169.254 address onto an interface configured for IPv6, waited 15 seconds
 * more for that, and finally reported AMI_NET_ERR_CONFIG -- which makes
 * OpenLibrary("bsdsocket.library") return NULL, so the machine had no network
 * of either family.
 */
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

/* Pick the requested interface, or the configured AutoIP default when the
   caller has no per-interface reason for starting it. */
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
        /* First interface explicitly asking for link-local; failing that, the
           first one expecting any IPv4 address (the startup fallback case). */
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

    /* AutoIP cannot run without an IPv4-capable interface. An IPv6-only stack
       never calls this helper; treat reaching it as a configuration failure. */
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
     * Already built. nx_auto_ip_stop() only suspends the module's thread, so
     * coming back is a start rather than a create, and nx_auto_ip_start() with
     * a starting address of zero makes it draw a fresh candidate instead of
     * re-probing the previous one.
     *
     * Guarded on ns_AutoIpRunning because a start while it is already running
     * resets its conflict count and discards the address it is holding.
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

/* --------------------------------------------------- address notifications,
 *
 * The NetX Duo DHCP client and its AutoIP module both change the interface
 * address from their own threads and announce nothing unless something
 * registers for it. Without these callbacks a lost lease -- address removed,
 * gateway cleared, every socket dead -- goes unreported.
 *
 * Both run on a NetX Duo thread. AMI_INFO()/AMI_WARN() are RawPutChar() and
 * RawDoFmt(), which are Exec-only and legal from any Task, and nothing here
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
 * Called by NetX Duo whenever the address or mask of any interface changes:
 * from the DHCP thread on a lease, from the AutoIP thread on a link-local
 * claim, and from ami_ns_configure_addresses() for a static one.
 */
/*
 * Another host answered an ARP for an address of this machine.
 *
 * NetX Duo already defends the address, and it told nobody, because
 * nx_interface_ip_conflict_notify_handler was registered nowhere.  A duplicate
 * address is the fault where every counter looks healthy and the network still
 * does not work: both hosts answer, and whichever is first wins each exchange.
 * So it is counted as well as logged, because the log is off in a shipping
 * build and the count outlives it.
 *
 * Runs from _nx_arp_packet_receive() on the IP thread.  Nothing here allocates
 * or calls back into NetX Duo.
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
         * Announce it.  RFC 5227 2.3: a host that has just configured an
         * address broadcasts an ARP for it, so every neighbour cache is
         * corrected at once rather than after its own entry ages out, which
         * is what made a DHCP address that moved between machines take
         * minutes to become reachable.
         *
         * Safe here: _nx_ip_interface_address_set() releases the protection
         * mutex before calling this notify, so re-taking it inside
         * nx_arp_gratuitous_send() is not a recursion.  Failure is not worth a
         * branch: the address works either way, and the neighbours are slower
         * to learn it.
         */
        if (addr != 0UL)
            (VOID)nx_arp_gratuitous_send(&ns->ns_Ip, NX_NULL);

        /* Wake anyone waiting for an address. tx_semaphore_put() does not
           block, which matters because this runs on the IP thread. */
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
         * RFC 3927 1.9: a routable address supersedes a link-local one on the
         * same interface. The AutoIP thread does not watch for this itself: it
         * sits in an indefinite wait for a conflict. So it is stopped here,
         * and can be restarted if the lease is later lost. A routable address
         * on another card must not suspend the one machine-wide AutoIP object
         * while it is managing this interface.
         *
         * Never from the AutoIP thread itself: nx_auto_ip_stop() is
         * tx_thread_suspend(), and calling it on the running thread suspends
         * it in the middle of its own announcement.
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
     * Last, and once for the whole sweep rather than once per interface: a
     * program asked whether the addressing of the machine changed, and not
     * which slot moved.  The hook signals tasks and takes a semaphore held for
     * two list operations.  It must not come back into NetX Duo, because this
     * is the IP thread and it is inside a notification NetX Duo made.
     */
    if (changed)
        ami_address_change_notify();
}

/* NX_DHCP_STATE_* in order, for the bring-up marks. */
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

/*
 * Called by the NetX Duo DHCP client on every state transition, per interface.
 *
 * The transition of interest is into INIT from a state that held an address: a
 * NAK, or a lease that ran out with nothing answering, after which
 * _nx_dhcp_interface_reinitialize() has taken the address and the gateway off
 * the interface. The response is RFC 3927 1.7, as when DHCP never answers.
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

    /* Which state, and when. A lease that takes seconds rather than the four
       packets it is made of is a retransmission, and this is what says so. */
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
        /*
         * Only from a state that held a lease. A NAK in REQUESTING is an
         * ordinary part of acquiring one, and reporting a first boot as a lost
         * lease is a false alarm.
         */
        if (previous >= (UBYTE)NX_DHCP_STATE_BOUND)
        {
            AMI_WARN("netstack: interface %ld has LOST its DHCP lease. The "
                     "address and the gateway are off it. Every open "
                     "connection through it is dead",
                     (long)iface_index);

            /* The lease owns no resolver entries after this transition.  As
               on BOUND, a caller task performs the actual reconciliation. */
            ami_netstack_dns_dhcp_changed(ns, (UWORD)iface_index);

            /* RFC 3927 1.7: keep the machine reachable on the local wire
               while the DHCP client tries again. */
            if (ami_ns_start_autoip(ns, (LONG)iface_index) != AMI_NET_OK)
                AMI_WARN("netstack: no link-local fallback on this machine");
        }
        break;

    default:
        break;
    }
}

/*
 * Wait for any configured interface to have an address.
 *
 * nx_ip_status_check() is nx_ip_interface_status_check() on interface 0 (see
 * the bottom of nx_ip_status_check.c), so a wait built on it cannot see an
 * address arrive on interface 1: a machine with a static interface 0 and a
 * DHCP interface 1 waited out the full timeout and then reported that nothing
 * had an address.
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
         * Wait out the whole remaining timeout on the notification, not a
         * slice of it. NetX Duo reports an arriving address through the
         * registered ami_ns_address_changed(), so a shorter wait finds nothing
         * sooner.  Capping it at the poll interval instead woke this thread
         * fifty times a second to re-walk every interface through
         * nx_ip_interface_address_get(), taking the IP protection mutex each
         * time and contending with the IP and DHCP threads it is waiting for.
         * A thirty-second wait for a server that never answers was 1,500 of
         * those passes and is now one.
         *
         * No wake is lost: ns_AddrArrived counts, so a post landing between
         * the scan above and the get below is held and returns the get at
         * once. A post for something other than an address arriving (a
         * different interface, an address going away) returns it too, which is
         * why the scan is at the top of the loop rather than after the wait,
         * and the elapsed time rather than the slice is what bounds the loop.
         */
        if (ns->ns_AddrArrivedReady)
        {
            UINT got = tx_semaphore_get(&ns->ns_AddrArrived, remaining);

            /*
             * TX_SUCCESS means something changed, so go and look. TX_NO_INSTANCE
             * is the timeout, and the scan at the top runs once more before the
             * elapsed check ends it. Anything else means the semaphore is gone,
             * which only teardown does and it cannot run while startup holds
             * ami_ns_lock, and a return that consumes no time spins.
             */
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
 * Send the first DISCOVER now rather than a second from now.
 *
 * RFC 2131 4.4.1 asks a client to wait a random one to ten seconds before its
 * first DHCPDISCOVER, so that a room of machines returning together after a
 * power cut does not answer as one. NetX Duo takes the bottom of that range,
 * writes NX_IP_PERIODIC_RATE into the timeout of the interface record, and
 * lets its own one-second timer expire before anything is sent: a flat second
 * of nothing on every boot, and the largest single item in AddNetInterface,
 * 1,201 ms of DHCP in a 1,980 ms command, of which 1,000 ms was this wait and
 * ~200 ms the four packets. The case it guards against is a room of diskless
 * workstations on one power circuit.  An Amiga is switched on by hand.
 *
 * The timer gates the first send, so re-arming it to expire on the next tick
 * is the whole change: the expiry that follows finds the same
 * NX_IP_PERIODIC_RATE timeout already at or below one interval and runs the
 * INIT case that it ran a second later before. Every interval after it, the
 * retransmission backoff, the `secs` field the INIT case zeroes, the renewal
 * times, is left as NetX Duo set it, because the reschedule period handed
 * back here is the one it created the timer with.
 */
static VOID ami_ns_dhcp_discover_now(NX_DHCP *dhcp)
{
    (VOID)tx_timer_deactivate(&dhcp->nx_dhcp_timer);
    (VOID)tx_timer_change(&dhcp->nx_dhcp_timer, 1UL,
                          (ULONG)NX_DHCP_TIME_INTERVAL);
    (VOID)tx_timer_activate(&dhcp->nx_dhcp_timer);
}

/*
 * DHCP option 61, the client identifier.
 *
 * NetX Duo names the option and its size (nxd_dhcp_client.h) and then never
 * sends it.  The user-option callback is the only way in. Roadshow sends it on
 * every DISCOVER and REQUEST, and a router that keys its lease table on the
 * identifier rather than on chaddr therefore treats the two stacks as two
 * different clients: measured against a UniFi gateway on 2026-08-01, one A1200
 * with a2065 MAC 00:80:10:49:00:07 was handed 192.168.1.137 under Roadshow and
 * 192.168.1.118 under this stack, in back-to-back boots. So a machine changes
 * address when it changes stack, and any reservation the user made stops
 * applying.
 *
 * The form is the RFC 2132 section 9.14 hardware type plus hardware address:
 * 0x01 for Ethernet then the six MAC bytes, which is what identifies the same
 * machine to a server that also saw Roadshow, AmigaOS 4, or a PC on that NIC.
 *
 * Returning anything but NX_TRUE makes NetX Duo drop the whole message
 * (nxd_dhcp_client.c, _nx_dhcp_send_request_internal), so a short buffer costs
 * the option, never the DHCP exchange.
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

/*
 * Everything a fresh NX_DHCP needs, wherever it was created.
 *
 * Start-up and netstack_interface_dhcp_start() both create the one client there
 * can be, and the start-up one used to get only the state-change callback: the
 * parameter request list stayed at the NetX Duo default of mask/gateway/DNS, so
 * a machine configured for DHCP in DEVS:NetInterfaces came up with no domain
 * name and no static routes while the same machine configured by hand did. A
 * conforming server sends what the list asks for and nothing else.
 */
static VOID ami_ns_dhcp_configure(AmiNetStack *ns)
{
    (VOID)nx_dhcp_interface_state_change_notify(&ns->ns_Dhcp,
                                                ami_ns_dhcp_state_changed);

    (VOID)nx_dhcp_user_option_add_callback_set(&ns->ns_Dhcp,
                                               ami_ns_dhcp_client_id);

    /*
     * Ask the server for the options an application can be given back. Without
     * this they are not in the parameter request list and a conforming server
     * has no reason to send any, so the tables come back empty.
     */
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
     * Create the semaphore before registering the callback that posts it: the
     * notification can fire the moment a static interface is addressed below,
     * and ns_AddrArrivedReady stops the callback touching a semaphore that
     * does not exist yet. Both happen before anything can change an address,
     * so the first one is announced too.
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
     * The conflict handler is per interface and there is no API that sets it,
     * so it is assigned the way the NetX Duo AutoIP module assigns it: into
     * the NX_INTERFACE.  Every interface, because a duplicate address on the
     * second card is as invisible as one on the first.
     */
    for (i = 0; i < ns->ns_IfaceCount; i++)
        ns->ns_Ip.nx_ip_interface[i].nx_interface_ip_conflict_notify_handler =
            ami_ns_ip_conflict;

    /* Static interfaces are already addressed by nx_ip_create()/attach.  Check
       that a static interface 0 has what the configuration asked for. */
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
         * The name here is DHCP option 12, the host name the client announces:
         * what the client list of the router shows and what many routers put
         * in their local DNS. It comes from the configured HOSTNAME so that two
         * AmiNetXDuo machines on one network are distinguishable, and failing
         * that from ami_ns_name_after_card(), which leaves it non-empty
         * either way. The literal guards nx_dhcp_create() against a
         * zero-length name, which it has no defence of its own against.
         *
         * Into ns_DhcpName, whose comment says why the client gets a copy
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

            /* Before the client starts, so the first BOUND is reported as well
               as everything after it, and so the first DISCOVER already carries
               the client identifier and the full parameter request list. */
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
     * Before the wait below, not after it, and this is worth several seconds.
     *
     * Nothing here needs an IPv4 address: it configures the link-local
     * address, arms the router solicitation and returns, and everything that
     * takes time -- duplicate address detection, the solicitation, the address
     * an advertisement brings and the duplicate address detection for that --
     * runs afterwards on the IP thread. Run after the wait, all of that started
     * only once the lease had landed, so a boot paid for the DHCP exchange and
     * the IPv6 tail one after the other.  Run here, the two overlap and
     * bring-up costs the longer of them.
     *
     * `resolved` is about IPv4 and gates the AMI_NET_ERR_CONFIG return, so it
     * is not touched: a machine with IPv6 and no IPv4 address still reports
     * that no interface has an address, which is what every IPv4 caller finds.
     * ami_ns6_address_changed() reports each IPv6 address as it lands, and
     * netstack_ipv6_address_get() reports what has arrived so far.
     */
    ami_netstack_ipv6_configure(ns);

    /*
     * And DHCPv6, after the addresses because the client needs the interface's
     * link-local address as a source. It returns at once unless some interface
     * asked for CONFIGURE6=DHCP or AUTO, and under AUTO it sends nothing until
     * a router advertisement asks it to, so a machine that does not use DHCPv6
     * pays a thread create here and no packets.
     */
    ami_netstack_dhcpv6_configure(ns);
#endif

    /*
     * The IPv6-only machine returns here, and this is the whole of what it
     * needed: nothing below waits for, falls back to, or reports an IPv4
     * address, because none was ever asked for.
     *
     * `resolved` is what gates the AMI_NET_ERR_CONFIG return, and
     * AMI_NET_ERR_CONFIG is what makes bsd_lib_open() answer NULL. Saying
     * "resolved" about a machine with no IPv4 address reads wrong and is
     * right: the question the flag answers is "did the configuration get what
     * it asked for", and an interface that asked for no IPv4 has.
     *
     * It does not wait for an IPv6 address either. Bring-up has never waited
     * for one -- the link-local is TENTATIVE for a second of duplicate address
     * detection and a SLAAC or DHCPv6 global arrives later still, and the
     * comment above ami_netstack_ipv6_configure() is about exactly that
     * overlap being worth several seconds. ami_ns6_address_changed() reports
     * each address as it lands and netstack_ipv6_address_get() reports what
     * has arrived so far.
     */
    if (!resolved && !ami_ns_wants_ipv4(ns))
    {
        AMI_INFO("netstack: no interface expects an IPv4 address, not waiting "
                 "for one");
        resolved = TRUE;
    }

    if (!resolved)
    {
        /* Block until some interface has an address, or DHCP gives up. */
        resolved = ami_ns_wait_for_address(ns, AMI_DHCP_TIMEOUT_TICKS);

        if (!resolved && ns->ns_DhcpStarted)
        {
            AMI_WARN("netstack: no DHCP server answered in %lu seconds",
                     (unsigned long)(AMI_DHCP_TIMEOUT_TICKS /
                                     (ULONG)NX_IP_PERIODIC_RATE));

            /* RFC 3927: fall back to a link-local address.  If that could
               not be started there is nothing on the way, so do not spend the
               fifteen seconds waiting for an address that cannot arrive. */
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

        /* An IPv6-only machine printed "address 0.0.0.0 mask 0.0.0.0" here,
           which reads as a fault and is not one.  The IPv6 addresses are
           reported one at a time by ami_ns6_address_changed(), as they pass
           duplicate address detection. */
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
     * Every interface configured down is not a failure.
     *
     * STATE=down says "bring the stack up, leave this card alone", and a card
     * that is down cannot be given an address, so waiting for one and then
     * calling its absence a configuration error refuses the exact thing that
     * was asked for. bsd_lib_open() turns that into a NULL, so OpenLibrary()
     * fails and nothing on the machine can open bsdsocket.library: no Online
     * to bring the interface up with, and no ShowNetStatus to see it.
     *
     * A stack with no address is a working stack that has no address. It is
     * the same state a machine reaches when DHCP does not answer, which
     * already returns OK for this reason.  Here it was deliberate.
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

/* ------------------------------------------------------------------ bring-up */

/*
 * Called with ami_ns_lock held. A failed stop is not merely a failed cleanup:
 * one of the port's Exec Tasks may still be running on code or data in this
 * hunk. Keep that fact separate from ami_ns, which has already been destroyed
 * by then, so the library can refuse expunge and a later call can retry.
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

    /* Every TX_THREAD the slot table still names died with the kernel, and
       the table outlives the stack. Only on success: on anything else a
       thread can still be inside a bracket. */
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

    /* ---- 1. configuration (AmigaDOS, must precede adoption) ------------- */

    if (ami_config_load(&ns->ns_Config) != AMI_CFG_OK)
    {
        ami_free(ns);
        return AMI_NET_ERR_CONFIG;
    }

    if (ns->ns_Config.interface_count == 0)
    {
        AMI_ERROR("netstack: nothing to bring up, DEVS:NetInterfaces holds "
                  "no usable interface file. Run NetSetup to write one, or "
                  "ShowNetStatus to see what is wrong with the one there");
        ami_free(ns);
        return AMI_NET_ERR_CONFIG;
    }

    /* ---- 2. SANA-II devices (OpenDevice/DoIO, also before adoption) ----- */

    status = ami_ns_open_devices(ns);
    if (status != AMI_NET_OK)
    {
        ami_ns_destroy(ns);
        return status;
    }

    /* The cards are open, so their addresses can be read.  Every consumer of
       ns_Config.hostname is still ahead of this point. */
    ami_ns_name_after_card(ns);

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
     * Install the baton release/acquire hooks first.  Without them the first
     * CMD_READ freezes the whole kernel (netstack_baton.c).
     */
    ami_sana2_set_block_hooks(ami_netstack_baton_release,
                              ami_netstack_baton_acquire);

    /* ---- 4. ThreadX ----------------------------------------------------- */

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

    /* ---- 5. become a ThreadX thread ------------------------------------- */

    AMI_INFO("netstack: kernel up, adopting this task");
    status = ami_netstack_enter(&caller);
    if (status != AMI_NET_OK)
    {
        ami_ns_destroy(ns);
        (VOID)ami_ns_kernel_stop_locked();
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
        (VOID)ami_ns_kernel_stop_locked();
        return status;
    }

#ifdef AMINETXDUO_BPF
    /* Capture goes up with the interfaces and before any address is
       configured, so DHCP, ARP and IPv6 neighbour discovery are inside the
       trace rather than in front of it. */
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
     * After the addresses, because the record this machine announces is its
     * address.  Starting first claims a name resolving to 0.0.0.0 until the
     * lease arrives. The module watches for later changes, so this affects
     * only the first announcement.
     *
     * After the resolver, because netstack_resolve() sends .local here and
     * everything else to the DNS client. Bringing up half the lookup path
     * first leaves a window in which a .local name goes to the unicast
     * servers, which RFC 6762 6.7 forbids.
     *
     * Failure is not fatal and is not waited for. Probing takes about a second
     * (three probes, 250 ms apart) on the thread of the module.  Blocking
     * startup on it adds that second to every boot for a result no caller of
     * netstack_startup() acts on.
     */
    AMI_INFO("netstack: starting mDNS");
    (VOID)ami_netstack_mdns_start(ns);
#endif

    /*
     * The heartbeat, last thing inside the bracket: ThreadX exists from step
     * 4 and the timer must be created before the caller stops being a TX
     * thread.  Not fatal if it cannot be made.  The stack works without a
     * heartbeat, and stops noticing tasks that exited without closing the
     * library, so the failure is logged.  Nothing outside can see that state.
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

    /* The stack exists from here on, address or not, so this releases anything
       waiting for `WaitForPort AMITCP`. */
    ami_ns_port_create();
    ami_netstack_baton_set_sampler(netstack_pool_mark_low);
    /* Once here, so the mark has the pool from the moment it is published
       rather than from whenever the stack is next used. */
    netstack_pool_sample();
    ami_netstack_health_publish();
#ifdef AMINETXDUO_AREXX
    ami_sana2_set_open_hooks(ami_netstack_rexx_suspend,
                             ami_netstack_rexx_resume);
#endif

    if (status != AMI_NET_OK)
    {
        /*
         * No address. The stack is otherwise healthy, so keep it up: a caller
         * can still want loopback, and Online/AddNetInterface can fix the
         * interface later. Report the failure, so bsdsocket does not claim
         * otherwise.
         */
        AMI_WARN("netstack: up, but no interface has an address, check the "
                 "cable, or that something on this network hands out addresses");
        return status;
    }

    ns->ns_Refs = 1;

    return AMI_NET_OK;
}

/* --------------------------------------------------------------- the API, */

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

    /* A previous stop can have refused while an application thread was still
       alive, or timed out after the port began stopping. Never start a second
       kernel on top of it. A retry is useful for the refusal case: the thread
       may have exited since the last close. */
    status = ami_ns_kernel_stop_locked();
    if (status != AMI_NET_OK)
    {
        ReleaseSemaphore(&ami_ns_lock);
        return status;
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

    /* Take the barrier down before the stack behind it goes. The hooks point
       at the port that is about to be freed, so they go first. */
    ami_sana2_set_open_hooks(NULL, NULL);
    ami_ns_port_delete();
    ami_netstack_baton_set_sampler(NULL);
    /* The next stack gets its own low-water mark, not this one's. */
    ami_mem_stats()->ms_PoolTotal = 0UL;
    ami_netstack_health_unpublish();

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
     * Stop ThreadX last. The caller is a plain Exec Task at this point
     * (ami_netstack_leave() orphaned it), which is what
     * tx_amiga_kernel_stop() documents for its caller.
     *
     * Only TX_SUCCESS means it is safe for this program to exit or for the
     * library to be expunged: the tick and scheduler Tasks run on stacks in
     * this hunk, so leaving them alive past an unload means they execute freed
     * memory. On anything else the caller must stay resident.  Stop refuses,
     * and leaves the kernel usable, if any application thread or a live zombie
     * remains.
     *
     * This can block for up to ~5 s in the worst case, with ami_ns_lock held.
     */
    (VOID)ami_ns_kernel_stop_locked();

    ReleaseSemaphore(&ami_ns_lock);
}

BOOL netstack_can_unload(VOID)
{
    BOOL safe;

    ami_ns_lock_init();

    /*
     * Attempt, not Obtain. bsd_lib_expunge() is one caller and Exec runs it
     * under Forbid(); blocking here would Wait() and break the Forbid that
     * protects the library list. Somebody else holding this lock is also, on
     * its own, a reason to answer no: netstack_shutdown() can hold it for
     * seconds around tx_amiga_kernel_stop(), which is precisely the window in
     * which the answer is not yet known. A contended lock therefore means
     * "cannot prove it is safe", which is the direction to fail in.
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
 * blocks on the stack it is describing is useless on the machine it exists
 * for.  The same reasoning covers the tick and baton counters the health mark
 * already points at.
 */
/*
 * The half that every stack operation has to pay for.
 *
 * ms_PoolLow is a running minimum and cannot be reconstructed afterwards, so
 * it is sampled on the way out of each operation.  Everything else
 * netstack_pool_sample() records is a current value, readable when somebody
 * asks for it, and the asker is a person running ShowNetStatus.
 *
 * It was one function doing both, at 72 samples of 9606 on an A1200 wire
 * transfer, so keeping a low-water mark for a status display cost more than
 * nx_tcp_packet_send_ack.
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

    /* ms_PoolTotal is zero until the first full sample, which netstack_start()
       takes, so a low of zero cannot be recorded before there is a pool. */
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

    /* First sample: nothing has ever been lower than what is free now. */
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
        nx_index >= (UWORD)AMI_CFG_MAX_INTERFACES)
        return NULL;

    slot = ns->ns_IfaceCfg[nx_index];
    if (slot >= (UWORD)AMI_CFG_MAX_INTERFACES ||
        !ns->ns_Config.interfaces[slot].configured)
        return NULL;

    return &ns->ns_Config.interfaces[slot];
}

BOOL netstack_iface_mdns(UWORD nx_index)
{
    const AmiNetStack *ns = ami_netstack_raw();

    if (ns == NULL || nx_index >= (UWORD)AMI_CFG_MAX_INTERFACES)
        return FALSE;

    return ns->ns_IfaceMdns[nx_index];
}

LONG netstack_iface_mdns_set(UWORD nx_index, BOOL enable)
{
#ifdef AMINETXDUO_MDNS
    AmiNetStack *ns = ami_netstack_raw();

    if (ns == NULL || !ns->ns_IpCreated ||
        nx_index >= (UWORD)AMI_CFG_MAX_INTERFACES ||
        ns->ns_Iface[nx_index] == NULL)
        return AMI_NET_ERR_STATE;

    return ami_netstack_mdns_iface_set(ns, nx_index, enable);
#else
    (VOID)nx_index;
    (VOID)enable;

    return AMI_NET_ERR_NODEV;
#endif
}

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
     * Inside the bracket, because of what else reads what it writes.
     * ami_config_hostname_offer() does not touch NetX Duo. NX_DHCP was handed
     * ns_Config.hostname as a pointer at create time (nx_dhcp_create above),
     * so the DHCP thread reads this buffer while building a request. Holding
     * the baton across the copy means that thread is not running during it,
     * and no request can carry half of one name and half of another.
     */
    caller = ami_netstack_enter_alloc();
    if (caller == NULL)
        return AMI_NET_ERR_KERNEL;

    taken = ami_config_hostname_offer(&ns->ns_Config, source, name);

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
        /* Re-arm router solicitation first: SLAAC is what a resumed DHCPv6
           client's M and O bits come from, so asking the router again before
           restarting the client keeps the two in the order bring-up uses. */
        ami_netstack_ipv6_interface_up(ns, index);
        ami_netstack_dhcpv6_resume(ns, index);
    }
#endif

    ami_netstack_leave_free(caller);

    return (status == NX_SUCCESS) ? AMI_NET_OK : AMI_NET_ERR_NODEV;
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
 * Give the DHCPv6 address back before the wire goes away.
 *
 * This is NetShutdown's second step -- src/tools/netshutdown.c takes every
 * interface down through NETCTRL_INTERFACE_DOWN, which lands here -- and it is
 * also Offline and the ARexx bulk-down. All three mean the same thing to a
 * DHCPv6 server: this client is finished with the address, so RFC 8415 18.2.7
 * says send a Release and let somebody else have it. Nothing else in the
 * shutdown path can do it, because by the time the stack is being deleted the
 * interface can no longer transmit.
 *
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
 * IFA_DownGoesOffline turns this back into the offline path, "bringing the
 * interface 'down' ... will cause the associated SANA-II device driver to be
 * switched offline". Default is FALSE, which is the bzero of the field.
 */
LONG netstack_interface_stack_down(UWORD index)
{
    AmiNetStack *ns = ami_ns;

    if (ns != NULL && index < (UWORD)AMI_CFG_MAX_INTERFACES &&
        ns->ns_Config.interfaces[index].down_goes_offline)
        return netstack_interface_down(index);

    /* SM_Down stops this stack transmitting on the interface, which ends the
       lease as surely as taking the card offline does. */
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

/* ------------------------------------------- interfaces at run time ------
 *
 * The AddInterfaceTagList() and RemoveInterface() in bsdsocket.library are the
 * only callers, and this is the only path by which ns_Iface[] changes after
 * netstack_startup() has returned. It lives here rather than in the library
 * because half the work belongs to NetX Duo and half is the SANA-II device,
 * the BPF registration and the configuration slot.  netstack_shutdown() does
 * not close an interface that got only one half.
 *
 * A removal does not decrement ns_IfaceCount. It counts slots ever populated
 * rather than slots live, so a removal in the middle does not renumber the
 * ones above it, and an interface index is a handle a caller can already hold.
 * Every loop over it that touches ns_Iface[] checks the slot.  Loops that read
 * ns_Config or call a NetX Duo API by index are safe on a hole either way.
 */

/*
 * Whether anything is still using this interface, the question the `force`
 * parameter of RemoveInterface() overrides: "RemoveInterface() will refuse to
 * remove an interface which is still in use."
 *
 * Counted as TCP connections routed out of it. UDP sockets are not counted: a
 * datagram socket is not bound to an interface. TCP is different, since
 * nx_ip_interface_detach() resets every such connection, which is visible at
 * the other end of the wire.
 *
 * Must be called inside a ThreadX bracket. The created-socket list is
 * circular, so the walk is bounded by the NetX Duo count rather than by a NULL
 * that never comes.
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
        index >= (UWORD)AMI_CFG_MAX_INTERFACES || ns->ns_Iface[index] == NULL)
        return AMI_NET_ERR_STATE;

    /* `force` overrides live TCP users, not an operation that still owns this
       numeric slot. Reusing it while that operation runs would redirect its
       writes to the replacement interface. */
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
     * Stop answering .local here first. nx_mdns_disable() leaves 224.0.0.251
     * on this interface and queues the RFC 6762 10.1 goodbye, and both need an
     * NX_INTERFACE that still exists: nx_ip_interface_detach() zeroes it.
     *
     * The goodbye does not reach the wire, because the thread of the responder
     * sends it 250 ms later and the interface is gone by then. The same thing
     * happens to the records of any host when its cable is pulled, which is
     * why RFC 6762 10 gives them a TTL. What matters here is the flag: an
     * interface that has been removed must not still report that it answers
     * .local, which is what it did.
     */
    (VOID)ami_netstack_mdns_iface_set(ns, index, FALSE);
#endif

    /*
     * netstack_interface_down() below sends the DHCPv6 Release before it takes
     * the wire offline. That ordering is the whole difference from the DHCPv4
     * release further down: nx_dhcp_interface_release() is best-effort and
     * gets away with being called after NX_LINK_DISABLE, and a DHCPv6 Release
     * has to reach the server, which after the disable it cannot.
     *
     * Stop the readers before anything is detached. NX_LINK_DISABLE takes the
     * wire offline and reclaims the outstanding CMD_READs, and it is where a
     * device that does not give them back shows up, which has to be known
     * before nx_ip_interface_detach() zeroes the NX_INTERFACE.
     */
    (VOID)netstack_interface_down(index);

    if (ami_sana2_orphaned(iface))
    {
        /*
         * The device still holds read requests pointing into this allocation.
         * Freeing it hands the device memory returned to the system, so
         * nothing is freed and the interface stays registered: down, and not
         * removable until NetShutdown. The published API warns about this
         * state under `force`.
         */
        AMI_ERROR("netstack: '%s' cannot be removed, the device still holds "
                  "read requests inside it",
                  ns->ns_Config.interfaces[index].name);
        return AMI_NET_ERR_STATE;
    }

    /*
     * Stop the DHCP client on this interface before the interface goes.
     * nx_ip_interface_detach() knows nothing about DHCP, so a client left
     * enabled keeps trying to renew a lease for a slot that no longer exists,
     * and ns_DhcpState[] keeps saying BOUND, the state a later
     * BeginInterfaceConfig() on the same slot reads to decide whether an
     * allocation is already under way. Not clearing it made a
     * removed-and-re-added interface answer AAMR_Busy forever.
     *
     * The lease is released rather than abandoned, so the server can give the
     * address to the next machine.
     */
    if (ns->ns_DhcpCreated)
        (VOID)netstack_interface_dhcp_stop(index, TRUE);

#ifdef AMINETXDUO_BPF
    /* src/bpf/ holds the AmiSana2If as an opaque cookie, so it has to stop
       being reachable before the memory goes. */
    ami_netstack_capture_detach_one(ns, index);
#endif

    caller = ami_netstack_enter_alloc();
    if (caller == NULL)
        return AMI_NET_ERR_KERNEL;

    /* AutoIP keeps only the numeric NX_INTERFACE slot. If that slot is
       detached and later reused, a surviving worker can configure the new
       interface even though its configuration never requested link-local
       addressing. Destroy it while the selected interface still exists; a
       later link-local or DHCP fallback request creates it again. */
    if (ns->ns_AutoIpCreated &&
        ns->ns_AutoIp.nx_ip_interface_index == (UINT)index)
    {
        (VOID)nx_auto_ip_stop(&ns->ns_AutoIp);
        (VOID)nx_auto_ip_delete(&ns->ns_AutoIp);
        ns->ns_AutoIpCreated = FALSE;
        ns->ns_AutoIpRunning = FALSE;
        autoip_removed = TRUE;
    }

    /*
     * nx_ip_interface_detach() does the whole of the NetX Duo side: it resets
     * the TCP connections that went out of this interface, deletes its ARP
     * entries, drops the static routes and the default gateway that pointed
     * at it, leaves its multicast groups, calls the driver with
     * NX_LINK_INTERFACE_DETACH, which is where sana2_driver.c unbinds, and
     * zeroes the NX_INTERFACE.
     */
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

    /* CloseDevice() and the reply-port teardown are exec I/O, so they happen
       outside the bracket. */
    ami_sana2_close(iface);

    ns->ns_Iface[index] = NULL;
    ns->ns_Config.interfaces[index].configured = FALSE;

    AMI_INFO("netstack: interface %ld removed", (long)index);

    return AMI_NET_OK;
}

/* Paired with netstack_interface_add().  See the note there. */
LONG netstack_interface_remove(UWORD index, BOOL force)
{
    LONG rc;

    ami_ns_lock_init();
    ObtainSemaphore(&ami_ns_lock);
    rc = ami_ns_interface_remove_locked(index, force);
    ReleaseSemaphore(&ami_ns_lock);

    return rc;
}

/* Resolve and remove as one transaction. A slot is deliberately reusable, so
   resolving the name before taking ami_ns_lock can remove a different
   interface that another task installed in the meantime. */
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

    for (i = 0; i < (UWORD)AMI_CFG_MAX_INTERFACES; i++)
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

/* ---------------------------------------------- DHCP on one interface ---- */

static VOID ami_ns_zero(APTR p, ULONG size)
{
    UBYTE *b = (UBYTE *)p;

    while (size-- > 0)
        *b++ = 0;
}


/*
 * The client, created on demand. A machine whose DEVS:NetInterfaces are all
 * static boots without an NX_DHCP, and an application can still ask for one
 * interface to be configured by DHCP. There can be only one client, because
 * there is only one UDP port 68, so it is created here if start-up did not,
 * with the same host name and state-change callback, and every later request
 * shares it.
 *
 * Must be called inside a ThreadX bracket.
 */
static LONG ami_ns_dhcp_ensure(AmiNetStack *ns)
{
    UINT status;

    if (ns->ns_DhcpCreated)
        return AMI_NET_OK;

    /* NetX Duo keeps the host name pointer rather than a copy, so it must be
       storage that outlives the NX_DHCP, the same reason start-up hands it
       ns_Config. The literal is reached only when no card gave
       ami_ns_name_after_card() a hardware address to work from. */
    status = nx_dhcp_create(&ns->ns_Dhcp, &ns->ns_Ip,
                            (ns->ns_Config.hostname[0] != '\0')
                                ? (CHAR *)ns->ns_Config.hostname
                                : (CHAR *)"amiga");
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
        index >= (UWORD)AMI_CFG_MAX_INTERFACES || ns->ns_Iface[index] == NULL)
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

    /*
     * Busy means an allocation in progress on this interface, not that the
     * client has ever run here: a BOUND interface was already reported with
     * AAMR_AddressKnown, and a removed-and-re-added interface has had its
     * state cleared by netstack_interface_remove(). Testing NOT_STARTED
     * instead made a re-added interface answer AAMR_Busy for as long as the
     * machine was up.
     */
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
     * "A DHCP client can make a wish, as to which IP address should be
     * allocated for it." skip_discover is 0: skipping DISCOVER turns the wish
     * into a demand, and a server that disagrees answers NAK rather than
     * offering a different address.
     */
    if (requested_address != 0)
        (VOID)nx_dhcp_interface_request_client_ip(&ns->ns_Dhcp, (UINT)index,
                                                  requested_address, 0);

    status = nx_dhcp_interface_start(&ns->ns_Dhcp, (UINT)index);

    /* Same reason as at startup: an interface brought up by hand need not sit
       out RFC 2131's desynchronisation second either. */
    if (status == NX_SUCCESS)
        ami_ns_dhcp_discover_now(&ns->ns_Dhcp);

    ami_netstack_leave_free(caller);

    if (status != NX_SUCCESS)
    {
        AMI_WARN("netstack: DHCP did not start on interface %ld (%ld)",
                 (long)index, (long)status);
        return AMI_NET_ERR_STATE;
    }

    /* So netstatus and QueryInterfaceTagList report the binding type this
       interface now has. */
    ns->ns_Config.interfaces[index].iptype = AMI_IPTYPE_DHCP;

    AMI_INFO("netstack: DHCP started on interface %ld", (long)index);

    return AMI_NET_OK;
}

/*
 * Where the client has got to. Read from ns_DhcpState[], which the
 * state-change callback maintains for every interface, so no bracket is needed
 * to read a byte the callback wrote.
 */
LONG netstack_interface_dhcp_state(UWORD index)
{
    AmiNetStack *ns = ami_ns;

    if (ns == NULL || index >= (UWORD)AMI_CFG_MAX_INTERFACES)
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
            /* BOOT, INIT, SELECTING, REQUESTING, FORCERENEW, ADDRESS_PROBING:
               all still trying, which is all a caller waiting on a deadline
               needs. */
            return AMI_DHCP_WORKING;
    }
}

/* One DHCP option that is a list of IPv4 addresses, into `out`. */
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

        /* "A router address of 0 should be ignored", and the same for the
           other two lists, which the autodoc states in the same words. */
        if (addr != 0)
            out[count++] = addr;
    }

    return count;
}

/* One DHCP option that is text. Not NUL-terminated on the wire. */
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
        index >= (UWORD)AMI_CFG_MAX_INTERFACES)
        return AMI_NET_ERR_STATE;

    if (netstack_interface_dhcp_state(index) != AMI_DHCP_BOUND)
        return AMI_NET_ERR_STATE;

    caller = ami_netstack_enter_alloc();
    if (caller == NULL)
        return AMI_NET_ERR_KERNEL;

    /* The address and mask come from the interface rather than the options:
       they are what the client configured, and what the caller finds on the
       interface afterwards. */
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
 * NETSTATUS_DHCPRAW_* is published as NX_DHCP_STATE_* verbatim, and this is the
 * only file that can see both. If a NetX Duo update renumbers them the build
 * stops here rather than at a user reading "renewing" off a lease that is
 * being requested for the first time. Same rule and same reason as the
 * NX_TCP_* asserts at the head of src/bsdsocket/netstatus.c.
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

/*
 * The uncollapsed state, from the same ns_DhcpState[] the callback maintains,
 * so no bracket is needed to read a byte the callback wrote.
 */
UWORD netstack_interface_dhcp_raw_state(UWORD index)
{
    AmiNetStack *ns = ami_ns;

    if (ns == NULL || !ns->ns_DhcpCreated ||
        index >= (UWORD)AMI_CFG_MAX_INTERFACES)
        return (UWORD)NX_DHCP_STATE_NOT_STARTED;

    return (UWORD)ns->ns_DhcpState[index];
}

LONG netstack_interface_dhcp_renew(UWORD index)
{
    AmiNetStack  *ns = ami_ns;
    AmiNetCaller *caller;
    UINT          status;

    if (ns == NULL || !ns->ns_IpCreated || !ns->ns_DhcpCreated ||
        index >= (UWORD)AMI_CFG_MAX_INTERFACES)
        return AMI_NET_ERR_STATE;

    /*
     * Checked here as well as inside NetX Duo, because
     * nx_dhcp_interface_force_renew() answers NX_DHCP_NOT_BOUND for an
     * interface that has no lease and for one the client has never heard of.
     * The two want the same answer from up here and arrive by different
     * routes: ns_DhcpState[] is the record kept for every interface, and
     * _nx_dhcp_interface_record_find() only knows the ones that were enabled.
     */
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
        index >= (UWORD)AMI_CFG_MAX_INTERFACES)
        return AMI_NET_ERR_STATE;

    caller = ami_netstack_enter_alloc();
    if (caller == NULL)
        return AMI_NET_ERR_KERNEL;

    /* Releasing tells the server the address is free again: right when an
       allocation is abandoned, wrong when it succeeded, so the caller
       chooses. */
    if (release)
        (VOID)nx_dhcp_interface_release(&ns->ns_Dhcp, (UINT)index);

    (VOID)nx_dhcp_interface_stop(&ns->ns_Dhcp, (UINT)index);

    ns->ns_DhcpState[index] = NX_DHCP_STATE_NOT_STARTED;
    ami_netstack_dns_dhcp_changed(ns, index);

    ami_netstack_leave_free(caller);

    return AMI_NET_OK;
}

/*
 * Compare interface names the way the rest of the stack does. They come from
 * file names in DEVS:NetInterfaces and AmigaDOS file names are
 * case-insensitive, so "ETH0" and "eth0" are one interface.
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
 * Turn a name into a stable numeric interface slot. Add, remove, claim and
 * release all use ami_ns_lock, so once the count is raised the slot cannot be
 * detached and reused until the matching release.
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
        for (i = 0; i < (UWORD)AMI_CFG_MAX_INTERFACES; i++)
        {
            UWORD cfg_index;

            if (ns->ns_Iface[i] == NULL)
                continue;

            cfg_index = ns->ns_IfaceCfg[i];
            if (cfg_index >= (UWORD)AMI_CFG_MAX_INTERFACES ||
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
 * The BPF interface table deliberately treats its SANA-II pointer as an
 * opaque cookie. Resolve and claim that cookie under the runtime-interface
 * lock before an injector turns it back into a pointer. This is the cookie
 * counterpart of netstack_interface_claim(): once the count is raised,
 * RemoveInterface() cannot detach or free the allocation until release.
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
        for (i = 0; i < (UWORD)AMI_CFG_MAX_INTERFACES; i++)
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
    if (ns != NULL && index < (UWORD)AMI_CFG_MAX_INTERFACES &&
        ns->ns_IfaceClaims[index] != 0)
        ns->ns_IfaceClaims[index]--;

    ReleaseSemaphore(&ami_ns_lock);
}

/*
 * Predict the slot a new interface will land in.
 *
 * nx_ip_interface_attach() scans nx_ip_interface[] from zero and takes the
 * first entry whose nx_interface_valid is clear, so the prediction is the same
 * scan. It has to be predicted because ami_sana2_attach() records the
 * (NX_IP, index) binding the driver entry looks itself up by, and that must be
 * in place before the attach calls the driver.
 *
 * The matching ns_Iface[] slot must be free too. The two disagree only if a
 * previous removal left one behind, and that case is refused.
 */
static LONG ami_ns_free_interface_slot(AmiNetStack *ns)
{
    UWORD i;

    for (i = 0; i < (UWORD)NX_MAX_PHYSICAL_INTERFACES &&
                i < (UWORD)AMI_CFG_MAX_INTERFACES; i++)
    {
        if (ns->ns_Ip.nx_ip_interface[i].nx_interface_valid == 0 &&
            ns->ns_Iface[i] == NULL && ns->ns_IfaceClaims[i] == 0)
            return (LONG)i;
    }

    return -1;
}

static LONG ami_ns_interface_add_locked(const AmiIfConfig *cfg,
                                        UWORD *index_out)
{
    AmiNetStack  *ns = ami_ns;
    AmiNetCaller *caller;
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
     * The configuration is copied into the storage of the netstack before the
     * device is opened, and stays there: nx_ip_interface_attach() keeps the
     * name pointer rather than the name, so the string must outlive the tag
     * list of the caller.
     */
    slot_cfg = &ns->ns_Config.interfaces[slot];
    *slot_cfg = *cfg;
    slot_cfg->configured = TRUE;

#ifdef AMINETXDUO_IPV6
    /*
     * AddInterfaceTagList() has no IPv6 tag, so a caller cannot express an
     * IPv6 mode and arrives with the zero of the enum, which reads as OFF. An
     * interface file with no CONFIGURE6 line is defaulted to AUTO
     * (config_parse.c) and this is the same interface by another route: the
     * link-local RFC 4291 requires is not addressing the caller supplies.
     */
    if (slot_cfg->ip6type == AMI_IP6TYPE_OFF)
    {
        slot_cfg->ip6type = AMI_IP6TYPE_AUTO;
        slot_cfg->prefix6 = 64;
    }
#endif

    if ((UWORD)slot >= ns->ns_Config.interface_count)
        ns->ns_Config.interface_count = (UWORD)(slot + 1);

    iface = ami_sana2_open(slot_cfg, &err);
    if (iface == NULL)
    {
        AMI_ERROR("netstack: interface \'%s\' did not start: %s unit %lu %s",
                  slot_cfg->name, slot_cfg->device,
                  (unsigned long)slot_cfg->unit,
                  (err == AMI_NET_ERR_DEVBAD) ? "refused a SANA-II command"
                                              : "did not answer");
        slot_cfg->configured = FALSE;
        return (err != AMI_NET_OK) ? err : AMI_NET_ERR_NODEV;
    }

    ns->ns_Iface[slot] = iface;

    caller = ami_netstack_enter_alloc();
    if (caller == NULL)
    {
        ami_sana2_close(iface);
        ns->ns_Iface[slot] = NULL;
        slot_cfg->configured = FALSE;
        return AMI_NET_ERR_KERNEL;
    }

    /* The binding first, for the reason in ami_ns_free_interface_slot(). */
    if (ami_sana2_attach(iface, &ns->ns_Ip, (UINT)slot) != AMI_NET_OK)
    {
        ami_netstack_leave_free(caller);
        ami_sana2_close(iface);
        ns->ns_Iface[slot] = NULL;
        slot_cfg->configured = FALSE;
        return AMI_NET_ERR_STATE;
    }

    /*
     * The address and mask the caller gave, and nothing else.
     * AddInterfaceTagList() has no address, netmask, gateway or broadcast tag.
     * "such as setting interface addresses, status and routing metrics" is the
     * sentence for ConfigureInterfaceTagList(). So an interface added through
     * that vector arrives bare and is addressed by the configure that follows,
     * and only startup, which reads DEVS:NetInterfaces, reaches here with a
     * static pair to attach with.
     */
    status = nx_ip_interface_attach(&ns->ns_Ip, (CHAR *)slot_cfg->name,
                                    (slot_cfg->iptype == AMI_IPTYPE_STATIC)
                                        ? slot_cfg->address : 0UL,
                                    (slot_cfg->iptype == AMI_IPTYPE_STATIC)
                                        ? slot_cfg->netmask : 0UL,
                                    ami_sana2_driver_entry);

    /* The slot NetX Duo took must be the predicted one, because the driver
       binding was made against the prediction.  A mismatch makes the interface
       answer for the wrong device. */
    if (status == NX_SUCCESS &&
        ns->ns_Ip.nx_ip_interface[slot].nx_interface_valid == 0)
        status = NX_INVALID_INTERFACE;

    ami_netstack_leave_free(caller);

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

    /* The two per-slot tables ami_ns_open_devices() fills at start-up.
       netstack_iface_config() reads ns_IfaceCfg[] to answer what this
       interface was configured from, and answered for whatever was in the
       slot before without it. */
    ns->ns_IfaceCfg[slot]  = (UWORD)slot;

    /*
     * MDNS= is acted on and not merely recorded. Setting the flag alone left an
     * interface reporting NETSTATUS_IF_MDNS while it had joined no group,
     * probed for no name and answered nothing, because nx_mdns_enable() had one
     * caller and it was start-up. Cleared first, because a slot re-used by a
     * different interface must not inherit the answer of the last one.
     */
    ns->ns_IfaceMdns[slot] = FALSE;
#ifdef AMINETXDUO_MDNS
    if (slot_cfg->mdns)
        (VOID)ami_netstack_mdns_iface_set(ns, (UWORD)slot, TRUE);
#endif

#ifdef AMINETXDUO_BPF
    ami_netstack_capture_attach_one(ns, (UWORD)slot);
#endif

#ifdef AMINETXDUO_IPV6
    /*
     * IPv6, which startup does once in ami_ns_configure_addresses() for the
     * interfaces the file named. Without this an interface added here has no
     * link-local address at all, which RFC 4291 requires, and no
     * solicited-node multicast membership, so neighbour discovery cannot reach
     * it. nx_ip_interface_detach() zeroes the whole IPv6 address list of the
     * interface, so a remove/add pair loses it and nothing else puts it back.
     *
     * After the capture attach, for the reason startup starts capture before
     * addressing: the duplicate address detection this sends belongs inside
     * the trace.
     *
     * The bracket is the third and last one here. What happens inside it is
     * configuration only: duplicate address detection runs afterwards, on the
     * IP thread, and is reported by ami_ns6_address_changed() rather than
     * waited for here.
     */
    caller = ami_netstack_enter_alloc();
    if (caller != NULL)
    {
        ami_netstack_ipv6_configure_one(ns, (UWORD)slot);
        ami_netstack_leave_free(caller);
    }
#endif

    if (index_out != NULL)
        *index_out = (UWORD)slot;

    AMI_INFO("netstack: interface \'%s\' added as %ld", slot_cfg->name,
             (long)slot);

    return AMI_NET_OK;
}

/*
 * The slot is picked, then the SANA-II device is opened, Exec I/O, and long
 * before anything marks it taken, so two tasks adding at once can both leave
 * with the same one. ami_ns_lock is the outer lock the startup and shutdown
 * paths already use.  The ThreadX bracket cannot serve here, because add and
 * remove deliberately run most of their work outside it. Exec semaphores nest
 * per task, so a caller already holding it is not deadlocked by this.
 */
LONG netstack_interface_add(const AmiIfConfig *cfg, UWORD *index_out)
{
    LONG rc;

    ami_ns_lock_init();
    ObtainSemaphore(&ami_ns_lock);
    rc = ami_ns_interface_add_locked(cfg, index_out);
    ReleaseSemaphore(&ami_ns_lock);

    return rc;
}

/*
 * The same add, plus everything start-up does to an interface it finds in
 * DEVS:NetInterfaces: the address, the gateway, the address allocation the
 * file asks for and STATE=down.
 *
 * netstack_interface_add() deliberately stops short of all of it, because
 * AddInterfaceTagList() promises a bare interface its caller addresses with
 * ConfigureInterfaceTagList(). AddNetInterface has a file rather than a tag
 * list, so its interface has to arrive the way a booted one does.
 */
LONG netstack_interface_start(const AmiIfConfig *cfg, UWORD *index_out)
{
    AmiNetStack  *ns;
    AmiNetCaller *caller;
    UWORD         index                  = 0;
    ULONG         gateway                = 0UL;
    BOOL          autoip_created_before  = FALSE;
    BOOL          autoip_running_before  = FALSE;
    BOOL          added                  = FALSE;
    LONG          rc;

    ami_ns_lock_init();
    ObtainSemaphore(&ami_ns_lock);

    ns = ami_ns;
    if (ns == NULL || !ns->ns_IpCreated || cfg == NULL)
    {
        ReleaseSemaphore(&ami_ns_lock);
        return AMI_NET_ERR_STATE;
    }

    autoip_created_before = ns->ns_AutoIpCreated;
    autoip_running_before = ns->ns_AutoIpRunning;

    rc = ami_ns_interface_add_locked(cfg, &index);
    if (rc != AMI_NET_OK)
        goto out;
    added = TRUE;

    /*
     * The static address arrived with the attach, which is given it.  The
     * gateway did not. nx_ip_interface_detach() takes the default gateway with
     * the interface that carried it, so a removed and re-added interface comes
     * back with a subnet and no way off it. The GATEWAY in the interface file
     * first, then the one for the machine, which is where that line ends up
     * when there is one interface. A leased address brings its own gateway, so
     * DHCP is left to put that back. The gateway is installed last so a later
     * configuration failure cannot overwrite the machine-wide route and then
     * require a best-effort restore.
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

        rc = ami_ns_start_autoip(ns, -1L);
        ami_netstack_leave_free(caller);
        if (rc != AMI_NET_OK)
            goto rollback;
    }

    /* STATE=down, honoured after the attach and the same way start-up honours
       it: the attach is what brought the link up. */
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
            AMI_WARN("netstack: gateway set failed (%ld)", (long)status);
            rc = AMI_NET_ERR_STATE;
            goto rollback;
        }
    }

    if (index_out != NULL)
        *index_out = index;

    rc = AMI_NET_OK;
    goto out;

rollback:
    /* AutoIP is one machine-wide object rather than one object per interface.
       If this transaction created or restarted it, restore the state that was
       present before removing the interface it may currently name. */
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

out:
    ReleaseSemaphore(&ami_ns_lock);
    return rc;
}

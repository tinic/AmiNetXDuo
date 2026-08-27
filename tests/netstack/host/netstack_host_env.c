/*
 * The machine src/netstack/netstack.c runs on in the host tier.  See
 * netstack_host_env.h for what this is and what it is not.
 *
 * Three groups of definition, in this order:
 *
 *   1. Exec.  Memory is malloc(), the semaphores are counters and Forbid() is
 *      a depth, which is enough for a single-threaded harness and makes an
 *      unbalanced Permit() visible.
 *   2. tx_amiga_*, the NetX Duo entry points and the src/ symbols the tests
 *      steer or observe.  Each carries the claim it exists for.
 *   3. The inert remainder, generated from the vendored prototypes: every
 *      other NetX Duo, ThreadX and src/ symbol netstack.c names, answering
 *      success and recording nothing.  A call netstack.c does not make today
 *      fails to link rather than being answered by a default.
 *
 * SPDX-License-Identifier: MIT
 */

#include "netstack_host_env.h"

#include "aminetxduo/netstatus.h"
#include "aminetxduo/events.h"
#include "aminetxduo/budget.h"
#include "aminetxduo/random.h"
#include "aminetxduo/config.h"

#include "netstack_dhcp_hostname.h"
#include "netstack_gateway.h"

#include <proto/exec.h>
#include <exec/memory.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

NetStackHostEnv nsh;

VOID nsh_reset(VOID)
{
    memset(&nsh, 0, sizeof(nsh));

    nsh.tx_start_status     = TX_SUCCESS;
    nsh.tx_stop_status      = TX_SUCCESS;
    nsh.tx_adopt_status     = TX_SUCCESS;
    nsh.dhcp_start_status   = NX_SUCCESS;
    nsh.dhcp_restart_status = NX_SUCCESS;
    nsh.dhcp_create_status  = NX_SUCCESS;
    nsh.ip_create_status    = NX_SUCCESS;
    nsh.cfg_interfaces      = 1;
    nsh.cfg_iptype          = (UWORD)AMI_IPTYPE_DHCP;
    nsh.iface_address       = 0xC0A80132UL;     /* 192.168.1.50 */
}

static VOID nsh_trace(char c)
{
    if (nsh.dhcp_trace_len + 1 < sizeof(nsh.dhcp_trace))
        nsh.dhcp_trace[nsh.dhcp_trace_len++] = c;
}

int nsh_dhcp_trace_is(const char *want)
{
    return strcmp(nsh.dhcp_trace, want) == 0;
}

VOID nsh_trace_clear(VOID)
{
    memset(nsh.dhcp_trace, 0, sizeof(nsh.dhcp_trace));
    nsh.dhcp_trace_len = 0;
}

/* ------------------------------------------------------------------ Exec -- */

static struct Task     nsh_task;
static struct MsgPort  nsh_port;

APTR AllocMem(ULONG byteSize, ULONG requirements)
{
    APTR p = malloc(byteSize ? byteSize : 1);

    if (p != NULL)
    {
        nsh.allocs++;
        if ((requirements & MEMF_CLEAR) != 0)
            memset(p, 0, byteSize);
    }

    return p;
}

VOID FreeMem(APTR memoryBlock, ULONG byteSize)
{
    (VOID)byteSize;

    if (memoryBlock != NULL)
    {
        nsh.frees++;
        free(memoryBlock);
    }
}

ULONG AvailMem(ULONG requirements)
{
    (VOID)requirements;

    return 8UL * 1024UL * 1024UL;
}

VOID Forbid(VOID)
{
    nsh.forbids++;
    nsh.forbid_depth++;
}

VOID Permit(VOID)
{
    nsh.forbid_depth--;
}

VOID InitSemaphore(struct SignalSemaphore *sigSem)
{
    memset(sigSem, 0, sizeof(*sigSem));
}

VOID ObtainSemaphore(struct SignalSemaphore *sigSem)
{
    sigSem->ss_NestCount++;
}

VOID ReleaseSemaphore(struct SignalSemaphore *sigSem)
{
    sigSem->ss_NestCount--;
}

ULONG AttemptSemaphore(struct SignalSemaphore *sigSem)
{
    if (nsh.attempt_semaphore_fails)
        return 0;

    sigSem->ss_NestCount++;

    return 1;
}

struct Task *FindTask(const char *name)
{
    (VOID)name;

    return &nsh_task;
}

struct MsgPort *CreateMsgPort(VOID)
{
    memset(&nsh_port, 0, sizeof(nsh_port));

    return &nsh_port;
}

VOID DeleteMsgPort(struct MsgPort *port)
{
    (VOID)port;
}

VOID AddPort(struct MsgPort *port)
{
    (VOID)port;
}

VOID RemPort(struct MsgPort *port)
{
    (VOID)port;
}

struct MsgPort *FindPort(const UBYTE *name)
{
    (VOID)name;

    return NULL;
}

struct Message *GetMsg(struct MsgPort *port)
{
    (VOID)port;

    return NULL;
}

VOID ReplyMsg(struct Message *message)
{
    (VOID)message;
}

/* ------------------------------------------------------- the ThreadX port -- */

/*
 * The half of the expunge refusal this tier can move.  A failed
 * tx_amiga_kernel_stop() means Exec Tasks are still running on code in the
 * library's hunk, and ami_ns_kernel_started must stay set so that
 * netstack_can_unload() keeps answering FALSE.
 */
UINT tx_amiga_kernel_start(VOID)
{
    nsh.tx_starts++;

    return nsh.tx_start_status;
}

UINT tx_amiga_kernel_stop(VOID)
{
    nsh.tx_stops++;

    if (nsh.tx_stop_status == TX_SUCCESS)
        nsh.tx_stops_ok++;

    return nsh.tx_stop_status;
}

UINT tx_amiga_kernel_running(VOID)
{
    return TX_TRUE;
}

/* Taken back only on a successful stop: on anything else a thread can still
   be inside a bracket (netstack.c, ami_ns_kernel_stop_locked). */
VOID ami_netstack_baton_reset(VOID)
{
    nsh.baton_resets++;
}

UINT tx_amiga_adopt_thread(TX_THREAD *thread_ptr, CHAR *name, UINT priority)
{
    (VOID)thread_ptr;
    (VOID)name;
    (VOID)priority;

    return nsh.tx_adopt_status;
}

UINT tx_amiga_orphan_thread(TX_THREAD *thread_ptr)
{
    (VOID)thread_ptr;

    return TX_SUCCESS;
}

UINT tx_amiga_adopt_resume(TX_THREAD *thread_ptr)
{
    (VOID)thread_ptr;

    return TX_SUCCESS;
}

UINT tx_amiga_adopt_suspend(TX_THREAD *thread_ptr)
{
    (VOID)thread_ptr;

    return TX_SUCCESS;
}

UINT tx_amiga_adopt_try_resume(TX_THREAD *thread_ptr)
{
    (VOID)thread_ptr;

    return TX_SUCCESS;
}

UINT tx_amiga_baton_free(VOID)
{
    return TX_TRUE;
}

UINT tx_amiga_discard_thread(TX_THREAD *thread_ptr)
{
    (VOID)thread_ptr;

    return TX_SUCCESS;
}

UINT tx_amiga_caller_is_thread(VOID)
{
    return (UINT)TX_FALSE;
}

/* Advancing, so any bounded wait in netstack.c terminates rather than
   spinning on a clock that never moves. */
ULONG _tx_time_get(VOID)
{
    static ULONG now;

    return now++;
}

/* --------------------------------------------------------- the DHCP client -- */

/*
 * netstack_interface_dhcp_start()'s restart.  NX_DHCP_ALREADY_STARTED means
 * the client's own record disagrees with ns_DhcpState[], and returning it
 * leaves the record armed with no DISCOVER on the wire, so the shipping code
 * stops the interface and starts it again.  dhcp_restart_status is what that
 * second start answers.
 */
UINT _nxe_dhcp_interface_start(NX_DHCP *dhcp_ptr, UINT iface_index)
{
    (VOID)dhcp_ptr;
    (VOID)iface_index;

    nsh_trace('s');

    return (nsh.dhcp_starts++ == 0) ? nsh.dhcp_start_status
                                    : nsh.dhcp_restart_status;
}

UINT _nxe_dhcp_interface_stop(NX_DHCP *dhcp_ptr, UINT iface_index)
{
    (VOID)dhcp_ptr;
    (VOID)iface_index;

    nsh.dhcp_stops++;
    nsh_trace('x');

    return NX_SUCCESS;
}

UINT _nxe_dhcp_interface_enable(NX_DHCP *dhcp_ptr, UINT iface_index)
{
    (VOID)dhcp_ptr;
    (VOID)iface_index;

    nsh.dhcp_enables++;
    nsh_trace('e');

    return NX_SUCCESS;
}

UINT _nxe_dhcp_interface_request_client_ip(NX_DHCP *dhcp_ptr, UINT iface_index,
                                           ULONG client_ip_address,
                                           UINT skip_discover_message)
{
    (VOID)dhcp_ptr;
    (VOID)iface_index;

    nsh.dhcp_requests++;
    nsh.dhcp_request_addr = client_ip_address;
    nsh.dhcp_request_skip = skip_discover_message;
    nsh_trace('r');

    return NX_SUCCESS;
}

/*
 * ami_ns_dhcp_discover_now() deactivates, shortens and reactivates the DHCP
 * client's own timer so the first DISCOVER leaves at the next tick instead of
 * one NX_DHCP_TIME_INTERVAL later.  Tracing the activation is how a test can
 * tell "the record was armed" from "a DISCOVER is on the way", which is the
 * distinction the ALREADY_STARTED re-arm exists for.
 */
UINT _txe_timer_activate(TX_TIMER *timer_ptr)
{
    (VOID)timer_ptr;

    nsh.dhcp_discovers++;
    nsh_trace('d');

    return TX_SUCCESS;
}

UINT _nxe_dhcp_create(NX_DHCP *dhcp_ptr, NX_IP *ip_ptr, CHAR *name_ptr)
{
    (VOID)ip_ptr;
    (VOID)name_ptr;

    memset(dhcp_ptr, 0, sizeof(*dhcp_ptr));

    return nsh.dhcp_create_status;
}

/* --------------------------------------------------------------- NetX Duo -- */

UINT _nxe_ip_create(NX_IP *ip_ptr, CHAR *name, ULONG ip_address,
                    ULONG network_mask, NX_PACKET_POOL *default_pool,
                    VOID (*ip_link_driver)(NX_IP_DRIVER *), VOID *memory_ptr,
                    ULONG memory_size, UINT priority,
                    UINT ip_control_block_size)
{
    (VOID)name;
    (VOID)ip_address;
    (VOID)network_mask;
    (VOID)default_pool;
    (VOID)ip_link_driver;
    (VOID)memory_ptr;
    (VOID)memory_size;
    (VOID)priority;
    (VOID)ip_control_block_size;

    if (nsh.ip_create_status == NX_SUCCESS)
    {
        memset(ip_ptr, 0, sizeof(*ip_ptr));
        ip_ptr -> nx_ip_id = NX_IP_ID;
    }

    return nsh.ip_create_status;
}

UINT _nxe_packet_pool_create(NX_PACKET_POOL *pool_ptr, CHAR *name,
                             ULONG payload_size, VOID *memory_ptr,
                             ULONG memory_size, UINT pool_control_block_size)
{
    (VOID)name;
    (VOID)payload_size;
    (VOID)memory_ptr;
    (VOID)memory_size;
    (VOID)pool_control_block_size;

    memset(pool_ptr, 0, sizeof(*pool_ptr));

    return NX_SUCCESS;
}

/* Nonzero, so ami_ns_wait_for_address() returns on its first scan.  Whether
   bring-up waits is not what these tests are about. */
UINT _nxe_ip_interface_address_get(NX_IP *ip_ptr, UINT interface_index,
                                   ULONG *ip_address, ULONG *network_mask)
{
    (VOID)ip_ptr;
    (VOID)interface_index;

    *ip_address   = nsh.iface_address;
    *network_mask = 0xFFFFFF00UL;

    return NX_SUCCESS;
}

/* ----------------------------------------------------------- src/, steered -- */

APTR ami_alloc(ULONG size)
{
    return AllocMem(size, MEMF_PUBLIC | MEMF_CLEAR);
}

APTR ami_alloc_flags(ULONG size, ULONG memf)
{
    return AllocMem(size, memf);
}

VOID ami_free(APTR ptr)
{
    FreeMem(ptr, 0);
}

/*
 * One interface, described exactly the way NetSetup writes a DHCP one.  The
 * count and the address type are the two things a test moves.
 */
LONG ami_config_load(AmiConfig *cfg)
{
    UWORD i;

    memset(cfg, 0, sizeof(*cfg));

    if (nsh.cfg_interfaces == 0)
        return AMI_CFG_OK;

    cfg->interfaces = (AmiIfConfig *)
        calloc(nsh.cfg_interfaces, sizeof(AmiIfConfig));
    if (cfg->interfaces == NULL)
        return AMI_CFG_ERR_NOMEM;

    cfg->interface_count    = nsh.cfg_interfaces;
    cfg->interface_capacity = nsh.cfg_interfaces;

    for (i = 0; i < nsh.cfg_interfaces; i++)
    {
        AmiIfConfig *ifc = &cfg->interfaces[i];

        snprintf(ifc->name, sizeof(ifc->name), "eth%u", (unsigned)i);
        snprintf(ifc->device, sizeof(ifc->device), "host.device");
        ifc->unit       = i;
        ifc->iptype     = (AmiIpType)nsh.cfg_iptype;
        ifc->up         = TRUE;
        ifc->configured = TRUE;
    }

    return AMI_CFG_OK;
}

VOID ami_config_free(AmiConfig *cfg)
{
    free(cfg->interfaces);
    cfg->interfaces         = NULL;
    cfg->interface_count    = 0;
    cfg->interface_capacity = 0;
}

/* A cookie, never dereferenced: netstack.c only ever hands it back to
   src/sana2, which is stubbed here as well. */
static UBYTE nsh_iface_cookie[4][1];

AmiSana2If *ami_sana2_open(const AmiIfConfig *cfg, LONG *err)
{
    static UWORD n;

    if (nsh.sana2_open_fails)
    {
        *err = nsh.sana2_open_error ? nsh.sana2_open_error : AMI_NET_ERR_NODEV;
        return NULL;
    }

    (VOID)cfg;
    *err = AMI_NET_OK;

    if (n >= 4)
        n = 0;

    return (AmiSana2If *)&nsh_iface_cookie[n++][0];
}

BOOL ami_sana2_close(AmiSana2If *iface)
{
    (VOID)iface;

    return TRUE;
}

VOID ami_sana2_get_mac(const AmiSana2If *iface, UCHAR mac[AMI_ETH_ADDR_SIZE])
{
    (VOID)iface;

    mac[0] = 0x02; mac[1] = 0x00; mac[2] = 0x00;
    mac[3] = 0x00; mac[4] = 0x00; mac[5] = 0x01;
}

BOOL ami_sana2_orphaned(const AmiSana2If *iface)
{
    (VOID)iface;

    return FALSE;
}

AmiMemStats *ami_mem_stats(VOID)
{
    static AmiMemStats stats;

    return &stats;
}

/* The one-second heartbeat's callback would otherwise be an undefined
   reference; nothing here runs it. */
VOID ami_second_notify(VOID)
{
}

ULONG ami_random_ulong(VOID)
{
    return 0x12345678UL;
}

/* Every interface this harness describes has an IPv4 address type; the
   configuration loader above is what decides which. */
BOOL ami_config_iface_wants_ipv4(const AmiIfConfig *cfg)
{
    return (cfg->iptype != AMI_IPTYPE_NONE) ? TRUE : FALSE;
}

/* ----------------------------------------------------- src/, inert ------- */

VOID ami_address_change_notify(VOID)
{

}

BOOL ami_config_hostname_from_hwaddr(const UBYTE *hw, ULONG hwlen, char *out, ULONG size)
{
    (VOID)hw;
    (VOID)hwlen;
    (VOID)out;
    (VOID)size;

    return FALSE;
}

BOOL ami_config_hostname_offer(AmiConfig *cfg, UWORD source, const char *name)
{
    (VOID)cfg;
    (VOID)source;
    (VOID)name;

    return FALSE;
}

/* The fallback, not zero: ami_ns_pool_packets() divides free memory by it,
   and a zero divisor is a SIGFPE on x86 and a silent wrong answer on arm64. */
ULONG ami_config_pool_divisor(ULONG fallback)
{
    return fallback;
}

BOOL ami_config_reserve(AmiConfig *cfg, UWORD want)
{
    (VOID)cfg;
    (VOID)want;

    return FALSE;
}

VOID ami_event(UWORD code, UWORD index, ULONG value)
{
    (VOID)code;
    (VOID)index;
    (VOID)value;
}

BOOL ami_netstack_baton_abandon(TX_THREAD *thread)
{
    (VOID)thread;

    return FALSE;
}

VOID ami_netstack_baton_acquire(VOID)
{

}

VOID ami_netstack_baton_release(VOID)
{

}

VOID ami_netstack_baton_set_sampler(VOID (*fn)(VOID))
{
    (VOID)fn;
}

VOID ami_netstack_dhcpv6_configure(AmiNetStack *ns)
{
    (VOID)ns;
}

VOID ami_netstack_dhcpv6_destroy(AmiNetStack *ns)
{
    (VOID)ns;
}

VOID ami_netstack_dhcpv6_pause(AmiNetStack *ns)
{
    (VOID)ns;
}

VOID ami_netstack_dhcpv6_release(AmiNetStack *ns)
{
    (VOID)ns;
}

VOID ami_netstack_dhcpv6_resume(AmiNetStack *ns, UWORD interface_index)
{
    (VOID)ns;
    (VOID)interface_index;
}

VOID ami_netstack_dns_dhcp_changed(AmiNetStack *ns, UWORD interface_index)
{
    (VOID)ns;
    (VOID)interface_index;
}

LONG ami_netstack_dns_start(AmiNetStack *ns)
{
    (VOID)ns;

    return AMI_NET_OK;
}

VOID ami_netstack_dns_stop(AmiNetStack *ns)
{
    (VOID)ns;
}

VOID ami_netstack_health_publish(VOID)
{

}

VOID ami_netstack_health_unpublish(VOID)
{

}

VOID ami_netstack_ipv6_configure(AmiNetStack *ns)
{
    (VOID)ns;
}

VOID ami_netstack_ipv6_configure_one(AmiNetStack *ns, UWORD index)
{
    (VOID)ns;
    (VOID)index;
}

LONG ami_netstack_ipv6_enable(AmiNetStack *ns)
{
    (VOID)ns;

    return AMI_NET_OK;
}

VOID ami_netstack_ipv6_interface_up(AmiNetStack *ns, UWORD interface_index)
{
    (VOID)ns;
    (VOID)interface_index;
}

VOID ami_ns_copy_name(char *dst, const char *src, ULONG size)
{
    (VOID)dst;
    (VOID)src;
    (VOID)size;
}

VOID ami_ns_dhcp_hostname_displace(AmiNsDhcpHostnameState *state)
{
    (VOID)state;
}

UWORD ami_ns_gateway_candidates(const AmiNsGatewayIface *iface, UWORD count, UWORD removed, ULONG *out, UWORD max)
{
    (VOID)iface;
    (VOID)count;
    (VOID)removed;
    (VOID)out;
    (VOID)max;

    return 0;
}

LONG ami_sana2_attach(AmiSana2If *iface, NX_IP *ip, UINT index)
{
    (VOID)iface;
    (VOID)ip;
    (VOID)index;

    return AMI_NET_OK;
}

VOID ami_sana2_driver_entry(NX_IP_DRIVER *driver_req)
{
    (VOID)driver_req;
}

VOID ami_sana2_set_block_hooks(AmiSana2BlockHook before_wait, AmiSana2BlockHook after_wait)
{
    (VOID)before_wait;
    (VOID)after_wait;
}

VOID ami_sana2_set_open_hooks(VOID (*quiesce)(VOID), VOID (*restore)(VOID))
{
    (VOID)quiesce;
    (VOID)restore;
}

/* ------------------------------------------ NetX Duo and ThreadX, inert -- */

/* Two the generator could not find a prototype for: IGMP is behind
   AMINETXDUO_MULTICAST, and _tx_thread_identify() is declared in tx_thread.h,
   which is ThreadX's private header rather than tx_api.h. */
UINT _nxe_igmp_enable(NX_IP *ip_ptr)
{
    (VOID)ip_ptr;

    return NX_SUCCESS;
}

TX_THREAD *_tx_thread_identify(VOID)
{
    return TX_NULL;
}

UINT _nx_mld_enable(NX_IP *ip_ptr)
{
    (VOID)ip_ptr;

    return TX_SUCCESS;
}

VOID _nx_system_initialize(VOID)
{

}

UINT _nxe_arp_enable(NX_IP *ip_ptr, VOID *arp_cache_memory, ULONG arp_cache_size)
{
    (VOID)ip_ptr;
    (VOID)arp_cache_memory;
    (VOID)arp_cache_size;

    return TX_SUCCESS;
}

UINT _nxe_arp_gratuitous_send(NX_IP *ip_ptr, VOID (*response_handler)(NX_IP *ip_ptr, NX_PACKET *packet_ptr))
{
    (VOID)ip_ptr;
    (VOID)response_handler;

    return TX_SUCCESS;
}

UINT _nxe_auto_ip_create(NX_AUTO_IP *auto_ip_ptr, CHAR *name, NX_IP *ip_ptr, VOID *stack_ptr, ULONG stack_size, UINT priority)
{
    (VOID)auto_ip_ptr;
    (VOID)name;
    (VOID)ip_ptr;
    (VOID)stack_ptr;
    (VOID)stack_size;
    (VOID)priority;

    return TX_SUCCESS;
}

UINT _nxe_auto_ip_delete(NX_AUTO_IP *auto_ip_ptr)
{
    (VOID)auto_ip_ptr;

    return TX_SUCCESS;
}

UINT _nxe_auto_ip_set_interface(NX_AUTO_IP *auto_ip_ptr, UINT interface_index)
{
    (VOID)auto_ip_ptr;
    (VOID)interface_index;

    return TX_SUCCESS;
}

UINT _nxe_auto_ip_start(NX_AUTO_IP *auto_ip_ptr, ULONG starting_local_address)
{
    (VOID)auto_ip_ptr;
    (VOID)starting_local_address;

    return TX_SUCCESS;
}

UINT _nxe_auto_ip_stop(NX_AUTO_IP *auto_ip_ptr)
{
    (VOID)auto_ip_ptr;

    return TX_SUCCESS;
}

UINT _nxe_dhcp_delete(NX_DHCP *dhcp_ptr)
{
    (VOID)dhcp_ptr;

    return TX_SUCCESS;
}

UINT _nxe_dhcp_interface_force_renew(NX_DHCP *dhcp_ptr, UINT iface_index)
{
    (VOID)dhcp_ptr;
    (VOID)iface_index;

    return TX_SUCCESS;
}

UINT _nxe_dhcp_interface_release(NX_DHCP *dhcp_ptr, UINT iface_index)
{
    (VOID)dhcp_ptr;
    (VOID)iface_index;

    return TX_SUCCESS;
}

UINT _nxe_dhcp_interface_server_address_get(NX_DHCP *dhcp_ptr, UINT iface_index, ULONG *server_address)
{
    (VOID)dhcp_ptr;
    (VOID)iface_index;
    (VOID)server_address;

    return TX_SUCCESS;
}

UINT _nxe_dhcp_interface_state_change_notify(NX_DHCP *dhcp_ptr, VOID (*dhcp_interface_state_change_notify)(NX_DHCP *dhcp_ptr, UINT iface_index, UCHAR new_state))
{
    (VOID)dhcp_ptr;
    (VOID)dhcp_interface_state_change_notify;

    return TX_SUCCESS;
}

UINT _nxe_dhcp_interface_user_option_retrieve(NX_DHCP *dhcp_ptr, UINT iface_index, UINT option_request, UCHAR *destination_ptr, UINT *destination_size)
{
    (VOID)dhcp_ptr;
    (VOID)iface_index;
    (VOID)option_request;
    (VOID)destination_ptr;
    (VOID)destination_size;

    return TX_SUCCESS;
}

UINT _nxe_dhcp_start(NX_DHCP *dhcp_ptr)
{
    (VOID)dhcp_ptr;

    return TX_SUCCESS;
}

UINT _nxe_dhcp_stop(NX_DHCP *dhcp_ptr)
{
    (VOID)dhcp_ptr;

    return TX_SUCCESS;
}

UINT _nxe_dhcp_user_option_add_callback_set(NX_DHCP *dhcp_ptr, UINT (*dhcp_user_option_add)(NX_DHCP *dhcp_ptr, UINT iface_index, UINT message_type, UCHAR *user_option_ptr, UINT *user_option_length))
{
    (VOID)dhcp_ptr;
    (VOID)dhcp_user_option_add;

    return TX_SUCCESS;
}

UINT _nxe_dhcp_user_option_request(NX_DHCP *dhcp_ptr, UINT option_code)
{
    (VOID)dhcp_ptr;
    (VOID)option_code;

    return TX_SUCCESS;
}

UINT _nxe_ip_address_change_notify(NX_IP *ip_ptr, VOID (*ip_address_change_notify)(NX_IP *, VOID *), VOID *additional_info)
{
    (VOID)ip_ptr;
    (VOID)ip_address_change_notify;
    (VOID)additional_info;

    return TX_SUCCESS;
}

UINT _nxe_ip_address_get(NX_IP *ip_ptr, ULONG *ip_address, ULONG *network_mask)
{
    (VOID)ip_ptr;
    (VOID)ip_address;
    (VOID)network_mask;

    return TX_SUCCESS;
}

UINT _nxe_ip_delete(NX_IP *ip_ptr)
{
    (VOID)ip_ptr;

    return TX_SUCCESS;
}

UINT _nxe_ip_driver_interface_direct_command(NX_IP *ip_ptr, UINT command, UINT interface_index, ULONG *return_value_ptr)
{
    (VOID)ip_ptr;
    (VOID)command;
    (VOID)interface_index;
    (VOID)return_value_ptr;

    return TX_SUCCESS;
}

UINT _nxe_ip_fragment_enable(NX_IP *ip_ptr)
{
    (VOID)ip_ptr;

    return TX_SUCCESS;
}

UINT _nxe_ip_gateway_address_get(NX_IP *ip_ptr, ULONG *ip_address)
{
    (VOID)ip_ptr;
    (VOID)ip_address;

    return TX_SUCCESS;
}

UINT _nxe_ip_gateway_address_set(NX_IP *ip_ptr, ULONG ip_address)
{
    (VOID)ip_ptr;
    (VOID)ip_address;

    return TX_SUCCESS;
}

UINT _nxe_ip_interface_address_set(NX_IP *ip_ptr, UINT interface_index, ULONG ip_address, ULONG network_mask)
{
    (VOID)ip_ptr;
    (VOID)interface_index;
    (VOID)ip_address;
    (VOID)network_mask;

    return TX_SUCCESS;
}

UINT _nxe_ip_interface_attach(NX_IP *ip_ptr, CHAR *interface_name, ULONG ip_address, ULONG network_mask, VOID (*ip_link_driver)(struct NX_IP_DRIVER_STRUCT *))
{
    (VOID)ip_ptr;
    (VOID)interface_name;
    (VOID)ip_address;
    (VOID)network_mask;
    (VOID)ip_link_driver;

    return TX_SUCCESS;
}

UINT _nxe_ip_interface_detach(NX_IP *ip_ptr, UINT index)
{
    (VOID)ip_ptr;
    (VOID)index;

    return TX_SUCCESS;
}

UINT _nxe_ip_status_check(NX_IP *ip_ptr, ULONG needed_status, ULONG *actual_status, ULONG wait_option)
{
    (VOID)ip_ptr;
    (VOID)needed_status;
    (VOID)actual_status;
    (VOID)wait_option;

    return TX_SUCCESS;
}

UINT _nxe_packet_pool_delete(NX_PACKET_POOL *pool_ptr)
{
    (VOID)pool_ptr;

    return TX_SUCCESS;
}

UINT _nxe_tcp_enable(NX_IP *ip_ptr)
{
    (VOID)ip_ptr;

    return TX_SUCCESS;
}

UINT _nxe_udp_enable(NX_IP *ip_ptr)
{
    (VOID)ip_ptr;

    return TX_SUCCESS;
}

UINT _tx_thread_sleep(ULONG timer_ticks)
{
    (VOID)timer_ticks;

    return TX_SUCCESS;
}

UINT _txe_semaphore_create(TX_SEMAPHORE *semaphore_ptr, CHAR *name_ptr, ULONG initial_count, UINT semaphore_control_block_size)
{
    (VOID)semaphore_ptr;
    (VOID)name_ptr;
    (VOID)initial_count;
    (VOID)semaphore_control_block_size;

    return TX_SUCCESS;
}

UINT _txe_semaphore_delete(TX_SEMAPHORE *semaphore_ptr)
{
    (VOID)semaphore_ptr;

    return TX_SUCCESS;
}

UINT _txe_semaphore_get(TX_SEMAPHORE *semaphore_ptr, ULONG wait_option)
{
    (VOID)semaphore_ptr;
    (VOID)wait_option;

    return TX_SUCCESS;
}

UINT _txe_semaphore_put(TX_SEMAPHORE *semaphore_ptr)
{
    (VOID)semaphore_ptr;

    return TX_SUCCESS;
}

UINT _txe_timer_change(TX_TIMER *timer_ptr, ULONG initial_ticks, ULONG reschedule_ticks)
{
    (VOID)timer_ptr;
    (VOID)initial_ticks;
    (VOID)reschedule_ticks;

    return TX_SUCCESS;
}

UINT _txe_timer_create(TX_TIMER *timer_ptr, CHAR *name_ptr, VOID (*expiration_function)(ULONG input), ULONG expiration_input, ULONG initial_ticks, ULONG reschedule_ticks, UINT auto_activate, UINT timer_control_block_size)
{
    (VOID)timer_ptr;
    (VOID)name_ptr;
    (VOID)expiration_function;
    (VOID)expiration_input;
    (VOID)initial_ticks;
    (VOID)reschedule_ticks;
    (VOID)auto_activate;
    (VOID)timer_control_block_size;

    return TX_SUCCESS;
}

UINT _txe_timer_deactivate(TX_TIMER *timer_ptr)
{
    (VOID)timer_ptr;

    return TX_SUCCESS;
}

UINT _txe_timer_delete(TX_TIMER *timer_ptr)
{
    (VOID)timer_ptr;

    return TX_SUCCESS;
}

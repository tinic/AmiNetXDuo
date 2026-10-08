/* Lifetime gate around unchanged pinned IP create/delete/helper.
 * SPDX-License-Identifier: MIT */
#define NX_SOURCE_CODE
#include "exec_ip.h"
#include "tx_thread.h"
#include "tx_timer.h"
#include "tx_mutex.h"
#include "tx_event_flags.h"
#include "nx_ip.h"
#include "nx_packet.h"
#include "nx_system.h"
#include <exec/execbase.h>
#include <proto/exec.h>
static AnxExecIp *active;
static int idle(void)
{
    return SysBase->TDNestCnt<0 && SysBase->IDNestCnt<0 && anx_tx_runtime_idle();
}
static int disjoint(const VOID *a, ULONG as, const VOID *b, ULONG bs)
{
    uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;
    return x && y && x<=UINTPTR_MAX-as && y<=UINTPTR_MAX-bs &&
           (x+as<=y || y+bs<=x);
}
UINT anx_nx_original_ip_create(NX_IP *,CHAR *,ULONG,ULONG,NX_PACKET_POOL *,
                             VOID (*)(NX_IP_DRIVER *),VOID *,ULONG,UINT);
UINT anx_nx_original_ip_delete(NX_IP *);

/* Reject direct raw bypass before constructor memset or delete mutation. */
UINT _nx_ip_create(NX_IP *ip,CHAR *name,ULONG address,ULONG mask,NX_PACKET_POOL *pool,
                   VOID (*driver)(NX_IP_DRIVER *),VOID *stack,ULONG size,UINT priority)
{
    anx_tx_require_context(0);
    if (!active || active->ip!=ip || active->capability!=1 || active->state!=ANX_IP_PREPARED)
        return NX_NOT_ENABLED;
    active->capability=0; /* consume before invoking any downstream code */
    return anx_nx_original_ip_create(ip,name,address,mask,pool,driver,stack,size,priority);
}
UINT _nx_ip_delete(NX_IP *ip)
{
    anx_tx_require_context(0);
    if (!active || active->ip!=ip || active->capability!=2 || active->state!=ANX_IP_CLOSING)
        return NX_NOT_ENABLED;
    active->capability=0; /* callbacks cannot recursively consume this permit */
    return anx_nx_original_ip_delete(ip);
}
static int create_ready(TX_THREAD *caller,NX_PACKET_POOL *pool)
{
    return !(caller->tx_thread_id!=TX_THREAD_ID || caller->tx_thread_state!=TX_READY ||
        caller->tx_thread_priority!=caller->tx_thread_user_priority ||
        caller->tx_thread_inherit_priority!=TX_MAX_PRIORITIES ||
        caller->tx_thread_preempt_threshold!=caller->tx_thread_user_priority ||
        caller->tx_thread_owned_mutex_count || caller->tx_thread_owned_mutex_list ||
        _nx_ip_created_count || _nx_ip_created_ptr || _tx_thread_created_count || _tx_thread_created_ptr ||
        _tx_mutex_created_count || _tx_mutex_created_ptr ||
        _tx_event_flags_created_count || _tx_event_flags_created_ptr ||
        _tx_timer_created_count || _tx_timer_created_ptr || _tx_thread_preempt_disable ||
        caller->tx_thread_user_preempt_threshold!=caller->tx_thread_user_priority ||
        pool->nx_packet_pool_id!=NX_PACKET_POOL_ID || pool->nx_packet_pool_suspended_count ||
        pool->nx_packet_pool_suspension_list || pool->nx_packet_pool_available!=pool->nx_packet_pool_total ||
        !(_nx_system_build_options_1|_nx_system_build_options_2|_nx_system_build_options_3|
          _nx_system_build_options_4|_nx_system_build_options_5));
}
UINT anx_exec_ip_create(AnxExecIp *r,TX_THREAD *caller,NX_IP *ip,CHAR *name,NX_PACKET_POOL *pool,
                        VOID (*driver)(NX_IP_DRIVER *),VOID *stack,ULONG size,UINT priority)
{
    AnxTxContext f;
    UINT status;
    if (!idle()) return NX_CALLER_ERROR;
    if (!r || !caller || !ip || !name || !pool || !driver || !stack) return NX_PTR_ERROR;
    if (caller->tx_thread_amiga_task!=(VOID *)FindTask(0)) return NX_CALLER_ERROR;
    if (active || (r->state!=ANX_IP_EMPTY && r->state!=ANX_IP_REAPED)) return NX_NOT_ENABLED;
    if (priority>=TX_MAX_PRIORITIES || priority>caller->tx_thread_user_priority) return NX_NOT_ENABLED;
    if (!disjoint(stack,size,r,sizeof(*r)) || !disjoint(r,sizeof(*r),ip,sizeof(*ip)) ||
        !disjoint(stack,size,ip,sizeof(*ip)) ||
        !disjoint(r,sizeof(*r),pool,sizeof(*pool)) || !disjoint(stack,size,pool,sizeof(*pool)) ||
        !disjoint(ip,sizeof(*ip),pool,sizeof(*pool)) || !disjoint(ip,sizeof(*ip),caller,sizeof(*caller)) ||
        !disjoint(r,sizeof(*r),caller,sizeof(*caller)) || !disjoint(stack,size,caller,sizeof(*caller))) return NX_PTR_ERROR;
    anx_tx_context_begin(&f,caller,0);
    if (!create_ready(caller,pool) ||
        !disjoint(ip,sizeof(*ip),pool->nx_packet_pool_start,pool->nx_packet_pool_size) ||
        !disjoint(r,sizeof(*r),pool->nx_packet_pool_start,pool->nx_packet_pool_size) ||
        !disjoint(stack,size,pool->nx_packet_pool_start,pool->nx_packet_pool_size)) {
        anx_tx_context_end(&f);return NX_NOT_ENABLED;
    }
    anx_tx_context_end(&f);
    if (!anx_exec_thread_prepare(&r->helper,&ip->nx_ip_thread,name,stack,size)) return NX_NOT_ENABLED;
    anx_tx_context_begin(&f,caller,0);
    /* Prepare waited outside serialization; revalidate before raw memset. */
    if (!create_ready(caller,pool) || active) {
        anx_tx_context_end(&f);
        if (!anx_exec_thread_cancel(&r->helper)) anx_tx_unsupported("IP reservation rollback failed");
        return NX_NOT_ENABLED;
    }
    r->ip=ip;r->pool=pool;r->driver=driver;r->caller=caller;r->creator=FindTask(0);r->clock=0;
    r->state=ANX_IP_PREPARED;active=r;
    r->capability=1;
    status=_nx_ip_create(ip,name,IP_ADDRESS(192,0,2,1),0xffffff00UL,pool,driver,stack,size,priority);
    r->capability=0;
    if (status!=NX_SUCCESS || ip->nx_ip_id!=NX_IP_ID || _nx_ip_created_ptr!=ip || _nx_ip_created_count!=1 ||
        r->helper.state!=ANX_THREAD_BOUND || r->helper.entered || ip->nx_ip_thread.tx_thread_entry!=_nx_ip_thread_entry ||
        ip->nx_ip_thread.tx_thread_state!=TX_READY || ip->nx_ip_protection.tx_mutex_id!=TX_MUTEX_ID ||
        ip->nx_ip_events.tx_event_flags_group_id!=TX_EVENT_FLAGS_ID ||
        ip->nx_ip_periodic_timer.tx_timer_id!=TX_TIMER_ID || _tx_timer_created_count!=1 ||
        _tx_mutex_created_count!=1 || _tx_event_flags_created_count!=1 || _tx_thread_created_count!=1 ||
        caller->tx_thread_preempt_threshold!=caller->tx_thread_user_priority)
        anx_tx_unsupported("real IP create failed supported contract");
    r->state=ANX_IP_LIVE;anx_tx_context_end(&f);
    return status;
}
UINT anx_exec_ip_clock_start(AnxExecIp *r,AnxExecClock *clock,CHAR *name,VOID *stack,ULONG size)
{
    if (!idle() || !r || r->creator!=FindTask(0)) return NX_CALLER_ERROR;
    if (active!=r || r->state!=ANX_IP_LIVE || r->clock) return NX_NOT_ENABLED;
    if (!clock || !name || !stack) return NX_PTR_ERROR;
    const VOID *regions[]={r,r->ip,r->pool,r->caller,r->helper.stack,r->pool->nx_packet_pool_start};
    const ULONG sizes[]={sizeof(*r),sizeof(*r->ip),sizeof(*r->pool),sizeof(*r->caller),
                        r->helper.stack_size,r->pool->nx_packet_pool_size};
    for (unsigned i=0;i<sizeof(regions)/sizeof(*regions);i++)
        if (!disjoint(clock,sizeof(*clock),regions[i],sizes[i]) ||
            !disjoint(stack,size,regions[i],sizes[i])) return NX_PTR_ERROR;
    /* Only this creator can alter the experimental IP lifetime. Native start
     * may advance timers, but waits outside serialization and retains all IP
     * storage. No other API/packet producers are admitted by this contract. */
    if (!anx_exec_clock_start(clock,name,stack,size)) return NX_NOT_ENABLED;
    AnxTxContext f;anx_tx_context_begin(&f,r->caller,0);r->clock=clock;anx_tx_context_end(&f);
    return NX_SUCCESS;
}
UINT anx_exec_ip_event(AnxExecIp *r,ULONG flags)
{
    anx_tx_require_context(0);
    if (!r || active!=r || r->state!=ANX_IP_LIVE) return NX_NOT_ENABLED;
    return tx_event_flags_set(&r->ip->nx_ip_events,flags,TX_OR);
}
static int delete_ready(NX_IP *ip)
{
    if (ip->nx_ip_default_packet_pool!=active->pool ||
        ip->nx_ip_interface[0].nx_interface_link_driver_entry!=active->driver ||
        !ip->nx_ip_interface[0].nx_interface_valid) return 0;
    for (UINT i=1;i<NX_MAX_PHYSICAL_INTERFACES;i++)
        if (ip->nx_ip_interface[i].nx_interface_valid) return 0;
    return !(ip->nx_ip_udp_created_sockets_count || ip->nx_ip_tcp_created_sockets_count ||
        ip->nx_ip_id!=NX_IP_ID || _nx_ip_created_count!=1 || _nx_ip_created_ptr!=ip ||
        _tx_mutex_created_count!=1 || _tx_mutex_created_ptr!=&ip->nx_ip_protection ||
        _tx_event_flags_created_count!=1 || _tx_event_flags_created_ptr!=&ip->nx_ip_events ||
        _tx_timer_created_count!=1 || _tx_timer_created_ptr!=&ip->nx_ip_periodic_timer ||
        _tx_thread_created_count!=1 || _tx_thread_created_ptr!=&ip->nx_ip_thread ||
        ip->nx_ip_protection.tx_mutex_id!=TX_MUTEX_ID || ip->nx_ip_events.tx_event_flags_group_id!=TX_EVENT_FLAGS_ID ||
        ip->nx_ip_periodic_timer.tx_timer_id!=TX_TIMER_ID ||
        ip->nx_ip_protection.tx_mutex_owned_next || ip->nx_ip_protection.tx_mutex_owned_previous ||
        ip->nx_ip_events.tx_event_flags_group_reset_search || ip->nx_ip_events.tx_event_flags_group_delayed_clear ||
        ip->nx_ip_fragment_processing || ip->nx_ip_tcp_packet_receive || ip->nx_ip_tcp_periodic_processing ||
        ip->nx_ip_tcp_fast_periodic_processing || ip->nx_ip_tcp_queue_process || ip->nx_ip_udp_packet_receive ||
        ip->nx_ip_icmp_queue_process || ip->nx_ip_igmp_queue_process || ip->nx_ip_igmp_periodic_processing ||
        ip->nx_ip_arp_allocate || ip->nx_ip_arp_queue_process ||
        ip->nx_ip_arp_periodic_update || ip->nx_ip_rarp_queue_process || ip->nx_ip_rarp_periodic_update ||
        ip->nx_ip_protection.tx_mutex_owner || ip->nx_ip_protection.tx_mutex_ownership_count ||
        ip->nx_ip_protection.tx_mutex_suspended_count || ip->nx_ip_protection.tx_mutex_suspension_list ||
        ip->nx_ip_fast_periodic_timer_created || ip->nx_ip_raw_packet_suspension_list ||
        ip->nx_ip_icmp_ping_suspension_list || ip->nx_ip_raw_received_packet_head ||
        ip->nx_ip_deferred_received_packet_head || ip->nx_ip_icmp_queue_head || ip->nx_ip_igmp_queue_head ||
        ip->nx_ip_arp_deferred_received_packet_head || ip->nx_ip_rarp_deferred_received_packet_head ||
        ip->nx_ip_tcp_queue_head || ip->nx_ip_default_packet_pool->nx_packet_pool_suspended_count ||
        ip->nx_ip_default_packet_pool->nx_packet_pool_suspension_list ||
        ip->nx_ip_default_packet_pool->nx_packet_pool_available!=ip->nx_ip_default_packet_pool->nx_packet_pool_total);
}
UINT anx_exec_ip_delete(AnxExecIp *r)
{
    AnxTxContext f;
    NX_IP *ip;
    UINT status;
    if (!idle() || !r || r->creator!=FindTask(0)) return NX_CALLER_ERROR;
    if (active!=r || r->state!=ANX_IP_LIVE) return NX_NOT_ENABLED;
    ip=r->ip;anx_tx_context_begin(&f,r->caller,0);
    if (ip->nx_ip_udp_created_sockets_count || ip->nx_ip_tcp_created_sockets_count) {
        anx_tx_context_end(&f);return NX_SOCKETS_BOUND;
    }
    /* Strict one-IP proof: only the explicitly associated clock is admitted.
     * No queue-clear callbacks or pool waiter resumes in this first contract. */
    if (!delete_ready(ip) || (r->clock && !anx_exec_clock_can_stop(r->clock)) ||
        !anx_exec_thread_stop_event(&r->helper,&ip->nx_ip_events)) {
        anx_tx_context_end(&f);return NX_NOT_ENABLED;
    }
    r->state=ANX_IP_CLOSING;
    if (r->clock && !anx_exec_clock_stop(r->clock))
        anx_tx_unsupported("IP clock pre-stop failed");
    if (tx_timer_deactivate(&ip->nx_ip_periodic_timer)!=TX_SUCCESS)
        anx_tx_unsupported("IP periodic timer pre-stop deactivate failed");
    anx_tx_context_end(&f);
    if (r->clock && !anx_exec_clock_join(r->clock))
        anx_tx_unsupported("IP clock native retirement failed");
    if (!anx_exec_thread_wait(&r->helper)) anx_tx_unsupported("IP helper native retirement failed");
    anx_tx_context_begin(&f,r->caller,0);
    if (!delete_ready(ip) || r->helper.state!=ANX_THREAD_FINISHED || r->helper.wait.opened ||
        ip->nx_ip_events.tx_event_flags_group_suspended_count || ip->nx_ip_protection.tx_mutex_owner)
        anx_tx_unsupported("IP delete after acknowledgement lost quiescence");
    r->capability=2;status=_nx_ip_delete(ip);r->capability=0;
    if (status!=NX_SUCCESS || ip->nx_ip_id || _nx_ip_created_count || _tx_thread_created_count ||
        _tx_mutex_created_count || _tx_event_flags_created_count || _tx_timer_created_count ||
        r->helper.state!=ANX_THREAD_REAPED)
        anx_tx_unsupported("real IP delete failed supported contract");
    r->state=ANX_IP_REAPED;active=0;anx_tx_context_end(&f);
    return status;
}

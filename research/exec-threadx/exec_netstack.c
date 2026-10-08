/* Retained Exec-wait brackets, health and actual counters. No ready-list baton.
 * SPDX-License-Identifier: MIT */
#include "exec_netstack.h"
#include "aminetxduo/compat.h"
#include <proto/exec.h>
#include "tx_thread.h"
AmiBatonStats ami_baton_stats;
static AmiHealthMark health;
static char health_name[]=AMI_HEALTH_NAME;
static unsigned health_up;
static VOID (*sampler)(VOID);
VOID ami_netstack_baton_set_sampler(VOID (*fn)(VOID)) {Forbid();sampler=fn;Permit();}
static void observe(void)
{
    if (_tx_thread_system_state>ami_baton_stats.bs_StateMax) ami_baton_stats.bs_StateMax=_tx_thread_system_state;
    if (_tx_thread_system_state) ami_baton_stats.bs_StateShared++;
    if (sampler) sampler();
}
VOID ami_netstack_baton_release(VOID)
{
    unsigned nesting;
    if (!anx_tx_exec_wait_state(&nesting)) return; /* plain/dormant Exec caller */
    if (!nesting) {
        Forbid();observe();ami_baton_stats.bs_Live++;
        if (ami_baton_stats.bs_Live>ami_baton_stats.bs_LiveMax) ami_baton_stats.bs_LiveMax=ami_baton_stats.bs_Live;
        Permit();
    }
    if (!anx_tx_context_pause()) anx_tx_unsupported("production Exec-wait release refused");
}
VOID ami_netstack_baton_acquire(VOID)
{
    unsigned nesting;
    if (!anx_tx_exec_wait_state(&nesting) || !nesting) return;
    Forbid();observe();Permit();
    if (!anx_tx_context_resume()) anx_tx_unsupported("production Exec-wait restore refused");
    if (nesting==1) {
        if (!ami_baton_stats.bs_Live) anx_tx_unsupported("production Exec-wait count underflow");
        ami_baton_stats.bs_Live--;ami_baton_stats.bs_Transitions++;
    }
}
BOOL ami_netstack_baton_abandon(TX_THREAD *thread)
{
    if (anx_tx_thread_paused(thread)) anx_tx_unsupported("removed paused owner reclamation not implemented");
    return FALSE; /* No bracket existed; do not fabricate foreign cleanup. */
}
BOOL ami_netstack_baton_reclaim_dead(VOID)
{
    AnxCallerStats before,after;
    anx_exec_callers_stats(&before);tx_amiga_adopt_sweep_unpublished();anx_exec_callers_stats(&after);
    Forbid();ami_baton_stats.bs_Reclaimed+=(ULONG)(after.reclaimed-before.reclaimed);Permit();
    return after.reclaimed!=before.reclaimed;
}
VOID ami_netstack_baton_reset(VOID)
{
    Forbid();
    if (ami_baton_stats.bs_Live || tx_amiga_kernel_running()) anx_tx_unsupported("production hooks reset before retirement");
    sampler=0;Permit();
}
VOID ami_netstack_health_set_sblock(APTR lock) {Forbid();health.hm_SbLock=lock;Permit();}
VOID ami_netstack_health_publish(VOID)
{
    Forbid();
    if (health_up) {Permit();return;}
    health.hm_Magic=AMI_HEALTH_MAGIC;health.hm_Version=AMI_HEALTH_VERSION;health.hm_Size=sizeof(health);
    health.hm_Tick=tx_amiga_tick_stats_live();health.hm_Baton=&ami_baton_stats;
    health.hm_Mem=ami_mem_stats();health.hm_Holder=&anx_tx_holder;
    InitSemaphore(&health.hm_Semaphore);health.hm_Semaphore.ss_Link.ln_Name=health_name;
    if (!FindSemaphore((STRPTR)health_name)) {AddSemaphore(&health.hm_Semaphore);health_up=1;}
    Permit();
}
VOID ami_netstack_health_unpublish(VOID)
{
    Forbid();if (health_up) {RemSemaphore(&health.hm_Semaphore);health_up=0;}Permit();
}

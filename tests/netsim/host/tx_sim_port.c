/*
 * AmiNetXDuo, host network simulator: the ThreadX port (shim/tx_port.h).
 *
 * The CPU is a token.  sim_owner names the thread that holds it, or nothing,
 * which means the scheduler does; whoever does not hold it waits on its own
 * condition variable.  So exactly one of these host threads executes ThreadX,
 * NetX Duo or simulator code at any moment, and which one is decided by
 * ThreadX's own ready list -- the order a run takes is the kernel's, not the
 * host's.
 *
 * Interrupts are the scheduler's: with no thread ready it calls
 * netsim_port_idle(), which moves the virtual clock to the next event and
 * handles it with _tx_thread_system_state raised, as an ISR would run.
 *
 * SPDX-License-Identifier: MIT
 */

#define TX_SOURCE_CODE

#include "tx_api.h"
#include "tx_initialize.h"
#include "tx_thread.h"
#include "tx_timer.h"

#include <stdio.h>

#include "netsim.h"

static pthread_mutex_t  sim_lock  = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t   sim_sched = PTHREAD_COND_INITIALIZER;
static TX_THREAD       *sim_owner;

/* What tx_application_define() is handed as first unused memory. */
static ALIGN_TYPE       sim_free_memory[1024];

VOID _tx_initialize_low_level(VOID)
{
    _tx_initialize_unused_memory = (VOID *)sim_free_memory;
}

UINT _tx_thread_interrupt_control(UINT new_posture)
{
    (VOID)new_posture;
    return TX_INT_ENABLE;
}

#define SIM_LIVE        1U
#define SIM_RETIRING    2U

/* Wait, with sim_lock held, until `self` holds the CPU.  A thread retired
   while it waited leaves here and exits. */
static VOID sim_wait_turn(TX_THREAD *self)
{
    while (sim_owner != self)
    {
        if (self->tx_thread_sim_live == SIM_RETIRING)
        {
            pthread_mutex_unlock(&sim_lock);
            pthread_exit(NULL);
        }
        pthread_cond_wait(&self->tx_thread_sim_run, &sim_lock);
    }
}

/* Hand the CPU to `next` (NULL: the scheduler) and wait until `self` has it
   back.  sim_lock is held. */
static VOID sim_hand_over(TX_THREAD *self, TX_THREAD *next)
{
    sim_owner = next;

    if (next != TX_NULL)
        pthread_cond_signal(&next->tx_thread_sim_run);
    else
        pthread_cond_signal(&sim_sched);

    if (self != TX_NULL)
        sim_wait_turn(self);
    else
    {
        while (sim_owner != TX_NULL)
            pthread_cond_wait(&sim_sched, &sim_lock);
    }
}

static void *sim_thread_main(void *arg)
{
    TX_THREAD *self = (TX_THREAD *)arg;

    pthread_mutex_lock(&sim_lock);
    sim_wait_turn(self);
    pthread_mutex_unlock(&sim_lock);

    /* Runs the entry function; a thread that returns from it is completed
       and suspends itself, so this never comes back. */
    _tx_thread_shell_entry();

    return NULL;
}

/* tx_thread_delete() and tx_thread_reset(), from the thread that called
   them: the host thread of `thread_ptr` is parked, never to be scheduled
   again, so it is woken to exit and joined before the block is reused. */
VOID _tx_sim_thread_retire(TX_THREAD *thread_ptr)
{
    if (thread_ptr->tx_thread_sim_live != SIM_LIVE)
        return;

    pthread_mutex_lock(&sim_lock);
    thread_ptr->tx_thread_sim_live = SIM_RETIRING;
    pthread_cond_signal(&thread_ptr->tx_thread_sim_run);
    pthread_mutex_unlock(&sim_lock);

    pthread_join(thread_ptr->tx_thread_sim_host, NULL);
    pthread_cond_destroy(&thread_ptr->tx_thread_sim_run);
    thread_ptr->tx_thread_sim_live = 0;
}

VOID _tx_thread_stack_build(TX_THREAD *thread_ptr, VOID (*function_ptr)(VOID))
{
    pthread_attr_t attr;

    (VOID)function_ptr;

    thread_ptr->tx_thread_sim_live = 0;
    pthread_cond_init(&thread_ptr->tx_thread_sim_run, NULL);

    /* NetX Duo's receive path nests deep enough that the host default for
       a second thread (512 KB on macOS) is the floor worth having. */
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 1024UL * 1024UL);

    if (pthread_create(&thread_ptr->tx_thread_sim_host, &attr,
                       sim_thread_main, thread_ptr) == 0)
        thread_ptr->tx_thread_sim_live = SIM_LIVE;

    pthread_attr_destroy(&attr);

    /* The ThreadX stack is never run on; the field only has to be set. */
    thread_ptr->tx_thread_stack_ptr = thread_ptr->tx_thread_stack_end;
}

VOID _tx_thread_system_return(VOID)
{
    TX_THREAD *self = _tx_thread_current_ptr;

    if (_tx_timer_time_slice)
    {
        self->tx_thread_time_slice = _tx_timer_time_slice;
        _tx_timer_time_slice = 0;
    }

    _tx_thread_current_ptr = TX_NULL;

    pthread_mutex_lock(&sim_lock);
    sim_hand_over(self, TX_NULL);
    pthread_mutex_unlock(&sim_lock);
}

VOID _tx_thread_schedule(VOID)
{
    for (;;)
    {
        TX_THREAD *next;

        while (_tx_thread_execute_ptr == TX_NULL)
        {
            _tx_thread_system_state++;
            netsim_port_idle();
            _tx_thread_system_state--;
        }

        next = _tx_thread_execute_ptr;
        _tx_thread_current_ptr = next;
        next->tx_thread_run_count++;
        _tx_timer_time_slice = next->tx_thread_time_slice;

        pthread_mutex_lock(&sim_lock);
        sim_hand_over(TX_NULL, next);
        pthread_mutex_unlock(&sim_lock);
    }
}

/* One tick, from netsim_port_idle(), the Linux port's body without its lock. */
VOID _tx_timer_interrupt(VOID)
{
    _tx_timer_system_clock++;

    if (_tx_timer_time_slice)
    {
        _tx_timer_time_slice--;
        if (_tx_timer_time_slice == 0)
            _tx_timer_expired_time_slice = TX_TRUE;
    }

    if (*_tx_timer_current_ptr)
    {
        _tx_timer_expired = TX_TRUE;
    }
    else
    {
        _tx_timer_current_ptr++;
        if (_tx_timer_current_ptr == _tx_timer_list_end)
            _tx_timer_current_ptr = _tx_timer_list_start;
    }

    if (_tx_timer_expired)
        _tx_timer_expiration_process();

    if (_tx_timer_expired_time_slice)
        _tx_thread_time_slice();
}

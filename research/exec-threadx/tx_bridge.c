/* Limited implementation behind real ThreadX headers, for suspension research.
 * SPDX-License-Identifier: MIT */
#define TX_SOURCE_CODE
#include "tx_bridge.h"
#include "tx_thread.h"
#include "tx_timer.h"
#include "tx_mutex.h"
#include "tx_event_flags.h"
#include <string.h>

TX_THREAD *_tx_thread_current_ptr;
volatile ULONG _tx_thread_system_state;
volatile UINT _tx_thread_preempt_disable;
void (*anx_tx_after_mutex_put)(TX_MUTEX *);

static const AnxTxPlatform *platform;
static AnxTxThread *threads;
static TX_TIMER_INTERNAL *timers;
static unsigned contexts;
static AnxTxContext *current_frame;

static void need(int condition, const char *message)
{
    if (!condition) {
        platform->panic(platform->context, message);
        for (;;) {} /* a returning panic must not continue with corrupt state */
    }
}

static AnxTxThread *find(TX_THREAD *thread)
{
    AnxTxThread *t;
    for (t = threads; t; t = t->next)
        if (t->thread == thread)
            return t;
    need(0, "unregistered ThreadX control block");
    return 0;
}

UINT anx_tx_host_disable(void)
{
    platform->enter(platform->context);
    return 0;
}

void anx_tx_host_restore(UINT posture)
{
    (void)posture;
    platform->leave(platform->context);
}

void anx_tx_runtime_init(const AnxTxPlatform *p)
{
    /* Reinitialization is allowed only after every owner has detached. */
    if (platform)
        need(!threads && !timers && !contexts && !current_frame &&
             !_tx_thread_preempt_disable, "reinitialize active domain");
    platform = p;
    threads = 0;
    timers = 0;
    contexts = 0;
    current_frame = 0;
    _tx_thread_current_ptr = 0;
    _tx_thread_system_state = 0;
    _tx_thread_preempt_disable = 0;
    anx_tx_after_mutex_put = 0;
}

int anx_tx_attach(AnxTxThread *t, TX_THREAD *thread, AnxWait *wait, uintptr_t owner)
{
    AnxTxThread *other;
    platform->enter(platform->context);
    for (other = threads; other; other = other->next)
        if (other == t || other->thread == thread || other->owner == owner) {
            platform->leave(platform->context);
            return 0;
        }
    if (!owner || platform->caller(platform->context) != owner ||
        wait->result == ANX_WAIT_PENDING) {
        platform->leave(platform->context);
        return 0;
    }
    memset(thread, 0, sizeof(*thread));
    thread->tx_thread_id = TX_THREAD_ID;
    thread->tx_thread_state = TX_READY;
    thread->tx_thread_amiga_task = (VOID *)owner;
    *t = (AnxTxThread){.thread=thread, .wait=wait, .owner=owner, .next=threads};
    threads = t;
    platform->leave(platform->context);
    return 1;
}

int anx_tx_detach(AnxTxThread *t)
{
    AnxTxThread **link;
    platform->enter(platform->context);
    if (platform->caller(platform->context) != t->owner ||
        t->wait->result == ANX_WAIT_PENDING || t->thread->tx_thread_state != TX_READY ||
        t->thread->tx_thread_suspend_cleanup || t->pending_resume || t->pending_token ||
        t->thread->tx_thread_timer.tx_timer_internal_list_head ||
        _tx_thread_current_ptr == t->thread) {
        platform->leave(platform->context);
        return 0;
    }
    for (link=&threads; *link && *link!=t; link=&(*link)->next) {}
    if (!*link) {
        platform->leave(platform->context);
        return 0;
    }
    *link=t->next;
    t->thread->tx_thread_id=0;
    platform->leave(platform->context);
    return 1;
}

int anx_tx_set_resume_cleanup(AnxTxThread *t, int (*hook)(AnxTxThread *))
{
    int valid;
    platform->enter(platform->context);
    valid=find(t->thread)==t && platform->caller(platform->context)==t->owner &&
        t->thread->tx_thread_state==TX_READY && !t->thread->tx_thread_suspend_cleanup &&
        !t->pending_resume && !t->pending_token && t->wait->result!=ANX_WAIT_PENDING;
    if (valid) t->resume_cleanup=hook;
    platform->leave(platform->context);
    return valid;
}

void anx_tx_context_begin(AnxTxContext *frame, TX_THREAD *thread, ULONG state)
{
    platform->enter(platform->context);
    frame->owner = platform->caller(platform->context);
    if (thread)
        need(find(thread)->owner == frame->owner, "foreign caller identity");
    if (!contexts)
        need(!_tx_thread_preempt_disable && !_tx_thread_current_ptr &&
             !_tx_thread_system_state, "stale call context");
    frame->previous = current_frame;
    current_frame = frame;
    frame->saved_thread = _tx_thread_current_ptr;
    frame->saved_state = _tx_thread_system_state;
    contexts++;
    _tx_thread_current_ptr=thread;
    _tx_thread_system_state=state;
}

void anx_tx_context_end(AnxTxContext *frame)
{
    need(contexts && current_frame == frame &&
         frame->owner == platform->caller(platform->context), "context owner/order");
    need(!_tx_thread_preempt_disable, "unbalanced preemption counter");
    if (contexts==1) {
        AnxTxThread *t;
        /* A ThreadX abort can mark READY while NetX's deferred sentinel still
         * owns a node. Only actual cleanup, under a later producer boundary,
         * makes the owner runnable. No cleanup callback is forged here. */
        for (t=threads;t;t=t->next) {
            if (t->pending_resume && !t->thread->tx_thread_suspend_cleanup) {
                need(t->thread->tx_thread_state==TX_READY,"pending resume state changed");
                need(t->pending_token==t->token && t->token==t->wait->generation,
                     "pending resume generation changed");
                t->pending_resume=0;
                t->pending_token=0;
                if (t->token)
                    (void)anx_wait_complete(t->wait,t->token,ANX_WAIT_READY);
            }
        }
    }
    _tx_thread_current_ptr=frame->saved_thread;
    _tx_thread_system_state=frame->saved_state;
    contexts--;
    current_frame = frame->previous;
    platform->leave(platform->context);
}

UINT tx_amiga_caller_is_thread(void)
{
    UINT result;
    platform->enter(platform->context);
    result = contexts && current_frame && _tx_thread_current_ptr &&
        !_tx_thread_system_state &&
        find(_tx_thread_current_ptr)->owner == platform->caller(platform->context);
    platform->leave(platform->context);
    return result;
}

TX_THREAD *_tx_thread_identify(void)
{
    return tx_amiga_caller_is_thread() ? _tx_thread_current_ptr : TX_NULL;
}

/* A backend-owned active timer list, using the real internal fields. Private
 * wait IO provides the deadline; cancellation wakes it and park reaps the IO.
 * External application TX_TIMER objects are not implemented by this spike. */
VOID _tx_timer_system_activate(TX_TIMER_INTERNAL *timer)
{
    need(!timer->tx_timer_internal_list_head, "timer already active");
    timer->tx_timer_internal_active_next=timers ? timers : timer;
    timer->tx_timer_internal_active_previous=timers ? timers->tx_timer_internal_active_previous : timer;
    if (timers) {
        timers->tx_timer_internal_active_previous->tx_timer_internal_active_next=timer;
        timers->tx_timer_internal_active_previous=timer;
    } else timers=timer;
    timer->tx_timer_internal_list_head=&timers;
}

VOID _tx_timer_system_deactivate(TX_TIMER_INTERNAL *timer)
{
    if (timer->tx_timer_internal_list_head) {
        need(timer->tx_timer_internal_list_head == &timers, "foreign timer domain");
        if (timer->tx_timer_internal_active_next == timer) timers=0;
        else {
            timer->tx_timer_internal_active_next->tx_timer_internal_active_previous=timer->tx_timer_internal_active_previous;
            timer->tx_timer_internal_active_previous->tx_timer_internal_active_next=timer->tx_timer_internal_active_next;
            if (timers==timer) timers=timer->tx_timer_internal_active_next;
        }
        timer->tx_timer_internal_list_head=0;
    }
    timer->tx_timer_internal_remaining_ticks=0;
}

int anx_tx_expire(TX_THREAD *thread, uint32_t token)
{
    AnxTxThread *t=find(thread);
    need(contexts && _tx_thread_system_state, "timeout outside marked timer context");
    if (t->token!=token || t->expiry_dispatched ||
        !thread->tx_thread_timer.tx_timer_internal_list_head ||
        thread->tx_thread_state==TX_READY)
        return 0;
    t->expiry_dispatched=1;
    _tx_timer_system_deactivate(&thread->tx_thread_timer);
    (void)anx_wait_complete(t->wait,token,ANX_WAIT_TIMEOUT);
    /* This callback is deliberately separate from the primitive's protected
     * queue-removal hook. Real NetX TCP cleanup defers work to its IP context. */
    _tx_thread_timeout((ULONG)(uintptr_t)thread);
    return 1;
}

VOID _tx_thread_system_suspend(TX_THREAD *thread)
{
    AnxTxThread *t=find(thread);
    need(!t->pending_resume && !t->pending_token,"new suspension retained pending wake");
    ULONG ticks=thread->tx_thread_timer.tx_timer_internal_remaining_ticks;
    AnxTxContext *outer_frame=current_frame;
    unsigned awaiting_cleanup=0;
    need(contexts==1 && !_tx_thread_system_state && _tx_thread_current_ptr==thread &&
         t->owner==platform->caller(platform->context), "unsupported blocking context");
    need(_tx_thread_preempt_disable>0, "suspend missing preemption increment");
    _tx_thread_preempt_disable--;
    need(!_tx_thread_preempt_disable, "nested preemption blocking");
    if (!thread->tx_thread_suspending) {
        need(thread->tx_thread_state==TX_READY && !thread->tx_thread_suspend_cleanup,
             "early resume left cleanup pending");
        return;
    }
    need(ticks!=TX_NO_WAIT, "zero timeout suspension");
    t->cleanup_at_suspend=thread->tx_thread_suspend_cleanup;
    t->control_at_suspend=thread->tx_thread_suspend_control_block;
    t->sequence_at_suspend=thread->tx_thread_suspension_sequence;
    thread->tx_thread_suspending=TX_FALSE;
    t->expiry_dispatched=0;
    t->token=anx_wait_begin(t->wait,ticks==TX_WAIT_FOREVER ? ANX_WAIT_FOREVER :
                          (uint64_t)ticks*1000000/TX_TIMER_TICKS_PER_SECOND,0,0,0,0);
    need(t->token!=0,"wait setup failed");
    if (ticks!=TX_WAIT_FOREVER) _tx_timer_system_activate(&thread->tx_thread_timer);
    while (thread->tx_thread_state!=TX_READY || thread->tx_thread_suspend_cleanup) {
        AnxWaitResult result;
        /* Drop precisely the outer call boundary. Every TX_DISABLE inside
         * NetX has been restored; no foreign current pointer remains live. */
        _tx_thread_current_ptr=0;
        contexts=0;
        current_frame=0;
        platform->leave(platform->context);
        t->parks++;
        result=anx_wait_run(t->wait,t->token);
        platform->enter(platform->context);
        need(!contexts && !current_frame && !_tx_thread_current_ptr && !_tx_thread_system_state &&
             !_tx_thread_preempt_disable,"foreign context retained across yield");
        contexts=1;
        current_frame=outer_frame;
        _tx_thread_current_ptr=thread;
        if (thread->tx_thread_state==TX_READY && !thread->tx_thread_suspend_cleanup) break;
        need(result==ANX_WAIT_TIMEOUT,"wait ended without ThreadX resume");
        need(!awaiting_cleanup,"cleanup did not complete within research grace");
        if (!t->expiry_dispatched) {
            AnxTxContext timer_context;
            anx_tx_context_begin(&timer_context,TX_NULL,1);
            need(anx_tx_expire(thread,t->token),"timeout dispatch rejected");
            anx_tx_context_end(&timer_context);
        }
        /* Deferred cleanup must really resume the thread before returning.
         * Do not manufacture NX_NO_PACKET or leave a live suspension node. */
        if (thread->tx_thread_state!=TX_READY || thread->tx_thread_suspend_cleanup) {
            t->token=anx_wait_begin(t->wait,ANX_TX_CLEANUP_GRACE_US,0,0,0,0);
            need(t->token!=0,"deferred cleanup wait setup failed");
            if (t->pending_resume) t->pending_token=t->token;
            awaiting_cleanup=1;
        }
    }
    /* READY alone is not sufficient: the original NetX node must be removed.
     * An outer producer boundary releases gated wakes after real cleanup. */
    need(!thread->tx_thread_suspend_cleanup,"resume left cleanup pending");
}

VOID _tx_thread_system_resume(TX_THREAD *thread)
{
    AnxTxThread *t=find(thread);
    need(contexts && _tx_thread_preempt_disable>0,"resume missing protected preemption increment");
    _tx_thread_preempt_disable--;
    if (thread->tx_thread_suspend_cleanup && t->resume_cleanup)
        need(t->resume_cleanup(t),"resume cleanup integration rejected");
    _tx_timer_system_deactivate(&thread->tx_thread_timer);
    if (thread->tx_thread_state==TX_READY) return;
    thread->tx_thread_suspending=TX_FALSE;
    thread->tx_thread_state=TX_READY;
    t->resumes++;
    if (thread->tx_thread_suspend_cleanup) {
        t->pending_resume=1;
        t->pending_token=t->token;
    }
    else if (t->token)
        (void)anx_wait_complete(t->wait,t->token,ANX_WAIT_READY);
}

VOID _tx_thread_system_preempt_check(void)
{
    /* The serialized call boundary defers Exec dispatch until its Permit.
     * This is not a ThreadX ready-list/priority-threshold implementation. */
    need(contexts!=0,"preemption check outside call boundary");
}

static void need_context(void)
{
    need(contexts && current_frame &&
         current_frame->owner==platform->caller(platform->context),
         "service outside serialized call boundary");
}

/* Only non-inheriting, uncontended/recursive mutex operations are implemented.
 * Missing blocking/get/event services remain absent from the link contract. */
UINT _tx_mutex_create(TX_MUTEX *m, CHAR *name, UINT inherit)
{
    need_context();
    if (inherit!=TX_NO_INHERIT) return TX_FEATURE_NOT_ENABLED;
    memset(m,0,sizeof(*m)); m->tx_mutex_name=name; m->tx_mutex_id=TX_MUTEX_ID;
    return TX_SUCCESS;
}

UINT _tx_mutex_get(TX_MUTEX *m, ULONG wait)
{
    TX_THREAD *current;
    need_context();
    current=_tx_thread_identify();
    need(current!=TX_NULL,"mutex get without registered thread context");
    if (m->tx_mutex_id!=TX_MUTEX_ID) return TX_MUTEX_ERROR;
    if (m->tx_mutex_ownership_count && m->tx_mutex_owner!=current) {
        if (wait==TX_NO_WAIT) return TX_NOT_AVAILABLE;
        /* NetX often ignores blocking mutex-get status. Returning unsupported
         * would let it proceed without protection, so fail closed here. */
        need(0,"blocking mutex contention not implemented");
    }
    need(m->tx_mutex_ownership_count!=(UINT)-1,"mutex recursion overflow");
    m->tx_mutex_owner=current; m->tx_mutex_ownership_count++;
    return TX_SUCCESS;
}

UINT _tx_mutex_put(TX_MUTEX *m)
{
    need_context();
    if (m->tx_mutex_id!=TX_MUTEX_ID) return TX_MUTEX_ERROR;
    if (!m->tx_mutex_ownership_count || m->tx_mutex_owner!=_tx_thread_identify()) return TX_NOT_OWNED;
    need(!m->tx_mutex_suspended_count,"mutex contention not implemented");
    if (!--m->tx_mutex_ownership_count) m->tx_mutex_owner=TX_NULL;
    if (anx_tx_after_mutex_put) anx_tx_after_mutex_put(m);
    return TX_SUCCESS;
}

UINT _tx_event_flags_create(TX_EVENT_FLAGS_GROUP *g, CHAR *name)
{
    need_context();
    memset(g,0,sizeof(*g)); g->tx_event_flags_group_name=name; g->tx_event_flags_group_id=TX_EVENT_FLAGS_ID;
    return TX_SUCCESS;
}

UINT _tx_event_flags_set(TX_EVENT_FLAGS_GROUP *g, ULONG flags, UINT option)
{
    need_context();
    if (g->tx_event_flags_group_id!=TX_EVENT_FLAGS_ID) return TX_GROUP_ERROR;
    if (option!=TX_OR && option!=TX_AND) return TX_OPTION_ERROR;
    need(!g->tx_event_flags_group_suspended_count,"event waiters not implemented");
    if (option==TX_OR) g->tx_event_flags_group_current|=flags;
    else g->tx_event_flags_group_current&=flags;
    return TX_SUCCESS;
}

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
static unsigned resume_hook_depth;
TX_TIMER *_tx_timer_created_ptr;
ULONG _tx_timer_created_count;
volatile ULONG _tx_timer_system_clock;
static unsigned timer_dispatch;
static unsigned runtime_holds;

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
             !_tx_thread_preempt_disable && !resume_hook_depth && !_tx_timer_created_count &&
             !timer_dispatch && !runtime_holds, "reinitialize active domain");
    platform = p;
    threads = 0;
    timers = 0;
    contexts = 0;
    current_frame = 0;
    _tx_thread_current_ptr = 0;
    _tx_thread_system_state = 0;
    _tx_thread_preempt_disable = 0;
    anx_tx_after_mutex_put = 0;
    resume_hook_depth=0;
    _tx_timer_created_ptr=TX_NULL; _tx_timer_created_count=0;
    _tx_timer_system_clock=0; timer_dispatch=0;
    runtime_holds=0;
}

void anx_tx_runtime_hold(void)
{
    platform->enter(platform->context);
    need(runtime_holds!=(unsigned)-1,"runtime hold overflow");
    runtime_holds++;
    platform->leave(platform->context);
}
void anx_tx_runtime_drop(void)
{
    platform->enter(platform->context);
    need(runtime_holds>0,"runtime hold underflow");
    runtime_holds--;
    platform->leave(platform->context);
}

int anx_tx_runtime_idle(void)
{
    int idle;
    platform->enter(platform->context);
    idle=!contexts && !current_frame && !_tx_thread_current_ptr &&
         !_tx_thread_system_state && !_tx_thread_preempt_disable && !resume_hook_depth;
    platform->leave(platform->context);
    return idle;
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

int anx_tx_bind_created(AnxTxThread *t, TX_THREAD *thread, AnxWait *wait, uintptr_t owner)
{
    AnxTxThread *other;
    anx_tx_require_context(0);
    for (other=threads;other;other=other->next)
        if (other==t || other->thread==thread || other->owner==owner) return 0;
    if (!owner || wait->result==ANX_WAIT_PENDING || thread->tx_thread_id!=TX_THREAD_ID ||
        thread->tx_thread_state!=TX_SUSPENDED || thread->tx_thread_amiga_task!=(VOID *)owner ||
        thread->tx_thread_suspend_cleanup || thread->tx_thread_owned_mutex_count ||
        thread->tx_thread_owned_mutex_list || thread->tx_thread_timer.tx_timer_internal_list_head)
        return 0;
    *t=(AnxTxThread){.thread=thread,.wait=wait,.owner=owner,.next=threads};
    threads=t;
    return 1;
}

static int detach(AnxTxThread *t, int completed)
{
    AnxTxThread **link;
    platform->enter(platform->context);
    if (platform->caller(platform->context) != t->owner ||
        t->wait->result == ANX_WAIT_PENDING || t->thread->tx_thread_state != TX_READY ||
        t->thread->tx_thread_suspend_cleanup || t->pending_resume || t->pending_token || t->abort_pins ||
        t->thread->tx_thread_owned_mutex_count || t->thread->tx_thread_owned_mutex_list ||
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
    if (completed) t->thread->tx_thread_state=TX_COMPLETED;
    else t->thread->tx_thread_id=0;
    platform->leave(platform->context);
    return 1;
}

int anx_tx_detach(AnxTxThread *t) { return detach(t,0); }
int anx_tx_complete(AnxTxThread *t) { return detach(t,1); }

int anx_tx_set_resume_cleanup(AnxTxThread *t, int (*hook)(AnxTxThread *))
{
    int valid;
    platform->enter(platform->context);
    valid=find(t->thread)==t && platform->caller(platform->context)==t->owner &&
        t->thread->tx_thread_state==TX_READY && !t->thread->tx_thread_suspend_cleanup &&
        !t->pending_resume && !t->pending_token && !t->abort_pins && t->wait->result!=ANX_WAIT_PENDING;
    if (valid) t->resume_cleanup=hook;
    platform->leave(platform->context);
    return valid;
}

int anx_tx_set_abort_policy(AnxTxThread *t, int (*policy)(AnxTxThread *, UINT *))
{
    int valid;
    platform->enter(platform->context);
    valid=find(t->thread)==t && platform->caller(platform->context)==t->owner &&
        t->thread->tx_thread_state==TX_READY && !t->thread->tx_thread_suspend_cleanup &&
        !t->pending_resume && !t->pending_token && !t->abort_pins && t->wait->result!=ANX_WAIT_PENDING;
    if (valid) t->abort_policy=policy;
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
 * Application timers share the protected list but are driven only by explicit
 * anx_tx_timer_tick calls; it never advances thread private deadlines. */
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
    need(t->operation!=UINT32_MAX,"suspension operation exhausted");
    t->operation++;
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
    if (thread->tx_thread_suspend_cleanup && t->resume_cleanup) {
        resume_hook_depth++;
        need(t->resume_cleanup(t),"resume cleanup integration rejected");
        resume_hook_depth--;
    }
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

void anx_tx_require_context(UINT blocking)
{
    need_context();
    if (blocking)
        need(contexts==1 && !_tx_thread_system_state && _tx_thread_identify() &&
             !_tx_thread_preempt_disable && !resume_hook_depth,
             "unsupported NetX blocking context");
}
void anx_tx_unsupported(const char *reason)
{
    need(0,reason);
}

UINT _tx_thread_wait_abort(TX_THREAD *thread)
{
    AnxTxThread *t;
    UINT status;
    need_context();
    t=find(thread);
    if (!t->abort_policy) return anx_tx_original_wait_abort(thread);
    need(t->abort_pins!=(unsigned)-1,"abort pin overflow");
    t->abort_pins++;
    need(t->abort_policy(t,&status),"abort policy integration rejected");
    need(t->abort_pins>0,"abort pin lost");
    t->abort_pins--;
    return status;
}

/* Real control-block owned lists, independent of recursion count. */
static void mutex_own(TX_MUTEX *m, TX_THREAD *thread)
{
    TX_MUTEX *head=thread->tx_thread_owned_mutex_list;
    need(thread->tx_thread_owned_mutex_count!=(UINT)-1,"owned mutex count overflow");
    if (head) {
        m->tx_mutex_owned_next=head;
        m->tx_mutex_owned_previous=head->tx_mutex_owned_previous;
        head->tx_mutex_owned_previous->tx_mutex_owned_next=m;
        head->tx_mutex_owned_previous=m;
    } else {
        thread->tx_thread_owned_mutex_list=m;
        m->tx_mutex_owned_next=m; m->tx_mutex_owned_previous=m;
    }
    thread->tx_thread_owned_mutex_count++;
    m->tx_mutex_owner=thread; m->tx_mutex_ownership_count=1;
}

static void mutex_disown(TX_MUTEX *m)
{
    TX_THREAD *thread=m->tx_mutex_owner;
    need(thread && thread->tx_thread_owned_mutex_count,"missing owned mutex");
    if (m->tx_mutex_owned_next==m) thread->tx_thread_owned_mutex_list=TX_NULL;
    else {
        m->tx_mutex_owned_previous->tx_mutex_owned_next=m->tx_mutex_owned_next;
        m->tx_mutex_owned_next->tx_mutex_owned_previous=m->tx_mutex_owned_previous;
        if (thread->tx_thread_owned_mutex_list==m)
            thread->tx_thread_owned_mutex_list=m->tx_mutex_owned_next;
    }
    thread->tx_thread_owned_mutex_count--;
    m->tx_mutex_owner=TX_NULL;
    m->tx_mutex_owned_next=TX_NULL; m->tx_mutex_owned_previous=TX_NULL;
}

/* NO_INHERIT only. FIFO handoff uses actual upstream cleanup for timeout/abort.
 * No priority scheduling, forced owner release or mutex deletion. */
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
    need(m->tx_mutex_inherit==TX_NO_INHERIT,"mutex inheritance not implemented");
    if (!m->tx_mutex_ownership_count) {
        need(!m->tx_mutex_suspended_count && !m->tx_mutex_suspension_list,"unowned mutex retained waiters");
        mutex_own(m,current);
        return TX_SUCCESS;
    }
    if (m->tx_mutex_ownership_count && m->tx_mutex_owner!=current) {
        if (wait==TX_NO_WAIT) return TX_NOT_AVAILABLE;
        /* NetX ignores many blocking-get return values: unsupported contexts
         * must fail closed before publishing a waiter. */
        need(!resume_hook_depth,"mutex blocking inside resume hook");
        need(!_tx_thread_preempt_disable,"mutex blocking while preemption disabled");
        need(contexts==1 && !_tx_thread_system_state,"unsupported mutex blocking context");
        need(current->tx_thread_suspension_sequence!=(ULONG)-1,"mutex suspension sequence exhausted");
        need(m->tx_mutex_suspended_count!=(UINT)-1,"mutex waiter count overflow");
        current->tx_thread_suspend_cleanup=_tx_mutex_cleanup;
        current->tx_thread_suspend_control_block=m;
        current->tx_thread_suspension_sequence++;
        TX_THREAD *head=m->tx_mutex_suspension_list;
        current->tx_thread_suspended_next=head ? head : current;
        current->tx_thread_suspended_previous=head ? head->tx_thread_suspended_previous : current;
        if (head) {
            head->tx_thread_suspended_previous->tx_thread_suspended_next=current;
            head->tx_thread_suspended_previous=current;
        } else m->tx_mutex_suspension_list=current;
        m->tx_mutex_suspended_count++;
        current->tx_thread_state=TX_MUTEX_SUSP;
        current->tx_thread_suspending=TX_TRUE;
        current->tx_thread_timer.tx_timer_internal_remaining_ticks=wait;
        _tx_thread_preempt_disable++;
        _tx_thread_system_suspend(current);
        return current->tx_thread_suspend_status;
    }
    need(m->tx_mutex_ownership_count!=(UINT)-1,"mutex recursion overflow");
    m->tx_mutex_owner=current; m->tx_mutex_ownership_count++;
    return TX_SUCCESS;
}

UINT _tx_mutex_put(TX_MUTEX *m)
{
    need_context();
    if (m->tx_mutex_id!=TX_MUTEX_ID) return TX_MUTEX_ERROR;
    /* The linked upstream cleanup TU also contains thread_release. Its forced
     * foreign-owner loop would retry forever if put silently returned an error.
     * Thread termination/forced release is outside this research backend. */
    need(!_tx_thread_preempt_disable || !m->tx_mutex_ownership_count ||
         m->tx_mutex_owner==_tx_thread_identify(),"foreign mutex release not implemented");
    if (!m->tx_mutex_ownership_count || m->tx_mutex_owner!=_tx_thread_identify()) return TX_NOT_OWNED;
    if (!--m->tx_mutex_ownership_count) {
        mutex_disown(m);
        TX_THREAD *thread=m->tx_mutex_suspension_list;
        if (thread) {
            need(m->tx_mutex_suspended_count && thread->tx_thread_state==TX_MUTEX_SUSP &&
                 thread->tx_thread_suspend_cleanup==_tx_mutex_cleanup &&
                 thread->tx_thread_suspend_control_block==m,"invalid mutex handoff waiter");
            if (!--m->tx_mutex_suspended_count) m->tx_mutex_suspension_list=TX_NULL;
            else {
                m->tx_mutex_suspension_list=thread->tx_thread_suspended_next;
                thread->tx_thread_suspended_next->tx_thread_suspended_previous=thread->tx_thread_suspended_previous;
                thread->tx_thread_suspended_previous->tx_thread_suspended_next=thread->tx_thread_suspended_next;
            }
            thread->tx_thread_suspend_cleanup=TX_NULL;
            thread->tx_thread_suspend_status=TX_SUCCESS;
            mutex_own(m,thread);
            _tx_thread_preempt_disable++;
            _tx_thread_system_resume(thread);
        }
    }
    if (anx_tx_after_mutex_put) anx_tx_after_mutex_put(m);
    return TX_SUCCESS;
}

UINT _tx_event_flags_create(TX_EVENT_FLAGS_GROUP *g, CHAR *name)
{
    need_context();
    memset(g,0,sizeof(*g)); g->tx_event_flags_group_name=name; g->tx_event_flags_group_id=TX_EVENT_FLAGS_ID;
    return TX_SUCCESS;
}

/* Compile unchanged vendor bodies under research-only names. Wrappers reject
 * unsupported contexts before a vendor body publishes a suspension. */
UINT anx_tx_original_event_flags_get(TX_EVENT_FLAGS_GROUP *, ULONG, UINT, ULONG *, ULONG);
UINT anx_tx_original_event_flags_set(TX_EVENT_FLAGS_GROUP *, ULONG, UINT);
UINT anx_tx_original_timer_create(TX_TIMER *, CHAR *, VOID (*)(ULONG), ULONG, ULONG, ULONG, UINT);
UINT anx_tx_original_timer_activate(TX_TIMER *);
UINT anx_tx_original_timer_change(TX_TIMER *, ULONG, ULONG);
UINT anx_tx_original_timer_delete(TX_TIMER *);

UINT _tx_event_flags_get(TX_EVENT_FLAGS_GROUP *g, ULONG flags, UINT option,
                        ULONG *actual, ULONG wait)
{
    TX_THREAD *thread;
    need_context();
    if (!g || g->tx_event_flags_group_id!=TX_EVENT_FLAGS_ID) return TX_GROUP_ERROR;
    if (!actual) return TX_PTR_ERROR;
    if (option>TX_AND_CLEAR) return TX_OPTION_ERROR;
    if (wait!=TX_NO_WAIT) {
        thread=_tx_thread_identify();
        need(thread && contexts==1 && !resume_hook_depth,
             "unsupported event blocking context");
        need(thread->tx_thread_suspension_sequence!=(ULONG)-1,"event suspension sequence exhausted");
        need(g->tx_event_flags_group_suspended_count!=(UINT)-1,"event waiter count overflow");
    }
    return anx_tx_original_event_flags_get(g,flags,option,actual,wait);
}

UINT _tx_event_flags_set(TX_EVENT_FLAGS_GROUP *g, ULONG flags, UINT option)
{
    need_context();
    if (!g || g->tx_event_flags_group_id!=TX_EVENT_FLAGS_ID) return TX_GROUP_ERROR;
    if (option!=TX_OR && option!=TX_AND) return TX_OPTION_ERROR;
    /* Notification callbacks may not park while raw set owns its search state. */
#ifndef TX_DISABLE_NOTIFY_CALLBACKS
    need(!g->tx_event_flags_group_set_notify,"event notification callback not implemented");
#endif
    return anx_tx_original_event_flags_set(g,flags,option);
}

static void timer_lifecycle(void)
{
    need_context();
    need(!timer_dispatch,"timer lifecycle inside callback not implemented");
}
UINT _tx_timer_create(TX_TIMER *t, CHAR *name, VOID (*callback)(ULONG),
                      ULONG input, ULONG initial, ULONG reload, UINT activate)
{
    TX_TIMER *other;
    timer_lifecycle();
    if (!t) return TX_TIMER_ERROR;
    if (!initial) return TX_TICK_ERROR;
    if (activate!=TX_AUTO_ACTIVATE && activate!=TX_NO_ACTIVATE) return TX_ACTIVATE_ERROR;
    other=_tx_timer_created_ptr;
    for (ULONG i=0;i<_tx_timer_created_count;i++,other=other->tx_timer_created_next)
        if (other==t) return TX_TIMER_ERROR;
    need(_tx_timer_created_count!=(ULONG)-1,"timer count overflow");
    return anx_tx_original_timer_create(t,name,callback,input,initial,reload,activate);
}
UINT _tx_timer_activate(TX_TIMER *t)
{
    timer_lifecycle();
    if (!t || t->tx_timer_id!=TX_TIMER_ID) return TX_TIMER_ERROR;
    return anx_tx_original_timer_activate(t);
}
UINT _tx_timer_change(TX_TIMER *t, ULONG initial, ULONG reload)
{
    timer_lifecycle();
    if (!t || t->tx_timer_id!=TX_TIMER_ID) return TX_TIMER_ERROR;
    if (!initial) return TX_TICK_ERROR;
    return anx_tx_original_timer_change(t,initial,reload);
}
UINT _tx_timer_delete(TX_TIMER *t)
{
    timer_lifecycle();
    if (!t || t->tx_timer_id!=TX_TIMER_ID) return TX_TIMER_ERROR;
    return anx_tx_original_timer_delete(t);
}
UINT _tx_timer_deactivate(TX_TIMER *t)
{
    ULONG remaining;
    need_context();
    if (!t || t->tx_timer_id!=TX_TIMER_ID) return TX_TIMER_ERROR;
    /* Our flat active list has no wheel-position correction. Preserve ticks
     * remaining for later activation, including reload installed before callback. */
    remaining=t->tx_timer_internal.tx_timer_internal_remaining_ticks;
    if (t->tx_timer_internal.tx_timer_internal_list_head && !remaining)
        remaining=t->tx_timer_internal.tx_timer_internal_re_initialize_ticks;
    _tx_timer_system_deactivate(&t->tx_timer_internal);
    t->tx_timer_internal.tx_timer_internal_remaining_ticks=remaining;
    return TX_SUCCESS;
}
ULONG _tx_time_get(void)
{
    need_context(); return _tx_timer_system_clock;
}
void anx_tx_timer_tick(void)
{
    TX_TIMER *t;
    need_context();
    need(_tx_thread_system_state && contexts==1 && !_tx_thread_current_ptr &&
         !_tx_thread_preempt_disable && !timer_dispatch,"unsupported timer tick context");
    /* No storage may be freed or created/deleted/reactivated during dispatch.
     * Callbacks may set events and deactivate timers, including themselves. */
    timer_dispatch=1;
    _tx_timer_system_clock++;
    t=_tx_timer_created_ptr;
    for (ULONG i=0;i<_tx_timer_created_count;i++,t=t->tx_timer_created_next) {
        TX_TIMER_INTERNAL *internal=&t->tx_timer_internal;
        if (internal->tx_timer_internal_list_head) {
            need(internal->tx_timer_internal_remaining_ticks>0,"zero active application timer");
            internal->tx_timer_internal_remaining_ticks--;
        }
    }
    t=_tx_timer_created_ptr;
    for (ULONG i=0;i<_tx_timer_created_count;i++,t=t->tx_timer_created_next) {
        TX_TIMER_INTERNAL *internal=&t->tx_timer_internal;
        if (internal->tx_timer_internal_list_head && !internal->tx_timer_internal_remaining_ticks) {
            VOID (*callback)(ULONG)=internal->tx_timer_internal_timeout_function;
            ULONG input=internal->tx_timer_internal_timeout_param;
            _tx_timer_system_deactivate(internal);
            internal->tx_timer_internal_remaining_ticks=internal->tx_timer_internal_re_initialize_ticks;
            if (internal->tx_timer_internal_remaining_ticks) _tx_timer_system_activate(internal);
            if (callback) callback(input);
            need(!_tx_thread_preempt_disable,"timer callback retained preemption counter");
        }
    }
    timer_dispatch=0;
}

/* Deterministic boundary schedules, not ThreadX/NetX conformance.
 * SPDX-License-Identifier: MIT */
#include "wait.h"
#include <stdio.h>
#include <stdlib.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
typedef struct {
    AnxWait wait;
    AnxWaitOps ops;
    uint64_t now, first_deadline;
    unsigned locked, cleanup_count, notifications, parks, publish_count;
    unsigned schedule;
} Model;
static void enter(void *p) { Model *m=p; CHECK(!m->locked); m->locked=1; }
static void leave(void *p) { Model *m=p; CHECK(m->locked); m->locked=0; }
static uint64_t clock_now(void *p) { Model *m=p; CHECK(m->locked); return m->now; }
static void notify(void *p) { Model *m=p; CHECK(m->locked); m->notifications++; }
static void cleanup(void *p, AnxWaitResult result)
{ Model *m=p; CHECK(m->locked); CHECK(result>=ANX_WAIT_READY); m->cleanup_count++; }
static void publish(void *p, AnxWait *w, uint32_t token)
{ Model *m=p; CHECK(m->locked); CHECK(w->result==ANX_WAIT_PENDING); CHECK(token); m->publish_count++; }
static int park(void *p, uint64_t deadline)
{
    Model *m=p; CHECK(!m->locked); m->parks++;
    if (m->parks==1) m->first_deadline=deadline;
    CHECK(deadline==m->first_deadline);
    if (m->schedule==4)
        return -1;
    if (m->schedule==1) {
        CHECK(anx_wait_complete(&m->wait,m->wait.generation,ANX_WAIT_READY));
        return 1; /* Producer wins in the check-to-park window. */
    }
    if (m->schedule==2) {
        CHECK(anx_wait_complete(&m->wait,m->wait.generation,ANX_WAIT_READY));
        return 0; /* Packet commits while the timer also reports expiry. */
    }
    if (m->schedule==3 && m->parks<3) { m->now+=4; return 1; }
    CHECK(deadline!=ANX_WAIT_FOREVER);
    m->now=deadline;
    return 0;
}
static void init(Model *m)
{
    *m=(Model){0};
    m->ops=(AnxWaitOps){enter,leave,clock_now,park,notify,m};
    anx_wait_init(&m->wait,&m->ops);
}
static uint32_t begin(Model *m, uint64_t timeout)
{ return anx_wait_begin(&m->wait,timeout,publish,m,cleanup,m); }

int main(void)
{
    Model m; uint32_t token, old;
    init(&m); token=begin(&m,10); CHECK(token);
    CHECK(anx_wait_complete(&m.wait,token,ANX_WAIT_READY));
    CHECK(anx_wait_run(&m.wait,token)==ANX_WAIT_READY);
    CHECK(m.parks==0 && m.cleanup_count==1 && m.notifications==1);
    CHECK(!anx_wait_complete(&m.wait,token,ANX_WAIT_TIMEOUT));
    CHECK(m.cleanup_count==1);

    init(&m); m.schedule=1; token=begin(&m,10);
    CHECK(anx_wait_run(&m.wait,token)==ANX_WAIT_READY && m.parks==1);
    init(&m); m.schedule=2; token=begin(&m,10);
    CHECK(anx_wait_run(&m.wait,token)==ANX_WAIT_READY && m.cleanup_count==1);
    init(&m); m.schedule=3; token=begin(&m,10);
    CHECK(anx_wait_run(&m.wait,token)==ANX_WAIT_TIMEOUT);
    CHECK(m.parks==3 && m.now==10 && m.cleanup_count==1);
    init(&m); m.schedule=4; token=begin(&m,10);
    CHECK(anx_wait_run(&m.wait,token)==ANX_WAIT_ERROR && m.cleanup_count==1);

    for (AnxWaitResult r=ANX_WAIT_CANCELLED; r<=ANX_WAIT_DELETED; r++) {
        init(&m); token=begin(&m,ANX_WAIT_FOREVER);
        CHECK(anx_wait_complete(&m.wait,token,r));
        CHECK(anx_wait_run(&m.wait,token)==r && m.cleanup_count==1);
    }
    init(&m); token=begin(&m,ANX_WAIT_FOREVER); m.schedule=1;
    CHECK(anx_wait_run(&m.wait,token)==ANX_WAIT_READY);

    init(&m); old=begin(&m,10);
    CHECK(!begin(&m,20) && m.publish_count==1);
    CHECK(anx_wait_complete(&m.wait,old,ANX_WAIT_CANCELLED));
    CHECK(anx_wait_run(&m.wait,old)==ANX_WAIT_CANCELLED);
    token=begin(&m,10); CHECK(token!=old);
    CHECK(!anx_wait_complete(&m.wait,old,ANX_WAIT_READY));
    CHECK(anx_wait_run(&m.wait,old)==ANX_WAIT_ERROR);
    CHECK(anx_wait_run(&m.wait,token)==ANX_WAIT_TIMEOUT && m.cleanup_count==2);

    init(&m); CHECK(!begin(&m,0) && m.publish_count==0);
    m.now=UINT64_MAX-4; CHECK(!begin(&m,4));
    m.now=0; m.wait.generation=UINT32_MAX; CHECK(!begin(&m,10));
    CHECK(!anx_wait_complete(&m.wait,0,ANX_WAIT_READY));
    CHECK(!anx_wait_complete(&m.wait,1,ANX_WAIT_IDLE));
    puts("research_wait_model=PASS prewake/window/race/deadline/cancel/delete/reuse/bounds");
    return 0;
}

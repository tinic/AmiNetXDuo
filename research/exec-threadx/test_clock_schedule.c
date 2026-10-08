/* Tick phase and bounded catch-up arithmetic, not native IO. SPDX-License-Identifier: MIT */
#include "clock_schedule.h"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(x) do {if (!(x)) {fprintf(stderr,"FAIL %s\n",#x);exit(1);}} while (0)
int main(void)
{
    uint64_t next=20000;
    CHECK(!anx_clock_batch(19999,next,20000));
    CHECK(anx_clock_batch(20000,next,20000)==1);
    CHECK(anx_clock_batch(39999,next,20000)==1);
    CHECK(anx_clock_batch(40000,next,20000)==2);
    unsigned ticks=0,batches=0;
    /* A 1.01-second dispatch delay: drain exactly 50 ticks in seven batches,
     * retain original phase, and leave the next future tick unconsumed. */
    unsigned due;
    while ((due=anx_clock_batch(1010000,next,20000))) {
        CHECK(due<=ANX_CLOCK_BATCH);ticks+=due;batches++;next+=due*20000;
    }
    CHECK(ticks==50 && batches==7 && next==1020000);
    CHECK(!anx_clock_batch(1019999,next,20000));
    CHECK(anx_clock_batch(1020000,next,20000)==1);
    CHECK(anx_clock_batch(UINT64_MAX,0,1)==ANX_CLOCK_BATCH);
    CHECK(anx_clock_batch(UINT64_MAX,UINT64_MAX,1)==1);
    CHECK(!anx_clock_batch(0,1,1));
    CHECK(!anx_clock_batch(1,0,0));
    puts("research_clock_schedule=PASS absolute phase, no drift, bounded lossless backlog, extreme arithmetic");
    return 0;
}

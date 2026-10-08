#include "clock_schedule.h"
unsigned anx_clock_batch(uint64_t now, uint64_t next, uint32_t period)
{
    uint64_t elapsed;
    if (!period || now<next) return 0;
    elapsed=(now-next)/period;
    return elapsed>=ANX_CLOCK_BATCH-1 ? ANX_CLOCK_BATCH : (unsigned)elapsed+1;
}

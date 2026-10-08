/* Absolute tick phase; bounded catch-up without skipped ticks. SPDX-License-Identifier: MIT */
#ifndef ANX_CLOCK_SCHEDULE_H
#define ANX_CLOCK_SCHEDULE_H
#include <stdint.h>
#define ANX_CLOCK_BATCH 8
unsigned anx_clock_batch(uint64_t now, uint64_t next, uint32_t period);
#endif

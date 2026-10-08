/* Research kernel lifecycle, not default-worker/public socket completion.
 * SPDX-License-Identifier: MIT */
#ifndef ANX_EXEC_KERNEL_H
#define ANX_EXEC_KERNEL_H
#include "exec_caller.h"
#include "exec_thread.h"
typedef struct {
    struct Task *manager,*clock;
    APTR manager_stack,clock_stack;
    ULONG stack_size;
    unsigned phase;
} AnxKernelSnapshot;
void anx_exec_kernel_snapshot(AnxKernelSnapshot *);
#endif

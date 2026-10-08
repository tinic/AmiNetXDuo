/* Production entry-point seam, research implementation. SPDX-License-Identifier: MIT */
#ifndef ANX_EXEC_NETSTACK_H
#define ANX_EXEC_NETSTACK_H
#include "exec_kernel.h"
#include "aminetxduo/netstack.h"
#include "aminetxduo/health.h"
VOID ami_netstack_baton_release(VOID);
VOID ami_netstack_baton_acquire(VOID);
BOOL ami_netstack_baton_abandon(TX_THREAD *);
BOOL ami_netstack_baton_reclaim_dead(VOID);
VOID ami_netstack_baton_reset(VOID);
VOID ami_netstack_baton_set_sampler(VOID (*)(VOID));
VOID ami_netstack_health_publish(VOID);
VOID ami_netstack_health_unpublish(VOID);
#endif

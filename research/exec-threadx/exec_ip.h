/* Single-IP, synchronous-driver lifecycle experiment. SPDX-License-Identifier: MIT */
#ifndef ANX_EXEC_IP_H
#define ANX_EXEC_IP_H
#include "exec_thread.h"
#include "exec_clock.h"
#include "nx_api.h"
enum {ANX_IP_EMPTY, ANX_IP_PREPARED, ANX_IP_LIVE, ANX_IP_CLOSING, ANX_IP_REAPED};
typedef struct {
    AnxExecThread helper;
    NX_IP *ip;
    NX_PACKET_POOL *pool;
    VOID (*driver)(NX_IP_DRIVER *);
    TX_THREAD *caller;
    struct Task *creator;
    unsigned state,capability;
    AnxExecClock *clock;
} AnxExecIp;
/* Zero-init once; call outside all boundaries in the attached creator task.
 * Only one IP/domain, no other ThreadX objects, no enabled protocol callbacks,
 * fast timer, queued packets or socket/raw/ping waiters; only primary interface
 * and fixed TEST-NET IPv4 address 192.0.2.1/24. Real upstream bodies
 * run; the driver must be synchronous and all external producers excluded
 * except the optional associated clock below.
 * Caller retains IP/pool/record/stack/name until successful delete. */
UINT anx_exec_ip_create(AnxExecIp *, TX_THREAD *, NX_IP *, CHAR *, NX_PACKET_POOL *,
                        VOID (*)(NX_IP_DRIVER *), VOID *, ULONG, UINT);
UINT anx_exec_ip_delete(AnxExecIp *);
/* Optional domain clock, creator-only after IP startup. The record/stack must
 * be disjoint from all retained IP storage. Successful IP delete owns clock
 * stop/join; retain both through that delete, do not independently retire it. */
UINT anx_exec_ip_clock_start(AnxExecIp *,AnxExecClock *,CHAR *,VOID *,ULONG);
/* Only producer admitted by this experiment. Requires caller's boundary.
 * Closed state refuses before accessing the IP. Not a production API gate. */
UINT anx_exec_ip_event(AnxExecIp *, ULONG);
#endif

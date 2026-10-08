/* Single-IP research lifecycle, task-context producers only. SPDX-License-Identifier: MIT */
#ifndef ANX_EXEC_IP_H
#define ANX_EXEC_IP_H
#include "exec_thread.h"
#include "exec_clock.h"
#include "nx_api.h"
enum {ANX_IP_EMPTY, ANX_IP_PREPARED, ANX_IP_LIVE, ANX_IP_CLOSING, ANX_IP_REAPED};
struct AnxExecIpIo;
typedef struct AnxExecIp {
    AnxExecThread helper;
    NX_IP *ip;
    NX_PACKET_POOL *pool;
    VOID (*driver)(NX_IP_DRIVER *);
    TX_THREAD *caller;
    struct Task *creator;
    unsigned state,capability;
    AnxExecClock *clock;
    struct AnxExecIpIo *io;
} AnxExecIp;
/* Zero-init once and retain across generations, including rejected late calls.
 * Exactly one registered task producer and one outstanding TX packet. This
 * record is not device IO or an ISR adapter: the driver's native owner must
 * drain its actual IO before close, then complete/reap before IP deletion.
 * Successful accept/receive transfer packet ownership; failures retain it.
 * Caller-owned storage/packets must be valid and disjoint from live IP storage.
 * Output-token storage must be disjoint from control records and packets.
 * Calls require a normal serialized task boundary. Open/complete/receive/close
 * are producer-owner-only and require a registered public native worker;
 * accept may run in any admitted driver call context.
 * Save generation at open and pass that token with every later operation;
 * it never wraps. Rejected old tokens do not touch IP or packet storage. */
typedef struct AnxExecIpIo {
    AnxExecIp *domain;
    TX_THREAD *owner;
    struct Task *task;
    NX_PACKET *pending;
    ULONG generation,submission;
    unsigned open;
} AnxExecIpIo;
/* Defined only by the real-protocol research link. No production API/ABI. */
UINT anx_exec_ip_io_open(AnxExecIpIo *,AnxExecIp *);
/* Every accepted TX also returns a non-wrapping operation token. A late
 * completion must match both tokens, even if the pool reused its address. */
UINT anx_exec_ip_io_accept(AnxExecIpIo *,ULONG,NX_PACKET *,ULONG *);
UINT anx_exec_ip_io_complete(AnxExecIpIo *,ULONG,ULONG,NX_PACKET *);
UINT anx_exec_ip_io_receive(AnxExecIpIo *,ULONG,NX_PACKET *);
UINT anx_exec_ip_io_close(AnxExecIpIo *,ULONG);
/* Zero-init once; call outside all boundaries in the attached creator task.
 * Only one IP/domain, no other ThreadX objects, no enabled protocol callbacks,
 * fast timer, queued packets or socket/raw/ping waiters; only primary interface
 * and fixed TEST-NET IPv4 address 192.0.2.1/24. Real upstream bodies
 * run; all external producers are excluded except the optional associated
 * clock and real-protocol target's explicit task-producer lease. The lease
 * must close and all non-helper public workers retire before successful delete.
 * Caller retains IP/pool/record/stack/name until successful delete. */
UINT anx_exec_ip_create(AnxExecIp *, TX_THREAD *, NX_IP *, CHAR *, NX_PACKET_POOL *,
                        VOID (*)(NX_IP_DRIVER *), VOID *, ULONG, UINT);
UINT anx_exec_ip_delete(AnxExecIp *);
/* Optional domain clock, creator-only after IP startup. The record/stack must
 * be disjoint from all retained IP storage. Successful IP delete owns clock
 * stop/join; retain both through that delete, do not independently retire it. */
UINT anx_exec_ip_clock_start(AnxExecIp *,AnxExecClock *,CHAR *,VOID *,ULONG);
/* Serialized event producer. Requires caller's boundary.
 * Closed state refuses before accessing the IP. Not a production API gate. */
UINT anx_exec_ip_event(AnxExecIp *, ULONG);
#endif

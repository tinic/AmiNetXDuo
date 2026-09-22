/*
 * tx_amiga.h for the netstack host tier.
 *
 * netstack.c includes the real one, port/threadx-amiga/inc/tx_amiga.h, which
 * cannot be on the include path here: it carries the Amiga tx_port.h and this
 * tier builds against ThreadX's linux port with tests/perf/host/shim's type
 * widths on top.  Only the kernel and thread-adoption entry points netstack.c
 * names are declared, and the declarations are the real header's, so a change
 * of signature there stops the build here rather than being answered by an
 * implicit int.
 *
 * A directory of its own, not tests/bsdsocket/host/shim-netstatus, for the
 * reason that file's own header gives: whichever copy of this name comes first
 * on the include path shadows the others, and that one is the netstatus
 * counter set.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_NETSTACK_HOST_TX_AMIGA_H
#define AMINETXDUO_NETSTACK_HOST_TX_AMIGA_H

#include "tx_api.h"

UINT    tx_amiga_kernel_start(VOID);
UINT    tx_amiga_kernel_running(VOID);
UINT    tx_amiga_kernel_stop(VOID);

UINT    tx_amiga_adopt_thread(TX_THREAD **thread_ptr, ULONG *generation,
                              CHAR *name, UINT priority, UINT reserved);
UINT    tx_amiga_orphan_thread(TX_THREAD *thread_ptr, ULONG generation);
UINT    tx_amiga_adopt_resume(TX_THREAD *thread_ptr, ULONG generation);
UINT    tx_amiga_adopt_suspend(TX_THREAD *thread_ptr, ULONG generation);
UINT    tx_amiga_adopt_try_resume(TX_THREAD *thread_ptr);
UINT    tx_amiga_baton_free(VOID);
UINT    tx_amiga_discard_thread(TX_THREAD *thread_ptr, ULONG generation);
ULONG   tx_amiga_adopt_slots(VOID);
ULONG   tx_amiga_adopt_reserve(VOID);
ULONG   tx_amiga_adopt_slots_free(VOID);
VOID    tx_amiga_adopt_sweep_unpublished(VOID);
UINT    tx_amiga_adopt_claim_orphan(VOID);
ULONG   tx_amiga_adopt_generation(TX_THREAD *thread_ptr);
UINT    tx_amiga_adopt_handle_valid(TX_THREAD *thread_ptr, ULONG generation);
UINT    tx_amiga_caller_is_thread(VOID);

#endif /* AMINETXDUO_NETSTACK_HOST_TX_AMIGA_H */

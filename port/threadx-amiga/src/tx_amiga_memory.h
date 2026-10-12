/* ThreadX application-definition memory; private port/link policy.
 * SPDX-License-Identifier: MIT
 */
#ifndef TX_AMIGA_MEMORY_H
#define TX_AMIGA_MEMORY_H

#include "tx_api.h"

extern VOID *_tx_amiga_kernel_memory;
extern ULONG _tx_amiga_kernel_memory_size;

/* Standalone applications use TX_AMIGA_MEMORY_SIZE by default. The shared
   library overrides this to zero: its application-definition hook is empty. */
ULONG _tx_amiga_application_memory_size(VOID);
VOID _tx_amiga_application_memory_init(VOID);
VOID _tx_amiga_application_memory_release(VOID);

#endif

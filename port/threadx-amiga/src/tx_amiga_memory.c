/* Memory passed to tx_application_define(), independent of thread stacks.
 * SPDX-License-Identifier: MIT
 */
#include "tx_amiga_memory.h"
#include "tx_amiga.h"
#include <exec/memory.h>
#include <proto/exec.h>

VOID *_tx_amiga_kernel_memory = (VOID *) 0;
ULONG _tx_amiga_kernel_memory_size = 0UL;
static UINT memory_owned = TX_FALSE;
extern VOID *_tx_initialize_unused_memory;

__attribute__((weak)) ULONG _tx_amiga_application_memory_size(VOID)
{
    return (ULONG) TX_AMIGA_MEMORY_SIZE;
}

VOID tx_amiga_set_kernel_memory(VOID *memory, ULONG size)
{
    _tx_amiga_kernel_memory = memory;
    _tx_amiga_kernel_memory_size = size;
    memory_owned = TX_FALSE;
}

VOID _tx_amiga_application_memory_init(VOID)
{
    if (_tx_amiga_kernel_memory == (VOID *) 0)
    {
        _tx_amiga_kernel_memory_size = _tx_amiga_application_memory_size();
        if (_tx_amiga_kernel_memory_size != 0UL)
        {
            _tx_amiga_kernel_memory = AllocMem(_tx_amiga_kernel_memory_size,
                                              MEMF_PUBLIC | MEMF_CLEAR);
            if (_tx_amiga_kernel_memory == (VOID *) 0)
                _tx_amiga_kernel_memory_size = 0UL;
            else
                memory_owned = TX_TRUE;
        }
    }
    _tx_initialize_unused_memory = _tx_amiga_kernel_memory;
}

/* Called only after all port-created tasks have stopped. Caller-supplied
   memory remains theirs and survives stop/restart, as before. */
VOID _tx_amiga_application_memory_release(VOID)
{
    if (memory_owned != TX_FALSE && _tx_amiga_kernel_memory != (VOID *) 0)
    {
        FreeMem(_tx_amiga_kernel_memory, _tx_amiga_kernel_memory_size);
        _tx_amiga_kernel_memory = (VOID *) 0;
        _tx_amiga_kernel_memory_size = 0UL;
        memory_owned = TX_FALSE;
    }
}

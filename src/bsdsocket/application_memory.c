/* bsdsocket.library creates its networking objects separately; its empty
 * tx_application_define() hook needs no application arena. This override is
 * linked only into the shared library, not standalone NetX/ThreadX programs.
 * SPDX-License-Identifier: MIT
 */
#include "../../port/threadx-amiga/src/tx_amiga_memory.h"

ULONG _tx_amiga_application_memory_size(VOID)
{
    return 0UL;
}

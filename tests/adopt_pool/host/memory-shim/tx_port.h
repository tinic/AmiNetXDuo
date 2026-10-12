/* The real port declares this ULONG global while the width shim temporarily
 * renames ULONG. Re-declare it with target width in tx_amiga_memory.h instead.
 * SPDX-License-Identifier: MIT
 */
#define _tx_amiga_kernel_memory_size aminetxduo_host_wide_kernel_memory_size
#include "../../../perf/host/shim/tx_port.h"
#undef _tx_amiga_kernel_memory_size

/*
 * AmiNetXDuo, host shim: ThreadX port header with the m68k's type widths.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_HOST_TX_PORT_SHIM
#define AMINETXDUO_HOST_TX_PORT_SHIM

#ifndef AMINETXDUO_HOST_VENDORED_TX_PORT
#error "AMINETXDUO_HOST_VENDORED_TX_PORT must name the tx_port.h to wrap: ThreadX's linux port or port/threadx-amiga/inc/tx_port.h"
#endif

#define LONG    aminetxduo_host_shim_LONG
#define ULONG   aminetxduo_host_shim_ULONG
#include AMINETXDUO_HOST_VENDORED_TX_PORT
#undef LONG
#undef ULONG

typedef int             LONG;
typedef unsigned int    ULONG;

#undef ALIGN_TYPE_DEFINED
#define ALIGN_TYPE_DEFINED
typedef unsigned long   aminetxduo_host_align_t;
#define ALIGN_TYPE      aminetxduo_host_align_t

#endif /* AMINETXDUO_HOST_TX_PORT_SHIM */

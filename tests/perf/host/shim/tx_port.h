/*
 * AmiNetXDuo, host shim: ThreadX port header with the m68k's type widths.
 *
 * The host tier of the checksum differential test compiles two
 * implementations of _nx_ip_checksum_compute(), the vendored one under a
 * renamed symbol, and src/net68k/n68k_checksum.c, and checks that they agree.
 * That comparison is only meaningful if both see the widths the real target
 * has, because the vendored loop reads the payload through a `ULONG *` and
 * splits each word with `& 0xFFFF` and `>> 16`.  With a 64-bit ULONG it would
 * read eight bytes at a time and sum bits [15:0] and [63:16], which is not the
 * function under test.
 *
 * ThreadX's Linux port already does this for x86_64, 32-bit LONG/ULONG,
 * 64-bit ALIGN_TYPE so a pointer still fits, but the branch is
 * `#if defined(__x86_64__)`, and this project's host builds also run on
 * arm64 macOS.  Defining __x86_64__ on an arm64 host to reach that branch
 * would eventually be caught by the system headers, so instead the port's own
 * typedefs are renamed out of the way on the way in and replaced afterwards.
 * Nothing here is linked; the shim exists so one vendored translation unit
 * compiles.
 *
 * Endianness is left as the Linux port has it (little).  The m68k is
 * big-endian, but both implementations consult the same
 * NX_CHANGE_USHORT_ENDIAN, so the differential holds either way, and the
 * little-endian setting exercises the byte-swap path that the big-endian
 * target compiles away entirely.
 *
 * The host tier does not cover the hand-written 68020 assembly, which cannot
 * be assembled here.  AMINETXDUO_NET68K_ASM is OFF off target for that reason,
 * and the assembly stays an emulator-tier test, the same split
 * src/crypto68k/ documents.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_HOST_TX_PORT_SHIM
#define AMINETXDUO_HOST_TX_PORT_SHIM

/*
 * The vendored header cannot be reached by name: this file is "tx_port.h" on
 * the include path.  CMake passes its absolute path instead.
 */
#ifndef AMINETXDUO_HOST_VENDORED_TX_PORT
#error "AMINETXDUO_HOST_VENDORED_TX_PORT must name ThreadX's linux tx_port.h"
#endif

#define LONG    aminetxduo_host_shim_LONG
#define ULONG   aminetxduo_host_shim_ULONG
#include AMINETXDUO_HOST_VENDORED_TX_PORT
#undef LONG
#undef ULONG

typedef int             LONG;
typedef unsigned int    ULONG;

/*
 * ALIGN_TYPE has to hold a pointer, the checksum casts prepend/append
 * pointers to it and does arithmetic on them, so it is the one type that
 * must stay host-width rather than target-width.
 */
#undef ALIGN_TYPE_DEFINED
#define ALIGN_TYPE_DEFINED
typedef unsigned long   aminetxduo_host_align_t;
#define ALIGN_TYPE      aminetxduo_host_align_t

#endif /* AMINETXDUO_HOST_TX_PORT_SHIM */

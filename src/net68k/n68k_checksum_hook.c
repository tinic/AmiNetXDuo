/*
 * AmiNetXDuo, give the net68k checksum NetX Duo's name.
 *
 * The top-level CMakeLists drops nx_ip_checksum_compute.c from the vendored
 * source glob, so _nx_ip_checksum_compute() is undefined.  Every caller inside
 * NetX Duo (IP, TCP, UDP, ICMP, ICMPv6) then resolves to this one.  No
 * vendored file is edited and no call site is patched.
 *
 * The hook is in a file of its own so that a program can link the algorithm
 * without it and supply its own symbol.  tests/perf does that, to run the
 * vendored implementation and this one back to back in one process with one
 * variable changed.
 *
 * SPDX-License-Identifier: MIT
 */

#include "net68k.h"

USHORT _nx_ip_checksum_compute(NX_PACKET *packet_ptr, ULONG protocol,
                               UINT data_length, ULONG *src_ip_addr,
                               ULONG *dest_ip_addr)
{

    return(n68k_ip_checksum_compute(packet_ptr, protocol, data_length,
                                    src_ip_addr, dest_ip_addr));
}

/*
 * Event-trace builds must not take bsdsocket's packet shortcut: the native
 * NetX Duo helpers are what insert the packet-length and extraction events.
 *
 * SPDX-License-Identifier: MIT
 */

#include "packet_extract.h"

#include <stdio.h>
#include <string.h>

#define CHECK(expr)                                                        \
    do {                                                                   \
        if (!(expr)) {                                                     \
            fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr);       \
            return 1;                                                      \
        }                                                                  \
    } while (0)

static ULONG length_calls;
static ULONG extract_calls;

UINT _nxe_packet_length_get(NX_PACKET *packet, ULONG *length)
{
    (void)packet;
    length_calls++;
    *length = 123UL;
    return NX_SUCCESS;
}

UINT _nxe_packet_data_extract_offset(NX_PACKET *packet, ULONG offset,
                                     VOID *buffer, ULONG buffer_length,
                                     ULONG *bytes_copied)
{
    static const UCHAR result[] = "trace";

    (void)packet;
    (void)offset;
    CHECK(buffer_length == 5UL);
    extract_calls++;
    memcpy(buffer, result, 5UL);
    *bytes_copied = 5UL;
    return NX_SUCCESS;
}

int main(void)
{
    NX_PACKET packet;
    UCHAR source[] = "direct";
    UCHAR out[8] = {0};
    ULONG moved = 0;

    memset(&packet, 0, sizeof(packet));
    packet.nx_packet_prepend_ptr = source;
    packet.nx_packet_append_ptr = source + 6;
    packet.nx_packet_length = 6UL;

    CHECK(bsd_packet_length(&packet) == 123UL);
    CHECK(length_calls == 1UL);
    CHECK(bsd_packet_extract(&packet, 0UL, out, 5UL, &moved) == NX_SUCCESS);
    CHECK(extract_calls == 1UL);
    CHECK(moved == 5UL);
    CHECK(memcmp(out, "trace", 5UL) == 0);

    puts("packet extract: NetX helpers retained under event trace");
    return 0;
}

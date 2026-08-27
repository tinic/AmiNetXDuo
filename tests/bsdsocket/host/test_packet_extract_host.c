/*
 * Regression coverage for bsdsocket's one-buffer TCP receive shortcut.
 * The helper is included directly so this test executes the same inline code
 * transfer.c ships; the fallback is NetX Duo's vendored implementation.
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

static void packet_set(NX_PACKET *packet, UCHAR *data, ULONG length)
{
    memset(packet, 0, sizeof(*packet));
    packet->nx_packet_prepend_ptr = data;
    packet->nx_packet_append_ptr = data + length;
    packet->nx_packet_length = length;
}

int main(void)
{
    static const UCHAR first_data[] = "abcdefgh";
    static const UCHAR second_data[] = "ijklmnop";
    NX_PACKET first;
    NX_PACKET second;
    UCHAR out[16];
    ULONG moved;
    UINT status;

    packet_set(&first, (UCHAR *)(VOID *)first_data, 8UL);
    memset(out, 0, sizeof(out));
    moved = 99UL;
    status = bsd_packet_extract(&first, 0UL, out, 4UL, &moved);
    CHECK(status == NX_SUCCESS);
    CHECK(moved == 4UL);
    CHECK(memcmp(out, "abcd", 4) == 0);

    /* A nonzero offset ending exactly at the logical end is valid. */
    memset(out, 0, sizeof(out));
    moved = 99UL;
    status = bsd_packet_extract(&first, 4UL, out, 4UL, &moved);
    CHECK(status == NX_SUCCESS);
    CHECK(moved == 4UL);
    CHECK(memcmp(out, "efgh", 4) == 0);

    /* But the offset itself may not equal the end of a nonempty packet,
       including a zero-byte request.  This is the native NetX boundary. */
    moved = 99UL;
    status = bsd_packet_extract(&first, 8UL, out, 0UL, &moved);
    CHECK(status == NX_PACKET_OFFSET_ERROR);
    CHECK(moved == 99UL);

    /* A request crossing fragments must use the generic chain walk. */
    packet_set(&second, (UCHAR *)(VOID *)second_data, 8UL);
    first.nx_packet_next = &second;
    first.nx_packet_length = 16UL;
    memset(out, 0, sizeof(out));
    moved = 99UL;
    status = bsd_packet_extract(&first, 6UL, out, 6UL, &moved);
    CHECK(status == NX_SUCCESS);
    CHECK(moved == 6UL);
    CHECK(memcmp(out, "ghijkl", 6) == 0);

    /* Physical and logical lengths are independent invariants.  If they do
       not agree, the shortcut must not copy bytes beyond the logical packet. */
    first.nx_packet_next = NX_NULL;
    first.nx_packet_length = 4UL;
    memset(out, 0xA5, sizeof(out));
    moved = 99UL;
    status = bsd_packet_extract(&first, 0UL, out, 8UL, &moved);
    CHECK(status == NX_SUCCESS);
    CHECK(moved == 4UL);
    CHECK(memcmp(out, "abcd", 4) == 0);
    CHECK(out[4] == 0xA5);

    CHECK(bsd_packet_length(&first) == 4UL);
    CHECK(bsd_packet_length(NX_NULL) == 0UL);

    puts("packet extract: single-buffer shortcut and chain fallback");
    return 0;
}

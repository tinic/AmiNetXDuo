/*
 * AmiNetXDuo: bounded IPv6 fragment-copy contract (N-020).
 * SPDX-License-Identifier: MIT
 *
 * Links the actual vendor function with real NX_PACKET declarations and the
 * existing m68k-width ULONG shim. Buffers deliberately have generous slack:
 * the regression is an excess store within allocated memory, not a fault or
 * network reproduction. Test word/chunk boundaries and untouched slack as
 * well as the caller-visible cursor/length contract.
 */
#include "nx_api.h"
#include "nx_ipv6.h"
#include <stdio.h>
#include <string.h>

enum { CAPACITY = 256, LIMIT = 128, POISON = 0xd7 };
union storage { ULONG align; UCHAR bytes[CAPACITY]; };
_Static_assert(sizeof(ULONG) == 4, "copy words must have target width");

static unsigned checks, failures;

static void check(int good, const char *what, unsigned size, unsigned layout)
{
    ++checks;
    if (!good)
    {
        ++failures;
        printf("FAIL %s size=%u layout=%u\n", what, size, layout);
    }
}

static void init_packet(NX_PACKET *p, union storage *data, unsigned available,
                        int source)
{
    *p = (NX_PACKET){0};
    p->nx_packet_data_start = data->bytes;
    p->nx_packet_data_end = data->bytes + available;
    p->nx_packet_prepend_ptr = data->bytes;
    p->nx_packet_append_ptr = data->bytes + (source ? available : 0);
    p->nx_packet_length = available;
    p->nx_packet_last = p;
}

static UCHAR pattern(unsigned offset)
{
    return (UCHAR)((offset * 37u + 19u) & 255u);
}

static void run_copy(unsigned size, unsigned layout)
{
    NX_PACKET src[2], dst[2];
    union storage s[2], d[2];
    unsigned sc = layout == 1 ? 32u : LIMIT;
    unsigned dc = layout == 2 ? 64u : LIMIT;
    unsigned i, j, source_used, dest_used;
    UINT rc;

    for (i = 0; i < 2; ++i)
    {
        for (j = 0; j < CAPACITY; ++j)
        {
            s[i].bytes[j] = pattern(i * sc + j);
            d[i].bytes[j] = POISON;
        }
        init_packet(&src[i], &s[i], i ? LIMIT : sc, 1);
        init_packet(&dst[i], &d[i], i ? LIMIT : dc, 0);
    }
    src[0].nx_packet_next = &src[1];
    dst[0].nx_packet_next = &dst[1];
    rc = _nx_ipv6_packet_copy(&src[0], &dst[0], size);
    check(rc == NX_SUCCESS, "success", size, layout);

    for (i = 0; i < 2; ++i)
    {
        source_used = i ? (size > sc ? size - sc : 0u)
                        : (size < sc ? size : sc);
        dest_used = i ? (size > dc ? size - dc : 0u)
                      : (size < dc ? size : dc);
        check(src[i].nx_packet_prepend_ptr == s[i].bytes + source_used,
              "source cursor", size, layout);
        check(dst[i].nx_packet_append_ptr == d[i].bytes + dest_used,
              "destination cursor", size, layout);
        check(dst[i].nx_packet_length == (i ? LIMIT : dc) - dest_used,
              "destination remaining capacity", size, layout);
        for (j = 0; j < CAPACITY; ++j)
        {
            check(s[i].bytes[j] == pattern(i * sc + j),
                  "source unchanged", size, layout);
            check(d[i].bytes[j] == (j < dest_used ? pattern(i * dc + j) : POISON),
                  "requested bytes and untouched slack", size, layout);
        }
    }
    check(src[0].nx_packet_last == &src[size > sc ? 1 : 0],
          "source last cursor", size, layout);
    check(dst[0].nx_packet_last == &dst[size > dc ? 1 : 0],
          "destination last cursor", size, layout);
}

static void run_missing_chain(int missing_source)
{
    NX_PACKET src, dst;
    union storage s, d;
    unsigned i;
    UINT rc;

    for (i = 0; i < CAPACITY; ++i)
    {
        s.bytes[i] = pattern(i);
        d.bytes[i] = POISON;
    }
    init_packet(&src, &s, missing_source ? 32u : LIMIT, 1);
    init_packet(&dst, &d, missing_source ? LIMIT : 32u, 0);
    rc = _nx_ipv6_packet_copy(&src, &dst, 64);
    check(rc == NX_NOT_SUCCESSFUL, "missing chain status", 64,
          (unsigned)missing_source + 3u);
    check(src.nx_packet_prepend_ptr == s.bytes + 32 &&
          dst.nx_packet_append_ptr == d.bytes + 32,
          "partial progress preserved", 64, (unsigned)missing_source + 3u);
    for (i = 0; i < CAPACITY; ++i)
        check(d.bytes[i] == (i < 32 ? pattern(i) : POISON),
              "partial copy and untouched slack", 64, (unsigned)missing_source + 3u);
}

int main(void)
{
    unsigned size, layout;
    for (layout = 0; layout < 3; ++layout)
        for (size = 0; size <= LIMIT; ++size)
            run_copy(size, layout);
    run_missing_chain(0);
    run_missing_chain(1);
    printf("IPv6 fragment copy: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}

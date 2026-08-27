/*
 * tls.library's own NX_PACKET pool, src/tlslib/tls_packet.c.
 *
 * This code replaced a borrowed pool from bsdsocket.library, and nx_secure
 * reads NX_PACKET's fields directly rather than through an interface, so every
 * way of getting it wrong is a wild pointer inside a handshake on a machine
 * with no MMU.  The chain arithmetic in particular is what a decrypted record
 * larger than one block lands on, and a short answer there is a truncated
 * plaintext, not a crash.
 *
 * SPDX-License-Identifier: MIT
 */

#include "tls_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks;
static int failures;

#define CHECK(expr)                                                        \
    do {                                                                   \
        checks++;                                                          \
        if (!(expr))                                                       \
        {                                                                  \
            failures++;                                                    \
            printf("  FAIL line %d: %s\n", __LINE__, #expr);               \
        }                                                                  \
    } while (0)

VOID tls_bzero(APTR ptr, ULONG size)
{
    memset(ptr, 0, (size_t)size);
}

VOID tls_memcpy(APTR dst, const void *src, ULONG size)
{
    memcpy(dst, src, (size_t)size);
}

/* ------------------------------------------------------------------------ */

typedef struct Fixture
{
    NX_PACKET_POOL pool;
    ULONG          packets;
    void          *memory;
} Fixture;

static void fixture_open(Fixture *f, ULONG record_bytes)
{
    f->packets = tls_packet_pool_count(record_bytes);
    f->memory  = malloc((size_t)tls_packet_pool_bytes(f->packets));

    CHECK(f->memory != NULL);
    CHECK(tls_packet_pool_create(&f->pool, f->memory, f->packets) == NX_SUCCESS);
    CHECK(f->pool.nx_packet_pool_available == f->packets);
    CHECK(f->pool.nx_packet_pool_total == f->packets);
}

static void fixture_close(Fixture *f)
{
    tls_packet_pool_delete(&f->pool);
    free(f->memory);
}

/* --------------------------------------------------------------- sizing --- */

static void test_count(void)
{
    printf("tls_packet: the block count follows TLSA_RecordBuffer\n");

    /* The default record buffer is TLS_DEFAULT_RECORD_BUFFER, and a decrypted
       record that big is a chain of ceil(10240/2560) blocks. */
    CHECK(tls_packet_pool_count(TLS_DEFAULT_RECORD_BUFFER) ==
          (TLS_DEFAULT_RECORD_BUFFER / TLS_PACKET_PAYLOAD) + TLS_PACKET_SPARE);

    /* Not a multiple, and not zero: both round up to a whole block. */
    CHECK(tls_packet_pool_count(TLS_PACKET_PAYLOAD + 1) == 2 + TLS_PACKET_SPARE);
    CHECK(tls_packet_pool_count(0) == 1 + TLS_PACKET_SPARE);

    /* A caller that raises the ceiling pays for the chain it just allowed. */
    CHECK(tls_packet_pool_count(16384) > tls_packet_pool_count(4096));
}

/* ------------------------------------------------------------- allocate --- */

static void test_allocate(void)
{
    Fixture    f;
    NX_PACKET *packet = NX_NULL;
    NX_PACKET *held[64];
    ULONG      i;

    printf("tls_packet: allocate honours the prepend reserve and the count\n");

    fixture_open(&f, TLS_DEFAULT_RECORD_BUFFER);

    /* A receive packet has no reserve: the whole block is payload. */
    CHECK(_nx_packet_allocate(&f.pool, &packet, NX_RECEIVE_PACKET, 0) ==
          NX_SUCCESS);
    CHECK(packet != NX_NULL);
    CHECK(packet->nx_packet_prepend_ptr == packet->nx_packet_data_start);
    CHECK(packet->nx_packet_append_ptr == packet->nx_packet_prepend_ptr);
    CHECK(packet->nx_packet_length == 0);
    CHECK(packet->nx_packet_next == NX_NULL);
    CHECK((ULONG)(packet->nx_packet_data_end - packet->nx_packet_data_start) ==
          f.pool.nx_packet_pool_payload_size);
    CHECK(packet->nx_packet_pool_owner == &f.pool);
    CHECK(f.pool.nx_packet_pool_available == f.packets - 1);

    /*
     * A TCP packet reserves exactly its type value, which is what
     * _nx_secure_tls_packet_allocate() then backs into for the record header
     * and the explicit IV.  NetX Duo's own allocator has the same contract.
     */
    CHECK(_nx_packet_release(packet) == NX_SUCCESS);
    CHECK(f.pool.nx_packet_pool_available == f.packets);

    CHECK(_nx_packet_allocate(&f.pool, &packet, NX_IPv4_TCP_PACKET, 0) ==
          NX_SUCCESS);
    CHECK((ULONG)(packet->nx_packet_prepend_ptr -
                  packet->nx_packet_data_start) == (ULONG)NX_IPv4_TCP_PACKET);
    CHECK((ULONG)(packet->nx_packet_data_end - packet->nx_packet_prepend_ptr) >
          NX_SECURE_TLS_RECORD_HEADER_SIZE + 16UL + TLS_WRITE_CHUNK);
    CHECK(_nx_packet_release(packet) == NX_SUCCESS);

    /* A reserve past the end of the block is a caller error, never a short
       packet: nx_secure would write the record header outside it. */
    CHECK(_nx_packet_allocate(&f.pool, &packet,
                              f.pool.nx_packet_pool_payload_size + 1UL, 0) ==
          NX_OPTION_ERROR);

    /* Exhaustion answers rather than suspends -- there is no ThreadX here to
       suspend on -- and it is counted, so the sizing can be second-guessed. */
    CHECK(f.packets <= (ULONG)(sizeof(held) / sizeof(held[0])));
    for (i = 0; i < f.packets; i++)
    {
        held[i] = NX_NULL;
        CHECK(_nx_packet_allocate(&f.pool, &held[i], 0, 0) == NX_SUCCESS);
    }
    CHECK(f.pool.nx_packet_pool_available == 0);
    packet = NX_NULL;
    CHECK(_nx_packet_allocate(&f.pool, &packet, 0, NX_WAIT_FOREVER) ==
          NX_NO_PACKET);
    CHECK(packet == NX_NULL);
    CHECK(f.pool.nx_packet_pool_empty_requests == 1);

    for (i = 0; i < f.packets; i++)
        CHECK(_nx_packet_release(held[i]) == NX_SUCCESS);
    CHECK(f.pool.nx_packet_pool_available == f.packets);

    fixture_close(&f);
}

/* --------------------------------------------------------------- append --- */

static void test_append_chain(void)
{
    Fixture    f;
    NX_PACKET *packet = NX_NULL;
    UCHAR     *source;
    ULONG      total;
    ULONG      i;
    ULONG      links = 0;
    NX_PACKET *walk;

    printf("tls_packet: append chains past one block and keeps the length\n");

    fixture_open(&f, TLS_DEFAULT_RECORD_BUFFER);

    /* Two and a half blocks, so the chain is three long and the last one is
       partly filled: the shape a decrypted record arrives in. */
    total  = (TLS_PACKET_PAYLOAD * 2UL) + (TLS_PACKET_PAYLOAD / 2UL);
    source = (UCHAR *)malloc((size_t)total);
    CHECK(source != NULL);
    for (i = 0; i < total; i++)
        source[i] = (UCHAR)(i & 0xFFU);

    CHECK(_nx_packet_allocate(&f.pool, &packet, 0, 0) == NX_SUCCESS);
    CHECK(_nx_packet_data_append(packet, source, total, &f.pool, 0) ==
          NX_SUCCESS);

    CHECK(packet->nx_packet_length == total);
    CHECK(packet->nx_packet_next != NX_NULL);
    CHECK(packet->nx_packet_last != NX_NULL);

    for (walk = packet; walk != NX_NULL; walk = walk->nx_packet_next)
    {
        links++;
        if (walk->nx_packet_next == NX_NULL)
            CHECK(walk == packet->nx_packet_last);
    }
    CHECK(links == 3);
    CHECK(f.pool.nx_packet_pool_available == f.packets - 3);

    /* Every byte, in order, across the joins. */
    {
        UCHAR *out    = (UCHAR *)malloc((size_t)total);
        ULONG  copied = 0;

        CHECK(out != NULL);
        CHECK(_nx_packet_data_extract_offset(packet, 0, out, total, &copied) ==
              NX_SUCCESS);
        CHECK(copied == total);
        CHECK(memcmp(out, source, (size_t)total) == 0);

        /* An offset that lands inside the second block, and a buffer that
           stops inside the third: both joins crossed in one call. */
        copied = 0;
        CHECK(_nx_packet_data_extract_offset(packet, TLS_PACKET_PAYLOAD + 7UL,
                                             out, total - TLS_PACKET_PAYLOAD,
                                             &copied) == NX_SUCCESS);
        CHECK(copied == total - TLS_PACKET_PAYLOAD - 7UL);
        CHECK(memcmp(out, &source[TLS_PACKET_PAYLOAD + 7UL],
                     (size_t)copied) == 0);

        /* Past the end is zero bytes and success, not a short read of
           whatever was in the buffer. */
        copied = 0xFFFFFFFFUL;
        CHECK(_nx_packet_data_extract_offset(packet, total, out, total,
                                             &copied) == NX_SUCCESS);
        CHECK(copied == 0);

        free(out);
    }

    /* Releasing the head releases the chain, or the pool leaks a block per
       record and the connection dies after a few kilobytes. */
    CHECK(_nx_packet_release(packet) == NX_SUCCESS);
    CHECK(f.pool.nx_packet_pool_available == f.packets);

    free(source);
    fixture_close(&f);
}

static void test_append_exhausted(void)
{
    Fixture    f;
    NX_PACKET *packet = NX_NULL;
    UCHAR     *source;
    ULONG      total;

    printf("tls_packet: an append that cannot chain fails, it does not tear\n");

    fixture_open(&f, 4096);

    /* More than the whole pool can hold. */
    total  = (f.packets + 2UL) * TLS_PACKET_PAYLOAD;
    source = (UCHAR *)calloc(1, (size_t)total);
    CHECK(source != NULL);

    CHECK(_nx_packet_allocate(&f.pool, &packet, 0, 0) == NX_SUCCESS);
    CHECK(_nx_packet_data_append(packet, source, total, &f.pool, 0) ==
          NX_NO_PACKET);

    /* What was written is still coherent, and releasing the head still
       returns every block the failed append had taken. */
    CHECK(packet->nx_packet_length == f.packets * TLS_PACKET_PAYLOAD);
    CHECK(_nx_packet_release(packet) == NX_SUCCESS);
    CHECK(f.pool.nx_packet_pool_available == f.packets);

    free(source);
    fixture_close(&f);
}

/* ---------------------------------------------------------------- guards --- */

static void test_guards(void)
{
    Fixture    f;
    NX_PACKET *packet = NX_NULL;
    ULONG      copied = 0;

    printf("tls_packet: a pool that was never created is refused\n");

    fixture_open(&f, 4096);

    CHECK(_nx_packet_allocate(NX_NULL, &packet, 0, 0) == NX_PTR_ERROR);
    CHECK(_nx_packet_allocate(&f.pool, NX_NULL, 0, 0) == NX_PTR_ERROR);
    CHECK(_nx_packet_release(NX_NULL) == NX_PTR_ERROR);
    CHECK(_nx_packet_data_extract_offset(NX_NULL, 0, &copied, 4, &copied) ==
          NX_PTR_ERROR);

    /* After delete the pool no longer answers: a TLSClose() racing a
       TLSRead() on the same connection must not hand out freed memory. */
    tls_packet_pool_delete(&f.pool);
    CHECK(_nx_packet_allocate(&f.pool, &packet, 0, 0) == NX_PTR_ERROR);

    free(f.memory);
}

int main(void)
{
    test_count();
    test_allocate();
    test_append_chain();
    test_append_exhausted();
    test_guards();

    printf("%d checks, %d failure(s)\n", checks, failures);

    return (failures == 0) ? 0 : 1;
}

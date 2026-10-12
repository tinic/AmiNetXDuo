/* Exercise the real port arena allocation and link-time library override.
 * SPDX-License-Identifier: MIT
 */
#include "tx_amiga_memory.h"
#include "tx_amiga.h"
#include <exec/memory.h>
#include <proto/exec.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

VOID *_tx_initialize_unused_memory;
struct ExecBase *SysBase;
static unsigned allocations, releases;
static ULONG allocated_size;
static BOOL fail_allocation;
static APTR owned;
static unsigned checks;

#define CHECK(condition) do {     ++checks;     if (!(condition)) {         fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition);         exit(1);     } } while (0)

APTR AllocMem(ULONG size, ULONG flags)
{
    ++allocations;
    CHECK(size == (ULONG) EXPECTED_ARENA_SIZE);
    CHECK(flags == (MEMF_PUBLIC | MEMF_CLEAR));
    if (fail_allocation)
        return NULL;
    CHECK(owned == NULL);
    owned = calloc(1, size);
    CHECK(owned != NULL);
    allocated_size = size;
    return owned;
}

VOID FreeMem(APTR memory, ULONG size)
{
    ++releases;
    CHECK(memory == owned && memory != NULL);
    CHECK(size == allocated_size);
    free(memory);
    owned = NULL;
}

static VOID check_owned_restart(VOID)
{
    unsigned round;
    for (round = 0; round < 2; ++round)
    {
        unsigned before = allocations;
        _tx_amiga_application_memory_init();
        CHECK(_tx_amiga_kernel_memory_size == (ULONG) EXPECTED_ARENA_SIZE);
        CHECK(_tx_initialize_unused_memory == _tx_amiga_kernel_memory);
        CHECK(allocations == before + (EXPECTED_ARENA_SIZE != 0));
        CHECK((_tx_amiga_kernel_memory != NULL) == (EXPECTED_ARENA_SIZE != 0));
        if (EXPECTED_ARENA_SIZE != 0)
        {
            ((UBYTE *) _tx_amiga_kernel_memory)[EXPECTED_ARENA_SIZE - 1] = 0x5a;
            _tx_amiga_application_memory_init();
            CHECK(allocations == before + 1);
            CHECK(((UBYTE *) _tx_initialize_unused_memory)[EXPECTED_ARENA_SIZE - 1] == 0x5a);
        }
        _tx_amiga_application_memory_release();
        CHECK(_tx_amiga_kernel_memory == NULL);
        CHECK(_tx_amiga_kernel_memory_size == 0);
        _tx_amiga_application_memory_release();
        CHECK(releases == (round + 1) * (EXPECTED_ARENA_SIZE != 0));
    }
}

static VOID check_allocation_failure(VOID)
{
    unsigned before = allocations, freed = releases;
    fail_allocation = TRUE;
    _tx_amiga_application_memory_init();
    CHECK(_tx_amiga_kernel_memory == NULL);
    CHECK(_tx_initialize_unused_memory == NULL);
    CHECK(_tx_amiga_kernel_memory_size == 0);
    CHECK(allocations == before + (EXPECTED_ARENA_SIZE != 0));
    _tx_amiga_application_memory_release();
    CHECK(releases == freed);
    fail_allocation = FALSE;
    _tx_amiga_application_memory_init();
    CHECK(_tx_amiga_kernel_memory_size == (ULONG) EXPECTED_ARENA_SIZE);
    _tx_amiga_application_memory_release();
}

static VOID check_caller_memory(VOID)
{
    UBYTE buffer[32];
    unsigned before = allocations, freed = releases;
    memset(buffer, 0xa5, sizeof(buffer));
    tx_amiga_set_kernel_memory(buffer, (ULONG) sizeof(buffer));
    _tx_amiga_application_memory_init();
    CHECK(_tx_initialize_unused_memory == buffer);
    CHECK(_tx_amiga_kernel_memory_size == sizeof(buffer));
    CHECK(allocations == before);
    _tx_amiga_application_memory_release();
    CHECK(releases == freed);
    CHECK(_tx_amiga_kernel_memory == buffer);
    CHECK(_tx_amiga_kernel_memory_size == sizeof(buffer));
    _tx_amiga_application_memory_init();
    CHECK(_tx_initialize_unused_memory == buffer);
    CHECK(allocations == before && releases == freed);
    CHECK(buffer[0] == 0xa5 && buffer[31] == 0xa5);
    tx_amiga_set_kernel_memory(NULL, 0UL);
}

int main(void)
{
    CHECK(_tx_amiga_application_memory_size() == (ULONG) EXPECTED_ARENA_SIZE);
    check_owned_restart();
    check_allocation_failure();
    check_caller_memory();
    printf("application_memory: %u checks, 0 failures, arena=%lu\n", checks,
           (unsigned long) EXPECTED_ARENA_SIZE);
    return 0;
}

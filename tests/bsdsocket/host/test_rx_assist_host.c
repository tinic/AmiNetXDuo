/* Optional receive stack ownership; the native bracket probe tests StackSwap.
 * SPDX-License-Identifier: MIT */
#include "../../../src/bsdsocket/rx_assist.c"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks, failures, allocations, frees, calls, forbidden;
static BOOL fail_alloc, alive, nested, close_in_work, no_ip;
static struct Task owner, foreign, *current;
static struct AmiSocketBase base;
static NX_IP ip;

#define CHECK(c, msg) do { checks++; if (!(c)) { failures++; \
    printf("FAIL: %s\n", msg); } } while (0)

struct Task *FindTask(const char *name) { (VOID)name; return current; }
VOID Forbid(VOID) { forbidden++; }
VOID Permit(VOID) { CHECK(forbidden > 0, "balanced exclusion"); forbidden--; }
UINT tx_amiga_exec_task_alive(VOID *task)
{
    CHECK(task == &owner, "liveness checks the original stack owner");
    return alive ? TX_TRUE : TX_FALSE;
}
APTR ami_alloc_flags(ULONG bytes, ULONG flags)
{
    CHECK(flags == (MEMF_PUBLIC | MEMF_CLEAR), "private public cleared allocation");
    allocations++;
    return fail_alloc ? NULL : calloc(1, bytes);
}
VOID ami_free(APTR p) { if (p != NULL) { frees++; free(p); } }
NX_IP *netstack_ip(VOID) { return no_ip ? NULL : &ip; }
ULONG ami_sana2_rx_assist_stack_size(VOID) { return 8192; }
UWORD ami_sana2_rx_assist(NX_IP *p, UWORD budget)
{
    calls++;
    CHECK(p == &ip && budget == 32 && base.sb_RxAssist->busy,
          "service uses the guarded opener context and aggregate allowance");
    if (nested)
        CHECK(bsd_rx_assist(&base) == 0, "recursive service cannot borrow active stack");
    if (close_in_work)
    {
        unsigned before = frees;
        bsd_rx_assist_release(&base);
        CHECK(base.sb_RxAssist == NULL && frees == before,
              "close defers freeing a live work stack until the callback returns");
    }
    return 7;
}

static void reset(void)
{
    memset(&base, 0, sizeof(base));
    allocations = frees = calls = forbidden = 0;
    fail_alloc = nested = close_in_work = no_ip = FALSE;
    alive = TRUE;
    current = base.sb_Task = base.sb_NxTask = &owner;
    base.sb_NxNest = 1;
}

int main(void)
{
    struct BsdRxAssist *ctx;
    reset();
    current = &foreign;
    CHECK(bsd_rx_assist(&base) == 0 && allocations == 0, "foreign opener skips allocation");
    current = &owner;
    base.sb_NxNest = 2;
    CHECK(bsd_rx_assist(&base) == 0 && allocations == 0, "nested bracket skips allocation");
    base.sb_NxNest = 1;
    base.sb_NxTask = &foreign;
    CHECK(bsd_rx_assist(&base) == 0 && allocations == 0, "wrong bracket owner skips allocation");
    base.sb_NxTask = &owner;
    no_ip = TRUE;
    CHECK(bsd_rx_assist(&base) == 0 && allocations == 0, "absent IP skips allocation");
    no_ip = FALSE;
    fail_alloc = TRUE;
    CHECK(bsd_rx_assist(&base) == 0 && calls == 0 && base.sb_RxAssist == NULL,
          "memory pressure skips optional service without retaining a context");
    fail_alloc = FALSE;
    nested = TRUE;
    CHECK(bsd_rx_assist(&base) == 7 && calls == 1, "one guarded callback returns its work");
    ctx = base.sb_RxAssist;
    CHECK(ctx != NULL && !ctx->busy && ctx->owner == &owner, "context returns idle and owned");
    CHECK(bsd_rx_assist(&base) == 7 && base.sb_RxAssist == ctx && allocations == 2,
          "later polling calls reuse the same stack");
    ctx->owner = &foreign;
    CHECK(bsd_rx_assist(&base) == 0 && calls == 2, "foreign context cannot be reused");
    ctx->owner = &owner;
    bsd_rx_assist_release(&base);
    CHECK(base.sb_RxAssist == NULL && frees == 1, "normal close frees the idle stack");
    bsd_rx_assist_release(&base);
    CHECK(frees == 1, "repeat release is harmless");

    reset();
    close_in_work = TRUE;
    CHECK(bsd_rx_assist(&base) == 7 && frees == 1 && base.sb_RxAssist == NULL,
          "live deferred close frees exactly once after work returns");

    reset();
    CHECK(bsd_rx_assist(&base) == 7, "create a stack for orphan cleanup");
    base.sb_RxAssist->busy = TRUE; /* owner died while executing on it */
    alive = FALSE;
    bsd_rx_assist_release(&base);
    CHECK(frees == 1 && base.sb_RxAssist == NULL,
          "proven dead owner cannot strand a busy work stack");
    CHECK(forbidden == 0, "exclusion restored on every path");
    printf("%u checks, %u failures\n", checks, failures);
    return failures != 0;
}

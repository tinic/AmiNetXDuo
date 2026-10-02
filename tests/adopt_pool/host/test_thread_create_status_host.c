/*
 * AmiNetXDuo, the port-to-core contract for a failed thread create.
 * SPDX-License-Identifier: MIT
 *
 * The port's stack builder is void; when it cannot create the backing Exec
 * Task it leaves tx_thread_amiga_task NULL and the port's
 * TX_THREAD_STACK_BUILD_STATUS reports TX_NO_MEMORY.  Only our ThreadX fork
 * (third_party/threadx 755a42f1) evaluates that hook in _tx_thread_create();
 * upstream returns TX_SUCCESS unconditionally, and sana2_rx.c and
 * netstack_dhcpv6.c would then trust a thread with no Task behind it.
 *
 * third_party/threadx/common/src/tx_thread_create.c is compiled in whole,
 * against the port's real tx_port.h.  The lock and the scheduler entry points
 * are faked below.
 *
 *   fail  builder leaves no Task: TX_NO_MEMORY, nothing published, no
 *         lock taken, preempt-disable unchanged, nothing resumed
 *   ok    builder makes a Task: TX_SUCCESS, published and resumed once
 */

#include "tx_port.h"

/* The port's TX_DISABLE is m68k asm on Exec's TDNestCnt.  Count it instead. */
static int      lock_depth;
static unsigned lock_taken;

#undef  TX_INTERRUPT_SAVE_AREA
#undef  TX_DISABLE
#undef  TX_RESTORE
#define TX_INTERRUPT_SAVE_AREA
#define TX_DISABLE              { lock_depth++; lock_taken++; }
#define TX_RESTORE              { lock_depth--; }

#include "tx_thread_create.c"

#include <stdio.h>
#include <string.h>


/* ----------------------------------------------------- fake scheduler --- */

/* tx_thread_initialize.c defines these; it is not compiled here. */
TX_THREAD          *_tx_thread_created_ptr;
ULONG               _tx_thread_created_count;
TX_THREAD          *_tx_thread_execute_ptr;
volatile UINT       _tx_thread_preempt_disable;
volatile ULONG      _tx_thread_system_state;

static int      builder_makes_task;
static unsigned resume_calls;
static char     fake_task;

VOID _tx_thread_stack_build(TX_THREAD *thread_ptr, VOID (*function_ptr)(VOID))
{
    (void) function_ptr;
    thread_ptr->tx_thread_amiga_task = builder_makes_task ? (VOID *) &fake_task
                                                          : (VOID *) 0;
}

VOID _tx_thread_shell_entry(VOID) { }

VOID _tx_thread_timeout(ULONG timeout_input) { (void) timeout_input; }

/* The real resume drops the preempt-disable count create raised for it. */
VOID _tx_thread_system_resume(TX_THREAD *thread_ptr)
{
    (void) thread_ptr;
    _tx_thread_preempt_disable--;
    resume_calls++;
}

VOID _tx_thread_system_ni_resume(TX_THREAD *thread_ptr)
{
    (void) thread_ptr;
    _tx_thread_preempt_disable--;
    resume_calls++;
}

VOID _tx_thread_system_preempt_check(VOID) { }


/* --------------------------------------------------------------- test --- */

static TX_THREAD    thread;
static ULONG        stack[1024];

static UINT create(void)
{
    return _tx_thread_create(&thread, (CHAR *) "contract", (VOID (*)(ULONG)) 0,
                             0, stack, sizeof stack, 16, 16,
                             TX_NO_TIME_SLICE, TX_AUTO_START);
}

#define CHECK(c) do { if (!(c)) { \
    printf("thread_create_status=FAIL check=%s line=%d\n", #c, __LINE__); \
    return 1; } } while (0)

static int test_fail(void)
{
    UINT status;

    builder_makes_task = 0;
    status = create();

    printf("thread_create_status status=%u created=%lu lock_taken=%u "
           "resumes=%u\n", status, (unsigned long) _tx_thread_created_count,
           lock_taken, resume_calls);
    CHECK(status == TX_NO_MEMORY);
    CHECK(_tx_thread_created_ptr == TX_NULL);
    CHECK(_tx_thread_created_count == 0);
    CHECK(_tx_thread_preempt_disable == 0);
    CHECK(lock_taken == 0 && lock_depth == 0);
    CHECK(resume_calls == 0);
    return 0;
}

static int test_ok(void)
{
    UINT status;

    builder_makes_task = 1;
    status = create();

    printf("thread_create_status status=%u created=%lu lock_taken=%u "
           "resumes=%u\n", status, (unsigned long) _tx_thread_created_count,
           lock_taken, resume_calls);
    CHECK(status == TX_SUCCESS);
    CHECK(_tx_thread_created_ptr == &thread);
    CHECK(_tx_thread_created_count == 1);
    CHECK(_tx_thread_preempt_disable == 0);
    CHECK(lock_depth == 0);
    CHECK(resume_calls == 1);
    return 0;
}

int main(int argc, char **argv)
{
    int rc;

    if (argc != 2)
        return 2;
    if (strcmp(argv[1], "fail") == 0)
        rc = test_fail();
    else if (strcmp(argv[1], "ok") == 0)
        rc = test_ok();
    else
        return 2;
    if (rc == 0)
        printf("thread_create_status=PASS case=%s\n", argv[1]);
    return rc;
}

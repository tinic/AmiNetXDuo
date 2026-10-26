/* Run the driver suite, then interleave the public assist and the real NetX
 * detach-begin command on host threads. Pthread mutexes model the ThreadX IP
 * mutex and Exec Forbid; this does not model Exec priority or packet delivery.
 * SPDX-License-Identifier: MIT */
#define main driver_serial_main
#define Forbid serial_Forbid
#define Permit serial_Permit
#define _tx_mutex_get serial_mutex_get
#define _txe_mutex_get serial_mutex_get_checked
#define _tx_mutex_put serial_mutex_put
#define _txe_mutex_put serial_mutex_put_checked
#define ami_sana2_rx_assist_reader serial_assist_reader
#include "test_sana2_driver_host.c"
#undef main
#undef Forbid
#undef Permit
#undef _tx_mutex_get
#undef _txe_mutex_get
#undef _tx_mutex_put
#undef _txe_mutex_put
#undef ami_sana2_rx_assist_reader

#include "nx_ip.h"
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <time.h>

static BOOL threaded, detach_first;
static pthread_mutex_t ip_lock, exec_lock;
static pthread_mutex_t control = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static _Thread_local unsigned lock_depth;
static _Thread_local BOOL detacher;
static BOOL entered, attempted, blocked, inspected, helper_done, detach_held, release_detach;
static unsigned helper_calls;

/* Synchronization failures must end the test, never leave CI waiting forever. */
static void must(int rc)
{
    if (rc != 0)
    {
        fprintf(stderr, "thread synchronization failed: %d\n", rc);
        exit(2);
    }
}

static void await_flag(const BOOL *flag)
{
    struct timespec until;
    must(clock_gettime(CLOCK_REALTIME, &until));
    until.tv_sec += 2;
    while (!*flag)
        must(pthread_cond_timedwait(&changed, &control, &until));
}

VOID Forbid(VOID)
{
    if (threaded)
        must(pthread_mutex_lock(&exec_lock));
    else
        serial_Forbid();
}

VOID Permit(VOID)
{
    if (threaded)
        must(pthread_mutex_unlock(&exec_lock));
    else
        serial_Permit();
}

UINT _tx_mutex_get(TX_MUTEX *mutex, ULONG wait)
{
    int rc;
    if (!threaded)
        return serial_mutex_get(mutex, wait);
    must(pthread_mutex_lock(&control));
    h_check(mutex == &ip.nx_ip_protection, "both operations use the same real IP mutex");
    h_check(wait == (detacher ? TX_WAIT_FOREVER : TX_NO_WAIT),
            "detach waits for ownership; optional assistance never waits");
    must(pthread_mutex_unlock(&control));
    rc = pthread_mutex_trylock(&ip_lock);
    if (detacher && !detach_first)
    {
        must(pthread_mutex_lock(&control));
        attempted = TRUE;
        blocked = (rc == EBUSY);
        must(pthread_cond_broadcast(&changed));
        /* Let the helper inspect the attempt before detach can touch iface. */
        await_flag(&inspected);
        must(pthread_mutex_unlock(&control));
    }
    if (rc == EBUSY && detacher && wait == TX_WAIT_FOREVER)
        rc = pthread_mutex_lock(&ip_lock);
    if (rc == EBUSY)
        return TX_NOT_AVAILABLE;
    must(rc);
    lock_depth++;
    return TX_SUCCESS;
}

UINT _txe_mutex_get(TX_MUTEX *mutex, ULONG wait)
{ return _tx_mutex_get(mutex, wait); }

UINT _tx_mutex_put(TX_MUTEX *mutex)
{
    if (!threaded)
        return serial_mutex_put(mutex);
    must(pthread_mutex_lock(&control));
    h_check(lock_depth != 0, "mutex release is owned by this thread");
    must(pthread_mutex_unlock(&control));
    lock_depth--;
    must(pthread_mutex_unlock(&ip_lock));
    return TX_SUCCESS;
}

UINT _txe_mutex_put(TX_MUTEX *mutex)
{ return _tx_mutex_put(mutex); }

UWORD ami_sana2_rx_assist_reader(AmiSana2Reader *rd, UWORD budget)
{
    if (!threaded)
        return serial_assist_reader(rd, budget);
    /* Simulate a drain taking and releasing the recursive inner lock before
       a handoff. The public assist must retain its outer acquisition. */
    must(pthread_mutex_lock(&control));
    helper_calls++;
    h_check(rd == &iface.reader && budget == 32 && lock_depth == 1,
            "public assist enters its helper with the selected binding protected");
    must(pthread_mutex_unlock(&control));
    must(_tx_mutex_get(&ip.nx_ip_protection, TX_NO_WAIT) == TX_SUCCESS ? 0 : EBUSY);
    must(_tx_mutex_put(&ip.nx_ip_protection) == TX_SUCCESS ? 0 : EINVAL);
    must(pthread_mutex_lock(&control));
    entered = TRUE;
    must(pthread_cond_broadcast(&changed));
    await_flag(&attempted);
    h_check(blocked && !iface.detaching && lock_depth == 1,
            "detach cannot publish after an inner drain unlock or during a helper handoff");
    helper_done = inspected = TRUE;
    must(pthread_cond_broadcast(&changed));
    must(pthread_mutex_unlock(&control));
    return 1;
}

static VOID detach_entry(NX_IP_DRIVER *request)
{
    must(pthread_mutex_lock(&control));
    h_check(lock_depth == 1 && (detach_first || helper_done),
            "real detach command reaches the driver only after assist releases ownership");
    if (detach_first)
    {
        detach_held = TRUE;
        must(pthread_cond_broadcast(&changed));
        await_flag(&release_detach);
    }
    must(pthread_mutex_unlock(&control));
    ami_sana2_driver_entry(request);
}

static void *detach_thread(void *arg)
{
    UINT status;
    ULONG value = 0;
    (void)arg;
    detacher = TRUE;
    if (!detach_first)
    {
        must(pthread_mutex_lock(&control));
        await_flag(&entered);
        must(pthread_mutex_unlock(&control));
    }
    status = _nx_ip_driver_interface_direct_command(&ip, AMI_LINK_DETACH_BEGIN, 0, &value);
    must(pthread_mutex_lock(&control));
    h_check(status == NX_SUCCESS && iface.detaching && lock_depth == 0,
            "detach eventually publishes its state and releases the IP mutex");
    must(pthread_mutex_unlock(&control));
    return NULL;
}

static void test_scheduled_detach(BOOL first)
{
    pthread_t worker;
    UWORD took;
    fixture_init(AMI_ETH_ADDR_SIZE);
    detach_first = first;
    entered = attempted = blocked = inspected = helper_done = detach_held = release_detach = FALSE;
    helper_calls = 0;
    iface.reader.iface = &iface;
    ip.nx_ip_interface[0].nx_interface_link_driver_entry = detach_entry;
    h_check(ami_sana2_attach(&iface, &ip, 0) == AMI_NET_OK, "attach the concurrent-test binding");
    must(pthread_create(&worker, NULL, detach_thread, NULL));
    if (first)
    {
        must(pthread_mutex_lock(&control));
        await_flag(&detach_held);
        must(pthread_mutex_unlock(&control));
    }
    took = ami_sana2_rx_assist(&ip, 32);
    must(pthread_mutex_lock(&control));
    h_check(took == (first ? 0 : 1),
            "public assist returns its work count or skips a mutex held by teardown");
    must(pthread_mutex_unlock(&control));
    if (first)
    {
        must(pthread_mutex_lock(&control));
        h_check(helper_calls == 0, "busy-lock assistance never touches the reader");
        release_detach = TRUE;
        must(pthread_cond_broadcast(&changed));
        must(pthread_mutex_unlock(&control));
    }
    must(pthread_join(worker, NULL));
    h_check(ami_sana2_rx_assist(&ip, 32) == 0 && helper_calls == (first ? 0U : 1U),
            "subsequent assistance skips the detaching interface");
    h_check(lock_depth == 0, "assistance leaves no recursive mutex acquisition behind");
    ami_sana2_unbind(&iface);
}

int main(void)
{
    pthread_mutexattr_t attr;
    (void)driver_serial_main();
    must(pthread_mutexattr_init(&attr));
    must(pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE));
    must(pthread_mutex_init(&ip_lock, &attr));
    must(pthread_mutex_init(&exec_lock, &attr));
    must(pthread_mutexattr_destroy(&attr));
    threaded = TRUE;
    test_scheduled_detach(FALSE);
    test_scheduled_detach(TRUE);
    must(pthread_mutex_destroy(&ip_lock));
    must(pthread_mutex_destroy(&exec_lock));
    printf("driver including scheduled detach: %lu checks, %lu failures\n", h_checks, h_failures);
    return h_failures != 0;
}

/*
 * AmiNetXDuo -- milestone 3 bring-up test.
 *
 * Brings the whole stack up against a real SANA-II device and reports what
 * actually happened:
 *
 *   - netstack_startup() reads DEVS:NetInterfaces, opens the device, starts
 *     ThreadX, creates the pool and the NX_IP, runs DHCP and blocks for an
 *     address;
 *   - the address, netmask and gateway are read back from the live NX_IP;
 *   - ICMP echo to the gateway proves the SANA-II TX path, the ARP exchange,
 *     the RX reader threads and the baton hand-back all work end to end;
 *   - a DNS lookup proves the resolver, if the lease brought name servers;
 *   - netstack_shutdown() tears it back down.
 *
 * Shape follows tests/ram_driver/ram_driver_test.c: main() is an AmigaDOS
 * Process, output goes to the serial debug port as it happens and is replayed
 * to stdout at the end.
 *
 * SPDX-License-Identifier: MIT
 */

#include "tx_api.h"
#include "tx_amiga.h"
#include "tx_thread.h"
#include "nx_api.h"

#include "aminetxduo/netstack.h"
#include "aminetxduo/config.h"
#include "aminetxduo/crashguard.h"

#include <exec/types.h>
#include <exec/execbase.h>
#include <dos/dos.h>
#include <dos/dostags.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include <stdarg.h>


/* ------------------------------------------------------------- logging --- */

#ifndef RawPutChar
#  define RawPutChar(c) \
      LP1NR(0x204, RawPutChar, UBYTE, (c), d0, , EXEC_BASE_NAME)
#endif

#define T_LOG_SIZE      8192

static char     t_log_buffer[T_LOG_SIZE];
static ULONG    t_log_used;

static VOID t_put(UBYTE c)
{

    RawPutChar(c);

    if (t_log_used < (ULONG) (T_LOG_SIZE - 1))
    {
        t_log_buffer[t_log_used++] = (char) c;
    }
}

static VOID t_put_char(register UBYTE c      __asm("d0"),
                       register APTR  unused __asm("a3"))
{

    (VOID) unused;
    if (c != '\0')
    {
        t_put(c);
    }
}

static VOID t_log(const char *fmt, ...)
{

va_list args;


    va_start(args, fmt);
    RawDoFmt((STRPTR) fmt, args, (void (*)()) t_put_char, NULL);
    va_end(args);

    t_put('\n');
}

/*
 * Replay whatever has been logged since the last call. Incremental, because
 * the teardown step below can wedge (see the note in main()) and the results
 * gathered before it must reach stdout either way.
 */
static ULONG    t_log_flushed;

static VOID t_flush(VOID)
{

BPTR    out;


    out =  Output();
    if ((out != (BPTR) 0) && (t_log_used > t_log_flushed))
    {
        (VOID) Write(out, (APTR) &t_log_buffer[t_log_flushed],
                     (LONG) (t_log_used - t_log_flushed));
    }
    t_log_flushed =  t_log_used;
}


/* -------------------------------------------------------------- results -- */

static volatile ULONG   t_checks;
static volatile ULONG   t_failures;

static UINT t_check(UINT ok, const char *what, ULONG detail)
{

    Forbid();
    t_checks++;
    if (!ok)
    {
        t_failures++;
    }
    Permit();

    if (ok)
    {
        t_log("  ok   %s", what);
    }
    else
    {
        t_log("  FAIL %s (0x%lx)", what, detail);
    }

    return(ok);
}

#define T_OK(status, what)      t_check((UINT) ((status) == NX_SUCCESS), (what), (ULONG) (status))
#define T_NET_OK(status, what)  t_check((UINT) ((status) == AMI_NET_OK), (what), (ULONG) (status))


static VOID t_log_ip(const char *label, ULONG addr)
{

    t_log("  %s %lu.%lu.%lu.%lu", label,
          (addr >> 24) & 0xFFUL, (addr >> 16) & 0xFFUL,
          (addr >>  8) & 0xFFUL,  addr        & 0xFFUL);
}


/* ------------------------------------------------------------- watchdog --- */

/*
 * A bring-up test that hangs tells you nothing. This dumps the ThreadX
 * scheduler state -- who holds the baton, who wants it, and what every thread
 * in the system is doing -- from an ordinary Exec Process, so it keeps
 * running even when every ThreadX thread is wedged.
 */
static const char *const t_state_name[] =
{
    "READY", "COMPLETED", "TERMINATED", "SUSPENDED", "SLEEP", "QUEUE_SUSP",
    "SEMAPHORE_SUSP", "EVENT_FLAG", "BLOCK_MEMORY", "BYTE_MEMORY", "IO_DRIVER",
    "FILE", "TCP_IP", "MUTEX_SUSP", "PRIORITY_CHANGE"
};

static VOID t_dump_threads(VOID)
{

TX_THREAD   *cur;
TX_THREAD   *exe;
TX_THREAD   *walk;
ULONG        state;
ULONG        preempt;
ULONG        count;
ULONG        i;


    Forbid();
    cur     =  _tx_thread_current_ptr;
    exe     =  _tx_thread_execute_ptr;
    state   =  _tx_thread_system_state;
    preempt =  (ULONG) _tx_thread_preempt_disable;
    walk    =  _tx_thread_created_ptr;
    count   =  _tx_thread_created_count;
    Permit();

    t_log("  wd: current=%s execute=%s system_state=%ld preempt=%ld time=%ld",
          (cur != TX_NULL) ? cur -> tx_thread_name : (CHAR *) "-",
          (exe != TX_NULL) ? exe -> tx_thread_name : (CHAR *) "-",
          state, preempt, tx_time_get());

    for (i = 0; (i < count) && (walk != TX_NULL); i++)
    {

        ULONG  ts =  (ULONG) walk -> tx_thread_state;

        t_log("  wd:   %-16s pri %ld state %ld (%s) task %lx",
              walk -> tx_thread_name, (ULONG) walk -> tx_thread_priority, ts,
              (ts < (ULONG) (sizeof(t_state_name) / sizeof(t_state_name[0])))
                  ? (CHAR *) t_state_name[ts] : (CHAR *) "?",
              (ULONG) walk -> tx_thread_amiga_task);

        walk =  walk -> tx_thread_created_next;
    }
}

static VOID t_watchdog_entry(VOID)
{

ULONG   i;


    for (i = 0; i < 20UL; i++)
    {

        Delay(50UL * 10UL);          /* 10 seconds */
        t_log("watchdog tick %ld", i + 1UL);
        t_dump_threads();
    }
}

static VOID t_watchdog_start(VOID)
{

struct TagItem  tags[5];


    tags[0].ti_Tag  =  NP_Entry;
    tags[0].ti_Data =  (ULONG) t_watchdog_entry;
    tags[1].ti_Tag  =  NP_Name;
    tags[1].ti_Data =  (ULONG) "aminetxduo watchdog";
    tags[2].ti_Tag  =  NP_StackSize;
    tags[2].ti_Data =  16384UL;
    tags[3].ti_Tag  =  NP_Priority;
    tags[3].ti_Data =  5UL;
    tags[4].ti_Tag  =  TAG_DONE;
    tags[4].ti_Data =  0UL;

    (VOID) CreateNewProc(tags);
}


/* ------------------------------------------------------------- the test --- */

#define T_PING_TIMEOUT      (5UL * (ULONG) NX_IP_PERIODIC_RATE)
#define T_PING_TRIES        4
#define T_DNS_TIMEOUT       (10UL * (ULONG) NX_IP_PERIODIC_RATE)

static TX_THREAD    t_main_thread;

static const char   t_ping_data[] = "AmiNetXDuo";


static UINT t_ping(NX_IP *ip, ULONG target, const char *label)
{

NX_PACKET  *response;
UINT        status;
UINT        try;


    for (try = 0; try < T_PING_TRIES; try++)
    {

        response =  NX_NULL;
        status   =  nx_icmp_ping(ip, target, (CHAR *) t_ping_data,
                                 (ULONG) sizeof(t_ping_data) - 1UL,
                                 &response, T_PING_TIMEOUT);

        if (status == NX_SUCCESS)
        {

            ULONG length =  0;

            (VOID) nx_packet_length_get(response, &length);
            t_log("  ping %s: reply, %ld bytes, attempt %ld", label,
                  (ULONG) length, (ULONG) (try + 1));
            (VOID) nx_packet_release(response);

            return(TX_TRUE);
        }

        if (response != NX_NULL)
        {
            (VOID) nx_packet_release(response);
        }

        t_log("  ping %s: attempt %ld failed (0x%lx)", label,
              (ULONG) (try + 1), (ULONG) status);
    }

    return(TX_FALSE);
}


static VOID t_run(VOID)
{

const AmiConfig *cfg;
NX_IP           *ip;
NX_PACKET_POOL  *pool;
ULONG            address =  0UL;
ULONG            netmask =  0UL;
ULONG            gateway =  0UL;
ULONG            resolved;
UINT             status;
UWORD            i;


    cfg =  netstack_config();
    (VOID) t_check((UINT) (cfg != NULL), "config is readable", 0UL);

    if (cfg != NULL)
    {

        t_log("  hostname '%s', %ld interface(s), %ld name server(s)",
              cfg->hostname, (ULONG) cfg->interface_count,
              (ULONG) cfg->resolver.nameserver_count);

        for (i = 0; i < cfg->interface_count; i++)
        {
            t_log("  cfg[%ld] '%s' %s unit %ld iptype %ld", (ULONG) i,
                  cfg->interfaces[i].name, cfg->interfaces[i].device,
                  (ULONG) cfg->interfaces[i].unit,
                  (ULONG) cfg->interfaces[i].iptype);
        }
    }

    (VOID) t_check((UINT) (netstack_interface_count() > 0),
                   "at least one interface is up", 0UL);

    ip =  netstack_ip();
    (VOID) t_check((UINT) (ip != NULL), "NX_IP exists", 0UL);

    pool =  netstack_pool();
    (VOID) t_check((UINT) (pool != NULL), "packet pool exists", 0UL);

    if (pool != NULL)
    {

        ULONG total   =  0UL;
        ULONG free    =  0UL;
        ULONG payload =  0UL;

        (VOID) nx_packet_pool_info_get(pool, &total, &free, NX_NULL, NX_NULL,
                                       NX_NULL);
        payload =  pool -> nx_packet_pool_payload_size;

        t_log("  pool %ld packets (%ld free) of %ld bytes",
              total, free, payload);

        (VOID) t_check((UINT) (total >= (ULONG) AMI_POOL_MIN_PACKETS),
                       "pool is at least AMI_POOL_MIN_PACKETS", total);
    }

    if (ip == NULL)
    {
        return;
    }

    (VOID) t_check((UINT) (netstack_interface_is_up(0) != FALSE),
                   "interface 0 link is up", 0UL);

    /* Everything below suspends this task inside NetX Duo. */
    status =  tx_amiga_adopt_thread(&t_main_thread, "netstack test", 16);
    if (!t_check((UINT) (status == TX_SUCCESS), "adopted this Exec Task",
                 (ULONG) status))
    {
        return;
    }

    (VOID) nx_ip_address_get(ip, &address, &netmask);
    (VOID) nx_ip_gateway_address_get(ip, &gateway);

    t_log_ip("address", address);
    t_log_ip("netmask", netmask);
    t_log_ip("gateway", gateway);

    (VOID) t_check((UINT) (address != 0UL), "interface 0 has an address",
                   address);

    /* Loopback always works and isolates "is the stack alive" from "is the
       wire alive". */
    (VOID) t_check(t_ping(ip, IP_ADDRESS(127, 0, 0, 1), "loopback"),
                   "ICMP echo to loopback", 0UL);

    if (address != 0UL)
    {
        (VOID) t_check(t_ping(ip, address, "self"), "ICMP echo to self", 0UL);
    }

    if (gateway != 0UL)
    {
        (VOID) t_check(t_ping(ip, gateway, "gateway"), "ICMP echo to gateway",
                       gateway);
    }
    else
    {
        t_log("  (no gateway configured -- skipping the wire test)");
    }

    /* Resolver. */
    resolved =  0UL;
    if (netstack_resolve("localhost", &resolved, T_DNS_TIMEOUT) == AMI_NET_OK)
    {
        t_log_ip("localhost resolves to", resolved);
    }

    resolved =  0UL;
    status   =  (UINT) netstack_resolve("example.com", &resolved, T_DNS_TIMEOUT);
    if (status == (UINT) AMI_NET_OK)
    {
        t_log_ip("example.com resolves to", resolved);
        (VOID) t_check(TX_TRUE, "DNS lookup", resolved);
    }
    else
    {
        t_log("  DNS lookup of example.com failed (%ld) -- not fatal",
              (ULONG) status);
    }

    (VOID) tx_amiga_orphan_thread(&t_main_thread);
}


/* ------------------------------------------------------------------ main -- */

int main(void)
{

LONG    status;


    t_log("AmiNetXDuo -- netstack bring-up test");
    t_log("  ticks/sec %ld, pool payload %ld, pool %ld..%ld packets",
          (ULONG) TX_TIMER_TICKS_PER_SECOND, (ULONG) AMI_POOL_PAYLOAD,
          (ULONG) AMI_POOL_MIN_PACKETS, (ULONG) AMI_POOL_MAX_PACKETS);

    ami_crash_set_reference((APTR) main, "netstack_test");
    if (!ami_crash_install())
    {

        /* Resuming after a caught crash is documented as unreliable, so this
           is a report-and-die path, not a recovery path. */
        t_log("FATAL: caught a CPU exception -- see the dump above");
        t_flush();
        return(20);
    }

    t_watchdog_start();

    status =  netstack_startup();
    (VOID) T_NET_OK(status, "netstack_startup()");

    if (netstack_get() == NULL)
    {
        t_log("FATAL: no stack -- see the messages above");
        t_log("");
        t_log("%ld checks, %ld failures -- FAIL", t_checks, t_failures + 1UL);
        t_flush();
        return(20);
    }

    t_run();

    t_log("");
    t_log("%ld checks, %ld failures -- %s",
          t_checks, t_failures, (t_failures == 0UL) ? "PASS" : "FAIL");
    t_flush();

    /*
     * Teardown is reported separately, and last, because it can hang: on
     * Commodore's a2065.device 2.16 an AbortIO() on a pending SANA-II CMD_READ
     * is not honoured, so ami_sana2_rx_teardown()'s WaitIO() never returns and
     * the reader thread never posts its "exited" semaphore. Everything above
     * has already reached stdout by this point.
     */
    t_log("");
    t_log("tearing down (netstack_shutdown)");
    t_flush();

    netstack_shutdown();
    (VOID) t_check((UINT) (netstack_get() == NULL), "netstack_shutdown()", 0UL);

    t_log("%ld checks, %ld failures after teardown -- %s",
          t_checks, t_failures, (t_failures == 0UL) ? "PASS" : "FAIL");

    t_flush();
    ami_crash_remove();

    return((t_failures == 0UL) ? 0 : 20);
}

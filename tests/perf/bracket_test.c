/*
 * AmiNetXDuo -- what the ThreadX adoption bracket costs, measured.
 *
 * docs/RESEARCH.md 29.3 records two instruments disagreeing about this stack's
 * throughput: our own NetTrace makes it 55% faster than Roadshow on the wire
 * and the stack-agnostic Aminet curl makes it 12% slower, five runs out of
 * five, with the gap scaling with the body rather than sitting in setup.
 * 32.11 named a candidate without changing it: bsd_nx_enter()/bsd_nx_leave()
 * bracket every recv(), every send() and every poll pass inside WaitSelect(),
 * and src/bsdsocket/netx_call.c prices one bracket at an AllocSignal(), a
 * _tx_thread_create(), a baton acquire and their inverses.  curl reads small
 * and selects between reads; NetTrace reads 4,096 bytes through one
 * WaitSelect() loop.  A per-call constant is exactly the shape of "we win the
 * first byte and lose everything after it".
 *
 * That is a prediction.  This binary is the measurement, and it is here
 * because this project has had five predicted bottlenecks overturned by one
 * in three days.
 *
 *   1. One tx_amiga_adopt_thread() + tx_amiga_orphan_thread() pair, split
 *      into its two halves, from a plain Exec Task with the kernel up and an
 *      NX_IP running -- i.e. the real bracket in the real population.
 *   2. Its parts, so the fix has somewhere to aim: the AllocSignal()/
 *      FreeSignal() pair, the _tx_amiga_wake_scheduler() poke, and the
 *      already-adopted fast path bsd_nx_enter() takes when tx_thread_identify()
 *      is non-NULL.
 *   3. A 256 KB TCP transfer over loopback drained by the calling task at
 *      four read sizes, in three arms:
 *
 *        per-call   a bracket around every logical recv(), which is what
 *                   src/bsdsocket/transfer.c does today;
 *        on-miss    a bracket only when the parked packet is exhausted and
 *                   nx_tcp_socket_receive() has to be called;
 *        once       one bracket around the whole transfer -- the floor.
 *
 *      The difference between the first two is what a recv() that can be
 *      satisfied from already-extracted data pays for nothing.  The
 *      difference between the first and the third is the whole bracket bill
 *      for that read size.
 *
 * ONLY THE 68020 Profiles mean anything -- tests/perf/cpucal.c measures why:
 * FS-UAE charges no cycles at all above a 68020.  Run under -m A1200.
 *
 *   cmake --build build/cm --parallel --target bracket_test
 *   AMINETXDUO_RUN_TAG=brk ./tools/fsuae-run.sh -x -t 900 -m A1200 \
 *       build/cm/tests/perf/bracket_test
 *
 * SPDX-License-Identifier: MIT
 */

#include "tx_api.h"
#include "tx_amiga.h"
#include "nx_api.h"

#include "aminetxduo/compat.h"

#include <exec/types.h>
#include <exec/execbase.h>
#include <dos/dos.h>
#include <devices/timer.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/timer.h>

#include <stdarg.h>
#include <string.h>


/* ------------------------------------------------------------- logging --- */

#ifndef RawPutChar
#  define RawPutChar(c) \
      LP1NR(0x204, RawPutChar, UBYTE, (c), d0, , EXEC_BASE_NAME)
#endif

#define B_LOG_SIZE      8192

static char     b_log_buffer[B_LOG_SIZE];
static ULONG    b_log_used;

static VOID b_put(UBYTE c)
{
    RawPutChar(c);

    if (b_log_used < (ULONG)(B_LOG_SIZE - 1))
    {
        b_log_buffer[b_log_used++] = (char)c;
    }
}

static VOID b_put_char(register UBYTE c      __asm("d0"),
                       register APTR  unused __asm("a3"))
{
    (VOID)unused;
    if (c != '\0')
    {
        b_put(c);
    }
}

static VOID b_log(const char *fmt, ...)
{
va_list args;

    va_start(args, fmt);
    RawDoFmt((STRPTR)fmt, args, (void (*)())b_put_char, NULL);
    va_end(args);

    b_put('\n');
}

static VOID b_flush(VOID)
{
BPTR    out;

    out = Output();
    if (out != (BPTR)0)
    {
        (VOID)Write(out, (APTR)b_log_buffer, (LONG)b_log_used);
    }
}

static ULONG    b_checks;
static ULONG    b_failures;

static UINT b_check(UINT ok, const char *what, ULONG detail)
{
    b_checks++;
    if (!ok)
    {
        b_failures++;
        b_log("  FAIL %s (0x%lx)", (LONG)what, (LONG)detail);
    }

    return(ok);
}


/* -------------------------------------------------------------- timing --- */

extern struct Device *TimerBase;        /* src/common/compat.c owns it */

static ULONG    b_rate;
static ULONG    b_tick_ns;
static ULONG    b_per_ms;
static ULONG    b_bracket;              /* cost of one measurement, ticks */

static ULONG b_now(VOID)
{
struct EClockVal ev;

    (VOID)ReadEClock(&ev);

    return(ev.ev_lo);
}

static ULONG b_elapsed(ULONG start, ULONG stop)
{
ULONG   d = stop - start;

    return((d > b_bracket) ? (d - b_bracket) : 0UL);
}

static ULONG b_ms(ULONG ticks)
{
    return(ticks / b_per_ms);
}

/* Microseconds x100, so a sub-microsecond figure survives an integer print. */
static ULONG b_us_x100(ULONG ticks, ULONG reps)
{
    if (reps == 0UL)
    {
        return(0UL);
    }

    return(((ticks * b_tick_ns) / reps) / 10UL);
}

static VOID b_timer_init(VOID)
{
struct EClockVal ev;
ULONG            i, t0, t1, total;

    (VOID)ami_millis();                 /* opens timer.device, sets TimerBase */

    b_rate    = ReadEClock(&ev);
    b_tick_ns = 1000000000UL / b_rate;
    b_per_ms  = b_rate / 1000UL;
    if (b_per_ms == 0UL)
    {
        b_per_ms = 1UL;
    }

    b_bracket = 0UL;
    total     = 0UL;
    for (i = 0UL; i < 256UL; i++)
    {
        t0 = b_now();
        t1 = b_now();
        total += (t1 - t0);
    }
    b_bracket = total / 256UL;
}


/* -------------------------------------------------------- test fabric ---- */

#define B_PACKET_PAYLOAD    1568        /* == AMI_POOL_PAYLOAD              */
#define B_PACKET_COUNT      64
#define B_PACKET_OVERHEAD   96

#define B_IP_ADDRESS        IP_ADDRESS(192, 168, 101, 1)
#define B_IP2_ADDRESS       IP_ADDRESS(192, 168, 101, 2)
#define B_LOOPBACK          IP_ADDRESS(127, 0, 0, 1)
#define B_NETMASK           0xFFFFFF00UL
#define B_PORT              5301

#define B_IP_STACK_SIZE     3072
#define B_SRV_STACK_SIZE    4096

/* Bytes per arm.  256 KB is long enough that the per-call constant under
   test is hundreds of milliseconds at the small read sizes, and short enough
   that thirteen arms fit inside one emulator session. */
#define B_XFER_BYTES        (256UL * 1024UL)

/* What the sender hands NetX Duo per call -- the same 8 KB bsd_send_tcp()
   sees from an application, clamped to the MSS on the way out. */
#define B_SEND_CHUNK        8192UL

/* The largest read the receiver arms use. */
#define B_MAX_READ          16384UL

/* The bracket policies under test, and how many arms the sender must serve. */
#define B_ARM_PER_CALL      0
#define B_ARM_ON_MISS       1
#define B_ARM_ONCE          2
#define B_ARM_CACHED        3       /* per-call, resume/suspend not adopt/orphan */
#define B_ARM_COUNT         16UL

extern VOID _nx_ram_network_driver(NX_IP_DRIVER *driver_req_ptr);

static NX_PACKET_POOL   b_pool;
static NX_IP            b_ip;
static NX_IP            b_ip2;         /* see tx_application_define() */
static NX_TCP_SOCKET    b_client;
static NX_TCP_SOCKET    b_server;

static TX_THREAD        b_srv_thread;
static TX_THREAD        b_caller;       /* the adopted TX_THREAD under test */
static TX_SEMAPHORE     b_srv_ready;
static TX_SEMAPHORE     b_srv_accepted;
static TX_SEMAPHORE     b_srv_go;
static TX_SEMAPHORE     b_srv_done;

static ULONG            b_pool_memory[(B_PACKET_COUNT * (B_PACKET_PAYLOAD + B_PACKET_OVERHEAD)) / sizeof(ULONG)];
static ULONG            b_ip_stack[B_IP_STACK_SIZE / sizeof(ULONG)];
static ULONG            b_ip2_stack[B_IP_STACK_SIZE / sizeof(ULONG)];
static ULONG            b_srv_stack[B_SRV_STACK_SIZE / sizeof(ULONG)];
static ULONG            b_arp_cache[1024 / sizeof(ULONG)];
static ULONG            b_arp2_cache[1024 / sizeof(ULONG)];

static UCHAR            b_src_buf[B_SEND_CHUNK] __attribute__((aligned(4)));
static UCHAR            b_dst_buf[B_MAX_READ]   __attribute__((aligned(4)));

static ULONG            b_srv_sent;
static UINT             b_srv_status;
static UINT             b_srv_up;


/* ---------------------------------------------------------- the bracket -- */

/*
 * The same two calls src/netstack/netstack.c's ami_netstack_enter() /
 * ami_netstack_leave() make, and therefore the same two src/bsdsocket/
 * netx_call.c's bsd_nx_enter() / bsd_nx_leave() make.  Open-coded here rather
 * than linked so this binary needs no netstack singleton, no config file and
 * no SANA-II device.
 */
static UINT     b_cache_live;           /* the cached TX_THREAD exists, dormant */

static UINT b_enter(VOID)
{
    if (tx_thread_identify() != TX_NULL)
    {
        return(TX_SUCCESS);             /* nested: nothing to do */
    }

    if (b_cache_live != 0U &&
        tx_amiga_adopt_resume(&b_caller) == TX_SUCCESS)
    {
        return(TX_SUCCESS);
    }

    return(tx_amiga_adopt_thread(&b_caller, (CHAR *)"bracket caller", 16));
}

static VOID b_leave(VOID)
{
    if (b_cache_live != 0U &&
        tx_amiga_adopt_suspend(&b_caller) == TX_SUCCESS)
    {
        return;
    }

    (VOID)tx_amiga_orphan_thread(&b_caller);
}


/* --------------------------------------------------- micro benchmarks ----- */

static VOID b_bench_bracket(VOID)
{
ULONG   reps = 400UL;
ULONG   i, t0, t1, t2;
ULONG   adopt = 0UL, orphan = 0UL;
ULONG   ticks;
BYTE    sig;
UINT    status;

    /*
     * The pair, halves timed separately.  Reading the clock between them
     * costs one calibrated bracket each, which b_elapsed() subtracts; at
     * ~1.4 us a tick against a pair this size that is noise, and timing the
     * halves is worth it because they are not symmetric.
     */
    for (i = 0UL; i < reps; i++)
    {
        t0     = b_now();
        status = tx_amiga_adopt_thread(&b_caller, (CHAR *)"bracket caller", 16);
        t1     = b_now();
        if (status != TX_SUCCESS)
        {
            (VOID)b_check(0U, "adopt during micro benchmark", status);
            return;
        }
        (VOID)tx_amiga_orphan_thread(&b_caller);
        t2 = b_now();

        adopt  += b_elapsed(t0, t1);
        orphan += b_elapsed(t1, t2);
    }

    b_log("  %-38s %5ld.%02ld us", (LONG)"adopt + orphan pair",
          (LONG)(b_us_x100(adopt + orphan, reps) / 100UL),
          (LONG)(b_us_x100(adopt + orphan, reps) % 100UL));
    b_log("  %-38s %5ld.%02ld us", (LONG)"  ... adopt half",
          (LONG)(b_us_x100(adopt, reps) / 100UL),
          (LONG)(b_us_x100(adopt, reps) % 100UL));
    b_log("  %-38s %5ld.%02ld us", (LONG)"  ... orphan half",
          (LONG)(b_us_x100(orphan, reps) / 100UL),
          (LONG)(b_us_x100(orphan, reps) % 100UL));

    /* The signal allocation the adoption cannot do on anyone else's behalf. */
    t0 = b_now();
    for (i = 0UL; i < reps; i++)
    {
        sig = AllocSignal(-1);
        if (sig >= 0)
        {
            FreeSignal(sig);
        }
    }
    ticks = b_elapsed(t0, b_now());
    b_log("  %-38s %5ld.%02ld us", (LONG)"  ... AllocSignal + FreeSignal",
          (LONG)(b_us_x100(ticks, reps) / 100UL),
          (LONG)(b_us_x100(ticks, reps) % 100UL));

    /* The scheduler poke both halves end with.  Signal() to a task that is
       not waiting is the cheap case; this is that case, so it is a floor. */
    t0 = b_now();
    for (i = 0UL; i < reps; i++)
    {
        _tx_amiga_wake_scheduler();
    }
    ticks = b_elapsed(t0, b_now());
    b_log("  %-38s %5ld.%02ld us", (LONG)"  ... wake_scheduler poke",
          (LONG)(b_us_x100(ticks, reps) / 100UL),
          (LONG)(b_us_x100(ticks, reps) % 100UL));

    /* And the nested path: bsd_nx_enter() with sb_NxNest > 0 never gets this
       far, but ami_netstack_enter() does, and this is what it costs. */
    status = tx_amiga_adopt_thread(&b_caller, (CHAR *)"bracket caller", 16);
    if (b_check((UINT)(status == TX_SUCCESS), "adopt for nested measurement",
                status))
    {
        t0 = b_now();
        for (i = 0UL; i < reps * 10UL; i++)
        {
            (VOID)tx_thread_identify();
        }
        ticks = b_elapsed(t0, b_now());
        b_log("  %-38s %5ld.%02ld us", (LONG)"  ... nested (tx_thread_identify)",
              (LONG)(b_us_x100(ticks, reps * 10UL) / 100UL),
              (LONG)(b_us_x100(ticks, reps * 10UL) % 100UL));

        (VOID)tx_amiga_orphan_thread(&b_caller);
    }

    /*
     * And the cached bracket: the same baton handoff with the registration
     * kept.  Adopt once, then time resume/suspend pairs, then orphan.
     */
    status = tx_amiga_adopt_thread(&b_caller, (CHAR *)"bracket caller", 16);
    if (b_check((UINT)(status == TX_SUCCESS), "adopt for cached measurement",
                status))
    {
        adopt  = 0UL;
        orphan = 0UL;

        (VOID)tx_amiga_adopt_suspend(&b_caller);

        for (i = 0UL; i < reps; i++)
        {
            t0     = b_now();
            status = tx_amiga_adopt_resume(&b_caller);
            t1     = b_now();
            if (status != TX_SUCCESS)
            {
                (VOID)b_check(0U, "resume during micro benchmark", status);
                break;
            }
            (VOID)tx_amiga_adopt_suspend(&b_caller);
            t2 = b_now();

            adopt  += b_elapsed(t0, t1);
            orphan += b_elapsed(t1, t2);
        }

        b_log("  %-38s %5ld.%02ld us", (LONG)"resume + suspend pair (cached)",
              (LONG)(b_us_x100(adopt + orphan, reps) / 100UL),
              (LONG)(b_us_x100(adopt + orphan, reps) % 100UL));
        b_log("  %-38s %5ld.%02ld us", (LONG)"  ... resume half",
              (LONG)(b_us_x100(adopt, reps) / 100UL),
              (LONG)(b_us_x100(adopt, reps) % 100UL));
        b_log("  %-38s %5ld.%02ld us", (LONG)"  ... suspend half",
              (LONG)(b_us_x100(orphan, reps) / 100UL),
              (LONG)(b_us_x100(orphan, reps) % 100UL));

        (VOID)tx_amiga_adopt_resume(&b_caller);
        (VOID)tx_amiga_orphan_thread(&b_caller);
    }

    /* Forbid()/Permit(), for scale: the uninterruptible section every ThreadX
       service call on this port opens with. */
    t0 = b_now();
    for (i = 0UL; i < reps * 10UL; i++)
    {
        Forbid();
        Permit();
    }
    ticks = b_elapsed(t0, b_now());
    b_log("  %-38s %5ld.%02ld us", (LONG)"  ... one Forbid/Permit pair",
          (LONG)(b_us_x100(ticks, reps * 10UL) / 100UL),
          (LONG)(b_us_x100(ticks, reps * 10UL) % 100UL));
}


/* ------------------------------------------------------- the transfer ----- */

/*
 * The sender is a ThreadX thread, so it never brackets anything: the variable
 * under test is what the RECEIVER pays, and putting the sender on a native
 * thread keeps it out of the measurement.
 */
static VOID b_server_entry(ULONG id)
{
UINT        status;
ULONG       sent, arm;
NX_PACKET  *pkt;

    (VOID)id;

    (VOID)tx_semaphore_get(&b_srv_ready, TX_WAIT_FOREVER);

    /*
     * The listen and the accept happen HERE, on a native ThreadX thread, and
     * the client connects from the Exec Task in main().  That is the shape
     * tests/perf/perf_test.c uses and it is not cosmetic: a listen armed from
     * the same task that then blocks in connect() never completes on this
     * port.
     */
    status = nx_tcp_socket_create(&b_ip, &b_server, "bracket server",
                                  NX_IP_NORMAL, NX_FRAGMENT_OKAY,
                                  NX_IP_TIME_TO_LIVE, 16384, NX_NULL, NX_NULL);
    (VOID)b_check((UINT)(status == NX_SUCCESS), "server socket create", status);

    status = nx_tcp_server_socket_listen(&b_ip, B_PORT, &b_server, 4, NX_NULL);
    (VOID)b_check((UINT)(status == NX_SUCCESS), "listen", status);

    status = nx_tcp_server_socket_accept(&b_server, 30UL * NX_IP_PERIODIC_RATE);
    if (!b_check((UINT)(status == NX_SUCCESS), "accept", status))
    {
        b_srv_up = 0U;
    }
    else
    {
        b_srv_up = 1U;
    }

    (VOID)tx_semaphore_put(&b_srv_accepted);

    for (arm = 0UL; arm < B_ARM_COUNT; arm++)
    {
        (VOID)tx_semaphore_get(&b_srv_go, TX_WAIT_FOREVER);

        sent         = 0UL;
        b_srv_status = NX_SUCCESS;

        while (b_srv_up != 0U && sent < B_XFER_BYTES)
        {
        ULONG chunk = B_XFER_BYTES - sent;

            if (chunk > B_SEND_CHUNK)
            {
                chunk = B_SEND_CHUNK;
            }

            pkt    = NX_NULL;
            status = nx_packet_allocate(&b_pool, &pkt, NX_TCP_PACKET,
                                        10UL * NX_IP_PERIODIC_RATE);
            if (status != NX_SUCCESS)
            {
                b_srv_status = status;
                break;
            }

            status = nx_packet_data_append(pkt, b_src_buf, chunk, &b_pool,
                                           10UL * NX_IP_PERIODIC_RATE);
            if (status != NX_SUCCESS)
            {
                (VOID)nx_packet_release(pkt);
                b_srv_status = status;
                break;
            }

            status = nx_tcp_socket_send(&b_server, pkt,
                                        30UL * NX_IP_PERIODIC_RATE);
            if (status != NX_SUCCESS)
            {
                (VOID)nx_packet_release(pkt);
                b_srv_status = status;
                break;
            }

            sent += chunk;
        }

        b_srv_sent = sent;

        (VOID)tx_semaphore_put(&b_srv_done);
    }

    (VOID)nx_tcp_socket_disconnect(&b_server, 2UL * NX_IP_PERIODIC_RATE);
    (VOID)nx_tcp_server_socket_unaccept(&b_server);
    (VOID)nx_tcp_server_socket_unlisten(&b_ip, B_PORT);
    (VOID)nx_tcp_socket_delete(&b_server);

    (VOID)tx_semaphore_put(&b_srv_done);

    (VOID)tx_thread_suspend(&b_srv_thread);
}


/*
 * Drain B_XFER_BYTES with reads of `read_size`, under one of the three
 * bracket policies.  The packet bookkeeping is deliberately the same shape as
 * bsd_recv_tcp()'s: a partially drained packet is parked and the next call
 * continues out of it.
 *
 * Returns elapsed E-Clock ticks; *calls_out and *misses_out report how many
 * logical recv() calls were made and how many of them had to reach
 * nx_tcp_socket_receive().
 */
static ULONG b_drain(ULONG read_size, UINT arm, ULONG *calls_out,
                     ULONG *misses_out)
{
NX_PACKET  *pending = NX_NULL;
ULONG       offset  = 0UL;
ULONG       got     = 0UL;
ULONG       calls   = 0UL;
ULONG       misses  = 0UL;
ULONG       t0;
UINT        status;

    t0 = b_now();

    if (arm == B_ARM_ONCE)
    {
        (VOID)b_enter();
    }

    while (got < B_XFER_BYTES)
    {
    ULONG   want = read_size;
    ULONG   copied = 0UL;
    UINT    held = 0U;

        calls++;

        if (arm == B_ARM_PER_CALL || arm == B_ARM_CACHED)
        {
            (VOID)b_enter();
            held = 1U;
        }

        while (copied < want && got < B_XFER_BYTES)
        {
        ULONG length = 0UL, avail, take, moved = 0UL;

            if (pending == NX_NULL)
            {
                misses++;

                if (arm == B_ARM_ON_MISS && held == 0U)
                {
                    (VOID)b_enter();
                    held = 1U;
                }

                status = nx_tcp_socket_receive(&b_client, &pending,
                                               30UL * NX_IP_PERIODIC_RATE);
                if (status != NX_SUCCESS)
                {
                    (VOID)b_check(0U, "receive", status);
                    if (held != 0U)
                    {
                        b_leave();
                    }
                    if (arm == B_ARM_ONCE)
                    {
                        b_leave();
                    }
                    goto done;
                }

                offset = 0UL;

                /*
                 * The first read after a miss is the only place the arms
                 * differ, so on-miss gives the context back here rather than
                 * holding it for the copy -- nx_packet_data_extract_offset()
                 * and nx_packet_release() are among the nx_packet_* helpers
                 * with no caller check at all.
                 */
                if (arm == B_ARM_ON_MISS)
                {
                    b_leave();
                    held = 0U;
                }
            }

            (VOID)nx_packet_length_get(pending, &length);
            if (length <= offset)
            {
                (VOID)nx_packet_release(pending);
                pending = NX_NULL;
                continue;
            }

            avail = length - offset;
            take  = want - copied;
            if (take > avail)
            {
                take = avail;
            }

            status = nx_packet_data_extract_offset(pending, offset, b_dst_buf,
                                                   take, &moved);
            if (status != NX_SUCCESS || moved == 0UL)
            {
                (VOID)b_check(0U, "extract", status);
                break;
            }

            copied += moved;
            got    += moved;
            offset += moved;

            if (offset >= length)
            {
                (VOID)nx_packet_release(pending);
                pending = NX_NULL;
            }
        }

        if (held != 0U)
        {
            b_leave();
        }
    }

    if (arm == B_ARM_ONCE)
    {
        b_leave();
    }

done:

    if (pending != NX_NULL)
    {
        (VOID)nx_packet_release(pending);
        pending = NX_NULL;
    }

    if (calls_out != NX_NULL)
    {
        *calls_out = calls;
    }
    if (misses_out != NX_NULL)
    {
        *misses_out = misses;
    }

    return(b_elapsed(t0, b_now()));
}

static const char *b_arm_name(UINT arm)
{
    if (arm == B_ARM_PER_CALL)
    {
        return("per-call");
    }
    if (arm == B_ARM_ON_MISS)
    {
        return("on-miss ");
    }
    if (arm == B_ARM_CACHED)
    {
        return("cached  ");
    }

    return("once    ");
}

static VOID b_run_arm(ULONG read_size, UINT arm)
{
ULONG   ticks, calls = 0UL, misses = 0UL, ms, kbps;
UINT    status;

    if (arm == B_ARM_CACHED)
    {
        status = tx_amiga_adopt_thread(&b_caller, (CHAR *)"bracket caller", 16);
        if (!b_check((UINT)(status == TX_SUCCESS), "adopt for cached arm",
                     status))
        {
            return;
        }
        (VOID)tx_amiga_adopt_suspend(&b_caller);
        b_cache_live = 1U;
    }

    (VOID)tx_semaphore_put(&b_srv_go);

    ticks = b_drain(read_size, arm, &calls, &misses);

    (VOID)tx_semaphore_get(&b_srv_done, 120UL * NX_IP_PERIODIC_RATE);

    if (arm == B_ARM_CACHED)
    {
        b_cache_live = 0U;
        (VOID)tx_amiga_adopt_resume(&b_caller);
        (VOID)tx_amiga_orphan_thread(&b_caller);
    }

    (VOID)b_check((UINT)(b_srv_sent == B_XFER_BYTES), "sender sent every byte",
                  b_srv_sent);

    ms   = b_ms(ticks);
    kbps = (ms != 0UL) ? ((B_XFER_BYTES / 1024UL) * 1000UL / ms) : 0UL;

    b_log("  read %5ld  %s  %5ld ms  %4ld KB/s  %5ld calls, %4ld to NetX Duo",
          (LONG)read_size, (LONG)b_arm_name(arm), (LONG)ms, (LONG)kbps,
          (LONG)calls, (LONG)misses);
}

static UINT b_connect(VOID)
{
UINT    status;

    status = nx_tcp_socket_create(&b_ip, &b_client, "bracket client",
                                  NX_IP_NORMAL, NX_FRAGMENT_OKAY,
                                  NX_IP_TIME_TO_LIVE, 16384, NX_NULL, NX_NULL);
    if (!b_check((UINT)(status == NX_SUCCESS), "client socket create", status))
    {
        return(0U);
    }

    status = nx_tcp_client_socket_bind(&b_client, NX_ANY_PORT,
                                       5UL * NX_IP_PERIODIC_RATE);
    if (!b_check((UINT)(status == NX_SUCCESS), "client bind", status))
    {
        return(0U);
    }

    status = nx_tcp_client_socket_connect(&b_client, B_LOOPBACK, B_PORT,
                                          20UL * NX_IP_PERIODIC_RATE);
    if (!b_check((UINT)(status == NX_SUCCESS), "connect", status))
    {
        return(0U);
    }

    return(1U);
}


/* ------------------------------------------------------ ThreadX startup --- */

VOID tx_application_define(VOID *first_unused_memory)
{
UINT    status;

    (VOID)first_unused_memory;

    nx_system_initialize();

    status = nx_packet_pool_create(&b_pool, "bracket pool", B_PACKET_PAYLOAD,
                                   (VOID *)b_pool_memory,
                                   (ULONG)sizeof(b_pool_memory));
    (VOID)b_check((UINT)(status == NX_SUCCESS), "packet pool", status);

    status = nx_ip_create(&b_ip, "ip", B_IP_ADDRESS, B_NETMASK, &b_pool,
                          _nx_ram_network_driver,
                          (VOID *)b_ip_stack, (ULONG)sizeof(b_ip_stack), 1);
    (VOID)b_check((UINT)(status == NX_SUCCESS), "ip create", status);

    /*
     * A SECOND NX_IP, which nothing in this test addresses.
     *
     * _nx_ram_network_driver keeps a fixed table of attached instances and
     * decides where a frame goes by walking it.  With one instance in the
     * table it accepts the frame and delivers it nowhere, and a loopback
     * connect() on the sole instance times out with its SYN counted as
     * received and never matched -- measured, five SYN attempts and zero TCP
     * packets received, before this line existed.  tests/perf/perf_test.c has
     * always created two for the same reason and does not say so.
     */
    status = nx_ip_create(&b_ip2, "ip2", B_IP2_ADDRESS, B_NETMASK, &b_pool,
                          _nx_ram_network_driver,
                          (VOID *)b_ip2_stack, (ULONG)sizeof(b_ip2_stack), 1);
    (VOID)b_check((UINT)(status == NX_SUCCESS), "ip2 create", status);

    (VOID)nx_arp_enable(&b_ip,  (VOID *)b_arp_cache,  (ULONG)sizeof(b_arp_cache));
    (VOID)nx_arp_enable(&b_ip2, (VOID *)b_arp2_cache, (ULONG)sizeof(b_arp2_cache));
    (VOID)nx_tcp_enable(&b_ip);
    (VOID)nx_tcp_enable(&b_ip2);

    (VOID)tx_semaphore_create(&b_srv_ready,    "srv ready",    0);
    (VOID)tx_semaphore_create(&b_srv_accepted, "srv accepted", 0);
    (VOID)tx_semaphore_create(&b_srv_go,       "srv go",       0);
    (VOID)tx_semaphore_create(&b_srv_done,     "srv done",     0);

    status = tx_thread_create(&b_srv_thread, "bracket server", b_server_entry,
                              0UL, (VOID *)b_srv_stack,
                              (ULONG)sizeof(b_srv_stack),
                              16, 16, TX_NO_TIME_SLICE, TX_AUTO_START);
    (VOID)b_check((UINT)(status == TX_SUCCESS), "server thread", status);
}


/* ------------------------------------------------------------------ main -- */

static const ULONG  b_read_sizes[] = { 512UL, 1460UL, 4096UL, 16384UL };
#define B_READ_COUNT    (sizeof(b_read_sizes) / sizeof(b_read_sizes[0]))

int main(void)
{
UINT    status;
ULONG   actual;
ULONG   i;

    b_log("AmiNetXDuo -- the ThreadX adoption bracket, priced");

    status = tx_amiga_kernel_start();
    if (status != TX_SUCCESS)
    {
        b_log("FATAL: tx_amiga_kernel_start() = %ld", (LONG)status);
        b_flush();
        return(20);
    }

    b_timer_init();
    b_log("");
    b_log("E-Clock %ld Hz, %ld ns/tick, measurement bracket %ld ticks",
          (LONG)b_rate, (LONG)b_tick_ns, (LONG)b_bracket);

    /* Everything below runs from a plain Exec Task, which is the point. */
    status = tx_amiga_adopt_thread(&b_caller, (CHAR *)"bracket caller", 16);
    if (!b_check((UINT)(status == TX_SUCCESS), "first adoption", status))
    {
        b_flush();
        return(20);
    }
    status = nx_ip_status_check(&b_ip, NX_IP_INITIALIZE_DONE, &actual,
                                10UL * NX_IP_PERIODIC_RATE);
    (VOID)b_check((UINT)(status == NX_SUCCESS), "ip initialised", status);
    status = nx_ip_status_check(&b_ip2, NX_IP_INITIALIZE_DONE, &actual,
                                10UL * NX_IP_PERIODIC_RATE);
    (VOID)b_check((UINT)(status == NX_SUCCESS), "ip2 initialised", status);
    (VOID)tx_amiga_orphan_thread(&b_caller);

    b_log("");
    b_log("-- 256 KB over loopback, drained by an Exec Task ---------------");

    /* Let the server thread arm its listen and park in accept() first. */
    status = tx_amiga_adopt_thread(&b_caller, (CHAR *)"bracket caller", 16);
    if (b_check((UINT)(status == TX_SUCCESS), "adopt for setup", status))
    {
        (VOID)tx_semaphore_put(&b_srv_ready);
        tx_thread_sleep(NX_IP_PERIODIC_RATE / 5UL);

        status = b_connect();

        if (status != 0U)
        {
            (VOID)tx_semaphore_get(&b_srv_accepted,
                                   30UL * NX_IP_PERIODIC_RATE);
        }

        (VOID)tx_amiga_orphan_thread(&b_caller);

        if (status != 0U && b_srv_up != 0U)
        {
            for (i = 0UL; i < (ULONG)B_READ_COUNT; i++)
            {
                b_run_arm(b_read_sizes[i], B_ARM_PER_CALL);
                b_run_arm(b_read_sizes[i], B_ARM_CACHED);
                b_run_arm(b_read_sizes[i], B_ARM_ON_MISS);
                b_run_arm(b_read_sizes[i], B_ARM_ONCE);
            }

            status = tx_amiga_adopt_thread(&b_caller, (CHAR *)"bracket caller",
                                           16);
            if (status == TX_SUCCESS)
            {
                (VOID)nx_tcp_socket_disconnect(&b_client,
                                               2UL * NX_IP_PERIODIC_RATE);
                (VOID)nx_tcp_client_socket_unbind(&b_client);
                (VOID)nx_tcp_socket_delete(&b_client);
                (VOID)tx_amiga_orphan_thread(&b_caller);
            }
        }
    }

    b_log("");
    b_log("-- one bracket ------------------------------------------------");
    b_bench_bracket();

    b_log("");
    b_log("%ld checks, %ld failures -- %s",
          (LONG)b_checks, (LONG)b_failures,
          (LONG)((b_failures == 0UL) ? "PASS" : "FAIL"));

    b_flush();

    return((b_failures == 0UL) ? 0 : 20);
}

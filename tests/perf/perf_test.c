/*
 * AmiNetXDuo -- where a megabyte of TCP actually goes.
 *
 * There is no profiler on this platform, so this is the substitute: measure
 * every primitive the data path touches per byte, count how many times the
 * data path touches it per megabyte, multiply, and compare the total against
 * a measured end-to-end transfer.  Whatever is left over is not in the copy
 * loops.
 *
 *   1. E-Clock bracket overhead, so it can be subtracted from everything else.
 *   2. Per-primitive cost: the checksum (vendored and net68k, in one process),
 *      memcpy at every one of the sixteen alignment pairs, net68k's movem.l
 *      copy, the SANA-II shim's copy loop, nx_packet_data_append,
 *      nx_packet_data_extract_offset, nx_packet_copy, an allocate/release
 *      pair with and without NetX Duo's _nxe_ error-checking wrappers, and a
 *      bare Forbid()/Permit() pair.
 *   3. An alignment census of the pointers the stack actually hands those
 *      routines, because a memcpy penalty that never fires is not a finding.
 *   4. A pipeline benchmark: exactly the operations a loopback segment pays
 *      for, in order, with no protocol -- so its reciprocal is the ceiling the
 *      copy loops impose.
 *   5. End to end: TCP over 127.0.0.1 and TCP between two NX_IP instances
 *      over the simulated RAM driver, each run with the vendored checksum and
 *      with net68k's, back to back, with nothing else changed.
 *
 * bsdsocket.library is not measured here.  This binary talks to NetX Duo
 * directly, so the difference between its loopback figure and the conformance
 * suite's throughput category is the library layer, and that is how the
 * library's cost is priced.
 *
 * Only the 68020 profiles mean anything, and that is measured rather than
 * suspected: tests/perf/cpucal.c times instructions with published cycle costs
 * and finds FS-UAE's A1200 model faithful to under 2% for two-cycle integer
 * work, while its 68030 -- any 68030, including `-c 68030` on the A1200 model
 * -- charges no cycles at all, because FS-UAE turns cycle accounting off above
 * a 68020.  Run this under `-m A3000` as a correctness check and do not quote
 * the numbers.
 *
 * `tools/fsuae-run.sh -k MHZ` moves the 68020 model's clock without losing the
 * cycle accounting, so "what would this do at 25 MHz?" has an answer here even
 * though "what would this do on an A3000?" does not.
 *
 * SPDX-License-Identifier: MIT
 */

#include "tx_api.h"
#include "tx_amiga.h"
#include "nx_api.h"
#include "nx_packet.h"

#include "net68k.h"
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

#define P_LOG_SIZE      12288

static char     p_log_buffer[P_LOG_SIZE];
static ULONG    p_log_used;

static VOID p_put(UBYTE c)
{
    RawPutChar(c);

    if (p_log_used < (ULONG)(P_LOG_SIZE - 1))
    {
        p_log_buffer[p_log_used++] = (char)c;
    }
}

static VOID p_put_char(register UBYTE c      __asm("d0"),
                       register APTR  unused __asm("a3"))
{
    (VOID)unused;
    if (c != '\0')
    {
        p_put(c);
    }
}

static VOID p_log(const char *fmt, ...)
{
va_list args;

    va_start(args, fmt);
    RawDoFmt((STRPTR)fmt, args, (void (*)())p_put_char, NULL);
    va_end(args);

    p_put('\n');
}

static VOID p_flush(VOID)
{
BPTR    out;

    out = Output();
    if (out != (BPTR)0)
    {
        (VOID)Write(out, (APTR)p_log_buffer, (LONG)p_log_used);
    }
}

static volatile ULONG   p_checks;
static volatile ULONG   p_failures;

static UINT p_check(UINT ok, const char *what, ULONG detail)
{
    Forbid();
    p_checks++;
    if (!ok)
    {
        p_failures++;
    }
    Permit();

    if (!ok)
    {
        p_log("  FAIL %s (0x%lx)", what, detail);
    }

    return(ok);
}

#define P_OK(status, what)  p_check((UINT)((status) == NX_SUCCESS), (what), (ULONG)(status))


/* -------------------------------------------------------------- timing --- */

/*
 * ReadEClock() is the finest clock a shared library can reach: ~709 kHz PAL,
 * so one tick is about 1.41 us, or twenty 68020 cycles at 14 MHz.  That is
 * fine enough to time a 1460-byte loop (hundreds of ticks) and far too coarse
 * to time one instruction, which is why everything below is a loop with a
 * repeat count rather than a single call.
 */

extern struct Device *TimerBase;        /* src/common/compat.c owns it */

static ULONG    p_rate;                 /* E-Clock ticks per second     */
static ULONG    p_tick_ns;              /* nanoseconds per tick         */
static ULONG    p_per_ms;               /* ticks per millisecond        */
static ULONG    p_bracket;              /* cost of one measurement, ticks */

static ULONG p_now(VOID)
{
struct EClockVal ev;

    (VOID)ReadEClock(&ev);

    return(ev.ev_lo);
}

/* Subtract the measurement's own cost; never report a negative interval. */
static ULONG p_elapsed(ULONG start, ULONG stop)
{
ULONG   d = stop - start;

    return((d > p_bracket) ? (d - p_bracket) : 0UL);
}

/* Microseconds, for intervals below ~3 seconds. */
static ULONG p_us(ULONG ticks)
{
    return((ticks * p_tick_ns) / 1000UL);
}

/* Milliseconds, for anything longer. */
static ULONG p_ms(ULONG ticks)
{
    return(ticks / p_per_ms);
}

/*
 * Nanoseconds per byte, x100 so a fraction survives an integer print, taken
 * over the whole loop rather than as a per-iteration mean.
 *
 * The per-iteration form quantises to one E-Clock tick per rep -- 1.409 us --
 * and at 14 MHz a 1460-byte copy is ~180 ticks, so the granularity is half a
 * percent and invisible.  At 24.5 MHz with `fsuae-run.sh -k 25` it is ~105
 * ticks, and three copy routines that differ by 4% all printed the same
 * 102.29 ns/B.  Dividing once, at the end, by the total byte count removes it.
 *
 * `total_bytes` is reps * bytes and is large, so the scaling divides it by 100
 * rather than multiplying the ticks by it: the pipeline benchmark's 105,000
 * ticks * 1409 is already 1.5e8, and another 100 would not fit in a longword.
 */
static ULONG p_ns_per_byte_x100(ULONG ticks, ULONG total_bytes)
{
    if (total_bytes < 100UL)
    {
        return(0UL);
    }

    return((ticks * p_tick_ns) / (total_bytes / 100UL));
}

static VOID p_timer_init(VOID)
{
struct EClockVal ev;
ULONG            i;
ULONG            t0, t1, total;

    /* ami_millis() opens timer.device and publishes TimerBase. */
    (VOID)ami_millis();

    p_rate    = ReadEClock(&ev);
    p_tick_ns = 1000000000UL / p_rate;
    p_per_ms  = p_rate / 1000UL;
    if (p_per_ms == 0UL)
    {
        p_per_ms = 1UL;
    }

    /*
     * Calibrate the bracket.  A back-to-back pair of reads measures nothing
     * but itself; take the mean of many so one interrupt does not set the
     * floor.
     */
    p_bracket = 0UL;
    total     = 0UL;
    for (i = 0UL; i < 256UL; i++)
    {
        t0 = p_now();
        t1 = p_now();
        total += (t1 - t0);
    }
    p_bracket = total / 256UL;
}


/* ------------------------------------------ the checksum under the test --- */

/*
 * NetX Duo calls _nx_ip_checksum_compute() by name from IP, TCP, UDP and
 * ICMP.  The top-level CMakeLists drops the vendored object from the core, so
 * the symbol is ours to define; defining it here, rather than linking
 * src/net68k's hook, lets one process run both implementations back to back
 * with a single variable between them.
 */

USHORT n68k_checksum_reference(NX_PACKET *packet_ptr, ULONG protocol,
                               UINT data_length, ULONG *src_ip_addr,
                               ULONG *dest_ip_addr);

#define P_CK_NET68K     0
#define P_CK_VENDORED   1

static volatile ULONG   p_ck_mode = P_CK_NET68K;

static volatile ULONG   p_ck_calls;
static volatile ULONG   p_ck_bytes;
static volatile ULONG   p_ck_misaligned;

USHORT _nx_ip_checksum_compute(NX_PACKET *packet_ptr, ULONG protocol,
                               UINT data_length, ULONG *src_ip_addr,
                               ULONG *dest_ip_addr)
{
    p_ck_calls++;
    p_ck_bytes += (ULONG)data_length;
    if ((((ULONG)packet_ptr -> nx_packet_prepend_ptr) & 3UL) != 0UL)
    {
        p_ck_misaligned++;
    }

    if (p_ck_mode == P_CK_VENDORED)
    {
        return(n68k_checksum_reference(packet_ptr, protocol, data_length,
                                       src_ip_addr, dest_ip_addr));
    }

    return(n68k_ip_checksum_compute(packet_ptr, protocol, data_length,
                                    src_ip_addr, dest_ip_addr));
}


/* -------------------------------------------------------- test fabric ---- */

#define P_PACKET_PAYLOAD    1568        /* == AMI_POOL_PAYLOAD              */
#define P_PACKET_COUNT      64
#define P_PACKET_OVERHEAD   96

#define P_IP0_ADDRESS       IP_ADDRESS(192, 168, 100, 1)
#define P_IP1_ADDRESS       IP_ADDRESS(192, 168, 100, 2)
#define P_LOOPBACK          IP_ADDRESS(127, 0, 0, 1)
#define P_NETMASK           0xFFFFFF00UL
#define P_PORT_LOOP         5101
#define P_PORT_WIRE         5102

#define P_IP_STACK_SIZE     3072
#define P_SRV_STACK_SIZE    4096

/* Bytes per end-to-end run.  256 KB is long enough to swamp connection setup
   and short enough that eight runs fit inside one emulator session. */
#define P_XFER_BYTES        (256UL * 1024UL)

/* What bsdsocket.library's send() hands NetX Duo: the conformance suite's
   throughput test uses an 8 KB application buffer and bsd_send_tcp() clamps
   only to the MSS, which on loopback is 65495. */
#define P_APP_CHUNK         8192UL

extern VOID _nx_ram_network_driver(NX_IP_DRIVER *driver_req_ptr);

static NX_PACKET_POOL   p_pool;
static NX_IP            p_ip0;
static NX_IP            p_ip1;
/*
 * The receive window both ends of an end-to-end case are created with.  A
 * global rather than an argument because the server socket is created inside
 * p_server_entry(), which p_transfer() reaches only through the semaphore
 * handshake and cannot pass anything to.
 */
static ULONG            p_window = 16384UL;

static NX_TCP_SOCKET    p_client;
static NX_TCP_SOCKET    p_server;

static TX_THREAD        p_srv_thread;
static TX_THREAD        p_main_thread;
static TX_SEMAPHORE     p_srv_ready;
static TX_SEMAPHORE     p_srv_gotall;
static TX_SEMAPHORE     p_srv_done;

static ULONG            p_pool_memory[(P_PACKET_COUNT * (P_PACKET_PAYLOAD + P_PACKET_OVERHEAD)) / sizeof(ULONG)];
static ULONG            p_ip0_stack[P_IP_STACK_SIZE / sizeof(ULONG)];
static ULONG            p_ip1_stack[P_IP_STACK_SIZE / sizeof(ULONG)];
static ULONG            p_srv_stack[P_SRV_STACK_SIZE / sizeof(ULONG)];
static ULONG            p_arp0_cache[1024 / sizeof(ULONG)];
static ULONG            p_arp1_cache[1024 / sizeof(ULONG)];

/*
 * Application buffers.  Explicitly longword aligned so the alignment sweep
 * below means what it says: `p_src_buf + 1` is then a genuinely misaligned
 * source, not an accident of what the compiler happened to choose.
 */
static UCHAR            p_src_buf[P_APP_CHUNK + 8] __attribute__((aligned(4)));
static UCHAR            p_dst_buf[P_APP_CHUNK + 8] __attribute__((aligned(4)));


/* --------------------------------------------------- micro benchmarks ----- */

/*
 * Every micro benchmark has the same shape: run the operation `reps` times
 * over `bytes` bytes, bracket the lot, subtract the bracket, report the mean.
 * `reps` is chosen so the whole loop is at least a few thousand ticks, which
 * puts the ~1 tick of bracket jitter three orders of magnitude down.
 */

static VOID p_report(const char *what, ULONG ticks, ULONG reps, ULONG bytes)
{
ULONG   each  = (reps != 0UL) ? (ticks / reps) : 0UL;
ULONG   nspb  = p_ns_per_byte_x100(ticks, reps * bytes);

    p_log("  %-34s %6ld us  %4ld.%02ld ns/B  (%ld x %ld B)",
          (LONG)what,
          (LONG)p_us(each),
          (LONG)(nspb / 100UL),
          (LONG)(nspb % 100UL),
          (LONG)reps, (LONG)bytes);
}

/* Copy candidate: what the SANA-II shim uses at interrupt level. */
extern VOID ami_sana2_copy_bytes(UCHAR *to, const UCHAR *from, ULONG len);

static NX_PACKET *p_scratch_packet;
static NX_PACKET *p_scratch_chain;

static VOID p_bench_checksum(VOID)
{
ULONG   t0, reps, i;
ULONG   ticks_ref, ticks_new;
USHORT  a, b;
ULONG   src = P_IP0_ADDRESS;
ULONG   dst = P_IP1_ADDRESS;
ULONG   len = 1460UL;

    if (p_scratch_packet == NX_NULL)
    {
        return;
    }

    reps = 200UL;

    t0 = p_now();
    for (i = 0UL; i < reps; i++)
    {
        (VOID)n68k_checksum_reference(p_scratch_packet, NX_PROTOCOL_TCP,
                                      (UINT)len, &src, &dst);
    }
    ticks_ref = p_elapsed(t0, p_now());

    t0 = p_now();
    for (i = 0UL; i < reps; i++)
    {
        (VOID)n68k_ip_checksum_compute(p_scratch_packet, NX_PROTOCOL_TCP,
                                       (UINT)len, &src, &dst);
    }
    ticks_new = p_elapsed(t0, p_now());

    p_report("checksum, vendored", ticks_ref, reps, len);
    p_report("checksum, net68k", ticks_new, reps, len);

    if (ticks_new != 0UL)
    {
        p_log("    ratio %ld.%02ldx",
              (LONG)(ticks_ref * 100UL / ticks_new / 100UL),
              (LONG)(ticks_ref * 100UL / ticks_new % 100UL));
    }

    /* The A/B is worthless if the two disagree; check here as well as in the
       host tier, because the host tier cannot run the assembly. */
    a = n68k_checksum_reference(p_scratch_packet, NX_PROTOCOL_TCP, (UINT)len,
                                &src, &dst);
    b = n68k_ip_checksum_compute(p_scratch_packet, NX_PROTOCOL_TCP, (UINT)len,
                                 &src, &dst);
    (VOID)p_check((UINT)(a == b), "checksum agrees on a 1460 B packet",
                  ((ULONG)a << 16) | (ULONG)b);

    /* And over a chain, which is the shape a loopback segment actually has. */
    if (p_scratch_chain != NX_NULL)
    {
    ULONG   clen = 0UL;

        (VOID)nx_packet_length_get(p_scratch_chain, &clen);

        a = n68k_checksum_reference(p_scratch_chain, NX_PROTOCOL_TCP,
                                    (UINT)clen, &src, &dst);
        b = n68k_ip_checksum_compute(p_scratch_chain, NX_PROTOCOL_TCP,
                                     (UINT)clen, &src, &dst);
        (VOID)p_check((UINT)(a == b), "checksum agrees on a chain",
                      ((ULONG)a << 16) | (ULONG)b);

        reps = 40UL;

        t0 = p_now();
        for (i = 0UL; i < reps; i++)
        {
            (VOID)n68k_checksum_reference(p_scratch_chain, NX_PROTOCOL_TCP,
                                          (UINT)clen, &src, &dst);
        }
        ticks_ref = p_elapsed(t0, p_now());

        t0 = p_now();
        for (i = 0UL; i < reps; i++)
        {
            (VOID)n68k_ip_checksum_compute(p_scratch_chain, NX_PROTOCOL_TCP,
                                           (UINT)clen, &src, &dst);
        }
        ticks_new = p_elapsed(t0, p_now());

        p_report("checksum chain, vendored", ticks_ref, reps, clen);
        p_report("checksum chain, net68k", ticks_new, reps, clen);
    }

    /* Every length from 0 to 40 and a few beyond, on the real packet. */
    for (i = 0UL; i <= 40UL; i++)
    {
        a = n68k_checksum_reference(p_scratch_packet, NX_PROTOCOL_TCP, (UINT)i,
                                    &src, &dst);
        b = n68k_ip_checksum_compute(p_scratch_packet, NX_PROTOCOL_TCP, (UINT)i,
                                     &src, &dst);
        if (a != b)
        {
            (VOID)p_check(0, "checksum agrees at a short length", i);
            break;
        }
    }
    (VOID)p_check(1, "checksum agrees at lengths 0..40", 0UL);
}

static VOID p_bench_copies(VOID)
{
ULONG   t0, ticks, reps, i;
ULONG   len = 1460UL;
UINT    da, sa;

    reps = 200UL;

    /*
     * memcpy(), at every combination of destination and source alignment.
     *
     * Read the build flags before reading these rows.  With
     * AMINETXDUO_NET68K_MEMCPY=ON -- the default on a cross build -- memcpy()
     * is n68k_copy_bytes(), and these rows measure it twice under two names.
     * The C library's own numbers quoted in docs/RESEARCH.md were taken from a
     * -DAMINETXDUO_NET68K_MEMCPY=OFF build, which is the only way to get them.
     * The libm020 multilib -- which -m68020 selects, verified in the link map
     * -- aligns only the destination and then moves longwords regardless of
     * what the source is doing, so the expensive case is supposed to be a
     * misaligned destination, not a misaligned source.  This measurement says
     * whether that is true.
     */
    for (da = 0; da < 4; da++)
    {
        for (sa = 0; sa < 4; sa++)
        {
        static const char *names[16] =
        {
            "memcpy d0 s0", "memcpy d0 s1", "memcpy d0 s2", "memcpy d0 s3",
            "memcpy d1 s0", "memcpy d1 s1", "memcpy d1 s2", "memcpy d1 s3",
            "memcpy d2 s0", "memcpy d2 s1", "memcpy d2 s2", "memcpy d2 s3",
            "memcpy d3 s0", "memcpy d3 s1", "memcpy d3 s2", "memcpy d3 s3"
        };

            t0 = p_now();
            for (i = 0UL; i < reps; i++)
            {
                (VOID)memcpy(p_dst_buf + da, p_src_buf + sa, len);
            }
            ticks = p_elapsed(t0, p_now());

            p_report(names[(da * 4) + sa], ticks, reps, len);
        }
    }

    /*
     * The movem.l candidate, checked before it is timed.  This routine is
     * memcpy() for the whole library when AMINETXDUO_NET68K_MEMCPY is on, so
     * "fast" is worthless without "exact".  Every length from 0 to 96 and
     * every one of the sixteen source/destination alignment combinations,
     * verifying three things each time: the copied bytes match, the byte
     * before the destination is untouched, and the byte after it is
     * untouched.  That is 1552 cases; a routine that mishandles the head
     * alignment, the movem block, the longword tail or the byte tail fails at
     * least one of them.
     */
    {
    ULONG   n, da, sa, k;
    UINT    ok = 1;

        for (n = 0UL; n <= 96UL; n++)
        {
            for (da = 0UL; da < 4UL; da++)
            {
                for (sa = 0UL; sa < 4UL; sa++)
                {
                    for (k = 0UL; k < n + 8UL; k++)
                    {
                        p_dst_buf[k] = 0xA5;
                    }

                    n68k_copy_bytes(p_dst_buf + 4UL + da, p_src_buf + sa, n);

                    for (k = 0UL; k < n; k++)
                    {
                        if (p_dst_buf[4UL + da + k] != p_src_buf[sa + k])
                        {
                            ok = 0;
                        }
                    }

                    if (p_dst_buf[4UL + da - 1UL] != 0xA5 ||
                        p_dst_buf[4UL + da + n] != 0xA5)
                    {
                        ok = 0;             /* wrote outside the destination */
                    }
                }
            }
        }
        (VOID)p_check(ok, "n68k_copy_bytes exact, 1552 length/alignment cases",
                      0UL);

        /* And one long copy, where the movem block does the work. */
        for (k = 0UL; k < sizeof(p_dst_buf); k++)
        {
            p_dst_buf[k] = 0xA5;
        }
        n68k_copy_bytes(p_dst_buf + 1UL, p_src_buf + 2UL, P_APP_CHUNK - 8UL);
        ok = 1;
        for (k = 0UL; k < P_APP_CHUNK - 8UL; k++)
        {
            if (p_dst_buf[1UL + k] != p_src_buf[2UL + k])
            {
                ok = 0;
            }
        }
        if (p_dst_buf[0] != 0xA5 || p_dst_buf[P_APP_CHUNK - 7UL] != 0xA5)
        {
            ok = 0;
        }
        (VOID)p_check(ok, "n68k_copy_bytes exact, 8184 B misaligned", 0UL);
    }

    t0 = p_now();
    for (i = 0UL; i < reps; i++)
    {
        n68k_copy_bytes(p_dst_buf, p_src_buf, len);
    }
    ticks = p_elapsed(t0, p_now());
    p_report("n68k_copy_bytes d0 s0", ticks, reps, len);

    /*
     * The melded copy-and-sum, against the two operations it replaces.  The
     * host tier cannot reach the assembly (tests/perf/host/shim), so this is
     * where it is checked: the sum has to equal n68k_sum_longwords() over the
     * same source and the copied bytes have to be identical, because a right
     * sum over a wrongly-copied payload is the silent-corruption case.
     */
    {
        ULONG   words = len / 4UL;
        ULONG   want;
        ULONG   got;
        UINT    ok = 1U;

        want = n68k_sum_longwords((const ULONG *)p_src_buf, words);
        got  = n68k_copy_sum_longwords((ULONG *)p_dst_buf,
                                       (const ULONG *)p_src_buf, words);

        (VOID)p_check((UINT)(got == want), "copy_sum matches sum_longwords",
                      got ^ want);

        for (i = 0UL; i < words * 4UL; i++)
        {
            if (p_dst_buf[i] != p_src_buf[i])
            {
                ok = 0U;
            }
        }
        (VOID)p_check(ok, "copy_sum copies exactly", 0UL);

        t0 = p_now();
        for (i = 0UL; i < reps; i++)
        {
            (VOID)n68k_copy_sum_longwords((ULONG *)p_dst_buf,
                                          (const ULONG *)p_src_buf, words);
        }
        ticks = p_elapsed(t0, p_now());
        p_report("n68k_copy_sum_longwords d0 s0", ticks, reps, words * 4UL);
    }

    t0 = p_now();
    for (i = 0UL; i < reps; i++)
    {
        n68k_copy_bytes(p_dst_buf, p_src_buf + 2, len);
    }
    ticks = p_elapsed(t0, p_now());
    p_report("n68k_copy_bytes d0 s2", ticks, reps, len);

    /* Opposite parity -- the case with a genuine cliff in it, see below. */
    t0 = p_now();
    for (i = 0UL; i < reps; i++)
    {
        n68k_copy_bytes(p_dst_buf, p_src_buf + 1, len);
    }
    ticks = p_elapsed(t0, p_now());
    p_report("n68k_copy_bytes d0 s1", ticks, reps, len);

    /* The SANA-II shim's own loop, for comparison at the two alignments it
       actually sees. */
    t0 = p_now();
    for (i = 0UL; i < reps; i++)
    {
        ami_sana2_copy_bytes(p_dst_buf, p_src_buf, len);
    }
    ticks = p_elapsed(t0, p_now());
    p_report("ami_sana2_copy_bytes d0 s0", ticks, reps, len);

    t0 = p_now();
    for (i = 0UL; i < reps; i++)
    {
        ami_sana2_copy_bytes(p_dst_buf, p_src_buf + 2, len);
    }
    ticks = p_elapsed(t0, p_now());
    p_report("ami_sana2_copy_bytes d0 s2", ticks, reps, len);

    /*
     * Opposite parity, where a real cliff lives -- in our own code, not the C
     * library: the SANA-II shim's loop takes its longword path only when
     * source and destination agree mod 2, and drops to one byte per iteration
     * when they do not.
     */
    t0 = p_now();
    for (i = 0UL; i < reps; i++)
    {
        ami_sana2_copy_bytes(p_dst_buf, p_src_buf + 1, len);
    }
    ticks = p_elapsed(t0, p_now());
    p_report("ami_sana2_copy_bytes d0 s1", ticks, reps, len);
}

static VOID p_bench_packets(VOID)
{
ULONG       t0, ticks, reps, i;
NX_PACKET  *pkt;
NX_PACKET  *copy;
ULONG       moved;
UINT        status;
ULONG       len = 1460UL;

    reps = 100UL;

    /* allocate + release, the per-packet floor. */
    t0 = p_now();
    for (i = 0UL; i < reps; i++)
    {
        pkt = NX_NULL;
        if (nx_packet_allocate(&p_pool, &pkt, NX_TCP_PACKET, NX_NO_WAIT) == NX_SUCCESS)
        {
            (VOID)nx_packet_release(pkt);
        }
    }
    ticks = p_elapsed(t0, p_now());
    p_log("  %-34s %6ld us each (%ld reps)",
          (LONG)"nx_packet_allocate + release", (LONG)p_us(ticks / reps),
          (LONG)reps);

    /*
     * The same pair through the internal entry points.  nx_user.h leaves
     * NX_DISABLE_ERROR_CHECKING unset ("revisit for the release build"), so
     * every nx_* call in the tree goes through an _nxe_ wrapper first.  This
     * is what that costs on the hottest call in the stack.
     */
    t0 = p_now();
    for (i = 0UL; i < reps; i++)
    {
        pkt = NX_NULL;
        if (_nx_packet_allocate(&p_pool, &pkt, NX_TCP_PACKET, NX_NO_WAIT) == NX_SUCCESS)
        {
            (VOID)_nx_packet_release(pkt);
        }
    }
    ticks = p_elapsed(t0, p_now());
    p_log("  %-34s %6ld us each (%ld reps)",
          (LONG)"  ... without the _nxe_ wrappers", (LONG)p_us(ticks / reps),
          (LONG)reps);

    /* And the Forbid()/Permit() pair TX_DISABLE/TX_RESTORE expand to, which
       is what the allocator spends its uninterruptible section on. */
    t0 = p_now();
    for (i = 0UL; i < reps * 10UL; i++)
    {
        Forbid();
        Permit();
    }
    ticks = p_elapsed(t0, p_now());
    p_log("  %-34s %6ld ns each",
          (LONG)"  ... one Forbid/Permit pair",
          (LONG)((ticks * p_tick_ns) / (reps * 10UL)));

    /* data_append of one MSS into a fresh packet. */
    t0 = p_now();
    for (i = 0UL; i < reps; i++)
    {
        pkt = NX_NULL;
        if (nx_packet_allocate(&p_pool, &pkt, NX_TCP_PACKET, NX_NO_WAIT) == NX_SUCCESS)
        {
            (VOID)nx_packet_data_append(pkt, p_src_buf, len, &p_pool, NX_NO_WAIT);
            (VOID)nx_packet_release(pkt);
        }
    }
    ticks = p_elapsed(t0, p_now());
    p_report("allocate + append 1460 + release", ticks, reps, len);

    /* data_append of a full application chunk -- this is the one that chains. */
    t0 = p_now();
    for (i = 0UL; i < reps; i++)
    {
        pkt = NX_NULL;
        if (nx_packet_allocate(&p_pool, &pkt, NX_TCP_PACKET, NX_NO_WAIT) == NX_SUCCESS)
        {
            (VOID)nx_packet_data_append(pkt, p_src_buf, P_APP_CHUNK, &p_pool,
                                        NX_NO_WAIT);
            (VOID)nx_packet_release(pkt);
        }
    }
    ticks = p_elapsed(t0, p_now());
    p_report("allocate + append 8192 + release", ticks, reps, P_APP_CHUNK);

    /* extract_offset out of a packet, which is what recv() does. */
    if (p_scratch_packet != NX_NULL)
    {
        t0 = p_now();
        for (i = 0UL; i < reps; i++)
        {
            moved = 0UL;
            (VOID)nx_packet_data_extract_offset(p_scratch_packet, 0UL,
                                                p_dst_buf, len, &moved);
        }
        ticks = p_elapsed(t0, p_now());
        p_report("extract_offset 1460", ticks, reps, len);
    }

    if (p_scratch_chain != NX_NULL)
    {
    ULONG clen = 0UL;

        (VOID)nx_packet_length_get(p_scratch_chain, &clen);

        t0 = p_now();
        for (i = 0UL; i < reps; i++)
        {
            moved = 0UL;
            (VOID)nx_packet_data_extract_offset(p_scratch_chain, 0UL,
                                                p_dst_buf, clen, &moved);
        }
        ticks = p_elapsed(t0, p_now());
        p_report("extract_offset chain", ticks, reps, clen);

        /* nx_packet_copy: what the loopback and RAM driver paths pay per
           packet, and the one cost that exists for no reason but the API. */
        reps = 40UL;
        t0 = p_now();
        for (i = 0UL; i < reps; i++)
        {
            copy   = NX_NULL;
            status = nx_packet_copy(p_scratch_chain, &copy, &p_pool, NX_NO_WAIT);
            if (status == NX_SUCCESS)
            {
                (VOID)nx_packet_release(copy);
            }
        }
        ticks = p_elapsed(t0, p_now());
        p_report("nx_packet_copy of the chain", ticks, reps, clen);
    }
}

/*
 * The pipeline ceiling: every operation a loopback TCP segment pays for, in
 * order, with no protocol, no scheduler and no timers in the way:
 *
 *   allocate -> append (the send() copy) -> TCP checksum ->
 *   nx_packet_copy (the loopback hand-over) -> TCP checksum (receive side) ->
 *   extract (the recv() copy) -> release x2
 *
 * The reciprocal of the per-byte total is the fastest this stack could
 * possibly move loopback TCP if the protocol were free.  Anything the
 * end-to-end runs fall short of it by is not in the copy loops.
 */
static VOID p_bench_pipeline(ULONG mode, const char *what)
{
ULONG       t0, ticks, reps, i;
NX_PACKET  *pkt;
NX_PACKET  *copy;
ULONG       moved, len;
ULONG       src = P_IP0_ADDRESS;
ULONG       dst = P_IP1_ADDRESS;

    p_ck_mode = mode;
    reps      = 30UL;
    len       = P_APP_CHUNK;

    t0 = p_now();
    for (i = 0UL; i < reps; i++)
    {
        pkt = NX_NULL;
        if (nx_packet_allocate(&p_pool, &pkt, NX_TCP_PACKET, NX_NO_WAIT)
                != NX_SUCCESS)
        {
            break;
        }

        (VOID)nx_packet_data_append(pkt, p_src_buf, len, &p_pool, NX_NO_WAIT);

        (VOID)_nx_ip_checksum_compute(pkt, NX_PROTOCOL_TCP, (UINT)len,
                                      &src, &dst);

        copy = NX_NULL;
        if (nx_packet_copy(pkt, &copy, &p_pool, NX_NO_WAIT) == NX_SUCCESS)
        {
            (VOID)_nx_ip_checksum_compute(copy, NX_PROTOCOL_TCP, (UINT)len,
                                          &src, &dst);

            moved = 0UL;
            (VOID)nx_packet_data_extract_offset(copy, 0UL, p_dst_buf, len,
                                                &moved);

            (VOID)nx_packet_release(copy);
        }

        (VOID)nx_packet_release(pkt);
    }
    ticks = p_elapsed(t0, p_now());

    p_report(what, ticks, reps, len);

    if (ticks != 0UL)
    {
    ULONG each_us = p_us(ticks / reps);

        if (each_us != 0UL)
        {
            p_log("      implied ceiling %ld KB/s",
                  (LONG)((len * 1000UL) / each_us * 1000UL / 1024UL));
        }
    }
}

/*
 * The alignment census.  A memcpy penalty that never fires is not a finding,
 * so this prints the alignments the stack actually produces: where a freshly
 * allocated TCP packet's payload starts, where a received packet's payload
 * starts, and where the application buffers sit.
 */
static VOID p_census_alignment(VOID)
{
NX_PACKET  *pkt = NX_NULL;

    p_log("  application buffers:  src mod 4 = %ld, dst mod 4 = %ld",
          (LONG)(((ULONG)p_src_buf) & 3UL), (LONG)(((ULONG)p_dst_buf) & 3UL));

    if (nx_packet_allocate(&p_pool, &pkt, NX_TCP_PACKET, NX_NO_WAIT) == NX_SUCCESS)
    {
        p_log("  fresh NX_TCP_PACKET:  data_start mod 4 = %ld, prepend mod 4 = %ld,"
              " append mod 4 = %ld",
              (LONG)(((ULONG)pkt -> nx_packet_data_start) & 3UL),
              (LONG)(((ULONG)pkt -> nx_packet_prepend_ptr) & 3UL),
              (LONG)(((ULONG)pkt -> nx_packet_append_ptr) & 3UL));

        (VOID)nx_packet_release(pkt);
    }

    if (p_scratch_packet != NX_NULL)
    {
        p_log("  filled 1460 B packet: prepend mod 4 = %ld, append mod 4 = %ld",
              (LONG)(((ULONG)p_scratch_packet -> nx_packet_prepend_ptr) & 3UL),
              (LONG)(((ULONG)p_scratch_packet -> nx_packet_append_ptr) & 3UL));
    }
}

static VOID p_scratch_build(VOID)
{
UINT    status;
ULONG   i;

    for (i = 0UL; i < sizeof(p_src_buf); i++)
    {
        p_src_buf[i] = (UCHAR)(i * 7UL + 13UL);
    }

    p_scratch_packet = NX_NULL;
    status = nx_packet_allocate(&p_pool, &p_scratch_packet, NX_TCP_PACKET,
                                NX_NO_WAIT);
    if (status == NX_SUCCESS)
    {
        (VOID)nx_packet_data_append(p_scratch_packet, p_src_buf, 1460UL,
                                    &p_pool, NX_NO_WAIT);
    }
    else
    {
        p_scratch_packet = NX_NULL;
    }

    p_scratch_chain = NX_NULL;
    status = nx_packet_allocate(&p_pool, &p_scratch_chain, NX_TCP_PACKET,
                                NX_NO_WAIT);
    if (status == NX_SUCCESS)
    {
        (VOID)nx_packet_data_append(p_scratch_chain, p_src_buf, P_APP_CHUNK,
                                    &p_pool, NX_NO_WAIT);
    }
    else
    {
        p_scratch_chain = NX_NULL;
    }
}

static VOID p_scratch_free(VOID)
{
    if (p_scratch_packet != NX_NULL)
    {
        (VOID)nx_packet_release(p_scratch_packet);
        p_scratch_packet = NX_NULL;
    }
    if (p_scratch_chain != NX_NULL)
    {
        (VOID)nx_packet_release(p_scratch_chain);
        p_scratch_chain = NX_NULL;
    }
}


/* -------------------------------------------------- end-to-end transfer --- */

/*
 * Two knobs, set by the client before it releases the server:
 *   p_run_ip      which NX_IP the server listens on
 *   p_run_extract whether the receiver copies the payload out to a user
 *                 buffer (what recv() does) or just releases the packet
 * The difference between the two prices nx_packet_data_extract_offset() in
 * situ rather than in a micro benchmark.
 */
static ULONG    p_tcp_retransmits;
static ULONG    p_tcp_txqueue;
static ULONG    p_tcp_txwindow;
static ULONG    p_tcp_rxwindow;
static ULONG    p_pool_empty_before;
static ULONG    p_pool_empty_after;

static NX_IP   *p_run_ip;
static UINT     p_run_port;
static UINT     p_run_extract;
static ULONG    p_run_target;

static volatile ULONG   p_srv_bytes;
static volatile UINT    p_srv_status;

/*
 * Sender phase accounting.  Bracketing every call costs one bracket (30 ticks,
 * 42 us) per call, which against a transfer measured in seconds is under one
 * part in a thousand -- and it is the only way to tell "the copy loops are
 * slow" from "the sender is asleep waiting for an acknowledgement".
 */
static ULONG    p_phase_alloc;
static ULONG    p_phase_append;
static ULONG    p_phase_send;
static ULONG    p_phase_calls;

/* Receiver phase accounting, same argument. */
static volatile ULONG   p_phase_recv;
static volatile ULONG   p_phase_extract;
static volatile ULONG   p_phase_rcalls;

static VOID p_server_entry(ULONG id)
{
UINT        status;
NX_PACKET  *pkt;
ULONG       length, moved;
ULONG       t0;

    (VOID)id;

    for (;;)
    {
        /* Wait to be told which configuration to serve. */
        if (tx_semaphore_get(&p_srv_ready, TX_WAIT_FOREVER) != TX_SUCCESS)
        {
            return;
        }

        p_srv_bytes  = 0UL;
        p_srv_status = NX_SUCCESS;

        status = nx_tcp_socket_create(p_run_ip, &p_server, "perf server",
                                      NX_IP_NORMAL, NX_FRAGMENT_OKAY,
                                      NX_IP_TIME_TO_LIVE, p_window,
                                      NX_NULL, NX_NULL);
        if (status != NX_SUCCESS)
        {
            p_srv_status = status;
            (VOID)tx_semaphore_put(&p_srv_done);
            continue;
        }

        (VOID)nx_tcp_server_socket_listen(p_run_ip, p_run_port, &p_server, 5,
                                          NX_NULL);

        status = nx_tcp_server_socket_accept(&p_server,
                                             20UL * NX_IP_PERIODIC_RATE);
        if (status == NX_SUCCESS)
        {
            while (p_srv_bytes < p_run_target)
            {
                pkt    = NX_NULL;
                t0     = p_now();
                status = nx_tcp_socket_receive(&p_server, &pkt,
                                               20UL * NX_IP_PERIODIC_RATE);
                p_phase_recv += p_elapsed(t0, p_now());
                p_phase_rcalls++;
                if (status != NX_SUCCESS)
                {
                    p_srv_status = status;
                    break;
                }

                length = 0UL;
                (VOID)nx_packet_length_get(pkt, &length);

                if (p_run_extract)
                {
                ULONG off = 0UL;

                    t0 = p_now();

                    /* Exactly what bsd_recv_tcp() does: drain the packet into
                       the caller's buffer in application-sized bites. */
                    while (off < length)
                    {
                    ULONG want = length - off;

                        if (want > P_APP_CHUNK)
                        {
                            want = P_APP_CHUNK;
                        }

                        moved = 0UL;
                        if (nx_packet_data_extract_offset(pkt, off, p_dst_buf,
                                                          want, &moved)
                                != NX_SUCCESS || moved == 0UL)
                        {
                            break;
                        }

                        off += moved;
                    }

                    p_phase_extract += p_elapsed(t0, p_now());
                }

                p_srv_bytes += length;

                (VOID)nx_packet_release(pkt);
            }
        }
        else
        {
            p_srv_status = status;
        }

        /*
         * The clock stops here, not after the teardown.  Stopping it after
         * nx_tcp_socket_disconnect() added a flat five seconds to every run:
         * the server closes first, the client is still blocked waiting to be
         * told the transfer finished, so nobody completes the handshake and
         * the disconnect burns its whole timeout.  Every figure in the first
         * pass was ~5000 ms of nothing plus the real transfer, and they all
         * came out around 45 KB/s regardless of what was changed -- a constant
         * offset that flattens every comparison.
         */
        (VOID)tx_semaphore_put(&p_srv_gotall);

        (VOID)nx_tcp_socket_disconnect(&p_server, 5UL * NX_IP_PERIODIC_RATE);
        (VOID)nx_tcp_server_socket_unaccept(&p_server);
        (VOID)nx_tcp_server_socket_unlisten(p_run_ip, p_run_port);
        (VOID)nx_tcp_socket_delete(&p_server);

        (VOID)tx_semaphore_put(&p_srv_done);
    }
}

/*
 * One transfer.  Returns the elapsed E-Clock ticks, or 0 if it did not
 * complete -- the caller prints the failure.
 */
static ULONG p_transfer(NX_IP *cip, NX_IP *sip, ULONG peer, UINT port,
                        UINT extract, ULONG bytes, ULONG *packets_out)
{
UINT        status;
NX_PACKET  *pkt;
ULONG       sent = 0UL;
ULONG       t0, t1, ticks;
ULONG       before_sent;

    p_run_ip      = sip;
    p_run_port    = port;
    p_run_extract = extract;
    p_run_target  = bytes;

    /* Let the server set itself up and start listening. */
    (VOID)tx_semaphore_put(&p_srv_ready);
    tx_thread_sleep(NX_IP_PERIODIC_RATE / 5UL);

    status = nx_tcp_socket_create(cip, &p_client, "perf client", NX_IP_NORMAL,
                                  NX_FRAGMENT_OKAY, NX_IP_TIME_TO_LIVE,
                                  p_window, NX_NULL, NX_NULL);
    if (!P_OK(status, "client socket create"))
    {
        (VOID)tx_semaphore_get(&p_srv_done, 30UL * NX_IP_PERIODIC_RATE);
        return(0UL);
    }

    (VOID)nx_tcp_client_socket_bind(&p_client, NX_ANY_PORT,
                                    5UL * NX_IP_PERIODIC_RATE);

    status = nx_tcp_client_socket_connect(&p_client, peer, port,
                                          20UL * NX_IP_PERIODIC_RATE);
    if (!P_OK(status, "client connect"))
    {
        (VOID)nx_tcp_client_socket_unbind(&p_client);
        (VOID)nx_tcp_socket_delete(&p_client);
        (VOID)tx_semaphore_get(&p_srv_done, 30UL * NX_IP_PERIODIC_RATE);
        return(0UL);
    }

    before_sent = cip -> nx_ip_total_packets_sent;

    p_ck_calls      = 0UL;
    p_ck_bytes      = 0UL;
    p_ck_misaligned = 0UL;

    p_phase_alloc   = 0UL;
    p_phase_append  = 0UL;
    p_phase_send    = 0UL;
    p_phase_calls   = 0UL;
    p_phase_recv    = 0UL;
    p_phase_extract = 0UL;
    p_phase_rcalls  = 0UL;

    t0 = p_now();

    while (sent < bytes)
    {
    ULONG chunk = bytes - sent;

        if (chunk > P_APP_CHUNK)
        {
            chunk = P_APP_CHUNK;
        }

        pkt    = NX_NULL;
        t1     = p_now();
        status = nx_packet_allocate(&p_pool, &pkt, NX_TCP_PACKET,
                                    10UL * NX_IP_PERIODIC_RATE);
        p_phase_alloc += p_elapsed(t1, p_now());
        p_phase_calls++;
        if (status != NX_SUCCESS)
        {
            (VOID)P_OK(status, "send: packet allocate");
            break;
        }

        t1     = p_now();
        status = nx_packet_data_append(pkt, p_src_buf, chunk, &p_pool,
                                       10UL * NX_IP_PERIODIC_RATE);
        p_phase_append += p_elapsed(t1, p_now());
        if (status != NX_SUCCESS)
        {
            (VOID)nx_packet_release(pkt);
            (VOID)P_OK(status, "send: data append");
            break;
        }

        t1     = p_now();
        status = nx_tcp_socket_send(&p_client, pkt,
                                    20UL * NX_IP_PERIODIC_RATE);
        p_phase_send += p_elapsed(t1, p_now());
        if (status != NX_SUCCESS)
        {
            (VOID)nx_packet_release(pkt);
            (VOID)P_OK(status, "send: socket send");
            break;
        }

        sent += chunk;
    }

    /* The transfer is not over until the receiver has it, and it is over
       before either side tears its socket down. */
    (VOID)tx_semaphore_get(&p_srv_gotall, 60UL * NX_IP_PERIODIC_RATE);

    ticks = p_elapsed(t0, p_now());

    if (packets_out != NX_NULL)
    {
        *packets_out = cip -> nx_ip_total_packets_sent - before_sent;
    }

    /*
     * Read the socket's own counters before it is torn down.  A retransmit
     * count above zero on a loopback transfer means packets are being dropped
     * -- almost always the pool -- and every conclusion about copy cost drawn
     * from a lossy run would be wrong.
     */
    (VOID)nx_tcp_socket_info_get(&p_client, NX_NULL, NX_NULL, NX_NULL, NX_NULL,
                                 &p_tcp_retransmits, NX_NULL, NX_NULL, NX_NULL,
                                 &p_tcp_txqueue, &p_tcp_txwindow,
                                 &p_tcp_rxwindow);

    (VOID)nx_packet_pool_info_get(&p_pool, NX_NULL, NX_NULL,
                                  &p_pool_empty_after, NX_NULL, NX_NULL);

    (VOID)nx_tcp_socket_disconnect(&p_client, 2UL * NX_IP_PERIODIC_RATE);
    (VOID)nx_tcp_client_socket_unbind(&p_client);
    (VOID)nx_tcp_socket_delete(&p_client);

    (VOID)tx_semaphore_get(&p_srv_done, 60UL * NX_IP_PERIODIC_RATE);

    (VOID)p_check((UINT)(p_srv_bytes >= bytes), "receiver got every byte",
                  p_srv_bytes);

    return(ticks);
}

static VOID p_run_case(const char *what, NX_IP *cip, NX_IP *sip, ULONG peer,
                       UINT port, UINT extract, ULONG mode)
{
ULONG   ticks, packets = 0UL, ms, kbps;
ULONG   calls, ckbytes, misaligned;

    p_ck_mode = mode;

    p_pool_empty_before = 0UL;
    (VOID)nx_packet_pool_info_get(&p_pool, NX_NULL, NX_NULL,
                                  &p_pool_empty_before, NX_NULL, NX_NULL);

    ticks = p_transfer(cip, sip, peer, port, extract, P_XFER_BYTES, &packets);

    calls      = p_ck_calls;
    ckbytes    = p_ck_bytes;
    misaligned = p_ck_misaligned;

    if (ticks == 0UL)
    {
        p_log("  %s: FAILED", (LONG)what);
        return;
    }

    ms   = p_ms(ticks);
    kbps = (ms != 0UL) ? ((P_XFER_BYTES / 1024UL) * 1000UL / ms) : 0UL;

    p_log("  %s", (LONG)what);
    p_log("      %ld ms, %ld KB/s, %ld IP packets, checksum %ld calls over %ld KB"
          " (%ld misaligned)",
          (LONG)ms, (LONG)kbps, (LONG)packets, (LONG)calls,
          (LONG)(ckbytes / 1024UL), (LONG)misaligned);
    p_log("      sender %ld calls: alloc %ld ms, append %ld ms, send %ld ms",
          (LONG)p_phase_calls, (LONG)p_ms(p_phase_alloc),
          (LONG)p_ms(p_phase_append), (LONG)p_ms(p_phase_send));
    p_log("      receiver %ld calls: receive %ld ms, extract %ld ms",
          (LONG)p_phase_rcalls, (LONG)p_ms(p_phase_recv),
          (LONG)p_ms(p_phase_extract));
    p_log("      tcp: retransmits %ld, tx queue %ld, tx window %ld,"
          " rx window %ld, pool empty %ld",
          (LONG)p_tcp_retransmits, (LONG)p_tcp_txqueue, (LONG)p_tcp_txwindow,
          (LONG)p_tcp_rxwindow,
          (LONG)(p_pool_empty_after - p_pool_empty_before));
}



/* ------------------------------------------------------- window sweep --- */

/*
 * One transfer per receive window, printed as a curve.
 *
 * The question this answers is whether the receive window bounds this stack
 * at its operating point, or whether the CPU does.  A window helps only when
 * the sender is waiting for an ACK; when the machine is the bottleneck the
 * curve is flat and every byte of extra window is pool held for nothing.
 *
 * 65535 is the last window a socket can be created with while
 * NX_ENABLE_TCP_WINDOW_SCALING is off -- nxe_tcp_socket_create.c:170 refuses
 * anything larger -- so it is the ceiling of the sweep rather than an
 * arbitrary stopping point.
 */

static const ULONG p_sweep_windows[] =
{
    2048UL, 4096UL, 8192UL, 16384UL, 32768UL, 65535UL
};

#define P_SWEEP_BYTES   (128UL * 1024UL)

static VOID p_window_sweep(const char *what, NX_IP *cip, NX_IP *sip,
                           ULONG peer, UINT port)
{
ULONG   i, ticks, ms, kbps, packets;
ULONG   saved = p_window;

    for (i = 0UL; i < (sizeof(p_sweep_windows) / sizeof(p_sweep_windows[0])); i++)
    {
        p_window = p_sweep_windows[i];
        p_ck_mode = P_CK_NET68K;

        packets = 0UL;
        ticks   = p_transfer(cip, sip, peer, port, 1, P_SWEEP_BYTES, &packets);
        if (ticks == 0UL)
        {
            p_log("  %-10s window %5ld: FAILED", (LONG)what, (LONG)p_window);
            continue;
        }

        ms   = p_ms(ticks);
        kbps = (ms != 0UL) ? ((P_SWEEP_BYTES / 1024UL) * 1000UL / ms) : 0UL;

        p_log("  %-10s window %5ld: %5ld ms, %4ld KB/s, %ld packets,"
              " retransmits %ld",
              (LONG)what, (LONG)p_window, (LONG)ms, (LONG)kbps,
              (LONG)packets, (LONG)p_tcp_retransmits);
    }

    p_window = saved;
}


/* ------------------------------------------------------ ThreadX startup --- */

VOID tx_application_define(VOID *first_unused_memory)
{
UINT    status;

    (VOID)first_unused_memory;

    nx_system_initialize();

    status = nx_packet_pool_create(&p_pool, "perf pool", P_PACKET_PAYLOAD,
                                   (VOID *)p_pool_memory,
                                   (ULONG)sizeof(p_pool_memory));
    (VOID)P_OK(status, "packet pool");

    status = nx_ip_create(&p_ip0, "ip0", P_IP0_ADDRESS, P_NETMASK, &p_pool,
                          _nx_ram_network_driver,
                          (VOID *)p_ip0_stack, (ULONG)sizeof(p_ip0_stack), 1);
    (VOID)P_OK(status, "ip0 create");

    status = nx_ip_create(&p_ip1, "ip1", P_IP1_ADDRESS, P_NETMASK, &p_pool,
                          _nx_ram_network_driver,
                          (VOID *)p_ip1_stack, (ULONG)sizeof(p_ip1_stack), 1);
    (VOID)P_OK(status, "ip1 create");

    (VOID)nx_arp_enable(&p_ip0, (VOID *)p_arp0_cache, (ULONG)sizeof(p_arp0_cache));
    (VOID)nx_arp_enable(&p_ip1, (VOID *)p_arp1_cache, (ULONG)sizeof(p_arp1_cache));

    (VOID)nx_tcp_enable(&p_ip0);
    (VOID)nx_tcp_enable(&p_ip1);

    (VOID)tx_semaphore_create(&p_srv_ready,  "srv ready", 0);
    (VOID)tx_semaphore_create(&p_srv_gotall, "srv gotall", 0);
    (VOID)tx_semaphore_create(&p_srv_done,   "srv done",  0);

    status = tx_thread_create(&p_srv_thread, "perf server", p_server_entry, 0UL,
                              (VOID *)p_srv_stack, (ULONG)sizeof(p_srv_stack),
                              16, 16, TX_NO_TIME_SLICE, TX_AUTO_START);
    (VOID)p_check((UINT)(status == TX_SUCCESS), "server thread", status);
}


/* ------------------------------------------------------------------ main -- */

int main(void)
{
UINT    status;
ULONG   actual;

    p_log("AmiNetXDuo -- TCP data path cost census");

    status = tx_amiga_kernel_start();
    if (status != TX_SUCCESS)
    {
        p_log("FATAL: tx_amiga_kernel_start() = %ld", (ULONG)status);
        p_flush();
        return(20);
    }

    status = tx_amiga_adopt_thread(&p_main_thread, "perf client", 16);
    if (!p_check((UINT)(status == TX_SUCCESS), "adopted this Exec Task", status))
    {
        p_flush();
        return(20);
    }

    p_timer_init();
    p_log("");
    p_log("E-Clock %ld Hz, %ld ns/tick, bracket overhead %ld ticks (%ld ns)",
          (LONG)p_rate, (LONG)p_tick_ns, (LONG)p_bracket,
          (LONG)(p_bracket * p_tick_ns));

    (VOID)nx_ip_status_check(&p_ip0, NX_IP_INITIALIZE_DONE, &actual,
                             10UL * NX_IP_PERIODIC_RATE);
    (VOID)nx_ip_status_check(&p_ip1, NX_IP_INITIALIZE_DONE, &actual,
                             10UL * NX_IP_PERIODIC_RATE);

    p_scratch_build();

    p_log("");
    p_log("-- alignment census -------------------------------------------");
    p_census_alignment();

    p_log("");
    p_log("-- checksum ---------------------------------------------------");
    p_bench_checksum();

    p_log("");
    p_log("-- copies -----------------------------------------------------");
    p_bench_copies();

    p_log("");
    p_log("-- packet plumbing --------------------------------------------");
    p_bench_packets();

    p_scratch_free();

    p_log("");
    p_log("-- pipeline ceiling (no protocol) -----------------------------");
    p_bench_pipeline(P_CK_VENDORED, "loopback pipeline, vendored ck");
    p_bench_pipeline(P_CK_NET68K,   "loopback pipeline, net68k ck");

    p_log("");
    p_log("-- end to end, %ld KB per run ---------------------------------",
          (LONG)(P_XFER_BYTES / 1024UL));

    /*
     * Loopback first, because that is the figure the README quotes and the
     * conformance suite measures.  "drain only" releases each packet the
     * moment it arrives; "+extract" copies it out to an application buffer
     * the way recv() does, so the difference between the two is what
     * nx_packet_data_extract_offset() costs in situ.
     */
    p_run_case("loopback, drain only, vendored", &p_ip0, &p_ip0, P_LOOPBACK,
               P_PORT_LOOP, 0, P_CK_VENDORED);
    p_run_case("loopback, drain only, net68k",   &p_ip0, &p_ip0, P_LOOPBACK,
               P_PORT_LOOP, 0, P_CK_NET68K);
    p_run_case("loopback, +extract, vendored",   &p_ip0, &p_ip0, P_LOOPBACK,
               P_PORT_LOOP, 1, P_CK_VENDORED);
    p_run_case("loopback, +extract, net68k",     &p_ip0, &p_ip0, P_LOOPBACK,
               P_PORT_LOOP, 1, P_CK_NET68K);

    /*
     * And over the simulated wire, which is the shape a real interface has:
     * MSS-sized segments rather than 8 KB chains, an Ethernet header, and a
     * driver that copies the frame instead of handing the packet over.
     */
    p_run_case("wire, +extract, vendored",       &p_ip0, &p_ip1, P_IP1_ADDRESS,
               P_PORT_WIRE, 1, P_CK_VENDORED);
    p_run_case("wire, +extract, net68k",         &p_ip0, &p_ip1, P_IP1_ADDRESS,
               P_PORT_WIRE, 1, P_CK_NET68K);

    p_log("");
    p_log("-- receive window sweep, %ld KB per run -----------------------",
          (LONG)(P_SWEEP_BYTES / 1024UL));
    p_window_sweep("loopback", &p_ip0, &p_ip0, P_LOOPBACK, P_PORT_LOOP);
    p_window_sweep("wire",     &p_ip0, &p_ip1, P_IP1_ADDRESS, P_PORT_WIRE);

    p_log("");
    p_log("%ld checks, %ld failures -- %s",
          p_checks, p_failures, (p_failures == 0UL) ? "PASS" : "FAIL");

    (VOID)tx_amiga_orphan_thread(&p_main_thread);

    p_flush();

    return((p_failures == 0UL) ? 0 : 20);
}

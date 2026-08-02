/*
 * AmiNetXDuo -- a TCP transfer with the PC sampler running over it.
 *
 * WHY THIS WORKLOAD.  tests/perf/perf_test.c prices the primitives and finds
 * about 22% of a transfer in the copy and the checksum, leaving 78% inside
 * NetX Duo with no name on it.  The two cases below are the same two the
 * census runs, cut down to the transfer itself:
 *
 *   loopback   client and server on one NX_IP through 127.0.0.1.  No driver,
 *              no Ethernet header, no emulated NIC -- protocol processing and
 *              nothing else.  This is the cleanest possible read on the 78%,
 *              because every sample that is not application code is NetX Duo
 *              or the OS underneath it.
 *   wire       two NX_IPs across _nx_ram_network_driver, so MSS-sized
 *              segments, a link header and a driver that copies the frame.
 *              Closer to the shape of a real interface, and the difference
 *              between the two rankings is what the driver and the segment
 *              size cost.
 *
 * Neither goes near bsdsocket.library, which is deliberate: the library is a
 * separately loaded hunk whose relocation would have to be recovered before
 * any address meant anything, and it is not where the missing time is.  The
 * unaccounted 78% is inside NetX Duo, and NetX Duo is linked into this
 * executable, so prof_scan_segs() walking our own seglist resolves all of it.
 *
 * tests/tcpdrill and tests/perf/fitzbench were the alternatives.  Both drive
 * real transfers through the library over an emulated A2065, and both would
 * have put the interesting code in a hunk this profiler cannot yet locate,
 * on top of a NIC whose own interrupt is level 2 and whose SLIRP backend adds
 * host-side variance to every run.  They answer a different question.
 *
 * SPDX-License-Identifier: MIT
 */

#include "tx_api.h"
#include "tx_amiga.h"
#include "nx_api.h"
#include "nx_packet.h"

#include "prof.h"
#include "aminetxduo/compat.h"

#include <exec/types.h>
#include <proto/exec.h>

#include <string.h>


/* Same fabric as perf_test.c's end-to-end cases, and the same numbers, so the
   two are comparable. */
#define T_PACKET_PAYLOAD    1568
#define T_PACKET_COUNT      64
#define T_PACKET_OVERHEAD   96

#define T_IP0_ADDRESS       IP_ADDRESS(192, 168, 100, 1)
#define T_IP1_ADDRESS       IP_ADDRESS(192, 168, 100, 2)
#define T_LOOPBACK          IP_ADDRESS(127, 0, 0, 1)
#define T_NETMASK           0xFFFFFF00UL
#define T_PORT_LOOP         5101
#define T_PORT_WIRE         5102

#define T_IP_STACK_SIZE     3072
#define T_SRV_STACK_SIZE    4096
#define T_APP_CHUNK         8192UL
#define T_WINDOW            16384UL

#define T_XFER_BYTES        (1024UL * 1024UL)
#define T_RATE              1000UL
#define T_MAX_SAMPLES       120000UL

extern VOID _nx_ram_network_driver(NX_IP_DRIVER *driver_req_ptr);

static NX_PACKET_POOL   t_pool;
static NX_IP            t_ip0;
static NX_IP            t_ip1;
static NX_TCP_SOCKET    t_client;
static NX_TCP_SOCKET    t_server;
static TX_THREAD        t_srv_thread;
static TX_THREAD        t_main_thread;
static TX_SEMAPHORE     t_srv_ready;
static TX_SEMAPHORE     t_srv_gotall;
static TX_SEMAPHORE     t_srv_done;

static ULONG    t_pool_memory[(T_PACKET_COUNT * (T_PACKET_PAYLOAD + T_PACKET_OVERHEAD)) / sizeof(ULONG)];
static ULONG    t_ip0_stack[T_IP_STACK_SIZE / sizeof(ULONG)];
static ULONG    t_ip1_stack[T_IP_STACK_SIZE / sizeof(ULONG)];
static ULONG    t_srv_stack[T_SRV_STACK_SIZE / sizeof(ULONG)];
static ULONG    t_arp0_cache[1024 / sizeof(ULONG)];
static ULONG    t_arp1_cache[1024 / sizeof(ULONG)];

static UCHAR    t_src_buf[T_APP_CHUNK] __attribute__((aligned(4)));
static UCHAR    t_dst_buf[T_APP_CHUNK] __attribute__((aligned(4)));

static NX_IP           *t_run_ip;
static UINT             t_run_port;
static ULONG            t_run_target;
static volatile ULONG   t_srv_bytes;
static ULONG            t_failures;


static VOID t_server_entry(ULONG id)
{
UINT        status;
NX_PACKET  *pkt;
ULONG       length, moved, off;

    (VOID)id;

    for (;;)
    {
        if (tx_semaphore_get(&t_srv_ready, TX_WAIT_FOREVER) != TX_SUCCESS)
        {
            return;
        }

        t_srv_bytes = 0UL;

        status = nx_tcp_socket_create(t_run_ip, &t_server, "prof server",
                                      NX_IP_NORMAL, NX_FRAGMENT_OKAY,
                                      NX_IP_TIME_TO_LIVE, T_WINDOW,
                                      NX_NULL, NX_NULL);
        if (status != NX_SUCCESS)
        {
            (VOID)tx_semaphore_put(&t_srv_gotall);
            (VOID)tx_semaphore_put(&t_srv_done);
            continue;
        }

        (VOID)nx_tcp_server_socket_listen(t_run_ip, t_run_port, &t_server, 5,
                                          NX_NULL);

        if (nx_tcp_server_socket_accept(&t_server, 20UL * NX_IP_PERIODIC_RATE)
                == NX_SUCCESS)
        {
            while (t_srv_bytes < t_run_target)
            {
                pkt = NX_NULL;
                if (nx_tcp_socket_receive(&t_server, &pkt,
                                          20UL * NX_IP_PERIODIC_RATE)
                        != NX_SUCCESS)
                {
                    break;
                }

                length = 0UL;
                (VOID)nx_packet_length_get(pkt, &length);

                /* Drain to a user buffer, which is what bsd_recv_tcp() does.
                   Without it the receive side is only a packet release and
                   the profile misses nx_packet_data_extract_offset(). */
                for (off = 0UL; off < length; )
                {
                ULONG want = length - off;

                    if (want > T_APP_CHUNK)
                    {
                        want = T_APP_CHUNK;
                    }
                    moved = 0UL;
                    if (nx_packet_data_extract_offset(pkt, off, t_dst_buf,
                                                      want, &moved)
                            != NX_SUCCESS || moved == 0UL)
                    {
                        break;
                    }
                    off += moved;
                }

                t_srv_bytes += length;
                (VOID)nx_packet_release(pkt);
            }
        }

        (VOID)tx_semaphore_put(&t_srv_gotall);

        (VOID)nx_tcp_socket_disconnect(&t_server, 5UL * NX_IP_PERIODIC_RATE);
        (VOID)nx_tcp_server_socket_unaccept(&t_server);
        (VOID)nx_tcp_server_socket_unlisten(t_run_ip, t_run_port);
        (VOID)nx_tcp_socket_delete(&t_server);

        (VOID)tx_semaphore_put(&t_srv_done);
    }
}


/*
 * One transfer, with the sampler bracketing the bulk phase only.  Setup and
 * teardown are marked separately rather than excluded, so the report can show
 * how little of the run they are instead of the reader having to trust it.
 */
static ULONG t_transfer(NX_IP *cip, NX_IP *sip, ULONG peer, UINT port,
                        ULONG bytes)
{
UINT        status;
NX_PACKET  *pkt;
ULONG       sent = 0UL;
ULONG       t0, ms;

    t_run_ip     = sip;
    t_run_port   = port;
    t_run_target = bytes;

    prof_mark("setup");

    (VOID)tx_semaphore_put(&t_srv_ready);
    tx_thread_sleep(NX_IP_PERIODIC_RATE / 5UL);

    status = nx_tcp_socket_create(cip, &t_client, "prof client", NX_IP_NORMAL,
                                  NX_FRAGMENT_OKAY, NX_IP_TIME_TO_LIVE,
                                  T_WINDOW, NX_NULL, NX_NULL);
    if (status != NX_SUCCESS)
    {
        t_failures++;
        (VOID)tx_semaphore_get(&t_srv_done, 30UL * NX_IP_PERIODIC_RATE);
        return(0UL);
    }

    (VOID)nx_tcp_client_socket_bind(&t_client, NX_ANY_PORT,
                                    5UL * NX_IP_PERIODIC_RATE);

    status = nx_tcp_client_socket_connect(&t_client, peer, port,
                                          20UL * NX_IP_PERIODIC_RATE);
    if (status != NX_SUCCESS)
    {
        t_failures++;
        (VOID)nx_tcp_client_socket_unbind(&t_client);
        (VOID)nx_tcp_socket_delete(&t_client);
        (VOID)tx_semaphore_get(&t_srv_done, 30UL * NX_IP_PERIODIC_RATE);
        return(0UL);
    }

    prof_mark("transfer");
    t0 = ami_millis();

    while (sent < bytes)
    {
    ULONG chunk = bytes - sent;

        if (chunk > T_APP_CHUNK)
        {
            chunk = T_APP_CHUNK;
        }

        pkt = NX_NULL;
        if (nx_packet_allocate(&t_pool, &pkt, NX_TCP_PACKET,
                               10UL * NX_IP_PERIODIC_RATE) != NX_SUCCESS)
        {
            t_failures++;
            break;
        }
        if (nx_packet_data_append(pkt, t_src_buf, chunk, &t_pool,
                                  10UL * NX_IP_PERIODIC_RATE) != NX_SUCCESS)
        {
            (VOID)nx_packet_release(pkt);
            t_failures++;
            break;
        }
        if (nx_tcp_socket_send(&t_client, pkt,
                               20UL * NX_IP_PERIODIC_RATE) != NX_SUCCESS)
        {
            (VOID)nx_packet_release(pkt);
            t_failures++;
            break;
        }

        sent += chunk;
    }

    (VOID)tx_semaphore_get(&t_srv_gotall, 120UL * NX_IP_PERIODIC_RATE);
    ms = ami_millis() - t0;

    prof_mark("teardown");

    (VOID)nx_tcp_socket_disconnect(&t_client, 2UL * NX_IP_PERIODIC_RATE);
    (VOID)nx_tcp_client_socket_unbind(&t_client);
    (VOID)nx_tcp_socket_delete(&t_client);
    (VOID)tx_semaphore_get(&t_srv_done, 60UL * NX_IP_PERIODIC_RATE);

    if (t_srv_bytes < bytes)
    {
        prof_log("  WARNING: receiver got %ld of %ld bytes",
                 (long)t_srv_bytes, (long)bytes);
        t_failures++;
    }

    return(ms);
}


static VOID t_case(const char *what, const char *file,
                   NX_IP *cip, NX_IP *sip, ULONG peer, UINT port)
{
ULONG ms;

    if (!prof_start(T_MAX_SAMPLES, T_RATE))
    {
        prof_log("FATAL: prof_start: %s", prof_error());
        t_failures++;
        return;
    }

    ms = t_transfer(cip, sip, peer, port, T_XFER_BYTES);

    prof_mark("end");
    prof_stop();

    prof_log("%s: %ld KB in %ld ms (%ld KB/s), %ld samples, %ld interrupts,"
             " %ld ours, %ld dropped",
             what, (long)(T_XFER_BYTES / 1024UL), (long)ms,
             (long)(ms ? (T_XFER_BYTES / 1024UL) * 1000UL / ms : 0UL),
             (long)prof_stored(), (long)prof_hit_count(),
             (long)prof_cia_count(), (long)prof_drop_count());

    if (prof_odd_formats() != 0UL)
    {
        prof_log("  FATAL: %ld frames were not format $0 -- the PCs in this"
                 " profile are not trustworthy", (long)prof_odd_formats());
        t_failures++;
    }

    if (!prof_write(file))
    {
        prof_log("  could not write %s: %s", file, prof_error());
        t_failures++;
    }
    else
    {
        prof_log("  wrote %s", file);
    }

    prof_free();
}


VOID tx_application_define(VOID *first_unused_memory)
{
    (VOID)first_unused_memory;

    nx_system_initialize();

    (VOID)nx_packet_pool_create(&t_pool, "prof pool", T_PACKET_PAYLOAD,
                                (VOID *)t_pool_memory,
                                (ULONG)sizeof(t_pool_memory));

    (VOID)nx_ip_create(&t_ip0, "ip0", T_IP0_ADDRESS, T_NETMASK, &t_pool,
                       _nx_ram_network_driver,
                       (VOID *)t_ip0_stack, (ULONG)sizeof(t_ip0_stack), 1);
    (VOID)nx_ip_create(&t_ip1, "ip1", T_IP1_ADDRESS, T_NETMASK, &t_pool,
                       _nx_ram_network_driver,
                       (VOID *)t_ip1_stack, (ULONG)sizeof(t_ip1_stack), 1);

    (VOID)nx_arp_enable(&t_ip0, (VOID *)t_arp0_cache, (ULONG)sizeof(t_arp0_cache));
    (VOID)nx_arp_enable(&t_ip1, (VOID *)t_arp1_cache, (ULONG)sizeof(t_arp1_cache));

    (VOID)nx_tcp_enable(&t_ip0);
    (VOID)nx_tcp_enable(&t_ip1);

    (VOID)tx_semaphore_create(&t_srv_ready,  "srv ready",  0);
    (VOID)tx_semaphore_create(&t_srv_gotall, "srv gotall", 0);
    (VOID)tx_semaphore_create(&t_srv_done,   "srv done",   0);

    (VOID)tx_thread_create(&t_srv_thread, "prof server", t_server_entry, 0UL,
                           (VOID *)t_srv_stack, (ULONG)sizeof(t_srv_stack),
                           16, 16, TX_NO_TIME_SLICE, TX_AUTO_START);
}


int main(void)
{
UINT status;

    prof_log("AmiNetXDuo -- TCP transfer under the PC sampler");

    memset(t_src_buf, 0x5A, sizeof(t_src_buf));

    status = tx_amiga_kernel_start();
    if (status != TX_SUCCESS)
    {
        prof_log("FATAL: tx_amiga_kernel_start() = %ld", (long)status);
        return(20);
    }

    status = tx_amiga_adopt_thread(&t_main_thread, "prof client", 16);
    if (status != TX_SUCCESS)
    {
        prof_log("FATAL: tx_amiga_adopt_thread() = %ld", (long)status);
        return(20);
    }

    t_case("loopback", "profile-loop.bin", &t_ip0, &t_ip0,
           T_LOOPBACK, T_PORT_LOOP);
    t_case("wire",     "profile-wire.bin", &t_ip0, &t_ip1,
           T_IP1_ADDRESS, T_PORT_WIRE);

    (VOID)tx_amiga_orphan_thread(&t_main_thread);

    prof_log("%ld failures -- %s", (long)t_failures,
             t_failures == 0UL ? "PASS" : "FAIL");

    return(t_failures == 0UL ? 0 : 20);
}

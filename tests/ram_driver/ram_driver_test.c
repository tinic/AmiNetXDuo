/*
 * AmiNetXDuo -- milestone 2 bring-up test.
 *
 * Proves, on real 68k, that:
 *   - the ThreadX Exec port schedules (baton model, docs/RESEARCH.md 6.2);
 *   - a pre-existing Exec Task can be adopted as a TX_THREAD and be suspended
 *     and resumed by NetX Duo (docs/RESEARCH.md 6.3, option A);
 *   - the periodic tick drives ThreadX timers and NetX Duo's IP timers;
 *   - the NetX Duo packet pool, two NX_IP instances, ARP and TCP work;
 *   - data survives a round trip between the two IP instances over the
 *     in-tree simulated RAM driver.
 *
 * Shape: main() is an ordinary AmigaDOS Process.  It starts ThreadX on a
 * private task with tx_amiga_kernel_start(), then ADOPTS ITSELF so it can run
 * the client half of the test in its own context -- which is precisely the
 * thing bsdsocket.library will have to do for every application task.  The
 * server half runs on a thread that ThreadX created, so both thread flavours
 * are exercised against each other.
 *
 * Output goes to the serial debug port (visible in FS-UAE/WinUAE's serial log)
 * as it happens, and is replayed to stdout at the end -- ThreadX threads are
 * Tasks, not Processes, so they cannot use dos.library themselves.
 *
 * SPDX-License-Identifier: MIT
 */

#include "tx_api.h"
#include "tx_amiga.h"
#include "nx_api.h"

#include <exec/types.h>
#include <exec/execbase.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include <stdarg.h>


/* ------------------------------------------------------------- logging --- */

#ifndef RawPutChar
#  define RawPutChar(c) \
      LP1NR(0x204, RawPutChar, UBYTE, (c), d0, , EXEC_BASE_NAME)
#endif

#define T_LOG_SIZE      6144

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


    /*
     * No lock.  Every caller is either tx_application_define() (before any
     * thread exists) or a ThreadX thread, and the baton guarantees that only
     * one ThreadX thread runs at a time -- so the log cannot interleave.
     * Wrapping this in Forbid() would be worse than useless: RawPutChar()
     * busy-waits on the serial port, and 60 characters at 9600 baud would
     * hold off the tick task for tens of milliseconds and distort the very
     * timing the test is measuring.
     */
    va_start(args, fmt);
    RawDoFmt((STRPTR) fmt, args, (void (*)()) t_put_char, NULL);
    va_end(args);

    t_put('\n');
}


/*
 * Replay the captured log to stdout.  Only safe from main(): dos.library
 * requires a Process, and every ThreadX thread here is a bare Exec Task.
 */
static VOID t_flush(VOID)
{

BPTR    out;


    out =  Output();
    if (out != (BPTR) 0)
    {
        (VOID) Write(out, (APTR) t_log_buffer, (LONG) t_log_used);
    }
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
#define T_TX_OK(status, what)   t_check((UINT) ((status) == TX_SUCCESS), (what), (ULONG) (status))


/* ---------------------------------------------------------- test fabric -- */

#define T_PACKET_PAYLOAD        1568        /* == AMI_POOL_PAYLOAD          */
#define T_PACKET_COUNT          24
#define T_PACKET_OVERHEAD       96          /* NX_PACKET header + alignment */

#define T_IP0_ADDRESS           IP_ADDRESS(192, 168, 100, 1)
#define T_IP1_ADDRESS           IP_ADDRESS(192, 168, 100, 2)
#define T_NETMASK               0xFFFFFF00UL
#define T_PORT                  5001

#define T_IP_STACK_SIZE         3072
#define T_SERVER_STACK_SIZE     3072

static const char t_message[] = "AmiNetXDuo says hello from an adopted Exec Task";

extern VOID _nx_ram_network_driver(NX_IP_DRIVER *driver_req_ptr);

static NX_PACKET_POOL   t_pool;
static NX_IP            t_ip0;
static NX_IP            t_ip1;
static NX_TCP_SOCKET    t_client_socket;
static NX_TCP_SOCKET    t_server_socket;

static TX_THREAD        t_server_thread;
static TX_THREAD        t_main_thread;
static TX_SEMAPHORE     t_server_done;

static ULONG            t_pool_memory[(T_PACKET_COUNT * (T_PACKET_PAYLOAD + T_PACKET_OVERHEAD)) / sizeof(ULONG)];
static ULONG            t_ip0_stack[T_IP_STACK_SIZE / sizeof(ULONG)];
static ULONG            t_ip1_stack[T_IP_STACK_SIZE / sizeof(ULONG)];
static ULONG            t_server_stack[T_SERVER_STACK_SIZE / sizeof(ULONG)];
static ULONG            t_arp0_cache[1024 / sizeof(ULONG)];
static ULONG            t_arp1_cache[1024 / sizeof(ULONG)];


/* --------------------------------------------------------- server half --- */

static VOID t_server_entry(ULONG id)
{

UINT        status;
NX_PACKET  *packet_ptr;
ULONG       length;
ULONG       actual;
CHAR        buffer[80];


    (VOID) id;

    t_log("server: thread entry (ThreadX-created Exec Task)");

    status =  nx_ip_status_check(&t_ip1, NX_IP_INITIALIZE_DONE, &actual,
                                 5UL * NX_IP_PERIODIC_RATE);
    (VOID) T_OK(status, "server: ip1 initialised");

    status =  nx_tcp_socket_create(&t_ip1, &t_server_socket, "server socket",
                                   NX_IP_NORMAL, NX_FRAGMENT_OKAY, NX_IP_TIME_TO_LIVE,
                                   2048, NX_NULL, NX_NULL);
    (VOID) T_OK(status, "server: socket create");

    status =  nx_tcp_server_socket_listen(&t_ip1, T_PORT, &t_server_socket, 5, NX_NULL);
    (VOID) T_OK(status, "server: listen");

    status =  nx_tcp_server_socket_accept(&t_server_socket, 10UL * NX_IP_PERIODIC_RATE);
    (VOID) T_OK(status, "server: accept");

    /* Receive.  This suspends THIS ThreadX thread inside NetX Duo.  */
    packet_ptr =  NX_NULL;
    status =  nx_tcp_socket_receive(&t_server_socket, &packet_ptr,
                                    10UL * NX_IP_PERIODIC_RATE);
    if (T_OK(status, "server: receive"))
    {

        length =  0;
        (VOID) nx_packet_length_get(packet_ptr, &length);
        (VOID) t_check((UINT) (length == (ULONG) sizeof(t_message)),
                       "server: payload length", length);

        actual =  0;
        (VOID) nx_packet_data_retrieve(packet_ptr, buffer, &actual);
        buffer[sizeof(buffer) - 1] =  '\0';
        t_log("server: got \"%s\" (%ld bytes)", buffer, actual);

        /* Echo it straight back.  */
        status =  nx_tcp_socket_send(&t_server_socket, packet_ptr,
                                     5UL * NX_IP_PERIODIC_RATE);
        if (!T_OK(status, "server: echo send"))
        {
            (VOID) nx_packet_release(packet_ptr);
        }
    }

    status =  nx_tcp_socket_disconnect(&t_server_socket, 5UL * NX_IP_PERIODIC_RATE);
    (VOID) T_OK(status, "server: disconnect");

    (VOID) nx_tcp_server_socket_unaccept(&t_server_socket);
    (VOID) nx_tcp_server_socket_unlisten(&t_ip1, T_PORT);
    (VOID) nx_tcp_socket_delete(&t_server_socket);

    t_log("server: done");

    (VOID) tx_semaphore_put(&t_server_done);

    /* Falls off the end -> TX_COMPLETED; the Exec Task parks until deleted.  */
}


/* --------------------------------------------------------- client half --- */

static UINT t_client_run(VOID)
{

UINT        status;
NX_PACKET  *packet_ptr;
ULONG       actual;
CHAR        buffer[80];
UINT        i;


    t_log("client: running on the adopted Exec Task");

    status =  nx_ip_status_check(&t_ip0, NX_IP_INITIALIZE_DONE, &actual,
                                 5UL * NX_IP_PERIODIC_RATE);
    (VOID) T_OK(status, "client: ip0 initialised");

    /* The ThreadX clock must be advancing, or nothing below can time out
       correctly.  This is the tick task under test.  */
    actual =  tx_time_get();
    tx_thread_sleep(NX_IP_PERIODIC_RATE / 4UL);
    (VOID) t_check((UINT) (tx_time_get() > actual), "client: tick advances",
                   tx_time_get() - actual);

    status =  nx_tcp_socket_create(&t_ip0, &t_client_socket, "client socket",
                                   NX_IP_NORMAL, NX_FRAGMENT_OKAY, NX_IP_TIME_TO_LIVE,
                                   2048, NX_NULL, NX_NULL);
    (VOID) T_OK(status, "client: socket create");

    status =  nx_tcp_client_socket_bind(&t_client_socket, NX_ANY_PORT,
                                        5UL * NX_IP_PERIODIC_RATE);
    (VOID) T_OK(status, "client: bind");

    status =  nx_tcp_client_socket_connect(&t_client_socket, T_IP1_ADDRESS, T_PORT,
                                           10UL * NX_IP_PERIODIC_RATE);
    if (!T_OK(status, "client: connect"))
    {
        (VOID) nx_tcp_client_socket_unbind(&t_client_socket);
        (VOID) nx_tcp_socket_delete(&t_client_socket);
        return(TX_FALSE);
    }

    status =  nx_packet_allocate(&t_pool, &packet_ptr, NX_TCP_PACKET,
                                 5UL * NX_IP_PERIODIC_RATE);
    (VOID) T_OK(status, "client: packet allocate");

    if (status == NX_SUCCESS)
    {

        status =  nx_packet_data_append(packet_ptr, (VOID *) t_message,
                                        (ULONG) sizeof(t_message), &t_pool,
                                        5UL * NX_IP_PERIODIC_RATE);
        (VOID) T_OK(status, "client: data append");

        status =  nx_tcp_socket_send(&t_client_socket, packet_ptr,
                                     5UL * NX_IP_PERIODIC_RATE);
        if (!T_OK(status, "client: send"))
        {
            (VOID) nx_packet_release(packet_ptr);
        }
    }

    /* Read the echo back.  Suspends the adopted Exec Task inside NetX Duo --
       the whole point of the adoption experiment.  */
    packet_ptr =  NX_NULL;
    status =  nx_tcp_socket_receive(&t_client_socket, &packet_ptr,
                                    10UL * NX_IP_PERIODIC_RATE);
    if (T_OK(status, "client: receive echo"))
    {

        actual =  0;
        for (i = 0; i < (UINT) sizeof(buffer); i++)
        {
            buffer[i] =  (CHAR) 0;
        }
        (VOID) nx_packet_data_retrieve(packet_ptr, buffer, &actual);

        (VOID) t_check((UINT) (actual == (ULONG) sizeof(t_message)),
                       "client: echo length", actual);

        status =  NX_SUCCESS;
        for (i = 0; i < (UINT) sizeof(t_message); i++)
        {
            if (buffer[i] != t_message[i])
            {
                status =  (UINT) i + 1U;
                break;
            }
        }
        (VOID) t_check((UINT) (status == NX_SUCCESS), "client: echo contents",
                       (ULONG) status);

        (VOID) nx_packet_release(packet_ptr);
    }

    (VOID) nx_tcp_socket_disconnect(&t_client_socket, 5UL * NX_IP_PERIODIC_RATE);
    (VOID) nx_tcp_client_socket_unbind(&t_client_socket);
    (VOID) nx_tcp_socket_delete(&t_client_socket);

    /* Wait for the server thread to finish before we tear anything down.  */
    status =  tx_semaphore_get(&t_server_done, 10UL * NX_IP_PERIODIC_RATE);
    (VOID) T_TX_OK(status, "client: server completed");

    t_log("client: done");

    return(TX_TRUE);
}


/* ------------------------------------------------------ ThreadX startup --- */

VOID tx_application_define(VOID *first_unused_memory)
{

UINT    status;


    (VOID) first_unused_memory;

    t_log("define: nx_system_initialize");
    nx_system_initialize();

    status =  nx_packet_pool_create(&t_pool, "AmiNetXDuo pool",
                                    T_PACKET_PAYLOAD,
                                    (VOID *) t_pool_memory,
                                    (ULONG) sizeof(t_pool_memory));
    (VOID) T_OK(status, "define: packet pool");

    status =  nx_ip_create(&t_ip0, "ip0", T_IP0_ADDRESS, T_NETMASK, &t_pool,
                           _nx_ram_network_driver,
                           (VOID *) t_ip0_stack, (ULONG) sizeof(t_ip0_stack), 1);
    (VOID) T_OK(status, "define: ip0 create");

    status =  nx_ip_create(&t_ip1, "ip1", T_IP1_ADDRESS, T_NETMASK, &t_pool,
                           _nx_ram_network_driver,
                           (VOID *) t_ip1_stack, (ULONG) sizeof(t_ip1_stack), 1);
    (VOID) T_OK(status, "define: ip1 create");

    status =  nx_arp_enable(&t_ip0, (VOID *) t_arp0_cache, (ULONG) sizeof(t_arp0_cache));
    (VOID) T_OK(status, "define: ip0 arp");

    status =  nx_arp_enable(&t_ip1, (VOID *) t_arp1_cache, (ULONG) sizeof(t_arp1_cache));
    (VOID) T_OK(status, "define: ip1 arp");

    status =  nx_tcp_enable(&t_ip0);
    (VOID) T_OK(status, "define: ip0 tcp");

    status =  nx_tcp_enable(&t_ip1);
    (VOID) T_OK(status, "define: ip1 tcp");

    status =  tx_semaphore_create(&t_server_done, "server done", 0);
    (VOID) T_TX_OK(status, "define: semaphore");

    status =  tx_thread_create(&t_server_thread, "server", t_server_entry, 0UL,
                               (VOID *) t_server_stack, (ULONG) sizeof(t_server_stack),
                               16, 16, TX_NO_TIME_SLICE, TX_AUTO_START);
    (VOID) T_TX_OK(status, "define: server thread");
}


/* ------------------------------------------------------------------ main -- */

int main(void)
{

UINT    status;


    t_log("AmiNetXDuo -- ThreadX/NetX Duo RAM driver bring-up test");
    t_log("  ticks/sec %ld, pool %ld x %ld bytes",
          (ULONG) TX_TIMER_TICKS_PER_SECOND, (ULONG) T_PACKET_COUNT,
          (ULONG) T_PACKET_PAYLOAD);

    status =  tx_amiga_kernel_start();
    if (status != TX_SUCCESS)
    {
        t_log("FATAL: tx_amiga_kernel_start() = %ld", (ULONG) status);
        t_flush();
        return(20);
    }
    t_log("kernel: scheduler running");

    status =  tx_amiga_adopt_thread(&t_main_thread, "test client", 16);
    if (!T_TX_OK(status, "main: adopted this Exec Task"))
    {
        t_flush();
        return(20);
    }

    (VOID) t_check((UINT) (tx_thread_identify() == &t_main_thread),
                   "main: tx_thread_identify() is us", 0);

    (VOID) t_client_run();

    status =  tx_amiga_orphan_thread(&t_main_thread);
    (VOID) T_TX_OK(status, "main: orphaned this Exec Task");

    t_log("");
    t_log("%ld checks, %ld failures -- %s",
          t_checks, t_failures, (t_failures == 0UL) ? "PASS" : "FAIL");

    t_flush();

    return((t_failures == 0UL) ? 0 : 20);
}

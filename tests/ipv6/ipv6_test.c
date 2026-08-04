/*
 * AmiNetXDuo, milestone 8: the IPv6 dual stack, on 68k.
 *
 * Shaped like tests/ram_driver/ram_driver_test.c, and for the same reason:
 * two NX_IP instances on one simulated wire prove the stack end to end
 * without needing anything outside the emulator.  That matters more here than
 * it did for IPv4, since the emulated network may not carry IPv6 at all,
 * see tests/ipv6/ipv6_link_test.c, which probes that empirically.  This
 * test's result does not depend on it.
 *
 *   1. nxd_ipv6_enable() + nxd_icmp_enable() bring the dual stack up on a
 *      68020, and ::1 is configured by doing so.
 *   2. A link-local address is derived from the interface MAC as RFC 4291
 *      modified EUI-64 requires, and survives duplicate address detection,
 *      which means solicited-node multicast, neighbour solicitations and
 *      neighbour advertisements all work.
 *   3. ICMPv6 echo request/reply works, over loopback and between two
 *      separate NX_IP instances across the link.
 *   4. TCP over IPv6 completes a three-way handshake, moves data both ways
 *      and closes, between an adopted Exec Task and a ThreadX-created one.
 *   5. UDP over IPv6 carries a datagram and reports the source correctly
 *      through nxd_udp_source_extract().
 *   6. The IPv6 text conversions round-trip.
 *
 * None of this runs over a real SANA-II device: the shim's 0x86DD reader has
 * no wire here (that is ipv6_link_test.c's job), nor does bsdsocket.library's
 * AF_INET6 surface (ipv6_socket_test.c's).
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

#ifdef NX_DISABLE_IPV6
#error "tests/ipv6 requires -DAMINETXDUO_IPV6=ON"
#endif


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

    if (t_log_used < (ULONG)(T_LOG_SIZE - 1))
    {
        t_log_buffer[t_log_used++] = (char)c;
    }
}

static VOID t_put_char(register UBYTE c      __asm("d0"),
                       register APTR  unused __asm("a3"))
{
    (VOID)unused;
    if (c != '\0')
    {
        t_put(c);
    }
}

static VOID t_log(const char *fmt, ...)
{

va_list args;

    va_start(args, fmt);
    RawDoFmt((STRPTR)fmt, args, (void (*)()) t_put_char, NULL);
    va_end(args);

    t_put('\n');
}

static VOID t_flush(VOID)
{

BPTR    out;

    out =  Output();
    if (out != (BPTR)0)
    {
        (VOID)Write(out, (APTR)t_log_buffer, (LONG)t_log_used);
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

#define T_OK(status, what)      t_check((UINT)((status) == NX_SUCCESS), (what), (ULONG)(status))
#define T_TX_OK(status, what)   t_check((UINT)((status) == TX_SUCCESS), (what), (ULONG)(status))


/* ---------------------------------------------------------- test fabric -- */

#define T_PACKET_PAYLOAD        1568        /* == AMI_POOL_PAYLOAD          */
#define T_PACKET_COUNT          32
#define T_PACKET_OVERHEAD       96

#define T_TCP_PORT              5001
#define T_UDP_PORT              5002

#define T_IP_STACK_SIZE         4096        /* IPv6 adds ND to the IP thread */
#define T_SERVER_STACK_SIZE     4096

/*
 * The IPv4 addresses are here only so nx_ip_create() has something to take;
 * nothing in this test uses them. The same NX_IP carries both families.
 */
#define T_IP0_ADDRESS           IP_ADDRESS(192, 168, 100, 1)
#define T_IP1_ADDRESS           IP_ADDRESS(192, 168, 100, 2)
#define T_NETMASK               0xFFFFFF00UL

static const char t_message[] = "AmiNetXDuo speaks IPv6 on a 68020";
static const char t_datagram[] = "one datagram over IPv6";

extern VOID _nx_ram_network_driver(NX_IP_DRIVER *driver_req_ptr);

static NX_PACKET_POOL   t_pool;
static NX_IP            t_ip0;
static NX_IP            t_ip1;
static NX_TCP_SOCKET    t_client_socket;
static NX_TCP_SOCKET    t_server_socket;
static NX_UDP_SOCKET    t_client_udp;
static NX_UDP_SOCKET    t_server_udp;

static TX_THREAD        t_server_thread;
static TX_THREAD        t_main_thread;
static TX_SEMAPHORE     t_server_done;
static TX_SEMAPHORE     t_server_ready;

static ULONG            t_pool_memory[(T_PACKET_COUNT * (T_PACKET_PAYLOAD + T_PACKET_OVERHEAD)) / sizeof(ULONG)];
static ULONG            t_ip0_stack[T_IP_STACK_SIZE / sizeof(ULONG)];
static ULONG            t_ip1_stack[T_IP_STACK_SIZE / sizeof(ULONG)];
static ULONG            t_server_stack[T_SERVER_STACK_SIZE / sizeof(ULONG)];
static ULONG            t_arp0_cache[1024 / sizeof(ULONG)];
static ULONG            t_arp1_cache[1024 / sizeof(ULONG)];

/* The two link-local addresses, filled in as each side configures itself. */
static NXD_ADDRESS      t_addr0;
static NXD_ADDRESS      t_addr1;


/* ------------------------------------------------------- address helpers -- */

static VOID t_log_addr(const char *what, const NXD_ADDRESS *a)
{
    t_log("  %s %04lx:%04lx:%04lx:%04lx:%04lx:%04lx:%04lx:%04lx", what,
          (a->nxd_ip_address.v6[0] >> 16) & 0xFFFFUL,
           a->nxd_ip_address.v6[0]        & 0xFFFFUL,
          (a->nxd_ip_address.v6[1] >> 16) & 0xFFFFUL,
           a->nxd_ip_address.v6[1]        & 0xFFFFUL,
          (a->nxd_ip_address.v6[2] >> 16) & 0xFFFFUL,
           a->nxd_ip_address.v6[2]        & 0xFFFFUL,
          (a->nxd_ip_address.v6[3] >> 16) & 0xFFFFUL,
           a->nxd_ip_address.v6[3]        & 0xFFFFUL);
}

/*
 * Configure the link-local address and wait for duplicate address detection.
 *
 * DAD sends NX_IPV6_DAD_TRANSMITS neighbour solicitations to the address's
 * own solicited-node multicast group and declares the address usable only if
 * nothing answers. Until then the address is TENTATIVE and cannot be a
 * source, so a send issued too early either picks another address or fails.
 * The wait rules that race out.
 */
static UINT t_bring_up_ipv6(NX_IP *ip, NXD_ADDRESS *out, const char *who)
{

UINT    status;
UINT    index =  0;
ULONG   waited = 0;
ULONG   prefix = 0;
UINT    if_index = 0;

    status =  nxd_ipv6_enable(ip);
    if (!T_OK(status, "ipv6 enable"))
    {
        return(status);
    }

    /* nxd_icmp_enable, not nx_icmp_enable: only the dual-stack form installs
       _nx_icmpv6_packet_process and clears the neighbour cache. */
    status =  nxd_icmp_enable(ip);
    if (!T_OK(status, "icmpv6 enable"))
    {
        return(status);
    }

    /* NULL address with prefix length 10 == "derive fe80::/64 from the MAC". */
    status =  nxd_ipv6_address_set(ip, 0, NX_NULL, 10, &index);
    if (!T_OK(status, "link-local address set"))
    {
        return(status);
    }

    /*
     * The address starts TENTATIVE and becomes usable when DAD finishes.
     * The terminal state is VALID, not PREFERRED: PREFERRED belongs to an
     * address carrying a preferred lifetime, which stateless autoconfiguration
     * produces from a router advertisement's prefix option. A link-local or
     * manually configured address has no lifetime and lands in VALID
     * (nxd_ipv6_address_set.c). Both are usable as a source; only TENTATIVE
     * is not.
     */
    while (waited < (10UL * NX_IP_PERIODIC_RATE))
    {
        UCHAR state =  ip -> nx_ipv6_address[index].nxd_ipv6_address_state;

        if (state == NX_IPV6_ADDR_STATE_PREFERRED ||
            state == NX_IPV6_ADDR_STATE_VALID)
        {
            break;
        }
        tx_thread_sleep(NX_IP_PERIODIC_RATE / 5);
        waited +=  NX_IP_PERIODIC_RATE / 5;
    }

    (VOID)t_check((UINT)(ip -> nx_ipv6_address[index].nxd_ipv6_address_state ==
                             NX_IPV6_ADDR_STATE_PREFERRED ||
                         ip -> nx_ipv6_address[index].nxd_ipv6_address_state ==
                             NX_IPV6_ADDR_STATE_VALID),
                  "duplicate address detection passed",
                  (ULONG)ip -> nx_ipv6_address[index].nxd_ipv6_address_state);

    status =  nxd_ipv6_address_get(ip, index, out, &prefix, &if_index);
    if (!T_OK(status, "link-local address read back"))
    {
        return(status);
    }

    t_log_addr(who, out);

    /* fe80::/10 with the universal/local bit inverted in the EUI-64, as
       RFC 4291 requires and as a peer will look for. */
    (VOID)t_check((UINT)((out -> nxd_ip_address.v6[0] & 0xFFC00000UL) ==
                         0xFE800000UL),
                  "address is inside fe80::/10", out -> nxd_ip_address.v6[0]);
    (VOID)t_check((UINT)(out -> nxd_ip_address.v6[1] == 0UL),
                  "link-local subnet id is zero", out -> nxd_ip_address.v6[1]);
    /*
     * The interface identifier is the MAC with 0xFFFE inserted in the middle
     * and the universal/local bit (bit 1 of the first byte) inverted,
     * RFC 4291 appendix A. The RAM driver's MACs are 00:11:22:33:44:56 and
     * ...:57, so the identifier must read 0211:22ff:fe33:4456: the ff is the
     * low byte of word 2, the fe is the high byte of word 3, and the leading
     * 00 has become 02.
     */
    (VOID)t_check((UINT)((out -> nxd_ip_address.v6[2] & 0x000000FFUL) == 0x000000FFUL &&
                         (out -> nxd_ip_address.v6[3] & 0xFF000000UL) == 0xFE000000UL),
                  "interface id carries the EUI-64 fffe",
                  out -> nxd_ip_address.v6[2]);
    (VOID)t_check((UINT)((out -> nxd_ip_address.v6[2] & 0x02000000UL) != 0UL),
                  "universal/local bit inverted",
                  out -> nxd_ip_address.v6[2]);

    return(NX_SUCCESS);
}


/* --------------------------------------------------------- server half --- */

static VOID t_server_entry(ULONG id)
{

UINT        status;
NX_PACKET  *packet_ptr;
NXD_ADDRESS peer;
ULONG       length;
ULONG       actual;
UINT        peer_port;
CHAR        buffer[80];

    (VOID)id;

    t_log("server: thread entry (ThreadX-created Exec Task)");

    status =  nx_ip_status_check(&t_ip1, NX_IP_INITIALIZE_DONE, &actual,
                                 5UL * NX_IP_PERIODIC_RATE);
    (VOID)T_OK(status, "server: ip1 initialised");

    (VOID)t_bring_up_ipv6(&t_ip1, &t_addr1, "server: link-local");

    /* The client cannot address us until t_addr1 exists. */
    (VOID)tx_semaphore_put(&t_server_ready);

    /* ---- TCP ---------------------------------------------------------- */

    status =  nx_tcp_socket_create(&t_ip1, &t_server_socket, "server socket",
                                   NX_IP_NORMAL, NX_FRAGMENT_OKAY,
                                   NX_IP_TIME_TO_LIVE, 2048, NX_NULL, NX_NULL);
    (VOID)T_OK(status, "server: tcp socket create");

    status =  nx_tcp_server_socket_listen(&t_ip1, T_TCP_PORT, &t_server_socket,
                                          5, NX_NULL);
    (VOID)T_OK(status, "server: listen");

    status =  nx_tcp_server_socket_accept(&t_server_socket,
                                          20UL * NX_IP_PERIODIC_RATE);
    (VOID)T_OK(status, "server: accept over IPv6");

    if (status == NX_SUCCESS)
    {
        /*
         * The peer must come back as an IPv6 address. nx_tcp_socket_peer_
         * info_get(), the v4-only entry point, reports zero here, which
         * is why bsdsocket.library's accept() uses the nxd_ form.
         */
        peer.nxd_ip_version =  0;
        status =  nxd_tcp_socket_peer_info_get(&t_server_socket, &peer,
                                               &actual);
        (VOID)T_OK(status, "server: peer info");
        (VOID)t_check((UINT)(peer.nxd_ip_version == NX_IP_VERSION_V6),
                      "server: peer is IPv6", peer.nxd_ip_version);
        (VOID)t_check((UINT)(peer.nxd_ip_address.v6[3] ==
                             t_addr0.nxd_ip_address.v6[3]),
                      "server: peer is the client's link-local",
                      peer.nxd_ip_address.v6[3]);
    }

    packet_ptr =  NX_NULL;
    status =  nx_tcp_socket_receive(&t_server_socket, &packet_ptr,
                                    10UL * NX_IP_PERIODIC_RATE);
    if (T_OK(status, "server: receive over IPv6"))
    {
        length =  0;
        (VOID)nx_packet_length_get(packet_ptr, &length);
        (VOID)t_check((UINT)(length == (ULONG)sizeof(t_message)),
                      "server: payload length", length);

        actual =  0;
        (VOID)nx_packet_data_retrieve(packet_ptr, buffer, &actual);
        buffer[sizeof(buffer) - 1] =  '\0';
        t_log("server: got \"%s\" (%ld bytes)", buffer, actual);

        status =  nx_tcp_socket_send(&t_server_socket, packet_ptr,
                                     5UL * NX_IP_PERIODIC_RATE);
        if (!T_OK(status, "server: echo send"))
        {
            (VOID)nx_packet_release(packet_ptr);
        }
    }

    status =  nx_tcp_socket_disconnect(&t_server_socket,
                                       5UL * NX_IP_PERIODIC_RATE);
    (VOID)T_OK(status, "server: disconnect");

    (VOID)nx_tcp_server_socket_unaccept(&t_server_socket);
    (VOID)nx_tcp_server_socket_unlisten(&t_ip1, T_TCP_PORT);
    (VOID)nx_tcp_socket_delete(&t_server_socket);

    /* ---- UDP ---------------------------------------------------------- */

    status =  nx_udp_socket_create(&t_ip1, &t_server_udp, "server udp",
                                   NX_IP_NORMAL, NX_FRAGMENT_OKAY,
                                   NX_IP_TIME_TO_LIVE, 8);
    (VOID)T_OK(status, "server: udp socket create");

    status =  nx_udp_socket_bind(&t_server_udp, T_UDP_PORT,
                                 5UL * NX_IP_PERIODIC_RATE);
    (VOID)T_OK(status, "server: udp bind");

    packet_ptr =  NX_NULL;
    status =  nx_udp_socket_receive(&t_server_udp, &packet_ptr,
                                    15UL * NX_IP_PERIODIC_RATE);
    if (T_OK(status, "server: udp receive over IPv6"))
    {
        peer.nxd_ip_version =  0;
        peer_port =  0;

        /* nxd_, not nx_: nx_udp_source_extract() cannot report an IPv6
           source and answers 0.0.0.0. */
        status =  nxd_udp_source_extract(packet_ptr, &peer, &peer_port);
        (VOID)T_OK(status, "server: udp source extract");
        (VOID)t_check((UINT)(peer.nxd_ip_version == NX_IP_VERSION_V6),
                      "server: udp source is IPv6", peer.nxd_ip_version);
        (VOID)t_check((UINT)(peer.nxd_ip_address.v6[3] ==
                             t_addr0.nxd_ip_address.v6[3]),
                      "server: udp source is the client",
                      peer.nxd_ip_address.v6[3]);

        actual =  0;
        (VOID)nx_packet_data_retrieve(packet_ptr, buffer, &actual);
        (VOID)t_check((UINT)(actual == (ULONG)sizeof(t_datagram)),
                      "server: datagram length", actual);

        /* Bounce it straight back to where it came from. */
        status =  nxd_udp_socket_send(&t_server_udp, packet_ptr, &peer,
                                      peer_port);
        if (!T_OK(status, "server: udp echo send"))
        {
            (VOID)nx_packet_release(packet_ptr);
        }
    }

    (VOID)nx_udp_socket_unbind(&t_server_udp);
    (VOID)nx_udp_socket_delete(&t_server_udp);

    t_log("server: done");

    (VOID)tx_semaphore_put(&t_server_done);
}


/* --------------------------------------------------------- client half --- */

static VOID t_ping(NXD_ADDRESS *target, const char *what)
{

UINT        status;
NX_PACKET  *response =  NX_NULL;

    status =  nxd_icmp_ping(&t_ip0, target, (CHAR *)"AmiNetXDuo", 10,
                            &response, 10UL * NX_IP_PERIODIC_RATE);

    (VOID)T_OK(status, what);

    if (status == NX_SUCCESS && response != NX_NULL)
    {
        ULONG length =  0;

        (VOID)nx_packet_length_get(response, &length);
        (VOID)t_check((UINT)(length == 10UL), "echo reply carries the payload",
                      length);
        (VOID)nx_packet_release(response);
    }
}

static UINT t_client_run(VOID)
{

UINT        status;
NX_PACKET  *packet_ptr;
NXD_ADDRESS loopback;
NXD_ADDRESS peer;
ULONG       actual;
CHAR        buffer[80];
UINT        i;

    t_log("client: running on the adopted Exec Task");

    status =  nx_ip_status_check(&t_ip0, NX_IP_INITIALIZE_DONE, &actual,
                                 5UL * NX_IP_PERIODIC_RATE);
    (VOID)T_OK(status, "client: ip0 initialised");

    (VOID)t_bring_up_ipv6(&t_ip0, &t_addr0, "client: link-local");

    /* ---- ICMPv6 over the internal loopback ------------------------------ */

    /*
     * ::1 is configured by nxd_ipv6_enable() itself, on the interface
     * nx_ip_create() always makes, so this leg works on a machine with no
     * network card present.
     */
    loopback.nxd_ip_version       =  NX_IP_VERSION_V6;
    loopback.nxd_ip_address.v6[0] =  0UL;
    loopback.nxd_ip_address.v6[1] =  0UL;
    loopback.nxd_ip_address.v6[2] =  0UL;
    loopback.nxd_ip_address.v6[3] =  1UL;

    t_ping(&loopback, "client: ICMPv6 echo to ::1");

    /* ---- wait for the far side ------------------------------------------ */

    status =  tx_semaphore_get(&t_server_ready, 20UL * NX_IP_PERIODIC_RATE);
    if (!T_TX_OK(status, "client: server configured its address"))
    {
        return(TX_FALSE);
    }

    /* ---- ICMPv6 across the link ----------------------------------------- */

    /*
     * This is the leg that proves neighbour discovery: ip0 has never seen
     * ip1's MAC, so the first echo request is queued behind a neighbour
     * solicitation to ip1's solicited-node multicast group, and only goes out
     * when the advertisement comes back.
     */
    t_ping(&t_addr1, "client: ICMPv6 echo to the peer's link-local");

    /* ---- TCP over IPv6 --------------------------------------------------- */

    status =  nx_tcp_socket_create(&t_ip0, &t_client_socket, "client socket",
                                   NX_IP_NORMAL, NX_FRAGMENT_OKAY,
                                   NX_IP_TIME_TO_LIVE, 2048, NX_NULL, NX_NULL);
    (VOID)T_OK(status, "client: tcp socket create");

    status =  nx_tcp_client_socket_bind(&t_client_socket, NX_ANY_PORT,
                                        5UL * NX_IP_PERIODIC_RATE);
    (VOID)T_OK(status, "client: bind");

    status =  nxd_tcp_client_socket_connect(&t_client_socket, &t_addr1,
                                            T_TCP_PORT,
                                            15UL * NX_IP_PERIODIC_RATE);
    if (!T_OK(status, "client: connect over IPv6"))
    {
        (VOID)nx_tcp_client_socket_unbind(&t_client_socket);
        (VOID)nx_tcp_socket_delete(&t_client_socket);
        return(TX_FALSE);
    }

    status =  nx_packet_allocate(&t_pool, &packet_ptr, NX_IPv6_TCP_PACKET,
                                 5UL * NX_IP_PERIODIC_RATE);
    (VOID)T_OK(status, "client: packet allocate (IPv6 headroom)");

    if (status == NX_SUCCESS)
    {
        status =  nx_packet_data_append(packet_ptr, (VOID *)t_message,
                                        (ULONG)sizeof(t_message), &t_pool,
                                        5UL * NX_IP_PERIODIC_RATE);
        (VOID)T_OK(status, "client: data append");

        status =  nx_tcp_socket_send(&t_client_socket, packet_ptr,
                                     5UL * NX_IP_PERIODIC_RATE);
        if (!T_OK(status, "client: send over IPv6"))
        {
            (VOID)nx_packet_release(packet_ptr);
        }
    }

    packet_ptr =  NX_NULL;
    status =  nx_tcp_socket_receive(&t_client_socket, &packet_ptr,
                                    10UL * NX_IP_PERIODIC_RATE);
    if (T_OK(status, "client: receive echo over IPv6"))
    {
        actual =  0;
        for (i = 0; i < (UINT)sizeof(buffer); i++)
        {
            buffer[i] =  (CHAR)0;
        }
        (VOID)nx_packet_data_retrieve(packet_ptr, buffer, &actual);

        (VOID)t_check((UINT)(actual == (ULONG)sizeof(t_message)),
                      "client: echo length", actual);

        status =  NX_SUCCESS;
        for (i = 0; i < (UINT)sizeof(t_message); i++)
        {
            if (buffer[i] != t_message[i])
            {
                status =  (UINT)i + 1U;
                break;
            }
        }
        (VOID)t_check((UINT)(status == NX_SUCCESS), "client: echo contents",
                      (ULONG)status);

        (VOID)nx_packet_release(packet_ptr);
    }

    (VOID)nx_tcp_socket_disconnect(&t_client_socket,
                                   5UL * NX_IP_PERIODIC_RATE);
    (VOID)nx_tcp_client_socket_unbind(&t_client_socket);
    (VOID)nx_tcp_socket_delete(&t_client_socket);

    /* ---- UDP over IPv6 --------------------------------------------------- */

    status =  nx_udp_socket_create(&t_ip0, &t_client_udp, "client udp",
                                   NX_IP_NORMAL, NX_FRAGMENT_OKAY,
                                   NX_IP_TIME_TO_LIVE, 8);
    (VOID)T_OK(status, "client: udp socket create");

    status =  nx_udp_socket_bind(&t_client_udp, NX_ANY_PORT,
                                 5UL * NX_IP_PERIODIC_RATE);
    (VOID)T_OK(status, "client: udp bind");

    status =  nx_packet_allocate(&t_pool, &packet_ptr, NX_IPv6_UDP_PACKET,
                                 5UL * NX_IP_PERIODIC_RATE);
    (VOID)T_OK(status, "client: udp packet allocate");

    if (status == NX_SUCCESS)
    {
        status =  nx_packet_data_append(packet_ptr, (VOID *)t_datagram,
                                        (ULONG)sizeof(t_datagram), &t_pool,
                                        5UL * NX_IP_PERIODIC_RATE);
        (VOID)T_OK(status, "client: udp data append");

        status =  nxd_udp_socket_send(&t_client_udp, packet_ptr, &t_addr1,
                                      T_UDP_PORT);
        if (!T_OK(status, "client: udp send over IPv6"))
        {
            (VOID)nx_packet_release(packet_ptr);
        }
    }

    packet_ptr =  NX_NULL;
    status =  nx_udp_socket_receive(&t_client_udp, &packet_ptr,
                                    15UL * NX_IP_PERIODIC_RATE);
    if (T_OK(status, "client: udp echo received"))
    {
        UINT reply_port =  0;

        peer.nxd_ip_version =  0;
        (VOID)nxd_udp_source_extract(packet_ptr, &peer, &reply_port);

        (VOID)t_check((UINT)(peer.nxd_ip_version == NX_IP_VERSION_V6 &&
                             peer.nxd_ip_address.v6[3] ==
                             t_addr1.nxd_ip_address.v6[3]),
                      "client: udp echo came from the server",
                      peer.nxd_ip_address.v6[3]);

        actual =  0;
        for (i = 0; i < (UINT)sizeof(buffer); i++)
        {
            buffer[i] =  (CHAR)0;
        }
        (VOID)nx_packet_data_retrieve(packet_ptr, buffer, &actual);
        (VOID)t_check((UINT)(actual == (ULONG)sizeof(t_datagram)),
                      "client: udp echo length", actual);

        status =  NX_SUCCESS;
        for (i = 0; i < (UINT)sizeof(t_datagram); i++)
        {
            if (buffer[i] != t_datagram[i])
            {
                status =  (UINT)i + 1U;
                break;
            }
        }
        (VOID)t_check((UINT)(status == NX_SUCCESS), "client: udp echo contents",
                      (ULONG)status);

        (VOID)nx_packet_release(packet_ptr);
    }

    (VOID)nx_udp_socket_unbind(&t_client_udp);
    (VOID)nx_udp_socket_delete(&t_client_udp);

    /* ---- neighbour cache ------------------------------------------------- */

    {
        NXD_ADDRESS lookup =  t_addr1;
        ULONG       msw = 0, lsw = 0;
        UINT        if_index =  0;

        /* interface_index is not optional, the error-checking wrapper
           returns NX_PTR_ERROR (0x07) for a NULL, unlike most NetX Duo
           out-parameters. */
        status =  nxd_nd_cache_hardware_address_find(&t_ip0, &lookup,
                                                     &msw, &lsw, &if_index);
        (VOID)T_OK(status, "client: peer is in the neighbour cache");
        (VOID)t_check((UINT)((msw | lsw) != 0UL),
                      "client: cached MAC is not empty", lsw);
    }

    status =  tx_semaphore_get(&t_server_done, 20UL * NX_IP_PERIODIC_RATE);
    (VOID)T_TX_OK(status, "client: server completed");

    t_log("client: done");

    return(TX_TRUE);
}


/* ------------------------------------------------------ ThreadX startup --- */

VOID tx_application_define(VOID *first_unused_memory)
{

UINT    status;

    (VOID)first_unused_memory;

    t_log("define: nx_system_initialize");
    nx_system_initialize();

    status =  nx_packet_pool_create(&t_pool, "AmiNetXDuo pool",
                                    T_PACKET_PAYLOAD,
                                    (VOID *)t_pool_memory,
                                    (ULONG)sizeof(t_pool_memory));
    (VOID)T_OK(status, "define: packet pool");

    status =  nx_ip_create(&t_ip0, "ip0", T_IP0_ADDRESS, T_NETMASK, &t_pool,
                           _nx_ram_network_driver,
                           (VOID *)t_ip0_stack, (ULONG)sizeof(t_ip0_stack), 1);
    (VOID)T_OK(status, "define: ip0 create");

    status =  nx_ip_create(&t_ip1, "ip1", T_IP1_ADDRESS, T_NETMASK, &t_pool,
                           _nx_ram_network_driver,
                           (VOID *)t_ip1_stack, (ULONG)sizeof(t_ip1_stack), 1);
    (VOID)T_OK(status, "define: ip1 create");

    status =  nx_arp_enable(&t_ip0, (VOID *)t_arp0_cache, (ULONG)sizeof(t_arp0_cache));
    (VOID)T_OK(status, "define: ip0 arp");

    status =  nx_arp_enable(&t_ip1, (VOID *)t_arp1_cache, (ULONG)sizeof(t_arp1_cache));
    (VOID)T_OK(status, "define: ip1 arp");

    status =  nx_tcp_enable(&t_ip0);
    (VOID)T_OK(status, "define: ip0 tcp");

    status =  nx_tcp_enable(&t_ip1);
    (VOID)T_OK(status, "define: ip1 tcp");

    status =  nx_udp_enable(&t_ip0);
    (VOID)T_OK(status, "define: ip0 udp");

    status =  nx_udp_enable(&t_ip1);
    (VOID)T_OK(status, "define: ip1 udp");

    status =  tx_semaphore_create(&t_server_done, "server done", 0);
    (VOID)T_TX_OK(status, "define: done semaphore");

    status =  tx_semaphore_create(&t_server_ready, "server ready", 0);
    (VOID)T_TX_OK(status, "define: ready semaphore");

    status =  tx_thread_create(&t_server_thread, "server", t_server_entry, 0UL,
                               (VOID *)t_server_stack, (ULONG)sizeof(t_server_stack),
                               16, 16, TX_NO_TIME_SLICE, TX_AUTO_START);
    (VOID)T_TX_OK(status, "define: server thread");
}


/* ------------------------------------------------------------------ main -- */

int main(void)
{

UINT    status;

    t_log("AmiNetXDuo, IPv6 dual stack over the RAM driver");
    t_log("  ticks/sec %ld, pool %ld x %ld bytes, NX_IP %ld bytes",
          (ULONG)TX_TIMER_TICKS_PER_SECOND, (ULONG)T_PACKET_COUNT,
          (ULONG)T_PACKET_PAYLOAD, (ULONG)sizeof(NX_IP));

    status =  tx_amiga_kernel_start();
    if (status != TX_SUCCESS)
    {
        t_log("FATAL: tx_amiga_kernel_start() = %ld", (ULONG)status);
        t_flush();
        return(20);
    }
    t_log("kernel: scheduler running");

    status =  tx_amiga_adopt_thread(&t_main_thread, "ipv6 client", 16);
    if (!T_TX_OK(status, "main: adopted this Exec Task"))
    {
        t_flush();
        return(20);
    }

    (VOID)t_client_run();

    status =  tx_amiga_orphan_thread(&t_main_thread);
    (VOID)T_TX_OK(status, "main: orphaned this Exec Task");

    t_log("");
    t_log("%ld checks, %ld failures, %s",
          t_checks, t_failures, (t_failures == 0UL) ? "PASS" : "FAIL");

    t_flush();

    return((t_failures == 0UL) ? 0 : 20);
}

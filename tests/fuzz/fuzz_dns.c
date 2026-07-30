/*
 * AmiNetXDuo -- host fuzz driver for the DNS response parser.
 *
 * What is under test is the code that reads a datagram from whatever answered
 * a query.  In this stack that is addons/dns/nxd_dns.c: src/netstack/
 * netstack_dns.c holds the policy (hosts file first, .local to mDNS, which
 * status means which error) and calls nx_dns_host_by_name_get(),
 * nx_dns_host_by_address_get() and nxd_dns_ipv6_address_by_name_get() to do
 * the parsing.  SECURITY.md named netstack_dns.c as the parser; it is not one,
 * and fuzzing it would have proved nothing.  So the vendored parser is
 * compiled here and driven through those three entry points -- the three the
 * library exposes -- with the response supplied a datagram at a time.
 *
 * The wire is one stub.  _nxe_udp_socket_receive() below hands the parser the
 * fuzz bytes as a real NX_PACKET off a real packet pool; everything above it,
 * from nx_dns_host_by_name_get() down through _nx_dns_response_receive(),
 * _nx_dns_response_process(), the name unencoder, the resource walk and the
 * answer cache, is the shipping code compiled from third_party.  Nothing here
 * reimplements any of it.
 *
 * Type widths are the target's: tests/perf/host/shim's tx_port.h gives a
 * 32-bit ULONG, so the length and TTL arithmetic is the arithmetic the m68k
 * does.  Pointers are the host's 64 bits either way, so a name walk that
 * would wrap a 32-bit pointer cannot be reproduced on this host at all.
 *
 * Usage:
 *   fuzz_dns -s                  every seed case, named
 *   fuzz_dns -c NAME             one seed case by name
 *   fuzz_dns < datagram          one datagram from stdin
 *   fuzz_dns -r SEED COUNT       seeds plus mutations, no corpus needed
 *
 * SPDX-License-Identifier: MIT
 */

#include "nx_api.h"
#include "nx_ip.h"
#include "nx_udp.h"
#include "nx_packet.h"
#include "nxd_dns.h"

#include "fuzz_wire.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The name every query asks for, so a seed can match the question section. */
#define FZ_QNAME        "test.example.com"
#define FZ_SERVER       IP_ADDRESS(10, 0, 0, 1)
#define FZ_OUR_IP       IP_ADDRESS(10, 0, 0, 17)
#define FZ_NETMASK      IP_ADDRESS(255, 255, 255, 0)

static NX_IP            fz_ip;
static NX_DNS           fz_dns;

/* A pool of its own for the datagrams that arrive: the DNS client's internal
   pool carries 512-byte payloads and a sender is not held to that. */
#define FZ_WIRE_PAYLOAD (FZW_MAX + 64)
#define FZ_WIRE_PACKETS 8
static ULONG            fz_wire_area[((FZ_WIRE_PAYLOAD + sizeof(NX_PACKET) +
                                      32) * FZ_WIRE_PACKETS) / sizeof(ULONG)];
static NX_PACKET_POOL   fz_wire_pool;

/* Big enough that the cache path runs rather than failing to insert. */
static ULONG            fz_cache[512];

/* The datagram under test, and whether it has been handed over yet. */
static FzwBuf           fz_case;
static int              fz_delivered;
static int              fz_patch_id;    /* make the header ID match the query */

static unsigned long    fz_cases;
static const char      *fz_case_name = "stdin";


/* ----------------------------------------------------------- ThreadX ----- */

static TX_THREAD        fz_caller_thread;

TX_THREAD              *_tx_thread_current_ptr = &fz_caller_thread;
TX_THREAD               _tx_timer_thread;
UINT                    _tx_thread_preempt_disable;
volatile ULONG          _tx_thread_system_state;

UINT _tx_thread_interrupt_disable(VOID) { return 0; }

VOID _tx_thread_interrupt_restore(UINT previous_posture)
{
    NX_PARAMETER_NOT_USED(previous_posture);
}

UINT _tx_thread_sleep(ULONG timer_ticks)
{
    NX_PARAMETER_NOT_USED(timer_ticks);
    return TX_SUCCESS;
}

TX_THREAD *_tx_thread_identify(VOID) { return _tx_thread_current_ptr; }

/* The mutex, through the checking wrappers the addon actually calls: only
   NetX Duo's error checking is disabled inside nxd_dns.c, not ThreadX's. */
UINT _txe_mutex_create(TX_MUTEX *m, CHAR *name, UINT inherit, UINT size)
{
    NX_PARAMETER_NOT_USED(m);
    NX_PARAMETER_NOT_USED(name);
    NX_PARAMETER_NOT_USED(inherit);
    NX_PARAMETER_NOT_USED(size);
    return TX_SUCCESS;
}

UINT _txe_mutex_delete(TX_MUTEX *m)
{
    NX_PARAMETER_NOT_USED(m);
    return TX_SUCCESS;
}

UINT _txe_mutex_get(TX_MUTEX *m, ULONG wait_option)
{
    NX_PARAMETER_NOT_USED(m);
    NX_PARAMETER_NOT_USED(wait_option);
    return TX_SUCCESS;
}

UINT _txe_mutex_put(TX_MUTEX *m)
{
    NX_PARAMETER_NOT_USED(m);
    return TX_SUCCESS;
}

VOID _tx_thread_system_suspend(TX_THREAD *t) { NX_PARAMETER_NOT_USED(t); }
VOID _tx_thread_system_resume(TX_THREAD *t)  { NX_PARAMETER_NOT_USED(t); }
VOID _tx_thread_system_preempt_check(VOID)   { }

UINT _tx_thread_preemption_change(TX_THREAD *t, UINT new_threshold,
                                  UINT *old_threshold)
{
    NX_PARAMETER_NOT_USED(t);
    NX_PARAMETER_NOT_USED(new_threshold);

    if (old_threshold != NX_NULL)
        *old_threshold = 0;

    return TX_SUCCESS;
}

UINT _tx_event_flags_set(TX_EVENT_FLAGS_GROUP *g, ULONG flags, UINT option)
{
    NX_PARAMETER_NOT_USED(g);
    NX_PARAMETER_NOT_USED(flags);
    NX_PARAMETER_NOT_USED(option);
    return TX_SUCCESS;
}

/*
 * Time has to advance.  _nx_dns_response_receive() decides when to give up by
 * subtracting tx_time_get() from the time it started, so a clock that stands
 * still turns "no more packets" into an endless loop.
 */
static ULONG fz_ticks;

ULONG _tx_time_get(VOID)
{
    return fz_ticks++;
}


/* --------------------------------------------------------------- UDP ----- */

/*
 * The socket, in as much detail as the DNS client can tell.  It never looks at
 * the port table or the interface list, so these are counters and a queue of
 * one.  The internal names, not the _nxe_ ones: nxd_dns.c defines
 * NX_DISABLE_ERROR_CHECKING for itself, so it calls straight through.
 */

UINT _nx_udp_socket_create(NX_IP *ip_ptr, NX_UDP_SOCKET *socket_ptr,
                           CHAR *name, ULONG type_of_service, ULONG fragment,
                           UINT time_to_live, ULONG queue_maximum)
{
    NX_PARAMETER_NOT_USED(name);
    NX_PARAMETER_NOT_USED(type_of_service);
    NX_PARAMETER_NOT_USED(fragment);
    NX_PARAMETER_NOT_USED(time_to_live);
    NX_PARAMETER_NOT_USED(queue_maximum);

    memset(socket_ptr, 0, sizeof(*socket_ptr));
    socket_ptr -> nx_udp_socket_ip_ptr = ip_ptr;
    socket_ptr -> nx_udp_socket_id     = NX_UDP_ID;

    return NX_SUCCESS;
}

UINT _nx_udp_socket_delete(NX_UDP_SOCKET *socket_ptr)
{
    socket_ptr -> nx_udp_socket_id = 0;
    return NX_SUCCESS;
}

UINT _nx_udp_socket_bind(NX_UDP_SOCKET *socket_ptr, UINT port,
                         ULONG wait_option)
{
    NX_PARAMETER_NOT_USED(wait_option);

    socket_ptr -> nx_udp_socket_port       = (port == NX_ANY_PORT) ? 49152 : port;
    socket_ptr -> nx_udp_socket_bound_next = socket_ptr;

    return NX_SUCCESS;
}

UINT _nx_udp_socket_unbind(NX_UDP_SOCKET *socket_ptr)
{
    socket_ptr -> nx_udp_socket_bound_next = NX_NULL;
    return NX_SUCCESS;
}

/* The query goes nowhere.  It still has to be released, or a leaked query
   packet would be reported as a leak in the parser. */
UINT _nxd_udp_socket_send(NX_UDP_SOCKET *socket_ptr, NX_PACKET *packet_ptr,
                          NXD_ADDRESS *ip_address, UINT port)
{
    NX_PARAMETER_NOT_USED(socket_ptr);
    NX_PARAMETER_NOT_USED(ip_address);
    NX_PARAMETER_NOT_USED(port);

    _nx_packet_release(packet_ptr);

    return NX_SUCCESS;
}

/*
 * The wire.  One datagram per query, then nothing -- which is what a resolver
 * sees when a single hostile answer arrives and the real server never
 * replies.
 */
UINT _nx_udp_socket_receive(NX_UDP_SOCKET *socket_ptr, NX_PACKET **packet_ptr,
                            ULONG wait_option)
{
    NX_PACKET *p;
    UINT       status;

    NX_PARAMETER_NOT_USED(socket_ptr);
    NX_PARAMETER_NOT_USED(wait_option);

    if (fz_delivered)
        return NX_NO_PACKET;

    status = _nx_packet_allocate(&fz_wire_pool, &p, NX_UDP_PACKET, NX_NO_WAIT);
    if (status != NX_SUCCESS)
        return NX_NO_PACKET;

    if (fz_case.len > 0)
    {
        memcpy(p -> nx_packet_prepend_ptr, fz_case.b, fz_case.len);

        /* The query ID.  Patching it is what gets the datagram past the two ID
           gates and into the record loop; leaving it alone exercises the drop
           path, which is the other thing a hostile answer meets. */
        if (fz_patch_id && fz_case.len >= 2)
        {
            USHORT id = fz_dns.nx_dns_transmit_id;

            p -> nx_packet_prepend_ptr[0] = (UCHAR)(id >> 8);
            p -> nx_packet_prepend_ptr[1] = (UCHAR)id;
        }
    }

    p -> nx_packet_append_ptr    = p -> nx_packet_prepend_ptr + fz_case.len;
    p -> nx_packet_length        = (ULONG)fz_case.len;
    p -> nx_packet_ip_version    = NX_IP_VERSION_V4;
    p -> nx_packet_ip_interface  = &fz_ip.nx_ip_interface[0];
    p -> nx_packet_next          = NX_NULL;
    p -> nx_packet_last          = NX_NULL;

    fz_delivered = 1;
    *packet_ptr  = p;

    return NX_SUCCESS;
}

VOID _nx_ip_packet_deferred_receive(NX_IP *ip_ptr, NX_PACKET *packet_ptr)
{
    NX_PARAMETER_NOT_USED(ip_ptr);
    _nx_packet_release(packet_ptr);
}


/* --------------------------------------------------------- the drives ---- */

static void fz_fail(const char *what)
{
    printf("fuzz_dns: %s (case '%s', %lu cases in)\n", what, fz_case_name,
           fz_cases);
    fflush(stdout);
    abort();
}

static void fz_pool_check(void)
{
    if (fz_wire_pool.nx_packet_pool_available !=
        fz_wire_pool.nx_packet_pool_total)
        fz_fail("a received datagram was not released");

    if (fz_dns.nx_dns_pool.nx_packet_pool_available !=
        fz_dns.nx_dns_pool.nx_packet_pool_total)
        fz_fail("a query packet was not released");
}

/*
 * One datagram, through everything the library exposes.  Each entry point is
 * asked twice: the second call may be answered from the DNS cache, which is
 * the other consumer of whatever the first parse decided to store.
 */
static void fz_run_once(const unsigned char *data, size_t len)
{
    UCHAR record[256];
    UCHAR name[NX_DNS_NAME_MAX + 1];
    ULONG address;
    UINT  count;
    int   pass;

    if (len > FZW_MAX)
        len = FZW_MAX;

    memcpy(fz_case.b, data, len);
    fz_case.len = len;

    memset(fz_cache, 0, sizeof(fz_cache));

    if (nx_dns_create(&fz_dns, &fz_ip, (UCHAR *)"example.com") != NX_SUCCESS)
        fz_fail("nx_dns_create failed");

    (VOID)nx_dns_cache_initialize(&fz_dns, fz_cache, (UINT)sizeof(fz_cache));

    if (nx_dns_server_add(&fz_dns, FZ_SERVER) != NX_SUCCESS)
        fz_fail("nx_dns_server_add failed");

    /* One try per server: the retry loop would only re-deliver the same
       datagram, and every case pays for it. */
    fz_dns.nx_dns_retries = 1;

    for (pass = 0; pass < 2; pass++)
    {
        fz_delivered = 0;
        address      = 0;
        (VOID)nx_dns_host_by_name_get(&fz_dns, (UCHAR *)FZ_QNAME, &address, 4);
    }

    for (pass = 0; pass < 2; pass++)
    {
        fz_delivered = 0;
        memset(name, 0, sizeof(name));
        (VOID)nx_dns_host_by_address_get(&fz_dns, IP_ADDRESS(10, 0, 0, 9),
                                         name, (UINT)sizeof(name), 4);

        /* A parser that reports a name must report a terminated one. */
        if (name[sizeof(name) - 1] != 0)
            fz_fail("the reverse lookup wrote past the name buffer's last byte");
    }

#ifdef FEATURE_NX_IPV6
    for (pass = 0; pass < 2; pass++)
    {
        fz_delivered = 0;
        count        = 0;
        memset(record, 0, sizeof(record));
        (VOID)nxd_dns_ipv6_address_by_name_get(&fz_dns, (UCHAR *)FZ_QNAME,
                                               record, (UINT)sizeof(record),
                                               &count, 4);
    }
#else
    NX_PARAMETER_NOT_USED(record);
    NX_PARAMETER_NOT_USED(count);
#endif

    fz_pool_check();

    if (nx_dns_delete(&fz_dns) != NX_SUCCESS)
        fz_fail("nx_dns_delete failed");

    fz_cases++;
}

/*
 * The harness has to be shown to reach the parser, or a sweep of malformed
 * input proves only that the datagrams were thrown away.  Three well-formed
 * answers, one per entry point, and each must come back with the value the
 * datagram carries.
 */
static void fz_selftest(void)
{
    FzwBuf w;
    ULONG  address = 0;
    UCHAR  name[NX_DNS_NAME_MAX + 1];

    fz_case_name = "selftest";
    fz_patch_id  = 1;

    if (nx_dns_create(&fz_dns, &fz_ip, (UCHAR *)"example.com") != NX_SUCCESS ||
        nx_dns_server_add(&fz_dns, FZ_SERVER) != NX_SUCCESS)
        fz_fail("the DNS client would not start");

    fz_dns.nx_dns_retries = 1;

    fzw_reset(&w);
    fzs_a_answer(&w, FZ_QNAME);
    memcpy(fz_case.b, w.b, w.len);
    fz_case.len   = w.len;
    fz_delivered  = 0;

    if (nx_dns_host_by_name_get(&fz_dns, (UCHAR *)FZ_QNAME, &address, 4) !=
        NX_SUCCESS)
        fz_fail("a well-formed A answer did not resolve");
    if (address != IP_ADDRESS(10, 0, 0, 9))
        fz_fail("a well-formed A answer resolved to the wrong address");

    fzw_reset(&w);
    fzs_ptr_answer(&w, FZ_QNAME);
    memcpy(fz_case.b, w.b, w.len);
    fz_case.len  = w.len;
    fz_delivered = 0;
    memset(name, 0, sizeof(name));

    if (nx_dns_host_by_address_get(&fz_dns, IP_ADDRESS(10, 0, 0, 9), name,
                                   (UINT)sizeof(name), 4) != NX_SUCCESS)
        fz_fail("a well-formed PTR answer did not resolve");
    if (strcmp((const char *)name, "amiga.example.com") != 0)
        fz_fail("a well-formed PTR answer gave the wrong name");

#ifdef FEATURE_NX_IPV6
    {
        NX_DNS_IPV6_ADDRESS answer[2];
        UINT                count = 0;

        fzw_reset(&w);
        fzs_aaaa_answer(&w, FZ_QNAME);
        memcpy(fz_case.b, w.b, w.len);
        fz_case.len  = w.len;
        fz_delivered = 0;
        memset(answer, 0, sizeof(answer));

        if (nxd_dns_ipv6_address_by_name_get(&fz_dns, (UCHAR *)FZ_QNAME,
                                             answer, (UINT)sizeof(answer),
                                             &count, 4) != NX_SUCCESS)
            fz_fail("a well-formed AAAA answer did not resolve");
        if (count != 1 || answer[0].ipv6_address[0] != 0x00010203UL)
            fz_fail("a well-formed AAAA answer gave the wrong address");
    }
#endif

    fz_pool_check();
    (VOID)nx_dns_delete(&fz_dns);
}

static void fz_run_seed(int which, int patch_id)
{
    FzwBuf w;

    fzw_reset(&w);
    fzw_seeds[which].build(&w, FZ_QNAME);

    fz_case_name = fzw_seeds[which].name;
    fz_patch_id  = patch_id;

    fz_run_once(w.b, w.len);
}


/* --------------------------------------------------------------- setup --- */

static void fz_setup(void)
{
    NX_INTERFACE *ifp = &fz_ip.nx_ip_interface[0];

    memset(&fz_ip, 0, sizeof(fz_ip));

    ifp -> nx_interface_valid            = NX_TRUE;
    ifp -> nx_interface_name             = "eth0";
    ifp -> nx_interface_link_up          = NX_TRUE;
    ifp -> nx_interface_ip_address       = FZ_OUR_IP;
    ifp -> nx_interface_ip_network_mask  = FZ_NETMASK;
    ifp -> nx_interface_ip_network       = FZ_OUR_IP & FZ_NETMASK;
    ifp -> nx_interface_ip_mtu_size      = 1500;

    fz_ip.nx_ip_id = NX_IP_ID;

    if (_nx_packet_pool_create(&fz_wire_pool, "wire", FZ_WIRE_PAYLOAD,
                               (VOID *)fz_wire_area,
                               (ULONG)sizeof(fz_wire_area)) != NX_SUCCESS)
    {
        printf("fuzz_dns: the wire packet pool would not create\n");
        exit(1);
    }
}


int main(int argc, char **argv)
{
    int i;

    fz_setup();
    fz_selftest();

    for (i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "-s") == 0)
        {
            int s;

            for (s = 0; s < FZW_SEED_COUNT; s++)
            {
                fz_run_seed(s, 1);
                fz_run_seed(s, 0);
                printf("  %-24s ok\n", fzw_seeds[s].name);
            }

            printf("fuzz_dns: %d seed case(s), clean\n", FZW_SEED_COUNT);
            return 0;
        }

        if (strcmp(argv[i], "-c") == 0 && i + 1 < argc)
        {
            const char *want = argv[++i];
            int         s;

            for (s = 0; s < FZW_SEED_COUNT; s++)
            {
                if (strcmp(fzw_seeds[s].name, want) == 0)
                {
                    fz_run_seed(s, 1);
                    fz_run_seed(s, 0);
                    printf("fuzz_dns: seed '%s', clean\n", want);
                    return 0;
                }
            }

            printf("fuzz_dns: no seed case named '%s'\n", want);
            return 2;
        }

        if (strcmp(argv[i], "-r") == 0 && i + 2 < argc)
        {
            unsigned long seed  = strtoul(argv[++i], NULL, 0);
            unsigned long count = strtoul(argv[++i], NULL, 0);
            unsigned long n;
            FzwBuf        w;
            int           s;

            /* The seeds first, always: a mutation sweep that never runs the
               known shapes is a sweep whose coverage nobody can state. */
            for (s = 0; s < FZW_SEED_COUNT; s++)
            {
                fz_run_seed(s, 1);
                fz_run_seed(s, 0);
            }

            fzw_state = seed;

            for (n = 0; n < count; n++)
            {
                s = (int)fzw_below((unsigned)FZW_SEED_COUNT);

                fzw_reset(&w);
                fzw_seeds[s].build(&w, FZ_QNAME);
                fzw_mutate(&w);

                fz_case_name = fzw_seeds[s].name;
                fz_patch_id  = (fzw_below(8) != 0);

                fz_run_once(w.b, w.len);
            }

            printf("fuzz_dns: %d seed(s) + %lu mutation(s) from seed %lu, "
                   "%lu datagram(s) parsed, clean\n", FZW_SEED_COUNT, count,
                   seed, fz_cases);
            return 0;
        }
    }

    {
        static unsigned char buf[FZW_MAX];
        size_t               len = fread(buf, 1, sizeof(buf), stdin);

        fz_patch_id = 1;
        fz_run_once(buf, len);
        fz_patch_id = 0;
        fz_run_once(buf, len);
    }

    printf("fuzz_dns: one datagram, clean\n");

    return 0;
}

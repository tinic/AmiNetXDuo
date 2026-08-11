/*
 * AmiNetXDuo, netstack singleton internals.
 *
 * Private to src/netstack/. Include order matters: tx_api.h and nx_api.h come
 * before any exec header, because <exec/types.h> turns VOID into a macro and
 * that breaks the ThreadX typedefs.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_NETSTACK_INTERNAL_H
#define AMINETXDUO_NETSTACK_INTERNAL_H

#include "tx_api.h"
#include "nx_api.h"

#include "nxd_dhcp_client.h"
#include "nxd_dns.h"
#include "nx_auto_ip.h"
#ifdef AMINETXDUO_MDNS
#include "nxd_mdns.h"
#endif

#include <exec/types.h>

#include "aminetxduo/netstack.h"
#include "aminetxduo/config.h"
#include "aminetxduo/compat.h"
#include "aminetxduo/sana2.h"

/* ------------------------------------------------------------- tunables, */

#include "../thread_priorities.h"

#define AMI_IP_STACK_SIZE           4096
#define AMI_ARP_CACHE_SIZE          1024

/*
 * 4096 and not the 2048 NetX Duo's own samples use, because the AutoIP thread
 * on this port is not the one those samples describe.  -fstack-usage over the
 * m68000 build puts the deepest chain from _nx_auto_ip_thread_entry at 704
 * bytes, and it gets there through _nx_ip_interface_status_check into
 * ami_sana2_driver_entry, so this thread reaches a SANA-II device and makes
 * exec DoIO/SendIO calls, which run on whatever stack is current and are not
 * in that 704.  1,344 bytes of headroom for an Exec device call, on a machine
 * with no guard page and no fault, is not a margin worth defending; the other
 * 2 KB is.  docs/ALIGNMENT.md has the measurements.
 */
#define AMI_AUTOIP_STACK_SIZE       4096

/* Fraction of AvailMem() the packet pool may claim (1/AMI_POOL_MEM_DIVISOR). */
#define AMI_POOL_MEM_DIVISOR        16

#ifdef AMINETXDUO_IPV6
/* Recursive DNS servers held from router advertisements; see ns_Rdnss. */
#define AMI_RDNSS_MAX               4

/*
 * One advertisement's worth of RFC 8106 5.2 search domains, as they arrive:
 * the encoded label sequences, not the names.  AMI_CFG_MAX_SEARCH is 6 and
 * AMI_CFG_NAME_LEN is 64, so a list this buffer cannot hold is longer than the
 * list it feeds, and the decoder stops at the first name it cannot store
 * either way.
 */
#define AMI_DNSSL_MAX               256
#endif

/*
 * DNS answer cache size.
 *
 * NetX Duo's cache is one buffer with resource records growing up from the
 * bottom and the strings they name growing down from the top.  On this target
 * an NX_DNS_RR is 20 bytes and a name costs ((len & ~3) + 8) in the string
 * table, so a cached A record for a 15-character host name such as
 * "www.example.com" is 20 + 20 = 40 bytes; eight bytes of the buffer are the
 * two end pointers.  2048 bytes is therefore about fifty cached names.
 *
 * Real workloads are far smaller: a shell session resolves one host, `fetch`
 * following redirects two or three, the tests/curl suite one peer.  The
 * largest consumer is the reverse lookups ShowNetStatus NAMES and netstat do,
 * one per peer address on screen, bounded by TOOL_MAX_SOCK (32).
 *
 * Undersizing costs only a DNS query, since NetX Duo replaces the least
 * recently used record rather than failing; oversizing costs resident memory
 * forever.  2048 bytes is 0.05% of the 4 MB floor target, and a fifth of the
 * 9,792-byte packet pool the DNS client already carries inside the same
 * NX_DNS (10,112 bytes) for its own queries, measured on this toolchain.
 */
#define AMI_DNS_CACHE_BYTES         2048

#ifdef AMINETXDUO_MDNS
/*
 * The two mDNS cache sizes.
 *
 * Same layout as the DNS answer cache above: fixed records growing up from the
 * bottom of one buffer, the strings they name growing down from the top, the
 * two end pointers in between.  An NX_MDNS_RR is larger than an NX_DNS_RR,
 * it carries the record's state machine, its retransmit counters and its
 * interface index as well as the data, so the arithmetic differs.
 *
 * The local cache holds what this machine claims: the A record for
 * <host>.local per interface, plus four records per declared service, an
 * SRV, a TXT and two PTRs, one of them the _services._dns-sd._udp enumeration
 * and the names they point at.  An NX_MDNS_RR is 56 bytes and the names run
 * to about 90 more, so 384 per service covers it with room over; the base
 * kilobyte is the host's own name.  If it will not hold our own name, the
 * machine has no name; if it will not hold a service, that service is not
 * advertised and ami_ns_mdns_services() says which.
 *
 * The peer cache holds what has been learnt; full means the oldest record is
 * evicted, nothing fails.  Two workloads land here.  Name resolution is the
 * small one, one or two names per shell session, reverse lookups behind
 * `netstat` and ShowNetStatus NAMES bounded by TOOL_MAX_SOCK (32), and
 * undersizing costs only a query on the wire.
 *
 * Browsing is the one that sizes it.  ShowNetServices holds a continuous query
 * open (itself a record here) and every instance it hears about arrives as
 * four, PTR, SRV, TXT and A, plus the names they point at, around 400
 * bytes together.
 *
 * 32 KB, and it is measured rather than chosen.  A browse of one ordinary
 * house LAN, five cameras, two printers, a 3D printer, an amplifier, a Hue
 * bridge, several Macs, turns up over twenty service types and more than
 * thirty instances.  At 2 KB most of them were evicted before the window
 * closed; at 8 KB they were listed but two thirds of them printed "no address",
 * because the PTR and SRV records survived and the A record they pointed at
 * had been thrown out from under them.  A cache that is too small does not
 * report a smaller network, which would be honest, it reports the same
 * network with the answers hollowed out.
 *
 * Both are inline in the AmiNetStack for the reason ns_DnsCache is: identical
 * lifetime, and an allocation that could fail would need a "no mDNS" path.
 */
#define AMI_MDNS_LOCAL_CACHE_BYTES  \
    (1024 + AMI_CFG_MAX_SD_SERVICES * 384)
#define AMI_MDNS_PEER_CACHE_BYTES   32768
#endif

/*
 * Three DHCP options NetX Duo does not name. It retrieves any option code the
 * server sent, so a number suffices: RFC 2132 3.17 for the domain name, 3.12
 * for the classful static route list that the published API's
 * aam_StaticRouteTable carries, and RFC 3397 for the domain search list.
 */
#define AMI_DHCP_OPTION_DOMAIN          15
#define AMI_DHCP_OPTION_STATIC_ROUTE    33
#define AMI_DHCP_OPTION_SEARCH         119

/* How long netstack_startup() blocks waiting for the first address. */
#define AMI_DHCP_TIMEOUT_TICKS      (30UL * (ULONG)NX_IP_PERIODIC_RATE)
#define AMI_LINK_TIMEOUT_TICKS      (10UL * (ULONG)NX_IP_PERIODIC_RATE)

/*
 * How long to wait after the RFC 3927 fallback fires. The probe/announce
 * sequence is PROBE_WAIT (0-1 s) + PROBE_NUM probes (1-2 s each) + a claim,
 * so 15 s is ample.
 */
#define AMI_AUTOIP_TIMEOUT_TICKS    (15UL * (ULONG)NX_IP_PERIODIC_RATE)

/*
 * Only for the case where the address-arrival semaphore could not be created:
 * ami_ns_wait_for_address() then has nothing to be woken by and has to look.
 * Each look walks every interface through nx_ip_interface_address_get(), which
 * takes the IP protection mutex, so the interval is set well clear of the IP
 * and DHCP threads it would otherwise contend with rather than at the one-tick
 * floor. It bounds only how late the address is noticed on a path that is
 * already degraded.
 */
#define AMI_ADDRESS_POLL_TICKS      ((ULONG)NX_IP_PERIODIC_RATE / 10UL)

/* --------------------------------------------------------------- the state */

struct AmiNetStack
{
    ULONG               ns_Refs;

    AmiConfig           ns_Config;

    NX_PACKET_POOL      ns_Pool;
    APTR                ns_PoolMemory;
    ULONG               ns_PoolBytes;
    ULONG               ns_PoolPackets;

    NX_IP               ns_Ip;
    APTR                ns_IpStack;
    APTR                ns_ArpCache;
    BOOL                ns_IpCreated;

    AmiSana2If         *ns_Iface[AMI_CFG_MAX_INTERFACES];

    /* MDNS= for each interface, by NX interface index rather than by
       config index: ns_Iface[] is filled in open order and an
       interface that fails to open takes no slot, so the two are not
       the same number. */
    BOOL                ns_IfaceMdns[AMI_CFG_MAX_INTERFACES];

    /* Whether service_discovery's services have been registered on that
       interface.  nx_mdns_disable() leaves the local records suspended rather
       than deleting them and nx_mdns_enable() re-announces what it finds, so
       adding them a second time on an off/on pair would announce each service
       twice. */
    BOOL                ns_IfaceMdnsSvc[AMI_CFG_MAX_INTERFACES];

    /* Which config slot each opened interface came from.  ns_Iface[]
       is in open order, so this is the only mapping back: an
       interface that failed to open advanced the config index and
       not the NX one. */
    UWORD               ns_IfaceCfg[AMI_CFG_MAX_INTERFACES];
    UWORD               ns_IfaceCount;

    NX_DHCP             ns_Dhcp;
    BOOL                ns_DhcpCreated;
    BOOL                ns_DhcpStarted;

    /*
     * The name the client announces as option 12. nx_dhcp_create() keeps the
     * pointer rather than a copy, so it needs storage that outlives the
     * NX_DHCP, and storage of its own, because ns_Config.hostname is now
     * written after startup: an option 12 coming back from the server can
     * rename the machine, and feeding that straight back into what the next
     * REQUEST announces would let a server rewrite the name from under a
     * client that is still transmitting it.
     */
    char                ns_DhcpName[AMI_CFG_NAME_LEN];

    /*
     * Last DHCP state and last address seen per interface. NetX Duo's
     * callbacks report only the new value, so the previous one is kept here to
     * let the notifications report transitions such as a lost lease.
     */
    UBYTE               ns_DhcpState[AMI_CFG_MAX_INTERFACES];
    ULONG               ns_LastAddress[AMI_CFG_MAX_INTERFACES];

    /*
     * Somebody else answered an ARP for one of our addresses.  Counted rather
     * than only logged, because the log is off in a shipping build and this is
     * the one fault where the machine is working and the network is not: two
     * hosts on one address, and whichever answers first wins each exchange.
     */
    ULONG               ns_AddrConflicts;
    ULONG               ns_LastConflictAddr;

    /*
     * Posted by ami_ns_address_changed() when an interface gains or loses an
     * address, so ami_ns_wait_for_address() can block instead of polling.
     */
    TX_SEMAPHORE        ns_AddrArrived;
    BOOL                ns_AddrArrivedReady;

    NX_AUTO_IP          ns_AutoIp;
    APTR                ns_AutoIpStack;
    BOOL                ns_AutoIpCreated;
    BOOL                ns_AutoIpRunning;

    NX_DNS              ns_Dns;
    BOOL                ns_DnsCreated;

#ifdef AMINETXDUO_IPV6
    /*
     * Recursive DNS servers a router advertised, RFC 8106.  ami_ns6_rdnss()
     * runs on the IP thread and may not call into the DNS client, that
     * client holds its mutex across a query, and a query needs the IP thread
     * so it only writes here, and the next lookup takes what it finds.
     *
     * This is the set the router last described, not a log of what it has ever
     * said: a lifetime of zero takes an entry back out (RFC 8106 5.1), and the
     * absorb step reconciles the DNS client and the reported configuration
     * against it rather than only adding.  Without that a withdrawn server
     * stays in the resolver for the life of the machine.
     *
     * Four is what RFC 8106 section 5.1 expects a router to advertise (it
     * recommends no more than three) and is the same order as
     * NX_DNS_MAX_SERVERS.  A fifth is dropped rather than replacing one that
     * is answering.
     */
    NXD_ADDRESS         ns_Rdnss[AMI_RDNSS_MAX];
    UWORD               ns_RdnssCount;
    volatile BOOL       ns_RdnssPending;    /* written by the IP thread */

    /*
     * The search domains from the same advertisement, still encoded, for the
     * same reason: ami_ns6_dnssl() runs on the IP thread and the list it feeds
     * is read by every resolver call.  ns_DnsslLifetime is the option's, so
     * the absorb step knows whether to add the names or take them back.
     */
    UBYTE               ns_Dnssl[AMI_DNSSL_MAX];
    UWORD               ns_DnsslLen;
    ULONG               ns_DnsslLifetime;
    volatile BOOL       ns_DnsslPending;    /* written by the IP thread */
#endif
#ifdef NX_DNS_CACHE_ENABLE
    /* Inline rather than separately allocated: small, same lifetime as the
       NX_DNS it belongs to, and an allocation that can fail would need a "no
       cache" path.  Explicitly aligned because nx_dns_cache_initialize()
       rejects a buffer that is not longword aligned and m68k gives a ULONG
       only two-byte alignment by default. */
    ULONG               ns_DnsCache[AMI_DNS_CACHE_BYTES / sizeof(ULONG)]
                            __attribute__((aligned(4)));
#endif

#ifdef AMINETXDUO_MDNS
    /*
     * The responder, its thread stack and its two caches. Sized in
     * netstack_mdns.c; both caches are inline for the reason ns_DnsCache is,
     * and longword-aligned because the module lays resource records out from
     * both ends of the buffer and m68k gives a UCHAR array no alignment.
     */
    NX_MDNS             ns_Mdns;
    APTR                ns_MdnsStack;
    BOOL                ns_MdnsCreated;
    BOOL                ns_MdnsClaimed;     /* probing finished, name is ours */
    char                ns_MdnsLabel[NX_MDNS_HOST_NAME_MAX];  /* as configured */
    UCHAR               ns_MdnsLocalCache[AMI_MDNS_LOCAL_CACHE_BYTES]
                            __attribute__((aligned(4)));
    UCHAR               ns_MdnsPeerCache[AMI_MDNS_PEER_CACHE_BYTES]
                            __attribute__((aligned(4)));
#endif

#ifdef AMINETXDUO_IPV6
    BOOL                ns_Ipv6Enabled;
#endif
};

#ifdef AMINETXDUO_IPV6
/* netstack_ipv6.c, run from ami_ns_create_ip()/ami_ns_configure_addresses()
   at the points marked there, and from netstack_interface_add() for an
   interface that arrives after startup. Both callers must hold a ThreadX
   bracket. _configure_one() is idempotent: an interface that already has its
   link-local is left alone. */
LONG ami_netstack_ipv6_enable(AmiNetStack *ns);
VOID ami_netstack_ipv6_configure(AmiNetStack *ns);
VOID ami_netstack_ipv6_configure_one(AmiNetStack *ns, UWORD index);
#endif

/* --------------------------------------------------------- bring-up marks */

/*
 * One line per bring-up milestone, stamped with ami_millis(), which is the
 * guest's own E-Clock and so is not affected by how loaded the host running
 * the emulator is.  The shape is
 *
 *     netstack: mark <event> <ms> ms
 *
 * and tests/ipv6/run-bringup.sh reads it; the event names are that script's
 * keys.  An AMI_INFO, so a default build carries neither the strings nor the
 * call.
 *
 * It exists because the interesting part of bring-up is what happens after the
 * command returns: duplicate address detection, the router solicitation and
 * the address a router advertisement brings all land on the IP thread, minutes
 * of wall clock apart from nothing that a caller could time.
 */
VOID ami_netstack_mark(const char *event);

/* ------------------------------------------------------------- baton hooks */

/*
 * Registered with the SANA-II shim through ami_sana2_set_block_hooks(). See
 * netstack_baton.c: these make it safe for a ThreadX thread to block in exec
 * Wait() for an IORequest.
 */
VOID ami_netstack_baton_release(VOID);
VOID ami_netstack_baton_acquire(VOID);

/*
 * The public anchor for the baton counters and the tick task's, published for
 * as long as the stack is up. netstack_baton.c, and
 * include/aminetxduo/health.h for what reads it.
 */
VOID ami_netstack_health_publish(VOID);
VOID ami_netstack_health_unpublish(VOID);

/*
 * The bracket calls this on every transition so the packet-pool figures in the
 * health mark are never much older than the last thing the stack did.
 *
 * Through a pointer rather than by name: tests/bracket compiles netstack_baton.c
 * on its own against threadx_port, without the rest of the stack, and a direct
 * call to netstack.c would not link there.  NULL until the pool exists, and
 * NULL again before it goes.
 */
VOID ami_netstack_baton_set_sampler(VOID (*fn)(VOID));

/* The per-operation half of netstack_pool_sample(): the running minimum only,
   which is the one figure that cannot be read back later. */
VOID netstack_pool_mark_low(VOID);

/* Wipe the slot table. Only valid once ThreadX has stopped; netstack_baton.c
   says what it is for. */
VOID ami_netstack_baton_reset(VOID);

/* ---------------------------------------------------------- adoption glue,
 *
 * AmiNetCaller / ami_netstack_enter() / ami_netstack_leave() are public; they
 * live in include/aminetxduo/netstack.h so bsdsocket.library and the tools
 * share this bracket rather than growing their own.
 */

#ifdef AMINETXDUO_BPF
/* netstack_capture.c, registers the interfaces with src/bpf/ and installs
   the IP-level filter, the only way to trace loopback. */
VOID ami_netstack_capture_start(AmiNetStack *ns);
VOID ami_netstack_capture_stop(AmiNetStack *ns);

/* The same, for one interface that appeared or went away after start-up. */
VOID ami_netstack_capture_attach_one(AmiNetStack *ns, UWORD index);
VOID ami_netstack_capture_detach_one(AmiNetStack *ns, UWORD index);
#endif

/* ---------------------------------------------------------------- resolver */

LONG ami_netstack_dns_start(AmiNetStack *ns);
VOID ami_netstack_dns_stop(AmiNetStack *ns);

/* Bounded string copy, always NUL-terminating. netstack_dns.c. */
VOID ami_ns_copy_name(char *dst, const char *src, ULONG size);

#ifdef AMINETXDUO_IPV6
/* nx_ipv6_rdnss_notify, on the IP thread. netstack_dns.c. */
VOID ami_ns6_rdnss(NX_IP *ip_ptr, UINT interface_index, ULONG *dns_address,
                   ULONG lifetime);

/* nx_ipv6_dnssl_notify, on the IP thread. netstack_dns.c. */
VOID ami_ns6_dnssl(NX_IP *ip_ptr, UINT interface_index, UCHAR *domains,
                   UINT length, ULONG lifetime);
#endif

/* One DHCP option that is text, from one interface's lease. Not
   NUL-terminated on the wire; `out` always is. netstack.c. */
VOID ami_ns_dhcp_text(AmiNetStack *ns, UWORD index, UINT option,
                      char *out, ULONG outlen);

#ifdef AMINETXDUO_MDNS
/* netstack_mdns.c, the RFC 6762 responder, and the ".local" branch
   netstack_resolve() takes before it reaches the unicast DNS client. */
LONG ami_netstack_mdns_start(AmiNetStack *ns);
VOID ami_netstack_mdns_stop(AmiNetStack *ns);
/* One interface, either way, while the stack runs.  netstack_iface_mdns_set()
   in <aminetxduo/netstack.h> is the published spelling; the interface add and
   remove paths call this one directly, having the AmiNetStack already. */
LONG ami_netstack_mdns_iface_set(AmiNetStack *ns, UWORD index, BOOL enable);
LONG ami_netstack_mdns_resolve(const char *name, ULONG *addr_out,
                               ULONG timeout_ticks);
/* The browse is public to bsdsocket.library; see <aminetxduo/netstack.h>. */
#endif

/* ------------------------------------------------------------ AMITCP port,
 *
 * netstack_rexx.c, the AMITCP public port and the ARexx host servicing it.
 * The port is what `WaitForPort AMITCP` waits on; the host is why a script that
 * addresses it gets an answer instead of blocking (docs/RESEARCH.md 75.7).
 *
 * Without AMINETXDUO_AREXX there is no host and netstack.c opens the port on
 * its own, so the wait still returns and a script still blocks.
 */
#ifdef AMINETXDUO_AREXX
VOID ami_netstack_rexx_start(VOID);
VOID ami_netstack_rexx_stop(VOID);

/* Called around every SANA-II OpenDevice, via ami_sana2_set_open_hooks(). */
VOID ami_netstack_rexx_suspend(VOID);
VOID ami_netstack_rexx_resume(VOID);
#endif

/*
 * Is this name in the .local domain?  Defined in netstack_dns.c and declared
 * outside the mDNS guard on purpose: a build without the responder still has
 * to keep a .local name away from the unicast server (RFC 6762 3), so the
 * test has to exist whether or not anything can answer one.
 */

/* The singleton, without the "is it up" filtering the public accessor does. */
AmiNetStack *ami_netstack_raw(VOID);

#endif /* AMINETXDUO_NETSTACK_INTERNAL_H */

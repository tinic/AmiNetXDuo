/*
 * NETSTATUS_NEIGHBOURS on the host: which neighbours carry NETSTATUS_ND_ROUTER.
 *
 * WHY THIS FILE EXISTS
 *
 *   `arp` prints "router" beside a neighbour when the library sets that flag,
 *   and on a real segment it never did: the machine's own IPv6 router listed
 *   as a plain STALE neighbour with nothing to tell it apart from a host.
 *   Intermittently -- one run in two -- which is what kept it unexplained.
 *
 *   The flag came from ND_CACHE_ENTRY.nx_nd_cache_is_router, a back pointer
 *   NetX Duo writes in exactly one place: nx_icmpv6_process_ra.c:701, inside
 *   the arm that handles an RA's Source Link-Layer Address option. An RA
 *   without that option adds the router to nx_ipv6_default_router_table and
 *   leaves the back pointer NULL for ever; the neighbour entry that the first
 *   packet through the router creates is then an ordinary neighbour. Whether
 *   the option was there, and whether the cache already had an entry when it
 *   was, is the segment's business and changes between boots. Hence one run
 *   in two.
 *
 *   ns_neighbour_is_router() now asks the default router table, which is what
 *   "it is a router for this machine" means, and treats the back pointer as
 *   the shortcut it is.
 *
 * WHAT RUNS HERE
 *
 *   The shipping bsd_NetStackQuery(), over the shipping ns_fill_neighbours().
 *   src/bsdsocket/netstatus.c is compiled whole into this binary, so the walk,
 *   the writer and the flag are the ones the Amiga runs. What is faked is the
 *   NX_IP the query reads: the neighbour cache and the default router table
 *   are filled by hand, which is the only way to stage an RA that carried no
 *   link-layer option.
 *
 *   Every other extern netstatus.c names is stubbed below. None of them is on
 *   the NETSTATUS_NEIGHBOURS path.
 *
 * SPDX-License-Identifier: MIT
 */

#include "bsdsocket_vectors.h"

#include "aminetxduo/netstatus.h"
#include "aminetxduo/sana2.h"
#include "aminetxduo/config.h"
#include "aminetxduo/netstack.h"

#include "nx_nd_cache.h"
#include "tx_amiga.h"

#include <stdio.h>
#include <string.h>

/* --------------------------------------------------------------- reporting */

static unsigned long h_checks;
static unsigned long h_failures;

#define CHECK(cond, what)                                                     \
    do {                                                                      \
        h_checks++;                                                           \
        if (!(cond)) {                                                        \
            h_failures++;                                                     \
            printf("  FAIL %s\n", (what));                                    \
        }                                                                     \
    } while (0)

/* ------------------------------------------------------------- the fixture */

static NX_IP           h_ip;
static NX_INTERFACE   *h_if0;
static NX_INTERFACE   *h_if1;
static BOOL            h_ipv6_on = TRUE;

/* Three addresses: the router, a host on the same link, and the same router
   address seen on a second interface. */
#define R0  0xFE800000UL
#define R3  0x00000001UL
#define H3  0x00000042UL

static VOID hh_addr(ULONG *dst, ULONG last)
{
    dst[0] = R0;
    dst[1] = 0;
    dst[2] = 0;
    dst[3] = last;
}

static VOID h_reset(VOID)
{
    memset(&h_ip, 0, sizeof(h_ip));

    h_if0 = &h_ip.nx_ip_interface[0];
    h_if1 = &h_ip.nx_ip_interface[1];

    h_if0->nx_interface_valid = NX_TRUE;
    h_if1->nx_interface_valid = NX_TRUE;

    h_ipv6_on = TRUE;
}

/* One neighbour cache entry: address, state, which interface, and whether
   NetX Duo happened to write the back pointer. */
static VOID h_neighbour(UINT slot, ULONG last, UCHAR state,
                        NX_INTERFACE *iface,
                        NX_IPV6_DEFAULT_ROUTER_ENTRY *back)
{
    ND_CACHE_ENTRY *e = &h_ip.nx_ipv6_nd_cache[slot];

    memset(e, 0, sizeof(*e));
    hh_addr(e->nx_nd_cache_dest_ip, last);
    e->nx_nd_cache_nd_status     = state;
    e->nx_nd_cache_interface_ptr = iface;
    e->nx_nd_cache_is_router     = back;
}

/* One default router table entry, the way an RA with no Source Link-Layer
   Address option leaves it: valid, addressed, and pointing at no neighbour. */
static NX_IPV6_DEFAULT_ROUTER_ENTRY *h_router(UINT slot, ULONG last,
                                              NX_INTERFACE *iface)
{
    NX_IPV6_DEFAULT_ROUTER_ENTRY *r = &h_ip.nx_ipv6_default_router_table[slot];

    memset(r, 0, sizeof(*r));
    hh_addr(r->nx_ipv6_default_router_entry_router_address, last);
    r->nx_ipv6_default_router_entry_flag           = NX_IPV6_ROUTE_TYPE_VALID;
    r->nx_ipv6_default_router_entry_interface_ptr  = iface;
    r->nx_ipv6_default_router_entry_life_time      = 1800;

    return r;
}

/* --------------------------------------------------------------- the query */

#define H_MAX 8

static NetStatusHeader     *h_hdr;
static NetStatusNeighbour  *h_entry;
static UBYTE                h_buffer[sizeof(NetStatusHeader) +
                                     H_MAX * sizeof(NetStatusNeighbour)];

static LONG h_query(VOID)
{
    h_hdr   = (NetStatusHeader *)h_buffer;
    h_entry = (NetStatusNeighbour *)NETSTATUS_ENTRIES(h_hdr);

    memset(h_buffer, 0, sizeof(h_buffer));
    h_hdr->nsh_Magic   = AMI_NETSTATUS_MAGIC;
    h_hdr->nsh_Version = AMI_NETSTATUS_VERSION;

    return bsd_NetStackQuery(AMI_NETSTATUS_MAGIC, NETSTATUS_NEIGHBOURS,
                             h_buffer, (ULONG)sizeof(h_buffer), NULL);
}

/* The entry for an address, or NULL: the walk is over cache slots and its
   order is not part of the contract. */
static const NetStatusNeighbour *h_find(ULONG last)
{
    UWORD i;

    for (i = 0; i < h_hdr->nsh_Count; i++)
    {
        if (h_entry[i].nsn6_Address[3] == last &&
            h_entry[i].nsn6_Address[0] == R0)
            return &h_entry[i];
    }

    return NULL;
}

static BOOL h_is_router(ULONG last)
{
    const NetStatusNeighbour *e = h_find(last);

    return (BOOL)(e != NULL && (e->nsn6_Flags & NETSTATUS_ND_ROUTER) != 0);
}

/* ------------------------------------------------------------- the stubs -- */

NX_IP *netstack_ip(VOID)                    { return &h_ip; }
BOOL   netstack_ipv6_enabled(VOID)          { return h_ipv6_on; }
VOID   netstack_dns_absorb_pending(VOID)    { }

/* The event ring. Stubbed empty and not linked: NETSTATUS_EVENTS is answered
   from src/common/events.c, this harness asks for NETSTATUS_NEIGHBOURS, and
   src/common/test/test_events.c is where the ring is tested. */
ULONG ami_event_snapshot(NetStatusEvent *out, ULONG room, ULONG *held)
{
    (VOID)out;
    (VOID)room;
    if (held != NULL)
        *held = 0UL;
    return 0UL;
}

LONG bsd_nx_enter(struct AmiSocketBase *b)  { (VOID)b; return 0; }
VOID bsd_nx_leave(struct AmiSocketBase *b)  { (VOID)b; }
LONG bsd_fail(struct AmiSocketBase *b, LONG code) { (VOID)b; (VOID)code; return -1; }

VOID tx_amiga_tick_stats(TX_AMIGA_TICK_STATS *s) { memset(s, 0, sizeof(*s)); }
ULONG ami_eclock_rate(VOID) { return 0; }

AmiBatonStats ami_baton_stats;

/*
 * Everything below is named by netstatus.c and reached by no query this file
 * makes. They exist so the translation unit links; a call to one is a test
 * that has wandered off the neighbour path.
 */
static VOID h_unreachable(const char *what)
{
    printf("  FAIL %s was called on the neighbour path\n", what);
    h_failures++;
}

const AmiConfig *netstack_config(VOID)
{ h_unreachable("netstack_config"); return NULL; }
const AmiIfConfig *netstack_iface_config(UWORD i)
{ (VOID)i; h_unreachable("netstack_iface_config"); return NULL; }
BOOL netstack_iface_mdns(UWORD i)
{ (VOID)i; h_unreachable("netstack_iface_mdns"); return FALSE; }
LONG netstack_hostname_offer(UWORD src, const char *n)
{ (VOID)src; (VOID)n; h_unreachable("netstack_hostname_offer"); return -1; }
LONG netstack_interface_start(const AmiIfConfig *c, UWORD *out)
{ (VOID)c; (VOID)out; h_unreachable("netstack_interface_start"); return -1; }
LONG netstack_interface_remove(UWORD i, BOOL force)
{ (VOID)i; (VOID)force; h_unreachable("netstack_interface_remove"); return -1; }
LONG netstack_interface_up(UWORD i)
{ (VOID)i; h_unreachable("netstack_interface_up"); return -1; }
LONG netstack_interface_down(UWORD i)
{ (VOID)i; h_unreachable("netstack_interface_down"); return -1; }
LONG netstack_interface_dhcp_start(UWORD i, ULONG a)
{ (VOID)i; (VOID)a; h_unreachable("netstack_interface_dhcp_start"); return -1; }
LONG netstack_interface_dhcp_stop(UWORD i, BOOL rel)
{ (VOID)i; (VOID)rel; h_unreachable("netstack_interface_dhcp_stop"); return -1; }
LONG netstack_interface_dhcp_renew(UWORD i)
{ (VOID)i; h_unreachable("netstack_interface_dhcp_renew"); return -1; }
LONG netstack_interface_dhcp_state(UWORD i)
{ (VOID)i; h_unreachable("netstack_interface_dhcp_state"); return -1; }
UWORD netstack_interface_dhcp_raw_state(UWORD i)
{ (VOID)i; h_unreachable("netstack_interface_dhcp_raw_state"); return 0; }
LONG netstack_interface_dhcp_lease(UWORD i, AmiDhcpLease *out)
{ (VOID)i; (VOID)out; h_unreachable("netstack_interface_dhcp_lease"); return -1; }
BOOL netstack_ipv6_address_get(UWORD i, UWORD slot, ULONG a[4], ULONG *p, ULONG *st)
{ (VOID)i; (VOID)slot; (VOID)a; (VOID)p; (VOID)st;
  h_unreachable("netstack_ipv6_address_get"); return FALSE; }
UINT netstack_ipv6_route_add(const ULONG d[4], ULONG len, const ULONG nh[4], UWORD i)
{ (VOID)d; (VOID)len; (VOID)nh; (VOID)i;
  h_unreachable("netstack_ipv6_route_add"); return 1; }
UINT netstack_ipv6_route_delete(const ULONG d[4], ULONG len, const ULONG nh[4])
{ (VOID)d; (VOID)len; (VOID)nh;
  h_unreachable("netstack_ipv6_route_delete"); return 1; }
VOID netstack_pool_sample(VOID) { h_unreachable("netstack_pool_sample"); }

AmiMemStats *ami_mem_stats(VOID) { h_unreachable("ami_mem_stats"); return NULL; }
LONG ami_config_load_interface(const char *n, AmiIfConfig *out)
{ (VOID)n; (VOID)out; h_unreachable("ami_config_load_interface"); return -1; }
ULONG ami_sana2_get_bps(const AmiSana2If *i)
{ (VOID)i; h_unreachable("ami_sana2_get_bps"); return 0; }
VOID ami_sana2_get_stats(const AmiSana2If *i, AmiSana2Stats *o)
{ (VOID)i; (VOID)o; h_unreachable("ami_sana2_get_stats"); }
BOOL ami_sana2_is_online(const AmiSana2If *i)
{ (VOID)i; h_unreachable("ami_sana2_is_online"); return FALSE; }

LONG bsd_if_set_address(struct AmiSocketBase *b, LONG i, BOOL ha, ULONG a,
                        BOOL hm, ULONG m)
{ (VOID)b; (VOID)i; (VOID)ha; (VOID)a; (VOID)hm; (VOID)m;
  h_unreachable("bsd_if_set_address"); return -1; }
LONG bsd_openers_list(struct AmiSocketBase *b, NetStatusOpener *o, LONG room,
                      LONG *avail)
{ (VOID)b; (VOID)o; (VOID)room; (VOID)avail;
  h_unreachable("bsd_openers_list"); return 0; }
ULONG bsd_open_count(struct AmiSocketBase *b)
{ (VOID)b; h_unreachable("bsd_open_count"); return 0; }
LONG bsd_stack_notify(struct AmiSocketBase *b, ULONG *sig)
{ (VOID)b; (VOID)sig; h_unreachable("bsd_stack_notify"); return -1; }
LONG bsd_stack_hold(struct AmiSocketBase *b)
{ (VOID)b; h_unreachable("bsd_stack_hold"); return -1; }
LONG bsd_stack_unhold(struct AmiSocketBase *b)
{ (VOID)b; h_unreachable("bsd_stack_unhold"); return -1; }


/*
 * The NetX Duo entry points netstatus.c calls, none of them on this path.
 * Prototypes copied from third_party/netxduo/common/inc/nx_api.h; the linker
 * would reject a drift and that is the point of writing them out.
 */
UINT _nxe_ip_gateway_address_get(NX_IP *ip, ULONG *a)
{ (VOID)ip; (VOID)a; h_unreachable("nx_ip_gateway_address_get"); return 1; }
UINT _nxe_ip_gateway_address_set(NX_IP *ip, ULONG a)
{ (VOID)ip; (VOID)a; h_unreachable("nx_ip_gateway_address_set"); return 1; }
UINT _nxe_ip_gateway_address_clear(NX_IP *ip)
{ (VOID)ip; h_unreachable("nx_ip_gateway_address_clear"); return 1; }
UINT _nxe_ip_static_route_add(NX_IP *ip, ULONG n, ULONG m, ULONG h)
{ (VOID)ip; (VOID)n; (VOID)m; (VOID)h;
  h_unreachable("nx_ip_static_route_add"); return 1; }
UINT _nxe_ip_static_route_delete(NX_IP *ip, ULONG n, ULONG m)
{ (VOID)ip; (VOID)n; (VOID)m;
  h_unreachable("nx_ip_static_route_delete"); return 1; }
UINT _nxe_arp_static_entry_create(NX_IP *ip, ULONG a, ULONG msw, ULONG lsw)
{ (VOID)ip; (VOID)a; (VOID)msw; (VOID)lsw;
  h_unreachable("nx_arp_static_entry_create"); return 1; }
UINT _nxe_arp_entry_delete(NX_IP *ip, ULONG a)
{ (VOID)ip; (VOID)a; h_unreachable("nx_arp_entry_delete"); return 1; }
UINT _nxe_arp_dynamic_entries_invalidate(NX_IP *ip)
{ (VOID)ip; h_unreachable("nx_arp_dynamic_entries_invalidate"); return 1; }
UINT _nxde_nd_cache_entry_set(NX_IP *ip, ULONG *d, UINT i, CHAR *m)
{ (VOID)ip; (VOID)d; (VOID)i; (VOID)m;
  h_unreachable("nxd_nd_cache_entry_set"); return 1; }
UINT _nxde_nd_cache_entry_delete(NX_IP *ip, ULONG *d)
{ (VOID)ip; (VOID)d; h_unreachable("nxd_nd_cache_entry_delete"); return 1; }
ULONG IPv6_Address_Type(ULONG *a)
{ (VOID)a; h_unreachable("IPv6_Address_Type"); return 0; }

UINT _nxe_packet_pool_info_get(NX_PACKET_POOL *p, ULONG *a, ULONG *b, ULONG *c,
                               ULONG *d, ULONG *e)
{ (VOID)p; (VOID)a; (VOID)b; (VOID)c; (VOID)d; (VOID)e;
  h_unreachable("nx_packet_pool_info_get"); return 1; }
UINT _nxe_ip_info_get(NX_IP *ip, ULONG *a, ULONG *b, ULONG *c, ULONG *d,
                      ULONG *e, ULONG *f, ULONG *g, ULONG *h, ULONG *i,
                      ULONG *j)
{ (VOID)ip; (VOID)a; (VOID)b; (VOID)c; (VOID)d; (VOID)e; (VOID)f; (VOID)g;
  (VOID)h; (VOID)i; (VOID)j; h_unreachable("nx_ip_info_get"); return 1; }
UINT _nxe_icmp_info_get(NX_IP *ip, ULONG *a, ULONG *b, ULONG *c, ULONG *d,
                        ULONG *e, ULONG *f)
{ (VOID)ip; (VOID)a; (VOID)b; (VOID)c; (VOID)d; (VOID)e; (VOID)f;
  h_unreachable("nx_icmp_info_get"); return 1; }
UINT _nxe_tcp_info_get(NX_IP *ip, ULONG *a, ULONG *b, ULONG *c, ULONG *d,
                       ULONG *e, ULONG *f, ULONG *g, ULONG *h, ULONG *i,
                       ULONG *j, ULONG *k)
{ (VOID)ip; (VOID)a; (VOID)b; (VOID)c; (VOID)d; (VOID)e; (VOID)f; (VOID)g;
  (VOID)h; (VOID)i; (VOID)j; (VOID)k; h_unreachable("nx_tcp_info_get");
  return 1; }
UINT _nxe_udp_info_get(NX_IP *ip, ULONG *a, ULONG *b, ULONG *c, ULONG *d,
                       ULONG *e, ULONG *f, ULONG *g)
{ (VOID)ip; (VOID)a; (VOID)b; (VOID)c; (VOID)d; (VOID)e; (VOID)f; (VOID)g;
  h_unreachable("nx_udp_info_get"); return 1; }
UINT _nxe_arp_info_get(NX_IP *ip, ULONG *a, ULONG *b, ULONG *c, ULONG *d,
                       ULONG *e, ULONG *f, ULONG *g, ULONG *h)
{ (VOID)ip; (VOID)a; (VOID)b; (VOID)c; (VOID)d; (VOID)e; (VOID)f; (VOID)g;
  (VOID)h; h_unreachable("nx_arp_info_get"); return 1; }


/*
 * The mDNS half. The host tier defines AMINETXDUO_MDNS, so NETSTATUS_SERVICES
 * and the MDNS controls are compiled here as they are in the shipping
 * library, and they name these.
 */
APTR ami_alloc(ULONG n) { (VOID)n; h_unreachable("ami_alloc"); return NULL; }
VOID ami_free(APTR p) { (VOID)p; h_unreachable("ami_free"); }
UWORD netstack_mdns_browse_collect(const char *t, AmiMdnsService *o, UWORD max,
                                   UWORD *avail)
{ (VOID)t; (VOID)o; (VOID)max; (VOID)avail;
  h_unreachable("netstack_mdns_browse_collect"); return 0; }
const char *netstack_mdns_hostname(VOID)
{ h_unreachable("netstack_mdns_hostname"); return NULL; }
LONG netstack_iface_mdns_set(UWORD i, BOOL on)
{ (VOID)i; (VOID)on; h_unreachable("netstack_iface_mdns_set"); return -1; }
LONG netstack_mdns_browse_start(const char *t)
{ (VOID)t; h_unreachable("netstack_mdns_browse_start"); return -1; }
LONG netstack_mdns_browse_stop(const char *t)
{ (VOID)t; h_unreachable("netstack_mdns_browse_stop"); return -1; }

/* ------------------------------------------------------------- the tests -- */

/*
 * The defect, exactly: an RA with no Source Link-Layer Address option. The
 * router is in the default router table, the neighbour cache learnt it from
 * traffic, and nx_nd_cache_is_router is NULL. Before the fix this row printed
 * as a plain STALE neighbour.
 */
static VOID t_router_without_back_pointer(VOID)
{
    LONG rc;

    h_reset();
    (VOID)h_router(0, R3, h_if0);
    h_neighbour(0, R3, ND_CACHE_STATE_STALE, h_if0, NULL);
    h_neighbour(1, H3, ND_CACHE_STATE_REACHABLE, h_if0, NULL);

    rc = h_query();

    CHECK(rc == 2, "both neighbours are listed");
    CHECK(h_hdr->nsh_Count == 2, "the header counts both");
    CHECK(h_is_router(R3), "the default router is flagged, with no back pointer");
    CHECK(!h_is_router(H3), "an ordinary neighbour is not flagged");
}

/* The path NetX Duo does write. It must still answer, and answer the same. */
static VOID t_router_with_back_pointer(VOID)
{
    NX_IPV6_DEFAULT_ROUTER_ENTRY *r;

    h_reset();
    r = h_router(0, R3, h_if0);
    h_neighbour(0, R3, ND_CACHE_STATE_REACHABLE, h_if0, r);

    (VOID)h_query();

    CHECK(h_is_router(R3), "the back pointer still flags a router");
}

/*
 * A router whose lifetime ran out. nxd_ipv6_prefix_router_timer_tick.c clears
 * the valid bit and the back pointer together, and the neighbour outlives
 * both. Flagging it then would be worse than not flagging it at all: it would
 * say packets still leave that way.
 */
static VOID t_expired_router_is_not_flagged(VOID)
{
    NX_IPV6_DEFAULT_ROUTER_ENTRY *r;

    h_reset();
    r = h_router(0, R3, h_if0);
    r->nx_ipv6_default_router_entry_flag = 0;      /* not VALID any more */
    h_neighbour(0, R3, ND_CACHE_STATE_STALE, h_if0, NULL);

    (VOID)h_query();

    CHECK(h_hdr->nsh_Count == 1, "the neighbour is still listed");
    CHECK(!h_is_router(R3), "an expired router is not flagged");
}

/*
 * fe80::1 is a different machine on each link, so the interface has to match.
 * Two interfaces, the router on the second, the neighbour on the first.
 */
static VOID t_other_interface_is_not_flagged(VOID)
{
    h_reset();
    (VOID)h_router(0, R3, h_if1);
    h_neighbour(0, R3, ND_CACHE_STATE_STALE, h_if0, NULL);

    (VOID)h_query();

    CHECK(h_hdr->nsh_Count == 1, "the neighbour is still listed");
    CHECK(!h_is_router(R3),
          "the same address on another interface is not this link's router");
}

/* An empty router table is the ordinary IPv4-plus-link-local case. */
static VOID t_no_routers_at_all(VOID)
{
    h_reset();
    h_neighbour(0, H3, ND_CACHE_STATE_REACHABLE, h_if0, NULL);

    (VOID)h_query();

    CHECK(h_hdr->nsh_Count == 1, "the neighbour is listed");
    CHECK(!h_is_router(H3), "nothing is a router when the table is empty");
}

/* INVALID slots are holes in a flat array, not entries. */
static VOID t_invalid_slots_are_skipped(VOID)
{
    h_reset();
    (VOID)h_router(0, R3, h_if0);
    h_neighbour(0, R3, ND_CACHE_STATE_INVALID, h_if0, NULL);
    h_neighbour(1, H3, ND_CACHE_STATE_REACHABLE, h_if0, NULL);

    (VOID)h_query();

    CHECK(h_hdr->nsh_Count == 1, "an INVALID slot is not an entry");
    CHECK(h_find(R3) == NULL, "the INVALID slot's address is not reported");
}

int main(void)
{
    printf("NETSTATUS_NEIGHBOURS host tests\n");

    t_router_without_back_pointer();
    t_router_with_back_pointer();
    t_expired_router_is_not_flagged();
    t_other_interface_is_not_flagged();
    t_no_routers_at_all();
    t_invalid_slots_are_skipped();

    printf("neighbour_router checks=%lu failures=%lu\n", h_checks, h_failures);
    return h_failures == 0 ? 0 : 1;
}

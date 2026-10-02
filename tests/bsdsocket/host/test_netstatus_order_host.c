/*
 * NETSTATUS_SYSTEM's opener count, through the shipping netstatus.c:
 * bsd_openers_list() takes sb_Lock, so it must run before bsd_nx_enter().
 * Inside the bracket it made baton -> sb_Lock against the sb_Lock -> job task
 * Wait() -> baton that bsd_stack_interface_link() and _start() hold, and the
 * whole stack stopped.  The stubs are test_hostsource_host.c's, with the
 * bracket and the two opener calls recording their order.
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

#define H_SLOTS ((UWORD)NX_MAX_PHYSICAL_INTERFACES)

static NX_IP                h_ip;
static struct AmiSocketBase h_base;
static AmiIfConfig          h_cfg[NX_MAX_PHYSICAL_INTERFACES];
static LONG                 h_error;
static BOOL                 h_ipv6_on = TRUE;

static VOID h_reset(VOID)
{
    memset(&h_base, 0, sizeof(h_base));
    memset(&h_ip, 0, sizeof(h_ip));
    memset(h_cfg, 0, sizeof(h_cfg));
    h_base.sb_StackRefs = 1;
    h_base.sb_StackIp   = &h_ip;
    h_error             = 0;
}

static UBYTE h_buffer[sizeof(NetStatusHeader) + sizeof(NetStatusSystem) +
                      NX_MAX_PHYSICAL_INTERFACES * sizeof(NetStatusIfDevice)];

static NetStatusHeader *h_hdr = (NetStatusHeader *)h_buffer;

static LONG h_query(ULONG what, ULONG size, UWORD version)
{
    memset(h_buffer, 0xA5, sizeof(h_buffer));
    h_hdr->nsh_Magic   = AMI_NETSTATUS_MAGIC;
    h_hdr->nsh_Version = version;
    h_error = 0;

    return bsd_NetStackQuery(AMI_NETSTATUS_MAGIC, what, h_buffer, size,
                             &h_base);
}

LONG netstack_interface_dhcp_state(UWORD i)
{ (VOID)i; return AMI_DHCP_IDLE; }
LONG netstack_interface_dhcp6_status(UWORD i, AmiDhcp6Status *out)
{ (VOID)i; memset(out, 0, sizeof(*out)); return AMI_NET_OK; }
LONG netstack_interface_dhcp6_release(UWORD i)
{ (VOID)i; return AMI_NET_OK; }
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

/* The call order, one letter each: E enter, L leave, O bsd_openers_list(),
   C bsd_open_count(); and how many sb_Lock takers ran inside a bracket. */
static char h_order[64];
static int  h_order_n;
static int  h_depth;
static int  h_locked_inside;

static VOID h_note(char c)
{
    if (h_order_n + 1 < (int)sizeof(h_order))
    {
        h_order[h_order_n++] = c;
        h_order[h_order_n]   = '\0';
    }
}

LONG bsd_nx_enter(struct AmiSocketBase *b)
{ (VOID)b; h_note('E'); h_depth++; return 0; }
VOID bsd_nx_leave(struct AmiSocketBase *b)
{ (VOID)b; h_note('L'); h_depth--; }
LONG bsd_fail(struct AmiSocketBase *b, LONG code)
{ (VOID)b; h_error = code; return -1; }

/* NETSTATUS_HEALTH's half: the tick counters are staged here, so what the
   selector answers can be compared against what the port tracked. */
static TX_AMIGA_TICK_STATS h_tick;
static AmiMemStats         h_mem;

VOID tx_amiga_tick_stats(TX_AMIGA_TICK_STATS *s) { *s = h_tick; }
VOID tx_amiga_green_stats(TX_AMIGA_GREEN_STATS *s) { memset(s, 0, sizeof(*s)); }
ULONG ami_eclock_rate(VOID) { return 0; }

AmiBatonStats ami_baton_stats;

/* test_ifdevices' stubs, which call this for what its selector must not
   reach.  NETSTATUS_SYSTEM reaches some of them (the gateway, the mDNS name,
   the openers), and their answers are not what is tested here. */
static VOID h_unreachable(const char *what)
{
    (VOID)what;
}

static AmiConfig h_config;
const AmiConfig *netstack_config(VOID) { return &h_config; }
const AmiIfConfig *netstack_iface_config(UWORD i)
{ return (i < H_SLOTS && h_cfg[i].device[0] != '\0') ? &h_cfg[i] : NULL; }
BOOL netstack_iface_mdns(UWORD i)
{ (VOID)i; h_unreachable("netstack_iface_mdns"); return FALSE; }
LONG netstack_hostname_offer(UWORD src, const char *n)
{ (VOID)src; (VOID)n; h_unreachable("netstack_hostname_offer"); return -1; }
VOID netstack_gateway_override_set(ULONG gw)
{ (VOID)gw; h_unreachable("netstack_gateway_override_set"); }
VOID netstack_gateway_override_clear(VOID)
{ h_unreachable("netstack_gateway_override_clear"); }
/* Route notes from the routes file (netstack_routes.c): nothing to record
   here, NETCTRL_ROUTE_ADD/DELETE only have to link. */
VOID netstack_config_route_added(ULONG destination, ULONG netmask)
{
    (VOID)destination; (VOID)netmask;
}

/* The device-derived counters a status query asks reader 0 for
   (sana2_device.c): no reader here, so nothing is asked and nothing waited. */
BOOL  ami_sana2_stats_request(AmiSana2If *iface) { (VOID)iface; return FALSE; }

/* The status query's wait for the readers runs on a Process and sleeps in
   Delay(); a Task reads the copy as it stands.  A Task here, so nothing
   sleeps, and Delay() only has to link. */
static struct Task h_query_task;
struct Task *FindTask(const char *name) { (VOID)name; return &h_query_task; }
LONG Delay(ULONG ticks) { (VOID)ticks; return 0; }
ULONG ami_sana2_stats_epoch(const AmiSana2If *iface) { (VOID)iface; return 0; }
VOID netstack_config_route_deleted(ULONG destination, ULONG netmask)
{
    (VOID)destination; (VOID)netmask;
}
LONG netstack_interface_start(const AmiIfConfig *c, UWORD *out)
{ (VOID)c; (VOID)out; h_unreachable("netstack_interface_start"); return -1; }
LONG bsd_stack_interface_start(struct AmiSocketBase *b, const AmiIfConfig *c,
                               UWORD *out)
{ (VOID)b; (VOID)c; (VOID)out; h_unreachable("bsd_stack_interface_start");
  return -1; }
LONG bsd_stack_interface_link(struct AmiSocketBase *b, UWORD job, UWORD i,
                              BOOL force)
{ (VOID)b; (VOID)job; (VOID)i; (VOID)force;
  h_unreachable("bsd_stack_interface_link"); return -1; }
LONG netstack_interface_remove(UWORD i, BOOL force)
{ (VOID)i; (VOID)force; h_unreachable("netstack_interface_remove"); return -1; }
LONG netstack_interface_up(UWORD i)
{ (VOID)i; h_unreachable("netstack_interface_up"); return -1; }
LONG netstack_interface_down(UWORD i)
{ (VOID)i; h_unreachable("netstack_interface_down"); return -1; }
LONG netstack_interface_dhcp_start(UWORD i, ULONG a)
{ (VOID)i; (VOID)a; h_unreachable("netstack_interface_dhcp_start"); return -1; }
LONG netstack_interface_dhcp_stop(UWORD i, BOOL rel)
{ (VOID)i; (VOID)rel; h_unreachable("netstack_interface_dhcp_stop");
  return -1; }
LONG netstack_interface_dhcp_renew(UWORD i)
{ (VOID)i; h_unreachable("netstack_interface_dhcp_renew"); return -1; }
UWORD netstack_interface_dhcp_raw_state(UWORD i)
{ (VOID)i; h_unreachable("netstack_interface_dhcp_raw_state"); return 0; }
LONG netstack_interface_dhcp_lease(UWORD i, AmiDhcpLease *out)
{ (VOID)i; (VOID)out; h_unreachable("netstack_interface_dhcp_lease"); return -1; }
BOOL netstack_ipv6_address_get(UWORD i, UWORD slot, ULONG a[4], ULONG *p, ULONG *st)
{ (VOID)i; (VOID)slot; (VOID)a; (VOID)p; (VOID)st;
  h_unreachable("netstack_ipv6_address_get"); return FALSE; }

BOOL netstack_ipv6_address_origin(UWORD i, UWORD slot, ULONG *origin)
{ (VOID)i; (VOID)slot; (VOID)origin;
  h_unreachable("netstack_ipv6_address_origin"); return FALSE; }
UINT netstack_ipv6_route_add(const ULONG d[4], ULONG len, const ULONG nh[4], UWORD i)
{ (VOID)d; (VOID)len; (VOID)nh; (VOID)i;
  h_unreachable("netstack_ipv6_route_add"); return 1; }
UINT netstack_ipv6_route_delete(const ULONG d[4], ULONG len, const ULONG nh[4])
{ (VOID)d; (VOID)len; (VOID)nh;
  h_unreachable("netstack_ipv6_route_delete"); return 1; }
VOID netstack_pool_sample(VOID) { h_unreachable("netstack_pool_sample"); }

AmiMemStats *ami_mem_stats(VOID) { return &h_mem; }
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
/* library.c's takes sb_Lock (ObtainSemaphore), so it counts as a lock taker. */
LONG bsd_openers_list(struct AmiSocketBase *b, NetStatusOpener *o, LONG room,
                      LONG *avail)
{
    (VOID)b; (VOID)o; (VOID)room;
    h_note('O');
    if (h_depth > 0)
        h_locked_inside++;
    if (avail != NULL)
        *avail = 3;
    return 0;
}
ULONG bsd_open_count(struct AmiSocketBase *b)
{ (VOID)b; h_note('C'); return 7; }
LONG bsd_stack_notify(struct AmiSocketBase *b, ULONG *sig)
{ (VOID)b; (VOID)sig; h_unreachable("bsd_stack_notify"); return -1; }
LONG bsd_stack_hold(struct AmiSocketBase *b)
{ (VOID)b; h_unreachable("bsd_stack_hold"); return -1; }
LONG bsd_stack_unhold(struct AmiSocketBase *b)
{ (VOID)b; h_unreachable("bsd_stack_unhold"); return -1; }


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
LONG netstack_interface_priority_set(UWORD i, LONG p)
{ (VOID)i; (VOID)p; h_unreachable("netstack_interface_priority_set"); return -1; }
LONG netstack_mdns_browse_start(const char *t)
{ (VOID)t; h_unreachable("netstack_mdns_browse_start"); return -1; }
LONG netstack_mdns_browse_stop(const char *t)
{ (VOID)t; h_unreachable("netstack_mdns_browse_stop"); return -1; }

/* --------------------------------------------- NETSTATUS_SYSTEM order --- */

static const NetStatusSystem *h_system(VOID)
{
    return (const NetStatusSystem *)NETSTATUS_ENTRIES(h_hdr);
}

static VOID t_openers_before_bracket(VOID)
{
    LONG        rc;
    const char *e;
    const char *o;

    h_reset();
    memset(&h_config, 0, sizeof(h_config));
    h_order_n = 0;
    h_order[0] = '\0';
    h_depth = 0;
    h_locked_inside = 0;

    rc = h_query(NETSTATUS_SYSTEM, (ULONG)sizeof(h_buffer),
                 (UWORD)AMI_NETSTATUS_VERSION);
    printf("  order %s\n", h_order);

    e = strchr(h_order, 'E');
    o = strchr(h_order, 'O');
    CHECK(rc == 1, "NETSTATUS_SYSTEM answers one record");
    CHECK(o != NULL && e != NULL && o < e,
          "bsd_openers_list() runs before bsd_nx_enter()");
    CHECK(h_locked_inside == 0, "no sb_Lock taker runs inside the bracket");
    CHECK(h_depth == 0, "the bracket is left");
    CHECK(rc == 1 && h_system()->nss_Openers == 3 &&
          h_system()->nss_OpenCnt == 7,
          "the counts taken outside reach the record");
}

int main(void)
{
    printf("NETSTATUS_SYSTEM opener order host tests\n");

    t_openers_before_bracket();

    printf("system_order checks=%lu failures=%lu\n", h_checks, h_failures);
    return h_failures == 0 ? 0 : 1;
}

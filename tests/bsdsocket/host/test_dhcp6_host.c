/*
 * NETSTATUS_DHCP6 and NETCTRL_DHCP_RELEASE on an IPv6-only interface.
 *
 * Both were reachable only from stack shutdown before this: the selector did
 * not exist, and RELEASE was gated on netstack_interface_dhcp_state(), which
 * reads the IPv4 ns_DhcpState[] and so cannot see a DHCPv6 lease at all.
 *
 * The DHCPv6 client itself is staged rather than run -- netstack_dhcpv6.c
 * needs a live NetX Duo and a ThreadX thread -- so what is asserted here is
 * the netstatus.c half: which selector answers, and which netstack entry
 * point a RELEASE reaches for an interface with no IPv4 lease.
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

static NX_IP  h_ip;
static BOOL   h_ipv6_on = TRUE;

/* The interface the staged DHCPv6 client runs on, and the one with no IPv4. */
#define H_IF6   1U

static LONG   h_v4_state;               /* AMI_DHCP_* for every interface   */
static LONG   h_v6_state;               /* AMI_DHCP_* on H_IF6              */
static BOOL   h_v6_stateful;
static UWORD  h_v6_raw;
static LONG   h_v6_release_calls;
static UWORD  h_v6_release_index;
static LONG   h_v6_release_rc;
static LONG   h_error;                  /* what bsd_fail() was handed       */
static LONG   h_v4_stop_calls;
static UWORD  h_v4_stop_index;
static BOOL   h_v4_stop_release;

#define H_A0    0x20010DB8UL
#define H_A3    0x00000042UL

static VOID h_reset(VOID)
{
    memset(&h_ip, 0, sizeof(h_ip));
    h_ip.nx_ip_interface[0].nx_interface_valid = NX_TRUE;
    h_ip.nx_ip_interface[1].nx_interface_valid = NX_TRUE;

    h_ipv6_on          = TRUE;
    h_v4_state         = AMI_DHCP_IDLE;
    h_v6_state         = AMI_DHCP_IDLE;
    h_v6_stateful      = FALSE;
    h_v6_raw           = 0;
    h_v6_release_calls = 0;
    h_v6_release_index = 0xFFFFU;
    h_v6_release_rc    = AMI_NET_OK;
    h_error            = 0;
    h_v4_stop_calls    = 0;
    h_v4_stop_index    = 0xFFFFU;
    h_v4_stop_release  = FALSE;
}

/* A stateful lease on H_IF6 and nothing on IPv4: the IPv6-only machine. */
static VOID h_stage_v6_lease(VOID)
{
    h_v6_state    = AMI_DHCP_BOUND;
    h_v6_stateful = TRUE;
    h_v6_raw      = NETSTATUS_DHCP6RAW_BOUND;
}

#define H_MAX 8

static NetStatusHeader *h_hdr;
static NetStatusDhcp6  *h_entry;
static UBYTE            h_buffer[sizeof(NetStatusHeader) +
                                 H_MAX * sizeof(NetStatusDhcp6)];

static LONG h_query6(VOID)
{
    h_hdr   = (NetStatusHeader *)h_buffer;
    h_entry = (NetStatusDhcp6 *)NETSTATUS_ENTRIES(h_hdr);

    memset(h_buffer, 0, sizeof(h_buffer));
    h_hdr->nsh_Magic   = AMI_NETSTATUS_MAGIC;
    h_hdr->nsh_Version = AMI_NETSTATUS_VERSION;

    return bsd_NetStackQuery(AMI_NETSTATUS_MAGIC, NETSTATUS_DHCP6,
                             h_buffer, (ULONG)sizeof(h_buffer), NULL);
}

static LONG h_release(UWORD index)
{
    NetStatusControl ctl;

    memset(&ctl, 0, sizeof(ctl));
    ctl.nsc_Magic   = AMI_NETSTATUS_MAGIC;
    ctl.nsc_Version = (UWORD)AMI_NETSTATUS_VERSION;
    ctl.nsc_Index   = index;

    h_error = 0;

    return bsd_NetStackControl(AMI_NETSTATUS_MAGIC, NETCTRL_DHCP_RELEASE,
                               &ctl, (ULONG)sizeof(ctl), NULL);
}

/* The three netstack entry points the two paths under test reach.  Every
   other one is h_unreachable() below. */
LONG netstack_interface_dhcp_state(UWORD i)
{
    (VOID)i;
    return h_v4_state;
}

LONG netstack_interface_dhcp6_status(UWORD i, AmiDhcp6Status *out)
{
    memset(out, 0, sizeof(*out));
    out->ad6_State = (UWORD)AMI_DHCP_IDLE;

    if (i != (UWORD)H_IF6)
        return AMI_NET_OK;

    out->ad6_State    = (UWORD)h_v6_state;
    out->ad6_RawState = h_v6_raw;
    out->ad6_Stateful = h_v6_stateful;

    if (h_v6_state != AMI_DHCP_BOUND)
        return AMI_NET_OK;

    out->ad6_Address[0]       = H_A0;
    out->ad6_Address[3]       = H_A3;
    out->ad6_PreferredSeconds = 3600;
    out->ad6_ValidSeconds     = 7200;
    out->ad6_T1               = 1800;
    out->ad6_T2               = 2880;

    return AMI_NET_OK;
}

LONG netstack_interface_dhcp6_release(UWORD i)
{
    h_v6_release_calls++;
    h_v6_release_index = i;
    return h_v6_release_rc;
}

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
LONG bsd_fail(struct AmiSocketBase *b, LONG code)
{ (VOID)b; h_error = code; return -1; }

/* NETSTATUS_HEALTH's half: the tick counters are staged here, so what the
   selector answers can be compared against what the port tracked. */
static TX_AMIGA_TICK_STATS h_tick;
static AmiMemStats         h_mem;
static LONG                h_pool_samples;

VOID tx_amiga_tick_stats(TX_AMIGA_TICK_STATS *s) { *s = h_tick; }
VOID tx_amiga_green_stats(TX_AMIGA_GREEN_STATS *s) { memset(s, 0, sizeof(*s)); }
ULONG ami_eclock_rate(VOID) { return 0; }

AmiBatonStats ami_baton_stats;

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
{ h_v4_stop_calls++; h_v4_stop_index = i; h_v4_stop_release = rel;
  return AMI_NET_OK; }
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
VOID netstack_pool_sample(VOID) { h_pool_samples++; }

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
LONG netstack_mdns_browse_start(const char *t)
{ (VOID)t; h_unreachable("netstack_mdns_browse_start"); return -1; }
LONG netstack_mdns_browse_stop(const char *t)
{ (VOID)t; h_unreachable("netstack_mdns_browse_stop"); return -1; }

/* ------------------------------------------------------ NETSTATUS_DHCP6 --- */

static VOID t_selector_reports_the_tracked_lease(VOID)
{
    LONG rc;

    h_reset();
    h_stage_v6_lease();

    rc = h_query6();

    CHECK(rc == (LONG)NX_MAX_PHYSICAL_INTERFACES,
          "one row per interface, as NETSTATUS_DHCP does");
    CHECK(h_hdr->nsh_Type == NETSTATUS_DHCP6, "the header names the selector");
    CHECK(h_hdr->nsh_EntrySize == (UWORD)sizeof(NetStatusDhcp6),
          "the header states the library's own entry size");

    CHECK(h_entry[H_IF6].nsd6_Index == (UWORD)H_IF6, "the row is the interface");
    CHECK(h_entry[H_IF6].nsd6_State == NETSTATUS_DHCP_BOUND,
          "the DHCPv6 interface reports BOUND");
    CHECK(h_entry[H_IF6].nsd6_RawState == NETSTATUS_DHCP6RAW_BOUND,
          "the raw NX_DHCPV6_STATE_* is carried through");
    CHECK(h_entry[H_IF6].nsd6_Stateful == 1, "a lease is stateful");
    CHECK(h_entry[H_IF6].nsd6_Address[0] == H_A0 &&
          h_entry[H_IF6].nsd6_Address[3] == H_A3,
          "the leased address is reported");
    CHECK(h_entry[H_IF6].nsd6_ValidSeconds == 7200 &&
          h_entry[H_IF6].nsd6_T1 == 1800 && h_entry[H_IF6].nsd6_T2 == 2880,
          "the lifetimes and the two timers are reported");

    CHECK(h_entry[0].nsd6_State == NETSTATUS_DHCP_OFF,
          "an interface with no DHCPv6 client is OFF, not missing");
}

/* Nothing but the state, and no stale address from a previous lease. */
static VOID t_selector_reports_working(VOID)
{
    h_reset();
    h_v6_state = AMI_DHCP_WORKING;
    h_v6_raw   = NETSTATUS_DHCP6RAW_SOLICIT;

    (VOID)h_query6();

    CHECK(h_entry[H_IF6].nsd6_State == NETSTATUS_DHCP_WORKING,
          "a soliciting client is WORKING");
    CHECK(h_entry[H_IF6].nsd6_RawState == NETSTATUS_DHCP6RAW_SOLICIT,
          "the raw state distinguishes solicit from renew");
    CHECK(h_entry[H_IF6].nsd6_Address[0] == 0,
          "no address is reported before there is one");
}

/* ------------------------------------------------- NETCTRL_DHCP_RELEASE --- */

static VOID t_release_of_a_v6_only_interface(VOID)
{
    LONG rc;

    h_reset();
    h_stage_v6_lease();                 /* and h_v4_state stays IDLE */

    rc = h_release((UWORD)H_IF6);

    CHECK(rc == 0, "RELEASE on a v6-only interface with a lease succeeds");
    CHECK(h_v6_release_calls == 1,
          "it reaches netstack_interface_dhcp6_release()");
    CHECK(h_v6_release_index == (UWORD)H_IF6, "on the asked-for interface");
}

static VOID t_release_reports_the_release_failing(VOID)
{
    LONG rc;

    h_reset();
    h_stage_v6_lease();
    h_v6_release_rc = AMI_NET_ERR_STATE;

    rc = h_release((UWORD)H_IF6);

    CHECK(rc == -1, "a refused Release is not reported as success");
    CHECK(h_error == AMI_ENETDOWN, "and it is ENETDOWN, not ENOTCONN");
}

/* The refusal has to stay honest for the case it was written for. */
static VOID t_release_without_any_lease_is_refused(VOID)
{
    LONG rc;

    h_reset();

    rc = h_release((UWORD)H_IF6);

    CHECK(rc == -1, "RELEASE with no lease is refused");
    CHECK(h_error == AMI_ENOTCONN, "with ENOTCONN");
    CHECK(h_v6_release_calls == 0, "and nothing is released");
}

/* Information-Request leaves options and no address: RFC 8415 18.2.6. */
static VOID t_release_of_an_inform_only_client_is_refused(VOID)
{
    LONG rc;

    h_reset();
    h_v6_state    = AMI_DHCP_BOUND;
    h_v6_stateful = FALSE;

    rc = h_release((UWORD)H_IF6);

    CHECK(rc == -1, "a stateless client has no lease to give back");
    CHECK(h_error == AMI_ENOTCONN, "with ENOTCONN");
    CHECK(h_v6_release_calls == 0, "and nothing is released");
}

static VOID t_release_still_prefers_ipv4(VOID)
{
    LONG rc;

    h_reset();
    h_stage_v6_lease();
    h_v4_state = AMI_DHCP_BOUND;

    rc = h_release((UWORD)H_IF6);

    CHECK(rc == 0, "an interface with both leases still releases the v4 one");
    CHECK(h_v4_stop_calls == 1, "through netstack_interface_dhcp_stop()");
    CHECK(h_v4_stop_release, "with the release flag set");
    CHECK(h_v6_release_calls == 0, "and not through the DHCPv6 path");
}

/*
 * The tick task counts a wakeup that delivered more than one tick, and before
 * this the count reached only tx_initialize_low_level.c's serial dump, which
 * no shipped build compiles.  NETSTATUS_HEALTH is the wire netstat -h reads.
 */
static VOID t_health_reports_the_tick_catchups(VOID)
{
    NetStatusHeader *hdr = (NetStatusHeader *)h_buffer;
    NetStatusHealth *out = (NetStatusHealth *)NETSTATUS_ENTRIES(hdr);
    LONG             rc;

    h_reset();
    memset(&h_tick, 0, sizeof(h_tick));
    memset(&h_mem, 0, sizeof(h_mem));
    h_pool_samples = 0;

    h_tick.tx_amiga_tick_catchups = 4321UL;
    h_tick.tx_amiga_tick_delivered = 99UL;

    memset(h_buffer, 0, sizeof(h_buffer));
    hdr->nsh_Magic   = AMI_NETSTATUS_MAGIC;
    hdr->nsh_Version = AMI_NETSTATUS_VERSION;

    rc = bsd_NetStackQuery(AMI_NETSTATUS_MAGIC, NETSTATUS_HEALTH,
                           h_buffer, (ULONG)sizeof(h_buffer), NULL);

    CHECK(rc == 1, "NETSTATUS_HEALTH answers one record");
    CHECK(hdr->nsh_EntrySize == (UWORD)sizeof(NetStatusHealth),
          "of the library's own shape");
    CHECK(out->nsl_TickCatchups == 4321UL,
          "and it carries the tick task's catch-up count");
    CHECK(out->nsl_TickTicks == 99UL, "beside the ticks delivered");
}

int main(void)
{
    printf("NETSTATUS_DHCP6 and NETSTATUS_HEALTH host tests\n");

    t_selector_reports_the_tracked_lease();
    t_selector_reports_working();
    t_release_of_a_v6_only_interface();
    t_release_reports_the_release_failing();
    t_release_without_any_lease_is_refused();
    t_release_of_an_inform_only_client_is_refused();
    t_release_still_prefers_ipv4();
    t_health_reports_the_tick_catchups();

    printf("dhcp6_status_health checks=%lu failures=%lu\n",
           h_checks, h_failures);
    return h_failures == 0 ? 0 : 1;
}

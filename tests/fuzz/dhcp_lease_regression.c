/*
 * AmiNetXDuo, host regression for the DHCP lease-timer extraction and the
 * RENEWING -> REBINDING -> EXPIRED lifecycle.
 *
 * A server (buggy or hostile) can ACK a finite lease while sending T2 (the
 * rebind time) as the infinity sentinel 0xFFFFFFFF. Taken at face value the
 * Client sets rebind_time = 0xFFFFFFFF; when T1 fires it computes
 * renewal_remain_time = rebind_time - renewal_time = 0xFFFFFFFF - renewal,
 * which never counts down to the rebind transition, so the Client sits in
 * RENEWING for the life of the process and keeps an address the server is
 * free to reallocate. The lease-time and T1 sentinels are legitimate only
 * under an infinite lease; T2 must obey the same rule.
 *
 * This is a plain unit driver, not a fuzzer: the client's option parser and
 * its timeout state machine are both static, so the translation unit is
 * #included exactly as fuzz_dhcp.c does. The state machine is driven directly
 * by calling _nx_dhcp_timeout_process(): the packet pool is left empty so
 * every _nx_dhcp_send_request_internal() fails at allocation and touches no
 * driver (a silent server, which is the case that hangs), and the ThreadX
 * mutex calls are the no-op host stubs.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NX_THREAD_EXTENSION_PTR_GET(a, b, c)    { (a) = NX_NULL; }
#define NX_TIMER_EXTENSION_PTR_GET(a, b, c)     { (a) = NX_NULL; }

#include "nx_api.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include "nxd_dhcp_client.c"
#pragma GCC diagnostic pop

/* The shipping config defines NX_DHCP_CLIENT_SEND_ARP_PROBE, so the timeout
   state machine references the ARP prober. This driver keeps the probe
   countdown at zero throughout, so the prober is never called; the stub only
   satisfies the linker without pulling in the driver-backed ARP path. */
UINT _nx_arp_probe_send(NX_IP *ip_ptr, UINT interface_index, ULONG probe_address)
{
    NX_PARAMETER_NOT_USED(ip_ptr);
    NX_PARAMETER_NOT_USED(interface_index);
    NX_PARAMETER_NOT_USED(probe_address);
    return NX_SUCCESS;
}

#define RATE            ((ULONG)NX_IP_PERIODIC_RATE)
#define INFINITY32      ((ULONG)0xFFFFFFFF)

/* ---- message assembly: BOOTREPLY header, magic cookie, then options ---- */

#define MSG_MAX 512

typedef struct
{
    unsigned char b[MSG_MAX];
    unsigned      len;
} Msg;

static void m_u8(Msg *m, unsigned v)
{
    if (m->len < MSG_MAX)
        m->b[m->len++] = (unsigned char)v;
}

static void m_pad_to(Msg *m, unsigned off)
{
    while (m->len < off && m->len < MSG_MAX)
        m_u8(m, 0);
}

static void m_u32(Msg *m, unsigned long v)
{
    m_u8(m, (unsigned)(v >> 24));
    m_u8(m, (unsigned)(v >> 16));
    m_u8(m, (unsigned)(v >> 8));
    m_u8(m, (unsigned)v);
}

static void m_header(Msg *m)
{
    memset(m, 0, sizeof(*m));
    m_u8(m, 2);                     /* op: BOOTREPLY  */
    m_u8(m, 1);                     /* htype: ethernet */
    m_u8(m, 6);                     /* hlen */
    m_u8(m, 0);                     /* hops */
    m_u32(m, 0x12345678UL);         /* xid */
    m_pad_to(m, NX_BOOTP_OFFSET_YOUR_IP);
    m_u32(m, 0xC0A80164UL);         /* yiaddr 192.168.1.100 */
    m_pad_to(m, NX_BOOTP_OFFSET_VENDOR);
    m_u32(m, NX_BOOTP_MAGIC_COOKIE);
}

static void m_opt32(Msg *m, unsigned code, unsigned long v)
{
    m_u8(m, code);
    m_u8(m, 4);
    m_u32(m, v);
}

static void m_end(Msg *m)
{
    m_u8(m, NX_DHCP_OPTION_END);
    while (m->len < (NX_BOOTP_OFFSET_OPTIONS + 1))
        m_u8(m, NX_DHCP_OPTION_PAD);
}

/* Build an ACK carrying the given lease / T1 / T2 seconds. A sentinel of
   INFINITY32 is written as-is; a value of 0 omits that option. */
static void build_ack(Msg *m, unsigned long lease, unsigned long t1, unsigned long t2)
{
    m_header(m);

    m_u8(m, NX_DHCP_OPTION_DHCP_TYPE);
    m_u8(m, 1);
    m_u8(m, 5);                     /* DHCPACK */

    if (lease)
        m_opt32(m, NX_DHCP_OPTION_DHCP_LEASE, lease);
    if (t1)
        m_opt32(m, NX_DHCP_OPTION_RENEWAL, t1);
    if (t2)
        m_opt32(m, NX_DHCP_OPTION_REBIND, t2);
    m_end(m);
}

/* ---- harness ---- */

static int failures;

#define CHECK(cond, ...)                                                     \
    do {                                                                    \
        if (!(cond)) {                                                      \
            printf("FAIL: ");                                              \
            printf(__VA_ARGS__);                                           \
            printf("\n");                                                  \
            failures++;                                                    \
        }                                                                  \
    } while (0)

static NX_DHCP        g_dhcp;
static NX_IP          g_ip;
static NX_PACKET_POOL g_pool;       /* zeroed: allocation always fails */

/* Zero the world and hand back interface record 0, wired so that
   _nx_dhcp_timeout_process() will find it. */
static NX_DHCP_INTERFACE_RECORD *fresh(void)
{
    NX_DHCP_INTERFACE_RECORD *rec;

    memset(&g_dhcp, 0, sizeof(g_dhcp));
    memset(&g_ip, 0, sizeof(g_ip));
    memset(&g_pool, 0, sizeof(g_pool));

    g_ip.nx_ip_id = NX_IP_ID;
    g_ip.nx_ip_interface[0].nx_interface_valid   = NX_TRUE;
    g_ip.nx_ip_interface[0].nx_interface_link_up = NX_TRUE;

    g_dhcp.nx_dhcp_ip_ptr          = &g_ip;
    g_dhcp.nx_dhcp_packet_pool_ptr = &g_pool;

    rec = &g_dhcp.nx_dhcp_interface_record[0];
    rec->nx_dhcp_interface_index = 0;
    return rec;
}

static UINT extract(NX_DHCP_INTERFACE_RECORD *rec, Msg *m)
{
    unsigned char copy[MSG_MAX];

    memcpy(copy, m->b, m->len);
    return _nx_dhcp_extract_information(&g_dhcp, rec, copy, m->len);
}

/* Drive the timeout state machine from BOUND with no server answering.
   Returns 1 if INIT was reached (lease abandoned), 0 if the cap was hit.
   *saw_renewing / *saw_rebinding record the intermediate transitions. */
static int drive_to_init(NX_DHCP_INTERFACE_RECORD *rec, unsigned long cap,
                         int *saw_renewing, int *saw_rebinding)
{
    unsigned long i;

    *saw_renewing = *saw_rebinding = 0;

    /* Enter BOUND exactly as the acquisition path does: the renewal time is
       what BOUND holds in nx_dhcp_timeout, and address 0 keeps the
       reinitialize on expiry from calling into the (driverless) IP layer. */
    rec->nx_dhcp_record_valid = NX_TRUE;
    rec->nx_dhcp_state        = NX_DHCP_STATE_BOUND;
    rec->nx_dhcp_ip_address   = 0;
    rec->nx_dhcp_gateway_address = 0;
    rec->nx_dhcp_timeout      = rec->nx_dhcp_renewal_time;

    for (i = 0; i < cap; i++)
    {
        _nx_dhcp_timeout_process(&g_dhcp);

        if (rec->nx_dhcp_state == NX_DHCP_STATE_RENEWING)
            *saw_renewing = 1;
        if (rec->nx_dhcp_state == NX_DHCP_STATE_REBINDING)
            *saw_rebinding = 1;
        if (rec->nx_dhcp_state == NX_DHCP_STATE_INIT)
            return 1;
    }
    return 0;
}

int main(void)
{
    NX_DHCP_INTERFACE_RECORD *rec;
    Msg m;
    UINT status;

    /* 1. The defect and its fix: a finite lease with T2 = 0xFFFFFFFF must not
          leave rebind_time at the sentinel. It falls back to the RFC 2131
          default of 0.875 * lease that the lease block derived. */
    rec = fresh();
    build_ack(&m, 8, 4, INFINITY32);
    status = extract(rec, &m);
    CHECK(status == NX_SUCCESS, "extract of finite-lease/infinite-T2 ACK returned %u", (unsigned)status);
    CHECK(rec->nx_dhcp_lease_time  == 8 * RATE, "lease_time %lu, want %lu",
          (unsigned long)rec->nx_dhcp_lease_time, (unsigned long)(8 * RATE));
    CHECK(rec->nx_dhcp_rebind_time != INFINITY32,
          "rebind_time is the infinity sentinel under a finite lease (the defect)");
    CHECK(rec->nx_dhcp_rebind_time == (8 * RATE - (8 * RATE) / 8),
          "rebind_time %lu, want 0.875*lease %lu",
          (unsigned long)rec->nx_dhcp_rebind_time,
          (unsigned long)(8 * RATE - (8 * RATE) / 8));

    /* 2. The stuck state is now unreachable, and the lifecycle completes:
          RENEWING -> REBINDING -> INIT with no server answering. On the
          unfixed source rebind_time = 0xFFFFFFFF and this never reaches INIT
          within the cap. */
    {
        int reached, saw_renewing, saw_rebinding;

        reached = drive_to_init(rec, 100000UL, &saw_renewing, &saw_rebinding);
        CHECK(saw_renewing,  "never entered RENEWING");
        CHECK(saw_rebinding, "never entered REBINDING (stuck in RENEWING: the defect)");
        CHECK(reached,       "never reached INIT/EXPIRED (address never released: the defect)");
    }

    /* 3. A well-formed finite lease is unaffected: T1 and T2 present and sane
          are taken verbatim, and the lifecycle still runs to INIT. */
    rec = fresh();
    build_ack(&m, 8, 4, 7);
    status = extract(rec, &m);
    CHECK(status == NX_SUCCESS, "extract of normal finite ACK returned %u", (unsigned)status);
    CHECK(rec->nx_dhcp_renewal_time == 4 * RATE, "normal renewal_time %lu, want %lu",
          (unsigned long)rec->nx_dhcp_renewal_time, (unsigned long)(4 * RATE));
    CHECK(rec->nx_dhcp_rebind_time  == 7 * RATE, "normal rebind_time %lu, want %lu",
          (unsigned long)rec->nx_dhcp_rebind_time, (unsigned long)(7 * RATE));
    {
        int reached, saw_renewing, saw_rebinding;

        reached = drive_to_init(rec, 100000UL, &saw_renewing, &saw_rebinding);
        CHECK(saw_renewing && saw_rebinding && reached,
              "normal lease did not renew->rebind->expire (renew=%d rebind=%d init=%d)",
              saw_renewing, saw_rebinding, reached);
    }

    /* 4. A genuinely infinite lease still keeps an infinite rebind time: the
          guard blocks only an infinite T2 under a finite lease. */
    rec = fresh();
    build_ack(&m, INFINITY32, 0, INFINITY32);
    status = extract(rec, &m);
    CHECK(status == NX_SUCCESS, "extract of infinite-lease ACK returned %u", (unsigned)status);
    CHECK(rec->nx_dhcp_lease_time  == INFINITY32, "infinite lease_time not preserved: %lu",
          (unsigned long)rec->nx_dhcp_lease_time);
    CHECK(rec->nx_dhcp_rebind_time == INFINITY32,
          "infinite lease dropped its infinite rebind_time: %lu",
          (unsigned long)rec->nx_dhcp_rebind_time);

    /* 5. The T1 sibling (already guarded upstream of this fix) is intact: an
          infinite T1 under a finite lease keeps the derived finite renewal. */
    rec = fresh();
    build_ack(&m, 8, INFINITY32, 0);
    status = extract(rec, &m);
    CHECK(status == NX_SUCCESS, "extract of finite-lease/infinite-T1 ACK returned %u", (unsigned)status);
    CHECK(rec->nx_dhcp_renewal_time != INFINITY32,
          "renewal_time is the infinity sentinel under a finite lease (T1 sibling regressed)");
    CHECK(rec->nx_dhcp_renewal_time == 4 * RATE, "T1-sentinel renewal_time %lu, want lease/2 %lu",
          (unsigned long)rec->nx_dhcp_renewal_time, (unsigned long)(4 * RATE));

    if (failures)
    {
        printf("dhcp_lease_regression: %d check(s) FAILED\n", failures);
        return 1;
    }
    printf("dhcp_lease_regression: all checks passed\n");
    return 0;
}

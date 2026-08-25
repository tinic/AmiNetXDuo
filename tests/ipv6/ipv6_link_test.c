/*
 * AmiNetXDuo, IPv6 over a real SANA-II device.
 *
 * SPDX-License-Identifier: MIT
 */

#include "tx_api.h"
#include "tx_amiga.h"
#include "nx_api.h"
#include "nx_ipv6.h"
#include "nx_icmpv6.h"

#include "aminetxduo/netstack.h"
#include "aminetxduo/config.h"
#include "aminetxduo/crashguard.h"

#include <exec/types.h>
#include <exec/execbase.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <inline/macros.h>
#include <proto/dos.h>

#include <stdarg.h>

#ifdef NX_DISABLE_IPV6
#error "tests/ipv6 requires -DAMINETXDUO_IPV6=ON"
#endif

#ifndef RawPutChar
#  define RawPutChar(c) \
      LP1NR(0x204, RawPutChar, UBYTE, (c), d0, , EXEC_BASE_NAME)
#endif

#define T_LOG_SIZE      8192

static char     t_log_buffer[T_LOG_SIZE];
static ULONG    t_log_used;
static ULONG    t_log_flushed;

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
    if ((out != (BPTR)0) && (t_log_used > t_log_flushed))
    {
        (VOID)Write(out, (APTR)&t_log_buffer[t_log_flushed],
                    (LONG)(t_log_used - t_log_flushed));
    }
    t_log_flushed =  t_log_used;
}

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

/* A finding, not a check: something the wire either has or has not. */
static VOID t_finding(const char *what, UINT yes)
{
    t_log("  ---> %s: %s", what, yes ? (CHAR *)"YES" : (CHAR *)"no");
}

#define T_PING_TIMEOUT      (5UL * (ULONG)NX_IP_PERIODIC_RATE)
#define T_RA_WAIT_TICKS     (20UL * (ULONG)NX_IP_PERIODIC_RATE)

static TX_THREAD    t_main_thread;
static const char   t_ping_data[] = "AmiNetXDuo";

static VOID t_log_addr6(const char *label, const ULONG a[4])
{
    char text[AMI_CFG_IP6_STRLEN];

    ami_config_format_ip6(a, text, sizeof(text));
    t_log("  %s %s", label, text);
}

static UINT t_ping6(NX_IP *ip, const ULONG target[4], const char *label)
{

NXD_ADDRESS  dest;
NX_PACKET   *response =  NX_NULL;
UINT         status;

    dest.nxd_ip_version       =  NX_IP_VERSION_V6;
    dest.nxd_ip_address.v6[0] =  target[0];
    dest.nxd_ip_address.v6[1] =  target[1];
    dest.nxd_ip_address.v6[2] =  target[2];
    dest.nxd_ip_address.v6[3] =  target[3];

    status =  nxd_icmp_ping(ip, &dest, (CHAR *)t_ping_data,
                            (ULONG)sizeof(t_ping_data) - 1UL,
                            &response, T_PING_TIMEOUT);

    if (status == NX_SUCCESS)
    {
        ULONG length =  0;

        (VOID)nx_packet_length_get(response, &length);
        t_log("  ping6 %s: reply, %ld bytes", label, length);
        (VOID)nx_packet_release(response);

        return(TX_TRUE);
    }

    if (response != NX_NULL)
    {
        (VOID)nx_packet_release(response);
    }

    t_log("  ping6 %s: no reply (0x%lx)", label, (ULONG)status);

    return(TX_FALSE);
}

static const ULONG t_loopback6[4]     = { 0, 0, 0, 1UL };
static const ULONG t_allrouters6[4]   = { 0xFF020000UL, 0, 0, 2UL };


/* A usable fe80::/64 on this interface: present, and past DAD. */
static UINT t_has_linklocal(UWORD index)
{

ULONG   addr[4];
ULONG   prefix =  0;
ULONG   state  =  0;
UWORD   slot;

    for (slot = 0; slot < 8; slot++)
    {
        if (!netstack_ipv6_address_get(index, slot, addr, &prefix, &state))
        {
            break;
        }

        if (((addr[0] & 0xFFC00000UL) == 0xFE800000UL) &&
            (state != NX_IPV6_ADDR_STATE_TENTATIVE))
        {
            t_log_addr6("  link-local", addr);
            return(TX_TRUE);
        }
    }

    return(TX_FALSE);
}

#define T_DAD_BUDGET_TICKS \
    (((ULONG)NX_IPV6_DAD_TRANSMITS + 2UL) * (ULONG)NX_IP_PERIODIC_RATE)

static UINT t_await_linklocal(UWORD index, ULONG *ticks_out)
{

ULONG   waited =  0;

    while (!t_has_linklocal(index))
    {
        if (waited >= T_DAD_BUDGET_TICKS)
        {
            t_log("  duplicate address detection did not finish in %ld ticks",
                  T_DAD_BUDGET_TICKS);

            if (ticks_out != NULL)
            {
                *ticks_out =  waited;
            }

            return(TX_FALSE);
        }

        tx_thread_sleep(1);
        waited++;
    }

    t_log("  duplicate address detection finished in %ld ticks", waited);

    if (ticks_out != NULL)
    {
        *ticks_out =  waited;
    }

    return(TX_TRUE);
}

static VOID t_setstr(char *dst, ULONG size, const char *src)
{

ULONG   i;

    for (i = 0; (i + 1UL) < size && src[i] != '\0'; i++)
    {
        dst[i] =  src[i];
    }
    dst[i] =  '\0';
}

static VOID t_readd(VOID)
{

AmiIfConfig     cfg;
UBYTE          *raw =  (UBYTE *)&cfg;
ULONG           i;
UWORD           index =  0xFFFFU;
LONG            rc;

    t_log("remove and re-add interface 0:");

    for (i = 0; i < (ULONG)sizeof(cfg); i++)
    {
        raw[i] =  0;
    }

    /* The tag list AddInterfaceTagList() can express: a name, a device and a
       unit. Everything else is the machine's own record of the interface. */
    t_setstr(cfg.name,   sizeof(cfg.name),   "eth0");
    t_setstr(cfg.device, sizeof(cfg.device), "a2065.device");
    cfg.unit   =  0UL;
    cfg.iptype =  AMI_IPTYPE_STATIC;
    cfg.up     =  FALSE;

    rc =  netstack_interface_remove(0, TRUE);
    if (!t_check((UINT)(rc == AMI_NET_OK), "RemoveInterface(eth0)", (ULONG)rc))
    {
        return;
    }

    (VOID)t_check((UINT)(t_has_linklocal(0) == TX_FALSE),
                  "the removed interface kept no IPv6 address", 0UL);

    rc =  netstack_interface_add(&cfg, &index);
    if (!t_check((UINT)(rc == AMI_NET_OK && index == 0),
                 "AddInterfaceTagList(eth0) put it back", (ULONG)rc))
    {
        return;
    }

    {
        ULONG waited =  0;

        (VOID)t_check(t_await_linklocal(0, &waited),
                      "the re-added interface reaches a usable fe80::/64 "
                      "address", waited);
    }

    (VOID)netstack_interface_up(0);
}

static VOID t_reconnect_solicitation(NX_IP *ip)
{
const AmiIfConfig  *cfg = netstack_iface_config(0);
NX_INTERFACE       *ifp = &ip->nx_ip_interface[0];
LONG                rc;

    if (cfg == NULL || cfg->ip6type != AMI_IP6TYPE_AUTO)
        return;

    rc = netstack_interface_down(0);
    if (!t_check((UINT)(rc == AMI_NET_OK),
                 "AUTO interface went down for the reconnect test", (ULONG)rc))
        return;

    tx_mutex_get(&ip->nx_ip_protection, TX_WAIT_FOREVER);
    ifp->nx_ipv6_rtr_solicitation_max = 0;
    ifp->nx_ipv6_rtr_solicitation_count = 0;
    ifp->nx_ipv6_rtr_solicitation_interval = 0;
    ifp->nx_ipv6_rtr_solicitation_timer = 0;
    tx_mutex_put(&ip->nx_ip_protection);

    rc = netstack_interface_up(0);
    (VOID)t_check((UINT)(rc == AMI_NET_OK),
                  "AUTO interface came back up", (ULONG)rc);

    tx_mutex_get(&ip->nx_ip_protection, TX_WAIT_FOREVER);
    (VOID)t_check((UINT)(ifp->nx_ipv6_rtr_solicitation_max ==
                         NX_ICMPV6_MAX_RTR_SOLICITATIONS &&
                         ifp->nx_ipv6_rtr_solicitation_count ==
                         NX_ICMPV6_MAX_RTR_SOLICITATIONS),
                  "Online re-armed router solicitations",
                  (ULONG)ifp->nx_ipv6_rtr_solicitation_count);
    (VOID)t_check((UINT)(ifp->nx_ipv6_rtr_solicitation_interval ==
                         NX_ICMPV6_RTR_SOLICITATION_INTERVAL &&
                         ifp->nx_ipv6_rtr_solicitation_timer ==
                         NX_ICMPV6_RTR_SOLICITATION_DELAY),
                  "Online restored the solicitation schedule",
                  (ULONG)ifp->nx_ipv6_rtr_solicitation_timer);
    tx_mutex_put(&ip->nx_ip_protection);
}

static VOID t_run(VOID)
{

NX_IP           *ip;
ULONG            addr[4];
ULONG            prefix =  0;
ULONG            state  =  0;
ULONG            linklocal[4];
ULONG            dad_ticks =  0;
UINT             have_linklocal =  TX_FALSE;
UINT             have_global =  TX_FALSE;
UINT             status;
UWORD            slot;

    (VOID)t_check((UINT)(netstack_ipv6_enabled() != FALSE),
                  "IPv6 is enabled on the singleton", 0UL);

    ip =  netstack_ip();
    if (!t_check((UINT)(ip != NULL), "NX_IP exists", 0UL))
    {
        return;
    }

    status =  tx_amiga_adopt_thread(&t_main_thread, "ipv6 link test", 16);
    if (!t_check((UINT)(status == TX_SUCCESS), "adopted this Exec Task",
                 (ULONG)status))
    {
        return;
    }

    (VOID)t_await_linklocal(0, &dad_ticks);

    t_log("interface 0 IPv6 addresses:");

    for (slot = 0; slot < 8; slot++)
    {
        if (!netstack_ipv6_address_get(0, slot, addr, &prefix, &state))
        {
            break;
        }

        t_log_addr6("  addr", addr);
        t_log("        prefix /%ld state %ld%s", prefix, state,
              (state == NX_IPV6_ADDR_STATE_TENTATIVE)
                  ? (CHAR *)" (TENTATIVE, duplicate address detection is "
                            "still running)"
                  : (CHAR *)"");

        if ((addr[0] & 0xFFC00000UL) == 0xFE800000UL &&
            state != NX_IPV6_ADDR_STATE_TENTATIVE)
        {
            linklocal[0] =  addr[0];
            linklocal[1] =  addr[1];
            linklocal[2] =  addr[2];
            linklocal[3] =  addr[3];
            have_linklocal =  TX_TRUE;

            (VOID)t_check((UINT)(prefix == 64UL),
                          "the link-local is reported as a /64", prefix);
        }
        else if ((addr[0] & 0xE0000000UL) == 0x20000000UL)
        {
            /* 2000::/3, a global address, which on this link can only have
               come from a router advertisement. */
            t_finding("global address from stateless autoconfiguration",
                      TX_TRUE);
        }

        if ((addr[0] & 0xE0000000UL) == 0x20000000UL &&
            state != NX_IPV6_ADDR_STATE_TENTATIVE &&
            state != NX_IPV6_ADDR_STATE_UNKNOWN)
        {
            have_global =  TX_TRUE;
        }
    }

    (VOID)t_check((UINT)((netstack_ipv6_have_global() != FALSE) ==
                         (have_global != TX_FALSE)),
                  "netstack_ipv6_have_global() agrees with the address list",
                  (ULONG)have_global);

    (VOID)t_check(have_linklocal,
                  "interface 0 has a usable fe80::/64 address", dad_ticks);

    (VOID)t_check(t_ping6(ip, t_loopback6, "::1"),
                  "ICMPv6 echo to ::1", 0UL);

    if (have_linklocal)
    {
        (VOID)t_check(t_ping6(ip, linklocal, "self"),
                      "ICMPv6 echo to our own link-local address", 0UL);
    }

    t_log("waiting %ld ticks for a router advertisement...", T_RA_WAIT_TICKS);
    tx_thread_sleep(T_RA_WAIT_TICKS);

    {
        UINT router_count =  0;

        (VOID)nxd_ipv6_default_router_number_of_entries_get(ip, 0,
                                                            &router_count);
        t_finding("an IPv6 router advertised itself", (UINT)(router_count > 0));

        if (router_count > 0)
        {
            NXD_ADDRESS router;
            ULONG       lifetime =  0;
            ULONG       prefix_len =  0;

            if (nxd_ipv6_default_router_get(ip, 0, &router, &lifetime,
                                            &prefix_len) == NX_SUCCESS)
            {
                t_log_addr6("  default router", router.nxd_ip_address.v6);
                (VOID)t_ping6(ip, router.nxd_ip_address.v6, "default router");
            }
        }
    }

    /* Re-walk the address list: a prefix from an advertisement would have
       added a global address while we slept. */
    t_log("interface 0 IPv6 addresses after the wait:");
    for (slot = 0; slot < 8; slot++)
    {
        if (!netstack_ipv6_address_get(0, slot, addr, &prefix, &state))
        {
            break;
        }
        t_log_addr6("  addr", addr);
    }

    t_finding("something answered all-routers ff02::2",
              t_ping6(ip, t_allrouters6, "ff02::2"));

    t_reconnect_solicitation(ip);

    t_readd();

    (VOID)tx_amiga_orphan_thread(&t_main_thread);
}

int main(void)
{

LONG    status;

    ami_crash_install();
    ami_crash_install_alert_hook();

    t_log("AmiNetXDuo, IPv6 over a real SANA-II device");
    t_flush();

    status =  netstack_startup();
    t_log("netstack_startup() = %ld", (ULONG)status);

    if (status != AMI_NET_OK && status != AMI_NET_ERR_CONFIG)
    {
        t_log("the stack did not come up; nothing to test");
        t_log("");
        t_log("%ld checks, %ld failures, FAIL", t_checks, t_failures + 1UL);
        t_flush();
        return(20);
    }

    t_run();

    t_log("");
    t_log("%ld checks, %ld failures, %s",
          t_checks, t_failures, (t_failures == 0UL) ? "PASS" : "FAIL");
    t_flush();

    netstack_shutdown();
    t_log("netstack_shutdown() returned");
    t_flush();

    return((t_failures == 0UL) ? 0 : 20);
}

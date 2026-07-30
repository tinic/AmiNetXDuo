/*
 * AmiNetXDuo -- mDNS (RFC 6762), the responder and the ".local" resolver.
 *
 * A machine that fell back to an RFC 3927 link-local address has 169.254.x.y
 * and nothing on the network knows how to reach it: no DHCP server, so no
 * router client list and no local DNS zone carrying its name, and the address
 * was drawn at random and changes on the next boot. mDNS makes it findable --
 * `ping amiga.local` works from any Mac, any Linux box running Avahi and any
 * Windows since 10, with no server on that network. On an ordinary DHCP LAN it
 * saves looking up whichever address the router handed out this week.
 *
 * One host name and its A record: <HOSTNAME>.local, with whatever address the
 * interface currently has. HOSTNAME is the same string DHCP option 12 sends
 * (docs/RESEARCH.md 27); src/config/config_file.c resolves it from the config,
 * then ENV:HOSTNAME, then DEVS:Internet/hosts, and only then falls back to
 * "amiga". That is the single source of truth for the name, not this file.
 *
 * Services are advertised only when the user declares them, in
 * DEVS:Internet/service_discovery. AmiNetXDuo ships clients -- fetch, ftp,
 * telnet, tftp, nc, sntp, whois -- and no servers, so nothing here may invent
 * a _ftp._tcp record: it would point at nothing and a browser that believed it
 * would hang on a connection nothing will accept. What the machine may well be
 * running is somebody else's server -- AmiFTPd and its kind -- and those
 * binaries will never be recompiled to call an API of ours, so an API would
 * reach nothing that exists. A file the user writes does, and it is the user's
 * claim rather than ours. AmiTCP's db/inetd.conf says the same thing the same
 * way. Nothing below connects to the port to check it.
 *
 * RFC 6762 9: probe three times before claiming a name, and rename on a
 * conflict. The vendored module does both; ami_ns_mdns_probing() below reports
 * what it settled on.
 *
 * SPDX-License-Identifier: MIT
 */

#include "netstack_internal.h"

#include <proto/exec.h>

/*
 * The mDNS thread wakes on a query, walks two small caches and answers. It
 * sits below the IP thread (1) and the SANA-II readers (2) so an inbound burst
 * still drains, but above an adopted application task (16), since a responder
 * that ran only when nothing else wanted the CPU would answer after the
 * querier gave up. Next to AutoIP (3), which has the same urgency.
 */
#define AMI_MDNS_PRIORITY           4
#define AMI_MDNS_STACK_SIZE         4096

/* --------------------------------------------------------- the host label */

/*
 * mDNS wants a single DNS label; HOSTNAME may not be one.
 *
 * src/config/config_file.c's last resort before "amiga" is the first
 * non-loopback name in DEVS:Internet/hosts, and a hosts file conventionally
 * carries fully-qualified names such as "amiga.home.lan". Handing that to
 * nx_mdns_create() would claim "amiga.home.lan.local", which no querier asks
 * for.
 *
 * So: everything up to the first dot, unchanged otherwise. The case is left
 * alone; mDNS name comparison is case-insensitive (RFC 6762 16) and
 * lowercasing would make the log disagree with the configuration file.
 */
static VOID ami_ns_mdns_label(const AmiNetStack *ns, char *out, ULONG size)
{
    const char *src = ns->ns_Config.hostname;
    ULONG       i   = 0;

    if (size == 0)
        return;

    if (src == NULL || *src == '\0')
        src = "amiga";

    while (src[i] != '\0' && src[i] != '.' && i + 1 < size)
    {
        out[i] = src[i];
        i++;
    }
    out[i] = '\0';

    /* A HOSTNAME that begins with a dot leaves nothing to claim. */
    if (out[0] == '\0')
    {
        out[0] = 'a'; out[1] = 'm'; out[2] = 'i';
        out[3] = 'g'; out[4] = 'a'; out[5] = '\0';
    }
}

static BOOL ami_ns_mdns_differs(const char *a, const char *b)
{
    ULONG i = 0;

    if (a == NULL || b == NULL)
        return FALSE;

    while (a[i] != '\0' && a[i] == b[i])
        i++;

    return (a[i] != b[i]) ? TRUE : FALSE;
}

/* ------------------------------------------------------ what was claimed */

/*
 * Called by the module at the end of probing, per record. A failure state
 * means the name was contested NX_MDNS_CONFLICT_COUNT times and the module
 * gave up; before that it renames and re-probes on its own (RFC 6762 9), so
 * this callback normally only reports success.
 *
 * Known wart: the vendored renamer appends the RFC 6763 service-instance
 * suffix, so "amiga" becomes "amiga (2)". That is correct for a service
 * instance but not for a host name -- RFC 6762 9's example is
 * "PrinterOne-2.local." -- and a label with a space and parentheses is hard to
 * type at a shell. The rename function is `static` in nxd_mdns.c, so neither a
 * symbol override nor -Wl,--wrap can reach it, and this project does not patch
 * vendored source (docs/RESEARCH.md 13.2). Instead the claimed name is logged
 * along with the fix: set HOSTNAME.
 */
static VOID ami_ns_mdns_probing(NX_MDNS *mdns_ptr, UCHAR *name, UINT state)
{
    AmiNetStack *ns = ami_netstack_raw();

    if (ns == NULL || mdns_ptr != &ns->ns_Mdns)
        return;

    switch (state)
    {
    case NX_MDNS_LOCAL_HOST_REGISTERED_SUCCESS:
        ns->ns_MdnsClaimed = TRUE;
        AMI_INFO("netstack: this machine answers to %s.%s",
                 (name != NULL) ? (const char *)name : "?",
                 (const char *)ns->ns_Mdns.nx_mdns_domain_name);

        /*
         * The module renames in place, so nx_mdns_host_name is the claimed
         * name and ns_MdnsLabel the requested one. They differ only after a
         * collision, which gets its own log line.
         */
        if (name != NULL && ami_ns_mdns_differs((const char *)name,
                                                ns->ns_MdnsLabel))
        {
            AMI_WARN("netstack: the name '%s' is already taken on this "
                     "network, so this machine claimed '%s' instead -- set "
                     "HOSTNAME to give it a name of its own",
                     ns->ns_MdnsLabel, (const char *)name);
        }
        break;

    case NX_MDNS_LOCAL_SERVICE_REGISTERED_FAILURE:
        ns->ns_MdnsClaimed = FALSE;
        AMI_ERROR("netstack: '%s.%s' is taken and every alternative was too -- "
                  "this machine has NO mDNS name. Set HOSTNAME to something "
                  "nothing else on this network is using",
                  ns->ns_MdnsLabel,
                  (const char *)ns->ns_Mdns.nx_mdns_domain_name);
        break;

    default:
        break;
    }
}

/* --------------------------------------------------------- the services */

/*
 * Register what DEVS:Internet/service_discovery declared, on one interface.
 *
 * ttl 0 is "the RFC's own values", not "no TTL": the module then uses 120
 * seconds for the SRV and 4500 for the TXT and PTRs, per RFC 6762 10. One
 * number would flatten all three.
 *
 * priority and weight 0, because RFC 2782 uses them to choose between several
 * hosts offering one service and there is one host here.
 *
 * Unique rather than shared, because RFC 6763 4.1.1 makes the instance name
 * the thing that must not clash: the module then probes the SRV and TXT and
 * renames on a conflict. That is also where the renaming wart noted above
 * stops being one -- " (2)" is the RFC 6763 spelling for a second service
 * instance, and wrong only for a host name.
 *
 * A TXT record goes out either way (RFC 6763 6.1; the module writes one empty
 * string when none is given), so txt= chooses its content, not its existence.
 */
static VOID ami_ns_mdns_services(AmiNetStack *ns, UINT index)
{
    UWORD i;

    for (i = 0; i < ns->ns_Config.sd_service_count; i++)
    {
        AmiSdService *svc  = &ns->ns_Config.sd_services[i];
        char         *name = (svc->name[0] != '\0') ? svc->name
                                                    : ns->ns_MdnsLabel;
        UCHAR        *txt  = (svc->txt[0] != '\0') ? (UCHAR *)svc->txt
                                                   : NX_NULL;
        UINT          status;

        status = nx_mdns_service_add(&ns->ns_Mdns, (UCHAR *)name,
                                     (UCHAR *)svc->type, NX_NULL, txt,
                                     0UL, 0, 0, svc->port,
                                     NX_MDNS_RR_SET_UNIQUE, index);
        if (status != NX_SUCCESS)
        {
            AMI_WARN("netstack: '%s' on port %ld is not advertised (%ld)",
                     svc->type, (long)svc->port, (long)status);
            continue;
        }

        AMI_INFO("netstack: advertising %s port %ld as '%s'",
                 svc->type, (long)svc->port, name);
    }
}

/* ------------------------------------------------------------- lifecycle */

LONG ami_netstack_mdns_start(AmiNetStack *ns)
{
    UINT  status;
    UWORD i;
    UWORD enabled = 0;

    if (ns == NULL || !ns->ns_IpCreated)
        return AMI_NET_ERR_STATE;

    if (ns->ns_MdnsCreated)
        return AMI_NET_OK;

    ami_ns_mdns_label(ns, ns->ns_MdnsLabel, (ULONG)sizeof(ns->ns_MdnsLabel));

    ns->ns_MdnsStack = ami_alloc_flags((ULONG)AMI_MDNS_STACK_SIZE,
                                       MEMF_PUBLIC | MEMF_CLEAR);
    if (ns->ns_MdnsStack == NULL)
    {
        AMI_WARN("netstack: no memory for the mDNS thread");
        return AMI_NET_ERR_NOMEM;
    }

    /*
     * The module uses the stack pool, unlike the DNS client which carries a
     * private one. An mDNS response is one small datagram and the stack pool
     * is sized from AvailMem(), so a second fixed-size pool would reserve
     * memory against a load this never sees.
     */
    status = nx_mdns_create(&ns->ns_Mdns, &ns->ns_Ip, &ns->ns_Pool,
                            AMI_MDNS_PRIORITY,
                            ns->ns_MdnsStack, (ULONG)AMI_MDNS_STACK_SIZE,
                            (UCHAR *)ns->ns_MdnsLabel,
                            ns->ns_MdnsLocalCache,
                            (UINT)sizeof(ns->ns_MdnsLocalCache),
                            ns->ns_MdnsPeerCache,
                            (UINT)sizeof(ns->ns_MdnsPeerCache),
                            ami_ns_mdns_probing);
    if (status != NX_SUCCESS)
    {
        AMI_WARN("netstack: nx_mdns_create failed (%ld) -- this machine will "
                 "have no .local name", (long)status);
        ami_free(ns->ns_MdnsStack);
        ns->ns_MdnsStack = NULL;
        return AMI_NET_ERR_KERNEL;
    }

    ns->ns_MdnsCreated = TRUE;

    /*
     * Per interface. nx_mdns_enable() joins 224.0.0.251 on that interface and
     * starts probing there; the module keeps a separate record set per
     * interface index, so a machine with two interfaces answers on both.
     *
     * An interface with no address yet is still enabled: the module registers
     * for address changes (nx_ip_address_change_notify_internal) and fills the
     * A record in when one arrives, which is the DHCP-then-AutoIP sequence
     * this stack's startup performs.
     */
    for (i = 0; i < ns->ns_IfaceCount; i++)
    {
        status = nx_mdns_enable(&ns->ns_Mdns, (UINT)i);
        if (status != NX_SUCCESS)
        {
            AMI_WARN("netstack: mDNS not enabled on interface %ld (%ld)",
                     (long)i, (long)status);
            continue;
        }
        enabled++;

        /*
         * After nx_mdns_enable(), because the module keeps a record set per
         * interface index and a service added to an interface that is not
         * enabled would never be announced.
         */
        ami_ns_mdns_services(ns, (UINT)i);
    }

    if (enabled == 0)
    {
        AMI_WARN("netstack: mDNS is running on no interface at all");
        return AMI_NET_ERR_NODEV;
    }

    AMI_INFO("netstack: mDNS probing for '%s.local' on %ld interface(s), "
             "%ld service(s)",
             ns->ns_MdnsLabel, (long)enabled,
             (long)ns->ns_Config.sd_service_count);

    return AMI_NET_OK;
}

VOID ami_netstack_mdns_stop(AmiNetStack *ns)
{
    UWORD i;

    if (ns == NULL || !ns->ns_MdnsCreated)
        return;

    /*
     * Disable before delete, per interface: that is what sends the RFC 6762
     * 10.1 goodbye, the same records re-announced with a TTL of zero, so every
     * cache on the network drops this machine's name immediately rather than
     * two minutes later.
     */
    for (i = 0; i < ns->ns_IfaceCount; i++)
        (VOID)nx_mdns_disable(&ns->ns_Mdns, (UINT)i);

    (VOID)nx_mdns_delete(&ns->ns_Mdns);
    ns->ns_MdnsCreated = FALSE;
    ns->ns_MdnsClaimed = FALSE;

    if (ns->ns_MdnsStack != NULL)
    {
        ami_free(ns->ns_MdnsStack);
        ns->ns_MdnsStack = NULL;
    }
}

/* ------------------------------------------------------------- resolving */

/*
 * Test whether `name` is in the .local domain.
 *
 * Case-insensitive, as DNS is (RFC 4343). A trailing dot is accepted:
 * "amiga.local." is the fully-qualified spelling of the same name.
 *
 * The bare name "local" is not in the .local domain -- it is a single label
 * with no domain, and sending it to mDNS would claim a top-level name.
 */
BOOL ami_netstack_mdns_is_local(const char *name)
{
    static const char suffix[] = ".local";
    ULONG             len;
    ULONG             slen = (ULONG)(sizeof(suffix) - 1);
    ULONG             i;

    if (name == NULL)
        return FALSE;

    for (len = 0; name[len] != '\0'; len++)
        ;

    /* One trailing dot is the root label, and is not part of the comparison. */
    if (len > 0 && name[len - 1] == '.')
        len--;

    /* Strictly longer: ".local" alone is a name with an empty label. */
    if (len <= slen)
        return FALSE;

    for (i = 0; i < slen; i++)
    {
        char a = name[len - slen + i];
        char b = suffix[i];

        if (a >= 'A' && a <= 'Z')
            a = (char)(a - 'A' + 'a');

        if (a != b)
            return FALSE;
    }

    return TRUE;
}

/*
 * Strip the domain: nx_mdns_host_address_get() wants the host label and
 * appends the domain itself. "amiga.local." and "amiga.local" both become
 * "amiga"; "printer.study.local" becomes "printer.study", which the module
 * asks for verbatim and nothing answers, since a multi-label name under .local
 * is not a host name.
 */
static BOOL ami_ns_mdns_strip(const char *name, char *out, ULONG size)
{
    ULONG len;
    ULONG i;

    for (len = 0; name[len] != '\0'; len++)
        ;

    if (len > 0 && name[len - 1] == '.')
        len--;

    /* Drop ".local". */
    len -= 6;

    if (len == 0 || len + 1 > size)
        return FALSE;

    for (i = 0; i < len; i++)
        out[i] = name[i];
    out[len] = '\0';

    return TRUE;
}

LONG ami_netstack_mdns_resolve(const char *name, ULONG *addr_out,
                               ULONG timeout_ticks)
{
    AmiNetStack *ns = ami_netstack_raw();
    ULONG        v4 = 0;
    UINT         status;
    char         label[NX_MDNS_HOST_NAME_MAX];

    if (ns == NULL || !ns->ns_MdnsCreated)
        return AMI_NET_ERR_STATE;

    if (!ami_ns_mdns_strip(name, label, (ULONG)sizeof(label)))
        return AMI_NET_ERR_NONAME;

    /*
     * The caller is already a ThreadX thread: netstack_resolve() brackets the
     * whole lookup, mDNS branch included, so one adoption covers both
     * resolvers.
     *
     * NULL for ipv6_address. A non-NULL buffer makes the module also ask for
     * AAAA, and it does so serially: a full A timeout followed by a full AAAA
     * timeout. That doubles the wire traffic of every successful lookup and
     * the wall time of every failed one -- `host nosuchbox.local TIMEOUT 5`
     * spent fifteen seconds retrying, half of it for a record this build
     * cannot use. This is the IPv4 entry point and the module's IPv6 half is
     * not enabled (see CMakeLists.txt); netstack_resolve6() is where to ask if
     * it ever is.
     */
    status = nx_mdns_host_address_get(&ns->ns_Mdns, (UCHAR *)label, &v4,
                                      NX_NULL, timeout_ticks);

    if (status != NX_SUCCESS || v4 == 0UL)
        return AMI_NET_ERR_NONAME;

    *addr_out = v4;

    return AMI_NET_OK;
}

/* -------------------------------------------------------------- public API */

const char *netstack_mdns_hostname(VOID)
{
    AmiNetStack *ns = ami_netstack_raw();

    if (ns == NULL || !ns->ns_MdnsCreated || !ns->ns_MdnsClaimed)
        return NULL;

    /*
     * The claimed name rather than the configured one; after a collision they
     * differ, and callers must show what the network answers to.
     */
    return (const char *)ns->ns_Mdns.nx_mdns_host_name;
}

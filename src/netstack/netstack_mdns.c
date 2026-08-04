/*
 * AmiNetXDuo, mDNS (RFC 6762), the responder and the ".local" resolver.
 *
 * A machine that fell back to an RFC 3927 link-local address has 169.254.x.y
 * and nothing on the network knows how to reach it: no DHCP server, so no
 * router client list and no local DNS zone carrying its name, and the address
 * was drawn at random and changes on the next boot. mDNS makes it findable,
 * `ping amiga.local` works from any Mac, any Linux box running Avahi and any
 * Windows since 10, with no server on that network. On an ordinary DHCP LAN it
 * saves looking up whichever address the router handed out this week.
 *
 * One host name and its A record: <HOSTNAME>.local, with whatever address the
 * interface currently has. HOSTNAME is the same string DHCP option 12 sends
 * (docs/RESEARCH.md 27), resolved from the ranked sources in
 * aminetxduo/config.h. That is the single source of truth for the name, not
 * this file. It can be empty, nothing named the machine, and a label is
 * still needed, so "amiga" is the fallback here, as it is for option 12.
 *
 * <label>.local is not this machine's fully-qualified name and must never be
 * shown as one: RFC 6762 3 scopes it to the link and says it is not globally
 * unique. ShowNetStatus reports it on a line of its own for that reason.
 *
 * Services are advertised only when the user declares them, in
 * DEVS:Internet/service_discovery. AmiNetXDuo ships clients, fetch, ftp,
 * telnet, tftp, nc, sntp, whois, and no servers, so nothing here may invent
 * a _ftp._tcp record: it would point at nothing and a browser that believed it
 * would hang on a connection nothing will accept. What the machine may well be
 * running is somebody else's server, AmiFTPd and its kind, and those
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

/* AMI_MDNS_PRIORITY is in src/thread_priorities.h with the rest of the ladder,
   which asserts where it sits. Stack sizes are not ladder members. */
#define AMI_MDNS_STACK_SIZE         4096

/* --------------------------------------------------------- the host label */

/*
 * mDNS wants a single DNS label; HOSTNAME may not be one. A configured name
 * is often fully qualified, "amiga.home.lan", and handing that to
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
 * instance but not for a host name, RFC 6762 9's example is
 * "PrinterOne-2.local.", and a label with a space and parentheses is hard to
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
 * stops being one, " (2)" is the RFC 6763 spelling for a second service
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
     * the wall time of every failed one, `host nosuchbox.local TIMEOUT 5`
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

/* -------------------------------------------------------------- browsing */

/*
 * Discovery, which is the query half of the same module and until now unused.
 *
 * A browse is a subscription, not a call: nx_mdns_service_continuous_query()
 * registers a query record, the module's thread sends it and re-sends it on the
 * RFC 6762 5.2 backoff, and answers land in the peer cache whenever they
 * arrive. There is no completion, a responder that boots in ten minutes will
 * answer then. So start and stop do not wait for anything, and the caller
 * decides how long it is prepared to listen; ShowNetServices and the ARexx host
 * sleep on their own tasks, outside the bracket, because an Amiga Wait() taken
 * while holding the ThreadX baton would stop the stack for the duration.
 *
 * Collect is the exception: it chases a missing address, which is a query and
 * does wait, bounded (see AMI_MDNS_CHASE_BUDGET). That wait is a ThreadX
 * suspension and not a Wait(), it hands the baton on, as the mDNS branch of
 * netstack_resolve() already does.
 *
 * type NULL asks _services._dns-sd._udp.local (RFC 6763 9), which enumerates
 * the service TYPES present rather than instances of one. The module supports
 * it directly rather than needing the name spelled out: a NULL type in
 * _nx_mdns_service_continuous_query() selects that name and a PTR query, and
 * _nx_mdns_service_lookup() resolves the answers back through the same path as
 * an instance, with a NULL service_name, which is how a type-only row is told
 * from an instance below.
 */

/*
 * How many rows one collect will walk. The peer cache holds about twenty
 * services and nx_mdns_service_lookup() is indexed, so this is the loop's
 * termination and not a policy about how many services a network may have.
 */
#define AMI_MDNS_BROWSE_MAX     64

/*
 * An SRV that arrived without its A record.
 *
 * The module fills service_ipv4 from an A record already in the cache and never
 * asks for one, so a responder that answered with PTR and SRV and nothing else
 * leaves a row carrying a host name and no address, a service that cannot be
 * used. nx_mdns_host_address_get() puts the question on the wire, and returns
 * out of the cache without a packet when the record is already there, so the
 * wait falls only on the rows that need it.
 *
 * Bounded twice. Per name, because the case that waits is a host that has gone
 * while its SRV is still cached; and over the whole walk, so a cache holding a
 * dozen of those cannot turn one collect into a dozen waits. Half a second
 * covers a machine that is awake: RFC 6762 6 delays a shared answer 20-120 ms
 * and the reply follows.
 */
#define AMI_MDNS_CHASE_TICKS    ((ULONG)NX_IP_PERIODIC_RATE / 2)
#define AMI_MDNS_CHASE_BUDGET   ((ULONG)NX_IP_PERIODIC_RATE * 2)

static UCHAR *ami_ns_mdns_type_arg(const char *type)
{
    return (type != NULL && type[0] != '\0') ? (UCHAR *)type : NX_NULL;
}

/*
 * Is this SRV target this machine? The target is a full name, "amiga.local"
 *, and nx_mdns_host_name is the label alone, so the comparison is against the
 * first label, case-insensitively (RFC 6762 16). mDNS probing makes that label
 * unique on the link, so matching it is enough to be sure.
 */
static BOOL ami_ns_mdns_is_ours(const char *target, const char *label)
{
    ULONG i = 0;

    if (target == NULL || label == NULL || label[0] == '\0')
        return FALSE;

    while (label[i] != '\0')
    {
        char a = target[i];
        char b = label[i];

        if (a >= 'A' && a <= 'Z')
            a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z')
            b = (char)(b - 'A' + 'a');

        if (a != b)
            return FALSE;

        i++;
    }

    return (target[i] == '.' || target[i] == '\0') ? TRUE : FALSE;
}

static VOID ami_ns_mdns_copy(char *dst, ULONG size, const UCHAR *src,
                             BOOL *truncated)
{
    ULONG i = 0;

    if (truncated != NULL)
        *truncated = FALSE;

    if (size == 0)
        return;

    if (src != NULL)
    {
        while (src[i] != '\0' && i + 1 < size)
        {
            dst[i] = (char)src[i];
            i++;
        }

        if (truncated != NULL && src[i] != '\0')
            *truncated = TRUE;
    }

    dst[i] = '\0';
}

LONG netstack_mdns_browse_start(const char *type)
{
    AmiNetStack  *ns = ami_netstack_raw();
    AmiNetCaller *caller;
    UINT          status;

    if (ns == NULL || !ns->ns_MdnsCreated)
        return AMI_NET_ERR_STATE;

    caller = ami_netstack_enter_alloc();
    if (caller == NULL)
        return AMI_NET_ERR_STATE;

    status = nx_mdns_service_continuous_query(&ns->ns_Mdns, NX_NULL,
                                              ami_ns_mdns_type_arg(type),
                                              NX_NULL);

    ami_netstack_leave_free(caller);

    return (status == NX_MDNS_SUCCESS) ? AMI_NET_OK : AMI_NET_ERR_KERNEL;
}

/*
 * Stopping is not housekeeping. An unretired query is re-sent for as long as
 * the stack is up, and its record occupies the peer cache the answers have to
 * land in.
 *
 * NX_MDNS_ERROR here means there was no such query to retire, which a caller
 * that stops twice will see and which is not a failure worth a message.
 */
LONG netstack_mdns_browse_stop(const char *type)
{
    AmiNetStack  *ns = ami_netstack_raw();
    AmiNetCaller *caller;
    UINT          status;

    if (ns == NULL || !ns->ns_MdnsCreated)
        return AMI_NET_ERR_STATE;

    caller = ami_netstack_enter_alloc();
    if (caller == NULL)
        return AMI_NET_ERR_STATE;

    status = nx_mdns_service_query_stop(&ns->ns_Mdns, NX_NULL,
                                        ami_ns_mdns_type_arg(type), NX_NULL);

    ami_netstack_leave_free(caller);

    return (status == NX_MDNS_SUCCESS) ? AMI_NET_OK : AMI_NET_ERR_NONAME;
}

/*
 * What has arrived so far.
 *
 * nx_mdns_service_lookup() answers by index and walks both caches from the top
 * each time, so this is quadratic, in a cache that holds about twenty
 * services, which is the size the loop is against.
 *
 * The NX_MDNS_SERVICE it fills is 600-odd bytes and this runs on an ARexx host
 * with an 8 KB stack, so it is allocated rather than declared. It is also
 * reused across the loop: the module memsets it per call.
 *
 * *available counts what the cache had, so a caller with a smaller array can
 * tell a truncated list from a complete one. The cache is live, the responder
 * thread is still writing to it, so two calls a second apart legitimately
 * disagree, and neither is wrong.
 *
 * A row whose SRV came without an A record has its address asked for here, so
 * this can wait; AMI_MDNS_CHASE_BUDGET is the ceiling on how long.
 */

/* Both scratch buffers in one block: an NX_MDNS_SERVICE is 600-odd bytes and
   the row it is unpacked into is another 350, which is most of an ARexx host's
   stack between them. */
typedef struct AmiMdnsScratch
{
    NX_MDNS_SERVICE ams_Raw;
    AmiMdnsService  ams_Row;
} AmiMdnsScratch;

/*
 * Already listed?
 *
 * RFC 6763 4.1.1 makes instance plus type the identity of a service, so that
 * is the comparison. It is needed because the answers repeat: a responder
 * re-announces, and each announcement of the same PTR lands in the cache as a
 * record of its own, five cameras on a real network came back as seven rows,
 * three of them the same camera. nx_mdns_service_lookup() only merges
 * duplicates for the meta-query, and not for a named type.
 */
static BOOL ami_ns_mdns_listed(const AmiMdnsService *rows, UWORD count,
                               const AmiMdnsService *row)
{
    UWORD i;

    for (i = 0; i < count; i++)
    {
        if (ami_ns_mdns_differs(rows[i].ams_Name, row->ams_Name))
            continue;
        if (ami_ns_mdns_differs(rows[i].ams_Type, row->ams_Type))
            continue;

        return TRUE;
    }

    return FALSE;
}

/*
 * The address for one SRV target.
 *
 * Rows already written are searched first: several services on one machine
 * share a target, so they cost one query between them, and a target that was
 * asked about and did not answer is not asked about again.
 *
 * `budget` is decremented by what was actually spent, because
 * nx_mdns_host_address_get() repeats the wait per enabled interface.
 */
static ULONG ami_ns_mdns_chase(AmiNetStack *ns, const AmiMdnsService *rows,
                               UWORD count, const char *host,
                               const UCHAR *target, ULONG *budget)
{
    ULONG v4 = 0;
    ULONG wait;
    ULONG start;
    ULONG spent;
    UWORD i;

    for (i = 0; i < count; i++)
    {
        if (!ami_ns_mdns_differs(rows[i].ams_Host, host))
            return rows[i].ams_Address;
    }

    if (*budget == 0UL)
        return 0UL;

    wait  = (*budget < AMI_MDNS_CHASE_TICKS) ? *budget : AMI_MDNS_CHASE_TICKS;
    start = tx_time_get();

    if (nx_mdns_host_address_get(&ns->ns_Mdns, (UCHAR *)target, &v4, NX_NULL,
                                 (UINT)wait) != NX_MDNS_SUCCESS)
    {
        v4 = 0UL;
    }

    spent   = tx_time_get() - start;
    *budget = (*budget > spent) ? (*budget - spent) : 0UL;

    return v4;
}

UWORD netstack_mdns_browse_collect(const char *type, AmiMdnsService *out,
                                   UWORD max, UWORD *available)
{
    AmiNetStack    *ns = ami_netstack_raw();
    AmiNetCaller   *caller;
    AmiMdnsScratch *scratch;
    AmiMdnsService *row;
    ULONG           budget = AMI_MDNS_CHASE_BUDGET;
    UWORD           written = 0;
    UINT            i;

    if (available != NULL)
        *available = 0;

    if (ns == NULL || !ns->ns_MdnsCreated || out == NULL || max == 0)
        return 0;

    scratch = (AmiMdnsScratch *)ami_alloc_flags((ULONG)sizeof(AmiMdnsScratch),
                                                MEMF_PUBLIC | MEMF_CLEAR);
    if (scratch == NULL)
        return 0;

    row = &scratch->ams_Row;

    caller = ami_netstack_enter_alloc();
    if (caller == NULL)
    {
        ami_free(scratch);
        return 0;
    }

    /*
     * Bounded by the index rather than by the return code alone: the cache can
     * grow under the loop, and "keep asking until it says no more" against a
     * cache something else is filling has no reason to end.
     */
    for (i = 0; i < (UINT)AMI_MDNS_BROWSE_MAX; i++)
    {
        NX_MDNS_SERVICE *svc      = &scratch->ams_Raw;
        BOOL             cut      = FALSE;
        BOOL             host_cut = FALSE;

        if (nx_mdns_service_lookup(&ns->ns_Mdns, NX_NULL,
                                   ami_ns_mdns_type_arg(type), NX_NULL, i,
                                   svc) != NX_MDNS_SUCCESS)
            break;

        row->ams_Index   = (UWORD)svc->interface_index;
        row->ams_Port    = svc->service_port;
        row->ams_Address = svc->service_ipv4;
        row->ams_TextCut = FALSE;

        /*
         * service_name is NULL for a row that came from the meta-query: the
         * PTR pointed at "_http._tcp.local", which resolves to a type and a
         * domain with no instance in front of them.
         */
        ami_ns_mdns_copy(row->ams_Name, (ULONG)sizeof(row->ams_Name),
                         svc->service_name, NULL);
        ami_ns_mdns_copy(row->ams_Type, (ULONG)sizeof(row->ams_Type),
                         svc->service_type, NULL);
        ami_ns_mdns_copy(row->ams_Host, (ULONG)sizeof(row->ams_Host),
                         svc->service_host, &host_cut);

        if (svc->service_text_valid)
        {
            ami_ns_mdns_copy(row->ams_Text, (ULONG)sizeof(row->ams_Text),
                             svc->service_text, &cut);
            row->ams_TextCut = cut;
        }
        else
        {
            row->ams_Text[0] = '\0';
        }

        /*
         * Whether this is our own advertisement. The module searches the local
         * cache before the peer cache and does not say which one answered, so
         * it is decided by the name: a service whose host is the name this
         * machine claimed is this machine's.
         */
        row->ams_Local =
            ns->ns_MdnsClaimed
                ? ami_ns_mdns_is_ours(row->ams_Host,
                                      (const char *)
                                          ns->ns_Mdns.nx_mdns_host_name)
                : FALSE;

        if (ami_ns_mdns_listed(out, written, row))
            continue;

        /*
         * The PTR and the SRV arrived, the A did not. A host name cut short to
         * fit the row is not the name the responder gave, so it is not asked
         * about, the query would be about a different machine.
         */
        if (row->ams_Address == 0UL && row->ams_Host[0] != '\0' && !host_cut)
        {
            row->ams_Address =
                ami_ns_mdns_chase(ns, out, (written < max) ? written : max,
                                  row->ams_Host, svc->service_host, &budget);
        }

        /*
         * Past the caller's array the row is still counted, so a list that
         * had to stop can say so. It cannot be compared against for the
         * duplicate test, so the count past `max` is a lower bound.
         */
        if (written < max)
            out[written] = *row;

        written++;
    }

    ami_netstack_leave_free(caller);
    ami_free(scratch);

    if (available != NULL)
        *available = written;

    return (written < max) ? written : max;
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

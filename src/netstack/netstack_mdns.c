/*
 * AmiNetXDuo -- mDNS (RFC 6762), the responder and the ".local" resolver.
 *
 * WHY THIS EXISTS, and why it lands next to the RFC 3927 work.
 *
 * A machine that fell back to a link-local address has 169.254.x.y and nothing
 * on the network knows how to reach it. There is no DHCP server, so there is
 * no router client list and no local DNS zone with its name in it; the address
 * itself was drawn at random and changes on the next boot. RFC 3927 made the
 * machine reachable. This is what makes it FINDABLE -- `ping amiga.local` from
 * any Mac, any Linux box running Avahi, and any Windows since 10, without a
 * server of any kind existing on that network.
 *
 * It is worth having on an ordinary DHCP LAN too, for the plainer reason that
 * "amiga.local" is easier to say than whichever address the router handed out
 * this week.
 *
 * WHAT THIS MACHINE ANNOUNCES
 *
 * One host name and its A record: <HOSTNAME>.local, address whatever the
 * interface currently has. HOSTNAME is the SAME string DHCP option 12 sends
 * (docs/RESEARCH.md 27) -- src/config/config_file.c resolves it from the
 * config, then ENV:HOSTNAME, then DEVS:Internet/hosts, and only then falls
 * back to "amiga". Two names for one machine would be worse than none, so
 * there is exactly one source of truth and this is not it.
 *
 * NO SERVICES ARE ADVERTISED, deliberately. AmiNetXDuo ships clients: fetch,
 * ftp, telnet, tftp, nc, sntp, whois. There is no FTP server and no telnet
 * server on this machine, so a _ftp._tcp or _telnet._tcp record would be an
 * advertisement for something that is not there -- and a browser that believed
 * it would hang on a connection nothing will accept. src/tools/tftp.c already
 * settles the general question for this tree: "a mode that is announced and
 * not honoured is worse than one that is absent." When a server does exist,
 * nx_mdns_service_add() is one call and this is where it goes.
 *
 * NAME COLLISIONS
 *
 * RFC 6762 9: probe three times before claiming a name, and on a conflict
 * pick another. The vendored module does both, and ami_ns_mdns_probing()
 * below reports what it settled on -- see the comment there for the one place
 * its choice of new name is not what a Unix machine would have picked.
 *
 * SPDX-License-Identifier: MIT
 */

#include "netstack_internal.h"

#include <proto/exec.h>

/*
 * The mDNS thread is a background responder: it wakes on a query, walks two
 * small caches and answers. It must sit BELOW the IP thread (1) so an inbound
 * burst still drains, and below the SANA-II readers (2) for the same reason,
 * but above an adopted application task (16) -- a responder that only ran when
 * nothing else wanted the CPU would answer after the querier had given up.
 * Next to AutoIP (3), which has the same shape and the same urgency.
 */
#define AMI_MDNS_PRIORITY           4
#define AMI_MDNS_STACK_SIZE         4096

/* --------------------------------------------------------- the host label */

/*
 * mDNS wants ONE DNS label, and HOSTNAME may not be one.
 *
 * src/config/config_file.c's last resort before "amiga" is the first
 * non-loopback name in DEVS:Internet/hosts, and a hosts file conventionally
 * carries fully-qualified names -- "amiga.home.lan" is a perfectly ordinary
 * entry. Handing that to nx_mdns_create() would claim the name
 * "amiga.home.lan.local", which is not what anybody meant and is not a name
 * any querier will ask for.
 *
 * So: everything up to the first dot, and nothing else is changed. In
 * particular the case is left alone -- mDNS name comparison is
 * case-insensitive (RFC 6762 16) and lowercasing a name the user chose would
 * only make the log disagree with the configuration file.
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
 * Called by the module at the end of probing, per record.
 *
 * The state that matters is FAILURE, and what it means is precise: the name
 * was contested NX_MDNS_CONFLICT_COUNT times and the module gave up. Before
 * that it renames and re-probes on its own, which is RFC 6762 9's prescription
 * and is why this callback usually only ever reports success.
 *
 * ONE WART, RECORDED RATHER THAN PATCHED. The vendored renamer appends the
 * RFC 6763 service-instance suffix -- "amiga" becomes "amiga (2)" -- and for a
 * SERVICE instance that is correct and is what Bonjour shows in a browser. For
 * a HOST name it is not: RFC 6762 9's own example is "PrinterOne-2.local.",
 * and a host label containing a space and parentheses is one that no user will
 * successfully type at a shell. The rename function is `static` in
 * nxd_mdns.c, so neither a symbol override nor -Wl,--wrap can reach it, and
 * this project does not patch vendored source (docs/RESEARCH.md 13.2). What
 * is done instead is the useful half: say loudly which name was actually
 * claimed, and say what to do about it. Setting HOSTNAME fixes it permanently
 * and is the right fix anyway -- a network with two machines both called
 * "amiga" has a naming problem that renaming one of them at random does not
 * solve.
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
         * The module renames in place, so nx_mdns_host_name is the name that
         * was actually claimed and ns_MdnsLabel is the one that was asked
         * for. They differ only after a collision, and that is worth a line
         * of its own rather than being left for somebody to spot.
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
     * The module's own packet pool is the stack pool, unlike the DNS client
     * which carries a private one. That is the right way round here: an mDNS
     * response is one small datagram and the stack pool is sized from
     * AvailMem(), so a second fixed-size pool would be memory reserved
     * against a load this never has.
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
     * Per interface, not once. nx_mdns_enable() joins 224.0.0.251 on that
     * interface and starts probing there; a machine with Ethernet and PPP
     * should answer on both, and the module keeps a separate record set per
     * interface index for exactly this.
     *
     * An interface with no address yet is still enabled: the module registers
     * for address changes (nx_ip_address_change_notify_internal) and fills the
     * A record in when one arrives, which is precisely the DHCP-then-AutoIP
     * sequence this stack's startup performs.
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
    }

    if (enabled == 0)
    {
        AMI_WARN("netstack: mDNS is running on no interface at all");
        return AMI_NET_ERR_NODEV;
    }

    AMI_INFO("netstack: mDNS probing for '%s.local' on %ld interface(s)",
             ns->ns_MdnsLabel, (long)enabled);

    return AMI_NET_OK;
}

VOID ami_netstack_mdns_stop(AmiNetStack *ns)
{
    UWORD i;

    if (ns == NULL || !ns->ns_MdnsCreated)
        return;

    /*
     * Disable before delete, and per interface, because that is what sends the
     * RFC 6762 10.1 goodbye: the same records re-announced with a TTL of zero,
     * so every cache on the network drops this machine's name the moment it
     * goes away rather than two minutes later. A stack that shut down silently
     * would leave `ping amiga.local` answering into a hole.
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
 * Is `name` in the .local domain?
 *
 * Case-insensitive, because DNS is (RFC 4343) and because a user who types
 * "AMIGA.LOCAL" has not made a mistake. A trailing dot is accepted for the
 * same reason every resolver accepts it: "amiga.local." is the fully-qualified
 * spelling of the same name.
 *
 * The bare name "local" is NOT in the .local domain -- it is a single label
 * with no domain at all, and sending it to mDNS would claim a top-level name.
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
 * Strip the domain, because nx_mdns_host_address_get() wants the host label
 * and appends the domain itself. "amiga.local." and "amiga.local" both become
 * "amiga"; "printer.study.local" becomes "printer.study", which the module
 * will ask for verbatim and nothing will answer -- correctly, because a
 * multi-label name under .local is not a host name.
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
     * whole lookup, mDNS branch included, so that one adoption covers both
     * resolvers rather than each doing its own.
     *
     * NULL for ipv6_address, and it is not a detail. The module treats that
     * argument as "also ask for AAAA", and it asks SERIALLY: a full A timeout
     * and then a full AAAA timeout. Passing a buffer therefore doubled the
     * wire traffic of every successful lookup and doubled the wall time of
     * every failed one -- measured, `host nosuchbox.local TIMEOUT 5` spent
     * fifteen seconds retrying and the second half of it was for a record
     * this build cannot use. This is the IPv4 entry point, the module's IPv6
     * half is not enabled (see CMakeLists.txt), and netstack_resolve6() would
     * be the place to ask if it ever is.
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
     * The CLAIMED name, not the configured one. After a collision they differ,
     * and everything that displays this -- a log line, ShowNetStatus, whatever
     * comes next -- must show what the network will actually answer to.
     */
    return (const char *)ns->ns_Mdns.nx_mdns_host_name;
}

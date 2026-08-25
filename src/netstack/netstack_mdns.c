/*
 * AmiNetXDuo, mDNS (RFC 6762), the responder and the ".local" resolver.
 *
 * <label>.local is link-scoped and not globally unique (RFC 6762 3): it must
 * never be shown as the fully-qualified name of this machine.
 *
 * SPDX-License-Identifier: MIT
 */

#include "netstack_internal.h"

#include <proto/exec.h>

#define AMI_MDNS_STACK_SIZE         4096

/* nx_mdns_disable() schedules, rather than sends, the RFC 6762 goodbye. */
#define AMI_MDNS_GOODBYE_WAIT_TICKS ((ULONG)NX_MDNS_GOODBYE_TIMER_COUNT + 1UL)

/* Every one of these is milliseconds * NX_IP_PERIODIC_RATE / 1000 and this
   port's tick is 50 Hz, so anything under 20 ms divides to zero and
   _nx_mdns_timer_set() drops the work silently. */
_Static_assert(NX_MDNS_RESPONSE_UNIQUE_DELAY > 0,
               "a unique record's response would never be scheduled");
_Static_assert(NX_MDNS_RESPONSE_SHARED_DELAY_MIN > 0,
               "a shared record's response would never be scheduled");
_Static_assert(NX_MDNS_RESPONSE_TC_DELAY_MIN > 0,
               "a truncated query's response would never be scheduled");
_Static_assert(NX_MDNS_QUERY_DELAY_MIN > 0,
               "a query would never be sent");
_Static_assert(NX_MDNS_PROBING_TIMER_COUNT > 0 &&
               NX_MDNS_ANNOUNCING_TIMER_COUNT > 0 &&
               NX_MDNS_GOODBYE_TIMER_COUNT > 0,
               "probing, announcing or the goodbye would never be scheduled");

/* mDNS wants a single DNS label, so: everything up to the first dot.  The case
   is left alone, mDNS name comparison being case-insensitive (RFC 6762 16). */
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

/* Called by the module at the end of probing, per record.  The vendored
   renamer appends the RFC 6763 service-instance suffix ("amiga (2)"), which is
   wrong for a host name and cannot be overridden, so it is logged instead. */
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

        /* The module renames in place: nx_mdns_host_name is the claimed name,
           ns_MdnsLabel the requested one, and they differ after a collision. */
        if (name != NULL && ami_ns_mdns_differs((const char *)name,
                                                ns->ns_MdnsLabel))
        {
            AMI_WARN("netstack: the name '%s' is already taken on this "
                     "network, so this machine claimed '%s' instead. Set "
                     "HOSTNAME to give it a name of its own",
                     ns->ns_MdnsLabel, (const char *)name);
        }
        break;

    case NX_MDNS_LOCAL_HOST_REGISTERED_FAILURE:
        ns->ns_MdnsClaimed = FALSE;
        AMI_ERROR("netstack: '%s.%s' is taken and every alternative was too, "
                  "this machine has NO mDNS name. Set HOSTNAME to something "
                  "nothing else on this network is using",
                  ns->ns_MdnsLabel,
                  (const char *)ns->ns_Mdns.nx_mdns_domain_name);
        break;

    case NX_MDNS_LOCAL_SERVICE_REGISTERED_FAILURE:
        AMI_WARN("netstack: the service name '%s' is taken and every "
                 "alternative was too; the host name remains valid",
                 (name != NULL) ? (const char *)name : "?");
        break;

    default:
        break;
    }
}

/* Register what DEVS:Internet/service_discovery declared, on one interface.
   ttl 0 selects the RFC 6762 10 defaults rather than "no TTL"; priority and
   weight 0 (RFC 2782, one host); unique per RFC 6763 4.1.1. */
static BOOL ami_ns_mdns_services(AmiNetStack *ns, UINT index)
{
    BOOL  complete = TRUE;
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
        if (status == NX_MDNS_EXIST_SAME_SERVICE)
        {
            /* Installed by an earlier pass; retry only what is missing. */
            continue;
        }

        if (status != NX_SUCCESS)
        {
            AMI_WARN("netstack: '%s' on port %ld is not advertised (%ld)",
                     svc->type, (long)svc->port, (long)status);
            complete = FALSE;
            continue;
        }

        AMI_INFO("netstack: advertising %s port %ld as '%s'",
                 svc->type, (long)svc->port, name);
    }

    return complete;
}

/* The module, once.  Separate from the enable loop because an interface can
   ask for mDNS at any time, and nothing is created until one does. */
static LONG ami_ns_mdns_create(AmiNetStack *ns)
{
    UINT status;

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
        AMI_WARN("netstack: nx_mdns_create failed (%ld), this machine will "
                 "have no .local name", (long)status);
        ami_free(ns->ns_MdnsStack);
        ns->ns_MdnsStack = NULL;
        return AMI_NET_ERR_KERNEL;
    }

    ns->ns_MdnsCreated = TRUE;

    return AMI_NET_OK;
}

/* One interface on.  An interface with no address yet is still enabled: the
   module registers for address changes and fills the A record in when one
   arrives.  NX_MDNS_ALREADY_ENABLED is success. */
static LONG ami_ns_mdns_enable_one(AmiNetStack *ns, UWORD index)
{
    UINT status = nx_mdns_enable(&ns->ns_Mdns, (UINT)index);

    if (status != NX_MDNS_SUCCESS && status != NX_MDNS_ALREADY_ENABLED)
    {
        AMI_WARN("netstack: mDNS not enabled on interface %ld (%ld)",
                 (long)index, (long)status);
        /* So that what the status call reports is what is running. */
        ns->ns_IfaceMdns[index] = FALSE;
        return AMI_NET_ERR_KERNEL;
    }

    ns->ns_IfaceMdns[index] = TRUE;

    /* After nx_mdns_enable(): a service added to an interface that is not
       enabled is never announced.  Once per interface, because a disable only
       suspends these records and the enable above put them back. */
    if (!ns->ns_IfaceMdnsSvc[index])
    {
        /* False after any failed add, so a later pass retries the missing
           records; the successful ones are recognized as already present. */
        ns->ns_IfaceMdnsSvc[index] = ami_ns_mdns_services(ns, (UINT)index);
    }

    return AMI_NET_OK;
}

LONG ami_netstack_mdns_start(AmiNetStack *ns)
{
    UWORD i;
    UWORD enabled = 0;
    UWORD wanted  = 0;
    LONG  err;

    if (ns == NULL || !ns->ns_IpCreated)
        return AMI_NET_ERR_STATE;

    for (i = 0; i < ns->ns_IfaceCount; i++)
    {
        if (ns->ns_IfaceMdns[i])
            wanted++;
    }

    if (wanted == 0)
    {
        AMI_INFO("netstack: mDNS off, no interface asked for it");
        return AMI_NET_OK;
    }

    err = ami_ns_mdns_create(ns);
    if (err != AMI_NET_OK)
    {
        /* Nothing is answering, so nothing must claim to be. */
        for (i = 0; i < ns->ns_IfaceCount; i++)
            ns->ns_IfaceMdns[i] = FALSE;
        return err;
    }

    for (i = 0; i < ns->ns_IfaceCount; i++)
    {
        if (!ns->ns_IfaceMdns[i])
            continue;

        if (ami_ns_mdns_enable_one(ns, i) == AMI_NET_OK)
            enabled++;
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

/* One interface, either way, at any time; netstack_iface_mdns_set() is the
   published entry point.  OFF does not delete the module even on the last
   interface: that would guarantee the RFC 6762 10.1 goodbye never goes out. */
LONG ami_netstack_mdns_iface_set(AmiNetStack *ns, UWORD index, BOOL enable)
{
    AmiNetCaller *caller;
    LONG          err;
    UINT          status;

    if (ns == NULL || !ns->ns_IpCreated ||
        index >= (UWORD)AMI_CFG_MAX_ATTACHED ||
        index >= (UWORD)NX_MAX_PHYSICAL_INTERFACES)
        return AMI_NET_ERR_STATE;

    if (!enable && !ns->ns_MdnsCreated)
    {
        /* Never started, so there is nothing to stop and nothing to correct. */
        ns->ns_IfaceMdns[index] = FALSE;
        return AMI_NET_OK;
    }

    caller = ami_netstack_enter_alloc();
    if (caller == NULL)
        return AMI_NET_ERR_STATE;

    if (enable)
    {
        err = ami_ns_mdns_create(ns);
        if (err == AMI_NET_OK)
        {
            err = ami_ns_mdns_enable_one(ns, index);
            if (err == AMI_NET_OK)
            {
                if ((UWORD)(index + 1) > ns->ns_IfaceCount)
                    ns->ns_IfaceCount = (UWORD)(index + 1);

                AMI_INFO("netstack: mDNS on, interface %ld is probing for "
                         "'%s.local'", (long)index, ns->ns_MdnsLabel);
            }
        }
    }
    else
    {
        status = nx_mdns_disable(&ns->ns_Mdns, (UINT)index);

        /* NX_MDNS_NOT_ENABLED is the answer for an interface that was already
           off, which is what was asked for. */
        err = (status == NX_MDNS_SUCCESS || status == NX_MDNS_NOT_ENABLED)
                  ? AMI_NET_OK : AMI_NET_ERR_KERNEL;

        /* Cleared either way: a module that did not disable an interface is
           not answering on it in any sense a caller can use. */
        ns->ns_IfaceMdns[index] = FALSE;

        if (err == AMI_NET_OK && status == NX_MDNS_SUCCESS)
            AMI_INFO("netstack: mDNS off on interface %ld, goodbye queued",
                     (long)index);
        else if (err != AMI_NET_OK)
            AMI_WARN("netstack: mDNS did not stop on interface %ld (%ld)",
                     (long)index, (long)status);
    }

    ami_netstack_leave_free(caller);

    return err;
}

VOID ami_netstack_mdns_stop(AmiNetStack *ns)
{
    UINT  status;
    UWORD n;
    UWORD i;
    BOOL  goodbye_queued = FALSE;

    if (ns == NULL)
        return;

    for (n = 0; n < (UWORD)AMI_CFG_MAX_ATTACHED; n++)
    {
        ns->ns_IfaceMdns[n]    = FALSE;
        ns->ns_IfaceMdnsSvc[n] = FALSE;
    }

    if (!ns->ns_MdnsCreated)
        return;

    /* Disable before delete, per interface: that is what sends the RFC 6762
       10.1 goodbye, so every cache drops the name at once. */
    for (i = 0; i < ns->ns_IfaceCount; i++)
    {
        status = nx_mdns_disable(&ns->ns_Mdns, (UINT)i);
        if (status == NX_MDNS_SUCCESS)
            goodbye_queued = TRUE;
    }

    /* nx_mdns_disable() only arms the responder's timer, so deleting the
       instance now would keep the goodbye off the wire.  Sleep through it;
       guarded because tx_thread_sleep() is invalid from an unadopted task. */
    if (goodbye_queued &&
        tx_amiga_caller_is_thread() != (UINT)TX_FALSE)
    {
        (VOID)tx_thread_sleep(AMI_MDNS_GOODBYE_WAIT_TICKS);
    }

    (VOID)nx_mdns_delete(&ns->ns_Mdns);
    ns->ns_MdnsCreated = FALSE;
    ns->ns_MdnsClaimed = FALSE;

    if (ns->ns_MdnsStack != NULL)
    {
        ami_free(ns->ns_MdnsStack);
        ns->ns_MdnsStack = NULL;
    }
}

/* nx_mdns_host_address_get() wants the host label and appends the domain
   itself, so the trailing dot and ".local" come off here. */
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

    /* NULL for ipv6_address: a non-NULL buffer makes the module ask for AAAA
       as well, serially, doubling the wall time of every failed lookup, and
       the IPv6 half of the module is not enabled in this build. */
    status = nx_mdns_host_address_get(&ns->ns_Mdns, (UCHAR *)label, &v4,
                                      NX_NULL, timeout_ticks);

    if (status != NX_SUCCESS || v4 == 0UL)
        return AMI_NET_ERR_NONAME;

    *addr_out = v4;

    return AMI_NET_OK;
}

/* A browse is a subscription rather than a call: start and stop wait for
   nothing, and the caller decides how long it listens.  A NULL type asks
   _services._dns-sd._udp.local (RFC 6763 9), which enumerates types. */

/* How many rows one collect walks; this terminates the loop and is not a
   policy about how many services a network can have. */
#define AMI_MDNS_BROWSE_MAX     64

/* An SRV that arrived without its A record: the module never asks for one, so
   the address is chased here, bounded per name and over the whole walk.  Half
   a second covers a machine that is awake (RFC 6762 6 delays it 20-120 ms). */
#define AMI_MDNS_CHASE_TICKS    ((ULONG)NX_IP_PERIODIC_RATE / 2)
#define AMI_MDNS_CHASE_BUDGET   ((ULONG)NX_IP_PERIODIC_RATE * 2)

static UCHAR *ami_ns_mdns_type_arg(const char *type)
{
    return (type != NULL && type[0] != '\0') ? (UCHAR *)type : NX_NULL;
}

/* Is this SRV target this machine?  The target is a full name and
   nx_mdns_host_name is the label alone, so compare the first label only, and
   case-insensitively (RFC 6762 16). */
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

/* Stopping matters: an unretired query is re-sent for as long as the stack is
   up, and its record occupies the peer cache the answers land in.
   NX_MDNS_ERROR means there was no such query, which is not a failure. */
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

/* What has arrived so far.  *available counts what the cache had, so a caller
   with a smaller array can tell a truncated list from a complete one.  A row
   whose SRV came without an A record is chased here, so this can wait. */

typedef struct AmiMdnsIdentity
{
    char ami_Name[AMI_MDNS_SVC_NAME_LEN];
    char ami_Type[AMI_MDNS_SVC_TYPE_LEN];
} AmiMdnsIdentity;

/* One block: an NX_MDNS_SERVICE is 600-odd bytes and the row another 350,
   which is most of the stack of an ARexx host.  The identities must cover the
   whole walk, or duplicates beyond the caller's array inflate `available`. */
typedef struct AmiMdnsScratch
{
    NX_MDNS_SERVICE ams_Raw;
    AmiMdnsService  ams_Row;
    AmiMdnsIdentity ams_Seen[AMI_MDNS_BROWSE_MAX];
} AmiMdnsScratch;

/* RFC 6763 4.1.1 makes instance plus type the identity of a service.  Needed
   because announcements repeat, each landing as its own cache record, and
   nx_mdns_service_lookup() merges duplicates only for the meta-query. */
static BOOL ami_ns_mdns_listed(const AmiMdnsIdentity *rows, UWORD count,
                               const AmiMdnsService *row)
{
    UWORD i;

    for (i = 0; i < count; i++)
    {
        if (ami_ns_mdns_differs(rows[i].ami_Name, row->ams_Name))
            continue;
        if (ami_ns_mdns_differs(rows[i].ami_Type, row->ams_Type))
            continue;

        return TRUE;
    }

    return FALSE;
}

/* The address for one SRV target.  Rows already written are searched first, so
   several services on one machine cost one query between them.  `budget` is
   decremented by what was spent. */
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

    /* Bounded by the index as well as the return code: the cache can grow
       under the loop, so "until it says no more" has no reason to end. */
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

        /* service_name is NULL for a row from the meta-query: a type and a
           domain with no instance in front of them. */
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

        /* The module does not say which cache answered, so it is decided by
           the name: a service whose host is the claimed name is ours. */
        row->ams_Local =
            ns->ns_MdnsClaimed
                ? ami_ns_mdns_is_ours(row->ams_Host,
                                      (const char *)
                                          ns->ns_Mdns.nx_mdns_host_name)
                : FALSE;

        if (ami_ns_mdns_listed(scratch->ams_Seen, written, row))
            continue;

        ami_ns_mdns_copy(scratch->ams_Seen[written].ami_Name,
                         (ULONG)sizeof(scratch->ams_Seen[written].ami_Name),
                         (const UCHAR *)row->ams_Name, NULL);
        ami_ns_mdns_copy(scratch->ams_Seen[written].ami_Type,
                         (ULONG)sizeof(scratch->ams_Seen[written].ami_Type),
                         (const UCHAR *)row->ams_Type, NULL);

        /* A host name cut short to fit the row is not the name the responder
           gave, so it is not asked about: that query is a different one. */
        if (row->ams_Address == 0UL && row->ams_Host[0] != '\0' && !host_cut)
        {
            row->ams_Address =
                ami_ns_mdns_chase(ns, out, (written < max) ? written : max,
                                  row->ams_Host, svc->service_host, &budget);
        }

        /* Past the array of the caller the identity is still retained and the
           row is counted, so a list that had to stop reports an exact count. */
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

const char *netstack_mdns_hostname(VOID)
{
    AmiNetStack *ns = ami_netstack_raw();

    if (ns == NULL || !ns->ns_MdnsCreated || !ns->ns_MdnsClaimed)
        return NULL;

    /* The claimed name, not the configured one: the two differ after a
       collision, and callers must show what the network answers to. */
    return (const char *)ns->ns_Mdns.nx_mdns_host_name;
}

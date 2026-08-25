/*
 * ConfigureNetInterface, re-address a running interface in place.
 *
 * SPDX-License-Identifier: MIT
 */

#include "tools.h"

const char *const tool_name = "ConfigureNetInterface";

static const char version_tag[] __attribute__((used)) =
    TOOL_VERSTAG("ConfigureNetInterface");

#define TEMPLATE    "INTERFACE/A,QUIET/S,ADDRESS/K,NETMASK/K,GATEWAY/K,"     \
                    "ADDRESS6/K,GATEWAY6/K,MDNS/K,CONFIGURE/K,CONFIGURE6/K," \
                    "RELEASE=RELEASEADDRESS/S,TIMEOUT/K/N"

enum
{
    ARG_INTERFACE = 0,
    ARG_QUIET,
    ARG_ADDRESS,
    ARG_NETMASK,
    ARG_GATEWAY,
    ARG_ADDRESS6,
    ARG_GATEWAY6,
    ARG_MDNS,
    ARG_CONFIGURE,
    ARG_CONFIGURE6,
    ARG_RELEASE,
    ARG_TIMEOUT,
    ARG_COUNT
};

#define CNI_DHCP_TIMEOUT    60
#define CNI_DHCP_TIMEOUT_MIN 10

/* bsdsocket.library reports the BSD errno numbers, which newlib's <errno.h>
   does not agree with everywhere; its macros must not be used here. */
#define CNI_EIO             5
#define CNI_ENXIO           6
#define CNI_EBUSY           16
#define CNI_EINVAL          22
#define CNI_ENOBUFS         55
#define CNI_EADDRNOTAVAIL   49
#define CNI_ENOTCONN        57
#define CNI_ENOSYS          78

/* Sized here rather than from nx_user.h: those constants exist only in an
   AMINETXDUO_IPV6 build and this command is one binary for either library. */
#define CNI_MAX_ROUTES6     16
#define CNI_MAX_ADDRS6      12

/* An IPv6 address written out, with room for "/128". */
#define CNI_IP6_STRLEN      52

static BOOL cni_quiet;

static VOID say(const char *fmt, ...)
{
    va_list args;

    if (cni_quiet)
        return;

    va_start(args, fmt);
    VPrintf((CONST_STRPTR)fmt, (APTR)args);         /* (APTR): see tool_util.c */
    va_end(args);
}

/* Static: two of these are most of a Shell command's 4 KB stack on their own. */
static struct
{
    NetStatusHeader     hdr;
    NetStatusInterface  e[NX_MAX_PHYSICAL_INTERFACES];
} cni_ifaces;

static struct
{
    NetStatusHeader hdr;
    NetStatusDhcp   e[NX_MAX_PHYSICAL_INTERFACES];
} cni_dhcp;

/* Read one at a time and never across each other; cni_ifaces and cni_dhcp are
   read while these are live. */
static union
{
    struct { NetStatusHeader hdr; NetStatusSystem   e; } system;
    struct { NetStatusHeader hdr; NetStatusRoute6   e[CNI_MAX_ROUTES6]; } route6;
    struct { NetStatusHeader hdr; NetStatusAddress6 e[CNI_MAX_ADDRS6]; } addr6;
} cni_v6;

static BOOL same_name(const char *a, const char *b)
{
    while (*a != '\0' && *b != '\0')
    {
        char ca = *a++;
        char cb = *b++;

        if (ca >= 'A' && ca <= 'Z')
            ca = (char)(ca + 32);
        if (cb >= 'A' && cb <= 'Z')
            cb = (char)(cb + 32);
        if (ca != cb)
            return FALSE;
    }

    return (*a == '\0' && *b == '\0') ? TRUE : FALSE;
}

/* The running stack's own list. -2 when the library did not answer, -1 when
   the name is not there. */
static LONG find_index(struct Library *base, const char *name)
{
    LONG n;
    LONG i;

    n = tool_netstatus_query(base, NETSTATUS_INTERFACES, &cni_ifaces,
                             sizeof(cni_ifaces), sizeof(NetStatusInterface));
    if (n < 0)
        return -2;

    /* nsh_Count is the library's number, not ours: bound it by the table it is
       being used to index, as removenetinterface.c and tool_nx.c do. */
    for (i = 0; i < n && i < (LONG)NX_MAX_PHYSICAL_INTERFACES; i++)
    {
        if (!(cni_ifaces.e[i].nsi_Flags & NETSTATUS_IF_NAMED))
            continue;

        if (same_name(cni_ifaces.e[i].nsi_Name, name))
            return (LONG)cni_ifaces.e[i].nsi_Index;
    }

    return -1;
}

/* The entry for `index` in the snapshot find_index() last took, or NULL. */
static const NetStatusInterface *iface_row(LONG index)
{
    LONG i;

    for (i = 0; i < (LONG)cni_ifaces.hdr.nsh_Count &&
                i < (LONG)NX_MAX_PHYSICAL_INTERFACES; i++)
    {
        if ((LONG)cni_ifaces.e[i].nsi_Index == index)
            return &cni_ifaces.e[i];
    }

    return NULL;
}

/* The DHCP row for `index` in a freshly taken snapshot, or NULL. */
static const NetStatusDhcp *dhcp_row(struct Library *base, LONG index)
{
    LONG n;
    LONG i;

    n = tool_netstatus_query(base, NETSTATUS_DHCP, &cni_dhcp,
                             sizeof(cni_dhcp), sizeof(NetStatusDhcp));
    if (n < 0)
        return NULL;

    for (i = 0; i < n && i < (LONG)NX_MAX_PHYSICAL_INTERFACES; i++)
    {
        if ((LONG)cni_dhcp.e[i].nsd_Index == index)
            return &cni_dhcp.e[i];
    }

    return NULL;
}

/* TRUE when the DHCP client is running on this interface, bound or trying. */
static BOOL on_dhcp(struct Library *base, LONG index)
{
    const NetStatusDhcp *d = dhcp_row(base, index);

    return (BOOL)(d != NULL && d->nsd_State != NETSTATUS_DHCP_OFF);
}

/* TRUE when this interface holds a lease right now. */
static BOOL dhcp_bound(struct Library *base, LONG index)
{
    const NetStatusDhcp *d = dhcp_row(base, index);

    return (BOOL)(d != NULL && d->nsd_State == NETSTATUS_DHCP_BOUND);
}

/*
 * Wait up to `seconds` for the client on `index` to reach
 * NETSTATUS_DHCPRAW_BOUND. The RAW state is watched, not nsd_State: BOUND,
 * RENEWING and REBINDING are all NETSTATUS_DHCP_BOUND.
 */
static BOOL wait_for_lease(struct Library *base, LONG index, ULONG seconds)
{
    ULONG waited;

    for (waited = 0; waited <= seconds * 2UL; waited++)
    {
        const NetStatusDhcp *d = dhcp_row(base, index);

        if (d != NULL && d->nsd_RawState == NETSTATUS_DHCPRAW_BOUND)
            return TRUE;

        if (tool_delay_ticks(25))       /* half a second. Ctrl-C ends it */
            return FALSE;
    }

    return FALSE;
}

/* "192.168.1.5" or "192.168.1.5/24"; a prefix length is written to *mask and
   *have_mask. */
static BOOL parse_address(const char *text, ULONG *addr, ULONG *mask,
                          BOOL *have_mask)
{
    char  copy[64];
    ULONG i;
    LONG  slash = -1;

    if (text == NULL || text[0] == '\0')
        return FALSE;

    for (i = 0; text[i] != '\0'; i++)
    {
        if (i + 1 >= sizeof(copy))
            return FALSE;
        if (text[i] == '/')
            slash = (LONG)i;
        copy[i] = text[i];
    }
    copy[i] = '\0';

    if (slash >= 0)
    {
        ULONG bits = 0;
        ULONG j;

        copy[slash] = '\0';

        if (copy[slash + 1] == '\0')
            return FALSE;

        for (j = (ULONG)slash + 1; copy[j] != '\0'; j++)
        {
            if (copy[j] < '0' || copy[j] > '9')
                return FALSE;
            bits = bits * 10UL + (ULONG)(copy[j] - '0');
            if (bits > 32UL)
                return FALSE;
        }

        *mask      = (bits == 0) ? 0UL : (0xFFFFFFFFUL << (32UL - bits));
        *have_mask = TRUE;
    }

    return ami_config_parse_ip(copy, addr) ? TRUE : FALSE;
}

/* A mask has to be a run of ones followed by a run of zeroes. */
static BOOL mask_is_contiguous(ULONG mask)
{
    ULONG inverted = ~mask;

    return (BOOL)((inverted & (inverted + 1UL)) == 0UL);
}

/* One NETCTRL call on `index`, with the block filled the same way every time. */
static LONG control(struct Library *base, ULONG op, LONG index, ULONG dest,
                    ULONG mask, ULONG gw, ULONG flags, LONG *err)
{
    NetStatusControl ctl;
    ULONG            w;

    for (w = 0; w < (ULONG)(sizeof(ctl) / sizeof(ULONG)); w++)
        ((ULONG *)&ctl)[w] = 0;

    ctl.nsc_Magic       = AMI_NETSTATUS_MAGIC;
    ctl.nsc_Version     = (UWORD)AMI_NETSTATUS_VERSION;
    ctl.nsc_Index       = (UWORD)index;
    ctl.nsc_Destination = dest;
    ctl.nsc_NetMask     = mask;
    ctl.nsc_Gateway     = gw;
    ctl.nsc_Flags       = flags;

    return tool_netstatus_control(base, op, &ctl, err);
}

/* TRUE when the running library has IPv6 in it at all. */
static BOOL stack_has_ipv6(struct Library *base)
{
    if (tool_netstatus_query(base, NETSTATUS_SYSTEM, &cni_v6,
                             sizeof(cni_v6.system), sizeof(NetStatusSystem))
            <= 0)
    {
        return FALSE;
    }

    return (cni_v6.system.e.nss_Flags & NETSTATUS_SYS_IPV6) ? TRUE : FALSE;
}

/* "fe80::1%eth0", which AddNetRoute takes and this command has no use for. */
static BOOL has_zone(const char *text)
{
    ULONG i;

    for (i = 0; text[i] != '\0'; i++)
    {
        if (text[i] == '%')
            return TRUE;
    }

    return FALSE;
}

static BOOL same_address6(const ULONG a[4], const ULONG b[4])
{
    return (BOOL)(a[0] == b[0] && a[1] == b[1] &&
                  a[2] == b[2] && a[3] == b[3]);
}

/* NETCTRL_ROUTE6_ADD and _DELETE. Only the default route is asked for here:
   a next hop, no destination and no prefix. */
static LONG control6(struct Library *base, ULONG op, LONG index,
                     const ULONG gw6[4], LONG *err)
{
    NetStatusControl ctl;
    ULONG            w;

    for (w = 0; w < (ULONG)(sizeof(ctl) / sizeof(ULONG)); w++)
        ((ULONG *)&ctl)[w] = 0;

    ctl.nsc_Magic       = AMI_NETSTATUS_MAGIC;
    ctl.nsc_Version     = (UWORD)AMI_NETSTATUS_VERSION;
    ctl.nsc_Index       = (UWORD)index;
    ctl.nsc_Gateway6[0] = gw6[0];
    ctl.nsc_Gateway6[1] = gw6[1];
    ctl.nsc_Gateway6[2] = gw6[2];
    ctl.nsc_Gateway6[3] = gw6[3];

    return tool_netstatus_control(base, op, &ctl, err);
}

/*
 * One live IPv6 default router on `index`: `match` TRUE finds `want`, FALSE
 * finds any other. Re-read on every call -- NETSTATUS_ROUTES6 is a snapshot of
 * a table the stack edits, and a deletion renumbers what is left of it.
 */
static BOOL find_router6(struct Library *base, LONG index, const ULONG *want,
                         BOOL match, ULONG out[4])
{
    LONG n;
    LONG i;

    n = tool_netstatus_query(base, NETSTATUS_ROUTES6, &cni_v6,
                             sizeof(cni_v6.route6), sizeof(NetStatusRoute6));

    for (i = 0; i < n && i < (LONG)CNI_MAX_ROUTES6; i++)
    {
        const NetStatusRoute6 *r = &cni_v6.route6.e[i];
        BOOL                   same;

        if (!(r->nsr6_Flags & NETSTATUS_RT6_GATEWAY))
            continue;

        if ((LONG)r->nsr6_Interface != index)
            continue;

        same = (BOOL)(want != NULL && same_address6(r->nsr6_NextHop, want));

        if (match ? !same : same)
            continue;

        if (out != NULL)
        {
            out[0] = r->nsr6_NextHop[0];
            out[1] = r->nsr6_NextHop[1];
            out[2] = r->nsr6_NextHop[2];
            out[3] = r->nsr6_NextHop[3];
        }

        return TRUE;
    }

    return FALSE;
}

/* TRUE when `addr` is already a default router on this interface. */
static BOOL has_router6(struct Library *base, LONG index, const ULONG addr[4])
{
    return find_router6(base, index, addr, TRUE, NULL);
}

/* Every IPv6 default router on `index` except `keep`. Bounded by the table
   size, so a delete that reports success without removing anything ends. */
static BOOL drop_routers6(struct Library *base, LONG index, const ULONG *keep,
                          ULONG *dropped, LONG *err)
{
    ULONG pass;

    *dropped = 0;

    for (pass = 0; pass < (ULONG)CNI_MAX_ROUTES6; pass++)
    {
        ULONG gone[4];

        if (!find_router6(base, index, keep, FALSE, gone))
            return TRUE;

        if (control6(base, NETCTRL_ROUTE6_DELETE, index, gone, err) != 0)
            return FALSE;

        (*dropped)++;
    }

    return TRUE;
}

/* What this interface holds in IPv6, read back from the stack. */
static VOID say_addresses6(struct Library *base, LONG index, const char *name)
{
    LONG n;
    LONG i;
    LONG shown = 0;

    n = tool_netstatus_query(base, NETSTATUS_ADDRESSES6, &cni_v6,
                             sizeof(cni_v6.addr6), sizeof(NetStatusAddress6));

    for (i = 0; i < n && i < (LONG)CNI_MAX_ADDRS6; i++)
    {
        const NetStatusAddress6 *a = &cni_v6.addr6.e[i];
        char                     text[CNI_IP6_STRLEN];
        const char              *note;

        if ((LONG)a->nsn_Interface != index)
            continue;

        tool_format_ip6(a->nsn_Address, text, sizeof(text));

        /* Tentative is duplicate address detection still running, which is a
           second or two after an interface comes up and is the one state a
           reader must not take for usable. */
        note = (a->nsn_State == NETSTATUS_IP6_TENTATIVE) ? " (tentative)"
             : (a->nsn_State == NETSTATUS_IP6_DEPRECATED) ? " (deprecated)"
             : "";

        say("%s: %s/%lu%s\n", (LONG)name, (LONG)text, a->nsn_PrefixLength,
            (LONG)note);
        shown++;
    }

    if (shown == 0)
        say("%s: no IPv6 address\n", (LONG)name);
}

int main(int argc, char **argv)
{
    LONG             args[ARG_COUNT];
    struct RDArgs   *rda;
    struct Library  *base;
    const char      *name;
    ULONG            address = 0;
    ULONG            netmask = 0;
    ULONG            gateway = 0;
    BOOL             have_address = FALSE;
    BOOL             have_netmask = FALSE;
    BOOL             have_gateway = FALSE;
    ULONG            gateway6[4]  = { 0, 0, 0, 0 };
    BOOL             have_gateway6 = FALSE;
    BOOL             clear_gateway6 = FALSE;
    BOOL             have_mdns    = FALSE;
    BOOL             mdns_on      = FALSE;
    BOOL             want_dhcp    = FALSE;
    BOOL             want_release = FALSE;
    ULONG            timeout      = CNI_DHCP_TIMEOUT;
    LONG             index;
    LONG             err = 0;
    char             text[16];
    const NetStatusInterface *row;

    (VOID)argv;

    if (tool_from_workbench(argc))
        return RETURN_FAIL;

    tool_break_arm();

    args[ARG_INTERFACE] = 0;
    args[ARG_QUIET]     = 0;
    args[ARG_ADDRESS]   = 0;
    args[ARG_NETMASK]   = 0;
    args[ARG_GATEWAY]   = 0;
    args[ARG_ADDRESS6]  = 0;
    args[ARG_GATEWAY6]  = 0;
    args[ARG_MDNS]      = 0;
    args[ARG_CONFIGURE] = 0;
    args[ARG_CONFIGURE6] = 0;
    args[ARG_RELEASE]   = 0;
    args[ARG_TIMEOUT]   = 0;

    rda = ReadArgs((CONST_STRPTR)TEMPLATE, args, NULL);
    if (rda == NULL)
    {
        tool_fault(IoErr());
        tool_usage("<interface> [QUIET] [ADDRESS <a>[/<bits>]] [NETMASK <m>] "
                   "[GATEWAY <g>|NONE] [ADDRESS6 <a>] [GATEWAY6 <g>|NONE] "
                   "[MDNS YES|NO] [CONFIGURE DHCP] [CONFIGURE6 <mode>] "
                   "[RELEASE] [TIMEOUT <secs>]",
                   "Change what a running interface is addressed with.");
        return RETURN_ERROR;
    }

    cni_quiet = (args[ARG_QUIET] != 0) ? TRUE : FALSE;

    /* The same spelling AddNetInterface and RemoveNetInterface accept: a name,
       or the path of the file it was configured from. */
    name = tool_basename((const char *)args[ARG_INTERFACE]);

    if (args[ARG_ADDRESS] != 0 &&
        !parse_address((const char *)args[ARG_ADDRESS], &address, &netmask,
                       &have_netmask))
    {
        tool_error("\"%s\" is not an address", (LONG)args[ARG_ADDRESS]);
        FreeArgs(rda);
        return RETURN_ERROR;
    }
    have_address = (args[ARG_ADDRESS] != 0) ? TRUE : FALSE;

    if (args[ARG_NETMASK] != 0)
    {
        ULONG explicit_mask = 0;

        if (!ami_config_parse_ip((const char *)args[ARG_NETMASK],
                                 &explicit_mask))
        {
            tool_error("\"%s\" is not a netmask", (LONG)args[ARG_NETMASK]);
            FreeArgs(rda);
            return RETURN_ERROR;
        }

        if (have_netmask && explicit_mask != netmask)
        {
            tool_error("the prefix lengths in ADDRESS and NETMASK do not "
                       "agree");
            FreeArgs(rda);
            return RETURN_ERROR;
        }

        netmask      = explicit_mask;
        have_netmask = TRUE;
    }

    if (have_netmask && !mask_is_contiguous(netmask))
    {
        ami_config_format_ip(netmask, text, sizeof(text));
        tool_error("%s is not a netmask: the ones must come first",
                   (LONG)text);
        FreeArgs(rda);
        return RETURN_ERROR;
    }

    if (args[ARG_GATEWAY] != 0)
    {
        const char *g = (const char *)args[ARG_GATEWAY];

        have_gateway = TRUE;

        if (tool_stricmp(g, "NONE") == 0)
        {
            gateway = 0;
        }
        else if (!ami_config_parse_ip(g, &gateway))
        {
            tool_error("\"%s\" is not an address. GATEWAY NONE clears it",
                       (LONG)g);
            FreeArgs(rda);
            return RETURN_ERROR;
        }
    }

    /*
     * Refused before the library is opened and before anything else in the
     * call has been applied: a call must not re-address one family and then
     * fail in the other, leaving the machine half changed.
     */
    if (args[ARG_ADDRESS6] != 0)
    {
        tool_error("an interface's IPv6 address cannot be changed while it is "
                   "running");
        tool_printf("  Put  ADDRESS6 = %s  in DEVS:NetInterfaces/%s and bring "
                    "the interface up again:\n",
                    (LONG)args[ARG_ADDRESS6], (LONG)name);
        tool_printf("     RemoveNetInterface %s\n     AddNetInterface %s\n",
                    (LONG)name, (LONG)name);
        FreeArgs(rda);
        return RETURN_ERROR;
    }

    if (args[ARG_CONFIGURE6] != 0)
    {
        tool_error("CONFIGURE6 is decided when the interface comes up, so it "
                   "cannot be changed here");
        tool_printf("  Put  CONFIGURE6 = %s  in DEVS:NetInterfaces/%s and "
                    "bring the interface up again:\n",
                    (LONG)args[ARG_CONFIGURE6], (LONG)name);
        tool_printf("     RemoveNetInterface %s\n     AddNetInterface %s\n",
                    (LONG)name, (LONG)name);
        FreeArgs(rda);
        return RETURN_ERROR;
    }

    /* No zone on GATEWAY6: the interface is this command's first argument. */
    if (args[ARG_GATEWAY6] != 0)
    {
        const char *g = (const char *)args[ARG_GATEWAY6];

        have_gateway6 = TRUE;

        if (tool_stricmp(g, "NONE") == 0)
        {
            clear_gateway6 = TRUE;
        }
        else if (!tool_parse_ip6(g, gateway6))
        {
            tool_error("\"%s\" is not an IPv6 address. GATEWAY6 NONE clears "
                       "it", (LONG)g);

            if (has_zone(g))
                tool_printf("  The interface is the first argument here, so "
                            "the %c<name> is not needed.\n", (LONG)'%');

            FreeArgs(rda);
            return RETURN_ERROR;
        }
    }

    /* Only the two words DEVS:NetInterfaces takes; a wider set here would
       teach a spelling that fails at the next boot. */
    if (args[ARG_MDNS] != 0)
    {
        const char *m = (const char *)args[ARG_MDNS];

        have_mdns = TRUE;

        if (tool_stricmp(m, "YES") == 0)
            mdns_on = TRUE;
        else if (tool_stricmp(m, "NO") == 0)
            mdns_on = FALSE;
        else
        {
            tool_error("MDNS is YES or NO, not \"%s\"", (LONG)m);
            FreeArgs(rda);
            return RETURN_ERROR;
        }
    }

    want_release = (args[ARG_RELEASE] != 0) ? TRUE : FALSE;

    if (args[ARG_CONFIGURE] != 0)
    {
        const char *c = (const char *)args[ARG_CONFIGURE];

        if (tool_stricmp(c, "DHCP") != 0)
        {
            tool_error("CONFIGURE takes DHCP and nothing else. A link-local "
                       "address is CONFIGURE=LINKLOCAL in "
                       "DEVS:NetInterfaces/%s", (LONG)name);
            FreeArgs(rda);
            return RETURN_ERROR;
        }

        want_dhcp = TRUE;

        if (have_netmask || have_gateway)
        {
            tool_error("CONFIGURE=DHCP takes its netmask and gateway from the "
                       "server, so NETMASK and GATEWAY cannot be given with it");
            FreeArgs(rda);
            return RETURN_ERROR;
        }
    }

    if (args[ARG_TIMEOUT] != 0)
    {
        LONG seconds = *(LONG *)args[ARG_TIMEOUT];

        if (!want_dhcp)
        {
            tool_error("TIMEOUT is how long to wait for a lease, so it needs "
                       "CONFIGURE=DHCP");
            FreeArgs(rda);
            return RETURN_ERROR;
        }
        if (seconds < CNI_DHCP_TIMEOUT_MIN)
        {
            tool_error("a TIMEOUT of less than %ld seconds is too short to "
                       "tell anything about the network",
                       (LONG)CNI_DHCP_TIMEOUT_MIN);
            FreeArgs(rda);
            return RETURN_ERROR;
        }

        timeout = (ULONG)seconds;
    }

    if (!have_address && !have_netmask && !have_gateway && !have_gateway6 &&
        !have_mdns && !want_dhcp && !want_release)
    {
        tool_error("nothing to change: give ADDRESS, NETMASK, GATEWAY, "
                   "GATEWAY6, MDNS, CONFIGURE or RELEASE");
        FreeArgs(rda);
        return RETURN_ERROR;
    }

    /* Opening the library would start the stack. Starting a network to
       re-address an interface in it is not what was asked for. */
    if (!tool_stack_library_running())
    {
        say("The network is not running, so there is no interface to "
            "configure.\n");
        FreeArgs(rda);
        return RETURN_WARN;
    }

    /* FALSE: QUIET drops the report of what changed, never the reason it
       could not. */
    base = tool_netstatus_open(FALSE);
    if (base == NULL)
    {
        FreeArgs(rda);
        return RETURN_FAIL;
    }

    index = find_index(base, name);
    if (index == -2)
    {
        tool_error("the network did not say which interfaces it has");
        tool_explain_no_netstatus(base);
        tool_netstatus_close(base);
        FreeArgs(rda);
        return RETURN_FAIL;
    }
    if (index < 0)
    {
        tool_error("there is no interface called \"%s\"", (LONG)name);
        tool_netstatus_close(base);
        FreeArgs(rda);
        return RETURN_FAIL;
    }

    /* RELEASE first, so `RELEASE CONFIGURE=DHCP` gives the lease back and
       asks for a new one over the route the address below replaces. */
    if (want_release)
    {
        if (control(base, NETCTRL_DHCP_RELEASE, index, 0, 0, 0, 0, &err) != 0)
        {
            if (err == CNI_ENOTCONN)
                tool_error("%s has no lease to release", (LONG)name);
            else
                tool_error("%s did not release its lease", (LONG)name);

            tool_netstatus_close(base);
            FreeArgs(rda);
            return RETURN_FAIL;
        }

        say("%s: the lease is released. The address stays until something "
            "else changes it\n", (LONG)name);
    }

    if (want_dhcp)
    {
        BOOL  renewing = dhcp_bound(base, index);
        ULONG op       = renewing ? NETCTRL_DHCP_RENEW : NETCTRL_DHCP_START;

        if (control(base, op, index, address, 0, 0,
                    (have_address && !renewing) ? NETCTRL_F_ADDRESS : 0UL,
                    &err) != 0)
        {
            if (err == CNI_ENOTCONN)
                tool_error("%s has no lease to renew", (LONG)name);
            else if (err == CNI_EBUSY)
                tool_error("%s is already asking a DHCP server for an "
                           "address. Wait for that request to be answered or "
                           "to give up, then ask again", (LONG)name);
            else
                tool_error("%s has no DHCP client to ask", (LONG)name);

            tool_netstatus_close(base);
            FreeArgs(rda);
            return RETURN_FAIL;
        }

        if (!wait_for_lease(base, index, timeout))
        {
            tool_error("%s: no answer from a DHCP server within %lu seconds",
                       (LONG)name, timeout);
            tool_explain_dhcp(name);
            tool_netstatus_close(base);
            FreeArgs(rda);
            return RETURN_FAIL;
        }

        if (find_index(base, name) >= 0 && (row = iface_row(index)) != NULL)
        {
            const NetStatusDhcp *d = dhcp_row(base, index);
            char                 server[16];

            ami_config_format_ip(row->nsi_Address, text, sizeof(text));
            ami_config_format_ip((d != NULL) ? d->nsd_Server : 0UL,
                                 server, sizeof(server));

            say("%s: %s %s, from %s\n", (LONG)name,
                (LONG)(renewing ? "lease renewed," : "lease taken,"),
                (LONG)text, (LONG)server);
        }
    }

    /* With CONFIGURE=DHCP the ADDRESS was already asked for above; writing it
       on the interface here would overwrite whatever the server granted. */
    if (!want_dhcp && (have_address || have_netmask || have_gateway))
    {
        if (control(base, NETCTRL_INTERFACE_CONFIGURE, index, address, netmask,
                    gateway,
                    (have_address ? NETCTRL_F_ADDRESS : 0UL) |
                    (have_netmask ? NETCTRL_F_NETMASK : 0UL) |
                    (have_gateway ? NETCTRL_F_GATEWAY : 0UL), &err) != 0)
        {
            if (err == CNI_EADDRNOTAVAIL)
            {
                ami_config_format_ip(address, text, sizeof(text));
                tool_error("%s did not take %s", (LONG)name, (LONG)text);
            }
            else if (err == CNI_EINVAL && have_gateway && gateway != 0)
            {
                ami_config_format_ip(gateway, text, sizeof(text));
                tool_error("%s is not on any of this machine's own subnets, "
                           "so nothing here can reach it", (LONG)text);
            }
            else
            {
                tool_error("%s was not reconfigured", (LONG)name);
            }

            tool_netstatus_close(base);
            FreeArgs(rda);
            return RETURN_FAIL;
        }

        /* Report what the interface now has rather than what was asked for. */
        if (find_index(base, name) >= 0 && (row = iface_row(index)) != NULL)
        {
            char maskbuf[16];

            ami_config_format_ip(row->nsi_Address, text, sizeof(text));
            ami_config_format_ip(row->nsi_NetMask, maskbuf, sizeof(maskbuf));
            say("%s: %s netmask %s\n", (LONG)name, (LONG)text, (LONG)maskbuf);
        }

        if (have_gateway)
        {
            if (gateway == 0)
            {
                say("%s: the default gateway is cleared\n", (LONG)name);
            }
            else
            {
                ami_config_format_ip(gateway, text, sizeof(text));
                say("%s: the default gateway is %s\n", (LONG)name, (LONG)text);
            }
        }
    }

    /*
     * After the IPv4 half: a next hop is only reachable once the addresses
     * around it are what they are going to be. A router is REPLACED, not
     * added to; AddNetRoute DEFAULTGATEWAY adds a second.
     */
    if (have_gateway6)
    {
        ULONG dropped = 0;
        BOOL  already6;
        char  gwtext[CNI_IP6_STRLEN];

        tool_format_ip6(gateway6, gwtext, sizeof(gwtext));

        if (!stack_has_ipv6(base))
        {
            tool_error("this bsdsocket.library was built without IPv6, so "
                       "there is no IPv6 route to change");
            tool_netstatus_close(base);
            FreeArgs(rda);
            return RETURN_FAIL;
        }

        /* Asked for what it already has: keep it rather than remove and
           re-add, which would drop the route for the moment in between. */
        already6 = (BOOL)(!clear_gateway6 &&
                          has_router6(base, index, gateway6));

        /* Every router on this interface except the one being asked for.
           GATEWAY6 NONE keeps none. */
        if (!drop_routers6(base, index, clear_gateway6 ? NULL : gateway6,
                           &dropped, &err))
        {
            tool_error("%s: the IPv6 default router was not removed",
                       (LONG)name);
            tool_netstatus_close(base);
            FreeArgs(rda);
            return RETURN_FAIL;
        }

        if (clear_gateway6)
        {
            if (dropped == 0)
                say("%s: there was no IPv6 default router to clear\n",
                    (LONG)name);
            else
                say("%s: the IPv6 default router is cleared\n", (LONG)name);
        }
        else if (already6)
        {
            say("%s: the IPv6 default router is %s\n", (LONG)name,
                (LONG)gwtext);
        }
        else
        {
            if (control6(base, NETCTRL_ROUTE6_ADD, index, gateway6, &err) != 0)
            {
                if (err == CNI_ENOSYS)
                    tool_error("this bsdsocket.library was built without "
                               "IPv6, so there is no IPv6 route to change");
                else if (err == CNI_ENOBUFS)
                    tool_error("this stack holds no more default routers, so "
                               "%s was not added", (LONG)gwtext);
                else if (err == CNI_EINVAL)
                    tool_error("%s was refused as a next hop for %s",
                               (LONG)gwtext, (LONG)name);
                else
                    tool_error("%s: the IPv6 default router was not set to %s",
                               (LONG)name, (LONG)gwtext);

                if (dropped != 0)
                    tool_printf("  The router it had was removed first, so "
                                "there is none now.\n");

                tool_netstatus_close(base);
                FreeArgs(rda);
                return RETURN_FAIL;
            }

            say("%s: the IPv6 default router is %s\n", (LONG)name,
                (LONG)gwtext);
        }

        say_addresses6(base, index, name);
    }

    /* After the address: the responder claims whatever the interface holds
       when it is enabled. */
    if (have_mdns)
    {
        if (control(base, NETCTRL_INTERFACE_MDNS, index, 0, 0, 0,
                    mdns_on ? (ULONG)NETCTRL_F_MDNS : 0UL, &err) != 0)
        {
            if (err == CNI_ENOSYS)
                tool_error("this bsdsocket.library was built without mDNS, "
                           "so there is nothing to switch");
            else if (err == CNI_EIO)
                tool_error("%s did not %s answering .local", (LONG)name,
                           (LONG)(mdns_on ? "start" : "stop"));
            else if (err == CNI_ENXIO)
                tool_error("%s is no longer attached", (LONG)name);
            else
                tool_error("%s: MDNS=%s was refused", (LONG)name,
                           (LONG)(mdns_on ? "YES" : "NO"));

            tool_netstatus_close(base);
            FreeArgs(rda);
            return RETURN_FAIL;
        }

        /* Probing is not waited for, so this says what was started. */
        if (mdns_on)
            say("%s: answering .local here, and the name is claimed now\n",
                (LONG)name);
        else
            say("%s: no longer answering .local here, and the network was "
                "told to forget the name\n", (LONG)name);
    }

    /* Said last, so it is the line left on the screen. */
    if (!want_dhcp && (have_address || have_netmask || have_gateway) &&
        on_dhcp(base, index))
    {
        say("%s takes its address by DHCP, so the next lease writes over "
            "this.\n", (LONG)name);
    }

    tool_netstatus_close(base);
    FreeArgs(rda);

    return RETURN_OK;
}

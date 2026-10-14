/*
 * ConfigureNetInterface, re-address a running interface in place.
 *
 * SPDX-License-Identifier: MIT
 */

#include "tools.h"
#include "routematch.h"

/* IFC_LimitMTU, IFC_State and the four SM_ states.  <sys/types.h> first:
   libraries/bsdsocket.h reaches sys/socket.h, which names ssize_t. */
#include <sys/types.h>
#include <libraries/bsdsocket.h>

const char *const tool_name = "ConfigureNetInterface";

static const char version_tag[] __attribute__((used)) =
    TOOL_VERSTAG("ConfigureNetInterface");

#define TEMPLATE    "INTERFACE/A,QUIET/S,ADDRESS/K,NETMASK/K,GATEWAY/K,"     \
                    "ADDRESS6/K,GATEWAY6/K,MDNS/K,CONFIGURE/K,CONFIGURE6/K," \
                    "RELEASE=RELEASEADDRESS/S,TIMEOUT/K/N,MTU/K/N,"          \
                    "ONLINE/S,OFFLINE/S,UP/S,DOWN/S,PRIORITY/K/N"

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
    ARG_MTU,
    ARG_ONLINE,
    ARG_OFFLINE,
    ARG_UP,
    ARG_DOWN,
    ARG_PRIORITY,
    ARG_COUNT
};

/* PRIORITY= is a signed byte in the interface file (config.h) and here. */
#define CNI_PRIORITY_MIN    (-128)
#define CNI_PRIORITY_MAX    127

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
#define CNI_ENETDOWN        50

/* RFC 791 2: 68 bytes is the smallest datagram every host must carry. */
#define CNI_MTU_MIN         68

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

/* Every operation is synchronous and this command has one task.  Keeping the
   control block here stops main()'s frame and control()'s frame being charged
   together against the Shell's 4 KB stack. */
static NetStatusControl cni_control;

/* ReadArgs storage and the two parse/format temporaries also outlive no call,
   but making them static keeps the long option parser from taxing every later
   control path through main(). */
static LONG  args[ARG_COUNT];
static ULONG cni_gateway6[4];
static char  cni_text[16];

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

/* NETSTATUS_DHCPRAW_*, in order, so a refusal can name the state it is in. */
static const char *raw_state_name(UWORD raw)
{
    static const char *const names[] = {
        "not started", "booting",  "starting",  "selecting", "requesting",
        "bound",       "renewing", "rebinding", "forcerenew", "probing"
    };

    if (raw >= (UWORD)(sizeof(names) / sizeof(names[0])))
        return "unknown";

    return names[raw];
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
 * RENEWING and REBINDING are all NETSTATUS_DHCP_BOUND.  1 bound, 0 the wait
 * ran out (the client keeps asking), -1 Ctrl-C (F-146).
 */
static LONG wait_for_lease(struct Library *base, LONG index, ULONG seconds)
{
    ULONG waited;

    for (waited = 0; waited <= seconds * 2UL; waited++)
    {
        const NetStatusDhcp *d = dhcp_row(base, index);

        if (d != NULL && d->nsd_RawState == NETSTATUS_DHCPRAW_BOUND)
            return 1;

        if (tool_delay_ticks(25))       /* half a second. Ctrl-C ends it */
            return -1;
    }

    return 0;
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
    ULONG w;

    for (w = 0; w < (ULONG)(sizeof(cni_control) / sizeof(ULONG)); w++)
        ((ULONG *)&cni_control)[w] = 0;

    cni_control.nsc_Magic       = AMI_NETSTATUS_MAGIC;
    cni_control.nsc_Version     = (UWORD)AMI_NETSTATUS_VERSION;
    cni_control.nsc_Index       = (UWORD)index;
    cni_control.nsc_Destination = dest;
    cni_control.nsc_NetMask     = mask;
    cni_control.nsc_Gateway     = gw;
    cni_control.nsc_Flags       = flags;

    return tool_netstatus_control(base, op, &cni_control, err);
}

/* Keep this operation's control block off main()'s frame.  main also calls
   control(), and putting both blocks there makes their mutually exclusive
   paths consume stack at the same time. */
static LONG control_priority(struct Library *base, LONG index, LONG priority,
                             LONG *err)
{
    ULONG w;

    for (w = 0; w < (ULONG)(sizeof(cni_control) / sizeof(ULONG)); w++)
        ((ULONG *)&cni_control)[w] = 0;

    cni_control.nsc_Magic    = AMI_NETSTATUS_MAGIC;
    cni_control.nsc_Version  = (UWORD)AMI_NETSTATUS_VERSION;
    cni_control.nsc_Index    = (UWORD)index;
    cni_control.nsc_Priority = priority;

    return tool_netstatus_control(base, NETCTRL_INTERFACE_PRIORITY,
                                  &cni_control, err);
}


/*
 * THE INTERFACE'S STATE AND ITS MTU, through ConfigureInterfaceTagList().
 *
 * The library implements all four of Roadshow's states behind that vector,
 * in the order Roadshow documents -- SM_Online before the rest of the call,
 * SM_Up after it -- and clamps the MTU to what the hardware will carry.  This
 * command asks for them there rather than building a second set of semantics
 * on NetStackControl(), which is what let the address half of the two drift
 * apart in the first place.
 *
 * ONLINE puts the driver back on the wire; OFFLINE takes it off.  UP and DOWN
 * are the protocol's own state: DOWN stops transmitting and leaves the device
 * on the network, so the readers keep feeding a stack that does not answer,
 * unless the interface file says DOWNGOESOFFLINE.
 */
static BOOL apply_state(struct Library *base, const char *name, BOOL have_mtu,
                        ULONG mtu, BOOL have_state, LONG state, LONG *err)
{
    struct TagItem tags[3];
    ULONG          n = 0;

    if (have_mtu)
    {
        tags[n].ti_Tag  = IFC_LimitMTU;
        tags[n].ti_Data = (ULONG)mtu;
        n++;
    }

    if (have_state)
    {
        tags[n].ti_Tag  = IFC_State;
        tags[n].ti_Data = (ULONG)state;
        n++;
    }

    if (n == 0)
        return TRUE;

    tags[n].ti_Tag  = TAG_DONE;
    tags[n].ti_Data = 0;

    return (BOOL)(tool_configure_interface(base, name, tags, err) == 0);
}

/* What the four switches are called on the screen, in the order asked for. */
static const char *state_word(LONG state)
{
    switch (state)
    {
        case SM_Online:  return "online";
        case SM_Offline: return "offline";
        case SM_Up:      return "up";
        default:         return "down";
    }
}

/* 1 when the running library has IPv6 in it at all, 0 when it was built
   without, -1 when it would not say (route6_stack_ipv6). */
static LONG stack_has_ipv6(struct Library *base)
{
    LONG n = tool_netstatus_query(base, NETSTATUS_SYSTEM, &cni_v6,
                                  sizeof(cni_v6.system),
                                  sizeof(NetStatusSystem));

    return route6_stack_ipv6(n, (n > 0) ? (ULONG)cni_v6.system.e.nss_Flags
                                        : 0UL);
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
 * One live IPv6 default router on `index` (route6_find_router): 1, 0, or -1
 * when the table could not be read.  Re-read on every call --
 * NETSTATUS_ROUTES6 is a snapshot of a table the stack edits, and a deletion
 * renumbers what is left of it.
 */
static LONG find_router6(struct Library *base, LONG index, const ULONG *want,
                         BOOL match, ULONG out[4])
{
    LONG n = tool_netstatus_query(base, NETSTATUS_ROUTES6, &cni_v6,
                                  sizeof(cni_v6.route6),
                                  sizeof(NetStatusRoute6));

    return route6_find_router(cni_v6.route6.e, n, (LONG)CNI_MAX_ROUTES6,
                              index, want, match, out);
}

/* Every IPv6 default router on `index` except `keep`. Bounded by the table
   size, so a delete that reports success without removing anything ends.
   FALSE when a delete was refused, or the table could not be read: then
   nothing is known to be gone, and the caller must not add or report as if
   it were. */
static BOOL drop_routers6(struct Library *base, LONG index, const ULONG *keep,
                          ULONG *dropped, LONG *err)
{
    ULONG pass;

    *dropped = 0;

    for (pass = 0; pass < (ULONG)CNI_MAX_ROUTES6; pass++)
    {
        ULONG gone[4];
        LONG  found = find_router6(base, index, keep, FALSE, gone);

        if (found < 0)
            return FALSE;
        if (found == 0)
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

static int cni_main(int argc, char **argv);

int main(int argc, char **argv)
{
    return (int)tool_quiet_end(cni_main(argc, argv));
}

static int cni_main(int argc, char **argv)
{
    struct RDArgs   *rda;
    struct Library  *base;
    const char      *name;
    ULONG            address = 0;
    ULONG            netmask = 0;
    ULONG            gateway = 0;
    BOOL             have_address = FALSE;
    BOOL             have_netmask = FALSE;
    BOOL             have_gateway = FALSE;
    BOOL             have_gateway6 = FALSE;
    BOOL             clear_gateway6 = FALSE;
    BOOL             have_mdns    = FALSE;
    BOOL             mdns_on      = FALSE;
    BOOL             want_dhcp    = FALSE;
    BOOL             want_release = FALSE;
    ULONG            timeout      = CNI_DHCP_TIMEOUT;
    BOOL             have_mtu     = FALSE;
    BOOL             have_priority = FALSE;
    LONG             priority     = 0;
    ULONG            mtu          = 0;
    BOOL             have_state   = FALSE;
    LONG             state        = 0;
    LONG             index;
    LONG             err = 0;
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
    args[ARG_MTU]       = 0;
    args[ARG_ONLINE]    = 0;
    args[ARG_OFFLINE]   = 0;
    args[ARG_UP]        = 0;
    args[ARG_DOWN]      = 0;
    args[ARG_PRIORITY]  = 0;
    cni_gateway6[0] = 0;
    cni_gateway6[1] = 0;
    cni_gateway6[2] = 0;
    cni_gateway6[3] = 0;

    rda = ReadArgs((CONST_STRPTR)TEMPLATE, args, NULL);
    if (rda == NULL)
    {
        tool_fault(IoErr());
        tool_usage("<interface> [QUIET] [ADDRESS <a>[/<bits>]] [NETMASK <m>] "
                   "[GATEWAY <g>|NONE] [ADDRESS6 <a>] [GATEWAY6 <g>|NONE] "
                   "[MDNS YES|NO] [CONFIGURE DHCP] [CONFIGURE6 <mode>] "
                   "[RELEASE] [TIMEOUT <secs>] [MTU <bytes>] "
                   "[ONLINE|OFFLINE|UP|DOWN] [PRIORITY <n>]",
                   "Change what a running interface is addressed with.");
        return RETURN_ERROR;
    }

    cni_quiet = (args[ARG_QUIET] != 0) ? TRUE : FALSE;
    if (cni_quiet)
        tool_quiet_begin();

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
            tool_error("ADDRESS and NETMASK prefix lengths differ");
            FreeArgs(rda);
            return RETURN_ERROR;
        }

        netmask      = explicit_mask;
        have_netmask = TRUE;
    }

    if (have_netmask && !mask_is_contiguous(netmask))
    {
        ami_config_format_ip(netmask, cni_text, sizeof(cni_text));
        tool_error("%s is not a valid netmask",
                   (LONG)cni_text);
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
            tool_error("\"%s\" is not an address",
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
        tool_error("ADDRESS6 cannot be changed on a running interface");
        FreeArgs(rda);
        return RETURN_ERROR;
    }

    if (args[ARG_CONFIGURE6] != 0)
    {
        tool_error("CONFIGURE6 cannot be changed on a running interface");
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
        else if (!tool_parse_ip6(g, cni_gateway6))
        {
            if (has_zone(g))
                tool_error("\"%s\": %%zone not allowed", (LONG)g);
            else
                tool_error("\"%s\" is not an IPv6 address", (LONG)g);

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
            tool_error("CONFIGURE accepts only DHCP");
            FreeArgs(rda);
            return RETURN_ERROR;
        }

        want_dhcp = TRUE;

        if (have_netmask || have_gateway)
        {
            tool_error("NETMASK and GATEWAY cannot be used with CONFIGURE=DHCP");
            FreeArgs(rda);
            return RETURN_ERROR;
        }
    }

    if (args[ARG_TIMEOUT] != 0)
    {
        LONG seconds = *(LONG *)args[ARG_TIMEOUT];

        if (!want_dhcp)
        {
            tool_error("TIMEOUT requires CONFIGURE=DHCP");
            FreeArgs(rda);
            return RETURN_ERROR;
        }
        if (seconds < CNI_DHCP_TIMEOUT_MIN)
        {
            tool_error("TIMEOUT must be at least %ld seconds",
                       (LONG)CNI_DHCP_TIMEOUT_MIN);
            FreeArgs(rda);
            return RETURN_ERROR;
        }

        timeout = (ULONG)seconds;
    }

    /* One state, or none.  Two of them name two different interfaces to end
       up as, and applying either one silently would be a guess. */
    {
        LONG asked = (args[ARG_ONLINE]  != 0 ? 1 : 0) +
                     (args[ARG_OFFLINE] != 0 ? 1 : 0) +
                     (args[ARG_UP]      != 0 ? 1 : 0) +
                     (args[ARG_DOWN]    != 0 ? 1 : 0);

        if (asked > 1)
        {
            tool_error("ONLINE, OFFLINE, UP and DOWN are mutually exclusive");
            FreeArgs(rda);
            return RETURN_ERROR;
        }

        if (asked == 1)
        {
            have_state = TRUE;
            if (args[ARG_ONLINE] != 0)
                state = SM_Online;
            else if (args[ARG_OFFLINE] != 0)
                state = SM_Offline;
            else if (args[ARG_UP] != 0)
                state = SM_Up;
            else
                state = SM_Down;
        }
    }

    if (args[ARG_MTU] != 0)
    {
        LONG bytes = *(LONG *)args[ARG_MTU];

        /* RFC 791 2.  Below this a host cannot be required to reassemble a
           datagram, so an interface set there drops traffic it is given
           rather than carrying it slowly.  The library clamps the other end
           down to what the hardware will carry. */
        if (bytes < CNI_MTU_MIN)
        {
            tool_error("MTU %ld is below the IPv4 minimum of %ld",
                       bytes, (LONG)CNI_MTU_MIN);
            FreeArgs(rda);
            return RETURN_ERROR;
        }

        have_mtu = TRUE;
        mtu      = (ULONG)bytes;
    }

    /* The same range the interface file's PRIORITY= takes; the highest
       carries a route two interfaces could and picks the default gateway.
       0 is a value here: it puts an interface back to none. */
    if (args[ARG_PRIORITY] != 0)
    {
        priority = *(LONG *)args[ARG_PRIORITY];
        if (priority < CNI_PRIORITY_MIN || priority > CNI_PRIORITY_MAX)
        {
            tool_error("PRIORITY is %ld to %ld, not %ld",
                       (LONG)CNI_PRIORITY_MIN, (LONG)CNI_PRIORITY_MAX,
                       priority);
            FreeArgs(rda);
            return RETURN_ERROR;
        }
        have_priority = TRUE;
    }

    if (!have_address && !have_netmask && !have_gateway && !have_gateway6 &&
        !have_mdns && !want_dhcp && !want_release && !have_mtu && !have_state &&
        !have_priority)
    {
        tool_error("nothing to change");
        FreeArgs(rda);
        return RETURN_ERROR;
    }

    /* Opening the library would start the stack. Starting a network to
       re-address an interface in it is not what was asked for. */
    if (!tool_stack_library_running())
    {
        say("Network not running.\n");
        FreeArgs(rda);
        return RETURN_WARN;
    }

    base = tool_netstatus_open(FALSE);
    if (base == NULL)
    {
        FreeArgs(rda);
        return RETURN_FAIL;
    }

    index = find_index(base, name);
    if (index == -2)
    {
        tool_error("cannot read the interface list");
        tool_explain_no_netstatus(base);
        tool_netstatus_close(base);
        FreeArgs(rda);
        return RETURN_FAIL;
    }
    if (index < 0)
    {
        tool_error("no interface \"%s\"", (LONG)name);
        tool_netstatus_close(base);
        FreeArgs(rda);
        return RETURN_FAIL;
    }

    /*
     * GATEWAY6 against a library without IPv6 -- a full-build command on a
     * minimal or micro library -- is refused here, before anything below
     * changes the interface, so a mixed request is not half applied (F-147).
     */
    if (have_gateway6)
    {
        LONG v6 = stack_has_ipv6(base);

        if (v6 <= 0)
        {
            if (v6 == 0)
                tool_error("bsdsocket.library has no IPv6 support");
            else
                tool_error("cannot determine IPv6 support");
            tool_netstatus_close(base);
            FreeArgs(rda);
            return RETURN_FAIL;
        }
    }

    /*
     * ONLINE before the rest of the call, which is the order Roadshow
     * documents: an interface that was off the wire is put back on it, and
     * everything below then applies to a live interface rather than failing
     * against a dead one.
     */
    if (have_state && state == SM_Online)
    {
        if (!apply_state(base, name, FALSE, 0, TRUE, state, &err))
        {
            if (err == CNI_ENOSYS)
                tool_error("bsdsocket.library cannot change interface state");
            else
                tool_error("%s did not go online", (LONG)name);

            tool_netstatus_close(base);
            FreeArgs(rda);
            return RETURN_FAIL;
        }

        say("%s: online\n", (LONG)name);
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

        say("%s: lease released\n", (LONG)name);
    }

    if (want_dhcp)
    {
        BOOL  renewing = dhcp_bound(base, index);
        LONG  leased;
        ULONG op       = renewing ? NETCTRL_DHCP_RENEW : NETCTRL_DHCP_START;

        if (control(base, op, index, address, 0, 0,
                    (have_address && !renewing) ? NETCTRL_F_ADDRESS : 0UL,
                    &err) != 0)
        {
            if (err == CNI_ENOTCONN)
                tool_error("%s has no lease to renew", (LONG)name);
            else if (err == CNI_EBUSY)
                tool_error("%s: DHCP request already in progress", (LONG)name);
            else if (renewing)
                tool_error("%s has no DHCP client", (LONG)name);
            else
            {
                const NetStatusDhcp *d = dhcp_row(base, index);

                tool_error("%s: DHCP request not started (client %s)",
                           (LONG)name,
                           (LONG)raw_state_name((d != NULL) ? d->nsd_RawState
                                                            : (UWORD)0));
            }

            tool_netstatus_close(base);
            FreeArgs(rda);
            return RETURN_FAIL;
        }

        leased = wait_for_lease(base, index, timeout);
        if (leased <= 0)
        {
            /* The wait ended, not the request: the client keeps asking, and
               nothing after the DHCP step was applied. */
            if (leased < 0)
                tool_fault(ERROR_BREAK);
            else
                tool_explain_dhcp_waiting(name, timeout);
            tool_netstatus_close(base);
            FreeArgs(rda);
            return (leased < 0) ? RETURN_WARN : RETURN_FAIL;
        }

        if (find_index(base, name) >= 0 && (row = iface_row(index)) != NULL)
        {
            const NetStatusDhcp *d = dhcp_row(base, index);
            char                 server[16];

            ami_config_format_ip(row->nsi_Address, cni_text, sizeof(cni_text));
            ami_config_format_ip((d != NULL) ? d->nsd_Server : 0UL,
                                 server, sizeof(server));

            say("%s: %s %s, from %s\n", (LONG)name,
                (LONG)(renewing ? "lease renewed," : "lease taken,"),
                (LONG)cni_text, (LONG)server);
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
                ami_config_format_ip(address, cni_text, sizeof(cni_text));
                tool_error("%s did not take %s", (LONG)name, (LONG)cni_text);
            }
            else if (err == CNI_EINVAL && have_gateway && gateway != 0)
            {
                ami_config_format_ip(gateway, cni_text, sizeof(cni_text));
                tool_error("%s is not on a local subnet", (LONG)cni_text);
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

            ami_config_format_ip(row->nsi_Address, cni_text, sizeof(cni_text));
            ami_config_format_ip(row->nsi_NetMask, maskbuf, sizeof(maskbuf));
            say("%s: %s netmask %s\n", (LONG)name, (LONG)cni_text, (LONG)maskbuf);
        }

        if (have_gateway)
        {
            if (gateway == 0)
            {
                say("%s: default gateway cleared\n", (LONG)name);
            }
            else
            {
                ami_config_format_ip(gateway, cni_text, sizeof(cni_text));
                say("%s: default gateway %s\n", (LONG)name, (LONG)cni_text);
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

        tool_format_ip6(cni_gateway6, gwtext, sizeof(gwtext));

        /* Asked for what it already has: keep it rather than remove and
           re-add, which would drop the route for the moment in between.  A
           table that cannot be read says nothing either way (F-145). */
        if (clear_gateway6)
            already6 = FALSE;
        else
        {
            LONG found = find_router6(base, index, cni_gateway6, TRUE, NULL);

            if (found < 0)
            {
                tool_error("%s: cannot read IPv6 routes", (LONG)name);
                tool_netstatus_close(base);
                FreeArgs(rda);
                return RETURN_FAIL;
            }
            already6 = (BOOL)(found > 0);
        }

        /* Every router on this interface except the one being asked for.
           GATEWAY6 NONE keeps none. */
        if (!drop_routers6(base, index, clear_gateway6 ? NULL : cni_gateway6,
                           &dropped, &err))
        {
            tool_error("%s: IPv6 default router not removed",
                       (LONG)name);
            tool_netstatus_close(base);
            FreeArgs(rda);
            return RETURN_FAIL;
        }

        if (clear_gateway6)
        {
            if (dropped == 0)
                say("%s: no IPv6 default router\n",
                    (LONG)name);
            else
                say("%s: IPv6 default router cleared\n", (LONG)name);
        }
        else if (already6)
        {
            say("%s: IPv6 default router %s\n", (LONG)name,
                (LONG)gwtext);
        }
        else
        {
            if (control6(base, NETCTRL_ROUTE6_ADD, index, cni_gateway6, &err) != 0)
            {
                if (err == CNI_ENOSYS)
                    tool_error("bsdsocket.library has no IPv6 support");
                else if (err == CNI_ENOBUFS)
                    tool_error("default router table full; %s not added", (LONG)gwtext);
                else if (err == CNI_EINVAL)
                    tool_error("%s was refused as a next hop for %s",
                               (LONG)gwtext, (LONG)name);
                else
                    tool_error("%s: IPv6 default router not set to %s",
                               (LONG)name, (LONG)gwtext);

                if (dropped != 0)
                    tool_printf("  previous default router removed\n");

                tool_netstatus_close(base);
                FreeArgs(rda);
                return RETURN_FAIL;
            }

            say("%s: IPv6 default router %s\n", (LONG)name,
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
                tool_error("bsdsocket.library has no mDNS support");
            else if (err == CNI_EIO)
                tool_error("%s: mDNS %s failed", (LONG)name,
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
            say("%s: mDNS on\n",
                (LONG)name);
        else
            say("%s: mDNS off\n", (LONG)name);
    }

    /*
     * The MTU and the remaining three states, after the addresses: SM_Up is
     * documented to come last, and an MTU set before an address would be
     * applied to an interface the call is about to change anyway.
     */
    if (have_mtu || (have_state && state != SM_Online))
    {
        BOOL late_state = (BOOL)(have_state && state != SM_Online);

        if (!apply_state(base, name, have_mtu, mtu, late_state, state, &err))
        {
            if (err == CNI_ENOSYS)
                tool_error("bsdsocket.library cannot change MTU or interface state");
            else if (err == CNI_ENETDOWN)
                tool_error("%s is offline; MTU not set", (LONG)name);
            else if (have_mtu && !late_state)
                tool_error("%s: MTU %lu refused", (LONG)name, mtu);
            else if (have_mtu)
                tool_error("%s: MTU and %s not applied",
                           (LONG)name, (LONG)state_word(state));
            else
                tool_error("%s did not go %s", (LONG)name,
                           (LONG)state_word(state));

            tool_netstatus_close(base);
            FreeArgs(rda);
            return RETURN_FAIL;
        }

        /* What the interface now has, not what was asked for: the library
           clamps an MTU down to what the hardware carries. */
        if (have_mtu)
        {
            if (find_index(base, name) >= 0 && (row = iface_row(index)) != NULL)
                say("%s: MTU %lu\n", (LONG)name, row->nsi_MTU);
            else
                say("%s: MTU %lu set, not verified\n", (LONG)name, mtu);   /* F-148 */
        }

        if (late_state)
            say("%s: %s\n", (LONG)name, (LONG)state_word(state));
    }

    /*
     * The priority, after everything that could have changed which
     * interfaces carry an address: the library picks the default gateway
     * again from the new order the moment it is set, so what it chooses
     * sees the addresses above.
     */
    if (have_priority)
    {
        if (control_priority(base, index, priority, &err) != 0)
        {
            if (err == CNI_ENXIO)
                tool_error("%s is no longer attached", (LONG)name);
            else if (err == CNI_ENOSYS)
                tool_error("bsdsocket.library cannot change PRIORITY on a running interface");
            else
                tool_error("%s: PRIORITY %ld was refused", (LONG)name,
                           priority);

            tool_netstatus_close(base);
            FreeArgs(rda);
            return RETURN_FAIL;
        }

        say("%s: priority %ld until reboot\n", (LONG)name, priority);
    }

    /* Said last, so it is the line left on the screen. */
    if (!want_dhcp && (have_address || have_netmask || have_gateway) &&
        on_dhcp(base, index))
    {
        say("%s: DHCP; next lease overrides\n", (LONG)name);
    }

    tool_netstatus_close(base);
    FreeArgs(rda);

    return RETURN_OK;
}

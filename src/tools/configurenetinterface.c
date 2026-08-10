/*
 * ConfigureNetInterface, re-address a running interface in place.
 *
 *     C:ConfigureNetInterface eth0 [QUIET] [ADDRESS <a>[/<bits>]]
 *                                  [NETMASK <m>] [GATEWAY <g>|NONE]
 *
 * Until now the only way to change what a live interface is addressed with was
 * RemoveNetInterface followed by AddNetInterface, which closes the SANA-II
 * device, detaches the NetX Duo interface, resets every TCP connection routed
 * out of it and re-reads the file, so it can only put back what the file says.
 * This changes the three numbers and touches nothing else.
 *
 * THE NAME AND THE KEYWORDS ARE ROADSHOW'S. Its ConfigureNetInterface 4.37
 * carries this ReadArgs template:
 *
 *   INTERFACE/A,QUIET/S,ADDRESS/K,NETMASK/K,BROADCASTADDR/K,
 *   DESTINATION=DESTINATIONADDR/K,METRIC/K/N,MTU/K/N,ALIASADDR/K,DELETEADDR/K,
 *   ONLINE/S,OFFLINE/S,UP/S,DOWN/S,DEBUG/K,COMPLETE/K,CONFIGURE/K,LEASE/K,
 *   RELEASE=RELEASEADDRESS/S,ID/K,TIMEOUT/K/N,DHCPUNICAST/K
 *
 * INTERFACE, QUIET, ADDRESS and NETMASK are taken from it unchanged, in its
 * order, so a script written for Roadshow reads the same here. The rest is
 * deliberate, and each one is a decision rather than an omission:
 *
 *   GATEWAY/K          OURS, Roadshow has no such keyword. Roadshow sets the
 *                      default route from CONFIGURE=DHCP or from AddNetRoute
 *                      DEFAULTGATEWAY. It is here because the gateway is the
 *                      third number a person re-addressing a machine has to
 *                      change, and because NetX Duo refuses a gateway that is
 *                      not on an interface's own subnet -- which is exactly
 *                      what the ADDRESS in the same call has just decided, so
 *                      the two belong in one call and in this order.
 *
 *   BROADCASTADDR      NetX Duo derives the broadcast address from the address
 *                      and the mask and has no separate one to set;
 *                      IFQ_BroadcastAddress would contradict anything accepted
 *                      here. ConfigureInterfaceTagList() already refuses
 *                      IFC_BroadcastAddress for this reason.
 *   DESTINATIONADDR    the far end of a point-to-point link, which none of
 *                      these interfaces is. The SANA-II shim is Ethernet-shaped
 *                      throughout.
 *   ALIASADDR          one IPv4 address per interface here.
 *   DELETEADDR
 *   METRIC             there is no routing protocol in this stack to spend a
 *                      cost, and IFQ_Metric answers zero for every interface.
 *   MTU                ConfigureInterfaceTagList()'s IFC_LimitMTU does this and
 *                      nothing drives it yet; a keyword here would be a second
 *                      way in before the first one has a test.
 *   ONLINE/OFFLINE     Online and Offline are commands of their own here, with
 *   UP/DOWN            their own regression test over the whole of that
 *                      surface (tests/tools/run-onoff.sh). Two ways to switch
 *                      an interface would be two things to keep agreeing.
 *   DEBUG, COMPLETE    DEBUG prints DHCP progress, which NetTrace and
 *                      ShowNetStatus already answer; COMPLETE re-reads the
 *                      static route file, and this stack reads all of
 *                      DEVS:NetInterfaces and DEVS:Internet at startup and
 *                      defers nothing, so there is no first time left to cause.
 *   CONFIGURE, LEASE   the DHCP half. Not omitted -- deferred, see the DHCP
 *   RELEASE, ID        section added to this command separately.
 *   TIMEOUT, DHCPUNICAST
 *
 * WHAT IT CHANGES, AND WHAT IT DOES NOT
 *
 * ADDRESS and NETMASK are the interface's, and are applied together in one
 * call even when only one of them is given: NetX Duo takes both at once, and an
 * interface must never be seen carrying a new address with its old mask. The
 * attached route follows the pair, so `netstat -r` changes with them.
 *
 * GATEWAY is the machine's default route and not the interface's. NetX Duo
 * keeps one gateway for the whole stack, so setting it here is the same
 * gateway AddNetRoute DEFAULTGATEWAY sets; it is offered here because the
 * gateway is the third number a person re-addressing a machine has to change,
 * and because a gateway is refused unless it is on an interface's own subnet,
 * which is exactly what the ADDRESS in the same call has just decided.
 * GATEWAY NONE clears it.
 *
 * NOTHING HERE IS PERSISTENT. This is the live interface, as Online, Offline
 * and AddNetRoute are. DEVS:NetInterfaces/<name> is not rewritten, so the next
 * boot brings the interface up the way that file says; NetSetup is what edits
 * the file. A machine that must come up re-addressed needs both.
 *
 * AN INTERFACE ON DHCP IS TOLD SO. The client goes on running, and the next
 * lease it takes writes the address, the mask and the gateway again over
 * whatever was set here. That is not refused -- re-addressing a DHCP interface
 * for a few minutes is a real thing to want -- but it is said, because an
 * address that reverts on its own with nothing having reported an error is the
 * worst way to find out.
 *
 * ADDRESS TAKES A PREFIX LENGTH. `ADDRESS 192.168.1.5/24` is the address and
 * the mask in one argument, which is how a person writes it; NETMASK is the
 * other spelling and the two may not contradict each other.
 *
 * ADDRESSES ONLY, NOT NAMES, which is where this deviates from
 * ConfigureInterfaceTagList()'s IFC_Address ("a host name to be resolved or an
 * IP address in dotted-decimal notation"). A machine being re-addressed is one
 * whose resolver is about to stop working, or has already stopped: the name
 * server is usually reached through the interface being changed. Resolving here
 * would make the command depend on the network it is repairing.
 *
 * IPv4 ONLY. An IPv6 address on an interface is not one number that replaces
 * another -- an interface holds several, always including its link-local one --
 * so there is nothing here for this command's shape to change. AddNetRoute
 * takes the IPv6 routes.
 *
 * SPDX-License-Identifier: MIT
 */

#include "tools.h"

const char *const tool_name = "ConfigureNetInterface";

static const char version_tag[] __attribute__((used)) =
    TOOL_VERSTAG("ConfigureNetInterface");

#define TEMPLATE    "INTERFACE/A,QUIET/S,ADDRESS/K,NETMASK/K,GATEWAY/K"

enum
{
    ARG_INTERFACE = 0,
    ARG_QUIET,
    ARG_ADDRESS,
    ARG_NETMASK,
    ARG_GATEWAY,
    ARG_COUNT
};

/*
 * The errno numbers this command names, as bsdsocket.library reports them
 * (src/bsdsocket/bsdsocket_internal.h), spelled out for the reason
 * addnetroute.c spells its own out: the library uses the BSD numbering and
 * newlib's <errno.h> does not agree with it everywhere. EADDRNOTAVAIL is 49
 * here and something else there, so naming the macro would compare against the
 * wrong number and print the wrong explanation.
 */
#define CNI_EINVAL          22
#define CNI_EADDRNOTAVAIL   49

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

/*
 * The running stack's own list, which is the only one that counts: this command
 * links no netstack, so tool_find_interface() would read an empty one. -2 says
 * the library would not answer, which needs a different message from -1, a name
 * that is not there.
 */
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

/* TRUE when the DHCP client is running on this interface, bound or trying. */
static BOOL on_dhcp(struct Library *base, LONG index)
{
    LONG n;
    LONG i;

    n = tool_netstatus_query(base, NETSTATUS_DHCP, &cni_dhcp,
                             sizeof(cni_dhcp), sizeof(NetStatusDhcp));
    if (n < 0)
        return FALSE;

    for (i = 0; i < n && i < (LONG)NX_MAX_PHYSICAL_INTERFACES; i++)
    {
        if ((LONG)cni_dhcp.e[i].nsd_Index != index)
            continue;

        return (BOOL)(cni_dhcp.e[i].nsd_State != NETSTATUS_DHCP_OFF);
    }

    return FALSE;
}

/*
 * "192.168.1.5" or "192.168.1.5/24". The prefix length is written into *mask
 * and *have_mask, so a caller can tell it apart from no mask at all.
 */
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
    LONG             index;
    LONG             err = 0;
    NetStatusControl ctl;
    ULONG            w;
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

    rda = ReadArgs((CONST_STRPTR)TEMPLATE, args, NULL);
    if (rda == NULL)
    {
        tool_fault(IoErr());
        tool_usage("<interface> [QUIET] [ADDRESS <a>[/<bits>]] [NETMASK <m>] "
                   "[GATEWAY <g>|NONE]",
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

        /* Both spellings given: they have to agree, because acting on one of
           them would be acting on the one the user did not mean half the
           time. */
        if (have_netmask && explicit_mask != netmask)
        {
            tool_error("the prefix length in ADDRESS and NETMASK do not agree");
            FreeArgs(rda);
            return RETURN_ERROR;
        }

        netmask      = explicit_mask;
        have_netmask = TRUE;
    }

    if (have_netmask && !mask_is_contiguous(netmask))
    {
        ami_config_format_ip(netmask, text, sizeof(text));
        tool_error("%s is not a netmask: the ones have to come first",
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
            tool_error("\"%s\" is not an address; GATEWAY NONE clears it",
                       (LONG)g);
            FreeArgs(rda);
            return RETURN_ERROR;
        }
    }

    if (!have_address && !have_netmask && !have_gateway)
    {
        tool_error("nothing to change: give ADDRESS, NETMASK or GATEWAY");
        FreeArgs(rda);
        return RETURN_ERROR;
    }

    /* Opening the library would start the stack, and starting a network in
       order to re-address an interface in it is not what was asked for. */
    if (!tool_stack_library_running())
    {
        say("The network is not running, so there is no interface to "
            "configure.\n");
        FreeArgs(rda);
        return RETURN_WARN;
    }

    base = tool_netstatus_open(cni_quiet);
    if (base == NULL)
    {
        FreeArgs(rda);
        return RETURN_FAIL;
    }

    index = find_index(base, name);
    if (index == -2)
    {
        if (!cni_quiet)
        {
            tool_error("the network would not say which interfaces it has");
            tool_explain_no_netstatus(base);
        }
        tool_netstatus_close(base);
        FreeArgs(rda);
        return RETURN_FAIL;
    }
    if (index < 0)
    {
        if (!cni_quiet)
            tool_error("there is no interface called \"%s\"", (LONG)name);
        tool_netstatus_close(base);
        FreeArgs(rda);
        return RETURN_FAIL;
    }

    for (w = 0; w < (ULONG)(sizeof(ctl) / sizeof(ULONG)); w++)
        ((ULONG *)&ctl)[w] = 0;

    ctl.nsc_Magic       = AMI_NETSTATUS_MAGIC;
    ctl.nsc_Version     = (UWORD)AMI_NETSTATUS_VERSION;
    ctl.nsc_Index       = (UWORD)index;
    ctl.nsc_Destination = address;
    ctl.nsc_NetMask     = netmask;
    ctl.nsc_Gateway     = gateway;
    ctl.nsc_Flags       = (have_address ? NETCTRL_F_ADDRESS : 0UL) |
                          (have_netmask ? NETCTRL_F_NETMASK : 0UL) |
                          (have_gateway ? NETCTRL_F_GATEWAY : 0UL);

    if (tool_netstatus_control(base, NETCTRL_INTERFACE_CONFIGURE, &ctl,
                               &err) != 0)
    {
        if (!cni_quiet)
        {
            if (err == CNI_EADDRNOTAVAIL)
            {
                ami_config_format_ip(address, text, sizeof(text));
                tool_error("%s would not take %s", (LONG)name, (LONG)text);
            }
            else if (err == CNI_EINVAL && have_gateway && gateway != 0)
            {
                ami_config_format_ip(gateway, text, sizeof(text));
                tool_error("%s is not on any of this machine's own subnets, "
                           "so nothing here can reach it", (LONG)text);
            }
            else
            {
                tool_error("%s could not be reconfigured", (LONG)name);
            }
        }
        tool_netstatus_close(base);
        FreeArgs(rda);
        return RETURN_FAIL;
    }

    /*
     * Report what the interface now has rather than what was asked for. The
     * two differ whenever only one of the pair was given, and the interface is
     * the thing the next command will see.
     */
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

    /* Said last, so it is the line left on the screen. */
    if (on_dhcp(base, index))
    {
        say("%s takes its address by DHCP, so the next lease writes over "
            "this.\n", (LONG)name);
    }

    tool_netstatus_close(base);
    FreeArgs(rda);

    return RETURN_OK;
}

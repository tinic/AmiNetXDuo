/*
 * ConfigureNetInterface, re-address a running interface in place.
 *
 *     ConfigureNetInterface INTERFACE/A,QUIET/S,ADDRESS/K,NETMASK/K,
 *                           GATEWAY/K,MDNS/K,CONFIGURE/K,
 *                           RELEASE=RELEASEADDRESS/S,TIMEOUT/K/N
 *
 * Until now the only way to change what a live interface is addressed with was
 * RemoveNetInterface followed by AddNetInterface, which closes the SANA-II
 * device, detaches the NetX Duo interface, resets every TCP connection routed
 * out of it and re-reads the file, so it can only put back what the file says.
 * This changes the three numbers and touches nothing else.
 *
 * The name and the keywords are Roadshow's. Its ConfigureNetInterface 4.37
 * carries this ReadArgs template:
 *
 *   INTERFACE/A,QUIET/S,ADDRESS/K,NETMASK/K,BROADCASTADDR/K,
 *   DESTINATION=DESTINATIONADDR/K,METRIC/K/N,MTU/K/N,ALIASADDR/K,DELETEADDR/K,
 *   ONLINE/S,OFFLINE/S,UP/S,DOWN/S,DEBUG/K,COMPLETE/K,CONFIGURE/K,LEASE/K,
 *   RELEASE=RELEASEADDRESS/S,ID/K,TIMEOUT/K/N,DHCPUNICAST/K
 *
 * INTERFACE, QUIET, ADDRESS and NETMASK are taken from it unchanged, in its
 * order, so a script written for Roadshow reads the same here. The rest is
 * deliberate:
 *
 *   GATEWAY/K          Ours. Roadshow has no such keyword and sets the default
 *                      route from CONFIGURE=DHCP or from AddNetRoute
 *                      DEFAULTGATEWAY. It is here because the gateway is the
 *                      third number a person re-addressing a machine has to
 *                      change, and because NetX Duo refuses a gateway that is
 *                      not on an interface's own subnet, which is what the
 *                      ADDRESS in the same call has just decided. The two
 *                      belong in one call and in this order.
 *
 *   MDNS/K             Ours. Roadshow has no such keyword and no mDNS at all.
 *                      MDNS= is what DEVS:NetInterfaces/<name> is written with,
 *                      so it is the same word here, taking the same YES and NO.
 *                      It is on this command because answering .local is a
 *                      property of one interface, and because it was until now
 *                      the one thing in an interface file that could be asked
 *                      for only at boot.
 *
 *   BROADCASTADDR      NetX Duo derives the broadcast address from the address
 *                      and the mask and has no separate one to set.
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
 *                      nothing drives it yet. A keyword here would be a second
 *                      way in before the first one has a test.
 *   ONLINE/OFFLINE     Online and Offline are commands of their own here, with
 *   UP/DOWN            their own regression test over the whole of that
 *                      surface (tests/tools/run-onoff.sh). Two ways to switch
 *                      an interface would be two things to keep agreeing.
 *   DEBUG, COMPLETE    DEBUG prints DHCP progress, which NetTrace and
 *                      ShowNetStatus already answer. COMPLETE re-reads the
 *                      static route file, and this stack reads all of
 *                      DEVS:NetInterfaces and DEVS:Internet at startup and
 *                      defers nothing, so there is no first time left to cause.
 *   LEASE, ID          LEASE asks the server for a lease of a stated length
 *   DHCPUNICAST        and ID names the client to it. Both are things to put
 *                      in the request, and NetX Duo's client builds the
 *                      request from its own configuration rather than per call.
 *                      DHCPUNICAST is for servers that must answer by unicast,
 *                      which is a property of a network and belongs in
 *                      DEVS:NetInterfaces beside CONFIGURE=DHCP, not in a
 *                      command run by hand.
 *
 * CONFIGURE, RELEASE and TIMEOUT are taken, and are the DHCP half of this
 * command. That is Roadshow's answer to where releasing a lease lives. A lease
 * is a property of an interface, this is the command that changes an
 * interface's properties, and a DHCP command of its own would be a second
 * place to name an interface and a second thing to keep agreeing with this one.
 *
 *   CONFIGURE=DHCP     make this interface have a current lease. On an
 *                      interface that has none, because it is static or
 *                      because RELEASE dropped it, that is a fresh allocation.
 *                      On one that is already bound it is a renewal: RFC 2131
 *                      4.4.5's renewing state entered before the timer would
 *                      have entered it, which keeps the address and asks the
 *                      server that granted it for more time. Both are waited
 *                      for, up to TIMEOUT.
 *
 *                      Roadshow has no renewal at all. Its client extends
 *                      leases by itself and its documentation says the way to
 *                      stop it is to change the address or mark the interface
 *                      down, so the renewal half is ours. It is on this
 *                      keyword rather than on a new one because making an
 *                      interface hold a current lease is one thing to ask for,
 *                      and which of the two it takes is a fact about the
 *                      interface rather than about the request. The command
 *                      says which one it did.
 *
 *                      AUTO and FASTAUTO, Roadshow's other two values, are
 *                      link-local ZeroConf. DEVS:NetInterfaces takes
 *                      CONFIGURE=LINKLOCAL and this command does not, because
 *                      a link-local address is what a machine falls back to
 *                      rather than something to ask for by hand.
 *
 *   RELEASE            DHCPRELEASE to the server: the address is free again as
 *   RELEASEADDRESS     far as the server is concerned. Both spellings, as
 *                      Roadshow has them. Roadshow's documentation says "you
 *                      can only release what was previously allocated", so an
 *                      interface with no lease is refused rather than quietly
 *                      doing nothing.
 *
 *                      The interface keeps the address until something takes
 *                      it away. NetX Duo's client does not unconfigure the
 *                      interface on release and this does not either: an
 *                      interface that lost its address the instant the lease
 *                      was dropped would lose the route the DHCPRELEASE itself
 *                      goes out on. `ConfigureNetInterface eth0 ADDRESS ...`
 *                      is how the address is then changed, and that is why the
 *                      two halves are one command.
 *
 *   TIMEOUT            seconds to wait for CONFIGURE=DHCP. Roadshow's default
 *                      is 60 and its floor is 10. Both are kept.
 *
 * A known defect next door, which this does not fix and does not depend on: a
 * lease survives Offline followed by Online with its timer still running, so a
 * machine carried to another network keeps an address that network never gave
 * it. RELEASE followed by CONFIGURE=DHCP is the way round it by hand. Fixing
 * the defect itself is separate work.
 *
 * ADDRESS and NETMASK are the interface's, and are applied together in one
 * call even when only one of them is given: NetX Duo takes both at once, and an
 * interface must never be seen carrying a new address with its old mask. The
 * attached route follows the pair, so `netstat -r` changes with them.
 *
 * GATEWAY is the machine's default route and not the interface's. NetX Duo
 * keeps one gateway for the whole stack, so setting it here sets the same
 * gateway AddNetRoute DEFAULTGATEWAY sets. It is offered here because the
 * gateway is the third number a person re-addressing a machine has to change,
 * and because a gateway is refused unless it is on an interface's own subnet,
 * which is what the ADDRESS in the same call has just decided. GATEWAY NONE
 * clears it.
 *
 * MDNS=YES makes this interface join 224.0.0.251 and probe for
 * <HOSTNAME>.local on it, which takes about a second and is not waited for.
 * MDNS=NO sends the RFC 6762 10.1 goodbye, so every cache on the link drops the
 * name at once rather than in two minutes, and leaves the group. ShowNetStatus
 * reports which of the two an interface is in, and reports what is running
 * rather than what was asked for.
 *
 * It combines with the rest: `ADDRESS 192.168.1.5/24 MDNS=YES` re-addresses the
 * machine and announces the new address under the same name, in that order,
 * because the responder takes the address the interface has when it is enabled
 * and follows it afterwards.
 *
 * Nothing here is persistent. This is the live interface, as Online, Offline
 * and AddNetRoute are. DEVS:NetInterfaces/<name> is not rewritten, so the next
 * boot brings the interface up the way that file says. NetSetup is what edits
 * the file. A machine that must come up re-addressed needs both.
 *
 * An interface on DHCP is told so. The client goes on running, and the next
 * lease it takes writes the address, the mask and the gateway again over
 * whatever was set here. That is not refused, because re-addressing a DHCP
 * interface for a few minutes is a real thing to want, but it is said, because
 * the address otherwise reverts on its own with nothing having reported an
 * error.
 *
 * ADDRESS takes a prefix length. `ADDRESS 192.168.1.5/24` is the address and
 * the mask in one argument, which is how a person writes it. NETMASK is the
 * other spelling and the two must not contradict each other.
 *
 * Addresses only, and not names, which is where this deviates from
 * ConfigureInterfaceTagList()'s IFC_Address ("a host name to be resolved or an
 * IP address in dotted-decimal notation"). A machine being re-addressed is one
 * whose resolver is about to stop working, or has already stopped, because the
 * name server is usually reached through the interface being changed. Resolving
 * here would make the command depend on the network it is repairing.
 *
 * IPv4 only. An IPv6 address on an interface is not one number that replaces
 * another, because an interface holds several, always including its link-local
 * one, so there is nothing here for this command's shape to change.
 * AddNetRoute takes the IPv6 routes.
 *
 * SPDX-License-Identifier: MIT
 */

#include "tools.h"

const char *const tool_name = "ConfigureNetInterface";

static const char version_tag[] __attribute__((used)) =
    TOOL_VERSTAG("ConfigureNetInterface");

#define TEMPLATE    "INTERFACE/A,QUIET/S,ADDRESS/K,NETMASK/K,GATEWAY/K,"     \
                    "MDNS/K,CONFIGURE/K,RELEASE=RELEASEADDRESS/S,TIMEOUT/K/N"

enum
{
    ARG_INTERFACE = 0,
    ARG_QUIET,
    ARG_ADDRESS,
    ARG_NETMASK,
    ARG_GATEWAY,
    ARG_MDNS,
    ARG_CONFIGURE,
    ARG_RELEASE,
    ARG_TIMEOUT,
    ARG_COUNT
};

/*
 * Roadshow's TIMEOUT default is 60 seconds and "the timeout cannot be shorter
 * than ten seconds", which is the same floor for the same reason: a DISCOVER
 * that has not been answered in ten seconds has usually not been answered
 * because nothing is listening, and a caller who asks for three gets a failure
 * that says nothing about the network.
 */
#define CNI_DHCP_TIMEOUT    60
#define CNI_DHCP_TIMEOUT_MIN 10

/*
 * The errno numbers this command names, as bsdsocket.library reports them
 * (src/bsdsocket/bsdsocket_internal.h), spelled out for the reason
 * addnetroute.c spells its own out: the library uses the BSD numbering and
 * newlib's <errno.h> does not agree with it everywhere. EADDRNOTAVAIL is 49
 * here and something else there, so naming the macro would compare against the
 * wrong number and print the wrong explanation.
 */
#define CNI_EIO             5
#define CNI_ENXIO           6
#define CNI_EINVAL          22
#define CNI_EADDRNOTAVAIL   49
#define CNI_ENOTCONN        57
#define CNI_ENOSYS          78

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
 * the library did not answer, which needs a different message from -1, a name
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
 * Wait for the DHCP client on `index` to settle, up to `seconds`. TRUE if it
 * reached NETSTATUS_DHCPRAW_BOUND, FALSE on the deadline or a break.
 *
 * The RAW state is what is watched, not nsd_State: BOUND, RENEWING and
 * REBINDING are all NETSTATUS_DHCP_BOUND, so a renewal asked for on a bound
 * interface would look finished before the request had left the machine. Here
 * the wait ends when the client is back at plain BOUND, which is the state it
 * only re-enters on an ACK.
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
    args[ARG_MDNS]      = 0;
    args[ARG_CONFIGURE] = 0;
    args[ARG_RELEASE]   = 0;
    args[ARG_TIMEOUT]   = 0;

    rda = ReadArgs((CONST_STRPTR)TEMPLATE, args, NULL);
    if (rda == NULL)
    {
        tool_fault(IoErr());
        tool_usage("<interface> [QUIET] [ADDRESS <a>[/<bits>]] [NETMASK <m>] "
                   "[GATEWAY <g>|NONE] [MDNS YES|NO] [CONFIGURE DHCP] "
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

        /* Both spellings given: they have to agree, because acting on one of
           them would be acting on the one the user did not mean half the
           time. */
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
     * The same two words DEVS:NetInterfaces takes, and no others. The file's
     * parser accepts a wider set of spellings for a boolean. A command that
     * accepted one the file did not would teach a spelling that fails at the
     * next boot.
     */
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

        /* Roadshow's other two values are AUTO and FASTAUTO, which are
           link-local ZeroConf. Named in the refusal, so that somebody who typed
           one is told what this stack calls it rather than only that the value
           is wrong. */
        if (tool_stricmp(c, "DHCP") != 0)
        {
            tool_error("CONFIGURE takes DHCP and nothing else. A link-local "
                       "address is CONFIGURE=LINKLOCAL in "
                       "DEVS:NetInterfaces/%s", (LONG)name);
            FreeArgs(rda);
            return RETURN_ERROR;
        }

        want_dhcp = TRUE;

        /* The server decides both of these, so accepting them would be
           accepting something that is then written over. */
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
        timeout = (ULONG)(*(LONG *)args[ARG_TIMEOUT]);

        if (!want_dhcp)
        {
            tool_error("TIMEOUT is how long to wait for a lease, so it needs "
                       "CONFIGURE=DHCP");
            FreeArgs(rda);
            return RETURN_ERROR;
        }
        if (timeout < (ULONG)CNI_DHCP_TIMEOUT_MIN)
        {
            tool_error("a TIMEOUT of less than %ld seconds is too short to "
                       "tell anything about the network",
                       (LONG)CNI_DHCP_TIMEOUT_MIN);
            FreeArgs(rda);
            return RETURN_ERROR;
        }
    }

    if (!have_address && !have_netmask && !have_gateway && !have_mdns &&
        !want_dhcp && !want_release)
    {
        tool_error("nothing to change: give ADDRESS, NETMASK, GATEWAY, MDNS, "
                   "CONFIGURE or RELEASE");
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

    /*
     * RELEASE first, so `RELEASE CONFIGURE=DHCP` is a lease given back and a
     * new one asked for -- which is what a machine carried to another network
     * wants -- and `RELEASE ADDRESS=...` gives the lease back over the route
     * the address below is about to replace.
     */
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

    /*
     * Then the lease. Which of the two operations it is, is a fact about the
     * interface: one that holds a lease is asking to keep its address for
     * longer, one that does not is asking for an address.
     */
    if (want_dhcp)
    {
        BOOL  renewing = dhcp_bound(base, index);
        ULONG op       = renewing ? NETCTRL_DHCP_RENEW : NETCTRL_DHCP_START;

        /*
         * Roadshow's own wording is "Combine CONFIGURE=DHCP with
         * ADDRESS=x.x.x.x to request a specific address". It is a request and
         * not a demand: DISCOVER is still sent, so a server that disagrees
         * offers something else rather than answering NAK.
         */
        if (control(base, op, index, address, 0, 0,
                    (have_address && !renewing) ? NETCTRL_F_ADDRESS : 0UL,
                    &err) != 0)
        {
            if (err == CNI_ENOTCONN)
                tool_error("%s has no lease to renew", (LONG)name);
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

            /* Which of the two it was is said, because a renewal reported on a
               machine that had no lease, and a new lease reported on one that
               did, are both reports of something other than what happened. */
            say("%s: %s %s, from %s\n", (LONG)name,
                (LONG)(renewing ? "lease renewed," : "lease taken,"),
                (LONG)text, (LONG)server);
        }
    }

    /*
     * With CONFIGURE=DHCP an ADDRESS is the address that was asked for, and it
     * has already been asked for above. Writing it on the interface here would
     * overwrite whatever the server granted with what this machine would have
     * preferred.
     */
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

        /*
         * Report what the interface now has rather than what was asked for.
         * The two differ whenever only one of the pair was given, and the
         * interface is the thing the next command will see.
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
    }

    /*
     * After the address, because the A record the responder claims is whatever
     * the interface holds when it is enabled, and a machine that is being
     * re-addressed and made findable in one call means the new address.
     */
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

        /* Probing is three packets 250 ms apart and this does not wait for it,
           so the line says what was started rather than what was claimed.
           ShowNetStatus is where the name appears once it is this machine's. */
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

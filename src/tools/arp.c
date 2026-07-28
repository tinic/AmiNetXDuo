/*
 * arp -- the address resolution cache: what is at each address on this network.
 *
 *     arp [ADDRESS] [DELETE] [SET=<hardware address>] [UNIT=<n>] [STATS]
 *
 * WHAT IT IS FOR
 *
 * Every packet that leaves this machine for the local network needs the
 * ethernet address of whatever it is going to, and ARP is how that is found
 * out and remembered. The cache is therefore the answer to a question that
 * comes up constantly and that nothing else answers cheaply: "is the other
 * machine even there?"
 *
 *   an entry with a hardware address   it answered; the wire is fine, and
 *                                      whatever is wrong is above this layer
 *   an entry with no reply             we asked and nothing came back --
 *                                      wrong address, wrong network, or it
 *                                      is switched off
 *   no entry at all                    nothing here has tried to reach it
 *
 * That is worth more than a ping, because it separates "the network does not
 * work" from "that machine is not answering", and it needs nothing of the
 * other end but ARP -- which even a machine with a firewall that drops pings
 * still replies to.
 *
 *     arp                     the whole cache
 *     arp 192.168.1.1         one address
 *     arp STATS               how much asking it has taken
 *     arp 192.168.1.1 DELETE  forget it, so the next packet asks again
 *     arp 192.168.1.1 SET=02:11:22:33:44:55
 *                             a permanent entry, for something that does not
 *                             answer ARP or that keeps changing
 *
 * NOTHING AGES OUT. NX_ARP_EXPIRATION_RATE is 0 in this build, so an entry
 * stays until DELETE removes it or the stack stops. Worth knowing before
 * wondering why a machine that has since moved is still listed here.
 *
 * IT DOES NOT START THE NETWORK, unlike fetch or nslookup. The cache is a
 * statement about a running stack, and starting one to report that its cache
 * is empty would be a bug rather than a convenience -- the same rule
 * ShowNetStatus follows, for the same reason.
 *
 * SPDX-License-Identifier: MIT
 */

#include "tools.h"
#include "tools_nx.h"

const char *const tool_name = "arp";

static const char version_tag[] __attribute__((used)) =
    "$VER: arp 1.0 (28.7.2026)";

#define TEMPLATE    "ADDRESS,DELETE/S,SET/K,UNIT/K/N,STATS/S,QUIET/S"

enum
{
    ARG_ADDRESS = 0,
    ARG_DELETE,
    ARG_SET,
    ARG_UNIT,
    ARG_STATS,
    ARG_QUIET,
    ARG_COUNT
};

static ToolStats    arp_stats;
static ToolSnapshot arp_snap;

static VOID zero_control(NetStatusControl *ctl)
{
    ULONG i;

    for (i = 0; i < (ULONG)(sizeof(*ctl) / sizeof(ULONG)); i++)
        ((ULONG *)ctl)[i] = 0;
}

/*
 * "02:11:22:33:44:55". The separator may be ':' or '-' or absent, because all
 * three are what people have written down, and a command that rejects the
 * form printed on the underside of the machine is not much use.
 */
static BOOL parse_mac(const char *text, UBYTE *out)
{
    UWORD got = 0;

    if (text == NULL)
        return FALSE;

    while (*text != '\0' && got < (UWORD)AMI_ETH_ADDR_SIZE)
    {
        UWORD digits = 0;
        ULONG value  = 0;

        while (digits < 2)
        {
            char  c = *text;
            ULONG d;

            if (c >= '0' && c <= '9')       d = (ULONG)(c - '0');
            else if (c >= 'a' && c <= 'f')  d = (ULONG)(c - 'a') + 10UL;
            else if (c >= 'A' && c <= 'F')  d = (ULONG)(c - 'A') + 10UL;
            else                            break;

            value = (value << 4) | d;
            digits++;
            text++;
        }

        if (digits == 0)
            return FALSE;

        out[got++] = (UBYTE)value;

        if (*text == ':' || *text == '-')
            text++;
    }

    return (BOOL)(got == (UWORD)AMI_ETH_ADDR_SIZE && *text == '\0');
}

/*
 * The interface an entry was learnt on, by the name the rest of the tools use
 * for it. "interface 0" is a number out of the stack's internals; "a2065.0"
 * is the line the user wrote in the config file, and on a machine with two
 * cards it is the difference between a useful column and a decorative one.
 */
static const char *interface_name(UWORD nx_index, BOOL have_snapshot)
{
    UWORD i;

    if (!have_snapshot)
        return NULL;

    for (i = 0; i < arp_snap.iface_count; i++)
    {
        if (arp_snap.iface[i].nx_index == nx_index &&
            arp_snap.iface[i].nx_name[0] != '\0')
        {
            return arp_snap.iface[i].nx_name;
        }
    }

    return NULL;
}

static VOID print_entry(const ToolArpEntry *e, BOOL have_snapshot)
{
    char        addr[16];
    char        mac[24];
    const char *ifname = interface_name(e->nx_index, have_snapshot);

    ami_config_format_ip(e->address, addr, sizeof(addr));

    /*
     * An unresolved entry has an all-zero hardware address, and printing it
     * as 00:00:00:00:00:00 would read as a machine with a strange address
     * rather than as silence -- which is the one distinction this command
     * exists to draw. The retry count is the evidence: it is how many times
     * we asked before giving up on it for now.
     */
    if (!e->resolved)
    {
        tool_printf("  %-15s  no reply", (LONG)addr);
        if (e->retries != 0)
            tool_printf(" after %lu request%s",
                        (LONG)e->retries,
                        (LONG)((e->retries == 1) ? "" : "s"));
        tool_printf("\n");
        return;
    }

    tool_format_mac(e->mac, mac, sizeof(mac));

    tool_printf("  %-15s  %s", (LONG)addr, (LONG)mac);

    if (ifname != NULL)
        tool_printf("  %s", (LONG)ifname);
    else if (e->nx_index != 0)
        tool_printf("  interface %lu", (LONG)e->nx_index);

    if (e->is_static)
        tool_printf("  permanent");

    tool_printf("\n");
}

/* TRUE when `addr` is on the network of an interface this machine has up. */
static BOOL on_our_network(ULONG addr, BOOL have_snapshot)
{
    UWORD i;

    if (!have_snapshot)
        return TRUE;                /* cannot tell; do not assert otherwise */

    for (i = 0; i < arp_snap.iface_count; i++)
    {
        const ToolIfInfo *nif = &arp_snap.iface[i];

        if (!nif->attached || nif->address == 0 || nif->netmask == 0)
            continue;

        if ((nif->address & nif->netmask) == (addr & nif->netmask))
            return TRUE;
    }

    return FALSE;
}

/*
 * Why an address is not in the cache, which is two quite different things.
 *
 * ARP is only ever spoken to machines on this machine's own network. An
 * address anywhere else is reached by handing the packet to the router, so it
 * will NEVER appear here however much traffic goes to it -- and someone who
 * has just pinged it successfully and then found nothing in the cache is
 * owed that sentence rather than "nothing has tried to reach it".
 */
static VOID explain_absence(ULONG addr, BOOL have_snapshot)
{
    char gw[16];

    if (!on_our_network(addr, have_snapshot))
    {
        tool_advise("It is not on this machine's own network, so it is "
                    "reached through the router rather than by ARP -- it "
                    "will never appear here.");

        if (arp_snap.have_gateway && arp_snap.gateway != 0)
        {
            ami_config_format_ip(arp_snap.gateway, gw, sizeof(gw));
            tool_advise_blank();
            tool_printf("  The router is %s, and THAT is the entry worth "
                        "checking:\n", (LONG)gw);
            tool_printf("      arp %s\n", (LONG)gw);
        }
        return;
    }

    tool_advise("Nothing here has tried to reach it yet. Anything that sends "
                "it a packet -- ping, nc, fetch -- will put it in.");
}

/*
 * The protocol counters, which say how the cache came to look the way it
 * does. Requests sent with few responses received is a quiet network or a
 * wrong netmask; invalid messages are somebody else's malformed frames.
 */
static VOID print_stats(const ToolStats *s)
{
    if (!s->have_arp)
    {
        tool_printf("This stack does not keep ARP counters.\n");
        return;
    }

    tool_printf("Asking       %lu request%s sent, %lu response%s received\n",
                (LONG)s->arp_requests_sent,
                (LONG)((s->arp_requests_sent == 1) ? "" : "s"),
                (LONG)s->arp_responses_received,
                (LONG)((s->arp_responses_received == 1) ? "" : "s"));

    tool_printf("Answering    %lu request%s received, %lu response%s sent\n",
                (LONG)s->arp_requests_received,
                (LONG)((s->arp_requests_received == 1) ? "" : "s"),
                (LONG)s->arp_responses_sent,
                (LONG)((s->arp_responses_sent == 1) ? "" : "s"));

    tool_printf("Cache        %lu learnt, %lu permanent",
                (LONG)s->arp_dynamic_entries,
                (LONG)s->arp_static_entries);

    if (s->arp_aged_entries != 0)
        tool_printf(", %lu aged out", (LONG)s->arp_aged_entries);

    tool_printf("\n");

    if (s->arp_invalid_messages != 0)
    {
        tool_printf("Discarded    %lu malformed message%s\n",
                    (LONG)s->arp_invalid_messages,
                    (LONG)((s->arp_invalid_messages == 1) ? "" : "s"));
        tool_advise("Malformed ARP comes from another machine on this "
                    "network, not from this one.");
    }
}

int main(int argc, char **argv)
{
    LONG              args[ARG_COUNT];
    struct RDArgs    *rda;
    struct Library   *base;
    NetStatusControl  ctl;
    const char       *address_text = NULL;
    ULONG             want         = 0;
    BOOL              want_one     = FALSE;
    BOOL              quiet;
    BOOL              have_snapshot;
    LONG              err          = 0;
    LONG              rc           = RETURN_OK;
    ULONG             i;
    UWORD             shown        = 0;

    (VOID)argv;

    if (tool_from_workbench(argc))
        return RETURN_FAIL;

    tool_break_arm();

    for (i = 0; i < (ULONG)ARG_COUNT; i++)
        args[i] = 0;

    rda = ReadArgs((CONST_STRPTR)TEMPLATE, args, NULL);
    if (rda == NULL)
    {
        tool_fault(IoErr());
        return RETURN_ERROR;
    }

    quiet        = (BOOL)(args[ARG_QUIET] != 0);
    address_text = (const char *)args[ARG_ADDRESS];

    if (address_text != NULL)
    {
        if (!ami_config_parse_ip(address_text, &want))
        {
            tool_error("\"%s\" is not an address", (LONG)address_text);
            tool_advise("arp takes a numeric address like 192.168.1.1, not a "
                        "name -- use nslookup to turn a name into one.");
            FreeArgs(rda);
            return RETURN_ERROR;
        }
        want_one = TRUE;
    }
    else if (args[ARG_DELETE] != 0 || args[ARG_SET] != 0)
    {
        tool_error("which address? DELETE and SET each need one");
        FreeArgs(rda);
        return RETURN_ERROR;
    }

    /*
     * The interface table, for the name column and for deciding whether an
     * address is one ARP could ever have an answer about. Wanted on every
     * path, and a failure here is never fatal -- it only costs detail.
     */
    tool_nx_quiet(TRUE);
    have_snapshot = (BOOL)(tool_snapshot(&arp_snap, FALSE) == 0);
    tool_nx_quiet(FALSE);

    /* ------------------------------------------------- SET and DELETE --- */
    if (args[ARG_SET] != 0 || args[ARG_DELETE] != 0)
    {
        UBYTE mac[AMI_ETH_ADDR_SIZE];
        char  text[24];

        if (args[ARG_SET] != 0 && !parse_mac((const char *)args[ARG_SET], mac))
        {
            tool_error("\"%s\" is not a hardware address",
                       (LONG)args[ARG_SET]);
            tool_advise("Six bytes in hex, like 02:11:22:33:44:55.");
            FreeArgs(rda);
            return RETURN_ERROR;
        }

        /* Changing the cache needs the library, but still must not start it:
           there is no cache to change until something is running. */
        base = tool_netstatus_open(quiet);
        if (base == NULL)
        {
            FreeArgs(rda);
            return RETURN_ERROR;
        }

        zero_control(&ctl);
        ctl.nsc_Destination = want;

        if (args[ARG_SET] != 0)
        {
            if (args[ARG_UNIT] != 0)
                ctl.nsc_Index = (UWORD)(*(LONG *)args[ARG_UNIT]);

            for (i = 0; i < (ULONG)AMI_ETH_ADDR_SIZE; i++)
                ctl.nsc_HwAddress[i] = mac[i];

            if (tool_netstatus_control(base, NETCTRL_ARP_ADD, &ctl, &err) != 0)
            {
                tool_error("%s was not added to the cache",
                           (LONG)address_text);

                /*
                 * Nearly always this, and it is worth saying rather than
                 * leaving as an unexplained refusal from the stack: an entry
                 * maps an address to a card on the same wire, so an address
                 * on some other network has no card here to map it to.
                 */
                if (!on_our_network(want, have_snapshot))
                    tool_advise("It is not on any network this machine has an "
                                "interface on, and only addresses on this "
                                "machine's own network can have one.");

                tool_netstatus_close(base);
                FreeArgs(rda);
                return RETURN_FAIL;
            }

            tool_format_mac(mac, text, sizeof(text));
            if (!quiet)
                tool_printf("%s is now permanently %s.\n",
                            (LONG)address_text, (LONG)text);
        }
        else
        {
            if (tool_netstatus_control(base, NETCTRL_ARP_DELETE,
                                       &ctl, &err) != 0)
            {
                tool_error("%s was not removed from the cache",
                           (LONG)address_text);
                tool_advise("It may not have been in it -- arp with no "
                            "arguments lists what is.");
                tool_netstatus_close(base);
                FreeArgs(rda);
                return RETURN_FAIL;
            }

            if (!quiet)
                tool_printf("%s forgotten. The next packet to it will ask "
                            "again.\n", (LONG)address_text);
        }

        tool_netstatus_close(base);
        FreeArgs(rda);
        return RETURN_OK;
    }

    /* ------------------------------------------------------------ list --- */
    if (tool_stats(&arp_stats) != 0)
    {
        FreeArgs(rda);
        return RETURN_ERROR;        /* tool_stats has already explained */
    }

    if (args[ARG_STATS] != 0)
    {
        print_stats(&arp_stats);
        if (!want_one)
        {
            FreeArgs(rda);
            return RETURN_OK;
        }
        tool_printf("\n");
    }

    for (i = 0; i < (ULONG)arp_stats.arp_count; i++)
    {
        if (want_one && arp_stats.arp[i].address != want)
            continue;

        if (shown == 0 && !quiet)
            tool_printf("Address          Hardware address\n");

        print_entry(&arp_stats.arp[i], have_snapshot);
        shown++;
    }

    if (shown == 0)
    {
        if (want_one)
        {
            tool_printf("%s is not in the cache.\n", (LONG)address_text);
            explain_absence(want, have_snapshot);
            rc = RETURN_WARN;
        }
        else if (!quiet)
        {
            tool_printf("The address cache is empty.\n");
            tool_advise("It fills as this machine talks to others on the "
                        "local network.");
        }
    }
    else if (arp_stats.arp_truncated && !quiet)
    {
        /*
         * The snapshot holds TOOL_MAX_ARP entries and the stack may have
         * more. Saying so is the point of nsh_Available: a list that silently
         * stops reads as a complete one.
         */
        tool_printf("\n");
        tool_advise("More entries exist than are shown here.");
    }

    FreeArgs(rda);
    return rc;
}

/*
 * src/bsdsocket/routing.c on the host: ChangeRouteTagList's grammar and its
 * refusals.
 *
 * WHY A HOST TEST AND WHAT IT CANNOT SEE
 *
 *   The routing table is NetX Duo's, so the half of this vector that matters
 *   on a real machine, that a changed route sends the next packet to the new
 *   next hop, is only observable on the emulator; tests/tools/run-routes.sh
 *   asserts it there off the wire.  What is testable here is everything
 *   ChangeRouteTagList decides BEFORE it touches the table: which tag
 *   combinations it accepts, which it refuses and with which errno, whether it
 *   finds the entry the same way DeleteRouteTagList would, and whether a
 *   refusal leaves the table alone.  Those are the paths a running stack
 *   exercises least and a caller hits first.
 *
 *   The four NetX Duo entry points are stubbed rather than linked, because
 *   linking them drags in an NX_IP that has to be created, an IP thread and a
 *   driver.  Each stub reproduces what the vendored function does, from the
 *   source, and says where:
 *
 *     nx_ip_static_route_add.c:89-112    next hop must be on some valid
 *                                        interface, or NX_IP_ADDRESS_ERROR
 *                                        with the table untouched
 *     nx_ip_static_route_add.c:117-131   an exact (network, mask) match is
 *                                        updated IN PLACE: the next hop is
 *                                        assigned and nothing else, not even
 *                                        the entry's interface pointer
 *     nx_ip_static_route_add.c:142-146   NX_OVERFLOW when full
 *     nx_ip_static_route_delete.c        removes and compacts
 *     nx_ip_gateway_address_set.c:88-113 interface lookup, then one
 *                                        assignment of address and interface
 *
 *   The in-place update is the whole reason this vector can exist without
 *   dropping traffic, so a stub that "changed" a route by deleting and adding
 *   would test the wrong thing.  It is reproduced exactly, including the
 *   omission of the interface pointer, which is what routing.c has to work
 *   around.
 *
 * SPDX-License-Identifier: MIT
 */

#include "bsdsocket_vectors.h"

#include <stdio.h>
#include <string.h>

static unsigned long h_checks;
static unsigned long h_failures;

#define CHECK(cond, what)                                                     \
    do {                                                                      \
        h_checks++;                                                           \
        if (!(cond)) {                                                        \
            h_failures++;                                                     \
            printf("  FAIL %s\n", (what));                                    \
        }                                                                     \
    } while (0)

static struct AmiSocketBase h_base;
#define BASE (&h_base)

/* ------------------------------------------------------- the fake machine */

/*
 * Two interfaces, because the one case routing.c cannot do in place is a next
 * hop that moves from one to the other, and a single-interface fixture can
 * never reach it.
 *
 *   eth0  10.0.2.15/24     gateways 10.0.2.x
 *   eth1  192.168.9.4/24   gateways 192.168.9.x
 */
#define IF0_ADDR    0x0A00020FUL        /* 10.0.2.15    */
#define IF0_MASK    0xFFFFFF00UL
#define IF1_ADDR    0xC0A80904UL        /* 192.168.9.4  */
#define IF1_MASK    0xFFFFFF00UL

static NX_IP  h_ip;
static NX_IP *h_ip_ptr = &h_ip;

static VOID h_machine_reset(VOID)
{
    memset(&h_ip, 0, sizeof(h_ip));

    h_ip.nx_ip_interface[0].nx_interface_valid           = NX_TRUE;
    h_ip.nx_ip_interface[0].nx_interface_ip_address      = IF0_ADDR;
    h_ip.nx_ip_interface[0].nx_interface_ip_network_mask = IF0_MASK;
    h_ip.nx_ip_interface[0].nx_interface_ip_network      = IF0_ADDR & IF0_MASK;

    h_ip.nx_ip_interface[1].nx_interface_valid           = NX_TRUE;
    h_ip.nx_ip_interface[1].nx_interface_ip_address      = IF1_ADDR;
    h_ip.nx_ip_interface[1].nx_interface_ip_network_mask = IF1_MASK;
    h_ip.nx_ip_interface[1].nx_interface_ip_network      = IF1_ADDR & IF1_MASK;

    h_ip_ptr = &h_ip;
}

/* -------------------------------------------------- NetX Duo, reproduced */

static NX_INTERFACE *h_iface_for(NX_IP *ip, ULONG next_hop)
{
    UINT i;

    for (i = 0; i < (UINT)NX_MAX_IP_INTERFACES; i++)
    {
        if (ip->nx_ip_interface[i].nx_interface_valid &&
            ((next_hop & ip->nx_ip_interface[i].nx_interface_ip_network_mask) ==
             ip->nx_ip_interface[i].nx_interface_ip_network))
            return &ip->nx_ip_interface[i];
    }

    return NX_NULL;
}

UINT _nxe_ip_static_route_add(NX_IP *ip, ULONG network_address, ULONG net_mask,
                              ULONG next_hop)
{
    NX_INTERFACE *nxif = h_iface_for(ip, next_hop);
    ULONG         network;
    INT           i;
    INT           j;

    if (nxif == NX_NULL)
        return NX_IP_ADDRESS_ERROR;

    network = network_address & net_mask;

    for (i = 0; i < (INT)ip->nx_ip_routing_table_entry_count; i++)
    {
        if (ip->nx_ip_routing_table[i].nx_ip_routing_dest_ip == network &&
            ip->nx_ip_routing_table[i].nx_ip_routing_net_mask == net_mask)
        {
            /* The in-place update, and only the next hop: the entry's
               interface pointer is deliberately left as it was. */
            ip->nx_ip_routing_table[i].nx_ip_routing_next_hop_address = next_hop;
            return NX_SUCCESS;
        }

        if (ip->nx_ip_routing_table[i].nx_ip_routing_net_mask <= net_mask)
        {
            if (ip->nx_ip_routing_table_entry_count == NX_IP_ROUTING_TABLE_SIZE)
                return NX_OVERFLOW;

            for (j = (INT)ip->nx_ip_routing_table_entry_count - 1; j >= i; j--)
                ip->nx_ip_routing_table[j + 1] = ip->nx_ip_routing_table[j];

            break;
        }
    }

    if (i == NX_IP_ROUTING_TABLE_SIZE)
        return NX_OVERFLOW;

    ip->nx_ip_routing_table[i].nx_ip_routing_dest_ip              = network;
    ip->nx_ip_routing_table[i].nx_ip_routing_net_mask             = net_mask;
    ip->nx_ip_routing_table[i].nx_ip_routing_next_hop_address     = next_hop;
    ip->nx_ip_routing_table[i].nx_ip_routing_entry_ip_interface   = nxif;
    ip->nx_ip_routing_table_entry_count++;

    return NX_SUCCESS;
}

static ULONG h_deletes;

UINT _nxe_ip_static_route_delete(NX_IP *ip, ULONG network_address,
                                 ULONG net_mask)
{
    ULONG network = network_address & net_mask;
    UINT  i;

    h_deletes++;

    /* The vendored function's special case: an empty table reports success
       without looking for the requested entry. routing.c must mask this. */
    if (ip->nx_ip_routing_table_entry_count == 0)
        return NX_SUCCESS;

    for (i = 0; i < ip->nx_ip_routing_table_entry_count; i++)
    {
        if (ip->nx_ip_routing_table[i].nx_ip_routing_dest_ip == network &&
            ip->nx_ip_routing_table[i].nx_ip_routing_net_mask == net_mask)
        {
            for (; i + 1 < ip->nx_ip_routing_table_entry_count; i++)
                ip->nx_ip_routing_table[i] = ip->nx_ip_routing_table[i + 1];

            memset(&ip->nx_ip_routing_table[i], 0,
                   sizeof(ip->nx_ip_routing_table[i]));
            ip->nx_ip_routing_table_entry_count--;
            return NX_SUCCESS;
        }
    }

    return NX_NOT_SUCCESSFUL;
}

UINT _nxe_ip_gateway_address_get(NX_IP *ip, ULONG *address)
{
    if (ip->nx_ip_gateway_address == 0)
        return NX_NOT_FOUND;

    *address = ip->nx_ip_gateway_address;
    return NX_SUCCESS;
}

UINT _nxe_ip_gateway_address_set(NX_IP *ip, ULONG address)
{
    NX_INTERFACE *nxif = h_iface_for(ip, address);

    if (nxif == NX_NULL)
        return NX_IP_ADDRESS_ERROR;

    ip->nx_ip_gateway_address   = address;
    ip->nx_ip_gateway_interface = nxif;
    return NX_SUCCESS;
}

UINT _nxe_ip_gateway_address_clear(NX_IP *ip)
{
    ip->nx_ip_gateway_address   = 0;
    ip->nx_ip_gateway_interface = NX_NULL;
    return NX_SUCCESS;
}

/* ------------------------------------------------ what routing.c else uses */

static LONG h_errno_last;

LONG bsd_fail(struct AmiSocketBase *base, LONG code)
{
    (VOID)base;
    h_errno_last = code;
    return -1;
}

VOID bsd_bzero(APTR p, ULONG size)
{
    memset(p, 0, (size_t)size);
}

/* errno.c's, reproduced: linking errno.c brings the whole vector table with
   it.  The four control tags are what a caller's tag list may legally
   contain, so a walker that only knew TAG_DONE would pass tests a real one
   fails. */
struct TagItem *bsd_next_tag(struct TagItem **cursor)
{
    struct TagItem *item = *cursor;

    while (item != NULL)
    {
        switch (item->ti_Tag)
        {
            case TAG_DONE:  *cursor = NULL; return NULL;
            case TAG_IGNORE: item++; continue;
            case TAG_MORE:  item = (struct TagItem *)item->ti_Data; continue;
            case TAG_SKIP:  item += 1 + (LONG)item->ti_Data; continue;
            default:        *cursor = item + 1; return item;
        }
    }

    *cursor = NULL;
    return NULL;
}

NX_IP *netstack_ip(VOID)
{
    return h_ip_ptr;
}

LONG bsd_nx_enter(struct AmiSocketBase *base)
{
    (VOID)base;
    return 0;
}

VOID bsd_nx_leave(struct AmiSocketBase *base)
{
    (VOID)base;
}

APTR ami_alloc(ULONG size)
{
    (VOID)size;
    return NULL;
}

VOID ami_free(APTR ptr)
{
    (VOID)ptr;
}

/*
 * The address parsers.  ami_config_parse_ip() is src/config's and has its own
 * test; a dotted quad is all these cases need, and anything else must be
 * refused so that "not an address" reaches the name paths below.
 */
BOOL ami_config_parse_ip(const char *text, ULONG *out)
{
    ULONG octet[4];
    int   n = 0;
    int   digits;

    if (text == NULL)
        return FALSE;

    for (n = 0; n < 4; n++)
    {
        octet[n] = 0;
        digits = 0;
        while (*text >= '0' && *text <= '9')
        {
            octet[n] = octet[n] * 10 + (ULONG)(*text - '0');
            text++;
            digits++;
        }
        if (digits == 0 || octet[n] > 255)
            return FALSE;
        if (n < 3)
        {
            if (*text != '.')
                return FALSE;
            text++;
        }
    }

    if (*text != '\0')
        return FALSE;

    *out = (octet[0] << 24) | (octet[1] << 16) | (octet[2] << 8) | octet[3];
    return TRUE;
}

/* No DEVS:Internet/networks and no resolver here.  A name is therefore never
   an address, which is what makes the "unparseable" cases below refusals. */
const AmiNetdbEntry *ami_netdb_net_by_name(const char *name)
{
    (VOID)name;
    return NULL;
}

LONG netstack_resolve(const char *name, ULONG *addr_out, ULONG timeout_ticks)
{
    (VOID)name;
    (VOID)addr_out;
    (VOID)timeout_ticks;
    return -1;                  /* anything but AMI_NET_OK */
}

/* ------------------------------------------------------------- fixtures -- */

#define T_END       { TAG_DONE, 0 }

static ULONG h_route_next_hop(ULONG network, ULONG mask, int *found)
{
    UINT i;

    *found = 0;
    for (i = 0; i < h_ip.nx_ip_routing_table_entry_count; i++)
    {
        if (h_ip.nx_ip_routing_table[i].nx_ip_routing_dest_ip == network &&
            h_ip.nx_ip_routing_table[i].nx_ip_routing_net_mask == mask)
        {
            *found = 1;
            return h_ip.nx_ip_routing_table[i].nx_ip_routing_next_hop_address;
        }
    }

    return 0;
}

static NX_INTERFACE *h_route_iface(ULONG network, ULONG mask)
{
    UINT i;

    for (i = 0; i < h_ip.nx_ip_routing_table_entry_count; i++)
    {
        if (h_ip.nx_ip_routing_table[i].nx_ip_routing_dest_ip == network &&
            h_ip.nx_ip_routing_table[i].nx_ip_routing_net_mask == mask)
            return h_ip.nx_ip_routing_table[i].nx_ip_routing_entry_ip_interface;
    }

    return NX_NULL;
}

/* One 192.168.66.0/24 via 10.0.2.98, put there through the published Add so
   the mask this test looks for is the one Add derived, not one it assumed. */
static VOID h_add_fixture_route(VOID)
{
    struct TagItem t[3];

    t[0].ti_Tag  = RTA_Destination;
    t[0].ti_Data = (uintptr_t)"192.168.66.0";
    t[1].ti_Tag  = RTA_Gateway;
    t[1].ti_Data = (uintptr_t)"10.0.2.98";
    t[2].ti_Tag  = TAG_DONE;
    t[2].ti_Data = 0;

    if (bsd_AddRouteTagList(t, BASE) != 0)
        printf("  FAIL fixture: AddRouteTagList refused 192.168.66.0\n");
}

/* --------------------------------------------------------------- refusals */

static VOID t_refusals(VOID)
{
    struct TagItem t[4];
    LONG           rc;

    printf("ChangeRouteTagList: the tag lists it must refuse\n");

    h_machine_reset();
    h_add_fixture_route();

    rc = bsd_ChangeRouteTagList(NULL, BASE);
    CHECK(rc == -1 && h_errno_last == AMI_EINVAL, "NULL tag list is EINVAL");

    t[0] = (struct TagItem)T_END;
    rc = bsd_ChangeRouteTagList(t, BASE);
    CHECK(rc == -1 && h_errno_last == AMI_EINVAL,
          "an empty tag list names no change: EINVAL");

    /* A destination and nothing else.  The five RTA_ tags carry no metric and
       no flag, so there is nothing else it could have meant to change. */
    t[0].ti_Tag  = RTA_Destination;
    t[0].ti_Data = (uintptr_t)"192.168.66.0";
    t[1] = (struct TagItem)T_END;
    rc = bsd_ChangeRouteTagList(t, BASE);
    CHECK(rc == -1 && h_errno_last == AMI_EINVAL,
          "RTA_Destination with no RTA_Gateway is EINVAL");

    t[0].ti_Tag  = RTA_Gateway;
    t[0].ti_Data = (uintptr_t)"10.0.2.99";
    t[1] = (struct TagItem)T_END;
    rc = bsd_ChangeRouteTagList(t, BASE);
    CHECK(rc == -1 && h_errno_last == AMI_EINVAL,
          "RTA_Gateway with no destination names no entry: EINVAL");

    /* "The RTA_DefaultGateway tag excludes the use of the RTA_Destination and
       RTA_Gateway tags", the autodoc, on the Add page. */
    t[0].ti_Tag  = RTA_DefaultGateway;
    t[0].ti_Data = (uintptr_t)"10.0.2.2";
    t[1].ti_Tag  = RTA_Destination;
    t[1].ti_Data = (uintptr_t)"192.168.66.0";
    t[2] = (struct TagItem)T_END;
    rc = bsd_ChangeRouteTagList(t, BASE);
    CHECK(rc == -1 && h_errno_last == AMI_EINVAL,
          "RTA_DefaultGateway beside RTA_Destination is EINVAL");

    t[0].ti_Tag  = RTA_DefaultGateway;
    t[0].ti_Data = (uintptr_t)"10.0.2.2";
    t[1].ti_Tag  = RTA_Gateway;
    t[1].ti_Data = (uintptr_t)"10.0.2.99";
    t[2] = (struct TagItem)T_END;
    rc = bsd_ChangeRouteTagList(t, BASE);
    CHECK(rc == -1 && h_errno_last == AMI_EINVAL,
          "RTA_DefaultGateway beside RTA_Gateway is EINVAL");

    /* Two destinations: which one identifies the entry is undecidable, so
       neither does. */
    t[0].ti_Tag  = RTA_Destination;
    t[0].ti_Data = (uintptr_t)"192.168.66.0";
    t[1].ti_Tag  = RTA_DestinationHost;
    t[1].ti_Data = (uintptr_t)"192.168.67.7";
    t[2].ti_Tag  = RTA_Gateway;
    t[2].ti_Data = (uintptr_t)"10.0.2.99";
    t[3] = (struct TagItem)T_END;
    rc = bsd_ChangeRouteTagList(t, BASE);
    CHECK(rc == -1 && h_errno_last == AMI_EINVAL,
          "two destination tags in one list is EINVAL");

    t[0].ti_Tag  = RTA_Destination;
    t[0].ti_Data = (uintptr_t)"not.an.address";
    t[1].ti_Tag  = RTA_Gateway;
    t[1].ti_Data = (uintptr_t)"10.0.2.99";
    t[2] = (struct TagItem)T_END;
    rc = bsd_ChangeRouteTagList(t, BASE);
    CHECK(rc == -1 && h_errno_last == AMI_EINVAL,
          "a destination that resolves to nothing is EINVAL");

    t[0].ti_Tag  = RTA_Destination;
    t[0].ti_Data = (uintptr_t)"192.168.66.0";
    t[1].ti_Tag  = RTA_Gateway;
    t[1].ti_Data = (uintptr_t)"not.an.address";
    t[2] = (struct TagItem)T_END;
    rc = bsd_ChangeRouteTagList(t, BASE);
    CHECK(rc == -1 && h_errno_last == AMI_EINVAL,
          "a gateway that resolves to nothing is EINVAL");

    /* Every one of the above must have left the table exactly as it was. */
    CHECK(h_ip.nx_ip_routing_table_entry_count == 1,
          "no refusal added or removed an entry");
    {
        int   found = 0;
        ULONG hop = h_route_next_hop(0xC0A84200UL, 0xFFFFFF00UL, &found);

        CHECK(found && hop == 0x0A000262UL,
              "and none of them changed the next hop");
    }

    /* No stack at all. */
    h_ip_ptr = NULL;
    t[0].ti_Tag  = RTA_Destination;
    t[0].ti_Data = (uintptr_t)"192.168.66.0";
    t[1].ti_Tag  = RTA_Gateway;
    t[1].ti_Data = (uintptr_t)"10.0.2.99";
    t[2] = (struct TagItem)T_END;
    rc = bsd_ChangeRouteTagList(t, BASE);
    CHECK(rc == -1 && h_errno_last == AMI_ENETDOWN,
          "with no stack it is ENETDOWN, not EINVAL");
    h_ip_ptr = &h_ip;
}

/* ------------------------------------------------------- what is not there */

static VOID t_absent(VOID)
{
    struct TagItem t[3];
    LONG           rc;

    printf("ChangeRouteTagList: a change is not an add\n");

    h_machine_reset();
    h_add_fixture_route();

    /* "ESRCH if requested to delete a non-existent entry", the -route- page;
       RTM_CHANGE addresses an entry that exists for the same reason. */
    t[0].ti_Tag  = RTA_Destination;
    t[0].ti_Data = (uintptr_t)"192.168.99.0";
    t[1].ti_Tag  = RTA_Gateway;
    t[1].ti_Data = (uintptr_t)"10.0.2.99";
    t[2] = (struct TagItem)T_END;
    rc = bsd_ChangeRouteTagList(t, BASE);
    CHECK(rc == -1 && h_errno_last == AMI_ESRCH,
          "changing a route that was never added is ESRCH");
    CHECK(h_ip.nx_ip_routing_table_entry_count == 1,
          "and it did not install one");

    /*
     * The same destination string under a different tag is a different entry,
     * because the tag decides the mask.  192.168.66.0 as a HOST is a /32 and
     * the fixture is a /24, so this must miss.  If it did not, a caller could
     * change a /24 by naming a /32 and the two vectors would disagree about
     * what an entry is.
     */
    t[0].ti_Tag  = RTA_DestinationHost;
    t[0].ti_Data = (uintptr_t)"192.168.66.0";
    t[1].ti_Tag  = RTA_Gateway;
    t[1].ti_Data = (uintptr_t)"10.0.2.99";
    t[2] = (struct TagItem)T_END;
    rc = bsd_ChangeRouteTagList(t, BASE);
    CHECK(rc == -1 && h_errno_last == AMI_ESRCH,
          "RTA_DestinationHost names a /32, which is not the /24 fixture");

    /* The default gateway, with none installed. */
    t[0].ti_Tag  = RTA_DefaultGateway;
    t[0].ti_Data = (uintptr_t)"10.0.2.2";
    t[1] = (struct TagItem)T_END;
    rc = bsd_ChangeRouteTagList(t, BASE);
    CHECK(rc == -1 && h_errno_last == AMI_ESRCH,
          "changing the default gateway when there is none is ESRCH");
    CHECK(h_ip.nx_ip_gateway_address == 0,
          "and it did not install one");
}

/* -------------------------------------------------------------- deletion */

static VOID t_delete_absent(VOID)
{
    struct TagItem t[2];
    LONG           rc;

    printf("DeleteRouteTagList: absent entries are always ESRCH\n");

    t[0].ti_Tag  = RTA_Destination;
    t[0].ti_Data = (uintptr_t)"192.168.99.0";
    t[1] = (struct TagItem)T_END;

    h_machine_reset();
    h_deletes = 0;

    rc = bsd_DeleteRouteTagList(t, BASE);
    CHECK(rc == -1 && h_errno_last == AMI_ESRCH,
          "an absent route in an empty table is ESRCH");
    CHECK(h_deletes == 0,
          "the empty-table NetX success path was not trusted");

    h_add_fixture_route();
    rc = bsd_DeleteRouteTagList(t, BASE);
    CHECK(rc == -1 && h_errno_last == AMI_ESRCH,
          "an absent route in a nonempty table is also ESRCH");
    CHECK(h_deletes == 0,
          "an absent route never reaches the NetX delete call");

    t[0].ti_Data = (uintptr_t)"192.168.66.0";
    rc = bsd_DeleteRouteTagList(t, BASE);
    CHECK(rc == 0, "an existing route is deleted");
    CHECK(h_deletes == 1 && h_ip.nx_ip_routing_table_entry_count == 0,
          "and exactly one table entry was removed");
}

/* ------------------------------------------------------------ what changes */

static VOID t_changes(VOID)
{
    struct TagItem t[3];
    LONG           rc;
    int            found;
    ULONG          hop;

    printf("ChangeRouteTagList: what a change does\n");

    h_machine_reset();
    h_add_fixture_route();
    h_deletes = 0;

    t[0].ti_Tag  = RTA_Destination;
    t[0].ti_Data = (uintptr_t)"192.168.66.0";
    t[1].ti_Tag  = RTA_Gateway;
    t[1].ti_Data = (uintptr_t)"10.0.2.99";
    t[2] = (struct TagItem)T_END;
    rc = bsd_ChangeRouteTagList(t, BASE);
    CHECK(rc == 0, "a route that exists takes a new next hop");

    hop = h_route_next_hop(0xC0A84200UL, 0xFFFFFF00UL, &found);
    CHECK(found && hop == 0x0A000263UL, "and the new next hop is what is stored");
    CHECK(h_ip.nx_ip_routing_table_entry_count == 1,
          "the table still holds exactly one entry");
    CHECK(h_deletes == 0,
          "nothing was deleted: the entry was updated in place");
    CHECK(h_route_iface(0xC0A84200UL, 0xFFFFFF00UL) == &h_ip.nx_ip_interface[0],
          "and it still leaves by the interface the next hop is on");

    /* Same next hop again: a no-op, not an error. */
    rc = bsd_ChangeRouteTagList(t, BASE);
    CHECK(rc == 0, "changing a route to the next hop it already has succeeds");
    CHECK(h_ip.nx_ip_routing_table_entry_count == 1, "and changes nothing");

    /*
     * A next hop on none of this machine's subnets.  nx_ip_static_route_add()
     * checks before it writes, so the route must survive unchanged.
     */
    t[1].ti_Data = (uintptr_t)"172.31.0.1";
    rc = bsd_ChangeRouteTagList(t, BASE);
    CHECK(rc == -1 && h_errno_last == AMI_ENETUNREACH,
          "a next hop reaching no interface is ENETUNREACH");
    hop = h_route_next_hop(0xC0A84200UL, 0xFFFFFF00UL, &found);
    CHECK(found && hop == 0x0A000263UL,
          "and the route still has the next hop it had before");
    CHECK(h_deletes == 0, "a refused change deletes nothing");

    /*
     * The interface change.  10.0.2.99 is on eth0, 192.168.9.1 is on eth1.
     * nx_ip_static_route_add() would leave the entry pointing at eth0 and
     * nx_ip_route_find() would send the packets out of it, so routing.c takes
     * the entry out and puts it back instead.  Asserted on the interface
     * pointer, which is the field the in-place path does not touch.
     */
    t[1].ti_Data = (uintptr_t)"192.168.9.1";
    rc = bsd_ChangeRouteTagList(t, BASE);
    CHECK(rc == 0, "a next hop on the other interface is accepted");
    hop = h_route_next_hop(0xC0A84200UL, 0xFFFFFF00UL, &found);
    CHECK(found && hop == 0xC0A80901UL, "the new next hop is stored");
    CHECK(h_route_iface(0xC0A84200UL, 0xFFFFFF00UL) == &h_ip.nx_ip_interface[1],
          "and the entry now leaves by the interface that next hop is on");
    CHECK(h_deletes == 1,
          "which took a delete and re-add, the one path that is not in place");
    CHECK(h_ip.nx_ip_routing_table_entry_count == 1,
          "and left one entry, not two and not none");

    /* The default gateway. */
    h_machine_reset();
    CHECK(_nxe_ip_gateway_address_set(&h_ip, 0x0A000202UL) == NX_SUCCESS,
          "fixture: a default gateway is installed");

    t[0].ti_Tag  = RTA_DefaultGateway;
    t[0].ti_Data = (uintptr_t)"10.0.2.3";
    t[1] = (struct TagItem)T_END;
    rc = bsd_ChangeRouteTagList(t, BASE);
    CHECK(rc == 0, "the default gateway can be changed when one is installed");
    CHECK(h_ip.nx_ip_gateway_address == 0x0A000203UL,
          "and the new one is what is stored");

    /* Onto the other interface: nx_ip_gateway_address_set() writes both
       fields, so this one has no in-place hole to work around. */
    t[0].ti_Data = (uintptr_t)"192.168.9.1";
    rc = bsd_ChangeRouteTagList(t, BASE);
    CHECK(rc == 0, "and moved to a gateway on the other interface");
    CHECK(h_ip.nx_ip_gateway_address == 0xC0A80901UL &&
          h_ip.nx_ip_gateway_interface == &h_ip.nx_ip_interface[1],
          "with the gateway interface moved along with it");

    t[0].ti_Data = (uintptr_t)"172.31.0.1";
    rc = bsd_ChangeRouteTagList(t, BASE);
    CHECK(rc == -1 && h_errno_last == AMI_ENETUNREACH,
          "a default gateway reaching no interface is ENETUNREACH");
    CHECK(h_ip.nx_ip_gateway_address == 0xC0A80901UL,
          "and the machine keeps the default gateway it had");
}

/* ------------------------------------------------------- the control tags */

static VOID t_tag_walk(VOID)
{
    struct TagItem tail[3];
    struct TagItem head[4];
    LONG           rc;
    int            found;
    ULONG          hop;

    printf("ChangeRouteTagList: TAG_MORE, TAG_SKIP and TAG_IGNORE\n");

    h_machine_reset();
    h_add_fixture_route();

    tail[0].ti_Tag  = RTA_Gateway;
    tail[0].ti_Data = (uintptr_t)"10.0.2.77";
    tail[1] = (struct TagItem)T_END;
    tail[2] = (struct TagItem)T_END;

    head[0].ti_Tag  = TAG_IGNORE;
    head[0].ti_Data = 0;
    head[1].ti_Tag  = RTA_Destination;
    head[1].ti_Data = (uintptr_t)"192.168.66.0";
    head[2].ti_Tag  = TAG_MORE;
    head[2].ti_Data = (uintptr_t)tail;
    head[3] = (struct TagItem)T_END;

    rc = bsd_ChangeRouteTagList(head, BASE);
    CHECK(rc == 0, "a list split across TAG_MORE is one list");
    hop = h_route_next_hop(0xC0A84200UL, 0xFFFFFF00UL, &found);
    CHECK(found && hop == 0x0A00024DUL, "and both halves were read");
}

int main(void)
{
    printf("routing.c host tests\n");

    t_refusals();
    t_absent();
    t_delete_absent();
    t_changes();
    t_tag_walk();

    printf("%lu checks, %lu failures\n", h_checks, h_failures);
    return h_failures == 0 ? 0 : 1;
}

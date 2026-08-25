/*
 * src/bsdsocket/routing.c on the host: ChangeRouteTagList's grammar and its
 * refusals.
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

/* The two network-number shapes exercise routing.c's inet_makeaddr() rule.
   Everything else remains unresolved, which makes the malformed-name cases
   below refusals. */
const AmiNetdbEntry *ami_netdb_net_by_name(const char *name)
{
    static const AmiNetdbEntry three_octet =
        { "three-octet", NULL, 0x00C0A801UL, NULL };
    static const AmiNetdbEntry four_octet =
        { "four-octet", NULL, 0xC0A80100UL, NULL };

    if (strcmp(name, three_octet.name) == 0)
        return &three_octet;
    if (strcmp(name, four_octet.name) == 0)
        return &four_octet;

    return NULL;
}

LONG netstack_resolve(const char *name, ULONG *addr_out, ULONG timeout_ticks)
{
    (VOID)name;
    (VOID)addr_out;
    (VOID)timeout_ticks;
    return -1;                  /* anything but AMI_NET_OK */
}

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

static VOID t_symbolic_networks(VOID)
{
    struct TagItem t[3];
    LONG           rc;
    int            found;

    printf("AddRouteTagList: BSD network-number expansion\n");

    t[0].ti_Tag  = RTA_DestinationNet;
    t[1].ti_Tag  = RTA_Gateway;
    t[1].ti_Data = (uintptr_t)"10.0.2.2";
    t[2] = (struct TagItem)T_END;

    h_machine_reset();
    t[0].ti_Data = (uintptr_t)"three-octet";
    rc = bsd_AddRouteTagList(t, BASE);
    CHECK(rc == 0, "a three-octet network name is accepted");
    (VOID)h_route_next_hop(0xC0A80100UL, 0xFFFFFF00UL, &found);
    CHECK(found, "and 192.168.1 expands to 192.168.1.0");

    h_machine_reset();
    t[0].ti_Data = (uintptr_t)"four-octet";
    rc = bsd_AddRouteTagList(t, BASE);
    CHECK(rc == 0, "a four-octet network name is accepted");
    (VOID)h_route_next_hop(0xC0A80100UL, 0xFFFFFF00UL, &found);
    CHECK(found, "and 192.168.1.0 is not shifted to 168.1.0.0");
}

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

    t[1].ti_Data = (uintptr_t)"172.31.0.1";
    rc = bsd_ChangeRouteTagList(t, BASE);
    CHECK(rc == -1 && h_errno_last == AMI_ENETUNREACH,
          "a next hop reaching no interface is ENETUNREACH");
    hop = h_route_next_hop(0xC0A84200UL, 0xFFFFFF00UL, &found);
    CHECK(found && hop == 0x0A000263UL,
          "and the route still has the next hop it had before");
    CHECK(h_deletes == 0, "a refused change deletes nothing");

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
    t_symbolic_networks();
    t_changes();
    t_tag_walk();

    printf("%lu checks, %lu failures\n", h_checks, h_failures);
    return h_failures == 0 ? 0 : 1;
}

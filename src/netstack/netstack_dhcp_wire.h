/*
 * AmiNetXDuo, the DHCPv4 bytes at the client boundary, and the names of the
 * client's states.
 *
 * The client in NetX Duo does the protocol.  What this stack adds is the
 * option 61 it sends, how it reads the address lists and the text options
 * out of a lease, and what it calls each client state.  Every one of those is
 * a byte layout or a table that a host test can check without a client, so
 * they are here and netstack.c keeps the calls into the client.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_NETSTACK_DHCP_WIRE_H
#define AMINETXDUO_NETSTACK_DHCP_WIRE_H

#include <exec/types.h>

/* Option 61 as this stack sends it: code, length, hardware type 0x01, then the
   six MAC bytes.  NX_DHCP_OPTION_CLIENT_ID and _SIZE, pinned in netstack.c. */
#define AMI_DHCP_OPTION_CLIENT_ID       61
#define AMI_DHCP_CLIENT_ID_SIZE         7
#define AMI_DHCP_CLIENT_ID_LEN          (AMI_DHCP_CLIENT_ID_SIZE + 2)

/*
 * Write option 61 for the MAC NetX Duo keeps as (msw, lsw) into `option`, which
 * has `room` bytes.  The bytes written, or 0 when there is no room for all of
 * them: a short buffer costs the option and nothing else.
 */
UINT ami_ns_dhcp_client_id_build(ULONG msw, ULONG lsw, UCHAR *option,
                                 UINT room);

/*
 * A list-of-addresses option (3, 6, 33), `size` bytes of it, as host-order
 * addresses.  Four bytes per entry, a trailing partial entry is dropped, and
 * so is 0.0.0.0: "a router address of 0 should be ignored", and the same for
 * the other lists.  The count written, at most `max`.
 */
UWORD ami_ns_dhcp_addr_list_decode(const UCHAR *buffer, UINT size,
                                   ULONG *out, UWORD max);

/* A text option (12, 15), not NUL-terminated on the wire.  `out` always is,
   and takes at most outlen - 1 bytes. */
VOID ami_ns_dhcp_text_decode(const UCHAR *buffer, UINT size,
                             char *out, ULONG outlen);

/* The NETSTATUS_DHCPRAW_* number's name, "dhcp-other" past the table. */
const char *ami_ns_dhcp_state_name(UCHAR state);

/* The NETSTATUS_DHCPRAW_* number as AMI_DHCP_IDLE, AMI_DHCP_WORKING or
   AMI_DHCP_BOUND: BOUND, RENEWING and REBINDING all still hold a lease. */
LONG ami_ns_dhcp_state_class(UCHAR state);

#endif /* AMINETXDUO_NETSTACK_DHCP_WIRE_H */

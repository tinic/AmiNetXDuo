/*
 * AmiNetXDuo, the DHCPv4 bytes at the client boundary, and the names of the
 * client's states.
 *
 * SPDX-License-Identifier: MIT
 */

/* tx_api.h and nx_api.h before any exec header: <exec/types.h> turns VOID into
   a macro and that breaks the ThreadX typedefs. */
#include "tx_api.h"
#include "nx_api.h"

#include "netstack_dhcp_wire.h"

#include "aminetxduo/netstack.h"
#include "aminetxduo/netstatus.h"

UINT ami_ns_dhcp_client_id_build(ULONG msw, ULONG lsw, UCHAR *option,
                                 UINT room)
{
    if (option == NULL || room < (UINT)AMI_DHCP_CLIENT_ID_LEN)
        return 0;

    option[0] = AMI_DHCP_OPTION_CLIENT_ID;
    option[1] = AMI_DHCP_CLIENT_ID_SIZE;
    option[2] = 0x01;                           /* RFC 1700 hardware type */
    option[3] = (UCHAR)(msw >> 8);
    option[4] = (UCHAR)(msw);
    option[5] = (UCHAR)(lsw >> 24);
    option[6] = (UCHAR)(lsw >> 16);
    option[7] = (UCHAR)(lsw >> 8);
    option[8] = (UCHAR)(lsw);

    return (UINT)AMI_DHCP_CLIENT_ID_LEN;
}

UWORD ami_ns_dhcp_addr_list_decode(const UCHAR *buffer, UINT size,
                                   ULONG *out, UWORD max)
{
    UWORD count = 0;
    UWORD i;

    if (buffer == NULL || out == NULL)
        return 0;

    for (i = 0; (ULONG)(i + 1) * 4UL <= (ULONG)size && count < max; i++)
    {
        ULONG addr = ((ULONG)buffer[i * 4] << 24) |
                     ((ULONG)buffer[i * 4 + 1] << 16) |
                     ((ULONG)buffer[i * 4 + 2] << 8) |
                      (ULONG)buffer[i * 4 + 3];

        if (addr != 0)
            out[count++] = addr;
    }

    return count;
}

VOID ami_ns_dhcp_text_decode(const UCHAR *buffer, UINT size,
                             char *out, ULONG outlen)
{
    ULONG n;

    if (out == NULL || outlen == 0UL)
        return;

    out[0] = '\0';

    if (buffer == NULL)
        return;

    n = (ULONG)size;
    if (n >= outlen)
        n = outlen - 1;

    for (outlen = 0; outlen < n; outlen++)
        out[outlen] = (char)buffer[outlen];

    out[n] = '\0';
}

const char *ami_ns_dhcp_state_name(UCHAR state)
{
    static const char *const names[] = {
        "dhcp-notstarted", "dhcp-boot",       "dhcp-init",
        "dhcp-selecting",  "dhcp-requesting", "dhcp-bound",
        "dhcp-renewing",   "dhcp-rebinding",  "dhcp-forcerenew",
        "dhcp-probing"
    };

    if ((UINT)state >= (UINT)(sizeof(names) / sizeof(names[0])))
        return "dhcp-other";

    return names[state];
}

LONG ami_ns_dhcp_state_class(UCHAR state)
{
    switch (state)
    {
        case NETSTATUS_DHCPRAW_NOT_STARTED:
            return AMI_DHCP_IDLE;

        case NETSTATUS_DHCPRAW_BOUND:
        case NETSTATUS_DHCPRAW_RENEWING:
        case NETSTATUS_DHCPRAW_REBINDING:
            return AMI_DHCP_BOUND;

        default:
            return AMI_DHCP_WORKING;
    }
}

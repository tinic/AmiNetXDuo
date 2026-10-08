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
#include <stddef.h>

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
#define DHCP_STATE_NAMES(X) \
    X(notstarted, 0, "dhcp-notstarted") \
    X(boot,       1, "dhcp-boot") \
    X(init,       2, "dhcp-init") \
    X(selecting,  3, "dhcp-selecting") \
    X(requesting, 4, "dhcp-requesting") \
    X(bound,      5, "dhcp-bound") \
    X(renewing,   6, "dhcp-renewing") \
    X(rebinding,  7, "dhcp-rebinding") \
    X(forcerenew, 8, "dhcp-forcerenew") \
    X(probing,    9, "dhcp-probing")
    static const struct DhcpStateNames {
#define DHCP_STATE_FIELD(id, index, text) char id[sizeof(text)];
        DHCP_STATE_NAMES(DHCP_STATE_FIELD)
#undef DHCP_STATE_FIELD
    } names = {
#define DHCP_STATE_TEXT(id, index, text) text,
        DHCP_STATE_NAMES(DHCP_STATE_TEXT)
#undef DHCP_STATE_TEXT
    };
    static const UCHAR offsets[] = {
#define DHCP_STATE_OFFSET(id, index, text) [index] = offsetof(struct DhcpStateNames, id),
        DHCP_STATE_NAMES(DHCP_STATE_OFFSET)
#undef DHCP_STATE_OFFSET
    };
    _Static_assert(sizeof(names) <= 256, "DHCP state name offsets must fit in UCHAR");
#undef DHCP_STATE_NAMES

    if ((UINT)state >= (UINT)(sizeof(offsets) / sizeof(offsets[0])))
        return "dhcp-other";

    return (const char *)&names + offsets[state];
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

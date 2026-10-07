/*
 * The words that go with the numbers in the library's event ring.
 *
 * NOT ONE OF THESE STRINGS MAY REACH A SHIPPED LIBRARY OR DEVICE.
 * tools/check-no-diag-strings.sh reads this file and fails the build if one is
 * found in any linked image. A code this table does not know prints as a number.
 *
 * SPDX-License-Identifier: MIT
 */

#include "aminetxduo/events.h"
#include "tool_events.h"

/*
 * One row per code. ev_Detail says what nse_Value means for this code, or is
 * NULL when the value carries nothing.
 */
typedef struct ToolEventRow
{
    UWORD       ev_Code;
    const char *ev_Text;
    const char *ev_Detail;
} ToolEventRow;

static const ToolEventRow tool_event_rows[] =
{
    { NETEVENT_BRINGUP,
      "the stack came up",
      "interfaces" },
    { NETEVENT_SHUTDOWN,
      "the stack began shutting down",
      "interfaces held" },
    { NETEVENT_NOTIFY,
      "programs told the stack is stopping",
      "programs signalled" },
    { NETEVENT_RELEASE,
      "network hold released",
      "openers left" },

    { NETEVENT_DEVICE_OPEN,
      "the SANA-II device did not open",
      "error" },
    { NETEVENT_DEVICE_REFUSED,
      "SANA-II device refused a command",
      "error" },
    { NETEVENT_ATTACH_FAILED,
      "interface not attached",
      "NetX Duo status" },
    { NETEVENT_LINK_DOWN,
      "link down at attach",
      NULL },
    { NETEVENT_ONLINE_FAILED,
      "driver refused S2_ONLINE",
      "NetX Duo status" },
    { NETEVENT_ATTACH_LIMIT,
      "no free interface slot; not attached",
      "interfaces described" },
    { NETEVENT_ATTACH_YIELD,
      "slot yielded to a named interface",
      "interfaces described" },
    { NETEVENT_GATEWAY_REFUSED,
      "default route refused: gateway on no attached network",
      "NetX Duo status" },

    { NETEVENT_ADDR_REFUSED,
      "configured address refused",
      "NetX Duo status" },
    { NETEVENT_DHCP_UNREPORTED,
      "no DHCP bind notification",
      "NetX Duo status" },
    { NETEVENT_DHCP6_LIMIT,
      "DHCPv6 unavailable: the single client serves another interface",
      "client interface" },
    { NETEVENT_NETDB_NOMEM,
      "netdb table not loaded: out of memory",
      "tables (1 hosts, 2 networks, 4 protocols, 8 services)" },
    { NETEVENT_ADDR_UNREPORTED,
      "no address-change notification",
      "NetX Duo status" },

    { NETEVENT_OUT_OF_SERVICE,
      "the device went out of service and the link was marked down",
      NULL },
    { NETEVENT_OFFLINE_SKIPPED,
      "S2_OFFLINE skipped: already offline",
      NULL },
    { NETEVENT_OFFLINE_FAILED,
      "the device refused S2_OFFLINE",
      "wire error" },

    { NETEVENT_IFACE_RETAINED,
      "interface kept: device holds requests",
      NULL },
    { NETEVENT_STACK_RETAINED,
      "packet pool and stack memory kept: device holds requests",
      "interfaces retained" },

    { NETEVENT_EXPUNGE_DECLINED,
      "the library declined to be unloaded",
      NULL },
};

#define TOOL_EVENT_ROWS \
    (sizeof(tool_event_rows) / sizeof(tool_event_rows[0]))

/* The two codes whose value is a name rather than a number. */

static const char *tool_event_held(ULONG value)
{
    switch (value & (NETEVENT_HELD_RX | NETEVENT_HELD_TX))
    {
    case NETEVENT_HELD_RX:
        return "a read";
    case NETEVENT_HELD_TX:
        return "a write";
    case NETEVENT_HELD_RX | NETEVENT_HELD_TX:
        return "a read and a write";
    default:
        break;
    }

    return NULL;
}

static const char *tool_event_expunge(ULONG value)
{
    switch (value)
    {
    case NETEVENT_EXP_OPEN:
        return "a program still has it open";
    case NETEVENT_EXP_KERNEL:
        return "the ThreadX kernel would not stop";
    case NETEVENT_EXP_TCP:
        return "the TCP: handler is running";
    case NETEVENT_EXP_ADDRALLOC:
        return "an address allocation is still running";
    case NETEVENT_EXP_NETMON:
        return "a monitoring hook is installed";
    case NETEVENT_EXP_RETAINED:
        return "a SANA-II device still holds requests";
    default:
        break;
    }

    return NULL;
}

const char *tool_event_text(UWORD code)
{
    ULONG i;

    for (i = 0; i < (ULONG)TOOL_EVENT_ROWS; i++)
    {
        if (tool_event_rows[i].ev_Code == code)
            return tool_event_rows[i].ev_Text;
    }

    return NULL;
}

const char *tool_event_detail(UWORD code)
{
    ULONG i;

    for (i = 0; i < (ULONG)TOOL_EVENT_ROWS; i++)
    {
        if (tool_event_rows[i].ev_Code == code)
            return tool_event_rows[i].ev_Detail;
    }

    return NULL;
}

const char *tool_event_value_name(UWORD code, ULONG value)
{
    switch (code)
    {
    case NETEVENT_IFACE_RETAINED:
        return tool_event_held(value);
    case NETEVENT_EXPUNGE_DECLINED:
        return tool_event_expunge(value);
    default:
        break;
    }

    return NULL;
}

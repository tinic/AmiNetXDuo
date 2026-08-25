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
      "every program using the network was told it is stopping",
      "programs signalled" },
    { NETEVENT_RELEASE,
      "the reference that keeps the network standing was given back",
      "openers left" },

    { NETEVENT_DEVICE_OPEN,
      "the SANA-II device did not open",
      "OpenDevice error" },
    { NETEVENT_DEVICE_REFUSED,
      "the SANA-II device opened and then refused a command",
      "error" },
    { NETEVENT_ATTACH_FAILED,
      "the interface was not taken into the stack",
      "NetX Duo status" },
    { NETEVENT_LINK_DOWN,
      "the interface joined the stack with its link down, so it carries no "
      "traffic",
      NULL },
    { NETEVENT_ONLINE_FAILED,
      "the interface would not go online, so the driver was sent S2_ONLINE "
      "and refused it",
      "NetX Duo status" },
    { NETEVENT_ATTACH_LIMIT,
      "there was no free interface slot, so this interface was described but "
      "never attached",
      "interfaces described" },
    { NETEVENT_ATTACH_YIELD,
      "this interface was brought up on its own and gave its slot back to an "
      "interface that was asked for by name",
      "interfaces described" },
    { NETEVENT_GATEWAY_REFUSED,
      "the interface is up and the default route it asked for was refused, "
      "because that address is on no network this machine is on",
      "NetX Duo status" },

    { NETEVENT_OUT_OF_SERVICE,
      "the device went out of service and the link was marked down",
      NULL },
    { NETEVENT_OFFLINE_SKIPPED,
      "S2_OFFLINE was not sent, the interface was marked offline already",
      NULL },
    { NETEVENT_OFFLINE_FAILED,
      "the device refused S2_OFFLINE",
      "wire error" },

    { NETEVENT_IFACE_RETAINED,
      "the interface was left in memory, the device still holds requests "
      "inside it",
      NULL },
    { NETEVENT_STACK_RETAINED,
      "the packet pool and the stack memory were kept, a device still owns "
      "requests into them",
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

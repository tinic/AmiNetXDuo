/*
 * The words that go with the numbers in the library's event ring.
 *
 * WHY THEY ARE HERE.  bsdsocket.library is resident for the life of the
 * machine and its diagnostics would be resident with it, to be printed at most
 * once; that is why AMINETXDUO_LOG is off in every shipped build and why every
 * AMI_ERROR in the tree is absent from the binary a user has.  So the library
 * records a code and this table turns it into a sentence.  The user never sees
 * a code.  aminetxduo/anxdiag.h and CheckNetDevice are the same arrangement for
 * anxnet.device, and this is deliberately the same one rather than a second.
 *
 * NOT ONE OF THESE STRINGS MAY REACH A SHIPPED LIBRARY OR DEVICE.
 * tools/check-no-diag-strings.sh reads this file, takes every sentence out of
 * it, and fails the build if one is found in any linked image.  Adding a
 * sentence here extends that check by itself; there is no list to keep in step.
 *
 * A code this table does not know is printed as its number.  The numbers are a
 * wire format between two binaries, so an older command against a newer library
 * still says something true rather than dropping the entry.
 *
 * SPDX-License-Identifier: MIT
 */

#include "aminetxduo/events.h"
#include "tool_events.h"

/*
 * One row per code.  The sentence is what happened, in the past tense, with no
 * subject the reader has to supply: these are printed in a list, under a
 * heading, in the order they occurred.
 *
 * ev_Detail says what nse_Value means for this code, or is NULL when the value
 * carries nothing.  The caller prints it after the number.
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

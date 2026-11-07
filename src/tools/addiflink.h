/*
 * AddNetInterface: wait for a Wi-Fi card to join its network before the
 * interface is attached (GitHub #457).
 *
 * WirelessManager is started in the background just before AddNetInterface
 * (S:Network-Startup).  Attached at once, the interface spends its TIMEOUT
 * on DHCP requests that an unjoined card drops, and the stack opens the unit
 * while WirelessManager is still setting it up.  Attached after the join, as
 * WaitForLink (driver/net/prism2v2) does it, the first DISCOVER goes out on a
 * keyed link.  The wait shares the command's one TIMEOUT with DHCP.
 *
 * prism2.device 2.9 answers S2_ONEVENT with the state it is in: ONLINE or
 * OFFLINE, always true, so the request returns at once, plus LINKUP
 * (associated and keyed) or LINKDOWN.  An older prism2.device refuses
 * the LINK bits, which ends the wait.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef AMINETXDUO_ADDIFLINK_H
#define AMINETXDUO_ADDIFLINK_H

/* sana2.h and prism2.device 2.9; netwireless_dev.h has the same values. */
#define ADDIF_S2EVENT_ONLINE    (1UL << 3)
#define ADDIF_S2EVENT_OFFLINE   (1UL << 4)
#define ADDIF_S2EVENT_LINKUP    (1UL << 16)
#define ADDIF_S2EVENT_LINKDOWN  (1UL << 17)

/* Seconds a Wireless.prefs counts as a WirelessManager not yet seen:
   S:Network-Startup Runs it one line before AddNetInterface. */
#define ADDIF_WM_GRACE          5UL

enum
{
    ADDIF_LINK_GO,                  /* attach now: nothing to wait for     */
    ADDIF_LINK_UP,                  /* joined: attach now                  */
    ADDIF_LINK_WAIT                 /* not joined yet: ask again in 1 s    */
};

/*
 * One S2_ONEVENT answer.  io_error: the request's io_Error.  events: its
 * ios2_WireError.  supplicant: WirelessManager runs, or is expected to.
 * An unconfigured (OFFLINE) unit with no supplicant is left to the stack's
 * own S2_CONFIGINTERFACE, which is what joins an open network.
 */
static inline int addif_link_verdict(long io_error, unsigned long events,
                                     int supplicant)
{
    if (io_error != 0)
        return ADDIF_LINK_GO;
    if ((events & ADDIF_S2EVENT_LINKUP) != 0)
        return ADDIF_LINK_UP;
    if ((events & ADDIF_S2EVENT_OFFLINE) != 0)
        return supplicant ? ADDIF_LINK_WAIT : ADDIF_LINK_GO;
    if ((events & ADDIF_S2EVENT_LINKDOWN) != 0)
        return ADDIF_LINK_WAIT;
    return ADDIF_LINK_GO;
}

/* Whether the file part of `device` is one of `known`, any case. */
static inline int addif_driver_in(const char *device,
                                  const char *const *known)
{
    const char *base = device;
    int         i;

    if (device == 0)
        return 0;
    for (; *device != '\0'; device++)
        if (*device == ':' || *device == '/')
            base = device + 1;
    for (i = 0; known[i] != 0; i++)
    {
        const char *a = base, *b = known[i];

        while (*a != '\0' &&
               (*a == *b || (*a >= 'A' && *a <= 'Z' && *a + 32 == *b)))
        {
            a++;
            b++;
        }
        if (*a == '\0' && *b == '\0')
            return 1;
    }
    return 0;
}

/* The drivers asked: those whose S2_ONEVENT answers the link at once. */
static inline int addif_link_driver(const char *device)
{
    static const char *const known[] = { "prism2.device", 0 };

    return addif_driver_in(device, known);
}

/* The Wi-Fi drivers S:Network-Startup starts a WirelessManager for, as
   nw_wireless_driver() knows them. */
static inline int addif_wifi_driver(const char *device)
{
    static const char *const known[] =
        { "anxwifipi.device", "wifipi.device", "prism2.device", 0 };

    return addif_driver_in(device, known);
}

/*
 * The open gate (#457).  A driver that creates its unit inside OpenDevice()
 * and waits there, as prism2.device 2.9 does (GetPCCardUnit(), pccard.c:
 * the unit is listed only once InitialiseCard() and CreateUnit() have
 * returned), creates a second unit for an OpenDevice() made meanwhile.
 * AddNetInterface run at once after `Run WirelessManager` did that:
 * WirelessManager failed ("Failed to initialize driver interface") or the
 * A1200 restarted.  So while a supplicant is expected for the device, the
 * unit is opened only once the supplicant's open has finished: the driver's
 * unit process, named after the device, exists (unit), or, for a driver
 * that names none, the device has had an opener for ADDIF_OPEN_SETTLE
 * seconds (held).
 */
#define ADDIF_OPEN_SETTLE       2UL

static inline int addif_open_now(int supplicant, int unit, unsigned long held)
{
    return !supplicant || unit || held >= ADDIF_OPEN_SETTLE;
}

/* Whether WirelessManager arguments `args` name `device`: its file part,
   any case, as a whole word. */
static inline int addif_args_name(const char *args, const char *device)
{
    const char *base = device;
    const char *p;

    if (args == 0 || device == 0)
        return 0;
    for (; *device != '\0'; device++)
        if (*device == ':' || *device == '/')
            base = device + 1;
    if (*base == '\0')
        return 0;
    for (p = args; *p != '\0'; p++)
    {
        const char *a = p, *b = base;

        if (p != args && p[-1] != ':' && p[-1] != '/' && p[-1] != '=' &&
            p[-1] != '"' && p[-1] != ' ' && p[-1] != '\t')
            continue;
        while (*a != '\0' && *b != '\0' &&
               (*a == *b || (*a >= 'A' && *a <= 'Z' && *a + 32 == *b) ||
                (*b >= 'A' && *b <= 'Z' && *b + 32 == *a)))
        {
            a++;
            b++;
        }
        if (*b == '\0' && (*a == '\0' || *a == ' ' || *a == '"' ||
                            *a == '\t' || *a == '\n'))
            return 1;
    }
    return 0;
}

#ifdef __amigaos__
#include "tools.h"

typedef struct AddifLink
{
    struct MsgPort    *port;
    struct IOSana2Req *req;
    BOOL               open;
} AddifLink;

/*
 * Any driver: returns once the unit may be opened (addif_open_now()), or
 * `wait` has run out.  Before the first OpenDevice() of the interface.
 */
VOID addif_open_gate(const AmiIfConfig *ifc, ToolWait *wait);

/*
 * For a driver addif_link_driver() names: opens the unit and waits, one
 * second at a time on `wait`, until it has joined or nothing will join it.
 * TRUE when it has joined.  The unit stays open until addif_link_close(),
 * which the caller does after the attach: a last CloseDevice() before the
 * stack's OpenDevice() would have prism2.device delete the unit and reset
 * the card under WirelessManager.
 */
BOOL addif_link_wait(AddifLink *link, const AmiIfConfig *ifc, ToolWait *wait);
VOID addif_link_close(AddifLink *link);
#endif

#endif

/*
 * AddNetInterface's Wi-Fi join wait (GitHub #457): which S2_ONEVENT answers
 * hold the attach back, and which drivers are asked at all.
 *
 * SPDX-License-Identifier: MIT
 */
#include "addiflink.h"

#include <stdio.h>

static int checks, failures;

#define CHECK(c) do { checks++; if (!(c)) { failures++; \
    printf("  FAIL line %d: %s\n", __LINE__, #c); } } while (0)

#define ON      ADDIF_S2EVENT_ONLINE
#define OFF     ADDIF_S2EVENT_OFFLINE
#define UP      ADDIF_S2EVENT_LINKUP
#define DOWN    ADDIF_S2EVENT_LINKDOWN

int main(void)
{
    /* prism2.device 2.9, joined and keyed: attach. */
    CHECK(addif_link_verdict(0, ON | UP, 1) == ADDIF_LINK_UP);
    CHECK(addif_link_verdict(0, ON | UP, 0) == ADDIF_LINK_UP);

    /* Configured, not joined yet (WirelessManager scanning or keying). */
    CHECK(addif_link_verdict(0, ON | DOWN, 1) == ADDIF_LINK_WAIT);
    CHECK(addif_link_verdict(0, ON | DOWN, 0) == ADDIF_LINK_WAIT);

    /* Not configured yet: WirelessManager is about to; without one the
       stack's S2_CONFIGINTERFACE is what starts the card. */
    CHECK(addif_link_verdict(0, OFF | DOWN, 1) == ADDIF_LINK_WAIT);
    CHECK(addif_link_verdict(0, OFF, 1) == ADDIF_LINK_WAIT);
    CHECK(addif_link_verdict(0, OFF | DOWN, 0) == ADDIF_LINK_GO);
    CHECK(addif_link_verdict(0, OFF, 0) == ADDIF_LINK_GO);

    /* prism2.device before 2.9 refuses the LINK bits: any io_Error, no
       wait. */
    CHECK(addif_link_verdict(3, 0, 1) == ADDIF_LINK_GO);
    CHECK(addif_link_verdict(-3, 0, 1) == ADDIF_LINK_GO);

    /* A driver that answers ONLINE and says nothing of the link. */
    CHECK(addif_link_verdict(0, ON, 1) == ADDIF_LINK_GO);

    /* Asked: prism2.device under any path and case.  Not asked: the rest. */
    CHECK(addif_link_driver("prism2.device") == 1);
    CHECK(addif_link_driver("DEVS:Networks/prism2.device") == 1);
    CHECK(addif_link_driver("Networks/Prism2.device") == 1);
    CHECK(addif_link_driver("DEVS:Networks/genet.device") == 0);
    CHECK(addif_link_driver("anxnet.device") == 0);
    CHECK(addif_link_driver("prism2.device.old") == 0);
    CHECK(addif_link_driver("xprism2.device") == 0);
    CHECK(addif_link_driver("") == 0);
    CHECK(addif_link_driver(0) == 0);

    printf("addiflink: %d checks, %d failure(s)\n", checks, failures);
    return failures != 0;
}

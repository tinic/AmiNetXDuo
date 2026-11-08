/*
 * Online/Offline driver-name resolution (F-172): without UNIT a driver's only
 * interface is found whatever its unit, two or more ask for UNIT, and an
 * explicit UNIT still picks exactly that unit.
 *
 * SPDX-License-Identifier: MIT
 */
#include "onoffpick.h"

#include <stdio.h>

static int checks, failures;

#define CHECK(c) do { checks++; if (!(c)) { failures++; \
    printf("  FAIL line %d: %s\n", __LINE__, #c); } } while (0)

int main(void)
{
    int pick;

    {   /* The case that failed: one interface, on unit 1, no UNIT given. */
        const unsigned long u[] = { 1 };
        CHECK(onoff_pick(0, 0, u, 1, &pick) == 1 && pick == 0);
    }
    {   /* One interface on unit 0: unchanged. */
        const unsigned long u[] = { 0 };
        CHECK(onoff_pick(0, 0, u, 1, &pick) == 1 && pick == 0);
    }
    {   /* Two units of one driver, no UNIT: ambiguous, never a guess. */
        const unsigned long u[] = { 0, 1 };
        CHECK(onoff_pick(0, 0, u, 2, &pick) == 2);
    }
    {   /* Explicit UNIT picks that unit, and only that. */
        const unsigned long u[] = { 0, 1 };
        CHECK(onoff_pick(1, 1, u, 2, &pick) == 1 && pick == 1);
        CHECK(onoff_pick(1, 0, u, 2, &pick) == 1 && pick == 0);
        CHECK(onoff_pick(1, 2, u, 2, &pick) == 0 && pick == -1);
    }
    {   /* No interface uses the driver. */
        CHECK(onoff_pick(0, 0, 0, 0, &pick) == 0 && pick == -1);
    }

    /* Offline: STATE=DOWN leaves the device online, so it is not done. */
    CHECK(!onoff_already(0, 0, 1));
    CHECK(!onoff_already(0, 1, 1));
    CHECK(!onoff_already(0, 1, 0));
    CHECK(onoff_already(0, 0, 0));

    /* Online: the stack's link decides, as before. */
    CHECK(onoff_already(1, 1, 1));
    CHECK(onoff_already(1, 1, 0));
    CHECK(!onoff_already(1, 0, 1));
    CHECK(!onoff_already(1, 0, 0));

    printf("onoffpick: %d checks, %d failure(s)\n", checks, failures);
    return failures != 0;
}

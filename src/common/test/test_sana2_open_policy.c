/* X-Surf-only AMITCP port guard, compiled against the shipping predicate.
 * SPDX-License-Identifier: MIT
 */

#include "../sana2_open_policy.h"

#include <stdio.h>

static unsigned int checks;
static unsigned int failures;

static void check(const char *name, int expected)
{
    int actual = ami_sana2_needs_amitcp_guard(name);

    checks++;
    if (actual != expected)
    {
        failures++;
        printf("FAIL: %s: got %d, expected %d\n",
               name != NULL ? name : "(null)", actual, expected);
    }
}

int main(void)
{
    check("x-surf.device", 1);
    check("x-surf-100.device", 1);
    check("x-surf-500.device", 1);
    check("X-SURF-100.DEVICE", 1);
    check("DEVS:Networks/x-surf.device", 1);
    check("DH0:Devs/Networks/X-Surf-500.Device", 1);
    check("anxnet.device", 0);
    check("ZZ9000Net.device", 0);
    check("prism2.device", 0);
    check("3c589.device", 0);
    check("x-surf-100.device.old", 0);
    check("x-surf-300.device", 0);
    check("x-surf-", 0);
    check("x-surf-1", 0);
    check("my-x-surf.device", 0);
    check("DEVS:Networks/x-surf-100.device/other", 0);
    check("", 0);
    check(NULL, 0);

    printf("sana2 open policy: %u checks, %u failures\n", checks, failures);
    return failures != 0;
}

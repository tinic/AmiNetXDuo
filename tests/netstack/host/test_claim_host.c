/* Stable interface-slot claims in the shipping netstack implementation.
 * SPDX-License-Identifier: MIT
 */

#include "netstack_host_env.h"

#include "aminetxduo/netstack.h"

#include <stdio.h>

static unsigned long checks;
static unsigned long failures;

#define CHECK(cond, what)                                                     \
    do {                                                                      \
        checks++;                                                            \
        if (!(cond)) {                                                       \
            failures++;                                                      \
            printf("  FAIL %s\n", (what));                                   \
        }                                                                     \
    } while (0)

int main(void)
{
    AmiNetStack *ns;
    UWORD        index = 99;

    nsh_reset();
    CHECK(netstack_startup() == AMI_NET_OK, "the stack starts");
    ns = netstack_get();
    CHECK(ns != NULL, "the live stack is visible");

    CHECK(netstack_interface_claim("ETH0", &index) == AMI_NET_OK,
          "a name uses AmigaOS case rules");
    CHECK(index == 0 && ns->ns_IfaceClaims[0] == 1,
          "the first claim pins slot zero");
    CHECK(netstack_interface_claim("eth0", &index) == AMI_NET_OK &&
          ns->ns_IfaceClaims[0] == 2,
          "a second user has its own claim");

    netstack_interface_release(0);
    netstack_interface_release(0);
    netstack_interface_release(0);
    CHECK(ns->ns_IfaceClaims[0] == 0, "release never underflows");

    index = 99;
    ns->ns_IfaceClaims[0] = (UWORD)-1;
    CHECK(netstack_interface_claim("eth0", &index) == AMI_NET_ERR_BUSY,
          "a saturated claim count is refused");
    CHECK(index == 99, "a refused claim does not publish a slot");
    ns->ns_IfaceClaims[0] = 0;

    CHECK(netstack_interface_claim("absent", &index) == AMI_NET_ERR_STATE,
          "an absent interface is not claimed");
    CHECK(netstack_interface_claim(NULL, &index) == AMI_NET_ERR_CONFIG,
          "a missing name is invalid");
    CHECK(netstack_interface_claim("eth0", NULL) == AMI_NET_ERR_CONFIG,
          "a missing output is invalid");

    netstack_shutdown();
    netstack_shutdown();
    CHECK(netstack_interface_claim("eth0", &index) == AMI_NET_ERR_STATE,
          "no claim survives stack shutdown");

    printf("\n%lu checks, %lu failure(s)\n", checks, failures);
    return failures == 0 ? 0 : 1;
}

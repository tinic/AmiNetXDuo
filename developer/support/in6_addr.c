/* Optional link support for C runtimes without the RFC 3493 data symbols.
 * Compile and link this file only if the runtime does not provide them.
 * SPDX-License-Identifier: MIT */
#include <netinet/in.h>
const struct in6_addr in6addr_any = IN6ADDR_ANY_INIT;
const struct in6_addr in6addr_loopback = IN6ADDR_LOOPBACK_INIT;

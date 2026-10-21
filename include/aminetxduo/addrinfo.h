/* AmiNetXDuo resolver extension (bsdsocket.library 4.11 or later).
 * Identify the stack before use, or retry without this flag on EAI_BADFLAGS.
 * Kept out of common netdb.h so rebuilding an IPv4 program does not silently
 * enable an unsupported flag on Roadshow or other classic stacks.
 * SPDX-License-Identifier: MIT */
#ifndef AMINETXDUO_ADDRINFO_H
#define AMINETXDUO_ADDRINFO_H
#include <netdb.h>
#define AI_ADDRCONFIG 0x0400
#endif

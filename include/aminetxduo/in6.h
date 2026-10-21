/* Compatibility include; standard IPv6 names live in the common SDK.
 * SPDX-License-Identifier: MIT */
#ifndef AMINETXDUO_IN6_H
#define AMINETXDUO_IN6_H
#include <stddef.h>
#include <sys/types.h>
#include <netinet/in.h>
#if defined(__amigaos__) && !defined(SIN6_LEN)
#error "Use the AmiNetXDuo Developer/include headers before the old netinclude headers"
#endif
#include <netdb.h>
#endif

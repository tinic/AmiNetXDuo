/*
 * <libraries/bsdsocket.h> for the bsdsocket host tests.
 *
 * The SBTC_* tags are ABI constants and are NOT restated here: a second copy
 * of a number the NDK owns is a number that can disagree with it.  What is
 * here is only what the include chain needs to parse.  A test that reaches a
 * tag adds it, from the NDK, with the autodoc reference beside it.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef AMINETXDUO_BSD_TEST_LIBRARIES_BSDSOCKET_H
#define AMINETXDUO_BSD_TEST_LIBRARIES_BSDSOCKET_H
#include <exec/types.h>
#include <exec/libraries.h>
#include <utility/tagitem.h>

/*
 * The routing API's five tags, the whole of its vocabulary.  NDK 3.2
 * SANA+RoadshowTCP-IP/netinclude/libraries/bsdsocket.h:358-371; the meanings
 * are in doc/bsdsocket.doc under AddRouteTagList and DeleteRouteTagList,
 * except ChangeRouteTagList's, which has no page (src/bsdsocket/routing.c).
 */
#define RTA_BASE                (TAG_USER + 1600)
#define RTA_Destination         (RTA_BASE + 1)
#define RTA_Gateway             (RTA_BASE + 2)
#define RTA_DefaultGateway      (RTA_BASE + 3)
#define RTA_DestinationHost     (RTA_BASE + 4)
#define RTA_DestinationNet      (RTA_BASE + 5)

#endif

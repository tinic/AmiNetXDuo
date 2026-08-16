/* struct TagItem, for the bsdsocket host tests.

   ti_Data is uintptr_t, not ULONG.  On the Amiga they are the same width and
   half the tags in <libraries/bsdsocket.h> put a pointer in it; on an x86_64
   host ULONG is four bytes (ThreadX's tx_port.h for linux, see the comment in
   ../../../CMakeLists.txt) and a string address does not fit.  The layout
   stops matching the target's, which no test here reads, and every tag list a
   test writes carries the pointer the code under test dereferences, which
   every test here depends on.

   SPDX-License-Identifier: MIT */
#ifndef AMINETXDUO_BSD_TEST_UTILITY_TAGITEM_H
#define AMINETXDUO_BSD_TEST_UTILITY_TAGITEM_H
#include <stdint.h>
struct TagItem { ULONG ti_Tag; uintptr_t ti_Data; };
#define TAG_DONE   0UL
#define TAG_END    0UL
#define TAG_IGNORE 1UL
#define TAG_MORE   2UL
#define TAG_SKIP   3UL
#define TAG_USER   (1UL<<31)
#endif

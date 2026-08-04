/* struct TagItem, for the bsdsocket host tests.
   SPDX-License-Identifier: MIT */
#ifndef AMINETXDUO_BSD_TEST_UTILITY_TAGITEM_H
#define AMINETXDUO_BSD_TEST_UTILITY_TAGITEM_H
struct TagItem { ULONG ti_Tag; ULONG ti_Data; };
#define TAG_DONE   0UL
#define TAG_END    0UL
#define TAG_IGNORE 1UL
#define TAG_MORE   2UL
#define TAG_SKIP   3UL
#define TAG_USER   (1UL<<31)
#endif

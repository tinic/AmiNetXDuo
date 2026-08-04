/* <proto/utility.h> for the bsdsocket host tests.
   SPDX-License-Identifier: MIT */
#ifndef AMINETXDUO_BSD_TEST_PROTO_UTILITY_H
#define AMINETXDUO_BSD_TEST_PROTO_UTILITY_H
#include <exec/types.h>
#include <utility/tagitem.h>
ULONG GetTagData(ULONG tagValue, ULONG defaultVal, const struct TagItem *tagList);
struct TagItem *FindTagItem(ULONG tagValue, const struct TagItem *tagList);
struct TagItem *NextTagItem(struct TagItem **tagListPtr);
#endif

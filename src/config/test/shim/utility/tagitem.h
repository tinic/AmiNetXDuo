/* Host test shim, struct TagItem and the two tag values, from
   <utility/tagitem.h>.  SPDX-License-Identifier: MIT */

#ifndef AMINETXDUO_TEST_UTILITY_TAGITEM_H
#define AMINETXDUO_TEST_UTILITY_TAGITEM_H

#include <exec/types.h>

struct TagItem {
    ULONG ti_Tag;
    ULONG ti_Data;
};

#define TAG_DONE    0UL
#define TAG_USER    (1UL << 31)

#endif

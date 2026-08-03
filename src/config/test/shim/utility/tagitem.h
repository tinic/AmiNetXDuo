/*
 * Host test shim -- struct TagItem, referenced only through pointers.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_TEST_UTILITY_TAGITEM_H
#define AMINETXDUO_TEST_UTILITY_TAGITEM_H

#include <exec/types.h>

struct TagItem {
    ULONG ti_Tag;
    ULONG ti_Data;
};

#endif

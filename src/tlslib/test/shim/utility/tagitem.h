/*
 * Minimal utility/tagitem.h for tls_conn.c's host regression.
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_TLS_TEST_TAGITEM_H
#define AMINETXDUO_TLS_TEST_TAGITEM_H

#include <exec/types.h>

typedef ULONG Tag;

#define TAG_DONE    0UL
#define TAG_IGNORE  1UL
#define TAG_MORE    2UL
#define TAG_SKIP    3UL
#define TAG_USER    (1UL << 31)

struct TagItem
{
    Tag   ti_Tag;
    ULONG ti_Data;
};

#endif

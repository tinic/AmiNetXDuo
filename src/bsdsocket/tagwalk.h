/*
 * bsdsocket.library -- NextTagItem(), open-coded.
 *
 * utility.library is not open in a shared library that may be called before
 * anything else has opened it, and the four control tags are three lines of
 * arithmetic. errno.c does the same inline for SocketBaseTagList(); this
 * header exists because the Roadshow tag-list vectors in interfaces.c and
 * routing.c both need it and a second hand-written copy is a second place for
 * TAG_MORE to be got wrong.
 *
 * Static rather than a linked symbol: it compiles to a few instructions, and
 * a header that adds no link-time surface is easier to be sure about than one
 * that does.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_BSDSOCKET_TAGWALK_H
#define AMINETXDUO_BSDSOCKET_TAGWALK_H

#include "bsdsocket_internal.h"

/*
 * The next real tag, advancing *cursor past it, or NULL at the end of the
 * list. TAG_MORE follows the chain, TAG_SKIP skips ti_Data further items and
 * TAG_IGNORE skips itself -- none of the three is ever handed to the caller.
 */
static struct TagItem *bsd_next_tag(struct TagItem **cursor)
{
    struct TagItem *item = *cursor;

    while (item != NULL)
    {
        switch (item->ti_Tag)
        {
            case TAG_DONE:
                *cursor = NULL;
                return NULL;

            case TAG_IGNORE:
                item++;
                continue;

            case TAG_MORE:
                item = (struct TagItem *)item->ti_Data;
                continue;

            case TAG_SKIP:
                item += 1 + (LONG)item->ti_Data;
                continue;

            default:
                *cursor = item + 1;
                return item;
        }
    }

    *cursor = NULL;
    return NULL;
}

#endif /* AMINETXDUO_BSDSOCKET_TAGWALK_H */

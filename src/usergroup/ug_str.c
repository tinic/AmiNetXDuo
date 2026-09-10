/*
 * AmiNetXDuo, usergroup.library: the three string helpers, split out.
 *
 * WHY THEY ARE NOT IN ug_library.c ANY MORE.  That file carries the romtag and
 * a raw `__asm__()` block, so it cannot be compiled off-target at all -- and
 * these three were therefore STUBBED in every host test that needed them
 * (test_ug_db, test_ug_ids, test_ug_misc), with libc strcmp/strlen standing in.
 * That is not the same code: ug_strcmp(NULL, x) returns 1 and never a negative,
 * ug_strlen(NULL) is 0, and libc has undefined behaviour for both.  So the
 * tests agreed with a stub while the shipped implementation went unexercised.
 *
 * Nothing here touches exec or dos, so both the library and the tests link
 * this file and there is one implementation to disagree with.
 *
 * SPDX-License-Identifier: MIT
 */

#include "usergroup_internal.h"

#include "aminetxduo/compat.h"

ULONG ug_strlen(const char *s)
{
    const char *p = s;

    if (s == NULL)
        return 0;
    while (*p != '\0')
        p++;

    return (ULONG)(p - s);
}

void ug_strncpy(char *dst, const char *src, ULONG size)
{
    ULONG i = 0;

    if (dst == NULL || size == 0)
        return;
    if (src != NULL)
    {
        while (i + 1 < size && src[i] != '\0')
        {
            dst[i] = src[i];
            i++;
        }
    }
    dst[i] = '\0';
}

int ug_strcmp(const char *a, const char *b)
{
    if (a == NULL || b == NULL)
        return (a == b) ? 0 : 1;

    while (*a != '\0' && *a == *b)
    {
        a++;
        b++;
    }

    return (int)((unsigned char)*a) - (int)((unsigned char)*b);
}

/* NewList() lives in amiga.lib. A shared library open-codes it. */
void ug_newlist(struct MinList *list)
{
    list->mlh_Head     = (struct MinNode *)&list->mlh_Tail;
    list->mlh_Tail     = NULL;
    list->mlh_TailPred = (struct MinNode *)&list->mlh_Head;
}

/* ------------------------------------------------------------- dos base, */


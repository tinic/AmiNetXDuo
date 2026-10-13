/*
 * The C library pieces codesets.library takes from here instead of newlib.
 *
 * A shared library has no crt0, and newlib's strcasecmp(), atoi(), strtol()
 * and the ctype macros all reach _impure_ptr through the locale, which drags
 * in stdio, malloc and exit().  These stay in the C locale, which is all the
 * library asks of them: codeset names and numbers in charset files.  No
 * errno and no overflow clamp; nothing upstream reads either.  NewList() is
 * here for the reason beside it.
 *
 * SPDX-License-Identifier: MIT
 */

#include <exec/lists.h>
#include <clib/alib_protos.h>
#include <ctype.h>
#include <stdlib.h>

#include "cs_libc.h"

static int cs_lower(int c)
{
    return (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c;
}

int strnicmp(const char *a, const char *b, size_t n)
{
    for (; n > 0; n--, a++, b++)
    {
        int d = cs_lower((unsigned char)*a) - cs_lower((unsigned char)*b);

        if (d != 0 || *a == '\0')
            return d;
    }
    return 0;
}

int stricmp(const char *a, const char *b)
{
    return strnicmp(a, b, (size_t)-1);
}

static int cs_digit(int c)
{
    c = cs_lower(c);
    return (c >= '0' && c <= '9') ? c - '0'
         : (c >= 'a' && c <= 'z') ? c - 'a' + 10 : 99;
}

long strtol(const char *s, char **end, int base)
{
    const char   *p = s, *first;
    unsigned long n = 0;
    int           neg = 0, d;

    while (*p == ' ' || (*p >= '\t' && *p <= '\r'))
        p++;
    if (*p == '-' || *p == '+')
        neg = (*p++ == '-');
    if ((base == 0 || base == 16) && p[0] == '0' && cs_lower(p[1]) == 'x'
        && cs_digit(p[2]) < 16)
        p += 2, base = 16;
    else if (base == 0)
        base = (p[0] == '0') ? 8 : 10;
    first = p;
    while ((d = cs_digit((unsigned char)*p)) < base)
        n = n * (unsigned long)base + (unsigned long)d, p++;
    if (end != NULL)
        *end = (char *)(p == first ? s : p);
    return neg ? -(long)n : (long)n;
}

int atoi(const char *s)
{
    return (int)strtol(s, NULL, 10);
}

/* The C locale's table, without the locale object around it. */
const char *__locale_ctype_ptr(void)
{
    return _ctype_;
}

/* amiga.lib's, whose objects tools/check-pcrel-exact.py cannot read. */
VOID NewList(struct List *list)
{
    list->lh_Head     = (struct Node *)&list->lh_Tail;
    list->lh_Tail     = NULL;
    list->lh_TailPred = (struct Node *)&list->lh_Head;
}

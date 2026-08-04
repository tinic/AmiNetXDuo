/*
 * httplock, the WebDAV lock table.  See httplock.h for the rules it holds.
 *
 * Includes nothing but httplock.h, deliberately, for the reason httppath.c
 * gives: this translation unit is compiled twice, once for m68k as part of the
 * server and once natively by src/tools/test/test_httplock.c.
 *
 * SPDX-License-Identifier: MIT
 */

#include "httplock.h"

static HttpLock httplock_table[HTTPD_LOCK_MAX];

/* ------------------------------------------------------------- the small --- */

static int hl_equal(const char *a, const char *b)
{
    unsigned long i = 0;

    while (a[i] != '\0' && a[i] == b[i])
        i++;

    return (a[i] == b[i]) ? 1 : 0;
}

/* ------------------------------------------------------------------ table --- */

void httplock_reset(void)
{
    unsigned long i;

    for (i = 0; i < (unsigned long)HTTPD_LOCK_MAX; i++)
        httplock_table[i].used = 0;
}

void httplock_expire(unsigned long now)
{
    unsigned long i;

    for (i = 0; i < (unsigned long)HTTPD_LOCK_MAX; i++)
    {
        if (httplock_table[i].used && now >= httplock_table[i].expires)
            httplock_table[i].used = 0;
    }
}

HttpLock *httplock_on(const char *path, unsigned long now)
{
    unsigned long i;

    httplock_expire(now);

    for (i = 0; i < (unsigned long)HTTPD_LOCK_MAX; i++)
    {
        HttpLock *l = &httplock_table[i];

        if (!l->used)
            continue;

        if (hl_equal(l->path, path))
            return l;

        if (l->depth != 0 && http_path_within(l->path, path))
            return l;
    }

    return 0;
}

/*
 * Only a lock on this exact path is refreshed in place.  One inherited from a
 * drawer above belongs to that drawer, and writing this path into it would
 * move it.
 */
HttpLock *httplock_exact(const char *path)
{
    unsigned long i;

    for (i = 0; i < (unsigned long)HTTPD_LOCK_MAX; i++)
    {
        if (httplock_table[i].used && hl_equal(httplock_table[i].path, path))
            return &httplock_table[i];
    }

    return 0;
}

HttpLock *httplock_by_token(const char *token, unsigned long now)
{
    unsigned long i;

    if (token == 0 || token[0] == '\0')
        return 0;

    httplock_expire(now);

    for (i = 0; i < (unsigned long)HTTPD_LOCK_MAX; i++)
    {
        if (httplock_table[i].used && hl_equal(httplock_table[i].token, token))
            return &httplock_table[i];
    }

    return 0;
}

HttpLock *httplock_free_slot(void)
{
    unsigned long i;

    for (i = 0; i < (unsigned long)HTTPD_LOCK_MAX; i++)
    {
        if (!httplock_table[i].used)
            return &httplock_table[i];
    }

    return 0;
}

void httplock_drop(const char *path)
{
    unsigned long i;

    for (i = 0; i < (unsigned long)HTTPD_LOCK_MAX; i++)
    {
        HttpLock *l = &httplock_table[i];

        if (l->used && http_path_within(path, l->path))
            l->used = 0;
    }
}

/* ------------------------------------------------------------- the rules --- */

int httplock_covers(const HttpLock *l, const char *path)
{
    if (l == 0)
        return 0;

    if (hl_equal(l->path, path))
        return 1;

    return (l->depth != 0 && http_path_within(l->path, path)) ? 1 : 0;
}

int httplock_held(const char *tok0, const char *tok1, const HttpLock *l)
{
    if (l == 0)
        return 1;

    if (tok0 != 0 && hl_equal(tok0, l->token))
        return 1;

    if (tok1 != 0 && hl_equal(tok1, l->token))
        return 1;

    return 0;
}

int httplock_allows(const char *tok0, const char *tok1,
                    const char *path, unsigned long now)
{
    return httplock_held(tok0, tok1, httplock_on(path, now));
}

int httplock_allows_tree(const char *tok0, const char *tok1,
                         const char *path, unsigned long now)
{
    unsigned long i;

    if (!httplock_allows(tok0, tok1, path, now))
        return 0;

    httplock_expire(now);

    for (i = 0; i < (unsigned long)HTTPD_LOCK_MAX; i++)
    {
        HttpLock *l = &httplock_table[i];

        if (!l->used || !http_path_within(path, l->path))
            continue;

        if (httplock_held(tok0, tok1, l))
            continue;

        return 0;
    }

    return 1;
}

int httplock_allows_parent(const char *tok0, const char *tok1,
                           const char *path, unsigned long now)
{
    char          parent[HTTP_PATH_MAX];
    unsigned long i;

    for (i = 0; i + 1 < (unsigned long)HTTP_PATH_MAX && path[i] != '\0'; i++)
        parent[i] = path[i];

    parent[i] = '\0';

    http_path_up(parent);

    /* Unchanged means there is nothing above it: a device reference is the
       top and has no drawer to be a member of. */
    if (hl_equal(parent, path) || parent[0] == '\0')
        return 1;

    return httplock_held(tok0, tok1, httplock_on(parent, now));
}

/*
 * The tests for src/tools/httplock.c, the WebDAV lock table.
 *
 * SPDX-License-Identifier: MIT
 */

#include "httplock.h"

#include <stdio.h>

static int failures;
static int checks;

#define CHECK(cond)                                                          \
    do {                                                                     \
        checks++;                                                            \
        if (!(cond)) {                                                       \
            failures++;                                                      \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
        }                                                                    \
    } while (0)

#define TOKEN  "opaquelocktoken:aabbccdd"
#define OTHER  "opaquelocktoken:11223344"
#define NONE   ""

static void copy(char *dst, unsigned long len, const char *src)
{
    unsigned long i;

    for (i = 0; i + 1UL < len && src[i] != '\0'; i++)
        dst[i] = src[i];

    dst[i] = '\0';
}

/* Puts a lock in the table directly, which is what a granted LOCK does. */
static HttpLock *place(const char *path, const char *token,
                       int deep, unsigned long expires)
{
    HttpLock *l = httplock_free_slot();

    if (l == 0)
        return 0;

    copy(l->path,  (unsigned long)sizeof(l->path),  path);
    copy(l->token, (unsigned long)sizeof(l->token), token);
    copy(l->owner, (unsigned long)sizeof(l->owner), "somebody");

    l->expires = expires;
    l->timeout = expires;
    l->depth   = (unsigned char)(deep ? 1 : 0);
    l->used    = 1;

    return l;
}

/*
 * RFC 4918 §9.10.1.  A Depth: 0 lock is that resource and nothing else.  A
 * Depth: infinity lock is that resource and everything under it.
 */
static void test_depth(void)
{
    httplock_reset();

    CHECK(place("Work:Public/notes.txt", TOKEN, 0, 100) != 0);

    CHECK(httplock_on("Work:Public/notes.txt", 50) != 0);
    CHECK(httplock_on("Work:Public", 50) == 0);
    CHECK(httplock_on("Work:Public/other.txt", 50) == 0);

    httplock_reset();

    CHECK(place("Work:Public", TOKEN, 1, 100) != 0);

    CHECK(httplock_on("Work:Public", 50) != 0);
    CHECK(httplock_on("Work:Public/notes.txt", 50) != 0);
    CHECK(httplock_on("Work:Public/deep/down/here.txt", 50) != 0);

    /* And a Depth: 0 lock on the same collection reaches only itself. */
    httplock_reset();

    CHECK(place("Work:Public", TOKEN, 0, 100) != 0);

    CHECK(httplock_on("Work:Public", 50) != 0);
    CHECK(httplock_on("Work:Public/notes.txt", 50) == 0);
}

/*
 * The prefix that is not a parent.  "Work:Public" must not reach
 * "Work:PublicSecrets", which is a different drawer whose name starts the
 * same way.  This is http_path_within()'s rule and the reason the lock table
 * asks it rather than comparing strings.
 */
static void test_prefix_is_not_a_parent(void)
{
    httplock_reset();

    CHECK(place("Work:Public", TOKEN, 1, 100) != 0);

    CHECK(httplock_on("Work:PublicSecrets", 50) == 0);
    CHECK(httplock_on("Work:PublicSecrets/x.txt", 50) == 0);
    CHECK(httplock_on("Work:Public/x.txt", 50) != 0);
}

/* Paths on this machine are case-insensitive, so a lock has to be too. */
static void test_case_insensitive(void)
{
    httplock_reset();

    CHECK(place("Work:Public", TOKEN, 1, 100) != 0);

    CHECK(httplock_on("work:public/NOTES.TXT", 50) != 0);
    CHECK(httplock_covers(httplock_on("WORK:PUBLIC", 50), "WORK:PUBLIC") == 1);
}

/*
 * RFC 4918 §6.6.  Expiry is lazy and the boundary is `now >= expires`, so a
 * lock whose expiry is this second is already gone.
 */
static void test_expiry(void)
{
    httplock_reset();

    CHECK(place("Work:a.txt", TOKEN, 0, 100) != 0);

    CHECK(httplock_on("Work:a.txt", 99) != 0);   /* one second left */
    CHECK(httplock_on("Work:a.txt", 100) == 0);  /* the boundary is gone */

    /* And the slot is free again, not merely unreported. */
    httplock_reset();

    CHECK(place("Work:a.txt", TOKEN, 0, 100) != 0);

    httplock_expire(100);

    CHECK(httplock_exact("Work:a.txt") == 0);
    CHECK(httplock_free_slot() != 0);

    /* An expired lock does not answer by token either, which is what stops
       an UNLOCK from resurrecting one. */
    httplock_reset();

    CHECK(place("Work:a.txt", TOKEN, 0, 100) != 0);
    CHECK(httplock_by_token(TOKEN, 99) != 0);
    CHECK(httplock_by_token(TOKEN, 100) == 0);
}

/*
 * RFC 4918 §9.10.  A token is what makes the holder the holder.  The table is
 * global, so holding one lock's token must not open another's resource.
 */
static void test_tokens(void)
{
    httplock_reset();

    CHECK(place("Work:mine.txt",   TOKEN, 0, 100) != 0);
    CHECK(place("Work:theirs.txt", OTHER, 0, 100) != 0);

    CHECK(httplock_allows(TOKEN, NONE, "Work:mine.txt",   50) == 1);
    CHECK(httplock_allows(TOKEN, NONE, "Work:theirs.txt", 50) == 0);
    CHECK(httplock_allows(OTHER, NONE, "Work:theirs.txt", 50) == 1);
    CHECK(httplock_allows(NONE,  NONE, "Work:mine.txt",   50) == 0);

    /* An If: header carries up to two, and either can be the one. */
    CHECK(httplock_allows(OTHER, TOKEN, "Work:mine.txt", 50) == 1);
    CHECK(httplock_allows(TOKEN, OTHER, "Work:mine.txt", 50) == 1);

    /* A name nobody locked is everybody's. */
    CHECK(httplock_allows(NONE, NONE, "Work:free.txt", 50) == 1);
    CHECK(httplock_held(NONE, NONE, 0) == 1);
}

/*
 * httplock_covers() is asked by UNLOCK and by a LOCK refresh, and it is a
 * different question from httplock_on().  The token names the lock, and this
 * asks whether that lock reaches the address it was named through.
 */
static void test_covers(void)
{
    HttpLock *l;

    httplock_reset();

    l = place("Work:Public", TOKEN, 1, 100);

    CHECK(l != 0);
    CHECK(httplock_covers(l, "Work:Public") == 1);
    CHECK(httplock_covers(l, "Work:Public/notes.txt") == 1);
    CHECK(httplock_covers(l, "Work:Private/notes.txt") == 0);
    CHECK(httplock_covers(0, "Work:Public") == 0);

    /* A Depth: 0 lock covers its own name and no other. */
    httplock_reset();

    l = place("Work:Public", TOKEN, 0, 100);

    CHECK(l != 0);
    CHECK(httplock_covers(l, "Work:Public") == 1);
    CHECK(httplock_covers(l, "Work:Public/notes.txt") == 0);
}

/*
 * RFC 4918 §9.6.1.  DELETE and MOVE take everything below the address, so a
 * lock on something inside stops them even when the address itself is free.
 */
static void test_tree(void)
{
    httplock_reset();

    CHECK(place("Work:Public/sub/held.txt", OTHER, 0, 100) != 0);

    /* The drawer itself is unlocked, so a plain write to it is fine... */
    CHECK(httplock_allows(NONE, NONE, "Work:Public", 50) == 1);

    /* ...but removing it would take a locked name with it. */
    CHECK(httplock_allows_tree(NONE,  NONE, "Work:Public", 50) == 0);
    CHECK(httplock_allows_tree(OTHER, NONE, "Work:Public", 50) == 1);

    /* A lock outside the tree does not block it. */
    httplock_reset();

    CHECK(place("Work:Elsewhere/held.txt", OTHER, 0, 100) != 0);
    CHECK(httplock_allows_tree(NONE, NONE, "Work:Public", 50) == 1);

    /* An expired lock underneath does not block it either. */
    httplock_reset();

    CHECK(place("Work:Public/sub/held.txt", OTHER, 0, 100) != 0);
    CHECK(httplock_allows_tree(NONE, NONE, "Work:Public", 100) == 1);
}

/*
 * RFC 4918 §9.6.  A DELETE that succeeded destroys the locks it removed,
 * including the ones below it.  Without this the name is unusable by anybody
 * until it expires, including the client that just deleted it.
 */
static void test_drop(void)
{
    httplock_reset();

    CHECK(place("Work:Public",           TOKEN, 1, 100) != 0);
    CHECK(place("Work:Public/sub/a.txt", OTHER, 0, 100) != 0);
    CHECK(place("Work:Keep/b.txt",       OTHER, 0, 100) != 0);

    httplock_drop("Work:Public");

    CHECK(httplock_exact("Work:Public") == 0);
    CHECK(httplock_exact("Work:Public/sub/a.txt") == 0);
    CHECK(httplock_exact("Work:Keep/b.txt") != 0);
}

/*
 * RFC 4918 §7.1.  A lock on a collection, at any depth including 0, locks that
 * collection's internal member namespace.  Adding a name or taking one away
 * needs the token even though the name itself is not locked.
 */
static void test_parent(void)
{
    httplock_reset();

    CHECK(place("Work:Shared", TOKEN, 0, 100) != 0);

    /* The member is not locked, so writing its content is allowed... */
    CHECK(httplock_allows(NONE, NONE, "Work:Shared/new.txt", 50) == 1);

    /* ...but creating or removing it is a change to the drawer. */
    CHECK(httplock_allows_parent(NONE,  NONE, "Work:Shared/new.txt", 50) == 0);
    CHECK(httplock_allows_parent(TOKEN, NONE, "Work:Shared/new.txt", 50) == 1);

    /* Only the immediate drawer, not one further up that is unlocked. */
    CHECK(httplock_allows_parent(NONE, NONE, "Work:Other/new.txt", 50) == 1);

    /* A device is the top and has no drawer to be a member of. */
    CHECK(httplock_allows_parent(NONE, NONE, "Work:", 50) == 1);

    /* A name directly on a locked device is a member of it. */
    httplock_reset();

    CHECK(place("Work:", TOKEN, 0, 100) != 0);
    CHECK(httplock_allows_parent(NONE,  NONE, "Work:new.txt", 50) == 0);
    CHECK(httplock_allows_parent(TOKEN, NONE, "Work:new.txt", 50) == 1);
}

/*
 * The table is a fixed eight.  A ninth asker is told the server is busy, and
 * the eight already there keep working.
 */
static void test_table_full(void)
{
    int i;

    httplock_reset();

    for (i = 0; i < HTTPD_LOCK_MAX; i++)
    {
        char path[HTTP_PATH_MAX];

        copy(path, (unsigned long)sizeof(path), "Work:file0.txt");
        path[9] = (char)('0' + i);

        CHECK(place(path, TOKEN, 0, 100) != 0);
    }

    CHECK(httplock_free_slot() == 0);

    /* One expiring frees exactly one. */
    httplock_reset();

    for (i = 0; i < HTTPD_LOCK_MAX; i++)
    {
        char path[HTTP_PATH_MAX];

        copy(path, (unsigned long)sizeof(path), "Work:file0.txt");
        path[9] = (char)('0' + i);

        CHECK(place(path, TOKEN, 0, (i == 3) ? 100 : 900) != 0);
    }

    httplock_expire(100);

    CHECK(httplock_free_slot() != 0);
    CHECK(httplock_exact("Work:file3.txt") == 0);
    CHECK(httplock_exact("Work:file4.txt") != 0);
}

int main(void)
{
    test_depth();
    test_prefix_is_not_a_parent();
    test_case_insensitive();
    test_expiry();
    test_tokens();
    test_covers();
    test_tree();
    test_drop();
    test_parent();
    test_table_full();

    printf("\n%d checks, %d failure(s)\n", checks, failures);

    return failures == 0 ? 0 : 1;
}

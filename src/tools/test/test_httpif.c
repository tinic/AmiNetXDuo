/*
 * The tests for src/tools/httpif.c, the If: header, which decides whether a
 * write goes ahead against somebody else's lock.
 *
 * The failure this file exists to catch is silent in both directions.  An
 * evaluator that always says yes answers 201 to a request that named a lock
 * token nobody is holding, which is what the server did before this file
 * existed and which no client reports.  One that says no too often refuses a
 * client that was entitled to write, and that shows up as a mount that cannot
 * save.  Neither is visible in a transcript without knowing the correct
 * answer.
 *
 * So the shapes are written down here, one case each: lists are OR'd, the
 * conditions inside one are AND'd, Not inverts a condition and not a list, and
 * a Resource-Tag redirects the ones after it at another resource.
 *
 *   cc -std=c11 -Wall -Wextra -Isrc/tools \
 *      src/tools/test/test_httpif.c src/tools/httpif.c -o test_httpif
 *
 * SPDX-License-Identifier: MIT
 */

#include "httpif.h"

#include <stdio.h>
#include <string.h>

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

/* ------------------------------------------------------------- the world --- */

/*
 * Two resources.  The untagged one is the request target, and it is locked and
 * has a tag.  "/other" is locked with a different token.  Anything else is a
 * resource this server knows nothing about.
 */
#define TOKEN  "opaquelocktoken:aabbccdd"
#define OTHER  "opaquelocktoken:11223344"
#define ETAG   "\"1234-9000-61-25\""

static int lookups;

static void world(void *ctx, const char *tag, HttpIfState *out)
{
    (void)ctx;
    lookups++;

    if (tag[0] == '\0')
    {
        strcpy(out->token, TOKEN);
        strcpy(out->etag, ETAG);
        return;
    }

    if (strcmp(tag, "http://amiga:8080/other") == 0 ||
        strcmp(tag, "/other") == 0)
    {
        strcpy(out->token, OTHER);
        return;
    }

    /* Not ours.  Both fields stay empty, which is a list that cannot hold. */
}

/* A resource with no lock and no tag, which is the null resource a LOCK
   creates. */
static void nothing(void *ctx, const char *tag, HttpIfState *out)
{
    (void)ctx;
    (void)tag;
    (void)out;
    lookups++;
}

static int eval(const char *header)
{
    return http_if_eval(header, world, NULL);
}

/* ---------------------------------------------------------------- tokens --- */

static void test_state_tokens(void)
{
    printf("state tokens\n");

    /* The one the client is holding. */
    CHECK(eval("(<" TOKEN ">)") == 1);

    /*
     * The measured failure this file exists for.  A token nobody is holding
     * used to be answered as though the condition held, so a PUT behind it was
     * a 201 where it must be a 412.
     */
    CHECK(eval("(<opaquelocktoken:deadbeef>)") == 0);

    /* An empty header has no list, so it cannot hold.  The caller checks for
       an absent header rather than passing one through. */
    CHECK(eval("") == 0);
    CHECK(http_if_eval(NULL, world, NULL) == 0);

    /* A resource with nothing on it matches no token at all. */
    CHECK(http_if_eval("(<" TOKEN ">)", nothing, NULL) == 0);
}

static void test_not(void)
{
    printf("Not\n");

    /* The other measured failure.  Not against a token that is held has to be
       false, and used to be true along with everything else. */
    CHECK(eval("(Not <" TOKEN ">)") == 0);

    /* And true against one that is not. */
    CHECK(eval("(Not <opaquelocktoken:deadbeef>)") == 1);

    /* Not applies to the condition after it and not to the rest of the list,
       so the second condition here still has to hold on its own. */
    CHECK(eval("(Not <opaquelocktoken:deadbeef> <" TOKEN ">)") == 1);
    CHECK(eval("(Not <opaquelocktoken:deadbeef> <opaquelocktoken:00>)") == 0);

    /* "Notorious" is not "Not". */
    CHECK(eval("(Notatoken <" TOKEN ">)") == 0);
}

static void test_entity_tags(void)
{
    printf("entity tags\n");

    CHECK(eval("([" ETAG "])") == 1);
    CHECK(eval("([\"9999-1-1-1\"])") == 0);
    CHECK(eval("(Not [" ETAG "])") == 0);
    CHECK(eval("(Not [\"9999-1-1-1\"])") == 1);

    /* A weak tag is compared as the strong one it wraps.  This server emits
       none, so a client asking about one means the resource. */
    CHECK(eval("([W/" ETAG "])") == 1);

    /* A resource with no tag matches nothing, including an empty one. */
    CHECK(http_if_eval("([" ETAG "])", nothing, NULL) == 0);
    CHECK(eval("([])") == 0);

    /* Both kinds of condition in one list, AND'd. */
    CHECK(eval("(<" TOKEN "> [" ETAG "])") == 1);
    CHECK(eval("(<" TOKEN "> [\"9999-1-1-1\"])") == 0);
    CHECK(eval("(<opaquelocktoken:deadbeef> [" ETAG "])") == 0);
}

static void test_lists_are_or(void)
{
    printf("lists are OR'd, conditions are AND'd\n");

    /* One list holding is the whole header, whichever it is. */
    CHECK(eval("(<opaquelocktoken:deadbeef>) (<" TOKEN ">)") == 1);
    CHECK(eval("(<" TOKEN ">) (<opaquelocktoken:deadbeef>)") == 1);
    CHECK(eval("(<opaquelocktoken:00>) (<opaquelocktoken:01>)") == 0);

    /* And a list is only as good as its weakest condition. */
    CHECK(eval("(<" TOKEN "> <opaquelocktoken:deadbeef>)") == 0);
    CHECK(eval("(<" TOKEN "> <" TOKEN ">)") == 1);
}

static void test_tagged_lists(void)
{
    printf("tagged lists\n");

    /* The tag redirects the lists after it at another resource. */
    CHECK(eval("<http://amiga:8080/other> (<" OTHER ">)") == 1);
    CHECK(eval("<http://amiga:8080/other> (<" TOKEN ">)") == 0);

    /* Two tags, each with its own list, both of which have to be about the
       right resource, and either one holding is enough. */
    CHECK(eval("</other> (<" OTHER ">) </elsewhere> (<" TOKEN ">)") == 1);
    CHECK(eval("</elsewhere> (<" TOKEN ">) </nowhere> (<" OTHER ">)") == 0);

    /* A resource this server cannot speak for has no state, so a list about
       it cannot hold. */
    CHECK(eval("<http://elsewhere/thing> (<" TOKEN ">)") == 0);
    CHECK(eval("<http://elsewhere/thing> (Not <" TOKEN ">)") == 1);

    /* The state of one resource is looked up once however many lists name
       it, because a lookup is a Lock() and an Examine(). */
    lookups = 0;
    CHECK(eval("</other> (<" TOKEN ">) (<" OTHER ">)") == 1);
    CHECK(lookups == 1);

    /* A new tag is a new resource and a new lookup, even for the same name.
       The first list here fails, so the second is reached. */
    lookups = 0;
    CHECK(eval("</other> (<" TOKEN ">) </other> (<" OTHER ">)") == 1);
    CHECK(lookups == 2);
}

static void test_malformed(void)
{
    printf("what a client should not send\n");

    /* Unterminated, at every point one can be. */
    CHECK(eval("(<" TOKEN) == 0);
    CHECK(eval("(") == 0);
    CHECK(eval("(Not") == 0);
    CHECK(eval("<") == 0);
    CHECK(eval("<tag") == 0);
    CHECK(eval("([\"a") == 0);

    /* An empty list has no condition to hold. */
    CHECK(eval("()") == 0);
    CHECK(eval("() (<" TOKEN ">)") == 1);

    /* Junk inside a list is a list nobody can read, so it does not hold,
       even when a condition beside it does. */
    CHECK(eval("(rubbish <" TOKEN ">)") == 0);

    /* Junk between lists is skipped, and the lists still stand on their own. */
    CHECK(eval("rubbish (<" TOKEN ">)") == 1);

    /* Whitespace anywhere it is allowed. */
    CHECK(eval("  (  <" TOKEN ">  )  ") == 1);
    CHECK(eval("(\tNot\t<opaquelocktoken:deadbeef>\t)") == 1);

    /* A token longer than one this server ever issues cannot match one it
       did, and must not run off the end of the buffer it is copied into. */
    {
        char big[600];
        unsigned long i;

        big[0] = '(';
        big[1] = '<';
        for (i = 2; i < sizeof(big) - 3; i++)
            big[i] = 'a';
        big[sizeof(big) - 3] = '>';
        big[sizeof(big) - 2] = ')';
        big[sizeof(big) - 1] = '\0';

        CHECK(eval(big) == 0);
    }
}

int main(void)
{
    test_state_tokens();
    test_not();
    test_entity_tags();
    test_lists_are_or();
    test_tagged_lists();
    test_malformed();

    printf("\n%d checks, %d failure(s)\n", checks, failures);

    return failures == 0 ? 0 : 1;
}

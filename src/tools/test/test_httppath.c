/*
 * The tests for src/tools/httppath.c -- the one part of the WebDAV server
 * that decides whether a request can reach a file outside the document root.
 *
 * WHY THIS IS A TEST AND NOT A REVIEW
 *
 *   The server writes.  A path check that is merely "looked right" leaked a
 *   file when it only read and destroys one now that DELETE, MOVE and PUT are
 *   here, and the mistakes are all the same shape: a thing that does not look
 *   like an escape until something else has decoded it.  So the escapes are
 *   written down here, one case each, and the file is built for the host so
 *   they run in `tools/ci.sh host` on every change rather than only when
 *   somebody boots an Amiga.
 *
 *   Three of the destructive primitives live in that file for the same
 *   reason -- joining a child onto a walked path, backing up one level, and
 *   asking whether one path is inside another.  A walk that appends the
 *   wrong separator deletes the parent, and the containment test is what
 *   both a lock and a COPY-into-itself check are made of.
 *
 *   The AmigaOS case is the one no ported Unix server has: a colon makes
 *   everything before it a device or an assign, so "/RAM:foo" is the RAM disk
 *   and not a file called "RAM:foo", and every ../ check ever written is blind
 *   to it.  It is tested here in every position a colon can appear in.
 *
 *   cc -std=c11 -Wall -Wextra -Isrc/tools \
 *      src/tools/test/test_httppath.c src/tools/httppath.c -o test_httppath
 *
 * SPDX-License-Identifier: MIT
 */

#include "httppath.h"

#include <stdio.h>
#include <string.h>

/* --------------------------------------------------------------- harness --- */

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

/* Deliberately not the CHECK_STR in src/config/test/test_config.c: its NULL
   guard is dead for an array argument and costs a -Waddress exemption in
   cmake/ci-warnings.cmake. */
#define CHECK_STR(got, want)                                                 \
    do {                                                                     \
        const char *got_ = (got);                                            \
        checks++;                                                            \
        if (strcmp(got_, (want)) != 0) {                                     \
            failures++;                                                      \
            printf("  FAIL %s:%d: expected \"%s\", got \"%s\"\n",            \
                   __FILE__, __LINE__, (want), got_);                        \
        }                                                                    \
    } while (0)

/* The path a target resolves to under `root`, or NULL when it is refused. */
static const char *resolved(const char *root, const char *target)
{
    static HttpPath out;

    if (http_path_resolve(root, target, &out) != HTTP_PATH_OK)
        return NULL;

    return out.path;
}

static HttpPathResult refused(const char *target)
{
    HttpPath out;

    return http_path_resolve("Work:Public", target, &out);
}

/* -------------------------------------------------------- what should work */

static void test_ordinary(void)
{
    printf("ordinary paths\n");

    CHECK_STR(resolved("Work:Public", "/"), "Work:Public");
    CHECK_STR(resolved("Work:Public", "/readme.txt"), "Work:Public/readme.txt");
    CHECK_STR(resolved("Work:Public", "/Docs/readme.txt"),
              "Work:Public/Docs/readme.txt");
    CHECK_STR(resolved("Work:Public", "/Docs/"), "Work:Public/Docs");

    /* A root that is a whole volume already carries its separator; adding one
       would name the volume's root directory instead of the drawer, which is
       a different place on AmigaOS. */
    CHECK_STR(resolved("RAM:", "/foo"), "RAM:foo");
    CHECK_STR(resolved("RAM:", "/"), "RAM:");
    CHECK_STR(resolved("DH0:", "/Docs/x"), "DH0:Docs/x");

    /* A root written with a trailing slash must not produce a doubled one:
       "a//b" is a's PARENT's b here, not "a/b". */
    CHECK_STR(resolved("Work:Public/", "/x"), "Work:Public/x");

    /* Query strings and fragments are not part of the path. */
    CHECK_STR(resolved("RAM:", "/foo?bar=1"), "RAM:foo");
    CHECK_STR(resolved("RAM:", "/foo#frag"), "RAM:foo");

    /* The absolute form, which a proxy-aware client may send. */
    CHECK_STR(resolved("RAM:", "http://amiga.local/foo"), "RAM:foo");
    CHECK_STR(resolved("RAM:", "http://amiga.local:8080/foo"), "RAM:foo");
    CHECK_STR(resolved("RAM:", "http://amiga.local"), "RAM:");

    /* Percent-decoding of the things AmigaOS file names are actually full of:
       spaces, brackets and exclamation marks. */
    CHECK_STR(resolved("RAM:", "/My%20File.txt"), "RAM:My File.txt");
    CHECK_STR(resolved("RAM:", "/Kickstart%20%5B!%5D.rom"),
              "RAM:Kickstart [!].rom");

    /* Single dots and empty segments are noise, not an escape. */
    CHECK_STR(resolved("RAM:", "/./foo"), "RAM:foo");
    CHECK_STR(resolved("RAM:", "//foo"), "RAM:foo");
    CHECK_STR(resolved("RAM:", "/a//b"), "RAM:a/b");
    CHECK_STR(resolved("RAM:", "/a/./b"), "RAM:a/b");
}

/* ------------------------------------------------------ the AmigaOS escape */

static void test_device_escape(void)
{
    printf("the colon -- an AmigaOS device reference\n");

    /*
     * Every one of these resolves to a real place on a real Amiga, and not
     * one of them contains a "..".  This is the case the brief was written
     * around: a Unix server ported to this machine refuses none of them.
     */
    CHECK(refused("/RAM:foo") == HTTP_PATH_DEVICE);
    CHECK(refused("/DH0:") == HTTP_PATH_DEVICE);
    CHECK(refused("/S:startup-sequence") == HTTP_PATH_DEVICE);
    CHECK(refused("/SYS:Prefs") == HTTP_PATH_DEVICE);
    CHECK(refused("/Docs/RAM:secret") == HTTP_PATH_DEVICE);
    CHECK(refused("/Docs/DH0:/x") == HTTP_PATH_DEVICE);

    /* A colon anywhere in a segment, not only at its end -- "a:b" is still a
       device reference to AmigaDOS. */
    CHECK(refused("/a:b") == HTTP_PATH_DEVICE);
    CHECK(refused("/x/a:b/y") == HTTP_PATH_DEVICE);

    /* And the same thing hidden behind an escape.  %3A is a colon by the time
       anything looks at it, which is the whole reason decoding comes first. */
    CHECK(refused("/RAM%3Afoo") == HTTP_PATH_DEVICE);
    CHECK(refused("/RAM%3afoo") == HTTP_PATH_DEVICE);
    CHECK(refused("/%53%59%53%3A") == HTTP_PATH_DEVICE);
}

static void test_parent_escape(void)
{
    printf("the .. escape, decoded and not\n");

    CHECK(refused("/..") == HTTP_PATH_PARENT);
    CHECK(refused("/../") == HTTP_PATH_PARENT);
    CHECK(refused("/../secret") == HTTP_PATH_PARENT);
    CHECK(refused("/a/../../secret") == HTTP_PATH_PARENT);
    CHECK(refused("/a/b/..") == HTTP_PATH_PARENT);

    /* Percent-encoded, in every mixture of case a client can write it. */
    CHECK(refused("/%2E%2E/secret") == HTTP_PATH_PARENT);
    CHECK(refused("/%2e%2e/secret") == HTTP_PATH_PARENT);
    CHECK(refused("/%2e./secret") == HTTP_PATH_PARENT);
    CHECK(refused("/.%2e/secret") == HTTP_PATH_PARENT);

    /*
     * The doubly-awkward one: an encoded SEPARATOR next to an encoded parent.
     * Decoding first turns %2F into a real separator, which can only ever
     * create more segments -- and every segment is checked, so the ".." it was
     * hiding is found rather than smuggled through as one long name.
     */
    CHECK(refused("/a%2F..%2Fsecret") == HTTP_PATH_PARENT);
    CHECK(refused("/..%2Fsecret") == HTTP_PATH_PARENT);
    CHECK(refused("/%2e%2e%2f%2e%2e%2fsecret") == HTTP_PATH_PARENT);

    /* Double encoding is NOT decoded twice: "%252E" is the four characters
       %2E, which is a legal file name and not a parent reference. */
    CHECK_STR(resolved("RAM:", "/%252E%252E"), "RAM:%2E%2E");

    /* Things that merely look like it and are legal AmigaOS names. */
    CHECK_STR(resolved("RAM:", "/..."), "RAM:...");
    CHECK_STR(resolved("RAM:", "/..foo"), "RAM:..foo");
    CHECK_STR(resolved("RAM:", "/foo.."), "RAM:foo..");
}

static void test_malformed(void)
{
    printf("malformed targets\n");

    CHECK(refused("foo") == HTTP_PATH_NOT_ABSOLUTE);
    CHECK(refused("") == HTTP_PATH_NOT_ABSOLUTE);
    CHECK(refused("*") == HTTP_PATH_NOT_ABSOLUTE);

    /* A % that is not an escape.  Refused rather than passed through, because
       "%2" at the end of a buffer is how a decoder reads past it. */
    CHECK(refused("/%") == HTTP_PATH_BAD_ESCAPE);
    CHECK(refused("/%2") == HTTP_PATH_BAD_ESCAPE);
    CHECK(refused("/%zz") == HTTP_PATH_BAD_ESCAPE);
    CHECK(refused("/a%2Gb") == HTTP_PATH_BAD_ESCAPE);

    /* A NUL truncates every AmigaDOS call downstream, so an encoded one is
       refused rather than becoming the end of a shorter path. */
    CHECK(refused("/foo%00.txt") == HTTP_PATH_CONTROL);
    CHECK(refused("/foo%0A.txt") == HTTP_PATH_CONTROL);
    CHECK(refused("/foo%0D%0AX-Header:%20y") == HTTP_PATH_CONTROL);
    CHECK(refused("/foo%7F") == HTTP_PATH_CONTROL);

    /* A backslash means nothing to AmigaDOS but is a separator to whoever
       sent it, so a request containing one does not mean what it says. */
    CHECK(refused("/a\\b") == HTTP_PATH_BACKSLASH);
    CHECK(refused("/a%5Cb") == HTTP_PATH_BACKSLASH);

    /* Bounded: a client cannot make the server walk, and cannot make it build
       a path longer than AmigaDOS carries. */
    {
        char deep[HTTP_URL_MAX];
        size_t n = 0;
        int i;

        for (i = 0; i < 40 && n + 3 < sizeof(deep); i++)
        {
            deep[n++] = '/';
            deep[n++] = 'a';
        }
        deep[n] = '\0';

        CHECK(refused(deep) == HTTP_PATH_TOO_DEEP);
    }

    {
        char long_one[HTTP_URL_MAX];
        size_t n = 0;

        long_one[n++] = '/';
        while (n + 1 < sizeof(long_one))
            long_one[n++] = 'x';
        long_one[n] = '\0';

        CHECK(refused(long_one) == HTTP_PATH_TOO_LONG);
    }

    /* And every refusal has something to say in a log. */
    CHECK(strlen(http_path_error(HTTP_PATH_DEVICE)) > 0);
    CHECK(strlen(http_path_error(HTTP_PATH_PARENT)) > 0);
    CHECK(strlen(http_path_error((HttpPathResult)999)) > 0);
}

/* ------------------------------------------------------------- the fields */

static void test_fields(void)
{
    HttpPath p;

    printf("what the caller is told about the path\n");

    CHECK(http_path_resolve("Work:Public", "/", &p) == HTTP_PATH_OK);
    CHECK(p.segments == 0);
    CHECK_STR(p.url, "/");
    CHECK_STR(p.name, "");
    CHECK(p.trailing_slash == 1);

    CHECK(http_path_resolve("Work:Public", "/Docs/x.txt", &p) == HTTP_PATH_OK);
    CHECK(p.segments == 2);
    CHECK_STR(p.url, "/Docs/x.txt");
    CHECK_STR(p.name, "x.txt");
    CHECK(p.trailing_slash == 0);

    /* The URL is the normalised one, not the one that arrived: it is what the
       multistatus hrefs are built from, and a href that disagrees with the URL
       the client asked about is where a WebDAV client gives up. */
    CHECK(http_path_resolve("RAM:", "/a//b/./c", &p) == HTTP_PATH_OK);
    CHECK_STR(p.url, "/a/b/c");
    CHECK_STR(p.path, "RAM:a/b/c");

    CHECK(http_path_resolve("RAM:", "/My%20File.txt", &p) == HTTP_PATH_OK);
    CHECK_STR(p.name, "My File.txt");
}

/* ------------------------------------------------- what a write relies on */

/*
 * The document root itself is the one resource a client may not replace or
 * remove, and `segments == 0` is the ONLY thing that says a path is it.  Every
 * spelling of a root has to agree, because a root written with a trailing
 * slash that came back with one segment would make the drawer deletable.
 */
static void test_root_is_identifiable(void)
{
    HttpPath p;

    printf("the document root, under every spelling\n");

    CHECK(http_path_resolve("Work:Public", "/", &p) == HTTP_PATH_OK);
    CHECK(p.segments == 0);
    CHECK(http_path_resolve("Work:Public/", "/", &p) == HTTP_PATH_OK);
    CHECK(p.segments == 0);
    CHECK(http_path_resolve("RAM:", "/", &p) == HTTP_PATH_OK);
    CHECK(p.segments == 0);
    CHECK(http_path_resolve("RAM:", "", &p) != HTTP_PATH_OK);

    /* And the ways a client might try to say "the root" with something in it
       that collapses to nothing. */
    CHECK(http_path_resolve("Work:Public", "//", &p) == HTTP_PATH_OK);
    CHECK(p.segments == 0);
    CHECK(http_path_resolve("Work:Public", "/./", &p) == HTTP_PATH_OK);
    CHECK(p.segments == 0);
    CHECK(http_path_resolve("Work:Public", "/./././", &p) == HTTP_PATH_OK);
    CHECK(p.segments == 0);
    CHECK(http_path_resolve("Work:Public", "http://amiga.local/", &p) ==
          HTTP_PATH_OK);
    CHECK(p.segments == 0);

    /* One real segment is one segment, however much noise is around it. */
    CHECK(http_path_resolve("Work:Public", "/.//x//./", &p) == HTTP_PATH_OK);
    CHECK(p.segments == 1);
    CHECK_STR(p.path, "Work:Public/x");
}

/*
 * COPY and MOVE take the other end of the operation in a header rather than
 * on the request line, and it is an absolute URI.  It goes through this same
 * function, and these are the shapes clients send -- a Destination given its
 * own decoder is exactly the mistake this file exists to make impossible, so
 * every escape is asserted here too and not only on the request line.
 */
static void test_destination_forms(void)
{
    HttpPath p;

    printf("the Destination a COPY or a MOVE carries\n");

    CHECK_STR(resolved("Work:Public", "http://amiga.local/Docs/new.txt"),
              "Work:Public/Docs/new.txt");
    CHECK_STR(resolved("Work:Public", "http://amiga.local:8080/new.txt"),
              "Work:Public/new.txt");
    CHECK_STR(resolved("Work:Public", "http://192.168.0.9:8080/a/b"),
              "Work:Public/a/b");
    CHECK_STR(resolved("Work:Public", "/plain/relative"),
              "Work:Public/plain/relative");
    CHECK_STR(resolved("Work:Public", "http://amiga.local/My%20File.txt"),
              "Work:Public/My File.txt");

    /* A collection destination is written with a trailing slash and must
       resolve to the same place as one without: they are one drawer, and a
       MOVE that made two of them would lose the contents of one. */
    CHECK_STR(resolved("Work:Public", "http://amiga.local/Docs/"),
              "Work:Public/Docs");
    CHECK(http_path_resolve("Work:Public", "http://amiga.local/Docs/", &p) ==
          HTTP_PATH_OK);
    CHECK(p.trailing_slash == 1);
    CHECK_STR(p.name, "Docs");
    CHECK(http_path_resolve("Work:Public", "http://amiga.local/Docs", &p) ==
          HTTP_PATH_OK);
    CHECK(p.trailing_slash == 0);
    CHECK_STR(p.path, "Work:Public/Docs");

    /* Every escape, in the header rather than on the request line. */
    CHECK(refused("http://amiga.local/../secret") == HTTP_PATH_PARENT);
    CHECK(refused("http://amiga.local/%2e%2e/secret") == HTTP_PATH_PARENT);
    CHECK(refused("http://amiga.local/RAM:junk") == HTTP_PATH_DEVICE);
    CHECK(refused("http://amiga.local/RAM%3Ajunk") == HTTP_PATH_DEVICE);
    CHECK(refused("http://amiga.local/S:startup-sequence") ==
          HTTP_PATH_DEVICE);
    CHECK(refused("http://amiga.local/a%5Cb") == HTTP_PATH_BACKSLASH);
    CHECK(refused("http://amiga.local/x%00") == HTTP_PATH_CONTROL);
    CHECK(refused("http://amiga.local/%zz") == HTTP_PATH_BAD_ESCAPE);

    /* A Destination that is only an authority is the root, which the server
       refuses to overwrite -- but it must resolve rather than fall through as
       a relative path. */
    CHECK(http_path_resolve("Work:Public", "http://amiga.local", &p) ==
          HTTP_PATH_OK);
    CHECK(p.segments == 0);
}

/*
 * What every walk and every join downstream assumes about a resolved path.
 * These are cheap to assert and expensive to discover: a path that ended in a
 * separator would make the join produce "a//b", which is a's PARENT's b.
 */
static void test_resolved_shape(void)
{
    static const char *const targets[] =
    {
        "/", "/a", "/a/", "/a/b", "//a//b//", "/a/./b/",
        "/My%20File.txt", "/Docs/Kickstart%20%5B!%5D.rom", "/x/y/z",
        NULL
    };
    static const char *const roots[] =
    {
        "Work:Public", "Work:Public/", "RAM:", "DH0:", NULL
    };

    int r;
    int t;

    printf("the shape of every path a walk is handed\n");

    for (r = 0; roots[r] != NULL; r++)
    {
        for (t = 0; targets[t] != NULL; t++)
        {
            HttpPath p;
            size_t   n;
            size_t   i;
            int      doubled = 0;

            CHECK(http_path_resolve(roots[r], targets[t], &p) ==
                  HTTP_PATH_OK);

            n = strlen(p.path);
            CHECK(n > 0);

            /* No doubled separator anywhere: on AmigaOS that means the
               parent, so one here walks UP out of the document root. */
            for (i = 1; i < n; i++)
            {
                if (p.path[i] == '/' && p.path[i - 1] == '/')
                    doubled = 1;
            }
            CHECK(doubled == 0);

            /* Nothing ends in a separator except a device root, which is
               what httpd_parent() and the join both key off. */
            if (p.segments > 0)
            {
                CHECK(p.path[n - 1] != '/');
                CHECK(p.path[n - 1] != ':');
            }

            /* The name is one component and never a path. */
            CHECK(strchr(p.name, '/') == NULL);
            CHECK(strchr(p.name, ':') == NULL);

            /* A child's path always begins with its parent's, which is what
               a lock on a drawer and the copy-into-itself check both rest
               on. */
            CHECK(strncmp(p.path, roots[r], strlen(roots[r])) == 0);
        }
    }
}

/*
 * A name longer than a FileInfoBlock carries is refused rather than cut down.
 * Truncating would be worse than refusing: two different long names would
 * become one, and a PUT of the second would silently overwrite the first.
 */
static void test_long_name_refused(void)
{
    char   target[HTTP_URL_MAX];
    size_t n = 0;

    printf("a name longer than the machine carries\n");

    target[n++] = '/';
    while (n < (size_t)HTTP_NAME_MAX + 8)
        target[n++] = 'n';
    target[n] = '\0';

    CHECK(refused(target) == HTTP_PATH_TOO_LONG);

    /* And one that just fits still resolves, so the limit is the limit and
       not an off-by-one that costs a legal name. */
    target[HTTP_NAME_MAX - 1] = '\0';
    CHECK(refused(target) == HTTP_PATH_OK);
}

/*
 * A lock owner is collected into a fixed buffer and handed back inside a
 * <D:owner> in a document declared UTF-8.  Half a character in it is not a
 * character, and a client that validates rejects the whole answer over it.
 */
static void test_utf8_trim(void)
{
    char text[16];

    printf("a UTF-8 sequence cut off by the buffer\n");

    /* Nothing to do. */
    strcpy(text, "plain");
    http_utf8_trim(text);
    CHECK_STR(text, "plain");

    strcpy(text, "");
    http_utf8_trim(text);
    CHECK_STR(text, "");

    /* Complete sequences are left alone: two, three and four bytes. */
    strcpy(text, "Bj\xc3\xb6rn");
    http_utf8_trim(text);
    CHECK_STR(text, "Bj\xc3\xb6rn");

    strcpy(text, "a\xe2\x82\xac");             /* a euro sign               */
    http_utf8_trim(text);
    CHECK_STR(text, "a\xe2\x82\xac");

    strcpy(text, "a\xf0\x9f\x98\x80");         /* four bytes                */
    http_utf8_trim(text);
    CHECK_STR(text, "a\xf0\x9f\x98\x80");

    /* And the cut ones go, at every length and every point in the sequence. */
    strcpy(text, "Bj\xc3");                    /* half of two              */
    http_utf8_trim(text);
    CHECK_STR(text, "Bj");

    strcpy(text, "a\xe2");                     /* one of three             */
    http_utf8_trim(text);
    CHECK_STR(text, "a");

    strcpy(text, "a\xe2\x82");                 /* two of three             */
    http_utf8_trim(text);
    CHECK_STR(text, "a");

    strcpy(text, "a\xf0\x9f\x98");             /* three of four            */
    http_utf8_trim(text);
    CHECK_STR(text, "a");

    /* A continuation byte with no lead is not a character either. */
    strcpy(text, "a\x82");
    http_utf8_trim(text);
    CHECK_STR(text, "a");

    /* It never runs off the front of a string that is all continuations. */
    strcpy(text, "\x82\x82\x82\x82\x82");
    http_utf8_trim(text);
    CHECK(strlen(text) < 5);

    http_utf8_trim(NULL);
}

/* ------------------------------------------------------------- the walk --- */

static void test_join(void)
{
    char path[64];

    printf("joining a child onto a walked path\n");

    strcpy(path, "Work:Public");
    CHECK(http_path_join(path, sizeof(path), "Docs") == 1);
    CHECK_STR(path, "Work:Public/Docs");
    CHECK(http_path_join(path, sizeof(path), "readme.txt") == 1);
    CHECK_STR(path, "Work:Public/Docs/readme.txt");

    /* A device root already carries its separator.  "RAM:/foo" is the root
       DIRECTORY of the volume and not the drawer, which is a different
       place -- and "RAM://foo" is somewhere else again. */
    strcpy(path, "RAM:");
    CHECK(http_path_join(path, sizeof(path), "foo") == 1);
    CHECK_STR(path, "RAM:foo");

    strcpy(path, "Work:Public/");
    CHECK(http_path_join(path, sizeof(path), "x") == 1);
    CHECK_STR(path, "Work:Public/x");

    /* Names AmigaOS filenames really contain. */
    strcpy(path, "RAM:");
    CHECK(http_path_join(path, sizeof(path), "My File.txt") == 1);
    CHECK_STR(path, "RAM:My File.txt");
    strcpy(path, "RAM:");
    CHECK(http_path_join(path, sizeof(path), ".DS_Store") == 1);
    CHECK_STR(path, "RAM:.DS_Store");
    strcpy(path, "RAM:");
    CHECK(http_path_join(path, sizeof(path), "._resource") == 1);
    CHECK_STR(path, "RAM:._resource");

    /* A name that carries a separator did not come out of a FileInfoBlock,
       and joining it would step somewhere else entirely. */
    strcpy(path, "RAM:Docs");
    CHECK(http_path_join(path, sizeof(path), "a/b") == 0);
    CHECK_STR(path, "RAM:Docs");
    CHECK(http_path_join(path, sizeof(path), "DH0:x") == 0);
    CHECK_STR(path, "RAM:Docs");
    CHECK(http_path_join(path, sizeof(path), "a\\b") == 0);
    CHECK_STR(path, "RAM:Docs");
    CHECK(http_path_join(path, sizeof(path), "") == 0);
    CHECK_STR(path, "RAM:Docs");

    /* It does not fit, so it does not happen -- and the path is still the
       path it was, because the walk carries on using it. */
    {
        char tiny[16];

        strcpy(tiny, "RAM:Docs");
        CHECK(http_path_join(tiny, sizeof(tiny), "0123456789abcdef") == 0);
        CHECK_STR(tiny, "RAM:Docs");
    }
}

static void test_up(void)
{
    char path[64];

    printf("backing up one level\n");

    strcpy(path, "Work:Public/Docs/readme.txt");
    http_path_up(path);
    CHECK_STR(path, "Work:Public/Docs");
    http_path_up(path);
    CHECK_STR(path, "Work:Public");
    http_path_up(path);
    CHECK_STR(path, "Work:");

    /* A device reference has nothing above it, and going up from it for ever
       must not empty the string -- a walk that did would then delete
       whatever the current directory happens to be. */
    http_path_up(path);
    CHECK_STR(path, "Work:");
    http_path_up(path);
    CHECK_STR(path, "Work:");

    strcpy(path, "RAM:foo");
    http_path_up(path);
    CHECK_STR(path, "RAM:");
    http_path_up(path);
    CHECK_STR(path, "RAM:");

    /* Every join is undone by exactly one up, which is the property the tree
       walks are built on. */
    {
        int i;

        strcpy(path, "Work:Public");
        for (i = 0; i < 5; i++)
            CHECK(http_path_join(path, sizeof(path), "d") == 1);
        CHECK_STR(path, "Work:Public/d/d/d/d/d");
        for (i = 0; i < 5; i++)
            http_path_up(path);
        CHECK_STR(path, "Work:Public");
    }
}

static void test_within(void)
{
    printf("whether one path is inside another\n");

    CHECK(http_path_within("Work:Public", "Work:Public") == 1);
    CHECK(http_path_within("Work:Public", "Work:Public/a") == 1);
    CHECK(http_path_within("Work:Public", "Work:Public/a/b/c") == 1);
    CHECK(http_path_within("RAM:", "RAM:anything") == 1);
    CHECK(http_path_within("RAM:", "RAM:") == 1);

    /* AmigaDOS is case-insensitive, so a lock taken on one spelling has to
       cover a write made with another.  This is the check that decides
       whether a lock protects anything at all. */
    CHECK(http_path_within("Work:Public", "WORK:public/a") == 1);
    CHECK(http_path_within("Work:Docs/Notes", "work:docs/notes/x") == 1);

    /* A prefix is not a parent.  "Work:Public" must not cover
       "Work:PublicSecrets", or a lock on one drawer locks the other. */
    CHECK(http_path_within("Work:Public", "Work:PublicSecrets") == 0);
    CHECK(http_path_within("Work:Public", "Work:PublicSecrets/a") == 0);
    CHECK(http_path_within("Work:Public/a", "Work:Public/ab") == 0);

    /* Not inside at all, and the other way round. */
    CHECK(http_path_within("Work:Public/a", "Work:Public") == 0);
    CHECK(http_path_within("Work:Public", "DH0:Public/a") == 0);
    CHECK(http_path_within("Work:Public", "") == 0);
    CHECK(http_path_within("", "Work:Public") == 0);

    /* Both of the shapes a root can be written in. */
    CHECK(http_path_within("Work:Public/", "Work:Public/a") == 1);
    CHECK(http_path_within("Work:Public/", "Work:PublicSecrets") == 0);

    /* A COPY into itself is the walk that would not stop, and this is the
       test that catches it. */
    CHECK(http_path_within("Work:Public/Docs", "Work:Public/Docs/copy") == 1);
    CHECK(http_path_within("Work:Public/Docs", "Work:Public/Docs2") == 0);
}

/* ------------------------------------------------------------- escaping --- */

static void test_escaping(void)
{
    char out[256];

    printf("escaping, on the way back out\n");

    CHECK(http_url_escape("/Docs/x.txt", out, sizeof(out)) > 0);
    CHECK_STR(out, "/Docs/x.txt");

    /* The separator survives; everything a file name can hold does not. */
    CHECK(http_url_escape("/My File.txt", out, sizeof(out)) > 0);
    CHECK_STR(out, "/My%20File.txt");

    CHECK(http_url_escape("/a&b", out, sizeof(out)) > 0);
    CHECK_STR(out, "/a%26b");

    CHECK(http_url_escape("/Kickstart [!].rom", out, sizeof(out)) > 0);
    CHECK_STR(out, "/Kickstart%20%5B%21%5D.rom");

    /*
     * Percent-encoding leaves nothing an XML parser reads as markup, which is
     * why an href is escaped once and not twice.  If this ever stops being
     * true the multistatus needs the second pass.
     */
    CHECK(http_url_escape("/<a>&'\"", out, sizeof(out)) > 0);
    CHECK(strchr(out, '<') == NULL);
    CHECK(strchr(out, '>') == NULL);
    CHECK(strchr(out, '&') == NULL);
    CHECK(strchr(out, '"') == NULL);

    /* displayname is raw text, so it needs the other escaping. */
    CHECK(http_xml_escape("a & b", out, sizeof(out)) > 0);
    CHECK_STR(out, "a &amp; b");
    CHECK(http_xml_escape("<tag>", out, sizeof(out)) > 0);
    CHECK_STR(out, "&lt;tag&gt;");

    /* Neither writes past the end; both say so by returning 0. */
    {
        char tiny[4];

        CHECK(http_url_escape("/My File.txt", tiny, sizeof(tiny)) == 0);
        CHECK_STR(tiny, "");
        CHECK(http_xml_escape("aaaa&", tiny, sizeof(tiny)) == 0);
        CHECK_STR(tiny, "");
        CHECK(http_url_escape("x", tiny, 0) == 0);
    }
}

static void test_content_type(void)
{
    printf("content types\n");

    CHECK_STR(http_content_type("readme.txt"), "text/plain");
    CHECK_STR(http_content_type("INDEX.HTML"), "text/html");
    CHECK_STR(http_content_type("picture.IFF"), "image/x-ilbm");
    CHECK_STR(http_content_type("workbench.adf"), "application/x-amiga-disk");
    CHECK_STR(http_content_type("thing"), "application/octet-stream");
    CHECK_STR(http_content_type("thing."), "application/octet-stream");
    CHECK_STR(http_content_type("a.unknownsuffix"),
              "application/octet-stream");
    /* The suffix is the last one, not the first. */
    CHECK_STR(http_content_type("archive.tar.lha"), "application/x-lha");
}

int main(void)
{
    test_ordinary();
    test_device_escape();
    test_parent_escape();
    test_malformed();
    test_fields();
    test_root_is_identifiable();
    test_destination_forms();
    test_resolved_shape();
    test_long_name_refused();
    test_utf8_trim();
    test_join();
    test_up();
    test_within();
    test_escaping();
    test_content_type();

    printf("\n%d checks, %d failure(s)\n", checks, failures);

    return failures == 0 ? 0 : 1;
}

/*
 * The tests for src/tools/httppath.c -- the one part of the WebDAV server
 * that decides whether a request can reach a file outside the document root.
 *
 * WHY THIS IS A TEST AND NOT A REVIEW
 *
 *   The server is read-only today and is meant to write later.  A path check
 *   that is merely "looked right" leaks a file now and destroys one when
 *   DELETE lands, and the mistakes are all the same shape: a thing that does
 *   not look like an escape until something else has decoded it.  So the
 *   escapes are written down here, one case each, and the file is built for
 *   the host so they run in `tools/ci.sh host` on every change rather than
 *   only when somebody boots an Amiga.
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
    test_escaping();
    test_content_type();

    printf("\n%d checks, %d failure(s)\n", checks, failures);

    return failures == 0 ? 0 : 1;
}

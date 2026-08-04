/*
 * The tests for src/tools/fetchurl.c, the URL half of `fetch`.
 *
 * WHY THIS IS A TEST AND NOT A REVIEW
 *
 *   Both halves of this file fail silently when they are wrong.  A relative
 *   Location: resolved as if it were absolute asks the resolver for a host
 *   called "about.html", and "docs/x.html" becomes host "docs", neither
 *   reads as a bug in the code that does it.  An interim 1xx response taken
 *   for a final one writes the real response, status line and headers
 *   included, into the user's file as body and returns success.
 *
 *   RFC 3986 section 5.4 is a ready-made table of thirty-six references and
 *   the target URI each one resolves to against a single base, so the
 *   resolution half is not tested against what this implementation happens to
 *   do, it is tested against the specification's own worked answers, which
 *   is the only kind of test worth having here.  The handful that cannot pass
 *   verbatim are listed in the deviations block below with the reason.
 *
 *     cc -std=c11 -Wall -Wextra -Isrc/tools \
 *        src/tools/test/test_fetchurl.c src/tools/fetchurl.c -o test_fetchurl
 *
 * SPDX-License-Identifier: MIT
 */

#include "fetchurl.h"

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

/*
 * The absolute form of a parsed URL, for comparing against section 5.4's
 * right-hand column.  Sized for the worst case rather than FETCH_URL_MAX:
 * a maximum authority and a maximum path together overrun that, and a URL
 * this test never builds is still one -Wformat-truncation reasons about.
 */
#define COMPOSED_MAX    (8 + FETCH_AUTHORITY_MAX + FETCH_PATH_MAX + 8)

static const char *composed(const FetchUrl *u)
{
    static char out[COMPOSED_MAX];
    char        auth[FETCH_AUTHORITY_MAX];

    if (!fetch_url_authority(u, auth, sizeof(auth)))
        return "<authority too long>";

    snprintf(out, sizeof(out), "%s://%s%s",
             u->secure ? "https" : "http", auth, u->path);

    return out;
}


/* ------------------------------------------------- RFC 3986 section 5.4 --- */

/*
 * Base: http://a/b/c/d;p?q
 *
 * DEVIATIONS, all deliberate:
 *
 *   "g:h"  ->  "g:h".  A URI in a scheme this command does not speak.  It is
 *              resolved correctly, scheme "g", no authority, and then
 *              refused, which is checked below rather than skipped.
 *
 *   Every expected target carrying a "#fragment" has it dropped, so "#s"
 *              gives "http://a/b/c/d;p?q" rather than ".../d;p?q#s".  RFC 9110
 *              section 7.1: the target URI excludes the fragment, and section
 *              10.2.2's rule that a fragment-less Location inherits the
 *              original reference's fragment concerns what a user agent
 *              displays, not what it puts on the wire.
 *
 *   "//g"  ->  "http://g" becomes "http://g/".  An empty path is written "/"
 *              in the request line, per RFC 9112 section 3.2.1.
 *
 *   "http:g" -> the strict answer, "http:g", is a scheme with no authority,
 *              so it is refused here too.  Section 5.4.2 records the
 *              alternative ("http://a/b/c/g") as what a NON-strict parser
 *              produces for backward compatibility; this one is strict.
 */

typedef struct RefCase
{
    const char *ref;
    const char *want;       /* NULL: the reference must be refused */
} RefCase;

static const RefCase rfc3986_normal[] =
{
    { "g:h",        NULL                    },  /* deviation, see above */
    { "g",          "http://a/b/c/g"        },
    { "./g",        "http://a/b/c/g"        },
    { "g/",         "http://a/b/c/g/"       },
    { "/g",         "http://a/g"            },
    { "//g",        "http://g/"             },  /* deviation: empty path */
    { "?y",         "http://a/b/c/d;p?y"    },
    { "g?y",        "http://a/b/c/g?y"      },
    { "#s",         "http://a/b/c/d;p?q"    },  /* deviation: no fragment */
    { "g#s",        "http://a/b/c/g"        },  /* deviation: no fragment */
    { "g?y#s",      "http://a/b/c/g?y"      },  /* deviation: no fragment */
    { ";x",         "http://a/b/c/;x"       },
    { "g;x",        "http://a/b/c/g;x"      },
    { "g;x?y#s",    "http://a/b/c/g;x?y"    },  /* deviation: no fragment */
    { "",           "http://a/b/c/d;p?q"    },
    { ".",          "http://a/b/c/"         },
    { "./",         "http://a/b/c/"         },
    { "..",         "http://a/b/"           },
    { "../",        "http://a/b/"           },
    { "../g",       "http://a/b/g"          },
    { "../..",      "http://a/"             },
    { "../../",     "http://a/"             },
    { "../../g",    "http://a/g"            }
};

static const RefCase rfc3986_abnormal[] =
{
    { "../../../g",     "http://a/g"                },
    { "../../../../g",  "http://a/g"                },
    { "/./g",           "http://a/g"                },
    { "/../g",          "http://a/g"                },
    { "g.",             "http://a/b/c/g."           },
    { ".g",             "http://a/b/c/.g"           },
    { "g..",            "http://a/b/c/g.."          },
    { "..g",            "http://a/b/c/..g"          },
    { "./../g",         "http://a/b/g"              },
    { "./g/.",          "http://a/b/c/g/"           },
    { "g/./h",          "http://a/b/c/g/h"          },
    { "g/../h",         "http://a/b/c/h"            },
    { "g;x=1/./y",      "http://a/b/c/g;x=1/y"      },
    { "g;x=1/../y",     "http://a/b/c/y"            },
    { "g?y/./x",        "http://a/b/c/g?y/./x"      },
    { "g?y/../x",       "http://a/b/c/g?y/../x"     },
    { "g#s/./x",        "http://a/b/c/g"            },  /* no fragment */
    { "g#s/../x",       "http://a/b/c/g"            },  /* no fragment */
    { "http:g",         NULL                        }   /* strict, see above */
};

static void run_table(const FetchUrl *base, const RefCase *cases, int n,
                      const char *label)
{
    int i;

    for (i = 0; i < n; i++)
    {
        FetchUrl       got;
        FetchUrlResult why = fetch_url_resolve(base, cases[i].ref, &got);

        checks++;

        if (cases[i].want == NULL)
        {
            if (why == FETCH_URL_OK)
            {
                failures++;
                printf("  FAIL %s \"%s\": accepted, expected a refusal (%s)\n",
                       label, cases[i].ref, composed(&got));
            }
            continue;
        }

        if (why != FETCH_URL_OK)
        {
            failures++;
            printf("  FAIL %s \"%s\": %s\n", label, cases[i].ref,
                   fetch_url_error(why));
            continue;
        }

        if (strcmp(composed(&got), cases[i].want) != 0)
        {
            failures++;
            printf("  FAIL %s \"%s\": expected \"%s\", got \"%s\"\n",
                   label, cases[i].ref, cases[i].want, composed(&got));
        }
    }
}

static void test_rfc3986_examples(void)
{
    FetchUrl base;

    CHECK(fetch_url_parse("http://a/b/c/d;p?q", &base) == FETCH_URL_OK);
    CHECK_STR(composed(&base), "http://a/b/c/d;p?q");

    run_table(&base, rfc3986_normal,
              (int)(sizeof(rfc3986_normal) / sizeof(rfc3986_normal[0])),
              "5.4.1");
    run_table(&base, rfc3986_abnormal,
              (int)(sizeof(rfc3986_abnormal) / sizeof(rfc3986_abnormal[0])),
              "5.4.2");
}


/* ------------------------------------------------------ what fetch needs --- */

/* The two shapes the old resolver got wrong, spelled out on their own. */
static void test_relative_redirects(void)
{
    FetchUrl base;
    FetchUrl got;

    CHECK(fetch_url_parse("http://example.com/dir/page.html", &base)
          == FETCH_URL_OK);

    /* "about.html" used to become a DNS lookup for a host called about.html */
    CHECK(fetch_url_resolve(&base, "about.html", &got) == FETCH_URL_OK);
    CHECK_STR(got.host, "example.com");
    CHECK_STR(got.path, "/dir/about.html");

    /* "docs/x.html" used to become host "docs", path "/x.html" */
    CHECK(fetch_url_resolve(&base, "docs/x.html", &got) == FETCH_URL_OK);
    CHECK_STR(got.host, "example.com");
    CHECK_STR(got.path, "/dir/docs/x.html");

    /* An absolute one still works, scheme and all. */
    CHECK(fetch_url_resolve(&base, "https://other.example/x", &got)
          == FETCH_URL_OK);
    CHECK(got.secure == 1);
    CHECK(got.port == 443);
    CHECK_STR(got.host, "other.example");
    CHECK_STR(got.path, "/x");

    /* A network-path reference keeps the base's scheme, so an https: base
       cannot be walked down to http: by one. */
    CHECK(fetch_url_parse("https://secure.example/a/b", &base)
          == FETCH_URL_OK);
    CHECK(fetch_url_resolve(&base, "//other.example/c", &got)
          == FETCH_URL_OK);
    CHECK(got.secure == 1);
    CHECK(got.port == 443);
    CHECK_STR(got.host, "other.example");
    CHECK_STR(got.path, "/c");
}

/* RFC 3986 section 3.5 / RFC 9110 section 7.1: the fragment is never sent,
   including on the same-host fast path a redirect to "/..." takes. */
static void test_fragment_is_dropped(void)
{
    FetchUrl base;
    FetchUrl got;

    CHECK(fetch_url_parse("http://example.com/a/b?q=1#top", &base)
          == FETCH_URL_OK);
    CHECK_STR(base.path, "/a/b?q=1");

    CHECK(fetch_url_resolve(&base, "/next#frag", &got) == FETCH_URL_OK);
    CHECK_STR(got.path, "/next");

    CHECK(fetch_url_resolve(&base, "/next?x=1#frag", &got) == FETCH_URL_OK);
    CHECK_STR(got.path, "/next?x=1");
}

/* RFC 9112 section 3.2: the Host field is the target URI's authority, so a
   non-default port is part of it and a default one is not. */
static void test_host_field(void)
{
    FetchUrl u;
    char     auth[FETCH_AUTHORITY_MAX];

    CHECK(fetch_url_parse("http://host:8080/x", &u) == FETCH_URL_OK);
    CHECK(u.port == 8080);
    CHECK(fetch_url_authority(&u, auth, sizeof(auth)));
    CHECK_STR(auth, "host:8080");

    CHECK(fetch_url_parse("http://host/x", &u) == FETCH_URL_OK);
    CHECK(u.port == 80);
    CHECK(fetch_url_authority(&u, auth, sizeof(auth)));
    CHECK_STR(auth, "host");

    CHECK(fetch_url_parse("https://host/x", &u) == FETCH_URL_OK);
    CHECK(u.port == 443);
    CHECK(fetch_url_authority(&u, auth, sizeof(auth)));
    CHECK_STR(auth, "host");

    /* The default written out is still the default. */
    CHECK(fetch_url_parse("https://host:443/x", &u) == FETCH_URL_OK);
    CHECK(fetch_url_authority(&u, auth, sizeof(auth)));
    CHECK_STR(auth, "host");

    /* ...and 80 on an https: URL is not. */
    CHECK(fetch_url_parse("https://host:80/x", &u) == FETCH_URL_OK);
    CHECK(fetch_url_authority(&u, auth, sizeof(auth)));
    CHECK_STR(auth, "host:80");

    /* An IPv6 literal keeps its brackets in the field, per RFC 3986. */
    CHECK(fetch_url_parse("http://[2001:db8::1]:8080/x", &u) == FETCH_URL_OK);
    CHECK_STR(u.host, "[2001:db8::1]");
    CHECK(u.port == 8080);
    CHECK(fetch_url_authority(&u, auth, sizeof(auth)));
    CHECK_STR(auth, "[2001:db8::1]:8080");

    /* A redirect to a host with no port must not inherit the old one. */
    {
        FetchUrl got;

        CHECK(fetch_url_resolve(&u, "http://plain.example/y", &got)
              == FETCH_URL_OK);
        CHECK(got.port == 80);
    }
}

/* RFC 9110 section 4.2.4 */
static void test_userinfo_refused(void)
{
    FetchUrl u;
    FetchUrl got;

    CHECK(fetch_url_parse("http://user@example.com/", &u)
          == FETCH_URL_USERINFO);
    CHECK(fetch_url_parse("http://user:pass@example.com/", &u)
          == FETCH_URL_USERINFO);

    /* And on a redirect, which is where it is a phishing tool. */
    CHECK(fetch_url_parse("http://example.com/", &u) == FETCH_URL_OK);
    CHECK(fetch_url_resolve(&u, "http://example.com@evil.example/", &got)
          == FETCH_URL_USERINFO);
}

static void test_parse_shapes(void)
{
    FetchUrl u;

    /* A bare host is http, and an empty path is "/". */
    CHECK(fetch_url_parse("example.com", &u) == FETCH_URL_OK);
    CHECK(u.secure == 0);
    CHECK(u.port == 80);
    CHECK_STR(u.host, "example.com");
    CHECK_STR(u.path, "/");

    /* "host:port/path" is a port, not a scheme called "host". */
    CHECK(fetch_url_parse("example.com:8080/a", &u) == FETCH_URL_OK);
    CHECK_STR(u.host, "example.com");
    CHECK(u.port == 8080);
    CHECK_STR(u.path, "/a");

    CHECK(fetch_url_parse("ftp://example.com/a", &u) == FETCH_URL_SCHEME);
    CHECK(fetch_url_parse("http:///a", &u) == FETCH_URL_NO_HOST);
    CHECK(fetch_url_parse("http://[2001:db8::1/a", &u) == FETCH_URL_BRACKET);
    CHECK(fetch_url_parse("http://host:80x/a", &u) == FETCH_URL_PORT);
    CHECK(fetch_url_parse("http://host:99999/a", &u) == FETCH_URL_PORT_RANGE);

    /* Dot segments in a URL the user typed are removed too. */
    CHECK(fetch_url_parse("http://host/a/b/../c", &u) == FETCH_URL_OK);
    CHECK_STR(u.path, "/a/c");

    /* A query survives untouched, dots and all. */
    CHECK(fetch_url_parse("http://host/a?x=../y", &u) == FETCH_URL_OK);
    CHECK_STR(u.path, "/a?x=../y");
}


/* -------------------------------------------------- the interim response --- */

/*
 * RFC 9110 section 15.2: "A client MUST be able to parse one or more 1xx
 * responses received prior to a final response".  Feeding the bytes through
 * the same accumulator fetch uses proves the whole 103 is discarded and the
 * body is only what followed the 200.
 */
static void test_interim_responses(void)
{
    static const char wire[] =
        "HTTP/1.1 103 Early Hints\r\n"
        "Link: </s.css>; rel=preload; as=style\r\n"
        "\r\n"
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/plain\r\n"
        "\r\n"
        "the body";

    FetchHead     head;
    char          buf[FETCH_HEAD_MAX];
    unsigned long at    = 0;
    unsigned long total = sizeof(wire) - 1;
    int           interim = 0;
    char          line[128];

    fetch_head_start(&head, buf, sizeof(buf));

    for (;;)
    {
        at += fetch_head_feed(&head, (const unsigned char *)wire + at,
                              total - at);
        CHECK(head.complete);

        if (!fetch_head_interim(fetch_head_status(&head)))
            break;

        interim++;
        fetch_head_start(&head, buf, sizeof(buf));
    }

    CHECK(interim == 1);
    CHECK(fetch_head_status(&head) == 200);

    fetch_head_first_line(&head, line, sizeof(line));
    CHECK_STR(line, "HTTP/1.1 200 OK");

    /* The 103's headers are gone, not merely skipped over. */
    CHECK(fetch_head_field(&head, "link:") == 0);
    CHECK(fetch_head_field(&head, "content-type:") != 0);

    /* And what is left is the body and nothing else. */
    CHECK_STR(wire + at, "the body");
}

/* One byte at a time: the terminator scan must survive a split across reads,
   which is how a 1xx arrives in practice, alone in its own segment. */
static void test_interim_split_reads(void)
{
    static const char wire[] =
        "HTTP/1.1 100 Continue\r\n\r\n"
        "HTTP/1.1 204 No Content\r\n\r\n";

    FetchHead     head;
    char          buf[FETCH_HEAD_MAX];
    unsigned long at    = 0;
    unsigned long total = sizeof(wire) - 1;
    int           interim = 0;

    fetch_head_start(&head, buf, sizeof(buf));

    while (at < total)
    {
        at += fetch_head_feed(&head, (const unsigned char *)wire + at, 1);

        if (!head.complete)
            continue;

        if (!fetch_head_interim(fetch_head_status(&head)))
            break;

        interim++;
        fetch_head_start(&head, buf, sizeof(buf));
    }

    CHECK(interim == 1);
    CHECK(fetch_head_status(&head) == 204);
    CHECK(at == total);
}

/* A bare LFLF terminator, and a Location: read off it. */
static void test_head_fields(void)
{
    static const char wire[] =
        "HTTP/1.0 301 Moved Permanently\n"
        "Location: /elsewhere\n"
        "\n";

    FetchHead     head;
    char          buf[FETCH_HEAD_MAX];
    char          value[FETCH_URL_MAX];
    const char   *loc;
    unsigned long at;

    fetch_head_start(&head, buf, sizeof(buf));
    at = fetch_head_feed(&head, (const unsigned char *)wire, sizeof(wire) - 1);

    CHECK(head.complete);
    CHECK(at == sizeof(wire) - 1);
    CHECK(!head.truncated);
    CHECK(fetch_head_status(&head) == 301);

    loc = fetch_head_field(&head, "location:");
    CHECK(loc != 0);
    CHECK(loc != 0 && fetch_head_value(loc, value, sizeof(value)));
    CHECK_STR(value, "/elsewhere");

    /* A header name that only prefixes a real one must not match. */
    CHECK(fetch_head_field(&head, "loc:") == 0);
}

/* Running out of room loses the tail of the headers, not the transfer. */
static void test_head_truncation(void)
{
    FetchHead     head;
    char          buf[64];
    char          wire[512];
    unsigned long i;
    unsigned long at;

    memcpy(wire, "HTTP/1.1 200 OK\r\nX: ", 20);
    for (i = 20; i < sizeof(wire) - 5; i++)
        wire[i] = 'y';
    memcpy(wire + sizeof(wire) - 5, "\r\n\r\n", 4);

    fetch_head_start(&head, buf, sizeof(buf));
    at = fetch_head_feed(&head, (const unsigned char *)wire, sizeof(wire) - 1);

    CHECK(head.complete);
    CHECK(head.truncated);
    CHECK(at == sizeof(wire) - 1);
    CHECK(fetch_head_status(&head) == 200);
}


/* ------------------------------------------------------------------ main --- */

int main(void)
{
    test_rfc3986_examples();
    test_relative_redirects();
    test_fragment_is_dropped();
    test_host_field();
    test_userinfo_refused();
    test_parse_shapes();
    test_interim_responses();
    test_interim_split_reads();
    test_head_fields();
    test_head_truncation();

    printf("%s: %d checks, %d failures\n",
           failures == 0 ? "PASS" : "FAIL", checks, failures);

    return failures == 0 ? 0 : 1;
}

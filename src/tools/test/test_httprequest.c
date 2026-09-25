/* Tests for the portable request-value policy used by httpd.
 * SPDX-License-Identifier: MIT
 */

#include "httprequest.h"

#include <stdio.h>
#include <string.h>

static int checks;
static int failures;

#define CHECK(cond)                                                          \
    do {                                                                     \
        checks++;                                                            \
        if (!(cond)) {                                                       \
            failures++;                                                      \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
        }                                                                    \
    } while (0)

static void test_query(void)
{
    printf("interactive query\n");

    CHECK(http_request_query_take("/shell?take=1"));
    CHECK(http_request_query_take("/shell?a=0&take=1&b=2"));
    CHECK(http_request_query_take("/shell?TAKE=1"));
    CHECK(!http_request_query_take("/shell"));
    CHECK(!http_request_query_take("/shell?take=0"));
    CHECK(!http_request_query_take("/shell?retake=1"));
    CHECK(!http_request_query_take("/shell?take=10"));
    CHECK(!http_request_query_take(NULL));
    CHECK(http_request_query_session("/shell") == 0);
    CHECK(http_request_query_session("/shell?session=0") == 0);
    CHECK(http_request_query_session("/shell?take=1&session=1") == 1);
    CHECK(http_request_query_session("/shell?session=1&take=1") == 1);
    CHECK(http_request_query_session("/shell?session=2") == -1);
    CHECK(http_request_query_session("/shell?session=01") == -1);
    CHECK(http_request_query_session("/shell?session=1&session=0") == -1);
    CHECK(http_request_query_session("/shell?session") == -1);
}

static void test_timeout(void)
{
    printf("WebDAV Timeout\n");

    CHECK(http_request_timeout("Infinite", 3600UL) == 3600UL);
    CHECK(http_request_timeout(" infinite", 3600UL) == 3600UL);
    CHECK(http_request_timeout("Second-1", 3600UL) == 1UL);
    CHECK(http_request_timeout("Second-3600", 3600UL) == 3600UL);
    CHECK(http_request_timeout("Second-3601", 3600UL) == 3600UL);
    CHECK(http_request_timeout("Second-00012", 3600UL) == 12UL);
    CHECK(http_request_timeout("Second-999999999999999999999999999",
                               3600UL) == 3600UL);
    CHECK(http_request_timeout("Second-", 3600UL) == 0UL);
    CHECK(http_request_timeout("junk", 3600UL) == 0UL);
    CHECK(http_request_timeout(NULL, 3600UL) == 0UL);
}

static void test_gzip(void)
{
    printf("Accept-Encoding\n");

    CHECK(http_request_accepts_gzip("gzip"));
    CHECK(http_request_accepts_gzip("GZip"));
    CHECK(http_request_accepts_gzip("deflate, gzip;q=1.0, *;q=0.5"));
    CHECK(http_request_accepts_gzip("gzip;q=0.001"));
    CHECK(!http_request_accepts_gzip("gzip;q=0"));
    CHECK(!http_request_accepts_gzip("gzip;q=0.000"));
    CHECK(!http_request_accepts_gzip("br, deflate"));
    CHECK(!http_request_accepts_gzip("gzip-more"));
    CHECK(!http_request_accepts_gzip("*"));
    CHECK(!http_request_accepts_gzip(NULL));
}

static void test_lock_tokens(void)
{
    char token[2][32];
    char long_one[48];
    unsigned long n;

    printf("WebDAV If tokens\n");

    n = http_request_lock_tokens(
        "(<opaquelocktoken:first>) (<opaquelocktoken:second>)",
        &token[0][0], sizeof(token[0]), 2UL);
    CHECK(n == 2UL);
    CHECK(strcmp(token[0], "opaquelocktoken:first") == 0);
    CHECK(strcmp(token[1], "opaquelocktoken:second") == 0);

    n = http_request_lock_tokens(
        "<http://amiga.local/file> (<opaquelocktoken:one>)",
        &token[0][0], sizeof(token[0]), 2UL);
    CHECK(n == 1UL);
    CHECK(strcmp(token[0], "opaquelocktoken:one") == 0);
    CHECK(token[1][0] == '\0');

    memset(long_one, 'x', sizeof(long_one));
    memcpy(long_one, "<opaquelocktoken:", 17);
    long_one[sizeof(long_one) - 2] = '>';
    long_one[sizeof(long_one) - 1] = '\0';
    CHECK(http_request_lock_tokens(long_one, &token[0][0],
                                   sizeof(token[0]), 2UL) == 0UL);
    CHECK(token[0][0] == '\0' && token[1][0] == '\0');

    CHECK(http_request_lock_tokens(NULL, &token[0][0],
                                   sizeof(token[0]), 2UL) == 0UL);
    CHECK(http_request_lock_tokens("<opaquelocktoken:x>", NULL,
                                   sizeof(token[0]), 2UL) == 0UL);
}

static void test_iperf_segments(void)
{
    unsigned long v4 = 0;

    printf("iperf path segments\n");

    CHECK(http_request_decimal("10", 2UL) == 10L);
    CHECK(http_request_decimal("007", 3UL) == 7L);
    CHECK(http_request_decimal("999999999", 9UL) == 999999999L);
    CHECK(http_request_decimal("10/tcp", 2UL) == 10L);    /* len bounds it */
    CHECK(http_request_decimal("", 0UL) == -1L);
    CHECK(http_request_decimal("1000000000", 10UL) == -1L);
    CHECK(http_request_decimal("1x", 2UL) == -1L);
    CHECK(http_request_decimal("-1", 2UL) == -1L);

    CHECK(http_request_dotted("192.168.1.88", &v4) && v4 == 0xC0A80158UL);
    CHECK(http_request_dotted("0.0.0.0", &v4) && v4 == 0UL);
    CHECK(http_request_dotted("255.255.255.255", &v4) && v4 == 0xFFFFFFFFUL);
    CHECK(!http_request_dotted("256.1.1.1", &v4));
    CHECK(!http_request_dotted("1.2.3", &v4));
    CHECK(!http_request_dotted("1.2.3.4.5", &v4));
    CHECK(!http_request_dotted("1.2.3.4:5001", &v4));
    CHECK(!http_request_dotted("1..3.4", &v4));
    CHECK(!http_request_dotted("1234.1.1.1", &v4));
    CHECK(!http_request_dotted("amiga.local", &v4));
    CHECK(!http_request_dotted("", &v4));
}

int main(void)
{
    test_query();
    test_timeout();
    test_gzip();
    test_lock_tokens();
    test_iperf_segments();

    printf("\n%d checks, %d failure(s)\n", checks, failures);
    return failures == 0 ? 0 : 1;
}

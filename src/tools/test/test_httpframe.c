/*
 * The tests for src/tools/httpframe.c, how long a request body is, and where
 * it ends.
 *
 * SPDX-License-Identifier: MIT
 */

#include "httpframe.h"

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

static void test_length(void)
{
    unsigned long n;

    printf("Content-Length\n");

    CHECK(http_frame_length("0", &n) == HTTP_FRAME_OK && n == 0UL);
    CHECK(http_frame_length("1", &n) == HTTP_FRAME_OK && n == 1UL);
    CHECK(http_frame_length("65536", &n) == HTTP_FRAME_OK && n == 65536UL);
    CHECK(http_frame_length("0000012", &n) == HTTP_FRAME_OK && n == 12UL);

    /* The whole of a 32-bit count, and the first value past it. */
    CHECK(http_frame_length("4294967295", &n) == HTTP_FRAME_OK &&
          n == 4294967295UL);
    CHECK(http_frame_length("4294967296", &n) == HTTP_FRAME_OVERFLOW);

    CHECK(http_frame_length("4294967306", &n) == HTTP_FRAME_OVERFLOW);
    CHECK(http_frame_length("99999999999999999999", &n) ==
          HTTP_FRAME_OVERFLOW);

    CHECK(http_frame_length("5abc", &n) == HTTP_FRAME_JUNK);
    CHECK(http_frame_length("5 6", &n) == HTTP_FRAME_JUNK);
    CHECK(http_frame_length("+5", &n) == HTTP_FRAME_EMPTY);
    CHECK(http_frame_length("-5", &n) == HTTP_FRAME_EMPTY);
    CHECK(http_frame_length("", &n) == HTTP_FRAME_EMPTY);
    CHECK(http_frame_length(" ", &n) == HTTP_FRAME_EMPTY);
    CHECK(http_frame_length("0x10", &n) == HTTP_FRAME_JUNK);

    /* Trailing whitespace is what a header value can carry after it. */
    CHECK(http_frame_length("42 ", &n) == HTTP_FRAME_OK && n == 42UL);
    CHECK(http_frame_length("42\t", &n) == HTTP_FRAME_OK && n == 42UL);

    /* Nothing is written when it is refused. */
    n = 0xdeadUL;
    CHECK(http_frame_length("5abc", &n) != HTTP_FRAME_OK && n == 0UL);

    CHECK(http_frame_error(HTTP_FRAME_OVERFLOW) != NULL);
}

static void test_coding(void)
{
    printf("Transfer-Encoding\n");

    CHECK(http_frame_coding("chunked") == HTTP_TE_CHUNKED);
    CHECK(http_frame_coding("Chunked") == HTTP_TE_CHUNKED);
    CHECK(http_frame_coding("CHUNKED") == HTTP_TE_CHUNKED);
    CHECK(http_frame_coding(" chunked ") == HTTP_TE_CHUNKED);
    CHECK(http_frame_coding("\tchunked") == HTTP_TE_CHUNKED);

    CHECK(http_frame_coding(NULL) == HTTP_TE_IDENTITY);
    CHECK(http_frame_coding("") == HTTP_TE_UNSUPPORTED);
    CHECK(http_frame_coding("identity") == HTTP_TE_UNSUPPORTED);

    CHECK(http_frame_coding("chunkedX") == HTTP_TE_UNSUPPORTED);
    CHECK(http_frame_coding("chunked-ish") == HTTP_TE_UNSUPPORTED);
    CHECK(http_frame_coding("gzip, chunked") == HTTP_TE_UNSUPPORTED);
    CHECK(http_frame_coding("chunked, gzip") == HTTP_TE_UNSUPPORTED);
    CHECK(http_frame_coding("gzip") == HTTP_TE_UNSUPPORTED);
    CHECK(http_frame_coding("deflate") == HTTP_TE_UNSUPPORTED);

    /* Two of them is a list this server cannot apply, whichever they are. */
    CHECK(http_frame_coding("chunked, chunked") == HTTP_TE_UNSUPPORTED);
    CHECK(http_frame_coding("identity, chunked") == HTTP_TE_UNSUPPORTED);

    /* A parameter makes it a different coding. */
    CHECK(http_frame_coding("chunked;q=1") == HTTP_TE_UNSUPPORTED);
}

static void test_tokens(void)
{
    printf("HTTP token lists\n");

    CHECK(http_frame_token_is("100-continue", "100-continue"));
    CHECK(http_frame_token_is(" 100-continue\t", "100-continue"));
    CHECK(!http_frame_token_is("100-continue-more", "100-continue"));
    CHECK(!http_frame_token_is("100-continue, other", "100-continue"));
    CHECK(!http_frame_token_is(NULL, "100-continue"));
    CHECK(http_frame_token_is("T", "t"));
    CHECK(http_frame_token_is(" f ", "f"));
    CHECK(!http_frame_token_is("false", "f"));

    CHECK(http_frame_has_token("close", "close"));
    CHECK(http_frame_has_token("keep-alive, close", "close"));
    CHECK(http_frame_has_token(" close , Upgrade ", "upgrade"));
    CHECK(http_frame_has_token("Keep-Alive, UpGrAdE", "upgrade"));
    CHECK(http_frame_has_token("h2c, WebSocket", "websocket"));
    CHECK(!http_frame_has_token("keep-alive", "close"));
    CHECK(!http_frame_has_token("disclose", "close"));
    CHECK(!http_frame_has_token("upgrade-more", "upgrade"));
    CHECK(!http_frame_has_token("websocketX", "websocket"));
    CHECK(!http_frame_has_token("13junk", "13"));
    CHECK(!http_frame_has_token("", "close"));
    CHECK(!http_frame_has_token(NULL, "close"));
}

static void test_field_names(void)
{
    unsigned long colon;

    printf("HTTP field names\n");

    CHECK(http_frame_field_name("Host: example", 13, &colon) && colon == 4UL);
    CHECK(http_frame_field_name("X_a-1: yes", 10, &colon) && colon == 5UL);
    CHECK(!http_frame_field_name("Content-Length : 5", 18, &colon));
    CHECK(!http_frame_field_name(" Content-Length: 5", 18, &colon));
    CHECK(!http_frame_field_name("Content Length: 5", 17, &colon));
    CHECK(!http_frame_field_name("Content-Length", 14, &colon));
    CHECK(!http_frame_field_name(": value", 7, &colon));
    CHECK(!http_frame_field_name(NULL, 0, &colon));
}

static void test_etags(void)
{
    printf("entity-tag lists\n");

    CHECK(http_frame_etag_listed("\"1-2\"", "\"1-2\"", 0));
    CHECK(http_frame_etag_listed("\"wrong\", \"1-2\"", "\"1-2\"", 0));
    CHECK(!http_frame_etag_listed("\"1-2\"junk", "\"1-2\"", 0));
    CHECK(!http_frame_etag_listed("\"1-20\"", "\"1-2\"", 0));
    CHECK(!http_frame_etag_listed("\"ABC\"", "\"abc\"", 0));

    /* If-Match is strong; If-None-Match is weak. */
    CHECK(!http_frame_etag_listed("W/\"1-2\"", "\"1-2\"", 0));
    CHECK(http_frame_etag_listed("W/\"1-2\"", "\"1-2\"", 1));
    CHECK(http_frame_etag_listed("\"1-2\"", "W/\"1-2\"", 1));
    CHECK(!http_frame_etag_listed(NULL, "\"1-2\"", 1));
}

static void test_versions(void)
{
    printf("HTTP versions\n");

    CHECK(http_frame_version("HTTP/1.0", 8) == HTTP_VERSION_10);
    CHECK(http_frame_version("HTTP/1.1", 8) == HTTP_VERSION_11);
    CHECK(http_frame_version("HTTP/1.2", 8) == HTTP_VERSION_BAD);
    CHECK(http_frame_version("HTTP/1.10", 9) == HTTP_VERSION_BAD);
    CHECK(http_frame_version("HTTP/1.1 junk", 13) == HTTP_VERSION_BAD);
    CHECK(http_frame_version("", 0) == HTTP_VERSION_BAD);
    CHECK(http_frame_version(NULL, 0) == HTTP_VERSION_BAD);
}

static void test_combined_lists(void)
{
    char list[24];

    printf("combined field lines\n");

    list[0] = '\0';
    CHECK(http_frame_list_add(list, sizeof(list), "\"one\""));
    CHECK(strcmp(list, "\"one\"") == 0);
    CHECK(http_frame_list_add(list, sizeof(list), "\"two\""));
    CHECK(strcmp(list, "\"one\", \"two\"") == 0);

    /* A value that does not fit leaves the complete old condition intact. */
    CHECK(!http_frame_list_add(list, sizeof(list), "\"far-too-long\""));
    CHECK(strcmp(list, "\"one\", \"two\"") == 0);
    CHECK(!http_frame_list_add(NULL, 0, "x"));
}

static void test_ranges(void)
{
    unsigned long from;
    unsigned long to;

    printf("byte ranges\n");

    CHECK(http_frame_range("bytes=0-1023", &from, &to) &&
          from == 0UL && to == 1023UL);
    CHECK(http_frame_range("Bytes=1024-", &from, &to) &&
          from == 1024UL && to == 4294967295UL);
    CHECK(http_frame_range("bytes=1-2\t", &from, &to));
    CHECK(!http_frame_range("bytes=-512", &from, &to));
    CHECK(!http_frame_range("bytes=5", &from, &to));
    CHECK(!http_frame_range("bytes=0-1,2-3", &from, &to));
    CHECK(!http_frame_range("bytes=0-1junk", &from, &to));
    CHECK(!http_frame_range("bytes=4294967296-", &from, &to));
    CHECK(!http_frame_range("bytes=0-4294967296", &from, &to));
}

static char  sunk[512];
static long  sunk_n;

static void collect(void *ctx, const unsigned char *data, long len)
{
    long i;

    (void)ctx;

    for (i = 0; i < len; i++)
    {
        if (sunk_n + 1 < (long)sizeof(sunk))
            sunk[sunk_n++] = (char)data[i];
    }

    sunk[sunk_n] = '\0';
}

/* Feed a whole body in one call. */
static long feed(HttpChunk *ch, const char *text)
{
    sunk_n  = 0;
    sunk[0] = '\0';

    http_chunk_start(ch);

    return http_chunk_feed(ch, (const unsigned char *)text,
                           (long)strlen(text), collect, NULL);
}

/* The same, one byte per call.  A chunk boundary falls where the network put
   it, so every state has to survive being entered with no bytes yet. */
static long feed_dribbled(HttpChunk *ch, const char *text)
{
    long len = (long)strlen(text);
    long i;

    sunk_n  = 0;
    sunk[0] = '\0';

    http_chunk_start(ch);

    for (i = 0; i < len; i++)
    {
        long took = http_chunk_feed(ch, (const unsigned char *)&text[i], 1,
                                    collect, NULL);

        if (took == 0)
            break;                      /* DONE or ERROR: it stopped here  */
    }

    return i;
}

static void test_chunks(void)
{
    HttpChunk ch;
    long      took;

    printf("chunked bodies\n");

    /* The ordinary one. */
    took = feed(&ch, "5\r\nhello\r\n0\r\n\r\n");
    CHECK(ch.state == HTTP_CHUNK_DONE);
    CHECK(strcmp(sunk, "hello") == 0);
    CHECK(took == 15);
    CHECK(ch.total == 5UL);

    /* Two chunks, and a hex size that is not a decimal one. */
    took = feed(&ch, "a\r\n0123456789\r\n1\r\nx\r\n0\r\n\r\n");
    CHECK(ch.state == HTTP_CHUNK_DONE);
    CHECK(strcmp(sunk, "0123456789x") == 0);
    CHECK(ch.total == 11UL);

    /* One byte at a time reaches the same place. */
    (void)feed_dribbled(&ch, "5\r\nhello\r\n0\r\n\r\n");
    CHECK(ch.state == HTTP_CHUNK_DONE);
    CHECK(strcmp(sunk, "hello") == 0);

    /* A trailer before the blank line. */
    took = feed(&ch, "1\r\nx\r\n0\r\nX-Thing: 1\r\n\r\n");
    CHECK(ch.state == HTTP_CHUNK_DONE);
    CHECK(strcmp(sunk, "x") == 0);

    /* What the body ended before is the next request, and the count says
       where.  This is the number the server slides its buffer by. */
    took = feed(&ch, "1\r\nx\r\n0\r\n\r\nGET / HTTP/1.1\r\n\r\n");
    CHECK(ch.state == HTTP_CHUNK_DONE);
    CHECK(took == 11);

    /* A chunk extension is not part of the size. */
    took = feed(&ch, "5;a=b\r\nhello\r\n0\r\n\r\n");
    CHECK(ch.state == HTTP_CHUNK_DONE);
    CHECK(strcmp(sunk, "hello") == 0);

    printf("chunked bodies that are refused\n");

    (void)feed(&ch, "100000000\r\n");
    CHECK(ch.state == HTTP_CHUNK_ERROR);

    (void)feed(&ch, "ffffffff\r\n");     /* eight is the whole of a ULONG   */
    CHECK(ch.state == HTTP_CHUNK_DATA);
    CHECK(ch.left == 4294967295UL);

    (void)feed(&ch, "00000000ff\r\n");   /* leading zeros do not count      */
    CHECK(ch.state == HTTP_CHUNK_DATA);
    CHECK(ch.left == 255UL);

    /* A size with something after it is not a size. */
    (void)feed(&ch, "5X\r\nhello\r\n0\r\n\r\n");
    CHECK(ch.state == HTTP_CHUNK_ERROR);

    (void)feed(&ch, "\r\n");
    CHECK(ch.state == HTTP_CHUNK_ERROR);

    (void)feed(&ch, ";a=b\r\n");
    CHECK(ch.state == HTTP_CHUNK_ERROR);

    /* A size line longer than the buffer is refused rather than truncated
       into one that parses. */
    (void)feed(&ch, "000000000000000000000000000000000000000000005\r\nhello");
    CHECK(ch.state == HTTP_CHUNK_ERROR);

    (void)feed(&ch, "5\r\nhelloXX\r\n0\r\n\r\n");
    CHECK(ch.state == HTTP_CHUNK_ERROR);

    /* Refusals survive the dribble too. */
    (void)feed_dribbled(&ch, "100000000\r\n");
    CHECK(ch.state == HTTP_CHUNK_ERROR);

    (void)feed_dribbled(&ch, "5X\r\n");
    CHECK(ch.state == HTTP_CHUNK_ERROR);

    printf("a body that has not finished\n");

    /* An incomplete body is not an error, because more of it is coming. */
    (void)feed(&ch, "5\r\nhel");
    CHECK(ch.state == HTTP_CHUNK_DATA);
    CHECK(ch.left == 2UL);
    CHECK(strcmp(sunk, "hel") == 0);

    (void)feed(&ch, "5\r\nhello\r\n");
    CHECK(ch.state == HTTP_CHUNK_SIZE);

    (void)feed(&ch, "0\r\n");
    CHECK(ch.state == HTTP_CHUNK_TRAILER);

    /* And off is off. */
    http_chunk_off(&ch);
    CHECK(ch.state == HTTP_CHUNK_OFF);
    CHECK(ch.total == 0UL);
}

int main(void)
{
    test_length();
    test_coding();
    test_tokens();
    test_field_names();
    test_etags();
    test_versions();
    test_combined_lists();
    test_ranges();
    test_chunks();

    printf("%d checks, %d failures\n", checks, failures);

    return (failures == 0) ? 0 : 1;
}

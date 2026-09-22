/* Tests for httpstatus.c, the status-code to reason-phrase table.
 *
 * Every code the server maps and the unknown fallback, asserted
 * string-for-string: a table that loses a case, or a phrase that lost its
 * second half, compiles and reads right, so nothing short of the exact string
 * is proof the table still answers the same status line.
 *
 * SPDX-License-Identifier: MIT
 */

#include "httpstatus.h"

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

static void reason(unsigned long status, const char *want)
{
    CHECK(strcmp(http_status_reason(status), want) == 0);
}

int main(void)
{
    printf("status reason phrases\n");

    reason(100, "Continue");
    reason(101, "Switching Protocols");
    reason(200, "OK");
    reason(201, "Created");
    reason(204, "No Content");
    reason(206, "Partial Content");
    reason(207, "Multi-Status");
    reason(301, "Moved Permanently");
    reason(400, "Bad Request");
    reason(403, "Forbidden");
    reason(404, "Not Found");
    reason(405, "Method Not Allowed");
    reason(408, "Request Timeout");
    reason(409, "Conflict");
    reason(411, "Length Required");
    reason(412, "Precondition Failed");
    reason(413, "Payload Too Large");
    reason(414, "URI Too Long");
    reason(415, "Unsupported Media Type");
    reason(416, "Range Not Satisfiable");
    reason(417, "Expectation Failed");
    reason(423, "Locked");
    reason(424, "Failed Dependency");
    reason(431, "Request Header Fields Too Large");
    reason(500, "Internal Server Error");
    reason(501, "Not Implemented");
    reason(503, "Service Unavailable");
    reason(507, "Insufficient Storage");

    /* Unknown is the fallback for every code the table does not name. */
    reason(0, "Unknown");
    reason(199, "Unknown");
    reason(418, "Unknown");
    reason(999, "Unknown");
    reason(0xFFFFFFFFUL, "Unknown");

    printf("\n%d checks, %d failure(s)\n", checks, failures);
    return failures == 0 ? 0 : 1;
}

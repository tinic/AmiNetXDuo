/* httpstatus, the status-code to reason-phrase table.  See httpstatus.h for
 * why this is its own file.
 *
 * Includes nothing so src/tools/test/test_httpstatus.c compiles the same
 * code the m68k server runs.
 *
 * SPDX-License-Identifier: MIT
 */

#include "httpstatus.h"

const char *http_status_reason(unsigned long status)
{
    switch (status)
    {
        case 100: return "Continue";
        /* The upgrade writes its own status line, so this is for the log. */
        case 101: return "Switching Protocols";
        case 200: return "OK";
        case 201: return "Created";
        case 204: return "No Content";
        case 206: return "Partial Content";
        case 207: return "Multi-Status";
        case 301: return "Moved Permanently";
        case 400: return "Bad Request";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 408: return "Request Timeout";
        case 409: return "Conflict";
        case 411: return "Length Required";
        case 412: return "Precondition Failed";
        case 413: return "Payload Too Large";
        case 414: return "URI Too Long";
        case 415: return "Unsupported Media Type";
        case 416: return "Range Not Satisfiable";
        case 417: return "Expectation Failed";
        case 423: return "Locked";
        case 424: return "Failed Dependency";
        case 431: return "Request Header Fields Too Large";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 503: return "Service Unavailable";
        case 507: return "Insufficient Storage";
        default:  return "Unknown";
    }
}

/*
 * httppath, the request target a client sent, turned into an AmigaOS path
 * or refused, plus the escaping the answer needs.
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_HTTPPATH_H
#define AMINETXDUO_HTTPPATH_H

#define HTTP_PATH_MAX       256
#define HTTP_URL_MAX        256
#define HTTP_NAME_MAX       112

/* Deep enough for any real tree, and a bound on how far a client can make the
   server walk. */
#define HTTP_PATH_SEGMENTS  32

typedef enum HttpPathResult
{
    HTTP_PATH_OK = 0,
    HTTP_PATH_BAD_ESCAPE,       /* % not followed by two hex digits       */
    HTTP_PATH_CONTROL,          /* a control character, before or after   */
    HTTP_PATH_NOT_ABSOLUTE,     /* the target did not begin with /        */
    HTTP_PATH_PARENT,           /* a ".." segment                         */
    HTTP_PATH_DEVICE,           /* a ':', an AmigaOS device reference   */
    HTTP_PATH_BACKSLASH,        /* a '\', a separator to the client     */
    HTTP_PATH_TOO_LONG,
    HTTP_PATH_TOO_DEEP
} HttpPathResult;

typedef struct HttpPath
{
    char path[HTTP_PATH_MAX];   /* "Work:Docs/readme.txt", for Open()     */
    char url[HTTP_URL_MAX];     /* "/Docs/readme.txt", decoded, normalised */
    char name[HTTP_NAME_MAX];   /* "readme.txt", or "" at the root        */
    int  segments;              /* 0 means the document root itself       */
    int  trailing_slash;        /* the client wrote it as a collection    */
} HttpPath;

HttpPathResult http_path_resolve(const char *root, const char *target,
                                 HttpPath *out);

/* A sentence for the log.  Never NULL. */
const char *http_path_error(HttpPathResult why);

/* Non-zero on success.  An overlong root is refused rather than truncated,
   because a truncated root can name a different, existing drawer. */
int http_path_root(const char *given, char *out, unsigned long outlen);

int http_path_join(char *path, unsigned long pathlen, const char *name);

/* Back up one level, in place.  Only ever undoes a join.  A path that is a
   device reference is left alone, because "RAM:" has nothing above it. */
void http_path_up(char *path);

int http_path_within(const char *prefix, const char *path);

unsigned long http_url_escape(const char *path, char *out, unsigned long outlen);

/* &, < and > for XML text.  Same contract. */
unsigned long http_xml_escape(const char *text, char *out, unsigned long outlen);

void http_utf8_trim(char *text);

/*
 * The Content-Type for a file name, from its suffix.  Never NULL.  Anything
 * unrecognised is application/octet-stream, which a client downloads rather
 * than shows.
 */
const char *http_content_type(const char *name);

#endif /* AMINETXDUO_HTTPPATH_H */

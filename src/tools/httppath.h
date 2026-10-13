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
    HTTP_PATH_NOT_VOLUME,       /* first segment is not a mounted volume */
    HTTP_PATH_TOO_LONG,
    HTTP_PATH_TOO_DEEP,
    HTTP_PATH_BAD_UTF8,         /* the decoded bytes are not UTF-8         */
    HTTP_PATH_UNMAPPABLE        /* a character the local codeset lacks    */
} HttpPathResult;

typedef struct HttpPath
{
    char path[HTTP_PATH_MAX];   /* "Work:Docs/readme.txt", for Open()     */
    char url[HTTP_URL_MAX];     /* "/Docs/readme.txt", decoded, normalised */
    char name[HTTP_NAME_MAX];   /* "readme.txt", or "" at the root        */
    int  segments;              /* 0 means the document root itself       */
    int  trailing_slash;        /* the client wrote it as a collection    */
} HttpPath;

/* Targets are percent-encoded UTF-8; path, url and name come back in the
   local codeset, set here (NULL, the default, is ISO-8859-1). */
struct txt_codeset;
void http_path_codeset(const struct txt_codeset *cs);
const struct txt_codeset *http_path_get_codeset(void);

HttpPathResult http_path_resolve(const char *root, const char *target,
                                 HttpPath *out);

/* Resolve a machine-wide URL whose first segment is a volume name.  The
   virtual root has an empty path; "/Work/Docs/x" becomes "Work:Docs/x".
   Whether that volume is mounted is deliberately left to the AmigaOS caller. */
HttpPathResult http_path_resolve_volumes(const char *target, HttpPath *out);

/* COPY and MOVE carry their destination in a header.  A relative target has
   no authority and is local; an absolute one must name the request's Host.
   Ports are deliberately ignored, matching WebDAV clients behind a proxy. */
int http_path_destination_is_local(const char *url, const char *host);

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

/* Turn a resolved AmigaDOS path back into the escaped href WebDAV reports.
   `scratch` holds the unescaped URL so the caller controls the memory cost.
   Returns the escaped length, or 0 when the path is outside root/does not fit. */
unsigned long http_path_url(int volumes, const char *root, const char *path,
                            char *scratch, unsigned long scratchlen,
                            char *out, unsigned long outlen);

/* A local-codeset path to percent-encoded UTF-8. */
unsigned long http_url_escape(const char *path, char *out, unsigned long outlen);

/* &, < and > for XML text.  Same contract.  _name takes a local-codeset name
   and writes UTF-8; the plain one passes bytes (text that is UTF-8 already). */
unsigned long http_xml_escape(const char *text, char *out, unsigned long outlen);
unsigned long http_xml_escape_name(const char *name, char *out,
                                   unsigned long outlen);

void http_utf8_trim(char *text);

/*
 * The Content-Type for a file name, from its suffix.  Never NULL.  Anything
 * unrecognised is application/octet-stream, which a client downloads rather
 * than shows.
 */
const char *http_content_type(const char *name);

/* A conservative look at bytes from an otherwise unknown file.  Amiga text
   is commonly eight-bit, so only NUL and non-whitespace control bytes make a
   sample binary.  An empty sample is valid text. */
int http_content_looks_text(const unsigned char *data, unsigned long len);

#endif /* AMINETXDUO_HTTPPATH_H */

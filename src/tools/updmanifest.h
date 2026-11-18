/*
 * updmanifest, the update manifest and its signature file.
 *
 * Includes nothing; compiled natively by src/tools/test/test_updmanifest.c
 * and tests/fuzz/fuzz_updmanifest.c.  The parser reads the caller's buffer in
 * place, writes only the caller's structures and allocates nothing.
 *
 * THE MANIFEST, update.manifest
 *
 *   One record per line, "key=value", LF only.  Printable ASCII; a CR, a TAB,
 *   any other control byte or a byte above 0x7e is refused.  A line is at
 *   most UPD_LINE_MAX bytes without its LF.  A line starting with "#" is a
 *   comment.  A key is [a-z0-9.]; an unknown key is ignored, so a later
 *   release can add one without breaking this parser.  Fields inside a value
 *   are separated by one space, and the path or name is always the last.
 *
 *   format=1                    first record; any other value is refused
 *   product=AmiNetXDuo
 *   version=1.0.0-rc5           as tools/version.sh --product prints it
 *   channel=prerelease          release | prerelease; a release-channel
 *                               manifest may not carry a prerelease version
 *   certificates.prev=<sha256>  DEVS:Internet/certificates as the previous
 *                               version shipped it; required when any
 *                               flavour lists a certificates file
 *   container=<flavour> <size> <sha256> <name>
 *                               once per flavour offered; <name> is a plain
 *                               file name in the channel's folder
 *   file=<flavour> <class> <size> <sha256> <path>
 *                               once per file of that flavour
 *   end=<n>                     last record; <n> is the number of lines
 *                               before it, comments included
 *
 *   <flavour>  full | minimal | micro | fat
 *   <class>    LIBS | DEVS/Networks | C | certificates, the destination the
 *              path is relative to (certificates = DEVS:Internet/)
 *   <size>     decimal bytes, no leading zero, at most 4294967295
 *   <sha256>   64 hex digits
 *   <path>     relative: no leading "/", no "//", no trailing "/", no ":",
 *              no "." or ".." part, at most UPD_PATH_MAX bytes
 *
 *   product, version, channel, format and end appear once; so does
 *   certificates.prev and the container line of a flavour.  A file line
 *   repeating a class and path (case-insensitively, as AmigaDOS compares
 *   names) of the installed flavour is a duplicate.  Nothing may follow end=.
 *
 *   A container= or file= line whose flavour this updater does not know is
 *   skipped whole, and so is a file= line of another flavour whose class it
 *   does not know; a later release can add either.  An unknown class in the
 *   installed flavour is refused: that file has nowhere to go.
 *
 *   A shell writes it with printf and appends end= with
 *   `printf 'end=%s\n' "$(wc -l < update.manifest)"`.
 *
 * REFUSALS, applied only to a manifest that parses
 *
 *   product differs from the tool's         UPD_E_PRODUCT
 *   channel differs from the installed one  UPD_E_CHANNEL
 *   version not newer than the installed    UPD_E_NOT_NEWER
 *   no container for the installed flavour  UPD_E_NO_FLAVOUR
 *
 * THE SIGNATURE FILE, update.manifest.sig
 *
 *   key=<id> sig=<128 hex digits>
 *
 *   One or more lines, LF only, at most UPD_SIG_LINES.  <id> is [a-z0-9-],
 *   1 to UPD_KEYID_MAX bytes, and names the public key; <sig> is the raw
 *   Ed25519 signature (RFC 8032) of every byte of update.manifest, as
 *   `openssl pkeyutl -sign -rawin` writes it.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_UPDMANIFEST_H
#define AMINETXDUO_UPDMANIFEST_H

#include "updver.h"

#define UPD_LINE_MAX        255
#define UPD_PATH_MAX        107     /* destination-relative path           */
#define UPD_NAME_MAX        64      /* a container's file name             */
#define UPD_KEYID_MAX       16
#define UPD_SIG_LINES       4
#define UPD_PRODUCT         "AmiNetXDuo"

typedef enum UpdResult
{
    UPD_OK = 0,
    UPD_E_TRUNCATED,        /* no LF on the last line, or no end= line     */
    UPD_E_LINE_LONG,
    UPD_E_SYNTAX,           /* not key=value, a bad byte, a missing field  */
    UPD_E_FORMAT,           /* format=1 is not the first record            */
    UPD_E_DUPLICATE,
    UPD_E_MISSING,          /* a required record is absent                 */
    UPD_E_COUNT,            /* end= disagrees with the lines before it     */
    UPD_E_CLASS,            /* the installed flavour names an unknown class */
    UPD_E_HEX,
    UPD_E_NUMBER,
    UPD_E_PATH,             /* "..", ":", absolute, or too long            */
    UPD_E_VERSION,          /* not a version updver.h accepts              */
    UPD_E_TOO_MANY,         /* more files than the caller's table holds    */
    UPD_E_PRODUCT,
    UPD_E_CHANNEL,
    UPD_E_NOT_NEWER,
    UPD_E_NO_FLAVOUR,
    UPD_E_SIG_UNKNOWN_KEY,  /* no line names a key the tool holds          */
    UPD_E_SIG_BAD,          /* a known key, and no signature verifies      */
    UPD_E_ARG,
    UPD_RESULT_COUNT
} UpdResult;

typedef enum UpdFlavour
{
    UPD_FLAVOUR_FULL = 0,
    UPD_FLAVOUR_MINIMAL,
    UPD_FLAVOUR_MICRO,
    UPD_FLAVOUR_FAT,
    UPD_FLAVOUR_COUNT
} UpdFlavour;

typedef enum UpdClass
{
    UPD_CLASS_LIBS = 0,
    UPD_CLASS_NETWORKS,     /* DEVS:Networks/                              */
    UPD_CLASS_C,
    UPD_CLASS_CERTIFICATES, /* DEVS:Internet/                              */
    UPD_CLASS_COUNT
} UpdClass;

typedef enum UpdChannel
{
    UPD_CHANNEL_RELEASE = 0,
    UPD_CHANNEL_PRERELEASE,
    UPD_CHANNEL_COUNT
} UpdChannel;

/* Text pointers below are into the caller's buffer and not NUL-terminated. */
typedef struct UpdFile
{
    const char    *path;
    unsigned short path_len;
    unsigned char  cls;                 /* UpdClass                        */
    unsigned long  size;
    unsigned char  sha256[32];
} UpdFile;

typedef struct UpdManifest
{
    UpdVer         version;
    unsigned char  channel;             /* UpdChannel                      */
    unsigned char  has_certs_prev;
    unsigned char  certs_prev[32];

    /* The installed flavour's container and files. */
    const char    *container;
    unsigned short container_len;
    unsigned long  container_size;
    unsigned char  container_sha256[32];
    unsigned int   nfiles;
} UpdManifest;

/* What is installed: the refusals are judged against this. */
typedef struct UpdWant
{
    UpdVer         installed;
    unsigned char  channel;             /* UpdChannel                      */
    unsigned char  flavour;             /* UpdFlavour                      */
} UpdWant;

/*
 * Parse `len` bytes of `text`.  The installed flavour's file lines go into
 * `files`, at most `max_files` of them; the other flavours' are checked and
 * dropped.  Syntax is judged over the whole manifest before any refusal.
 */
UpdResult upd_manifest_parse(const char *text, unsigned long len,
                             const UpdWant *want, UpdManifest *m,
                             UpdFile *files, unsigned int max_files);

typedef struct UpdSigLine
{
    char          id[UPD_KEYID_MAX + 1];
    unsigned char sig[64];
} UpdSigLine;

/* The signature file into `lines`, at most UPD_SIG_LINES of them. */
UpdResult upd_sig_parse(const char *text, unsigned long len,
                        UpdSigLine *lines, unsigned int *nlines);

/* Words for the enums; NULL past the end. */
const char *upd_flavour_name(unsigned int f);
const char *upd_class_name(unsigned int c);
const char *upd_channel_name(unsigned int c);
const char *upd_result_text(UpdResult r);

#endif /* AMINETXDUO_UPDMANIFEST_H */

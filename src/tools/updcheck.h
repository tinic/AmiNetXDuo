/*
 * updcheck, the decisions of `UpdateAmiNetXDuo CHECK` that are not I/O: the
 * settings file, the installed flavour from bsdsocket.library's IdString,
 * the mirror URLs, the clock floor, the date, and the verdict over a fetched
 * manifest and its signature file.
 *
 * Includes nothing of AmigaOS; compiled natively by
 * src/tools/test/test_updcheck.c.  The CLI contract it serves is the header
 * of tests/updater/scenarios.tsv.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_UPDCHECK_H
#define AMINETXDUO_UPDCHECK_H

#include "updsig.h"

#define UPD_MIRROR_MAX      200     /* MIRROR=, with the NUL               */

/*
 * ENVARC:AmiNetXDuo/Update (ENV: at boot), key=value, one per line:
 *
 *   AUTO=ON|OFF                 the startup check; default ON
 *   CHANNEL=RELEASE|PRERELEASE  default: the installed version's own
 *   MIRROR=<url ending in />    the folder holding release/ and prerelease/
 *
 * Keys and words are matched without regard to case, LF or CR LF ends a
 * line, a "#" or ";" line is a comment and an unknown key is ignored.  A
 * value that is not one of the words above leaves that setting at its
 * default and counts in `bad`.  A MIRROR without its final "/" gets one.
 */
typedef struct UpdSettings
{
    unsigned char auto_check;           /* 1 ON, 0 OFF                     */
    signed char   channel;              /* UpdChannel, -1 for the default  */
    char          mirror[UPD_MIRROR_MAX];   /* "" for the default          */
    unsigned int  bad;
} UpdSettings;

void upd_settings_parse(const char *text, unsigned long len, UpdSettings *s);

/*
 * The flavour from bsdsocket.library's IdString, its "profile <name>" field:
 * default is full, and minimal, micro and fat are themselves.  -1 when the
 * field is absent or names none of them.
 */
int upd_flavour_from_id(const char *id);

/*
 * <mirror><channel>/<name>, NUL-terminated.  0 when it does not fit in
 * `outlen` bytes or `mirror` is empty.
 */
int upd_url(char *out, unsigned long outlen, const char *mirror,
            unsigned int channel, const char *name);

/*
 * 2026-01-01 as DateStamp() days since 1978-01-01, tls_expiry.h's
 * TLS_CLOCK_FLOOR in another unit.  A clock below it was never set.
 */
#define UPD_CLOCK_FLOOR_DAYS    17532UL

/* DateStamp() days to "YYYY-MM-DD"; `out` holds at least 11 bytes. */
void upd_date(unsigned long days, char *out);

/*
 * One key=value line, a + b + c (b, c NULL for none) and a LF, onto the
 * run's log text `buf` (`len` bytes of `cap` used): whole or not at all.
 * The first line that does not fit ends it with "truncated=yes" instead,
 * and nothing follows.  The new length.
 */
#define UPD_SAID_TRUNCATED  "truncated=yes\n"

unsigned long upd_said_add(char *buf, unsigned long cap, unsigned long len,
                           const char *a, const char *b, const char *c);

/* The stack the run gets, by its word: 64 KB for CHECK and INSTALL, which
   fetch (and an http: mirror may redirect to https:, tls.library), 16 KB
   for BOOT and CONFIRM, which do not. */
unsigned long upd_stack_size(const char *word);

typedef enum UpdVerdict
{
    UPD_V_CURRENT = 0,      /* update=current: nothing newer               */
    UPD_V_AVAILABLE,        /* update=available                            */
    UPD_V_SIGNATURE,        /* refused=signature                           */
    UPD_V_UNKNOWN_KEY,      /* refused=unknown_key                         */
    UPD_V_MANIFEST          /* refused=manifest                            */
} UpdVerdict;

/*
 * The signature first, over every byte as fetched; then the parse and its
 * refusals against `want`.  A version that is not newer is current; every
 * other parse failure or refusal is a refused manifest.  `why`, if not
 * NULL, gets the UpdResult behind the verdict.
 */
UpdVerdict upd_check_verdict(const unsigned char *m, unsigned long mlen,
                             const char *sig, unsigned long siglen,
                             const UpdKey *keys, unsigned int nkeys,
                             const UpdWant *want, UpdManifest *out,
                             UpdFile *files, unsigned int max_files,
                             UpdResult *why);

#endif /* AMINETXDUO_UPDCHECK_H */

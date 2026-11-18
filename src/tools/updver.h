/*
 * updver, the version strings tools/version.sh --product prints: 1.0.0,
 * 1.0.0-rc4, 1.0.0-beta8.  Parsed strictly and ordered so that a prerelease
 * sorts below its release and digit runs compare as numbers: beta8 < rc1,
 * rc9 < rc10 < 1.0.0.
 *
 * Includes nothing; compiled natively by src/tools/test/test_updver.c.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_UPDVER_H
#define AMINETXDUO_UPDVER_H

#define UPD_VER_MAX     32      /* longest version string, with the NUL   */
#define UPD_PRE_MAX     16      /* longest prerelease part, with the NUL  */
#define UPD_VER_NUM_MAX 99999UL /* a larger MAJOR/MINOR/PATCH is refused  */

typedef struct UpdVer
{
    unsigned long major;
    unsigned long minor;
    unsigned long patch;
    char          pre[UPD_PRE_MAX];     /* "" for a release               */
} UpdVer;

/*
 * MAJOR.MINOR.PATCH[-PRE].  Numbers have no leading zero.  PRE is letters,
 * digits and dots, the set version.sh allows, with no empty part between
 * dots and no leading zero in a digit run, so two strings that compare equal
 * are the same string.  `len` bytes of `s` are read; no NUL is needed.
 * Returns 1 and fills `v`, or 0 and leaves `v` undefined.
 */
int upd_ver_parse(const char *s, unsigned long len, UpdVer *v);

/* <0, 0 or >0 as a is older than, the same as or newer than b. */
int upd_ver_cmp(const UpdVer *a, const UpdVer *b);

/* Back to text; `out` holds at least UPD_VER_MAX bytes. */
void upd_ver_format(const UpdVer *v, char *out);

#endif /* AMINETXDUO_UPDVER_H */

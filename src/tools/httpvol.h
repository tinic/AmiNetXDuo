/* Mounted-volume enumeration and URL resolution for httpd.
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_HTTPVOL_H
#define AMINETXDUO_HTTPVOL_H

#include <exec/types.h>

#include "httppath.h"

BOOL http_vol_at(UWORD wanted, char *out, ULONG outlen);
HttpPathResult http_vol_resolve(BOOL volumes, const char *root,
                                const char *target, HttpPath *out);

#endif /* AMINETXDUO_HTTPVOL_H */

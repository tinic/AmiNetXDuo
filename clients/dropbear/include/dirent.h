/* clients/dropbear/include -- see clients/dropbear/build.sh.
 * newlib's <dirent.h> on this toolchain is `#error "<dirent.h> not supported"`.
 * Dropbear's includes.h includes it unconditionally and dbclient never opens a
 * directory, so this is the declaration nothing calls.
 * SPDX-License-Identifier: MIT */
#ifndef AMIGA_DIRENT_H
#define AMIGA_DIRENT_H
#include <sys/types.h>
struct dirent { long d_ino; char d_name[256]; };
typedef struct DIR DIR;
#endif

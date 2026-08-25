/* clients/dropbear/include, see clients/dropbear/build.sh.  newlib's <dirent.h>
 * is `#error "not supported"` and Dropbear's includes.h includes it
 * unconditionally; nothing here is ever called.  SPDX-License-Identifier: MIT */
#ifndef AMIGA_DIRENT_H
#define AMIGA_DIRENT_H
#include <sys/types.h>
struct dirent { long d_ino; char d_name[256]; };
typedef struct DIR DIR;
#endif

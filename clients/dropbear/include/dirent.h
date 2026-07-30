/* clients/dropbear/include -- see clients/dropbear/build.sh.
 * newlib's <dirent.h> on this toolchain is `#error "<dirent.h> not supported"`.
 * Dropbear's includes.h includes it unconditionally, and scp.c's rsource()
 * walks a directory for -r, so these three are implemented over Lock()/
 * Examine()/ExNext() in amiga_dropbear.c.
 *
 * "." and ".." are not entries on AmigaOS, so a caller that filters them --
 * scp.c does -- simply never sees them.
 * SPDX-License-Identifier: MIT */
#ifndef AMIGA_DIRENT_H
#define AMIGA_DIRENT_H
#include <sys/types.h>
struct dirent { long d_ino; char d_name[256]; };
typedef struct DIR DIR;
DIR           *opendir(const char *path);
struct dirent *readdir(DIR *dir);
int            closedir(DIR *dir);
#endif

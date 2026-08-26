/* clients/dropbear/include, see clients/dropbear/build.sh.  newlib's <dirent.h>
 * is `#error "not supported"`; the implementation used by scp lives in
 * amiga_scp.c.  SPDX-License-Identifier: MIT */
#ifndef AMIGA_DIRENT_H
#define AMIGA_DIRENT_H
#include <sys/types.h>
struct dirent { long d_ino; char d_name[256]; };
typedef struct DIR DIR;
DIR *opendir(const char *path);
struct dirent *readdir(DIR *dir);
int closedir(DIR *dir);
#endif

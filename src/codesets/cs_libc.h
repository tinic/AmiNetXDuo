/*
 * Forced into every third_party/libcodesets source by CMakeLists.txt here.
 * Upstream builds its AmigaOS 3 library against clib2, which has stricmp()
 * and strnicmp(); newlib has neither.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef CS_LIBC_H
#define CS_LIBC_H

#include <stddef.h>
#include "cs_ndk_compat.h"

int stricmp(const char *a, const char *b);
int strnicmp(const char *a, const char *b, size_t n);

#endif /* CS_LIBC_H */

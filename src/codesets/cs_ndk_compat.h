/* Match libcodesets' AmigaOS 3 UtilityBase definition with either SDK.
 * NDK 3.2's generated proto header declares struct Library *, while the
 * vendored library defines struct UtilityBase *. Suppress only that proto
 * header's base declaration, then provide the vendor's type consistently.
 * SPDX-License-Identifier: MIT
 */
#ifndef CS_NDK_COMPAT_H
#define CS_NDK_COMPAT_H

#ifndef __NOLIBBASE__
#define __NOLIBBASE__
#define CS_RESTORE_LIBBASE_DECLARATIONS
#endif
#include <proto/utility.h>
#ifdef CS_RESTORE_LIBBASE_DECLARATIONS
#undef CS_RESTORE_LIBBASE_DECLARATIONS
#undef __NOLIBBASE__
#endif

struct UtilityBase;
extern struct UtilityBase *UtilityBase;

#endif

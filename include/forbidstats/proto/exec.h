/*
 * AMINETXDUO_FORBIDSTATS only: ahead of the NDK on the include path of the
 * library's own targets (CMakeLists.txt), so every direct Forbid(), Permit(),
 * Disable(), Enable() and Wait() there goes through the census in
 * src/common/forbidstats.c.  AMI_FS_IMPL is that file, which needs Exec's.
 * SPDX-License-Identifier: MIT
 */
#include_next <proto/exec.h>

/* No guard on the redefinitions: the NDK's own headers come back through
   here while the outer proto/exec.h is still open, and its inline/exec.h
   then lands after them.  The outermost pass is the one that sticks. */
#if defined(AMINETXDUO_FORBIDSTATS) && !defined(AMI_FS_IMPL)

#include "aminetxduo/forbidstats.h"

#undef Forbid
#undef Permit
#undef Disable
#undef Enable
#undef Wait
#define Forbid()    ami_fs_forbid(AMI_FS_SITE)
#define Permit()    ami_fs_permit()
#define Disable()   ami_fs_disable(AMI_FS_SITE)
#define Enable()    ami_fs_enable()

/* Wait() stays Exec's inline with the bookkeeping either side of it: the
   port's scheduler hung when its Wait() went through a function. */
#define Wait(s)     ({                                                      \
    ULONG       _fs_s = (ULONG)(s);                                       \
    const char *_fs_f = ami_fs_sleep(AMI_FS_FORBID);                      \
    const char *_fs_d = ami_fs_sleep(AMI_FS_DISABLE);                     \
    ULONG       _fs_r = __Wait_base((EXEC_BASE_NAME), _fs_s);             \
    ami_fs_woken(AMI_FS_DISABLE, _fs_d);                                  \
    ami_fs_woken(AMI_FS_FORBID, _fs_f);                                   \
    _fs_r; })

#endif

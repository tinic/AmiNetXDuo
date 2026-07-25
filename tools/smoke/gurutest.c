/* Validates Guru (exec Alert) interception by double-freeing on purpose.
   SPDX-License-Identifier: MIT */
#include <proto/exec.h>
#include <proto/dos.h>
#include "aminetxduo/compat.h"
#include "aminetxduo/crashguard.h"

int main(void)
{
    APTR p;

    Printf((STRPTR)"gurutest: installing alert hook\n");
    if (!ami_crash_install_alert_hook())
    {
        Printf((STRPTR)"gurutest: SetFunction on Alert failed\n");
        return RETURN_ERROR;
    }

    AMI_ERROR("gurutest: about to free the same block twice");
    p = AllocVec(256, MEMF_PUBLIC | MEMF_CLEAR);
    FreeVec(p);
    FreeVec(p);                 /* AN_FreeTwice, 0x01000009 */

    Printf((STRPTR)"gurutest: survived (exec did not alert)\n");
    ami_crash_remove_alert_hook();
    return RETURN_OK;
}

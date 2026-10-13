/* Drivers exposed by Exec without a file in DEVS:.
 * SPDX-License-Identifier: MIT
 */
#ifndef NETPROBE_DEVICE_H
#define NETPROBE_DEVICE_H

#include <exec/execbase.h>
#include <exec/resident.h>
#include <proto/exec.h>

extern struct ExecBase *SysBase;

static BOOL netprobe_device_present(const char *name)
{
    struct Resident *resident;
    BOOL present;

    if (name == NULL || name[0] == '\0')
        return FALSE;

    /* AddDevice/RemDevice can change the list while another task runs.
       Do not open a device just to detect it: that can claim hardware. */
    Forbid();
    present = (BOOL)(FindName(&SysBase->DeviceList, (CONST_STRPTR)name) != NULL);
    if (!present)
    {
        resident = FindResident((CONST_STRPTR)name);
        present = (BOOL)(resident != NULL && resident->rt_Type == NT_DEVICE);
    }
    Permit();
    return present;
}

#endif

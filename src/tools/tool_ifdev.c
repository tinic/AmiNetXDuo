/*
 * The whole device path of each interface, from NETSTATUS_IFDEVICES.
 * nsi_Device holds 31 characters; a path from a drawer installation or a
 * RAM: file is longer.
 *
 * SPDX-License-Identifier: MIT
 */

#include "tools.h"

static struct
{
    NetStatusHeader   hdr;
    NetStatusIfDevice e[NX_MAX_PHYSICAL_INTERFACES];
} ifdev;

static LONG ifdev_count;

VOID tool_netstatus_devices(struct Library *base)
{
    LONG n;
    LONG i;

    /* -1 from a library without the selector: every slot falls back. */
    n = tool_netstatus_query(base, NETSTATUS_IFDEVICES, &ifdev,
                             sizeof(ifdev), sizeof(NetStatusIfDevice));

    ifdev_count = (n > 0) ? n : 0;

    for (i = 0; i < ifdev_count; i++)
        ifdev.e[i].nsd_Device[NETSTATUS_FILE_LEN - 1] = '\0';
}

VOID tool_if_device(char *dst, ULONG dstlen, const NetStatusInterface *e)
{
    LONG i;

    for (i = 0; i < ifdev_count; i++)
    {
        if (ifdev.e[i].nsd_Index == e->nsi_Index &&
            ifdev.e[i].nsd_Device[0] != '\0')
        {
            tool_copy_string(dst, dstlen, ifdev.e[i].nsd_Device);
            return;
        }
    }

    /* nsi_Device bounded to its own field, terminated or not. */
    if (dstlen > (ULONG)NETSTATUS_DEVICE_LEN + 1)
        dstlen = (ULONG)NETSTATUS_DEVICE_LEN + 1;
    tool_copy_string(dst, dstlen, e->nsi_Device);
}

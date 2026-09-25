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

BOOL tool_if_device(char *dst, ULONG dstlen, const NetStatusInterface *e)
{
    LONG i;

    for (i = 0; i < ifdev_count; i++)
    {
        if (ifdev.e[i].nsd_Index == e->nsi_Index &&
            ifdev.e[i].nsd_Device[0] != '\0')
        {
            tool_copy_string(dst, dstlen, ifdev.e[i].nsd_Device);
            return TRUE;
        }
    }

    /* nsi_Device bounded to its own field, terminated or not. */
    if (dstlen > (ULONG)NETSTATUS_DEVICE_LEN + 1)
        dstlen = (ULONG)NETSTATUS_DEVICE_LEN + 1;
    tool_copy_string(dst, dstlen, e->nsi_Device);
    return FALSE;
}

/*
 * nsi_Device is NETSTATUS_DEVICE_LEN bytes and the library fills it from ITS
 * copy of the interface file (netstatus.c, ns_config_for), so what arrives is
 * the file's own device name cut at 31 characters -- and a system
 * installation's Workbench:AmiNetXDuo/Devs/Networks/anxnet.device is 48.
 * Compared whole against the tool's full parse of the same file, that printed
 * the "changed after the network started" NOTE on every ShowNetStatus of a
 * drawer installation (the A3000, 2026-09-19).  A copy that fills the field
 * and is the file's prefix is the same name.  A library with
 * NETSTATUS_IFDEVICES sends the name whole (tool_if_device()), and then
 * only an exact match is the same name: tool_device_matches().
 */
BOOL tool_device_matches(const char *file, const char *live, BOOL whole)
{
    ULONG n = 0;

    if (whole)
        return (BOOL)(tool_stricmp(file, live) == 0);

    while (live[n] != '\0')
        n++;

    /* A copy that fills nsi_Device may be the file's name cut short. */
    if (n == (ULONG)NETSTATUS_DEVICE_LEN - 1)
        return (BOOL)(tool_stricmp_n(file, live, n) == 0);

    return (BOOL)(tool_stricmp(file, live) == 0);
}
